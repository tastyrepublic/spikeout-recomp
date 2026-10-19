#include "analysis/code_image.hpp"

#include <stdexcept>
#include <utility>

namespace analysis {

void CodeImage::add(std::string name, uint32_t base, std::vector<uint8_t> bytes) {
    Segment seg{std::move(name), base, std::move(bytes)};
    for (const Segment& s : segments_)
        if (seg.base < s.end() && s.base < seg.end())
            throw std::invalid_argument("CodeImage: segment '" + seg.name + "' overlaps '" + s.name + "'");
    segments_.push_back(std::move(seg));
}

const Segment* CodeImage::segment_at(uint32_t addr) const {
    for (const Segment& s : segments_)
        if (s.contains(addr)) return &s;
    return nullptr;
}

std::optional<uint32_t> CodeImage::read32(uint32_t addr) const {
    if (addr & 3) return std::nullopt;
    return read_data32(addr);
}

std::optional<uint32_t> CodeImage::read_data32(uint32_t addr) const {
    const Segment* s = segment_at(addr);
    if (!s || !s->contains(addr, 4)) return std::nullopt;
    const uint8_t* p = s->bytes.data() + (addr - s->base);
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

} // namespace analysis
