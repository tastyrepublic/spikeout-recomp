// Tile generator (2D layers) renderer: draws the four 64x64 tile layers
// from Tilegen RAM and registers into two 496x384 images, one below the 3D
// picture and one above it, a display line at a time.
//
// After Supermodel's TileGen.cpp (DrawLine and its helpers) and the format
// notes in its Render2D.cpp. The game writes Tilegen RAM little-endian
// (stwbrx), so the tile generator's 32-bit word i is a little-endian read of
// bytes 4i..4i+3 as the CPU addresses them:
//   0x00000-0xF5FFF  tile patterns; 0xF6000 + 0x400 * layer: line scroll
//   (16 bits per line, even lines in the high half); 0xF7000: line masks
//   (one word per line; high half layers 0/1, low half 2/3, a set bit
//   shows the primary layer for 32 pixels); 0xF8000 + 0x2000 * layer: name
//   tables (64x64 entries, a word holds two: the left one in the high half);
//   0x100000: palette (32768 words, low 16 bits tbbbbbgggggrrrrr, t =
//   transparent).
// Registers (as the tile generator sees them): 0x20 bits 8-11 layer above
// the 3D, bits 12-15 4-bit tiles; 0x40/0x44 colour offset for layers 0/1
// and 2/3 (signed 8-bit r, g, b, times 2); 0x60 + 4 * layer: enable (bit
// 31), y scroll (bits 16-24), line scroll on (bit 15), x scroll (bits 0-9).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace m3 {

class Bus;

class TilegenRenderer {
public:
    static constexpr int kWidth = 496, kHeight = 384;

    TilegenRenderer();

    // Clears both images (transparent).
    void begin_frame();
    // Draws display line `line` (0-383) from the current RAM and registers.
    void draw_line(std::span<const uint8_t> ram, const std::array<uint32_t, 64>& regs, int line);
    void draw_line(const Bus& bus, int line);

    // Pixels as 0xAARRGGBB; alpha 0 where nothing was drawn.
    std::span<const uint32_t> bottom() const { return bottom_; }
    std::span<const uint32_t> top() const { return top_; }
    // Black, then the layers below the 3D, the 3D picture (`three_d`, same
    // size, alpha 0 where empty; may be empty), then the layers above.
    void composite(std::span<uint32_t> out, std::span<const uint32_t> three_d = {}) const {
        composite(out, bottom_, top_, three_d);
    }
    static void composite(std::span<uint32_t> out, std::span<const uint32_t> bottom, std::span<const uint32_t> top,
                          std::span<const uint32_t> three_d);

private:
    std::vector<uint32_t> bottom_, top_;
};

} // namespace m3
