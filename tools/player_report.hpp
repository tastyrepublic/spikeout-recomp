// Player reports (docs/handoff.md, the owner's to-do of 2026-11-08): what the window prints kept in a log file, a
// crash report when it crashes, the controllers SDL sees, and "Report a problem": one zip of all of it for a bug
// report. Nothing is sent anywhere: the player attaches the zip to a GitHub issue.
//
//   spikeout.log, spikeout-prev.log (the run before)   in the data folder, next to settings.ini
//   crash-<time>.txt (+ crash-<time>.dmp on Windows)   when the window crashes
//   spikeout-report-<time>.zip                         Esc > Game > Report a problem
//
// The game never waits for the log: printf writes into a pipe, and a thread of its own copies it to the file (and to
// where the output went before: a console, a script's file), each line with the seconds since the start.
#pragma once

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <io.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace tools::report {

constexpr std::uintmax_t kLogLimit = 20u << 20; // (bytes: past this only the last lines are kept, for a crash report)
constexpr std::size_t kLastLines = 400;          // (kept in memory: a crash report's)

namespace detail {
// (the data folder's path is UTF-8: a user name in any language)
inline std::filesystem::path utf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
inline std::FILE* open(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(utf8(path).wstring().c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
struct Log {
    std::string folder;
    std::FILE* file = nullptr;
    std::uintmax_t written = 0;
    bool capped = false;
    int echo = -1;                 // (where the output went before, if anywhere)
    int read_end = -1;
    std::thread thread;
    std::mutex lock;               // (lines, file)
    std::deque<std::string> lines; // (the last kLastLines)
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::atomic<bool> crashed{false};
};
inline Log& log() {
    static Log* l = new Log; // (never destroyed: a crash at exit still finds it)
    return *l;
}
inline std::string stamp() { // (local time, for file names: 20261109-081500)
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &tm);
    return text;
}
inline void raw_write(int fd, const char* data, std::size_t size) {
    while (size > 0) {
#ifdef _WIN32
        const int n = _write(fd, data, static_cast<unsigned>(size));
#else
        const ssize_t n = ::write(fd, data, size);
#endif
        if (n <= 0) return;
        data += n, size -= static_cast<std::size_t>(n);
    }
}
// The thread copying the pipe: to the old output as it comes, to the file and the last lines a line at a time.
inline void pump() {
    Log& l = log();
    std::string pending;
    char buffer[4096];
    for (;;) {
#ifdef _WIN32
        const int n = _read(l.read_end, buffer, sizeof buffer);
#else
        const ssize_t n = ::read(l.read_end, buffer, sizeof buffer);
#endif
        if (n <= 0) break;
        if (l.echo >= 0) raw_write(l.echo, buffer, static_cast<std::size_t>(n));
        pending.append(buffer, static_cast<std::size_t>(n));
        std::size_t from = 0;
        for (std::size_t end; (end = pending.find('\n', from)) != std::string::npos; from = end + 1) {
            std::string line = pending.substr(from, end - from);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - l.start).count();
            char time[24];
            std::snprintf(time, sizeof time, "[%9.3f] ", s);
            std::lock_guard guard(l.lock);
            if (l.file && !l.capped) {
                std::fputs(time, l.file), std::fputs(line.c_str(), l.file), std::fputc('\n', l.file);
                std::fflush(l.file);
                l.written += line.size() + 13;
                if (l.written > kLogLimit) {
                    std::fputs("(the log reached its 20 MB: nothing more written; the last lines go in a crash report)\n", l.file);
                    std::fflush(l.file);
                    l.capped = true;
                }
            }
            l.lines.push_back(time + line);
            if (l.lines.size() > kLastLines) l.lines.pop_front();
        }
        pending.erase(0, from);
    }
}
} // namespace detail

// The data folder's log, started first thing in main: the last run's kept as spikeout-prev.log. False: no log (the
// output stays where it was).
inline bool start_log(const std::string& folder) {
    detail::Log& l = detail::log();
    l.folder = folder;
    std::error_code ec;
    const std::filesystem::path path = detail::utf8(folder + "spikeout.log");
    if (std::filesystem::exists(path, ec))
        std::filesystem::rename(path, detail::utf8(folder + "spikeout-prev.log"), ec);
#ifdef _WIN32
    l.file = _wfopen(path.wstring().c_str(), L"wb");
#else
    l.file = std::fopen(path.string().c_str(), "wb");
#endif
    if (!l.file) return false;
    std::fflush(stdout), std::fflush(stderr);
    int ends[2];
#ifdef _WIN32
    if (_pipe(ends, 1 << 20, _O_BINARY) != 0) return false;
    // (a Windows program started from Explorer has no output at all: one given, so that the pipe can take its place)
    if (_fileno(stdout) < 0) {
        std::FILE* f = nullptr;
        freopen_s(&f, "NUL", "w", stdout);
    }
    if (_fileno(stderr) < 0) {
        std::FILE* f = nullptr;
        freopen_s(&f, "NUL", "w", stderr);
    }
    const intptr_t old = _get_osfhandle(_fileno(stdout));
    if (old != -1 && old != -2 && GetFileType(reinterpret_cast<HANDLE>(old)) != FILE_TYPE_UNKNOWN) l.echo = _dup(_fileno(stdout));
    _dup2(ends[1], _fileno(stdout));
    _dup2(ends[1], _fileno(stderr));
    _close(ends[1]);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // (Windows' C library has no line buffering: each printf goes at once)
#else
    if (::pipe(ends) != 0) return false;
    if (::fcntl(1, F_GETFD) != -1) l.echo = ::dup(1);
    ::dup2(ends[1], 1);
    ::dup2(ends[1], 2);
    ::close(ends[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
#endif
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    l.read_end = ends[0];
    l.thread = std::thread(detail::pump);
    // (at the end: the pipe closed, so that the thread writes the last lines and stops)
    std::atexit([] {
        detail::Log& g = detail::log();
        std::fflush(stdout), std::fflush(stderr);
#ifdef _WIN32
        _dup2(g.echo >= 0 ? g.echo : _fileno(stdin), _fileno(stdout));
        _dup2(g.echo >= 0 ? g.echo : _fileno(stdin), _fileno(stderr));
#else
        const int none = ::open("/dev/null", O_WRONLY);
        ::dup2(g.echo >= 0 ? g.echo : none, 1);
        ::dup2(g.echo >= 0 ? g.echo : none, 2);
#endif
        if (g.thread.joinable()) g.thread.join();
        std::lock_guard guard(g.lock);
        if (g.file) std::fclose(g.file), g.file = nullptr;
    });
    return true;
}

inline std::vector<std::string> last_lines() {
    detail::Log& l = detail::log();
    std::lock_guard guard(l.lock);
    return {l.lines.begin(), l.lines.end()};
}

// ---- What this computer is (the log's first lines, and the report's system.txt) ----

inline std::string os_name() {
#ifdef _WIN32
    // (RtlGetVersion: GetVersionEx says 6.2 to programs without a manifest naming Windows 10)
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) fn(&v);
    char product[128] = "Windows";
    DWORD size = sizeof product;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", RRF_RT_REG_SZ, nullptr,
                 product, &size);
    char display[64] = "";
    size = sizeof display;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", RRF_RT_REG_SZ, nullptr,
                 display, &size);
    std::string name = product;
    // (Windows 11 still calls itself "Windows 10" there: build 22000 and later is 11)
    if (v.dwBuildNumber >= 22000 && name.rfind("Windows 10", 0) == 0) name.replace(8, 2, "11");
    return name + (display[0] ? std::string(" ") + display : std::string()) + " (" + std::to_string(v.dwMajorVersion) + "." +
           std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber) + ")";
