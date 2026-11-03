// Abstract real-time video input (port of capture.VideoSource).
#pragma once
#include "mtv/common/frame.h"
#include "mtv/common/json.h"

namespace mtv {

class VideoSource {
 public:
  virtual ~VideoSource() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  // Newest 32x25 frame, or nullptr when the source has none (HDMI without signal).
  virtual FramePtr get_frame() = 0;
  virtual json get_telemetry() = 0;
};

}  // namespace mtv
