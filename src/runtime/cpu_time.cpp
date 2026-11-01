#include "runtime/cpu_time.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

// Windows: the cycles each thread ran (QueryThreadCycleTime, QueryProcessCycleTime), not GetThreadTimes, which
// charges whole clock ticks (15.6 ms) to whatever thread is running at each: a thread that waits every few hundred
// microseconds (linked boards meeting) is counted far off. Cycles to seconds: the counter's rate measured once,
// against the performance counter, over a busy 50 ms.
namespace m3 {

namespace {
double cycles_per_second() {
    static std::atomic<double> rate{0};
    if (const double r = rate.load(); r > 0) return r;
    LARGE_INTEGER frequency, t0, t1;
    ULONG64 c0 = 0, c1 = 0;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&t0);
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    do QueryPerformanceCounter(&t1);
    while (t1.QuadPart - t0.QuadPart < frequency.QuadPart / 20);
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    const double r = static_cast<double>(c1 - c0) * static_cast<double>(frequency.QuadPart) / static_cast<double>(t1.QuadPart - t0.QuadPart);
    rate.store(r);
    return r;
}
} // namespace

double thread_cpu_seconds() {
    ULONG64 cycles = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? static_cast<double>(cycles) / cycles_per_second() : 0.0;
}

double process_cpu_seconds() {
    ULONG64 cycles = 0;
    return QueryProcessCycleTime(GetCurrentProcess(), &cycles) ? static_cast<double>(cycles) / cycles_per_second() : 0.0;
}

} // namespace m3

#else
#include <ctime>

namespace m3 {

namespace {
double seconds(clockid_t clock) {
    timespec t{};
    return clock_gettime(clock, &t) == 0 ? static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) * 1e-9 : 0.0;
}
} // namespace

double thread_cpu_seconds() { return seconds(CLOCK_THREAD_CPUTIME_ID); }
double process_cpu_seconds() { return seconds(CLOCK_PROCESS_CPUTIME_ID); }

} // namespace m3
#endif
