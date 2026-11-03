#include "mtv/common/rt.h"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <thread>

#include "mtv/common/env.h"

namespace mtv::rt {

bool set_fifo_priority(int priority, std::string* why) {
#ifdef __linux__
  sched_param sp{};
  sp.sched_priority = priority;
  int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
  if (rc != 0) {
    if (why) *why = std::string("SCHED_FIFO ") + std::to_string(priority) + " refused (" + std::strerror(rc) +
                    "); install.sh adds an rtprio limit for the gpio group, log out and in once";
    return false;
  }
  return true;
#else
  if (why) *why = "SCHED_FIFO is only used on Linux";
  return false;
#endif
}

bool pin_to_cpu(int cpu, std::string* why) {
#ifdef __linux__
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  int rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
  if (rc != 0) {
    if (why) *why = std::string("CPU affinity ") + std::to_string(cpu) + " refused (" + std::strerror(rc) + ")";
    return false;
  }
  return true;
#else
  if (why) *why = "CPU pinning is only used on Linux";
  return false;
#endif
}

bool lock_memory(std::string* why) {
#ifdef __linux__
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    if (why) *why = std::string("mlockall refused (") + std::strerror(errno) + "); install.sh raises the memlock limit for the gpio group";
    return false;
  }
  return true;
#else
  if (why) *why = "memory locking is only used on Linux";
  return false;
#endif
}

int cpu_count() {
  unsigned n = std::thread::hardware_concurrency();
  return n ? static_cast<int>(n) : 1;
}

int realtime_cpu() {
  if (env::is_set("MTV_RT_CPU")) return env::get_int("MTV_RT_CPU", -1);
  int n = cpu_count();
  return n >= 4 ? n - 1 : -1;
}

std::string isolated_cpus() {
  std::ifstream in("/sys/devices/system/cpu/isolated");
  std::string s;
  if (in) std::getline(in, s);
  return env::trim(s);
}

}  // namespace mtv::rt
