// Architectural state of the PowerPC 603e as seen by recompiled code.
//
// This is a plain, trivially copyable struct on purpose: save states (and the
// rollback netcode built on them) snapshot it with a single memcpy.
#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace ppc {

struct CpuState {
    // User-level registers
    std::array<uint32_t, 32> gpr{};
    std::array<uint64_t, 32> fpr{}; // raw IEEE-754 double bits
    uint32_t cr = 0;
    uint32_t xer = 0;
    uint32_t lr = 0;
    uint32_t ctr = 0;
    uint32_t fpscr = 0;
    uint32_t pc = 0;

    // Supervisor registers
    uint32_t msr = 0;
    uint32_t srr0 = 0;
    uint32_t srr1 = 0;
    std::array<uint32_t, 4> sprg{};
    uint32_t dsisr = 0;
    uint32_t dar = 0;
    uint32_t dec = 0;
    uint32_t sdr1 = 0;
    uint32_t ear = 0;
    uint64_t tb = 0;
    std::array<uint32_t, 16> sr{};
    std::array<uint32_t, 8> ibat{}; // ibat0u, ibat0l, ibat1u, ...
    std::array<uint32_t, 8> dbat{};

    // 603-specific
    uint32_t hid0 = 0;
    uint32_t hid1 = 0;
    uint32_t iabr = 0;
    uint32_t dmiss = 0, dcmp = 0, hash1 = 0, hash2 = 0;
    uint32_t imiss = 0, icmp = 0, rpa = 0;

    // Reservation for lwarx/stwcx.
    uint32_t reserve_addr = 0;
    bool reserve = false;

    // CR field n (0-7) as a 4-bit value: LT GT EQ SO.
    constexpr uint32_t crf(uint32_t n) const { return (cr >> (28 - 4 * n)) & 0xF; }
    constexpr void set_crf(uint32_t n, uint32_t v) {
        const uint32_t shift = 28 - 4 * n;
        cr = (cr & ~(0xFu << shift)) | ((v & 0xF) << shift);
    }
    // CR bit n (0 = MSB, as in the manuals).
    constexpr bool crbit(uint32_t n) const { return (cr >> (31 - n)) & 1; }
    constexpr void set_crbit(uint32_t n, bool v) {
        const uint32_t mask = 1u << (31 - n);
        cr = v ? (cr | mask) : (cr & ~mask);
    }

    static constexpr uint32_t kXerSO = 1u << 31;
    static constexpr uint32_t kXerOV = 1u << 30;
    static constexpr uint32_t kXerCA = 1u << 29;

    static constexpr uint32_t kMsrIP = 1u << 6; // exception prefix (0xFFF00000 when set)

    // Power-on state: MSR[IP] set, execution starts at the system reset vector.
    void reset() {
        *this = CpuState{};
        msr = kMsrIP;
        pc = 0xFFF00100;
    }
};

static_assert(std::is_trivially_copyable_v<CpuState>);

} // namespace ppc