#else
    std::string name;
#ifdef __APPLE__
    char product[64] = "";
    std::size_t size = sizeof product;
    if (sysctlbyname("kern.osproductversion", product, &size, nullptr, 0) == 0) name = std::string("macOS ") + product;
#else
    std::ifstream release("/etc/os-release");
    for (std::string line; std::getline(release, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            name = line.substr(12);
            if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);
        }
    if (std::ifstream("/.flatpak-info")) name += " (Flatpak)";
#endif
    utsname u{};
    if (uname(&u) == 0) name += std::string(name.empty() ? "" : ", ") + u.sysname + " " + u.release + " " + u.machine;
    return name;
#endif
}

inline std::string cpu_name() {
    std::string name;
#ifdef _WIN32
    char text[128] = "";
    DWORD size = sizeof text;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ,
                     nullptr, text, &size) == ERROR_SUCCESS)
        name = text;
#elif defined(__APPLE__)
    char text[128] = "";
    std::size_t size = sizeof text;
    if (sysctlbyname("machdep.cpu.brand_string", text, &size, nullptr, 0) == 0) name = text;
#else
    std::ifstream info("/proc/cpuinfo");
    for (std::string line; std::getline(info, line);)
        if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) {
            name = line.substr(line.find(':') + 2);
            break;
        }
