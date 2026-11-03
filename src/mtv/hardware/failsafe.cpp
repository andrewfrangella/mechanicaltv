#include "mtv/hardware/failsafe.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <initializer_list>
#ifdef MTV_LINUX
#include <linux/gpio.h>
#include <sys/ioctl.h>
#endif

namespace mtv::failsafe {

namespace {
constexpr int MAX_LINES = 8;
std::atomic<int> g_pwm_fd{-1};
std::atomic<int> g_line_fd[MAX_LINES];
std::atomic<int> g_line_value[MAX_LINES];
std::atomic<bool> g_init{false};

void init() {
  bool expected = false;
  if (g_init.compare_exchange_strong(expected, true)) {
    for (int i = 0; i < MAX_LINES; ++i) {
      g_line_fd[i] = -1;
      g_line_value[i] = 0;
    }
  }
}

void on_fatal(int sig) {
  trigger();
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}
}  // namespace

void set_pwm_enable_file(const char* path) {
  init();
  int old = g_pwm_fd.exchange(-1);
  if (old >= 0) ::close(old);
  if (path) g_pwm_fd = ::open(path, O_WRONLY | O_CLOEXEC);
}

void add_line(int line_fd, int safe_value) {
  init();
  for (int i = 0; i < MAX_LINES; ++i) {
    int expected = -1;
    if (g_line_fd[i].compare_exchange_strong(expected, line_fd)) {
      g_line_value[i] = safe_value;
      return;
    }
  }
}

void remove_line(int line_fd) {
  init();
  for (int i = 0; i < MAX_LINES; ++i) {
    int expected = line_fd;
    g_line_fd[i].compare_exchange_strong(expected, -1);
  }
}

void trigger() {
  init();
  int pwm = g_pwm_fd.load();
  if (pwm >= 0) {
    ssize_t r = ::write(pwm, "0\n", 2);
    (void)r;
  }
#ifdef MTV_LINUX
  for (int i = 0; i < MAX_LINES; ++i) {
    int fd = g_line_fd[i].load();
    if (fd < 0) continue;
    gpio_v2_line_values v{};
    v.mask = 1;
    v.bits = g_line_value[i].load() ? 1 : 0;
    ::ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
  }
#endif
}

void install_crash_handlers() {
  init();
  for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
    struct sigaction sa{};
    sa.sa_handler = on_fatal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    ::sigaction(sig, &sa, nullptr);
  }
}

}  // namespace mtv::failsafe
