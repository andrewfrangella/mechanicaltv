#include "mtv/capture/patterns.h"

#include <algorithm>
#include <cmath>

#include "mtv/common/clock.h"
#include "mtv/common/log.h"

namespace mtv {

const std::vector<std::string>& TestPatternGenerator::patterns() {
  static const std::vector<std::string> names = {
      "smpte_bars", "grayscale_ramp", "vertical_ramp", "grid", "rotating_bar",
      "pulse_test", "checkerboard", "single_column", "single_row", "diagonal_line"};
  return names;
}

bool TestPatternGenerator::is_pattern(const std::string& name) {
  const auto& p = patterns();
  return std::find(p.begin(), p.end(), name) != p.end();
}

TestPatternGenerator::TestPatternGenerator(const std::string& pattern)
    : pattern_(is_pattern(pattern) ? pattern : "smpte_bars") {
  start_time_ = now_s();
  update_pattern(pattern_);
}

TestPatternGenerator::~TestPatternGenerator() { stop(); }

void TestPatternGenerator::set_pattern(const std::string& name) {
  if (!is_pattern(name)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pattern_ = name;
}

void TestPatternGenerator::update_pattern(const std::string& name) {
  set_pattern(name);
  std::lock_guard<std::mutex> lock(mutex_);
  frame_ = std::make_shared<const Frame>(generate(pattern_, 0.0));
}

std::string TestPatternGenerator::pattern() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pattern_;
}

void TestPatternGenerator::start() {
  std::lock_guard<std::mutex> life(lifecycle_);
  if (running_.exchange(true)) return;
  if (thread_.joinable()) thread_.join();
  start_time_ = now_s();
  thread_ = std::thread([this] {
    log::set_thread_name("test-pattern-worker");
    loop();
  });
}

void TestPatternGenerator::stop() {
  std::lock_guard<std::mutex> life(lifecycle_);
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void TestPatternGenerator::loop() {
  const double interval = 1.0 / fps_;
  while (running_) {
    double t0 = now_s();
    std::string name = pattern();
    auto frame = std::make_shared<const Frame>(generate(name, t0 - start_time_));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_ = std::move(frame);
    }
    ++frame_count_;
    double elapsed = now_s() - t0;
    double wait = std::max(0.001, interval - elapsed);
    // Sleep in short steps so stop() returns promptly.
    double until = now_s() + wait;
    while (running_ && now_s() < until) sleep_s(std::min(0.02, until - now_s()));
  }
}

Frame TestPatternGenerator::generate(const std::string& pat, double t) {
  Frame buf{};
  auto at = [&](int x, int y) -> std::uint8_t& { return buf[static_cast<std::size_t>(y * WIDTH + x)]; };
  if (pat == "smpte_bars") {
    // 8 vertical bars of descending intensity, 4 columns each
    static const std::uint8_t levels[8] = {255, 219, 182, 146, 109, 73, 36, 0};
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x) at(x, y) = levels[std::min(7, x / 4)];
  } else if (pat == "grayscale_ramp") {
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x)
        at(x, y) = static_cast<std::uint8_t>(std::nearbyint(x * 255.0 / (WIDTH - 1)));
  } else if (pat == "vertical_ramp") {
    for (int y = 0; y < HEIGHT; ++y) {
      auto v = static_cast<std::uint8_t>(std::nearbyint(y * 255.0 / (HEIGHT - 1)));
      for (int x = 0; x < WIDTH; ++x) at(x, y) = v;
    }
  } else if (pat == "grid") {
    const int cx = WIDTH / 2, cy = HEIGHT / 2;
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x) {
        bool border = x == 0 || x == WIDTH - 1 || y == 0 || y == HEIGHT - 1;
        bool cross = x == cx || y == cy;
        at(x, y) = (border || cross) ? 255 : 0;
      }
  } else if (pat == "rotating_bar") {
    // Clock hand around the centre, 0.5 revolutions per second
    const double two_pi = 2.0 * M_PI;
    double angle = std::fmod(t * 0.5 * two_pi, two_pi);
    if (angle < 0) angle += two_pi;
    const double cx = (WIDTH - 1) / 2.0, cy = (HEIGHT - 1) / 2.0;
    const double ca = std::cos(angle), sa = std::sin(angle);
    for (int y = 0; y < HEIGHT; ++y) {
      double dy = y - cy;
      for (int x = 0; x < WIDTH; ++x) {
        double dx = x - cx;
        double dist = std::fabs(dx * sa - dy * ca);
        double along = dx * ca + dy * sa;
        if (dist <= 0.8 && along >= -1.0) at(x, y) = 255;
        else if (dist <= 1.5 && along >= -1.0) at(x, y) = 120;
        else at(x, y) = 10;
      }
    }
  } else if (pat == "pulse_test") {
    // Full black / full white, toggling every half second
    std::uint8_t v = (static_cast<long long>(std::floor(t * 2.0)) % 2 == 0) ? 255 : 0;
    buf.fill(v);
  } else if (pat == "checkerboard") {
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x) at(x, y) = ((x / 4 + y / 4) % 2 == 0) ? 255 : 0;
  } else if (pat == "single_column") {
    int col = static_cast<int>(static_cast<long long>(std::floor(t * 4.0)) % WIDTH);
    for (int y = 0; y < HEIGHT; ++y) at(col, y) = 255;
  } else if (pat == "single_row") {
    int row = static_cast<int>(static_cast<long long>(std::floor(t * 4.0)) % HEIGHT);
    for (int x = 0; x < WIDTH; ++x) at(x, row) = 255;
  } else if (pat == "diagonal_line") {
    int shift = static_cast<int>(static_cast<long long>(std::floor(t * 8.0)) % (WIDTH + HEIGHT));
    const int half = WIDTH / 2;
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x)
        if ((x + y) % half == shift % half) at(x, y) = 255;
  }
  return buf;
}

FramePtr TestPatternGenerator::get_frame() {
  std::lock_guard<std::mutex> lock(mutex_);
  return frame_;
}

json TestPatternGenerator::get_telemetry() {
  return {{"type", "pattern"}, {"pattern", pattern()}, {"running", running_.load()},
          {"frames_generated", frame_count_.load()}, {"connected", true}};
}

}  // namespace mtv
