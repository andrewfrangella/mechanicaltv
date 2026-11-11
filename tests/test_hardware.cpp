// Motor profile, index tracking, LED bitstream and the realtime controller in simulation
// (from test_realtime.py and test_free_run.py, plus the new start-up, stall and microstep logic).
#include <doctest/doctest.h>

#include <cmath>
#include <thread>

#include "helpers.h"
#include "mtv/capture/pipeline.h"
#include "mtv/hardware/controller.h"
#include "mtv/hardware/led.h"
#include "mtv/hardware/motor.h"
#include "mtv/hardware/sensor.h"

using namespace mtv;

TEST_CASE("ramp rate from the environment: default, clamping, malformed") {
  {
    testing::EnvGuard g("MTV_RAMP_RATE", "");
    CHECK(ramp_rate_from_env() == DEFAULT_RAMP_RATE);
  }
  {
    testing::EnvGuard g("MTV_RAMP_RATE", "0.5");
    CHECK(ramp_rate_from_env() == doctest::Approx(0.5));
  }
  {
    testing::EnvGuard g("MTV_RAMP_RATE", "999");
    CHECK(ramp_rate_from_env() == MAX_RAMP_RATE);
  }
  {
    testing::EnvGuard g("MTV_RAMP_RATE", "fast");
    CHECK(ramp_rate_from_env() == DEFAULT_RAMP_RATE);
  }
  {
    testing::EnvGuard g("MTV_RAMP_RATE", "nan");
    CHECK(ramp_rate_from_env() == DEFAULT_RAMP_RATE);
  }
}

TEST_CASE("S-curve profile: starts at a crawl, never steps the speed, eases into the target without overshoot") {
  SpeedProfile p;
  p.reset(0.02);
  const double dt = 0.005, a_max = 2.0, jerk_time = 0.5;
  double v_prev = p.velocity(), t = 0, max_a = 0;
  bool reached = false;
  for (int i = 0; i < 4000 && !reached; ++i) {
    double v = p.advance(10.0, a_max, jerk_time, dt);
    t += dt;
    double a = (v - v_prev) / dt;
    max_a = std::max(max_a, a);
    CHECK(v - v_prev <= a_max * dt + 1e-9);   // no velocity step bigger than one tick of max acceleration
    CHECK(v <= 10.0 + 1e-9);
    if (t < 0.1) CHECK(a < a_max * 0.5);       // acceleration builds up gradually (jerk limit)
    v_prev = v;
    reached = v >= 10.0;
  }
  CHECK(reached);
  CHECK(max_a <= a_max + 1e-6);
  CHECK(t == doctest::Approx(5.5).epsilon(0.1));   // ~ (10 - 0.02) / 2 + jerk time
  // and back down
  for (int i = 0; i < 4000; ++i) p.advance(1.0, a_max, jerk_time, dt);
  CHECK(p.velocity() == doctest::Approx(1.0));
}

TEST_CASE("mock motor integrates steps from its rate") {
  MockMotorDriver m(1, 3200);
  m.set_rate(32000);
  sleep_s(0.1);
  m.release();
  CHECK(m.step_count() == doctest::Approx(3200).epsilon(0.3));
  json t = m.telemetry();
  CHECK(t["driver"] == "MockMotorDriver");
  CHECK(t["released"] == true);
}

TEST_CASE("revolution tracker: debounce, lock hysteresis, jitter, silence, steps ratio") {
  RevolutionTracker t;
  const std::int64_t P = 100'000'000;   // 10 FPS
  std::int64_t now = 1'000'000'000;
  double steps = 0;
  t.on_edge(now, 10.0, steps, 3200);
  CHECK_FALSE(t.have_period());
  CHECK_FALSE(t.on_edge(now + 5'000'000, 10.0, steps, 3200));   // 5 ms bounce
  CHECK(t.rejected() == 1);
  for (int i = 1; i <= 6; ++i) {
    now += P;
    steps += 3200;
    CHECK(t.on_edge(now, 10.0, steps, 3200));
    CHECK(t.fps() == doctest::Approx(10.0));
  }
  CHECK_FALSE(t.locked());   // 6 periods: only 2 good ones since the history reached 5
  now += P;
  steps += 3200;
  t.on_edge(now, 10.0, steps, 3200);
  CHECK(t.locked());   // >= 5 periods and 3 good in a row
  CHECK(t.steps_ratio() == doctest::Approx(1.0));
  CHECK(t.jitter_ms() == doctest::Approx(0.0));
  // one noisy revolution does not unlock (hysteresis) ...
  now += P + 3'000'000;
  steps += 3200;
  t.on_edge(now, 10.0, steps, 3200);
  CHECK(t.jitter_ms() > 2.0);
  CHECK(t.locked());
  // ... two do
  now += P - 3'000'000;
  steps += 3200;
  t.on_edge(now, 10.0, steps, 3200);
  CHECK_FALSE(t.locked());
  // relock, then lose the pulses
  for (int i = 0; i < 4; ++i) {
    now += P;
    steps += 3200;
    t.on_edge(now, 10.0, steps, 3200);
  }
  CHECK(t.locked());
  CHECK(t.check_silence(now + 400'000'000, 0.35));
  CHECK_FALSE(t.locked());
  // off-speed never locks
  RevolutionTracker slow;
  for (int i = 0; i < 10; ++i) slow.on_edge(i * 120'000'000LL + 1, 10.0, i * 3200.0, 3200);
  CHECK_FALSE(slow.locked());
}