#endif
    while (!name.empty() && name.back() == ' ') name.pop_back();
    return name + " (" + std::to_string(SDL_GetNumLogicalCPUCores()) + " threads, " + std::to_string(SDL_GetSystemRAM()) + " MB memory)";
}

inline std::string system_lines() {
    std::ostringstream o;
    const int sdl = SDL_GetVersion();
    o << "system: " << os_name() << "\n";
    o << "system: " << cpu_name() << "\n";
    o << "system: SDL " << SDL_VERSIONNUM_MAJOR(sdl) << "." << SDL_VERSIONNUM_MINOR(sdl) << "." << SDL_VERSIONNUM_MICRO(sdl)
      << ", video " << (SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none") << ", audio "
      << (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none") << "\n";
    int n = 0;
    if (SDL_DisplayID* displays = SDL_GetDisplays(&n)) {
        for (int i = 0; i < n; ++i) {
            const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(displays[i]);
            const char* name = SDL_GetDisplayName(displays[i]);
            o << "system: display " << (i + 1) << ": " << (name ? name : "?");
            if (m) o << ", " << m->w << "x" << m->h << " at " << m->refresh_rate << " Hz, scale " << m->pixel_density;
            o << "\n";
        }
        SDL_free(displays);
    }
    return o.str();
}

// ---- Controllers: each one SDL sees, as it comes and goes (an event watch: whoever reads the events) ----

inline std::string joystick_line(SDL_JoystickID id) {
    char guid[64] = "";
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    const char* name = SDL_GetJoystickNameForID(id);
    const char* path = SDL_GetJoystickPathForID(id);
    std::ostringstream o;
    char ids[64];
    std::snprintf(ids, sizeof ids, "%04x:%04x version %04x", SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id),
                  SDL_GetJoystickProductVersionForID(id));
    o << "controller " << id << ": \"" << (name ? name : "?") << "\", " << ids << ", GUID " << guid;
    const bool pad = SDL_IsGamepad(id);
    o << (pad ? ", a gamepad to SDL" : ", NOT a gamepad to SDL (no mapping: the game can't use it)");
    if (pad) {
        const char* type = SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(id));
        o << " (" << (type ? type : "standard") << ")";
    }
    if (path && *path) o << ", path " << path;
    if (char* mapping = SDL_GetGamepadMappingForID(id)) {
        o << "\n  its mapping: " << mapping;
        SDL_free(mapping);
    }
    return o.str();
}

inline std::string controllers_now() {
    std::ostringstream o;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    o << "controllers connected now: " << n << "\n";
    for (int i = 0; i < n; ++i) o << "  " << joystick_line(ids[i]) << "\n";
    SDL_free(ids);
    return o.str();
}

inline void watch_controllers() {
    SDL_AddEventWatch(
        [](void*, SDL_Event* e) {
            if (e->type == SDL_EVENT_JOYSTICK_ADDED) std::printf("%s: connected\n", joystick_line(e->jdevice.which).c_str());
            else if (e->type == SDL_EVENT_JOYSTICK_REMOVED) std::printf("controller %u: removed\n", static_cast<unsigned>(e->jdevice.which));
            else if (e->type == SDL_EVENT_GAMEPAD_ADDED) std::printf("controller %u: SDL offers it as a gamepad\n", static_cast<unsigned>(e->gdevice.which));
            return true;
        },
        nullptr);
}

// ---- Crash reports ----

