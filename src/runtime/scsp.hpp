// Sega/Yamaha 315-5687 SCSP (Saturn Custom Sound Processor): 32 slots that
// play 8- or 16-bit PCM from the chip's own sound RAM, with envelopes, LFOs,
// FM through the slots' ring buffer, and an effect DSP. The Model 3 sound
// board has two (m3::SoundBoard); the first one's timers, interrupts and
// MIDI input serve the sound CPU.
//
// Adapted from Supermodel's Src/Sound/SCSP.cpp, SCSPLFO.cpp and SCSPDSP.cpp
// (GPL-3.0-or-later; ElSemi's SCSP, R. Belmont's and Paul Prosser's MAME
// conversion, the Supermodel Team). Where this differs from it, see
// docs/fidelity.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m3 {

class State;

class Scsp {
public:
    static constexpr uint32_t kRamSize = 0x80000; // 512 KB per chip (MAME's map; Supermodel gives it 1 MB)
    static constexpr int kSampleRate = 44100;

    // `ram` is the chip's sound RAM in 68000 byte order (big-endian words),
    // shared with the sound CPU. Only the master has timers and MIDI.
    Scsp(std::span<uint8_t> ram, bool master);

    // Register file, 0x000-0xEE3 within the chip (68000 view).
    uint8_t read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    void write8(uint32_t addr, uint8_t v);
    void write16(uint32_t addr, uint16_t v);

    // One output sample of this chip: 16-bit range, or 18-bit with DAC18B
    // (see the .cpp). `ext_left`/`ext_right` are the external inputs (EXTS
    // 0/1): mixed at slots 16/17's levels, and inputs to the effect DSP.
    // Nothing is connected to them on the Model 3 sound board.
    void sample(int32_t& left, int32_t& right, int16_t ext_left = 0, int16_t ext_right = 0);
    // Advance the timers by one sample (master).
    void tick_timers();
    // The interrupt level for the sound CPU, 0 when none (master).
    int irq_level();

    // MIDI input from the main board.
    void midi_in(uint8_t byte);
    std::size_t midi_pending() const { return static_cast<uint8_t>(midi_w_ - midi_r_); }
    uint64_t midi_dropped = 0;

    int active_slots() const;
    uint16_t common_reg(uint32_t addr) const { return regs_[(addr & 0x3E) / 2]; } // 0x400-0x42F, as last written
    uint16_t slot_reg(int slot, uint32_t addr) const { return slots_[static_cast<std::size_t>(slot & 31)].data[(addr & 0x1E) / 2]; }

    void state(State& s); // save states (runtime/state.hpp)

private:
    struct Lfo {
        uint16_t phase = 0;
        uint32_t step = 0;
        const int* table = nullptr;
        const int* scale = nullptr;
    };
    enum class EgState : uint8_t { Attack, Decay1, Decay2, Release };
    struct Eg {
        int volume = 0;
        EgState state = EgState::Release;
        int ar = 0, d1r = 0, d2r = 0, rr = 0, dl = 0;
        bool hold = false;
        bool below_dl = false; // put into decay 1 by the loop start (LPSLNK) under DL: never decay 2
    };
    struct Slot {
        std::array<uint16_t, 16> data{};
        bool active = false;
        uint32_t base = 0;              // start address in RAM
        uint32_t cur = 0, nxt = 0, step = 0; // play position and pitch, fixed point
        bool back = false;
        Eg eg;
        Lfo plfo, alfo;
    };
    struct Dsp {
        uint32_t rbp = 0, rbl = 8 * 1024;
        std::array<int16_t, 64> coef{};
        std::array<uint16_t, 32> madrs{};
        std::array<uint16_t, 128 * 4> mpro{};
        std::array<int32_t, 128> temp{};
        std::array<int32_t, 32> mems{};
        uint32_t dec = 0;
        std::array<int32_t, 16> mixs{};
        std::array<int16_t, 2> exts{};
        std::array<int16_t, 16> efreg{};
        bool stopped = true;
        int last_step = 0;
    };

    void update_slot_reg(int s, uint32_t reg);
    void update_reg(uint32_t reg);
    void update_reg_read(uint32_t reg);
    void start_slot(Slot& slot);
    void stop_slot(Slot& slot, bool keyoff);
    void compute_eg(Slot& slot);
    void compute_lfo(Slot& slot);
    int eg_update(Slot& slot);
    int32_t update_slot(Slot& slot, int16_t* ring_out);
    uint8_t decode_sci(int irq) const;
    void dsp_step();
    void dsp_start();
    uint16_t ram_word(uint32_t word) const;
    void set_ram_word(uint32_t word, uint16_t v);
    void timer_reload(int t);

    std::span<uint8_t> ram_;
    bool master_;
    std::array<uint16_t, 0x18> regs_{}; // 0x400-0x42F
    std::array<Slot, 32> slots_{};
    std::array<int16_t, 64> ring_{};
    uint8_t bufptr_ = 0;
    uint32_t noise_ = 1; // noise source state (SSCTL 1)
    Dsp dsp_;

    std::array<int, 3> timer_{0xFFFF, 0xFFFF, 0xFFFF};

    uint16_t mcieb_ = 0, mcipd_ = 0;

    std::array<uint8_t, 256> midi_{};
    uint8_t midi_w_ = 0, midi_r_ = 0;
};

} // namespace m3