TEST_CASE("LED bitstream: pixel spans, pulse widths, minimum pulse, chunked equals whole") {
  NipkowStream s{};
  s.fill(255);
  const double B = 200000;   // 10 FPS at 2 MHz
  std::vector<std::uint8_t> out(25008);
  std::size_t n = build_led_bits(s, B, 0, 200000, 10, out.data());
  CHECK(n == 25000);
  for (std::size_t i = 0; i < n; ++i) CHECK(out[i] == 0xFF);
  s.fill(0);
  s[0] = 128;   // round(128/255 * 250) = 125 bits
  s[1] = 5;     // 4.9 -> 5 bits < 10-bit minimum: dark
  s[2] = 255;
  n = build_led_bits(s, B, 0, 1000, 10, out.data());
  CHECK(n == 125);
  int ones = 0;
  for (int b = 0; b < 1000; ++b) ones += (out[static_cast<std::size_t>(b / 8)] >> (7 - b % 8)) & 1;
  CHECK(ones == 125 + 250);
  CHECK(((out[0] >> 7) & 1) == 1);                     // pixel 0 starts lit
  CHECK(((out[124 / 8] >> (7 - 124 % 8)) & 1) == 1);   // last lit bit of pixel 0
  CHECK(((out[125 / 8] >> (7 - 125 % 8)) & 1) == 0);
  CHECK(((out[500 / 8] >> (7 - 500 % 8)) & 1) == 1);   // pixel 2 starts at bit 500
  // building in two chunks gives the same bits as one pass
  for (int i = 0; i < 800; ++i) s[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((i * 37) % 256);
  std::vector<std::uint8_t> whole(25008), a(25008), b(25008);
  build_led_bits(s, B, 0, 200000, 10, whole.data());
  build_led_bits(s, B, 0, 80000, 10, a.data());
  build_led_bits(s, B, 80000, 200000, 10, b.data());
  CHECK(std::equal(a.begin(), a.begin() + 10000, whole.begin()));
  CHECK(std::equal(b.begin(), b.begin() + 15000, whole.begin() + 10000));
  // a late start skips the leading bits of the timeline
  std::vector<std::uint8_t> late(25008);
  build_led_bits(s, B, 8000, 200000, 10, late.data());
  CHECK(std::equal(late.begin(), late.begin() + 24000, whole.begin() + 1000));
}

namespace {

struct Sim {
  testing::EnvGuard ramp;
  testing::EnvGuard jerk{"MTV_JERK_TIME", "0.05"};
  testing::EnvGuard settle{"MTV_ENABLE_SETTLE", "0"};
  FramePipeline pipeline;
  MockMotorDriver* motor = nullptr;
  MockIndexSensor* sensor = nullptr;
  MockLedOutput* led = nullptr;
  std::unique_ptr<RealtimeController> rt;
  explicit Sim(int sensor_spr = 3200, double fps = 10.0, const char* ramp_rate = "50", bool blocking_led = false,
               double notch_phase = 0.0, std::unique_ptr<MockMotorDriver> custom_motor = nullptr)
      : ramp("MTV_RAMP_RATE", ramp_rate) {
    auto m = custom_motor ? std::move(custom_motor) : std::make_unique<MockMotorDriver>(1, 3200);
    motor = m.get();
    MockMotorDriver* mp = motor;
    auto s = std::make_unique<MockIndexSensor>(4, [mp] { return mp->step_count(); }, [mp] { return mp->rate(); }, sensor_spr,
                                               notch_phase);
    sensor = s.get();
    auto l = std::make_unique<MockLedOutput>(21, blocking_led);
    led = l.get();
    rt = std::make_unique<RealtimeController>(pipeline, std::move(m), std::move(s), std::move(l), fps);
  }
  ~Sim() {
    rt->close();
    pipeline.close();
  }
};

}  // namespace

TEST_CASE("controller lifecycle: ramps, locks, drives the LED, stops idle and blank") {
  Sim sim;
  CHECK(sim.rt->state() == "idle");
  sim.rt->start();
  CHECK(testing::wait_until([&] { return sim.rt->state() == "locked"; }, 6.0));
  CHECK(testing::wait_until([&] { return sim.led->frames_rendered > 3; }));
  json st = sim.rt->get_status();
  CHECK(st["sync_locked"] == true);
  CHECK(st["measured_fps"].get<double>() == doctest::Approx(10.0).epsilon(0.02));
  CHECK(st["sensor"]["steps_per_rev_measured"].get<double>() == doctest::Approx(3200).epsilon(0.01));
  CHECK(st["led"]["is_blanked"] == false);
  CHECK(st["output_clock"] == "index-sensor");
  for (const char* k : {"state", "target_fps", "target_rpm", "measured_fps", "measured_rpm", "sync_locked", "jitter_ms", "frame_counter",
                        "free_run", "free_run_frames", "output_clock", "error", "motor", "sensor", "led", "pipeline"})
    CHECK(st.contains(k));
  NipkowStream expect = sim.pipeline.serialize_nipkow_stream(*sim.pipeline.get_latest_frame());
  CHECK(sim.led->last_stream() == expect);
  sim.rt->stop();
  CHECK(sim.rt->state() == "idle");
  CHECK(sim.led->blanked);
  long frames = sim.led->frames_rendered;
  sleep_s(0.3);
  CHECK(sim.led->frames_rendered == frames);   // nothing is clocked while stopped
}

TEST_CASE("the LED stays dark until the disk reaches a measurable speed") {
  Sim sim(3200, 10.0, "0.2");
  sim.rt->start();
  sleep_s(0.6);   // still below 1 FPS
  CHECK(sim.led->frames_rendered == 0);
  sim.rt->stop();
}

TEST_CASE("free-run drives the LED and the preview while idle, at the target rate, without double clocking") {
  Sim sim;
  sim.rt->set_free_run(true);
  sim.rt->set_free_run(true);   // idempotent
  sleep_s(1.05);
  long n = sim.rt->free_run_frames();
  CHECK(n >= 9);
  CHECK(n <= 12);
  CHECK(sim.rt->get_status()["output_clock"] == "free-run");
  CHECK(sim.rt->get_preview_frame() != nullptr);
  // running the motor in free-run: index pulses do not add frames
  sim.rt->start();
  CHECK(testing::wait_until([&] { return sim.rt->motor().current_fps() >= 10.0; }));
  long a = sim.led->frames_rendered;
  sleep_s(1.0);
  long b = sim.led->frames_rendered;
  CHECK(b - a <= 12);
  sim.rt->emergency_blank();
  CHECK_FALSE(sim.rt->free_run());
  CHECK(sim.rt->state() == "idle");
  long c = sim.led->frames_rendered;
  sleep_s(0.3);
  CHECK(sim.led->frames_rendered == c);
}

TEST_CASE("speed ceiling is 24 FPS and the floor 1 FPS") {
  Sim sim;
  sim.rt->set_target_fps(99);
  CHECK(sim.rt->target_fps() == doctest::Approx(24.0));
  sim.rt->set_target_fps(0);
  CHECK(sim.rt->target_fps() == doctest::Approx(1.0));
}

TEST_CASE("stall detection: no index pulses while commanded to spin stops the motor with the reason") {
  Sim sim;
  sim.sensor->set_motor_coupled(false);   // the disk does not turn
  sim.rt->start();
  CHECK(testing::wait_until([&] { return sim.rt->state() == "error"; }, 4.0));
  json st = sim.rt->get_status();
  CHECK(st["error"].get<std::string>().find("no index pulse") != std::string::npos);
  CHECK(sim.rt->motor().is_running() == false);
  CHECK(sim.led->blanked);
  sim.sensor->set_motor_coupled(true);
  sim.rt->start();   // a new start clears the error
  CHECK(sim.rt->get_status()["error"] == "");
  sim.rt->stop();
}

TEST_CASE("microstep mismatch is detected while spinning: 16x too many revolutions means full step") {
  Sim sim(200, 1.0, "0.5");   // the disk turns once every 200 steps although the software assumes 3200
  sim.rt->start();
  CHECK(testing::wait_until([&] { return sim.rt->state() == "error"; }, 8.0));
  std::string err = sim.rt->get_status()["error"];
  CHECK(err.find("1/1 microstepping") != std::string::npos);
  CHECK(err.find("MS1, MS2 and MS3") != std::string::npos);
}

TEST_CASE("microstep self-test passes when the jumpers match and names the mode when they do not") {
  testing::EnvGuard fast("MTV_MICROSTEP_TEST_RPS", "4");
  {
    Sim sim;
    json r = sim.rt->run_microstep_test();
    CHECK(r["ok"] == true);
    CHECK(r["detected_microsteps"] == 16);
    CHECK(sim.rt->state() == "idle");
    CHECK_FALSE(sim.rt->motor().is_running());
  }
  {
    Sim sim(1600);   // 1/8
    json r = sim.rt->run_microstep_test();
    CHECK(r["ok"] == false);
    CHECK(r["detected_microsteps"] == 8);
    CHECK(r["message"].get<std::string>().find("FAILED") != std::string::npos);
  }
  {
    Sim sim;
    sim.rt->start();
    CHECK_THROWS_AS(sim.rt->run_microstep_test(), std::invalid_argument);
    sim.rt->stop();
  }
}

TEST_CASE("the preview shows the live source when nothing is being sent to the disk") {
  Sim sim;
  sim.rt->set_pattern("grid");
  sim.rt->set_source_mode("pattern");
  CHECK(testing::wait_until([&] { return *sim.rt->get_preview_frame() == TestPatternGenerator::generate("grid", 0); }));
}

TEST_CASE("a realistic spin-up with an LED output that blocks for the revolution reaches lock without a false stall") {
  // Regression (code audit): the step count was read when the edge was processed, not at its timestamp,
  // so a blocking LED output made the slip check see 0.2-2x the steps and halt within 3 s.
  testing::EnvGuard jerk_default("MTV_JERK_TIME", "0.5");
  testing::EnvGuard settle_default("MTV_ENABLE_SETTLE", "0.2");
  for (double phase : {0.0, 1100.0, 2400.0}) {
    Sim sim(3200, 10.0, "2.0", true, phase);
    sim.rt->start();
    bool locked = testing::wait_until([&] { return sim.rt->state() == "locked" || sim.rt->state() == "error"; }, 12.0);
    CHECK(locked);
    CHECK_MESSAGE(sim.rt->state() == "locked", sim.rt->get_status()["error"].get<std::string>());
    CHECK(sim.led->frames_rendered > 10);
    sim.rt->stop();
  }
}

TEST_CASE("steps per revolution are counted as magnitudes (a reversed motor still passes the slip check)") {
  RevolutionTracker t;
  double steps = 0;
  for (int i = 0; i < 4; ++i) {
    t.on_edge(1'000'000'000LL + i * 100'000'000LL, 10.0, steps, 3200);
    steps -= 3200;   // counting down
  }
  CHECK(t.steps_ratio() == doctest::Approx(1.0));
}

TEST_CASE("the microstep check ignores sensor bounces and can be cancelled") {
  testing::EnvGuard fast("MTV_MICROSTEP_TEST_RPS", "2");
  {
    Sim sim;
    std::thread bounce([&] {
      // a bounce a few ms after the first revolution's edge
      testing::wait_until([&] { return sim.rt->get_status()["sensor"]["pulse_count"].get<long>() >= 1; }, 5.0);
      sim.sensor->inject(now_ns() + 1'000'000);
    });
    json r = sim.rt->run_microstep_test();
    bounce.join();
    CHECK(r["ok"] == true);
    CHECK(r["detected_microsteps"] == 16);
  }
  {
    testing::EnvGuard slow("MTV_MICROSTEP_TEST_RPS", "0.1");
    Sim sim;
    double t0 = now_s();
    std::thread canceller([&] {
      sleep_s(0.5);
      sim.rt->emergency_blank();
    });
    json r = sim.rt->run_microstep_test();
    canceller.join();
    CHECK(now_s() - t0 < 3.0);
    CHECK(r["message"] == "Microstep check cancelled.");
    CHECK_FALSE(sim.rt->motor().is_running());
  }
}

namespace {
struct FailingMotor : MockMotorDriver {
  std::atomic<int> calls{0};
  FailingMotor() : MockMotorDriver(1, 3200) {}
  void set_rate(double r) override {
    if (++calls > 40) throw std::runtime_error("write /sys/class/pwm/pwmchip0/pwm0/period: Input/output error");
    MockMotorDriver::set_rate(r);
  }
};
}  // namespace

TEST_CASE("a motor driver error stops the run and is reported, not left 'ramping'") {
  Sim sim(3200, 10.0, "50", false, 0.0, std::make_unique<FailingMotor>());
  sim.rt->start();
  CHECK(testing::wait_until([&] { return sim.rt->state() == "error"; }, 4.0));
  CHECK(sim.rt->get_status()["error"].get<std::string>().find("Motor driver error") != std::string::npos);
  CHECK_FALSE(sim.rt->motor().is_running());
}

TEST_CASE("step accounting answers for past instants exactly across rate changes") {
  StepAccount a;
  std::int64_t t0 = now_ns();
  a.set_rate(1000);
  sleep_s(0.05);
  std::int64_t t1 = now_ns();
  a.set_rate(3000);
  sleep_s(0.05);
  double at1 = a.count_at(t1);
  CHECK(at1 == doctest::Approx((t1 - t0) / 1e9 * 1000).epsilon(0.05));
  CHECK(a.count() > at1 + 100);
}
