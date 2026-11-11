// Command line, environment report and engine construction for the console (port of the
// non-Tk parts of app.py: build_parser, check_report, resolve_default_source, usable_data_dir,
// build_studio).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "mtv/common/json.h"
#include "mtv/engine/studio.h"

namespace mtv::console {

struct Args {
  std::string env_file;
  std::string data;
  bool fullscreen = false;
  bool check = false;
  bool free_run = false;
  bool debug_window = false;
  bool mock_hardware = false;
  bool help = false;
  bool version = false;
  std::string hdmi_device = "/dev/video0";
  std::string hdmi_bridge = "auto";
  std::string hdmi_format = "auto";
  std::string hdmi_edid = "auto";
  std::string hdmi_fit = "fit";
  std::string hdmi_backend = "auto";
  std::string default_source = "auto";
  int motor_port = 1;
  int opto_pin = 4;
  int led_pin = 21;
  bool spi_led = false;
  std::string screenshot;   // developer option: save the window to this PNG after a few seconds, then quit
};

// Loads --env-file first (without overriding the real environment), then reads defaults from MTV_*
// variables, then applies the options. Throws std::invalid_argument with a usage message.
Args parse_args(const std::vector<std::string>& argv);
std::string usage();

std::string resolve_default_source(const Args& args);
// The requested data directory if this user can write it, else the per-user default (with a log line).
std::string usable_data_dir(const std::string& requested);
json check_report(const Args& args);
// Settings chosen in the console (Microsteps, Spin-up ramp), kept in <data>/settings.json. They
// override MTV_MICROSTEPS and MTV_RAMP_RATE. A missing or unreadable file is an empty object.
json load_console_settings(const std::string& data_dir);
// Merge `patch` into that file (other keys are kept). False, with *why set, when it cannot be written.
bool save_console_settings(const std::string& data_dir, const json& patch, std::string* why = nullptr);
// Create the engine; if real hardware fails to initialise, report why and fall back to simulation.
// Applies the saved console settings.
std::unique_ptr<Studio> build_studio(Args& args, std::string* hardware_note);

}  // namespace mtv::console
