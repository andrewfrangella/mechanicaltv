// Console: options, the --check report, data-directory fallback and the real window off-screen
// (from test_local_app.py).
#include <doctest/doctest.h>

#ifdef MTV_HAVE_CONSOLE
#include <QApplication>
#include <fstream>

#include "helpers.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/console/app_setup.h"
#include "mtv/console/console_window.h"
#include "mtv/console/debug_window.h"

using namespace mtv;

TEST_CASE("options: env file supplies defaults, flags override, bad choices are rejected") {
  testing::TempDir tmp;
  std::string env_file = tmp.str() + "/environment";
  std::ofstream(env_file) << "MTV_HDMI_FIT=crop\nMTV_LED_PIN=12\n";
  env::unset("MTV_HDMI_FIT");
  env::unset("MTV_LED_PIN");
  auto a = console::parse_args({"--env-file", env_file, "--opto-pin", "17", "--mock-hardware"});
  CHECK(a.hdmi_fit == "crop");
  CHECK(a.led_pin == 12);
  CHECK(a.opto_pin == 17);
  CHECK(a.mock_hardware);
  env::unset("MTV_HDMI_FIT");
  env::unset("MTV_LED_PIN");
  CHECK_THROWS_AS(console::parse_args({"--hdmi-fit", "stretch"}), std::invalid_argument);
  CHECK_THROWS_AS(console::parse_args({"--opto-pin", "x"}), std::invalid_argument);
  CHECK_THROWS_AS(console::parse_args({"--bogus"}), std::invalid_argument);
  CHECK(console::parse_args({"--default-source=hdmi"}).default_source == "hdmi");
}

TEST_CASE("default source: auto picks hdmi only when the device exists and hardware is real") {
  console::Args a;
  a.hdmi_device = "/nonexistent/video0";
  CHECK(console::resolve_default_source(a) == "pattern");
  testing::TempDir tmp;
  a.hdmi_device = tmp.str();
  CHECK(console::resolve_default_source(a) == "hdmi");
  a.mock_hardware = true;
  CHECK(console::resolve_default_source(a) == "pattern");
  a.default_source = "hdmi";
  CHECK(console::resolve_default_source(a) == "hdmi");
}

TEST_CASE("data directory falls back when the requested one is not writable") {
  std::string got = console::usable_data_dir("/proc/definitely/not/writable");
  CHECK(got != "/proc/definitely/not/writable");
}

TEST_CASE("--check report has the environment facts") {
  console::Args a;
  a.hdmi_device = "/nonexistent/video0";
  json r = console::check_report(a);
  for (const char* k : {"version", "toolkit", "ffmpeg", "v4l2_ctl", "motor_driver", "microsteps", "ramp_rate_fps", "hardware_pwm",
                        "gpiochip", "spidev_bufsiz", "led_output", "rtprio_limit", "hdmi_device_exists", "hdmi_bridge", "default_source"})
    CHECK(r.contains(k));
  CHECK(r["hdmi_bridge"] == "device missing");
}

