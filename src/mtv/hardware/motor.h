// Stepper drivers and the speed manager (port of hardware/motor.py, with a new start-up profile).
//
// Why the profile changed: the Python app enabled the driver and jumped from standstill straight to
// 0.5 rev/s. With the 1.75 mm disk (about 350x the rotor's inertia) that step needs ~0.7 A of drive to
// survive, so any current shortfall stalls a start that worked with the 0.6 mm disk. Here the driver
// is enabled first and allowed to settle, the step train starts at a crawl (MTV_START_FPS, default
// 0.02 rev/s) and the speed follows a jerk-limited S-curve to the target, updated every 5 ms.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mtv/common/json.h"

namespace mtv {

inline constexpr double DEFAULT_RAMP_RATE = 2.0;        // FPS gained (or shed) per second
inline constexpr double MIN_RAMP_RATE = 0.1, MAX_RAMP_RATE = 50.0;
inline constexpr double DEFAULT_START_FPS = 0.02;       // first step rate after enable (rev/s)
inline constexpr double DEFAULT_JERK_TIME = 0.5;        // seconds to reach full acceleration
inline constexpr double DEFAULT_ENABLE_SETTLE = 0.2;    // seconds the coils hold before the first step

// MTV_RAMP_RATE in FPS per second, clamped to [0.1, 50]; the default when unset, malformed or NaN.
double ramp_rate_from_env();

// Integrated step count of a rate-driven motor, queryable at any recent instant. Rate changes are
// kept as segments for about a second, so the count at an index edge's kernel timestamp is exact
// even when the edge is processed late. Counts are magnitudes (direction does not matter).
class StepAccount {
 public:
  StepAccount();
  void set_rate(double steps_per_s);
  double count_at(std::int64_t t_ns) const;
  double count() const;
  double rate() const;

 private:
  struct Seg {
    std::int64_t t;
    double count;
    double rate;
  };
  mutable std::mutex m_;
  std::deque<Seg> segs_;
};

class MotorDriver {
 public:
  virtual ~MotorDriver() = default;
  // Rate drivers (A4988 on hardware PWM, the mock) take a step rate; step drivers (the HAT) take
  // individual steps from a timing thread.
  virtual bool rate_driver() const = 0;
  virtual void set_rate(double steps_per_s) {}
  virtual void step(int direction) {}
  virtual void enable() {}            // energise the coils (A4988 ENABLE low)
  virtual void release() = 0;         // stop stepping, de-energise
  virtual int steps_per_rev() const = 0;
  virtual double step_count() = 0;    // integrated steps since start (fractional for rate drivers)
  // Steps commanded up to instant t_ns (CLOCK_MONOTONIC); drivers without a history return step_count().
  virtual double step_count_at(std::int64_t t_ns) { return step_count(); }
  virtual std::string name() const = 0;   // "A4988PWMDriver", "AdafruitMotorHATDriver", "MockMotorDriver"
  virtual bool is_mock() const { return false; }
  virtual json telemetry() = 0;
  int port = 1;
};

class MockMotorDriver : public MotorDriver {
 public:
  explicit MockMotorDriver(int port = 1, int steps_per_rev = 3200);
  bool rate_driver() const override { return true; }
  void set_rate(double steps_per_s) override;
  void enable() override;
  void release() override;
  int steps_per_rev() const override { return steps_per_rev_; }
  double step_count() override { return acct_.count(); }
  double step_count_at(std::int64_t t_ns) override { return acct_.count_at(t_ns); }
  double rate() const { return acct_.rate(); }
  std::string name() const override { return "MockMotorDriver"; }
  bool is_mock() const override { return true; }
  json telemetry() override;

 private:
  StepAccount acct_;
  int steps_per_rev_;
  std::atomic<bool> released_{true};
};

#ifdef MTV_LINUX
namespace hw { class GpioLine; class SysfsPwm; class I2cDev; }

// STEP/DIR driver (A4988, DRV8825, TMC2209) clocked by the Pi's hardware PWM: STEP pulses come
// from the PWM peripheral, so the step rate is exact; software only changes the rate.
class A4988PWMDriver : public MotorDriver {
 public:
  A4988PWMDriver(int dir_gpio, int enable_gpio, int microsteps, int direction, const std::string& pwm_chip, int pwm_channel = 0);
  ~A4988PWMDriver() override;
  bool rate_driver() const override { return true; }
  void set_rate(double steps_per_s) override;
  void set_direction(int direction);
  void enable() override;
  void release() override;
  int steps_per_rev() const override { return 200 * microsteps_; }
  double step_count() override { return acct_.count(); }
  double step_count_at(std::int64_t t_ns) override { return acct_.count_at(t_ns); }
  std::string name() const override { return "A4988PWMDriver"; }
  json telemetry() override;
  int microsteps() const { return microsteps_; }

