// Monotonic time helpers. All engine timing uses CLOCK_MONOTONIC seconds as double (like the Python
// time.monotonic()/perf_counter()) except the realtime core, which works in integer nanoseconds.
#pragma once
#include <chrono>
#include <cstdint>
#include <thread>

namespace mtv {

using Clock = std::chrono::steady_clock;

inline double now_s() {
  return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

inline void sleep_s(double seconds) {
  if (seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}  // namespace mtv
