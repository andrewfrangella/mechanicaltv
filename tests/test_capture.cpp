// HDMI capture: FFmpeg argv, adapter detection, CSI preparation order, failure reporting and the
// native worker, with a fake bridge and a fake frame reader (from test_capture.py / test_v4l2.py).
#include <doctest/doctest.h>

#include <atomic>
#include <fstream>
#include <vector>

#include "helpers.h"
#include "mtv/capture/csi_control.h"
#include "mtv/capture/ffmpeg_source.h"
#include "mtv/capture/hdmi_capture.h"
#include "mtv/capture/uyvy_sampler.h"
#include "mtv/common/log.h"

using namespace mtv;

namespace {

struct FakeCsi : CsiControl {
  std::atomic<int> edid_loads{0}, locks{0};
  std::atomic<bool> signal{true};
  std::atomic<double> lock_delay{0};
  std::string edid_error;
  DvTimings timings{1920, 1080, 30.0, 74250000, false};
  std::string load_edid(const std::string&, const std::vector<std::uint8_t>& e) override {
    ++edid_loads;
    if (e.size() % 128) return "bad";
    return edid_error;
  }
  std::string query_and_lock(const std::string&, DvTimings* t) override {
    if (lock_delay > 0) sleep_s(lock_delay);
    if (!signal) return "no link (no HDMI source, or the source sends nothing)";
    ++locks;
    *t = timings;
    return "";
  }
};

// Synthetic 1920x1080 UYVY: left half white, right half black.
std::vector<std::uint8_t> uyvy_frame(int w, int h, std::uint8_t left, std::uint8_t right) {
  std::vector<std::uint8_t> f(static_cast<std::size_t>(w) * h * 2);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; x += 2) {
      std::uint8_t* p = &f[(static_cast<std::size_t>(y) * w + x) * 2];
      std::uint8_t Y = x < w / 2 ? left : right;
      p[0] = 128;
      p[1] = Y;
      p[2] = 128;
      p[3] = Y;
    }
  return f;
}

struct ReaderState {
  std::atomic<int> dequeued{0}, requeued{0}, opened{0}, closed{0};
  std::atomic<int> short_next{0};
};

struct FakeReader : FrameReader {
  std::shared_ptr<ReaderState> st;
  std::vector<std::uint8_t> frame = uyvy_frame(1920, 1080, 235, 16);
  explicit FakeReader(std::shared_ptr<ReaderState> s) : st(std::move(s)) { ++st->opened; }
  ~FakeReader() override { ++st->closed; }
  Got dequeue(double) override {
    sleep_s(0.01);
    ++st->dequeued;
    if (st->short_next > 0) {
      --st->short_next;
      return {1, frame.data(), frame.size() / 2};
    }
    return {0, frame.data(), frame.size()};
  }
  void requeue(int) override { ++st->requeued; }
  int width() const override { return 1920; }
  int height() const override { return 1080; }
  int stride() const override { return 3840; }
  bool bt709() const override { return false; }
  int buffer_count() const override { return 4; }
  long error_frames() const override { return 0; }
  long dropped_frames() const override { return 0; }
};

std::unique_ptr<HDMICapture> csi_capture(FakeCsi*& csi_out, std::shared_ptr<ReaderState> st, const std::string& backend = "auto") {
  HDMIOptions o;
  o.device = "/dev/video-fake";
  o.bridge = "csi";
  o.backend = backend;
  o.edid = testing::repo("edid/1080p30.txt");
  auto cap = std::make_unique<HDMICapture>(o);
  auto csi = std::make_unique<FakeCsi>();
  csi_out = csi.get();
  cap->set_csi_control(std::move(csi));
  cap->set_device_exists([](const std::string&) { return true; });
  cap->set_retry_delay(0.05);
  cap->set_reader_factory([st](const std::string&) { return std::make_unique<FakeReader>(st); });
  return cap;
}

}  // namespace

