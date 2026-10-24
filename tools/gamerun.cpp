// gamerun: run the recompiled boot ROM into the recompiled game and report
// how far it gets.
//
//   gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE] [--dump-textures FILE.png]
//           [--dump-backup FILE] [--eeprom FILE]
//           [--netboard sim|real] [--net-trace N] [--net-disasm FILE]
//           [--screenshot FILE.png] [--shot-every N] [--press [B:]NAME@FRAME[:N]]...
//           [--wav FILE.wav] [--probe X,Y@FRAME] [--quads]
//           [--boards N] [--slice LINES] [--link-latency CYCLES] [--renderer software|gpu|compare]
//           [--gpu-scale N] [--gpu-time N] [--wide E] [--screen-time WxH] [--match-models] [--parallel] [--no-2d] [--no-sound]
//
// Built only when CMake's SPIKEOUT_ROM_DIR is set. A run ends at the first
// thing the runtime can't do yet (code that wasn't found, an exception, a
// device the game waits on), or after N basic blocks (default 200 million).
// That stop, the blocks before it and the devices the game touched are
// what to work on next. --text prints the text on the tile layers (what
// screen the game is on); --dump-tile writes the tile generator's RAM at the
// end (characters, name tables, palette; derived from the ROMs).
// --replay FILE plays a recording (spikeout --record; tools::Recording) from
// power-on: its clock, EEPROM and backup RAM, its inputs each frame, main RAM
// checked against its hashes (the report says if the run parted from it, and
// where), to its end unless --blocks says otherwise.
// --save-state FILE@FRAME saves the whole board at the end of FRAME (in the
// first block after it outside an exception; tools::board_state), and
// --load-state FILE runs on from such a state (or the window's, F9) instead
// of from power-on; with --replay, the recording's inputs from there.
// --blocks then counts from the state. Loaded, a run is the same as the one
// that saved it, to the byte.
// --watch-write LO-HI@FRAME[:N] (hex addresses) prints every write to
// [LO, HI) in N frames from FRAME (default 1): the block writing, LR and
// registers (finding the code that draws something; work RAM too).
// --dump-backup writes the backup RAM (128 KB) at the end. --dump-layers
// writes each tile layer's whole 512x512 name table with every screenshot
// (FILE-<frame>-L<n>.png).
// --eeprom FILE starts with that settings EEPROM (128 bytes, as the window's
// nvram/board<N>.eeprom; else a blank one, which the game sets up).
// --dump-textures FILE.png writes the Real3D's texture RAM at the end as a
// 2048x2048 picture, each texel read as T1RGB5 (the other formats show as
// noise; derived from the ROMs).
//
// The network board is its own 68000 running the firmware the game uploads
// (m3::CommBoard); --netboard sim uses the simulation instead. --net-trace N
// prints its first N accesses to the board's registers, --net-disasm writes
// a disassembly of its RAM (the firmware; derived from the ROMs).
//
// --boards N runs N complete boards (cabinets) in one process, their network
// boards linked in a ring (m3::LinkRing): board i sends to board i + 1. Each
// runs on its own thread, but only one at a time: they take turns every
// --slice display lines (default 8), in board order, so a run is the same
// every time. A packet arrives once the receiver's clock reaches the time it
// was sent (plus --link-latency main board cycles, default 0). --parallel
// runs the boards at once instead, meeting every slice, where the link's
// packets go on (still the same every run; the latency at least a slice).
// --no-2d and --no-sound leave out the tile generator's drawing and the
// sound and music boards: the game runs the same without them (nothing
// reads them back), as a remote player's cabinet would.
// Each board's
// link setting is its own: set it with --poke (board 0 master, board 1
// slave: `--poke 0:10019E=1@0:2000 --poke 1:10019E=2@0:2000`). --blocks
// counts for each board. Files from boards after the first get "-bN" before
// their extension; --net-disasm writes board 0's firmware. --link-trace N
// prints the first N packets; --net-watch prints each board's network
// status registers when they change. The link's open questions
// (docs/fidelity.md) have switches: --net-e0-irq5 off, --net-send-waits off,
// --net-other-bank, --link-byte-cycles C (68000 cycles a byte on the wire).
//
// The tile generator draws a line at a time as the frame runs; the 3D is
// drawn at the end of a frame, only when screenshots are being written.
// --screenshot writes the last finished frame as a PNG; with --shot-every N
// also FILE-<frame>.png every N frames.
//
// --press [B:]NAME@FRAME[:N] holds an input for N frames (default 6) from
// FRAME: coin, start, test, service, beat, charge, shift, jump, up, down,
// left, right. Repeat it for more presses. --poke [B:]ADDR=VALUE@FRAME[:N]
// writes a byte of RAM. Without B: they apply to every board.
//
// The sound board (m3::SoundBoard) runs with the game; --wav writes what
// it played (44.1 kHz stereo).
//
// --probe X,Y@FRAME draws the 3D at FRAME and lists every triangle that
// covered pixel (X, Y), with its polygon header and what became of it there
// (drawn, behind, cut out, blended): for finding holes and wrong pixels.
// --quads draws quads whole, as Supermodel's "quad rendering" option (slower).
//
// --renderer gpu draws the 3D on the GPU instead (gpu::Real3dGpu, read back
// for the screenshots); --renderer compare draws it both ways and reports
// how far apart the pictures are, with each screenshot's GPU version beside
// it (FILE-<frame>-gpu.png). --gpu-time SCALE (with --screenshot) also draws
// each frame on the GPU at that render scale, not read back, and reports how
// long the GPU took a frame (drawn 8 times running, so its clock stays up;
// the screenshots stay the software renderer's).
// --gpu-scale N draws the GPU's 3D at render scale N (the screenshots take
// every Nth pixel); the report gives a hash of every GPU picture, full size,
// for checking that a change to the GPU renderer draws exactly the same.
// --wide E (with --renderer gpu, scale 1) draws widescreen, E pixels wider
// each side (the window's 16:9 is 93), and writes each screenshot's whole
// picture as the window shows it too (FILE-<frame>-wide.png). The report
// gives the 2D layers' drawing time a frame, and with --wide the time
// making their sides (on average and the worst frame), as the window's game
// thread spends them. --screen-time WxH (with --wide and --screenshot)
// times the window's screen pass at that size, drawn off screen, widescreen
// and 4:3. --match-models builds each frame's scene and reports how many of
// its models are found in the frame before's (smooth motion's matching: by
// viewport, address and culling-node path).
#include "games/games.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/real3d_interp.hpp"
#include "runtime/real3d_render.hpp"
#include "runtime/tilegen.hpp"
#include "runtime/widescreen.hpp"
#include "game_setup.hpp"
#include "recording.hpp"
#include "png_write.hpp"
#include "wav_write.hpp"
#ifdef SPIKEOUT_GPU
#include "gpu/present.hpp"
#include "gpu/real3d_gpu.hpp"

#include <SDL3/SDL_gpu.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kAllBoards = -1;
constexpr int kGpuTimeRepeats = 8; // --gpu-time draws each frame this many times

struct Press {
    std::string name;
    uint64_t frame = 0, frames = 6;
    int bank = 0; // 0: input bank 0 (coin, start...), 1: the controls byte
    int bit = 0;
    int board = kAllBoards;
};

// --poke ADDR=VALUE@FRAME[:N]: write a byte of RAM at the end of N frames
// (default 1) from FRAME, for experiments (a setting the game reads, say).
struct Poke {
    uint32_t addr = 0;
    uint8_t value = 0;
    uint64_t frame = 0, frames = 1;
    int board = kAllBoards;
};

struct Options {
    const char* dir = nullptr;
    const char* dump_tile = nullptr;
    const char* dump_textures = nullptr;
    const char* dump_backup = nullptr;
    const tools::Recording* replay = nullptr; // --replay FILE
    std::string save_state;                   // --save-state FILE@FRAME
    uint64_t save_frame = 0;
    const char* load_state = nullptr;         // --load-state FILE
    uint32_t watch_lo = 0, watch_hi = 0;      // --watch-write LO-HI@FRAME:N
    uint64_t watch_frame = 0, watch_frames = 1;
    bool dump_layers = false;
    const char* eeprom = nullptr; // --eeprom: the settings EEPROM to start with (128 bytes, high byte first)
    const char* net_disasm = nullptr;
    const char* screenshot = nullptr;
    const char* wav = nullptr;
    uint64_t shot_every = 0, tilegen_trace = 0;
    int probe_x = -1, probe_y = -1;
    int sound_volume = 100, music_volume = 100;
    uint64_t probe_frame = 0;
    std::vector<Press> presses;
    std::vector<Poke> pokes;
    bool show_text = false, real_netboard = true, force_master = false, quads = false, midi_burst = false,
         dma_instant = false;
    std::size_t net_trace = 0;
    int64_t blocks = 200'000'000;
    std::size_t boards = 1;
    uint32_t slice = 8;
    uint64_t link_latency = 0;
    bool parallel = false; // --parallel: the boards run at once (tools::Turns, m3::LinkRing deferred)
    // --no-2d, --no-sound: what a remote cabinet needn't run (the game the same without them)
    bool no_2d = false, no_sound = false;
    uint64_t link_trace = 0;
    bool net_watch = false;
    // --renderer: the 3D in software, on the GPU, or both, compared.
    enum class Renderer { Software, Gpu, Compare } renderer = Renderer::Software;
    int gpu_time_scale = 0; // --gpu-time: the GPU's time a frame at this render scale
    int gpu_scale = 1;      // --gpu-scale: the GPU renderer's render scale (gpu, compare)
    int wide = 0;           // --wide: widescreen, the picture this many pixels wider each side (gpu)
    int screen_w = 0, screen_h = 0; // --screen-time WxH: the window's screen pass timed at that size
    bool match_models = false;      // --match-models: smooth motion's matching of models between frames, reported
    double gpu_profile_over = 0;    // --gpu-slow MS (with --gpu-time): the frames the GPU took longer than that, listed
    bool gpu_hash_frames = false;   // --gpu-hash-frames: each GPU picture's hash, printed
    uint64_t gpu_full_frame = 0;    // --gpu-full FRAME: that frame's GPU picture written whole (FILE-<frame>-full.png)
    bool interp = false;            // --interp: smooth motion's pictures between frames, reported and drawn
    bool interp_trace = false;      // --interp-trace FROM:TO: and each frame's, printed
    uint64_t interp_trace_from = 0, interp_trace_to = 0;
    bool irq5_on_e0 = true, send_waits_for_receive = m3::CommBoardOptions().send_waits_for_receive,
         host_sees_other_bank = false;
    double link_cycles_per_byte = m3::CommBoardOptions().link_cycles_per_byte;
};

