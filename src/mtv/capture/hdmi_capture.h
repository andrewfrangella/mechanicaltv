// Live HDMI ingest from a V4L2 device (port of capture.HDMICapture).
//
// Two adapter families: HDMI-to-CSI-2 bridges (Toshiba TC358743 behind the Pi's CSI receiver: EDID
// loaded once per run, DV timings locked before every open, UYVY read in-process from the driver's
// mmap buffers) and USB UVC dongles (MJPEG through FFmpeg). There is no video decoding on the CSI
// path: the bridge delivers raw pixels. Failures are never silent: a dead FFmpeg, a stalled stream
// or a missing signal clears the frame (black on the disk), is reported in telemetry and the log, and
// triggers a reopen every second. Each start() is a new worker generation; a stale worker can
// neither register a device nor release its successor's.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mtv/capture/csi_control.h"
#include "mtv/capture/video_source.h"
#include "mtv/common/subprocess.h"

namespace mtv {

class UYVYSampler;

// True when the V4L2 node is the Pi's CSI receiver (unicam, rp1-cfe) or the bridge rather than a USB dongle.
bool detect_csi_bridge(const std::string& device, const std::string& sysfs = "/sys/class/video4linux");
// Directory holding edid/: MTV_SHARE_DIR env, the compiled-in install dir, or the source tree.
std::string share_dir();
std::string packaged_edid_path(const std::string& name = "1080p30.txt");

struct HDMIOptions {
  std::string device = "/dev/video0";
  std::string fit = "fit";              // fit | crop
  std::string input_format = "auto";    // auto | uyvy422 | mjpeg | none | <ffmpeg name>
  std::string edid = "auto";            // auto | none | off | <path>
  std::string backend = "auto";         // auto | v4l2 | ffmpeg
  std::string bridge = "auto";          // auto | csi | usb
};

// The native reader seen through an interface so tests can feed synthetic frames.
class FrameReader {
 public:
  virtual ~FrameReader() = default;
  struct Got { int index; const std::uint8_t* data; std::size_t bytes; };
  // A frame, or index -1 on timeout; throws std::runtime_error on read errors / source change.
  virtual Got dequeue(double timeout_s) = 0;
  virtual void requeue(int index) = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual int stride() const = 0;
  virtual bool bt709() const = 0;
  virtual int buffer_count() const = 0;
  virtual long error_frames() const = 0;
  virtual long dropped_frames() const = 0;
};

class HDMICapture : public VideoSource {
 public:
  static constexpr double STALL_TIMEOUT = 5.0;      // seconds without a complete frame before the stream is reopened
  static constexpr double RETRY_DELAY = 1.0;        // seconds between open attempts
  static constexpr double STOP_JOIN_TIMEOUT = 3.0;
  static constexpr int BLACK_LEVEL = 20;            // 8-bit level at or below which a frame counts as black (video black is 16)
  static constexpr double FRESH_S = 1.0;            // a frame older than this is not shown (frozen source -> black)

  explicit HDMICapture(const HDMIOptions& options = {});
  ~HDMICapture() override;

  void start() override;
  void stop() override;
  FramePtr get_frame() override;
  PreviewPtr get_preview();   // 320x250 RGB of the whole input, or nullptr without a live frame
  json get_telemetry() override;

  const std::string& device() const { return opts_.device; }
  const std::string& fit() const { return opts_.fit; }
  std::string bridge();          // "csi" or "usb"
  std::string input_format();    // resolved: uyvy422 for csi, mjpeg for usb
  std::string edid_path();       // "" when none
  const std::string& backend_setting() const { return opts_.backend; }
  double convert_fps() const { return convert_fps_; }

  // Test hooks
  void set_csi_control(std::unique_ptr<CsiControl> c) { csi_ = std::move(c); }
  void set_reader_factory(std::function<std::unique_ptr<FrameReader>(const std::string&)> f) { reader_factory_ = std::move(f); }
  void set_device_exists(std::function<bool(const std::string&)> f) { device_exists_ = std::move(f); }
  void set_ffmpeg_argv(std::function<std::vector<std::string>()> f) { ffmpeg_argv_ = std::move(f); }
  void set_retry_delay(double s) { retry_delay_ = s; }
  void set_stall_timeout(double s) { stall_timeout_ = s; }
  std::string error();

 private:
  void capture_loop(int generation);
  bool current(int generation) const;
  void log(const std::string& message);
  void sleep(double seconds);
  void cleanup(int generation = -1);
  bool register_native(int generation, std::unique_ptr<FrameReader> reader, std::unique_ptr<UYVYSampler> sampler);
  bool register_ffmpeg(int generation, std::unique_ptr<proc::Child> child);
  void fail(int generation, const std::string& message);
  void set_error(const std::string& e);
  bool prepare_csi();
  bool open_device(int generation);
  bool open_native(int generation);
  bool open_ffmpeg(int generation);
  void read_native(int generation);
  void read_ffmpeg(int generation, std::vector<std::uint8_t>& buf);
  std::string last_stderr();
  void store(const Frame& frame, const Preview* preview);

  HDMIOptions opts_;
  std::mutex bridge_lock_;
  std::string bridge_;
  std::atomic<bool> running_{false};
  std::atomic<int> generation_{0};
  std::mutex lifecycle_;   // start()/stop() from different threads
  std::thread thread_;
  std::mutex lock_;         // guards frame_/preview_
  std::mutex state_lock_;   // guards reader_/sampler_/ffmpeg_ and the connected flag handoff
  FramePtr frame_;
  PreviewPtr preview_;
  std::unique_ptr<FrameReader> reader_;
  std::unique_ptr<UYVYSampler> sampler_;
  std::unique_ptr<proc::Child> ffmpeg_;
  std::atomic<bool> connected_{false};
  std::atomic<double> opened_at_{0.0};
  double next_convert_at_ = 0.0;
  std::atomic<long> short_frames_{0};
  std::atomic<long> reader_errors_{0};
  std::atomic<long> reader_dropped_{0};
  std::mutex stderr_lock_;
  std::deque<std::string> stderr_tail_;
  std::string stderr_partial_;
  std::atomic<int> input_width_{0}, input_height_{0};
  std::atomic<double> input_fps_{0.0};
  std::atomic<long> frame_count_{0};
  std::atomic<int> frame_peak_{0};
  std::atomic<double> frame_mean_{0.0};
  std::atomic<double> last_frame_at_{0.0};
  std::atomic<double> convert_ms_{0.0};
  std::mutex error_lock_;
  std::string error_;
  std::atomic<bool> edid_loaded_{false};
  std::mutex log_lock_;
  std::string last_log_;
  std::atomic<int> backend_kind_{0};   // 0 none, 1 v4l2, 2 ffmpeg
  std::string ffmpeg_path_;
  double convert_fps_ = 30.0;
  double retry_delay_ = RETRY_DELAY;
  double stall_timeout_ = STALL_TIMEOUT;
  std::unique_ptr<CsiControl> csi_;
  std::function<std::unique_ptr<FrameReader>(const std::string&)> reader_factory_;
  std::function<bool(const std::string&)> device_exists_;
  std::function<std::vector<std::string>()> ffmpeg_argv_;
};

}  // namespace mtv
