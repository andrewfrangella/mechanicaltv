// Streams prepared 32x25 raw clips from the library in real time (port of capture.LibraryClipSource).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mtv/capture/video_source.h"

namespace mtv {

class LibraryClipSource : public VideoSource {
 public:
  explicit LibraryClipSource(const std::string& raw_path, double fps = 10.0, bool loop = true);
  ~LibraryClipSource() override;
  void start() override;
  void stop() override;
  FramePtr get_frame() override;
  json get_telemetry() override;   // type, path, running, total_frames, current_frame, connected
  int total_frames() const { return total_frames_; }

 private:
  void loop();
  std::string path_;
  double fps_;
  bool loop_;
  std::vector<std::uint8_t> frames_;
  int total_frames_ = 0;
  std::atomic<int> current_index_{0};
  std::mutex mutex_;
  FramePtr frame_;
  std::atomic<bool> running_{false};
  std::mutex lifecycle_;
  std::thread thread_;
};

}  // namespace mtv
