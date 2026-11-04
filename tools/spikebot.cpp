// spikebot: plays Spikeout through the recompiled game, headless, so that
// what needs reaching (a stage, a scene, a bug) can be reached without a
// person playing.
//
//   spikebot <rom_dir> --load-state FILE [--out DIR] [--frames N] [--no-sound]
//            [--play] [--workers N] [--inspect EVERY] [--dump LO-HI]
//            [--route D:N,...] [--explore-test] [--moves-test] [--floor-map] [--weak-enemies]
//            [--bench] [--selftest] [--record-inputs FILE]
//
// The game runs on its own thread and stops at the end of every frame for
// the bot's next command (Emulator below): run some frames with some inputs
// held, keep a save state in memory or go back to one, write a screenshot.
// So the bot is written as plain code, and it can try things and take them
// back: a save state taken at a frame and loaded again runs on exactly as
// before (tools::board_state). Worker boards try things in parallel.
//
// --play plays (docs/bot.md: fighting, walking, the gate, continues, finding
// a way round when stuck). --out DIR gets screenshots and, every 300 frames,
// save states the window can load (spikeout --load-state).
// For looking into the game: --inspect EVERY runs with no input and prints
// the player and the character slots every EVERY frames; --dump LO-HI prints
// RAM; --route D:N,... walks direction D (0 up, 2 right, 4 down, 6 left, 8
// none) for N frames each, printing where the player goes; --explore-test
// runs the search for a way to the enemies once and walks it; --selftest
// checks that a save state run on twice (here and on a worker) ends the same.
#include "game_setup.hpp"
#include "input_log.hpp"
#include "png_write.hpp"

#include "games/games.hpp"
#include "runtime/commboard.hpp"
#include "runtime/real3d_render.hpp"
#include "runtime/tilegen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- What the bot reads of the game (found by searching RAM across save
// states; see docs/bot.md) ----
namespace ram {
// Character objects, 0x4000 bytes each: players 1-4 from 0x1A0000, then the
// enemies from 0x1B0000.
constexpr uint32_t kPlayer = 0x1A0000, kEnemies = 0x1B0000, kSlot = 0x4000;
constexpr int kEnemySlots = 16;
// Byte +0: nonzero (1, 3, 9 seen) while the character is in play; 0 once
// knocked out, and for the slots the last area left (those who ran off).
constexpr uint32_t kLive = 0x00;
constexpr uint32_t kX = 0x10, kY = 0x14, kZ = 0x18; // position (floats; y is up)
constexpr uint32_t kHealth = 0x3F02, kMaxHealth = 0x3F04; // 16-bit
// State flags (they change all the time: no use telling fighters from
// bystanders; the game's own "break the gate" says the fight is over).
constexpr uint32_t kKind = 0x44;
// Frames since the fight in this area began (the timer shows 99 - this / 64).
constexpr uint32_t kAreaFrames = 0x11BD22;
// The red arrow ("break the gate"): from the player (+0) to where it points
// (+0xC, x y z floats), kept after the gate is broken. (0x111C84 was taken
// for "break the gate" in stage 1, but it stayed 0 at the coast road's car
// park and was 1 in the opera house's hall, a fight: not that.)
constexpr uint32_t kArrow = 0x11DF48, kArrowTarget = 0x11DF54;
// The area's phase (32-bit): 0 fighting, 1 the boss come ("HERE COMES THE
// BOSS"), 2 the gate to break, 24 the boss down (those left run off: not to be chased), 64 the
// story scene between stages (and on into stage 3's first fight);
// kAreaFrames starts again at each.
constexpr uint32_t kAreaPhase = 0x11BD24;
// 1 while a game is being played, 0 in the attract mode (after a game over:
// the shipyard's gate and the opera house's hall timed out). By diffing play
// states from three stages against three attract-mode ones.
constexpr uint32_t kPlaying = 0x110000;
// Bits, not a count: Diesel Town's boss fight is 0x401 (the boss, and 0x400:
// not known). Seen alone: 4 (an area's start, a few frames), 8 (the gate
// broken, walking on), 32 (just after a gate, before 8).
constexpr uint32_t kPhaseBoss = 1, kPhaseGate = 2;
// (the area's fight over: gate, boss down. Not 64: on through the story scene
// after a stage, but also through stage 3's first fight)
constexpr uint32_t kPhaseOver = 2 | 8 | 16 | 32;
} // namespace ram

// Inputs, active low (hardware-notes.md, Inputs).
struct Inputs {
    uint8_t bank0 = 0xFF, controls = 0xFF;
};
enum Button : int { Beat = 0, Charge, Shift, Jump, Down, Up, Right, Left };
constexpr uint8_t kCoin = 0x01, kStart = 0x10; // bank 0
Inputs press(std::initializer_list<Button> buttons) {
    Inputs in;
    for (Button b : buttons) in.controls = static_cast<uint8_t>(in.controls & ~(1u << b));
    return in;
}

// A state kept in memory (a board's save state, or linked boards': the file
// gamerun --boards N --load-state takes).
struct Snapshot {
    std::vector<uint8_t> data;
    uint64_t frame = 0;
    std::size_t writes = 0; // (--record-inputs: the writes recorded after its frame by then)
    std::size_t changes = 0; // (--record-inputs, linked boards: the changes recorded by then)
};

// The game as the bot drives it: one board (Emulator), or linked boards (Rig,
// --boards N: each player's own board, the one controlled set by control()).
// Reading and stepping are the controlled player's board; writes go to every
// board (the game's state is each cabinet's, kept the same).
class Game {
public:
    virtual ~Game() = default;
    virtual bool start_from(std::vector<uint8_t> data, const std::string& name = "a save state") = 0;
    bool start(const std::string& path) { return start_from(tools::read_state_file(tools::read_file(path)).state, path); }
    // Runs `frames` frames with `in` held by the controlled player; screenshot: the last one's picture.
    virtual void step(Inputs in, int frames = 1, const std::string& screenshot = {}) = 0;
    virtual std::shared_ptr<Snapshot> save() = 0;
    virtual void load(const std::shared_ptr<Snapshot>& snap) = 0;
    bool write_state(const Snapshot& snap, const std::string& path) { return tools::write_file(path, snap.data); }
    virtual uint16_t read16(uint32_t addr) const = 0;
    // Board `board`'s RAM, whichever player is controlled (linked: the red arrow is kept by
    // board 0, the master, alone: board 1's was all zero at stage 1's gate).
    virtual uint16_t read16_on(std::size_t /*board*/, uint32_t addr) const { return read16(addr); }
    float readf_on(std::size_t board, uint32_t addr) const {
        const uint32_t v = static_cast<uint32_t>(read16_on(board, addr)) << 16 | read16_on(board, addr + 2);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    float readf(uint32_t addr) const {
        const uint32_t v = read32(addr);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    uint32_t read32(uint32_t addr) const { return static_cast<uint32_t>(read16(addr)) << 16 | read16(addr + 2); }
    virtual void write16(uint32_t addr, uint16_t v) = 0;
    // One board's RAM only (linked boards each keep their own copy of an enemy).
    virtual void write16_on(std::size_t board, uint32_t addr, uint16_t v) {
        (void)board;
        write16(addr, v);
    }
    virtual uint32_t r13() const = 0; // (the game's small-data base: its globals are off it)
    virtual uint64_t frame() const = 0;
    virtual const std::string& stopped() const = 0;
    // Linked boards: how many, the player controlled (its board's inputs and RAM), and what the
    // others hold meanwhile, frame by frame (by the frame's number: tries do the same).
    virtual std::size_t boards() const { return 1; }
    virtual void control(std::size_t /*player*/) {}
    virtual std::size_t player() const { return 0; }
    using Held = std::function<Inputs(uint64_t frame)>;
    virtual void hold(std::size_t /*board*/, Held /*inputs*/) {}
    virtual Held held(std::size_t /*board*/) const { return {}; }
};

// One board, run on its own thread one frame at a time for the bot.
class Emulator : public Game {
public:
    Emulator(const m3::RomSet& roms, std::span<const recomp::FunctionEntry> functions,
             std::span<const recomp::FunctionEntry> blocks, bool with_sound)
        : bus_(roms), c_(bus_, functions), sound_(roms), with_sound_(with_sound),
          screen_(m3::TilegenRenderer::kWidth * m3::TilegenRenderer::kHeight) {
        bus_.set_security_key(m3::games::kSpikeoutFinalEdition.security_key);
        bus_.use_netboard(std::make_unique<m3::CommBoard>(nullptr, m3::CommBoardOptions{}));
        c_.set_scheduler(&timing_);
        c_.set_blocks(blocks);
        m3::games::install_spikeout_hooks(c_);
        timing_.on_display_line = [this](uint32_t line) {
            if (!draw_2d_) return;
            if (line == 0) tilegen_.begin_frame();
            tilegen_.draw_line(bus_, static_cast<int>(line));
        };
        timing_.on_frame_end = [this] { frame_end(); };
    }
    ~Emulator() {
        if (thread_.joinable()) {
            command([](Emulator&) { throw Quit{}; });
            thread_.join();
        }
    }

    // Starts from a save state (the window's F9 or gamerun --save-state).
    bool start_from(std::vector<uint8_t> data, const std::string& name = "a save state") override {
        try {
            m3::State s(std::move(data));
            tools::board_state(s, c_, timing_, sound_fn());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "spikebot: %s: %s\n", name.c_str(), e.what());
            return false;
        }
        thread_ = std::thread([this] { run(); });
        wait_idle();
        return true;
    }

    // ---- Commands (the bot's thread) ----
    // Runs `frames` frames with `in` held; screenshot: the last one's picture.
    // The frames go in one command (the game's thread stops only after the
    // last), but the one with a picture, which draws the 2D as it goes.
    void step(Inputs in, int frames = 1, const std::string& screenshot = {}) override {
        const int plain = screenshot.empty() ? frames : frames - 1;
        auto run = [this, in](int n, const std::string& shot) {
            command([in, n, shot](Emulator& e) {
                e.bus_.inputs.bank0 = in.bank0;
                e.bus_.inputs.controls = in.controls;
                e.shot_ = shot;
                e.draw_2d_ = !shot.empty();
                e.frames_left_ = n;
                e.go_ = true;
            });
        };
        if (plain > 0) run(plain, {});
        if (plain < frames) run(1, screenshot);
    }
    // The state now, kept in memory: the game runs on to the first block it
    // can resume at (outside any exception: Context::request_snapshot), takes
    // it there and waits; the next step's inputs are set there, as they are
    // when the state is loaded again, so both go on the same way.
    std::shared_ptr<Snapshot> save() override {
        auto snap = std::make_shared<Snapshot>();
        command([snap](Emulator& e) {
            e.c_.request_snapshot([&e, snap](uint32_t) {
                m3::State s(e.state_size_); // (room for it: no growing)
                tools::board_state(s, e.c_, e.timing_, e.sound_fn());
                snap->data = std::move(s.data());
                e.state_size_ = snap->data.size();
                snap->frame = e.timing_.frames();
                if (e.log_) snap->writes = e.log_->frames[snap->frame].writes.size();
                e.serve(); // (paused here until the next step)
            });
            e.go_ = true;
        });
        return snap;
    }
    // Goes back to `snap`; the next step's inputs replace the ones it held.
    void load(const std::shared_ptr<Snapshot>& snap) override {
        if (!snap || snap->data.empty()) return;
        command([snap](Emulator&) { throw Rewind{snap}; });
    }

    // Reading the game's RAM between frames (big-endian, as the CPU sees it).
    uint16_t read16(uint32_t addr) const override { return static_cast<uint16_t>(bus_.ram()[addr] << 8 | bus_.ram()[addr + 1]); }
    // Writing it (between frames, the game's thread waiting: as reading).
    void write16(uint32_t addr, uint16_t v) override {
        bus_.ram()[addr] = static_cast<uint8_t>(v >> 8);
        bus_.ram()[addr + 1] = static_cast<uint8_t>(v);
        if (log_) log_->frames[timing_.frames()].writes.emplace_back(addr, v);
    }
    // --record-inputs: what this board runs with, frame by frame (tools::InputLog).
    void record_into(tools::InputLog* log) { log_ = log; }
    uint32_t r13() const override { return c_.cpu.gpr[13]; }
    uint64_t frame() const override { return timing_.frames(); }
    const std::string& stopped() const override { return stopped_; }

private:
    struct Quit {};
    struct Rewind {
        std::shared_ptr<Snapshot> snap;
    };

    std::function<void(m3::State&)> sound_fn() {
        if (!with_sound_) return {};
        return [this](m3::State& s) { sound_.state(s); };
    }

    // The game's thread: runs the board; at each frame's end (and after
    // going back to a save state) waits for commands until one says go.
    void run() {
        bool resume = true; // (from a loaded state: the snapshot's block)
        for (;;) {
            try {
                if (resume) {
                    serve();
                    c_.resume_block(c_.cpu.pc);
                }
                c_.call(c_.cpu.pc);
                stopped_ = "the reset code returned";
                break;
            } catch (const Rewind& r) {
                m3::State s(std::span<const uint8_t>(r.snap->data)); // (not copied)
                tools::board_state(s, c_, timing_, sound_fn());
                if (log_) log_->forget_after(timing_.frames(), r.snap->writes);
                resume = true;
            } catch (const Quit&) {
                break;
            } catch (const std::exception& e) {
                stopped_ = e.what();
                break;
            }
        }
        std::lock_guard lock(mutex_);
        idle_ = true;
        done_ = true;
        changed_.notify_all();
    }

    void frame_end() {
        if (with_sound_) sound_.frame(bus_.midi_out(), timing_.frame_start(timing_.frames() - 1), timing_.frame_cycles());
        else bus_.midi_out().clear();
        if (log_) { // (the inputs this frame ran with; main RAM's hash at its end now and then)
            tools::InputLog::Frame& f = log_->frames[timing_.frames()];
            f.bank0 = bus_.inputs.bank0, f.controls = bus_.inputs.controls, f.writes.clear();
            f.hash = timing_.frames() % tools::InputLog::kHashEvery == 0 ? tools::Recording::ram_hash(bus_.ram()) : 0;
        }
        if (!shot_.empty()) {
            real3d_.render(bus_.real3d());
            tilegen_.composite(screen_, real3d_.image());
            tools::write_png(shot_, screen_, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
            shot_.clear();
        }
        if (--frames_left_ > 0) return; // (more frames of the same step)
        serve();
    }

    // Runs commands until one lets the game go on (or throws to go back).
    void serve() {
        go_ = false;
        while (!go_) {
            std::function<void(Emulator&)> next;
            {
                std::unique_lock lock(mutex_);
                idle_ = true;
                changed_.notify_all();
                changed_.wait(lock, [this] { return static_cast<bool>(command_); });
                next = std::move(command_);
                command_ = nullptr;
                idle_ = false;
            }
            next(*this);
        }
    }

    // The bot's thread: hands a command over and waits until the game's
    // thread is waiting again (the command done, or the frame run).
    void command(std::function<void(Emulator&)> fn) {
        {
            std::unique_lock lock(mutex_);
            if (done_) return;
            command_ = std::move(fn);
            idle_ = false;
            changed_.notify_all();
        }
        wait_idle();
    }
    void wait_idle() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return (idle_ && !command_) || done_; });
    }

    m3::Bus bus_;
    recomp::Context c_;
    recomp::FrameTiming timing_;
    tools::FrameSound sound_;
    bool with_sound_;
    m3::TilegenRenderer tilegen_;
    m3::Real3dRenderer real3d_;
    std::vector<uint32_t> screen_;
    bool draw_2d_ = false, go_ = false;
    int frames_left_ = 0; // (of the step being run)
    std::size_t state_size_ = 0; // (the last one's: room made for the next)
    std::string shot_, stopped_;
    tools::InputLog* log_ = nullptr; // --record-inputs

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::function<void(Emulator&)> command_;
    bool idle_ = false, done_ = false;
};

