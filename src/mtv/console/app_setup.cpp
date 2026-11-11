#include "mtv/console/app_setup.h"

#include <sys/resource.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "mtv/capture/hdmi_capture.h"
#include "mtv/common/constants.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/rt.h"
#include "mtv/common/subprocess.h"
#include "mtv/common/sysinfo.h"
#include "mtv/hardware/motor.h"

namespace mtv::console {

namespace fs = std::filesystem;

std::string usage() {
  return R"(Usage: mechanical-tv [options]

Mechanical TV operator console (runs on the Pi's own display).

  --env-file FILE        read MTV_* defaults from a KEY=value file, e.g. /etc/mechanical-tv/environment
  --data DIR             library, settings and logs (default ~/.local/share/mechanical-tv)
  --fullscreen           start full screen (F11 toggles, Esc leaves)
  --check                print the environment report as JSON and exit without a window
  --free-run             start with free-run output enabled (bench test without a disk)
  --debug-window         open the debug log window at start (F12 toggles it)
  --mock-hardware        simulated motor, index sensor and LED
  --hdmi-device DEV      V4L2 capture node (default /dev/video0)
  --hdmi-bridge MODE     auto | csi | usb
  --hdmi-format FMT      auto | uyvy422 | mjpeg | none | <ffmpeg name>
  --hdmi-edid EDID       auto | none | /path/to/edid.txt
  --hdmi-fit MODE        fit | crop
  --hdmi-backend MODE    auto | v4l2 | ffmpeg
  --default-source SRC   auto | pattern | hdmi (auto: hdmi when the capture device exists)
  --motor-port N         Adafruit Motor HAT stepper port (1 or 2)
  --opto-pin N           index sensor GPIO (BCM, default 4)
  --led-pin N            LED GPIO for the bit-banged output (BCM, default 21)
  --spi-led              drive the LED from SPI0 MOSI (GPIO 10) by DMA
  --screenshot FILE      save the window as a PNG after 4 s and quit (documentation, CI)
  --version              print the version
  -h, --help             this text
)";
}

namespace {

const std::vector<std::string> BRIDGES = {"auto", "csi", "usb"};
const std::vector<std::string> FITS = {"fit", "crop"};
const std::vector<std::string> BACKENDS = {"auto", "v4l2", "ffmpeg", "opencv"};
const std::vector<std::string> SOURCES = {"auto", "pattern", "hdmi"};

void choose(const std::string& opt, const std::string& value, const std::vector<std::string>& options) {
  for (const auto& o : options)
    if (o == value) return;
  std::string list;
  for (const auto& o : options) list += (list.empty() ? "" : ", ") + o;
  throw std::invalid_argument("argument " + opt + ": invalid choice: '" + value + "' (choose from " + list + ")");
}

}  // namespace

Args parse_args(const std::vector<std::string>& argv) {
  // Pass 1: --env-file, so its values become the defaults below.
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--env-file" && i + 1 < argv.size()) env::load_env_file(argv[i + 1]);
    else if (argv[i].rfind("--env-file=", 0) == 0) env::load_env_file(argv[i].substr(11));
  }
  Args a;
  a.data = env::get("MTV_DATA", sysinfo::default_data_dir());
  a.fullscreen = env::flag("MTV_FULLSCREEN");
  a.debug_window = env::flag("MTV_DEBUG");
  a.mock_hardware = env::flag("MTV_MOCK_HARDWARE");
  a.hdmi_device = env::get("MTV_HDMI_DEVICE", "/dev/video0");
  a.hdmi_bridge = env::get("MTV_HDMI_BRIDGE", "auto");
  a.hdmi_format = env::get("MTV_HDMI_FORMAT", "auto");
  a.hdmi_edid = env::get("MTV_HDMI_EDID", "auto");
  a.hdmi_fit = env::get("MTV_HDMI_FIT", "fit");
  a.hdmi_backend = env::get("MTV_HDMI_BACKEND", "auto");
  a.default_source = env::get("MTV_DEFAULT_SOURCE", "auto");
  a.motor_port = env::get_int("MTV_MOTOR_PORT", 1);
  a.opto_pin = env::get_int("MTV_OPTO_PIN", 4);
  a.led_pin = env::get_int("MTV_LED_PIN", 21);
  a.spi_led = env::flag("MTV_SPI_LED");

