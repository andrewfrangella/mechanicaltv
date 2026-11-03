#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>

#include "mtv/common/clock.h"

namespace testing {

// A fresh temporary directory removed at scope exit.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    auto base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto p = base / ("mtv-test-" + std::to_string(::getpid()) + "-" + std::to_string(i) + "-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000));
      if (std::filesystem::create_directory(p)) {
        path = p;
        return;
      }
    }
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string str() const { return path.string(); }
};

// Set an environment variable for the scope, restoring the previous value.
struct EnvGuard {
  std::string name;
  bool had;
  std::string old;
  EnvGuard(const std::string& n, const std::string& value) : name(n) {
    const char* v = std::getenv(n.c_str());
    had = v != nullptr;
    if (v) old = v;
    ::setenv(n.c_str(), value.c_str(), 1);
  }
  ~EnvGuard() {
    if (had) ::setenv(name.c_str(), old.c_str(), 1);
    else ::unsetenv(name.c_str());
  }
};

inline bool wait_until(const std::function<bool()>& cond, double timeout_s = 5.0) {
  double end = mtv::now_s() + timeout_s;
  while (mtv::now_s() < end) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cond();
}

inline std::string repo(const std::string& rel) { return std::string(MTV_REPO_ROOT) + "/" + rel; }

}  // namespace testing