// Linked boards (--boards N, a player on each), as gamerun --boards N
// --parallel runs them: each on its own thread, all at once, meeting every
// kSlice lines (tools::Turns), the link's packets moved on at the meeting
// (m3::LinkRing, deferred, its latency a slice): the same every run. The bot
// stops them at a meeting: a step's frames run, the first meeting after the
// last one ends. A save runs one frame more and keeps every board's state at
// its own point after that frame's end (the first block it can resume at)
// with the ring's part, the packets on their way between those points: a
// linked save state, gamerun's format (gamerun --boards N --load-state runs
// on from it). As gamerun --rollback-test goes back, loading puts every board
// back there with the others stopped at the meeting; each goes on from its
// own point as it leaves. A step after a save loads it first, as each try from
// it does, so the run and its tries go on alike.
class Rig : public Game {
public:
    static constexpr uint32_t kSlice = 8; // (lines between meetings: gamerun's default)

    Rig(std::size_t n, const m3::RomSet& roms, std::span<const recomp::FunctionEntry> functions,
        std::span<const recomp::FunctionEntry> blocks)
        : ring_(n), turns_(n, true, [this] { between(); }), held_(n) {
        const uint64_t slice_cycles =
            (recomp::FrameTiming().frame_cycles() * kSlice + recomp::FrameTiming::kLines - 1) / recomp::FrameTiming::kLines;
        ring_.set_deferred(true);
        ring_.set_latency(slice_cycles);
        for (std::size_t i = 0; i < n; ++i) {
            boards_.push_back(std::make_unique<Board>(roms, functions));
            Board& b = *boards_.back();
            b.bus.set_security_key(m3::games::kSpikeoutFinalEdition.security_key);
            b.bus.use_netboard(std::make_unique<m3::CommBoard>(ring_.port(i), m3::CommBoardOptions{}));
            b.c.set_scheduler(&b.timing);
            b.c.set_blocks(blocks);
            m3::games::install_spikeout_hooks(b.c);
            b.timing.on_display_line = [this, i](uint32_t line) { display_line(i, line); };
            b.timing.on_frame_end = [this, i] { frame_end(i); };
        }
    }
    ~Rig() override {
        {
            std::lock_guard lock(mutex_);
            quit_ = true, go_ = true;
        }
        changed_.notify_all();
        for (auto& b : boards_)
            if (b->thread.joinable()) b->thread.join();
    }

    // From linked boards' state (gamerun --boards N --save-state, the window's F9 in network play).
    bool start_from(std::vector<uint8_t> data, const std::string& name = "a save state") override {
        auto snap = std::make_shared<Snapshot>();
        snap->data = std::move(data);
        try {
            put_back(*snap);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "spikebot: %s: %s (a state of %zu linked boards?)\n", name.c_str(), e.what(), boards_.size());
            return false;
        }
        snap->frame = frame_ = boards_[0]->timing.frames();
        saved_ = snap;
        for (std::size_t i = 0; i < boards_.size(); ++i) boards_[i]->thread = std::thread([this, i] { run(i); });
        return true;
    }

    void step(Inputs in, int frames = 1, const std::string& screenshot = {}) override {
        if (!stopped_.empty()) return;
        if (behind_saved_) put_back(*saved_); // (as a try from the save: from every board's point)
        behind_saved_ = false;
        held_[me_] = [in](uint64_t) { return in; };
        set_inputs(frame_ + 1);
        run_to(frame_ + static_cast<uint64_t>(std::max(frames, 1)), false, screenshot);
    }
    std::shared_ptr<Snapshot> save() override {
        if (!stopped_.empty()) return saved_;
        if (behind_saved_) put_back(*saved_);
        set_inputs(frame_ + 1);
        run_to(frame_ + 1, true, {});
        behind_saved_ = true; // (the boards ran on to the meeting after it)
        return saved_;
    }
    void load(const std::shared_ptr<Snapshot>& snap) override {
        if (!snap || snap->data.empty() || !stopped_.empty()) return;
        put_back(*snap);
        saved_ = snap;
        frame_ = snap->frame;
        behind_saved_ = false;
    }

    uint16_t read16(uint32_t addr) const override { return read16_on(me_, addr); }
    uint16_t read16_on(std::size_t board, uint32_t addr) const override {
        const auto ram = boards_[std::min(board, boards_.size() - 1)]->bus.ram();
        return static_cast<uint16_t>(ram[addr] << 8 | ram[addr + 1]);
    }
    void write16(uint32_t addr, uint16_t v) override {
        for (std::size_t i = 0; i < boards_.size(); ++i) write16_on(i, addr, v);
    }
    void write16_on(std::size_t board, uint32_t addr, uint16_t v) override {
        board = std::min(board, boards_.size() - 1);
        auto ram = boards_[board]->bus.ram();
        ram[addr] = static_cast<uint8_t>(v >> 8);
        ram[addr + 1] = static_cast<uint8_t>(v);
        record_write(board, addr, v);
    }
    // --record-inputs: what the boards run with and where (tools::LinkedInputLog).
    void record_into(tools::LinkedInputLog* log) { log_ = log; }
    uint32_t r13() const override { return boards_[me_]->c.cpu.gpr[13]; }
    uint64_t frame() const override { return frame_; }
    const std::string& stopped() const override { return stopped_; }
    std::size_t boards() const override { return boards_.size(); }
    void control(std::size_t player) override { me_ = std::min(player, boards_.size() - 1); }
    std::size_t player() const override { return me_; }
    void hold(std::size_t board, Held inputs) override {
        if (board < held_.size()) held_[board] = std::move(inputs);
    }
    Held held(std::size_t board) const override { return board < held_.size() ? held_[board] : Held{}; }

private:
    struct Rewound {};
    struct Board {
        Board(const m3::RomSet& roms, std::span<const recomp::FunctionEntry> functions)
            : bus(roms), c(bus, functions), screen(m3::TilegenRenderer::kWidth * m3::TilegenRenderer::kHeight) {}
        m3::Bus bus;
        recomp::Context c;
        recomp::FrameTiming timing;
        m3::TilegenRenderer tilegen;
        m3::Real3dRenderer real3d;
        std::vector<uint32_t> screen;
        std::thread thread;
        std::vector<uint8_t> saved; // (its state at its own point, while a save is under way)
        uint32_t line = 0;          // (at the last meeting)
        bool has_saved = false, rewind = false, draw_2d = false;
        std::string shot;
    };

    // The boards run on to the first meeting after frame `to` ends (and, `snap`, keep their
    // states after its end); the bot's thread waits for it.
    void run_to(uint64_t to, bool snap, const std::string& shot) {
        {
            std::unique_lock lock(mutex_);
            to_ = to, want_snap_ = snap;
            Board& b = *boards_[me_];
            b.shot = shot, b.draw_2d = !shot.empty();
            paused_ = false, go_ = true;
        }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return paused_ || !stopped_.empty(); });
        frame_ = to;
    }

    // Each board's inputs for the frame `frame` (the controlled player's, the others' held).
    void set_inputs(uint64_t frame) {
        for (std::size_t i = 0; i < boards_.size(); ++i) {
            const Inputs in = held_[i] ? held_[i](frame) : Inputs{};
            boards_[i]->bus.inputs.bank0 = in.bank0;
            boards_[i]->bus.inputs.controls = in.controls;
            if (log_) {
                tools::LinkedInputLog::Change c;
                c.inputs = true, c.bank0 = in.bank0, c.controls = in.controls;
                record(i, std::move(c));
            }
        }
    }

    // --record-inputs: a change made where the boards stand for the bot (here_).
    void record(std::size_t board, tools::LinkedInputLog::Change c) {
        tools::LinkedInputLog::Where where = here_;
        where.board = static_cast<uint8_t>(board);
        std::lock_guard lock(log_mutex_);
        log_->changes.emplace_back(where, std::move(c));
    }
    void record_write(std::size_t board, uint32_t addr, uint16_t v) {
        if (!log_) return;
        tools::LinkedInputLog::Change c;
        c.writes.emplace_back(addr, v);
        record(board, std::move(c));
    }

    // Every board and the ring put back as `snap` has them (the boards stopped: not yet
    // started, or at a meeting, going on from their own points as they leave it).
    void put_back(const Snapshot& snap) {
        m3::State s(std::span<const uint8_t>(snap.data));
        s.tag("spikeout linked save state 1");
        ring_.cut_state(s);
        for (auto& b : boards_) {
            std::vector<uint8_t> bytes;
            s.io(bytes);
            m3::State bs(std::move(bytes));
            tools::board_state(bs, b->c, b->timing, {});
            b->rewind = started_;
            b->has_saved = false;
        }
        s.tag("end");
        here_ = {tools::LinkedInputLog::At::Resumed, 0, boards_[0]->timing.frames(), 0};
        if (log_) {
            std::lock_guard lock(log_mutex_);
            if (log_->changes.size() > snap.changes) log_->changes.resize(snap.changes);
        }
    }

    void run(std::size_t i) {
        Board& b = *boards_[i];
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return go_; });
            started_ = true;
        }
        for (;;) {
            if (quit_) break;
            try {
                b.c.resume_block(b.c.cpu.pc);
            } catch (const Rewound&) {
                continue;
            } catch (const Quit&) {
                break;
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex_);
                if (stopped_.empty()) stopped_ = "board " + std::to_string(i) + ": " + e.what();
                break;
            }
            std::lock_guard lock(mutex_);
            if (stopped_.empty()) stopped_ = "board " + std::to_string(i) + ": the reset code returned";
            break;
        }
        turns_.finish(i);
        changed_.notify_all();
    }

    void display_line(std::size_t i, uint32_t line) {
        Board& b = *boards_[i];
        if (b.draw_2d) {
            if (line == 0) b.tilegen.begin_frame();
            b.tilegen.draw_line(b.bus, static_cast<int>(line));
        }
        if (line % kSlice == kSlice - 1) {
            b.bus.netboard().sync(b.c.cycles());
            b.line = line;
            turns_.pass(i);
            if (quit_) throw Quit{};
            if (b.rewind) {
                b.rewind = false;
                throw Rewound{};
            }
        }
    }

    void frame_end(std::size_t i) {
        Board& b = *boards_[i];
        b.bus.midi_out().clear();
        const uint64_t frame = b.timing.frames();
        if (frame == to_ && !b.shot.empty()) {
            b.real3d.render(b.bus.real3d());
            b.tilegen.composite(b.screen, b.real3d.image());
            tools::write_png(b.shot, b.screen, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
            b.shot.clear(), b.draw_2d = false;
        }
        if (frame == to_ && want_snap_)
            b.c.request_snapshot([this, &b](uint32_t) {
                m3::State s;
                tools::board_state(s, b.c, b.timing, {});
                b.saved = std::move(s.data());
                b.has_saved = true;
            });
        tools::LinkedInputLog::Change c; // (--record-inputs: main RAM's hash now and then, the inputs set)
        if (log_ && frame % tools::LinkedInputLog::kHashEvery == 0) c.hash = tools::Recording::ram_hash(b.bus.ram());
        if (frame < to_) {
            const Held& held = held_[i]; // (the next frame's inputs)
            const Inputs in = held ? held(frame + 1) : Inputs{};
            b.bus.inputs.bank0 = in.bank0;
            b.bus.inputs.controls = in.controls;
            c.inputs = true, c.bank0 = in.bank0, c.controls = in.controls;
        }
        if (log_ && (c.inputs || c.hash)) {
            std::lock_guard lock(log_mutex_);
            log_->changes.emplace_back(tools::LinkedInputLog::Where{tools::LinkedInputLog::At::FrameEnd, static_cast<uint8_t>(i), frame, 0},
                                       std::move(c));
        }
    }

    // At a meeting, every board stopped (the last to come runs this): the link's packets on;
    // the boards stopped for the bot once all have ended frame to_ (and saved, if asked).
    void between() {
        ring_.exchange();
        if (!go_) return;
        for (auto& b : boards_)
            if (b->timing.frames() < to_ || (want_snap_ && !b->has_saved)) return;
        if (want_snap_) {
            if (!ring_.cut_complete()) return;
            auto snap = std::make_shared<Snapshot>();
            m3::State s;
            s.tag("spikeout linked save state 1");
            ring_.cut_state(s);
            for (auto& b : boards_) {
                s.io(b->saved);
                b->has_saved = false;
                std::vector<uint8_t>().swap(b->saved);
            }
            s.tag("end");
            snap->data = std::move(s.data());
            snap->frame = to_;
            if (log_) {
                std::lock_guard lock(log_mutex_);
                snap->changes = log_->changes.size();
            }
            saved_ = snap;
        }
        here_ = {tools::LinkedInputLog::At::Meeting, 0, boards_[0]->timing.frames(), boards_[0]->line};
        std::unique_lock lock(mutex_);
        go_ = false, paused_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return go_; }); // (the bot's turn: reading, writing, loading)
    }

    struct Quit {};
    m3::LinkRing ring_;
    tools::Turns turns_;
    std::vector<std::unique_ptr<Board>> boards_;
    std::vector<Held> held_;
    std::size_t me_ = 0;
    std::shared_ptr<Snapshot> saved_; // (the last save, or the state loaded)
    bool behind_saved_ = false;       // (the boards ran on past it to a meeting)
    uint64_t frame_ = 0, to_ = 0;
    bool want_snap_ = false;
    std::string stopped_;
    tools::LinkedInputLog* log_ = nullptr; // --record-inputs
    tools::LinkedInputLog::Where here_;    // (where the boards stand for the bot)
    std::mutex log_mutex_;

    std::mutex mutex_;
    std::condition_variable changed_;
    bool go_ = false, paused_ = false, quit_ = false, started_ = false;
};

// Boards that try things from a save state in parallel (one thread each):
// try_all runs fn(worker, k) for k in [0, n) from `snap`, every worker taking
// the next k, and returns each one's result.
class Workers {
public:
    // make: a game like the bot's (one board, or linked boards); `main`: the bot's, whose
    // controlled player and other players' inputs every try takes.
    Workers(int n, std::function<std::unique_ptr<Game>()> make, const Game& main)
        : make_(std::move(make)), main_(main), count_(std::max(n, 1)) {}

