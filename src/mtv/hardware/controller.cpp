#include "mtv/hardware/controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <stdexcept>

#include "mtv/common/clock.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/rt.h"

namespace mtv {

namespace {
constexpr double MIN_LED_FPS = 0.9;          // the LED stays dark below this measured speed
constexpr double STALL_ARM_FPS = 0.3;        // the no-pulse check runs once the commanded speed exceeds this
constexpr double SLIP_TOLERANCE = 0.08;      // measured/expected steps per revolution
constexpr int SLIP_REVS = 3;                 // consecutive off-ratio revolutions that count as a slip
constexpr double WATCHDOG_S = 0.35;          // thermal/stall interlock: LED off this long after the last pulse
constexpr std::int64_t MAX_WAIT_NS = 50'000'000;

// The disk follows the motor: steps per revolution between the last two notches match the setting.
bool in_step(double steps_ratio) { return steps_ratio > 0 && std::fabs(steps_ratio - 1.0) <= SLIP_TOLERANCE; }

// Length of the revolution that starts now, from the motor's speed `v` (rev/s) and the ramp
// acceleration `a` (rev/s^2, negative when slowing): solves v*T + a*T^2/2 = 1, with the speed held at
// `target` once it is reached.
double revolution_from_motor(double v, double target, double a) {
  if (v <= 0) return 0.0;
  if (std::fabs(target - v) < 1e-6 || a == 0.0) return 1.0 / v;
  a = target > v ? std::fabs(a) : -std::fabs(a);
  const double t_reach = (target - v) / a;   // time until the ramp reaches the target
  const double d_reach = v * t_reach + 0.5 * a * t_reach * t_reach;
  if (d_reach >= 1.0) {
    const double disc = v * v + 2.0 * a;
    if (disc > 0) return (std::sqrt(disc) - v) / a;
    return 1.0 / v;
  }
  return t_reach + (1.0 - d_reach) / target;
}

int nearest_microstep(double measured_steps_per_rev) {
  static const int options[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
  double ms = measured_steps_per_rev / 200.0;
  int best = 1;
  for (int o : options)
    if (std::fabs(o - ms) < std::fabs(best - ms)) best = o;
  return best;
}
}  // namespace

RealtimeController::RealtimeController(FramePipeline& pipeline, const ControllerOptions& o)
    : pipeline_(pipeline), target_fps_(std::clamp(o.target_fps, MIN_FPS, MAX_FPS)) {
  std::string note;
  auto driver = create_motor_driver(o.motor_port, o.force_mock, &note, o.microsteps);   // A4988 errors propagate
  if (!note.empty()) notes_.push_back(note);
  bool mock_motor = driver->is_mock();
  motor_ = std::make_unique<MotorController>(std::move(driver), target_fps_, o.ramp_rate);
  if (mock_motor || o.force_mock) {
    MotorController* m = motor_.get();
    auto* mock = dynamic_cast<MockMotorDriver*>(&m->driver());
    sensor_ = std::make_unique<MockIndexSensor>(
        o.opto_pin, [m] { return m->step_count(); },
        [m, mock] { return mock ? mock->rate() : m->current_fps() * m->steps_per_rev(); }, m->steps_per_rev());
  } else {
    sensor_ = make_gpio_index_sensor(o.opto_pin);
    if (!sensor_) {
      notes_.push_back("index sensor needs Linux");
      MotorController* m = motor_.get();
      sensor_ = std::make_unique<MockIndexSensor>(o.opto_pin, [m] { return m->step_count(); },
                                                  [m] { return m->current_fps() * m->steps_per_rev(); }, m->steps_per_rev());
    }
  }
  std::string led_note;
  led_ = create_led_output(o.led_pin, o.use_spi_led || env::flag("MTV_SPI_LED"), o.force_mock || mock_motor, &led_note);
  if (!led_note.empty()) notes_.push_back(led_note);
  launch();
}

RealtimeController::RealtimeController(FramePipeline& pipeline, std::unique_ptr<MotorDriver> motor,
                                       std::unique_ptr<IndexSensor> sensor, std::unique_ptr<LedOutput> led, double target_fps)
    : pipeline_(pipeline),
      motor_(std::make_unique<MotorController>(std::move(motor), target_fps)),
      sensor_(std::move(sensor)),
      led_(std::move(led)),
      target_fps_(std::clamp(target_fps, MIN_FPS, MAX_FPS)) {
  launch();
}

RealtimeController::~RealtimeController() { close(); }

void RealtimeController::launch() {
  stall_detect_ = env::get("MTV_STALL_DETECT", "1") != "0";
  // The motor runs on its own clock and keeps turning whatever the sensor sees. MTV_STALL_STOP=1
  // brings back stopping the motor on missing or mistimed index pulses.
  stall_stop_ = stall_detect_ && env::get("MTV_STALL_STOP", "0") == "1";
  guard_ns_ = static_cast<std::int64_t>(std::clamp(env::get_double("MTV_LED_GUARD_US", 150.0), 0.0, 5000.0) * 1000.0);
  rt_ = std::thread([this] {
    log::set_thread_name("realtime-led");
    rt_loop();
  });
}

std::string RealtimeController::state() {
  if (testing_) return "testing";
  if (running_ && motor_test_) return "motor test";
  if (!running_) {
    std::lock_guard<std::mutex> lock(stats_lock_);
    return stalled_ ? "error" : "idle";
  }
  bool locked;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    locked = tracker_.locked();
  }
  double cur = motor_->current_fps(), tgt = target_fps_;
  if (cur >= tgt * 0.95 && locked) return "locked";
  return cur < tgt * 0.9 ? "ramping" : "running";
}

void RealtimeController::start() {
  if (testing_) throw std::invalid_argument("The microstep check is running; wait for it to finish.");
  if (running_ && motor_test_) throw std::invalid_argument("The motor test is running; stop it first.");
  if (running_) return;
  set_led_test("off");   // the index sensor drives the LED from here on
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    tracker_.reset();
    error_.clear();
    warning_.clear();
    stalled_ = false;
    slip_revs_ = 0;
    safety_tripped_ = false;
  }
  started_at_ = now_s();
  motor_->set_target_fps(target_fps_);
  inhibit_ = false;
  running_ = true;
  motor_->start();
}

