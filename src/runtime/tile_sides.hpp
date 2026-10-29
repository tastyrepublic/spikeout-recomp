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
#include <functional>
#include <cstdint>

namespace m3 {

class State;

class TileSides {
public:
    // The buffer's columns, those on the screen (496 pixels), and those kept past each side.
    static constexpr int kLayers = 4, kRows = 64, kColumns = 64, kScreen = 62, kSide = 12, kScreenRows = 48;
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
    // A cell past the edge of a picture whose edge is a fade (the linking
    // screen's spotlight): no tile, the fade continued there (m3::WideLayer).
    static constexpr uint32_t kFade = 0xFFFFFFFEu;

    // ---- As the game draws ----
    // Map `map` is about to be drawn on `layer` over rows [first_row,
    // end_row): its cells there from before the last copy go.
    void begin_map(int layer, uint16_t map, int first_row, int end_row);
    // Debug aid (gamerun --map-trace): told of each map drawn (layer, map, rows).
    std::function<void(int layer, uint16_t map, int first_row, int end_row)> on_map;
    // A map's entry (the layer buffer's form) at (row, column), column outside [0, kScreen).
    void draw(int layer, int row, int column, uint16_t entry, uint16_t map);
    // Map `map`'s picture fades out at the edge here: the fade continued.
    void fade(int layer, int row, int column, uint16_t map);
    // A fill reaching the screen's left (right) edge on `row`.
    void fill_edge(int layer, int row, bool right);
    // The whole layer cleared: every cell past its edges empty (at the next copy).
    void clear_layer(int layer);
    // The game copies `layer` to the name table: its sides with it.
    void copy(int layer);

    // ---- For the tile generator ----
    // The name-table entry at (row, column) as last copied, column -kSide..-1
    // or kScreen..kScreen+kSide-1; kEmpty if none, kFade for a fade.
    uint32_t entry(int layer, int row, int column) const;
    // Whether `row` has any entry on the left (right) side.
    bool any(int layer, int row, bool right) const;

    // ---- Full-screen pictures ----
    // A map drawn over the whole screen as a still picture, with nothing of
    // its own past the screen's edges (a stage's "NEXT STAGE COMING UP!!"
    // card, a story scene's picture): widescreen makes its sides from the
    // picture itself, enlarged and blurred (m3::WideLayer; the owner's choice
    // over the edges carried on, which made patterns and blocks there).
    static bool blurred(uint16_t map);
    // Whether `layer` shows such a picture: as last drawn (not every layer's
    // pictures go through the copy routine watched: a card's doesn't).
    bool picture(int layer) const { return layer >= 0 && layer < kLayers && picture_[layer] != 0; }

    // ---- The staff credits ----
    // From their set-up (begin_credits: games/spikeofe_hooks.cpp) or their
    // maps (the heading and names, 0xE7-0xEA, layer 0: a state loaded after
    // the set-up) until the next map over the whole screen (the title after
    // them): widescreen's 2D past the left side is the picture mirrored at
    // its edge (the sunset's sky, behind its 3D opened out), past the right
    // the names' black carried on, no wider than 16:9 (black past that);
    // m3::WideLayer. The owner's choice of three previews.
    static bool credits(uint16_t map) { return map >= 0xE7 && map <= 0xEA; }
    void begin_credits() { black_ = true; }
    bool black() const { return black_; } // (the credits' sides)

    // Save states: the cells. The pictures and the credits aren't kept (a
    // state from before them still loads): set again as the game next draws them.
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
    std::array<uint16_t, kLayers> picture_{}; // (the picture's map; 0: none)
    bool black_ = false;                     // (the credits)
    static constexpr std::array<uint32_t, kCells> filled() {
        std::array<uint32_t, kCells> a{};
        for (uint32_t& v : a) v = kEmpty;
        return a;
    }
};

} // namespace m3
