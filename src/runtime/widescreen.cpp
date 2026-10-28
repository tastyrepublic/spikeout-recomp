#include "runtime/widescreen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace m3 {

namespace {

constexpr int kPlainTest = 16, kAverage = 8, kSpread = 18; // a plain edge: 16 pixels within two 5-bit steps
constexpr float kRepeatError = 24.0f, kRowError = 30.0f;
constexpr int kMinBand = 4;
constexpr int kSearches = 64; // rows a side a frame

int channel(uint32_t p, int c) { return static_cast<int>((p >> (16 - 8 * c)) & 0xFF); }

// The largest difference of a colour channel between two pixels.
int difference(uint32_t a, uint32_t b) {
    int d = 0;
    for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(channel(a, c) - channel(b, c)));
    return d;
}

} // namespace

void WideLayer::composite(std::span<uint32_t> out, std::span<const uint32_t> bottom, std::span<const uint32_t> top,
                          std::span<const uint32_t> three_d, int extend) {
    const int e = std::max(extend, 0), wide = kWidth + 2 * e;
    const auto size = static_cast<std::size_t>(wide) * kHeight;
    if (out.size() < size || bottom.size() < size || top.size() < size || three_d.size() < size) return;
    // as the shader's: 48 rows of the edge column looked at
    auto reaches = [&](int x) {
        for (int i = 0; i < 48; ++i)
            if (three_d[static_cast<std::size_t>((2 * i + 1) * kHeight / 96) * static_cast<std::size_t>(wide) +
                        static_cast<std::size_t>(x)] >> 24)
                return true;
        return false;
    };
    const bool left = reaches(e), right = reaches(e + kWidth - 1);
    std::vector<uint32_t> shown(three_d.begin(), three_d.begin() + static_cast<std::ptrdiff_t>(size));
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < wide; ++x)
            if ((x < e && !left) || (x >= e + kWidth && !right)) shown[static_cast<std::size_t>(y * wide + x)] = 0;
    TilegenRenderer::composite(out.first(size), bottom.first(size), top.first(size), shown);
}

