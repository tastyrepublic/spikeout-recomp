// One board a PC (docs/netplay.md, "One board a PC" and "One board a PC over
// the network"): the hand-over, the shadows and the rules, for gamerun
// (--one-board, --net-one-board) and the window alike. The board that stays
// (this machine's player's) makes the others with its
// tools::remote::Cabinets from their players' buttons; a board that goes
// keeps its thread, waiting to be run whole again as a shadow of its cabinet
// where side runs can't follow it (from the staying board's state and the
// module's own data), and is handed back to side runs where they can.
//
// A program gives each of its boards a Board (its context, bus and frame
// timing, which it keeps running), sets them up with Game::set_up before
// they run, and calls in from its own frame loop: meet() at each meeting,
// end_frame() at each frame's end (the staying board's), hand_over() where
// a board goes, and from its run loop handed_back() when HandedBack is
// thrown, stopped() as a board stops.
#pragma once

#include "game_setup.hpp"
#include "input_table.hpp"
#include "remote_cabinet.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tools::one_board {

// Thrown from a shadow's hook where it finished `count`: the module wants it back there (Game::handed_back).
struct HandedBack {
    uint32_t count;
};

// Network play: a board's state as its own machine took it at the end of a frame (tools::board_state, no sound), and
// the packets its network board had taken in from the ring there (LinkRing::taken).
// Or (from_backup) as its backup of that frame has it (tools::backup::Copy, kept here and brought up to it by changes:
// the state's big buffers whole, the small rest), sent instead of the whole state (5 MB packed, saved and packed on
// the game's thread): only the pages written since its last backup, off it (no state here: "my backup", pack_remote
// with none; the counts then from the backup).
struct RemoteBoard {
    std::vector<uint8_t> state;
    uint64_t taken = 0, sent = 0; // (and those it had sent)
    std::vector<std::vector<uint8_t>> regions; // (from_backup)
    std::vector<uint8_t> small;
    bool from_backup() const { return !regions.empty(); }
};
// (on the network: the counts, then the state compressed)
inline std::vector<uint8_t> pack_remote(uint64_t taken, uint64_t sent, std::vector<uint8_t> packed_state) {
    std::vector<uint8_t> out(16);
    for (int k = 0; k < 8; ++k) {
        out[static_cast<std::size_t>(k)] = static_cast<uint8_t>(taken >> (8 * k));
        out[static_cast<std::size_t>(8 + k)] = static_cast<uint8_t>(sent >> (8 * k));
    }
    out.insert(out.end(), packed_state.begin(), packed_state.end());
    return out;
}
inline std::optional<RemoteBoard> unpack_remote(const std::vector<uint8_t>& in,
                                                const std::function<std::optional<std::vector<uint8_t>>(std::span<const uint8_t>)>& unpack) {
    if (in.size() < 16) return std::nullopt;
    RemoteBoard b;
    for (int k = 0; k < 8; ++k) {
        b.taken |= uint64_t{in[static_cast<std::size_t>(k)]} << (8 * k);
        b.sent |= uint64_t{in[static_cast<std::size_t>(8 + k)]} << (8 * k);
    }
    if (in.size() == 16) return b; // ("my backup": no state)
    auto state = unpack(std::span<const uint8_t>(in).subspan(16));
    if (!state) return std::nullopt;
    b.state = std::move(*state);
    return b;
}

class Game;

// A board's part: on the board that stays, the other cabinets made by it; on one that goes, its own variables as
// each flow of its starts (by logic count), to start the side flow from; its RAM where it finished each count.
struct Board {
    Board(recomp::Context& context, m3::Bus& b, recomp::FrameTiming& t, std::size_t i) : c(context), bus(b), timing(t), index(i) {}
    recomp::Context& c;
    m3::Bus& bus;
    recomp::FrameTiming& timing;
    std::size_t index;
    InputTable* inputs = nullptr; // (network play: every board's inputs by frame)
    Game* game = nullptr;         // (set up: its hooks')

    std::unique_ptr<remote::Cabinets> cabinets;
    std::mutex cabinets_lock;
    std::map<uint32_t, std::vector<uint8_t>> own_seed;
    std::map<uint32_t, remote::Seed> done_seed; // (on the one that goes: by the count it finished, at 0x36E94)
    bool shadowing = false;                     // (on the one that goes: running as the other's shadow)
    std::size_t peer = 0;                       // (the other of the two, the staying board or the one that goes)
    uint64_t last_link_frame = 0;               // (on the one that goes: the frame of its last link)
    bool one_board_auto = false;
    uint64_t lookahead = 60; // (how far the rules look at the other cabinet's coin and start)
    bool net_inputs = false; // (network play: those from the input table)
    uint64_t hand_back_at = 0; // (gamerun --one-board-hand-back FRAME)
    uint32_t slice = 8;        // (the lines between meetings, for a shadow's wait)
    // the rules, on the one that stays: frames calm in a row while a shadow runs; the mask last seen and since when;
    // whether the other is to be run whole from now on (a shadow, never handed back: every board again)
    uint64_t calm_frames = 0, mask_since = 0;
    bool whole = false;
    bool whole_for_state = false; // (and for now: every board's state wanted, a player joining in progress)
    uint32_t last_mask = 0;
    // a shadow wanted: on the board that stays, from when its state is taken for the other's shadow (at its first
    // 0x36E94 from then on), and whether it has been
    uint64_t shadow_from = 0;
    bool shadow_taken = false;
    // (both boards, until the hand-over: their RAM where each finished a count (0x36E94), by count; on the leaving
    // board then: its RAM at its last, and the bytes that differed from the staying board's at the same count, its
    // own for a shadow (the module's own variables, camera and link memory aside))
    std::map<uint32_t, std::vector<uint8_t>> ram_at;
    bool keep_ram = false;
    // (a board held whole (hand_over's `held`): its RAM and the staying board's kept only from a few frames before the
    // rules ask for it back, 8 MB a count on both being dear; on its own thread, set by the staying board's)
    std::atomic<bool> keeping{true};
    std::atomic<bool> held{false}; // (on the one that goes: held whole, its hand-back not asked yet)
    std::vector<uint8_t> shadow_base;
    std::vector<uint32_t> shadow_own;
};

class Game {
public:
    struct Settings {
        std::size_t stays = 0;           // the board that stays
        std::vector<std::size_t> going;  // the boards it makes (the first: the one the shadows and the rules are for)
        uint64_t frame = 0;              // the hand-over (gamerun's camera recording starts its side runs ten before)
        bool tasks = false;              // remote::Cabinets::use_tasks
        bool rules = false;              // shadows started and stopped by the rules (two boards)
        uint64_t lookahead = 60;         // how far they look at the other's coin and start
        uint64_t shadow_from = 0;        // gamerun --one-board-shadow FRAME
        uint64_t hand_back_at = 0;       // gamerun --one-board-hand-back FRAME
        bool keep_ram = true;            // the boards' RAM kept at each count (for shadows and hand-backs)
        uint32_t slice = 8;              // the lines between meetings
        bool net = false;                // network play: the buttons from each board's input table (Board::inputs), else from `buttons`
        // (no network: a seat's buttons at a frame, bank 0 and controls (nothing: none given anywhere))
        std::function<std::optional<InputTable::Inputs>(std::size_t seat, uint64_t frame)> buttons;
        // (gamerun --one-board-camera: the other board's camera recording, by logic count)
        std::shared_ptr<std::map<uint32_t, std::vector<uint8_t>>> cameras;
        // Network play: a shadow from the other board's real state, asked of its player's machine (which runs it) for
        // the end of a frame a little ahead, and waited for as that frame ends here (its state, uncompressed; nothing:
        // none came). Without them, a shadow is made from this board's state (exact where the other cabinet's work of
        // a count fits this board's frame: docs/netplay.md).
        std::function<void(std::size_t seat, uint64_t frame)> ask_board;
        std::function<std::optional<RemoteBoard>(std::size_t seat, uint64_t frame)> wait_board;
        std::function<void(std::size_t seat, const RemoteBoard& board)> rewind_link; // (its port of the ring as there)
        // Network play: a board whose player went without a parting, run on here from its machine's last backup
        // (tools::backup): that backup's frame (none: no backup of it), and the run, to the end of `until` (waits, with
        // `live`, for this machine's game to have run each frame first: its packets to it in the log).
        std::function<std::optional<uint64_t>(std::size_t seat)> backup_frame;
        std::function<std::optional<RemoteBoard>(std::size_t seat, uint64_t until, std::function<bool(uint64_t)> live)> replay;
        // Rollback (tools/rollback.hpp) with one board: a board made whole or put on side runs here (a part of the
        // rollback's own, or one in the staying board's); whether every input up to a frame is in and none of them
        // was guessed wrong (a switch only there: never gone back across); a board on side runs, as its turns go:
        // the frame gone back to since it last asked (0: none).
        std::function<void(std::size_t board, bool whole, uint64_t frame)> part_whole;
        std::function<bool(uint64_t frame)> settled;
        std::function<bool(uint64_t frame)> wait_settled; // (the same, its inputs waited for: false, one guessed wrong)
        std::function<uint64_t(std::size_t board)> rewound;
        std::function<void(uint64_t frame)> pin; // (rollback::Linked::pin: nothing gone back to before it)
        uint64_t guess_ahead = 0; // (the frames a guess may run ahead: the rollback's window)
        // Rollback, a shadow's state not come by its frame: whether it has (waited for up to `ms`, left to wait_board),
        // and every board run again from a frame (InputTable::run_again_from). The game runs on meanwhile, the other
        // cabinet on side runs; once it comes, back to its frame, loaded there (shadow_wait).
        std::function<bool(std::size_t seat, uint64_t frame, uint32_t ms)> has_board;
        std::function<void(uint64_t frame)> run_again_from;
        // (network play: each record made, its cabinet's own bytes from its real ones: tools::NodeCheck::fix_record)
        remote::Cabinets::RecordFix record_fix;
        remote::Cabinets::HeaderFix header_fix;
    };
    static constexpr uint64_t kAskAhead = 30; // (frames: the other machine reaching it, its state on its way)
    // (frames from a gone player's last inputs to its board taken whole here, run on from its backup: seat_gone)
    static constexpr uint64_t kGoneHandOver = 600;
    // (the calm frames before a hand-back; tests: ONEBOARD_CALM=N, a hand-back put where wanted)
    static inline const uint64_t kCalm = [] {
        const char* n = std::getenv("ONEBOARD_CALM");
        return n ? std::max<uint64_t>(std::strtoull(n, nullptr, 0), 11) : uint64_t{300};
    }();

