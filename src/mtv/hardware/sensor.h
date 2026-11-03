// Optical index sensor (port of hardware/sensor.py, redesigned for timing accuracy).
//
// The Python app timestamped each edge inside a Python callback behind the GIL (0.5-10 ms of
// variable latency), which is what made the lock flap. Here the timestamp is the kernel's: the GPIO
// character device records CLOCK_MONOTONIC in the hard-IRQ handler, microseconds after the edge,
// and that value is used for every period, jitter and LED-phase calculation. Edges are debounced in
// user space against the predicted period (kernel debounce would move timestamping to a workqueue).
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "mtv/common/json.h"

namespace mtv {

struct EdgeResult {
  enum Kind { Edge, Timeout, Error } kind = Timeout;
  std::int64_t t_ns = 0;   // CLOCK_MONOTONIC
  std::string error;
};

class IndexSensor {
 public:
  virtual ~IndexSensor() = default;
  virtual bool open(std::string* why) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;
  // Block up to timeout_ns for the next rising edge.
  virtual EdgeResult wait(std::int64_t timeout_ns) = 0;
  // True when an edge is already queued (non-blocking). Used to end a revolution early.
  virtual bool pending() = 0;
  virtual std::string driver() const = 0;    // "GPIOOptoSensor" | "MockOptoSensor"
  virtual std::string backend() const = 0;   // "gpio-cdev" | "simulated"
  virtual bool is_mock() const { return false; }
  int pin = 4;
};

// Simulated sensor: one edge each time the (simulated) motor's integrated step count passes a
// multiple of steps_per_rev, at the exact moment it does. Tests can also inject edges.
class MockIndexSensor : public IndexSensor {
 public:
  // phase_steps shifts where in the revolution the notch sits (tests).
  MockIndexSensor(int pin, std::function<double()> step_count, std::function<double()> step_rate, int steps_per_rev,
                  double phase_steps = 0.0);
  bool open(std::string* why) override;
  void close() override;
  bool is_open() const override { return open_; }
  EdgeResult wait(std::int64_t timeout_ns) override;
  bool pending() override;
  std::string driver() const override { return "MockOptoSensor"; }
  std::string backend() const override { return "simulated"; }
  bool is_mock() const override { return true; }
  void inject(std::int64_t t_ns);   // queue an edge (tests)
  void set_motor_coupled(bool on) { coupled_ = on; }

 private:
  std::function<double()> count_, rate_;
  int spr_;
  double phase_ = 0.0;
  std::atomic<bool> open_{false};
  std::atomic<bool> coupled_{true};
  double next_boundary_ = 0;
  std::mutex lock_;
  std::deque<std::int64_t> injected_;
};

std::unique_ptr<IndexSensor> make_gpio_index_sensor(int pin);   // nullptr when not on Linux

// Per-revolution statistics and sync lock with hysteresis.
class RevolutionTracker {
 public:
  static constexpr double MIN_PERIOD_S = 0.02;   // absolute debounce: 50 rev/s
  static constexpr double LOCK_TOLERANCE = 0.03; // measured FPS within 3 % of the target
  static constexpr double LOCK_JITTER_MS = 2.0;  // and jitter below 2 ms
  static constexpr int LOCK_HISTORY = 5;         // after at least 5 periods
  static constexpr int LOCK_IN = 3;              // consecutive good revolutions to lock
  static constexpr int LOCK_OUT = 2;             // consecutive bad revolutions to unlock

  void reset();
  // Returns true when the edge was accepted (false: bounce or duplicate).
  bool on_edge(std::int64_t t_ns, double target_fps, double step_count, int steps_per_rev);
  // Drop the lock after a silence longer than `limit_s`; returns true if the lock was dropped.
  bool check_silence(std::int64_t now_ns, double limit_s);

  long pulse_count() const { return pulses_; }
  long rejected() const { return rejected_; }
  bool locked() const { return locked_; }
  bool have_period() const { return period_s_ > 0; }
  double period_s() const { return period_s_; }
  double predicted_period_s() const;   // mean of the last 4 periods when locked, else the last one
  double fps() const { return period_s_ > 0 ? 1.0 / period_s_ : 0.0; }
  double jitter_ms() const { return jitter_ms_; }
  double jitter_rms_ms() const;
  std::int64_t last_edge_ns() const { return last_ns_; }
  double steps_ratio() const { return steps_ratio_; }       // measured steps per rev / expected (1.0 = in step)
  double steps_per_rev_measured() const { return steps_measured_; }

 private:
  long pulses_ = 0;
  long rejected_ = 0;
  std::int64_t last_ns_ = 0;
  double last_steps_ = -1;
  double period_s_ = 0;
  double jitter_ms_ = 0;
  double steps_ratio_ = 0;
  double steps_measured_ = 0;
  std::deque<double> history_;   // last 10 periods
  bool locked_ = false;
  int good_ = 0;
  int bad_ = 0;
};

}  // namespace mtv