// Where each input is, active low (hardware-notes.md, Inputs).
bool input_bit(const std::string& name, int& bank, int& bit) {
    static const struct {
        const char* name;
        int bank, bit;
    } kInputs[] = {{"coin", 0, 0},  {"test", 0, 2},  {"service", 0, 3}, {"start", 0, 4},
                   {"beat", 1, 0},  {"charge", 1, 1}, {"shift", 1, 2},  {"jump", 1, 3},
                   {"down", 1, 4},  {"up", 1, 5},     {"right", 1, 6},  {"left", 1, 7}};
    for (const auto& i : kInputs)
        if (name == i.name) {
            bank = i.bank;
            bit = i.bit;
            return true;
        }
    return false;
}

// "B:rest" -> board B and "rest"; anything else -> every board.
std::string board_prefix(const std::string& spec, int& board) {
    const size_t colon = spec.find(':');
    const size_t end = spec.find_first_of("=@");
    if (colon != std::string::npos && colon > 0 && colon < end &&
        spec.find_first_not_of("0123456789") == colon) {
        board = std::atoi(spec.c_str());
        return spec.substr(colon + 1);
    }
    board = kAllBoards;
    return spec;
}

// FILE.ext -> FILE-bN.ext for boards after the first.
std::string board_path(const std::string& path, std::size_t board) {
    if (board == 0) return path;
    const std::string tag = "-b" + std::to_string(board);
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + tag;
    return path.substr(0, dot) + tag + path.substr(dot);
}

// --probe: every triangle that covered the pixel, and what became of it.
void print_probe(const m3::Real3dRenderer& r, int x, int y, uint64_t frame) {
    std::printf("Probe %d,%d at frame %llu: %zu triangles cover it\n", x, y, static_cast<unsigned long long>(frame),
                r.probe().size());
    for (const auto& h : r.probe())
        std::printf("  polygon %06x [%08x %08x %08x %08x %08x %08x %08x] priority %d, %s format %d%s, alpha %.2f, "
                    "texel alpha %.2f, 1/w %.4g, texture %ux%u level %.2f, %s: %s\n",
                    h.polygon, h.header[0], h.header[1], h.header[2], h.header[3], h.header[4], h.header[5],
                    h.header[6], h.priority, h.textured ? "textured" : "flat", h.format, h.contour ? " contour" : "",
                    static_cast<double>(h.alpha), static_cast<double>(h.texel_alpha), static_cast<double>(h.w), h.tw, h.th,
                    static_cast<double>(h.lod), h.pass,
                    h.outcome);
}

// One Model 3 board and what the run keeps of it.
struct Machine {
    Machine(std::size_t i, const m3::RomSet& roms, std::span<const recomp::FunctionEntry> functions)
        : index(i), bus(roms), c(bus, functions), sound(roms),
          screen(m3::TilegenRenderer::kWidth * m3::TilegenRenderer::kHeight) {}

    std::size_t index;
    m3::Bus bus;
    m3::CommBoard* comm = nullptr;
    bool loaded = false; // --load-state: the run resumes where the state was saved
    uint64_t watch_from = 0, watch_to = 0; // --watch-write: the frames whose writes are printed
    // --replay: the next recorded inputs and hash, hashes matched, the first frame not
    size_t replay_input = 0, replay_hash = 0;
    uint64_t replay_matched = 0, replay_parted = 0;
    recomp::Context c;
    recomp::FrameTiming timing;
    m3::TilegenRenderer tilegen;
    m3::Real3dRenderer real3d;
    tools::FrameSound sound;
    std::vector<uint32_t> screen;
    std::vector<int16_t> audio;

    std::string why;
    double seconds = 0;
    int peak = 0;
    double energy = 0, sound_seconds = 0, render_seconds = 0, setup_ms = 0, raster_ms = 0;
    // --match-models: each frame's scene, and its models found in the frame before's
    m3::Real3dSceneBuilder match_builder;
    m3::Real3dScene match_scene[2];
    uint64_t match_models = 0, match_one = 0, match_several = 0, match_unmatched = 0, match_frames = 0;
    uint64_t match_unmatched_ram = 0, match_unmatched_path = 0;
    // --interp: the scene halfway from the frame before (smooth motion), and how it went
    m3::Real3dScene interp_scene;
    uint64_t interp_frames = 0, interp_cuts = 0, interp_models = 0, interp_blended = 0, interp_moved = 0;
    uint64_t interp_nearest = 0, interp_ambiguous = 0, interp_worst_frame = 0, interp_followed = 0;
    std::vector<uint32_t> sky_before; // the frame before's layers below the 3D, and its sky's scroll
    uint32_t sky_scroll_before = 0;
    uint64_t sky_frames = 0, sky_found = 0;
    m3::InterpStats interp_worst_stats;
    double interp_seconds = 0, interp_worst = 0;
    uint64_t textures_seen = 0, texture_frames = 0; // frames in which the game uploaded textures
    // the 2D layers' drawing, and (--wide) their widescreen sides' making, every frame
    double tilegen_seconds = 0, wide_seconds = 0, wide_worst = 0;
    uint64_t tilegen_frames = 0, wide_frames = 0, wide_worst_frame = 0;
    uint64_t frames_rendered = 0, frames_spotlit = 0, frames_scroll_fog = 0, frames_ambient_fog = 0,
             frames_block_culled = 0;
    uint64_t micro_tris = 0, translator_tris = 0, inverted_tris = 0;
    uint16_t net_status[2] = {0, 0}; // --net-watch

    // --renderer gpu / compare: the GPU's picture, and how it compared with
    // the software renderer's (pixels with a channel more than 2 apart).
#ifdef SPIKEOUT_GPU
    std::unique_ptr<gpu::Real3dGpu> gpu;
    SDL_GPUDevice* gpu_device = nullptr;
#endif
    m3::Real3dSceneBuilder builder;
    m3::Real3dScene scene;
    std::vector<uint32_t> gpu_image;
    // --wide: the GPU's whole widescreen picture, and the 2D layers made as wide
    std::vector<uint32_t> gpu_wide, bottom_wide, top_wide;
    m3::WideLayer wide_bottom, wide_top;
#ifdef SPIKEOUT_GPU
    // --screen-time: the window's screen pass, drawn off screen and timed (ms, summed over the screenshots)
    std::unique_ptr<gpu::Presenter> presenter;
    SDL_GPUTexture* screen_target = nullptr;
    double screen_wide_ms = 0, screen_43_ms = 0;
    uint64_t screen_samples = 0;
#endif
    double gpu_seconds = 0, gpu_worst = 0;
    // --gpu-time: a frame drawn alone, its costs (ms, summed): the CPU's parts, the GPU's after
    double profile_pack = 0, profile_upload = 0, profile_record = 0, profile_cpu = 0, profile_gpu = 0, profile_gpu_worst = 0;
    uint64_t profile_frames = 0, profile_passes = 0, profile_draws = 0;
    uint64_t gpu_worst_frame = 0, gpu_slow = 0; // (slow: over a frame, 17.4 ms)
    uint64_t gpu_hash = 0xCBF29CE484222325ull; // of every picture read back, full size (--gpu-scale)
    uint64_t gpu_frames = 0, frames_compared = 0, frames_differing = 0, pixels_differing = 0;
    uint64_t worst_frame = 0, worst_pixels = 0;
    int max_difference = 0;
};

// How far apart two pictures are: pixels with a channel more than 2 apart,
// and the largest difference.
std::pair<uint64_t, int> difference(std::span<const uint32_t> a, std::span<const uint32_t> b) {
    uint64_t pixels = 0;
    int largest = 0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i] == b[i]) continue;
        int most = 0;
        for (int s = 0; s < 32; s += 8)
            most = std::max(most, std::abs(static_cast<int>((a[i] >> s) & 0xFF) - static_cast<int>((b[i] >> s) & 0xFF)));
        if (most > 2) ++pixels;
        largest = std::max(largest, most);
    }
    return {pixels, largest};
}

bool for_board(int board, std::size_t index) { return board == kAllBoards || static_cast<std::size_t>(board) == index; }

// --replay: the end of `frame`: the recorded inputs for the next, main RAM
// checked against the recorded hash, and the run ended after the last.
void replay_frame(Machine& m, const tools::Recording& rec, uint64_t frame) {
    while (m.replay_input < rec.inputs.size() && rec.inputs[m.replay_input].first < frame) ++m.replay_input;
    if (m.replay_input < rec.inputs.size() && rec.inputs[m.replay_input].first == frame) {
        const auto& in = rec.inputs[m.replay_input].second;
        m.bus.inputs.bank0 = in.bank0;
        m.bus.inputs.bank1 = in.bank1;
        m.bus.inputs.controls = in.controls;
        m.bus.inputs.controls2 = in.controls2;
    }
    while (m.replay_hash < rec.hashes.size() && rec.hashes[m.replay_hash].first < frame) ++m.replay_hash;
    if (m.replay_hash < rec.hashes.size() && rec.hashes[m.replay_hash].first == frame) {
        if (tools::Recording::ram_hash(m.bus.ram()) == rec.hashes[m.replay_hash].second) {
            ++m.replay_matched;
        } else if (!m.replay_parted) {
            m.replay_parted = frame;
            std::printf("replay: main RAM differs from the recording's at frame %llu\n", static_cast<unsigned long long>(frame));
        }
    }
    if (!rec.inputs.empty() && frame >= rec.inputs.back().first) m.c.limit_blocks(0); // the recording's end
}

// Save states: the sound board's part (none with --no-sound).
std::function<void(m3::State&)> sound_state(Machine& m, const Options& o) {
    if (o.no_sound) return {};
    return [&m](m3::State& s) { m.sound.state(s); };
}

