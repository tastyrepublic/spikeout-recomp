// platform_icons: the network status list's platform icons (assets/ui/platforms.png, a row of 36 x 36 cells, and
// platforms.rcss's sprites): Windows, macOS, Linux and Android in one colour, white (the list tints each as its
// row's text: RmlUi's image-color), with a dark outline round them as the list's text has (they show over the
// game's picture). macOS and Android: Simple Icons' paths (platform_icon_paths.inc); Windows: its logo's four
// squares; Linux: a flat penguin, its belly and beak cut out (the owner's choice: the line art Tux too thin at
// this size). Each drawn 4 x 4 times finer and averaged (smooth edges).
//
//   platform_icons OUT.png
#include "png_write.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <string_view>
#include <vector>

namespace {

#include "platform_icon_paths.inc"

constexpr int kCell = 36, kFine = 4;
constexpr double kInset = 3; // (the logo's 24 x 24 drawn in the cell less this each side: room for its outline)
constexpr double kOutline = 2.6; // (its width: px of the 36: about the list's text's 2dp, the icons drawn at 18dp)

struct Point {
    double x, y;
};
using Poly = std::vector<Point>; // (a closed outline, flattened)

// An SVG path (the commands Simple Icons' use: M L H V C S Q T A Z, absolute and relative) as closed outlines.
class Path {
public:
    explicit Path(std::string_view d) : d_(d) {
        char cmd = 0;
        while (skip(), at_ < d_.size()) {
            if (std::isalpha(static_cast<unsigned char>(d_[at_]))) cmd = d_[at_++];
            else if (cmd == 'M') cmd = 'L'; // (numbers after a move: lines)
            else if (cmd == 'm') cmd = 'l';
            step(cmd);
        }
        close();
    }
    const std::vector<Poly>& polys() const { return polys_; }

private:
    void skip() {
        while (at_ < d_.size() && (d_[at_] == ' ' || d_[at_] == ',' || d_[at_] == '\n')) ++at_;
    }
    double num() {
        skip();
        const char* start = d_.data() + at_;
        char* end = nullptr;
        const double v = std::strtod(start, &end);
        at_ += static_cast<std::size_t>(end - start);
        return v;
    }
    bool flag() { // (an arc's flags: one digit, perhaps with nothing between them)
        skip();
        return d_[at_++] == '1';
    }
    void line(Point p) { cur_.push_back(p), pen_ = p; }
    void close() {
        if (cur_.size() > 2) polys_.push_back(cur_);
        cur_.clear();
    }
    void cubic(Point a, Point b, Point e) {
        const Point s = pen_;
        for (int i = 1; i <= 24; ++i) {
            const double t = i / 24.0, u = 1 - t;
            line({u * u * u * s.x + 3 * u * u * t * a.x + 3 * u * t * t * b.x + t * t * t * e.x,
                  u * u * u * s.y + 3 * u * u * t * a.y + 3 * u * t * t * b.y + t * t * t * e.y});
        }
        ctrl_ = b;
    }
    void quad(Point a, Point e) {
        const Point s = pen_;
        for (int i = 1; i <= 16; ++i) {
            const double t = i / 16.0, u = 1 - t;
            line({u * u * s.x + 2 * u * t * a.x + t * t * e.x, u * u * s.y + 2 * u * t * a.y + t * t * e.y});
        }
        ctrl_ = a;
    }
    void arc(double rx, double ry, double angle, bool large, bool sweep, Point e) { // (SVG's endpoint form, F.6.5)
        const Point s = pen_;
        if (rx == 0 || ry == 0) return line(e);
        rx = std::abs(rx), ry = std::abs(ry);
        const double phi = angle * std::numbers::pi / 180, cp = std::cos(phi), sp = std::sin(phi);
        const double dx = (s.x - e.x) / 2, dy = (s.y - e.y) / 2;
        const double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
        if (const double l = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry); l > 1) rx *= std::sqrt(l), ry *= std::sqrt(l);
        const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double k = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
        if (large == sweep) k = -k;
        const double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
        const double cx = cp * cx1 - sp * cy1 + (s.x + e.x) / 2, cy = sp * cx1 + cp * cy1 + (s.y + e.y) / 2;
        const auto angle_of = [](double ux, double uy, double vx, double vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const double t1 = angle_of(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
        double dt = angle_of((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
        if (!sweep && dt > 0) dt -= 2 * std::numbers::pi;
        if (sweep && dt < 0) dt += 2 * std::numbers::pi;
        const int n = std::max(4, static_cast<int>(std::abs(dt) * 12));
        for (int i = 1; i <= n; ++i) {
            const double t = t1 + dt * i / n;
            line(i == n ? e : Point{cx + rx * std::cos(t) * cp - ry * std::sin(t) * sp, cy + rx * std::cos(t) * sp + ry * std::sin(t) * cp});
        }
    }
    void step(char cmd) {
        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const double ox = rel ? pen_.x : 0, oy = rel ? pen_.y : 0;
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        const char was = last_;
        last_ = c;
        switch (c) {
        case 'M': {
            close();
            const double x = num(), y = num();
            pen_ = start_ = {ox + x, oy + y};
            cur_.push_back(pen_);
            break;
        }
        case 'L': {
            const double x = num(), y = num();
            line({ox + x, oy + y});
            break;
        }
        case 'H': line({ox + num(), pen_.y}); break;
        case 'V': line({pen_.x, oy + num()}); break;
        case 'C': {
            const double ax = num(), ay = num(), bx = num(), by = num(), ex = num(), ey = num();
            cubic({ox + ax, oy + ay}, {ox + bx, oy + by}, {ox + ex, oy + ey});
            break;
        }
        case 'S': { // (its first control the last one's reflection)
            const Point a = was == 'C' || was == 'S' ? Point{2 * pen_.x - ctrl_.x, 2 * pen_.y - ctrl_.y} : pen_;
            const double bx = num(), by = num(), ex = num(), ey = num();
            cubic(a, {ox + bx, oy + by}, {ox + ex, oy + ey});
            break;
        }
        case 'Q': {
            const double ax = num(), ay = num(), ex = num(), ey = num();
            quad({ox + ax, oy + ay}, {ox + ex, oy + ey});
            break;
        }
        case 'T': {
            const Point a = was == 'Q' || was == 'T' ? Point{2 * pen_.x - ctrl_.x, 2 * pen_.y - ctrl_.y} : pen_;
            const double ex = num(), ey = num();
            quad(a, {ox + ex, oy + ey});
            break;
        }
        case 'A': {
            const double rx = num(), ry = num(), a = num();
            const bool large = flag(), sweep = flag();
            const double ex = num(), ey = num();
            arc(rx, ry, a, large, sweep, {ox + ex, oy + ey});
            break;
        }
        case 'Z':
            close();
            pen_ = start_;
            break;
        default: ++at_; break; // (anything else: past it)
        }
    }

    std::string_view d_;
    std::size_t at_ = 0;
    Point pen_{0, 0}, start_{0, 0}, ctrl_{0, 0};
    char last_ = 0;
    Poly cur_;
    std::vector<Poly> polys_;
};

// Whether (x, y) is inside the outlines, by the nonzero rule (SVG's default).
bool inside(const std::vector<Poly>& polys, double x, double y) {
    int winding = 0;
    for (const Poly& p : polys)
        for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
            const Point a = p[j], b = p[i];
            if ((a.y <= y) != (b.y <= y)) {
                const double cross = (b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y);
                winding += b.y > a.y ? (cross > 0 ? 1 : 0) : (cross < 0 ? -1 : 0);
            }
        }
    return winding != 0;
}

// Linux: the flat penguin (Simple Icons' Tux is line art, too thin at this size): head and body one shape, its
// flippers out at the sides, the belly and the beak cut out. Drawn on an 800 x 600 sketch (the penguin 400 wide,
// 425 tall from y 90), as the others' 24 x 24.
double flat_penguin(double x, double y) {
    constexpr double kScale = 24.0 / 425;
    const double X = 400 + (x - 12) / kScale, Y = 90 + y / kScale;
    const auto ellipse = [&](double cx, double cy, double rx, double ry) {
        return (X - cx) * (X - cx) / (rx * rx) + (Y - cy) * (Y - cy) / (ry * ry) <= 1;
    };
    const auto triangle = [&](Point a, Point b, Point c) {
        const auto side = [&](Point p, Point q) { return (q.x - p.x) * (Y - p.y) - (q.y - p.y) * (X - p.x); };
        const double d1 = side(a, b), d2 = side(b, c), d3 = side(c, a);
        return (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
    };
    // (the beak: a band across the face, pointed below)
    // (its body the icon's colour, the beak and the belly cut out: the picture behind them; the owner's choice)
    constexpr double kBody = 1;
    if (triangle({345, 212}, {455, 212}, {400, 256}) || (ellipse(400, 214, 56, 16) && Y >= 200)) return 0;
    if (ellipse(400, 352, 102, 112) && (Y > 300 || ellipse(400, 352, 92, 112))) return 0; // (the belly)
    const bool head = ellipse(400, 196, 100, 106) && Y <= 240;
    const bool neck = X >= 305 && X <= 495 && Y >= 190 && Y <= 320;
    const bool body = ellipse(400, 395, 152, 120);
    const bool flippers = triangle({302, 236}, {200, 398}, {262, 402}) || ellipse(232, 398, 32, 12) ||
                          triangle({498, 236}, {600, 398}, {538, 402}) || ellipse(568, 398, 32, 12);
    return head || neck || body || flippers ? kBody : 0.0;
}

// Windows' logo: four squares, a gap between them (the logo's: a twelfth of its width), as Simple Icons' 24 x 24.
bool windows(double x, double y) {
    const double gap = 2, side = (24 - gap) / 2;
    const auto in = [](double v, double a, double b) { return v >= a && v < b; };
    return (in(x, 0, side) || in(x, side + gap, 24)) && (in(y, 0, side) || in(y, side + gap, 24));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: platform_icons OUT.png\n");
        return 1;
    }
    const Path apple(kApplePath), android(kAndroidPath);
    // (in platforms.rcss's order: platform-windows, -macos, -linux, -android)
    const std::function<double(double, double)> icons[] = {
        windows,
        [&](double x, double y) { return inside(apple.polys(), x, y); },
        flat_penguin,
        [&](double x, double y) { return inside(android.polys(), x, y); },
    };
    constexpr int kIcons = 4;
    const int w = kIcons * (kCell + 2), h = kCell + 2; // (a pixel of space round each, as flags.png's)
    std::vector<uint32_t> out(static_cast<std::size_t>(w) * h, 0);
    const double scale = (kCell - 2 * kInset) / 24.0;
    for (int i = 0; i < kIcons; ++i) {
        std::vector<double> cover(kCell * kCell), shade(kCell * kCell); // (how much covered, and how bright)
        for (int py = 0; py < kCell; ++py)
            for (int px = 0; px < kCell; ++px) {
                int n = 0;
                double lit = 0;
                for (int sy = 0; sy < kFine; ++sy)
                    for (int sx = 0; sx < kFine; ++sx)
                        if (const double v = icons[i]((px + (sx + 0.5) / kFine - kInset) / scale, (py + (sy + 0.5) / kFine - kInset) / scale); v > 0)
                            ++n, lit += v;
                const std::size_t k = static_cast<std::size_t>(py) * kCell + px;
                cover[k] = static_cast<double>(n) / (kFine * kFine), shade[k] = n ? lit / n : 0;
            }
        // (white over a black outline: the most covered pixel within kOutline px, fading over its last one)
        for (int py = 0; py < kCell; ++py)
            for (int px = 0; px < kCell; ++px) {
                double ring = 0;
                for (int dy = -3; dy <= 3; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= kCell || ny >= kCell) continue;
                        const double d = std::sqrt(double(dx * dx + dy * dy));
                        ring = std::max(ring, cover[static_cast<std::size_t>(ny) * kCell + nx] * std::clamp(kOutline + 0.2 - d, 0.0, 1.0));
                    }
                const double a = cover[static_cast<std::size_t>(py) * kCell + px], o = std::max(ring, a);
                const auto to8 = [](double v) { return static_cast<uint32_t>(std::clamp(v, 0.0, 255.0) + 0.5); };
                // (white (or its shade) as much as covered, the rest the outline's black)
                const uint32_t v = o > 0 ? to8(255 * shade[static_cast<std::size_t>(py) * kCell + px] * a / o) : 0;
                out[static_cast<std::size_t>(py + 1) * w + i * (kCell + 2) + px + 1] = to8(o * 255) << 24 | v << 16 | v << 8 | v;
            }
    }
    if (!tools::write_png(argv[1], out, static_cast<uint32_t>(w), static_cast<uint32_t>(h), true)) return 1;
    std::printf("%s: %d icons, %d x %d\n", argv[1], kIcons, w, h);
    return 0;
}