  for (std::size_t i = 0; i < argv.size(); ++i) {
    std::string opt = argv[i], value;
    bool inline_value = false;
    if (auto eq = opt.find('='); opt.rfind("--", 0) == 0 && eq != std::string::npos) {
      value = opt.substr(eq + 1);
      opt = opt.substr(0, eq);
      inline_value = true;
    }
    auto need = [&]() -> std::string {
      if (inline_value) return value;
      if (i + 1 >= argv.size()) throw std::invalid_argument("argument " + opt + ": expected one argument");
      return argv[++i];
    };
    auto need_int = [&]() -> int {
      std::string v = need();
      try {
        std::size_t used = 0;
        int n = std::stoi(v, &used);
        if (used != v.size()) throw std::invalid_argument("");
        return n;
      } catch (...) {
        throw std::invalid_argument("argument " + opt + ": invalid int value: '" + v + "'");
      }
    };
    if (opt == "--env-file") a.env_file = need();
    else if (opt == "--data") a.data = need();
    else if (opt == "--fullscreen") a.fullscreen = true;
    else if (opt == "--check") a.check = true;
    else if (opt == "--free-run") a.free_run = true;
    else if (opt == "--debug-window") a.debug_window = true;
    else if (opt == "--mock-hardware") a.mock_hardware = true;
    else if (opt == "--hdmi-device") a.hdmi_device = need();
    else if (opt == "--hdmi-bridge") a.hdmi_bridge = need();
    else if (opt == "--hdmi-format") a.hdmi_format = need();
    else if (opt == "--hdmi-edid") a.hdmi_edid = need();
    else if (opt == "--hdmi-fit") a.hdmi_fit = need();
    else if (opt == "--hdmi-backend") a.hdmi_backend = need();
    else if (opt == "--default-source") a.default_source = need();
    else if (opt == "--motor-port") a.motor_port = need_int();
    else if (opt == "--opto-pin") a.opto_pin = need_int();
    else if (opt == "--led-pin") a.led_pin = need_int();
    else if (opt == "--spi-led") a.spi_led = true;
    else if (opt == "--screenshot") a.screenshot = need();
    else if (opt == "-h" || opt == "--help") a.help = true;
    else if (opt == "--version") a.version = true;
    else throw std::invalid_argument("unrecognized argument: " + argv[i]);
  }
  choose("--hdmi-bridge", a.hdmi_bridge, BRIDGES);
  choose("--hdmi-fit", a.hdmi_fit, FITS);
  choose("--hdmi-backend", a.hdmi_backend, BACKENDS);
  choose("--default-source", a.default_source, SOURCES);
  return a;
}

std::string resolve_default_source(const Args& args) {
  if (args.default_source != "auto") return args.default_source;
  // A simulation run must not grab a real capture device another process may own.
  return sysinfo::path_exists(args.hdmi_device) && !args.mock_hardware ? "hdmi" : "pattern";
}

std::string usable_data_dir(const std::string& requested) {
  std::error_code ec;
  fs::create_directories(requested, ec);
  if (!ec) {
    fs::path probe = fs::path(requested) / ".write-test";
    std::ofstream f(probe);
    if (f) {
      f.close();
      fs::remove(probe, ec);
      return requested;
    }
  }
  std::string fallback = sysinfo::default_data_dir();
  fs::create_directories(fallback, ec);   // the caller logs the fallback once the log file is open
  return fallback;
}