    template <class T>
    std::vector<T> try_all(const std::shared_ptr<Snapshot>& snap, std::size_t n, const std::function<T(Game&, std::size_t)>& fn) {
        return try_from<T>([&snap](std::size_t) { return snap; }, n, fn);
    }
    // The same, try k starting from from(k).
    template <class T>
    std::vector<T> try_from(const std::function<std::shared_ptr<Snapshot>(std::size_t)>& from, std::size_t n,
                            const std::function<T(Game&, std::size_t)>& fn) {
        if (boards_.empty()) // (made from the first snapshot)
            for (int i = 0; i < count_; ++i) {
                boards_.push_back(make_());
                boards_.back()->start_from(from(0)->data);
            }
        std::vector<T> out(n);
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> threads;
        for (auto& b : boards_)
            threads.emplace_back([&, board = b.get()] {
                for (std::size_t k; (k = next++) < n;) {
                    board->load(from(k));
                    board->control(main_.player());
                    for (std::size_t i = 0; i < main_.boards(); ++i) board->hold(i, main_.held(i));
                    out[k] = fn(*board, k);
                }
            });
        for (auto& t : threads) t.join();
        return out;
    }

private:
    std::function<std::unique_ptr<Game>()> make_;
    const Game& main_;
    int count_;
    std::vector<std::unique_ptr<Game>> boards_;
};

// ---- The game as the bot sees it ----
struct Character {
    uint32_t base = 0;
    int health = 0, max_health = 0;
    float x = 0, y = 0, z = 0;
};

// The controlled player's character (player 1's on one board; linked, the one
// whose board the bot is controlling: player 2's object on board 1).
uint32_t me(const Game& e) { return ram::kPlayer + static_cast<uint32_t>(e.player()) * ram::kSlot; }
// For the log: "P2 " when linked boards are played, else nothing.
std::string who(const Game& e) { return e.boards() > 1 ? "P" + std::to_string(e.player() + 1) + " " : std::string(); }

Character character(const Game& e, uint32_t base) {
    return {base, e.read16(base + ram::kHealth), e.read16(base + ram::kMaxHealth), e.readf(base + ram::kX),
            e.readf(base + ram::kY), e.readf(base + ram::kZ)};
}

uint16_t area_phase(const Game& e) { return e.read16(ram::kAreaPhase + 2); }

// The enemies to fight: those with health while the area's fight is on (after
// a boss those left run off, and between stages the slots keep the last ones).
std::vector<Character> enemies(const Game& e) {
    std::vector<Character> out;
    if (area_phase(e) & ram::kPhaseOver) return out;
    for (int i = 0; i < ram::kEnemySlots; ++i) {
        const Character ch = character(e, ram::kEnemies + static_cast<uint32_t>(i) * ram::kSlot);
        if (ch.max_health > 0 && ch.health > 0 && e.read16(ch.base + ram::kLive) >> 8 != 0) out.push_back(ch);
    }
    return out;
}

// --unmarked-gate: at a gate with another beside the arrow's, that one is broken instead (Victor's
// in stage 2: the sewers). Set by the play loop when the gate is called (gate_objects), read by
// the tries: x, y (the arrow's height), z.
bool g_unmarked_gate = false;
std::optional<std::array<float, 3>> g_gate_point;
// --gate-object N: object N (a gate, gate_objects) broken whenever it can be, the fight or not:
// stage 2's basketball court (area 15) has its gates live during the last boss, no gate called;
// 0x01 (beside the court) leads to the mall, 0x0B/0x0C (Victor's from the far side) to stage
// change 57 and area 24. While it stands, it is the target (g_gate_point).
int g_gate_object = -1;
bool gate_object_standing(const Game& e) {
    if (g_gate_object < 0) return false;
    const uint32_t objects = e.read32(e.r13() - 16100);
    if (objects >= 0x800000) return false;
    const uint32_t flags = e.read32(objects + 2028 + static_cast<uint32_t>(g_gate_object) * 24);
    return (flags & 0x01000000u) && !(flags & 0x04000000u);
}

// Where the red arrow points, while the game asks for the gate to be broken (or the gate chosen).
std::optional<std::pair<float, float>> arrow_target(const Game& e) {
    if (g_gate_object >= 0 && g_gate_point && gate_object_standing(e)) return std::make_pair((*g_gate_point)[0], (*g_gate_point)[2]);
    if (!(area_phase(e) & ram::kPhaseGate)) return std::nullopt;
    if (g_gate_point) return std::make_pair((*g_gate_point)[0], (*g_gate_point)[2]);
    return std::make_pair(e.readf_on(0, ram::kArrowTarget), e.readf_on(0, ram::kArrowTarget + 8));
}
float gate_height(const Game& e) { return g_gate_point ? (*g_gate_point)[1] : e.readf_on(0, ram::kArrowTarget + 4); }

void print_state(const Game& e) {
    const Character p = character(e, me(e));
    std::printf("frame %llu: area phase %u, frames %u; player hp %d/%d at %.2f %.2f %.2f\n", static_cast<unsigned long long>(e.frame()),
                area_phase(e), e.read16(ram::kAreaFrames), p.health, p.max_health, p.x, p.y, p.z);
    for (int i = 0; i < ram::kEnemySlots; ++i) {
        const Character ch = character(e, ram::kEnemies + static_cast<uint32_t>(i) * ram::kSlot);
        if (ch.max_health == 0 && ch.health == 0) continue;
        std::printf("  slot %06x: kind %04x hp %d/%d at %.2f %.2f %.2f\n", ch.base, e.read16(ch.base + ram::kKind), ch.health,
                    ch.max_health, ch.x, ch.y, ch.z);
    }
    std::printf("  arrow to %.2f %.2f %.2f%s\n", static_cast<double>(e.readf_on(0, ram::kArrowTarget)),
                static_cast<double>(e.readf_on(0, ram::kArrowTarget + 4)), static_cast<double>(e.readf_on(0, ram::kArrowTarget + 8)),
                arrow_target(e) ? " (break the gate)" : "");
}

// ---- The area's floor, as the game keeps it (read from its floor query at
// 0x4FAB4, which the characters' movement calls for each of their probe
// points: x in f1, z in f3, the height to look down from in f2) ----
// A grid over the area: the pointer at r13 - 7832 points to {cells, ..., header
// at +12}; the header has the bounds (x0 +8, z0 +12, x1 +16, z1 +20) and the
// columns (+24). A cell is 1 / scale units square (scale: the float at
// 0x7F768). Each cell's entry points to its planes, 24 bytes each until a
// flags word of -1: flags, then d a b c (floats): the height at (fx, fz) within
// the cell (0..1) is d + a/(b k) fx + c/(b k) fz, k the float at 0x7F770. A
// cell lists every floor over it (a crate's top and the ground beside it,
// stairs, the floor above); the game takes the first at or below the height
// looked from. Flags bit 0x02000000: skipped by the game (not a floor);
// 0x00040000: a solid block (the warehouse's crate stacks: the character's
// probes round his body push him off it, and no jump lands on it; the way
// into the crates' pen is round by the floor below, which the bot then
// walked). As the floor query has it: 0x04000000 isn't a floor (under the
// opera house's balcony edge: the bot planned a drop onto it and couldn't
// make it); 0x02000000 is one only while a switch is on (a byte at
// *(r13 - 15084) + 19); bit 31 means nothing there (the way to Mikhail is
// 0x01000145). SPIKEBOT_SKIP=hex: flags not stood on (the solid blocks).
constexpr uint32_t kNotFloor = 0x00040000u;
struct Floor {
    struct Plane {
        uint32_t flags;
        float d, a, c; // (a and c divided by b k already)
    };
    // A wall (the hedges, fences: what the floor doesn't stop), from the wall test at
    // 0x50BBC: the grid's +8 points to a table of them, and each cell's first plane's +20
    // to its list of wall numbers (16-bit, until 0xFFFF). A wall record: flags (bit 31:
    // skipped), cos +4 and sin +8 of its direction, its ends (x y z) at +12 and +24 (y the
    // bottom), its height above each end +36, +40. In its own frame (u along it = x sin -
    // z cos, v across = z sin + x cos) it runs from u0 to u1 at v = d; it stops a body
    // coming from the side v > d, pushing it out to v = d + its radius.
    struct Wall {
        float c, s, u0, u1, d, y0, y1, h0, h1;
        uint32_t flags = 0;          // (the record's first word: byte 1 an object's index, a gate's)
        float x0 = 0, z0 = 0, x1 = 0, z1 = 0; // (its ends in the world)
    };
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0, scale = 1;
    int columns = 0, rows = 0;
    bool switched = false; // (the byte at *(r13 - 15084) + 19: planes flagged 0x02000000 are floors while it's set)
    std::vector<std::vector<Plane>> cells;
    std::vector<std::vector<uint16_t>> cell_walls;
    std::vector<Wall> walls;

    // Whether walls stop a body (radius r, standing at height h) going from (ax, az) to
    // (bx, bz): the move crosses a wall's line pushed out by r, from its front, at a
    // height the wall covers.
    static constexpr float kBody = 1.6f;
    // (0.25: with 0.5 the maze's gaps a cell or two wide were shut, and the way to its
    // enemies with them; the game stops him at the walls themselves. SPIKEBOT_RADIUS=r)
    static inline const float kRadius = std::getenv("SPIKEBOT_RADIUS") ? static_cast<float>(std::atof(std::getenv("SPIKEBOT_RADIUS"))) : 0.25f;
    bool blocked(int cell_a, int cell_b, float ax, float az, float bx, float bz, float h) const {
        for (int cell : {cell_a, cell_b})
            for (uint16_t n : cell_walls[static_cast<std::size_t>(cell)]) {
                if (n >= walls.size()) continue;
                const Wall& w = walls[n];
                const float va = az * w.s + ax * w.c - w.d - kRadius, vb = bz * w.s + bx * w.c - w.d - kRadius;
                // (crossing it either way: the game pushes only from the front, but the theatre's
                // seat rows stopped the player crossing from behind, the planner's way through)
                const float vfa = va + 2 * kRadius, vfb = vb + 2 * kRadius; // (its back, pushed out too)
                static const bool one_sided = std::getenv("SPIKEBOT_ONESIDED") != nullptr;
                if (!((va >= 0 && vb < 0) || (!one_sided && vfa <= 0 && vfb > 0))) continue;
                const float t = va / (va - vb);
                const float ua = ax * w.s - az * w.c, ub = bx * w.s - bz * w.c, u = ua + (ub - ua) * t;
                const float lo = std::min(w.u0, w.u1), hi = std::max(w.u0, w.u1);
                if (u < lo - kRadius || u > hi + kRadius) continue;
                const float k = hi > lo ? std::clamp((u - w.u0) / (w.u1 - w.u0), 0.0f, 1.0f) : 0.0f;
                const float bottom = w.y0 + (w.y1 - w.y0) * k, top = bottom + w.h0 + (w.h1 - w.h0) * k;
                if (h < top && h + kBody > bottom) return true;
            }
        return false;
    }

    bool valid() const { return columns > 0; }
    // The cell of (x, z), or -1 outside.
    int cell(float x, float z) const {
        if (x < x0 || z < z0 || x >= x1 || z >= z1) return -1;
        const int cx = static_cast<int>((x - x0) * scale), cz = static_cast<int>((z - z0) * scale);
        return cx < columns && cz < rows ? cz * columns + cx : -1;
    }
    // The heights of the floors over (x, z) that can be stood on, highest first as listed: a
    // cell lists the ground under a crate as well as its top, and the ground there is inside it
    // (kHeadroom: room to stand under whatever is above).
    static constexpr float kHeadroom = 1.6f;
    std::vector<float> heights(float x, float z) const {
        std::vector<float> all, floors, out; // (all: what takes room, a solid block's top too)
        const int i = cell(x, z);
        if (i < 0) return out;
        const float fx = (x - x0) * scale - std::floor((x - x0) * scale), fz = (z - z0) * scale - std::floor((z - z0) * scale);
        static const uint32_t skip = std::getenv("SPIKEBOT_SKIP") ? std::strtoul(std::getenv("SPIKEBOT_SKIP"), nullptr, 16) : kNotFloor;
        for (const Plane& p : cells[static_cast<std::size_t>(i)]) {
            // (as the game's floor query takes them: 0x04000000 never; 0x02000000 only while the
            // switch is on; a solid block's top takes room, but isn't stood on)
            if (p.flags & 0x04000000u) continue;
            static const bool no_switch = std::getenv("SPIKEBOT_NOSWITCH") != nullptr;
            if ((p.flags & 0x02000000u) && (!switched || no_switch)) continue;
            // (planes with flags 0: not floors, ceilings it seems; taken as room-takers they hid
            // the opera house's stairs under them)
            if (p.flags == 0) continue;
            all.push_back(p.d + p.a * fx + p.c * fz);
            if (!(p.flags & skip)) floors.push_back(all.back());
        }
        for (float h : floors)
            if (std::none_of(all.begin(), all.end(), [h](float o) { return o > h + 0.01f && o < h + kHeadroom; })) out.push_back(h);
        return out;
    }
};

// A way over the floor grid (A*): from a floor to a neighbouring cell's
// floor is a walk if it rises no more than kWalkUp, a jump up to kJumpUp (a
// jump lifts the player 1.5: --moves-test), a drop down to kDropDown. Each
// point: a cell's middle on its floor, and whether it is jumped up to.
const float kWalkUp = std::getenv("SPIKEBOT_STEP") ? static_cast<float>(std::atof(std::getenv("SPIKEBOT_STEP"))) : 0.35f;
// A jump goes higher the longer the button is held (the owner): tapped 2 frames it lifts 1.5, held
// 20 it lifts 2.8 and carries on forward (over the theatre's stage wall, which no tap crossed).
constexpr int kJumpHold = 20;
// Running: the lever tapped (kRunTap frames), let go as long, then held (the guide; the owner:
// "always run to our enemy instead of walk").
const int kRunTap = std::getenv("SPIKEBOT_RUNTAP") ? std::atoi(std::getenv("SPIKEBOT_RUNTAP")) : 2;
constexpr float kJumpUp = 2.4f, kDropDown = 4.0f;
// A wall is jumped only when its top is this little above the floor (the theatre's stage wall,
// 0.85, was; the opera house's walls by its stairs, planned with 2.4, weren't: the bot shuttled
// at them instead of going round by the outdoor area).
constexpr float kWallJump = 1.0f;
constexpr float kTargetOverFloor = 2.0f; // (a target this far from its cell's nearest floor stands on it)
struct Waypoint {
    float x, y, z;
    bool jump;
};

// Cells found barred while following a way (the opera house's balcony: a railing the walls
// don't show; six turns no nearer the point headed for): left out until the area changes.
std::set<int> g_barred;

