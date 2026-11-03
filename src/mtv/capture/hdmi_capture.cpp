#include "mtv/capture/hdmi_capture.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "mtv/capture/ffmpeg_source.h"
#include "mtv/capture/uyvy_sampler.h"
#include "mtv/common/clock.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/sysinfo.h"
#ifdef MTV_LINUX
#include "mtv/capture/v4l2_reader.h"
#endif

namespace mtv {

namespace fs = std::filesystem;

bool detect_csi_bridge(const std::string& device, const std::string& sysfs) {
  std::error_code ec;
  fs::path real = fs::weakly_canonical(device, ec);
  std::string node = (ec ? fs::path(device) : real).filename().string();
  std::ifstream in(fs::path(sysfs) / node / "name");
  if (!in) return false;
  std::string name;
  std::getline(in, name);
  name = env::lower(env::trim(name));
  for (const char* tag : {"unicam", "rp1-cfe", "tc358743"}) {
    if (name.find(tag) != std::string::npos) return true;
  }
  return false;
}

std::string share_dir() {
  std::string env_dir = env::get("MTV_SHARE_DIR");
  if (!env_dir.empty() && sysinfo::is_dir(env_dir + "/edid")) return env_dir;
#ifdef MTV_SHARE_DIR
  if (sysinfo::is_dir(std::string(MTV_SHARE_DIR) + "/edid")) return MTV_SHARE_DIR;
#endif
  // Next to the executable: <prefix>/bin/../share/mechanical-tv
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    fs::path candidate = exe.parent_path().parent_path() / "share" / "mechanical-tv";
    if (sysinfo::is_dir((candidate / "edid").string())) return candidate.string();
  }
#ifdef MTV_SOURCE_SHARE_DIR
  return MTV_SOURCE_SHARE_DIR;
#else
  return ".";
#endif
}

std::string packaged_edid_path(const std::string& name) { return share_dir() + "/edid/" + name; }

namespace {

#ifdef MTV_LINUX
class V4L2FrameReader : public FrameReader {
 public:
  explicit V4L2FrameReader(const std::string& device) : r_(device) {}
  Got dequeue(double timeout_s) override {
    auto b = r_.dequeue(timeout_s);
    if (!b) return {-1, nullptr, 0};
    return {b->index, b->data, b->bytes};
  }
  void requeue(int index) override { r_.requeue(index); }
  int width() const override { return r_.width(); }
  int height() const override { return r_.height(); }
  int stride() const override { return r_.stride(); }
  bool bt709() const override { return r_.bt709(); }
  int buffer_count() const override { return r_.buffer_count(); }
  long error_frames() const override { return r_.error_frames(); }
  long dropped_frames() const override { return r_.dropped_frames(); }

 private:
  V4L2Reader r_;
};
#endif

}  // namespace

HDMICapture::HDMICapture(const HDMIOptions& options) : opts_(options) {
  if (opts_.fit != "fit" && opts_.fit != "crop") opts_.fit = "fit";
  opts_.input_format = env::lower(env::trim(opts_.input_format));
  if (opts_.input_format.empty()) opts_.input_format = "auto";
  opts_.edid = env::trim(opts_.edid);
  if (opts_.edid.empty()) opts_.edid = "auto";
  opts_.backend = env::lower(env::trim(opts_.backend));
  if (opts_.backend.empty()) opts_.backend = "auto";
  if (opts_.backend == "opencv") {
    log("the OpenCV backend is not part of the C++ build; using auto");
    opts_.backend = "auto";
  } else if (opts_.backend != "auto" && opts_.backend != "v4l2" && opts_.backend != "ffmpeg") {
    log("unknown backend '" + opts_.backend + "' (want auto, v4l2 or ffmpeg); using auto");
    opts_.backend = "auto";
  }
  opts_.bridge = env::lower(env::trim(opts_.bridge));
  if (opts_.bridge.empty()) opts_.bridge = "auto";
  if (opts_.bridge == "csi" || opts_.bridge == "usb") bridge_ = opts_.bridge;
  // Frames converted per second on the native path. Converting costs ~2 ms on a Pi 4, so by default
  // every frame is converted and the disk always gets the newest picture at its index edge.
  convert_fps_ = std::clamp(env::get_double("MTV_HDMI_CONVERT_FPS", 30.0), 1.0, 120.0);
  ffmpeg_path_ = proc::which("ffmpeg");
  if (ffmpeg_path_.empty()) ffmpeg_path_ = "ffmpeg";
  csi_ = make_csi_control();
  device_exists_ = [](const std::string& d) { return sysinfo::path_exists(d); };
#ifdef MTV_LINUX
  reader_factory_ = [](const std::string& d) -> std::unique_ptr<FrameReader> { return std::make_unique<V4L2FrameReader>(d); };
#endif
}

