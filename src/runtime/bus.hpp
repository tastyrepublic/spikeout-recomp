// Main-CPU bus of a Model 3 Step 2.1 board: work RAM, the program ROMs and
// the devices emulated so far. Accesses to anything else are open bus
// (reads return all ones, writes are dropped) and are counted per region so
// the runtime can report what the game touched.
//
// Memory is big-endian, as the CPU sees it: a 32-bit read of RAM returns the
// four bytes at that address with the first one in the top byte.
#pragma once

#include "ppc/cpu_state.hpp"
#include "runtime/watched_memory.hpp"
#include "runtime/crypt315_5881.hpp"
#include "runtime/real3d.hpp"
#include "runtime/rtc.hpp"
#include "runtime/state.hpp"
#include "runtime/tile_sides.hpp"
#include "runtime/eeprom.hpp"
#include "runtime/jtag.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/netboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/usart8251.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace m3 {

constexpr uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

// MPC106 PCI bridge: PCI configuration space through CONFIG_ADDR
// (0xFEC00CF8) and CONFIG_DATA (0xFEE00CFC). The bridge itself is bus 0,
// device 0; the Real3D is device 13 and answers only its ID (0x178611DB,
// the Step 2.x DMA device: Real3D.cpp, ReadPCIConfigSpace), which the game
// checks before it uses DMA. Other devices read as all ones. Reset
// values and the byte order of the data port follow Supermodel's
// Src/Model3/MPC10x.cpp: the registers are stored little-endian, as on PCI,
// and the data port hands the bytes to the CPU in address order, so the
// boot code reads them with lhbrx/lwbrx.
class Mpc106 {
public:
    Mpc106() { reset(); }
    void reset();

    // CONFIG_ADDR as written by the CPU (byte-reversed with stwbrx, since
    // the register is little-endian).
    void write_addr(uint32_t value) { addr_ = bswap32(value); }
    uint32_t read_addr() const { return bswap32(addr_); }
    uint32_t config_addr() const { return addr_; }

    // Byte `offset` (0-3) of the selected configuration register.
    uint8_t read_data(uint32_t offset) const;
    void write_data(uint32_t offset, uint8_t value);

    std::span<const uint8_t> registers() const { return regs_; }
    void state(State& s) { s.io(addr_), s.io(regs_); }

private:
    bool bridge_selected() const { return ((addr_ >> 8) & 0xFFFF) == 0; } // bus 0, device 0, function 0
    static constexpr uint32_t kReal3dDevice = 13;
    uint32_t addr_ = 0;
    std::array<uint8_t, 256> regs_{};
};

// Interrupt controller in the system registers (Supermodel's IRQ.cpp):
// pending bits and an enable mask; the CPU's external interrupt line is up
// while a pending bit is enabled. Bits above 0xFF can't be masked.
// Known sources: 0x02 display (line 383), 0x0D end of VBlank (0x01, 0x04,
// 0x08 together), 0x40 sound board MIDI.
class Irq {
public:
    void assert_irq(uint32_t bits) {
        state_ |= bits;
        notify();
    }
    void deassert(uint32_t bits) {
        state_ &= ~bits;
        notify();
    }
    void set_enable(uint32_t mask) {
        enable_ = mask;
        notify();
    }
    uint32_t state() const { return state_; }
    uint32_t enable() const { return enable_; }
    bool line() const { return (state_ & enable_ & 0xFF) != 0 || state_ > 0xFF; }
    // (Loading doesn't call on_change: the runtime looks again after a load.)
    void state(State& s) { s.io(state_), s.io(enable_); }

    // Called whenever the line may have changed (the runtime rechecks it).
    void (*on_change)(void* user) = nullptr;
    void* on_change_user = nullptr;

private:
    void notify() {
        if (on_change) on_change(on_change_user);
    }
    uint32_t state_ = 0;
    uint32_t enable_ = 0;
};

class Bus {
public:
    // `roms` must outlive the bus. Without program ROMs (tests) the ROM
    // window reads as open bus.
    explicit Bus(const RomSet& roms);