std::vector<Waypoint> plan_way(const Floor& f, float x, float y, float z, float tx, float ty, float tz) {
    std::vector<Waypoint> out;
    if (!f.valid()) return out;
    const float step = 1 / f.scale;
    auto middle = [&](int cell, float& cx, float& cz) {
        cx = f.x0 + (static_cast<float>(cell % f.columns) + 0.5f) * step;
        cz = f.z0 + (static_cast<float>(cell / f.columns) + 0.5f) * step;
    };
    // A node: a cell and one of its floors (its index in heights()).
    auto floors = [&](int cell) {
        float cx, cz;
        middle(cell, cx, cz);
        return f.heights(cx, cz);
    };
    auto nearest_floor = [&](int cell, float h) {
        const std::vector<float> hs = floors(cell);
        int best = -1;
        for (std::size_t i = 0; i < hs.size(); ++i)
            if (best < 0 || std::fabs(hs[i] - h) < std::fabs(hs[static_cast<std::size_t>(best)] - h)) best = static_cast<int>(i);
        return best;
    };
    const int c0 = f.cell(x, z), c1 = f.cell(tx, tz);
    if (c0 < 0 || c1 < 0) return out;
    const int l0 = nearest_floor(c0, y), l1 = nearest_floor(c1, ty);
    if (l0 < 0 || l1 < 0) return out;
    constexpr int kLevels = 16;
    auto id = [](int cell, int level) { return static_cast<int64_t>(cell) * kLevels + level; };
    const int64_t goal = id(c1, l1);
    float gx, gz;
    middle(c1, gx, gz);
    // (the target's floor; or its own height when the cell lists none near it: on stage 2's
    // walkways two enemies paced a ledge whose floor the grid has in some cells and not others,
    // and where it hadn't, the ground 5.3 below was taken for theirs, the way out of reach ending
    // on a walkway near the ground instead of on the ledge; the way flipped between the two every
    // turn and the bot shuttled until the time ran out. A gate's arrow, 1.5 over its floor,
    // keeps the floor.)
    const float listed = floors(c1)[static_cast<std::size_t>(l1)];
    const float th = std::fabs(listed - ty) <= kTargetOverFloor ? listed : ty;
    std::map<int64_t, float> cost;
    std::map<int64_t, int64_t> came;
    using Item = std::pair<float, int64_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    cost[id(c0, l0)] = 0;
    open.push({0, id(c0, l0)});
    int expanded = 0;
    // (the target itself may be out of reach: a gate's arrow floats in the doorway, a floor
    // up; then the way goes to the cell reached nearest it)
    int64_t nearest = id(c0, l0);
    float nearest_off = 1e9f;
    while (!open.empty() && expanded < 400000) {
        const auto [est, node] = open.top();
        open.pop();
        if (node == goal) break;
        ++expanded;
        const int cell = static_cast<int>(node / kLevels), level = static_cast<int>(node % kLevels);
        if (const std::vector<float> hs = floors(cell); level < static_cast<int>(hs.size())) {
            float mx, mz;
            middle(cell, mx, mz); // (its height too: not the floor under a balcony)
            const float off = std::hypot(gx - mx, gz - mz) + 2 * std::fabs(hs[static_cast<std::size_t>(level)] - th);
            if (off < nearest_off) nearest_off = off, nearest = node;
        }
        const std::vector<float> here = floors(cell);
        if (level >= static_cast<int>(here.size())) continue;
        const float h = here[static_cast<std::size_t>(level)];
        const int cx = cell % f.columns, cz = cell / f.columns;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                const int nx = cx + dx, nz = cz + dz;
                if (nx < 0 || nz < 0 || nx >= f.columns || nz >= f.rows) continue;
                const int next = nz * f.columns + nx;
                if (g_barred.count(next)) continue;
                const std::vector<float> there = floors(next);
                float ax, az, bx, bz;
                middle(cell, ax, az);
                middle(next, bx, bz);
                for (std::size_t l = 0; l < there.size() && l < kLevels; ++l) {
                    const float rise = there[l] - h;
                    if (rise > kJumpUp || rise < -kDropDown) continue;
                    // A wall no higher than a jump is jumped (the theatre's stage: a step up from the
                    // aisle, "jump 1 step up to the stage", the owner).
                    bool over = false;
                    if (f.blocked(cell, next, ax, az, bx, bz, std::max(h, there[l]))) {
                        if (f.blocked(cell, next, ax, az, bx, bz, std::max(h, there[l]) + kWallJump)) continue;
                        over = true;
                    }
                    const float g = cost[node] + step * (dx && dz ? 1.414f : 1.0f) + (rise > kWalkUp || over ? 2.0f : 0.0f);
                    const int64_t n = id(next, static_cast<int>(l));
                    auto it = cost.find(n);
                    if (it != cost.end() && it->second <= g) continue;
                    cost[n] = g;
                    came[n] = node;
                    float mx, mz;
                    middle(next, mx, mz);
                    open.push({g + std::hypot(gx - mx, gz - mz), n});
                }
            }
    }
    const int64_t end = came.count(goal) ? goal : nearest;
    if (!came.count(end)) return out;
    for (int64_t n = end; came.count(n); n = came[n]) {
        const int cell = static_cast<int>(n / kLevels), level = static_cast<int>(n % kLevels);
        float mx, mz;
        middle(cell, mx, mz);
        const float h = floors(cell)[static_cast<std::size_t>(level)];
        const int pcell = static_cast<int>(came[n] / kLevels), plevel = static_cast<int>(came[n] % kLevels);
        const float ph = floors(pcell)[static_cast<std::size_t>(plevel)];
        float px, pz;
        middle(pcell, px, pz);
        out.push_back({mx, h, mz, h - ph > kWalkUp || f.blocked(pcell, cell, px, pz, mx, mz, std::max(h, ph))});
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// Where to head along a way from (x, y, z): a few cells on from the point
// nearest the player (on his level), but no further than the next point
// jumped up to.
constexpr std::size_t kLookahead = 4;
std::optional<Waypoint> next_on_way(const std::vector<Waypoint>& way, float x, float y, float z) {
    if (way.empty()) return std::nullopt;
    std::size_t at = 0;
    float best = 1e9f;
    for (std::size_t i = 0; i < way.size(); ++i)
        if (const float d = std::hypot(way[i].x - x, way[i].z - z) + 2 * std::fabs(way[i].y - y); d < best) best = d, at = i;
    if (way[at].jump && y < way[at].y - kWalkUp) return way[at]; // (the step up not made yet)
    std::size_t to = std::min(at + kLookahead, way.size() - 1);
    for (std::size_t i = at + 1; i <= to; ++i)
        if (way[i].jump) {
            to = i;
            break;
        }
    return way[to];
}

float as_float(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

Floor read_floor(const Game& e) {
    Floor f;
    const uint32_t grid = e.read32(e.r13() - 7832);
    if (grid == 0xFFFFFFFFu || grid >= 0x800000) return f;
    const uint32_t table = e.read32(grid), header = e.read32(grid + 12);
    if (table == 0xFFFFFFFFu || table >= 0x800000 || header >= 0x800000) return f;
    f.x0 = as_float(e.read32(header + 8)), f.z0 = as_float(e.read32(header + 12));
    f.x1 = as_float(e.read32(header + 16)), f.z1 = as_float(e.read32(header + 20));
    f.columns = static_cast<int>(e.read32(header + 24));
    f.scale = as_float(e.read32(0x7F768));
    if (const uint32_t sw = e.read32(e.r13() - 15084); sw < 0x800000) f.switched = e.read16(sw + 18) & 0xFF;
    const float k = as_float(e.read32(0x7F770));
    if (f.columns <= 0 || f.columns > 4096 || !(f.scale > 0)) return Floor{};
    f.rows = static_cast<int>(std::ceil((f.z1 - f.z0) * f.scale));
    f.cells.resize(static_cast<std::size_t>(f.columns) * static_cast<std::size_t>(f.rows));
    f.cell_walls.resize(f.cells.size());
    for (std::size_t i = 0; i < f.cells.size(); ++i) {
        const uint32_t first = e.read32(table + static_cast<uint32_t>(i) * 4);
        uint32_t p = first;
        for (int n = 0; p < 0x800000 && n < 64; ++n, p += 24) {
            const uint32_t flags = e.read32(p);
            if (flags == 0xFFFFFFFFu) break;
            const float b = as_float(e.read32(p + 12)) * k;
            f.cells[i].push_back({flags, as_float(e.read32(p + 4)), as_float(e.read32(p + 8)) / b, as_float(e.read32(p + 16)) / b});
        }
        if (!f.cells[i].empty())
            for (uint32_t q = e.read32(first + 20), n = 0; q < 0x800000 && n < 256; q += 2, ++n) {
                const uint16_t w = e.read16(q);
                if (w == 0xFFFF) break;
                f.cell_walls[i].push_back(w);
            }
    }
    const uint32_t walls = e.read32(grid + 8);
    uint16_t most = 0;
    for (const auto& list : f.cell_walls)
        for (uint16_t w : list) most = std::max(most, w);
    if (walls < 0x800000)
        for (uint32_t n = 0; n <= most; ++n) {
            const uint32_t r = e.read32(walls + n * 4);
            Floor::Wall w{};
            if (r < 0x800000 && !(e.read32(r) & 0x80000000u)) {
                auto at = [&](uint32_t off) { return as_float(e.read32(r + off)); };
                // (the ends in the world: x y z at +12 +16 +20 and +24 +28 +32; into the wall's frame)
                const float c = at(4), s = at(8), x0 = at(12), z0 = at(20), x1 = at(24), z1 = at(32);
                w = {c, s, x0 * s - z0 * c, x1 * s - z1 * c, z0 * s + x0 * c, at(16), at(28), at(36), at(40), e.read32(r), x0, z0, x1, z1};
            } else {
                w.d = 1e9f; // (skipped: never crossed)
            }
            f.walls.push_back(w);
        }
    return f;
}

// The area's objects that walls belong to (gates: hit, a wall's byte 1 picks an entry of the
// byte table at the object table + 24320, the object's number; 0x5103C sets that object's
// 0x08000000, and broken it has 0x04000000, which the area's script tests, condition 0x0C). The
// objects: 24 bytes each at *(r13 - 16100) + 2028, the first word their flags. A gate the script
// can tell broken has 0x01000000 (Victor's two in stage 2: 0x0B/0x0C, the arrow's, and 0x05/0x06,
// unmarked; each sends the script its own way).
struct GateObject {
    int number = 0;
    uint32_t flags = 0;
    float x = 0, y = 0, z = 0; // (the middle of its walls)
    int walls = 0;
};
std::vector<GateObject> gate_objects(const Game& e, const Floor& f) {
    std::vector<GateObject> out;
    const uint32_t objects = e.read32(e.r13() - 16100);
    if (objects >= 0x800000) return out;
    std::map<int, GateObject> by;
    for (const Floor::Wall& w : f.walls) {
        if (w.d > 1e8f) continue; // (skipped)
        const int index = static_cast<int>(w.flags >> 8 & 0xFF);
        if (!index) continue;
        const int n = e.read16(objects + 24320 + static_cast<uint32_t>(index & ~1)) >> (index & 1 ? 0 : 8) & 0xFF;
        if (!n) continue;
        GateObject& g = by[n];
        g.number = n;
        g.x += (w.x0 + w.x1) / 2, g.y += w.y0, g.z += (w.z0 + w.z1) / 2;
        ++g.walls;
    }
    for (auto& [n, g] : by) {
        g.flags = e.read32(objects + 2028 + static_cast<uint32_t>(n) * 24);
        g.x /= static_cast<float>(g.walls), g.y /= static_cast<float>(g.walls), g.z /= static_cast<float>(g.walls);
        out.push_back(g);
    }
    return out;
}

// ---- Playing ----
// A move: what is held each frame of a try (frame i of it).
struct Move {
    const char* name;
    std::function<Inputs(int i)> inputs;
    int length = 20; // (its own inputs this long; then beat taps to the end of a try. A move longer
                     // than a turn, a charge, is played whole.)
};

std::vector<Move> moves() {
    static const Button kDirs[8][2] = {{Up, Up}, {Up, Right}, {Right, Right}, {Down, Right},
                                       {Down, Down}, {Down, Left}, {Left, Left}, {Up, Left}};
    static const char* kNames[8] = {"up", "up-right", "right", "down-right", "down", "down-left", "left", "up-left"};
    std::vector<Move> m;
    // Beat taps: pressed 2 frames in every 6 (the game reads presses, not holds).
    auto tap = [](int i) { return i % 6 < 2; };
    // (ties go to the first: beat taps, which free the player from a hold;
    // waiting last)
    m.push_back({"beat", [tap](int i) { return tap(i) ? press({Beat}) : Inputs{}; }});
    m.push_back({"charge", [](int i) { return i < 16 ? press({Charge}) : Inputs{}; }});
    m.push_back({"jump", [](int i) { return i < 2 ? press({Jump}) : Inputs{}; }});
    // From a player's guide (testamentenjoyment.net/spikeout/guide-part1, see
    // docs/bot.md): beat and charge together sweep (low: hits enemies below),
    // beat and jump leap at the nearest enemy faced (the homing attack), all
    // three are the special attack (one of the special items; invincible,
    // also out of a hold), and wiggling the lever left and right escapes a hold.
    m.push_back({"sweep", [](int i) { return i < 2 ? press({Beat, Charge}) : Inputs{}; }});
    m.push_back({"homing", [](int i) { return i < 2 ? press({Beat, Jump}) : Inputs{}; }});
    m.push_back({"special", [](int i) { return i < 2 ? press({Beat, Charge, Jump}) : Inputs{}; }});
    m.push_back({"wiggle", [](int i) { return press({i % 4 < 2 ? Left : Right}); }});
    // Charge held longer (the guide: level 2 a launcher, 3 a stun, 4 a bowling strike that
    // knocks a crowd down; the gauge at the top), released and given a few frames.
    for (const int hold : {30, 45}) {
        static const char* kCharge[] = {"charge-30", "charge-45"};
        m.push_back({kCharge[(hold - 30) / 15], [hold](int i) { return i < hold ? press({Charge}) : Inputs{}; }, hold + 4});
    }
    // Beat taps walking only the 4 straight ways: with a diagonal they ended
    // like a straight one (21 moves: two rounds of the 11 workers, not three).
    for (int d = 0; d < 8; ++d) {
        const Button a = kDirs[d][0], b = kDirs[d][1];
        m.push_back({kNames[d], [a, b](int) { return press({a, b}); }});
        if (d % 2 == 0) m.push_back({nullptr, [a, b, tap](int i) { return tap(i) ? press({a, b, Beat}) : press({a, b}); }});
    }
    m.push_back({"wait", [](int) { return Inputs{}; }});
    static std::vector<std::string> names;
    if (names.empty())
        for (int d = 0; d < 8; d += 2) names.push_back(std::string(kNames[d]) + "+beat");
    for (std::size_t i = 0, d = 0; i < m.size(); ++i)
        if (!m[i].name) m[i].name = names[d++].c_str();
    return m;
}

const Character* boss_of(const Game& e, const std::vector<Character>& foes);

// How things stand, for comparing tries.
struct Score {
    int player_health = 0, enemy_health = 0, enemies = 0;
    int boss_health = 0; // (in a boss fight: the boss's, the one with the most health at full)
    float player_y = 0;  // (a fall off a walkway shows here)
    float nearest = 0; // distance to the nearest enemy (on the ground), else to where the arrow points
    bool arrow = false; // the red arrow shown
};

// The red arrow floats about this far above the floor by the gate (1.3 at the shipyard's and
// the opera house's, 2 at the car park's).
constexpr float kArrowAbove = 1.3f;

Score score(const Game& e) {
    Score s;
    const Character p = character(e, me(e));
    s.player_health = p.health;
    s.player_y = p.y;
    const auto t = arrow_target(e);
    s.arrow = t.has_value();
    if (t) { // the fight is over: where the arrow points (those left are bystanders); with its
             // height (the opera house's gate is on a balcony above the doors below it)
        const float up = gate_height(e) - kArrowAbove - p.y;
        s.nearest = std::hypot(std::hypot(t->first - p.x, t->second - p.z), std::max(0.0f, std::fabs(up) - 1.0f));
        return s;
    }
    s.nearest = 1e9f;
    const std::vector<Character> foes = enemies(e);
    const Character* boss = boss_of(e, foes);
    for (const Character& en : foes) {
        s.enemy_health += en.health;
        ++s.enemies;
        if (boss && &en != boss) continue; // (in a boss fight, the boss is the one to reach)
        // (height beyond 1.5 counts: one on the floor below a balcony is no nearer for being
        // under it; a crouching one is about 1 lower than one standing)
        const float below = 2 * std::max(0.0f, std::fabs(en.y - p.y) - 1.5f); // (double: one 4.6 under the
                                                                                 // balcony stays out of reach)
        s.nearest = std::min(s.nearest, std::hypot(std::hypot(en.x - p.x, en.z - p.z), below));
    }
    if (boss) s.boss_health = boss->health;
    if (s.enemies == 0) s.nearest = 0;
    return s;
}

// In a boss fight (the phase's boss bit), the boss: the enemy with the most health at full, by
// far (Mikhail 1500, his guards 200); only he needs to go down (the owner: "every stage the goal
// is to kill the boss"; guards keep coming). Else none.
const Character* boss_of(const Game& e, const std::vector<Character>& foes) {
    if (!(area_phase(e) & ram::kPhaseBoss)) return nullptr;
    const Character* boss = nullptr;
    for (const Character& en : foes)
        if (!boss || en.max_health > boss->max_health) boss = &en;
    return boss;
}

// Where the bot is headed: the boss in a boss fight, else the nearest enemy, else where the
// arrow points while the game asks for the gate.
std::optional<Character> target(const Game& e) {
    const Character p = character(e, me(e));
    if (g_gate_object >= 0 && g_gate_point && gate_object_standing(e)) { // (--gate-object: it first)
        Character gate;
        gate.x = (*g_gate_point)[0], gate.y = (*g_gate_point)[1], gate.z = (*g_gate_point)[2];
        return gate;
    }
    const std::vector<Character> foes = enemies(e);
    if (const Character* boss = boss_of(e, foes)) return *boss;
    std::optional<Character> best;
    for (const Character& en : foes)
        if (!best || std::hypot(en.x - p.x, en.z - p.z) < std::hypot(best->x - p.x, best->z - p.z)) best = en;
    if (const auto t = arrow_target(e); !best && t) {
        Character gate;
        gate.x = t->first, gate.y = gate_height(e), gate.z = t->second;
        best = gate;
    }
    return best;
}

// Better is more damage dealt, less taken, closer to the nearest enemy; at
// the gate, the arrow gone (the gate broken).
double value(const Score& before, const Score& after) {
    // (the boss's damage counts three times: the fight ends when he does)
    const double dealt = (before.enemy_health - after.enemy_health) + 2.0 * (before.boss_health - after.boss_health);
    const double taken = before.player_health - after.player_health;
    const double broken = before.arrow && !after.arrow ? 1000.0 : 0.0;
    // (at a gate within reach, nearer is no better: it takes more hits than a try lands, and
    // with nothing breaking it the walks won by a little; now ties go to the first, beat taps)
    // (a little, though: at Diesel Town's fence gate the bot beat the bars 1.65 from the door
    // until the time ran out; with this the moves that step and hit win, and it drifts there)
    const double near = (before.arrow && before.enemies == 0 ? 0.1 : 0.5) * after.nearest;
    // (falling off where the fight is costs a long way back: Mikhail's walkway, 6.7 up, "stay
    // on the center of the stage", the owner)
    const double fell = after.player_y < before.player_y - 1.5f ? 500.0 : 0.0;
    return 3.0 * dealt - 4.0 * taken - near + broken - fell;
}

// Walking to the nearest enemy (or the gate) when none is within reach:
// every route of two legs (a direction for kLeg frames, then another) is
// tried from a save state, and the first leg of the one that ends closest
// is walked (around what is in the way, which one leg toward it can't).
constexpr int kLeg = 30;
// (following a way: a jump's leg lasts until it lands: held, it crouches ~16 frames, takes off,
// and comes down ~80 frames after the press, the theatre's stage wall crossed)
constexpr int kWayLeg = 12, kWayJumpLeg = 80;
// Along a way, the lever with shift held (strafing: the guide's lock-on): 0.093 a frame at once and
// straight on, where a walk starts slowly, wanders and settles at 0.074 (from fullrec's 10704, 60
// frames: 5.39 against 3.64). The guide's run (the lever tapped twice, then held) never started,
// with taps and gaps of 1-12 and 1-16 frames. SPIKEBOT_WALK=1: walked, as before.
// Only while the target is further than kStrafeFrom: strafing keeps the way the player faced, and
// at stage 1's gate (fullrec4's 81415) he came up to it facing away, no try broke it, and he
// waited; the last of the way walked turns him to face it.
const bool g_strafe = std::getenv("SPIKEBOT_WALK") == nullptr;
constexpr float kStrafeFrom = 5.0f;
constexpr int kRunUp = 20; // (a jump's run-up: back this long, then toward the step)
constexpr float kReach = 4.0f, kGateReach = 2.5f; // (a gate's distance has its height in it: score())
const Button kWalk[8][2] = {{Up, Up}, {Up, Right}, {Right, Right}, {Down, Right},
                            {Down, Down}, {Down, Left}, {Left, Left}, {Up, Left}};

// The player held where he stands (a scene: stage 2's stairs show the floor
// above for 500 frames, then the player is put there): 20 frames' walking in
// each of the 4 straight ways moves him nowhere. Nothing to search then.
bool frozen(Workers& w, const std::shared_ptr<Snapshot>& snap) {
    const auto moved = w.try_all<float>(snap, 4, [](Game& b, std::size_t k) {
        const Character p0 = character(b, me(b));
        b.step(press({kWalk[k * 2][0], kWalk[k * 2][1]}), 20); // (20: in a corner he turns before he moves)
        const Character p = character(b, me(b));
        return std::hypot(p.x - p0.x, p.z - p0.z) + std::fabs(p.y - p0.y);
    });
    if (std::getenv("SPIKEBOT_VERBOSE"))
        std::printf("    held? moved %.3f %.3f %.3f %.3f\n", static_cast<double>(moved[0]), static_cast<double>(moved[1]),
                    static_cast<double>(moved[2]), static_cast<double>(moved[3]));
    return std::all_of(moved.begin(), moved.end(), [](float m) { return m < 0.01f; });
}

int navigate(Workers& w, const std::shared_ptr<Snapshot>& snap, float from, bool verbose) {
    // One leg each way first (an eighth of the cost): when the best gets well
    // closer, nothing is in the way; the two-leg routes only when it doesn't.
    constexpr float kClear = 1.5f; // (a leg walked in the open covers about 2.5)
    const std::vector<float> legs = w.try_all<float>(snap, 8, [](Game& b, std::size_t k) {
        b.step(press({kWalk[k][0], kWalk[k][1]}), kLeg);
        return score(b).nearest;
    });
    const std::size_t leg = static_cast<std::size_t>(std::min_element(legs.begin(), legs.end()) - legs.begin());
    if (verbose) std::printf("    one leg: best %zu, %.2f -> %.2f\n", leg, static_cast<double>(from), static_cast<double>(legs[leg]));
    if (from - legs[leg] >= kClear) return static_cast<int>(leg);
    const std::vector<float> ends = w.try_all<float>(snap, 64, [](Game& b, std::size_t k) {
        const std::size_t a = k / 8, c = k % 8;
        b.step(press({kWalk[a][0], kWalk[a][1]}), kLeg);
        b.step(press({kWalk[c][0], kWalk[c][1]}), kLeg);
        return score(b).nearest;
    });
    std::size_t pick = 0;
    for (std::size_t k = 0; k < ends.size(); ++k) {
        if (verbose) std::printf("    route %zu,%zu: nearest %.2f\n", k / 8, k % 8, static_cast<double>(ends[k]));
        if (ends[k] < ends[pick]) pick = k;
    }
    return static_cast<int>(pick / 8);
}

// Stuck (enemies near on the ground but out of reach: behind a kerb, on
// another level): explores. An archive of places reached (cells of the
// ground and height), each with the route there from the save state (a
// direction a leg); every round the places tried least (spreading over the
// whole area: the way round starts away from them), the nearest the nearest
// enemy of those first, are each tried in the 8 directions on the workers.
// Returns the route to the best place found (empty: none better). The way
// can open only after a while (stage 2's coast road: north past the kerb,
// then down a slope to the road), so a search that finds nothing is tried
// again when the player has been stuck as long again.
constexpr int kExploreRounds = 100, kExploreLeg = 20, kExplorePicks = 8, kExploreIdle = 6;
constexpr float kCell = 1.5f, kCellHeight = 0.5f;
constexpr float kClimbWorth = 4.0f; // (a unit up or down ranks as 4 nearer)
constexpr int kExploreStale = 10;    // rounds with nowhere nearer: then the furthest from the start first

// How far from the nearest enemy, on the ground (a crouching enemy's position
// is lower than one standing: height is no guide).
// A leg of a route: direction d & 7 for kExploreLeg frames, with a jump at
// its start when d >= 8 (onto crates and ledges a walk can't get up).
constexpr int kExploreWays = 16;
void explore_leg(Game& b, uint8_t d) {
    const Button a = kWalk[d & 7][0], c = kWalk[d & 7][1];
    if (d < 8) return b.step(press({a, c}), kExploreLeg);
    b.step(press({a, c, Jump}), kExploreLeg); // (held: higher; kJumpHold)
}

std::optional<std::pair<float, float>> g_goal; // (SPIKEBOT_GOAL=x,z: --explore-test heads there instead)
// --weak-enemies: the bot sets every enemy in play to 1 health each turn, so any hit knocks it out
// (the game's own knock-out still runs: its counts and time bonuses) - for getting through the
// stages, not for playing them.
bool g_weak_enemies = false;
// --idle-others (linked boards): only player 1 is played; the others stand where they are, no
// input, and are continued when knocked out (their cabinets stay in the shared game). The owner:
// player 1 alone clears a stage, the others need only be there.
bool g_idle_others = false;
// --record-inputs FILE: the main board's run (linked boards: every board's, tools::LinkedInputLog),
// saved with each checkpoint state and at the end.
tools::InputLog* g_input_log = nullptr;
tools::LinkedInputLog* g_linked_log = nullptr; // (linked boards)
std::string g_input_log_path;

float apart(const Game& e) {
    const Character p = character(e, me(e));
    if (g_goal) return std::hypot(g_goal->first - p.x, g_goal->second - p.z);
    float best = 1e9f;
    for (const Character& en : enemies(e)) best = std::min(best, std::hypot(en.x - p.x, en.z - p.z));
    return best;
}

std::vector<uint8_t> explore(Workers& w, const std::shared_ptr<Snapshot>& snap, float start, bool verbose) {
    struct Place {
        std::vector<uint8_t> route;
        float apart = 0;
        int tried = 0;
        float climb = 0; // (how far above or below the start: another level, a way the floor doesn't give)
        float away = 0;  // (from the start, on the ground)
    };
    std::map<std::tuple<int, int, int>, Place> archive;
    const Character p0 = w.try_all<Character>(snap, 1, [](Game& b, std::size_t) { return character(b, me(b)); })[0];
    const float y0 = p0.y;
    archive[{static_cast<int>(std::floor(p0.x / kCell)), static_cast<int>(std::floor(p0.z / kCell)),
             static_cast<int>(std::floor(p0.y / kCellHeight))}] = {{}, start, 0}; // (where it stands)
    const Place* best = &archive.begin()->second;
    std::size_t places = archive.size();
    float best_apart = start;
    for (int round = 0, idle = 0, stale = 0; round < kExploreRounds && best->apart > 1.0f && idle < kExploreIdle; ++round) {
        std::vector<Place*> picks;
        for (auto& [key, place] : archive) picks.push_back(&place);
        // The places tried least first (spreading over the whole area: the way round a kerb
        // or a wall starts away from them), the nearest of those first; another level
        // counts as nearer (stage 2's warehouse: a stair of crates, one jump a step, up
        // and over to the last two enemies). Once nowhere nearer has been found for
        // kExploreStale rounds, the furthest from the start first instead: the way can be
        // back where the player came from (the shipyard's gate, up the stairs before it).
        const bool spread = stale >= kExploreStale;
        auto rank = [spread](const Place* p) { return (spread ? -p->away : p->apart) - kClimbWorth * p->climb; };
        std::sort(picks.begin(), picks.end(), [&](const Place* a, const Place* b) {
            return a->tried != b->tried ? a->tried < b->tried : rank(a) < rank(b);
        });
        picks.resize(std::min<std::size_t>(picks.size(), kExplorePicks));
        struct Reached {
            std::vector<uint8_t> route;
            float x = 0, y = 0, z = 0, apart = 0;
        };
        for (Place* p : picks) ++p->tried;
        // Each place picked is walked to once and saved there; the 8 ways are
        // tried from that (a route walked again for each would cost 8 times).
        using Snap = std::shared_ptr<Snapshot>;
        const std::vector<Snap> at = w.try_all<Snap>(snap, picks.size(), [&](Game& b, std::size_t k) {
            for (uint8_t d : picks[k]->route) explore_leg(b, d);
            return b.save();
        });
        const std::vector<Reached> reached = w.try_from<Reached>([&](std::size_t k) { return at[k / kExploreWays]; },
                                                                 picks.size() * kExploreWays, [&](Game& b, std::size_t k) {
            const uint8_t d = static_cast<uint8_t>(k % kExploreWays);
            explore_leg(b, d);
            const Character p = character(b, me(b));
            std::vector<uint8_t> route = picks[k / kExploreWays]->route;
            route.push_back(d);
            return Reached{std::move(route), p.x, p.y, p.z, apart(b)};
        });
        for (const Reached& r : reached) {
            const std::tuple<int, int, int> key{static_cast<int>(std::floor(r.x / kCell)), static_cast<int>(std::floor(r.z / kCell)),
                                                static_cast<int>(std::floor(r.y / kCellHeight))};
            auto it = archive.find(key);
            if (it == archive.end() || r.route.size() < it->second.route.size())
                archive[key] = Place{r.route, r.apart, it == archive.end() ? 0 : it->second.tried, std::fabs(r.y - y0),
                                     std::hypot(r.x - p0.x, r.z - p0.z)};
        }
        best = &archive.begin()->second;
        for (const auto& [key, place] : archive)
            if (place.apart < best->apart) best = &place;
        stale = best->apart < best_apart - 0.5f ? 0 : stale + 1;
        best_apart = std::min(best_apart, best->apart);
        idle = archive.size() > places ? 0 : idle + 1; // (rounds that found nowhere new)
        places = archive.size();
        if (verbose)
            std::printf("    explore round %d: %zu places, best %.2f (%zu legs)\n", round, archive.size(),
                        static_cast<double>(best->apart), best->route.size());
    }
    if (verbose) {
        int x0 = 1 << 30, x1 = -(1 << 30), z0 = x0, z1 = x1;
        for (const auto& [key, place] : archive) {
            x0 = std::min(x0, std::get<0>(key)), x1 = std::max(x1, std::get<0>(key));
            z0 = std::min(z0, std::get<1>(key)), z1 = std::max(z1, std::get<1>(key));
        }
        const double c = kCell;
        std::printf("    explored x %.1f..%.1f, z %.1f..%.1f\n", x0 * c, (x1 + 1) * c, z0 * c, (z1 + 1) * c);
        // A plan of the cells reached (z down the page, x across): # reached, E an enemy, @ the start.
        using Cell = std::pair<int, int>;
        auto cell = [](const Character& ch) { return Cell{static_cast<int>(std::floor(ch.x / kCell)), static_cast<int>(std::floor(ch.z / kCell))}; };
        const auto seen = w.try_all<std::vector<Cell>>(snap, 1, [&](Game& b, std::size_t) {
            std::vector<Cell> v{cell(character(b, me(b)))}; // (the start first)
            for (const Character& en : enemies(b)) v.push_back(cell(en));
            return v;
        })[0];
        const Cell at0 = seen[0];
        const std::vector<Cell> foes(seen.begin() + 1, seen.end());
        for (const auto& f : foes) x0 = std::min(x0, f.first), x1 = std::max(x1, f.first), z0 = std::min(z0, f.second), z1 = std::max(z1, f.second);
        std::printf("    plan, x %.1f.. across, z %.1f.. down, cells of %.1f:\n", x0 * c, z0 * c, c);
        for (int z = z0; z <= z1; ++z) {
            std::string row;
            for (int x = x0; x <= x1; ++x) {
                char ch = '.';
                for (int h = -40; h <= 40 && ch == '.'; ++h)
                    if (archive.count({x, z, h})) ch = '#';
                if (std::make_pair(x, z) == at0) ch = '@';
                for (const auto& f : foes)
                    if (f == std::make_pair(x, z)) ch = 'E';
                row += ch;
            }
            std::printf("    %s\n", row.c_str());
        }
    }
    return best->apart < start - 0.5f ? best->route : std::vector<uint8_t>();
}

// Knocked out: "CONTINUE?" counts down from 9 for about 10 s. A coin
// (taken as a credit; a real player pays too), then start and beat in turn
// (start continues, beat picks the fighter shown on "SELECT A FIGHTER", the
// same one) until the player is back with health (or the time is up).
// Returns whether it was.
bool continue_game(Game& e, const std::string& out) {
    Inputs coin, start;
    coin.bank0 = static_cast<uint8_t>(0xFF & ~kCoin);
    start.bank0 = static_cast<uint8_t>(0xFF & ~kStart);
    e.step(coin, 4);
    e.step({}, 20);
    for (int tries = 0; tries < 60; ++tries) {
        // start for "CONTINUE?", then beat on "SELECT A FIGHTER" (the one shown: the same)
        e.step(tries % 2 ? press({Beat}) : start, 4);
        const bool shot = !out.empty() && tries % 3 == 0;
        e.step({}, 16, shot ? out + "/c-" + std::to_string(e.frame() + 16) + ".png" : std::string());
        if (character(e, me(e)).health > 0) return true;
    }
    return false;
}

// Fights: every `decide` frames, tries each move for `horizon` frames from a
// save state (on the workers) and plays the best for `decide` frames.
void play(Game& e, Workers& w, int frames, const std::string& out) {
    constexpr int kDecide = 10, kHorizon = 20, kFightHorizon = 55, kGateHorizon = 120;
    constexpr uint64_t kStateEvery = 300;
    constexpr int kKnockedOut = 120; // (until "CONTINUE?" shows)
    constexpr int kStuck = 300;      // frames with enemies about and no hit either way
    constexpr int kStuckGate = 150;  // frames no nearer the gate
    const std::vector<Move> all = moves();
    const uint64_t end = e.frame() + static_cast<uint64_t>(frames);
    uint64_t next_shot = e.frame(), next_state = e.frame();
    const bool verbose = std::getenv("SPIKEBOT_VERBOSE") != nullptr;
    auto checkpoint = [&](const std::shared_ptr<Snapshot>& snap) { // one the window can load
        if (out.empty() || snap->frame < next_state) return;
        e.write_state(*snap, out + "/bot-" + std::to_string(snap->frame) + ".state");
        if (g_input_log) g_input_log->save(g_input_log_path);
        if (g_linked_log) g_linked_log->save(g_input_log_path);
        next_state += kStateEvery;
    };
    // What the bot keeps about each player it plays (linked boards: one each; their turns
    // alternate, each player's inputs held meanwhile as its last turn left them).
    struct Mind {
        int down_for = 0; // frames with no health (a knock-out shows "K.O." first)
        int still_for = 0, last_enemy_health = -1, last_player_health = -1; // (nothing hit either way)
        float closest = 1e9f;   // (while still: the nearest they have been; getting closer is not stuck)
        uint64_t turn_start = 0; // (the frames since the last turn: walking takes longer than fighting)
        int no_hits = 0;  // (fight turns in a row where no try hurt anyone)
        int walk_for = 0; // (turns left walking the way though within reach)
        int way_stalled = 0; // (turns following a way no nearer the point headed for)
        Waypoint way_aim{0, 0, 0, false};
        float way_best = 1e9f;
    };
    std::vector<Mind> minds(e.boards());
    for (Mind& m : minds) m.turn_start = e.frame();
    uint64_t turns = 0;
    int last_phase = -1; // (each change logged: where the stages and areas begin)
    constexpr int kNoHits = 6, kWalkFor = 10;
    constexpr int kWayStalled = 6;
    uint64_t playing_at = e.frame(); // (the last frame a game was being played)
    while (e.frame() < end && e.stopped().empty()) {
        if (g_idle_others) // (the others: continued when knocked out, otherwise left alone)
            for (std::size_t q = 1; q < e.boards(); ++q) {
                e.control(q);
                if (character(e, me(e)).health == 0 && (minds[q].down_for += kDecide) >= kKnockedOut) {
                    const bool back = continue_game(e, {});
                    std::printf("frame %llu: %sknocked out; %s\n", static_cast<unsigned long long>(e.frame()), who(e).c_str(),
                                back ? "continued" : "no continue");
                    minds[q].down_for = 0;
                }
                e.hold(q, {});
            }
        const std::size_t player = g_idle_others ? 0 : turns++ % e.boards();
        e.control(player);
        Mind& mind = minds[player];
        int& down_for = mind.down_for;
        int &still_for = mind.still_for, &last_enemy_health = mind.last_enemy_health, &last_player_health = mind.last_player_health;
        float& closest = mind.closest;
        uint64_t& turn_start = mind.turn_start;
        int &no_hits = mind.no_hits, &walk_for = mind.walk_for, &way_stalled = mind.way_stalled;
        Waypoint& way_aim = mind.way_aim;
        float& way_best = mind.way_best;
        if (g_weak_enemies) // (--weak-enemies: each turn, every enemy in play down to 1 health, on
                            // every board: their copies differ, and cabinets that disagree about a
                            // kill stop with a communication error)
            for (std::size_t board = 0; board < e.boards(); ++board)
                for (int i = 0; i < ram::kEnemySlots; ++i) {
                    const uint32_t base = ram::kEnemies + static_cast<uint32_t>(i) * ram::kSlot;
                    if (e.read16_on(board, base + ram::kLive) >> 8 != 0 && e.read16_on(board, base + ram::kHealth) > 1)
                        e.write16_on(board, base + ram::kHealth, 1);
                }
        if (e.read32(ram::kPlaying) != 0) playing_at = e.frame();
        else if (e.frame() - playing_at > 600) {
            std::printf("frame %llu: game over (the attract mode)\n", static_cast<unsigned long long>(e.frame()));
            break;
        }
        if (const int phase = area_phase(e); phase != last_phase) {
            const Character p = character(e, me(e));
            std::printf("frame %llu: area phase %d, player at %.1f %.1f %.1f\n", static_cast<unsigned long long>(e.frame()), phase,
                        static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
            last_phase = phase;
            g_barred.clear(); // (a new area, or the same one changed: try everywhere again)
            // --unmarked-gate: with the gate called, a gate other than the arrow's (its walls'
            // middle; the arrow's height), the nearest if more
            g_gate_point.reset();
            if (g_unmarked_gate && (phase & ram::kPhaseGate)) {
                const float ax = e.readf_on(0, ram::kArrowTarget), ay = e.readf_on(0, ram::kArrowTarget + 4),
                            az = e.readf_on(0, ram::kArrowTarget + 8);
                float best = 1e9f;
                for (const GateObject& g : gate_objects(e, read_floor(e))) {
                    if (!(g.flags & 0x01000000u) || (g.flags & 0x04000000u) || std::hypot(g.x - ax, g.z - az) < 5.0f) continue;
                    if (const float d = std::hypot(g.x - p.x, g.z - p.z); d < best) best = d, g_gate_point = std::array<float, 3>{g.x, ay, g.z};
                }
                if (g_gate_point)
                    std::printf("frame %llu: the unmarked gate at %.1f %.1f (the arrow's at %.1f %.1f)\n",
                                static_cast<unsigned long long>(e.frame()), static_cast<double>((*g_gate_point)[0]),
                                static_cast<double>((*g_gate_point)[2]), static_cast<double>(ax), static_cast<double>(az));
            }
        }
        // --gate-object N: its place once it stands (the script registers it), at about the
        // arrow's height over its walls (2.5)
        if (g_gate_object >= 0 && !g_gate_point && gate_object_standing(e))
            for (const GateObject& g : gate_objects(e, read_floor(e)))
                if (g.number == g_gate_object) {
                    g_gate_point = std::array<float, 3>{g.x, g.y + 2.5f, g.z};
                    std::printf("frame %llu: gate object %02x at %.1f %.1f\n", static_cast<unsigned long long>(e.frame()), g.number,
                                static_cast<double>(g.x), static_cast<double>(g.z));
                }
        const Score now = score(e);
        const int turn = static_cast<int>(e.frame() - turn_start);
        turn_start = e.frame();
        if (now.player_health == 0) {
            if ((down_for += kDecide) < kKnockedOut) {
                e.step({}, kDecide);
                continue;
            }
            const bool back = continue_game(e, out);
            std::printf("frame %llu: knocked out; %s\n", static_cast<unsigned long long>(e.frame()),
                        back ? "continued" : "no continue (game over)");
            down_for = 0;
            if (!back) break;
            continue;
        }
        down_for = 0;
        // Nothing happening with enemies about (out of reach), or no nearer the gate: explore a
        // way there. (At the gate it's sooner: 30 seconds to break it, or the game is over; the
        // shipyard's gate is a floor up, round a long way.)
        const bool to_gate = now.enemies == 0 && now.arrow;
        if ((now.enemies > 0 || to_gate) && now.enemy_health == last_enemy_health && now.player_health == last_player_health &&
            now.nearest > closest - 1.0f) {
            if ((still_for += turn) >= (to_gate ? kStuckGate : kStuck)) {
                auto snap = e.save();
                // Not in a scene, nor when within reach (keep fighting: the opera house's hall timed
                // out while a search took the player 42 legs away from two enemies 1.6 off), nor
                // when the floor grid has a way there (following it, steps that fail are barred and
                // planned round).
                const Character pl = character(e, me(e));
                const std::optional<Character> t = target(e);
                const bool no_search = now.nearest <= (to_gate ? kGateReach : kReach) ||
                                       (t && plan_way(read_floor(e), pl.x, pl.y, pl.z, t->x, t->y, t->z).size() > 1) ||
                                       frozen(w, snap);
                if (no_search) still_for = 0;
            }
            if (still_for >= (to_gate ? kStuckGate : kStuck)) {
                auto snap = e.save();
                if (to_gate) g_goal = arrow_target(e);
                const float start = apart(e);
                const std::vector<uint8_t> route = explore(w, snap, start, verbose);
                g_goal.reset();
                std::printf("frame %llu: stuck %.1f from %s; explored a route of %zu legs\n",
                            static_cast<unsigned long long>(e.frame()), static_cast<double>(start),
                            to_gate ? "the gate" : "them", route.size());
                for (uint8_t d : route) explore_leg(e, d);
                still_for = 0;
                turn_start = e.frame(); // (the route walked isn't time stuck)
                closest = score(e).nearest;
                continue;
            }
        } else {
            still_for = 0;
            closest = now.nearest;
        }
        last_enemy_health = now.enemy_health;
        last_player_health = now.player_health;
        if (now.enemies == 0 && !now.arrow) { // nobody about, no gate yet (the next ones coming): no search
            if (!out.empty() && e.frame() >= next_state) checkpoint(e.save());
            e.step({}, kDecide,
                   !out.empty() && e.frame() + kDecide >= next_shot ? out + "/p-" + std::to_string(e.frame() + kDecide) + ".png"
                                                                    : std::string());
            if (!out.empty() && e.frame() >= next_shot) next_shot += 60;
            std::printf("frame %llu: wait (nobody)\n", static_cast<unsigned long long>(e.frame()));
            continue;
        }
        auto snap = e.save(); // (the game waits there: the tries start from it, then the move chosen)
        checkpoint(snap);
        // Too far to fight (or from the gate: nearer, its distance counting height) - walk there.
        // Also when no try has hurt anyone for kNoHits turns: within reach on the map, but across
        // the opera house's balcony railing (2,000 frames swung at the air there).
        if (no_hits >= kNoHits) no_hits = 0, walk_for = kWalkFor; // (walk on a while, then try fighting again)
        // At a gate the way runs out against the gate itself (the theatre's: a wall at the stage's
        // front, the arrow 7 behind it): there, hit it.
        bool gate_reached = false;
        if (to_gate && now.nearest > kGateReach) {
            const Character p = character(e, me(e));
            if (const std::optional<Character> t = target(e))
                gate_reached = plan_way(read_floor(e), p.x, p.y, p.z, t->x, t->y, t->z).size() <= 2;
        }
        if ((now.nearest > (to_gate ? kGateReach : kReach) && !gate_reached) || (now.enemies > 0 && walk_for > 0)) {
            if (walk_for > 0) --walk_for;
            if (frozen(w, snap)) {
                // With enemies within reach it's a hold, not a scene: struggle (beat taps free the
                // player; waiting, stage 1's boss held him until the time ran out).
                // (any enemy's distance: in a boss fight `nearest` is the boss's, and one of the others
                // held the player while the bot waited for a scene to end; read now, and not apart(),
                // which gives the gate's distance on the way to one: stage 2's fence gate, a grabber
                // came after the area's last enemy and held the player until the time ran out; nor
                // enemies(), empty once the area's phase says it's over, as it did then)
                const Character p = character(e, me(e));
                float held_by = 1e9f;
                for (int i = 0; i < ram::kEnemySlots; ++i) {
                    const Character en = character(e, ram::kEnemies + static_cast<uint32_t>(i) * ram::kSlot);
                    if (en.max_health > 0 && en.health > 0 && e.read16(en.base + ram::kLive) >> 8 != 0)
                        held_by = std::min(held_by, std::hypot(en.x - p.x, en.z - p.z));
                }
                if (held_by <= kReach) {
                    walk_for = 0;
                    for (int i = 0; i < kDecide; ++i) e.step(i % 6 < 2 ? press({Beat}) : press({i % 4 < 2 ? Left : Right}));
                    std::printf("frame %llu: %sstruggle (held)\n", static_cast<unsigned long long>(e.frame()), who(e).c_str());
                    continue;
                }
                e.step({}, kLeg); // (a scene: no search)
                std::printf("frame %llu: %swait (held)\n", static_cast<unsigned long long>(e.frame()), who(e).c_str());
                continue;
            }
            // A way over the area's floor (the game's own grid), when there is one: each turn
            // the 8 directions walked and jumped are tried, and the one ending nearest the next
            // point on the way is played.
            const Character p = character(e, me(e));
            if (const std::optional<Character> t = target(e)) {
                const std::vector<Waypoint> way = plan_way(read_floor(e), p.x, p.y, p.z, t->x, t->y, t->z);
                if (const std::optional<Waypoint> aim = next_on_way(way, p.x, p.y, p.z)) {
                    // Legs 0-7 walk; 8-15 jump; for a point jumped up to, 16 on: a run-up (a jump
                    // keeps the speed it starts with, and no more: from a standstill it goes
                    // straight up, and from a walk it covers 0.8 in 40 frames) back the other way
                    // for kRunUp frames, then toward it for 2, 4, ... 24 and jump: it must leave
                    // the ground moving, just short of the step's edge.
                    const std::size_t tries = aim->jump ? 16 + 8 * 12 : 16;
                    auto leg = [strafe = g_strafe && now.nearest > kStrafeFrom](Game& b, std::size_t k, const std::string& shot) {
                        if (k >= 16) {
                            const std::size_t to = (k - 16) % 8, away = (to + 4) % 8;
                            const int run_in = 2 * static_cast<int>(1 + (k - 16) / 8);
                            b.step(press({kWalk[away][0], kWalk[away][1]}), kRunUp);
                            b.step(press({kWalk[to][0], kWalk[to][1]}), run_in);
                            k = to + 8;
                        }
                        const Button a = kWalk[k & 7][0], c = kWalk[k & 7][1];
                        if (k < 8) return b.step(strafe ? press({a, c, Shift}) : press({a, c}), kWayLeg, shot);
                        b.step(press({a, c, Jump}), kJumpHold);
                        b.step(strafe ? press({a, c, Shift}) : press({a, c}), kWayJumpLeg - kJumpHold, shot);
                    };
                    auto off = [&aim](const Game& b) {
                        const Character q = character(b, me(b));
                        return std::hypot(aim->x - q.x, aim->z - q.z) + 2 * std::fabs(aim->y - q.y);
                    };
                    const float before = off(e);
                    const std::vector<float> ends = w.try_all<float>(snap, tries, [&](Game& b, std::size_t k) {
                        leg(b, k, {});
                        return off(b);
                    });
                    const std::size_t k = static_cast<std::size_t>(std::min_element(ends.begin(), ends.end()) - ends.begin());
                    leg(e, k, out.empty() ? std::string() : out + "/p-" + std::to_string(e.frame()) + ".png");
                    if (off(e) < before - 0.2f) still_for = 0, closest = score(e).nearest; // (on its way: not stuck)
                    // Progress toward this aim: against the best reached for it (the distance wobbles
                    // at a railing, 9.3 to 9.9, and each dip looked like progress).
                    // (an aim within a unit is the same one: at the balcony it moved by half a cell a turn)
                    if (std::hypot(aim->x - way_aim.x, aim->z - way_aim.z) > 1.0f || std::fabs(aim->y - way_aim.y) > 0.5f)
                        way_aim = *aim, way_best = before, way_stalled = 0;
                    // (a jump at a point jumped up to that gets no nearer bars it at once: the opera
                    // house's pole, "jumped" over on the map, the bot back where it was every time)
                    const bool jump_failed = aim->jump && k >= 8 && off(e) >= before - 0.3f;
                    if (!jump_failed && off(e) < way_best - 0.3f) {
                        way_best = off(e);
                        way_stalled = 0;
                    } else if (jump_failed || ++way_stalled >= kWayStalled) { // (that step can't be made: plan round it)
                        const Floor f = read_floor(e);
                        for (int dz = -1; dz <= 1; ++dz) // (and the cells round it: the next try would be one along)
                            for (int dx = -1; dx <= 1; ++dx)
                                if (const int c = f.cell(aim->x + static_cast<float>(dx) / f.scale, aim->z + static_cast<float>(dz) / f.scale);
                                    c >= 0)
                                    g_barred.insert(c);
                        std::printf("frame %llu: way barred at %.1f %.1f %.1f\n", static_cast<unsigned long long>(e.frame()),
                                    static_cast<double>(aim->x), static_cast<double>(aim->y), static_cast<double>(aim->z));
                        way_stalled = 0;
                    }
                    const Character q = character(e, me(e));
                    std::printf("frame %llu: %sway %zu%s (%zu points, %.1f to the next) nearest %.1f\n",
                                static_cast<unsigned long long>(e.frame()), who(e).c_str(), k < 16 ? k & 7 : (k - 16) % 8,
                                k < 8 ? "" : k < 16 ? " jumping" : (" jumping after a run-up of " + std::to_string(2 * (1 + (k - 16) / 8))).c_str(),
                                way.size(), static_cast<double>(off(e)), static_cast<double>(score(e).nearest));
                    if (verbose)
                        std::printf("    at %.2f %.2f %.2f, aimed at %.2f %.2f %.2f%s\n", static_cast<double>(q.x), static_cast<double>(q.y),
                                    static_cast<double>(q.z), static_cast<double>(aim->x), static_cast<double>(aim->y),
                                    static_cast<double>(aim->z), aim->jump ? " (jumped up to)" : "");
                    continue;
                }
            }
            const int dir = navigate(w, snap, now.nearest, verbose);
            e.step(press({kWalk[dir][0], kWalk[dir][1]}), kLeg,
                   out.empty() ? std::string() : out + "/p-" + std::to_string(e.frame() + kLeg) + ".png");
            const Score after = score(e);
            std::printf("frame %llu: %swalk %d       hp %d, enemies %d (hp %d), nearest %.1f\n",
                        static_cast<unsigned long long>(e.frame()), who(e).c_str(), dir, after.player_health, after.enemies,
                        after.enemy_health, static_cast<double>(after.nearest));
            if (verbose) print_state(e);
            continue;
        }
        // Each move tried for the same time: its own inputs, then beat taps (a charge takes longer
        // to land than a tap; scored on 20 frames it never paid). SPIKEBOT_OLDFIGHT=1: 20 frames of
        // each move's own inputs, no charges (as before, for comparing).
        // (with --weak-enemies too: any hit knocks one out, so quick hits, not charges; the owner)
        static const bool old_fight = std::getenv("SPIKEBOT_OLDFIGHT") != nullptr || g_weak_enemies;
        const bool gate = now.enemies == 0 && now.arrow;
        const int horizon = gate ? kGateHorizon : old_fight ? kHorizon : kFightHorizon; // (the gate takes longer to show)
        auto tap = [](int i) { return i % 6 < 2 ? press({Beat}) : Inputs{}; };
        auto skipped = [&](std::size_t k) { return old_fight && all[k].length > kHorizon; };
        const std::vector<Score> tried = w.try_all<Score>(snap, all.size(), [&](Game& b, std::size_t k) {
            if (skipped(k)) return score(b);
            for (int i = 0; i < horizon; ++i) b.step(gate || old_fight || i < all[k].length ? all[k].inputs(i) : tap(i));
            return score(b);
        });
        double best = -1e18;
        std::size_t pick = 0;
        const bool any_hit = std::any_of(tried.begin(), tried.end(), [&](const Score& s) { return s.enemy_health < now.enemy_health; });
        no_hits = any_hit || gate ? 0 : no_hits + 1;
        for (std::size_t k = 0; k < all.size(); ++k) {
            if (skipped(k)) continue;
            const double v = value(now, tried[k]);
            if (verbose)
                std::printf("    %-12s value %8.2f  hp %d enemy hp %d nearest %.2f\n", all[k].name, v, tried[k].player_health,
                            tried[k].enemy_health, static_cast<double>(tried[k].nearest));
            if (v > best) best = v, pick = k;
        }
        const int played = all[pick].length > kHorizon ? all[pick].length : kDecide; // (a charge whole)
        const bool shot = !out.empty() && e.frame() + static_cast<uint64_t>(played) >= next_shot;
        for (int i = 0; i < played; ++i)
            e.step(all[pick].inputs(i), 1,
                   shot && i == played - 1 ? out + "/p-" + std::to_string(e.frame() + 1) + ".png" : std::string());
        if (shot) next_shot += 60;
        if (e.boards() > 1) // (while the others take their turns, this player keeps at the move, frame by frame)
            e.hold(player, [move = all[pick], start = e.frame() - static_cast<uint64_t>(played)](uint64_t f) {
                return move.inputs(static_cast<int>(f - start));
            });
        const Score after = score(e);
        std::printf("frame %llu: %s%-12s hp %d, enemies %d (hp %d), nearest %.1f\n", static_cast<unsigned long long>(e.frame()),
                    e.boards() > 1 ? ("P" + std::to_string(player + 1) + " ").c_str() : "", all[pick].name, after.player_health,
                    after.enemies, after.enemy_health, static_cast<double>(after.nearest));
        if (verbose && after.enemies > 0 && after.nearest < 0.05f) print_state(e);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // (a long run's log written as it goes; Windows has no line buffering)
    const char* dir = nullptr;
    std::string load, out, route_spec;
    int frames = 3000, inspect = 0;
    bool with_sound = true, play_mode = false, selftest = false, explore_test = false, moves_test = false, bench = false, floor_map = false, gates_list = false;
    unsigned dump_lo = 0, dump_hi = 0;
    std::size_t boards = 1, player_arg = 0;
    int workers = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() - 1));
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--load-state" && i + 1 < argc) load = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (arg == "--inspect" && i + 1 < argc) inspect = std::atoi(argv[++i]);
        else if (arg == "--no-sound") with_sound = false;
        else if (arg == "--play") play_mode = true;
        else if (arg == "--selftest") selftest = true;
        else if (arg == "--explore-test") explore_test = true;
        else if (arg == "--moves-test") moves_test = true;
        else if (arg == "--bench") bench = true;
        else if (arg == "--floor-map") floor_map = true;
        else if (arg == "--gates") gates_list = true;
        else if (arg == "--unmarked-gate") g_unmarked_gate = true;
        else if (arg == "--gate-object" && i + 1 < argc) g_gate_object = static_cast<int>(std::strtol(argv[++i], nullptr, 16));
        else if (arg == "--weak-enemies") g_weak_enemies = true;
        else if (arg == "--idle-others") g_idle_others = true;
        else if (arg == "--record-inputs" && i + 1 < argc) g_input_log_path = argv[++i];
        else if (arg == "--route" && i + 1 < argc) route_spec = argv[++i];
        else if (arg == "--workers" && i + 1 < argc) workers = std::atoi(argv[++i]);
        else if (arg == "--boards" && i + 1 < argc) boards = std::max<std::size_t>(1, std::strtoull(argv[++i], nullptr, 0));
        else if (arg == "--player" && i + 1 < argc) player_arg = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--dump" && i + 1 < argc) std::sscanf(argv[++i], "%x-%x", &dump_lo, &dump_hi);
        else if (!dir) dir = argv[i];
    }
    if (!dir || load.empty()) {
        std::fprintf(stderr, "usage: spikebot <rom_dir> --load-state FILE [--out DIR] [--frames N] [--no-sound]\n"
                             "                [--play] [--workers N] [--inspect EVERY] [--dump LO-HI]\n"
                             "                [--route D:N,...] [--explore-test] [--moves-test] [--floor-map] [--weak-enemies]\n"
                             "                [--bench] [--selftest]\n");
        return 2;
    }
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const m3::RomSet roms = m3::load_rom_set(game, dir);
    if (!roms.complete()) {
        std::fprintf(stderr, "spikebot: ROM set incomplete; run romcheck for details\n");
        return 1;
    }
    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();
    const std::vector<recomp::FunctionEntry> blocks = tools::game_blocks();
    // One board, or linked boards (--boards N: a linked save state, gamerun --boards N's).
    auto make = [&]() -> std::unique_ptr<Game> {
        if (boards > 1) return std::make_unique<Rig>(boards, roms, functions, blocks);
        return std::make_unique<Emulator>(roms, functions, blocks, with_sound);
    };
    const std::unique_ptr<Game> main_game = make();
    Game& e = *main_game;
    tools::InputLog input_log;
    tools::LinkedInputLog linked_log;
    if (!g_input_log_path.empty()) {
        if (boards > 1) static_cast<Rig&>(e).record_into(g_linked_log = &linked_log);
        else static_cast<Emulator&>(e).record_into(g_input_log = &input_log);
    }
    if (!e.start(load)) return 1;
    std::printf("Loaded %s at frame %llu\n", load.c_str(), static_cast<unsigned long long>(e.frame()));
    e.control(player_arg); // (--player N: linked boards, the board read and driven by the tools below)

    if (inspect > 0) {
        for (int done = 0; done < frames && e.stopped().empty(); done += inspect) {
            e.step({}, inspect, out.empty() ? std::string() : out + "/i-" + std::to_string(e.frame() + inspect) + ".png");
            print_state(e);
        }
    }
    if (dump_hi > dump_lo) { // --dump LO-HI: RAM as words and floats
        for (uint32_t a = dump_lo & ~15u; a < dump_hi; a += 16) {
            std::printf("%06x:", a);
            for (uint32_t k = 0; k < 16; k += 4)
                std::printf(" %04x%04x", e.read16(a + k), e.read16(a + k + 2));
            std::printf("  ");
            for (uint32_t k = 0; k < 16; k += 4) {
                const float f = e.readf(a + k);
                if (std::fabs(f) > 1e-3f && std::fabs(f) < 1e5f) std::printf(" %9.2f", static_cast<double>(f));
                else std::printf("         .");
            }
            std::printf("\n");
        }
    }
    if (!route_spec.empty()) { // --route D:N,D:N...: walk direction D (0 up, 2 right, 4 down, 6 left) for N frames each
        int leg = 0;
        for (std::size_t at = 0; at < route_spec.size();) {
            int d = 0, n = 0, used = 0;
            if (std::sscanf(route_spec.c_str() + at, "%d:%d%n", &d, &n, &used) != 2) break;
            at += static_cast<std::size_t>(used) + 1;
            for (int i = 0; i < n; i += 20) { // (direction 8: no input; 9: beat taps where it stands; 10-17: 0-7 jumping first)
                if (d >= 30 && d < 38) { // (30-37: 0-7 run: tapped, let go, then held)
                    const Inputs lever = press({kWalk[d - 30][0], kWalk[d - 30][1]});
                    if (i == 0) e.step(lever, kRunTap), e.step({}, kRunTap);
                    e.step(lever, std::min(20, n - i) - (i == 0 ? 2 * kRunTap : 0));
                } else if (d >= 20 && d < 28) { // (20-27: 0-7 with jump held: held longer, it goes higher)
                    const Button a = kWalk[d - 20][0], c = kWalk[d - 20][1];
                    e.step(press({a, c, Jump}), std::min(20, n - i));
                } else if (d >= 10 && d < 18) {
                    const Button a = kWalk[d - 10][0], c = kWalk[d - 10][1];
                    e.step(i == 0 ? press({a, c, Jump}) : press({a, c}), 2);
                    e.step(press({a, c}), std::min(20, n - i) - 2);
                } else if (d == 9)
                    for (int k = 0; k < std::min(20, n - i); ++k) e.step(k % 6 < 2 ? press({Beat}) : Inputs{});
                else e.step(d >= 8 ? Inputs{} : press({kWalk[d & 7][0], kWalk[d & 7][1]}), std::min(20, n - i));
                const Character p = character(e, me(e));
                float near = 1e9f;
                for (const Character& en : enemies(e)) near = std::min(near, std::hypot(en.x - p.x, en.z - p.z));
                std::printf("leg %d dir %d: (%.1f %.2f %.1f) nearest %.2f\n", leg, d, static_cast<double>(p.x),
                            static_cast<double>(p.y), static_cast<double>(p.z), static_cast<double>(near));
            }
            if (!out.empty()) { // (the picture on one more frame of the leg's own inputs: a release would end a held jump)
                Inputs held;
                if (d >= 20 && d < 28) held = press({kWalk[d - 20][0], kWalk[d - 20][1], Jump});
                else if (d >= 10 && d < 18) held = press({kWalk[d - 10][0], kWalk[d - 10][1]});
                else if (d < 8) held = press({kWalk[d][0], kWalk[d][1]});
                e.step(held, 1, out + "/route-" + std::to_string(leg) + ".png");
            }
            ++leg;
        }
        print_state(e);
    }
    if (gates_list) { // --gates: the area's objects with walls (gate_objects), where they are
        const Character p = character(e, me(e));
        std::printf("player at %.2f %.2f %.2f\n", static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
        for (const GateObject& g : gate_objects(e, read_floor(e)))
            std::printf("object %02x: flags %08x%s, %d walls round %.2f %.2f %.2f (%.1f away)\n", g.number, g.flags,
                        g.flags & 0x04000000u ? " (broken)" : g.flags & 0x01000000u ? " (a gate)" : "", g.walls, static_cast<double>(g.x),
                        static_cast<double>(g.y), static_cast<double>(g.z), static_cast<double>(std::hypot(g.x - p.x, g.z - p.z)));
        if (const auto t = arrow_target(e)) std::printf("arrow to %.2f %.2f\n", static_cast<double>(t->first), static_cast<double>(t->second));
    }
    if (floor_map) { // the area's floor grid (docs/bot.md): its header, and a map round the player
        const Floor f = read_floor(e);
        const Character p = character(e, me(e));
        std::printf("floor: x %.2f..%.2f z %.2f..%.2f, %d x %d cells of %.3f\n", static_cast<double>(f.x0), static_cast<double>(f.x1),
                    static_cast<double>(f.z0), static_cast<double>(f.z1), f.columns, f.rows, static_cast<double>(1 / f.scale));
        if (f.valid()) {
            const int i = f.cell(p.x, p.z);
            std::printf("player at %.2f %.2f %.2f, cell %d:", static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z), i);
            for (float h : f.heights(p.x, p.z)) std::printf(" %.2f", static_cast<double>(h));
            std::printf("\n");
            std::printf("walls: %zu; in his cell:", f.walls.size());
            if (i >= 0)
                for (uint16_t n : f.cell_walls[static_cast<std::size_t>(i)]) {
                    if (n >= f.walls.size()) continue;
                    const Floor::Wall& wl = f.walls[n];
                    std::printf("\n  %u: dir %.2f %.2f u %.2f..%.2f v %.2f y %.2f %.2f +%.2f %.2f (he is at u %.2f v %.2f)", n,
                                static_cast<double>(wl.c), static_cast<double>(wl.s), static_cast<double>(wl.u0),
                                static_cast<double>(wl.u1), static_cast<double>(wl.d), static_cast<double>(wl.y0),
                                static_cast<double>(wl.y1), static_cast<double>(wl.h0), static_cast<double>(wl.h1),
                                static_cast<double>(p.x * wl.s - p.z * wl.c), static_cast<double>(p.z * wl.s + p.x * wl.c));
                }
            std::printf("\n");
            // The planes of the cells round him, or round SPIKEBOT_PROBE=x,z (flags, and the
            // height at the cell's middle).
            float px = p.x, pz = p.z;
            if (const char* probe = std::getenv("SPIKEBOT_PROBE")) std::sscanf(probe, "%f,%f", &px, &pz);
            for (int dz = -2; dz <= 4; ++dz) {
                const float z = pz + static_cast<float>(dz) / f.scale;
                const int c = f.cell(px, z);
                if (c < 0) continue;
                std::printf("  z %.2f:", static_cast<double>(z));
                for (const Floor::Plane& q : f.cells[static_cast<std::size_t>(c)])
                    std::printf(" [%08x %.2f]", q.flags, static_cast<double>(q.d + 0.5f * (q.a + q.c)));
                std::printf("\n");
            }
            // Each cell: the floor nearest the player's height: '.' level (within 0.4), a digit how many
            // units above (1-2), 'v' below by up to 2, '-' further below, ' ' no floor at all.
            const float step = 1 / f.scale;
            std::vector<Character> foes = enemies(e);
            // The way to the nearest enemy (or where the arrow points), drawn '*', 'J' where jumped up.
            std::vector<Waypoint> way;
            {
                float tx = 0, ty = p.y, tz = 0, best = 1e9f;
                for (const Character& en : foes)
                    if (const float d = std::hypot(en.x - p.x, en.z - p.z); d < best) best = d, tx = en.x, ty = en.y, tz = en.z;
                if (best > 1e8f) {
                    tx = e.readf_on(0, ram::kArrowTarget), ty = e.readf_on(0, ram::kArrowTarget + 4), tz = e.readf_on(0, ram::kArrowTarget + 8);
                }
                way = plan_way(f, p.x, p.y, p.z, tx, ty, tz);
                std::printf("way to %.2f %.2f %.2f: %zu points", static_cast<double>(tx), static_cast<double>(ty), static_cast<double>(tz),
                            way.size());
                if (!way.empty())
                    std::printf(", ending at %.2f %.2f %.2f", static_cast<double>(way.back().x), static_cast<double>(way.back().y),
                                static_cast<double>(way.back().z));
                std::printf("\n");
            }
            for (float z = p.z - 30 * step; z <= p.z + 30 * step; z += step) {
                std::string row;
                for (float x = p.x - 50 * step; x <= p.x + 50 * step; x += step) {
                    const std::vector<float> hs = f.heights(x, z);
                    char ch = ' ';
                    float nearest = 1e9f;
                    for (float h : hs)
                        if (std::fabs(h - p.y) < std::fabs(nearest)) nearest = h - p.y;
                    if (!hs.empty())
                        ch = std::fabs(nearest) < 0.4f ? '.' : nearest > 0 ? (nearest < 2.5f ? static_cast<char>('0' + std::lround(nearest)) : '^')
                                                       : (nearest > -2.0f ? 'v' : '-');
                    if (const int c = f.cell(x, z); c >= 0)
                        for (uint16_t n : f.cell_walls[static_cast<std::size_t>(c)]) {
                            if (n >= f.walls.size()) continue;
                            const Floor::Wall& wl = f.walls[n];
                            const float v = z * wl.s + x * wl.c - wl.d, u = x * wl.s - z * wl.c;
                            if (std::fabs(v) < step * 0.75f && u >= std::min(wl.u0, wl.u1) && u <= std::max(wl.u0, wl.u1) &&
                                p.y < std::max(wl.y0 + wl.h0, wl.y1 + wl.h1) && p.y + Floor::kBody > std::min(wl.y0, wl.y1))
                                ch = '#';
                        }
                    for (const Waypoint& w : way)
                        if (std::fabs(x - w.x) < step / 2 && std::fabs(z - w.z) < step / 2) ch = w.jump ? 'J' : '*';
                    if (std::fabs(x - p.x) < step / 2 && std::fabs(z - p.z) < step / 2) ch = '@';
                    for (const Character& en : foes)
                        if (std::fabs(x - en.x) < step / 2 && std::fabs(z - en.z) < step / 2) ch = 'E';
                    row += ch;
                }
                std::printf("%s\n", row.c_str());
            }
        }
    }
    if (bench) { // what the searches cost: frames run, saves and loads, alone and on the workers
        using clock = std::chrono::steady_clock;
        auto secs = [](clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); };
        auto t = clock::now();
        e.step({}, 120);
        std::printf("bench: 120 frames on the board: %.1f frames/s\n", 120 / secs(t));
        t = clock::now();
        std::shared_ptr<Snapshot> snap;
        for (int i = 0; i < 20; ++i) snap = e.save(), e.step({});
        std::printf("bench: a save (and a frame): %.2f ms, %zu bytes\n", secs(t) * 1000 / 20, snap->data.size());
        t = clock::now();
        for (int i = 0; i < 20; ++i) e.load(snap), e.step({});
        std::printf("bench: a load (and a frame): %.2f ms\n", secs(t) * 1000 / 20);
        Workers w(workers, make, e);
        w.try_all<int>(snap, 1, [](Game& b, std::size_t) { return static_cast<int>(b.frame()); }); // (made)
        for (const int len : {1, 20, 60}) {
            t = clock::now();
            const std::size_t n = 128;
            w.try_all<int>(snap, n, [len](Game& b, std::size_t) {
                b.step({}, len);
                return 0;
            });
            const double s = secs(t);
            std::printf("bench: %zu tries of %d frames on %d workers: %.0f ms (%.0f frames/s)\n", n, len, workers, s * 1000,
                        static_cast<double>(n) * len / s);
        }
    }
    if (moves_test) { // each move for 40 frames from here: how high the player goes, where it ends
        const std::vector<Move> all = moves();
        auto snap = e.save();
        const Character p0 = character(e, me(e));
        for (const Move& m : all) {
            e.load(snap);
            float top = p0.y;
            for (int i = 0; i < 40; ++i) {
                e.step(m.inputs(i), 1, !out.empty() && i == 12 ? out + "/move-" + m.name + ".png" : std::string());
                top = std::max(top, character(e, me(e)).y);
            }
            const Character p = character(e, me(e));
            const Score s = score(e);
            std::printf("%-16s up %.2f, moved %.2f %.2f %.2f, hp %d, enemy hp %d\n", m.name, static_cast<double>(top - p0.y),
                        static_cast<double>(p.x - p0.x), static_cast<double>(p.y - p0.y), static_cast<double>(p.z - p0.z),
                        s.player_health, s.enemy_health);
        }
        // Turning each way (6 frames), then a sweep or a homing attack: the damage dealt.
        for (int d = 0; d < 8; ++d)
            for (int kind = 0; kind < 2; ++kind) {
                e.load(snap);
                const int before = score(e).enemy_health;
                e.step(press({kWalk[d][0], kWalk[d][1]}), 6);
                for (int i = 0; i < 50; ++i)
                    e.step(i < 2 ? press({Beat, kind ? Jump : Charge}) : Inputs{});
                std::printf("turn %d, %s: dealt %d\n", d, kind ? "homing" : "sweep ", before - score(e).enemy_health);
            }
        // Walking 60 frames each way, without and with a jump first: where it ends, how near them.
        for (int d = 0; d < 8; ++d)
            for (int jump = 0; jump < 2; ++jump) {
                e.load(snap);
                for (int i = 0; i < 60; ++i)
                    e.step(jump && i < 2 ? press({kWalk[d][0], kWalk[d][1], Jump}) : press({kWalk[d][0], kWalk[d][1]}));
                const Character p = character(e, me(e));
                std::printf("walk %d%s: at %.2f %.2f %.2f, nearest %.2f\n", d, jump ? " jumping" : "        ",
                            static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z), static_cast<double>(apart(e)));
            }
        e.load(snap);
    }
    if (explore_test) { // the exploration alone, from here: the route found, walked, and where it ends
        Workers w(workers, make, e);
        auto snap = e.save();
        if (const char* goal = std::getenv("SPIKEBOT_GOAL")) {
            float gx = 0, gz = 0;
            if (std::sscanf(goal, "%f,%f", &gx, &gz) == 2) g_goal = std::make_pair(gx, gz);
        }
        const std::vector<uint8_t> route = explore(w, snap, apart(e), true);
        std::printf("route of %zu legs\n", route.size());
        for (uint8_t d : route) explore_leg(e, d);
        if (!out.empty()) e.step({}, 1, out + "/explored-" + std::to_string(e.frame() + 1) + ".png");
        print_state(e);
        if (const char* wait = std::getenv("SPIKEBOT_WAIT")) // (then waits there, printing every 200 frames)
            for (int n = std::atoi(wait); n > 0; n -= 200) {
                e.step({}, 200);
                print_state(e);
            }
    }
    if (play_mode) {
        Workers w(workers, make, e);
        play(e, w, frames, out);
    }
    if (selftest) { // a save state run on twice with the same inputs: the same state
        auto ram_hashes = [&e] { // (each board's main RAM, hashed)
            std::vector<uint64_t> h;
            const std::size_t was = e.player();
            for (std::size_t p = 0; p < e.boards(); ++p) {
                e.control(p);
                uint64_t x = 0xCBF29CE484222325ull;
                for (uint32_t a = 0; a < 0x800000; a += 4) x = (x ^ e.read32(a)) * 0x100000001B3ull;
                h.push_back(x);
            }
            e.control(was);
            return h;
        };
        auto snap = e.save();
        e.step(press({Beat, Up}), 120);
        const Score a = score(e);
        const float ax = character(e, me(e)).x;
        const auto ah = ram_hashes();
        e.load(snap);
        e.step(press({Beat, Up}), 120);
        const Score b = score(e);
        const float bx = character(e, me(e)).x;
        const auto bh = ram_hashes();
        std::printf("selftest: player hp %d / %d, enemy hp %d / %d, x %.4f / %.4f: %s; every board's RAM: %s\n", a.player_health,
                    b.player_health, a.enemy_health, b.enemy_health, static_cast<double>(ax), static_cast<double>(bx),
                    a.player_health == b.player_health && a.enemy_health == b.enemy_health && ax == bx ? "same" : "DIFFERENT",
                    ah == bh ? "the same" : "DIFFERENT");
        // And on a worker, from a fresh save: the same as the board itself.
        Workers w(2, make, e);
        auto snap2 = e.save();
        const auto on_worker = w.try_all<std::pair<Score, float>>(snap2, 2, [](Game& board, std::size_t) {
            board.step(press({Beat, Up}), 120);
            return std::make_pair(score(board), character(board, me(board)).x);
        });
        e.step(press({Beat, Up}), 120);
        const Score c = score(e);
        const float cx = character(e, me(e)).x;
        for (const auto& [sc, x] : on_worker)
            std::printf("selftest worker: player hp %d / %d, enemy hp %d / %d, x %.4f / %.4f: %s\n", sc.player_health,
                        c.player_health, sc.enemy_health, c.enemy_health, static_cast<double>(x), static_cast<double>(cx),
                        sc.player_health == c.player_health && sc.enemy_health == c.enemy_health && x == cx ? "same"
                                                                                                           : "DIFFERENT");
    }
    if (!e.stopped().empty()) std::printf("Stopped: %s\n", e.stopped().c_str());
    if (g_input_log) {
        if (input_log.save(g_input_log_path))
            std::printf("Recorded %zu frames' inputs in %s (gamerun --load-state %s --inputs %s plays them again)\n",
                        input_log.frames.size(), g_input_log_path.c_str(), load.c_str(), g_input_log_path.c_str());
        else
            std::fprintf(stderr, "spikebot: cannot write %s\n", g_input_log_path.c_str());
    }
    if (g_linked_log) {
        if (linked_log.save(g_input_log_path))
            std::printf("Recorded %zu changes to %zu boards in %s (gamerun --boards %zu --parallel --load-state %s --inputs %s plays them "
                        "again)\n",
                        linked_log.changes.size(), e.boards(), g_input_log_path.c_str(), e.boards(), load.c_str(), g_input_log_path.c_str());
        else
            std::fprintf(stderr, "spikebot: cannot write %s\n", g_input_log_path.c_str());
    }
    return 0;
}