TEST_CASE("ffmpeg argv: input format precedes the input; none lets ffmpeg negotiate") {
  auto cmd = ffmpeg::build_command("/dev/video0");
  auto pos = [&](const std::string& s) { return std::find(cmd.begin(), cmd.end(), s) - cmd.begin(); };
  CHECK(cmd[0] == "ffmpeg");
  CHECK(pos("-input_format") < pos("-i"));
  CHECK(cmd[static_cast<std::size_t>(pos("-input_format") + 1)] == "mjpeg");
  CHECK(cmd.back() == "pipe:1");
  auto csi = ffmpeg::build_command("/dev/video0", "fit", "uyvy422");
  CHECK(std::find(csi.begin(), csi.end(), "uyvy422") != csi.end());
  auto none = ffmpeg::build_command("/dev/video0", "fit", "none");
  CHECK(std::find(none.begin(), none.end(), "-input_format") == none.end());
  CHECK(ffmpeg::build_filter("crop").find("crop=32:25") != std::string::npos);
  CHECK(ffmpeg::build_filter("fit").find("pad=32:25") != std::string::npos);
}

TEST_CASE("ffmpeg frame split takes the disk rows from below the preview") {
  std::vector<std::uint8_t> buf(ffmpeg::FRAME_BYTES_OUT, 7);
  for (int y = 0; y < HEIGHT; ++y)
    for (int x = 0; x < WIDTH; ++x) buf[PREVIEW_BYTES + (static_cast<std::size_t>(y) * PREVIEW_W + x) * 3] = static_cast<std::uint8_t>(y * 10 + x % 10);
  Preview pv;
  Frame f{};
  ffmpeg::split_frame(buf.data(), buf.size(), pv, f);
  CHECK(pv.size() == PREVIEW_BYTES);
  CHECK(f[0] == 0);
  CHECK(f[3 * 32 + 5] == 35);
  CHECK_THROWS_AS(ffmpeg::split_frame(buf.data(), buf.size() - 1, pv, f), std::invalid_argument);
}

TEST_CASE("adapter detection from sysfs: Pi CSI receiver yes, USB dongle no") {
  testing::TempDir sys;
  std::filesystem::create_directories(sys.path / "video0");
  std::filesystem::create_directories(sys.path / "video1");
  std::ofstream(sys.path / "video0" / "name") << "unicam\n";
  std::ofstream(sys.path / "video1" / "name") << "USB Video: USB Video\n";
  CHECK(detect_csi_bridge("/dev/video0", sys.str()));
  CHECK_FALSE(detect_csi_bridge("/dev/video1", sys.str()));
  CHECK_FALSE(detect_csi_bridge("/dev/video9", sys.str()));
}

TEST_CASE("input format and EDID follow the adapter type") {
  HDMIOptions csi;
  csi.bridge = "csi";
  HDMICapture c(csi);
  CHECK(c.input_format() == "uyvy422");
  CHECK(c.edid_path().find("edid/1080p30.txt") != std::string::npos);
  HDMIOptions usb;
  usb.bridge = "usb";
  HDMICapture u(usb);
  CHECK(u.input_format() == "mjpeg");
  CHECK(u.edid_path().empty());
  HDMIOptions off = csi;
  off.edid = "none";
  CHECK(HDMICapture(off).edid_path().empty());
  HDMIOptions bad = csi;
  bad.backend = "nonsense";
  CHECK(HDMICapture(bad).backend_setting() == "auto");
}

TEST_CASE("shipped EDIDs are whole blocks with valid checksums; 1080p30 is the preferred timing") {
  for (const char* name : {"edid/1080p30.txt", "edid/720p60.txt"}) {
    auto e = read_edid_file(testing::repo(name));
    CHECK(e.size() % 128 == 0);
    CHECK(edid_checksums_ok(e));
    CHECK(e[0] == 0x00);
    CHECK(e[1] == 0xff);
  }
  auto e = read_edid_file(testing::repo("edid/1080p30.txt"));
  int clock = e[54] | (e[55] << 8);   // 10 kHz units
  int h = e[56] | ((e[58] >> 4) << 8);
  int v = e[59] | ((e[61] >> 4) << 8);
  CHECK(clock == 7425);
  CHECK(h == 1920);
  CHECK(v == 1080);
  CHECK_THROWS(read_edid_file("/nonexistent/edid.txt"));
}

