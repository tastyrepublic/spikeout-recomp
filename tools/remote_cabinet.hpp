// One board a PC (docs/netplay.md, "One board a PC"): the other cabinets of
// a linked game made here, from this board's own game and their players'
// buttons, with no main board of theirs running. Each other cabinet keeps
// its network board (on the ring as before), fed each frame a block made the
// way its main board would have written it: this board's records of the same
// logic counts (the shared game is the same on every board) with that
// cabinet's own fields: its buttons, its camera's heading and view of every
// character (its camera run here on the side: its update, its view tests),
// its status (its own flow, the mode dispatcher, run here on the side with
// its own variables), its player's fields as this board has them when it
// writes its own record.
//
// A side run is game code run once more, as the other cabinet: its camera
// in the camera's place, its own variables in theirs, its buttons, its local
// player; the bus's journal puts RAM back after, device writes are held
// (its sounds aren't this board's), the CPU and its clock put back, no
// interrupts. So this board's own game is untouched: the same, hash for
// hash, as with the other boards running.
//
// Relations: a slave runs a frame behind the master. A record of logic count
// L carries its camera's heading after update L-1, its view of each
// character from the view tests of L-2, its player's first word's view bits
// from L-1. A slave's game reads the master's record from two counts back,
// which a slave has made itself by then; the master's newest (the count a
// slave hasn't reached) is made from the slave's own newest for now.
#pragma once

#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/netboard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tools::remote {

// The camera (r12 its update's, at 0x11D480; +0x2A its heading as the link
// sends it, +0x4A the player it follows, +0x640 its view matrix), its update,
// its view of a character (r14: flags bit 0x08 on its screen, 0x10 near it),
// what makes its view after the update; the update's end (0xC670 called from
// 0x32514). The logic counter.
constexpr uint32_t kCamera = 0x11D480, kCameraBytes = 0xC00, kCameraHeading = 0x2A, kCameraFollows = 0x4A, kViewMatrix = 0x640;
constexpr uint32_t kCameraUpdate = 0x2C4DC, kViewTest = 0x33A6C, kCameraView = 0x32578;
constexpr uint32_t kAfterUpdate = 0xC670, kAfterUpdateReturn = 0x32514;
constexpr uint32_t kLogicCounter = 0x1000A8;
// Where a cabinet writes its own record (+0x04 its buttons, +0x10 its
// player's first word, +0xDC the logic count).
constexpr uint32_t kRecordWrite = 0x6DA90;
// Camera shakes (0x345E4, r3 the kind: the table at 0x90114, 8 bytes a kind,
// its byte into the camera's +0xD2 and +0xD3, its float into +0xD4) decided
// by a cabinet's own player: 0x21C88 (a character's animation command, r14
// the character, r28+4 the kind: in mode 3 with a flag every cabinet's, else
// the local player's or one holding it) and 0x22750 (kind 7 for the player
// the camera follows). This board's own decisions are written at 0x345EC.
constexpr uint32_t kShakeByAnimation = 0x21C88, kShakeFollowed = 0x22750, kShakeWrite = 0x345EC, kShakeTable = 0x90114;
// A cabinet's own flow: the mode dispatcher, once a frame from the main loop.
constexpr uint32_t kModeDispatch = 0x492AC;
// This cabinet's own continue and name entry keep their working values in
// its camera (+0xB04-0xB1E): not the other's.
constexpr uint32_t kOwnCameraWriters[] = {0x7682C, 0x76F78, 0x76868, 0x76944};
// The block: header 0x100, four records of 0x200 (a record of logic count L
// in slot L & 3), at 0x141000 in RAM's order; written to the network board
// at 0xC0000100.
constexpr uint32_t kBlock = 0x141000, kBlockWords = 0x240, kNetBlock = 0xC0000100;
using Block = std::array<uint32_t, kBlockWords>;
// The header's bytes that are each cabinet's own, kept from its last block
// (the counters +0x00, +0x1C, +0x58 one up a frame; its status +0x06-0x07
// from its flow).
constexpr uint32_t kHeaderOwn[] = {0x02, 0x03, 0x16, 0x17, 0x1E, 0x1F, 0x23, 0x25, 0x26, 0x41, 0x44, 0x45, 0x46, 0x47, 0x4E, 0x4F, 0x5A, 0x5B};
constexpr uint32_t kHeaderCounters[] = {0x00, 0x1C, 0x58};