HDMICapture::~HDMICapture() { stop(); }

std::string HDMICapture::bridge() {
  std::lock_guard<std::mutex> lock(bridge_lock_);
  if (bridge_.empty()) bridge_ = detect_csi_bridge(opts_.device) ? "csi" : "usb";
  return bridge_;
}

std::string HDMICapture::input_format() {
  if (opts_.input_format != "auto") return opts_.input_format;
  return bridge() == "csi" ? "uyvy422" : "mjpeg";
}

std::string HDMICapture::edid_path() {
  std::string s = env::lower(opts_.edid);
  if (s == "none" || s == "off") return "";
  if (s == "auto") return bridge() == "csi" ? packaged_edid_path() : "";
  return opts_.edid;
}

std::string HDMICapture::error() {
  std::lock_guard<std::mutex> lock(error_lock_);
  return error_;
}

void HDMICapture::set_error(const std::string& e) {
  std::lock_guard<std::mutex> lock(error_lock_);
  error_ = e;
}

void HDMICapture::start() {
  std::lock_guard<std::mutex> life(lifecycle_);
  if (running_.exchange(true)) return;
  int gen = ++generation_;
  if (thread_.joinable()) thread_.join();   // a previous worker that is already exiting
  thread_ = std::thread([this, gen] {
    log::set_thread_name("hdmi-capture-worker");
    capture_loop(gen);
  });
}

void HDMICapture::stop() {
  std::lock_guard<std::mutex> life(lifecycle_);
  running_ = false;
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  cleanup();
}

bool HDMICapture::current(int generation) const { return running_ && generation_ == generation; }

void HDMICapture::log(const std::string& message) {
  if (message.empty()) return;
  {
    std::lock_guard<std::mutex> lock(log_lock_);
    if (message == last_log_) return;
    last_log_ = message;
  }
  log::info("HDMI capture: " + message);
}

void HDMICapture::sleep(double seconds) {
  double deadline = now_s() + seconds;
  while (running_ && now_s() < deadline) sleep_s(std::min(0.05, deadline - now_s()));
}

void HDMICapture::cleanup(int generation) {
  std::unique_ptr<FrameReader> reader;
  std::unique_ptr<proc::Child> child;
  {
    std::lock_guard<std::mutex> lock(state_lock_);
    if (generation >= 0 && generation != generation_) return;
    reader = std::move(reader_);
    sampler_.reset();
    child = std::move(ffmpeg_);
    connected_ = false;
    backend_kind_ = 0;
    std::lock_guard<std::mutex> l2(lock_);
    frame_.reset();
    preview_.reset();
  }
  reader.reset();   // closes the device (STREAMOFF, munmap, close)
  if (child) {
    child->terminate(1.0);
    child->close_pipes();
  }
}

bool HDMICapture::register_native(int generation, std::unique_ptr<FrameReader> reader, std::unique_ptr<UYVYSampler> sampler) {
  std::lock_guard<std::mutex> lock(state_lock_);
  if (!current(generation)) return false;
  reader_ = std::move(reader);
  sampler_ = std::move(sampler);
  connected_ = true;
  backend_kind_ = 1;
  opened_at_ = now_s();
  set_error("");
  return true;
}

bool HDMICapture::register_ffmpeg(int generation, std::unique_ptr<proc::Child> child) {
  std::lock_guard<std::mutex> lock(state_lock_);
  if (!current(generation)) return false;
  ffmpeg_ = std::move(child);
  connected_ = true;
  backend_kind_ = 2;
  opened_at_ = now_s();
  set_error("");
  return true;
}

void HDMICapture::fail(int generation, const std::string& message) {
  if (generation != generation_) return;
  cleanup(generation);   // clear frame/connection first so telemetry never pairs a new error with an old frame
  set_error(message);
  log(message);
  sleep(retry_delay_);
}