void set_up(Machine& m, const Options& o, const m3::GameSpec& game, m3::LinkRing* ring, tools::Turns& turns) {
    m3::Bus& bus = m.bus;
    bus.midi_burst = o.midi_burst;
    bus.dma_timed = !o.dma_instant;
    bus.set_security_key(game.security_key);
    if (o.replay) { // as the window started the recorded run: its clock, EEPROM and backup RAM, RAM as it was
        bus.rtc().set_time(o.replay->clock, 0);
        std::copy(o.replay->eeprom.begin(), o.replay->eeprom.end(), bus.eeprom().words().begin());
        if (o.replay->backup.size() == m3::Bus::kBackupRamSize)
            std::copy(o.replay->backup.begin(), o.replay->backup.end(), bus.backup_ram().begin());
    } else {
        tools::set_rtc_to_host(bus);
        for (uint8_t& b : bus.ram()) b = 0xA5;
    }
    if (o.eeprom) {
        std::ifstream in(o.eeprom, std::ios::binary);
        uint8_t bytes[128] = {};
        if (in.read(reinterpret_cast<char*>(bytes), sizeof bytes))
            for (std::size_t i = 0; i < 64; ++i) bus.eeprom().words()[i] = static_cast<uint16_t>(bytes[2 * i] << 8 | bytes[2 * i + 1]);
        else
            std::fprintf(stderr, "gamerun: cannot read %s (128 bytes)\n", o.eeprom);
    }
    if (o.real_netboard) {
        m3::CommBoardOptions options;
        options.irq5_on_e0 = o.irq5_on_e0;
        options.link_cycles_per_byte = o.link_cycles_per_byte;
        options.send_waits_for_receive = o.send_waits_for_receive;
        options.host_sees_other_bank = o.host_sees_other_bank;
        auto board = std::make_unique<m3::CommBoard>(ring ? ring->port(m.index) : nullptr, options);
        m.comm = board.get();
        m.comm->trace_limit = o.net_trace;
        m.comm->force_master = o.force_master;
        bus.use_netboard(std::move(board));
    } else if (ring) {
        bus.use_netboard(std::make_unique<m3::SimNetBoard>(ring->port(m.index)));
    }
    m.c.set_scheduler(&m.timing);
    if (o.load_state) { // everything above that the game sees, as it was saved
        try {
            m3::State s(tools::read_file(o.load_state));
            tools::board_state(s, m.c, m.timing, sound_state(m, o));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "gamerun: %s: %s\n", o.load_state, e.what());
            std::exit(1);
        }
        m.loaded = true;
        std::printf("Loaded %s: frame %llu, block %08x\n", o.load_state, static_cast<unsigned long long>(m.timing.frames()),
                    m.c.cpu.pc);
    }
    m.c.limit_blocks(static_cast<uint64_t>(o.blocks));
    if (o.watch_hi > o.watch_lo) { // --watch-write: each write there, where the code was, and its registers
        m.watch_from = o.watch_frame;
        m.watch_to = o.watch_frame + o.watch_frames;
        bus.watch_writes(o.watch_lo, o.watch_hi, [](void* user, uint32_t addr, uint32_t value, uint32_t size) {
            Machine& w = *static_cast<Machine*>(user);
            const uint64_t frame = w.timing.frames();
            if (frame < w.watch_from || frame >= w.watch_to) return;
            const auto& r = w.c.cpu.gpr;
            std::printf("frame %llu: write%u %08x = %08x at block %08x, LR %08x; r3-r12 %08x %08x %08x %08x %08x %08x %08x %08x "
                        "%08x %08x, r24-r31 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        static_cast<unsigned long long>(frame), 8 * size, addr, value, w.c.cpu.pc, w.c.cpu.lr, r[3], r[4], r[5],
                        r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[24], r[25], r[26], r[27], r[28], r[29], r[30], r[31]);
        }, &m);
    }
    m.real3d.set_probe(o.probe_x, o.probe_y);
    m.real3d.set_quads(o.quads);
    m.sound.board.set_volumes(o.sound_volume, o.music_volume);

    // The 2D layers, a line at a time; each finished frame is kept for --screenshot.
    // With several boards, the turn passes on every o.slice lines, the
    // network board caught up first so what it sent is on the link.
    m.timing.on_display_line = [&m, &turns, slice = o.slice, no_2d = o.no_2d](uint32_t line) {
        if (!no_2d) {
            const auto start = std::chrono::steady_clock::now();
            if (line == 0) {
                m.tilegen.begin_frame();
                ++m.tilegen_frames;
            }
            m.tilegen.draw_line(m.bus, static_cast<int>(line));
            m.tilegen_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        if (turns.boards() > 1 && line % slice == slice - 1) {
            m.bus.netboard().sync(m.c.cycles());
            turns.pass(m.index);
        }
    };
    m.timing.on_frame_end = [&m, &o] {
        const uint64_t frame = m.timing.frames();
        if (const uint64_t up = m.bus.real3d().textures_uploaded(); up != m.textures_seen) { // a frame that changed textures
            m.textures_seen = up;
            ++m.texture_frames;
        }
        if (o.match_models) { // each frame's models found in the frame before's (smooth motion)
            std::swap(m.match_scene[0], m.match_scene[1]);
            m.match_builder.build(m.bus.real3d(), m.match_scene[1]);
            const auto& before = m.match_scene[0].models;
            const auto& now = m.match_scene[1].models;
            std::unordered_map<uint64_t, int> keys; // (viewport, address, path): how many models in the frame before
            auto key = [](const m3::Real3dScene::Model& model) {
                return static_cast<uint64_t>(model.viewport) << 48 ^ static_cast<uint64_t>(model.address) << 24 ^ model.path;
            };
            for (const auto& model : before) ++keys[key(model)];
            for (const auto& model : now) {
                const auto it = keys.find(key(model));
                if (it == keys.end()) {
                    ++m.match_unmatched;
                    if (model.address < 0x100000) ++m.match_unmatched_ram; // a model the game writes (polygon RAM)
                    bool moved = false; // the same model and viewport, under other nodes (a level of detail, a new path)
                    for (const auto& b : before)
                        if (b.address == model.address && b.viewport == model.viewport) {
                            moved = true;
                            break;
                        }
                    if (moved) ++m.match_unmatched_path;
                }
                else if (it->second == 1) ++m.match_one;
                else ++m.match_several;
            }
            m.match_models += now.size();
            if (!before.empty() && !now.empty()) ++m.match_frames;
            if (o.interp && !before.empty() && !now.empty()) {
                m3::InterpStats stats;
                const auto start = std::chrono::steady_clock::now();
                m3::interpolate(m.match_scene[0], m.match_scene[1], 0.5f, m.interp_scene, &stats);
                const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                m.interp_seconds += took;
                if (took > m.interp_worst) {
                    m.interp_worst = took;
                    m.interp_worst_frame = frame;
                    m.interp_worst_stats = stats;
                }
                ++m.interp_frames;
                if (stats.cut) ++m.interp_cuts;
                m.interp_models += stats.models;
                m.interp_blended += stats.blended;
                m.interp_moved += stats.moved;
                m.interp_followed += stats.followed;
                m.interp_nearest += stats.nearest;
                m.interp_ambiguous += stats.ambiguous;
                // The sky's move (layer 3 below the 3D, its register's scroll), as the window finds it.
                const uint32_t sky = m.bus.tile_reg(0x6C);
                int sky_x = 0, sky_y = 0;
                bool sky_moved = false;
                if ((sky & 0x80000000u) && !(sky & 0x8000) && !(m.bus.tile_reg(0x20) & 0x800) && !m.sky_before.empty()) {
                    sky_moved = m3::TilegenRenderer::find_move(m.sky_before, m.tilegen.bottom(), m.sky_scroll_before, sky,
                                                               sky_x, sky_y);
                    ++m.sky_frames;
                    if (sky_moved) ++m.sky_found;
                }
                m.sky_before.assign(m.tilegen.bottom().begin(), m.tilegen.bottom().end());
                m.sky_scroll_before = sky;
                if (o.interp_trace && frame >= o.interp_trace_from && frame < o.interp_trace_to)
                    std::printf("frame %llu: %zu models, %zu blended, %zu not found (%zu followed), %zu moved, %zu spun; %zu nearest (%zu "
                                "ambiguous, %zu compared)%s; %.3f ms; sky %s %d, %d\n",
                                static_cast<unsigned long long>(frame), stats.models, stats.blended, stats.unmatched, stats.followed,
                                stats.moved, stats.spun, stats.nearest, stats.ambiguous, stats.compared,
                                stats.cut ? ", a cut" : "", 1000.0 * took, sky_moved ? "moved" : "(none)", sky_x, sky_y);
            }
        }
        // The 3D is drawn only when it will be seen.
        using R = Options::Renderer;
        const bool drawn = o.screenshot || (o.probe_frame && frame == o.probe_frame);
        if (drawn && o.renderer != R::Gpu) { // in software
            const auto render_start = std::chrono::steady_clock::now();
            m.real3d.render(m.bus.real3d());
            const auto& s = m.real3d.stats();
            m.setup_ms += s.setup_ms;
            m.raster_ms += s.raster_ms;
            if (s.spotlights) ++m.frames_spotlit;
            if (s.block_culled) ++m.frames_block_culled;
            m.frames_scroll_fog += s.scroll_fog;
            m.frames_ambient_fog += s.ambient_fog;
            m.micro_tris += s.micro_polys;
            m.translator_tris += s.translator_polys;
            m.inverted_tris += s.inverted_polys;
            m.render_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - render_start).count();
            ++m.frames_rendered;
        }
#ifdef SPIKEOUT_GPU
        if (drawn && m.gpu) { // on the GPU, from the same scene
            const auto start = std::chrono::steady_clock::now();
            const m3::Real3d& mem = m.bus.real3d();
            if (o.renderer == R::Gpu) m.builder.build(mem, m.scene);
            if (o.gpu_time_scale) { // drawn and waited for, not read back; several times, to keep the GPU busy (its clock up)
                if (m.builder.extend() > 0) m.builder.build(mem, m.scene); // (--wide: its scene opened out)
                const m3::Real3dScene& timed = m.builder.extend() > 0 ? m.scene : m.real3d.scene();
                for (int i = 0; i < kGpuTimeRepeats; ++i) m.gpu->render(mem, timed);
                SDL_WaitForGPUIdle(m.gpu_device);
                // And once alone: its CPU parts, and the GPU's time after (from idle).
                const auto submitting = std::chrono::steady_clock::now();
                m.gpu->render(mem, timed);
                const auto submitted = std::chrono::steady_clock::now();
                SDL_WaitForGPUIdle(m.gpu_device);
                const double gpu_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitted).count();
                const auto& cost = m.gpu->cost();
                m.profile_pack += cost.pack_ms;
                m.profile_upload += cost.upload_ms;
                m.profile_record += cost.record_ms;
                m.profile_cpu += std::chrono::duration<double, std::milli>(submitted - submitting).count();
                m.profile_gpu += gpu_ms;
                m.profile_gpu_worst = std::max(m.profile_gpu_worst, gpu_ms);
                if (o.gpu_profile_over > 0 && gpu_ms > o.gpu_profile_over)
                    std::printf("frame %llu: the GPU %.2f ms; %zu polygons, %zu viewports, %d passes, %d draws\n",
                                static_cast<unsigned long long>(frame), gpu_ms, timed.polygons.size(), timed.viewports.size(),
                                cost.passes, cost.draws);
                m.profile_passes += cost.passes;
                m.profile_draws += cost.draws;
                ++m.profile_frames;
            } else {
                m.gpu->render(mem, o.renderer == R::Gpu ? m.scene : m.real3d.scene(), &m.gpu_image);
                for (uint32_t p : m.gpu_image) m.gpu_hash = (m.gpu_hash ^ p) * 0x100000001B3ull; // FNV-1a, a word at a time
                if (o.gpu_full_frame && frame == o.gpu_full_frame && o.screenshot) { // --gpu-full: the picture whole
                    const std::string path = o.screenshot;
                    const size_t dot = path.find_last_of('.');
                    std::vector<uint32_t> rgb(m.gpu_image.size());
                    for (std::size_t i = 0; i < rgb.size(); ++i) rgb[i] = 0xFF000000u | m.gpu_image[i];
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-full" + path.substr(dot), rgb,
                                     m.gpu->width(), m.gpu->height());
                }
                if (o.gpu_hash_frames) { // each picture's own hash, to find where two runs part
                    uint64_t h = 0xCBF29CE484222325ull;
                    for (uint32_t p : m.gpu_image) h = (h ^ p) * 0x100000001B3ull;
                    std::printf("frame %llu: GPU picture %016llx\n", static_cast<unsigned long long>(frame),
                                static_cast<unsigned long long>(h));
                }
                if (const int e = m.builder.extend(); e > 0) { // widescreen: kept whole, its middle for the rest
                    const int w = m3::TilegenRenderer::kWidth, h = m3::TilegenRenderer::kHeight;
                    m.gpu_wide = m.gpu_image;
                    m.gpu_image.resize(static_cast<size_t>(w) * h);
                    for (int y = 0; y < h; ++y)
                        std::copy_n(m.gpu_wide.begin() + static_cast<std::ptrdiff_t>(y * (w + 2 * e) + e), w,
                                    m.gpu_image.begin() + static_cast<std::ptrdiff_t>(y * w));
                }
                if (o.gpu_scale > 1) { // every scale-th pixel, for the screenshots
                    const int s = o.gpu_scale, w = m3::TilegenRenderer::kWidth, h = m3::TilegenRenderer::kHeight;
                    std::vector<uint32_t> small(static_cast<size_t>(w) * h);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            small[static_cast<size_t>(y) * w + x] = m.gpu_image[static_cast<size_t>(y * s) * (w * s) + x * s];
                    m.gpu_image.swap(small);
                }
            }
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() /
                                (o.gpu_time_scale ? kGpuTimeRepeats : 1);
            m.gpu_seconds += took;
            if (took > m.gpu_worst) {
                m.gpu_worst = took;
                m.gpu_worst_frame = frame;
            }
            if (took > 1.0 / recomp::FrameTiming::kRefreshHz) ++m.gpu_slow;
            ++m.gpu_frames;
            if (o.renderer == R::Compare) {
                const auto [pixels, largest] = difference(m.real3d.image(), m.gpu_image);
                ++m.frames_compared;
                if (pixels) ++m.frames_differing;
                m.pixels_differing += pixels;
                m.max_difference = std::max(m.max_difference, largest);
                if (pixels > m.worst_pixels) {
                    m.worst_pixels = pixels;
                    m.worst_frame = frame;
                }
            }
        }
