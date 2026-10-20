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
        } layers[2];
        for (int k = 0; k < 2; ++k) {
            const uint32_t n = static_cast<uint32_t>(pair * 2 + k);
            const uint32_t scroll = regs[0x60 / 4 + n];
            uint32_t sx = scroll & 0x3FF;
            if (scroll & 0x8000) { // a scroll value per line
                const uint32_t w = word((kLineScroll + n * 0x400) / 4 + y / 2);
                sx = (y & 1) ? w & 0xFFFF : w >> 16;
            }
            layers[k] = {(scroll & 0x80000000u) != 0, ((control >> (8 + n)) & 1) != 0, ((control >> (12 + n)) & 1) != 0,
                         sx, (scroll >> 16) & 0x1FF, (kNameTables + n * 0x2000) / 4};
        }
        if (!layers[0].enabled && !layers[1].enabled) continue;

        // Spans that share a layer (the mask has a bit per 32 screen pixels)
        // and a tile row (8 pixels of the scrolled layer).
        for (uint32_t x = 0; x < kWidth;) {
            const uint32_t mask_end = std::min<uint32_t>((x / 32 + 1) * 32, kWidth);
            const Layer& l = layers[(mask >> (15 - x / 32)) & 1 ? 0 : 1];
            if (!l.enabled) {
                x = mask_end;
                continue;
            }
            const uint32_t px = (x + l.sx) & 511, py = (y + l.sy) & 511;
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
            uint32_t* out = (l.above ? top_ : bottom_).data() + row;
            for (uint32_t fx = px & 7; x < end; ++x, ++fx) {
                const uint32_t colour = word(kPalette / 4 + (base | pixels[fx]));
                if (colour & 0x8000) continue; // transparent
                out[x] = 0xFF000000u | channel(colour & 0x1F, offset.r) << 16 | channel((colour >> 5) & 0x1F, offset.g) << 8 |
                         channel((colour >> 10) & 0x1F, offset.b);
            }
        }
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
