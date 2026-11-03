// Media library, conversion, import, health and logging (from test_app.py / test_local_app.py).
#include <doctest/doctest.h>

#include <fstream>

#include "helpers.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/subprocess.h"
#include "mtv/engine/library.h"
#include "mtv/engine/studio.h"

using namespace mtv;
namespace fs = std::filesystem;

namespace {
bool have_ffmpeg() { return !proc::which("ffmpeg").empty() && !proc::which("ffprobe").empty(); }

std::string make_clip(const testing::TempDir& dir, const std::string& name = "clip.mp4") {
  std::string out = (dir.path / name).string();
  auto r = proc::run({"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=10",
                      "-t", "2", "-pix_fmt", "yuv420p", out},
                     60);
  REQUIRE(r.ok());
  return out;
}
}  // namespace

TEST_CASE("library: create, list, update, validate ids, delete, interrupted-job recovery") {
  testing::TempDir tmp;
  std::string id;
  {
    Library lib(tmp.str());
    CHECK_THROWS_WITH_AS(lib.create("x", "stretch"), "Choose Fit or Crop", std::invalid_argument);
    id = lib.create(std::string(300, 'n'), "fit");
    CHECK(Library::valid_ident(id));
    CHECK(fs::is_directory(tmp.path / id));
    auto item = lib.get(id);
    CHECK(item.name.size() == 160);
    CHECK(item.status == "uploading");
    CHECK_THROWS_WITH_AS(lib.get("../etc"), "Invalid video identifier", std::invalid_argument);
    CHECK_THROWS_WITH_AS(lib.get(std::string(32, 'a')), "Video not found", std::invalid_argument);
    CHECK_THROWS_WITH_AS(lib.update(id, {{"name", "x"}}), "Invalid update", std::invalid_argument);
    CHECK_THROWS_WITH_AS(lib.remove(id), "Wait for preparation before deleting", std::invalid_argument);
    std::ofstream(tmp.path / id / "upload.part") << "partial";
  }
  {
    Library lib(tmp.str());   // restart: the interrupted import is marked failed and its part file removed
    auto item = lib.get(id);
    CHECK(item.status == "failed");
    CHECK(item.error == "Interrupted by restart. Delete and upload again.");
    CHECK_FALSE(fs::exists(tmp.path / id / "upload.part"));
    std::string id2 = lib.create("second", "crop");
    auto items = lib.items();
    REQUIRE(items.size() == 2);
    CHECK(items[0].id == id2);   // newest first
    lib.remove(id);
    CHECK(lib.items().size() == 1);
    CHECK_FALSE(fs::exists(tmp.path / id));
  }
}

TEST_CASE("conversion produces 32x25 gray frames at 10 fps; a corrupt file fails with a message") {
  if (!have_ffmpeg()) {
    MESSAGE("ffmpeg/ffprobe not installed: conversion test skipped, not passed");
    return;
  }
  testing::TempDir tmp, media;
  std::string clip = make_clip(tmp);
  Library lib(media.str());
  std::string id = lib.create("clip.mp4", "fit");
  fs::copy_file(clip, media.path / id / "original");
  lib.update(id, {{"status", "queued"}});
  lib.prepare(id);
  auto item = lib.get(id);
  CHECK(item.status == "ready");
  CHECK(item.frames == 20);
  CHECK(item.duration == doctest::Approx(2.0));
  CHECK(fs::file_size(media.path / id / "frames.raw") == 20 * 800);
  CHECK(fs::exists(media.path / id / "profile.json"));
  CHECK_FALSE(fs::exists(media.path / id / "frames.part"));

  std::string bad = lib.create("bad.mp4", "fit");
  std::ofstream(media.path / bad / "original") << "this is not a video";
  lib.prepare(bad);
  auto b = lib.get(bad);
  CHECK(b.status == "failed");
  CHECK_FALSE(b.error.empty());
}