void RealtimeController::stop() {
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    abort_test_ = true;
    test_cv_.notify_all();
  }
  running_ = false;
  if (!free_run_) inhibit_ = true;
  led_->blank();
  motor_->stop();
  motor_test_ = false;
  led_->blank();
}

void RealtimeController::emergency_blank() {
  inhibit_ = true;
  free_run_ = false;
  led_test_ = 0;
  led_test_abort_ = true;
  led_->blank();
  stop();
  led_->blank();
}

void RealtimeController::set_free_run(bool enabled) {
  if (enabled == free_run_) return;
  if (enabled) {
    if (motor_test()) throw std::invalid_argument("The motor test keeps the LED dark; stop it before turning free-run on.");
    set_led_test("off");
    inhibit_ = false;
    free_run_ = true;
    return;
  }
  free_run_ = false;
  if (!running_) inhibit_ = true;
  led_->blank();   // the index sensor relights it on the next pulse when the motor is running
}

// ---------------------------------------------------------------- bench tests

void RealtimeController::start_motor_test() {
  if (testing_) throw std::invalid_argument("The microstep check is running; wait for it to finish.");
  if (running_) {
    if (motor_test_) return;
    throw std::invalid_argument("The motor is already running; stop it before the motor test.");
  }
  if (free_run_) throw std::invalid_argument("Free-run lights the LED; turn it off before the motor-only test.");
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    tracker_.reset();
    error_.clear();
    warning_.clear();
    stalled_ = false;
    slip_revs_ = 0;
    safety_tripped_ = false;
  }
  started_at_ = now_s();
  motor_->set_target_fps(target_fps_);
  inhibit_ = true;   // the index sensor never drives the LED in this test
  motor_test_ = true;
  running_ = true;
  motor_->start();
  log::info(log::format("motor test: motor only, %.2f FPS at %d steps per revolution; LED off, no stall checks",
                        target_fps_.load(), motor_->steps_per_rev()));
}

void RealtimeController::set_sensor_watch(bool on) {
  if (on && !sensor_watch_) {
    std::lock_guard<std::mutex> lock(stats_lock_);
    raw_edges_.clear();
    raw_edge_count_ = 0;
    if (!running_ && !testing_) tracker_.reset();
  }
  sensor_watch_ = on;
  log::info(std::string("sensor watch ") + (on ? "on" : "off"));
}

