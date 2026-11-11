#include "mtv/hardware/sensor.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "mtv/common/clock.h"
#ifdef MTV_LINUX
#include <poll.h>
#include <time.h>

#include <cerrno>
#include <cstring>

#include "mtv/common/env.h"
#include "mtv/hardware/linux_hw.h"
#endif

namespace mtv {

// ---------------------------------------------------------------- mock

MockIndexSensor::MockIndexSensor(int p, std::function<double()> step_count, std::function<double()> step_rate, int steps_per_rev,
                                 double phase_steps)
    : count_(std::move(step_count)), rate_(std::move(step_rate)), spr_(std::max(1, steps_per_rev)), phase_(phase_steps) {
  pin = p;
}

bool MockIndexSensor::open(std::string*) {
  open_ = true;
  double c = count_ ? count_() : 0.0;
  next_boundary_ = (std::floor((c - phase_) / spr_) + 1.0) * spr_ + phase_;
  return true;
}

void MockIndexSensor::close() { open_ = false; }

void MockIndexSensor::set_steps_per_rev(int spr) {
  spr_ = std::max(1, spr);
  rephase_ = true;
}

void MockIndexSensor::inject(std::int64_t t_ns) {
  std::lock_guard<std::mutex> lock(lock_);
  injected_.push_back(t_ns);
}

EdgeResult MockIndexSensor::wait(std::int64_t timeout_ns) {
  const std::int64_t deadline = now_ns() + timeout_ns;
  while (true) {
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (!injected_.empty()) {
        EdgeResult r{EdgeResult::Edge, injected_.front(), ""};
        injected_.pop_front();
        return r;
      }
    }
    std::int64_t now = now_ns();
    if (coupled_ && count_ && rate_) {
      double c = count_();
      if (rephase_.exchange(false)) next_boundary_ = (std::floor((c - phase_) / spr_) + 1.0) * spr_ + phase_;
      if (c >= next_boundary_) {
        // Report the moment the count crossed the boundary, like a kernel edge timestamp, even if
        // this call came late (the caller was busy emitting the previous revolution).
        double r = rate_();
        std::int64_t t = now;
        if (r > 0) t = now - static_cast<std::int64_t>((c - next_boundary_) / r * 1e9);
        next_boundary_ += spr_;
        if (c >= next_boundary_) next_boundary_ = (std::floor((c - phase_) / spr_) + 1.0) * spr_ + phase_;   // skipped revolutions
        return {EdgeResult::Edge, t, ""};
      }
      double r = rate_();
      if (r > 0) {
        auto until = static_cast<std::int64_t>((next_boundary_ - c) / r * 1e9);
        if (now + until <= deadline) {
          sleep_s(std::min(until, std::int64_t(20'000'000)) / 1e9);   // re-check every 20 ms (the rate ramps)
          continue;
        }
      }
    }
    if (now >= deadline) return {EdgeResult::Timeout, 0, ""};
    sleep_s(std::min<std::int64_t>(deadline - now, 20'000'000) / 1e9);
  }
}

bool MockIndexSensor::pending() {
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (!injected_.empty()) return true;
  }
  return coupled_ && count_ && count_() >= next_boundary_;
}

// ---------------------------------------------------------------- GPIO character device

#ifdef MTV_LINUX
namespace {

class GpioIndexSensor : public IndexSensor {
 public:
  explicit GpioIndexSensor(int p) { pin = p; }
  bool open(std::string* why) override {
    std::string pull = env::lower(env::trim(env::get("MTV_OPTO_PULL", "down")));
    hw::GpioLine::Bias bias = pull == "up" ? hw::GpioLine::Bias::PullUp
                              : (pull == "none" || pull == "off") ? hw::GpioLine::Bias::None
                                                                  : hw::GpioLine::Bias::PullDown;
    line_ = hw::GpioLine::input_rising(hw::main_gpiochip(), pin, "mechanical-tv-opto", bias, why);
    open_ = line_ != nullptr;
    return open_;
  }
  void close() override {
    open_ = false;
    line_.reset();
  }
  bool is_open() const override { return open_; }
  bool pending() override {
    if (!line_) return false;
    pollfd p{line_->fd(), POLLIN, 0};
    return ::poll(&p, 1, 0) > 0;
  }
  EdgeResult wait(std::int64_t timeout_ns) override {
    if (!line_) return {EdgeResult::Error, 0, "index sensor line not open"};
    pollfd p{line_->fd(), POLLIN, 0};
    timespec ts{static_cast<time_t>(timeout_ns / 1'000'000'000), static_cast<long>(timeout_ns % 1'000'000'000)};
    int rc = ::ppoll(&p, 1, &ts, nullptr);
    if (rc < 0) {
      if (errno == EINTR) return {EdgeResult::Timeout, 0, ""};
      return {EdgeResult::Error, 0, std::string("poll: ") + std::strerror(errno)};
    }
    if (rc == 0) return {EdgeResult::Timeout, 0, ""};
    std::int64_t t = 0;
    if (!line_->read_event(&t)) return {EdgeResult::Timeout, 0, ""};
    return {EdgeResult::Edge, t, ""};
  }
  std::string driver() const override { return "GPIOOptoSensor"; }
  std::string backend() const override { return "gpio-cdev"; }
  int level() override { return line_ ? line_->get() : -1; }

