// Persistent logging shared by the console and the web service (port of mechanical_tv/logs.py).
//
//   <data>/logs/mechanical-tv.log   rotating, 1 MB x 5 files, format
//   "YYYY-mm-dd HH:MM:SS,mmm LEVEL   thread-name: message"
//
// Every record is also echoed to stdout/stderr (journald / terminal) and offered to a listener (the
// console's debug window). Logging never throws and never blocks on a slow listener.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace mtv::log {

enum class Level : int { Debug = 10, Info = 20, Warning = 30, Error = 40, Critical = 50 };
const char* level_name(Level level);   // "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"

struct Record {
  std::int64_t wall_ms = 0;     // milliseconds since the Unix epoch
  Level level = Level::Info;
  std::string thread;           // name of the emitting thread
  std::string message;
  std::string format(bool short_time = false) const;   // the file line (date and time) or the window line (time only)
};

using Listener = std::function<void(const Record&)>;

// Configure the file sink. Creates the directory. Returns the log path, or "" when the file cannot be
// opened (logging then continues to stdout/stderr only). Safe to call more than once.
std::string setup(const std::string& directory, std::uint64_t max_bytes = 1'000'000, int backup_count = 5);
void set_listener(Listener listener);   // called on the logging thread; keep it fast
void set_echo(bool enabled);            // echo records to stdout/stderr (default on)

void post(Level level, const std::string& message);
void debug(const std::string& message);
void info(const std::string& message);
void warning(const std::string& message);
void error(const std::string& message);
// printf-style helpers
void infof(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void errorf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warningf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void debugf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Record an exception with context (used by UI action handlers): "where: what".
void exception(const std::string& where, const std::exception& exc);

std::string current_thread_name();
void set_thread_name(const std::string& name);   // pthread name (15 chars) plus the full name for records

std::string vformat(const char* fmt, va_list args);
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace mtv::log