    // Before the boards run: every board's Board, by index.
    void set_up(std::vector<Board*> boards, Settings settings) {
        boards_ = std::move(boards);
        settings_ = std::move(settings);
        for (Board* b : boards_) b->game = this;
        if (!settings_.cameras) settings_.cameras = std::make_shared<std::map<uint32_t, std::vector<uint8_t>>>();
        Board& m = *boards_[settings_.stays];
        const std::size_t stays = settings_.stays;
        m.peer = settings_.going.front(); // (the board the shadows and the rules are for)
        // (its buttons: network play's from its player's machine, through the input table (the input delay: there
        // before this board's frame needs them; waited for if not); else the given ones)
        auto held = [this, here = &m](std::size_t seat, uint64_t frame) {
            if (InputTable* table = here->inputs; here->net_inputs && table) {
                std::optional<InputTable::Inputs> in = table->get(seat, frame);
                bool guessed = false; // (rollback: a guess where one may be, as a board's own inputs are)
                if (!in && frame >= table->next(seat)) {
                    const auto waited = std::chrono::steady_clock::now(); // (held_wait_seconds)
                    in = rolling() && may_guess(frame) ? table->guess(seat, frame, settings_.guess_ahead, guessed) : table->wait(seat, frame);
                    held_wait_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - waited).count();
                    ++held_waits_;
                }
                static const std::pair<uint64_t, uint64_t> log_range = [] {
                    unsigned long long a = 0, b = 0;
                    if (const char* v = std::getenv("ONEBOARD_HELD_LOG")) std::sscanf(v, "%llu-%llu", &a, &b);
                    return std::pair<uint64_t, uint64_t>{a, b};
                }();
                if (frame >= log_range.first && frame <= log_range.second)
                    std::printf("held: seat %zu frame %llu (board at %llu): %s %08x, the table %llu-%llu\n", seat,
                                (unsigned long long)frame, (unsigned long long)here->timing.frames(), in ? "in" : "NONE",
                                in ? remote::held_word(in->bank0, in->controls, in->controls2) : 0u,
                                (unsigned long long)table->first(seat), (unsigned long long)table->next(seat));
                return in ? remote::held_word(in->bank0, in->controls, in->controls2) : 0u; // (before its first: none)
            }
            const auto in = settings_.buttons ? settings_.buttons(seat, frame) : std::nullopt;
            return in ? remote::held_word(in->bank0, in->controls) : remote::held_word(0xFF, 0xFF);
        };
        m.cabinets = std::make_unique<remote::Cabinets>(m.c, m.bus, m.timing, stays, held);
        if (settings_.record_fix) m.cabinets->set_record_fix(settings_.record_fix);
        if (settings_.header_fix) m.cabinets->set_header_fix(settings_.header_fix);
        m.shadow_from = settings_.shadow_from;
        // (shadows from the other machines' real boards need none of the boards' RAM kept: shadow_own_bytes is the local
        // shadows'; 8 MB a count spared)
        m.keep_ram = settings_.keep_ram && !settings_.ask_board;
        m.one_board_auto = settings_.rules;
        m.lookahead = settings_.lookahead, m.net_inputs = settings_.net;
        m.hand_back_at = settings_.hand_back_at;
        m.slice = settings_.slice;
        const uint64_t from = settings_.frame >= 10 ? settings_.frame - 10 : 0;
        const auto cameras = settings_.cameras;
        Board* s = &m;
        for (const std::size_t b : settings_.going) {
            Board* l = boards_[b];
            l->peer = stays, l->keep_ram = m.keep_ram, l->slice = settings_.slice, l->net_inputs = m.net_inputs;
            // (its logic count less this board's: the master runs a count ahead of the slaves)
            const int ahead = (b == 0 ? 1 : 0) - (stays == 0 ? 1 : 0);
            m.cabinets->add(b, ahead, [cameras, l, s, from](std::size_t, uint32_t logic, remote::Seed& seed) {
                if (s->timing.frames() < from) return false;
                if (cameras->empty()) { // (its own, camera and regions where it finished the count before: 0x36E94)
                    std::lock_guard guard(l->cabinets_lock);
                    const auto done = l->done_seed.find(logic - 1);
                    if (done == l->done_seed.end()) return false;
                    seed = done->second;
                    return true;
                }
                const auto camera = cameras->find(logic);
                if (camera == cameras->end()) return false;
                std::lock_guard guard(l->cabinets_lock);
                auto own = l->own_seed.upper_bound(logic);
                if (own == l->own_seed.begin()) return false;
                --own;
                seed.camera = camera->second, seed.own = own->second, seed.own_logic = own->first - 1;
                seed.regions = remote::gather_regions(l->bus.ram());
                return true;
            });
            l->c.set_hook(&Game::leaving_hook, l);
        }
        m.cabinets->use_tasks(settings_.tasks);
        m.c.set_hook(&Game::staying_hook, &m);
        // (the game's other writes into the camera, for the other cabinets' cameras: not in tasks mode, where their
        // own tasks make them; the watch sends every work RAM write the slow way)
        if (!settings_.tasks)
            m.bus.watch_writes(remote::kCamera, remote::kCamera + remote::kCameraBytes, &remote::Cabinets::camera_written,
                               m.cabinets.get());
    }

    bool goes(std::size_t board) const {
        return std::find(settings_.going.begin(), settings_.going.end(), board) != settings_.going.end();
    }
    const Settings& settings() const { return settings_; }

    // The staying board's meeting after `line`: the other cabinets' blocks due, their network boards brought up to it.
    static void meet(Board& m, uint32_t line) {
        if (!m.cabinets) return;
        if (m.game) m.game->wait_hand_backs(m);
        std::lock_guard guard(m.cabinets_lock);
        const uint64_t f = m.timing.frames();
        m.cabinets->meet(f, m.timing.frame_start(f) + m.timing.line_cycles() * (recomp::FrameTiming::kVBlankLines + line));
    }
    // The staying board's frame end: the other cabinets' frames end with it (frames() counts those done).
    static void end_frame(Board& m) {
        if (!m.cabinets) return;
        {
            std::lock_guard guard(m.cabinets_lock);
            m.cabinets->end_frame(m.timing.frames() - 1);
            static const bool say = std::getenv("ONEBOARD_STATE_BYTES") != nullptr; // (tests: the module's own state, every 300 frames)
            if (say && m.timing.frames() % 300 == 0)
                std::printf("board %zu frame %llu: the module's state %zu KB\n", m.index, (unsigned long long)m.timing.frames(),
                            m.cabinets->state_bytes() / 1024);
        }
        if (m.game) m.game->live(m.timing.frames());
    }

    // A board that goes, at the end of the hand-over's frame (no camera recording): handed over as a shadow is,
    // where the module asks it to stop (a slave's next count then caught up there): its camera and own variables
    // its own, as they are. `held`: not asked yet, run on whole until the rules find it calm.
    void hand_over(Board& m, bool held) {
        Board& other = *boards_[m.peer];
        if (held) m.held = true, m.keeping = other.keeping = false; // (until the rules are about to ask)
        std::lock_guard guard(other.cabinets_lock);
        other.cabinets->running(m.index, m.bus.netboard());
        if (!held) other.cabinets->ask_hand_back(m.index);
        m.shadowing = true;
    }

    // A board that goes, from its run loop: its shadow stopped where it finished `count` (HandedBack), the module on
    // from there. True: on as a shadow again (resume from its state); false: it stops (`why` set if empty).
    bool handed_back(Board& m, Turns& turns, uint32_t count, std::string& why) {
        Board& other = *boards_[m.peer];
        m.shadowing = false;
        if (m.keep_ram) { // (its main board itself handed over (--one-board with no camera recording): as a drop's)
            shadow_own_bytes(m, other);
            why = "one board: its blocks made by board " + std::to_string(other.index) + " from frame " + std::to_string(m.timing.frames());
        }
        {
            std::scoped_lock guard(m.cabinets_lock, other.cabinets_lock);
            const auto seed = m.done_seed.find(count);
            if (seed == m.done_seed.end()) { // (not kept (a going back since): a shadow still, handed back later; it threw)
                std::printf("board %zu: no seed of count %u to hand back with: a shadow still\n", m.index, count);
                m.shadowing = true;
                return true;
            }
            other.cabinets->hand_back(m.index, seed->second, m.bus.ram(), m.last_link_frame ? m.last_link_frame : m.timing.frames() - 1);
        }
        std::printf("board %zu: its shadow handed back to board %zu's side runs after count %u (frame %llu)\n", m.index, other.index,
                    count, (unsigned long long)m.timing.frames());
        if (rolling()) { // (rollback: no part of its own now, in the staying board's; guesses again after the next frame,
            // nothing gone back to before it: its save the first in the new way)
            settings_.part_whole(m.index, false, m.timing.frames());
            if (settings_.pin) settings_.pin(m.timing.frames() + 1);
            std::lock_guard guard(lock_);
            no_guess_[m.index] = m.timing.frames() + 1;
        }
        if (!other.one_board_auto && !other.shadow_from) { // (no shadow of it can be wanted: out of the meetings)
            if (why.empty()) why = "one board: its shadow handed back";
            return false;
        }
        if (shadow_wait(m, turns, m.slice)) return true;
        why = "one board: its shadow handed back, no other wanted";
        return false;
    }

    // On the staying board's thread: whether the other cabinet is made by side runs now (not run whole); and the
    // other to be run whole from now on (with the rules: a shadow again at once, never handed back).
    static bool on_side_runs(const Board& stays) { return stays.cabinets && stays.cabinets->making(stays.peer); }
    // On the staying board's thread, network play (shadows from the real boards: rules_each):
    // - whether every other board runs whole here (held, or a shadow; or the module gone): every board's state can be
    //   saved here as it is (a player joining in progress);
    // - every board run whole for now (shadows asked of their machines), or no longer (want_whole);
    // - seat `seat` leaving after `frame`, its board's state at its end sent by its machine (Session::announce_parting):
    //   a shadow of it from that, its cabinet whole here from then on (the player gone, its inputs none).
    // - a seat whose player has gone without a parting (Session's idle: a crash, a lost connection): its board is had
    //   nowhere whole any more; never asked for, and every board's state can't be saved here while it is made.
    bool whole_here(std::size_t seat) const {
        return boards_[settings_.stays]->cabinets->shadowed(seat) && boards_[seat]->shadowing;
    }
    bool all_whole() const {
        const Board& m = *boards_[settings_.stays];
        if (!m.cabinets) return true;
        for (const std::size_t gone : settings_.going)
            if (!whole_here(gone)) return false;
        return true;
    }
    // Rollback: whether a board's input for `frame` may be guessed. Not at a switch between side runs and whole (a
    // hand-back: from the frame it is asked at to the one after it; the window's set-up): those frames' inputs waited
    // for, as delay-based play does, so that none at or before the switch can turn out wrong (going back across it
    // would find the other cabinet in the other mode); the frame after it pinned (Settings::pin), nothing gone back
    // to before it. A shadow waits at its frame for every input up to it (Settings::wait_settled), the frames before
    // it guessed as any.
    // The boards' turns (Turns), for wait_hand_backs: set where they are made.
    void set_turns(Turns* turns) { turns_ = turns; }
    // At a meeting of the staying board, before what the module does there: a shadow to be handed back any time now
    // (its own thread stopping it where its count ends, between meetings) waited for until it has come to this
    // meeting too, or been handed back. Its hand-back then always before the module's work at this meeting, as the
    // boards' speeds have it or not (the staying board run faster, the idle wait skipped: else now and then after,
    // its network board then synced and its link written another time).
    void wait_hand_backs(Board& stays) {
        if (!turns_ || !stays.cabinets) return;
        for (const std::size_t gone : settings_.going)
            for (;;) {
                {
                    std::lock_guard guard(stays.cabinets_lock);
                    if (!stays.cabinets->hand_back_pending(gone)) break;
                }
                if (turns_->waiting(gone)) break;
                std::this_thread::yield(); // (its part of this stretch still running, a few microseconds)
            }
    }
    // (the side runs' buttons not in yet when they were wanted: how many, and the time waiting for them (or guessing))
    uint64_t held_waits() const { return held_waits_; }
    double held_wait_seconds() const { return held_wait_seconds_; }
    bool may_guess(uint64_t frame) const {
        std::lock_guard guard(lock_);
        for (const auto& [seat, through] : no_guess_)
            if (frame <= through) return false;
        return true;
    }
    // (and none through `frame`: the window's set-up, at a meeting, as a switch)
    void hold_guesses(uint64_t frame) {
        std::lock_guard guard(lock_);
        no_guess_[~std::size_t{0}] = frame;
    }
    bool can_be_whole() const { // (every board whole here, or each that isn't to be asked of its machine)
        const Board& m = *boards_[settings_.stays];
        if (!m.cabinets) return true;
        std::lock_guard guard(lock_);
        for (const std::size_t gone : settings_.going)
            if (!whole_here(gone) && lost_.count(gone)) return false;
        return true;
    }
    void want_whole(bool on) { boards_[settings_.stays]->whole_for_state = on; }
    // Rollback: whether a shadow here waits for a state of a frame before `frame` (Settings::has_board): every board
    // will go back to that one and run `frame` again, so this board's own state of `frame`, asked by another machine,
    // is taken when it does (as after a guess found wrong: what was run first had that cabinet on side runs).
    bool late_before(uint64_t frame) const {
        std::lock_guard guard(lock_);
        for (const auto& [seat, l] : late_)
            if (l.frame < frame) return true;
        return false;
    }
    void parting(std::size_t seat, uint64_t frame) {
        const Board& m = *boards_[settings_.stays];
        if (!m.cabinets || !goes(seat)) return;
        std::lock_guard guard(lock_);
        // (come too late: that frame passed here, or about to (the waiting board a frame off this one); its board run on
        // from its backup once its player is gone, as one gone without a parting (seat_gone). Its announcement comes a
        // little ahead (kAskAhead: 0.5 s in real time), but gamerun runs faster, and a slow network may take longer)
        if (!whole_here(seat) && m.timing.frames() + 2 > frame) {
            std::printf("board %zu: board %zu's player leaving after frame %llu, told at frame %llu: too late; its board from its "
                        "backup once it has gone\n",
                        m.index, seat, (unsigned long long)frame, (unsigned long long)m.timing.frames());
            return;
        }
        departed_.insert(seat);
        if (whole_here(seat)) return; // (whole here already: on as it is)
        remote_frames_[seat] = frame;
        std::printf("board %zu: board %zu's player leaving after frame %llu: a shadow of it from its state then, whole from there on\n",
                    m.index, seat, (unsigned long long)frame);
    }
    // A player taking a seat whose player went (a game joined in progress): its board is that player's machine's now,
    // to be asked of it and handed back to side runs here as any other's once the game is calm.
    void seat_taken(std::size_t seat) {
        std::lock_guard guard(lock_);
        if (departed_.erase(seat) + lost_.erase(seat))
            std::printf("board %zu: board %zu taken by a new player: its machine's from here, handed back when calm\n", settings_.stays,
                        seat);
    }
    // (called at the staying board's frame end)
    // (`idle_from`: the frame its inputs are none from, agreed by every machine)
    void seat_gone(std::size_t seat, uint64_t idle_from) {
        Board& m = *boards_[settings_.stays];
        std::lock_guard guard(lock_);
        if (departed_.count(seat) || lost_.count(seat) || !m.cabinets || !goes(seat)) return;
        if (whole_here(seat)) { // (whole here as it went: on as it is, never handed back)
            departed_.insert(seat);
            std::printf("board %zu: board %zu's player gone, its board whole here: on from here\n", m.index, seat);
            return;
        }
        const uint64_t now = m.timing.frames();
        const std::optional<uint64_t> from = settings_.backup_frame ? settings_.backup_frame(seat) : std::nullopt;
        if (!from || !settings_.replay || *from >= now) {
            lost_.insert(seat);
            std::printf("board %zu: board %zu's player gone, no backup of its board: had nowhere whole\n", m.index, seat);
            return;
        }
        // (run from its backup on its own, a frame of it once this game has run that frame, to a frame ahead: as
        // many frames on as it is behind (twice as fast as the game), at least two seconds; this game waits there
        // if it hasn't come)
        // The frame it is taken whole from: the same on every machine, kGoneHandOver frames after its player's inputs
        // end (agreed), not from when each machine noticed (a frame apart, the boards' network boards then differed:
        // onepc-net-tests leave3 -LeaverFirst, 2026-11-06). Late enough for the run from its backup (at least twice
        // the game's speed) to come up to it after a drop noticed 3 s on; if this machine is past that, as before.
        uint64_t until = idle_from + kGoneHandOver;
        if (until < std::max(now + 120, 2 * now - *from)) {
            const uint64_t late = now + std::max<uint64_t>(120, now - *from);
            std::printf("board %zu: board %zu's player gone at frame %llu, noticed at %llu: whole from %llu, not %llu (this machine's own)\n",
                        m.index, seat, (unsigned long long)idle_from, (unsigned long long)now, (unsigned long long)late,
                        (unsigned long long)until);
            until = late;
        }
        // (tests: ONEBOARD_UNTIL_EXTRA=STAYS:N, the machine whose own board is STAYS takes it whole N frames later:
        // two machines doing so at different frames, as their timing may have them)
        if (const char* e = std::getenv("ONEBOARD_UNTIL_EXTRA")) {
            unsigned long long stays = 0, extra = 0;
            if (std::sscanf(e, "%llu:%llu", &stays, &extra) == 2 && stays == settings_.stays) until += extra;
        }
        departed_.insert(seat);
        remote_frames_[seat] = until;
        replays_[seat] = Replaying{};
        std::printf("board %zu: board %zu's player gone: its board run on here from its backup at frame %llu, whole from frame %llu\n",
                    m.index, seat, (unsigned long long)*from, (unsigned long long)until);
        replay_threads_.emplace_back([this, seat, until] {
            std::optional<RemoteBoard> board = settings_.replay(seat, until, [this](uint64_t f) { return wait_live(f); });
            std::lock_guard g(lock_);
            replays_[seat].done = true;
            replays_[seat].board = std::move(board);
            replayed_.notify_all();
        });
    }
    ~Game() {
        {
            std::lock_guard guard(lock_);
            live_stopped_ = true;
        }
        live_changed_.notify_all();
        replay_threads_.clear(); // (none left if the staying board stopped: end_replays)
    }
    static void keep_whole(Board& stays) { stays.whole = true; }
    // Where the boards meet (every one stopped): once the other is run whole for good (keep_whole) and its shadow is
    // running, nothing of the module is needed: the game's own hooks back, the side runs' machinery gone (every
    // board as plain network play, for what it costs). Its RAM isn't the other machine's copy of that board in
    // all (its shadow's, exact in what it sends): a program still keeps its hash and state to itself. True: done.
    // Torn down (tear_down), to be set up again with another board staying (a spectator watching another player: its
    // board, whole now, stays; the one that stayed goes): what each board and the module kept of the last set-up let go.
    void again() {
        for (Board* b : boards_) {
            std::lock_guard guard(b->cabinets_lock);
            b->own_seed.clear(), b->done_seed.clear(), b->ram_at.clear();
            b->shadowing = false, b->last_link_frame = 0;
            b->calm_frames = 0, b->mask_since = 0, b->last_mask = 0;
            b->whole = false, b->whole_for_state = false, b->shadow_taken = false;
            b->keeping = true, b->held = false;
            b->shadow_base.clear(), b->shadow_own.clear();
        }
        std::lock_guard guard(lock_);
        shadow_frame_ = ~0ull;
        late_.clear();
        no_guess_.clear();
        master_ready_ = false;
    }

    bool tear_down() {
        Board& m = *boards_[settings_.stays];
        if (rolling()) return false; // (rollback: its kept frames hold the module's state: kept to the end)
        if (!m.whole || !m.cabinets) return false;
        for (const std::size_t b : settings_.going)
            if (!boards_[b]->shadowing) return false; // (its shadow not running yet)
        for (Board* b : boards_) {
            b->c.set_hook(m3::games::spikeout_hook, nullptr);
            b->shadowing = false, b->keep_ram = false;
        }
        std::lock_guard guard(m.cabinets_lock);
        m.cabinets.reset();
        return true;
    }

    // A board stopped. The staying board in network play: its shadows of the others with it (their players may have
    // gone already, nothing more of theirs to wait for).
    static void stopped(Board& m) {
        if (m.net_inputs && m.cabinets && m.inputs) m.inputs->close();
        if (m.cabinets && m.game) m.game->live(~0ull, true);
        // (the staying board: the runs of boards from their backups (seat_gone) ended with it, as they read the
        // network's inputs and logs, gone once the run is over; gamerun's Game, a global, was destroyed after them)
        if (m.game && m.index == m.game->settings_.stays) m.game->end_replays();
    }
    void end_replays() {
        live(~0ull, true);
        replay_threads_.clear(); // (joined: each stops at its next frame, the live game stopped)
    }

    // The leaving board's own bytes for a shadow of it later: where its RAM and the staying board's differ, at its
    // last count (from then on neither keeps its RAM).
    static void shadow_own_bytes(Board& m, Board& other) {
        std::scoped_lock guard(m.cabinets_lock, other.cabinets_lock);
        if (!m.ram_at.empty())
            if (const auto theirs = other.ram_at.find(m.ram_at.rbegin()->first); theirs != other.ram_at.end()) {
                m.shadow_base = m.ram_at.rbegin()->second;
                for (uint32_t a = 0; a < m.shadow_base.size(); ++a)
                    if (m.shadow_base[a] != theirs->second[a]) m.shadow_own.push_back(a);
                std::printf("board %zu: %zu bytes its own (against board %zu's, after count %u)\n", m.index, m.shadow_own.size(),
                            other.index, theirs->first);
            }
        m.keep_ram = other.keep_ram = false;
        m.ram_at.clear(), other.ram_at.clear();
    }

    // The leaving board, its main board gone, waiting (taking its turns, a frame at a time) for the staying board's
    // state; then made the other cabinet's shadow from it, a frame on (a slave's world is the master's a frame back),
    // with what is that cabinet's own from the staying board's remote::Cabinets. False: the other board stopped first.
    bool shadow_wait(Board& m, Turns& turns, uint32_t slice) {
        Board& other = *boards_[m.peer];
        remote::Cabinets::Shadow own;
        for (uint64_t f = m.timing.frames();; ++f) {
            if (settings_.wait_board) { // (network play: from its machine's real state, at the end of the frame asked)
                // (its state at the end of frame `want`, its count of frames then `want`: on from the top of frame
                // `want` here, this board having taken frame want - 1's meetings, the staying board at its first of `want`)
                uint64_t want = ~0ull;
                bool late = false; // (its state not come by its frame: on side runs meanwhile)
                {
                    std::lock_guard guard(lock_);
                    if (const auto it = remote_frames_.find(m.index); it != remote_frames_.end()) {
                        want = it->second;
                        if (const auto l = late_.find(m.index); l != late_.end() && l->second.frame == want) late = true;
                        else if (f > want) remote_frames_.erase(it), want = ~0ull; // (gone by: asked again)
                    }
                }
                // (rollback, its state late: once it has come, every board back to its frame, this one loading it there
                // as the boards meet again (rewound below: f set back to it); come too late to go back that far, waited
                // for where there is still room)
                if (late && f > want) {
                    bool back_asked = false;
                    {
                        std::lock_guard guard(lock_); // (gone meanwhile: its late load dealt with, nothing to ask)
                        const auto l = late_.find(m.index);
                        back_asked = l == late_.end() || l->second.back_asked;
                    }
                    const uint64_t room = settings_.guess_ahead > kLateRoom ? settings_.guess_ahead - kLateRoom : 0;
                    if (!back_asked) {
                        const bool last = f >= want + room;
                        // (every shadow asked for that frame come: they go back together, loaded at it together; one
                        // loaded there already couldn't be gone back to)
                        if (come(m.index, want, f, last ? 30000 : 0) && all_come(want, f, last ? 30000 : 0)) {
                            {
                                std::lock_guard guard(lock_);
                                for (const auto& [seat, at] : remote_frames_)
                                    if (at == want) late_[seat] = Late{want, true};
                            }
                            settings_.run_again_from(want + 1);
                            std::printf("board %zu: its shadow's state for frame %llu came at frame %llu: every board back to it\n",
                                        m.index, (unsigned long long)want, (unsigned long long)f);
                        } else if (last) {
                            std::printf("board %zu: no state of it came from its machine for frame %llu\n", m.index,
                                        (unsigned long long)want);
                            std::lock_guard guard(lock_);
                            late_.erase(m.index), remote_frames_.erase(m.index); // (asked again by the rules, if still wanted)
                            want = ~0ull;
                        }
                    } else if (f > want + settings_.guess_ahead) { // (not gone back after all: given up, asked again)
                        std::lock_guard guard(lock_);
                        late_.erase(m.index), remote_frames_.erase(m.index);
                        want = ~0ull;
                    }
                }
                // (rollback: only where every input up to it is in (waited for: the other machine's for this frame
                // come with time), none wrong: else asked again at once, a little later (the rules may not ask
                // again: what wanted it gone by))
                if (want != ~0ull && f == want && rolling() &&
                    !(settings_.wait_settled ? settings_.wait_settled(want) : settings_.settled(want))) {
                    const uint64_t again = want + kAskAhead;
                    std::printf("board %zu: its shadow's frame %llu guessed wrong: asked again for frame %llu\n", m.index,
                                (unsigned long long)want, (unsigned long long)again);
                    {
                        std::lock_guard guard(lock_);
                        remote_frames_[m.index] = again;
                        late_.erase(m.index);
                    }
                    settings_.ask_board(m.index, again);
                    if (late_loads()) settings_.pin(again);
                    want = ~0ull;
                }
                // (rollback: its state not come yet, the game run on, the other cabinet on side runs meanwhile; loaded
                // here once it has, every board gone back to this frame)
                if (want != ~0ull && f == want && late_loads()) {
                    // (the shadows of one frame deciding one at a time: all of them loaded at it, or none, gone back to
                    // it together once every state has come (one loaded there, that frame couldn't be gone back to);
                    // one deciding to load marked so first, its state then taken from those come)
                    std::lock_guard group(group_lock_);
                    const bool wait = late_waiting(want) || !come(m.index, want, f, 0) || !all_come(want, f, 0);
                    std::lock_guard guard(lock_);
                    if (!wait) {
                        loading_[m.index] = want;
                    } else {
                        if (!late_.count(m.index)) {
                            late_[m.index] = Late{want, false};
                            std::printf("board %zu: its shadow's state for frame %llu not come yet: on side runs meanwhile\n", m.index,
                                        (unsigned long long)want);
                        }
                        want = ~0ull; // (not loaded at this frame now)
                    }
                }
                if (want != ~0ull && f == want) {
                    const bool loaded = load_remote_shadow(m, want);
                    if (loaded && rolling()) { // (a part of its own again, from here; nothing gone back to before it: every
                        // frame up to it settled)
                        settings_.part_whole(m.index, true, want);
                        if (settings_.pin) settings_.pin(want);
                        // (loaded late, gone back to: the link's part of this frame kept from before the load; nothing
                        // gone back to it again, the next frame's inputs waited for, not guessed)
                        std::lock_guard guard(lock_);
                        if (late_.count(m.index)) no_guess_[m.index] = std::max(no_guess_[m.index], want + 1);
                    }
                    {
                        std::lock_guard guard(lock_);
                        // (used; or none came: asked again, while wanted; unless set again meanwhile: its player gone,
                        // its backup run on to a frame ahead)
                        if (const auto it = remote_frames_.find(m.index); it != remote_frames_.end() && it->second == want)
                            remote_frames_.erase(it);
                        late_.erase(m.index);
                        loading_.erase(m.index);
                    }
                    if (loaded) return true;
                }
            }
            // (the shadow to start in this frame: where the staying board's state was taken, a frame on)
            uint64_t begin = ~0ull;
            {
                std::scoped_lock guard(lock_, other.cabinets_lock);
                if (shadow_frame_ != ~0ull && f > shadow_frame_ + 1) {
                    std::printf("board %zu: the staying board's state came too late (taken at frame %llu, here at %llu)\n", m.index,
                                (unsigned long long)shadow_frame_, (unsigned long long)f);
                    return false;
                }
                if (shadow_frame_ != ~0ull && f == shadow_frame_ + 1) {
                    begin = shadow_cycles_ + m.timing.frame_cycles();
                    // (its link of the count not made yet, which the module makes near the frame's end: at the
                    // staying board's meeting before the shadow starts instead)
                    if (!other.cabinets->shadow_ready(m.index, shadow_count_))
                        other.cabinets->link_early(m.index, begin - static_cast<uint64_t>(slice) * m.timing.line_cycles());
                }
            }
            if (turns.alone(m.index) || turns.finished(m.peer)) return false; // (the staying board stopped: with it)
            // (its turns at the frame's lines, as a running board takes them; in the first, those still to come; in the
            // shadow's, those before it starts: the other cabinet's link of the count may come in them, where the staying
            // board finished it late in its frame (its work light), as the power-on game's master does)
            const uint64_t now = m.c.cycles(), start = m.timing.frame_start(f) + m.timing.line_cycles() * recomp::FrameTiming::kVBlankLines;
            uint64_t rewound = 0; // (rollback: the boards gone back to the end of this frame where they met: on from the next)
            for (uint32_t line = slice - 1; line < recomp::FrameTiming::kDisplayLines; line += slice)
                if (const uint64_t at = start + line * m.timing.line_cycles(); at >= now) {
                    if (at >= begin) break;
                    turns.pass(m.index);
                    if (settings_.rewound && (rewound = settings_.rewound(m.index)) != 0) break;
                    if (load_master_shadow(m)) return true; // (the master's: as soon as it is there, mid-frame)
                }
            if (rewound) {
                // (++f: the frame after it, from its first meeting. `f` is the frame being run, its count of frames
                // during it: the boards back at the end of frame `rewound`, their count then `rewound`, run on as frame
                // `rewound` here (f = rewound had this board a frame ahead from then on: a shadow taken after it was
                // loaded a frame early, its link two packets ahead of the staying board's, both waiting for ever)
                f = rewound - 1;
                continue;
            }
            if (begin != ~0ull) {
                std::scoped_lock guard(lock_, other.cabinets_lock);
                if (other.cabinets->shadow_state(m.index, shadow_count_, own)) break;
                // (none for that count: that cabinet a count off this board's just then, after a frame it missed or held
                // back: asked again the next frame)
                std::printf("board %zu: no shadow state for count %u, again\n", m.index, shadow_count_);
                shadow_frame_ = ~0ull, other.shadow_taken = false;
            }
            if (begin != ~0ull) // (and the frame's turns after where it was to start)
                for (uint32_t line = slice - 1; line < recomp::FrameTiming::kDisplayLines; line += slice)
                    if (const uint64_t at = start + line * m.timing.line_cycles(); at >= now && at >= begin) {
                        turns.pass(m.index);
                        if (load_master_shadow(m)) return true;
                    }
        }
        m3::State nb, ee;
        m.bus.netboard().state(nb);
        m.bus.eeprom().state(ee);
        const auto inputs = m.bus.inputs;
        const uint8_t role = m.bus.read8(kRole);
        {
            std::lock_guard guard(other.cabinets_lock);
            other.cabinets->stop_making(m.index);
        }
        {
            std::lock_guard guard(lock_);
            std::vector<uint8_t> bytes = state_;
            m3::State s(std::move(bytes));
            board_state(s, m.c, m.timing, {});
        }
        m3::State nb_in(std::move(nb.data())), ee_in(std::move(ee.data()));
        m.bus.netboard().state(nb_in);
        m.bus.eeprom().state(ee_in);
        m.bus.inputs = inputs;
        const uint64_t shift = m.timing.frame_cycles(); // (its time: the staying board's, a frame on)
        m.timing.move_on(1);
        shadow_inputs(m); // (its inputs for this frame, as its frame's end before would have set them)
        recomp::Context::Aside a = m.c.set_aside();
        a.cycles += shift, a.next_check += shift, a.scheduler_due += shift;
        a.tb_offset -= shift * 33 / 332;
        m.c.put_back(a);
        // (its own: first what differed from the staying board's at the hand-over, as it was then (the module's own
        // variables aside, which it has newer); then the module's: link memory and variables, own variables (their
        // own bits), camera, working record but +0x100)
        auto ram = m.bus.ram();
        {
            std::vector<uint32_t> tracked = other.cabinets->own_addresses_list();
            // (and the characters' fields each cabinet keeps for itself: their kept values below (own.fields), dropped
            // once a character is set up again; as they were at the hand-over, they came back stale (2026-11-07))
            for (uint32_t k = 0; k < 12; ++k)
                for (const auto& [off, size] : remote::kEntryFields)
                    for (uint32_t b = 0; b < size; ++b) tracked.push_back(remote::character_object(k) + off + b);
            std::sort(tracked.begin(), tracked.end());
            for (uint32_t at : m.shadow_own)
                if (!std::binary_search(tracked.begin(), tracked.end(), at)) ram[at] = m.shadow_base[at];
        }
        std::copy(own.link.begin(), own.link.end(), ram.begin() + remote::kLinkMemory);
        {
            std::size_t n = 0;
            for (const auto& [lo, hi] : remote::kLinkVars)
                for (uint32_t addr = lo; addr < hi; ++addr) ram[addr] = own.link_vars[n++];
        }
        const auto& addrs = other.cabinets->own_addresses_list();
        const auto& masks = other.cabinets->own_masks();
        for (std::size_t k = 0; k < addrs.size(); ++k)
            ram[addrs[k]] = static_cast<uint8_t>((ram[addrs[k]] & ~masks[k]) | (own.own[k] & masks[k]));
        std::copy(own.camera.begin(), own.camera.end(), ram.begin() + remote::kCamera);
        if (own.regions.size() == remote::kRegionBytes) remote::scatter_regions(ram, own.regions);
        for (uint32_t i = 0; i < 0x80; ++i)
            if (i != 0x100 / 4) m.bus.write32(remote::kWorkingRecord + 4 * i, own.working[i]);
        for (const auto& f : own.fields) { // (its characters' fields it keeps for itself)
            if (f.size == 2) m.bus.write16(f.addr, static_cast<uint16_t>(f.value));
            else m.bus.write32(f.addr, f.value);
        }
        m.bus.write8(kRole, role);
        std::printf("board %zu: its shadow from board %zu's state after count %u (frame %llu), at frame %llu\n", m.index, other.index,
                    shadow_count_, (unsigned long long)shadow_frame_, (unsigned long long)m.timing.frames());
        {
            std::lock_guard guard(lock_);
            shadow_frame_ = ~0ull; // (taken: the next shadow from a state taken anew)
        }
        m.shadowing = true;
        return true;
    }

