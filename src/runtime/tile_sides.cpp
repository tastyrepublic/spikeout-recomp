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

// The full-screen pictures whose sides are blurred: the stages' "NEXT STAGE
// COMING UP!!" cards, a run of maps (0x80 Diesel Town's, 0x7F the Ship
// Yard's, 0x7D and 0x7E the later stages': every card of the bot's whole
// game), and stage 1's closing story (0x82, Mikhail's eyes; its captions are
// maps of their own on layer 0). Found with gamerun --map-trace over the
// bot's run. Not the ranking (0x81: its fade) nor the heroes' faces (0x96:
// that picture scrolls, its sides are its own).
bool TileSides::blurred(uint16_t map) { return (map >= 0x7D && map <= 0x80) || map == 0x82; }

void TileSides::begin_map(int layer, uint16_t map, int first_row, int end_row) {
    if (on_map) on_map(layer, map, first_row, end_row);
    if (layer < 0 || layer >= kLayers) return;
    if (first_row <= 0 && end_row >= kScreenRows) { // (a map over the whole screen)
        picture_[layer] = blurred(map) ? map : 0;
        black_ = false;
    }
    if (credits(map)) black_ = true;
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

void TileSides::fade(int layer, int row, int column, uint16_t map) {
    if (!valid(layer, row)) return;
    const bool right = column >= kScreen;
    const int i = right ? column - kScreen : -1 - column;
    if (i < 0 || i >= kSide) return;
    pending_[static_cast<std::size_t>(index(layer, row, right, i))] = Cell{0, map, kFadeCell, 1};
}

void TileSides::fill_edge(int layer, int row, bool right) {
    if (!valid(layer, row)) return;
    for (int i = 0; i < kSide; ++i) {
        Cell& c = pending_[static_cast<std::size_t>(index(layer, row, right, i))];
        if (!c.drawn) c = Cell{};
    }
}

void TileSides::clear_layer(int layer) {
    if (layer < 0 || layer >= kLayers) return;
    picture_[layer] = 0;
    for (int row = 0; row < kRows; ++row)
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < kSide; ++i) pending_[static_cast<std::size_t>(index(layer, row, side != 0, i))] = Cell{};
}

void TileSides::copy(int layer) {
    if (layer < 0 || layer >= kLayers) return;
    for (int row = 0; row < kRows; ++row)
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < kSide; ++i) {
                const auto at = static_cast<std::size_t>(index(layer, row, side != 0, i));
                Cell& c = pending_[at];
                shown_[at] = c.full == kFadeCell ? kFade : c.full ? name_table_entry(c.entry) : kEmpty;
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

bool TileSides::any(int layer, int row, bool right, int columns) const {
    if (!valid(layer, row)) return false;
    for (int i = 0; i < std::min(columns, kSide); ++i)
        if (shown_[static_cast<std::size_t>(index(layer, row, right, i))] != kEmpty) return true;
    return false;
}

void TileSides::state(State& s) {
    if (s.loading()) picture_ = {}, black_ = false;
    // A state from before 21:9 (kOldSide columns a side, its own tag): those, the rest empty (until next drawn).
    if (s.tagged("tile sides")) {
        std::array<Cell, kCells / kSide * kOldSide> pending{};
        std::array<uint32_t, kCells / kSide * kOldSide> shown{};
        s.io(pending);
        s.io(shown);
        pending_ = {}, shown_ = filled();
        for (std::size_t line = 0; line < kCells / kSide; ++line)
            for (std::size_t i = 0; i < kOldSide; ++i) {
                pending_[line * kSide + i] = pending[line * kOldSide + i];
                shown_[line * kSide + i] = shown[line * kOldSide + i];
            }
        return;
    }
    s.tag("tile sides 26");
    s.io(pending_);
    s.io(shown_);
}

} // namespace m3
