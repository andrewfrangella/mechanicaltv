// Mechanical TV desktop application: the operator console running on the Pi's own display.
//
//   mechanical-tv                       window, hardware if present
//   mechanical-tv --fullscreen          appliance / kiosk
//   mechanical-tv --mock-hardware       development without a Pi
//   mechanical-tv --check               no window: report what would run
#include <QApplication>
#include <QIcon>
#include <QSocketNotifier>
#include <QScreen>
#include <QTimer>
#include <csignal>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>

#include "mtv/common/constants.h"
#include "mtv/common/env.h"
#include "mtv/common/log.h"
#include "mtv/common/rt.h"
#include "mtv/console/app_setup.h"
#include "mtv/console/console_window.h"
#include "mtv/console/debug_window.h"
#include "mtv/hardware/failsafe.h"

using namespace mtv;

namespace {
// SIGTERM (logout, shutdown), SIGINT (Ctrl+C) and SIGHUP become a normal quit through a self-pipe,
// so the motor is stopped and the LED blanked exactly as with Ctrl+Q.
int g_signal_pipe[2] = {-1, -1};
void on_quit_signal(int sig) {
  unsigned char b = static_cast<unsigned char>(sig);
  ssize_t r = ::write(g_signal_pipe[1], &b, 1);
  (void)r;
}
}  // namespace

int main(int argc, char** argv) {
  log::set_thread_name("MainThread");
  std::vector<std::string> args_in(argv + 1, argv + argc);
  console::Args args;
  try {
    args = console::parse_args(args_in);
  } catch (const std::exception& exc) {
    std::fprintf(stderr, "%s%s\n", console::usage().c_str(), exc.what());
    return 2;
  }
  if (args.help) {
    std::fputs(console::usage().c_str(), stdout);
    return 0;
  }
  if (args.version) {
    std::printf("Mechanical TV %s\n", VERSION);
    return 0;
  }
  if (args.check) {
    log::set_echo(false);
    json report = console::check_report(args);
    std::printf("%s\n", report.dump(2).c_str());
    return report["ffmpeg"].get<std::string>().empty() ? 1 : 0;
  }

  // Crashes: turn the motor and LED off before the process dies (GPIO and PWM outputs persist).
  failsafe::install_crash_handlers();
  std::set_terminate([] {
    failsafe::trigger();
    std::abort();
  });

  QApplication app(argc, argv);
  app.setApplicationName("Mechanical TV");
  app.setDesktopFileName("mechanical-tv");
  app.setWindowIcon(QIcon::fromTheme("mechanical-tv"));

  const std::string requested_data = args.data;
  args.data = console::usable_data_dir(args.data);
  std::string log_path = log::setup((std::filesystem::path(args.data) / "logs").string());
  if (log_path.empty()) std::fprintf(stderr, "Log file unavailable under %s/logs\n", args.data.c_str());
  log::info(std::string("Mechanical TV ") + VERSION + " local console starting (data " + args.data + ")");
  if (requested_data != args.data) log::info("Data directory " + requested_data + " is not writable; using " + args.data);

  std::string note;
  std::unique_ptr<Studio> studio = console::build_studio(args, &note);
  if (studio->hardware_enabled() && env::get("MTV_MLOCK", "1") != "0") {
    // Keep the realtime threads from ever waiting on a page fault.
    std::string why;
    if (!rt::lock_memory(&why)) log::warning(why);
  }
  if (args.free_run) studio->realtime().set_free_run(true);
  auto* window = new console::ConsoleWindow(*studio, args.fullscreen, QString::fromStdString(note), QString::fromStdString(log_path));
  if (::pipe(g_signal_pipe) == 0) {
    ::fcntl(g_signal_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(g_signal_pipe[1], F_SETFD, FD_CLOEXEC);
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) {
      struct sigaction sa{};
      sa.sa_handler = on_quit_signal;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_RESTART;
      ::sigaction(sig, &sa, nullptr);
    }
    auto* notifier = new QSocketNotifier(g_signal_pipe[0], QSocketNotifier::Read, window);
    QObject::connect(notifier, &QSocketNotifier::activated, window, [window] {
      unsigned char b = 0;
      ssize_t r = ::read(g_signal_pipe[0], &b, 1);
      (void)r;
      log::info(log::format("signal %d: shutting down", static_cast<int>(b)));
      window->quit_console();
    });
  }
  if (!args.fullscreen) window->show();
  if (args.debug_window) window->debug()->show_window();
  log::info(std::string("Mechanical TV ") + VERSION + ": local console — " + (studio->hardware_enabled() ? "REALTIME HARDWARE" : "SIMULATION"));
  if (!args.screenshot.empty()) {
    QString file = QString::fromStdString(args.screenshot);
    QTimer::singleShot(4000, window, [window, file] {
      window->grab().save(file);
      window->quit_console();
    });
  }
  int rc = app.exec();
  if (!window->closing()) studio->close();
  delete window;
  studio.reset();
  return rc;
}
