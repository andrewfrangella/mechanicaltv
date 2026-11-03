#include "mtv/common/sysinfo.h"

#include <pwd.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace mtv::sysinfo {

std::optional<double> temperature_c() {
  std::ifstream in("/sys/class/thermal/thermal_zone0/temp");
  if (!in) return std::nullopt;
  long value = 0;
  if (!(in >> value)) return std::nullopt;
  return value / 1000.0;
}

DiskUsage disk_usage(const std::string& path) {
  DiskUsage u;
  struct statvfs st{};
  if (::statvfs(path.c_str(), &st) == 0) {
    u.total = static_cast<std::uint64_t>(st.f_frsize) * st.f_blocks;
    u.free = static_cast<std::uint64_t>(st.f_frsize) * st.f_bavail;
  }
  return u;
}

std::optional<std::array<double, 3>> load_average() {
  double loads[3];
  if (::getloadavg(loads, 3) != 3) return std::nullopt;
  return std::array<double, 3>{loads[0], loads[1], loads[2]};
}

std::string home_dir() {
  const char* h = std::getenv("HOME");
  if (h && *h) return h;
  if (struct passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir ? pw->pw_dir : "";
  return "";
}

std::string default_data_dir() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  std::filesystem::path base = (xdg && *xdg) ? std::filesystem::path(xdg) : std::filesystem::path(home_dir()) / ".local" / "share";
  return (base / "mechanical-tv").string();
}

bool path_exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}
bool is_dir(const std::string& path) {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
}
bool is_file(const std::string& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

}  // namespace mtv::sysinfo
