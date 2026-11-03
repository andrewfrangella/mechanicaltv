// Procedural calibration and test patterns (port of capture.TestPatternGenerator).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mtv/capture/video_source.h"

namespace mtv {

class TestPatternGenerator : public VideoSource {
 public:
  static const std::vector<std::string>& patterns();   // the 10 pattern names in UI order
  static bool is_pattern(const std::string& name);

  explicit TestPatternGenerator(const std::string& pattern = "smpte_bars");
  ~TestPatternGenerator() override;

  void set_pattern(const std::string& name);           // unknown names are ignored
  void update_pattern(const std::string& name);        // set and regenerate the frame at t = 0
  std::string pattern() const;
  void start() override;                               // 10 fps generator thread
  void stop() override;
  FramePtr get_frame() override;
  json get_telemetry() override;                       // type, pattern, running, frames_generated, connected

  // Pure function: the frame for pattern `name` at animation time t (seconds since start).
  static Frame generate(const std::string& name, double t);

 private:
  void loop();
  mutable std::mutex mutex_;
  std::string pattern_;
  FramePtr frame_;
  std::atomic<bool> running_{false};
  std::mutex lifecycle_;   // start()/stop() from different threads
  std::thread thread_;
  double start_time_ = 0.0;
  double fps_ = 10.0;
  std::atomic<long> frame_count_{0};
};

}  // namespace mtv
