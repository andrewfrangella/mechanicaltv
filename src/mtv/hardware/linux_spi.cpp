#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "mtv/hardware/linux_hw.h"

namespace mtv::hw {

SpiDev::SpiDev(const std::string& path, std::uint32_t speed_hz, int mode) : path_(path), speed_(speed_hz) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    throw std::runtime_error(path + ": " + std::strerror(errno) +
                             (errno == ENOENT ? " (enable SPI: dtparam=spi=on in /boot/firmware/config.txt)" : ""));
  }
  std::uint8_t m = static_cast<std::uint8_t>(mode);
  std::uint8_t bits = 8;
  if (::ioctl(fd_, SPI_IOC_WR_MODE, &m) < 0 || ::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed_) < 0) {
    int err = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error(path + ": SPI setup failed: " + std::strerror(err));
  }
  std::size_t bufsiz = 4096;
  std::ifstream in("/sys/module/spidev/parameters/bufsiz");
  if (in) in >> bufsiz;
  // spi-bcm2835 limits one DMA transfer to 65,535 bytes (16-bit DLEN); keep a word-aligned margin.
  max_transfer_ = std::min<std::size_t>(bufsiz, 65532);
}

SpiDev::~SpiDev() {
  if (fd_ >= 0) ::close(fd_);
}

bool SpiDev::write(const std::uint8_t* data, std::size_t len, std::string* why) {
  spi_ioc_transfer tr{};
  tr.tx_buf = reinterpret_cast<std::uintptr_t>(data);
  tr.rx_buf = 0;
  tr.len = static_cast<__u32>(len);
  tr.speed_hz = speed_;
  tr.bits_per_word = 8;
  int rc;
  do {
    rc = ::ioctl(fd_, SPI_IOC_MESSAGE(1), &tr);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    if (why) *why = std::string("SPI transfer failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace mtv::hw