void RealtimeController::set_led_test(const std::string& mode, double level) {
  if (mode == "off") {
    if (led_test_.exchange(0) != 0) {
      led_test_abort_ = true;
      led_->blank();
      log::info("LED test off");
    }
    return;
  }
  int m = mode == "steady" ? 1 : mode == "blink" ? 2 : 0;
  if (!m) throw std::invalid_argument("LED test mode must be off, steady or blink.");
  if (running_ && !motor_test_)
    throw std::invalid_argument("The index sensor drives the LED while the motor runs; stop the motor (or use the motor test) first.");
  if (free_run_) throw std::invalid_argument("Free-run is driving the LED; turn it off before the LED test.");
  led_test_level_ = std::clamp(level, 0.0, 1.0);
  led_test_started_ = now_s();
  led_test_abort_ = false;
  led_test_ = m;
  log::info(log::format("LED test: %s at %.0f%%, off by itself after %.0f s", mode.c_str(), led_test_level_.load() * 100,
                        LED_TEST_MAX_S));
}

std::string RealtimeController::led_test() const {
  switch (led_test_.load()) {
    case 1: return "steady";
    case 2: return "blink";
    default: return "off";
  }
}

bool RealtimeController::led_test_active() {
  if (!led_test_) return false;
  if ((running_ && !motor_test_) || free_run_) return false;
  if (now_s() - led_test_started_ > LED_TEST_MAX_S) {
    led_test_ = 0;
    led_test_abort_ = true;
    led_->blank();
    log::info(log::format("LED test ended after %.0f s (safety limit)", LED_TEST_MAX_S));
    return false;
  }
  return true;
}

void RealtimeController::led_test_slice() {
  constexpr std::int64_t SLICE_NS = 10'000'000;
  const bool on = led_test_ == 1 || std::fmod(now_s() - led_test_started_, 1.0) < 0.5;
  const std::int64_t t0 = now_ns();
  if (on) {
    NipkowStream stream;
    stream.fill(static_cast<std::uint8_t>(std::lround(255.0 * led_test_level_)));
    led_->output(stream, t0, SLICE_NS, t0 + SLICE_NS, led_test_abort_, nullptr);
  } else if (!led_->blanked) {
    led_->blank();
  }
  const std::int64_t left = t0 + SLICE_NS - now_ns();
  if (left > 0) sleep_s(left / 1e9);
}

json RealtimeController::bench_status() {
  std::deque<RawEdge> edges;
  long count;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    edges = raw_edges_;
    count = raw_edge_count_;
  }
  const std::int64_t now = now_ns();
  json recent = json::array();   // the last 5 s, for the timeline
  long last_second = 0, accepted = 0;
  for (const auto& e : edges) {
    double age = (now - e.t_ns) / 1e9;
    if (age <= 1.0) ++last_second;
    if (e.accepted) ++accepted;
    if (age <= 5.0) recent.push_back({{"age_s", round_to(age, 4)}, {"accepted", e.accepted}});
  }
  json last = json::array();   // newest first: time and motor steps since the edge before it
  for (std::size_t i = edges.size(); i-- > 0 && last.size() < 12;) {
    json row = {{"accepted", edges[i].accepted}, {"age_s", round_to((now - edges[i].t_ns) / 1e9, 2)}};
    if (i > 0) {
      row["dt_ms"] = round_to((edges[i].t_ns - edges[i - 1].t_ns) / 1e6, 2);
      row["steps"] = std::round(std::fabs(edges[i].steps - edges[i - 1].steps));
    }
    last.push_back(row);
  }
  json revs = json::array();   // newest first: one row per accepted edge pair
  const RawEdge* next = nullptr;
  for (std::size_t i = edges.size(); i-- > 0 && revs.size() < 10;) {
    if (!edges[i].accepted) continue;
    if (next) {
      double period = (next->t_ns - edges[i].t_ns) / 1e9;
      revs.push_back({{"period_ms", round_to(period * 1000, 2)},
                      {"fps", round_to(period > 0 ? 1.0 / period : 0.0, 2)},
                      {"steps", std::round(std::fabs(next->steps - edges[i].steps))}});
    }
    next = &edges[i];
  }
  return {{"motor_test", motor_test()},
          {"sensor_watch", sensor_watch_.load()},
          {"sensor_open", sensor_->is_open()},
          {"sensor_level", sensor_level_.load()},
          {"led_test", led_test()},
          {"led_test_level", round_to(led_test_level_, 2)},
          {"led_test_left_s", led_test_ ? round_to(std::max(0.0, LED_TEST_MAX_S - (now_s() - led_test_started_)), 1) : 0.0},
          {"edges_total", count},
          {"edges_shown", edges.size()},
          {"edges_accepted", accepted},
          {"edges_last_second", last_second},
          {"recent", recent},
          {"last_edges", last},
          {"revolutions", revs}};
}

