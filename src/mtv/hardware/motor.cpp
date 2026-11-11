#include "mtv/hardware/motor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <time.h>

#include "mtv/common/clock.h"
#include "mtv/common/constants.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/rt.h"
#include "mtv/hardware/failsafe.h"
#ifdef MTV_LINUX
#include "mtv/hardware/linux_hw.h"
#endif

namespace mtv {

double ramp_rate_from_env() {
  std::string raw = env::trim(env::get("MTV_RAMP_RATE"));
  if (raw.empty()) return DEFAULT_RAMP_RATE;
  try {
    std::size_t used = 0;
    double v = std::stod(raw, &used);
    if (used != raw.size() || v != v) return DEFAULT_RAMP_RATE;
    return std::clamp(v, MIN_RAMP_RATE, MAX_RAMP_RATE);
  } catch (...) {
    return DEFAULT_RAMP_RATE;
  }
}

// ---------------------------------------------------------------- step accounting

StepAccount::StepAccount() { segs_.push_back({now_ns(), 0.0, 0.0}); }

void StepAccount::set_rate(double steps_per_s) {
  const std::int64_t now = now_ns();
  std::lock_guard<std::mutex> lock(m_);
  const Seg& last = segs_.back();
  double count = last.count + last.rate * (now - last.t) / 1e9;
  segs_.push_back({now, count, std::max(0.0, steps_per_s)});
  // keep about a second of history (rate changes every 5 ms while ramping)
  while (segs_.size() > 2 && now - segs_[1].t > 1'500'000'000) segs_.pop_front();
}

double StepAccount::count_at(std::int64_t t_ns) const {
  std::lock_guard<std::mutex> lock(m_);
  for (auto it = segs_.rbegin(); it != segs_.rend(); ++it) {
    if (it->t <= t_ns) return it->count + it->rate * (t_ns - it->t) / 1e9;
  }
  return segs_.front().count;   // older than the history: best effort
}

double StepAccount::count() const { return count_at(now_ns()); }

double StepAccount::rate() const {
  std::lock_guard<std::mutex> lock(m_);
  return segs_.back().rate;
}

// ---------------------------------------------------------------- mock

MockMotorDriver::MockMotorDriver(int p, int spr) : steps_per_rev_(spr) { port = p; }

void MockMotorDriver::set_rate(double steps_per_s) {
  acct_.set_rate(steps_per_s);
  if (steps_per_s > 0) released_ = false;
}

void MockMotorDriver::enable() { released_ = false; }

void MockMotorDriver::release() {
  acct_.set_rate(0.0);
  released_ = true;
}

json MockMotorDriver::telemetry() {
  return {{"driver", "MockMotorDriver"}, {"port", port},        {"step_count", static_cast<long long>(acct_.count())},
          {"released", released_.load()}, {"connected", true},  {"steps_per_rev", steps_per_rev_.load()},
          {"step_rate_hz", round_to(acct_.rate(), 1)}, {"mode", "simulated"}};
}

#ifdef MTV_LINUX
// ---------------------------------------------------------------- A4988 on hardware PWM

A4988PWMDriver::A4988PWMDriver(int dir_gpio, int enable_gpio, int microsteps, int direction, const std::string& pwm_chip,
                               int pwm_channel)
    : microsteps_(std::max(1, microsteps)), direction_(direction >= 0 ? 1 : -1), dir_gpio_(dir_gpio), en_gpio_(enable_gpio) {
  std::string chip = hw::main_gpiochip();
  std::string why;
  en_ = hw::GpioLine::output(chip, enable_gpio, "mechanical-tv-enable", 1, &why);   // start with the driver OFF
  if (!en_) throw std::runtime_error("A4988 ENABLE: " + why);
  dir_ = hw::GpioLine::output(chip, dir_gpio, "mechanical-tv-dir", direction_ > 0 ? 0 : 1, &why);
  if (!dir_) throw std::runtime_error("A4988 DIR: " + why);
  pwm_ = std::make_unique<hw::SysfsPwm>(pwm_chip, pwm_channel);
  failsafe::set_pwm_enable_file((pwm_->path() + "/enable").c_str());
  failsafe::add_line(en_->fd(), 1);   // a crash leaves the coils released
}

A4988PWMDriver::~A4988PWMDriver() {
  try {
    release();
  } catch (...) {
  }
  failsafe::remove_line(en_->fd());
  failsafe::set_pwm_enable_file(nullptr);
}

void A4988PWMDriver::set_rate(double steps_per_s) {
  std::lock_guard<std::mutex> lock(lock_);
  if (steps_per_s <= 0) {
    if (rate_ > 0) pwm_->enable(false);
    rate_ = 0.0;
    acct_.set_rate(0.0);
    return;
  }
  // >= 2 us: the A4988 needs 1 us high + 1 us low
  auto period = static_cast<std::uint64_t>(std::max(2000.0, std::nearbyint(1e9 / steps_per_s)));
  pwm_->set_period_ns(period);
  if (rate_ == 0.0) {
    en_->set(0);   // driver ON
    released_ = false;
    pwm_->enable(true);
  }
  rate_ = 1e9 / static_cast<double>(period);
  acct_.set_rate(rate_);   // the rate actually generated (quantised period)
}

void A4988PWMDriver::set_direction(int direction) {
  std::lock_guard<std::mutex> lock(lock_);
  direction_ = direction >= 0 ? 1 : -1;
  dir_->set(direction_ > 0 ? 0 : 1);
}

void A4988PWMDriver::enable() {
  std::lock_guard<std::mutex> lock(lock_);
  en_->set(0);
  released_ = false;
}

void A4988PWMDriver::release() {
  std::lock_guard<std::mutex> lock(lock_);
  if (rate_ > 0) pwm_->enable(false);
  rate_ = 0.0;
  acct_.set_rate(0.0);
  en_->set(1);   // driver OFF, coils de-energised
  released_ = true;
}

json A4988PWMDriver::telemetry() {
  std::lock_guard<std::mutex> lock(lock_);
  return {{"driver", "A4988PWMDriver"},
          {"microsteps", microsteps_.load()},
          {"steps_per_rev", 200 * microsteps_.load()},
          {"step_rate_hz", round_to(rate_, 1)},
          {"step_count", static_cast<long long>(acct_.count())},
          {"direction", direction_.load()},
          {"released", released_.load()},
          {"mode", "1/" + std::to_string(microsteps_.load()) + " microstep"},
          {"pwm", pwm_->path()},
          {"pwm_period_ns", pwm_->period_ns()},
          {"connected", true}};
}

// ---------------------------------------------------------------- Adafruit Motor HAT

namespace {
constexpr std::uint8_t MODE1 = 0x00, MODE2 = 0x01, PRESCALE = 0xFE, LED0_ON_L = 0x06;
// (coil_a, coil_b): 1 forward, -1 reverse, 0 off
constexpr int HALF_STEP[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
constexpr int FULL_STEP[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
const std::uint8_t PIN_ON[4] = {0x00, 0x10, 0x00, 0x00};    // full ON (bit 4 of ON_H)
const std::uint8_t PIN_OFF[4] = {0x00, 0x00, 0x00, 0x10};   // full OFF (bit 4 of OFF_H)

// 24 bytes for the six contiguous channels [pwm_a, in2_a, in1_a, in1_b, in2_b, pwm_b]
void step_block(int a, int b, std::uint8_t out[24]) {
  const std::uint8_t* ch[6];
  if (a > 0) { ch[0] = PIN_ON; ch[1] = PIN_OFF; ch[2] = PIN_ON; }
  else if (a < 0) { ch[0] = PIN_ON; ch[1] = PIN_ON; ch[2] = PIN_OFF; }
  else { ch[0] = PIN_OFF; ch[1] = PIN_OFF; ch[2] = PIN_OFF; }
  if (b > 0) { ch[3] = PIN_ON; ch[4] = PIN_OFF; ch[5] = PIN_ON; }
  else if (b < 0) { ch[3] = PIN_OFF; ch[4] = PIN_ON; ch[5] = PIN_ON; }
  else { ch[3] = PIN_OFF; ch[4] = PIN_OFF; ch[5] = PIN_OFF; }
  for (int i = 0; i < 6; ++i) std::copy(ch[i], ch[i] + 4, out + 4 * i);
}
}  // namespace

AdafruitMotorHATDriver::AdafruitMotorHATDriver(int p, int address, int bus) : address_(address) {
  port = p;
  base_reg_ = LED0_ON_L + 4 * (p == 1 ? 8 : 2);
  try {
    bus_ = std::make_unique<hw::I2cDev>(bus, address);
    bus_->write_byte(MODE1, 0x00);   // normal mode
    bus_->write_byte(MODE2, 0x04);   // totem pole outputs
    const int prescale = static_cast<int>(std::nearbyint(25000000.0 / (4096 * 1600.0))) - 1;   // ~1600 Hz
    std::uint8_t old = bus_->read_byte(MODE1);
    bus_->write_byte(MODE1, static_cast<std::uint8_t>((old & 0x7F) | 0x10));   // sleep to set prescale
    bus_->write_byte(PRESCALE, static_cast<std::uint8_t>(prescale));
    bus_->write_byte(MODE1, old);
    sleep_s(0.005);
    bus_->write_byte(MODE1, static_cast<std::uint8_t>(old | 0xa1));   // auto-increment + restart
  } catch (const std::exception& exc) {
    throw std::runtime_error("Could not initialize I2C bus " + std::to_string(bus) + " at 0x" + log::format("%02x", address) + ": " + exc.what());
  }
  release();
}

AdafruitMotorHATDriver::~AdafruitMotorHATDriver() {
  try {
    release();
  } catch (...) {
  }
}

void AdafruitMotorHATDriver::set_stepping_mode(bool half_step) {
  std::lock_guard<std::mutex> lock(lock_);
  half_step_ = half_step;
  step_index_ = 0;
  last_a_ = last_b_ = 99;
}

void AdafruitMotorHATDriver::step(int direction) {
  std::lock_guard<std::mutex> lock(lock_);
  const int n = half_step_ ? 8 : 4;
  step_index_ = ((step_index_ + direction) % n + n) % n;
  const int a = half_step_ ? HALF_STEP[step_index_][0] : FULL_STEP[step_index_][0];
  const int b = half_step_ ? HALF_STEP[step_index_][1] : FULL_STEP[step_index_][1];
  std::uint8_t block[24];
  step_block(a, b, block);
  released_ = false;
  if (last_a_ == 99 || (last_a_ != a && last_b_ != b)) {
    bus_->write_block(static_cast<std::uint8_t>(base_reg_), block, 24);   // first step after release: both coils
  } else if (last_a_ != a) {
    bus_->write_block(static_cast<std::uint8_t>(base_reg_), block, 12);   // coil A channels
  } else if (last_b_ != b) {
    bus_->write_block(static_cast<std::uint8_t>(base_reg_ + 12), block + 12, 12);   // coil B channels
  }
  last_a_ = a;
  last_b_ = b;
  ++count_;
}

void AdafruitMotorHATDriver::release() {
  std::lock_guard<std::mutex> lock(lock_);
  std::uint8_t block[24];
  for (int i = 0; i < 6; ++i) std::copy(PIN_OFF, PIN_OFF + 4, block + 4 * i);
  bus_->write_block(static_cast<std::uint8_t>(base_reg_), block, 24);
  last_a_ = last_b_ = 99;
  released_ = true;
}

double AdafruitMotorHATDriver::step_count() {
  std::lock_guard<std::mutex> lock(lock_);
  return static_cast<double>(count_);
}

json AdafruitMotorHATDriver::telemetry() {
  std::lock_guard<std::mutex> lock(lock_);
  return {{"driver", "AdafruitMotorHATDriver"},
          {"port", port},
          {"address", log::format("0x%x", address_)},
          {"step_count", count_},
          {"released", released_},
          {"mode", half_step_ ? "half-step" : "full-step"},
          {"steps_per_rev", half_step_ ? 400 : 200},
          {"connected", bus_ != nullptr}};
}
#endif

std::unique_ptr<MotorDriver> create_motor_driver(int port, bool force_mock, std::string* note, int microsteps) {
  const int ms = microsteps > 0 ? microsteps : std::max(1, env::get_int("MTV_MICROSTEPS", 16));
  const int mock_spr = 200 * ms;
  if (force_mock || env::flag("MTV_MOCK_HARDWARE")) return std::make_unique<MockMotorDriver>(port, mock_spr);
  // The installer's default is the A4988; the HAT must be asked for (MTV_MOTOR_DRIVER=hat).
  std::string kind = env::lower(env::trim(env::get("MTV_MOTOR_DRIVER", "a4988")));
#ifdef MTV_LINUX
  if (kind == "a4988" || kind == "stepdir" || kind == "drv8825" || kind == "tmc2209") {
    // Errors propagate: with a STEP/DIR driver selected the HAT has no motor attached.
    return std::make_unique<A4988PWMDriver>(env::get_int("MTV_DIR_GPIO", 23), env::get_int("MTV_ENABLE_GPIO", 24),
                                            ms, env::get_int("MTV_MOTOR_DIRECTION", 1),
                                            env::get("MTV_PWM_CHIP"));
  }
  if (kind != "hat" && kind != "adafruit") {
    if (note) *note = "unknown MTV_MOTOR_DRIVER=" + kind + " (want a4988 or hat)";
    return std::make_unique<MockMotorDriver>(port, mock_spr);
  }
  try {
    auto hat = std::make_unique<AdafruitMotorHATDriver>(port, 0x60);
    if (env::lower(env::trim(env::get("MTV_STEP_MODE", "half"))) == "full") hat->set_stepping_mode(false);
    return hat;
  } catch (const std::exception& exc) {
    if (note) *note = std::string("Adafruit Motor HAT unavailable: ") + exc.what();
  }
#else
  if (note) *note = "motor hardware needs Linux (MTV_MOTOR_DRIVER=" + kind + ")";
#endif
  return std::make_unique<MockMotorDriver>(port, mock_spr);
}

// ---------------------------------------------------------------- speed profile

double SpeedProfile::advance(double target, double a_max, double jerk_time, double dt) {
  double err = target - v_;
  if (std::fabs(err) < 1e-9) {
    v_ = target;
    a_ = 0.0;
    return v_;
  }
  const double jerk = a_max / std::max(1e-3, jerk_time);
  const double dir = err > 0 ? 1.0 : -1.0;
  // The largest acceleration that can still be ramped back to zero by the time the target is reached
  // (a^2 / 2j <= |err|): the speed eases into the target instead of arriving with full acceleration.
  const double cap = std::min(a_max, std::sqrt(2.0 * jerk * std::fabs(err)));
  const double desired = dir * cap;
  if (a_ < desired) a_ = std::min(desired, a_ + jerk * dt);
  else a_ = std::max(desired, a_ - jerk * dt);
  v_ += a_ * dt;
  if ((dir > 0 && v_ > target) || (dir < 0 && v_ < target)) {
    v_ = target;
    a_ = 0.0;
  }
  return v_;
}

// ---------------------------------------------------------------- controller

MotorController::MotorController(std::unique_ptr<MotorDriver> driver, double target_fps, double ramp_rate)
    : driver_(std::move(driver)), target_fps_(std::clamp(target_fps, MIN_FPS, MAX_FPS)) {
  if (!driver_) driver_ = std::make_unique<MockMotorDriver>();
  ramp_rate_ = ramp_rate > 0 ? std::clamp(ramp_rate, MIN_RAMP_RATE, MAX_RAMP_RATE) : ramp_rate_from_env();
  start_fps_ = std::clamp(env::get_double("MTV_START_FPS", DEFAULT_START_FPS), 0.001, 1.0);
  jerk_time_ = std::clamp(env::get_double("MTV_JERK_TIME", DEFAULT_JERK_TIME), 0.0, 5.0);
  settle_s_ = std::clamp(env::get_double("MTV_ENABLE_SETTLE", DEFAULT_ENABLE_SETTLE), 0.0, 2.0);
}

MotorController::~MotorController() { stop(); }

void MotorController::start() {
  std::lock_guard<std::mutex> lock(lock_);
  if (running_) return;
  if (thread_.joinable()) thread_.join();
  stop_ = false;
  failed_ = false;
  running_ = true;
  current_fps_ = 0.0;
  if (driver_->rate_driver()) {
    thread_ = std::thread([this] {
      log::set_thread_name("motor-rate");
      rate_loop();
    });
  } else {
    thread_ = std::thread([this] {
      log::set_thread_name("motor-stepper");
      step_loop();
    });
  }
}

void MotorController::start_test_rate(double steps_per_s) {
  test_rate_ = std::max(1.0, steps_per_s);
  start();
}

void MotorController::stop() {
  {
    std::lock_guard<std::mutex> lock(lock_);
    stop_ = true;
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    running_ = false;
    current_fps_ = 0.0;
    test_rate_ = 0.0;
  }
  try {
    driver_->release();
  } catch (const std::exception& exc) {
    log::error(std::string("motor release failed: ") + exc.what());
  }
}

void MotorController::set_target_fps(double fps) { target_fps_ = std::clamp(fps, MIN_FPS, MAX_FPS); }

bool MotorController::set_microsteps(int microsteps) {
  std::lock_guard<std::mutex> lock(lock_);
  if (running_) return false;
  return driver_->set_microsteps(microsteps);
}

void MotorController::set_ramp_rate(double fps_per_s) { ramp_rate_ = std::clamp(fps_per_s, MIN_RAMP_RATE, MAX_RAMP_RATE); }

void MotorController::rate_loop() {
  const int spr = driver_->steps_per_rev();
  try {
    // Energise the coils and let the rotor settle on its detent before the first step: the step
    // train then starts from a known rotor position instead of fighting a free-swinging disk.
    driver_->enable();
    double settle_until = now_s() + settle_s_;
    while (!stop_ && now_s() < settle_until) sleep_s(0.005);
    SpeedProfile profile;
    profile.reset(std::min(start_fps_, test_rate_ > 0 ? test_rate_ / spr : target_fps_.load()));
    current_fps_ = profile.velocity();
    driver_->set_rate(profile.velocity() * spr);
    double last = now_s();
    while (!stop_) {
      sleep_s(0.005);
      double now = now_s();
      double dt = std::min(0.05, now - last);
      last = now;
      double target = test_rate_ > 0 ? test_rate_ / spr : target_fps_.load();
      double v = profile.advance(target, ramp_rate_, jerk_time_, dt);
      current_fps_ = v;
      driver_->set_rate(v * spr);
    }
  } catch (const std::exception& exc) {
    {
      std::lock_guard<std::mutex> lock(failure_lock_);
      failure_ = std::string("Motor driver error: ") + exc.what();
    }
    log::error(failure_);
    failed_ = true;
  }
  try {
    driver_->set_rate(0);
  } catch (...) {
  }
  current_fps_ = 0.0;
}

void MotorController::step_loop() {
  // Software stepping (Motor HAT): realtime priority so other threads cannot delay steps.
  std::string why;
  if (!rt::set_fifo_priority(50, &why)) log::warning("motor stepper: " + why);
  const int spr = driver_->steps_per_rev();
  SpeedProfile profile;
  profile.reset(std::min(start_fps_, target_fps_.load()));
  double last_ramp = now_s();
  double t_next = now_s();
  try {
    while (!stop_) {
      double now = now_s();
      if (now - last_ramp >= 0.005) {
        double target = test_rate_ > 0 ? test_rate_ / spr : target_fps_.load();
        profile.advance(target, ramp_rate_, jerk_time_, std::min(0.05, now - last_ramp));
        last_ramp = now;
      }
      double v = std::max(profile.velocity(), 1e-3);
      current_fps_ = profile.velocity();
      double delay = std::max(0.00005, 1.0 / (v * spr));
      driver_->step(1);
      t_next += delay;
      now = now_s();
      // Below 3 FPS the rotor follows each step: drop any backlog (no bursts). Above, momentum
      // carries the rotor, so a short backlog (up to 3 ms) is caught up to stay in phase.
      double backlog = profile.velocity() < 3.0 ? delay : std::max(delay, 0.003);
      if (t_next < now - backlog) t_next = now;
      double rem = t_next - now_s();
      if (rem > 0.002) sleep_s(rem - 0.001);
      while (now_s() < t_next) {
      }
    }
  } catch (const std::exception& exc) {
    {
      std::lock_guard<std::mutex> lock(failure_lock_);
      failure_ = std::string("Motor driver error: ") + exc.what();
    }
    log::error(failure_);
    failed_ = true;
  }
  current_fps_ = 0.0;
}

std::string MotorController::failure() {
  std::lock_guard<std::mutex> lock(failure_lock_);
  return failure_;
}

json MotorController::telemetry() {
  json drv = driver_->telemetry();
  return {{"is_running", running_.load()},
          {"target_fps", target_fps_.load()},
          {"target_rpm", target_fps_.load() * 60.0},
          {"current_fps", round_to(current_fps_, 2)},
          {"current_rpm", round_to(current_fps_ * 60.0, 1)},
          {"ramp_rate_fps", ramp_rate_.load()},
          {"microsteps", driver_->microsteps()},
          {"start_fps", start_fps_},
          {"trim_us", 0.0},
          {"driver", drv}};
}

}  // namespace mtv
