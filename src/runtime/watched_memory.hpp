// Memory whose written pages can be found (rollback snapshots: m3::Snapshots
// copies only the pages a frame wrote). A board's big memories (main RAM,
// the Real3D's, the tile generator's, the sound and network boards' RAM)
// are WatchedVectors.
//
// Each system's own way (docs/netplay.md, "Rollback"), whoever writes the
// memory (the recompiled code, DMA, the devices, the 68000s):
// - Windows: write watch (MEM_WRITE_WATCH, GetWriteWatch). The system notes
//   the pages written at no cost to the writes.
// - Linux 6.7 and later: the kernel's own (userfaultfd's asynchronous write
//   protection, listed and protected again with PAGEMAP_SCAN): no signals.
//   Checked when first used; else, as on macOS:
// - Write protection: the memory made read-only at each snapshot, the first
//   write to each page after it caught (a signal), the page noted and made
//   writable. A system call writing into it would fail: nothing does (they
//   write into their own buffers first).
// Memory isn't watched until a snapshot starts (forget_written): runs that
// take none pay nothing. A board's memory is written by its own thread
// only, which also takes its snapshots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace m3 {

namespace watched {
constexpr std::size_t kPage = 4096; // pages are listed in these (a system's larger pages: each of its parts)

enum class Method : uint8_t {
    none,          // can't list: the snapshots compare pages instead
    write_watch,   // Windows
    pagemap_scan,  // Linux 6.7+
    write_protect, // Linux, macOS
};
const char* name(Method m);
// The method memory allocated from now on uses (the best this system has).
Method method();
// Tests: memory allocated from now on uses `m` instead, if this system has
// it (false if not; none is always there).
bool use(Method m);
// Whether pages written can be listed here.
inline bool available() { return method() != Method::none; }

void* allocate(std::size_t bytes);
void release(void* p, std::size_t bytes);
// The pages of [base, base + bytes) written since the last call (or since
// forget_written), as page indexes from base; the record starts again. False
// if they can't be listed (not watched memory, or watching not started).
bool take_written(void* base, std::size_t bytes, std::vector<std::size_t>& pages);
// Watching starts (or starts again) here: nothing written so far.
void forget_written(void* base, std::size_t bytes);
// Another reader of the same memory's record (a side-run worker's copy of RAM, beside rollback's snapshots): each
// reader has its own, as the two above (reader 0's), neither missing what the other has taken. Its watching starts
// at its first forget_written.
int new_reader();
bool take_written(int reader, void* base, std::size_t bytes, std::vector<std::size_t>& pages);
void forget_written(int reader, void* base, std::size_t bytes);
} // namespace watched

template <class T> struct WatchedAllocator {
    using value_type = T;
    WatchedAllocator() = default;
    template <class U> WatchedAllocator(const WatchedAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = watched::allocate(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t n) noexcept { watched::release(p, n * sizeof(T)); }
    template <class U> bool operator==(const WatchedAllocator<U>&) const noexcept { return true; }
};

template <class T> using WatchedVector = std::vector<T, WatchedAllocator<T>>;

} // namespace m3
