// The memory the CPU can execute code from, as seen by the static analysis:
// a few address ranges, each backed by bytes in the CPU's big-endian order.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace analysis {

struct Segment {
    std::string name;
    uint32_t base = 0;
    std::vector<uint8_t> bytes;

    // 64-bit so a segment ending at 0xFFFFFFFF doesn't wrap.
    uint64_t end() const { return uint64_t{base} + bytes.size(); }
    bool contains(uint32_t addr, uint32_t len = 1) const {
        return addr >= base && uint64_t{addr} + len <= end();
    }
};

class CodeImage {
public:
    // Segments must not overlap.
    void add(std::string name, uint32_t base, std::vector<uint8_t> bytes);

    const Segment* segment_at(uint32_t addr) const;
    std::optional<uint32_t> read32(uint32_t addr) const;
    // A data word at any alignment (tables can be packed: Spikeout has one at
    // 0xA1579); the CPU reads those fine in big-endian mode.
    std::optional<uint32_t> read_data32(uint32_t addr) const;
    bool contains(uint32_t addr) const { return segment_at(addr) != nullptr; }

    std::span<const Segment> segments() const { return segments_; }

private:
    std::vector<Segment> segments_;
};

} // namespace analysis
