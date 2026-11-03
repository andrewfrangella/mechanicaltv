// Child processes: run-to-completion with captured output and a timeout (ffprobe, ffmpeg conversions,
// v4l2-ctl) and a streaming reader for a long-lived child (the FFmpeg capture fallback).
#pragma once
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace mtv::proc {

struct Result {
  int exit_code = -1;        // -1: could not start; -2: killed on timeout
  bool timed_out = false;
  std::string out;
  std::string err;
  std::string error;         // start failure reason
  bool ok() const { return exit_code == 0; }
};

// Run argv to completion; stdin is /dev/null. timeout <= 0 means no timeout. Setting *cancel kills
// the child (reported like a timeout).
Result run(const std::vector<std::string>& argv, double timeout_s = 0.0, const std::atomic<bool>* cancel = nullptr);

// Full path of an executable on PATH, or "" (like shutil.which).
std::string which(const std::string& name);

// A running child with its stdout (and stderr) available as file descriptors.
class Child {
 public:
  Child() = default;
  ~Child();
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  // Start argv with stdout and stderr piped; false (with reason) when exec fails.
  bool start(const std::vector<std::string>& argv, std::string* error);
  int stdout_fd() const { return out_fd_; }
  int stderr_fd() const { return err_fd_; }
  int pid() const { return pid_; }
  bool running() const { return pid_ > 0; }
  // Non-blocking exit check: nullopt while running, else the exit status (negative = signal).
  std::optional<int> poll();
  // terminate(), wait up to timeout_s, kill(), wait again. Returns the exit status or nullopt if hung.
  std::optional<int> terminate(double timeout_s = 1.0);
  void close_pipes();

 private:
  int pid_ = -1;
  int out_fd_ = -1;
  int err_fd_ = -1;
  std::optional<int> status_;
};

}  // namespace mtv::proc