void RealtimeController::set_target_fps(double fps) {
  target_fps_ = std::clamp(fps, MIN_FPS, MAX_FPS);
  motor_->set_target_fps(target_fps_);
}

void RealtimeController::set_microsteps(int microsteps) {
  static const int allowed[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
  if (std::find(std::begin(allowed), std::end(allowed), microsteps) == std::end(allowed))
    throw std::invalid_argument("Microsteps must be 1, 2, 4, 8, 16, 32, 64, 128 or 256.");
  if (running_ || testing_) throw std::invalid_argument("Stop the motor before changing the microstep setting.");
  if (!motor_->set_microsteps(microsteps))
    throw std::invalid_argument("The " + motor_->driver().name() +
                                " has a fixed step mode; the microstep setting applies to STEP/DIR drivers (A4988, DRV8825, TMC2209).");
  if (auto* mock = dynamic_cast<MockIndexSensor*>(sensor_.get())) mock->set_steps_per_rev(motor_->steps_per_rev());
  std::lock_guard<std::mutex> lock(stats_lock_);
  tracker_.reset();
  slip_revs_ = 0;
}

void RealtimeController::set_ramp_rate(double fps_per_s) { motor_->set_ramp_rate(fps_per_s); }

void RealtimeController::set_source_mode(const std::string& mode, const std::string& library_path) {
  pipeline_.set_source_mode(mode, library_path);
}

void RealtimeController::set_pattern(const std::string& pattern) { pipeline_.pattern_gen().update_pattern(pattern); }

void RealtimeController::set_calibration(const Calibration& cal) { pipeline_.set_calibration(cal); }

void RealtimeController::open_sensor_if_needed() {
  if (sensor_->is_open()) return;
  std::string why;
  bool ok = sensor_->open(&why);
  std::string report;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    if (ok) {
      sensor_error_.clear();
    } else if (sensor_error_ != why) {
      sensor_error_ = why;
      error_ = "Index sensor unavailable: " + why;
      report = error_;
    }
  }
  if (!report.empty()) log::error(report);
}

void RealtimeController::close_sensor_if_open() {
  if (sensor_->is_open()) sensor_->close();
}

void RealtimeController::halt(const std::string& reason) {
  if (!free_run_) inhibit_ = true;
  led_->blank();
  motor_->stop();   // coils released before the state reads "error"
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    stalled_ = true;
    error_ = reason;
  }
  running_ = false;
  log::error(reason);
}

void RealtimeController::on_edge(std::int64_t t_ns) {
  // The steps commanded up to the edge's own timestamp, not up to now: the edge may be processed
  // tens of milliseconds late while the previous revolution is still being emitted.
  double steps = motor_->step_count_at(t_ns);
  int spr = motor_->steps_per_rev();
  std::string halt_reason, warn, cleared;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    // With the motor stopped (sensor watch, disk turned by hand) the step count stands still, so the
    // step debounce would reject every edge: it applies only while the motor is turning.
    bool accepted = tracker_.on_edge(t_ns, testing_ || motor_test_ ? 0.0 : target_fps_.load(), steps,
                                     motor_->is_running() ? spr : 0);
    raw_edges_.push_back({t_ns, steps, accepted});
    if (raw_edges_.size() > 400) raw_edges_.pop_front();
    ++raw_edge_count_;
    if (accepted && testing_) {
      test_edges_.emplace_back(t_ns, steps);
      test_cv_.notify_all();
    }
    safety_tripped_ = false;
    if (accepted && running_ && !testing_ && !motor_test_ && tracker_.steps_ratio() > 0 && stall_detect_) {
      double ratio = tracker_.steps_ratio();
      slip_revs_ = in_step(ratio) ? 0 : slip_revs_ + 1;
      if (slip_revs_ >= SLIP_REVS) {
        int configured = spr / 200;
        double measured = tracker_.steps_per_rev_measured();
        const char* outcome = stall_stop_ ? "Motor stopped" : "Disk not following the motor (LED timed from the index sensor)";
        std::string reason;
        if (ratio < 0.75) {
          int driver_ms = nearest_microstep(spr * ratio);
          reason = log::format(
              "%s: the disk turns %.1fx faster than commanded (%.0f of %d steps per revolution). The driver "
              "appears to be at 1/%d microstepping while the console is set to 1/%d: set MS1, MS2 and MS3 high on the "
              "A4988 for 1/16, or choose 1/%d under Microsteps in the console.",
              outcome, 1.0 / ratio, measured, spr, driver_ms, configured, driver_ms);
        } else {
          reason = log::format(
              "%s: the notch passed the sensor every %.0f motor steps, %d expected, for %d revolutions. Either the "
              "rotor is slipping (check the A4988 current (Vref), the supply voltage, the rim rubbing in the sensor "
              "slot, or lower the spin-up ramp) or the microstep jumpers do not match the console setting.",
              outcome, measured, spr, SLIP_REVS);
        }
        if (stall_stop_) halt_reason = reason;
        else if (warning_ != reason) warn = warning_ = reason;
      } else if (slip_revs_ == 0 && !warning_.empty()) {
        warning_.clear();
        cleared = log::format("Index sensor: the notch is in step with the motor again (%.0f steps per revolution); "
                              "the LED is timed from the motor again.", tracker_.steps_per_rev_measured());
      }
    }
  }
  if (!warn.empty()) log::warning(warn);
  if (!cleared.empty()) log::info(cleared);
  if (!halt_reason.empty()) halt(halt_reason);
}

