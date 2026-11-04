#include "runtime/watched_memory.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>

namespace m3::watched {

// Each system's own record (below), read once: its pages, the record started again.
bool os_take_written(void* base, std::size_t bytes, std::vector<std::size_t>& pages);
void os_forget_written(void* base, std::size_t bytes);
void drop_records(void* base); // (memory released: its readers' records gone)

const char* name(Method m) {
    switch (m) {
    case Method::write_watch: return "write watch";
    case Method::pagemap_scan: return "PAGEMAP_SCAN";
    case Method::write_protect: return "write protection";
    default: return "compared";
    }
}

} // namespace m3::watched

#ifdef _WIN32
// ---------------------------------------------------------------- Windows

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace m3::watched {

namespace {
bool g_watch = true; // (tests: use(Method::none))
}

Method method() { return g_watch ? Method::write_watch : Method::none; }

bool use(Method m) {
    if (m != Method::write_watch && m != Method::none) return false;
    g_watch = m == Method::write_watch;
    return true;
}

void* allocate(std::size_t bytes) {
    return VirtualAlloc(nullptr, bytes ? bytes : 1, MEM_RESERVE | MEM_COMMIT | (g_watch ? MEM_WRITE_WATCH : 0), PAGE_READWRITE);
}

void release(void* p, std::size_t) {
    drop_records(p);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

bool os_take_written(void* base, std::size_t bytes, std::vector<std::size_t>& pages) {
    pages.clear();
    if (!bytes) return true;
    const std::size_t total = (bytes + kPage - 1) / kPage;
    static thread_local std::vector<void*> found;
    found.resize(total);
    ULONG_PTR n = total;
    DWORD granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, bytes, found.data(), &n, &granularity) != 0) return false;
    for (ULONG_PTR i = 0; i < n; ++i)
        pages.push_back(static_cast<std::size_t>(static_cast<uint8_t*>(found[i]) - static_cast<uint8_t*>(base)) / kPage);
    return true;
}

void os_forget_written(void* base, std::size_t bytes) {
    if (bytes) ResetWriteWatch(base, bytes);
}

} // namespace m3::watched

#else
// ---------------------------------------------------------------- Linux, macOS

#include <atomic>
#include <csignal>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_FEATURE_WP_UNPOPULATED) && defined(UFFD_USER_MODE_ONLY)
#define M3_PAGEMAP_SCAN 1
#endif
#endif

