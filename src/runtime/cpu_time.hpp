// CPU time used (user and kernel), for reports: what a run costs a machine,
// apart from how long it waited (for other boards, for the network).
#pragma once

namespace m3 {

// The calling thread's, in seconds.
double thread_cpu_seconds();
// The whole process's (every thread), in seconds.
double process_cpu_seconds();

} // namespace m3
