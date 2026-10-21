// gamerun: run the recompiled boot ROM into the recompiled game and report
// how far it gets.
//
//   gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE] [--dump-textures FILE.png]
//           [--dump-backup FILE] [--eeprom FILE]
//           [--netboard sim|real] [--net-trace N] [--net-disasm FILE]
//           [--screenshot FILE.png] [--shot-every N] [--press [B:]NAME@FRAME[:N]]...
//           [--wav FILE.wav] [--probe X,Y@FRAME] [--quads]
//           [--boards N] [--slice LINES] [--link-latency CYCLES] [--renderer software|gpu|compare]
//           [--gpu-scale N] [--gpu-time N]
//
// Built only when CMake's SPIKEOUT_ROM_DIR is set. A run ends at the first
// thing the runtime can't do yet (code that wasn't found, an exception, a
// device the game waits on), or after N basic blocks (default 200 million).
// That stop, the blocks before it and the devices the game touched are
// what to work on next. --text prints the text on the tile layers (what
// screen the game is on); --dump-tile writes the tile generator's RAM at the
// end (characters, name tables, palette; derived from the ROMs).
// --dump-backup writes the backup RAM (128 KB) at the end.
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
// was sent (plus --link-latency main board cycles, default 0). Each board's
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
#include "games/games.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/real3d_render.hpp"
#include "runtime/tilegen.hpp"
#include "game_setup.hpp"
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
    uint64_t link_trace = 0;
    bool net_watch = false;
    // --renderer: the 3D in software, on the GPU, or both, compared.
    enum class Renderer { Software, Gpu, Compare } renderer = Renderer::Software;
    int gpu_time_scale = 0; // --gpu-time: the GPU's time a frame at this render scale
    int gpu_scale = 1;      // --gpu-scale: the GPU renderer's render scale (gpu, compare)
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
    double gpu_seconds = 0, gpu_worst = 0;
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

void set_up(Machine& m, const Options& o, const m3::GameSpec& game, m3::LinkRing* ring, tools::Turns& turns) {
    m3::Bus& bus = m.bus;
    bus.midi_burst = o.midi_burst;
    bus.dma_timed = !o.dma_instant;
    bus.set_security_key(game.security_key);
    tools::set_rtc_to_host(bus);
    for (uint8_t& b : bus.ram()) b = 0xA5;
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
    m.c.limit_blocks(static_cast<uint64_t>(o.blocks));
    m.c.set_scheduler(&m.timing);
    m.real3d.set_probe(o.probe_x, o.probe_y);
    m.real3d.set_quads(o.quads);
    m.sound.board.set_volumes(o.sound_volume, o.music_volume);

    // The 2D layers, a line at a time; each finished frame is kept for --screenshot.
    // With several boards, the turn passes on every o.slice lines, the
    // network board caught up first so what it sent is on the link.
    m.timing.on_display_line = [&m, &turns, slice = o.slice](uint32_t line) {
        if (line == 0) m.tilegen.begin_frame();
        m.tilegen.draw_line(m.bus, static_cast<int>(line));
        if (turns.boards() > 1 && line % slice == slice - 1) {
            m.bus.netboard().sync(m.c.cycles());
            turns.pass(m.index);
        }
    };
    m.timing.on_frame_end = [&m, &o] {
        const uint64_t frame = m.timing.frames();
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
                for (int i = 0; i < kGpuTimeRepeats; ++i) m.gpu->render(mem, m.real3d.scene());
                SDL_WaitForGPUIdle(m.gpu_device);
            } else {
                m.gpu->render(mem, o.renderer == R::Gpu ? m.scene : m.real3d.scene(), &m.gpu_image);
                for (uint32_t p : m.gpu_image) m.gpu_hash = (m.gpu_hash ^ p) * 0x100000001B3ull; // FNV-1a, a word at a time
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
        m.tilegen.composite(m.screen, o.renderer == R::Gpu ? std::span<const uint32_t>(m.gpu_image) : m.real3d.image());
        const auto sound_start = std::chrono::steady_clock::now();
        const std::span<const int16_t> pcm =
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--blocks" && i + 1 < argc) o.blocks = std::strtoll(argv[++i], nullptr, 0);
        else if (arg == "--dump-tile" && i + 1 < argc) o.dump_tile = argv[++i];
        else if (arg == "--dump-textures" && i + 1 < argc) o.dump_textures = argv[++i];
        else if (arg == "--dump-backup" && i + 1 < argc) o.dump_backup = argv[++i];
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
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const m3::RomSet roms = m3::load_rom_set(game, o.dir);
    if (!roms.complete()) {
        std::fprintf(stderr, "gamerun: ROM set incomplete; run romcheck for details\n");
        return 1;
    }

    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();

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
    tools::Turns turns(o.boards);
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
        set_up(*machines.back(), o, game, ring.get(), turns);
#ifdef SPIKEOUT_GPU
        if (o.renderer != Options::Renderer::Software || o.gpu_time_scale) {
            auto& m = *machines.back();
            m.gpu = std::make_unique<gpu::Real3dGpu>();
            m.gpu->set_quads(o.quads);
            m.gpu_device = gpu_device.get();
            if (!m.gpu->open(gpu_device, o.gpu_time_scale ? o.gpu_time_scale : o.gpu_scale)) {
                std::fprintf(stderr, "gamerun: GPU renderer: %s\n", m.gpu->error().c_str());
                return 1;
            }
        }
#endif
    }

    std::printf("%zu recompiled functions; running up to %lld blocks from %08x", functions.size(),
                static_cast<long long>(o.blocks), machines[0]->c.cpu.pc);
    if (o.boards > 1)
        std::printf(" on each of %zu linked boards, taking turns every %u lines", o.boards, o.slice);
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