 private:
  std::mutex lock_;   // serialises device I/O; never taken by the realtime thread
  StepAccount acct_;
  int microsteps_;
  std::atomic<int> direction_;
  double rate_ = 0.0;
  std::atomic<bool> released_{true};
  std::unique_ptr<hw::GpioLine> dir_, en_;
  std::unique_ptr<hw::SysfsPwm> pwm_;
  int dir_gpio_, en_gpio_;
};

// Adafruit DC & Stepper Motor HAT (PCA9685 at 0x60 driving TB6612 bridges), half-step by default.
// Each step rewrites only the coil that changed (one I2C block write).
class AdafruitMotorHATDriver : public MotorDriver {
 public:
  explicit AdafruitMotorHATDriver(int port = 1, int address = 0x60, int bus = 1);
  ~AdafruitMotorHATDriver() override;
  bool rate_driver() const override { return false; }
  void step(int direction) override;
  void release() override;
  void set_stepping_mode(bool half_step);
  int steps_per_rev() const override { return half_step_ ? 400 : 200; }
  double step_count() override;
  std::string name() const override { return "AdafruitMotorHATDriver"; }
  json telemetry() override;

 private:
  void write_both(int index);
  std::mutex lock_;
  std::unique_ptr<hw::I2cDev> bus_;
  int address_;
  int base_reg_;
  bool half_step_ = true;
  int step_index_ = 0;
  long count_ = 0;
  bool released_ = true;
  int last_a_ = 99, last_b_ = 99;   // 99 = unknown / released
};
#endif

// Factory: MTV_MOCK_HARDWARE=1 or force_mock -> mock; MTV_MOTOR_DRIVER=a4988|stepdir|drv8825|tmc2209
// -> A4988 (errors propagate: with this selected the HAT has no motor); otherwise try the HAT, then
// fall back to the mock with *note set to the reason.
std::unique_ptr<MotorDriver> create_motor_driver(int port, bool force_mock, std::string* note = nullptr);

// Jerk-limited speed profile shared by the rate thread and the HAT stepping thread.
class SpeedProfile {
 public:
  void reset(double start_fps) { v_ = start_fps; a_ = 0.0; }
  // Advance by dt toward target with max acceleration a_max (FPS/s) and jerk a_max / jerk_time.
  double advance(double target, double a_max, double jerk_time, double dt);
  double velocity() const { return v_; }
  double acceleration() const { return a_; }

 private:
  double v_ = 0.0;
  double a_ = 0.0;
};

class MotorController {
 public:
  explicit MotorController(std::unique_ptr<MotorDriver> driver, double target_fps = 10.0, double ramp_rate = -1.0);
  ~MotorController();

  void start();   // enable, settle, ramp from MTV_START_FPS
  void stop();    // stop stepping and release the coils (the disk coasts down)
  void set_target_fps(double fps);   // clamped to [1, 24]
  // Run at an exact low step rate for the microstep self-test (bypasses the FPS clamp).
  void start_test_rate(double steps_per_s);
  bool is_running() const { return running_; }
  double target_fps() const { return target_fps_; }
  double current_fps() const { return current_fps_; }
  double ramp_rate() const { return ramp_rate_; }
  double start_fps() const { return start_fps_; }
  bool ramp_complete() const { return running_ && current_fps_ >= target_fps_ - 1e-9 && current_fps_ <= target_fps_ + 1e-9; }
  MotorDriver& driver() { return *driver_; }
  double step_count() { return driver_->step_count(); }
  double step_count_at(std::int64_t t_ns) { return driver_->step_count_at(t_ns); }
  // The motor thread stopped on a driver error (the controller reports it and stops).
  bool failed() const { return failed_; }
  std::string failure();
  int steps_per_rev() const { return driver_->steps_per_rev(); }
  json telemetry();

 private:
  void rate_loop();
  void step_loop();
  std::unique_ptr<MotorDriver> driver_;
  std::atomic<double> target_fps_;
  std::atomic<double> current_fps_{0.0};
  std::atomic<double> ramp_rate_;
  double start_fps_ = DEFAULT_START_FPS;
  double jerk_time_ = DEFAULT_JERK_TIME;
  double settle_s_ = DEFAULT_ENABLE_SETTLE;
  std::atomic<double> test_rate_{0.0};   // >0: fixed step rate (self-test)
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::mutex lock_;
  std::mutex failure_lock_;
  std::string failure_;
  std::thread thread_;
};

}  // namespace mtv
