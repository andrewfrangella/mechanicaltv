#include "mtv/capture/library_clip.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "mtv/common/clock.h"
#include "mtv/common/log.h"

namespace mtv {

LibraryClipSource::LibraryClipSource(const std::string& raw_path, double fps, bool loop)
    : path_(raw_path), fps_(fps > 0 ? fps : 10.0), loop_(loop) {
  std::ifstream in(path_, std::ios::binary);
  if (in) frames_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  total_frames_ = static_cast<int>(frames_.size() / FRAME_PIXELS);
  Frame first{};
  if (total_frames_ > 0) std::copy_n(frames_.begin(), FRAME_PIXELS, first.begin());
  frame_ = std::make_shared<const Frame>(first);
}

LibraryClipSource::~LibraryClipSource() { stop(); }

void LibraryClipSource::start() {
  std::lock_guard<std::mutex> life(lifecycle_);
  if (running_ || total_frames_ == 0) return;
  if (thread_.joinable()) thread_.join();
  running_ = true;
  thread_ = std::thread([this] {
    log::set_thread_name("library-clip-worker");
    loop();
  });
}

void LibraryClipSource::stop() {
  std::lock_guard<std::mutex> life(lifecycle_);
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void LibraryClipSource::loop() {
  const double interval = 1.0 / fps_;
  int idx = 0;
  double next = now_s();
  while (running_ && total_frames_ > 0) {
    Frame f{};
    std::copy_n(frames_.begin() + static_cast<std::ptrdiff_t>(idx) * FRAME_PIXELS, FRAME_PIXELS, f.begin());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_ = std::make_shared<const Frame>(f);
    }
    current_index_ = idx;
    if (++idx >= total_frames_) {
      if (loop_) idx = 0;
      else break;
    }
    // Absolute schedule: clips keep their 10 fps on average instead of drifting by the work time.
    next += interval;
    double now = now_s();
    if (next < now - interval) next = now;
    while (running_ && now_s() < next) sleep_s(std::min(0.02, next - now_s()));
  }
}

FramePtr LibraryClipSource::get_frame() {
  std::lock_guard<std::mutex> lock(mutex_);
  return frame_;
}

json LibraryClipSource::get_telemetry() {
  return {{"type", "library"}, {"path", path_}, {"running", running_.load()}, {"total_frames", total_frames_},
          {"current_frame", current_index_.load()}, {"connected", total_frames_ > 0}};
}

}  // namespace mtv