private:
    static constexpr uint32_t kRole = 0x10019E; // (the link's role: 1 the master, 2 a slave)

    // A shadow's inputs for the frame it starts in, as its frame's end before would have set them: over the network
    // from the input table, else the given buttons.
    void shadow_inputs(Board& m) {
        if (m.net_inputs) {
            if (const auto in = m.inputs->wait(m.index, m.timing.frames())) {
                m.bus.inputs.bank0 = in->bank0, m.bus.inputs.bank1 = in->bank1;
                m.bus.inputs.controls = in->controls, m.bus.inputs.controls2 = in->controls2;
            }
            return;
        }
        if (!settings_.buttons) return;
        const auto in = settings_.buttons(m.index, m.timing.frames());
        m.bus.inputs.bank0 = in ? in->bank0 : 0xFF;
        m.bus.inputs.controls = in ? in->controls : 0xFF;
    }

    // Tests: ONEBOARD_TRACE=FROM:TO prints, in those frames, the line where each board (with the module's hooks) writes
    // its record (its link), starts its flow (the mode dispatcher) and finishes its count (0x36E94), with its count.
    static void probe(Board& m, uint32_t pc) {
        static const char* spec = std::getenv("ONEBOARD_TRACE");
        if (!spec || (pc != remote::kRecordWrite && pc != remote::kLinkSkipped && pc != remote::kModeDispatch)) return;
        unsigned long long from = 0, to = 0;
        std::sscanf(spec, "%llu:%llu", &from, &to);
        const uint64_t f = m.timing.frames();
        if (f < from || f > to) return;
        std::printf("trace board %zu frame %llu line %llu: %s, count %u\n", m.index, (unsigned long long)f,
                    (unsigned long long)((m.c.cycles() - m.timing.frame_start(f)) / m.timing.line_cycles()),
                    pc == remote::kRecordWrite ? "link (its record)" : pc == remote::kLinkSkipped ? "count done" : "dispatcher",
                    m.bus.read32(remote::kLogicCounter));
    }
    static void keep_ram(Board& m) { // (at 0x36E94, while wanted: this board's RAM after the count it finished)
        if (!m.keep_ram || !m.keeping) return;
        std::lock_guard guard(m.cabinets_lock);
        auto ram = m.bus.ram();
        m.ram_at[m.bus.read32(remote::kLogicCounter)].assign(ram.begin(), ram.end());
        while (m.ram_at.size() > 3) m.ram_at.erase(m.ram_at.begin());
    }

    // The rules: once a frame on the board that stays, where it finished a count (0x36E94). A shadow of the other
    // cabinet started when trouble may come, that side runs can't follow (the other then missing frames to its
    // banners, or the games apart): a player in the game with 5 s or less (the time table at 0x1062B0, a halfword a
    // player, 1/64 s), this board out of the game's mode (-16374(r13), 7), the link's players changed (the header's
    // mask, +0x28) in the last 60 frames, or the other cabinet's coin or start buttons pressed in the next `lookahead`
    // frames. Handed back to side runs after 300 frames in a row of the opposite with room to spare: every player in
    // the game with more than 15 s, both cabinets in the game's mode with the same players, no coin or start coming.
    void rules(Board& m) {
        if (settings_.ask_board) return rules_each(m);
        const std::size_t gone = m.peer;
        Board& other = *boards_[gone];
        remote::Cabinets& cab = *m.cabinets;
        if (!cab.making(gone) && !cab.shadowed(gone)) return; // (its main board still there)
        const uint64_t frame = m.timing.frames();
        const uint32_t mask = m.bus.read32(0x140128);
        if (mask != m.last_mask) m.mask_since = m.last_mask ? frame : 0, m.last_mask = mask; // (the first seen: no change)
        uint32_t least = 0xFFFF; // (the least time of the players in the game)
        for (uint32_t node = 0; node < 4; ++node)
            if (mask & (0x80000000u >> node)) least = std::min<uint32_t>(least, m.bus.read16(0x1062B0 + 2 * node));
        // (its coin or start buttons, bank 0, in the next `lookahead` frames: over the network as far as its inputs
        // are in the table, the input delay's frames ahead; else the given ones)
        bool joining = false;
        for (uint64_t f = frame; f < frame + m.lookahead && !joining; ++f) {
            const auto in = m.net_inputs ? m.inputs->get(gone, f) : settings_.buttons ? settings_.buttons(gone, f) : std::nullopt;
            joining = in && in->bank0 != 0xFF;
        }
        const bool in_game = m.bus.read8(0x10400A) == 7;
        if (!cab.shadowed(gone)) {
            // (tests: ONEBOARD_NO_SHADOWS=1, none after the first hand-back: side runs alone through everything. A shadow
            // here starts from this board's state, an approximation; network play's come from the other's real board)
            static const bool no_shadows = std::getenv("ONEBOARD_NO_SHADOWS") != nullptr;
            if (no_shadows) return;
            const bool trouble = m.whole || least <= 5 * 64 || !in_game || frame < m.mask_since + 60 || joining;
            bool waiting; // (a state taken, not yet used or refused by the leaving board: not asked again over it)
            {
                std::lock_guard guard(lock_);
                waiting = shadow_frame_ != ~0ull || master_ready_;
            }
            if (trouble && m.shadow_taken && !waiting) {
                m.shadow_from = frame, m.shadow_taken = false;
                std::printf("board %zu: a shadow of board %zu wanted at frame %llu (least time %u/64 s, mode %u, mask %08x%s)\n", m.index,
                            gone, (unsigned long long)frame, least, m.bus.read8(0x10400A), mask, joining ? ", its buttons" : "");
            }
            if (trouble && !m.shadow_from) m.shadow_from = frame;
            return;
        }
        const bool calm = !m.whole && least > 15 * 64 && in_game && other.bus.read8(0x10400A) == 7 &&
                          other.bus.read32(0x140128) == mask && std::popcount(mask) >= 2 && !joining;
        m.calm_frames = calm ? m.calm_frames + 1 : 0;
        if (other.held) // (a board still held: both RAMs wanted from 10 frames before the hand-back is asked)
            if (const bool want = m.calm_frames + 10 >= kCalm; want != m.keeping) m.keeping = other.keeping = want;
        if (m.calm_frames >= kCalm) {
            other.held = false; // (asked: kept from here, until the hand-back)
            cab.ask_hand_back(gone);
            m.calm_frames = 0;
            std::printf("board %zu: board %zu handed back to side runs from frame %llu\n", m.index, gone, (unsigned long long)frame);
        }
    }

    // The rules in network play, shadows from the other machines' real boards: each other cabinet followed on its own
    // (any number of them), as rules() follows one: a shadow of it asked of its machine when trouble may come (a player
    // with 5 s or less, a cabinet run whole here out of the game's mode, the players changed in the last 60 frames, any
    // cabinet's coin or start coming), handed back after 300 calm frames (the opposite with room to spare, it with the
    // same players).
    void rules_each(Board& m) {
        remote::Cabinets& cab = *m.cabinets;
        const uint64_t frame = m.timing.frames();
        if (rolling()) { // (switches done: their windows gone by)
            std::lock_guard guard(lock_);
            for (auto it = no_guess_.begin(); it != no_guess_.end();)
                it = it->second != ~0ull && it->second + 2 < frame ? no_guess_.erase(it) : std::next(it);
        }
        const uint32_t mask = m.bus.read32(0x140128);
        if (mask != m.last_mask) m.mask_since = m.last_mask ? frame : 0, m.last_mask = mask; // (the first seen: no change)
        uint32_t least = 0xFFFF; // (the least time of the players in the game)
        for (uint32_t node = 0; node < 4; ++node)
            if (mask & (0x80000000u >> node)) least = std::min<uint32_t>(least, m.bus.read16(0x1062B0 + 2 * node));
        const bool in_game = m.bus.read8(0x10400A) == 7;
        // (every cabinet run whole here (this one, those held or shadowed) in the game's mode: one out of it is a player
        // joining, or one gone after a game over, the link's players changing soon for all)
        bool all_in_game = in_game;
        for (const std::size_t gone : settings_.going)
            if (cab.shadowed(gone) && boards_[gone]->bus.read8(0x10400A) != 7) all_in_game = false;
        // (any cabinet's coin or start, as far as its inputs are in the table: a player joining changes every cabinet's game)
        bool joining = false;
        for (std::size_t seat = 0; seat < boards_.size() && !joining; ++seat)
            for (uint64_t f = frame; f < frame + m.lookahead && !joining; ++f) {
                const auto in = m.inputs->get(seat, f);
                joining = in && in->bank0 != 0xFF;
            }
        bool any_keeping = false;
        for (const std::size_t gone : settings_.going) {
            Board& other = *boards_[gone];
            if (!cab.making(gone) && !cab.shadowed(gone)) continue; // (its main board still there)
            if (!cab.shadowed(gone)) {
                const bool trouble =
                    m.whole || m.whole_for_state || least <= 5 * 64 || !all_in_game || frame < m.mask_since + 60 || joining;
                std::lock_guard guard(lock_);
                if (trouble && !remote_frames_.count(gone) && !lost_.count(gone)) { // (asked once, for a frame a little ahead)
                    remote_frames_[gone] = frame + kAskAhead;
                    settings_.ask_board(gone, frame + kAskAhead);
                    if (late_loads()) settings_.pin(frame + kAskAhead); // (kept as it is, should its state come late)
                    std::printf("board %zu: a shadow of board %zu wanted at frame %llu (least time %u/64 s, mode %u, mask %08x%s): its "
                                "machine's state asked for frame %llu\n",
                                m.index, gone, (unsigned long long)frame, least, m.bus.read8(0x10400A), mask, joining ? ", its buttons" : "",
                                (unsigned long long)(frame + kAskAhead));
                }
                continue;
            }
            {
                std::lock_guard guard(lock_);
                if (departed_.count(gone)) continue; // (its player gone: whole here from its parting on, never handed back)
            }
            const bool calm = !m.whole && !m.whole_for_state && least > 15 * 64 && all_in_game &&
                              other.bus.read32(0x140128) == mask && std::popcount(mask) >= 2 && !joining;
            uint64_t& calm_frames = calm_[gone];
            calm_frames = calm ? calm_frames + 1 : 0;
            if (other.held) other.keeping = calm_frames + 10 >= kCalm; // (still held: its seeds wanted from 10 frames before)
            any_keeping = any_keeping || other.keeping;
            if (calm_frames >= kCalm && rolling()) { // (rollback: no guesses from here; asked once every frame up to here is in)
                std::lock_guard guard(lock_);
                if (!no_guess_.count(gone)) no_guess_[gone] = ~0ull;
                if (!settings_.settled(frame)) continue;
            }
            if (calm_frames >= kCalm) {
                other.held = false; // (asked: its seeds kept from here, until the hand-back)
                cab.ask_hand_back(gone);
                calm_frames = 0;
                std::printf("board %zu: board %zu handed back to side runs from frame %llu\n", m.index, gone, (unsigned long long)frame);
            }
        }
        m.keeping = any_keeping;
    }

    // The staying board's hook (the other cabinets' side runs first; nothing else in a side run).
    static void staying_hook(recomp::Context& c, uint32_t pc, void* user) {
        Board& m = *static_cast<Board*>(user);
        Game& g = *m.game;
        if (m.cabinets->inside()) { // (a side run's: only where it ends)
            m.cabinets->hook(pc);
            return;
        }
        // (a shadow of the master: at this board's dispatcher, from the master's own run then)
        if (pc == remote::kModeDispatch && m.shadow_from && !m.shadow_taken && m.timing.frames() >= m.shadow_from && m.index != 0) {
            std::vector<uint8_t> ram;
            ppc::CpuState cpu{};
            if (m.cabinets->shadow_master(0, ram, cpu)) {
                m.shadow_taken = true;
                m3::State s; // (this board's devices and clock now; the master's CPU at its vblank wait)
                board_state(s, m.c, m.timing, {});
                std::lock_guard guard(g.lock_);
                g.state_ = std::move(s.data()), g.master_ram_ = std::move(ram), g.master_cpu_ = cpu;
                g.master_ready_ = true;
                std::printf("board %zu: the master's shadow from its side run of count %u (frame %llu)\n", m.index,
                            m.bus.read32(remote::kLogicCounter), (unsigned long long)m.timing.frames());
            }
        }
        probe(m, pc);
        if (pc == remote::kLinkSkipped) keep_ram(m);
        if (m.cabinets->hook(pc)) return;
        if (pc == remote::kLinkSkipped && m.one_board_auto) g.rules(m);
        if (pc == remote::kLinkSkipped && m.hand_back_at && m.timing.frames() == m.hand_back_at) // --one-board-hand-back FRAME
            m.cabinets->ask_hand_back(m.peer);
        // (a shadow of a slave: this board's state where it has finished a count's work)
        if (pc == remote::kLinkSkipped && m.shadow_from && !m.shadow_taken && m.timing.frames() >= m.shadow_from && m.index == 0) {
            m.shadow_taken = true;
            const uint32_t count = m.bus.read32(remote::kLogicCounter);
            const uint64_t frame = m.timing.frames();
            std::printf("board %zu: state for the shadow asked at frame %llu, count %u\n", m.index, (unsigned long long)frame, count);
            c.request_snapshot([&g, &m, count, frame](uint32_t at) {
                std::printf("board %zu: state for the shadow taken at frame %llu (asked at %llu), pc %08x%s\n", m.index,
                            (unsigned long long)m.timing.frames(), (unsigned long long)frame, at,
                            m.cabinets->inside() ? ", in a side run" : "");
                m3::State s;
                board_state(s, m.c, m.timing, {});
                std::lock_guard guard(g.lock_);
                g.state_ = std::move(s.data()), g.shadow_count_ = count, g.shadow_frame_ = frame, g.shadow_cycles_ = m.c.cycles();
            });
        }
        m3::games::spikeout_hook(c, pc, nullptr);
    }

    // The leaving board's hook: its own variables as each flow of its starts, while it still runs.
    static void leaving_hook(recomp::Context& c, uint32_t pc, void* user) {
        Board& m = *static_cast<Board*>(user);
        Game& g = *m.game;
        if (pc == remote::kRecordWrite) m.last_link_frame = m.timing.frames(); // (its link of the frame: where its blocks are its own)
        probe(m, pc);
        if (pc == remote::kLinkSkipped && m.keeping) { // (held, and no hand-back near: nothing of it wanted)
            keep_ram(m);
            // (where it finished a count: its own variables, camera and regions then, which side runs as it start from)
            static const std::vector<uint32_t> own = remote::own_addresses();
            remote::Seed seed;
            const auto ram = m.bus.ram();
            seed.own.resize(own.size());
            for (std::size_t k = 0; k < own.size(); ++k) seed.own[k] = ram[own[k]];
            seed.camera.assign(ram.begin() + remote::kCamera, ram.begin() + remote::kCamera + remote::kCameraBytes);
            seed.regions = remote::gather_regions(ram);
            seed.own_logic = m.bus.read32(remote::kLogicCounter);
            const uint32_t count = seed.own_logic;
            std::lock_guard guard(m.cabinets_lock);
            m.done_seed[count] = std::move(seed);
            while (m.done_seed.size() > 4) m.done_seed.erase(m.done_seed.begin());
            if (m.shadowing) { // (a shadow: stopped here when the module wants it back, after this count (0: not asked yet,
                               // where the count is still 0 at power-on))
                Board& other = *g.boards_[m.peer];
                if (const uint32_t stop = other.cabinets->stop_after(m.index); stop != 0 && stop == count) throw HandedBack{count};
            }
        }
        if (pc == remote::kModeDispatch && !g.settings_.cameras->empty()) { // (only gamerun's camera recording reads them)
            static const std::vector<uint32_t> own = remote::own_addresses();
            std::vector<uint8_t> bytes(own.size());
            for (std::size_t k = 0; k < own.size(); ++k) bytes[k] = m.bus.read8(own[k]);
            std::lock_guard guard(m.cabinets_lock);
            m.own_seed[m.bus.read32(remote::kLogicCounter)] = std::move(bytes);
        }
        m3::games::spikeout_hook(c, pc, nullptr);
    }

    // Network play: the shadow from the other board's real state at the end of `frame`, from its player's machine
    // (waited for here, at the top of the frame on from it: the staying board waits at its first meeting meanwhile).
    bool load_remote_shadow(Board& m, uint64_t frame) {
        const auto started = std::chrono::steady_clock::now();
        std::optional<RemoteBoard> state;
        bool replayed = false;
        {
            std::unique_lock guard(lock_);
            if (const auto it = replays_.find(m.index); it != replays_.end()) { // (its player gone: run on here from its backup)
                replayed = true;
                replayed_.wait(guard, [&] { return replays_[m.index].done; });
                state = std::move(replays_[m.index].board);
                replays_.erase(m.index);
                if (!state) lost_.insert(m.index), departed_.erase(m.index);
            }
        }
        if (!replayed) state = settings_.wait_board(m.index, frame);
        if (!state) {
            std::printf("board %zu: no state of it %s for frame %llu\n", m.index, replayed ? "from its backup" : "came from its machine",
                        (unsigned long long)frame);
            return false;
        }
        Board& other = *boards_[m.peer];
        {
            std::lock_guard guard(other.cabinets_lock);
            other.cabinets->stop_making(m.index);
        }
        if (state->from_backup()) { // (its big buffers copied in, then the small rest loaded around them: backup::Replay)
            struct Collect : m3::State::Regions {
                std::vector<std::pair<uint8_t*, std::size_t>> found;
                void region(void* data, std::size_t bytes) override { found.emplace_back(static_cast<uint8_t*>(data), bytes); }
            } here;
            {
                m3::State s;
                s.set_regions(&here);
                board_state(s, m.c, m.timing, {});
            }
            if (here.found.size() != state->regions.size()) throw std::runtime_error("shadow from a backup: not this board's buffers");
            for (std::size_t r = 0; r < here.found.size(); ++r) {
                if (here.found[r].second != state->regions[r].size()) throw std::runtime_error("shadow from a backup: a buffer's size");
                std::memcpy(here.found[r].first, state->regions[r].data(), here.found[r].second);
            }
            Collect check;
            m3::State s(std::move(state->small));
            s.set_regions(&check);
            board_state(s, m.c, m.timing, {});
        } else {
            m3::State s(std::move(state->state));
            board_state(s, m.c, m.timing, {}); // (its network board, EEPROM and all, as they are there)
        }
        // (and the ring's packets to it from where its network board had taken them in there: over there this board's
        // blocks are written by the module, later in the frame, so one may not have come yet that has come here)
        if (settings_.rewind_link) settings_.rewind_link(m.index, *state);
        shadow_inputs(m);
        m.shadowing = true;
        std::printf("board %zu: its shadow from its machine's state at frame %llu (waited %.0f ms; board %zu at frame %llu)\n", m.index,
                    (unsigned long long)frame, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                    other.index, (unsigned long long)other.timing.frames());
        return true;
    }

    // The master's shadow: once the staying board has its state (at its dispatcher's entry) and the master's RAM
    // (Cabinets::shadow_master), this board goes on from them, as the master, at that time: its own network board,
    // EEPROM and inputs kept, its role.
    bool load_master_shadow(Board& m) {
        std::vector<uint8_t> state, ram;
        ppc::CpuState cpu{};
        {
            std::lock_guard guard(lock_);
            if (!master_ready_) return false;
            master_ready_ = false;
            state = std::move(state_), ram = std::move(master_ram_), cpu = master_cpu_;
        }
        m3::State nb, ee;
        m.bus.netboard().state(nb);
        m.bus.eeprom().state(ee);
        const auto inputs = m.bus.inputs;
        const uint8_t role = m.bus.read8(kRole);
        m3::State s(std::move(state));
        board_state(s, m.c, m.timing, {});
        m3::State nb_in(std::move(nb.data())), ee_in(std::move(ee.data()));
        m.bus.netboard().state(nb_in);
        m.bus.eeprom().state(ee_in);
        m.bus.inputs = inputs;
        shadow_inputs(m);
        // (the master's RAM whole, as its run through its drawing left it, there in 0x36E94: its OS's work too (inside
        // its calls there; its transfers' flags cleared where they were waited for))
        std::copy(ram.begin(), ram.end(), m.bus.ram().begin());
        // (in 0x36E94, its MSR as a board's there; on at its idle wait (0x36F44) with the flag as read there (r3, 0),
        // the tile generator's flip before it (0x36F30, its 2D) passed by)
        m.c.cpu = cpu;
        m.c.cpu.pc = 0x36F44, m.c.cpu.gpr[3] = 0;
        // (its wake-up, which came at its link time just before (this board's decrementer had fired): pending, to come
        // as soon as it waits)
        m.bus.write8(remote::kWakeFlag, 0);
        {
            recomp::Context::Aside a = m.c.set_aside();
            a.dec_pending = true;
            m.c.put_back(a);
        }
        m.bus.write8(kRole, role);
        m.shadowing = true;
        std::printf("board %zu: the master's shadow from frame %llu, cycle %llu\n", m.index, (unsigned long long)m.timing.frames(),
                    (unsigned long long)(m.c.cycles() - m.timing.frame_start(m.timing.frames())));
        return true;
    }

    std::vector<Board*> boards_;
    Settings settings_;
    // A shadow's start: the staying board's state, taken where it finished count shadow_count_, in its frame
    // shadow_frame_ (~0: not yet); or the master's (master_ready_): the staying board's state at its dispatcher's
    // entry, the master's RAM and CPU.
    mutable std::mutex lock_;
    std::vector<uint8_t> state_;
    uint32_t shadow_count_ = 0;
    uint64_t shadow_frame_ = ~0ull, shadow_cycles_ = 0; // (and its time there)
    // (network play: by board, the frame whose end its state was asked for of its machine; and its calm frames in a row;
    // the seats whose player left with a parting (kept whole), or went without one (never asked for))
    std::map<std::size_t, uint64_t> remote_frames_, calm_;
    std::map<std::size_t, uint64_t> no_guess_; // (rollback: by board being switched, the frame through which no guesses)
    Turns* turns_ = nullptr; // (set_turns)
    uint64_t held_waits_ = 0;          // (held_waits: the staying board's thread only)
    double held_wait_seconds_ = 0;
    bool rolling() const { return static_cast<bool>(settings_.settled); }
    // (rollback: a shadow's state loaded late, the game gone back to its frame (Settings::has_board); tests and the
    // old way: ONEBOARD_WAIT_SHADOWS=1, waited for at its frame)
    bool late_loads() const {
        static const bool wait = std::getenv("ONEBOARD_WAIT_SHADOWS") != nullptr;
        return rolling() && settings_.has_board && settings_.run_again_from && settings_.pin && !wait;
    }
    // (a shadow whose state hadn't come by its frame: that frame, and whether every board was sent back to it; lock_)
    struct Late {
        uint64_t frame = 0;
        bool back_asked = false;
    };
    std::map<std::size_t, Late> late_;
    std::map<std::size_t, uint64_t> loading_; // (a shadow loading at that frame: all_come; lock_)
    std::mutex group_lock_; // (shadow_wait: whether each shadow loads at its frame, one board at a time)
    // (going back from as far as the window less this: waited for there, the frames since still kept)
    static constexpr uint64_t kLateRoom = 20;
    // (whether a shadow's state for `want` has come, at this board's frame `f`; tests: ONEBOARD_LATE_TEST=N, taken
    // as not come before frame want + N)
    bool come(std::size_t seat, uint64_t want, uint64_t f, uint32_t ms) const {
        static const uint64_t hold = [] {
            const char* n = std::getenv("ONEBOARD_LATE_TEST");
            return n ? std::strtoull(n, nullptr, 0) : uint64_t{0};
        }();
        if (f < want + hold) {
            std::lock_guard guard(lock_);
            if (const auto l = late_.find(seat); l == late_.end() || !l->second.back_asked) return false; // (gone back for it: come)
        }
        return settings_.has_board(seat, want, ms);
    }
    // (another shadow of that frame found late already, not gone back for yet: this one waits with it, though its own
    // state has come, the two loaded there together; loaded first, that frame couldn't be gone back to for the other)
    bool late_waiting(uint64_t want) const {
        std::lock_guard guard(lock_);
        for (const auto& [seat, l] : late_)
            if (l.frame == want && !l.back_asked) return true;
        return false;
    }
    // (and every other shadow asked for the same frame)
    bool all_come(uint64_t want, uint64_t f, uint32_t ms) const {
        std::vector<std::size_t> seats;
        {
            std::lock_guard guard(lock_);
            // (one being loaded: come, its state taken from those come (wait_board) as it loads)
            for (const auto& [seat, at] : remote_frames_)
                if (at == want && !(loading_.count(seat) && loading_.at(seat) == want)) seats.push_back(seat);
        }
        for (const std::size_t seat : seats)
            if (!come(seat, want, f, ms)) return false;
        return true;
    }
    std::set<std::size_t> departed_, lost_;
    // (a board whose player went, run on here from its backup: by board, the run's state when done)
    struct Replaying {
        bool done = false;
        std::optional<RemoteBoard> board;
    };
    std::map<std::size_t, Replaying> replays_;
    std::condition_variable replayed_, live_changed_;
    uint64_t live_frame_ = 0;
    bool live_stopped_ = false;
    std::vector<std::jthread> replay_threads_;
    void live(uint64_t frame, bool stop = false) {
        {
            std::lock_guard guard(lock_);
            if (stop) live_stopped_ = true;
            else live_frame_ = frame;
        }
        live_changed_.notify_all();
    }
    bool wait_live(uint64_t frame) {
        std::unique_lock guard(lock_);
        live_changed_.wait(guard, [&] { return live_stopped_ || live_frame_ >= frame; });
        return live_frame_ >= frame;
    }
    std::vector<uint8_t> master_ram_;
    ppc::CpuState master_cpu_{};
    bool master_ready_ = false;
};

} // namespace tools::one_board