    uint8_t read8(uint32_t addr) {
        return addr < kWorkRamSize ? ram_[addr] : read_byte(addr);
    }
    uint16_t read16(uint32_t addr) {
        if (addr < kWorkRamSize - 1) return static_cast<uint16_t>(ram_[addr] << 8 | ram_[addr + 1]);
        return read_slow16(addr);
    }
    uint32_t read32(uint32_t addr) {
        if (addr < kWorkRamSize - 3) return load_be32(&ram_[addr]);
        return read_slow32(addr);
    }
    // 64-bit accesses are two 32-bit ones, high word first (Supermodel).
    uint64_t read64(uint32_t addr) { return uint64_t{read32(addr)} << 32 | read32(addr + 4); }

    void write8(uint32_t addr, uint8_t value) {
        if (addr < fast_limit_[0]) ram_[addr] = value;
        else write_slow(addr, value, 1);
    }
    void write16(uint32_t addr, uint16_t value) {
        if (addr < fast_limit_[1]) {
            ram_[addr] = static_cast<uint8_t>(value >> 8);
            ram_[addr + 1] = static_cast<uint8_t>(value);
        } else {
            write_slow(addr, value, 2);
        }
    }
    void write32(uint32_t addr, uint32_t value) {
        if (addr < fast_limit_[2]) store_be32(&ram_[addr], value);
        else write_slow(addr, value, 4);
    }
    void write64(uint32_t addr, uint64_t value) {
        write32(addr, static_cast<uint32_t>(value >> 32));
        write32(addr + 4, static_cast<uint32_t>(value));
    }

    std::span<uint8_t> ram() { return ram_; }
    std::span<const uint8_t> ram() const { return ram_; }
    Mpc106& pci() { return pci_; }
    Irq& irq() { return irq_; }
    NetBoardDevice& netboard() { return *netboard_; }
    // Swap in another network board (a CommBoard runs the real firmware;
    // the default is the SimNetBoard simulation).
    void use_netboard(std::unique_ptr<NetBoardDevice> board) { netboard_ = std::move(board); }
    // Another board's network board in this one's place for a while (one
    // board a PC: another cabinet's link code run here as that cabinet), its
    // reads and writes going there even while device writes are held;
    // nullptr puts this board's own back.
    void borrow_netboard(NetBoardDevice* board) { borrowed_ = board; }

    // The main CPU's time in cycles, for devices with their own clock to
    // catch up to before an access (set by the runtime).
    void set_clock(uint64_t (*fn)(const void*), const void* user) {
        clock_fn_ = fn;
        clock_user_ = user;
    }
    uint64_t now() const { return clock_fn_ ? clock_fn_(clock_user_) : 0; }