// A cabinet's own variables (its flow's, its camera's): the bytes of r13's
// areas that differ between two linked boards through a time over, a
// continue and a game over, less the main loop's timings; its buttons'
// words (0x5094's) and copies; its header status; each character's view of
// it (+0x34A0-0x34BF, which its view tests write and its camera's update
// reads) and view bits (the first word's low byte).
constexpr std::pair<uint32_t, uint32_t> kCabinetOwn[] = {
    {0x104000, 0x104014}, {0x10401A, 0x10401C}, {0x104024, 0x10402C}, {0x104051, 0x104052}, {0x10407B, 0x10407C},
    {0x104083, 0x104084}, {0x104090, 0x104094}, {0x1040B0, 0x1040BC}, {0x1040C7, 0x1040C8}, {0x1040F0, 0x1040FC},
    {0x104103, 0x104104}, {0x10065B, 0x10065C}, {0x106004, 0x10600C}, {0x106180, 0x106181}, {0x1061B7, 0x1061BB},
    {0x1061CD, 0x1061E6}, {0x106242, 0x10624D}, {0x106268, 0x10626C}, {0x106295, 0x1062A4}, {0x1062AD, 0x1062AE},
    {0x1062C7, 0x1062C8}, {0x1062DC, 0x1062DD}, {0x100100, 0x100120}, {0x140104, 0x140108},
    {0x1A34A0, 0x1A34C0}, {0x1A74A0, 0x1A74C0}, {0x1AB4A0, 0x1AB4C0}, {0x1AF4A0, 0x1AF4C0}, {0x1B34A0, 0x1B34C0},
    {0x1B74A0, 0x1B74C0}, {0x1BB4A0, 0x1BB4C0}, {0x1BF4A0, 0x1BF4C0}, {0x1C34A0, 0x1C34C0}, {0x1C74A0, 0x1C74C0},
    {0x1CB4A0, 0x1CB4C0}, {0x1CF4A0, 0x1CF4C0},
    {0x1A0003, 0x1A0004}, {0x1A4003, 0x1A4004}, {0x1A8003, 0x1A8004}, {0x1AC003, 0x1AC004}, {0x1B0003, 0x1B0004},
    {0x1B4003, 0x1B4004}, {0x1B8003, 0x1B8004}, {0x1BC003, 0x1BC004}, {0x1C0003, 0x1C0004}, {0x1C4003, 0x1C4004},
    {0x1C8003, 0x1C8004}, {0x1CC003, 0x1CC004}};

inline std::vector<uint32_t> own_addresses() {
    std::vector<uint32_t> out;
    for (const auto& [lo, hi] : kCabinetOwn)
        for (uint32_t a = lo; a < hi; ++a) out.push_back(a);
    return out;
}
// A player's object (0..3), an enemy's (0..7).
constexpr uint32_t player_object(std::size_t seat) { return 0x1A0000 + 0x4000 * static_cast<uint32_t>(seat); }
constexpr uint32_t character_object(uint32_t k) { return k < 4 ? 0x1A0000 + 0x4000 * k : 0x1B0000 + 0x4000 * (k - 4); }

// A seat's buttons word (0x5094's held word: the system board's 0x04 under
// select 0 in bits 0-7, 0x08 in 8-15, 0x0C in 16-23, 0x04 under select 1 in
// 24-31, inverted; the EEPROM's data line idle) from its input bytes.
constexpr uint32_t held_word(uint8_t bank0, uint8_t controls, uint8_t controls2 = 0xFF) {
    return ~(uint32_t{bank0} | uint32_t{controls} << 8 | uint32_t{controls2} << 16 | 0xFFu << 24);
}

// What a side cabinet starts from: its camera (kCameraBytes) and its own
// variables (own_addresses' order), the latter after its flow of count
// `own_logic`.
struct Seed {
    std::vector<uint8_t> camera, own;
    uint32_t own_logic = 0;
};