void WideLayer::build(std::span<const uint32_t> layer, std::span<const uint32_t> side, int extend,
                      std::vector<uint32_t>& out, bool above, std::span<const uint32_t> own,
                      std::span<const uint8_t> real) {
    const int e = std::max(extend, 0), wide = kWidth + 2 * e;
    out.assign(static_cast<std::size_t>(wide) * kHeight, 0u);
    if (layer.size() < static_cast<std::size_t>(kWidth) * kHeight) return;
    for (int y = 0; y < kHeight; ++y)
        std::copy_n(layer.begin() + y * kWidth, kWidth, out.begin() + static_cast<std::ptrdiff_t>(y) * wide + e);
    if (e == 0) return;
    const bool has_side = side.size() == static_cast<std::size_t>(2 * e) * kHeight;

    for (int s = 0; s < 2; ++s) {
        // The row's pixels from the edge inwards.
        auto at = [&](int y, int i) { return layer[static_cast<std::size_t>(y * kWidth + (s ? kWidth - 1 - i : i))]; };
        std::array<Edge, kHeight>& edges = edges_[static_cast<std::size_t>(s)];
        // Rows whose side the tile generator drew all of (a sky, a marquee): nothing to work out.
        std::array<bool, kHeight> covered{};
        if (has_side)
            for (int y = 0; y < kHeight; ++y) {
                const uint32_t* from = side.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(2 * e) +
                                       static_cast<std::size_t>(s ? e : 0);
                covered[static_cast<std::size_t>(y)] = std::all_of(from, from + e, [](uint32_t p) { return (p >> 24) != 0; });
            }
        int searches = 0; // the period searched for in at most kSearches rows a frame (a new screen: over a few)
        for (int y = 0; y < kHeight; ++y) {
            if (covered[static_cast<std::size_t>(y)]) continue;
            Edge& edge = edges[static_cast<std::size_t>(y)];
            uint64_t hash = 1469598103934665603ull;
            for (int i = 0; i < kProbe; ++i) hash = (hash ^ at(y, i)) * 1099511628211ull;
            if (edge.known && edge.hash == hash) continue;
            edge = Edge{};
            edge.hash = hash;
            edge.known = true;
            bool opaque = true;
            for (int i = 0; i < kProbe && opaque; ++i) opaque = (at(y, i) >> 24) != 0;
            int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, sum[3] = {0, 0, 0};
            bool plain = true;
            for (int i = 0; i < kPlainTest && plain; ++i) {
                const uint32_t p = at(y, i);
                plain = (p >> 24) != 0;
                for (int c = 0; c < 3; ++c) {
                    lo[c] = std::min(lo[c], channel(p, c));
                    hi[c] = std::max(hi[c], channel(p, c));
                    if (i < kAverage) sum[c] += channel(p, c);
                }
            }
            for (int c = 0; c < 3 && plain; ++c) plain = hi[c] - lo[c] <= kSpread;
            if (plain) {
                edge.colour = 0xFF000000u | static_cast<uint32_t>(sum[0] / kAverage) << 16 |
                              static_cast<uint32_t>(sum[1] / kAverage) << 8 | static_cast<uint32_t>(sum[2] / kAverage);
                continue;
            }
            if (!opaque) continue;
            if (searches++ >= kSearches) { // next frame
                edge = Edge{};
                continue;
            }
            for (int p = kMinPeriod; p <= kMaxPeriod; ++p) { // every second pixel is enough
                int total = 0, count = 0;
                for (int i = 0; i + p < kProbe; i += 2, ++count) total += difference(at(y, i), at(y, i + p));
                const float error = static_cast<float>(total) / static_cast<float>(count);
                edge.error[static_cast<std::size_t>(p - kMinPeriod)] = error;
                edge.best = std::min(edge.best, error);
            }
        }

        // Which rows carry on what: plain (with plain rows either side), or
        // repeating in a band of them.
        auto plain = [&](int y) { return edges[static_cast<std::size_t>(y)].colour; };
        auto repeats = [&](int y) {
            const Edge& edge = edges[static_cast<std::size_t>(y)];
            return !edge.colour && edge.best <= kRepeatError;
        };
        std::array<uint32_t, kHeight> colour{};
        std::array<int, kHeight> period{};
        int rows = 0;
        for (int y = 0; y < kHeight; ++y) {
            if (covered[static_cast<std::size_t>(y)]) {
                ++rows;
                continue;
            }
            if (plain(y) || repeats(y)) ++rows;
            auto near = [&](int r) { return covered[static_cast<std::size_t>(r)] || plain(r); };
            const bool up = y == 0 || near(y - 1), down = y == kHeight - 1 || near(y + 1);
            if (plain(y) && up && down) colour[static_cast<std::size_t>(y)] = plain(y);
        }
        const bool carry = rows * 10 >= kHeight * 6;
        for (int y = 0; y < kHeight && carry;) {
            int end = y;
            while (end < kHeight && repeats(end)) ++end;
            if (end - y >= kMinBand) { // the band's period, else (far off it) the row's own
                int best = 0;
                float best_error = 1e9f;
                for (int p = kMinPeriod; p <= kMaxPeriod; ++p) {
                    float total = 0;
                    for (int r = y; r < end; ++r) total += edges[static_cast<std::size_t>(r)].error[static_cast<std::size_t>(p - kMinPeriod)];
                    if (total < best_error) {
                        best_error = total;
                        best = p;
                    }
                }
                for (int r = y; r < end; ++r) {
                    const Edge& edge = edges[static_cast<std::size_t>(r)];
                    int p = best;
                    if (edge.error[static_cast<std::size_t>(best - kMinPeriod)] > kRowError)
                        p = static_cast<int>(std::min_element(edge.error.begin(), edge.error.end()) - edge.error.begin()) + kMinPeriod;
                    period[static_cast<std::size_t>(r)] = p;
                }
            }
            y = end == y ? y + 1 : end;
        }

        for (int y = 0; y < kHeight; ++y) {
            const auto row = static_cast<std::size_t>(y);
            uint32_t* o = out.data() + row * static_cast<std::size_t>(wide);
            for (int d = 1; d <= e; ++d) { // d pixels out from the edge
                const int x = s ? e + kWidth - 1 + d : e - d;
                uint32_t p = 0;
                if (has_side) p = side[row * static_cast<std::size_t>(2 * e) + static_cast<std::size_t>(s ? e + d - 1 : e - d)];
                if (!(p >> 24) && carry) {
                    if (colour[row]) { // its last 8 pixels repeated: its dither or grain as drawn
                        p = at(y, 7 - (d - 1) % 8);
                    } else if (const int per = period[row]) {
                        const int i = per - 1 - (d - 1) % per; // the same distance in, a period back
                        p = at(y, i);
                    }
                }
                o[x] = p;
            }
        }

        // Rows of the picture between carried ones (a border between a band
        // and a texture, a line of the logo reaching the edge): as the
        // nearest carried row within 8, where the tile generator drew nothing.
        // Above the 3D, nothing nearby: its own last 8 pixels if plain, or the nearest
        // carried row's however far (the 3D it covered stays covered).
        if (!carry) continue;
        auto carried = [&](int y) { return colour[static_cast<std::size_t>(y)] || period[static_cast<std::size_t>(y)]; };
        for (int y = 0; y < kHeight; ++y) {
            if (carried(y) || !(at(y, 0) >> 24)) continue;
            uint32_t* o = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(wide);
            if (above && edges[static_cast<std::size_t>(y)].colour) { // plain itself: its last 8 pixels
                for (int d = 1; d <= e; ++d) {
                    const int x = s ? e + kWidth - 1 + d : e - d;
                    if (!(o[x] >> 24)) o[x] = at(y, 7 - (d - 1) % 8);
                }
                continue;
            }
            int from = -1;
            for (int d = 1; d <= (above ? kHeight : 8) && from < 0; ++d) {
                if (y - d >= 0 && carried(y - d)) from = y - d;
                else if (y + d < kHeight && carried(y + d)) from = y + d;
            }
            if (from < 0) continue;
            const uint32_t* f = out.data() + static_cast<std::size_t>(from) * static_cast<std::size_t>(wide);
            for (int d = 1; d <= e; ++d) {
                const int x = s ? e + kWidth - 1 + d : e - d;
                if (!(o[x] >> 24)) o[x] = f[x];
            }
        }
    }

    // The sides the tile generator drew from the game's own columns past the
    // screen (the title's doors), over whatever was made for them above: the
    // rest is made as it always was, these rows not looked at.
    if (!has_side || own.size() != side.size() || real.size() != static_cast<std::size_t>(kHeight)) return;
    // Each side's fading picture's own colour at the edge (its pixels there
    // over all its fade rows, summed): what a fade row's pixels must be like.
    std::array<std::array<float, 3>, 2> hue{};
    for (int s = 0; s < 2; ++s)
        for (int y = 0; y < kHeight; ++y) {
            if (!(real[static_cast<std::size_t>(y)] & (4u << s))) continue;
            for (int i = 0; i < kFadeSpan; ++i) {
                const uint32_t p = layer[static_cast<std::size_t>(y * kWidth + (s ? kWidth - 1 - i : i))];
                for (int c = 0; c < 3; ++c) hue[static_cast<std::size_t>(s)][static_cast<std::size_t>(c)] += static_cast<float>((p >> (8 * c)) & 255);
            }
        }
    for (int y = 0; y < kHeight; ++y)
        for (int s = 0; s < 2; ++s) {
            const uint8_t mark = real[static_cast<std::size_t>(y)];
            if (!(mark & (1u << s))) continue;
            const uint32_t* from = own.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(2 * e) +
                                   static_cast<std::size_t>(s ? e : 0);
            uint32_t* to = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(wide) +
                           static_cast<std::size_t>(s ? e + kWidth : 0);
            if (!(mark & (4u << s))) {
                std::copy_n(from, e, to);
                continue;
            }
            fade(layer, real, own, hue[static_cast<std::size_t>(s)], y, s, e, to);
            for (int i = 0; i < e; ++i) // what the tile generator drew in front of it
                if (from[i] >> 24) to[i] = from[i];
        }
}