#endif
        if (o.probe_frame && frame == o.probe_frame && m.index == 0) print_probe(m.real3d, o.probe_x, o.probe_y, frame);
        if (!o.no_2d) m.tilegen.composite(m.screen, o.renderer == R::Gpu ? std::span<const uint32_t>(m.gpu_image) : m.real3d.image());
        if (const int e = m.tilegen.extend(); e > 0 && !o.no_2d) { // --wide: the sides made every frame, as the window does
            const auto start = std::chrono::steady_clock::now();
            m.wide_bottom.build(m.tilegen.bottom(), m.tilegen.bottom_side(), e, m.bottom_wide, false, m.tilegen.bottom_own(),
                                m.tilegen.bottom_real());
            m.wide_top.build(m.tilegen.top(), m.tilegen.top_side(), e, m.top_wide, true, m.tilegen.top_own(),
                             m.tilegen.top_real());
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            m.wide_seconds += took;
            if (took > m.wide_worst) {
                m.wide_worst = took;
                m.wide_worst_frame = frame;
            }
            ++m.wide_frames;
        }
        const auto sound_start = std::chrono::steady_clock::now();
        if (o.no_sound) m.bus.midi_out().clear();
        const std::span<const int16_t> pcm = o.no_sound ? std::span<const int16_t>() :
            m.sound.frame(m.bus.midi_out(), m.timing.frame_start(frame - 1), m.timing.frame_cycles());
        m.sound_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - sound_start).count();
        if (o.wav) m.audio.insert(m.audio.end(), pcm.begin(), pcm.end());
        for (int16_t s : pcm) {
            m.peak = std::max(m.peak, std::abs(static_cast<int>(s)));
            m.energy += static_cast<double>(s) * s;
        }
        for (const Poke& k : o.pokes)
            if (for_board(k.board, m.index) && frame >= k.frame && frame < k.frame + k.frames)
                m.bus.write8(k.addr, k.value);
        // --press: inputs held for the coming frame (active low).
        uint8_t in[2] = {0xFF, 0xFF};
        for (const Press& p : o.presses)
            if (for_board(p.board, m.index) && frame >= p.frame && frame < p.frame + p.frames)
                in[p.bank] = static_cast<uint8_t>(in[p.bank] & ~(1u << p.bit));
        m.bus.inputs.bank0 = in[0];
        m.bus.inputs.controls = in[1];
        if (o.replay && m.index == 0) replay_frame(m, *o.replay, frame);
        if (!o.save_state.empty() && frame == o.save_frame && m.index == 0) // --save-state: at the next block it can resume at
            m.c.request_snapshot([&m, &o](uint32_t pc) {
                m3::State s;
                tools::board_state(s, m.c, m.timing, sound_state(m, o));
                if (tools::write_file(o.save_state, s.data()))
                    std::printf("Saved %s: frame %llu, block %08x, %zu bytes\n", o.save_state.c_str(),
                                static_cast<unsigned long long>(m.timing.frames()), pc, s.data().size());
                else
                    std::fprintf(stderr, "gamerun: cannot write %s\n", o.save_state.c_str());
            });
        // --net-watch: the network board's status registers, when they change.
        if (o.net_watch && m.comm) {
            const uint16_t s0 = m.comm->io(0x88), s1 = m.comm->io(0x8A);
            if (s0 != m.net_status[0] || s1 != m.net_status[1])
                std::printf("board %zu frame %llu: network status0 %04x status1 %04x, 68000 PC %06x\n", m.index,
                            static_cast<unsigned long long>(frame), s0, s1, m.comm->pc());
            m.net_status[0] = s0;
            m.net_status[1] = s1;
        }
        if (o.tilegen_trace && frame % o.tilegen_trace == 0) {
            std::printf("%sframe %6llu tilegen", m.index ? ("board " + std::to_string(m.index) + " ").c_str() : "",
                        static_cast<unsigned long long>(frame));
            for (uint32_t reg : {0x20u, 0x40u, 0x44u, 0x60u, 0x64u, 0x68u, 0x6Cu}) std::printf(" %02x=%08x", reg, m.bus.tile_reg(reg));
            std::printf("\n");
        }
        if (o.screenshot && o.shot_every && frame % o.shot_every == 0) {
            const std::string path = board_path(o.screenshot, m.index);
            const size_t dot = path.find_last_of('.');
#ifdef SPIKEOUT_GPU
            if (o.interp && m.gpu && m.builder.extend() == 0 && !m.match_scene[0].models.empty()) {
                // --interp: the frame before and the picture halfway from it (the 2D layers this frame's)
                auto write = [&](const m3::Real3dScene& scene, const char* tag) {
                    std::vector<uint32_t> three_d, screen(m.screen.size());
                    m.gpu->render(m.bus.real3d(), scene, &three_d);
                    m.tilegen.composite(screen, three_d);
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + tag + path.substr(dot), screen,
                                     m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                };
                write(m.match_scene[0], "-before");
                write(m.interp_scene, "-mid");
            }
#endif
            if (const int e = m.builder.extend(); e > 0 && !m.gpu_wide.empty() && m.tilegen.extend() == e) {
                // --wide: the widescreen picture as the window shows it
                m.wide_bottom.build(m.tilegen.bottom(), m.tilegen.bottom_side(), e, m.bottom_wide, false,
                                    m.tilegen.bottom_own(), m.tilegen.bottom_real());
                m.wide_top.build(m.tilegen.top(), m.tilegen.top_side(), e, m.top_wide, true, m.tilegen.top_own(),
                                 m.tilegen.top_real());
                std::vector<uint32_t> wide(m.bottom_wide.size());
                m3::WideLayer::composite(wide, m.bottom_wide, m.top_wide, m.gpu_wide, e);
#ifdef SPIKEOUT_GPU
                if (m.presenter) { // --screen-time: the screen pass (layers uploaded, shaded), widescreen and 4:3
                    constexpr int kRepeats = 50;
                    auto time_draws = [&](std::span<const uint32_t> bottom, std::span<const uint32_t> top, int extend) {
                        const int w = m3::TilegenRenderer::kWidth, h = m3::TilegenRenderer::kHeight;
                        m.presenter->present_layers(bottom, top, m.gpu->display_image(), w, h, extend); // (its textures made)
                        SDL_WaitForGPUIdle(m.gpu_device);
                        const auto start = std::chrono::steady_clock::now();
                        for (int i = 0; i < kRepeats; ++i)
                            m.presenter->present_layers(bottom, top, m.gpu->display_image(), w, h, extend);
                        SDL_WaitForGPUIdle(m.gpu_device);
                        return 1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / kRepeats;
                    };
                    m.screen_wide_ms += time_draws(m.bottom_wide, m.top_wide, e);
                    m.screen_43_ms += time_draws(m.tilegen.bottom(), m.tilegen.top(), 0);
                    ++m.screen_samples;
                }
#endif
                tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-wide" + path.substr(dot), wide,
                                 m3::TilegenRenderer::kWidth + 2 * e, m3::TilegenRenderer::kHeight);
                if (o.dump_layers) { // and the layers below the 3D alone, and the 3D's alpha (grey)
                    std::vector<uint32_t> below(m.bottom_wide.size());
                    for (std::size_t i = 0; i < below.size(); ++i) below[i] = 0xFF000000u | m.bottom_wide[i];
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-wide2d" + path.substr(dot), below,
                                     m3::TilegenRenderer::kWidth + 2 * e, m3::TilegenRenderer::kHeight);
                    std::vector<uint32_t> alpha(m.gpu_wide.size());
                    for (std::size_t i = 0; i < alpha.size(); ++i) {
                        const uint32_t a = m.gpu_wide[i] >> 24;
                        alpha[i] = 0xFF000000u | a << 16 | a << 8 | a;
                    }
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-wide3da" + path.substr(dot), alpha,
                                     m3::TilegenRenderer::kWidth + 2 * e, m3::TilegenRenderer::kHeight);
                }
            }
            if (o.dump_layers) // each layer's whole name table, for looking at
                for (int n = 0; n < 4; ++n) {
                    std::vector<uint32_t> map(512 * 512);
                    m3::TilegenRenderer::draw_map(m.bus.tile_ram(), m.bus.tile_regs(), n, map);
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-L" + std::to_string(n) +
                                         path.substr(dot), map, 512, 512);
                    // and its name table's entries, a line per tile row, then the line scroll values
                    const std::string text = path.substr(0, dot) + "-" + std::to_string(frame) + "-L" + std::to_string(n) + ".txt";
                    if (std::FILE* f = std::fopen(text.c_str(), "w")) {
                        const auto ram = m.bus.tile_ram();
                        auto word = [&](uint32_t at) {
                            return uint32_t{ram[at]} | uint32_t{ram[at + 1]} << 8 | uint32_t{ram[at + 2]} << 16 |
                                   uint32_t{ram[at + 3]} << 24;
                        };
                        std::fprintf(f, "reg %08x\n", m.bus.tile_regs()[0x60 / 4 + static_cast<std::size_t>(n)]);
                        for (uint32_t row = 0; row < 64; ++row) {
                            for (uint32_t col = 0; col < 32; ++col) {
                                const uint32_t w = word(0xF8000 + static_cast<uint32_t>(n) * 0x2000 + (row * 32 + col) * 4);
                                std::fprintf(f, "%04x %04x ", w >> 16, w & 0xFFFF);
                            }
                            std::fprintf(f, "\n");
                        }
                        for (uint32_t line = 0; line < 384; line += 2) {
                            const uint32_t w = word(0xF6000 + static_cast<uint32_t>(n) * 0x400 + line * 2);
                            std::fprintf(f, "%s%03x %03x", line % 32 ? " " : "\nscroll ", w >> 16, w & 0xFFFF);
                        }
                        std::fprintf(f, "\n");
                        std::fclose(f);
                    }
                }
            tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + path.substr(dot), m.screen,
                             m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
            if (o.renderer == R::Compare && !m.gpu_image.empty()) { // and the GPU's, beside it
                std::vector<uint32_t> gpu_screen(m.screen.size());
                m.tilegen.composite(gpu_screen, m.gpu_image);
                tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-gpu" + path.substr(dot), gpu_screen,
                                 m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                // Where the 3D differs: red, over the software picture dimmed.
                std::vector<uint32_t> diff(m.screen.size());
                const auto soft = m.real3d.image();
                for (std::size_t i = 0; i < diff.size() && i < soft.size() && i < m.gpu_image.size(); ++i) {
                    const auto [pixels, largest] = difference(soft.subspan(i, 1), std::span<const uint32_t>(m.gpu_image).subspan(i, 1));
                    (void)largest;
                    const uint32_t p = m.screen[i];
                    diff[i] = pixels ? 0xFFFF0000u : 0xFF000000u | ((p >> 2) & 0x003F3F3Fu);
                }
                tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-diff" + path.substr(dot), diff,
                                 m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                // The two 3D pictures alone, colour and alpha (grey).
                auto write_3d = [&](std::span<const uint32_t> img, const std::string& tag) {
                    std::vector<uint32_t> rgb(img.size()), alpha(img.size());
                    for (std::size_t i = 0; i < img.size(); ++i) {
                        rgb[i] = 0xFF000000u | img[i];
                        const uint32_t a = img[i] >> 24;
                        alpha[i] = 0xFF000000u | a << 16 | a << 8 | a;
                    }
                    const std::string at = path.substr(0, dot) + "-" + std::to_string(frame);
                    tools::write_png(at + "-3d" + tag + path.substr(dot), rgb, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                    tools::write_png(at + "-3da" + tag + path.substr(dot), alpha, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                };
                write_3d(soft, "");
                write_3d(m.gpu_image, "-gpu");
            }
        }
    };
}

