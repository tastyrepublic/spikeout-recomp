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
class TileSides;

class TilegenRenderer {
public:
    static constexpr int kWidth = 496, kHeight = 384;

    TilegenRenderer();

    // Clears both images (transparent).
    void begin_frame();
    // Draws display line `line` (0-383) from the current RAM and registers
    // (widescreen: and `sides`, the game's own columns past the screen).
    void draw_line(std::span<const uint8_t> ram, const std::array<uint32_t, 64>& regs, int line,
                   const TileSides* sides = nullptr);
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

    // Widescreen: also draws `extend` pixels past each side of the screen,
    // into bottom_side() and top_side() (each line: the left `extend`
    // pixels, then the right), from the name tables, which wrap at 512: on
    // lines where a layer is scrolled by 8 to 504 pixels (a picture panning
    // across its name table, the map screen's faces), or scrolled apart from
    // most of its lines (a band scrolling through a still picture: the
    // character select's marquee), or (without line scroll) whose scroll
    // register changed in the last 120 frames, or (always) whose map row is
    // a panorama one screen wide: the stages' skies are runs of 62 tiles
    // written round the map's 64 (its last two columns the first two again;
    // scrolled 0-7 as the camera turns while the game rewrites the tiles),
    // and wrap every 496 pixels, not 512. A layer that never scrolls is
    // otherwise a picture the size of the screen (the title, the character
    // select): past each side of it, where the tile at the screen's edge is
    // the row's background (at least 4 of the screen's 62 columns: plain
    // bands, the orange round the title's logo, dithered as it is), that
    // tile is carried on exactly, where an edge colour would be its average.
    // Apart from all that, on a layer that isn't scrolled across (x scroll 0,
    // no line scroll), a map row that has the game's own columns past the
    // screen (m3::TileSides: the title's doors) is drawn from them on that
    // side into bottom_own() and top_own() (laid out as the sides; nothing
    // where there are none), such lines marked in bottom_real() and
    // top_real() (bit 0 left, bit 1 right), for m3::WideLayer to put over
    // its sides there. The other layers' sides are drawn there too, in
    // front of or behind as on the screen. A fade past a picture's edge
    // (TileSides::kFade) draws nothing there and is marked too (bit 2 left,
    // bit 3 right): WideLayer continues the fade under what was drawn.
    // Takes effect at begin_frame.
    void set_extend(int extend) { next_extend_ = extend; }
    int extend() const { return extend_; }
    std::span<const uint32_t> bottom_side() const { return bottom_side_; }
    std::span<const uint32_t> top_side() const { return top_side_; }
    std::span<const uint32_t> bottom_own() const { return bottom_own_; }
    std::span<const uint32_t> top_own() const { return top_own_; }
    std::span<const uint8_t> bottom_real() const { return bottom_real_; }
    std::span<const uint8_t> top_real() const { return top_real_; }

    // Smooth motion: how far the picture below the 3D (`before`, then `now`,
    // 496x384 each) moved on screen in a frame, now(x, y) = before(x - dx,
    // y - dy), as one layer's scroll register went from `scroll_before` to
    // `scroll_now` (the stages' skies, layer 3: the register holds the move
    // within a tile, the game moving it on by whole tiles as it rewrites the
    // name table; after vf3-recomp's TileGen::scrolled). The move is the
    // register's change plus or minus whole tiles (up to 4), the one that
    // lines the two up best; false if none does (a cut, another scene).
    static bool find_move(std::span<const uint32_t> before, std::span<const uint32_t> now, uint32_t scroll_before,
                          uint32_t scroll_now, int& dx, int& dy);

    // For looking at: layer `layer`'s whole 512x512 name table, unscrolled,
    // into `out` (512 * 512 pixels; alpha 0 where transparent).
    static void draw_map(std::span<const uint8_t> ram, const std::array<uint32_t, 64>& regs, int layer,
                         std::span<uint32_t> out);

private:
    std::vector<uint32_t> bottom_, top_;
    int extend_ = 0, next_extend_ = 0;
    std::vector<uint32_t> bottom_side_, top_side_;
    std::vector<uint32_t> bottom_own_, top_own_;
    std::vector<uint8_t> bottom_real_, top_real_;
    // Each layer's scroll register as last seen, and the frames since it changed.
    std::array<uint32_t, 4> scroll_seen_{};
    std::array<uint32_t, 4> scroll_still_{1000, 1000, 1000, 1000};
    // Each layer's last tile row looked at for the sides (this frame): its
    // row and scroll, whether it wraps as a panorama, and its edge tiles.
    struct RowLook {
        uint32_t key = 0, period = 512;
        int edge[2] = {-1, -1};
    };
    std::array<RowLook, 4> row_look_{};
};

} // namespace m3
