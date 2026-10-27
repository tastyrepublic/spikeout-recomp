// Widescreen's sides for the 2D layers: each 496x384 picture (one of
// m3::TilegenRenderer's, below or above the 3D) made `extend` pixels wider
// each side, after vf3-recomp's sides (no new art). Each side pixel is, in
// order:
//   - what the tile generator drew there itself (TilegenRenderer::set_extend:
//     scrolling layers from their name tables, which loop: the character
//     select's marquee, the map screen's faces, the stages' skies);
//   - the row's last 8 pixels repeated, where its 16 pixels at the edge are
//     opaque and within two steps of the 5-bit colours of each other, as
//     are the rows above and below (plain bands, backgrounds and skies,
//     their dither or grain kept as drawn: the character select's silver
//     bars, which change along the row tile by tile);
//   - the row's last P pixels repeated, where the 192 at the edge repeat
//     every P (32-160) pixels to within 24 levels on average (a texture,
//     such as the title's riveted wall), P chosen for each band of such
//     rows together (at least 4) so that they stay aligned;
//   - for a row of the picture between such rows (a border, a line of a
//     logo reaching the edge): the nearest one's within 8 rows (above the
//     3D: its own last 8 pixels if plain, else the nearest however far, so that the 3D
//     it covers stays covered);
//   - else nothing (alpha 0).
// Plain edges and repeats are carried only on a side where most rows (60%)
// have one: a picture whose edge is plain or smooth here and there leaves
// those sides empty, not streaked.
#pragma once

#include "runtime/tilegen.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace m3 {

class WideLayer {
public:
    static constexpr int kWidth = TilegenRenderer::kWidth, kHeight = TilegenRenderer::kHeight;
    static constexpr int kProbe = 192, kMinPeriod = 32, kMaxPeriod = 160;

    // `out` becomes (kWidth + 2 * extend) x kHeight. `side`: the tile
    // generator's side pixels (TilegenRenderer::bottom_side or top_side, for
    // the same extend; else ignored). `above`: the layers above the 3D.
    // `own` and `real`: sides the tile generator drew from the game's own
    // columns past the screen, and the lines it did (TilegenRenderer::
    // bottom_own/top_own and bottom_real/top_real): put over the sides made
    // there, everything else made as without them.
    void build(std::span<const uint32_t> layer, std::span<const uint32_t> side, int extend, std::vector<uint32_t>& out,
               bool above = false, std::span<const uint32_t> own = {}, std::span<const uint8_t> real = {});

    // The widescreen picture as the window's screen shader makes it
    // (src/gpu/shaders/screen.frag), for gamerun: the wide layers and 3D
    // (all (kWidth + 2 * extend) x kHeight), the sides showing the 3D only
    // where it reaches the arcade screen's edge on that side.
    static void composite(std::span<uint32_t> out, std::span<const uint32_t> bottom, std::span<const uint32_t> top,
                          std::span<const uint32_t> three_d, int extend);

private:
    // Fades: the pixels in from the edge their rate is measured over, the
    // pixels averaged at each end, the rows averaged each way (weighted to
    // the nearer), how far apart a grey's channels may be, and how much
    // further out than its rate the fade eases into black.
    static constexpr int kFadeProbe = 12, kFadeSpan = 4, kFadeRows = 12, kGrey = 12;
    static constexpr float kFadeEase = 1.5f;
    // A fade marked past a picture's edge (TileSides::kFade), continued on row y's side s into `to`.
    static void fade(std::span<const uint32_t> layer, std::span<const uint8_t> real, std::span<const uint32_t> own, int y,
                     int s, int e, uint32_t* to);

    // A row's edge, kept while its pixels there stay the same (the period
    // search is the slow part, and screens hold still).
    struct Edge {
        uint64_t hash = 0;
        bool known = false;
        uint32_t colour = 0;   // its plain colour (0: not plain)
        float best = 1e9f;     // the smallest error of any period
        std::array<float, kMaxPeriod - kMinPeriod + 1> error{}; // each period's
    };
    std::array<std::array<Edge, kHeight>, 2> edges_;
};

} // namespace m3