bool HDMICapture::prepare_csi() {
  std::string edid = edid_path();
  if (!edid.empty() && !edid_loaded_) {
    std::vector<std::uint8_t> bytes;
    try {
      bytes = read_edid_file(edid);
    } catch (const std::exception& exc) {
      set_error(exc.what());
      return false;
    }
    std::string why = csi_->load_edid(opts_.device, bytes);
    if (!why.empty()) {
      set_error("EDID load failed: " + why + " (is " + opts_.device + " the HDMI bridge and not a Pi camera?)");
      return false;
    }
    edid_loaded_ = true;
    log("EDID " + edid + " loaded; the HDMI source should now detect a display");
  }
  DvTimings t;
  std::string why = csi_->query_and_lock(opts_.device, &t);
  if (!why.empty()) {
    std::string state = edid_loaded_ ? "loaded" : "not loaded";
    set_error("No HDMI signal on the CSI bridge: connect the source and check that it shows a second display (EDID " +
              state + "; " + why + ")");
    return false;
  }
  input_width_ = t.width;
  input_height_ = t.height;
  input_fps_ = t.fps;
  if (t.height >= 1080 && t.fps > 50) {
    log(std::to_string(t.width) + "x" + std::to_string(t.height) + "@" + log::format("%.0f", t.fps) +
        " exceeds the Pi 4 2-lane CSI limit; expect errors. Use the shipped EDID.");
  }
  return true;
}

bool HDMICapture::open_device(int generation) {
  cleanup(generation);
  if (opts_.bridge == "auto") {
    std::lock_guard<std::mutex> lock(bridge_lock_);
    bridge_ = detect_csi_bridge(opts_.device) ? "csi" : "usb";
  }
  if (!device_exists_(opts_.device)) {
    set_error("Capture device " + opts_.device + " not found: USB dongle unplugged, or for an HDMI-to-CSI adapter "
              "dtoverlay=tc358743 is missing from config.txt");
    return false;
  }
  if (bridge() == "csi" && !prepare_csi()) return false;
  if (!current(generation)) return false;   // stop() ran while the bridge was being prepared
  const std::string fmt = input_format();
  if (opts_.backend == "auto" || opts_.backend == "v4l2") {
    if (fmt != "uyvy422") {
      if (opts_.backend == "v4l2") log("native V4L2 backend needs UYVY input, not " + fmt + "; using ffmpeg");
    } else if (!reader_factory_) {
      if (opts_.backend == "v4l2") {
        set_error("native V4L2 backend unavailable (needs Linux)");
        return false;
      }
      log("native V4L2 backend unavailable (needs Linux); using ffmpeg");
    } else {
      if (open_native(generation)) return true;
      if (opts_.backend == "v4l2") return false;   // asked for explicitly: report instead of hiding behind ffmpeg
      log(error() + "; using ffmpeg");
    }
  }
  return open_ffmpeg(generation);
}

bool HDMICapture::open_native(int generation) {
  std::unique_ptr<FrameReader> reader;
  try {
    reader = reader_factory_(opts_.device);
  } catch (const std::exception& exc) {
    set_error(std::string("native V4L2 open failed: ") + exc.what());
    return false;
  }
  std::unique_ptr<UYVYSampler> sampler;
  try {
    sampler = std::make_unique<UYVYSampler>(reader->width(), reader->height(), reader->stride(), opts_.fit, reader->bt709());
  } catch (const std::exception& exc) {
    set_error(std::string("native V4L2 sampler failed: ") + exc.what());
    return false;
  }
  int w = reader->width(), h = reader->height(), n = reader->buffer_count();
  if (!register_native(generation, std::move(reader), std::move(sampler))) return false;
  input_width_ = w;
  input_height_ = h;
  next_convert_at_ = now_s();
  short_frames_ = 0;
  log("opened " + opts_.device + " natively (V4L2 mmap, " + std::to_string(w) + "x" + std::to_string(h) + " UYVY, " +
      std::to_string(n) + " buffers, up to " + log::format("%g", convert_fps_) + " fps converted)");
  return true;
}

void HDMICapture::read_native(int generation) {
  FrameReader* reader;
  UYVYSampler* sampler;
  {
    std::lock_guard<std::mutex> lock(state_lock_);
    reader = reader_.get();
    sampler = sampler_.get();
  }
  if (!reader || !sampler) return;
  FrameReader::Got got{};
  try {
    got = reader->dequeue(0.5);
  } catch (const std::exception& exc) {
    fail(generation, std::string("V4L2 read error: ") + exc.what() + " (signal lost or mode changed); reopening");
    return;
  }
  reader_errors_ = reader->error_frames();
  reader_dropped_ = reader->dropped_frames();
  if (got.index < 0) {
    if (now_s() - std::max(last_frame_at_.load(), opened_at_.load()) > stall_timeout_) {
      fail(generation, "no frames for " + log::format("%g", stall_timeout_) + "s (source stopped or changed mode); reopening");
    }
    return;
  }
  double now = now_s();
  if (now >= next_convert_at_) {
    double interval = 1.0 / convert_fps_;
    next_convert_at_ = std::max(next_convert_at_ + interval, now - interval);
    Frame frame{};
    Preview preview;
    try {
      double t0 = now_s();
      sampler->convert(got.data, got.bytes, preview, frame);
      convert_ms_ = (now_s() - t0) * 1000.0;
      store(frame, &preview);
    } catch (const std::invalid_argument& exc) {
      // One bad frame is not worth a reopen; if they never stop, the stall timer reopens.
      long n = ++short_frames_;
      if (n == 1 || n == 10 || n == 100 || n % 1000 == 0) {
        log(std::string("skipped a short V4L2 frame (") + exc.what() + "); " + std::to_string(n) + " so far");
      }
    }
  }
  try {
    reader->requeue(got.index);
  } catch (const std::exception& exc) {
    fail(generation, std::string("V4L2 requeue error: ") + exc.what() + "; reopening");
  }
}