void RealtimeController::drive(std::int64_t t0, std::int64_t period_ns, bool free_run) {
  auto frame = pipeline_.get_latest_frame();
  auto transform = pipeline_.transform();
  NipkowStream stream{};
  serialize_nipkow(*transform, *frame, stream);
  std::int64_t guard = guard_ns_;
  LedOutput::EdgeProbe probe;
  if (!free_run) {
    bool locked;
    {
      std::lock_guard<std::mutex> lock(stats_lock_);
      locked = tracker_.locked();
    }
    // Before lock the period is still changing: leave more room before the predicted edge.
    if (!locked) guard = std::max<std::int64_t>(guard, period_ns * 3 / 100);
    probe = [this] { return sensor_->is_open() && sensor_->pending(); };
  }
  guard = std::min<std::int64_t>(guard, period_ns / 4);
  led_->output(stream, t0, period_ns, t0 + period_ns - guard, inhibit_, probe);
  ++frame_counter_;
  if (free_run) ++free_run_frames_;
  std::lock_guard<std::mutex> lock(stats_lock_);
  preview_ = frame;
  preview_at_ = now_s();
}

void RealtimeController::rt_loop() {
  json info = json::object();
  std::string why;
  int prio = std::clamp(env::get_int("MTV_RT_PRIORITY", 60), 1, 99);
  bool fifo = rt::set_fifo_priority(prio, &why);
  info["scheduling"] = fifo ? "SCHED_FIFO " + std::to_string(prio) : "normal (" + why + ")";
  if (!fifo && !hardware_mock()) log::warning("realtime LED thread: " + why);
  int cpu = rt::realtime_cpu();
  std::string pin_why;
  bool pinned = cpu >= 0 && rt::pin_to_cpu(cpu, &pin_why);
  info["cpu"] = pinned ? json(cpu) : json(nullptr);
  info["isolated_cpus"] = rt::isolated_cpus();
  if (!hardware_mock()) {
    log::info("realtime LED thread: " + info["scheduling"].get<std::string>() + ", CPU " +
              (pinned ? std::to_string(cpu) : std::string("not pinned")) + ", isolated CPUs: " +
              (info["isolated_cpus"].get<std::string>().empty() ? std::string("none") : info["isolated_cpus"].get<std::string>()) +
              ", LED " + led_->backend() + ", index " + sensor_->backend());
  }
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    rt_info_ = info;
  }
  std::int64_t next_tick = 0;
  std::int64_t armed_at = 0;
  std::string last_sensor_error;
  bool led_test_was_on = false;
  while (!quit_) {
    const bool index_mode = running_ || testing_ || sensor_watch_;
    if (index_mode) open_sensor_if_needed();
    else close_sensor_if_open();
    // The line level, for the Tests tab; not sampled in the normal run (no ioctl on the edge path).
    if (sensor_watch_ || motor_test_) sensor_level_ = sensor_->is_open() ? sensor_->level() : -1;
    else sensor_level_ = -1;
    const bool fr = free_run_;
    const bool led_test = led_test_active();
    if (led_test_was_on && !led_test && !fr) led_->blank();
    led_test_was_on = led_test;
    if (led_test) led_test_slice();   // ~10 ms, then the sensor is drained without waiting
    if (!index_mode && !fr) {
      if (led_test) continue;
      if (!led_->blanked) led_->blank();
      next_tick = 0;
      armed_at = 0;
      sleep_s(0.02);
      continue;
    }
    std::int64_t now = now_ns();
    std::int64_t wait_ns = MAX_WAIT_NS;
    if (fr) {
      if (next_tick == 0) next_tick = now;
      wait_ns = std::clamp<std::int64_t>(next_tick - now, 0, MAX_WAIT_NS);
    } else {
      next_tick = 0;
    }
    if (led_test) wait_ns = 0;
    EdgeResult r;
    if (index_mode && sensor_->is_open()) {
      r = sensor_->wait(wait_ns);
    } else {
      if (wait_ns > 0) sleep_s(wait_ns / 1e9);
      r.kind = EdgeResult::Timeout;
    }
    if (r.kind == EdgeResult::Edge) {
      double lat = (now_ns() - r.t_ns) / 1000.0;
      edge_latency_us_ = lat;
      if (lat > edge_latency_max_us_) edge_latency_max_us_ = lat;
      on_edge(r.t_ns);
      // An LED test slice held the thread: take every edge that queued up meanwhile.
      for (int n = 0; led_test && n < 64 && sensor_->pending(); ++n) {
        EdgeResult more = sensor_->wait(0);
        if (more.kind != EdgeResult::Edge) break;
        on_edge(more.t_ns);
      }
      if (!fr && running_ && !inhibit_ && !motor_test_) {
        // The notch fixes where the revolution starts. While the disk follows the motor (the last
        // revolution took the expected number of steps) the motor's speed says how long it lasts;
        // when it does not (disk slipping on the shaft, lost steps, wrong microstep setting) the
        // measured notch-to-notch period does, so there is still a picture and a warning.
        bool follows;
        double measured = 0.0;
        {
          std::lock_guard<std::mutex> lock(stats_lock_);
          follows = tracker_.have_period() && in_step(tracker_.steps_ratio());
          if (tracker_.have_period()) measured = tracker_.predicted_period_s();
        }
        double period = follows ? revolution_from_motor(motor_->current_fps(), motor_->target_fps(), motor_->ramp_rate())
                                : measured;
        if (period > 0 && 1.0 / period >= MIN_LED_FPS) drive(r.t_ns, static_cast<std::int64_t>(period * 1e9), false);
      }
    } else if (r.kind == EdgeResult::Error) {
      if (r.error != last_sensor_error) log::error("index sensor: " + r.error);
      last_sensor_error = r.error;
      sleep_s(0.05);
    }
    now = now_ns();
    if (fr && next_tick && now >= next_tick) {
      const auto period = static_cast<std::int64_t>(1e9 / std::max(1.0, target_fps_.load()));
      if (!inhibit_) drive(next_tick, period, true);
      next_tick += period;
      if (next_tick < now_ns()) next_tick = now_ns();   // fell behind: resume from now, never burst
    }
    if (!index_mode) continue;
    // Interlock and stall detection from the time since the last real pulse.
    now = now_ns();
    double cmd = motor_->current_fps();
    std::int64_t last_edge;
    double pred;
    {
      std::lock_guard<std::mutex> lock(stats_lock_);
      last_edge = tracker_.last_edge_ns();
      pred = tracker_.predicted_period_s();
      double limit = std::max(WATCHDOG_S, 1.5 * (pred > 0 ? pred : (cmd > 0 ? 1.0 / cmd : 0)));
      tracker_.check_silence(now, limit);
      if (!fr && !led_test && last_edge && (now - last_edge) / 1e9 > limit && !led_->blanked) {
        safety_tripped_ = true;
        led_->blank();
      }
    }
    if (running_ && motor_->failed()) {
      halt(motor_->failure());
      continue;
    }
    if (running_ && !testing_ && !motor_test_ && sensor_->is_open() && stall_detect_) {
      if (cmd >= STALL_ARM_FPS) {
        if (!armed_at) armed_at = now;
        std::int64_t ref = std::max(last_edge, armed_at);
        double silence = (now - ref) / 1e9;
        double limit = std::max(1.0, 3.0 / cmd);
        if (silence > limit) {
          if (stall_stop_) {
            halt(log::format("Motor stopped: no index pulse for %.1f s while the motor turns at %.2f FPS "
                             "(MTV_STALL_STOP=1). Check that the notch passes through the sensor slot and the sensor "
                             "wiring (GPIO %d), and that the disk turns freely.",
                             silence, cmd, sensor_->pin));
            armed_at = 0;
          } else {
            std::string warn;
            {
              std::lock_guard<std::mutex> lock(stats_lock_);
              if (warning_.rfind("No index pulse", 0) != 0) {
                warning_ = log::format(
                    "No index pulse for %.1f s while the motor turns at %.2f FPS. The motor keeps running; the LED "
                    "stays dark until the notch passes the sensor on GPIO %d. Check that the disk is fitted, the notch "
                    "passes through the sensor slot, and the sensor wiring.",
                    silence, cmd, sensor_->pin);
                warn = warning_;
              }
            }
            if (!warn.empty()) log::warning(warn);
          }
        } else if (last_edge > armed_at) {
          std::lock_guard<std::mutex> lock(stats_lock_);
          if (warning_.rfind("No index pulse", 0) == 0) warning_.clear();
        }
      } else {
        armed_at = 0;
      }
    }
  }
  led_->blank();
  close_sensor_if_open();
}