class Cabinets {
public:
    // `held(seat, frame)`: that seat's held buttons word for its frame (each
    // board's frames count alike). `write_at`: when in its frame a made block
    // is written (cycles after the frame starts; a cabinet's own is written
    // about 0.68 of a frame in).
    using Held = std::function<uint32_t(std::size_t seat, uint64_t frame)>;
    // A side cabinet's start, asked at this board's flow of count `logic` (its camera as its update of
    // `logic` starts, its own variables after its flow of Seed::own_logic).
    using Seeder = std::function<bool(std::size_t seat, uint32_t logic, Seed& out)>;
    Cabinets(recomp::Context& c, m3::Bus& bus, recomp::FrameTiming& timing, std::size_t local_seat, Held held,
             uint64_t write_at = 2400000)
        : c_(c), bus_(bus), timing_(timing), local_(local_seat), held_(std::move(held)), write_at_(write_at), own_(own_addresses()),
          own_mask_(own_.size(), 0xFF) {
        // (a character's first word's low byte: only its view bits, 0x08 on screen and 0x10 near, are a cabinet's own; the rest every board's)
        for (std::size_t k = 0; k < own_.size(); ++k)
            for (uint32_t ch = 0; ch < 12; ++ch)
                if (own_[k] == character_object(ch) + 3) own_mask_[k] = 0x18;
    }

    // Another cabinet: its seat, its logic count less this board's at the
    // same time (+1 the master seen from a slave, -1 a slave from the
    // master, 0 one slave from another), and how its side runs start (asked
    // at this board's flows until it can; false: not yet).
    void add(std::size_t seat, int ahead, Seeder seeder) {
        Remote r;
        r.seat = seat, r.ahead = ahead, r.seeder = std::move(seeder);
        remotes_.push_back(std::move(r));
    }
    // From now its blocks are made here, into its network board (its main
    // board gone): `last` its last block as its main board wrote it.
    void start_making(std::size_t seat, m3::NetBoardDevice& netboard, const Block& last) {
        if (Remote* r = find(seat)) r->netboard = &netboard, r->block = last, r->making = true, r->placeholder.fill(false);
    }
    bool making(std::size_t seat) const {
        for (const Remote& r : remotes_)
            if (r.seat == seat) return r.making;
        return false;
    }

    // The board's hook, for every hooked block: true when it was one of
    // these (or a side run's, which runs no other hooks either).
    bool hook(uint32_t pc);
    // The camera's write watch (bus.watch_writes(kCamera, kCamera +
    // kCameraBytes, &Cabinets::camera_written, this)): other code's writes
    // into this board's camera (the game's events) made to the others' too.
    static void camera_written(void* user, uint32_t addr, uint32_t value, uint32_t size);
    // At each of this board's meetings (cycle `t` of its frame `frame`) and
    // at its frame's end: the blocks due written, the network boards brought
    // up to then.
    void meet(uint64_t frame, uint64_t t);
    void end_frame(uint64_t frame);

    // In a side run (the board's other hooks are skipped then too).
    bool inside() const { return inside_; }
    uint64_t made() const { return made_; }
    double side_seconds() const { return side_seconds_; }
    uint64_t side_runs() const { return side_runs_; }
    std::string failed() const { return failed_; }

private:
    struct Remote {
        std::size_t seat = 0;
        int ahead = 0;
        m3::NetBoardDevice* netboard = nullptr;
        Seeder seeder;
        std::vector<uint8_t> camera, own; // (empty: not started)
        uint32_t own_logic = 0;           // (its own variables are after its flow of this count)
        uint64_t toggled_at = ~0ull;      // (the frame its toggles were flipped for)
        std::map<uint32_t, uint16_t> heading_at;                   // (after its update of a count)
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> view_at; // ((count, character): its first word after the test)
        std::map<uint32_t, std::array<uint8_t, 2>> status_at;      // (its header +0x06-0x07 after its flow of a count)
        bool making = false;
        Block block{};                     // (its last block)
        std::array<bool, 4> placeholder{}; // (a slot holding a count's record made before this board had it)
        uint64_t written_frame = ~0ull;
    };
    struct Players { // (as this board writes its own record of a count: each player's first word, +0x44, +0x2A, +0x74, +0x76)
        std::array<std::array<uint32_t, 5>, 4> p{};
    };

