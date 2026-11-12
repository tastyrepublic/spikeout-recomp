// Problems noticed by the game itself (the owner's choice, 2026-11-11: "auto collect their network play data to improve
// and find bugs"), part 1: when something goes wrong in network play, what is needed to find out why is saved there and
// then, locked to the project owner's key (tools/report_lock.hpp), in the data folder's saves/:
//   problem-<time>-<kind>.zip.locked   (gamerun --unlock FILE --report-key KEY gives the zip back)
// Kinds: desync (this machine's boards differ from another's), node (a made packet differs from its real one: one board a
// PC), hang (the game stood still kHangSeconds), stall (a frame over kStallMs late), crash (the run before crashed).
// In the zip: problem.txt (what, when, the counts), the log's last lines, the newest network game's recording
// (saves/net-game-*.inputs and .state), and for a hang every thread's stack (hang-stacks.txt; Windows: hang.dmp too).
// Report a problem takes the newest few in (tools::report::make_report); part 2 sends them by itself if the player
// allows it.
//
// Never on the game thread's time: note() only queues (a lock and a push); a thread of its own packs, locks and writes.
// At most one a kind every kSpacing seconds, kMaxRun a run; the newest kKeep kept.
#pragma once

#include "player_report.hpp"
#include "report_lock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <execinfo.h>
#include <pthread.h>
#endif