bool HDMICapture::open_ffmpeg(int generation) {
  auto child = std::make_unique<proc::Child>();
  std::vector<std::string> argv = ffmpeg_argv_ ? ffmpeg_argv_() : ffmpeg::build_command(opts_.device, opts_.fit, input_format(), ffmpeg_path_);
  std::string why;
  if (!child->start(argv, &why)) {
    set_error("Cannot start ffmpeg: " + why);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(stderr_lock_);
    stderr_tail_.clear();
    stderr_partial_.clear();
  }
  if (!register_ffmpeg(generation, std::move(child))) return false;
  log("opened " + opts_.device + " via ffmpeg (" + bridge() + " adapter, " + input_format() + ")");
  return true;
}

std::string HDMICapture::last_stderr() {
  std::lock_guard<std::mutex> lock(stderr_lock_);
  if (!stderr_partial_.empty()) return env::trim(stderr_partial_);
  return stderr_tail_.empty() ? "no error output" : stderr_tail_.back();
}

void HDMICapture::read_ffmpeg(int generation, std::vector<std::uint8_t>& buf) {
  proc::Child* child;
  {
    std::lock_guard<std::mutex> lock(state_lock_);
    child = ffmpeg_.get();
  }
  if (!child) return;
  pollfd fds[2] = {{child->stdout_fd(), POLLIN, 0}, {child->stderr_fd(), POLLIN, 0}};
  int rc = ::poll(fds, 2, 500);
  if (rc < 0 && errno != EINTR) {
    fail(generation, std::string("ffmpeg pipe error: ") + std::strerror(errno));
    return;
  }
  bool eof = false;
  if (rc > 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
    char tmp[4096];
    ssize_t n = ::read(fds[1].fd, tmp, sizeof tmp);
    if (n > 0) {
      std::lock_guard<std::mutex> lock(stderr_lock_);
      stderr_partial_.append(tmp, static_cast<std::size_t>(n));
      std::size_t nl;
      while ((nl = stderr_partial_.find('\n')) != std::string::npos) {
        std::string line = env::trim(stderr_partial_.substr(0, nl));
        stderr_partial_.erase(0, nl + 1);
        if (!line.empty()) {
          stderr_tail_.push_back(line);
          if (stderr_tail_.size() > 12) stderr_tail_.pop_front();
        }
      }
    }
  }
  if (rc > 0 && (fds[0].revents & (POLLIN | POLLHUP))) {
    std::size_t want = ffmpeg::FRAME_BYTES_OUT - buf.size();
    std::vector<std::uint8_t> chunk(want);
    ssize_t n = ::read(fds[0].fd, chunk.data(), want);
    if (n > 0) {
      buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
      if (buf.size() == ffmpeg::FRAME_BYTES_OUT) {
        Preview preview;
        Frame frame{};
        ffmpeg::split_frame(buf.data(), buf.size(), preview, frame);
        store(frame, &preview);
        buf.clear();
      }
      return;
    }
    if (n == 0) eof = true;
    else if (errno != EAGAIN && errno != EINTR) {
      fail(generation, std::string("ffmpeg pipe error: ") + std::strerror(errno));
      return;
    }
  }
  if (eof || child->poll().has_value()) {
    auto status = child->poll();
    if (!status) {
      for (int i = 0; i < 200 && !status; ++i) {   // up to 2 s, like proc.wait(timeout=2.0)
        sleep_s(0.01);
        status = child->poll();
      }
    }
    std::string rc_text = status ? std::to_string(*status) : "hung";
    // collect the final stderr line (non-blocking: another process may still hold the pipe open)
    ::fcntl(child->stderr_fd(), F_SETFL, ::fcntl(child->stderr_fd(), F_GETFL) | O_NONBLOCK);
    char tmp[4096];
    ssize_t n;
    while ((n = ::read(child->stderr_fd(), tmp, sizeof tmp)) > 0) {
      std::lock_guard<std::mutex> lock(stderr_lock_);
      stderr_partial_.append(tmp, static_cast<std::size_t>(n));
    }
    {
      std::lock_guard<std::mutex> lock(stderr_lock_);
      std::size_t nl;
      while ((nl = stderr_partial_.find('\n')) != std::string::npos) {
        std::string line = env::trim(stderr_partial_.substr(0, nl));
        stderr_partial_.erase(0, nl + 1);
        if (!line.empty()) stderr_tail_.push_back(line);
      }
    }
    fail(generation, "ffmpeg exited (" + rc_text + "): " + last_stderr());
    return;
  }
  if (now_s() - std::max(last_frame_at_.load(), opened_at_.load()) > stall_timeout_) {
    fail(generation, "no frames for " + log::format("%g", stall_timeout_) + "s (source stopped or changed mode); reopening");
  }
}