    Remote* find(std::size_t seat) {
        for (Remote& r : remotes_)
            if (r.seat == seat) return &r;
        return nullptr;
    }
    bool started(Remote& r, uint32_t logic);
    // Game code at `pc` run as cabinet `r` (see above); `flags`: the
    // character's (r14) first word after it. Its camera and own variables
    // after it kept (`keep`).
    void side_run(Remote& r, uint32_t pc, uint32_t* flags, bool keep);
    void shake(Remote& r, uint32_t kind);
    void make_block(Remote& r, uint64_t frame);
    void make_record(Remote& r, Block& next, const Block& mine, uint32_t logic, uint64_t frame, uint32_t old);
    uint8_t get8(const Block& b, uint32_t byte) const { return static_cast<uint8_t>(b[byte / 4] >> (8 * (3 - byte % 4))); }
    void put8(Block& b, uint32_t byte, uint8_t v) const {
        uint32_t& w = b[byte / 4];
        const uint32_t shift = 8 * (3 - byte % 4);
        w = (w & ~(0xFFu << shift)) | (uint32_t{v} << shift);
    }
    static int newest(const Block& b) {
        int best = 0;
        for (int s = 1; s < 4; ++s)
            if (b[(0x100 + s * 0x200 + 0xDC) / 4] > b[(0x100 + best * 0x200 + 0xDC) / 4]) best = s;
        return best;
    }

    recomp::Context& c_;
    m3::Bus& bus_;
    recomp::FrameTiming& timing_;
    std::size_t local_;
    Held held_;
    uint64_t write_at_;
    std::vector<uint32_t> own_;
    std::vector<uint8_t> own_mask_; // (the bits of each that are a cabinet's own)
    std::vector<Remote> remotes_;
    std::map<uint32_t, Players> players_at_; // (by count)
    bool inside_ = false, in_view_ = false, in_update_ = false;
    uint64_t made_ = 0, side_runs_ = 0;
    double side_seconds_ = 0;
    std::string failed_;
};

// ---- the implementation ----

inline bool Cabinets::started(Remote& r, uint32_t logic) {
    if (!r.camera.empty()) return true;
    Seed s;
    if (!r.seeder || !r.seeder(r.seat, logic, s) || s.camera.size() != kCameraBytes || s.own.size() != own_.size()) return false;
    r.camera = std::move(s.camera), r.own = std::move(s.own), r.own_logic = s.own_logic;
    return true;
}