json RealtimeController::run_microstep_test() {
  if (running_ || free_run_) throw std::invalid_argument("Stop the motor and turn free-run off before the microstep check.");
  if (testing_.exchange(true)) throw std::invalid_argument("The microstep check is already running.");
  abort_test_ = false;
  const int spr = motor_->steps_per_rev();
  // 0.25 rev/s if the jumpers match the setting: slow enough that even full-step (16x) still turns
  const double rps = std::clamp(env::get_double("MTV_MICROSTEP_TEST_RPS", 0.25), 0.05, 5.0);
  const double rate = rps * spr;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    test_edges_.clear();
    tracker_.reset();
    error_.clear();
    stalled_ = false;
  }
  log::info(log::format("microstep check: stepping at %.0f steps/s (%.2f rev/s at %d steps per revolution)", rate, rps, spr));
  motor_->start_test_rate(rate);
  std::vector<std::pair<std::int64_t, double>> edges;
  double timeout = 40.0;
  {
    std::unique_lock<std::mutex> lock(stats_lock_);
    // Long enough for three pulses even if the driver is at twice the configured microstepping.
    timeout = std::max(40.0, 7.0 / rps + 5.0);
    test_cv_.wait_for(lock, std::chrono::duration<double>(timeout),
                      [&] { return test_edges_.size() >= 3 || quit_ || abort_test_; });
    edges = test_edges_;
  }
  motor_->stop();
  testing_ = false;
  json result = {{"configured_steps_per_rev", spr}, {"configured_microsteps", spr / 200}, {"pulses", edges.size()}};
  if (abort_test_ || quit_) {
    result["ok"] = false;
    result["message"] = "Microstep check cancelled.";
    log::info("microstep check cancelled");
    return result;
  }
  if (edges.size() < 2) {
    result["ok"] = false;
    result["message"] = log::format(
        "Microstep check: %zu index pulse(s) in %.0f s. Either the disk did not turn (driver not enabled, no motor current, "
        "stalled) or the index sensor on GPIO %d is not seeing the notch. Watch the disk and the HUD INDEX SENSOR cell.",
        edges.size(), timeout, sensor_->pin);
  } else {
    double measured = edges.back().second - edges[edges.size() - 2].second;
    int detected = nearest_microstep(measured);
    double period = (edges.back().first - edges[edges.size() - 2].first) / 1e9;
    bool ok = std::fabs(measured - spr) / spr < 0.05;
    result["ok"] = ok;
    result["measured_steps_per_rev"] = std::round(measured);
    result["detected_microsteps"] = detected;
    result["revolution_s"] = round_to(period, 3);
    if (ok) {
      result["message"] = log::format("Microstep check passed: %.0f steps per revolution measured (1/%d), matching "
                                      "MTV_MICROSTEPS=%d.", measured, detected, spr / 200);
    } else {
      result["message"] = log::format(
          "Microstep check FAILED: %.0f steps per revolution measured, about 1/%d microstepping, but the console is set "
          "to 1/%d (%d steps). Every speed is %.1fx off. Set MS1, MS2 and MS3 high on the A4988 for 1/16 (DRV8825: 1/32 "
          "needs all three high; TMC2209: MS1 and MS2 high for 1/16), or choose 1/%d under Microsteps in the console.",
          measured, detected, spr / 200, spr, spr / std::max(1.0, measured), detected);
    }
  }
  if (result["ok"].get<bool>()) log::info(result["message"].get<std::string>());
  else log::error(result["message"].get<std::string>());
  return result;
}

