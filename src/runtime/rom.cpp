#include "runtime/rom.hpp"

#include "ppc/instruction.hpp"
#include "runtime/memory_map.hpp"
#include "runtime/zip.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>

namespace m3 {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Case-insensitive index of the regular files in a directory.
std::map<std::string, std::filesystem::path> index_directory(const std::filesystem::path& dir) {
    std::map<std::string, std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_regular_file()) files.emplace(lower(entry.path().filename().string()), entry.path());
    return files;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}

constexpr auto kCrcTable = make_crc_table();

uint32_t be32(std::span<const uint8_t> data, size_t offset) {
    return (uint32_t{data[offset]} << 24) | (uint32_t{data[offset + 1]} << 16) |
           (uint32_t{data[offset + 2]} << 8) | uint32_t{data[offset + 3]};
}

} // namespace

std::string_view rom_region_name(RomRegionId id) {
    switch (id) {
    case RomRegionId::Crom:         return "crom";
    case RomRegionId::BankedCrom:   return "banked_crom";
    case RomRegionId::Vrom:         return "vrom";
    case RomRegionId::SoundProgram: return "sound_program";
    case RomRegionId::SoundSamples: return "sound_samples";
    case RomRegionId::MpegProgram:  return "mpeg_program";
    case RomRegionId::MpegMusic:    return "mpeg_music";
    case RomRegionId::Count:        break;
    }
    return "?";
}

std::string_view word_order_name(WordOrder order) {
    switch (order) {
    case WordOrder::AsIs:      return "as-is";
    case WordOrder::Swap16:    return "swap16";
    case WordOrder::Swap32:    return "swap32";
    case WordOrder::Reverse32: return "reverse32";
    }
    return "?";
}

uint32_t crc32(std::span<const uint8_t> data) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : data) c = kCrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void place_rom_file(const RomRegionSpec& spec, const RomFile& file, std::span<const uint8_t> data,
                    std::span<uint8_t> region) {
    const size_t chunk = spec.chunk_size;
    const size_t chunks = data.size() / chunk;
    if (data.size() % chunk != 0) throw std::invalid_argument("ROM size is not a multiple of chunk size");
    if (chunks != 0 && file.offset + (chunks - 1) * spec.stride + chunk > region.size())
        throw std::out_of_range("ROM file does not fit in its region");

    // Byte swapping is defined on the file's own 16-bit words, before interleaving.
    std::vector<uint8_t> swapped;
    if (spec.byte_swap) {
        swapped.assign(data.begin(), data.end());
        apply_word_order(swapped, WordOrder::Swap16);
        data = swapped;
    }

    for (size_t i = 0; i < chunks; ++i) {
        const uint8_t* src = data.data() + i * chunk;
        std::copy(src, src + chunk, region.data() + file.offset + i * spec.stride);
    }
}

void apply_word_order(std::span<uint8_t> data, WordOrder order) {
    switch (order) {
    case WordOrder::AsIs:
        return;
    case WordOrder::Swap16:
        for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
        return;
    case WordOrder::Swap32:
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
            std::swap(data[i], data[i + 2]);
            std::swap(data[i + 1], data[i + 3]);
        }
        return;
    case WordOrder::Reverse32:
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
            std::swap(data[i], data[i + 3]);
            std::swap(data[i + 1], data[i + 2]);
        }
        return;
    }
}

WordOrder detect_program_word_order(std::span<const uint8_t> fixed_crom) {
    constexpr size_t kWindow = 64; // instructions examined at the reset vector
    const size_t start = kResetVector - kFixedCromBase;
    if (fixed_crom.size() < start + kWindow * 4) return WordOrder::AsIs;

    std::vector<uint8_t> probe(fixed_crom.begin() + start, fixed_crom.begin() + start + kWindow * 4);
    WordOrder best = WordOrder::AsIs;
    int best_score = -1;
    for (WordOrder order : {WordOrder::AsIs, WordOrder::Swap16, WordOrder::Swap32, WordOrder::Reverse32}) {
        std::vector<uint8_t> candidate = probe;
        apply_word_order(candidate, order);
        int score = 0;
        for (size_t i = 0; i < kWindow; ++i) {
            const uint32_t word = be32(candidate, i * 4);
            // Padding words decode as invalid or as a run of identical
            // instructions; neither says anything about the byte order.
            if (word != 0 && word != 0xFFFFFFFF && ppc::decode(word).valid()) ++score;
        }
        if (score > best_score) {
            best_score = score;
            best = order;
        }
    }
    return best;
}