namespace detail {
inline std::string& crash_folder() {
    static std::string* f = new std::string;
    return *f;
}
inline std::string& crash_version() {
    static std::string* v = new std::string;
    return *v;
}
// The crash's text file: what happened, where, then the log's last lines. Returns its name (without the folder).
inline std::string write_crash_text(const std::string& what, const std::string& where) {
    Log& l = log();
    l.crashed = true;
    std::fflush(stdout), std::fflush(stderr);
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // (the log's thread: the last lines in)
    const std::string name = "crash-" + stamp();
    std::FILE* f = open(crash_folder() + name + ".txt", "wb");
    if (!f) return name;
    std::fprintf(f, "Spikeout %s crashed: %s\n%s\n\nThe log's last lines:\n", crash_version().c_str(), what.c_str(), where.c_str());
    for (const std::string& line : l.lines) std::fprintf(f, "%s\n", line.c_str()); // (no lock: it may be held by the crash)
    std::fclose(f);
    if (std::FILE* mark = open(crash_folder() + "crashed.txt", "wb")) {
        std::fprintf(mark, "%s\n", name.c_str());
        std::fclose(mark);
    }
    return name;
}
#ifdef _WIN32
inline std::string module_at(DWORD64 address) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &m))
        return "(translated game code, or no module)";
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* file = std::strrchr(path, '\\');
    char text[MAX_PATH + 40];
    std::snprintf(text, sizeof text, "%s+0x%llx", file ? file + 1 : path,
                  static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(m)));
    return text;
}
inline LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    char what[96];
    std::snprintf(what, sizeof what, "exception 0x%08lx", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));
    // (the stack, from the crash's own registers: each frame's place as module+offset, for the release's symbols)
    std::string where = "at " + module_at(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress)) + "\nstack:";
    CONTEXT c = *info->ContextRecord;
    for (int i = 0; i < 48 && c.Rip; ++i) {
        where += "\n  " + module_at(c.Rip);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fn) { // (a leaf: its return address on top of the stack)
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
        } else {
            void* data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &data, &frame, nullptr);
        }
    }
    const std::string name = write_crash_text(what, where);
    // (and a minidump: the threads' stacks and registers, for a debugger)
    const HANDLE file = CreateFileW(utf8(crash_folder() + name + ".dmp").wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION e{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &e,
                          nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
inline void on_signal(int sig, siginfo_t* info, void*) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) {
        char what[96];
        std::snprintf(what, sizeof what, "signal %d (%s) at address %p", sig, strsignal(sig), info ? info->si_addr : nullptr);
        void* frames[64];
        const int n = backtrace(frames, 64);
        std::string where = "stack:";
        if (char** names = backtrace_symbols(frames, n)) {
            for (int i = 0; i < n; ++i) where += std::string("\n  ") + names[i];
            std::free(names);
        }
        write_crash_text(what, where);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif
} // namespace detail

// Crash reports into `folder` (the data folder) from now on.
inline void catch_crashes(const std::string& folder, const std::string& version) {
    detail::crash_folder() = folder;
    detail::crash_version() = version;
    std::set_terminate([] {
        std::string what = "an unhandled C++ exception";
        if (const std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                what += std::string(": ") + x.what();
            } catch (...) {
            }
        }
        detail::write_crash_text(what, "(std::terminate)");
        std::_Exit(3);
    });
#ifdef _WIN32
    SetUnhandledExceptionFilter(detail::on_crash);
#else
    static std::vector<char> stack(1 << 16);
    stack_t ss{};
    ss.ss_sp = stack.data();
    ss.ss_size = stack.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = detail::on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
#endif
}

// The last run crashed (its report's name; once: the mark removed).
inline std::string last_crash(const std::string& folder) {
    const std::filesystem::path mark = detail::utf8(folder + "crashed.txt");
    std::ifstream in(mark);
    std::string name;
    if (!std::getline(in, name)) return {};
    in.close();
    std::error_code ec;
    std::filesystem::remove(mark, ec);
    return name;
}

// ---- Report a problem: one zip ----

