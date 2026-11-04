// A small zip reader: see zip.hpp. The inflate follows the structure of
// zlib's puff.c (Mark Adler; zlib license), with a lookup table for the
// short codes in front of its bit-by-bit decode.
#include "runtime/zip.hpp"

#include "runtime/rom.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace m3 {

namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

constexpr int kMaxBits = 15;
constexpr int kFastBits = 10;

struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, 288> symbol{};
    // (the codes of up to kFastBits bits, by the next bits as read: the symbol << 4 | its length; 0 none)
    std::array<uint16_t, 1u << kFastBits> fast{};

    // From each symbol's code length; the codes left over (0: complete, < 0: too many).
    int build(const uint8_t* lengths, int n) {
        count.fill(0);
        for (int s = 0; s < n; ++s) ++count[lengths[s]];
        if (count[0] == n) return 0; // (no codes: complete, a block that can't use them)
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return left;
        }
        std::array<uint16_t, kMaxBits + 1> offs{};
        for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
        for (int s = 0; s < n; ++s)
            if (lengths[s] != 0) symbol[offs[lengths[s]]++] = static_cast<uint16_t>(s);
        // the canonical codes, each reversed (the stream holds them first bit first)
        fast.fill(0);
        int code = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            for (int i = 0; i < count[len]; ++i, ++code, ++index) {
                if (len > kFastBits) continue;
                int rev = 0;
                for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1) << (len - 1 - b);
                for (int k = rev; k < (1 << kFastBits); k += 1 << len)
                    fast[static_cast<std::size_t>(k)] = static_cast<uint16_t>(symbol[static_cast<std::size_t>(index)] << 4 | len);
            }
            code <<= 1;
        }
        return left;
    }
};

struct Inflater {
    const uint8_t* in;
    std::size_t in_size, in_pos = 0;
    uint64_t bitbuf = 0;
    int bitcnt = 0;
    uint8_t* out;
    std::size_t out_size, out_pos = 0;

    struct Bad {};

    void fill() {
        while (bitcnt <= 56 && in_pos < in_size) {
            bitbuf |= static_cast<uint64_t>(in[in_pos++]) << bitcnt;
            bitcnt += 8;
        }
    }
    int bits(int need) {
        if (bitcnt < need) {
            fill();
            if (bitcnt < need) throw Bad{};
        }
        const int v = static_cast<int>(bitbuf & ((uint64_t{1} << need) - 1));
        bitbuf >>= need;
        bitcnt -= need;
        return v;
    }

    int decode(const Huffman& h) {
        if (bitcnt < kMaxBits) fill();
        const uint16_t f = h.fast[bitbuf & ((1u << kFastBits) - 1)];
        if (f != 0) {
            const int len = f & 15;
            if (len > bitcnt) throw Bad{};
            bitbuf >>= len;
            bitcnt -= len;
            return f >> 4;
        }
        int code = 0, first = 0, index = 0; // (puff's decode, for the longer codes)
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            const int count = h.count[static_cast<std::size_t>(len)];
            if (code - count < first) return h.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Bad{};
    }

    void codes(const Huffman& lencode, const Huffman& distcode) {
        static constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (symbol < 256) {
                if (out_pos >= out_size) throw Bad{};
                out[out_pos++] = static_cast<uint8_t>(symbol);
            } else if (symbol == 256) {
                return;
            } else {
                symbol -= 257;
                if (symbol >= 29) throw Bad{};
                const std::size_t len = kLenBase[symbol] + static_cast<std::size_t>(bits(kLenExtra[symbol]));
                const int d = decode(distcode);
                if (d >= 30) throw Bad{};
                const std::size_t dist = kDistBase[d] + static_cast<std::size_t>(bits(kDistExtra[d]));
                if (dist > out_pos || len > out_size - out_pos) throw Bad{};
                uint8_t* to = out + out_pos;
                const uint8_t* from = to - dist;
                for (std::size_t i = 0; i < len; ++i) to[i] = from[i]; // (may overlap: byte by byte)
                out_pos += len;
            }
        }
    }

    void fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> t;
            uint8_t lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            t.first.build(lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            t.second.build(lengths, 30);
            return t;
        }();
        codes(tables.first, tables.second);
    }

    void dynamic() {
        static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) throw Bad{};
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<uint8_t>(bits(3));
        Huffman lencode, distcode;
        if (lencode.build(lengths, 19) != 0) throw Bad{};
        int index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (symbol < 16) {
                lengths[index++] = static_cast<uint8_t>(symbol);
                continue;
            }
            uint8_t len = 0;
            if (symbol == 16) {
                if (index == 0) throw Bad{};
                len = lengths[index - 1];
                symbol = 3 + bits(2);
            } else if (symbol == 17) {
                symbol = 3 + bits(3);
            } else {
                symbol = 11 + bits(7);
            }
            if (index + symbol > nlen + ndist) throw Bad{};
            while (symbol--) lengths[index++] = len;
        }
        if (lengths[256] == 0) throw Bad{};
        int err = lencode.build(lengths, nlen);
        if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) throw Bad{};
        err = distcode.build(lengths + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) throw Bad{};
        codes(lencode, distcode);
    }

    // A stored block: to the next byte's start in the stream (the bits read ahead given back), its length, its bytes.
    void stored_block() {
        const int drop = bitcnt & 7;
        bitbuf >>= drop;
        bitcnt -= drop;
        auto byte = [&]() -> std::size_t {
            if (bitcnt >= 8) {
                const auto v = static_cast<std::size_t>(bitbuf & 0xFF);
                bitbuf >>= 8;
                bitcnt -= 8;
                return v;
            }
            if (in_pos >= in_size) throw Bad{};
            return in[in_pos++];
        };
        const std::size_t len = byte() | byte() << 8;
        const std::size_t nlen = byte() | byte() << 8;
        if (len != (~nlen & 0xFFFF)) throw Bad{};
        if (len > out_size - out_pos) throw Bad{};
        for (std::size_t i = 0; i < len; ++i) out[out_pos++] = static_cast<uint8_t>(byte());
    }

    bool run() {
        try {
            for (bool last = false; !last;) {
                last = bits(1) != 0;
                switch (bits(2)) {
                case 0: stored_block(); break;
                case 1: fixed(); break;
                case 2: dynamic(); break;
                default: return false;
                }
            }
            return out_pos == out_size;
        } catch (const Bad&) {
            return false;
        }
    }
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

