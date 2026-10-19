// Helpers used by the generated code: flag computation for the integer
// instructions. Kept inline and branch-light; the generated code does the
// register moves itself.
#pragma once

#include "recomp/context.hpp"

#include <cstdint>

namespace recomp::ops {

using ppc::CpuState;

constexpr uint32_t kSO = CpuState::kXerSO;
constexpr uint32_t kOV = CpuState::kXerOV;
constexpr uint32_t kCA = CpuState::kXerCA;

inline uint32_t ca(const CpuState& s) { return (s.xer >> 29) & 1; }
inline uint32_t so(const CpuState& s) { return s.xer >> 31; }

inline void set_ca(CpuState& s, bool carry) { s.xer = carry ? (s.xer | kCA) : (s.xer & ~kCA); }
inline void set_ov(CpuState& s, bool overflow) {
    s.xer = overflow ? (s.xer | kOV | kSO) : (s.xer & ~kOV);
}

// CR field from a signed or unsigned comparison: LT GT EQ, plus XER[SO].
inline void cmps(CpuState& s, uint32_t field, int32_t a, int32_t b) {
    s.set_crf(field, (a < b ? 8u : a > b ? 4u : 2u) | so(s));
}
inline void cmpu(CpuState& s, uint32_t field, uint32_t a, uint32_t b) {
    s.set_crf(field, (a < b ? 8u : a > b ? 4u : 2u) | so(s));
}
inline void set_cr0(CpuState& s, uint32_t result) { cmps(s, 0, static_cast<int32_t>(result), 0); }

// x + y + carry_in, the core of add/addc/adde/addme/addze and (with x = ~a,
// carry_in = 1 or CA) of subf/subfc/subfe/subfme/subfze/neg/subfic.
// `record_ca` sets XER[CA]; `oe` sets XER[OV] (and SO when it overflows).
inline uint32_t addx(CpuState& s, uint32_t x, uint32_t y, uint32_t carry_in, bool record_ca, bool oe) {
    const uint64_t wide = uint64_t{x} + y + carry_in;
    const uint32_t r = static_cast<uint32_t>(wide);
    if (record_ca) set_ca(s, (wide >> 32) != 0);
    if (oe) set_ov(s, ((x ^ r) & (y ^ r)) >> 31);
    return r;
}

inline uint32_t mullw(CpuState& s, uint32_t a, uint32_t b, bool oe) {
    const int64_t p = int64_t{static_cast<int32_t>(a)} * static_cast<int32_t>(b);
    if (oe) set_ov(s, p != static_cast<int32_t>(p));
    return static_cast<uint32_t>(p);
}
inline uint32_t mulhw(uint32_t a, uint32_t b) {
    const int64_t p = int64_t{static_cast<int32_t>(a)} * static_cast<int32_t>(b);
    return static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
}
inline uint32_t mulhwu(uint32_t a, uint32_t b) { return static_cast<uint32_t>((uint64_t{a} * b) >> 32); }

// The result of an invalid division is undefined; this follows what the
// 603e is reported to do (and what MAME returns): 0, or -1 for a negative
// dividend in divw.
inline uint32_t divw(CpuState& s, uint32_t a, uint32_t b, bool oe) {
    const int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
    const bool invalid = y == 0 || (a == 0x80000000u && y == -1);
    if (oe) set_ov(s, invalid);
    if (invalid) return x < 0 ? 0xFFFFFFFFu : 0;
    return static_cast<uint32_t>(x / y);
}
inline uint32_t divwu(CpuState& s, uint32_t a, uint32_t b, bool oe) {
    if (oe) set_ov(s, b == 0);
    return b == 0 ? 0 : a / b;
}

inline uint32_t rotl(uint32_t x, uint32_t n) {
    n &= 31;
    return n == 0 ? x : (x << n) | (x >> (32 - n));
}

inline uint32_t slw(uint32_t s_, uint32_t b) { return (b & 0x20) ? 0 : s_ << (b & 31); }
inline uint32_t srw(uint32_t s_, uint32_t b) { return (b & 0x20) ? 0 : s_ >> (b & 31); }

// Shift right algebraic; CA is set if the value is negative and any 1 bits
// were shifted out.
inline uint32_t sraw(CpuState& s, uint32_t value, uint32_t n) {
    const int32_t v = static_cast<int32_t>(value);
    if (n >= 32) {
        set_ca(s, v < 0);
        return v < 0 ? 0xFFFFFFFFu : 0;
    }
    if (n == 0) {
        set_ca(s, false);
        return value;
    }
    set_ca(s, v < 0 && (value & ((1u << n) - 1)) != 0);
    return static_cast<uint32_t>(v >> n);
}

inline uint32_t cntlzw(uint32_t v) {
    uint32_t n = 0;
    for (uint32_t bit = 0x80000000u; bit && !(v & bit); bit >>= 1) ++n;
    return n;
}

inline uint32_t bswap16(uint32_t v) { return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }

// tw/twi condition: TO bits 16 lt, 8 gt, 4 eq (signed), 2 lt, 1 gt (unsigned).
inline bool trap_if(uint32_t to, uint32_t a, uint32_t b) {
    const int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
    return ((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && a == b) || ((to & 2) && a < b) ||
           ((to & 1) && a > b);
}

} // namespace recomp::ops
