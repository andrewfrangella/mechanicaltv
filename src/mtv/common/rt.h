// Realtime thread helpers: SCHED_FIFO priority, CPU affinity and memory locking. Each returns false
// with a human-readable reason when the system does not allow it (no rtprio limit, not Linux), so the
// caller can log how to enable it instead of failing.
#pragma once
#include <string>

namespace mtv::rt {

bool set_fifo_priority(int priority, std::string* why = nullptr);   // calling thread
bool pin_to_cpu(int cpu, std::string* why = nullptr);               // calling thread
bool lock_memory(std::string* why = nullptr);                       // mlockall(MCL_CURRENT | MCL_FUTURE)
int cpu_count();
// The realtime CPU: MTV_RT_CPU if set (-1 disables pinning), else the highest-numbered CPU when there
// are at least 4 (CPU 3 on a Pi 4, the one isolcpus=3 reserves), else -1.
int realtime_cpu();
std::string isolated_cpus();   // contents of /sys/devices/system/cpu/isolated ("" when none or not Linux)

}  // namespace mtv::rt