void run(Machine& m, tools::Turns& turns) {
    turns.wait(m.index);
    const auto start = std::chrono::steady_clock::now();
    try {
        if (m.loaded) m.c.resume_block(m.c.cpu.pc);
        m.c.call(m.c.cpu.pc);
        m.why = "the reset code returned";
    } catch (const recomp::OutOfBudget&) {
        m.why = "block limit reached";
    } catch (const recomp::MissingCode& e) {
        m.why = e.what();
    } catch (const recomp::GuestError& e) {
        m.why = e.what();
    } catch (const std::exception& e) {
        m.why = std::string("error: ") + e.what();
    }
    m.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    turns.finish(m.index);
}

void report(Machine& m, const Options& o) {
    recomp::Context& c = m.c;
    m3::Bus& bus = m.bus;
    const long long ran = static_cast<long long>(c.blocks);

    std::printf("Stopped: %s\n", m.why.c_str());
    std::printf("Ran %lld blocks in %.2f s (%.0f million blocks/s), %.2f 603e cycles a block\n", ran, m.seconds,
                static_cast<double>(ran) / m.seconds / 1e6,
                ran > 0 ? static_cast<double>(c.cycles()) / static_cast<double>(ran) : 0.0);
    std::printf("PC %08x  LR %08x  CTR %08x  MSR %08x  CR %08x\n", c.cpu.pc, c.cpu.lr, c.cpu.ctr, c.cpu.msr, c.cpu.cr);
    // Address translation as the game set it up (the runtime treats every
    // address as physical; these show whether that holds).
    {
        using A = m3::Jtag::Asic;
        const m3::Jtag& j = bus.jtag();
        std::printf("Real3D mode words (JTAG): Mercury %08x  Venus %08x  Earth %08x  Mars %08x  Jupiter %08x\n",
                    j.modeword(A::Mercury), j.modeword(A::Venus), j.modeword(A::Earth), j.modeword(A::Mars),
                    j.modeword(A::Jupiter));
    }
    std::printf("MMU: SDR1 %08x  HID0 %08x\n", c.cpu.sdr1, c.cpu.hid0);
    for (int i = 0; i < 4; ++i)
        std::printf("  IBAT%d %08x %08x   DBAT%d %08x %08x\n", i, c.cpu.ibat[2 * i], c.cpu.ibat[2 * i + 1], i,
                    c.cpu.dbat[2 * i], c.cpu.dbat[2 * i + 1]);
    std::printf("  SR");
    for (uint32_t sr : c.cpu.sr) std::printf(" %08x", sr);
    std::printf("\n");
    // The page tables in RAM: the one SDR1 gives (HTABORG, HTABMASK), which
    // Spikeout's instruction TLB miss handler searches, and the one 128 KB
    // above it its data handlers search. Every valid PTE's page, from its
    // group and API, against the page it maps to.
    for (const uint32_t org : {c.cpu.sdr1 & 0xFFFF0000u, (c.cpu.sdr1 & 0xFFFF0000u) + 0x20000}) {
        const uint32_t groups = ((c.cpu.sdr1 & 0x1FF) + 1) * 1024; // 64-byte groups of 8 PTEs
        uint32_t valid = 0, identity = 0, shown = 0;
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t g = 0; g < groups && org + (g + 1) * 64 <= bus.ram().size(); ++g)
            for (uint32_t k = 0; k < 8; ++k) {
                const uint32_t at = org + g * 64 + k * 8;
                const uint32_t w0 = bus.read32(at), w1 = bus.read32(at + 4);
                if (!(w0 >> 31)) continue;
                ++valid;
                const uint32_t vsid = (w0 >> 7) & 0xFFFFFF, h = (w0 >> 6) & 1, api = w0 & 0x3F;
                const uint32_t hash = h ? ~g : g;
                const uint32_t page = (api << 10) | ((hash ^ vsid) & 0x3FF); // 16-bit page index
                int seg = -1;
                for (int s = 0; s < 16; ++s)
                    if ((c.cpu.sr[s] & 0xFFFFFF) == vsid) seg = s;
                const uint32_t pa = w1 & 0xFFFFF000u;
                const uint32_t ea = seg < 0 ? 0 : static_cast<uint32_t>(seg) << 28 | page << 12;
                if (seg >= 0 && ea == pa) {
                    ++identity;
                    lo = std::min(lo, ea);
                    hi = std::max(hi, ea + 0xFFF);
                } else if (shown++ < 8) {
                    std::printf("  PTE at %06x: VSID %06x (segment %d) page %04x -> %08x, WIMG %x PP %u\n", at, vsid, seg,
                                page, pa, (w1 >> 3) & 0xF, w1 & 3);
                }
            }
        std::printf("Page table at %08x: %u valid PTEs, %u map their page to itself (%08x-%08x)\n", org, valid, identity,
                    lo == UINT32_MAX ? 0 : lo, hi);
    }

    std::printf("Frames %llu (%.2f s of game time); interrupts taken: %llu external, %llu decrementer; "
                "IRQ enable %02x, pending %02x\n",
                static_cast<unsigned long long>(m.timing.frames()),
                static_cast<double>(m.timing.frames()) / recomp::FrameTiming::kRefreshHz,
                static_cast<unsigned long long>(c.external_interrupts),
                static_cast<unsigned long long>(c.decrementer_interrupts), bus.irq().enable(), bus.irq().state());

    std::printf("Network board: %s\n", bus.netboard().describe().c_str());
    if (o.replay && m.index == 0)
        std::printf("Replay: %llu of %zu hashes matched the recording's%s\n", static_cast<unsigned long long>(m.replay_matched),
                    o.replay->hashes.size(),
                    m.replay_parted ? (", parted at frame " + std::to_string(m.replay_parted)).c_str() : " (the same run)");
    const double played = static_cast<double>(m.sound.board.samples());
    std::printf("Sound board: %s; %d slots playing, %llu MIDI bytes dropped; output peak %d, RMS %.0f; %.2f s to run it\n",
                m.sound.board.describe().c_str(), m.sound.board.active_slots(),
                static_cast<unsigned long long>(m.sound.board.midi_dropped()), m.peak,
                played > 0 ? std::sqrt(m.energy / (2 * played)) : 0.0, m.sound_seconds);
    if (const m3::Dsb2* music = m.sound.board.music())
        std::printf("Music board: %s; its output peak %d, RMS %.0f\n", music->describe().c_str(), music->music_peak,
                    played > 0 ? std::sqrt(music->music_energy / (2 * played)) : 0.0);
    if (o.wav) {
        const std::string path = board_path(o.wav, m.index);
        if (tools::write_wav(path.c_str(), m.audio, m3::Scsp::kSampleRate)) std::printf("Wrote %s\n", path.c_str());
    }
    if (c.first_nan_pc) // SPIKEOUT_FP_TRAP builds
        std::printf("First FP NaN: at %08x (%016llx)\n", c.first_nan_pc, static_cast<unsigned long long>(c.first_nan_bits));
    if (m.match_models) {
        const double n = static_cast<double>(m.match_models);
        std::printf("Models between frames: %llu over %llu frames; found in the frame before: %.2f%% once, %.2f%% several "
                    "times (the same key), %.2f%% not\n",
                    static_cast<unsigned long long>(m.match_models), static_cast<unsigned long long>(m.match_frames),
                    100.0 * static_cast<double>(m.match_one) / n, 100.0 * static_cast<double>(m.match_several) / n,
                    100.0 * static_cast<double>(m.match_unmatched) / n);
        std::printf("  of those not found: %.2f%% in polygon RAM (written by the game), %.2f%% the same model under "
                    "other nodes\n",
                    100.0 * static_cast<double>(m.match_unmatched_ram) / static_cast<double>(std::max<uint64_t>(m.match_unmatched, 1)),
                    100.0 * static_cast<double>(m.match_unmatched_path) / static_cast<double>(std::max<uint64_t>(m.match_unmatched, 1)));
    }
    if (m.interp_frames) {
        const double n = static_cast<double>(std::max<uint64_t>(m.interp_models, 1));
        std::printf("Smooth motion: %llu frames, %llu cuts (drawn as the frame); models blended %.2f%%, moved with a relative "
                    "%.2f%%, moved too far %.2f%%; %.3f ms a picture on average, %.3f at most\n",
                    static_cast<unsigned long long>(m.interp_frames), static_cast<unsigned long long>(m.interp_cuts),
                    100.0 * static_cast<double>(m.interp_blended) / n, 100.0 * static_cast<double>(m.interp_followed) / n,
                    100.0 * static_cast<double>(m.interp_moved) / n, 1000.0 * m.interp_seconds / static_cast<double>(m.interp_frames),
                    1000.0 * m.interp_worst);
        const m3::InterpStats& w = m.interp_worst_stats;
        std::printf("  found as the nearest of the same model %.2f%% (ambiguous %.2f%%); the slowest frame %llu: %zu models, "
                    "%zu nearest, %zu compared\n",
                    100.0 * static_cast<double>(m.interp_nearest) / n, 100.0 * static_cast<double>(m.interp_ambiguous) / n,
                    static_cast<unsigned long long>(m.interp_worst_frame), w.models, w.nearest, w.compared);
        std::printf("  the sky's move found in %llu of %llu frames with a sky\n", static_cast<unsigned long long>(m.sky_found),
                    static_cast<unsigned long long>(m.sky_frames));
    }
    std::printf("Textures: uploaded in %llu of %llu frames\n", static_cast<unsigned long long>(m.texture_frames),
                static_cast<unsigned long long>(m.timing.frames()));
    if (m.tilegen_frames)
        std::printf("2D layers: %.3f ms a frame drawing them on average\n",
                    1000.0 * m.tilegen_seconds / static_cast<double>(m.tilegen_frames));
    if (m.wide_frames)
        std::printf("Widescreen sides: %.3f ms a frame making them on average, %.3f at most (frame %llu)\n",
                    1000.0 * m.wide_seconds / static_cast<double>(m.wide_frames), 1000.0 * m.wide_worst,
                    static_cast<unsigned long long>(m.wide_worst_frame));