namespace tools::problems {

constexpr int kHangSeconds = 10;
constexpr int64_t kStallMs = 1000;
constexpr int kSpacing = 60;
constexpr int kMaxRun = 10;
constexpr std::size_t kKeep = 10;

// What problem.txt says besides the kind: a line each.
struct Note {
    std::string kind, detail;
    uint64_t frame = 0;
    std::string stacks;                  // (a hang: every thread's, or the stuck one's)
    std::vector<uint8_t> dump;           // (a hang on Windows: a minidump)
    std::vector<std::pair<std::string, std::string>> files; // (more, already read: a crash's report)
};

class Saver {
public:
    // `folder`: the data folder (with its slash); `about`: the version and what the window knows; `counts`: the game's
    // counts now (rollbacks, delay, ...), asked for on the saving thread.
    void start(const std::string& folder, std::string about, std::function<std::string()> counts) {
        folder_ = folder, about_ = std::move(about), counts_ = std::move(counts);
        if (!lock::owner_key()) return; // (no key built in: nothing could be locked, so nothing is kept)
        thread_ = std::thread([this] { run(); });
    }
    ~Saver() {
        {
            std::lock_guard guard(lock_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    // From any thread, cheap: a problem to save (dropped if one of its kind was saved within kSpacing seconds).
    void note(Note n) {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard guard(lock_);
            if (!thread_.joinable() || saved_ >= kMaxRun) return;
            auto& last = last_[n.kind];
            if (last.time_since_epoch().count() && now - last < std::chrono::seconds(kSpacing)) return;
            last = now, ++saved_;
            queue_.push_back(std::move(n));
        }
        wake_.notify_all();
    }
    void note(const std::string& kind, const std::string& detail, uint64_t frame = 0) { note(Note{kind, detail, frame, {}, {}, {}}); }
    // How many of each kind were noticed this run (the stats line, part 2).
    std::map<std::string, int> counts_by_kind() const {
        std::lock_guard guard(lock_);
        return seen_;
    }

private:
    void run() {
        for (;;) {
            Note n;
            {
                std::unique_lock guard(lock_);
                wake_.wait(guard, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) return;
                n = std::move(queue_.front());
                queue_.pop_front();
                ++seen_[n.kind];
            }
            save(n);
        }
    }
    void save(const Note& n) {
        namespace fs = std::filesystem;
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // (the log's thread: the lines up to now in)
        std::string text = "Spikeout problem: " + n.kind + "\n" + n.detail + "\nframe " + std::to_string(n.frame) + "\nat " +
                           report::detail::clock_text() + "\n\n" + about_;
        if (counts_) text += "\n" + counts_();
        std::vector<std::pair<std::string, std::string>> files;
        files.push_back({"problem.txt", report::detail::scrubbed(text)});
        std::string lines;
        for (const std::string& line : report::last_lines()) lines += line + "\n";
        files.push_back({"log-last-lines.txt", report::detail::scrubbed(lines)});
        if (!n.stacks.empty()) files.push_back({"hang-stacks.txt", n.stacks});
        if (!n.dump.empty()) files.push_back({"hang.dmp", std::string(n.dump.begin(), n.dump.end())});
        for (const auto& f : n.files) files.push_back(f);
        // (the newest network game's recording: the state from its start, the inputs up to now)
        std::error_code ec;
        fs::path newest;
        for (const auto& f : fs::directory_iterator(report::detail::utf8(folder_ + "saves/"), ec)) {
            const std::string name = f.path().filename().string();
            if (name.rfind("net-game-", 0) == 0 && f.path().extension() == ".inputs" && (newest.empty() || name > newest.filename().string()))
                newest = f.path();
        }
        if (!newest.empty()) {
            fs::path state = newest;
            state.replace_extension(".state");
            for (const fs::path& p : {newest, state})
                if (fs::exists(p, ec)) files.push_back({p.filename().string(), report::detail::read_all(p)});
        }
        // (zipped, then locked whole: the recording's state holds pieces of the ROM, the dump this computer's memory)
        const std::string base = "problem-" + report::detail::stamp() + "-" + n.kind;
        const fs::path zip = report::detail::utf8(folder_ + "saves/" + base + ".zip");
        fs::create_directories(zip.parent_path(), ec);
        if (!report::detail::write_zip(zip, files)) return;
        const std::string bytes = report::detail::read_all(zip);
        fs::remove(zip, ec);
        const auto owner = lock::owner_key();
        const std::vector<uint8_t> locked = owner ? lock::seal(std::vector<uint8_t>(bytes.begin(), bytes.end()), *owner) : std::vector<uint8_t>{};
        if (locked.empty()) return;
        const std::string path = folder_ + "saves/" + base + ".zip.locked";
        if (std::FILE* f = report::detail::open(path, "wb")) {
            std::fwrite(locked.data(), 1, locked.size(), f);
            std::fclose(f);
            std::printf("problem: %s saved (%s; %zu KB, locked: Report a problem takes it in)\n", base.c_str(), n.detail.c_str(),
                        locked.size() / 1024);
        }
        prune();
    }
    void prune() { // (the newest kKeep)
        namespace fs = std::filesystem;
        std::vector<fs::path> all;
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(report::detail::utf8(folder_ + "saves/"), ec))
            if (f.path().filename().string().rfind("problem-", 0) == 0) all.push_back(f.path());
        std::sort(all.begin(), all.end());
        while (all.size() > kKeep) fs::remove(all.front(), ec), all.erase(all.begin());
    }

    std::string folder_, about_;
    std::function<std::string()> counts_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Note> queue_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_;
    std::map<std::string, int> seen_;
    int saved_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

// The newest problem bundles in the data folder's saves/ (Report a problem's zip takes these in).
inline std::vector<std::filesystem::path> newest(const std::string& folder, std::size_t most) {
    namespace fs = std::filesystem;
    std::vector<fs::path> all;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(report::detail::utf8(folder + "saves/"), ec)) {
        const std::string name = f.path().filename().string();
        if (name.rfind("problem-", 0) == 0 && name.size() > 11 && name.substr(name.size() - 11) == ".zip.locked") all.push_back(f.path());
    }
    std::sort(all.rbegin(), all.rend());
    if (all.size() > most) all.resize(most);
    return all;
}

// ---- A hang: the game standing still ----
// The game thread beats (each frame it finishes); a watchdog thread looks once a second while `armed` (a network game
// going on), and when no beat came for kHangSeconds, takes the game thread's stack (and on Windows a minidump of every
// thread) and notes a hang. Once per hang: armed again when it beats again.
class Watchdog {
public:
    // On the game thread, each frame it finishes: a beat, and this thread the one to look at (the shown board's: it
    // changes when another board is watched).
    void beat_here() {
        if (std::this_thread::get_id() != watched_id_.load()) watch_this_thread();
        beat();
    }
    void watch_this_thread() {
        std::lock_guard guard(thread_lock_);
        watched_id_ = std::this_thread::get_id();
#ifdef _WIN32
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        if (thread_) CloseHandle(thread_);
        thread_ = h;
#else
        thread_ = pthread_self();
        have_thread_ = true;
        static std::once_flag handler;
        std::call_once(handler, [] {
        struct sigaction sa {};
        sa.sa_handler = [](int) { // (on the stuck thread: its stack's return addresses, read by the watchdog)
            caught().n = backtrace(caught().frames, 64);
            caught().done.store(true);
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR2, &sa, nullptr);
        });
#endif
    }
    void beat() { beats_.fetch_add(1, std::memory_order_relaxed); }
    void arm(bool on) { armed_ = on; }
    void start(Saver& saver, std::function<uint64_t()> frame) {
        saver_ = &saver, frame_ = std::move(frame);
        thread_run_ = std::thread([this] { run(); });
    }
    ~Watchdog() {
        stop_ = true;
        if (thread_run_.joinable()) thread_run_.join();
#ifdef _WIN32
        if (thread_) CloseHandle(thread_);
#endif
    }

private:
#ifndef _WIN32
    struct Caught {
        void* frames[64];
        int n = 0;
        std::atomic<bool> done{false};
    };
    static Caught& caught() {
        static Caught c;
        return c;
    }
#endif
    void run() {
        uint64_t last = beats_.load();
        int still = 0;
        bool told = false;
        while (!stop_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t now = beats_.load();
            if (now != last || !armed_) {
                last = now, still = 0, told = false;
                continue;
            }
            if (++still < kHangSeconds || told) continue;
            told = true;
            Note n;
            n.kind = "hang";
            n.detail = "the game stood still " + std::to_string(still) + " s (no frame finished)";
            n.frame = frame_ ? frame_() : 0;
            n.stacks = stuck_stack();
#ifdef _WIN32
            n.dump = minidump();
#endif
            std::printf("problem: the game has stood still %d s\n", still);
            saver_->note(std::move(n));
        }
    }
#ifdef _WIN32
    std::string stuck_stack() {
        std::lock_guard guard(thread_lock_);
        if (!thread_) return "(the game thread not known)";
        if (SuspendThread(thread_) == static_cast<DWORD>(-1)) return "(the game thread couldn't be stopped to look)";
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        std::string where = "the game thread's stack:";
        if (GetThreadContext(thread_, &c))
            for (int i = 0; i < 48 && c.Rip; ++i) { // (as the crash report walks it)
                where += "\n  " + report::detail::module_at(c.Rip);
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                if (!fn) {
                    c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                    c.Rsp += 8;
                } else {
                    void* data = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &data, &frame, nullptr);
                }
            }
        ResumeThread(thread_);
        return where;
    }
    static std::vector<uint8_t> minidump() { // (every thread's stack: through a temporary file)
        wchar_t dir[MAX_PATH], name[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"spk", 0, name)) return {};
        std::vector<uint8_t> out;
        const HANDLE file = CreateFileW(name, GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo, nullptr, nullptr, nullptr)) {
                const DWORD size = GetFileSize(file, nullptr);
                if (size != INVALID_FILE_SIZE && size < (64u << 20)) {
                    out.resize(size);
                    DWORD got = 0;
                    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
                    if (!ReadFile(file, out.data(), size, &got, nullptr) || got != size) out.clear();
                }
            }
            CloseHandle(file);
        }
        DeleteFileW(name);
        return out;
    }
    HANDLE thread_ = nullptr;
