#include "runtime/memory_free.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace m3 {

uint64_t memory_free_bytes() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
}

} // namespace m3

#elif defined(__APPLE__)
#include <mach/mach.h>

namespace m3 {

uint64_t memory_free_bytes() {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    const mach_port_t host = mach_host_self();
    if (host_page_size(host, &page) != KERN_SUCCESS ||
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS)
        return 0;
    return (static_cast<uint64_t>(vm.free_count) + vm.inactive_count) * page;
}

} // namespace m3

#else
#include <cstdio>

namespace m3 {

uint64_t memory_free_bytes() {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    std::fclose(f);
    return static_cast<uint64_t>(kb) * 1024;
}

} // namespace m3
#endif
