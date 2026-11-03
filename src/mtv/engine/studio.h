// The engine (port of studio.Studio): media library, frame pipeline, realtime controller, the
// background conversion worker, file import and health reporting. The desktop console drives it
// in-process; nothing passes through a socket or a browser.
#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mtv/capture/pipeline.h"
#include "mtv/common/json.h"
#include "mtv/engine/library.h"
#include "mtv/hardware/controller.h"

namespace mtv {

struct StudioOptions {
  std::string root;
  HDMIOptions hdmi;
  ControllerOptions hardware;
  std::string default_source = "pattern";   // pattern | hdmi
};

// HDMI settings from the environment (install.sh writes /etc/mechanical-tv/environment).
HDMIOptions hdmi_options_from_env(const std::string& device = "/dev/video0");

class Studio {
 public:
  explicit Studio(const StudioOptions& options);   // throws when real hardware fails to initialise
  ~Studio();
  void close();

  Library& library() { return *library_; }
  FramePipeline& pipeline() { return *pipeline_; }
  RealtimeController& realtime() { return *realtime_; }
  const std::string& root() const { return root_; }

  int pending_jobs();
  // Copy a video file into the library and queue its conversion. Throws std::invalid_argument.
  std::string import_file(const std::string& source, const std::string& fit = "fit", const std::string& name = "");
  std::string clip_frames_path(const std::string& ident);   // throws std::invalid_argument("Selected video is not ready")
  bool hardware_enabled();
  json health();
  bool worker_alive() const { return worker_alive_; }

 private:
  void work();
  std::string root_;
  std::unique_ptr<Library> library_;
  std::unique_ptr<FramePipeline> pipeline_;
  std::unique_ptr<RealtimeController> realtime_;
  std::mutex import_lock_;
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  bool stopping_ = false;
  std::atomic<bool> worker_alive_{false};
  double started_ = 0.0;
  std::thread worker_;
};

}  // namespace mtv
