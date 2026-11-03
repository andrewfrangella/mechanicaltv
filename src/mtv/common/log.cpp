#include "mtv/common/log.h"

#include <pthread.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mtv::log {

namespace {

struct Sink {
  std::mutex mutex;
  std::mutex listener_mutex;   // held while the listener runs, so set_listener(nullptr) waits for it
  std::string path;
  std::FILE* file = nullptr;
  std::uint64_t max_bytes = 1'000'000;
  int backup_count = 5;
  std::uint64_t written = 0;
  Listener listener;
  bool echo = true;
};

Sink& sink() {
  static Sink s;
  return s;
}

thread_local std::string t_thread_name;

void rotate_locked(Sink& s) {
  if (s.file) {
    std::fclose(s.file);
    s.file = nullptr;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  for (int i = s.backup_count - 1; i >= 1; --i) {
    fs::path from = s.path + "." + std::to_string(i);
    fs::path to = s.path + "." + std::to_string(i + 1);
    if (fs::exists(from, ec)) fs::rename(from, to, ec);
  }
  if (s.backup_count > 0) fs::rename(s.path, s.path + ".1", ec);
  s.file = std::fopen(s.path.c_str(), "a");
  s.written = 0;
}

}  // namespace

const char* level_name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARNING";
    case Level::Error: return "ERROR";
    case Level::Critical: return "CRITICAL";
  }
  return "INFO";
}

std::string Record::format(bool short_time) const {
  std::time_t secs = static_cast<std::time_t>(wall_ms / 1000);
  int ms = static_cast<int>(wall_ms % 1000);
  std::tm tm{};
  localtime_r(&secs, &tm);
  char stamp[64];
  if (short_time) {
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
  } else {
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
  }
  char line[128];
  std::snprintf(line, sizeof line, "%s,%03d %-7s %s: ", stamp, ms, level_name(level), thread.c_str());
  return std::string(line) + message;
}

std::string setup(const std::string& directory, std::uint64_t max_bytes, int backup_count) {
  Sink& s = sink();
  std::lock_guard<std::mutex> lock(s.mutex);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  std::string path = (std::filesystem::path(directory) / "mechanical-tv.log").string();
  if (s.file && s.path == path) return s.path;
  if (s.file) {
    std::fclose(s.file);
    s.file = nullptr;
  }
  s.path = path;
  s.max_bytes = max_bytes;
  s.backup_count = backup_count;
  s.file = std::fopen(path.c_str(), "a");
  if (!s.file) {
    s.path.clear();
    return "";
  }
  struct stat st{};
  s.written = (::stat(path.c_str(), &st) == 0) ? static_cast<std::uint64_t>(st.st_size) : 0;
  return s.path;
}

void set_listener(Listener listener) {
  Sink& s = sink();
  std::lock_guard<std::mutex> lock(s.listener_mutex);
  s.listener = std::move(listener);
}

void set_echo(bool enabled) {
  Sink& s = sink();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.echo = enabled;
}

std::string current_thread_name() {
  if (!t_thread_name.empty()) return t_thread_name;
  char buf[32] = {0};
#if defined(__linux__) || defined(__APPLE__)
  if (pthread_getname_np(pthread_self(), buf, sizeof buf) == 0 && buf[0]) {
    t_thread_name = buf;
    return t_thread_name;
  }
#endif
  return "MainThread";
}

void set_thread_name(const std::string& name) {
  t_thread_name = name;
  std::string short_name = name.substr(0, 15);
#if defined(__linux__)
  pthread_setname_np(pthread_self(), short_name.c_str());
#elif defined(__APPLE__)
  pthread_setname_np(short_name.c_str());
#endif
}

void post(Level level, const std::string& message) {
  Record rec;
  rec.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
  rec.level = level;
  rec.thread = current_thread_name();
  rec.message = message;
  Sink& s = sink();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.echo) {
      std::FILE* out = (level >= Level::Error) ? stderr : stdout;
      std::fputs(message.c_str(), out);
      std::fputc('\n', out);
      std::fflush(out);
    }
    if (s.file) {
      std::string line = rec.format() + "\n";
      if (s.max_bytes && s.written + line.size() > s.max_bytes) rotate_locked(s);
      if (s.file) {
        std::fputs(line.c_str(), s.file);
        std::fflush(s.file);
        s.written += line.size();
      }
    }
  }
  std::lock_guard<std::mutex> lock(s.listener_mutex);
  if (s.listener) {
    try {
      s.listener(rec);
    } catch (...) {
    }
  }
}

void debug(const std::string& m) { post(Level::Debug, m); }
void info(const std::string& m) { post(Level::Info, m); }
void warning(const std::string& m) { post(Level::Warning, m); }
void error(const std::string& m) { post(Level::Error, m); }

std::string vformat(const char* fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  int n = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (n < 0) return fmt;
  std::string out(static_cast<std::size_t>(n) + 1, '\0');
  std::vsnprintf(out.data(), out.size(), fmt, args);
  out.resize(static_cast<std::size_t>(n));
  return out;
}

std::string format(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::string s = vformat(fmt, args);
  va_end(args);
  return s;
}

#define MTV_LOG_F(name, level)              \
  void name(const char* fmt, ...) {         \
    va_list args;                           \
    va_start(args, fmt);                    \
    std::string s = vformat(fmt, args);     \
    va_end(args);                           \
    post(level, s);                         \
  }
MTV_LOG_F(infof, Level::Info)
MTV_LOG_F(errorf, Level::Error)
MTV_LOG_F(warningf, Level::Warning)
MTV_LOG_F(debugf, Level::Debug)
#undef MTV_LOG_F

void exception(const std::string& where, const std::exception& exc) {
  post(Level::Error, where + ": " + exc.what());
}

}  // namespace mtv::log