    // Real3D status registers (0x84000000, also read through the DMA
    // device's command 0x80000000 | reg). Register 0 holds the ping-pong
    // bit (0x02000000), which flips once a frame when the Real3D is done
    // with it; games pace their rendering on it (Supermodel's
    // CReal3D::ReadRegister / FlipPingPongBit).
    uint32_t real3d_register(uint32_t reg) const;
    void flip_ping_pong() {
        ping_pong_ = !ping_pong_;
        real3d_.ping_pong_flipped();
    }
    bool ping_pong() const { return ping_pong_; }
    // Start of VBlank, after the frame is drawn: the Real3D's buffered
    // updates land now (with the Mercury mode word for its frame signals).
    void real3d_begin_vblank() { real3d_.begin_vblank(mercury_mode()); }
    const Jtag& jtag() const { return jtag_; }
    // Tilegen registers (0xF1180000, 32-bit only), as the tile generator
    // sees them (little-endian: the bus byte-swaps, as Supermodel does).
    // Register 0x10 acknowledges IRQs: writing a bit clears it.
    uint32_t tile_reg(uint32_t reg) const { return tile_regs_[(reg & 0xFF) / 4]; }
    const std::array<uint32_t, 64>& tile_regs() const { return tile_regs_; }
    // Tilegen RAM 0xF1000000-0xF111FFFF (characters, name tables, palette),
    // in CPU byte order: byte i is what the CPU reads at 0xF1000000 + i.
    std::span<const uint8_t> tile_ram() const { return tile_ram_; }
    // Widescreen: the tile layers' columns past the screen, from the game's
    // own tile maps (not hardware: kept by the game's hooks; saved with the
    // bus, as the game's run decides them).
    TileSides& tile_sides() { return tile_sides_; }
    const TileSides& tile_sides() const { return tile_sides_; }
    // I/O board (315-5649, 0xF0040000): inputs and the settings EEPROM.
    // Register 0x00 selects the input bank and drives the EEPROM (CS bit 6,
    // CLK bit 7, DI bit 5); 0x04 reads the switches of that bank (bank 1:
    // EEPROM DO in bit 5); 0x08/0x0C the game's controls. Inputs are active
    // low: a frontend clears bits for pressed switches (Supermodel's
    // Model3.cpp, ReadInputs/WriteInputs, has the bit layout).
    struct Inputs {
        uint8_t bank0 = 0xFF; // coin 1/2 (bits 0-1), test A, service A, start 1/2 (bits 2-5)
        uint8_t bank1 = 0xFF; // service B (bit 6), test B (bit 7); bit 5 is the EEPROM
        uint8_t controls = 0xFF; // 0x08 Spikeout: beat, charge, shift, jump; down, up, right, left
        uint8_t controls2 = 0xFF; // 0x0C
    } inputs;
    Eeprom93C46& eeprom() { return eeprom_; }

    // Sound board MIDI port (0xF0080000): the uPD71051 serial chip
    // (`Usart8251`), +0 data, +4 mode/command (write) and status (read);
    // IRQ 0x40 follows its TxRDY pin. The bytes that have gone out, with the
    // cycle each arrived, collect in midi_out() for the frontend to empty.
    //
    // `midi_burst` selects Supermodel's timing instead: a byte goes out the
    // moment it is written, and the frame timing raises IRQ 0x40 every 1000
    // cycles at the start of VBlank while control bit 0x20 is set.
    bool midi_burst = false;
    uint8_t midi_control() const { return midi_ctrl_; }
    std::vector<SerialByte>& midi_out() { return midi_out_; }
    uint64_t midi_bytes() const { return midi_bytes_; }
    Usart8251& midi_uart() { return midi_uart_; }
    // The serial chip's next change of state, and moving it on to `cycle`
    // (for the board's scheduler).
    uint64_t midi_due() const { return midi_burst ? UINT64_MAX : midi_uart_.next_event(); }
    void run_midi(uint64_t cycle);
    // A Real3D DMA copy completing (status bit 7 drops, the IRQ if asked for).
    uint64_t dma_due() const { return dma_.busy ? dma_.done_at : UINT64_MAX; }
    void run_dma(uint64_t cycle);
    // Every device event on the bus, for the scheduler.
    uint64_t device_due() const { return std::min(midi_due(), dma_due()); }
    void run_devices(uint64_t cycle) {
        run_midi(cycle);
        run_dma(cycle);
    }
    // Real3D DMA: a copy takes at least the time the PCI bus needs to carry
    // it (one 32-bit word per 33 MHz clock: 166/33 CPU cycles a word). False:
    // finished at once, as in Supermodel and MAME.
    bool dma_timed = true;
    // Called when midi_due() may have moved earlier (the runtime asks again).
    void (*on_due_change)(void* user) = nullptr;
    void* on_due_change_user = nullptr;
    // Debugging (gamerun --watch-write): `fn` is called for each write to
    // [lo, hi), with the access as write_slow has it. Watching work RAM sends
    // all its writes the slow way while it lasts.
    void watch_writes(uint32_t lo, uint32_t hi, void (*fn)(void*, uint32_t, uint32_t, uint32_t), void* user) {
        watch_lo_ = lo;
        watch_hi_ = hi;
        on_watch_ = fn;
        on_watch_user_ = user;
        update_fast_limit();
    }