#else
    std::string stuck_stack() {
        std::lock_guard guard(thread_lock_);
        if (!have_thread_) return "(the game thread not known)";
        caught().done = false;
        if (pthread_kill(thread_, SIGUSR2) != 0) return "(the game thread couldn't be asked)";
        for (int i = 0; i < 100 && !caught().done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!caught().done.load()) return "(the game thread didn't answer: stuck where signals wait)";
        std::string where = "the game thread's stack:";
        if (char** names = backtrace_symbols(caught().frames, caught().n)) {
            for (int i = 0; i < caught().n; ++i) where += std::string("\n  ") + names[i];
            std::free(names);
        }
        return where;
    }
    pthread_t thread_{};
    bool have_thread_ = false;
#endif
    std::mutex thread_lock_;
    std::atomic<std::thread::id> watched_id_{};
    std::atomic<uint64_t> beats_{0};
    std::atomic<bool> armed_{false}, stop_{false};
    Saver* saver_ = nullptr;
    std::function<uint64_t()> frame_;
    std::thread thread_run_;
};

// ---- Part 2: sent by the game itself, when the player allows it (settings.ini send_reports = 1) ----
// After each network game its numbers (a line of JSON, stats()) wait in saves/outbox/; send() takes them and the problem
// bundles not sent yet (saves/sent.txt lists those sent) to the lobby (net::report::upload), on a thread of its own, only
// between games (never while one runs: the game waits on nothing, the line holds only the game's datagrams).

// A string in JSON.
inline std::string json_text(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') out += '\\', out += ch;
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", c);
            out += u;
        } else out += ch;
    }
    return out + "\"";
}

