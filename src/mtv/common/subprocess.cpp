#include "mtv/common/subprocess.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

extern char** environ;

namespace mtv::proc {

namespace {

// Pipes are close-on-exec so one child never inherits another's pipe ends (posix_spawn's dup2 onto
// 0/1/2 clears the flag for the ends the child is meant to have).
int cloexec_pipe(int fds[2]) {
#ifdef __linux__
  return ::pipe2(fds, O_CLOEXEC);
#else
  if (::pipe(fds) != 0) return -1;
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return 0;
#endif
}

std::vector<char*> c_argv(const std::vector<std::string>& argv) {
  std::vector<char*> out;
  out.reserve(argv.size() + 1);
  for (const auto& a : argv) out.push_back(const_cast<char*>(a.c_str()));
  out.push_back(nullptr);
  return out;
}

std::optional<int> decode_status(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return -WTERMSIG(status);
  return std::nullopt;
}

}  // namespace

std::string which(const std::string& name) {
  if (name.find('/') != std::string::npos) {
    return ::access(name.c_str(), X_OK) == 0 ? name : "";
  }
  const char* path = std::getenv("PATH");
  std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
  std::size_t start = 0;
  while (start <= p.size()) {
    std::size_t end = p.find(':', start);
    if (end == std::string::npos) end = p.size();
    std::string dir = p.substr(start, end - start);
    if (!dir.empty()) {
      std::string candidate = dir + "/" + name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
      }
    }
    start = end + 1;
  }
  return "";
}

Result run(const std::vector<std::string>& argv, double timeout_s, const std::atomic<bool>* cancel) {
  Result result;
  if (argv.empty()) {
    result.error = "empty command";
    return result;
  }
  int out_pipe[2], err_pipe[2];
  if (cloexec_pipe(out_pipe) != 0 || cloexec_pipe(err_pipe) != 0) {
    result.error = std::strerror(errno);
    return result;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
  posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
  auto cargv = c_argv(argv);
  pid_t pid = -1;
  int rc = ::posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (rc != 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    result.error = std::string(argv[0]) + ": " + std::strerror(rc);
    return result;
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s > 0 ? timeout_s : 1e9);
  bool out_open = true, err_open = true;
  char buf[8192];
  while (out_open || err_open) {
    struct pollfd fds[2];
    int n = 0;
    if (out_open) fds[n++] = {out_pipe[0], POLLIN, 0};
    if (err_open) fds[n++] = {err_pipe[0], POLLIN, 0};
    int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if ((timeout_s > 0 && remaining_ms <= 0) || (cancel && cancel->load())) {
      result.timed_out = true;
      break;
    }
    int pr = ::poll(fds, n, timeout_s > 0 ? std::min(remaining_ms, 200) : 200);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t got = ::read(fds[i].fd, buf, sizeof buf);
      bool is_out = fds[i].fd == out_pipe[0];
      if (got <= 0) {
        if (got < 0 && errno == EINTR) continue;
        ::close(fds[i].fd);
        if (is_out) out_open = false; else err_open = false;
      } else {
        (is_out ? result.out : result.err).append(buf, static_cast<std::size_t>(got));
      }
    }
  }
  if (result.timed_out) {
    ::kill(pid, SIGKILL);
    if (out_open) ::close(out_pipe[0]);
    if (err_open) ::close(err_pipe[0]);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (result.timed_out) {
    result.exit_code = -2;
  } else {
    result.exit_code = decode_status(status).value_or(-1);
  }
  return result;
}

Child::~Child() {
  if (pid_ > 0) terminate(0.5);
  close_pipes();
}

bool Child::start(const std::vector<std::string>& argv, std::string* error) {
  if (argv.empty()) {
    if (error) *error = "empty command";
    return false;
  }
  int out_pipe[2], err_pipe[2];
  if (cloexec_pipe(out_pipe) != 0 || cloexec_pipe(err_pipe) != 0) {
    if (error) *error = std::strerror(errno);
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
  posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
  auto cargv = c_argv(argv);
  pid_t pid = -1;
  int rc = ::posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (rc != 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    if (error) *error = std::string(argv[0]) + ": " + std::strerror(rc);
    return false;
  }
  pid_ = pid;
  out_fd_ = out_pipe[0];
  err_fd_ = err_pipe[0];
  status_.reset();
  return true;
}

std::optional<int> Child::poll() {
  if (status_) return status_;
  if (pid_ <= 0) return -1;
  int status = 0;
  pid_t r = ::waitpid(pid_, &status, WNOHANG);
  if (r == 0) return std::nullopt;
  if (r < 0) {
    status_ = -1;
  } else {
    status_ = decode_status(status).value_or(-1);
  }
  pid_ = -1;
  return status_;
}

std::optional<int> Child::terminate(double timeout_s) {
  if (status_) return status_;
  if (pid_ <= 0) return std::nullopt;
  ::kill(pid_, SIGTERM);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto s = poll()) return s;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::kill(pid_, SIGKILL);
  deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto s = poll()) return s;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

void Child::close_pipes() {
  if (out_fd_ >= 0) ::close(out_fd_);
  if (err_fd_ >= 0) ::close(err_fd_);
  out_fd_ = err_fd_ = -1;
}

}  // namespace mtv::proc
