// Patterns, gamma, the Nipkow spiral mapping and the frame pipeline (from test_realtime.py).
#include <doctest/doctest.h>

#include <set>

#include "helpers.h"
#include "mtv/capture/gamma.h"
#include "mtv/capture/pipeline.h"

using namespace mtv;

TEST_CASE("gamma LUT: identity, contrast and clamping") {
  auto linear = build_gamma_lut(1.0, 1.0);
  for (int i = 0; i < 256; ++i) CHECK(linear[static_cast<std::size_t>(i)] == i);
  auto g = build_gamma_lut(1.8, 1.0);
  CHECK(g[0] == 0);
  CHECK(g[255] == 255);
  CHECK(g[128] < 128);   // gamma > 1 darkens midtones
  auto c = build_gamma_lut(1.0, 2.0);
  CHECK(c[128] == 128);
  CHECK(c[200] == 255);   // (200 - 128) * 2 + 128 = 272 -> clamped
  CHECK(c[50] == 0);
  // Python's round() is round-half-even; 255 * (127.5/255) never appears, but check a known value
  CHECK(build_gamma_lut(2.0, 1.0)[128] == 64);   // 255 * (128/255)^2 = 64.25
}

TEST_CASE("test patterns: names, sizes and representative pixels") {
  CHECK(TestPatternGenerator::patterns().size() == 10);
  for (const auto& name : TestPatternGenerator::patterns()) {
    Frame f = TestPatternGenerator::generate(name, 0.3);
    CHECK(f.size() == 800);
  }
  Frame bars = TestPatternGenerator::generate("smpte_bars", 0);
  CHECK(bars[0] == 255);
  CHECK(bars[4] == 219);
  CHECK(bars[31] == 0);
  Frame ramp = TestPatternGenerator::generate("grayscale_ramp", 0);
  CHECK(ramp[0] == 0);
  CHECK(ramp[31] == 255);
  Frame vramp = TestPatternGenerator::generate("vertical_ramp", 0);
  CHECK(vramp[4 * 32] == 42);   // 42.5 rounds half to even, like Python
  Frame grid = TestPatternGenerator::generate("grid", 0);
  CHECK(grid[0] == 255);
  CHECK(grid[1 * 32 + 1] == 0);
  CHECK(grid[12 * 32 + 5] == 255);   // centre row
  Frame pulse_on = TestPatternGenerator::generate("pulse_test", 0.1);
  Frame pulse_off = TestPatternGenerator::generate("pulse_test", 0.6);
  CHECK(pulse_on[0] == 255);
  CHECK(pulse_off[0] == 0);
  Frame col = TestPatternGenerator::generate("single_column", 1.0);   // column 4
  CHECK(col[4] == 255);
  CHECK(col[5] == 0);
  TestPatternGenerator gen("nonsense");
  CHECK(gen.pattern() == "smpte_bars");
  gen.set_pattern("grid");
  CHECK(gen.pattern() == "grid");
  gen.set_pattern("not-a-pattern");
  CHECK(gen.pattern() == "grid");
}

TEST_CASE("Nipkow mapping follows bitluni's spiral and is a permutation") {
  PixelMap m = build_pixel_map(0, false, false);
  // pixel 0: col 0 -> x = 31, row 0 -> y = 24
  CHECK(m[0] == 24 * 32 + 31);
  // pixel 1: same hole, next line up
  CHECK(m[1] == 23 * 32 + 31);
  // pixel 25: next hole
  CHECK(m[25] == 24 * 32 + 30);
  std::set<int> seen(m.begin(), m.end());
  CHECK(seen.size() == 800);
  PixelMap inv = build_pixel_map(0, true, true);
  CHECK(inv[0] == 0);
  PixelMap shifted = build_pixel_map(25, false, false);
  CHECK(shifted[0] == m[25]);
  CHECK(build_pixel_map(799, false, false)[1] == m[0]);
}

TEST_CASE("serialisation applies gamma and brightness once; reconstruction inverts the map") {
  FramePipeline p;
  p.set_calibration({.gamma = 1.0, .brightness = 0.5});
  Frame f{};
  for (int i = 0; i < 800; ++i) f[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i % 256);
  NipkowStream s = p.serialize_nipkow_stream(f);
  auto t = p.transform();
  for (int i = 0; i < 800; ++i) CHECK(s[static_cast<std::size_t>(i)] == static_cast<int>(f[t->map[static_cast<std::size_t>(i)]] * 0.5));
  p.set_calibration({.brightness = 1.0});
  Frame back = p.reconstruct_display_preview(p.serialize_nipkow_stream(f));
  CHECK(back == f);
  p.close();
}

TEST_CASE("calibration clamps like the Python pipeline") {
  FramePipeline p;
  p.set_calibration({.phase_offset = -1, .gamma = 9.0, .brightness = 2.0, .contrast = 0.0});
  CHECK(p.phase_offset() == 799);
  CHECK(p.gamma() == doctest::Approx(3.5));
  CHECK(p.brightness() == doctest::Approx(1.0));
  CHECK(p.contrast() == doctest::Approx(0.1));
  p.set_calibration({.phase_offset = 1601});
  CHECK(p.phase_offset() == 1);
  p.close();
}

TEST_CASE("source modes: unknown is rejected, library falls back to the pattern, hdmi without signal is black") {
  testing::TempDir tmp;
  HDMIOptions o;
  o.device = tmp.str() + "/no-such-video";
  FramePipeline p(o);
  CHECK_THROWS_AS(p.set_source_mode("vhs"), std::invalid_argument);
  p.set_source_mode("library");   // no clip loaded
  CHECK(*p.get_latest_frame() == TestPatternGenerator::generate("smpte_bars", 0));
  p.set_source_mode("hdmi");
  CHECK(testing::wait_until([&] { return !p.hdmi().error().empty(); }));
  CHECK(p.hdmi().error().find("not found") != std::string::npos);
  Frame black{};
  CHECK(*p.get_latest_frame() == black);   // never a stale frame or a test pattern
  CHECK(p.get_source_preview() == nullptr);
  json t = p.get_telemetry();
  CHECK(t["source_mode"] == "hdmi");
  CHECK(t["source_details"]["signal"] == false);
  CHECK(t["source_details"]["picture"] == false);
  p.set_source_mode("pattern");
  CHECK(p.source_mode() == "pattern");
  p.close();
}

TEST_CASE("library clips stream their frames at 10 fps and loop") {
  testing::TempDir tmp;
  std::string path = tmp.str() + "/frames.raw";
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    for (int n = 0; n < 3; ++n)
      for (int i = 0; i < 800; ++i) std::fputc(n * 100, f);
    std::fclose(f);
  }
  LibraryClipSource clip(path);
  CHECK(clip.total_frames() == 3);
  CHECK((*clip.get_frame())[0] == 0);
  clip.start();
  CHECK(testing::wait_until([&] { return (*clip.get_frame())[0] == 200; }, 2.0));
  CHECK(testing::wait_until([&] { return (*clip.get_frame())[0] == 0; }, 2.0));   // looped
  clip.stop();
  CHECK(clip.get_telemetry()["total_frames"] == 3);
}
