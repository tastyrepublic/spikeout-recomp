#include "runtime/real3d.hpp"
#include "runtime/state.hpp"

#include <algorithm>

namespace m3 {

namespace {

constexpr uint32_t kCullingLowWords = 0x400000 / 4, kCullingHighWords = 0x100000 / 4, kPolygonWords = 0x400000 / 4;
constexpr size_t kFifoWords = 0x100000 / 4;

// Texels arrive in tiles of up to 8x8, in this order: pairs swapped, rows
// two at a time (Supermodel's decode8x8/8x4/8x2 tables, as a formula).
uint32_t tile_order(uint32_t row, uint32_t col, uint32_t tile_width) {
    return (row / 2) * (2 * tile_width) + (row % 2) * 2 + (col / 2) * 4 + ((col % 2) ^ 1);
}

} // namespace

Real3d::Real3d(std::span<const uint8_t> vrom)
    : vrom_(vrom), culling_lo_(kCullingLowWords), culling_hi_(kCullingHighWords), polygon_(kPolygonWords),
      texture_(size_t{kTextureWidth} * kTextureHeight) {
    fifo_.reserve(kFifoWords);
}

uint32_t Real3d::culling(uint32_t addr) const {
    const uint32_t* p = culling_ptr(addr);
    return p ? *p : 0;
}

const uint32_t* Real3d::culling_ptr(uint32_t addr) const {
    addr &= 0xFFFFFF;
    if (addr >= 0x800000 && addr < 0x800000 + kCullingHighWords) return &culling_hi_[addr - 0x800000];
    if (addr < kCullingLowWords) return &culling_lo_[addr];
    return nullptr;
}

uint32_t Real3d::model(uint32_t addr) const {
    addr &= 0xFFFFFF;
    if (addr < 0x100000) return addr < polygon_.size() ? polygon_[addr] : 0;
    const size_t at = size_t{addr} * 4;
    if (at + 3 >= vrom_.size()) return 0;
    // VROM as the ROM set loader lays it out (as Supermodel does): 32-bit words, little-endian.
    return uint32_t{vrom_[at]} | uint32_t{vrom_[at + 1]} << 8 | uint32_t{vrom_[at + 2]} << 16 | uint32_t{vrom_[at + 3]} << 24;
}

void Real3d::write_texture_fifo(uint32_t value) {
    if (fifo_.size() < kFifoWords) fifo_.push_back(swap(value));
}

void Real3d::write_command(uint32_t mercury_mode) {
    command_written_ = true;
    if (flipped_) return; // after the flip: at the start of VBlank
    flush_textures();
    if (tilegen_drew_) draw_frame(mercury_mode);
}

void Real3d::tilegen_draw_frame(uint32_t mercury_mode) {
    tilegen_drew_ = true;
    if (!flipped_ && command_written_) draw_frame(mercury_mode);
}

// Both signals for a frame: it takes the block culling bit then.
void Real3d::draw_frame(uint32_t mercury_mode) {
    tilegen_drew_ = command_written_ = false;
    block_culling_ = (mercury_mode & 4) != 0;
}

// Signals from here on are for the next frame.
void Real3d::ping_pong_flipped() {
    flipped_ = true;
    tilegen_drew_ = command_written_ = false;
}

void Real3d::write_culling_high(uint32_t offset, uint32_t value) {
    if (offset / 4 >= culling_hi_.size()) return;
    if (buffering()) buffered_write(false, culling_hi_, offset / 4, swap(value));
    else culling_hi_[offset / 4] = swap(value);
}

void Real3d::write_polygon(uint32_t offset, uint32_t value) {
    if (offset / 4 >= polygon_.size()) return;
    if (buffering()) buffered_write(true, polygon_, offset / 4, swap(value));
    else polygon_[offset / 4] = swap(value);
}

// High culling RAM's buffer is low culling RAM up to configuration register
// 0's size, polygon RAM's from there up to register 8's (sizes in words).
void Real3d::buffer_range(bool polygon, uint32_t& begin, uint32_t& end) const {
    const uint32_t size = static_cast<uint32_t>(culling_lo_.size());
    const uint32_t first = std::min(config_[0], size), second = std::min(config_[2], size);
    begin = polygon ? first : 0;
    end = polygon ? second : first;
}

void Real3d::buffered_write(bool polygon, WatchedVector<uint32_t>& target, uint32_t word, uint32_t value) {
    UpdateBuffer& buf = polygon ? polygon_buffer_ : high_buffer_;
    uint32_t begin, end;
    buffer_range(polygon, begin, end);
    if (!buf.used) {
        buf.cursor = begin;
        buf.block = UINT32_MAX;
        buf.used = true;
    }
    if (buf.block != UINT32_MAX && culling_lo_[buf.block + 1] + 1 == word) { // extends the current block
        if (buf.cursor + 1 <= end) {
            culling_lo_[buf.cursor++] = value;
            culling_lo_[buf.block + 1] = word;
            return;
        }
    } else if (buf.cursor + 3 <= end) { // a new block: first word, last word, data
        buf.block = buf.cursor;
        culling_lo_[buf.cursor++] = word;
        culling_lo_[buf.cursor++] = word;
        culling_lo_[buf.cursor++] = value;
        return;
    }
    // Full: apply what's buffered so this write stays after it, then write.
    apply(polygon, target);
    target[word] = value;
}

void Real3d::apply(bool polygon, WatchedVector<uint32_t>& target) {
    UpdateBuffer& buf = polygon ? polygon_buffer_ : high_buffer_;
    if (!buf.used) return;
    uint32_t begin, end;
    buffer_range(polygon, begin, end);
    for (uint32_t i = begin; i + 2 <= buf.cursor;) {
        const uint32_t first = culling_lo_[i], last = culling_lo_[i + 1];
        const uint32_t count = last - first + 1;
        for (uint32_t k = 0; k < count && i + 2 + k < buf.cursor; ++k)
            if (first + k < target.size()) target[first + k] = culling_lo_[i + 2 + k];
        i += 2 + count;
    }
    buf = {};
}

void Real3d::begin_vblank(uint32_t mercury_mode) {
    apply(false, culling_hi_);
    apply(true, polygon_);
    if (command_written_) flush_textures();
    if (tilegen_drew_ && command_written_) draw_frame(mercury_mode);
    flipped_ = false;
}

std::vector<std::pair<uint32_t, uint32_t>> Real3d::take_written_rows() {
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (uint32_t y = 0; y < kTextureHeight;) {
        if (!rows_written_[y]) {
            ++y;
            continue;
        }
        uint32_t end = y;
        while (end < kTextureHeight && rows_written_[end]) rows_written_[end++] = 0;
        runs.emplace_back(y, end - y);
        y = end;
    }
    return runs;
}

void Real3d::flush_textures() {
    // The FIFO holds textures back to back: a length word (in 16-bit
    // units, roughly), the header, then the data.
    std::vector<uint16_t> data;
    for (size_t i = 0; i + 2 < fifo_.size();) {
        const size_t words = (2 + fifo_[i] / 2) / 4;
        if (words == 0) break; // Spikeout uploads some zero-length textures
        data.clear();
        for (size_t k = i + 2; k < std::min(fifo_.size(), i + words); ++k) {
            data.push_back(static_cast<uint16_t>(fifo_[k]));
            data.push_back(static_cast<uint16_t>(fifo_[k] >> 16));
        }
        upload_texture(fifo_[i + 1], data);
        i += words;
    }
    fifo_.clear();
}

// Texture header:
//   bits 0-5 x / 32, 7-11 y / 32, 14-16 width (32 << n), 17-19 height
//   (32 << n), 20 page (the second 1024 lines), 21/22 8-bit data into the
//   low / high byte of each texel, 23 16-bit texels, 24-31 type: 0 texture
//   and its mipmaps, 1 texture only, 2 mipmaps only (0x80: a gamma table?).
void Real3d::upload_texture(uint32_t header, std::span<const uint16_t> data) {
    uint32_t x = 32 * (header & 0x3F), y = 32 * ((header >> 7) & 0x1F);
    uint32_t width = 32u << ((header >> 14) & 7), height = 32u << ((header >> 17) & 7);
    const uint32_t page = (header >> 20) & 1, type = header >> 24;
    const bool low = (header >> 21) & 1, high = (header >> 22) & 1, sixteen = (header >> 23) & 1;
    if (type > 2) return;
    ++textures_uploaded_;
    size_t at = 0, used = 0;
    if (type != 2) {
        store(x, y + page * 1024, width, height, data, used, sixteen, low, high);
        at += used;
        if (type == 1) return;
    }
    // Mipmaps: level i is at (2048 - 2048 >> i, 1024 - 1024 >> i) + position >> i.
    for (uint32_t level = 1; width >= 4 && height >= 4 && level < 11; ++level) {
        width /= 2;
        height /= 2;
        const uint32_t mx = (2048 - (2048 >> level)) + (x >> level), my = (1024 - (1024 >> level)) + (y >> level);
        store(mx, my + page * 1024, width, height, data.subspan(std::min(at, data.size())), used, sixteen, low, high);
        at += used;
    }
}

void Real3d::store(uint32_t x0, uint32_t y0, uint32_t width, uint32_t height, std::span<const uint16_t> data,
                   size_t& used, bool sixteen_bit, bool low_byte, bool high_byte) {
    const uint32_t tw = std::min(8u, width), th = std::min(8u, height);
    auto get = [&](size_t i) -> uint16_t { return i < data.size() ? data[i] : 0; };
    used = 0;
    for (uint32_t r = 0; r < height && r < kTextureHeight; ++r) rows_written_[(y0 + r) & 2047] = 1;
    if (sixteen_bit) {
        for (uint32_t ty = y0; ty < y0 + height; ty += th)
            for (uint32_t tx = x0; tx < x0 + width; tx += tw) {
                for (uint32_t r = 0; r < th; ++r)
                    for (uint32_t c = 0; c < tw; ++c)
                        texture_[((ty + r) & 2047) * kTextureWidth + ((tx + c) & 2047)] = get(used + tile_order(r, c, tw));
                used += size_t{tw} * th;
            }
        return;
    }
    // 8-bit texels go into one byte of each 16-bit texel (repeated in both
    // bytes, the other byte kept), with the rows of the order swapped in pairs.
    static constexpr uint16_t kKeep[4] = {0xFFFF, 0xFF00, 0x00FF, 0x0000};
    const uint16_t keep = kKeep[(low_byte ? 1 : 0) | (high_byte ? 2 : 0)];
    const size_t step = std::max<size_t>(1, size_t{tw} * th / 2);
    for (uint32_t ty = y0; ty < y0 + height; ty += th)
        for (uint32_t tx = x0; tx < x0 + width; tx += tw) {
            if (low_byte || high_byte)
                for (uint32_t r = 0; r < th; ++r)
                    for (uint32_t c = 0; c < tw; ++c) {
                        const uint32_t index = (r ^ 1) * tw + (c ^ 1) - (tw & 1);
                        const uint32_t order = tile_order(index / tw, index % tw, tw);
                        const uint16_t byte = (get(used + order / 2) >> (8 * ((c & 1) ^ 1))) & 0xFF;
                        uint16_t& texel = texture_[((ty + r) & 2047) * kTextureWidth + ((tx + c) & 2047)];
                        texel = static_cast<uint16_t>((texel & keep) | ((byte | byte << 8) & ~keep));
                    }
            used += step;
        }
}

void Real3d::state(State& s) {
    s.tag("real3d");
    s.fixed(culling_lo_), s.fixed(culling_hi_), s.fixed(polygon_), s.fixed(texture_);
    for (UpdateBuffer* b : {&high_buffer_, &polygon_buffer_}) s.io(b->cursor), s.io(b->block), s.io(b->used);
    s.io(flipped_), s.io(command_written_), s.io(tilegen_drew_), s.io(block_culling_);
    s.io(config_), s.io(fifo_), s.io(textures_uploaded_);
    if (s.loading()) std::fill(rows_written_.begin(), rows_written_.end(), uint8_t{1}); // a renderer's copy is stale
}

} // namespace m3