#ifdef SPIKEOUT_GPU
    if (m.profile_frames) {
        const double n = static_cast<double>(m.profile_frames);
        std::printf("A frame drawn alone: the CPU %.2f ms (packing %.2f, uploading %.2f, recording %.2f), then the GPU %.2f "
                    "(%.2f at most); %.1f passes, %.0f draws\n",
                    m.profile_cpu / n, m.profile_pack / n, m.profile_upload / n, m.profile_record / n, m.profile_gpu / n,
                    m.profile_gpu_worst, static_cast<double>(m.profile_passes) / n, static_cast<double>(m.profile_draws) / n);
    }
    if (m.screen_samples)
        std::printf("Screen pass at %dx%d: %.3f ms widescreen, %.3f ms 4:3 (layers uploaded and shaded, on average over "
                    "%llu screenshots)\n",
                    o.screen_w, o.screen_h, m.screen_wide_ms / static_cast<double>(m.screen_samples),
                    m.screen_43_ms / static_cast<double>(m.screen_samples), static_cast<unsigned long long>(m.screen_samples));
#endif
    if (o.screenshot) {
        if (m.frames_rendered)
            std::printf("Real3D renderer: %.2f ms a frame on average (%.2f setting up, %.2f drawing; %llu frames)\n",
                        1000.0 * m.render_seconds / static_cast<double>(m.frames_rendered),
                        m.setup_ms / static_cast<double>(m.frames_rendered),
                        m.raster_ms / static_cast<double>(m.frames_rendered),
                        static_cast<unsigned long long>(m.frames_rendered));
        if (m.gpu_frames)
            std::printf("GPU renderer: %.2f ms a frame on average, %.2f at most (frame %llu), %llu over 17.4 ms, %s (%llu frames)\n",
                        1000.0 * m.gpu_seconds / static_cast<double>(m.gpu_frames), 1000.0 * m.gpu_worst,
                        static_cast<unsigned long long>(m.gpu_worst_frame), static_cast<unsigned long long>(m.gpu_slow),
                        o.gpu_time_scale ? ("at render scale " + std::to_string(o.gpu_time_scale) + ", waited for").c_str()
                                         : "read back",
                        static_cast<unsigned long long>(m.gpu_frames));
        if (m.gpu_frames && !o.gpu_time_scale)
            std::printf("GPU pictures: hash %016llx (render scale %d)\n", static_cast<unsigned long long>(m.gpu_hash),
                        o.gpu_scale);
        if (m.frames_compared)
            std::printf("GPU against software: %llu of %llu frames differ (a channel more than 2 apart), %llu pixels "
                        "in all, %.1f a frame; the most in frame %llu (%llu pixels); largest difference %d\n",
                        static_cast<unsigned long long>(m.frames_differing), static_cast<unsigned long long>(m.frames_compared),
                        static_cast<unsigned long long>(m.pixels_differing),
                        static_cast<double>(m.pixels_differing) / static_cast<double>(m.frames_compared),
                        static_cast<unsigned long long>(m.worst_frame), static_cast<unsigned long long>(m.worst_pixels),
                        m.max_difference);
        std::printf("  block culling (white, no scene) in %llu frames; spotlight on in %llu; scroll fog in %llu, "
                    "ambient fog in %llu; triangle setups with a microtexture %llu, translator map %llu, inverted "
                    "texture %llu\n",
                    static_cast<unsigned long long>(m.frames_block_culled),
                    static_cast<unsigned long long>(m.frames_spotlit),
                    static_cast<unsigned long long>(m.frames_scroll_fog),
                    static_cast<unsigned long long>(m.frames_ambient_fog), static_cast<unsigned long long>(m.micro_tris),
                    static_cast<unsigned long long>(m.translator_tris), static_cast<unsigned long long>(m.inverted_tris));
        const auto& st = m.real3d.stats();
        std::printf("Real3D (last frame): %u viewports, %u nodes, %u models, %u polygons, %u triangles drawn; "
                    "%llu textures uploaded\n",
                    st.viewports, st.nodes, st.models, st.polygons, st.triangles_drawn,
                    static_cast<unsigned long long>(bus.real3d().textures_uploaded()));
        std::printf("  %u models in polygon RAM, %u hidden nodes, %u nodes too far (LOD), %u outside the view, "
                    "%u models faded out\n",
                    st.models_in_polygon_ram, st.hidden_nodes, st.lod_rejected, st.frustum_culled, st.faded_out);
        std::printf("  configuration registers: %08x %08x %08x %08x\n", bus.real3d().config(0), bus.real3d().config(1),
                    bus.real3d().config(2), bus.real3d().config(3));
        for (const auto& v : m.real3d.viewport_info())
            std::printf("  viewport %06x: priority %d, %gx%g at (%g, %g), eye matrix %u, %u models%s; "
                        "spotlight words %08x %08x %08x %08x %08x\n",
                        v.addr, v.priority, static_cast<double>(v.width), static_cast<double>(v.height),
                        static_cast<double>(v.x), static_cast<double>(v.y), v.eye, v.models, v.disabled ? ", off" : "",
                        v.spot_words[0], v.spot_words[1], v.spot_words[2], v.spot_words[3], v.spot_words[4]);
    }
    std::printf("EEPROM:");
    for (uint32_t i = 0; i < 64; ++i) std::printf("%s%04x", i % 16 == 0 ? "\n  " : " ", bus.eeprom().words()[i]);
    std::printf("\n");
    m3::CommBoard* comm = m.comm;
    if (comm && !comm->trace.empty()) {
        std::printf("\nNetwork board 68000, register accesses:\n");
        for (const auto& a : comm->trace)
            std::printf("  %10llu  pc %06x  %s%u %06x  %04x\n", static_cast<unsigned long long>(a.cycle), a.pc,
                        a.write ? "W" : "R", a.size * 8u, a.addr, a.value);
    }
    if (comm && o.net_disasm && m.index == 0) {
        if (std::FILE* f = std::fopen((std::string(o.net_disasm) + ".bin").c_str(), "wb")) { // raw 68000 RAM
            std::fwrite(comm->ram().data(), 1, comm->ram().size(), f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen(o.net_disasm, "w")) {
            const auto ram = comm->ram();
            for (uint32_t addr = 0; addr < 0x10000;) {
                uint32_t len = 2;
                const std::string text = comm->disassemble(addr, &len);
                std::fprintf(f, "%06x  %02x%02x  %s\n", addr, ram[addr], ram[addr + 1], text.c_str());
                addr += std::max<uint32_t>(len, 2);
            }
            std::fclose(f);
        }
    }

    std::printf("\nLast blocks:\n");
    for (uint32_t pc : c.recent_blocks())
        if (pc) {
            const uint32_t raw = bus.read32(pc);
            std::printf("  %08x  %s\n", pc, ppc::disassemble(ppc::decode(raw), pc).c_str());
        }

    if (o.show_text) {
        // The four layers' name tables: 64 x 64 entries of 16 bits from
        // 0xF10F8000, 0x2000 bytes apart. The tile generator is
        // little-endian, so the two entries in each CPU word are swapped.
        // Spikeout's font puts ASCII at tile numbers 0x20-0x7E.
        const auto tile = bus.tile_ram();
        std::printf("\nTilegen registers (as the tile generator sees them):\n ");
        for (uint32_t reg : {0x00u, 0x08u, 0x0Cu, 0x20u, 0x40u, 0x44u, 0x60u, 0x64u, 0x68u, 0x6Cu})
            std::printf(" %02x=%08x", reg, bus.tile_reg(reg));
        std::printf("\n\nText on the tile layers:\n");
        for (uint32_t layer = 0; layer < 4; ++layer) {
            for (uint32_t row = 0; row < 64; ++row) {
                std::string line;
                for (uint32_t col = 0; col < 64; ++col) {
                    const uint32_t off = 0xF8000 + layer * 0x2000 + row * 128 + (col ^ 1) * 2;
                    const uint32_t entry = uint32_t{tile[off]} << 8 | tile[off + 1];
                    const uint32_t code = entry >> 8;
                    line += (entry & 0xFF) == 0 && code >= 0x20 && code < 0x7F ? static_cast<char>(code) : '.';
                }
                if (line.find_first_not_of(" .") != std::string::npos)
                    std::printf("  layer %u row %2u: %s\n", layer, row, line.c_str());
            }
        }
    }

    if (o.screenshot) {
        const std::string path = board_path(o.screenshot, m.index);
        if (tools::write_png(path, m.screen, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight))
            std::printf("\nScreen (last finished frame) written to %s\n", path.c_str());
        else
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
    }

    if (o.dump_tile) {
        const std::string path = board_path(o.dump_tile, m.index);
        std::FILE* f = std::fopen(path.c_str(), "wb");
        const auto tile = bus.tile_ram();
        if (!f || std::fwrite(tile.data(), 1, tile.size(), f) != tile.size())
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
        if (f) std::fclose(f);
    }

    if (o.dump_backup) {
        const std::string path = board_path(o.dump_backup, m.index);
        std::FILE* f = std::fopen(path.c_str(), "wb");
        const auto ram = bus.backup_ram();
        if (!f || std::fwrite(ram.data(), 1, ram.size(), f) != ram.size())
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
        if (f) std::fclose(f);
    }

    if (o.dump_textures) {
        const std::string path = board_path(o.dump_textures, m.index);
        const auto ram = bus.real3d().texture_ram();
        std::vector<uint32_t> picture(ram.size());
        for (std::size_t i = 0; i < ram.size(); ++i) {
            const uint32_t t = ram[i];
            auto c5 = [](uint32_t v) { return (v << 3) | (v >> 2); };
            picture[i] = 0xFF000000u | c5((t >> 10) & 0x1F) << 16 | c5((t >> 5) & 0x1F) << 8 | c5(t & 0x1F);
        }
        if (!tools::write_png(path, picture, 2048, static_cast<int>(ram.size() / 2048)))
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
    }

    std::printf("\nDevices accessed that aren't emulated yet:\n");
    if (bus.unhandled().empty()) std::printf("  none\n");
    for (const auto& [region, u] : bus.unhandled())
        std::printf("  %-16.*s %llu reads, %llu writes (first at %08x)\n", int(region.size()), region.data(),
                    static_cast<unsigned long long>(u.reads), static_cast<unsigned long long>(u.writes),
                    u.first_addr);
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    tools::Recording recording;
    bool blocks_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--blocks" && i + 1 < argc) o.blocks = std::strtoll(argv[++i], nullptr, 0), blocks_given = true;
        else if (arg == "--replay" && i + 1 < argc) {
            std::string error;
            if (!recording.load(argv[++i], error)) {
                std::fprintf(stderr, "gamerun: %s\n", error.c_str());
                return 1;
            }
            o.replay = &recording;
        }
        else if (arg == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const size_t at = spec.find_last_of('@');
            if (at == std::string::npos) {
                std::fprintf(stderr, "gamerun: --save-state FILE@FRAME\n");
                return 1;
            }
            o.save_state = spec.substr(0, at);
            o.save_frame = std::strtoull(spec.c_str() + at + 1, nullptr, 0);
        }
        else if (arg == "--load-state" && i + 1 < argc) o.load_state = argv[++i];
        else if (arg == "--watch-write" && i + 1 < argc) { // LO-HI@FRAME[:N], hex addresses
            const std::string spec = argv[++i];
            char* end = nullptr;
            o.watch_lo = static_cast<uint32_t>(std::strtoul(spec.c_str(), &end, 16));
            if (*end == '-') o.watch_hi = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
            if (*end == '@') o.watch_frame = std::strtoull(end + 1, &end, 0);
            if (*end == ':') o.watch_frames = std::strtoull(end + 1, &end, 0);
        }
        else if (arg == "--dump-tile" && i + 1 < argc) o.dump_tile = argv[++i];
        else if (arg == "--dump-textures" && i + 1 < argc) o.dump_textures = argv[++i];
        else if (arg == "--dump-backup" && i + 1 < argc) o.dump_backup = argv[++i];
        else if (arg == "--dump-layers") o.dump_layers = true;
        else if (arg == "--eeprom" && i + 1 < argc) o.eeprom = argv[++i];
        else if (arg == "--text") o.show_text = true;
        else if (arg == "--netboard" && i + 1 < argc) o.real_netboard = std::string(argv[++i]) == "real";
        else if (arg == "--net-trace" && i + 1 < argc) o.net_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--net-disasm" && i + 1 < argc) o.net_disasm = argv[++i];
        else if (arg == "--net-force-master") o.force_master = true;
        else if (arg == "--net-send-waits" && i + 1 < argc) o.send_waits_for_receive = std::string(argv[++i]) != "off";
        else if (arg == "--net-watch") o.net_watch = true;
        else if (arg == "--net-other-bank") o.host_sees_other_bank = true;
        else if (arg == "--net-e0-irq5" && i + 1 < argc) o.irq5_on_e0 = std::string(argv[++i]) != "off";
        else if (arg == "--link-byte-cycles" && i + 1 < argc) o.link_cycles_per_byte = std::strtod(argv[++i], nullptr);
        else if (arg == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (arg == "--wav" && i + 1 < argc) o.wav = argv[++i];
        else if (arg == "--quads") o.quads = true;
        else if (arg == "--gpu-time" && i + 1 < argc) o.gpu_time_scale = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (arg == "--gpu-scale" && i + 1 < argc) o.gpu_scale = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (arg == "--wide" && i + 1 < argc) o.wide = std::clamp(std::atoi(argv[++i]), 0, 400);
        else if (arg == "--match-models") o.match_models = true;
        else if (arg == "--gpu-slow" && i + 1 < argc) o.gpu_profile_over = std::atof(argv[++i]);
        else if (arg == "--gpu-hash-frames") o.gpu_hash_frames = true;
        else if (arg == "--gpu-full" && i + 1 < argc) o.gpu_full_frame = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--interp") o.interp = o.match_models = true;
        else if (arg == "--interp-trace" && i + 1 < argc) {
            const std::string range = argv[++i];
            const size_t colon = range.find(':');
            o.interp = o.match_models = o.interp_trace = true;
            o.interp_trace_from = std::strtoull(range.c_str(), nullptr, 0);
            o.interp_trace_to = colon == std::string::npos ? o.interp_trace_from + 1
                                                           : std::strtoull(range.c_str() + colon + 1, nullptr, 0);
        }
        else if (arg == "--screen-time" && i + 1 < argc) {
            const std::string size = argv[++i];
            const size_t x = size.find('x');
            if (x != std::string::npos) {
                o.screen_w = std::clamp(std::atoi(size.substr(0, x).c_str()), 0, 8192);
                o.screen_h = std::clamp(std::atoi(size.substr(x + 1).c_str()), 0, 8192);
            }
        }
        else if (arg == "--renderer" && i + 1 < argc) {
            const std::string r = argv[++i];
            o.renderer = r == "gpu" ? Options::Renderer::Gpu : r == "compare" ? Options::Renderer::Compare
                                                                             : Options::Renderer::Software;
        }
        else if (arg == "--midi-burst") o.midi_burst = true;
        else if (arg == "--dma-instant") o.dma_instant = true;
        else if (arg == "--sound-volume" && i + 1 < argc) o.sound_volume = std::atoi(argv[++i]);
        else if (arg == "--music-volume" && i + 1 < argc) o.music_volume = std::atoi(argv[++i]);
        else if (arg == "--shot-every" && i + 1 < argc) o.shot_every = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--boards" && i + 1 < argc) o.boards = std::max<std::size_t>(1, std::strtoull(argv[++i], nullptr, 0));
        else if (arg == "--slice" && i + 1 < argc) o.slice = std::max<uint32_t>(1, static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
        else if (arg == "--link-latency" && i + 1 < argc) o.link_latency = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--parallel") o.parallel = true;
        else if (arg == "--no-2d") o.no_2d = true;
        else if (arg == "--no-sound") o.no_sound = true;
        else if (arg == "--link-trace" && i + 1 < argc) o.link_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--probe" && i + 1 < argc) { // X,Y@FRAME
            unsigned long long frame = 0;
            if (std::sscanf(argv[++i], "%d,%d@%llu", &o.probe_x, &o.probe_y, &frame) == 3) o.probe_frame = frame;
        }
        else if (arg == "--tilegen-trace" && i + 1 < argc) o.tilegen_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--poke" && i + 1 < argc) {
            Poke k;
            const std::string spec = board_prefix(argv[++i], k.board);
            unsigned addr = 0, value = 0;
            unsigned long long frame = 0, frames = 1;
            const int n = std::sscanf(spec.c_str(), "%x=%x@%llu:%llu", &addr, &value, &frame, &frames);
            if (n < 3) {
                std::fprintf(stderr, "gamerun: --poke [B:]ADDR=VALUE@FRAME[:N] (hex address and value)\n");
                return 2;
            }
            k.addr = addr;
            k.value = static_cast<uint8_t>(value);
            k.frame = frame;
            k.frames = n == 4 ? frames : 1;
            o.pokes.push_back(k);
        }
        else if (arg == "--press" && i + 1 < argc) {
            // [B:]NAME@FRAME[:N]: hold NAME for N frames (default 6) from FRAME.
            Press p;
            const std::string spec = board_prefix(argv[++i], p.board);
            const size_t at = spec.find('@'), colon = spec.find(':', at == std::string::npos ? 0 : at);
            p.name = spec.substr(0, at);
            if (at != std::string::npos) p.frame = std::strtoull(spec.c_str() + at + 1, nullptr, 0);
            if (colon != std::string::npos) p.frames = std::strtoull(spec.c_str() + colon + 1, nullptr, 0);
            if (!input_bit(p.name, p.bank, p.bit)) {
                std::fprintf(stderr, "gamerun: unknown input '%s'\n", p.name.c_str());
                return 2;
            }
            o.presses.push_back(p);
        }
        else if (!o.dir) o.dir = argv[i];
        else o.dir = nullptr, i = argc;
    }
    if (!o.dir) {
        std::fprintf(stderr, "usage: gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE]\n"
                             "               [--netboard sim|real] [--net-trace N] [--net-disasm FILE]\n"
                             "               [--screenshot FILE.png] [--shot-every N] [--wav FILE.wav]\n"
                             "               [--probe X,Y@FRAME] [--quads] [--midi-burst] [--dma-instant]\n"
                             "               [--sound-volume N] [--music-volume N]\n"
                             "               [--boards N] [--slice LINES] [--link-latency CYCLES]\n"
                             "               [--press [B:]NAME@FRAME[:N]]... [--poke [B:]ADDR=VALUE@FRAME[:N]]...\n");
        return 2;
    }
    if (o.replay) {
        if (!blocks_given) o.blocks = INT64_MAX / 2; // (to the recording's end)
        std::printf("replay: %zu frames of inputs, %zu hashes\n", o.replay->inputs.size(), o.replay->hashes.size());
    }
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const m3::RomSet roms = m3::load_rom_set(game, o.dir);
    if (!roms.complete()) {
        std::fprintf(stderr, "gamerun: ROM set incomplete; run romcheck for details\n");
        return 1;
    }

    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();
    const std::vector<recomp::FunctionEntry> blocks = tools::game_blocks(); // (save states)

    std::unique_ptr<m3::LinkRing> ring;
    if (o.boards > 1) {
        ring = std::make_unique<m3::LinkRing>(o.boards);
        ring->set_latency(o.link_latency);
        if (o.link_trace)
            ring->on_send([left = o.link_trace](std::size_t from, uint64_t at, const std::vector<uint8_t>& data) mutable {
                if (!left) return;
                --left;
                std::printf("link: board %zu sends at frame %.3f, %zu bytes:", from,
                            static_cast<double>(at) / static_cast<double>(recomp::FrameTiming().frame_cycles()),
                            data.size());
                for (std::size_t k = 0; k < std::min<std::size_t>(data.size(), 24); ++k) std::printf(" %02x", data[k]);
                std::printf("\n");
            });
    }
    // Parallel: the boards run at once, meeting every slice; the link's
    // packets go on there, so they take at least a slice to arrive.
    const bool parallel = o.parallel && o.boards > 1;
    if (parallel) {
        const uint64_t slice_cycles =
            (recomp::FrameTiming().frame_cycles() * o.slice + recomp::FrameTiming::kLines - 1) / recomp::FrameTiming::kLines;
        ring->set_deferred(true);
        ring->set_latency(std::max(o.link_latency, slice_cycles));
        std::printf("Parallel: the link's latency %llu main board cycles (a slice of %u lines)\n",
                    static_cast<unsigned long long>(std::max(o.link_latency, slice_cycles)), o.slice);
    }
    tools::Turns turns(o.boards, parallel, parallel ? std::function<void()>([&ring] { ring->exchange(); }) : std::function<void()>());
    // The GPU (--renderer gpu or compare), without a window; the boards take
    // turns, so they share it.