inline void Cabinets::side_run(Remote& r, uint32_t pc, uint32_t* flags, bool keep_camera) {
    const uint32_t logic = bus_.read32(kLogicCounter);
    const recomp::Context::Aside aside = c_.set_aside();
    bus_.journal(true);
    for (uint32_t i = 0; i < kCameraBytes; ++i)
        if (bus_.read8(kCamera + i) != r.camera[i]) bus_.write8(kCamera + i, r.camera[i]);
    for (std::size_t k = 0; k < own_.size(); ++k) {
        const uint8_t here = bus_.read8(own_[k]), v = static_cast<uint8_t>((here & ~own_mask_[k]) | (r.own[k] & own_mask_[k]));
        if (v != here) bus_.write8(own_[k], v);
    }
    // (its buttons, 0x5094's words, for its frame of this count; its toggles flipped once a frame)
    const uint64_t frame = timing_.frames() - static_cast<uint64_t>(static_cast<int64_t>(r.ahead));
    const uint32_t now = held_(r.seat, frame), was = held_(r.seat, frame - 1);
    bus_.write32(0x100100, now), bus_.write32(0x100104, now & ~was), bus_.write32(0x100108, was & ~now);
    if (r.toggled_at != frame) bus_.write32(0x10010C, bus_.read32(0x10010C) ^ (now & ~was)), r.toggled_at = frame;
    // (its local player, its number, its time's entry: as 0x11C3C sets them)
    bus_.write32(0x104024, player_object(r.seat));
    bus_.write8(0x10065B, static_cast<uint8_t>(r.seat + 1));
    bus_.write32(0x106008, 0x1062AE + 2 * static_cast<uint32_t>(r.seat + 1));
    c_.set_scheduler(nullptr);
    c_.cpu.msr &= ~recomp::Context::kMsrEE;
    c_.limit_aside(30'000'000);
    inside_ = true;
    bus_.hold_device_writes(true);
    const auto started_at = std::chrono::steady_clock::now();
    try {
        if (pc == kViewTest) { // (with this camera's view matrix, as the game has it in f20-f31 then)
            for (int k = 0; k < 12; ++k) {
                const uint32_t bits = bus_.read32(kCamera + kViewMatrix + 4 * static_cast<uint32_t>(k));
                float f;
                std::memcpy(&f, &bits, 4);
                const double d = f;
                std::memcpy(&c_.cpu.fpr[static_cast<std::size_t>(20 + k)], &d, 8);
            }
            c_.call(pc);
        } else if (pc == kCameraUpdate) { // (the update, then what makes its view)
            c_.call(pc);
            c_.cpu = aside.cpu;
            c_.cpu.msr &= ~recomp::Context::kMsrEE;
            c_.cpu.gpr[12] = kCamera;
            c_.call(kCameraView);
        } else {
            c_.call(pc);
        }
    } catch (const std::exception& e) {
        if (failed_.empty()) failed_ = "seat " + std::to_string(r.seat + 1) + ", side run at " + std::to_string(pc) + ": " + e.what();
    }
    side_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
    ++side_runs_;
    inside_ = false;
    bus_.hold_device_writes(false);
    if (keep_camera)
        for (uint32_t i = 0; i < kCameraBytes; ++i) r.camera[i] = bus_.read8(kCamera + i);
    for (std::size_t k = 0; k < own_.size(); ++k) r.own[k] = bus_.read8(own_[k]);
    if (flags) *flags = bus_.read32(aside.cpu.gpr[14]);
    if (pc == kModeDispatch) r.status_at[logic] = {bus_.read8(0x140106), bus_.read8(0x140107)};
    bus_.undo_journal();
    bus_.journal(false);
    c_.put_back(aside);
}

inline void Cabinets::shake(Remote& r, uint32_t kind) {
    const uint32_t entry = kShakeTable + 8 * (kind - 1);
    r.camera[0xD2] = r.camera[0xD3] = bus_.read8(entry);
    for (uint32_t k = 0; k < 4; ++k) r.camera[0xD4 + k] = bus_.read8(entry + 4 + k);
}

