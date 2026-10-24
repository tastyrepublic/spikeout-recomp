#include "runtime/tile_sides.hpp"

#include "runtime/state.hpp"

#include <algorithm>

namespace m3 {

namespace {

// The copy routine's change to each entry on its way to the name table
// (blocks 0x82b0 on): the dirty bit (15) off, bit 14 as bit 7.
uint32_t name_table_entry(uint16_t e) {
    const uint32_t v = e & 0x3FFFu;
    return v | (static_cast<uint32_t>(e & 0x80u) << 7);
}

bool valid(int layer, int row) { return layer >= 0 && layer < TileSides::kLayers && row >= 0 && row < TileSides::kRows; }

} // namespace

void TileSides::begin_map(int layer, uint16_t map, int first_row, int end_row) {
    if (layer < 0 || layer >= kLayers) return;
    for (int row = std::max(first_row, 0); row < std::min(end_row, kRows); ++row)
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < kSide; ++i) {
                Cell& c = pending_[static_cast<std::size_t>(index(layer, row, side != 0, i))];
                if (c.full && c.map == map && !c.drawn) c = Cell{};
            }
}

void TileSides::draw(int layer, int row, int column, uint16_t entry, uint16_t map) {
    if (!valid(layer, row)) return;
    const bool right = column >= kScreen;
    const int i = right ? column - kScreen : -1 - column;
    if (i < 0 || i >= kSide) return;
    pending_[static_cast<std::size_t>(index(layer, row, right, i))] = Cell{entry, map, 1, 1};
}

void TileSides::fill_edge(int layer, int row, bool right) {
    if (!valid(layer, row)) return;
    for (int i = 0; i < kSide; ++i) {
        Cell& c = pending_[static_cast<std::size_t>(index(layer, row, right, i))];
        if (!c.drawn) c = Cell{};
    }
}

void TileSides::copy(int layer) {
    if (layer < 0 || layer >= kLayers) return;
    for (int row = 0; row < kRows; ++row)
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < kSide; ++i) {
                const auto at = static_cast<std::size_t>(index(layer, row, side != 0, i));
                Cell& c = pending_[at];
                shown_[at] = c.full ? name_table_entry(c.entry) : kEmpty;
                c.drawn = 0;
            }
}

uint32_t TileSides::entry(int layer, int row, int column) const {
    if (!valid(layer, row)) return kEmpty;
    const bool right = column >= kScreen;
    const int i = right ? column - kScreen : -1 - column;
    if (i < 0 || i >= kSide) return kEmpty;
    return shown_[static_cast<std::size_t>(index(layer, row, right, i))];
}

bool TileSides::any(int layer, int row, bool right) const {
    if (!valid(layer, row)) return false;
    for (int i = 0; i < kSide; ++i)
        if (shown_[static_cast<std::size_t>(index(layer, row, right, i))] != kEmpty) return true;
    return false;
}

void TileSides::state(State& s) {
    s.tag("tile sides");
    s.io(pending_);
    s.io(shown_);
}

} // namespace m3
