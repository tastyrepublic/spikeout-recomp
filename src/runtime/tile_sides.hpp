// Widescreen's sides from the game's own tile maps: the columns past each
// side of the tile layers, as the game would have written them had its
// layers been wider.
//
// Spikeout keeps each layer in a buffer in work RAM (64 entries a row) and
// draws tile maps into it with one routine, which clips each map at the
// layer's columns 0 and 64; a copy routine then puts the entries that
// changed into the tile generator's name table. The title's doors slide
// across the screen as such maps: each frame a narrower or wider part of
// the title's map, drawn at the screen's edge, the rest of each door never
// written. games/spikeofe_hooks.cpp watches the routines
// (recomp::Context::hook) and keeps here what a map would have put past
// the screen's edges, kSide columns each side (the right side from the
// screen's edge, column 62: the buffer's columns 62 and 63 too), and when
// the game copies a layer, this layer's sides are copied too (to what the
// tile generator draws from), so they change in step with the screen.
//
// Kept: rows whose edge is the row's background (the title's band, logo
// and black bar), and the title's riveted wall (its own tiles repeated past
// the map's edge: games/spikeofe_hooks.cpp). A cell is empty until a map is
// drawn over it. A map drawn again clears the cells it drew on those rows
// before the last copy (the doors move on, the game clears nothing past the
// screen; the title's wall, drawn once, stays while its band is redrawn); a fill
// reaching the screen's edge empties the cells past that edge on its rows,
// except those drawn since the last copy (the game draws the doors, then
// clears the gap they left).
#pragma once

#include <array>
#include <cstdint>

namespace m3 {

class State;

class TileSides {
public:
    // The buffer's columns, those on the screen (496 pixels), and those kept past each side.
    static constexpr int kLayers = 4, kRows = 64, kColumns = 64, kScreen = 62, kSide = 12;
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
    // A cell past the edge of a picture whose edge is a fade (the linking
    // screen's spotlight): no tile, the fade continued there (m3::WideLayer).
    static constexpr uint32_t kFade = 0xFFFFFFFEu;

    // ---- As the game draws ----
    // Map `map` is about to be drawn on `layer` over rows [first_row,
    // end_row): its cells there from before the last copy go.
    void begin_map(int layer, uint16_t map, int first_row, int end_row);
    // A map's entry (the layer buffer's form) at (row, column), column outside [0, kScreen).
    void draw(int layer, int row, int column, uint16_t entry, uint16_t map);
    // Map `map`'s picture fades out at the edge here: the fade continued.
    void fade(int layer, int row, int column, uint16_t map);
    // A fill reaching the screen's left (right) edge on `row`.
    void fill_edge(int layer, int row, bool right);
    // The game copies `layer` to the name table: its sides with it.
    void copy(int layer);

    // ---- For the tile generator ----
    // The name-table entry at (row, column) as last copied, column -kSide..-1
    // or kScreen..kScreen+kSide-1; kEmpty if none, kFade for a fade.
    uint32_t entry(int layer, int row, int column) const;
    // Whether `row` has any entry on the left (right) side.
    bool any(int layer, int row, bool right) const;

    void state(State& s);

private:
    struct Cell {
        uint16_t entry = 0;  // the layer buffer's form
        uint16_t map = 0;    // the map that drew it
        uint8_t full = 0;    // a map drew it (else empty); kFadeCell: a fade
        uint8_t drawn = 0;   // since the last copy
    };
    static constexpr uint8_t kFadeCell = 2;
    // Cell (layer, row, side, i): column -1 - i on the left, kScreen + i on the right.
    static int index(int layer, int row, bool right, int i) {
        return ((layer * kRows + row) * 2 + (right ? 1 : 0)) * kSide + i;
    }
    static constexpr int kCells = kLayers * kRows * 2 * kSide;
    std::array<Cell, kCells> pending_{};
    std::array<uint32_t, kCells> shown_ = filled();
    static constexpr std::array<uint32_t, kCells> filled() {
        std::array<uint32_t, kCells> a{};
        for (uint32_t& v : a) v = kEmpty;
        return a;
    }
};

} // namespace m3