inline bool Cabinets::hook(uint32_t pc) {
    if (inside_) return true;
    const uint32_t logic = bus_.read32(kLogicCounter);
    switch (pc) {
    case kCameraView: // (its writes are the camera's own, until the next view test)
        in_view_ = true;
        return true;
    case kAfterUpdate: // (and the update's, until it returns)
        if (c_.cpu.lr == kAfterUpdateReturn) in_update_ = false;
        return true;
    case kRecordWrite: {
        Players& at = players_at_[logic];
        for (std::size_t k = 0; k < 4; ++k) {
            const uint32_t p = player_object(k);
            at.p[k] = {bus_.read32(p), bus_.read32(p + 0x44), bus_.read16(p + 0x2A), bus_.read16(p + 0x74), bus_.read16(p + 0x76)};
        }
        while (!players_at_.empty() && players_at_.begin()->first + 16 < logic) players_at_.erase(players_at_.begin());
        return true;
    }
    case kModeDispatch:
        for (Remote& r : remotes_) {
            if (!started(r, logic) || r.own_logic >= logic) continue;
            const uint32_t runs = std::min<uint32_t>(logic - r.own_logic, 8);
            for (uint32_t k = 0; k < runs; ++k) side_run(r, pc, nullptr, true);
            r.own_logic = logic;
            while (!r.status_at.empty() && r.status_at.begin()->first + 16 < logic) r.status_at.erase(r.status_at.begin());
        }
        return true;
    case kCameraUpdate:
        in_update_ = true, in_view_ = false;
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            side_run(r, pc, nullptr, true);
            r.heading_at[logic] = static_cast<uint16_t>(r.camera[kCameraHeading] << 8 | r.camera[kCameraHeading + 1]);
            while (!r.heading_at.empty() && r.heading_at.begin()->first + 16 < logic) r.heading_at.erase(r.heading_at.begin());
            while (!r.view_at.empty() && r.view_at.begin()->first.first + 16 < logic) r.view_at.erase(r.view_at.begin());
        }
        return true;
    case kViewTest:
        in_view_ = false;
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            uint32_t flags = 0;
            side_run(r, pc, &flags, false);
            r.view_at[{logic, c_.cpu.gpr[14]}] = flags;
        }
        return true;
    case kShakeByAnimation:
    case kShakeFollowed: {
        const uint32_t r13 = c_.cpu.gpr[13], character = c_.cpu.gpr[14];
        for (Remote& r : remotes_) {
            if (r.camera.empty()) continue;
            uint32_t kind = 0;
            if (pc == kShakeFollowed) {
                if (r.camera[kCameraFollows] == bus_.read8(character + 114)) kind = 7;
            } else {
                const uint32_t local = player_object(r.seat);
                bool shake;
                if (bus_.read8(r13 - 16376) == 3) shake = (bus_.read32(r13 - 16380) & 0x01080000u) != 0;
                else if (bus_.read32(character) & 0x04000000u) shake = false;
                else shake = local == character || ((bus_.read32(character + 68) & 0x1000u) && bus_.read32(character + 272) == local);
                if (shake) kind = bus_.read8(c_.cpu.gpr[28] + 4);
            }
            if (kind) this->shake(r, kind);
        }
        return true;
    }
    default:
        return false;
    }
}

inline void Cabinets::camera_written(void* user, uint32_t addr, uint32_t value, uint32_t size) {
    Cabinets& self = *static_cast<Cabinets*>(user);
    if (self.inside_ || self.in_view_ || self.in_update_) return;
    const uint32_t pc = self.c_.cpu.pc;
    if (pc == kShakeWrite) return; // (decided again for each, at its hooks)
    for (uint32_t w : kOwnCameraWriters)
        if (pc == w) return;
    for (Remote& r : self.remotes_) {
        if (r.camera.empty()) continue;
        for (uint32_t k = 0; k < size; ++k) {
            const uint32_t at = addr + k - kCamera;
            // (+0x00-0x07 each cabinet's flags and counter; +0x4A8-0x4BF 0x32470's, the camera's own)
            if (at < 8 || at >= kCameraBytes || (at >= 0x4A8 && at < 0x4C0)) continue;
            r.camera[at] = static_cast<uint8_t>(value >> (8 * (size - 1 - k)));
        }
    }
}

