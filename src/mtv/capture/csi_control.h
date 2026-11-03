// TC358743 bridge control through V4L2 ioctls (replaces the v4l2-ctl calls of the Python app):
// load the EDID into the chip, query the incoming DV timings and lock them before streaming.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mtv {

struct DvTimings {
  int width = 0;
  int height = 0;
  double fps = 0.0;
  std::uint64_t pixelclock = 0;
  bool interlaced = false;
};

// Parse an EDID hex text file (whitespace-separated hex bytes, as v4l2-ctl --set-edid reads).
// Returns the bytes; throws std::runtime_error when the file is missing, malformed or not a multiple
// of 128 bytes.
std::vector<std::uint8_t> read_edid_file(const std::string& path);
// True when every 128-byte block sums to 0 modulo 256.
bool edid_checksums_ok(const std::vector<std::uint8_t>& edid);

class CsiControl {
 public:
  virtual ~CsiControl() = default;
  // Each returns "" on success or the reason on failure.
  virtual std::string load_edid(const std::string& device, const std::vector<std::uint8_t>& edid) = 0;
  virtual std::string query_and_lock(const std::string& device, DvTimings* timings) = 0;
};

// ioctl implementation (Linux); elsewhere every call reports that it is unavailable.
std::unique_ptr<CsiControl> make_csi_control();

}  // namespace mtv
