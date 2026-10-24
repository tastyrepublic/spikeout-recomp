// Real3D Pro-1000 memory as the game fills it: the scene (culling RAM),
// models (polygon RAM, and VROM in the ROM set) and textures (uploaded
// through a FIFO into texture RAM). Drawing it is m3::Real3dRenderer.
//
// Formats from Supermodel (Src/Model3/Real3D.cpp, Src/Graphics/New3D) and
// its notes; this is our own code. Every word the CPU writes reaches the
// Real3D byte-swapped (the bus is little-endian on that side), so the
// Real3D's word is a little-endian read of the bytes as the CPU addresses
// them, as for the tile generator.
//
//   0x88000000  command port: a write ends a frame's uploads (flushes the
//               texture FIFO)
//   0x8C000000  low culling RAM (4 MB)       0x8E000000  high culling RAM (1 MB)
//   0x94000000  texture FIFO                 0x98000000  polygon RAM (4 MB)
//   0x9C000000  configuration registers (kept; their update-buffer meaning
//               isn't emulated: writes always go straight to memory)
//
// Culling and model addresses are in words: below 0x100000 low culling RAM
// / polygon RAM, 0x800000-0x83FFFF high culling RAM, model addresses from
// 0x100000 up are in VROM.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace m3 {

class State;

class Real3d {
public:
    static constexpr uint32_t kTextureWidth = 2048, kTextureHeight = 2048; // two 2048x1024 pages

    // `vrom`: the VROM region of the ROM set (may be empty).
    explicit Real3d(std::span<const uint8_t> vrom = {});

    // 32-bit writes from the CPU (`value` as the CPU wrote it).
    void write_culling_low(uint32_t offset, uint32_t value) { put(culling_lo_, offset, value); }
    void write_culling_high(uint32_t offset, uint32_t value);
    void write_polygon(uint32_t offset, uint32_t value);
    void write_config(uint32_t offset, uint32_t value) { config_[(offset / 4) & 3] = swap(value); }
    void write_texture_fifo(uint32_t value);

    // Frame timing. Once the ping-pong bit flips the Real3D is drawing this
    // frame's scene, and the game starts on the next: until VBlank, writes
    // to high culling RAM and polygon RAM go into update buffers in low
    // culling RAM (blocks of first word, last word, data; areas sized by
    // configuration registers 0 and 8), applied at the start of VBlank
    // (Supermodel's Real3D.cpp, UpdateBlock). Textures written after the
    // flip are stored at VBlank too.
    //
    // Two signals say a frame's data is complete (Supermodel): the command
    // port (the CPU wrote it all) and the tile generator's register 0x0C
    // (it draws a frame). When both come before the flip, or at VBlank for
    // the ones after it, the frame takes the Mercury ASIC's mode word bit 2,
    // block culling, which stops the 3D being drawn (`mercury_mode` is that
    // mode word as the JTAG chain last set it).
    void write_command(uint32_t mercury_mode); // the command port
    void tilegen_draw_frame(uint32_t mercury_mode);
    void ping_pong_flipped();
    void begin_vblank(uint32_t mercury_mode);
    bool block_culling() const { return block_culling_; }

    // Words as the Real3D sees them, by word address (0 outside memory).
    uint32_t culling(uint32_t addr) const;
    uint32_t model(uint32_t addr) const; // polygon RAM or VROM
    const uint32_t* culling_ptr(uint32_t addr) const; // nullptr outside memory
    uint16_t texel(uint32_t x, uint32_t y) const { return texture_[(y & 2047) * kTextureWidth + (x & 2047)]; }
    // All of texture RAM, 2048 x 2048 texels, row by row (for the GPU).
    std::span<const uint16_t> texture_ram() const { return texture_; }

    // Writes since the last flush (for tests and statistics).
    uint64_t textures_uploaded() const { return textures_uploaded_; }
    // The texture RAM rows written since last asked, as runs (first row,
    // rows); for a renderer that keeps a copy, to copy only those.
    std::vector<std::pair<uint32_t, uint32_t>> take_written_rows();
    uint32_t config(int reg) const { return config_[static_cast<size_t>(reg) & 3]; }

    // Store one texture as the FIFO would (header and little-endian 16-bit data).
    void upload_texture(uint32_t header, std::span<const uint16_t> data);

    // Save states (runtime/state.hpp). Loading marks all of texture RAM
    // written (take_written_rows).
    void state(State& s);

private:
    static uint32_t swap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
    static void put(std::vector<uint32_t>& mem, uint32_t offset, uint32_t value) {
        if (offset / 4 < mem.size()) mem[offset / 4] = swap(value);
    }
    void store(uint32_t x, uint32_t y, uint32_t width, uint32_t height, std::span<const uint16_t> data, size_t& used,
               bool sixteen_bit, bool low_byte, bool high_byte);

    // An update buffer: [begin, end) words of low culling RAM, blocks from
    // `begin` to `cursor`, the one being extended at `block`.
    struct UpdateBuffer {
        uint32_t cursor = 0, block = UINT32_MAX;
        bool used = false;
    };
    bool buffering() const { return flipped_ && config_[2] > config_[0]; }
    void buffer_range(bool polygon, uint32_t& begin, uint32_t& end) const;
    void buffered_write(bool polygon, std::vector<uint32_t>& target, uint32_t word, uint32_t value);
    void apply(bool polygon, std::vector<uint32_t>& target);
    void flush_textures();
    void draw_frame(uint32_t mercury_mode);

    std::span<const uint8_t> vrom_;
    std::vector<uint32_t> culling_lo_, culling_hi_, polygon_;
    UpdateBuffer high_buffer_, polygon_buffer_;
    bool flipped_ = false, command_written_ = false, tilegen_drew_ = false, block_culling_ = false;
    std::array<uint32_t, 4> config_{};
    std::vector<uint32_t> fifo_;
    std::vector<uint16_t> texture_;
    uint64_t textures_uploaded_ = 0;
    std::vector<uint8_t> rows_written_ = std::vector<uint8_t>(kTextureHeight, 0); // take_written_rows
};

} // namespace m3
