#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

#include "mtv/common/env.h"
#include "mtv/hardware/linux_hw.h"

namespace mtv::hw {

namespace {

int request_line(const std::string& chip, int offset, const std::string& consumer, std::uint64_t flags, int initial,
                 std::string* why) {
  int cfd = ::open(chip.c_str(), O_RDWR | O_CLOEXEC);
  if (cfd < 0) {
    if (why) *why = chip + ": " + std::strerror(errno) + (errno == EACCES ? " (add the user to the gpio group)" : "");
    return -1;
  }
  gpio_v2_line_request req{};
  req.offsets[0] = static_cast<__u32>(offset);
  req.num_lines = 1;
  std::strncpy(req.consumer, consumer.c_str(), sizeof req.consumer - 1);
  req.config.flags = flags;
  if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = initial ? 1 : 0;
    req.config.attrs[0].mask = 1;
  }
  if (flags & (GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING)) req.event_buffer_size = 64;
  int rc = ::ioctl(cfd, GPIO_V2_GET_LINE_IOCTL, &req);
  int err = errno;
  ::close(cfd);
  if (rc < 0) {
    if (why) {
      *why = "GPIO " + std::to_string(offset) + " on " + chip + ": " + std::strerror(err);
      if (err == EBUSY) *why += " (another program, or a device-tree overlay, owns this pin)";
    }
    return -1;
  }
  return req.fd;
}

}  // namespace

GpioLine::~GpioLine() {
  if (fd_ >= 0) ::close(fd_);
}

std::unique_ptr<GpioLine> GpioLine::input_rising(const std::string& chip, int offset, const std::string& consumer,
                                                 Bias bias, std::string* why) {
  // Deliberately no GPIO_V2_LINE_ATTR_ID_DEBOUNCE: kernel debounce moves timestamping from the
  // hard IRQ to a workqueue. Edges are debounced in user space against the predicted period.
  std::uint64_t flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING;
  if (bias == Bias::PullDown) flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
  else if (bias == Bias::PullUp) flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
  else flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;
  int fd = request_line(chip, offset, consumer, flags, 0, why);
  if (fd < 0) return nullptr;
  return std::unique_ptr<GpioLine>(new GpioLine(fd, offset));
}

std::unique_ptr<GpioLine> GpioLine::output(const std::string& chip, int offset, const std::string& consumer, int initial,
                                           std::string* why) {
  int fd = request_line(chip, offset, consumer, GPIO_V2_LINE_FLAG_OUTPUT, initial, why);
  if (fd < 0) return nullptr;
  return std::unique_ptr<GpioLine>(new GpioLine(fd, offset));
}

bool GpioLine::set(int value) {
  gpio_v2_line_values v{};
  v.mask = 1;
  v.bits = value ? 1 : 0;
  return ::ioctl(fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) == 0;
}

int GpioLine::get() {
  gpio_v2_line_values v{};
  v.mask = 1;
  if (::ioctl(fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) != 0) return -1;
  return (v.bits & 1) ? 1 : 0;
}

bool GpioLine::read_event(std::int64_t* timestamp_ns, std::uint32_t* seqno) {
  gpio_v2_line_event ev{};
  ssize_t n = ::read(fd_, &ev, sizeof ev);
  if (n != static_cast<ssize_t>(sizeof ev)) return false;
  if (timestamp_ns) *timestamp_ns = static_cast<std::int64_t>(ev.timestamp_ns);
  if (seqno) *seqno = ev.line_seqno;
  return true;
}

std::string main_gpiochip(std::string* label_out) {
  std::string setting = env::trim(env::get("MTV_GPIOCHIP"));
  if (!setting.empty()) {
    std::string path = setting.find('/') == std::string::npos ? "/dev/gpiochip" + setting : setting;
    if (label_out) *label_out = "";
    return path;
  }
  std::error_code ec;
  for (int i = 0; i < 16; ++i) {
    std::string path = "/dev/gpiochip" + std::to_string(i);
    if (!std::filesystem::exists(path, ec)) continue;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    gpiochip_info info{};
    int rc = ::ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info);
    ::close(fd);
    if (rc != 0) continue;
    std::string label(info.label);
    if (label.find("pinctrl-bcm2711") != std::string::npos || label.find("pinctrl-bcm2835") != std::string::npos ||
        label.find("pinctrl-rp1") != std::string::npos) {
      if (label_out) *label_out = label;
      return path;
    }
  }
  if (label_out) *label_out = "";
  return "/dev/gpiochip0";
}

std::unique_ptr<GpioMem> GpioMem::open(std::string* why) {
  std::string label;
  main_gpiochip(&label);
  if (!label.empty() && label.find("rp1") != std::string::npos) {
    if (why) *why = "/dev/gpiomem register access is for the Pi 0-4 GPIO block, not the Pi 5's RP1";
    return nullptr;
  }
  int fd = ::open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
  if (fd < 0) {
    if (why) *why = std::string("/dev/gpiomem: ") + std::strerror(errno);
    return nullptr;
  }
  void* p = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    if (why) *why = std::string("mmap /dev/gpiomem: ") + std::strerror(errno);
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<GpioMem>(new GpioMem(static_cast<volatile std::uint32_t*>(p), fd));
}

GpioMem::~GpioMem() {
  ::munmap(const_cast<std::uint32_t*>(regs_), 4096);
  ::close(fd_);
}

}  // namespace mtv::hw