#ifdef SPIKEOUT_GPU
    gpu::Device gpu_device;
    if (o.renderer != Options::Renderer::Software || o.gpu_time_scale) {
        if (gpu_device.open_headless()) {
            std::printf("GPU: %s\n", gpu_device.driver());
        } else {
            std::fprintf(stderr, "gamerun: no GPU (%s)\n", gpu_device.error().c_str());
            return 1;
        }
    }
#else
    if (o.renderer != Options::Renderer::Software || o.gpu_time_scale) {
        std::fprintf(stderr, "gamerun: built without the GPU renderer (SPIKEOUT_GPU)\n");
        return 1;
    }
#endif
    std::vector<std::unique_ptr<Machine>> machines;
    for (std::size_t i = 0; i < o.boards; ++i) {
        machines.push_back(std::make_unique<Machine>(i, roms, functions));
        machines.back()->c.set_blocks(blocks);
        m3::games::install_spikeout_hooks(machines.back()->c);
        set_up(*machines.back(), o, game, ring.get(), turns);
#ifdef SPIKEOUT_GPU
        if (o.renderer != Options::Renderer::Software || o.gpu_time_scale) {
            auto& m = *machines.back();
            m.gpu = std::make_unique<gpu::Real3dGpu>();
            m.gpu->set_quads(o.quads);
            m.gpu_device = gpu_device.get();
            if (o.wide && ((o.renderer == Options::Renderer::Gpu && o.gpu_scale == 1) || o.gpu_time_scale)) { // (--gpu-time: timed so)
                m.builder.set_extend(o.wide);
                m.tilegen.set_extend(o.wide);
            }
            if (!m.gpu->open(gpu_device, o.gpu_time_scale ? o.gpu_time_scale : o.gpu_scale, m.builder.extend())) {
                std::fprintf(stderr, "gamerun: GPU renderer: %s\n", m.gpu->error().c_str());
                return 1;
            }
            if (o.screen_w > 0 && o.screen_h > 0 && i == 0) { // the window's screen pass, off screen
                SDL_GPUTextureCreateInfo tex{};
                tex.type = SDL_GPU_TEXTURETYPE_2D;
                tex.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
                tex.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
                tex.width = static_cast<Uint32>(o.screen_w);
                tex.height = static_cast<Uint32>(o.screen_h);
                tex.layer_count_or_depth = 1;
                tex.num_levels = 1;
                m.screen_target = SDL_CreateGPUTexture(gpu_device.get(), &tex);
                m.presenter = std::make_unique<gpu::Presenter>();
                if (!m.screen_target || !m.presenter->open(gpu_device)) {
                    std::fprintf(stderr, "gamerun: --screen-time: can't draw the screen off screen\n");
                    return 1;
                }
                m.presenter->set_target(m.screen_target, static_cast<uint32_t>(o.screen_w), static_cast<uint32_t>(o.screen_h));
            }
        }
#endif
    }

    std::printf("%zu recompiled functions; running up to %lld blocks from %08x", functions.size(),
                static_cast<long long>(o.blocks), machines[0]->c.cpu.pc);
    if (o.boards > 1)
        std::printf(" on each of %zu linked boards, %s every %u lines", o.boards, parallel ? "running at once, meeting" : "taking turns", o.slice);
    std::printf("\n\n");

    if (o.boards == 1) {
        run(*machines[0], turns);
    } else {
        std::vector<std::thread> threads;
        for (auto& m : machines) threads.emplace_back([&m, &turns] { run(*m, turns); });
        for (auto& t : threads) t.join();
    }

    for (auto& m : machines) {
        if (o.boards > 1) std::printf("%s==== Board %zu ====\n", m->index ? "\n" : "", m->index);
        report(*m, o);
    }
    if (ring) std::printf("\nLink: %llu packets delivered around the ring\n", static_cast<unsigned long long>(ring->delivered()));
    return 0;
}