json check_report(const Args& args) {
  HDMIOptions o{args.hdmi_device, args.hdmi_fit, args.hdmi_format, args.hdmi_edid, args.hdmi_backend, args.hdmi_bridge};
  HDMICapture probe(o);
  const bool present = sysinfo::path_exists(args.hdmi_device);
  std::size_t bufsiz = 0;
  {
    std::ifstream in("/sys/module/spidev/parameters/bufsiz");
    if (in) in >> bufsiz;
  }
  rlimit rt{}, mem{};
#ifdef RLIMIT_RTPRIO
  getrlimit(RLIMIT_RTPRIO, &rt);
#endif
  getrlimit(RLIMIT_MEMLOCK, &mem);
  auto lim = [](rlim_t v) -> json { return v == RLIM_INFINITY ? json("unlimited") : json(static_cast<unsigned long long>(v)); };
  bool has_pwm = false;
  std::error_code ec;
  if (fs::exists("/sys/class/pwm", ec))
    for (auto& e : fs::directory_iterator("/sys/class/pwm", ec))
      if (e.path().filename().string().rfind("pwmchip", 0) == 0) has_pwm = true;
  const char* display = std::getenv("DISPLAY");
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  std::string led = env::lower(env::get("MTV_LED_OUTPUT"));
  if (led.empty()) led = args.spi_led ? "spi" : "gpio";
  return {
      {"version", VERSION},
      {"toolkit", "Qt"},
      {"display", display ? display : (wayland ? wayland : "")},
      {"data", args.data},
      {"ffmpeg", proc::which("ffmpeg")},
      {"ffprobe", proc::which("ffprobe")},
      {"v4l2_ctl", proc::which("v4l2-ctl")},
      {"motor_driver", env::get("MTV_MOTOR_DRIVER", "a4988")},
      {"microsteps", env::get_int("MTV_MICROSTEPS", 16)},
      {"ramp_rate_fps", ramp_rate_from_env()},
      {"start_fps", env::get_double("MTV_START_FPS", DEFAULT_START_FPS)},
      {"hardware_pwm", has_pwm},
      {"gpiochip", sysinfo::path_exists("/dev/gpiochip0")},
      {"gpiomem", sysinfo::path_exists("/dev/gpiomem")},
      {"i2c", sysinfo::path_exists("/dev/i2c-1")},
      {"spidev", sysinfo::path_exists("/dev/spidev0.0")},
      {"spidev_bufsiz", bufsiz},
      {"led_output", led},
      {"rtprio_limit", lim(rt.rlim_cur)},
      {"memlock_limit", lim(mem.rlim_cur)},
      {"isolated_cpus", rt::isolated_cpus()},
      {"realtime_cpu", rt::realtime_cpu()},
      {"hdmi_device", args.hdmi_device},
      {"hdmi_device_exists", present},
      {"hdmi_bridge", present ? probe.bridge() : "device missing"},
      {"hdmi_input_format", probe.input_format()},
      {"hdmi_backend", probe.backend_setting()},
      {"native_v4l2",
#ifdef MTV_LINUX
       true
#else
       false
#endif
      },
      {"hdmi_edid", probe.edid_path().empty() ? json(nullptr) : json(probe.edid_path())},
      {"default_source", resolve_default_source(args)},
      {"mock_hardware", args.mock_hardware},
      {"fullscreen", args.fullscreen},
  };
}

std::unique_ptr<Studio> build_studio(Args& args, std::string* hardware_note) {
  StudioOptions o;
  args.data = usable_data_dir(args.data);
  o.root = args.data;
  o.hdmi = {args.hdmi_device, args.hdmi_fit, args.hdmi_format, args.hdmi_edid, args.hdmi_backend, args.hdmi_bridge};
  o.hardware.motor_port = args.motor_port;
  o.hardware.opto_pin = args.opto_pin;
  o.hardware.led_pin = args.led_pin;
  o.hardware.use_spi_led = args.spi_led;
  o.default_source = resolve_default_source(args);
  if (args.mock_hardware) {
    o.hardware.force_mock = true;
    return std::make_unique<Studio>(o);
  }
  try {
    auto studio = std::make_unique<Studio>(o);
    std::string notes;
    for (const auto& n : studio->realtime().hardware_notes()) notes += (notes.empty() ? "" : "; ") + n;
    if (hardware_note) *hardware_note = notes;
    return studio;
  } catch (const std::exception& exc) {
    std::string reason = exc.what();
    log::error("Hardware initialisation failed (" + reason + "); running in simulation");
    if (hardware_note) *hardware_note = reason;
    o.hardware.force_mock = true;
    return std::make_unique<Studio>(o);
  }
}

}  // namespace mtv::console