bool RomSet::complete() const {
    return std::all_of(report.begin(), report.end(), [](const RomFileReport& r) {
        return r.status == RomFileStatus::Ok || r.status == RomFileStatus::WrongChecksum;
    });
}

uint32_t RomSet::read_program32(uint32_t addr, uint32_t bank) const {
    if (addr >= kFixedCromBase) {
        const auto& crom = region(RomRegionId::Crom);
        const size_t off = addr - kFixedCromBase;
        return off + 4 <= crom.size() ? be32(crom, off) : 0;
    }
    if (addr >= kBankedCromBase) {
        const auto& banked = region(RomRegionId::BankedCrom);
        const size_t off = size_t{bank} * kCromWindowSize + (addr - kBankedCromBase);
        return off + 4 <= banked.size() ? be32(banked, off) : 0;
    }
    return 0;
}

namespace {

bool is_zip(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && lower(path.extension().string()) == ".zip";
}

// The files of a folder or a zip, by lower-case name: each one's size, and its bytes when asked.
class RomSource {
public:
    explicit RomSource(const std::filesystem::path& path) {
        if (is_zip(path)) {
            zip_.emplace(path);
            return;
        }
        files_ = index_directory(path);
    }
    // A zip's own record of a file's CRC (a folder: none, without reading the file).
    std::optional<uint32_t> recorded_crc(const std::string& name) const {
        if (!zip_) return std::nullopt;
        const auto it = zip_->entries().find(name);
        return it == zip_->entries().end() ? std::nullopt : std::optional<uint32_t>(it->second.crc);
    }
    std::optional<uint64_t> size(const std::string& name) const {
        if (zip_) {
            const auto it = zip_->entries().find(name);
            return it == zip_->entries().end() ? std::nullopt : std::optional<uint64_t>(it->second.size);
        }
        const auto it = files_.find(name);
        if (it == files_.end()) return std::nullopt;
        std::error_code ec;
        const auto n = std::filesystem::file_size(it->second, ec);
        return ec ? std::nullopt : std::optional<uint64_t>(n);
    }
    std::vector<uint8_t> read(const std::string& name) {
        if (zip_) return zip_->read(zip_->entries().at(name));
        return read_file(files_.at(name));
    }

private:
    std::optional<ZipReader> zip_;
    std::map<std::string, std::filesystem::path> files_;
};

} // namespace

std::vector<RomFileReport> survey_rom_set(const GameSpec& game, const std::filesystem::path& path) {
    std::vector<RomFileReport> report;
    std::optional<RomSource> source;
    try {
        source.emplace(path);
    } catch (const std::exception&) {
    }
    for (const RomRegionSpec& spec : game.regions)
        for (const RomFile& file : spec.files) {
            RomFileReport rep{spec.id, std::string(file.name), RomFileStatus::Ok, 0, 0, file.crc32};
            const auto size = source ? source->size(lower(file.name)) : std::nullopt;
            if (!size) rep.status = RomFileStatus::Missing;
            else if ((rep.actual_size = *size) != file.size) rep.status = RomFileStatus::WrongSize;
            else if (const auto crc = source->recorded_crc(lower(file.name)); crc && (rep.actual_crc = *crc) != file.crc32)
                rep.status = RomFileStatus::WrongChecksum;
            report.push_back(rep);
        }
    return report;
}

RomSet load_rom_set(const GameSpec& game, const std::filesystem::path& dir) {
    RomSet set;
    RomSource files(dir);

    for (const RomRegionSpec& spec : game.regions) {
        auto& buffer = set.region(spec.id);
        buffer.assign(spec.size, 0);
        for (const RomFile& file : spec.files) {
            RomFileReport rep{spec.id, std::string(file.name), RomFileStatus::Ok, 0, 0, file.crc32};
            const std::string name = lower(file.name);
            if (!files.size(name)) {
                rep.status = RomFileStatus::Missing;
                set.report.push_back(rep);
                continue;
            }
            const std::vector<uint8_t> data = files.read(name);
            rep.actual_size = data.size();
            rep.actual_crc = crc32(data);
            if (data.size() != file.size) {
                rep.status = RomFileStatus::WrongSize;
            } else {
                if (rep.actual_crc != file.crc32) rep.status = RomFileStatus::WrongChecksum;
                place_rom_file(spec, file, data, buffer);
            }
            set.report.push_back(rep);
        }
    }

    set.program_word_order = detect_program_word_order(set.region(RomRegionId::Crom));
    apply_word_order(set.region(RomRegionId::Crom), set.program_word_order);
    apply_word_order(set.region(RomRegionId::BankedCrom), set.program_word_order);
    return set;
}

} // namespace m3
