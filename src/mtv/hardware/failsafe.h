// Last-resort hardware shutdown for crashes. GPIO outputs and the hardware PWM keep running after the
// process dies, which would leave the motor spinning with its coils energised and the LED possibly
// lit. Drivers register the file descriptors that turn their outputs safe; trigger() uses only
// async-signal-safe system calls (write, ioctl) so it can run from a fatal-signal handler.
#pragma once

namespace mtv::failsafe {

// The sysfs PWM "enable" file of the STEP clock (opened write-only here; -1 to forget).
void set_pwm_enable_file(const char* path);
// A GPIO v2 line request fd and the value that makes it safe (A4988 ENABLE -> 1, LED -> 0).
void add_line(int line_fd, int safe_value);
void remove_line(int line_fd);
// Make every registered output safe now. Async-signal-safe.
void trigger();
// SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT: trigger(), then re-raise with the default action.
void install_crash_handlers();

}  // namespace mtv::failsafe
