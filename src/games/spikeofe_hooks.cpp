// Spikeout's tile layers and widescreen (m3::TileSides). The game keeps
// each tile layer in a buffer in work RAM, 64 16-bit entries a row,
// described by a block (the layer's descriptor):
//
//   +8   columns (64)        +10  rows
//   +16  the buffer          +20  the tile generator's name table it is copied to
//   +38  flags (bit 0: changed)
//
// Three routines write the buffers (found with gamerun --watch-write):
//
//   0x948c  draws a tile map from ROM (its header at 0xFFD00004 + id * 4,
//           or 0xFFC00000 + id * 4: +4 width, +6 height, the entries
//           from +8), or a part of it, with r6 = y << 16 | x in tiles
//           (either may be negative), clipping it at the layer's rows and
//           columns (blocks 0x9640-0x96a8). Each entry's top bits are given
//           through a table in work RAM (tile banks given out at run time):
//           entry = (src & 0x7f) | (table[(src >> 6) & 0xfe] & 0xff80).
//           With flag 0x8000 it fills the rectangle with the blank tile
//           (0x20) instead, through 0x7e34.
//   0x7e34  fills w x h entries from byte offset r11 (y << 7 | x << 1) with r6.
//   0x8200  copies a layer's changed entries to its name table (bit 15 of
//           an entry in the buffer: copied; bit 14 there becomes bit 7).
//
// The hooks: 0x9628, in the draw routine with everything worked out but
// nothing clipped yet (r25 the entries less 2 of the part of the map drawn,
// its top left corner r10/r9 in the map, r26 x r27 its size, r28 the map's
// width, r6/r5 where it goes, r19 the layer, r29 the table, r18 the flags,
// r23 the map); 0x7e34, the fill's entry; 0x8230, the copy, r16 the layer.
// The title's doors are one map (0x53, the whole title, 62 x 48), each door
// a part of it drawn at the screen's edge, wider as it closes; the title's
// strips after the attract sequence are parts of it too.
#include "games/spikeofe_hooks.hpp"

#include "recomp/context.hpp"
#include "runtime/tile_sides.hpp"

#include <algorithm>

