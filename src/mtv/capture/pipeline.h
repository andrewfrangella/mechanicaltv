// Active video sources, optical transforms and Nipkow serialisation (port of capture.FramePipeline).
//
// The realtime LED thread calls get_latest_frame() and transform() once per revolution; both only
// copy a shared pointer under a short lock, so switching sources (which can take seconds while the
// HDMI worker shuts down) never blocks the disk.
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "mtv/capture/gamma.h"
#include "mtv/capture/hdmi_capture.h"
#include "mtv/capture/library_clip.h"
#include "mtv/capture/patterns.h"

namespace mtv {

struct Calibration {
  std::optional<int> phase_offset = std::nullopt;
  std::optional<bool> invert_x = std::nullopt;
  std::optional<bool> invert_y = std::nullopt;
  std::optional<double> gamma = std::nullopt;
  std::optional<double> brightness = std::nullopt;
  std::optional<double> contrast = std::nullopt;
};

// Pulse index -> row-major pixel index (bitluni's spiral):
//   s = (i + phase_offset) % 800; col = s / 25; row = s % 25;
//   x = invert_x ? col : 31 - col; y = invert_y ? row : 24 - row; map[i] = y * 32 + x
using PixelMap = std::array<std::uint16_t, FRAME_PIXELS>;
PixelMap build_pixel_map(int phase_offset, bool invert_x, bool invert_y);

struct Transform {
  Lut lut{};
  PixelMap map{};
  double brightness = 0.7;
};

// Gamma LUT + master brightness, in disk order: out[i] = int(lut[frame[map[i]]] * brightness).
void serialize_nipkow(const Transform& t, const Frame& frame, NipkowStream& out);

class FramePipeline {
 public:
  explicit FramePipeline(const HDMIOptions& hdmi = {});
  ~FramePipeline();

  // 'hdmi', 'pattern' or 'library'; throws std::invalid_argument("Unknown source mode: X").
  void set_source_mode(const std::string& mode, const std::string& library_path = "");
  std::string source_mode() const;
  void set_calibration(const Calibration& cal);
  FramePtr get_latest_frame();          // never null (black when the source has nothing)
  PreviewPtr get_source_preview();      // live HDMI picture, else nullptr
  NipkowStream serialize_nipkow_stream(const Frame& frame) const;
  Frame reconstruct_display_preview(const NipkowStream& stream) const;
  json get_telemetry();
  void close();

  double gamma() const { return gamma_; }
  double contrast() const { return contrast_; }
  double brightness() const { return brightness_; }
  bool invert_x() const { return invert_x_; }
  bool invert_y() const { return invert_y_; }
  int phase_offset() const { return phase_offset_; }
  std::shared_ptr<const Transform> transform() const;

  HDMICapture& hdmi() { return *hdmi_; }
  TestPatternGenerator& pattern_gen() { return *pattern_gen_; }

 private:
  void publish_transform();
  std::unique_ptr<HDMICapture> hdmi_;
  std::unique_ptr<TestPatternGenerator> pattern_gen_;
  std::shared_ptr<LibraryClipSource> library_source_;
  std::mutex switch_mutex_;      // serialises source switches
  mutable std::mutex mutex_;     // guards source_mode_ / library_source_ pointer
  std::string source_mode_ = "pattern";
  std::mutex cal_mutex_;
  std::atomic<double> gamma_{1.8};
  std::atomic<double> contrast_{1.0};
  std::atomic<double> brightness_{0.7};
  std::atomic<bool> invert_x_{false};
  std::atomic<bool> invert_y_{false};
  std::atomic<int> phase_offset_{0};
  mutable std::mutex transform_mutex_;
  std::shared_ptr<const Transform> transform_;
};

}  // namespace mtv