    // Side runs (game code run once more for another cabinet, with RAM put
    // back after: tools/remote_cabinet.hpp): writes outside work RAM dropped
    // while set, so the devices (the sound board's MIDI port, the interrupts
    // it raises...) don't see them. But the CROM bank (what the code then
    // reads from banked ROM): switched as the code asks, put back after.
    void hold_device_writes(bool hold) {
        if (hold && !hold_devices_) held_crom_ = {crom_bank_, crom_bank_reg_};
        if (!hold && hold_devices_) crom_bank_ = held_crom_[0], crom_bank_reg_ = held_crom_[1];
        hold_devices_ = hold;
    }
    // And work RAM's writes kept in a journal while it's on (every write the
    // slow way), each byte's value before; undo_journal() puts them back,
    // newest first, and empties it. Cheaper than copying all of RAM when a
    // side run writes a few KB.
    void journal(bool on) {
        journaling_ = on;
        update_fast_limit();
    }
    void undo_journal() {
        for (auto it = journal_.rbegin(); it != journal_.rend(); ++it) ram_[it->first] = it->second;
        journal_.clear();
    }
    std::size_t journal_size() const { return journal_.size(); }

    // Real-time clock (0xF0140000), on the emulated time. It starts at
    // 1970-01-01 00:00; a frontend sets it (rtc().set_time) from the host's
    // clock or a fixed time for reproducible runs.
    Rtc72421& rtc() { return rtc_; }

    // Real3D memory (scene, models, textures), for the renderer.
    const Real3d& real3d() const { return real3d_; }
    Real3d& real3d() { return real3d_; }

    // Security board (315-5881). The game writes encrypted data into the
    // board's RAM (0xF0180000, 128 KB), sets a sequence key and reads the
    // plaintext back through the registers at 0xF01A0000 (Supermodel's
    // Model3.cpp, ReadSecurity/WriteSecurity): 0x00 status (never busy),
    // 0x10/0x14 restart at address 0, 0x18 sequence key (byte-swapped, in the
    // top half), 0x1C data (top half; the first read after a restart gives
    // 0xFFFF). The decrypter reads word i from the top half of the RAM's
    // 32-bit word i. Without a key (set_security_key) data reads all ones.
    void set_security_key(uint32_t key) { crypt_.set_key(key); }
    std::span<uint8_t> security_ram() { return security_ram_; }
    static constexpr uint32_t kSecurityRamSize = 0x20000;

    // Battery-backed SRAM (0xF00C0000, 128 KB). Starts zeroed; saving and
    // loading it across runs is up to the frontend.
    std::span<uint8_t> backup_ram() { return backup_ram_; }
    static constexpr uint32_t kBackupRamSize = 0x20000;
    static constexpr uint32_t kTileRamBase = 0xF1000000;
    static constexpr uint32_t kTileRamSize = 0x120000;
    uint32_t crom_bank() const { return crom_bank_; }

    // Save states (runtime/state.hpp): everything on the board but the CPU
    // (recomp::Context) and the settings a frontend chose (midi_burst,
    // dma_timed); the network board too.
    void state(State& s);

    // Accesses to regions that aren't emulated yet, by region name
    // ("unmapped" for addresses no device decodes).
    struct Unhandled {
        uint64_t reads = 0;
        uint64_t writes = 0;
        uint32_t first_addr = 0;
    };
    const std::map<std::string_view, Unhandled>& unhandled() const { return unhandled_; }

    static uint32_t load_be32(const uint8_t* p) {
        return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
    }
    static void store_be32(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }

private:
    uint8_t read_byte(uint32_t addr);
    uint16_t read_slow16(uint32_t addr);
    uint32_t read_slow32(uint32_t addr);
    // `value` holds the whole access, `size` bytes, big-endian.
    void write_slow(uint32_t addr, uint32_t value, uint32_t size);
    void count(const Region* region, uint32_t addr, bool write);
    uint8_t device_read(const Region& region, uint32_t addr);

