#include "net/digest.hpp"

#include <cstring>
#include <vector>

namespace net {

namespace {

constexpr uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

// The message padded to whole 64-byte blocks: a 1 bit, zeros, its length in
// bits (big-endian for SHA-1, little-endian for MD5).
std::vector<uint8_t> padded(std::span<const uint8_t> data, bool big_endian) {
    std::vector<uint8_t> m(data.begin(), data.end());
    const uint64_t bits = uint64_t{data.size()} * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 0; i < 8; ++i) m.push_back(static_cast<uint8_t>(bits >> (big_endian ? 56 - 8 * i : 8 * i)));
    return m;
}

} // namespace

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const std::vector<uint8_t> m = padded(data, true);
    for (std::size_t block = 0; block < m.size(); block += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &m[block + 4 * static_cast<std::size_t>(i)];
            w[i] = uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::array<uint8_t, 20> out;
    for (int i = 0; i < 20; ++i) out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    uint8_t k[64] = {};
    if (key.size() > 64) {
        const auto hashed = sha1(key);
        std::memcpy(k, hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    std::vector<uint8_t> inner(64), outer(64);
    for (int i = 0; i < 64; ++i) inner[static_cast<std::size_t>(i)] = k[i] ^ 0x36, outer[static_cast<std::size_t>(i)] = k[i] ^ 0x5C;
    inner.insert(inner.end(), data.begin(), data.end());
    const auto first = sha1(inner);
    outer.insert(outer.end(), first.begin(), first.end());
    return sha1(outer);
}

std::array<uint8_t, 16> md5(std::span<const uint8_t> data) {
    static constexpr uint32_t kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                   14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::vector<uint8_t> m = padded(data, false);
    for (std::size_t block = 0; block < m.size(); block += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &m[block + 4 * static_cast<std::size_t>(i)];
            w[i] = uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            const uint32_t t = d;
            d = c, c = b;
            b = b + rol(a + f + kK[i] + w[g], kS[i]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    std::array<uint8_t, 16> out;
    for (int i = 0; i < 16; ++i) out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(h[i / 4] >> (8 * (i % 4)));
    return out;
}

} // namespace net
