#include "mtv/hardware/controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include "mtv/common/clock.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/rt.h"

namespace mtv {

namespace {
constexpr double MIN_LED_FPS = 0.9;          // the LED stays dark below this measured speed
constexpr double STALL_ARM_FPS = 0.3;        // stall detection runs once the commanded speed exceeds this
constexpr double SLIP_TOLERANCE = 0.08;      // measured/expected steps per revolution
constexpr int SLIP_REVS = 3;                 // consecutive off-ratio revolutions that count as a slip
constexpr double WATCHDOG_S = 0.35;          // thermal/stall interlock: LED off this long after the last pulse
constexpr std::int64_t MAX_WAIT_NS = 50'000'000;

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
  auto driver = create_motor_driver(o.motor_port, o.force_mock, &note);   // A4988 errors propagate
  if (!note.empty()) notes_.push_back(note);
  bool mock_motor = driver->is_mock();
  motor_ = std::make_unique<MotorController>(std::move(driver), target_fps_);
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
  guard_ns_ = static_cast<std::int64_t>(std::clamp(env::get_double("MTV_LED_GUARD_US", 150.0), 0.0, 5000.0) * 1000.0);
  rt_ = std::thread([this] {
    log::set_thread_name("realtime-led");
    rt_loop();
  });
}

std::string RealtimeController::state() {
  if (testing_) return "testing";
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
  if (running_) return;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    tracker_.reset();
    error_.clear();
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
  led_->blank();
}

void RealtimeController::emergency_blank() {
  inhibit_ = true;
  free_run_ = false;
  led_->blank();
  stop();
  led_->blank();
}

void RealtimeController::set_free_run(bool enabled) {
  if (enabled == free_run_) return;
  if (enabled) {
    inhibit_ = false;
    free_run_ = true;
    return;
  }
  free_run_ = false;
  if (!running_) inhibit_ = true;
  led_->blank();   // the index sensor relights it on the next pulse when the motor is running
}

void RealtimeController::set_target_fps(double fps) {
  target_fps_ = std::clamp(fps, MIN_FPS, MAX_FPS);
  motor_->set_target_fps(target_fps_);
}

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
  std::string halt_reason;
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    bool accepted = tracker_.on_edge(t_ns, testing_ ? 0.0 : target_fps_.load(), steps, spr);
    if (accepted && testing_) {
      test_edges_.emplace_back(t_ns, steps);
      test_cv_.notify_all();
    }
    safety_tripped_ = false;
    if (accepted && running_ && !testing_ && tracker_.steps_ratio() > 0 && stall_detect_) {
      double ratio = tracker_.steps_ratio();
      slip_revs_ = std::fabs(ratio - 1.0) > SLIP_TOLERANCE ? slip_revs_ + 1 : 0;
      if (slip_revs_ >= SLIP_REVS) {
        int configured = spr / 200;
        double measured = tracker_.steps_per_rev_measured();
        if (ratio < 0.75) {
          int driver_ms = nearest_microstep(spr * ratio);
          halt_reason = log::format(
              "Motor stopped: the disk turns %.1fx faster than commanded (%.0f of %d steps per revolution). The driver "
              "appears to be at 1/%d microstepping while MTV_MICROSTEPS=%d: set MS1, MS2 and MS3 high on the A4988 for "
              "1/16, or set MTV_MICROSTEPS to match the jumpers.",
              1.0 / ratio, measured, spr, driver_ms, configured);
        } else {
          halt_reason = log::format(
              "Motor stall detected at %.2f FPS: %.0f steps per revolution measured, %d expected, for %d revolutions "
              "(the rotor is slipping). Check the A4988 current (Vref), supply voltage, rim rubbing in the sensor "
              "slot, or lower MTV_RAMP_RATE.",
              motor_->current_fps(), measured, spr, SLIP_REVS);
        }
      }
    }
  }
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
  while (!quit_) {
    const bool index_mode = running_ || testing_;
    if (index_mode) open_sensor_if_needed();
    else close_sensor_if_open();
    const bool fr = free_run_;
    if (!index_mode && !fr) {
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
      if (!fr && running_ && !inhibit_) {
        double period;
        {
          std::lock_guard<std::mutex> lock(stats_lock_);
          period = tracker_.have_period() ? tracker_.predicted_period_s() : 0.0;
        }
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
      if (!fr && last_edge && (now - last_edge) / 1e9 > limit && !led_->blanked) {
        safety_tripped_ = true;
        led_->blank();
      }
    }
    if (running_ && motor_->failed()) {
      halt(motor_->failure());
      continue;
    }
    if (running_ && !testing_ && sensor_->is_open() && stall_detect_) {
      if (cmd >= STALL_ARM_FPS) {
        if (!armed_at) armed_at = now;
        std::int64_t ref = std::max(last_edge, armed_at);
        double silence = (now - ref) / 1e9;
        double limit = std::max(1.0, 3.0 / cmd);
        if (silence > limit) {
          halt(log::format("Motor stall detected: no index pulse for %.1f s while the motor is commanded to %.2f FPS. "
                           "Check that the disk turns freely, the A4988 current (Vref) and supply voltage, the microstep "
                           "jumpers (run Check microstepping), and the index sensor wiring (GPIO %d).",
                           silence, cmd, sensor_->pin));
          armed_at = 0;
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
          "Microstep check FAILED: %.0f steps per revolution measured, about 1/%d microstepping, but MTV_MICROSTEPS=%d "
          "expects %d. Every speed is %.1fx off. Set MS1, MS2 and MS3 high on the A4988 for 1/16 (DRV8825: 1/32 needs "
          "all three high; TMC2209: MS1 and MS2 high for 1/16), or set MTV_MICROSTEPS=%d.",
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
  std::string error, sensor_error;
  json rt;
  {
    // Copy under the lock, build JSON outside it: the realtime thread takes this lock on every edge.
    std::lock_guard<std::mutex> lock(stats_lock_);
    tracker = tracker_;
    error = error_;
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
          {"measured_fps", round_to(fps, 2)},
          {"measured_rpm", round_to(fps * 60.0, 1)},
          {"sync_locked", locked},
          {"jitter_ms", round_to(jitter, 3)},
          {"frame_counter", frame_counter_.load()},
          {"free_run", free_run_.load()},
          {"free_run_frames", free_run_frames_.load()},
          {"output_clock", free_run_ ? "free-run" : "index-sensor"},
          {"error", error},
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
  {
    std::lock_guard<std::mutex> lock(stats_lock_);
    test_cv_.notify_all();
  }
  if (motor_) motor_->stop();
  if (rt_.joinable()) rt_.join();
  if (led_) led_->blank();
}

}  // namespace mtv
