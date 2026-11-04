#include "runtime/tilegen.hpp"

#include "runtime/bus.hpp"
#include "runtime/tile_sides.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

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
    bottom_own_.assign(side, 0u);
    top_own_.assign(side, 0u);
    bottom_real_.assign(extend_ > 0 ? kHeight : 0, 0);
    top_real_.assign(extend_ > 0 ? kHeight : 0, 0);
}

void TilegenRenderer::draw_line(const Bus& bus, int line) {
    draw_line(bus.tile_ram(), bus.tile_regs(), line, &bus.tile_sides());
}

void TilegenRenderer::draw_line(std::span<const uint8_t> ram, const std::array<uint32_t, 64>& regs, int line,
                                const TileSides* sides) {
    if (line < 0 || line >= kHeight) return;
    auto word = [&](uint32_t index) -> uint32_t { // the tile generator's 32-bit word `index`
        const std::size_t at = std::size_t{index} * 4;
        if (at + 3 >= ram.size()) return 0;
        return uint32_t{ram[at]} | uint32_t{ram[at + 1]} << 8 | uint32_t{ram[at + 2]} << 16 | uint32_t{ram[at + 3]} << 24;
    };
    const uint32_t control = regs[0x20 / 4];
    const uint32_t y = static_cast<uint32_t>(line);
    const std::size_t row = std::size_t{y} * kWidth;
    if (extend_ > 0 && sides && sides->black()) { // the staff credits (m3::TileSides::black): WideLayer's own sides
        bottom_real_[y] |= 0x20;
        top_real_[y] |= 0x20;
    }

    // Layers 2/3 first, then 0/1 over them; in each pair the line mask picks
    // the primary (even) or alternate (odd) layer per 32 pixels.
    // (a palette word's colour, with the pair's offset: each 5-bit channel's 8 bits from a table made once a pair,
    // the same as channel() each time; the palette read straight, the tile RAM holding it whole)
    const uint8_t* palette = ram.size() >= kPalette + 0x8000 * 4 ? ram.data() + kPalette : nullptr;
    for (int pair = 1; pair >= 0; --pair) {
        const Offset offset = colour_offset(regs[(0x40 + 4 * static_cast<uint32_t>(pair)) / 4]);
        uint32_t to_r[32], to_g[32], to_b[32];
        for (uint32_t c = 0; c < 32; ++c)
            to_r[c] = channel(c, offset.r) << 16, to_g[c] = channel(c, offset.g) << 8, to_b[c] = channel(c, offset.b);
        auto colour_of = [&](uint32_t index) -> uint32_t { // (a palette entry: 15 bits)
            if (!palette || std::endian::native != std::endian::little) return word(kPalette / 4 + index);
            uint32_t w;
            std::memcpy(&w, palette + std::size_t{index} * 4, 4);
            return w; // (the tile generator's words little-endian, as word() reads them)
        };
        const uint32_t mask = (word(kLineMask / 4 + y) >> (pair == 0 ? 16 : 0)) & 0xFFFF;

        struct Layer {
            bool enabled, above, four_bit;
            uint32_t sx, sy, names;
            bool loops;      // widescreen: drawn past the screen on this line
            uint32_t period; // and wrapping there every this many pixels
            int edge[2];     // else the map column whose tile carries on past the left, right edge (-1: none)
            int index;       // the layer (0-3)
            bool own[2];     // ahead of those, the game's own columns past the left, right edge (m3::TileSides)
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
            if (extend_ > 0 && y == 0) { // the frames since the x scroll changed (a layer scrolled up or down
                                         // only doesn't wrap at the sides: the staff credits' names)
                const uint32_t xs = scroll & 0x3FFu;
                scroll_still_[n] = xs == scroll_seen_[n] ? std::min(scroll_still_[n] + 1, 1000u) : 0;
                scroll_seen_[n] = xs;
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
            bool own[2] = {false, false};
            if (extend_ > 0 && sides && !(scroll & 0x8000) && (sx & 511) == 0) {
                const int map_row = static_cast<int>(((y + ((scroll >> 16) & 0x1FF)) & 511) / 8);
                const int shown = (extend_ + 7) / 8; // (the columns this width shows: a narrower one's rows as they were)
                own[0] = sides->any(static_cast<int>(n), map_row, false, shown);
                own[1] = sides->any(static_cast<int>(n), map_row, true, shown);
            }
            // A still full-screen picture (m3::TileSides::picture; not scrolled: then none of its rows
            // is a panorama, though some rows of a card look like one): WideLayer makes its sides from it, blurred.
            if (extend_ > 0 && sides && (scroll & 0x80000000u) && (sx & 511) == 0 && !(scroll & 0x8000) &&
                sides->picture(static_cast<int>(n)))
                (((control >> (8 + n)) & 1) ? top_real_ : bottom_real_)[y] |= 0x10;
            layers[k] = {(scroll & 0x80000000u) != 0, ((control >> (8 + n)) & 1) != 0, ((control >> (12 + n)) & 1) != 0,
                         sx, (scroll >> 16) & 0x1FF, names, loops, period, {edge[0], edge[1]}, static_cast<int>(n),
                         {own[0], own[1]}};
        }
        if (!layers[0].enabled && !layers[1].enabled) continue;

        // Spans that share a layer (the mask has a bit per 32 screen pixels)
        // and a tile row (8 pixels of the scrolled layer). Widescreen: from
        // `extend` pixels left of the screen to as far right of it, where the
        // mask and name table wrap at 512 (drawn only from looping lines).
        // A second pass (widescreen) draws the sides again from the game's own columns past the
        // screen (m3::TileSides) where a map row has some, into bottom_own()/top_own(): the first's
        // left as it always was.
        const int first = -extend_, last = kWidth + extend_;
        for (int pass = 0; pass < (extend_ > 0 && sides ? 2 : 1); ++pass)
        for (int sx = first; sx < last;) {
            const bool side = sx < 0 || sx >= kWidth;
            if (pass == 1 && !side) { // (the sides alone)
                sx = kWidth;
                continue;
            }
            const uint32_t x = static_cast<uint32_t>(sx) & 511;
            const int stop = sx < 0 ? 0 : sx < kWidth ? kWidth : last;
            const uint32_t mask_end =
                x + static_cast<uint32_t>(std::min<int>(static_cast<int>((x / 32 + 1) * 32 - x), stop - sx));
            const Layer& l = layers[(mask >> (15 - x / 32)) & 1 ? 0 : 1];
            const int edge = l.edge[sx < 0 ? 0 : 1];
            // The game's own columns past the screen (x scroll 0), from the sides kept.
            // The other layers' sides are drawn in that pass too, as in the first, so that a
            // layer in front of one with its own columns stays in front (a marquee over a picture).
            const bool own = pass == 1 && l.enabled && l.own[sx < 0 ? 0 : 1];
            if (!l.enabled || (side && !own && !l.loops && edge < 0)) {
                sx += static_cast<int>(mask_end - x);
                continue;
            }
            uint32_t px = (x + l.sx) & 511;
            if (side && !own && l.loops && l.period < 512) { // where the screen's pixel sx is along the panorama
                const int along = (sx + static_cast<int>(l.sx)) % static_cast<int>(l.period);
                px = static_cast<uint32_t>(along < 0 ? along + static_cast<int>(l.period) : along);
            } else if (side && !own && !l.loops) { // the edge's tile, in step with the screen's
                px = static_cast<uint32_t>(edge * 8) | (static_cast<uint32_t>(sx + static_cast<int>(l.sx)) & 7);
            }
            const uint32_t py = (y + l.sy) & 511;
            const uint32_t end = std::min(mask_end, x + (8 - (px & 7)));
            uint32_t entry = 0;
            if (own) {
                entry = sides->entry(l.index, static_cast<int>(py / 8), sx >> 3);
                if (entry == TileSides::kEmpty) { // nothing there: the line's side left to WideLayer
                    sx += static_cast<int>(end - x);
                    continue;
                }
                (l.above ? top_real_ : bottom_real_)[y] |= static_cast<uint8_t>(sx < 0 ? 1 : 2);
                if (entry == TileSides::kFade) { // the picture's fade, continued by WideLayer under what's drawn here
                    (l.above ? top_real_ : bottom_real_)[y] |= static_cast<uint8_t>(sx < 0 ? 4 : 8);
                    sx += static_cast<int>(end - x);
                    continue;
                }
            } else {
                const uint32_t names = word(l.names + (py / 8) * 32 + px / 16);
                entry = (px & 8) ? names & 0xFFFF : names >> 16;
            }
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
                out = (pass == 1 ? (l.above ? top_own_ : bottom_own_) : (l.above ? top_side_ : bottom_side_)).data() +
                      static_cast<std::size_t>(2 * extend_) * y + (sx < 0 ? sx + extend_ : sx - kWidth + extend_);
            const uint32_t n = end - x;
            for (uint32_t i = 0, fx = px & 7; i < n; ++i, ++fx) {
                const uint32_t colour = colour_of(base | pixels[fx]);
                if (colour & 0x8000) continue; // transparent
                out[i] = 0xFF000000u | to_r[colour & 0x1F] | to_g[(colour >> 5) & 0x1F] | to_b[(colour >> 10) & 0x1F];
            }
            sx += static_cast<int>(n);
        }
    }
}

bool TilegenRenderer::find_move(std::span<const uint32_t> before, std::span<const uint32_t> now, uint32_t scroll_before,
                                uint32_t scroll_now, int& dx, int& dy) {
    dx = dy = 0;
    const auto size = static_cast<std::size_t>(kWidth) * kHeight;
    if (before.size() < size || now.size() < size) return false;
    // The registers' change, within (-8, 8): the screen moves the other way.
    auto change = [](int a, int b, int bits) {
        const int m = 1 << bits;
        int d = ((b - a) % m + m) % m;
        if (d >= m / 2) d -= m;
        return d;
    };
    const int fine_x = -change(static_cast<int>(scroll_before & 0x3FF), static_cast<int>(scroll_now & 0x3FF), 10);
    const int fine_y = -change(static_cast<int>((scroll_before >> 16) & 0x1FF), static_cast<int>((scroll_now >> 16) & 0x1FF), 9);
    double best = 1e9;
    int count_best = 0;
    for (int ky = -4; ky <= 4; ++ky)
        for (int kx = -4; kx <= 4; ++kx) {
            const int mx = fine_x + 8 * kx, my = fine_y + 8 * ky;
            if (std::abs(mx) > 40 || std::abs(my) > 40) continue;
            long total = 0;
            int count = 0;
            for (int y = 40; y < kHeight - 40; y += 6) // (every 6th line, 4th pixel, away from the edges)
                for (int x = 40; x < kWidth - 40; x += 4) {
                    const uint32_t a = before[static_cast<std::size_t>((y - my) * kWidth + (x - mx))];
                    const uint32_t b = now[static_cast<std::size_t>(y * kWidth + x)];
                    if (!(a >> 24) || !(b >> 24)) continue;
                    int d = 0;
                    for (int s = 0; s < 24; s += 8)
                        d = std::max(d, std::abs(static_cast<int>((a >> s) & 0xFF) - static_cast<int>((b >> s) & 0xFF)));
                    total += d;
                    ++count;
                }
            if (count < 200) continue; // (too little of it: no sky)
            const double error = static_cast<double>(total) / count;
            if (error < best) {
                best = error;
                count_best = count;
                dx = mx;
                dy = my;
            }
        }
    if (count_best == 0 || best > 12) { // nothing lines up: no move
        dx = dy = 0;
        return false;
    }
    return true;
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
