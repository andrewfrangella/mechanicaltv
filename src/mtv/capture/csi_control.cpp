#include "mtv/capture/csi_control.h"

#ifdef MTV_LINUX
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mtv {

std::vector<std::uint8_t> read_edid_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("EDID file not found: " + path);
  std::vector<std::uint8_t> out;
  std::string token;
  while (in >> token) {
    if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) token = token.substr(2);
    // Accept both "00 ff ..." and run-together "00ff..." forms.
    if (token.size() % 2 != 0) throw std::runtime_error("EDID file " + path + " is not hex bytes");
    for (std::size_t i = 0; i < token.size(); i += 2) {
      char* end = nullptr;
      std::string byte = token.substr(i, 2);
      long v = std::strtol(byte.c_str(), &end, 16);
      if (!end || *end) throw std::runtime_error("EDID file " + path + " is not hex bytes");
      out.push_back(static_cast<std::uint8_t>(v));
    }
  }
  if (out.empty() || out.size() % 128 != 0) {
    throw std::runtime_error("EDID file " + path + " holds " + std::to_string(out.size()) + " bytes, not whole 128-byte blocks");
  }
  return out;
}

bool edid_checksums_ok(const std::vector<std::uint8_t>& edid) {
  if (edid.empty() || edid.size() % 128 != 0) return false;
  for (std::size_t b = 0; b < edid.size(); b += 128) {
    unsigned sum = 0;
    for (std::size_t i = 0; i < 128; ++i) sum += edid[b + i];
    if (sum % 256 != 0) return false;
  }
  return true;
}

namespace {

#ifdef MTV_LINUX
class IoctlCsiControl : public CsiControl {
 public:
  std::string load_edid(const std::string& device, const std::vector<std::uint8_t>& edid) override {
    int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::string("cannot open ") + device + ": " + std::strerror(errno);
    v4l2_edid e{};
    e.pad = 0;
    e.start_block = 0;
    e.blocks = static_cast<__u32>(edid.size() / 128);
    e.edid = const_cast<__u8*>(edid.data());
    int rc = ::ioctl(fd, VIDIOC_S_EDID, &e);
    int err = errno;
    ::close(fd);
    if (rc != 0) return std::string("VIDIOC_S_EDID: ") + std::strerror(err);
    return "";
  }

  std::string query_and_lock(const std::string& device, DvTimings* out) override {
    int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::string("cannot open ") + device + ": " + std::strerror(errno);
    v4l2_dv_timings t{};
    if (::ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &t) != 0) {
      int err = errno;
      ::close(fd);
      if (err == ENOLINK) return "no link (no HDMI source, or the source sends nothing)";
      if (err == ENOLCK) return "signal not stable yet";
      if (err == ERANGE) return "the source's timings are outside the bridge's range";
      return std::string("VIDIOC_QUERY_DV_TIMINGS: ") + std::strerror(err);
    }
    if (::ioctl(fd, VIDIOC_S_DV_TIMINGS, &t) != 0) {
      int err = errno;
      ::close(fd);
      return std::string("VIDIOC_S_DV_TIMINGS: ") + std::strerror(err);
    }
    ::close(fd);
    if (out) {
      const v4l2_bt_timings& bt = t.bt;
      out->width = static_cast<int>(bt.width);
      out->height = static_cast<int>(bt.height);
      out->pixelclock = bt.pixelclock;
      out->interlaced = bt.interlaced != 0;
      double total_w = static_cast<double>(bt.width) + bt.hfrontporch + bt.hsync + bt.hbackporch;
      double total_h = static_cast<double>(bt.height) + bt.vfrontporch + bt.vsync + bt.vbackporch;
      if (bt.interlaced) total_h += static_cast<double>(bt.il_vfrontporch) + bt.il_vsync + bt.il_vbackporch;
      out->fps = (total_w > 0 && total_h > 0) ? static_cast<double>(bt.pixelclock) / (total_w * total_h) : 0.0;
    }
    return "";
  }
};
using PlatformCsiControl = IoctlCsiControl;
#else
class NoCsiControl : public CsiControl {
 public:
  std::string load_edid(const std::string&, const std::vector<std::uint8_t>&) override { return "V4L2 is only available on Linux"; }
  std::string query_and_lock(const std::string&, DvTimings*) override { return "V4L2 is only available on Linux"; }
};
using PlatformCsiControl = NoCsiControl;
#endif

}  // namespace

std::unique_ptr<CsiControl> make_csi_control() { return std::make_unique<PlatformCsiControl>(); }

}  // namespace mtv