namespace m3::watched {

namespace {

std::size_t os_page() {
    static const std::size_t n = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return n;
}

// The memory handed out (a slot each): the fault handler looks here, so a
// slot's fields are set before its base is (and its base cleared first).
struct Slot {
    std::atomic<uintptr_t> base{0};
    std::size_t bytes = 0; // whole pages of the system's
    Method how = Method::none;
    std::atomic<bool> watching{false};
    std::unique_ptr<std::atomic<uint8_t>[]> written; // write_protect: one a page of the system's
};
constexpr std::size_t kSlots = 64;
Slot g_slots[kSlots];
std::mutex g_lock; // (allocating, releasing)
std::once_flag g_once;
Method g_best = Method::none, g_method = Method::none;

Slot* find(void* base) {
    const auto b = reinterpret_cast<uintptr_t>(base);
    for (Slot& s : g_slots)
        if (s.base.load(std::memory_order_acquire) == b) return &s;
    return nullptr;
}

// ---- Write protection: a write's fault notes its page and lets it be written.

struct sigaction g_old_segv{}, g_old_bus{};

void on_fault(int sig, siginfo_t* info, void* context) {
    const auto at = reinterpret_cast<uintptr_t>(info->si_addr);
    for (Slot& s : g_slots) {
        const uintptr_t b = s.base.load(std::memory_order_acquire);
        if (!b || at < b || at >= b + s.bytes || s.how != Method::write_protect) continue;
        const std::size_t page = (at - b) / os_page();
        s.written[page].store(1, std::memory_order_relaxed);
        mprotect(reinterpret_cast<void*>(b + page * os_page()), os_page(), PROT_READ | PROT_WRITE);
        return;
    }
    // Not ours: whatever was there before us (by default: the crash it is).
    const struct sigaction& old = sig == SIGBUS ? g_old_bus : g_old_segv;
    if (old.sa_flags & SA_SIGINFO) {
        if (old.sa_sigaction) {
            old.sa_sigaction(sig, info, context);
            return;
        }
    } else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
        old.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL); // (the fault comes again, and does what it would have)
}

bool install_handler() {
    struct sigaction sa {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGSEGV, &sa, &g_old_segv) == 0 && sigaction(SIGBUS, &sa, &g_old_bus) == 0;
}

// ---- Linux 6.7+: userfaultfd's asynchronous write protection; PAGEMAP_SCAN
// lists the pages written and protects them again, in one call.

#ifdef M3_PAGEMAP_SCAN
int g_uffd = -1, g_pagemap = -1;

bool uffd_register(void* p, std::size_t n) {
    uffdio_register r{};
    r.range.start = reinterpret_cast<uint64_t>(p);
    r.range.len = n;
    r.mode = UFFDIO_REGISTER_MODE_WP;
    return ioctl(g_uffd, UFFDIO_REGISTER, &r) == 0;
}

void uffd_unregister(void* p, std::size_t n) {
    uffdio_range r{reinterpret_cast<uint64_t>(p), n};
    ioctl(g_uffd, UFFDIO_UNREGISTER, &r);
}

bool uffd_protect(void* p, std::size_t n) {
    uffdio_writeprotect w{};
    w.range.start = reinterpret_cast<uint64_t>(p);
    w.range.len = n;
    w.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    return ioctl(g_uffd, UFFDIO_WRITEPROTECT, &w) == 0;
}

// The pages written in [p, p + n), as kPage indexes from p; protected again.
bool scan(void* p, std::size_t n, std::vector<std::size_t>& pages) {
    const auto base = reinterpret_cast<uint64_t>(p);
    uint64_t from = base;
    const uint64_t end = base + n;
    page_region found[64];
    while (from < end) {
        pm_scan_arg a{};
        a.size = sizeof a;
        a.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        a.start = from;
        a.end = end;
        a.vec = reinterpret_cast<uint64_t>(found);
        a.vec_len = 64;
        a.category_mask = PAGE_IS_WRITTEN;
        a.return_mask = PAGE_IS_WRITTEN;
        const long got = ioctl(g_pagemap, PAGEMAP_SCAN, &a);
        if (got < 0) return false;
        for (long i = 0; i < got; ++i)
            for (uint64_t at = found[i].start; at < found[i].end; at += kPage) pages.push_back(static_cast<std::size_t>((at - base) / kPage));
        if (got < 64 || a.walk_end <= from) break;
        from = a.walk_end;
    }
    return true;
}

// Whether it's here and does what it should: a page written found, nothing else.
bool pagemap_works() {
    g_uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
    if (g_uffd < 0) return false;
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
    g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    bool ok = ioctl(g_uffd, UFFDIO_API, &api) == 0 && g_pagemap >= 0;
    const std::size_t n = 4 * os_page();
    void* p = ok ? mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
    if (p != MAP_FAILED) {
        std::memset(p, 0, n);
        std::vector<std::size_t> pages;
        ok = uffd_register(p, n) && uffd_protect(p, n) && scan(p, n, pages) && pages.empty();
        static_cast<volatile uint8_t*>(p)[2 * os_page() + 5] = 1;
        ok = ok && scan(p, n, pages) && pages.size() == os_page() / kPage && pages.front() == 2 * os_page() / kPage;
        pages.clear();
        ok = ok && scan(p, n, pages) && pages.empty(); // (protected again by the scan)
        uffd_unregister(p, n);
        munmap(p, n);
    } else {
        ok = false;
    }
    if (!ok) {
        if (g_uffd >= 0) close(g_uffd);
        if (g_pagemap >= 0) close(g_pagemap);
        g_uffd = g_pagemap = -1;
    }
    return ok;
}
#endif

void choose() {
    std::call_once(g_once, [] {
        const bool handler = install_handler();
#ifdef M3_PAGEMAP_SCAN
        if (pagemap_works()) g_best = Method::pagemap_scan;
        else
#endif
            if (handler) g_best = Method::write_protect;
        g_method = g_best;
    });
}

} // namespace

Method method() {
    choose();
    return g_method;
}

bool use(Method m) {
    choose();
    const bool here = m == Method::none || m == g_best || (m == Method::write_protect && g_best != Method::none);
    if (here) g_method = m;
    return here;
}

void* allocate(std::size_t bytes) {
    choose();
    const std::size_t n = (std::max<std::size_t>(bytes, 1) + os_page() - 1) / os_page() * os_page();
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (g_method == Method::none) return p;
    std::lock_guard guard(g_lock);
    for (Slot& s : g_slots) {
        if (s.base.load(std::memory_order_relaxed)) continue;
        s.bytes = n;
        s.how = g_method;
        s.watching = false;
        if (s.how == Method::write_protect) {
            s.written = std::make_unique<std::atomic<uint8_t>[]>(n / os_page());
            for (std::size_t i = 0; i < n / os_page(); ++i) s.written[i].store(0, std::memory_order_relaxed);
        }
#ifdef M3_PAGEMAP_SCAN
        if (s.how == Method::pagemap_scan && !uffd_register(p, n)) s.how = Method::none;
#endif
        s.base.store(reinterpret_cast<uintptr_t>(p), std::memory_order_release);
        break;
    }
    return p; // (no slot left: plain memory, compared)
}