namespace m3::games {

namespace {

constexpr uint32_t kDrawHook = 0x9628, kFillHook = 0x7E34, kCopyHook = 0x8230;
constexpr uint32_t kNameTables = 0xF10F8000, kNameTableSize = 0x2000;
constexpr uint16_t kBlank = 0x20; // the tile the clear flag fills with
// The title (map 0x53) and its wall's repeat past the map's edge: the
// rivets are about 50 pixels apart, and 31 columns on (a door's width, half
// the title) keeps that spacing across the join (49 and 48 pixels) and
// joins its photo best of the spacings that do (measured on the still
// title's layer: an average step of 19.7 and 19.1 across the joins, 14
// between neighbouring columns within the photo).
constexpr uint16_t kTitleMap = 0x53;
constexpr int32_t kTitleWallRepeat = 31;

// The tile layer a descriptor is copied to (-1: none known); its columns and rows.
int layer_of(recomp::Context& c, uint32_t desc, int& columns, int& rows) {
    const uint32_t table = c.bus.read32(desc + 20);
    columns = static_cast<int16_t>(c.bus.read16(desc + 8));
    rows = static_cast<int16_t>(c.bus.read16(desc + 10));
    if (table < kNameTables || (table - kNameTables) % kNameTableSize || (table - kNameTables) / kNameTableSize >= 4) return -1;
    if (columns != TileSides::kColumns || rows <= 0 || rows > TileSides::kRows) return -1;
    return static_cast<int>((table - kNameTables) / kNameTableSize);
}

// The draw routine at 0x9628: a part of a map (x0, y0 from its top left
// corner, w x h; all of it without a part given) about to go at column x,
// row y. Past the screen's edges go the part's columns there, and where the
// part reaches an edge, the map's columns beyond it, as far as the map goes
// (the doors: the half of the title not yet on the screen, or gone from
// it); never the map's columns between the part's inner end and an edge.
void draw(recomp::Context& c) {
    const auto& r = c.cpu.gpr;
    int columns = 0, rows = 0;
    const int layer = layer_of(c, r[19], columns, rows);
    if (layer < 0) return;
    TileSides& sides = c.bus.tile_sides();
    const auto map = static_cast<uint16_t>(r[23]);
    const auto x = static_cast<int32_t>(r[6]), y = static_cast<int32_t>(r[5]);
    const auto w = static_cast<int32_t>(r[26]), h = static_cast<int32_t>(r[27]);
    sides.begin_map(layer, map, y, y + std::max(h, 0));
    const auto stride = static_cast<int32_t>(r[28]), x0 = static_cast<int32_t>(r[10]), y0 = static_cast<int32_t>(r[9]);
    if (w <= 0 || h <= 0 || stride <= 0 || stride > 1024) return;
    // The map's entries less 2 (its header's height just before them).
    const uint32_t base = r[25] - static_cast<uint32_t>(y0 * stride + x0) * 2;
    const auto height = static_cast<int32_t>(c.bus.read16(base));
    const bool clear = (r[18] & 0x8000) != 0;
    const bool reaches_left = x <= 0, reaches_right = x + w >= TileSides::kScreen;
    auto source = [&](int32_t mr, int32_t mc) { return c.bus.read16(base + 2 + static_cast<uint32_t>(mr * stride + mc) * 2); };
    // Past the map's own edge: in rows whose edge on that side is the row's
    // background (at least 4 of its tiles: the title's band, its logo and the
    // black bar), that tile carried on, as the tile generator carries a still
    // picture's (TilegenRenderer::set_extend). In the title's other rows (its
    // riveted wall, a photo of 62 different tiles a row: none repeats), the
    // title itself again kTitleWallRepeat columns along. Other maps' other
    // rows are left to widescreen's usual sides.
    auto background = [&](int32_t mr, int32_t edge) {
        const uint16_t tile = source(mr, edge);
        int count = 0;
        for (int32_t mc = 0; mc < stride && count < 4; ++mc) count += source(mr, mc) == tile;
        return count >= 4;
    };
    const bool title = map == kTitleMap && stride > kTitleWallRepeat;
    auto put = [&](int32_t row, int32_t column, bool carry) {
        const bool part = column >= x && column < x + w;
        if (!part && !(column < x ? reaches_left : reaches_right)) return;
        const int32_t mr = y0 + (row - y);
        int32_t mc = x0 + (column - x);
        if (carry) {
            mc = std::clamp(mc, 0, stride - 1);
        } else {
            while (mc < 0) mc += kTitleWallRepeat;
            while (mc >= stride) mc -= kTitleWallRepeat;
        }
        uint16_t entry = kBlank;
        if (!clear) {
            const uint16_t src = source(mr, mc);
            const auto bank = static_cast<uint16_t>(c.bus.read16(r[29] + ((src >> 6) & 0xFEu)));
            entry = static_cast<uint16_t>((src & 0x7F) | (bank & 0xFF80));
        }
        sides.draw(layer, row, column, entry, map);
    };
    for (int32_t row = std::max(y, 0); row < std::min(y + h, rows); ++row) { // (clipped as the game does)
        const int32_t mr = y0 + (row - y);
        if (mr < 0 || mr >= height) continue;
        if (reaches_left) {
            const bool carry = background(mr, 0);
            if (carry || title)
                for (int32_t column = -TileSides::kSide; column < 0; ++column) put(row, column, carry);
        }
        if (reaches_right) {
            const bool carry = background(mr, stride - 1);
            if (carry || title)
                for (int32_t column = TileSides::kScreen; column < TileSides::kScreen + TileSides::kSide; ++column)
                    put(row, column, carry);
        }
    }
}

// The fill routine: one reaching the screen's edge empties what was kept past it on its rows.
void fill(recomp::Context& c) {
    const auto& r = c.cpu.gpr;
    int columns = 0, rows = 0;
    const int layer = layer_of(c, r[3], columns, rows);
    if (layer < 0) return;
    const auto w = static_cast<int32_t>(r[4]), h = static_cast<int32_t>(r[5]);
    // (the offset may be negative: the title's last band clear starts a row above the layer)
    const int32_t row_bytes = columns * 2, offset = static_cast<int32_t>(r[11]);
    const int32_t y = offset >= 0 ? offset / row_bytes : -((-offset + row_bytes - 1) / row_bytes);
    const int32_t x = (offset - y * row_bytes) / 2;
    const bool left = x == 0, right = x + w >= TileSides::kScreen;
    if (!left && !right) return;
    for (int32_t row = y; row < y + h && row < rows; ++row) {
        if (left) c.bus.tile_sides().fill_edge(layer, row, false);
        if (right) c.bus.tile_sides().fill_edge(layer, row, true);
    }
}

void copy(recomp::Context& c) {
    int columns = 0, rows = 0;
    const int layer = layer_of(c, c.cpu.gpr[16], columns, rows);
    if (layer >= 0) c.bus.tile_sides().copy(layer);
}

void hook(recomp::Context& c, uint32_t pc, void*) {
    if (pc == kDrawHook) draw(c);
    else if (pc == kFillHook) fill(c);
    else if (pc == kCopyHook) copy(c);
}

} // namespace

void install_spikeout_hooks(recomp::Context& c) { c.set_hook(hook, nullptr); }

} // namespace m3::games