bool inflate(const uint8_t* in, std::size_t in_size, std::vector<uint8_t>& out, std::size_t size) {
    out.assign(size, 0);
    Inflater f{in, in_size, 0, 0, 0, out.data(), size, 0};
    return f.run();
}

ZipReader::ZipReader(const std::filesystem::path& path) : in_(path, std::ios::binary) {
    if (!in_) throw std::runtime_error("cannot open " + path.string());
    in_.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in_.tellg());
    // the end of the central directory: in the last 22 bytes and up to 64 KB of comment
    const uint64_t tail = std::min<uint64_t>(file_size, 22 + 0xFFFF);
    std::vector<uint8_t> end(static_cast<std::size_t>(tail));
    in_.seekg(static_cast<std::streamoff>(file_size - tail));
    in_.read(reinterpret_cast<char*>(end.data()), static_cast<std::streamsize>(tail));
    if (!in_ || tail < 22) throw std::runtime_error(path.string() + " is not a zip file");
    std::size_t at = end.size() - 22;
    while (le32(&end[at]) != 0x06054B50) {
        if (at == 0) throw std::runtime_error(path.string() + " is not a zip file");
        --at;
    }
    const uint16_t count = le16(&end[at + 10]);
    const uint32_t dir_size = le32(&end[at + 12]), dir_at = le32(&end[at + 16]);
    if (dir_at == 0xFFFFFFFF || static_cast<uint64_t>(dir_at) + dir_size > file_size)
        throw std::runtime_error(path.string() + ": a zip this reader can't read (zip64)");
    std::vector<uint8_t> dir(dir_size);
    in_.seekg(dir_at);
    in_.read(reinterpret_cast<char*>(dir.data()), static_cast<std::streamsize>(dir_size));
    if (!in_) throw std::runtime_error(path.string() + ": its directory can't be read");
    std::size_t p = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > dir.size() || le32(&dir[p]) != 0x02014B50) throw std::runtime_error(path.string() + ": a damaged directory");
        const uint8_t* h = &dir[p];
        const std::size_t name_len = le16(h + 28), extra = le16(h + 30), comment = le16(h + 32);
        if (p + 46 + name_len > dir.size()) throw std::runtime_error(path.string() + ": a damaged directory");
        std::string name(reinterpret_cast<const char*>(h + 46), name_len);
        p += 46 + name_len + extra + comment;
        if (name.empty() || name.back() == '/' || (le16(h + 8) & 1)) continue; // (folders; encrypted files)
        Entry e;
        e.name = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
        e.method = le16(h + 10);
        e.crc = le32(h + 16);
        e.packed = le32(h + 20);
        e.size = le32(h + 24);
        e.header = le32(h + 42);
        entries_.emplace(lower(e.name), e);
    }
}

std::vector<uint8_t> ZipReader::read(const Entry& e) {
    if (e.size == 0xFFFFFFFF || e.packed == 0xFFFFFFFF) throw std::runtime_error(e.name + ": zip64 in the zip");
    uint8_t local[30];
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(e.header));
    in_.read(reinterpret_cast<char*>(local), 30);
    if (!in_ || le32(local) != 0x04034B50) throw std::runtime_error(e.name + ": damaged in the zip");
    in_.seekg(static_cast<std::streamoff>(e.header + 30 + le16(local + 26) + le16(local + 28)));
    std::vector<uint8_t> packed(static_cast<std::size_t>(e.packed));
    in_.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
    if (!in_) throw std::runtime_error(e.name + ": cut short in the zip");
    std::vector<uint8_t> data;
    if (e.method == 0) {
        if (e.packed != e.size) throw std::runtime_error(e.name + ": damaged in the zip");
        data = std::move(packed);
    } else if (e.method == 8) {
        if (!inflate(packed.data(), packed.size(), data, static_cast<std::size_t>(e.size)))
            throw std::runtime_error(e.name + ": damaged in the zip");
    } else {
        throw std::runtime_error(e.name + ": packed in a way this reader can't unpack (method " + std::to_string(e.method) + ")");
    }
    if (crc32(data) != e.crc) throw std::runtime_error(e.name + ": damaged in the zip (its CRC differs)");
    return data;
}

} // namespace m3
