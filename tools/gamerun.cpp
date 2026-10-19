// gamerun: run the recompiled boot ROM into the recompiled game and report
// how far it gets.
//
//   gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE]
//           [--netboard sim|real] [--net-trace N] [--net-disasm FILE]
//           [--screenshot FILE.png] [--shot-every N] [--coin FRAME]
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
// The tile generator draws a line at a time as the frame runs (no 3D yet).
// --screenshot writes the last finished frame as a PNG; with --shot-every N
// also FILE-<frame>.png every N frames.
#include "games/games.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/tilegen.hpp"
#include "game_setup.hpp"
#include "png_write.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const char* dir = nullptr;
    const char* dump_tile = nullptr;
    const char* net_disasm = nullptr;
    const char* screenshot = nullptr;
    uint64_t shot_every = 0, tilegen_trace = 0, coin_frame = 0;
    bool show_text = false, real_netboard = true, force_master = false;
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
        else if (arg == "--shot-every" && i + 1 < argc) shot_every = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--tilegen-trace" && i + 1 < argc) tilegen_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--coin" && i + 1 < argc) coin_frame = std::strtoull(argv[++i], nullptr, 0);
        else if (!dir) dir = argv[i];
        else dir = nullptr, i = argc;
    }
    if (!dir) {
        std::fprintf(stderr, "usage: gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE]\n"
                             "               [--netboard sim|real] [--net-trace N] [--net-disasm FILE]\n"
                             "               [--screenshot FILE.png] [--shot-every N]\n");
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
    std::vector<uint32_t> screen(m3::TilegenRenderer::kWidth * m3::TilegenRenderer::kHeight);
    timing.on_display_line = [&](uint32_t line) {
        if (line == 0) tilegen.begin_frame();
        tilegen.draw_line(bus, static_cast<int>(line));
    };
    timing.on_frame_end = [&] {
        tilegen.composite(screen);
        bus.midi_out().clear(); // no sound board yet
        // --coin: hold coin 1 (bank 0 bit 0, active low) for 6 frames.
        const bool coin = coin_frame && timing.frames() >= coin_frame && timing.frames() < coin_frame + 6;
        bus.inputs.bank0 = static_cast<uint8_t>(coin ? 0xFE : 0xFF);
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
    std::printf("Ran %lld blocks in %.2f s (%.0f million blocks/s)\n", ran, seconds,
                static_cast<double>(ran) / seconds / 1e6);
    std::printf("PC %08x  LR %08x  CTR %08x  MSR %08x  CR %08x\n", c.cpu.pc, c.cpu.lr, c.cpu.ctr, c.cpu.msr, c.cpu.cr);
    std::printf("Frames %llu (%.2f s of game time); interrupts taken: %llu external, %llu decrementer; "
                "IRQ enable %02x, pending %02x\n",
                static_cast<unsigned long long>(timing.frames()),
                static_cast<double>(timing.frames()) / recomp::FrameTiming::kRefreshHz,
                static_cast<unsigned long long>(c.external_interrupts),
                static_cast<unsigned long long>(c.decrementer_interrupts), bus.irq().enable(), bus.irq().state());

    std::printf("Network board: %s\n", bus.netboard().describe().c_str());
    std::printf("Sound board: %llu MIDI bytes sent (not emulated yet)\n", static_cast<unsigned long long>(bus.midi_bytes()));
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
