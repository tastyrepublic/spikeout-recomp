#include "runtime/tilegen.hpp"

#include "runtime/bus.hpp"

#include <algorithm>

namespace m3 {

namespace {

constexpr uint32_t kLineScroll = 0xF6000, kLineMask = 0xF7000, kNameTables = 0xF8000, kPalette = 0x100000;

struct Offset {
    int r, g, b;
};

// A 5-bit channel scaled to 8 bits, plus the colour offset, clamped.
uint32_t channel(uint32_t c5, int add) {
    return static_cast<uint32_t>(std::clamp(static_cast<int>(c5 * 255 / 31) + add, 0, 255));
}

Offset colour_offset(uint32_t reg) { // signed 8 bits each, doubled
    auto s8 = [](uint32_t v) { return static_cast<int>(static_cast<int8_t>(static_cast<uint8_t>(v))) * 2; };
    return {s8(reg), s8(reg >> 8), s8(reg >> 16)};
}

} // namespace

TilegenRenderer::TilegenRenderer() : bottom_(kWidth * kHeight), top_(kWidth * kHeight) {}

void TilegenRenderer::begin_frame() {
    std::fill(bottom_.begin(), bottom_.end(), 0u);
    std::fill(top_.begin(), top_.end(), 0u);
    extend_ = next_extend_;
    row_look_ = {};
    const auto side = static_cast<std::size_t>(2 * extend_ * kHeight);
    bottom_side_.assign(side, 0u);
    top_side_.assign(side, 0u);
}

void TilegenRenderer::draw_line(const Bus& bus, int line) { draw_line(bus.tile_ram(), bus.tile_regs(), line); }

void TilegenRenderer::draw_line(std::span<const uint8_t> ram, const std::array<uint32_t, 64>& regs, int line) {
    if (line < 0 || line >= kHeight) return;
    auto word = [&](uint32_t index) -> uint32_t { // the tile generator's 32-bit word `index`
        const std::size_t at = std::size_t{index} * 4;
        if (at + 3 >= ram.size()) return 0;
        return uint32_t{ram[at]} | uint32_t{ram[at + 1]} << 8 | uint32_t{ram[at + 2]} << 16 | uint32_t{ram[at + 3]} << 24;
    };
    const uint32_t control = regs[0x20 / 4];
    const uint32_t y = static_cast<uint32_t>(line);
    const std::size_t row = std::size_t{y} * kWidth;

    // Layers 2/3 first, then 0/1 over them; in each pair the line mask picks
    // the primary (even) or alternate (odd) layer per 32 pixels.
    for (int pair = 1; pair >= 0; --pair) {
        const Offset offset = colour_offset(regs[(0x40 + 4 * static_cast<uint32_t>(pair)) / 4]);
        const uint32_t mask = (word(kLineMask / 4 + y) >> (pair == 0 ? 16 : 0)) & 0xFFFF;

        struct Layer {
            bool enabled, above, four_bit;
            uint32_t sx, sy, names;
            bool loops;      // widescreen: drawn past the screen on this line
            uint32_t period; // and wrapping there every this many pixels
            int edge[2];     // else the map column whose tile carries on past the left, right edge (-1: none)
        } layers[2];
        for (int k = 0; k < 2; ++k) {
            const uint32_t n = static_cast<uint32_t>(pair * 2 + k);
            const uint32_t scroll = regs[0x60 / 4 + n];
            uint32_t sx = scroll & 0x3FF;
            bool loops = false;
            if (scroll & 0x8000) { // a scroll value per line
                auto line_scroll = [&](uint32_t at) {
                    const uint32_t w = word((kLineScroll + n * 0x400) / 4 + at / 2);
                    return (at & 1) ? w & 0xFFFF : w >> 16;
                };
                sx = line_scroll(y);
                if (extend_ > 0) { // scrolled apart from the most of the layer's lines (24 looked at)
                    int same = 0;
                    for (uint32_t at = 0; at < kHeight; at += 16) same += ((line_scroll(at) ^ sx) & 511) == 0;
                    loops = same < 12;
                }
            }
            if ((sx & 511) >= 8 && (sx & 511) <= 504) loops = true; // a picture panning across its name table
            if (extend_ > 0 && y == 0) { // the frames since the scroll (x and y) changed
                const uint32_t xy = scroll & 0x01FF03FFu;
                scroll_still_[n] = xy == scroll_seen_[n] ? std::min(scroll_still_[n] + 1, 1000u) : 0;
                scroll_seen_[n] = xy;
            }
            if (!(scroll & 0x8000) && scroll_still_[n] < 120) loops = true;
            // A panorama one screen wide (the stages' skies): its map rows are
            // runs of 62 tiles written round the map's 64, so the last two
            // columns repeat the first two (different tiles: not a picture's
            // plain margins, the title's); it wraps every 62, not 64.
            uint32_t period = 512;
            int edge[2] = {-1, -1};
            const uint32_t names = (kNameTables + n * 0x2000) / 4;
            if (extend_ > 0) {
                // (worked out once a tile row: the 8 lines of one share it)
                const uint32_t map_row = ((y + ((scroll >> 16) & 0x1FF)) & 511) / 8;
                RowLook& look = row_look_[n];
                const uint32_t key = map_row | (sx & 511) << 6 | 1u << 31;
                if (look.key != key) {
                    look.key = key;
                    std::array<uint32_t, 64> entry;
                    for (uint32_t c = 0; c < 64; c += 2) {
                        const uint32_t w = word(names + map_row * 32 + c / 2);
                        entry[c] = w >> 16;
                        entry[c + 1] = w & 0xFFFF;
                    }
                    look.period = entry[0] == entry[62] && entry[1] == entry[63] && entry[0] != entry[1] ? 62 * 8 : 512;
                    // Else, each side: where the tile at the screen's edge is the
                    // row's background, at least 4 of the screen's 62 columns (a
                    // plain band, the orange round the title's logo, dithered: two
                    // shades a pixel apart, which an edge colour would average),
                    // that tile carried on (a photo's tiles are each different).
                    for (int s = 0; s < 2; ++s) {
                        const uint32_t first_col = (sx & 511) / 8, col = (((s ? kWidth - 1 : 0) + sx) & 511) / 8;
                        int count = 0;
                        for (uint32_t c = 0; c < 62; ++c) count += entry[(first_col + c) % 64] == entry[col];
                        look.edge[s] = count >= 4 ? static_cast<int>(col) : -1;
                    }
                }
                period = look.period;
                if (period < 512) loops = true;
                if (!loops) {
                    edge[0] = look.edge[0];
                    edge[1] = look.edge[1];
                }
            }
            layers[k] = {(scroll & 0x80000000u) != 0, ((control >> (8 + n)) & 1) != 0, ((control >> (12 + n)) & 1) != 0,
                         sx, (scroll >> 16) & 0x1FF, names, loops, period, {edge[0], edge[1]}};
        }
        if (!layers[0].enabled && !layers[1].enabled) continue;

        // Spans that share a layer (the mask has a bit per 32 screen pixels)
        // and a tile row (8 pixels of the scrolled layer). Widescreen: from
        // `extend` pixels left of the screen to as far right of it, where the
        // mask and name table wrap at 512 (drawn only from looping lines).
        const int first = -extend_, last = kWidth + extend_;
        for (int sx = first; sx < last;) {
            const bool side = sx < 0 || sx >= kWidth;
            const uint32_t x = static_cast<uint32_t>(sx) & 511;
            const int stop = sx < 0 ? 0 : sx < kWidth ? kWidth : last;
            const uint32_t mask_end =
                x + static_cast<uint32_t>(std::min<int>(static_cast<int>((x / 32 + 1) * 32 - x), stop - sx));
            const Layer& l = layers[(mask >> (15 - x / 32)) & 1 ? 0 : 1];
            const int edge = l.edge[sx < 0 ? 0 : 1];
            if (!l.enabled || (side && !l.loops && edge < 0)) {
                sx += static_cast<int>(mask_end - x);
                continue;
            }
            uint32_t px = (x + l.sx) & 511;
            if (side && l.loops && l.period < 512) { // where the screen's pixel sx is along the panorama
                const int along = (sx + static_cast<int>(l.sx)) % static_cast<int>(l.period);
                px = static_cast<uint32_t>(along < 0 ? along + static_cast<int>(l.period) : along);
            } else if (side && !l.loops) { // the edge's tile, in step with the screen's
                px = static_cast<uint32_t>(edge * 8) | (static_cast<uint32_t>(sx + static_cast<int>(l.sx)) & 7);
            }
            const uint32_t py = (y + l.sy) & 511;
            const uint32_t end = std::min(mask_end, x + (8 - (px & 7)));
            const uint32_t names = word(l.names + (py / 8) * 32 + px / 16);
            const uint32_t entry = (px & 8) ? names & 0xFFFF : names >> 16;
            const uint32_t fy = py & 7;
            // The tile's row as 8 pixel values, left first.
            uint32_t base;
            uint8_t pixels[8];
            if (l.four_bit) { // 32-byte patterns; the entry's top bit is the pattern number's lowest
                const uint32_t pattern = (entry & 0x3FFF) << 1 | (entry >> 15);
                const uint32_t w = word(pattern * 8 + fy);
                for (int i = 0; i < 8; ++i) pixels[i] = static_cast<uint8_t>((w >> ((7 - i) * 4)) & 0xF);
                base = entry & 0x7FF0;
            } else { // 64-byte patterns
                const uint32_t pattern = entry & 0x3FFF;
                const uint32_t w0 = word(pattern * 16 + fy * 2), w1 = word(pattern * 16 + fy * 2 + 1);
                for (int i = 0; i < 4; ++i) {
                    pixels[i] = static_cast<uint8_t>(w0 >> ((3 - i) * 8));
                    pixels[i + 4] = static_cast<uint8_t>(w1 >> ((3 - i) * 8));
                }
                base = entry & 0x7F00;
            }
            uint32_t* out;
            if (!side)
                out = (l.above ? top_ : bottom_).data() + row + sx;
            else // this line's left pixels, then its right ones
                out = (l.above ? top_side_ : bottom_side_).data() + static_cast<std::size_t>(2 * extend_) * y +
                      (sx < 0 ? sx + extend_ : sx - kWidth + extend_);
            const uint32_t n = end - x;
            for (uint32_t i = 0, fx = px & 7; i < n; ++i, ++fx) {
                const uint32_t colour = word(kPalette / 4 + (base | pixels[fx]));
                if (colour & 0x8000) continue; // transparent
                out[i] = 0xFF000000u | channel(colour & 0x1F, offset.r) << 16 | channel((colour >> 5) & 0x1F, offset.g) << 8 |
                         channel((colour >> 10) & 0x1F, offset.b);
            }
            sx += static_cast<int>(n);
        }
    }
}

void TilegenRenderer::draw_map(std::span<const uint8_t> ram, const std::array<uint32_t, 64>& regs, int layer,
                               std::span<uint32_t> out) {
    auto word = [&](uint32_t index) -> uint32_t {
        const std::size_t at = std::size_t{index} * 4;
        if (at + 3 >= ram.size()) return 0;
        return uint32_t{ram[at]} | uint32_t{ram[at + 1]} << 8 | uint32_t{ram[at + 2]} << 16 | uint32_t{ram[at + 3]} << 24;
    };
    const uint32_t n = static_cast<uint32_t>(layer);
    const bool four_bit = ((regs[0x20 / 4] >> (12 + n)) & 1) != 0;
    const Offset offset = colour_offset(regs[(0x40 + 4 * (n / 2)) / 4]);
    const uint32_t names = (kNameTables + n * 0x2000) / 4;
    for (uint32_t py = 0; py < 512; ++py)
        for (uint32_t px = 0; px < 512; ++px) {
            const uint32_t pair = word(names + (py / 8) * 32 + px / 16);
            const uint32_t entry = (px & 8) ? pair & 0xFFFF : pair >> 16;
            const uint32_t fy = py & 7, fx = px & 7;
            uint32_t index;
            if (four_bit) {
                const uint32_t pattern = (entry & 0x3FFF) << 1 | (entry >> 15);
                index = (entry & 0x7FF0) | ((word(pattern * 8 + fy) >> ((7 - fx) * 4)) & 0xF);
            } else {
                const uint32_t pattern = entry & 0x3FFF;
                index = (entry & 0x7F00) | ((word(pattern * 16 + fy * 2 + fx / 4) >> ((3 - fx % 4) * 8)) & 0xFF);
            }
            const uint32_t colour = word(kPalette / 4 + index);
            out[py * 512 + px] = (colour & 0x8000) ? 0u
                                                    : 0xFF000000u | channel(colour & 0x1F, offset.r) << 16 |
                                                          channel((colour >> 5) & 0x1F, offset.g) << 8 |
                                                          channel((colour >> 10) & 0x1F, offset.b);
        }
}

void TilegenRenderer::composite(std::span<uint32_t> out, std::span<const uint32_t> bottom, std::span<const uint32_t> top,
                                std::span<const uint32_t> three_d) {
    const std::size_t n = std::min(out.size(), bottom.size());
    const bool has_3d = three_d.size() >= n;
    for (std::size_t i = 0; i < n; ++i) {
        uint32_t p = 0xFF000000u;
        if (bottom[i]) p = bottom[i];
        if (has_3d) {
            const uint32_t q = three_d[i];
            const uint32_t a = q >> 24;
            if (a == 0xFF) {
                p = q;
            } else if (a) { // translucent 3D over the layers below
                auto mix = [&](int shift) {
                    const uint32_t s = (q >> shift) & 0xFF, d = (p >> shift) & 0xFF;
                    return ((s * a + d * (255 - a)) / 255) << shift;
                };
                p = 0xFF000000u | mix(16) | mix(8) | mix(0);
            }
        }
        if (top[i]) p = top[i];
        out[i] = p;
    }
}

} // namespace m3
