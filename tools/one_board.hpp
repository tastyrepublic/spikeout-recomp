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
#include <bit>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tools::one_board {

// Thrown from a shadow's hook where it finished `count`: the module wants it back there (Game::handed_back).
struct HandedBack {
    uint32_t count;
};

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
    // the rules, on the one that stays: frames calm in a row while a shadow runs; the mask last seen and since when
    uint64_t calm_frames = 0, mask_since = 0;
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
    };

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
                if (!in && frame >= table->next(seat)) in = table->wait(seat, frame);
                return in ? remote::held_word(in->bank0, in->controls, in->controls2) : 0u; // (before its first: none)
            }
            const auto in = settings_.buttons ? settings_.buttons(seat, frame) : std::nullopt;
            return in ? remote::held_word(in->bank0, in->controls) : remote::held_word(0xFF, 0xFF);
        };
        m.cabinets = std::make_unique<remote::Cabinets>(m.c, m.bus, m.timing, stays, held);
        m.shadow_from = settings_.shadow_from;
        m.keep_ram = settings_.keep_ram;
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
        std::lock_guard guard(m.cabinets_lock);
        const uint64_t f = m.timing.frames();
        m.cabinets->meet(f, m.timing.frame_start(f) + m.timing.line_cycles() * (recomp::FrameTiming::kVBlankLines + line));
    }
    // The staying board's frame end: the other cabinets' frames end with it (frames() counts those done).
    static void end_frame(Board& m) {
        if (!m.cabinets) return;
        std::lock_guard guard(m.cabinets_lock);
        m.cabinets->end_frame(m.timing.frames() - 1);
    }

    // A board that goes, at the end of the hand-over's frame (no camera recording): handed over as a shadow is,
    // where the module asks it to stop (a slave's next count then caught up there): its camera and own variables
    // its own, as they are. `held`: not asked yet, run on whole until the rules find it calm.
    void hand_over(Board& m, bool held) {
        Board& other = *boards_[m.peer];
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
            other.cabinets->hand_back(m.index, m.done_seed.at(count), m.bus.ram(),
                                      m.last_link_frame ? m.last_link_frame : m.timing.frames() - 1);
        }
        std::printf("board %zu: its shadow handed back to board %zu's side runs after count %u (frame %llu)\n", m.index, other.index,
                    count, (unsigned long long)m.timing.frames());
        if (!other.one_board_auto && !other.shadow_from) { // (no shadow of it can be wanted: out of the meetings)
            if (why.empty()) why = "one board: its shadow handed back";
            return false;
        }
        if (shadow_wait(m, turns, m.slice)) return true;
        why = "one board: its shadow handed back, no other wanted";
        return false;
    }

    // A board stopped. The staying board in network play: its shadows of the others with it (their players may have
    // gone already, nothing more of theirs to wait for).
    static void stopped(Board& m) {
        if (m.net_inputs && m.cabinets && m.inputs) m.inputs->close();
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
            {
                std::scoped_lock guard(lock_, other.cabinets_lock);
                if (shadow_frame_ != ~0ull && f == shadow_frame_ + 1) {
                    if (other.cabinets->shadow_state(m.index, shadow_count_, own)) break;
                    // (none for that count: that cabinet a count off this board's just then, after a frame it missed
                    // or held back: asked again the next frame)
                    std::printf("board %zu: no shadow state for count %u, again\n", m.index, shadow_count_);
                    shadow_frame_ = ~0ull, other.shadow_taken = false;
                }
                if (shadow_frame_ != ~0ull && f > shadow_frame_ + 1) {
                    std::printf("board %zu: the staying board's state came too late (taken at frame %llu, here at %llu)\n", m.index,
                                (unsigned long long)shadow_frame_, (unsigned long long)f);
                    return false;
                }
            }
            if (turns.alone(m.index)) return false;
            // (its turns at the frame's lines, as a running board takes them; in the first, those still to come)
            const uint64_t now = m.c.cycles(), start = m.timing.frame_start(f) + m.timing.line_cycles() * recomp::FrameTiming::kVBlankLines;
            for (uint32_t line = slice - 1; line < recomp::FrameTiming::kDisplayLines; line += slice)
                if (start + line * m.timing.line_cycles() >= now) {
                    turns.pass(m.index);
                    if (load_master_shadow(m)) return true; // (the master's: as soon as it is there, mid-frame)
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

    static void keep_ram(Board& m) { // (at 0x36E94, while wanted: this board's RAM after the count it finished)
        if (!m.keep_ram) return;
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
            const bool trouble = least <= 5 * 64 || !in_game || frame < m.mask_since + 60 || joining;
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
        const bool calm = least > 15 * 64 && in_game && other.bus.read8(0x10400A) == 7 && other.bus.read32(0x140128) == mask &&
                          std::popcount(mask) >= 2 && !joining;
        m.calm_frames = calm ? m.calm_frames + 1 : 0;
        if (m.calm_frames >= 300) {
            cab.ask_hand_back(gone);
            m.calm_frames = 0;
            std::printf("board %zu: board %zu handed back to side runs from frame %llu\n", m.index, gone, (unsigned long long)frame);
        }
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
                g.state_ = std::move(s.data()), g.shadow_count_ = count, g.shadow_frame_ = frame;
            });
        }
        m3::games::spikeout_hook(c, pc, nullptr);
    }

    // The leaving board's hook: its own variables as each flow of its starts, while it still runs.
    static void leaving_hook(recomp::Context& c, uint32_t pc, void* user) {
        Board& m = *static_cast<Board*>(user);
        Game& g = *m.game;
        if (pc == remote::kRecordWrite) m.last_link_frame = m.timing.frames(); // (its link of the frame: where its blocks are its own)
        if (pc == remote::kLinkSkipped) {
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
        if (pc == remote::kModeDispatch) {
            static const std::vector<uint32_t> own = remote::own_addresses();
            std::vector<uint8_t> bytes(own.size());
            for (std::size_t k = 0; k < own.size(); ++k) bytes[k] = m.bus.read8(own[k]);
            std::lock_guard guard(m.cabinets_lock);
            m.own_seed[m.bus.read32(remote::kLogicCounter)] = std::move(bytes);
        }
        m3::games::spikeout_hook(c, pc, nullptr);
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
    std::mutex lock_;
    std::vector<uint8_t> state_;
    uint32_t shadow_count_ = 0;
    uint64_t shadow_frame_ = ~0ull;
    std::vector<uint8_t> master_ram_;
    ppc::CpuState master_cpu_{};
    bool master_ready_ = false;
};

} // namespace tools::one_board