json RealtimeController::get_status() {
  json motor = motor_->telemetry();
  std::string st = state();
  RevolutionTracker tracker;
  std::string error, warning, sensor_error;
  json rt;
  {
    // Copy under the lock, build JSON outside it: the realtime thread takes this lock on every edge.
    std::lock_guard<std::mutex> lock(stats_lock_);
    tracker = tracker_;
    error = error_;
    warning = running_ ? warning_ : std::string();
    sensor_error = sensor_error_;
    rt = rt_info_;
  }
  const double silence = tracker.last_edge_ns() ? (now_ns() - tracker.last_edge_ns()) / 1e9 : 1e9;
  const bool fresh = tracker.have_period() && silence < std::max(2.0, 3.0 * tracker.period_s());
  const double fps = fresh ? tracker.fps() : 0.0;
  const double jitter = tracker.jitter_ms();
  const bool locked = tracker.locked();
  const bool sensor_open = sensor_->is_open();
  json sensor = {{"driver", sensor_->driver()},
                 {"backend", sensor_open || sensor_->is_mock() || sensor_error.empty() ? sensor_->backend() : "unavailable"},
                 {"pin", sensor_->pin},
                 {"running", sensor_open},
                 {"pulse_count", tracker.pulse_count()},
                 {"rejected_edges", tracker.rejected()},
                 {"measured_fps", round_to(fps, 2)},
                 {"measured_rpm", round_to(fps * 60.0, 1)},
                 {"jitter_ms", round_to(jitter, 3)},
                 {"jitter_rms_ms", round_to(tracker.jitter_rms_ms(), 3)},
                 {"period_ms", round_to(tracker.period_s() * 1000.0, 3)},
                 {"steps_per_rev_measured", std::round(tracker.steps_per_rev_measured())},
                 {"sync_locked", locked},
                 {"connected", sensor_error.empty()},
                 {"error", sensor_error}};
  rt["edge_wake_us"] = round_to(edge_latency_us_, 1);
  rt["edge_wake_max_us"] = round_to(edge_latency_max_us_, 1);
  rt["lead_in_us"] = round_to(led_->lead_in_us, 1);
  rt["late_pixels"] = led_->late_pixels.load();
  json led = {{"driver", led_->driver()},
              {"backend", led_->backend()},
              {"pin", led_->pin},
              {"brightness", round_to(pipeline_.brightness(), 2)},
              {"is_blanked", led_->blanked.load()},
              {"safety_tripped", safety_tripped_.load()},
              {"frames_rendered", led_->frames_rendered.load()},
              {"average_duty_cycle", round_to(led_->blanked ? 0.0 : led_->average_duty.load(), 3)},
              {"connected", true}};
  const json details = led_->details();
  for (auto it = details.begin(); it != details.end(); ++it) led[it.key()] = it.value();
  const double tgt = target_fps_;
  return {{"state", st},
          {"target_fps", tgt},
          {"target_rpm", tgt * 60.0},
          {"microsteps", motor_->microsteps()},
          {"ramp_rate_fps", motor_->ramp_rate()},
          {"measured_fps", round_to(fps, 2)},
          {"measured_rpm", round_to(fps * 60.0, 1)},
          {"sync_locked", locked},
          {"jitter_ms", round_to(jitter, 3)},
          {"frame_counter", frame_counter_.load()},
          {"free_run", free_run_.load()},
          {"free_run_frames", free_run_frames_.load()},
          {"output_clock", free_run_ ? "free-run" : "index-sensor"},
          {"error", error},
          {"warning", warning},
          {"bench", bench_status()},
          {"hardware_notes", notes_},
          {"motor", motor},
          {"sensor", sensor},
          {"led", led},
          {"realtime", rt},
          {"pipeline", pipeline_.get_telemetry()}};
}

FramePtr RealtimeController::get_preview_frame() {
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    if (preview_ && now_s() - preview_at_ < 0.5) return preview_;
  }
  return pipeline_.get_latest_frame();
}

void RealtimeController::close() {
  if (quit_.exchange(true)) return;
  free_run_ = false;
  inhibit_ = true;
  running_ = false;
  led_test_ = 0;
  led_test_abort_ = true;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    test_cv_.notify_all();
  }
  if (motor_) motor_->stop();
  if (rt_.joinable()) rt_.join();
  if (led_) led_->blank();
}

}  // namespace mtv
