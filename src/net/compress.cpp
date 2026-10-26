#include "net/compress.hpp"

#include <zstd.h>

namespace net {

std::vector<uint8_t> compress(std::span<const uint8_t> data, int level) {
    std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
    const std::size_t n = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

std::optional<std::vector<uint8_t>> decompress(std::span<const uint8_t> data) {
    const unsigned long long size = ZSTD_getFrameContentSize(data.data(), data.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (1ull << 31)) return std::nullopt;
    std::vector<uint8_t> out(static_cast<std::size_t>(size));
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(n) || n != out.size()) return std::nullopt;
    return out;
}

} // namespace net
