#include "mtv/capture/pipeline.h"

#include <algorithm>
#include <stdexcept>

namespace mtv {

PixelMap build_pixel_map(int phase_offset, bool invert_x, bool invert_y) {
  PixelMap map{};
  for (int i = 0; i < FRAME_PIXELS; ++i) {
    int s = (i + phase_offset) % FRAME_PIXELS;
    int col = s / HEIGHT;
    int row = s % HEIGHT;
    int x = invert_x ? col : (WIDTH - 1 - col);
    int y = invert_y ? row : (HEIGHT - 1 - row);
    map[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(y * WIDTH + x);
  }
  return map;
}

void serialize_nipkow(const Transform& t, const Frame& frame, NipkowStream& out) {
  // brightness in 1/65536 fixed point, truncating like Python's int(gamma_val * b)
  const double b = std::clamp(t.brightness, 0.0, 1.0);
  for (int i = 0; i < FRAME_PIXELS; ++i) {
    int v = t.lut[frame[t.map[static_cast<std::size_t>(i)]]];
    int scaled = static_cast<int>(v * b);
    out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
  }
}

FramePipeline::FramePipeline(const HDMIOptions& hdmi)
    : hdmi_(std::make_unique<HDMICapture>(hdmi)), pattern_gen_(std::make_unique<TestPatternGenerator>("smpte_bars")) {
  publish_transform();
  pattern_gen_->start();
}

FramePipeline::~FramePipeline() { close(); }

std::string FramePipeline::source_mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return source_mode_;
}

void FramePipeline::set_source_mode(const std::string& mode, const std::string& library_path) {
  if (mode != "hdmi" && mode != "pattern" && mode != "library") throw std::invalid_argument("Unknown source mode: " + mode);
  std::lock_guard<std::mutex> sw(switch_mutex_);
  std::string old;
  std::shared_ptr<LibraryClipSource> old_lib;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old = source_mode_;
    old_lib = library_source_;
  }
  // Stop the current source outside the data lock (the HDMI worker can take a while to exit).
  if (old == "hdmi") hdmi_->stop();
  else if (old == "pattern") pattern_gen_->stop();
  else if (old == "library" && old_lib) old_lib->stop();

  std::shared_ptr<LibraryClipSource> lib = old_lib;
  if (mode == "library" && !library_path.empty()) lib = std::make_shared<LibraryClipSource>(library_path);
  if (mode == "hdmi") hdmi_->start();
  else if (mode == "pattern") pattern_gen_->start();
  else if (mode == "library" && lib) lib->start();

  std::lock_guard<std::mutex> lock(mutex_);
  source_mode_ = mode;
  library_source_ = lib;
}

void FramePipeline::set_calibration(const Calibration& cal) {
  std::lock_guard<std::mutex> lock(cal_mutex_);
  if (cal.phase_offset) phase_offset_ = ((*cal.phase_offset % FRAME_PIXELS) + FRAME_PIXELS) % FRAME_PIXELS;
  if (cal.invert_x) invert_x_ = *cal.invert_x;
  if (cal.invert_y) invert_y_ = *cal.invert_y;
  if (cal.gamma) gamma_ = std::clamp(*cal.gamma, 0.2, 3.5);
  if (cal.contrast) contrast_ = std::clamp(*cal.contrast, 0.1, 3.0);
  if (cal.brightness) brightness_ = std::clamp(*cal.brightness, 0.0, 1.0);
  publish_transform();
}

void FramePipeline::publish_transform() {
  auto t = std::make_shared<Transform>();
  t->lut = build_gamma_lut(gamma_, contrast_);
  t->map = build_pixel_map(phase_offset_, invert_x_, invert_y_);
  t->brightness = brightness_;
  std::lock_guard<std::mutex> lock(transform_mutex_);
  transform_ = std::move(t);
}

std::shared_ptr<const Transform> FramePipeline::transform() const {
  std::lock_guard<std::mutex> lock(transform_mutex_);
  return transform_;
}

FramePtr FramePipeline::get_latest_frame() {
  std::string mode;
  std::shared_ptr<LibraryClipSource> lib;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mode = source_mode_;
    lib = library_source_;
  }
  FramePtr frame;
  if (mode == "hdmi") {
    // No signal shows black, never a stale frame or a test pattern, so a capture failure is visible.
    frame = hdmi_->get_frame();
  } else if (mode == "pattern") {
    frame = pattern_gen_->get_frame();
  } else if (mode == "library") {
    if (lib) frame = lib->get_frame();
    if (!frame) frame = pattern_gen_->get_frame();
  }
  return frame ? frame : black_frame();
}

PreviewPtr FramePipeline::get_source_preview() {
  if (source_mode() == "hdmi") return hdmi_->get_preview();
  return nullptr;
}

NipkowStream FramePipeline::serialize_nipkow_stream(const Frame& frame) const {
  NipkowStream out{};
  serialize_nipkow(*transform(), frame, out);
  return out;
}

Frame FramePipeline::reconstruct_display_preview(const NipkowStream& stream) const {
  auto t = transform();
  Frame out{};
  for (int i = 0; i < FRAME_PIXELS; ++i) out[t->map[static_cast<std::size_t>(i)]] = stream[static_cast<std::size_t>(i)];
  return out;
}

json FramePipeline::get_telemetry() {
  std::string mode;
  std::shared_ptr<LibraryClipSource> lib;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mode = source_mode_;
    lib = library_source_;
  }
  json details = json::object();
  if (mode == "hdmi") details = hdmi_->get_telemetry();
  else if (mode == "pattern") details = pattern_gen_->get_telemetry();
  else if (mode == "library" && lib) details = lib->get_telemetry();
  return {{"source_mode", mode},       {"gamma", gamma_.load()},       {"brightness", brightness_.load()},
          {"contrast", contrast_.load()}, {"invert_x", invert_x_.load()}, {"invert_y", invert_y_.load()},
          {"phase_offset", phase_offset_.load()}, {"source_details", details}};
}

void FramePipeline::close() {
  std::lock_guard<std::mutex> sw(switch_mutex_);   // never in the middle of a source switch
  if (hdmi_) hdmi_->stop();
  if (pattern_gen_) pattern_gen_->stop();
  std::shared_ptr<LibraryClipSource> lib;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    lib = library_source_;
  }
  if (lib) lib->stop();
}

}  // namespace mtv