inline void Cabinets::make_record(Remote& r, Block& next, const Block& mine, uint32_t logic, uint64_t frame, uint32_t old) {
    const uint32_t slot = logic & 3, rec = 0x100 + 0x200 * slot;
    int from = -1;
    for (int s = 0; s < 4; ++s)
        if (mine[(0x100 + s * 0x200 + 0xDC) / 4] == logic) from = s;
    const bool placeholder = from < 0; // (a count this board hasn't reached: its newest's for now)
    if (placeholder) from = newest(mine);
    const uint32_t src = 0x100 + 0x200 * static_cast<uint32_t>(from);
    for (uint32_t i = 0; i < 0x80; ++i) next[rec / 4 + i] = mine[src / 4 + i];
    if (placeholder) next[(rec + 0xDC) / 4] = logic, put8(next, rec + 0xD9, static_cast<uint8_t>(logic));
    r.placeholder[slot] = placeholder;
    // (its buttons)
    next[(rec + 0x04) / 4] = held_(r.seat, frame);
    // (its player's fields as this board had them as it wrote its own record of this count; else as now)
    const uint32_t player = player_object(r.seat);
    std::array<uint32_t, 5> p{bus_.read32(player), bus_.read32(player + 0x44), bus_.read16(player + 0x2A), bus_.read16(player + 0x74),
                              bus_.read16(player + 0x76)};
    if (const auto at = players_at_.find(logic); at != players_at_.end()) p = at->second.p[r.seat];
    next[(rec + 0x08) / 4] = p[2] << 16 | ((p[1] & 0x80000000u) ? p[4] : p[3]);
    // (its camera's heading after its update of the count before; else as it was)
    const auto heading = r.heading_at.find(logic - 1);
    next[(rec + 0x0C) / 4] = (heading != r.heading_at.end() ? uint32_t{heading->second} << 16 : r.block[(old + 0x0C) / 4] & 0xFFFF0000u) |
                             (next[(rec + 0x0C) / 4] & 0xFFFFu);
    // (its player's first word, its view bits from its camera's test of the count before)
    next[(rec + 0x10) / 4] = p[0];
    if (const auto v = r.view_at.find({logic - 1, player}); v != r.view_at.end()) next[(rec + 0x10) / 4] = (p[0] & ~0xFFu) | (v->second & 0xFFu);
    // (the empty slots' leftovers and the on-screen bits, as they were)
    next[(rec + 0x34) / 4] = r.block[(old + 0x34) / 4], next[(rec + 0x40) / 4] = r.block[(old + 0x40) / 4];
    for (uint32_t byte : {0x47u, 0x5Fu, 0x6Bu, 0x77u}) put8(next, rec + byte, get8(r.block, old + byte));
    // (every character's view bits, players 1-4 then enemies 1-8, from its camera's tests two counts before; none
    // for one gone since: this board's entry all zero)
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t entry = rec + 0x14 + 12 * k;
        const auto v = r.view_at.find({logic - 2, character_object(k)});
        if (v != r.view_at.end() && next[entry / 4] != 0) put8(next, entry + 3, static_cast<uint8_t>(v->second));
    }
}

inline void Cabinets::make_block(Remote& r, uint64_t frame) {
    Block mine{};
    for (uint32_t i = 0; i < kBlockWords; ++i) mine[i] = bus_.read32(kBlock + 4 * i);
    Block next = r.block;
    for (uint32_t i = 0; i < 0x40; ++i) next[i] = mine[i]; // (the header: this board's, with its own bytes)
    for (uint32_t byte : kHeaderOwn) put8(next, byte, get8(r.block, byte));
    // (its counters, each a frame on: its logic count this board's (in this board's header, written at its link,
    // before) and its offset; the other two moved as far)
    const uint32_t logic = mine[0x1C / 4] + static_cast<uint32_t>(r.ahead);
    const uint32_t step = logic - r.block[0x1C / 4];
    for (uint32_t w : kHeaderCounters) next[w / 4] = r.block[w / 4] + step;
    if (const auto s = r.status_at.find(logic - 1); s != r.status_at.end()) put8(next, 0x06, s->second[0]), put8(next, 0x07, s->second[1]);
    const uint32_t old = 0x100 + 0x200 * static_cast<uint32_t>(newest(r.block));
    // (every record of the last four counts: its newest made now, the older kept as made unless made then
    // before this board had them)
    for (uint32_t back = 4; back-- > 0;) {
        const uint32_t x = logic - back, slot = x & 3;
        if (back > 0 && r.block[(0x100 + 0x200 * slot + 0xDC) / 4] == x && !r.placeholder[slot]) continue;
        make_record(r, next, mine, x, frame - back, old);
    }
    for (uint32_t i = 0; i < kBlockWords; ++i) r.netboard->write32(kNetBlock + 4 * i, next[i]);
    r.block = next;
    ++made_;
}

inline void Cabinets::meet(uint64_t frame, uint64_t t) {
    for (Remote& r : remotes_) {
        if (!r.making) continue;
        const uint64_t at = timing_.frame_start(frame) + write_at_;
        if (r.written_frame != frame && t >= at) {
            r.netboard->sync(at);
            make_block(r, frame);
            r.written_frame = frame;
        }
        r.netboard->sync(t);
    }
}

inline void Cabinets::end_frame(uint64_t frame) {
    const uint64_t end = timing_.frame_start(frame) + timing_.frame_cycles();
    meet(frame, end);
    for (Remote& r : remotes_)
        if (r.making) r.netboard->end_frame();
}

} // namespace tools::remote