void HDMICapture::capture_loop(int generation) {
  std::vector<std::uint8_t> buf;
  buf.reserve(ffmpeg::FRAME_BYTES_OUT);
  while (current(generation)) {
    try {
      if (!connected_) {
        if (!open_device(generation)) {
          log(error());
          sleep(retry_delay_);
          continue;
        }
        buf.clear();
      }
      int kind = backend_kind_;
      if (kind == 1) read_native(generation);
      else if (kind == 2) read_ffmpeg(generation, buf);
      else connected_ = false;
    } catch (const std::exception& exc) {
      // A dead worker would disable HDMI for the rest of the run; recover instead.
      fail(generation, std::string("capture worker error: ") + exc.what());
    }
  }
  if (generation_ == generation) cleanup(generation);
}

void HDMICapture::store(const Frame& frame, const Preview* preview) {
  int peak = 0;
  unsigned long sum = 0;
  for (auto v : frame) {
    peak = std::max(peak, static_cast<int>(v));
    sum += v;
  }
  auto f = std::make_shared<const Frame>(frame);
  PreviewPtr p = preview ? std::make_shared<const Preview>(*preview) : nullptr;
  {
    std::lock_guard<std::mutex> lock(lock_);
    frame_ = std::move(f);
    preview_ = std::move(p);
  }
  ++frame_count_;
  frame_peak_ = peak;
  frame_mean_ = static_cast<double>(sum) / FRAME_PIXELS;
  last_frame_at_ = now_s();
}

FramePtr HDMICapture::get_frame() {
  if (now_s() - last_frame_at_ > FRESH_S) return nullptr;   // the source froze: black, not a stale picture
  std::lock_guard<std::mutex> lock(lock_);
  return frame_;
}

PreviewPtr HDMICapture::get_preview() {
  if (now_s() - last_frame_at_ > FRESH_S) return nullptr;
  std::lock_guard<std::mutex> lock(lock_);
  return preview_;
}

json HDMICapture::get_telemetry() {
  double last = last_frame_at_;
  json age = nullptr;
  bool signal = false;
  if (last > 0) {
    double a = round_to(now_s() - last, 2);
    age = a;
    signal = connected_ && a < stall_timeout_;
  }
  int kind = backend_kind_;
  std::string backend = kind == 1 ? "v4l2" : kind == 2 ? "ffmpeg" : "";
  int w = input_width_, h = input_height_;
  bool has_preview;
  {
    std::lock_guard<std::mutex> lock(lock_);
    has_preview = preview_ != nullptr;
  }
  return {
      {"type", "hdmi"},
      {"device", opts_.device},
      {"bridge", bridge()},
      {"backend", backend},
      {"backend_setting", opts_.backend},
      {"preview", has_preview},
      {"bad_frames", (kind == 1 ? reader_errors_.load() : 0L) + short_frames_.load()},
      {"dropped_frames", kind == 1 ? reader_dropped_.load() : 0L},
      {"convert_ms", round_to(convert_ms_, 2)},
      {"input_format", input_format()},
      {"edid", edid_path()},
      {"edid_loaded", edid_loaded_.load()},
      {"connected", connected_.load()},
      {"signal", signal},
      {"running", running_.load()},
      {"input_resolution", w ? std::to_string(w) + "x" + std::to_string(h) : ""},
      {"input_fps", input_fps_.load()},
      {"frames_captured", frame_count_.load()},
      {"last_frame_age", age},
      {"frame_peak", frame_peak_.load()},
      {"frame_mean", round_to(frame_mean_, 1)},
      {"picture", signal && frame_peak_ > BLACK_LEVEL},
      {"error", error()},
  };
}

}  // namespace mtv
