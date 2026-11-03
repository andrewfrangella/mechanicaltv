#include "mtv/engine/studio.h"

#include <filesystem>
#include <stdexcept>

#include "mtv/common/clock.h"
#include "mtv/common/constants.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/subprocess.h"
#include "mtv/common/sysinfo.h"

namespace mtv {

namespace fs = std::filesystem;

HDMIOptions hdmi_options_from_env(const std::string& device) {
  HDMIOptions o;
  o.device = device;
  o.bridge = env::get("MTV_HDMI_BRIDGE", "auto");
  o.input_format = env::get("MTV_HDMI_FORMAT", "auto");
  o.edid = env::get("MTV_HDMI_EDID", "auto");
  o.fit = env::get("MTV_HDMI_FIT", "fit");
  o.backend = env::get("MTV_HDMI_BACKEND", "auto");
  return o;
}

Studio::Studio(const StudioOptions& options) : root_(options.root) {
  fs::create_directories(root_);
  library_ = std::make_unique<Library>((fs::path(root_) / "media").string());
  pipeline_ = std::make_unique<FramePipeline>(options.hdmi);
  try {
    realtime_ = std::make_unique<RealtimeController>(*pipeline_, options.hardware);
  } catch (...) {
    pipeline_->close();
    throw;
  }
  // Start capture (and the EDID load an HDMI source depends on) at launch instead of waiting for an operator.
  if (options.default_source == "hdmi") realtime_->set_source_mode("hdmi");
  started_ = now_s();
  worker_alive_ = true;
  worker_ = std::thread([this] {
    log::set_thread_name("conversion-worker");
    work();
    worker_alive_ = false;
  });
}

Studio::~Studio() { close(); }

void Studio::work() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(stop_mutex_);
      if (stop_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_; })) return;
    }
    try {
      // No new conversions while the disk is spinning: FFmpeg would compete with capture for CPU.
      std::string st = realtime_->state();
      if (st == "running" || st == "locked" || st == "ramping" || st == "testing") continue;
      for (const auto& item : library_->items()) {
        if (item.status == "queued") {
          library_->prepare(item.id);
          break;
        }
      }
    } catch (const std::exception& exc) {
      log::error(std::string("Worker error: ") + exc.what());
    }
  }
}

void Studio::close() {
  {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  stop_cv_.notify_all();
  if (library_) library_->cancel = true;   // kill a running conversion instead of waiting up to 15 min
  if (realtime_) realtime_->close();
  if (pipeline_) pipeline_->close();
  if (worker_.joinable()) worker_.join();
}

int Studio::pending_jobs() {
  int n = 0;
  for (const auto& i : library_->items())
    if (i.status == "uploading" || i.status == "queued" || i.status == "preparing") ++n;
  return n;
}

std::string Studio::import_file(const std::string& source, const std::string& fit, const std::string& name) {
  fs::path src(source);
  std::error_code ec;
  if (!fs::is_regular_file(src, ec)) throw std::invalid_argument(src.filename().string() + ": not a readable file");
  auto size = fs::file_size(src, ec);
  if (ec || size == 0 || size > MAX_UPLOAD) throw std::invalid_argument("Video must be between 1 byte and 256 MiB");
  if (sysinfo::disk_usage(root_).free < size + RESERVE_BYTES + 64ull * 1024 * 1024) {
    throw std::invalid_argument("Not enough free space; keep at least 512 MiB available");
  }
  if (pending_jobs() >= 3) throw std::invalid_argument("Preparation queue is full. Wait for a job to finish.");
  std::unique_lock<std::mutex> lock(import_lock_, std::try_to_lock);
  if (!lock.owns_lock()) throw std::invalid_argument("Another import is in progress. Try again shortly.");
  std::string ident;
  try {
    ident = library_->create(name.empty() ? src.filename().string() : name, fit);
    fs::path dir = library_->folder(ident);
    fs::copy_file(src, dir / "upload.part", fs::copy_options::overwrite_existing);
    fs::rename(dir / "upload.part", dir / "original");
    library_->update(ident, {{"status", "queued"}});
    return ident;
  } catch (...) {
    if (!ident.empty()) {
      fs::remove(fs::path(library_->folder(ident)) / "upload.part", ec);
      library_->update(ident, {{"status", "failed"}, {"error", "Import interrupted. Delete and import again."}});
    }
    throw;
  }
}

std::string Studio::clip_frames_path(const std::string& ident) {
  MediaItem item = library_->get(ident);
  if (item.status != "ready") throw std::invalid_argument("Selected video is not ready");
  return library_->frames_path(ident);
}

bool Studio::hardware_enabled() { return !realtime_->hardware_mock(); }

json Studio::health() {
  auto disk = sysinfo::disk_usage(root_);
  json rt = realtime_->get_status();
  bool hw = hardware_enabled();
  std::string motor = rt["motor"]["driver"].value("driver", "");
  std::string motor_desc = motor.find("HAT") != std::string::npos ? "Adafruit DC & Stepper Motor HAT (PCA9685 @ 0x60)"
                           : motor.find("A4988") != std::string::npos ? "A4988 STEP/DIR on hardware PWM (GPIO 18/23/24)"
                                                                      : motor;
  auto temp = sysinfo::temperature_c();
  auto load = sysinfo::load_average();
  auto& hdmi = pipeline_->hdmi();
  return {{"version", VERSION},
          {"mode", hw ? "realtime" : "realtime-simulation"},
          {"hardware_enabled", hw},
          {"uptime_seconds", static_cast<long>(now_s() - started_)},
          {"worker_alive", worker_alive_.load()},
          {"disk_free_bytes", disk.free},
          {"disk_total_bytes", disk.total},
          {"temperature_c", temp ? json(*temp) : json(nullptr)},
          {"load_average", load ? json(*load) : json(nullptr)},
          {"ffmpeg", !proc::which("ffmpeg").empty()},
          {"ffprobe", !proc::which("ffprobe").empty()},
          {"v4l2_ctl", !proc::which("v4l2-ctl").empty()},
          {"profile", {{"width", WIDTH}, {"height", HEIGHT}, {"fps", realtime_->target_fps()}}},
          {"realtime", rt},
          {"hardware",
           {{"computer", "Raspberry Pi 4 Model B"},
            {"motor_driver", motor.empty() ? "unknown" : motor},
            {"motor_hat", motor_desc},
            {"motor", "NEMA 17 Stepper (200 steps/rev)"},
            {"opto_sensor", "Photoelectric index on GPIO " + std::to_string(realtime_->sensor().pin)},
            {"led_modulator", "High-power LED on GPIO " + std::to_string(realtime_->led().pin) + " (" + realtime_->led().backend() + ")"},
            {"hdmi_input", hdmi.device() + " (" + hdmi.bridge() + " adapter)"}}}};
}

}  // namespace mtv
