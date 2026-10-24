#include "runtime/widescreen.hpp"

#include <algorithm>
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
    for (int y = 0; y < kHeight; ++y)
        for (int s = 0; s < 2; ++s) {
            if (!(real[static_cast<std::size_t>(y)] & (1u << s))) continue;
            const uint32_t* from = own.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(2 * e) +
                                   static_cast<std::size_t>(s ? e : 0);
            std::copy_n(from, e, out.begin() + static_cast<std::ptrdiff_t>(y) * wide + (s ? e + kWidth : 0));
        }
}

} // namespace m3
