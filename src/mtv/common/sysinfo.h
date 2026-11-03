// Small system facts the engine reports: CPU temperature, disk usage, load average, data directory.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace mtv::sysinfo {

std::optional<double> temperature_c();                 // /sys/class/thermal/thermal_zone0/temp, else nullopt
struct DiskUsage { std::uint64_t total = 0; std::uint64_t free = 0; };
DiskUsage disk_usage(const std::string& path);
std::optional<std::array<double, 3>> load_average();
std::string home_dir();
std::string default_data_dir();                       // $XDG_DATA_HOME or ~/.local/share, plus /mechanical-tv
bool path_exists(const std::string& path);
bool is_dir(const std::string& path);
bool is_file(const std::string& path);

}  // namespace mtv::sysinfo