TEST_CASE("the console window runs, refreshes, drives the engine and counts errors (offscreen)") {
  testing::EnvGuard qpa("QT_QPA_PLATFORM", "offscreen");
  testing::EnvGuard ramp("MTV_RAMP_RATE", "50");
  testing::EnvGuard settle("MTV_ENABLE_SETTLE", "0");
  int argc = 1;
  char name[] = "mtv_tests";
  char* argv[] = {name, nullptr};
  QApplication app(argc, argv);
  testing::TempDir data;
  console::Args args;
  args.data = data.str();
  args.mock_hardware = true;
  args.hdmi_device = data.str() + "/novideo";
  std::string note;
  auto studio = console::build_studio(args, &note);
  {
    console::ConsoleWindow w(*studio, false, "", "");
    w.show();
    w.resize(1400, 900);
    for (int i = 0; i < 6; ++i) {
      app.processEvents();
      w.refresh_now();
    }
    CHECK(w.pill_text("hw") == "SIMULATION");
    CHECK(w.pill_text("motor") == "MOTOR: IDLE");
    CHECK(w.hud_text("lock") == "STANDBY");
    CHECK(w.slider_label("phase") == "0 px (0°)");
    CHECK(w.source_status().startsWith("PROCEDURAL PATTERN"));
    CHECK(w.output_shown().width() % 32 == 0);
    CHECK(w.output_shown().width() >= 64);   // zoomed, whole pixels

    w.action("free_run", {{"enabled", true}});
    w.wait_idle();
    CHECK(testing::wait_until([&] { return studio->realtime().free_run_frames() > 2; }));
    w.refresh_now();
    CHECK(w.pill_text("sync") == "SYNC: FREE-RUN");
    CHECK(w.hud_text("watchdog") == "BYPASSED (FREE-RUN)");

    w.action("start");
    w.wait_idle();
    CHECK(testing::wait_until([&] { return studio->realtime().state() == "locked"; }, 6.0));
    w.action("emergency_blank");
    w.wait_idle();
    CHECK_FALSE(studio->realtime().free_run());
    CHECK(studio->realtime().state() == "idle");

    w.action("calibration", {{"gamma", 2.2}, {"phase_offset", 25}});
    w.wait_idle();
    CHECK(studio->pipeline().gamma() == doctest::Approx(2.2));
    CHECK(studio->pipeline().phase_offset() == 25);

    w.action("source", {{"mode", "pattern"}, {"pattern", "checkerboard"}});
    w.wait_idle();
    sleep_s(0.2);
    w.refresh_now();
    CHECK(w.source_status().contains("checkerboard"));

    log::error("deliberate test error");
    w.refresh_now();
    CHECK(w.error_count() >= 1);
    CHECK(w.pill_text("log").startsWith("LOG: "));
    CHECK(w.debug()->line_count() >= 1);
    w.debug()->toggle();
    CHECK(w.debug()->is_shown());
    w.debug()->toggle();

    // Emergency blank must not wait behind a slow queued job (code audit).
    {
      testing::EnvGuard slow("MTV_MICROSTEP_TEST_RPS", "0.1");
      w.action("microstep_test");
      CHECK(testing::wait_until([&] { return studio->realtime().state() == "testing"; }, 3.0));
      double t0 = now_s();
      w.action("emergency_blank");
      CHECK(now_s() - t0 < 1.0);
      CHECK_FALSE(studio->realtime().motor().is_running());
      w.wait_idle();
      CHECK(studio->realtime().state() == "idle");
    }
    w.quit_console();
    CHECK(w.closing());
  }
  studio.reset();
}

#include <csignal>
#include <sys/wait.h>
#include "mtv/common/subprocess.h"

TEST_CASE("SIGTERM makes the running console stop the motor and exit cleanly") {
  testing::TempDir data;
  std::string bin = std::string(MTV_REPO_ROOT) + "/build/mechanical-tv";
  if (!std::filesystem::exists(bin)) {
    MESSAGE("console binary not built next to the tests; skipped");
    return;
  }
  testing::EnvGuard qpa("QT_QPA_PLATFORM", "offscreen");
  proc::Child child;
  std::string why;
  REQUIRE(child.start({bin, "--mock-hardware", "--free-run", "--data", data.str()}, &why));
  sleep_s(2.0);
  ::kill(child.pid(), SIGTERM);
  std::optional<int> rc;
  for (int i = 0; i < 100 && !rc; ++i) {
    sleep_s(0.05);
    rc = child.poll();
  }
  REQUIRE(rc.has_value());
  CHECK(*rc == 0);
  std::ifstream in(data.path / "logs" / "mechanical-tv.log");
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(all.find("signal 15: shutting down") != std::string::npos);
  CHECK(all.find("console closing: stopping motor, blanking LED") != std::string::npos);
}
#endif