    const RomSet& roms_;
    WatchedVector<uint8_t> ram_;
    // Writes below these (by size: 1, 2, 4 bytes) go straight to work RAM.
    std::array<uint32_t, 3> fast_limit_{kWorkRamSize, kWorkRamSize - 1, kWorkRamSize - 3};
    uint32_t watch_lo_ = 0, watch_hi_ = 0;
    void (*on_watch_)(void* user, uint32_t addr, uint32_t value, uint32_t size) = nullptr;
    void* on_watch_user_ = nullptr;
    bool hold_devices_ = false; // (hold_device_writes)
    std::array<uint32_t, 2> held_crom_{}; // (the CROM bank and its register as the hold began)
    bool journaling_ = false;   // (journal)
    std::vector<std::pair<uint32_t, uint8_t>> journal_;
    void update_fast_limit() {
        const bool slow = journaling_ || (on_watch_ && watch_lo_ < kWorkRamSize);
        fast_limit_ = slow ? std::array<uint32_t, 3>{} : std::array<uint32_t, 3>{kWorkRamSize, kWorkRamSize - 1, kWorkRamSize - 3};
    }
    uint8_t system_read(uint32_t reg) const;
    void system_write(uint32_t reg, uint8_t value);

    // Real3D DMA device (0xC2000000; Supermodel's Real3D.cpp,
    // Read/WriteDMARegister8/32): copies words around the bus, and answers
    // commands through 0x10/0x14 (the ID command gives Mercury's PCI ID,
    // 0x16C311DB, which Step 2.x games expect).
    uint8_t dma_read8(uint32_t reg) const;
    void dma_write8(uint32_t reg, uint8_t value);
    uint32_t dma_read32(uint32_t reg) const;
    void dma_write32(uint32_t reg, uint32_t value);
    struct Dma {
        uint32_t src = 0, dest = 0, data = 0;
        uint8_t status = 0, config = 0;
        bool busy = false;
        uint64_t done_at = 0; // cycle the copy in progress completes
    } dma_;

    Mpc106 pci_;
    Irq irq_;
    std::unique_ptr<NetBoardDevice> netboard_;
    NetBoardDevice* borrowed_ = nullptr;
    NetBoardDevice* net() const { // the board, caught up to now
        NetBoardDevice* board = borrowed_ ? borrowed_ : netboard_.get();
        board->sync(now());
        return board;
    }
    uint64_t (*clock_fn_)(const void*) = nullptr;
    const void* clock_user_ = nullptr;
    bool ping_pong_ = false;
    Jtag jtag_;
    uint32_t mercury_mode() const { return jtag_.modeword(Jtag::Asic::Mercury); }
    Eeprom93C46 eeprom_{ppc::CpuState::kCoreClockHz};
    uint8_t io_bank_ = 0;
    uint8_t io_read(uint32_t reg);
    void io_write(uint32_t reg, uint8_t value);
    Real3d real3d_;
    Rtc72421 rtc_{ppc::CpuState::kCoreClockHz};
    uint8_t midi_ctrl_ = 0;
    Usart8251 midi_uart_{ppc::CpuState::kCoreClockHz, 500'000};
    std::vector<SerialByte> midi_out_;
    uint64_t midi_bytes_ = 0;
    void midi_write(uint32_t reg, uint8_t value);
    void ack_irq(uint32_t bits);
    WatchedVector<uint8_t> security_ram_;
    Crypt315_5881 crypt_;
    bool security_first_read_ = true;
    uint32_t security_read32(uint32_t reg);
    void security_write32(uint32_t reg, uint32_t value);
    std::array<uint32_t, 64> tile_regs_{};
    WatchedVector<uint8_t> tile_ram_;
    TileSides tile_sides_;
    WatchedVector<uint8_t> backup_ram_;
    uint32_t crom_bank_ = 0;
    uint32_t crom_bank_reg_ = 0xFF;
    std::map<std::string_view, Unhandled> unhandled_;
};

} // namespace m3
