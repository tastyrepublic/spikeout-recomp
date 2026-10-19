// Minimal WAV writer: 16-bit PCM, interleaved stereo.
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace tools {

inline bool write_wav(const std::string& path, std::span<const int16_t> samples, uint32_t rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const auto bytes = static_cast<uint32_t>(samples.size() * 2);
    auto le = [&](uint32_t v, int n) {
        for (int i = 0; i < n; ++i) std::fputc(static_cast<int>((v >> (8 * i)) & 0xFF), f);
    };
    std::fputs("RIFF", f);
    le(36 + bytes, 4);
    std::fputs("WAVEfmt ", f);
    le(16, 4);
    le(1, 2);        // PCM
    le(2, 2);        // channels
    le(rate, 4);
    le(rate * 4, 4); // bytes per second
    le(4, 2);        // bytes per frame
    le(16, 2);       // bits
    std::fputs("data", f);
    le(bytes, 4);
    for (int16_t s : samples) le(static_cast<uint16_t>(s), 2);
    return std::fclose(f) == 0;
}

} // namespace tools