// A game's numbers kept to send: `json` an object's text (no addresses, no names: what the game measured).
inline void keep_stats(const std::string& folder, const std::string& json) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(report::detail::utf8(folder + "saves/outbox/"), ec);
    const std::string path = folder + "saves/outbox/stats-" + report::detail::stamp() + ".json";
    if (std::FILE* f = report::detail::open(path, "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    }
    // (no more than 50 waiting: the oldest go)
    std::vector<fs::path> all;
    for (const auto& e : fs::directory_iterator(report::detail::utf8(folder + "saves/outbox/"), ec)) all.push_back(e.path());
    std::sort(all.begin(), all.end());
    while (all.size() > 50) fs::remove(all.front(), ec), all.erase(all.begin());
}

// A game's inputs kept to send (the owner, 2026-11-11: real players' presses for smarter rollback guesses,
// tools/guess-study.cpp): `inputs_path` a recording's NAME.inputs (tools/net_record.hpp: every seat's buttons, frame by
// frame; no state, no names), locked to the owner's key, waiting in saves/outbox/ as inputs-<time>.locked. Only what
// its seats pressed: a few MB at most (4 bytes a seat a frame).
inline void keep_inputs(const std::string& folder, const std::string& inputs_path) {
    namespace fs = std::filesystem;
    const auto owner = lock::owner_key();
    if (!owner) return;
    const std::string bytes = report::detail::read_all(report::detail::utf8(inputs_path));
    if (bytes.size() < 32 || bytes.size() > (8u << 20)) return;
    const std::vector<uint8_t> locked = lock::seal(std::vector<uint8_t>(bytes.begin(), bytes.end()), *owner);
    if (locked.empty()) return;
    std::error_code ec;
    fs::create_directories(report::detail::utf8(folder + "saves/outbox/"), ec);
    const std::string name = fs::path(report::detail::utf8(inputs_path)).stem().string(); // (net-game-<time>)
    if (std::FILE* f = report::detail::open(folder + "saves/outbox/inputs-" + name + ".locked", "wb")) {
        std::fwrite(locked.data(), 1, locked.size(), f);
        std::fclose(f);
    }
}

class Sender {
public:
    // Sends what waits (`upload`: net::report::upload, or a test's), unless sending already; returns at once. `may_go`
    // asked before each file: false (a network game started meanwhile), the rest waits for the next time.
    void send(const std::string& folder, std::function<bool(const std::string& what, const std::string& body, const std::string& type)> upload,
              std::function<bool()> may_go = {}) {
        if (busy_.exchange(true)) return;
        if (thread_.joinable()) thread_.join();
        thread_ = std::thread([this, folder, upload = std::move(upload), may_go = std::move(may_go)] {
            run(folder, upload, may_go);
            busy_ = false;
        });
    }
    ~Sender() {
        if (thread_.joinable()) thread_.join();
    }

private:
    static void run(const std::string& folder, const std::function<bool(const std::string&, const std::string&, const std::string&)>& upload,
                    const std::function<bool()>& may_go) {
        auto go = [&] { return !may_go || may_go(); };
        namespace fs = std::filesystem;
        std::error_code ec;
        int stats = 0, inputs = 0, bundles = 0;
        std::vector<fs::path> waiting;
        for (const auto& e : fs::directory_iterator(report::detail::utf8(folder + "saves/outbox/"), ec)) waiting.push_back(e.path());
        std::sort(waiting.begin(), waiting.end());
        for (const fs::path& p : waiting) { // (stats-*.json a game's numbers, inputs-*.locked its seats' presses)
            if (!go()) return;
            const bool presses = p.filename().string().rfind("inputs-", 0) == 0;
            if (!upload(presses ? "inputs" : "stats", report::detail::read_all(p), presses ? "application/octet-stream" : "application/json"))
                return; // (no connection: next time)
            fs::remove(p, ec), ++(presses ? inputs : stats);
        }
        const std::string list = folder + "saves/sent.txt";
        std::string sent = report::detail::read_all(report::detail::utf8(list));
        for (const fs::path& p : newest(folder, kKeep)) {
            const std::string name = p.filename().string();
            if (sent.find(name + "\n") != std::string::npos) continue;
            if (!go()) break;
            const std::string bytes = report::detail::read_all(p);
            if (bytes.empty() || bytes.size() > (24u << 20)) continue;
            if (!upload("problem", bytes, "application/octet-stream")) break;
            sent += name + "\n", ++bundles;
            if (std::FILE* f = report::detail::open(list, "ab")) {
                std::fprintf(f, "%s\n", name.c_str());
                std::fclose(f);
            }
        }
        if (stats || inputs || bundles)
            std::printf("reports: sent %d games' numbers, %d games' inputs and %d problem bundles to the project\n", stats, inputs, bundles);
    }
    std::atomic<bool> busy_{false};
    std::thread thread_;
};

} // namespace tools::problems