TEST_CASE("CSI bridge: EDID loaded once, timings locked before every open, frames flow and buffers return") {
  FakeCsi* csi = nullptr;
  auto st = std::make_shared<ReaderState>();
  auto cap = csi_capture(csi, st);
  cap->start();
  REQUIRE(testing::wait_until([&] { return cap->get_frame() != nullptr; }));
  CHECK(csi->edid_loads == 1);
  CHECK(csi->locks >= 1);
  CHECK(st->opened == 1);
  json t = cap->get_telemetry();
  CHECK(t["backend"] == "v4l2");
  CHECK(t["signal"] == true);
  CHECK(t["picture"] == true);
  CHECK(t["edid_loaded"] == true);
  CHECK(t["input_resolution"] == "1920x1080");
  CHECK(cap->get_preview() != nullptr);
  // left half white, right half black, letterboxed into disk rows 3-20
  Frame f = *cap->get_frame();
  CHECK(f[0] == 0);
  CHECK(f[3 * 32 + 0] == 255);
  CHECK(f[3 * 32 + 31] == 0);
  CHECK(f[21 * 32 + 0] == 0);
  CHECK(testing::wait_until([&] { return st->requeued >= 5; }));
  CHECK(st->requeued >= st->dequeued - 1);   // every buffer goes back to the driver
  cap->stop();
  CHECK(st->closed == st->opened);
  CHECK(cap->get_frame() == nullptr);
  // a second run reopens but keeps the EDID
  cap->start();
  REQUIRE(testing::wait_until([&] { return cap->get_frame() != nullptr; }));
  CHECK(csi->edid_loads == 1);
  CHECK(csi->locks >= 2);
  cap->stop();
}

TEST_CASE("no HDMI signal is reported with the reason and nothing is opened") {
  FakeCsi* csi = nullptr;
  auto st = std::make_shared<ReaderState>();
  auto cap = csi_capture(csi, st);
  csi->signal = false;
  cap->start();
  REQUIRE(testing::wait_until([&] { return cap->error().find("No HDMI signal") != std::string::npos; }));
  CHECK(cap->error().find("EDID loaded") != std::string::npos);
  CHECK(st->opened == 0);
  CHECK(cap->get_frame() == nullptr);
  csi->signal = true;   // source attached later: recovers without a restart
  CHECK(testing::wait_until([&] { return cap->get_frame() != nullptr; }));
  cap->stop();
}

TEST_CASE("EDID load failure names the likely cause") {
  FakeCsi* csi = nullptr;
  auto cap = csi_capture(csi, std::make_shared<ReaderState>());
  csi->edid_error = "Inappropriate ioctl for device";
  cap->start();
  REQUIRE(testing::wait_until([&] { return cap->error().find("EDID load failed") != std::string::npos; }));
  CHECK(cap->error().find("not a Pi camera") != std::string::npos);
  cap->stop();
}

TEST_CASE("missing capture device is reported, not masked") {
  HDMIOptions o;
  o.device = "/dev/video-missing";
  HDMICapture cap(o);
  cap.set_retry_delay(0.05);
  cap.start();
  REQUIRE(testing::wait_until([&] { return !cap.error().empty(); }));
  CHECK(cap.error().find("dtoverlay=tc358743") != std::string::npos);
  cap.stop();
}