namespace detail {
inline uint32_t crc32(const std::string& data) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
// A zip of files stored as they are (no packing: a few MB of text at most), names without folders.
inline bool write_zip(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    auto u16 = [](std::string& s, unsigned v) { s.push_back(static_cast<char>(v & 0xFF)), s.push_back(static_cast<char>((v >> 8) & 0xFF)); };
    auto u32 = [&](std::string& s, uint32_t v) { u16(s, v & 0xFFFF), u16(s, v >> 16); };
    std::string directory;
    uint32_t offset = 0;
    for (const auto& [name, data] : files) {
        const uint32_t crc = crc32(data), size = static_cast<uint32_t>(data.size());
        std::string local;
        u32(local, 0x04034b50), u16(local, 20), u16(local, 0x0800), u16(local, 0), u16(local, 0), u16(local, 0x21);
        u32(local, crc), u32(local, size), u32(local, size), u16(local, static_cast<unsigned>(name.size())), u16(local, 0);
        local += name;
        out.write(local.data(), static_cast<std::streamsize>(local.size()));
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        u32(directory, 0x02014b50), u16(directory, 20), u16(directory, 20), u16(directory, 0x0800), u16(directory, 0), u16(directory, 0);
        u16(directory, 0x21), u32(directory, crc), u32(directory, size), u32(directory, size);
        u16(directory, static_cast<unsigned>(name.size())), u16(directory, 0), u16(directory, 0), u16(directory, 0), u16(directory, 0);
        u32(directory, 0), u32(directory, offset);
        directory += name;
        offset += static_cast<uint32_t>(local.size()) + size;
    }
    std::string end;
    u32(end, 0x06054b50), u16(end, 0), u16(end, 0), u16(end, static_cast<unsigned>(files.size())), u16(end, static_cast<unsigned>(files.size()));
    u32(end, static_cast<uint32_t>(directory.size())), u32(end, offset), u16(end, 0);
    out.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    out.write(end.data(), static_cast<std::streamsize>(end.size()));
    return static_cast<bool>(out);
}
inline std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
// Private things out of a text: internet addresses (IPv4, and IPv6 written with letters or "::"), the computer's user
// name (in folder paths).
inline std::string scrubbed(std::string text) {
    std::string out;
    out.reserve(text.size());
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto is_hex = [&](char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    for (std::size_t i = 0; i < text.size();) {
        const bool start = i == 0 || !(is_hex(text[i - 1]) || text[i - 1] == '.' || text[i - 1] == ':');
        if (start && (is_hex(text[i]) || text[i] == ':')) {
            std::size_t j = i;
            int dots = 0, colons = 0;
            bool letters = false, all_digits_dots = true;
            while (j < text.size() && (is_hex(text[j]) || text[j] == '.' || text[j] == ':')) {
                if (text[j] == '.') ++dots;
                else if (text[j] == ':') ++colons, all_digits_dots = false;
                else if (!is_digit(text[j])) letters = true, all_digits_dots = false;
                ++j;
            }
            const std::string token = text.substr(i, j - i);
            const bool ipv4 = all_digits_dots && dots == 3 && token.back() != '.';
            const bool ipv6 = colons >= 2 && (letters || token.find("::") != std::string::npos) && token.size() >= 6;
            if (ipv4 || ipv6) {
                out += ipv4 ? "x.x.x.x" : "x:x::x";
                i = j;
                continue;
            }
            out += token;
            i = j;
            continue;
        }
        out += text[i++];
    }
    // (the user name only where folder paths have it: Users\NAME, /home/NAME, /Users/NAME)
    for (const char* var : {"USERNAME", "USER"})
        if (const char* user = std::getenv(var); user && *user)
            for (const char* folder : {"Users\\", "Users/", "home/"}) {
                const std::string from = std::string(folder) + user, to = std::string(folder) + "<user>";
                for (std::size_t at = 0; (at = out.find(from, at)) != std::string::npos; at += to.size()) {
                    const std::size_t end = at + from.size();
                    if (end < out.size() && out[end] != '\\' && out[end] != '/' && out[end] != '"' && out[end] != ' ' &&
                        out[end] != '\n' && out[end] != '\r')
                        continue; // (a longer name that starts with it)
                    out.replace(at, from.size(), to);
                }
            }
    return out;
}
} // namespace detail

// The zip, in the data folder: the logs, the newest crash reports, settings.ini, and system.txt (`about`: the
// version, what the window knows: its GPU, and so on). Returns its path ("" if it couldn't be written).
inline std::filesystem::path make_report(const std::string& folder, const std::string& about) {
    namespace fs = std::filesystem;
    std::fflush(stdout);
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({"system.txt", detail::scrubbed(about + system_lines() + controllers_now())});
    for (const char* name : {"spikeout.log", "spikeout-prev.log", "settings.ini"}) {
        const fs::path p = detail::utf8(folder + name);
        std::error_code ec;
        if (fs::exists(p, ec)) files.push_back({name, detail::scrubbed(detail::read_all(p))});
    }
    std::vector<fs::path> crashes; // (the newest two crashes' files)
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(detail::utf8(folder), ec))
        if (f.path().filename().string().rfind("crash-", 0) == 0) crashes.push_back(f.path());
    std::sort(crashes.begin(), crashes.end());
    std::size_t texts = 0;
    for (auto it = crashes.rbegin(); it != crashes.rend() && texts < 2; ++it) {
        const bool text = it->extension() == ".txt";
        if (text) ++texts;
        files.push_back({it->filename().string(), text ? detail::scrubbed(detail::read_all(*it)) : detail::read_all(*it)});
    }
    const fs::path zip = detail::utf8(folder + "spikeout-report-" + detail::stamp() + ".zip");
    return detail::write_zip(zip, files) ? zip : fs::path();
}

} // namespace tools::report