 private:
  std::unique_ptr<hw::GpioLine> line_;
  std::atomic<bool> open_{false};
};

}  // namespace

std::unique_ptr<IndexSensor> make_gpio_index_sensor(int pin) { return std::make_unique<GpioIndexSensor>(pin); }
#else
std::unique_ptr<IndexSensor> make_gpio_index_sensor(int) { return nullptr; }
#endif

// ---------------------------------------------------------------- tracker

void RevolutionTracker::reset() { *this = RevolutionTracker(); }

bool RevolutionTracker::on_edge(std::int64_t t_ns, double target_fps, double step_count, int steps_per_rev) {
  if (last_ns_ > 0) {
    double period = (t_ns - last_ns_) / 1e9;
    // Debounce: an edge closer than 20 ms, or than half the last revolution, is contact bounce or
    // noise on the sensor line, not a revolution.
    if (period < MIN_PERIOD_S || (period_s_ > 0 && period < 0.5 * period_s_)) {
      ++rejected_;
      return false;
    }
    if (steps_per_rev > 0 && last_steps_ >= 0 && std::fabs(step_count - last_steps_) < MIN_STEPS) {
      ++rejected_;
      return false;
    }
    period_s_ = period;
    if (!history_.empty()) {
      double mean = std::accumulate(history_.begin(), history_.end(), 0.0) / history_.size();
      jitter_ms_ = std::fabs(period - mean) * 1000.0;
    }
    history_.push_back(period);
    if (history_.size() > 10) history_.pop_front();
    if (last_steps_ >= 0 && steps_per_rev > 0) {
      steps_measured_ = std::fabs(step_count - last_steps_);
      steps_ratio_ = steps_measured_ / steps_per_rev;
    }
    double fps = 1.0 / period;
    bool good = target_fps > 0 && std::fabs(fps - target_fps) / target_fps < LOCK_TOLERANCE && jitter_ms_ < LOCK_JITTER_MS &&
                static_cast<int>(history_.size()) >= LOCK_HISTORY;
    if (good) {
      bad_ = 0;
      if (++good_ >= LOCK_IN) locked_ = true;
    } else {
      good_ = 0;
      if (++bad_ >= LOCK_OUT) locked_ = false;
    }
  }
  last_ns_ = t_ns;
  last_steps_ = step_count;
  ++pulses_;
  return true;
}

bool RevolutionTracker::check_silence(std::int64_t now_ns, double limit_s) {
  if (last_ns_ == 0 || (now_ns - last_ns_) / 1e9 <= limit_s) return false;
  bool was = locked_;
  locked_ = false;
  good_ = 0;
  return was;
}

double RevolutionTracker::predicted_period_s() const {
  if (history_.empty()) return period_s_;
  if (!locked_ || history_.size() < 4) {
    // While the disk accelerates every revolution is shorter than the one before: extrapolate, so
    // the revolution being emitted does not run into the next edge.
    if (history_.size() >= 2) {
      double ratio = history_.back() / history_[history_.size() - 2];
      if (ratio < 1.0) return period_s_ * std::max(ratio, 0.8);
    }
    return period_s_;
  }
  double sum = 0;
  for (std::size_t i = history_.size() - 4; i < history_.size(); ++i) sum += history_[i];
  return sum / 4.0;
}

double RevolutionTracker::jitter_rms_ms() const {
  if (history_.size() < 2) return 0.0;
  double mean = std::accumulate(history_.begin(), history_.end(), 0.0) / history_.size();
  double sq = 0;
  for (double p : history_) sq += (p - mean) * (p - mean);
  return std::sqrt(sq / (history_.size() - 1)) * 1000.0;
}

}  // namespace mtv