TEST_CASE("stop() during a slow open does not leave a device open") {
  FakeCsi* csi = nullptr;
  auto st = std::make_shared<ReaderState>();
  auto cap = csi_capture(csi, st);
  csi->lock_delay = 0.4;
  cap->start();
  sleep_s(0.1);
  cap->stop();
  sleep_s(0.5);
  CHECK(st->opened == st->closed);
  CHECK(cap->get_telemetry()["connected"] == false);
}

TEST_CASE("one short frame is skipped without reopening") {
  FakeCsi* csi = nullptr;
  auto st = std::make_shared<ReaderState>();
  st->short_next = 1;
  auto cap = csi_capture(csi, st);
  cap->start();
  REQUIRE(testing::wait_until([&] { return cap->get_frame() != nullptr && st->dequeued > 3; }));
  CHECK(st->opened == 1);
  CHECK(cap->get_telemetry()["bad_frames"] == 1);
  cap->stop();
}

TEST_CASE("explicit v4l2 backend reports an open failure instead of using ffmpeg; auto falls back") {
  for (std::string backend : {"v4l2", "auto"}) {
    FakeCsi* csi = nullptr;
    auto cap = csi_capture(csi, std::make_shared<ReaderState>(), backend);
    std::atomic<int> ffmpeg_starts{0};
    cap->set_reader_factory([](const std::string&) -> std::unique_ptr<FrameReader> { throw std::runtime_error("EBUSY"); });
    cap->set_ffmpeg_argv([&] {
      ++ffmpeg_starts;
      return std::vector<std::string>{"/bin/sh", "-c", "sleep 5"};
    });
    cap->start();
    REQUIRE(testing::wait_until([&] { return cap->error().find("native V4L2 open failed") != std::string::npos || ffmpeg_starts > 0; }));
    sleep_s(0.2);
    if (backend == "v4l2") CHECK(ffmpeg_starts == 0);
    else CHECK(ffmpeg_starts >= 1);
    cap->stop();
  }
}

TEST_CASE("a USB dongle never uses the native reader") {
  HDMIOptions o;
  o.device = "/dev/video-fake";
  o.bridge = "usb";
  HDMICapture cap(o);
  std::atomic<int> native{0}, ffmpeg_starts{0};
  cap.set_device_exists([](const std::string&) { return true; });
  cap.set_reader_factory([&](const std::string&) -> std::unique_ptr<FrameReader> {
    ++native;
    throw std::runtime_error("no");
  });
  cap.set_ffmpeg_argv([&] {
    ++ffmpeg_starts;
    return std::vector<std::string>{"/bin/sh", "-c", "sleep 5"};
  });
  cap.start();
  REQUIRE(testing::wait_until([&] { return ffmpeg_starts > 0; }));
  CHECK(native == 0);
  cap.stop();
}

TEST_CASE("ffmpeg path: frames flow; a dead ffmpeg is reported with its last stderr line and the frame is cleared") {
  HDMIOptions o;
  o.device = "/dev/video-fake";
  o.bridge = "usb";
  HDMICapture cap(o);
  cap.set_device_exists([](const std::string&) { return true; });
  cap.set_retry_delay(0.05);
  std::string script = "head -c " + std::to_string(ffmpeg::FRAME_BYTES_OUT) +
                       " /dev/zero; head -c 1000 /dev/zero; echo 'Device or resource busy' >&2; sleep 0.3; exit 1";
  cap.set_ffmpeg_argv([&] { return std::vector<std::string>{"/bin/sh", "-c", script}; });
  cap.start();
  REQUIRE(testing::wait_until([&] { return cap.get_frame() != nullptr; }));
  CHECK(cap.get_telemetry()["backend"] == "ffmpeg");
  REQUIRE(testing::wait_until([&] { return cap.error().find("ffmpeg exited (1)") != std::string::npos; }));
  CHECK(cap.error().find("Device or resource busy") != std::string::npos);
  cap.stop();
  CHECK(cap.get_frame() == nullptr);
}