void release(void* p, std::size_t bytes) {
    drop_records(p);
    if (!p) return;
    const std::size_t n = (std::max<std::size_t>(bytes, 1) + os_page() - 1) / os_page() * os_page();
    {
        std::lock_guard guard(g_lock);
        if (Slot* s = find(p)) {
            s->base.store(0, std::memory_order_release);
#ifdef M3_PAGEMAP_SCAN
            if (s->how == Method::pagemap_scan) uffd_unregister(p, n);
#endif
            s->written.reset();
            s->how = Method::none;
        }
    }
    munmap(p, n);
}

bool os_take_written(void* base, std::size_t bytes, std::vector<std::size_t>& pages) {
    pages.clear();
    Slot* s = find(base);
    if (!s || !s->watching || s->how == Method::none || bytes > s->bytes) return false;
#ifdef M3_PAGEMAP_SCAN
    if (s->how == Method::pagemap_scan) {
        if (!scan(base, s->bytes, pages)) return false;
        while (!pages.empty() && pages.back() * kPage >= bytes) pages.pop_back(); // (the rounding past its end)
        return true;
    }
#endif
    // Write protection: the pages noted, protected again (runs of them at once).
    const std::size_t sub = os_page() / kPage, count = s->bytes / os_page();
    auto* b = static_cast<uint8_t*>(base);
    for (std::size_t p = 0; p < count;) {
        if (!s->written[p].load(std::memory_order_relaxed)) {
            ++p;
            continue;
        }
        std::size_t q = p;
        for (; q < count && s->written[q].load(std::memory_order_relaxed); ++q) {
            s->written[q].store(0, std::memory_order_relaxed);
            for (std::size_t k = 0; k < sub; ++k)
                if ((q * sub + k) * kPage < bytes) pages.push_back(q * sub + k);
        }
        mprotect(b + p * os_page(), (q - p) * os_page(), PROT_READ);
        p = q;
    }
    return true;
}

void os_forget_written(void* base, std::size_t) {
    Slot* s = find(base);
    if (!s || s->how == Method::none) return;
#ifdef M3_PAGEMAP_SCAN
    if (s->how == Method::pagemap_scan) {
        s->watching = uffd_protect(base, s->bytes);
        return;
    }
#endif
    for (std::size_t i = 0; i < s->bytes / os_page(); ++i) s->written[i].store(0, std::memory_order_relaxed);
    s->watching = mprotect(base, s->bytes, PROT_READ) == 0;
}

} // namespace m3::watched

#endif

// ---------------------------------------------------------------- Several readers, each its own record
// The system's record starts again as it is read, so two readers of the same memory (rollback's snapshots and a
// side-run worker's copy of RAM) would each miss what the other took. Each read passes what the system noted on to
// every reader's own record (pages marked), then gives the asker its own and clears it.

namespace m3::watched {

namespace {
struct Records {
    std::map<int, std::vector<uint8_t>> marks; // by reader: a byte a page, 1 written since it last asked
};
std::mutex g_records_lock;
std::map<void*, Records> g_records; // by memory block
std::vector<std::size_t> g_noted;    // (scratch, under the lock)

// What the system has noted since, onto every reader's record (false: it couldn't say).
bool pass_on(void* base, std::size_t bytes, Records& r) {
    if (!os_take_written(base, bytes, g_noted)) return false;
    for (auto& [reader, marks] : r.marks)
        for (const std::size_t p : g_noted)
            if (p < marks.size()) marks[p] = 1;
    return true;
}
} // namespace

int new_reader() {
    static std::mutex lock;
    static int next = 1; // (0: the first, readers that name none)
    std::lock_guard guard(lock);
    return next++;
}

bool take_written(int reader, void* base, std::size_t bytes, std::vector<std::size_t>& pages) {
    pages.clear();
    std::lock_guard guard(g_records_lock);
    Records& r = g_records[base];
    const auto it = r.marks.find(reader);
    if (it == r.marks.end() || !pass_on(base, bytes, r)) return false; // (not watched by it yet; or not listable)
    std::vector<uint8_t>& marks = it->second;
    for (std::size_t p = 0; p < marks.size(); ++p)
        if (marks[p]) pages.push_back(p), marks[p] = 0;
    return true;
}

void forget_written(int reader, void* base, std::size_t bytes) {
    std::lock_guard guard(g_records_lock);
    Records& r = g_records[base];
    if (r.marks.empty()) os_forget_written(base, bytes); // (the first: watching starts)
    else pass_on(base, bytes, r);                        // (the others keep what was noted)
    r.marks[reader].assign((bytes + kPage - 1) / kPage, 0);
}

bool take_written(void* base, std::size_t bytes, std::vector<std::size_t>& pages) { return take_written(0, base, bytes, pages); }
void forget_written(void* base, std::size_t bytes) { forget_written(0, base, bytes); }

void drop_records(void* base) {
    std::lock_guard guard(g_records_lock);
    g_records.erase(base);
}

} // namespace m3::watched