TEST_CASE("studio: import queues a conversion that the worker completes while idle; health has the fields") {
  testing::TempDir data, tmp;
  StudioOptions o;
  o.root = data.str();
  o.hardware.force_mock = true;
  o.hdmi.device = tmp.str() + "/novideo";
  Studio studio(o);
  CHECK_THROWS_AS(studio.import_file(tmp.str() + "/missing.mp4"), std::invalid_argument);
  std::string empty = tmp.str() + "/empty.mp4";
  std::ofstream(empty).close();
  CHECK_THROWS_WITH_AS(studio.import_file(empty), "Video must be between 1 byte and 256 MiB", std::invalid_argument);
  if (have_ffmpeg()) {
    std::string clip = make_clip(tmp);
    std::string id = studio.import_file(clip, "crop");
    CHECK(studio.library().get(id).status != "failed");
    CHECK(testing::wait_until([&] { return studio.library().get(id).status == "ready"; }, 30.0));
    CHECK(studio.clip_frames_path(id).find("frames.raw") != std::string::npos);
    studio.realtime().set_source_mode("library", studio.clip_frames_path(id));
    CHECK(studio.pipeline().get_telemetry()["source_details"]["total_frames"] == 20);
  }
  json h = studio.health();
  for (const char* k : {"version", "mode", "hardware_enabled", "uptime_seconds", "worker_alive", "disk_free_bytes", "disk_total_bytes",
                        "temperature_c", "load_average", "ffmpeg", "ffprobe", "v4l2_ctl", "profile", "realtime", "hardware"})
    CHECK(h.contains(k));
  CHECK(h["mode"] == "realtime-simulation");
  CHECK(h["worker_alive"] == true);
  studio.close();
  CHECK_FALSE(studio.worker_alive());
}

TEST_CASE("environment file: KEY=value lines fill unset variables only") {
  testing::TempDir tmp;
  std::string path = tmp.str() + "/env";
  std::ofstream(path) << "# comment\nMTV_TEST_A=one\n  MTV_TEST_B = two \nnot a line\nMTV_TEST_C=three\n";
  testing::EnvGuard keep("MTV_TEST_C", "already");
  env::unset("MTV_TEST_A");
  env::unset("MTV_TEST_B");
  env::load_env_file(path);
  CHECK(env::get("MTV_TEST_A") == "one");
  CHECK(env::get("MTV_TEST_B") == "two");
  CHECK(env::get("MTV_TEST_C") == "already");
  env::unset("MTV_TEST_A");
  env::unset("MTV_TEST_B");
  env::load_env_file(tmp.str() + "/missing");   // ignored
}

TEST_CASE("logging: file, listener, levels and rotation") {
  testing::TempDir tmp;
  std::vector<log::Record> got;
  log::set_listener([&](const log::Record& r) { got.push_back(r); });
  std::string path = log::setup(tmp.str() + "/logs", 2000, 2);
  REQUIRE_FALSE(path.empty());
  log::info("hello file");
  log::error("something broke");
  log::set_listener(nullptr);
  REQUIRE(got.size() >= 2);
  CHECK(got[got.size() - 1].level == log::Level::Error);
  CHECK(got[got.size() - 1].format().find("ERROR") != std::string::npos);
  std::ifstream in(path);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(all.find("hello file") != std::string::npos);
  CHECK(all.find("something broke") != std::string::npos);
  log::set_echo(false);
  for (int i = 0; i < 100; ++i) log::info("filler line to force a rotation " + std::to_string(i));
  log::set_echo(true);
  CHECK(fs::exists(path + ".1"));
  CHECK(fs::file_size(path) <= 2000);
  log::setup(tmp.str() + "/other");   // detach from the temp file before it is removed
}

#include "mtv/hardware/failsafe.h"

TEST_CASE("a subprocess can be cancelled") {
  std::atomic<bool> cancel{false};
  std::thread t([&] {
    sleep_s(0.3);
    cancel = true;
  });
  double t0 = now_s();
  auto r = proc::run({"/bin/sh", "-c", "sleep 30"}, 60, &cancel);
  t.join();
  CHECK(now_s() - t0 < 3.0);
  CHECK_FALSE(r.ok());
}

TEST_CASE("the crash failsafe switches the STEP clock off") {
  testing::TempDir tmp;
  std::string enable = tmp.str() + "/enable";
  std::ofstream(enable) << "1";
  failsafe::set_pwm_enable_file(enable.c_str());
  failsafe::trigger();
  failsafe::set_pwm_enable_file(nullptr);
  std::ifstream in(enable);
  std::string v;
  in >> v;
  CHECK(v == "0");
}
