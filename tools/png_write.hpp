// Minimal PNG writer for screenshots: 8-bit RGB, stored (uncompressed)
// deflate blocks. Pixels are 0xAARRGGBB; alpha is dropped (`alpha`: kept,
// RGBA: the UI's pictures).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace tools {

inline bool write_png(const std::string& path, std::span<const uint32_t> pixels, uint32_t width, uint32_t height,
                      bool alpha = false) {
    auto crc = [](const uint8_t* p, std::size_t n, uint32_t c = 0xFFFFFFFFu) {
        for (std::size_t i = 0; i < n; ++i) {
            c ^= p[i];
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        return c;
    };
    auto be32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
    };

    // Raw scanlines: filter byte 0, then RGB.
    std::vector<uint8_t> raw;
    raw.reserve(std::size_t{height} * (1 + std::size_t{width} * (alpha ? 4 : 3)));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t p = pixels[std::size_t{y} * width + x];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
            if (alpha) raw.push_back(static_cast<uint8_t>(p >> 24));
        }
    }
    // zlib stream of stored blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, b << 16 | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(out, static_cast<uint32_t>(data.size()));
        const std::size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        be32(out, crc(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, width);
    be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, static_cast<uint8_t>(alpha ? 6 : 2), 0, 0, 0}); // 8-bit RGB (RGBA)
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace tools