TEST_CASE("a stalled stream is reopened") {
  HDMIOptions o;
  o.device = "/dev/video-fake";
  o.bridge = "usb";
  HDMICapture cap(o);
  cap.set_device_exists([](const std::string&) { return true; });
  cap.set_retry_delay(0.05);
  cap.set_stall_timeout(0.4);
  std::atomic<int> starts{0};
  std::atomic<bool> logged{false};
  log::set_listener([&](const log::Record& r) {
    if (r.message.find("no frames for 0.4s") != std::string::npos) logged = true;
  });
  cap.set_ffmpeg_argv([&] {
    ++starts;
    return std::vector<std::string>{"/bin/sh", "-c", "sleep 30"};
  });
  cap.start();
  REQUIRE(testing::wait_until([&] { return starts >= 2; }, 5.0));
  CHECK(logged);
  cap.stop();
  log::set_listener(nullptr);
}

TEST_CASE("UYVY sampler: placement snaps letterboxes to whole disk cells") {
  Placement p = placement(1920, 1080, 320, 250, 10);
  CHECK(p.w == 320);
  CHECK(p.h == 180);
  CHECK(p.x == 0);
  CHECK(p.y == 30);
  Placement q = placement(1280, 1024, 320, 250, 10);   // 5:4 pillarbox
  CHECK(q.h == 250);
  CHECK(q.x % 10 == 0);
}

TEST_CASE("UYVY sampler: luma is full range on the disk; crop fills every row; short frames are rejected") {
  auto f = uyvy_frame(720, 480, 235, 16);
  UYVYSampler fit(720, 480, 1440, "fit");
  Preview pv;
  Frame d{};
  fit.convert(f.data(), f.size(), pv, d);
  int lit_row = fit.picture().y / 10;
  CHECK(d[static_cast<std::size_t>(lit_row * 32)] == 255);   // video white 235 -> 255
  CHECK(d[static_cast<std::size_t>(lit_row * 32 + 31)] == 0);   // video black 16 -> 0
  CHECK(pv[(static_cast<std::size_t>(fit.picture().y) * 320) * 3] == 255);   // white preview pixel
  auto hd = uyvy_frame(1920, 1080, 235, 235);
  UYVYSampler crop(1920, 1080, 3840, "crop");
  crop.convert(hd.data(), hd.size(), pv, d);
  for (int y = 0; y < 25; ++y) CHECK(d[static_cast<std::size_t>(y * 32)] == 255);
  CHECK_THROWS_AS(crop.convert(hd.data(), hd.size() - 10, pv, d), std::invalid_argument);
}

TEST_CASE("UYVY sampler handles odd widths without reading past the line") {
  const int w = 241, h = 200, stride = 482;
  std::vector<std::uint8_t> f(static_cast<std::size_t>(stride) * h, 128);
  UYVYSampler s(w, h, stride, "fit");
  Preview pv;
  Frame d{};
  s.convert(f.data(), f.size(), pv, d);   // under ASan this was a 1-byte overread
  CHECK(pv.size() == PREVIEW_BYTES);
}

TEST_CASE("a source that stops sending frames turns black within a second, not after the 5 s reopen") {
  FakeCsi* csi = nullptr;
  auto st = std::make_shared<ReaderState>();
  auto cap = csi_capture(csi, st);
  struct Frozen : FakeReader {
    using FakeReader::FakeReader;
    std::atomic<int> left{5};
    Got dequeue(double t) override {
      if (left-- > 0) return FakeReader::dequeue(t);
      sleep_s(0.05);
      return {-1, nullptr, 0};
    }
  };
  cap->set_reader_factory([st](const std::string&) { return std::make_unique<Frozen>(st); });
  cap->start();
  REQUIRE(testing::wait_until([&] { return cap->get_frame() != nullptr; }));
  CHECK(testing::wait_until([&] { return cap->get_frame() == nullptr; }, 2.0));
  CHECK(cap->get_preview() == nullptr);
  cap->stop();
}
