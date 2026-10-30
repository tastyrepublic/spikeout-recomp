#include "net/compress.hpp"

#include <zstd.h>

namespace net {

// Long-distance matching over a 2^27 window (128 MB, the most a reader takes by default): every
// cabinet's 8 MB of main RAM is mostly the shared world, the same as the others', but 8 MB and
// more apart in the state, too far for the plain mode's window. Measured (2026-10-30, level 3):
// two cabinets 10,686 KB packed plain, 5,803 KB long; four 22,463 KB, 6,671 KB, packed faster.
std::vector<uint8_t> compress(std::span<const uint8_t> data, int level) {
    std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
    ZSTD_CCtx* c = ZSTD_createCCtx();
    if (!c) return {};
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
    ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, 27); // (zstd's ZSTD_WINDOWLOG_LIMIT_DEFAULT)
    const std::size_t n = ZSTD_compress2(c, out.data(), out.size(), data.data(), data.size());
    ZSTD_freeCCtx(c);
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
