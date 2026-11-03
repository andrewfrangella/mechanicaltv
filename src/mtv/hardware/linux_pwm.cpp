#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "mtv/hardware/linux_hw.h"

namespace mtv::hw {

namespace fs = std::filesystem;

SysfsPwm::SysfsPwm(const std::string& chip_setting, int channel) {
  std::vector<std::string> chips;
  std::error_code ec;
  if (!chip_setting.empty()) {
    chips.push_back(chip_setting);
  } else if (fs::exists("/sys/class/pwm", ec)) {
    for (auto& e : fs::directory_iterator("/sys/class/pwm", ec)) {
      if (e.path().filename().string().rfind("pwmchip", 0) == 0) chips.push_back(e.path().string());
    }
    std::sort(chips.begin(), chips.end());
  }
  if (chips.empty()) {
    throw std::runtime_error("No hardware PWM found: add 'dtoverlay=pwm,pin=18,func=2' to /boot/firmware/config.txt and reboot");
  }
  const std::string chip = chips.front();
  path_ = chip + "/pwm" + std::to_string(channel);
  if (!fs::is_directory(path_, ec)) {
    std::ofstream ex(chip + "/export");
    if (ex) ex << channel;
    for (int i = 0; i < 50 && !fs::exists(path_ + "/period", ec); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!fs::exists(path_ + "/period", ec)) {
    throw std::runtime_error("PWM channel " + path_ + " could not be exported (run install.sh, which grants the gpio group access)");
  }
  // udev hands the exported files to the gpio group a moment after export
  for (int i = 0; i < 25; ++i) {
    std::ofstream probe(path_ + "/enable");
    if (probe) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  write("enable", 0);
  std::ifstream in(path_ + "/period");
  in >> period_;
}

SysfsPwm::~SysfsPwm() {
  try {
    enable(false);
  } catch (...) {
  }
}

void SysfsPwm::write(const std::string& file, std::uint64_t value) {
  std::ofstream out(path_ + "/" + file);
  if (!out) throw std::runtime_error("cannot write " + path_ + "/" + file + ": " + std::strerror(errno));
  out << value;
  out.flush();
  if (!out) throw std::runtime_error("write " + std::to_string(value) + " to " + path_ + "/" + file + " failed: " + std::strerror(errno));
}

void SysfsPwm::set_period_ns(std::uint64_t period) {
  if (period == period_) return;
  // The kernel needs duty <= period at every moment: shrink duty first when speeding up.
  if (period_ && period < period_) {
    write("duty_cycle", period / 2);
    write("period", period);
  } else {
    write("period", period);
    write("duty_cycle", period / 2);
  }
  period_ = period;
}

void SysfsPwm::enable(bool on) {
  if (on == enabled_) return;
  write("enable", on ? 1 : 0);
  enabled_ = on;
}

}  // namespace mtv::hw