// A picture fading out at its edge (the linking screen's spotlight, the
// ranking's): each row's fade goes on at the rate it changes over the
// kFadeProbe pixels in from the edge, darkening only, so the spotlight
// closes in its own curve. The picture is dithered: its colour at the edge
// and kFadeProbe in are each the average of kFadeSpan pixels, over the fade
// rows within kFadeRows whose pixels there are all the picture's own (grey,
// or of its colour at that edge, `hue`: the ranking's is red; not text or a
// figure over it) and that nothing in front crosses past the edge (`own`: a
// marquee's letters, whose highlights and outlines can pass for grey);
// where none are, the nearest such row's.
void WideLayer::fade(std::span<const uint32_t> layer, std::span<const uint8_t> real, std::span<const uint32_t> own,
                     const std::array<float, 3>& hue, int y, int s, int e, uint32_t* to) {
    // (a grey picture's colour isn't used: grey pixels alone, as the linking screen was first made)
    const float hue_most = std::max({hue[0], hue[1], hue[2]}), hue_least = std::min({hue[0], hue[1], hue[2]});
    const bool coloured = hue_most > 0 && (hue_most - hue_least) / hue_most > kColoured;
    const float hue_length = coloured ? std::sqrt(hue[0] * hue[0] + hue[1] * hue[1] + hue[2] * hue[2]) : 0.0f;
    auto grey = [&](uint32_t p) {
        if (!(p >> 24)) return false;
        const int r = static_cast<int>((p >> 16) & 255), g = static_cast<int>((p >> 8) & 255), b = static_cast<int>(p & 255);
        if (std::abs(r - g) <= kGrey && std::abs(g - b) <= kGrey && std::abs(r - b) <= kGrey) return true;
        // (of the picture's colour: in its direction, near enough; with the channels as the sums have them)
        const float v[3] = {static_cast<float>(b), static_cast<float>(g), static_cast<float>(r)};
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (hue_length <= 0 || length <= 0) return false;
        return (v[0] * hue[0] + v[1] * hue[1] + v[2] * hue[2]) / (length * hue_length) >= kHueMatch;
    };
    // Adds row's edge and inner colours to the sums, if they're the picture's.
    std::array<float, 6> sum{};
    float weights = 0;
    auto add = [&](int row, float weight) {
        if (row < 0 || row >= kHeight || !(real[static_cast<std::size_t>(row)] & (4u << s))) return;
        const uint32_t* front = own.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(2 * e) +
                                static_cast<std::size_t>(s ? e : 0);
        if (std::any_of(front, front + e, [](uint32_t p) { return (p >> 24) != 0; })) return;
        auto px = [&](int i) { return layer[static_cast<std::size_t>(row * kWidth + (s ? kWidth - 1 - i : i))]; };
        std::array<float, 6> here{};
        for (int i = 0; i < kFadeSpan; ++i)
            for (const int at : {i, kFadeProbe + i}) {
                const uint32_t p = px(at);
                if (!grey(p)) return;
                for (int c = 0; c < 3; ++c) here[static_cast<std::size_t>((at == i ? 0 : 3) + c)] += static_cast<float>((p >> (8 * c)) & 255);
            }
        for (std::size_t k = 0; k < sum.size(); ++k) sum[k] += weight * here[k] / kFadeSpan;
        weights += weight;
    };
    for (int d = -kFadeRows; d <= kFadeRows; ++d) // nearer rows count more
        add(y + d, static_cast<float>(kFadeRows + 1 - std::abs(d)));
    for (int d = kFadeRows + 1; d < kHeight && weights == 0; ++d) {
        add(y - d, 1);
        if (weights == 0) add(y + d, 1);
    }
    // Out from the edge the fade starts at the picture's rate and eases into
    // black kFadeEase times as far out as that rate would reach it: with t
    // the distance over that, at * (1 - 1.5 t + 0.5 t^3), which leaves the
    // edge at the picture's rate and comes to black flat.
    for (int d = 1; d <= e; ++d) { // d pixels out from the edge
        uint32_t p = 0xFF000000u;
        if (weights > 0)
            for (int c = 0; c < 3; ++c) {
                const float at = sum[static_cast<std::size_t>(c)] / weights;
                const float in = sum[static_cast<std::size_t>(3 + c)] / weights;
                const float step = (in - at) / kFadeProbe;
                float v = at;
                if (step > 0) {
                    const float t = std::min(1.0f, static_cast<float>(d) * step / (kFadeEase * std::max(at, 1.0f)));
                    v = at * (1.0f - 1.5f * t + 0.5f * t * t * t);
                }
                p |= static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f) << (8 * c);
            }
        to[s ? d - 1 : e - d] = p;
    }
}

} // namespace m3
