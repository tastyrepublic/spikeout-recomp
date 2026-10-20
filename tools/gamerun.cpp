// gamerun: run the recompiled boot ROM into the recompiled game and report
// how far it gets.
//
//   gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE]
//           [--netboard sim|real] [--net-trace N] [--net-disasm FILE]
//           [--screenshot FILE.png] [--shot-every N] [--press NAME@FRAME[:N]]...
//           [--wav FILE.wav] [--probe X,Y@FRAME] [--quads]
//
// Built only when CMake's SPIKEOUT_ROM_DIR is set. A run ends at the first
// thing the runtime can't do yet (code that wasn't found, an exception, a
// device the game waits on), or after N basic blocks (default 200 million).
// That stop, the blocks before it and the devices the game touched are
// what to work on next. --text prints the text on the tile layers (what
// screen the game is on); --dump-tile writes the tile generator's RAM at the
// end (characters, name tables, palette; derived from the ROMs).
//
// The network board is its own 68000 running the firmware the game uploads
// (m3::CommBoard); --netboard sim uses the simulation instead. --net-trace N
// prints its first N accesses to the board's registers, --net-disasm writes
// a disassembly of its RAM (the firmware; derived from the ROMs).
//
// The tile generator draws a line at a time as the frame runs; the 3D is
// drawn at the end of a frame, only when screenshots are being written.
// --screenshot writes the last finished frame as a PNG; with --shot-every N
// also FILE-<frame>.png every N frames.
//
// --press NAME@FRAME[:N] holds an input for N frames (default 6) from FRAME:
// coin, start, test, service, beat, charge, shift, jump, up, down, left,
// right. Repeat it for more presses.
//
// The sound board (m3::SoundBoard) runs with the game; --wav writes what
// it played (44.1 kHz stereo).
//
// --probe X,Y@FRAME draws the 3D at FRAME and lists every triangle that
// covered pixel (X, Y), with its polygon header and what became of it there
// (drawn, behind, cut out, blended): for finding holes and wrong pixels.
// --quads draws quads whole, as Supermodel's "quad rendering" option (slower).
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

struct Press {
    std::string name;
    uint64_t frame = 0, frames = 6;
    int bank = 0; // 0: input bank 0 (coin, start...), 1: the controls byte
    int bit = 0;
};

// --poke ADDR=VALUE@FRAME[:N]: write a byte of RAM at the end of N frames
// (default 1) from FRAME, for experiments (a setting the game reads, say).
struct Poke {
    uint32_t addr = 0;
    uint8_t value = 0;
    uint64_t frame = 0, frames = 1;
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

// --probe: every triangle that covered the pixel, and what became of it.
void print_probe(const m3::Real3dRenderer& r, int x, int y, uint64_t frame) {
    std::printf("Probe %d,%d at frame %llu: %zu triangles cover it\n", x, y, static_cast<unsigned long long>(frame),
                r.probe().size());
    for (const auto& h : r.probe())
        std::printf("  polygon %06x [%08x %08x %08x %08x %08x %08x %08x] priority %d, %s format %d%s, alpha %.2f, "
                    "texel alpha %.2f, 1/w %.4g, %s: %s\n",
                    h.polygon, h.header[0], h.header[1], h.header[2], h.header[3], h.header[4], h.header[5],
                    h.header[6], h.priority, h.textured ? "textured" : "flat", h.format, h.contour ? " contour" : "",
                    static_cast<double>(h.alpha), static_cast<double>(h.texel_alpha), static_cast<double>(h.w), h.pass,
                    h.outcome);
}

} // namespace

int main(int argc, char** argv) {
    const char* dir = nullptr;
    const char* dump_tile = nullptr;
    const char* net_disasm = nullptr;
    const char* screenshot = nullptr;
    const char* wav = nullptr;
    uint64_t shot_every = 0, tilegen_trace = 0;
    int probe_x = -1, probe_y = -1;
    int sound_volume = 100, music_volume = 100;
    uint64_t probe_frame = 0;
    std::vector<Press> presses;
    std::vector<Poke> pokes;
    bool show_text = false, real_netboard = true, force_master = false, quads = false, midi_burst = false, dma_instant = false;
    std::size_t net_trace = 0;
    int64_t blocks = 200'000'000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--blocks" && i + 1 < argc) blocks = std::strtoll(argv[++i], nullptr, 0);
        else if (arg == "--dump-tile" && i + 1 < argc) dump_tile = argv[++i];
        else if (arg == "--text") show_text = true;
        else if (arg == "--netboard" && i + 1 < argc) real_netboard = std::string(argv[++i]) == "real";
        else if (arg == "--net-trace" && i + 1 < argc) net_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--net-disasm" && i + 1 < argc) net_disasm = argv[++i];
        else if (arg == "--net-force-master") force_master = true;
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (arg == "--wav" && i + 1 < argc) wav = argv[++i];
        else if (arg == "--quads") quads = true;
        else if (arg == "--midi-burst") midi_burst = true;
        else if (arg == "--dma-instant") dma_instant = true;
        else if (arg == "--sound-volume" && i + 1 < argc) sound_volume = std::atoi(argv[++i]);
        else if (arg == "--music-volume" && i + 1 < argc) music_volume = std::atoi(argv[++i]);
        else if (arg == "--shot-every" && i + 1 < argc) shot_every = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--probe" && i + 1 < argc) { // X,Y@FRAME
            unsigned long long frame = 0;
            if (std::sscanf(argv[++i], "%d,%d@%llu", &probe_x, &probe_y, &frame) == 3) probe_frame = frame;
        }
        else if (arg == "--tilegen-trace" && i + 1 < argc) tilegen_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--poke" && i + 1 < argc) {
            Poke k;
            unsigned addr = 0, value = 0;
            unsigned long long frame = 0, frames = 1;
            const int n = std::sscanf(argv[++i], "%x=%x@%llu:%llu", &addr, &value, &frame, &frames);
            if (n < 3) {
                std::fprintf(stderr, "gamerun: --poke ADDR=VALUE@FRAME[:N] (hex address and value)\n");
                return 2;
            }
            k.addr = addr;
            k.value = static_cast<uint8_t>(value);
            k.frame = frame;
            k.frames = n == 4 ? frames : 1;
            pokes.push_back(k);
        }
        else if (arg == "--press" && i + 1 < argc) {
            // NAME@FRAME[:N]: hold NAME for N frames (default 6) from FRAME.
            const std::string spec = argv[++i];
            const size_t at = spec.find('@'), colon = spec.find(':', at == std::string::npos ? 0 : at);
            Press p;
            p.name = spec.substr(0, at);
            if (at != std::string::npos) p.frame = std::strtoull(spec.c_str() + at + 1, nullptr, 0);
            if (colon != std::string::npos) p.frames = std::strtoull(spec.c_str() + colon + 1, nullptr, 0);
            if (!input_bit(p.name, p.bank, p.bit)) {
                std::fprintf(stderr, "gamerun: unknown input '%s'\n", p.name.c_str());
                return 2;
            }
            presses.push_back(p);
        }
        else if (!dir) dir = argv[i];
        else dir = nullptr, i = argc;
    }
    if (!dir) {
        std::fprintf(stderr, "usage: gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE]\n"
                             "               [--netboard sim|real] [--net-trace N] [--net-disasm FILE]\n"
                             "               [--screenshot FILE.png] [--shot-every N] [--wav FILE.wav]\n"
                             "               [--probe X,Y@FRAME] [--quads] [--midi-burst] [--dma-instant]\n"
                             "               [--sound-volume N] [--music-volume N]\n"
                             "               [--press NAME@FRAME[:N]]... [--poke ADDR=VALUE@FRAME[:N]]...\n");
        return 2;
    }
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const m3::RomSet roms = m3::load_rom_set(game, dir);
    if (!roms.complete()) {
        std::fprintf(stderr, "gamerun: ROM set incomplete; run romcheck for details\n");
        return 1;
    }

    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();

    m3::Bus bus(roms);
    bus.midi_burst = midi_burst;
    bus.dma_timed = !dma_instant;
    bus.set_security_key(game.security_key);
    tools::set_rtc_to_host(bus);
    for (uint8_t& b : bus.ram()) b = 0xA5;
    m3::CommBoard* comm = nullptr;
    if (real_netboard) {
        auto board = std::make_unique<m3::CommBoard>();
        comm = board.get();
        comm->trace_limit = net_trace;
        comm->force_master = force_master;
        bus.use_netboard(std::move(board));
    }
    recomp::Context c(bus, functions);
    c.limit_blocks(static_cast<uint64_t>(blocks));
    recomp::FrameTiming timing;
    c.set_scheduler(&timing);
    // The 2D layers, a line at a time; each finished frame is kept for --screenshot.
    m3::TilegenRenderer tilegen;
    m3::Real3dRenderer real3d;
    real3d.set_probe(probe_x, probe_y);
    real3d.set_quads(quads);
    std::vector<uint32_t> screen(m3::TilegenRenderer::kWidth * m3::TilegenRenderer::kHeight);
    // The sound board, a frame at a time; --wav keeps what it played.
    tools::FrameSound sound(roms);
    sound.board.set_volumes(sound_volume, music_volume);
    std::vector<int16_t> audio;
    int peak = 0;
    double energy = 0, sound_seconds = 0, render_seconds = 0, setup_ms = 0, raster_ms = 0;
    uint64_t frames_rendered = 0, frames_spotlit = 0, frames_scroll_fog = 0, frames_ambient_fog = 0, frames_block_culled = 0;
    uint64_t micro_tris = 0, translator_tris = 0, inverted_tris = 0;
    timing.on_display_line = [&](uint32_t line) {
        if (line == 0) tilegen.begin_frame();
        tilegen.draw_line(bus, static_cast<int>(line));
    };
    timing.on_frame_end = [&] {
        // The 3D is drawn in software: only when it will be seen.
        if (screenshot || (probe_frame && timing.frames() == probe_frame)) {
            const auto render_start = std::chrono::steady_clock::now();
            real3d.render(bus.real3d());
            setup_ms += real3d.stats().setup_ms;
            raster_ms += real3d.stats().raster_ms;
            if (real3d.stats().spotlights) ++frames_spotlit;
            if (real3d.stats().block_culled) ++frames_block_culled;
            {
                const auto& s = real3d.stats();
                frames_scroll_fog += s.scroll_fog;
                frames_ambient_fog += s.ambient_fog;
                micro_tris += s.micro_polys;
                translator_tris += s.translator_polys;
                inverted_tris += s.inverted_polys;
            }
            render_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - render_start).count();
            ++frames_rendered;
        }
        if (probe_frame && timing.frames() == probe_frame) print_probe(real3d, probe_x, probe_y, probe_frame);
        tilegen.composite(screen, real3d.image());
        const auto sound_start = std::chrono::steady_clock::now();
        const std::span<const int16_t> pcm = sound.frame(bus.midi_out(), timing.frame_start(timing.frames() - 1), timing.frame_cycles());
        sound_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - sound_start).count();
        if (wav) audio.insert(audio.end(), pcm.begin(), pcm.end());
        for (int16_t s : pcm) {
            peak = std::max(peak, std::abs(static_cast<int>(s)));
            energy += static_cast<double>(s) * s;
        }
        for (const Poke& k : pokes)
            if (timing.frames() >= k.frame && timing.frames() < k.frame + k.frames) bus.write8(k.addr, k.value);
        // --press: inputs held for the coming frame (active low).
        uint8_t in[2] = {0xFF, 0xFF};
        for (const Press& p : presses)
            if (timing.frames() >= p.frame && timing.frames() < p.frame + p.frames)
                in[p.bank] = static_cast<uint8_t>(in[p.bank] & ~(1u << p.bit));
        bus.inputs.bank0 = in[0];
        bus.inputs.controls = in[1];
        if (tilegen_trace && timing.frames() % tilegen_trace == 0) {
            std::printf("frame %6llu tilegen", static_cast<unsigned long long>(timing.frames()));
            for (uint32_t reg : {0x20u, 0x40u, 0x44u, 0x60u, 0x64u, 0x68u, 0x6Cu}) std::printf(" %02x=%08x", reg, bus.tile_reg(reg));
            std::printf("\n");
        }
        if (screenshot && shot_every && timing.frames() % shot_every == 0) {
            const std::string path = std::string(screenshot) + "-" + std::to_string(timing.frames()) + ".png";
            tools::write_png(path, screen, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
        }
    };
    std::printf("%zu recompiled functions; running up to %lld blocks from %08x\n\n", functions.size(),
                static_cast<long long>(blocks), c.cpu.pc);

    const auto start = std::chrono::steady_clock::now();
    std::string why;
    try {
        c.call(c.cpu.pc);
        why = "the reset code returned";
    } catch (const recomp::OutOfBudget&) {
        why = "block limit reached";
    } catch (const recomp::MissingCode& e) {
        why = e.what();
    } catch (const recomp::GuestError& e) {
        why = e.what();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const long long ran = static_cast<long long>(c.blocks);

    std::printf("Stopped: %s\n", why.c_str());
    std::printf("Ran %lld blocks in %.2f s (%.0f million blocks/s), %.2f 603e cycles a block\n", ran, seconds,
                static_cast<double>(ran) / seconds / 1e6,
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
                static_cast<unsigned long long>(timing.frames()),
                static_cast<double>(timing.frames()) / recomp::FrameTiming::kRefreshHz,
                static_cast<unsigned long long>(c.external_interrupts),
                static_cast<unsigned long long>(c.decrementer_interrupts), bus.irq().enable(), bus.irq().state());

    std::printf("Network board: %s\n", bus.netboard().describe().c_str());
    const double played = static_cast<double>(sound.board.samples());
    std::printf("Sound board: %s; %d slots playing, %llu MIDI bytes dropped; output peak %d, RMS %.0f; %.2f s to run it\n",
                sound.board.describe().c_str(), sound.board.active_slots(),
                static_cast<unsigned long long>(sound.board.midi_dropped()), peak,
                played > 0 ? std::sqrt(energy / (2 * played)) : 0.0, sound_seconds);
    if (const m3::Dsb2* music = sound.board.music())
        std::printf("Music board: %s; its output peak %d, RMS %.0f\n", music->describe().c_str(), music->music_peak,
                    played > 0 ? std::sqrt(music->music_energy / (2 * played)) : 0.0);
    if (wav && tools::write_wav(wav, audio, m3::Scsp::kSampleRate)) std::printf("Wrote %s\n", wav);
    if (c.first_nan_pc) // SPIKEOUT_FP_TRAP builds
        std::printf("First FP NaN: at %08x (%016llx)\n", c.first_nan_pc, static_cast<unsigned long long>(c.first_nan_bits));
    if (screenshot) {
        if (frames_rendered)
            std::printf("Real3D renderer: %.2f ms a frame on average (%.2f setting up, %.2f drawing; %llu frames)\n",
                        1000.0 * render_seconds / static_cast<double>(frames_rendered),
                        setup_ms / static_cast<double>(frames_rendered), raster_ms / static_cast<double>(frames_rendered),
                        static_cast<unsigned long long>(frames_rendered));
        std::printf("  block culling (white, no scene) in %llu frames; spotlight on in %llu; scroll fog in %llu, "
                    "ambient fog in %llu; triangle setups with a microtexture %llu, translator map %llu, inverted "
                    "texture %llu\n",
                    static_cast<unsigned long long>(frames_block_culled), static_cast<unsigned long long>(frames_spotlit),
                    static_cast<unsigned long long>(frames_scroll_fog),
                    static_cast<unsigned long long>(frames_ambient_fog), static_cast<unsigned long long>(micro_tris),
                    static_cast<unsigned long long>(translator_tris), static_cast<unsigned long long>(inverted_tris));
        const auto& st = real3d.stats();
        std::printf("Real3D (last frame): %u viewports, %u nodes, %u models, %u polygons, %u triangles drawn; "
                    "%llu textures uploaded\n",
                    st.viewports, st.nodes, st.models, st.polygons, st.triangles_drawn,
                    static_cast<unsigned long long>(bus.real3d().textures_uploaded()));
        std::printf("  %u models in polygon RAM, %u hidden nodes, %u nodes too far (LOD), %u outside the view, "
                    "%u models faded out\n",
                    st.models_in_polygon_ram, st.hidden_nodes, st.lod_rejected, st.frustum_culled, st.faded_out);
        std::printf("  configuration registers: %08x %08x %08x %08x\n", bus.real3d().config(0), bus.real3d().config(1),
                    bus.real3d().config(2), bus.real3d().config(3));
        for (const auto& v : real3d.viewport_info())
            std::printf("  viewport %06x: priority %d, %gx%g at (%g, %g), eye matrix %u, %u models%s; "
                        "spotlight words %08x %08x %08x %08x %08x\n",
                        v.addr, v.priority, static_cast<double>(v.width), static_cast<double>(v.height),
                        static_cast<double>(v.x), static_cast<double>(v.y), v.eye, v.models, v.disabled ? ", off" : "",
                        v.spot_words[0], v.spot_words[1], v.spot_words[2], v.spot_words[3], v.spot_words[4]);
    }
    std::printf("EEPROM:");
    for (uint32_t i = 0; i < 64; ++i) std::printf("%s%04x", i % 16 == 0 ? "\n  " : " ", bus.eeprom().words()[i]);
    std::printf("\n");
    if (comm && !comm->trace.empty()) {
        std::printf("\nNetwork board 68000, register accesses:\n");
        for (const auto& a : comm->trace)
            std::printf("  %10llu  pc %06x  %s%u %06x  %04x\n", static_cast<unsigned long long>(a.cycle), a.pc,
                        a.write ? "W" : "R", a.size * 8u, a.addr, a.value);
    }
    if (comm && net_disasm) {
        if (std::FILE* f = std::fopen((std::string(net_disasm) + ".bin").c_str(), "wb")) { // raw 68000 RAM
            std::fwrite(comm->ram().data(), 1, comm->ram().size(), f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen(net_disasm, "w")) {
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

    if (show_text) {
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

    if (screenshot) {
        if (tools::write_png(screenshot, screen, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight))
            std::printf("\nScreen (last finished frame) written to %s\n", screenshot);
        else
            std::fprintf(stderr, "gamerun: cannot write %s\n", screenshot);
    }

    if (dump_tile) {
        std::FILE* f = std::fopen(dump_tile, "wb");
        const auto tile = bus.tile_ram();
        if (!f || std::fwrite(tile.data(), 1, tile.size(), f) != tile.size()) std::fprintf(stderr, "gamerun: cannot write %s\n", dump_tile);
        if (f) std::fclose(f);
    }

    std::printf("\nDevices accessed that aren't emulated yet:\n");
    if (bus.unhandled().empty()) std::printf("  none\n");
    for (const auto& [region, u] : bus.unhandled())
        std::printf("  %-16.*s %llu reads, %llu writes (first at %08x)\n", int(region.size()), region.data(),
                    static_cast<unsigned long long>(u.reads), static_cast<unsigned long long>(u.writes),
                    u.first_addr);
    return 0;
}
