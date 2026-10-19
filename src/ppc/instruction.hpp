// PowerPC 603e instruction representation and decoder.
//
// The Model 3 Step 2.x main CPU is a PowerPC 603ev: 32-bit, big-endian,
// with an FPU and the 603-specific software TLB reload instructions.
// Bit numbering in comments follows the PowerPC manuals (bit 0 = MSB).
#pragma once

#include <cstdint>
#include <optional>

namespace ppc {

// Instruction forms, used by the disassembler and (later) the lifter to know
// which fields an opcode uses.
enum class Form : uint8_t {
    None,       // sc, rfi, isync, sync, eieio, tlbia, tlbsync
    DAB,        // rD, rA, rB        (XO-form, OE + Rc)
    DABNoOE,    // rD, rA, rB        (mulhw/mulhwu: Rc only)
    DA,         // rD, rA            (XO-form, OE + Rc)
    DASimm,     // rD, rA, SIMM
    ASUimm,     // rA, rS, UIMM      (logical immediates)
    ASB,        // rA, rS, rB        (Rc)
    AS,         // rA, rS            (Rc)
    ASSh,       // rA, rS, SH        (srawi, Rc)
    Rlwi,       // rA, rS, SH, MB, ME (Rc)
    Rlwnm,      // rA, rS, rB, MB, ME (Rc)
    Cmp,        // crfD, rA, rB
    CmpSimm,    // crfD, rA, SIMM
    CmpUimm,    // crfD, rA, UIMM
    LoadStoreD, // rD, d(rA)
    FLoadStoreD,// frD, d(rA)
    LoadStoreX, // rD, rA, rB
    FLoadStoreX,// frD, rA, rB
    LoadStoreNB,// rD, rA, NB        (lswi/stswi)
    Cache,      // rA, rB
    Branch,     // b / ba / bl / bla
    BranchCond, // bc
    BranchLR,   // bclr
    BranchCTR,  // bcctr
    Trap,       // TO, rA, rB
    TrapImm,    // TO, rA, SIMM
    MfSpr,      // rD, SPR
    MtSpr,      // SPR, rS
    MfTb,       // rD, TBR
    MfReg,      // rD                (mfcr, mfmsr)
    MtReg,      // rS                (mtmsr)
    MtCrf,      // CRM, rS
    MfSr,       // rD, SR
    MtSr,       // SR, rS
    MfSrin,     // rD, rB
    MtSrin,     // rS, rB
    RB,         // rB                (tlbie, tlbld, tlbli)
    CrBit,      // crbD, crbA, crbB
    Mcrf,       // crfD, crfS
    Mcrxr,      // crfD
    FDAB,       // frD, frA, frB     (Rc)
    FDAC,       // frD, frA, frC     (Rc)
    FDACB,      // frD, frA, frC, frB (Rc)
    FDB,        // frD, frB          (Rc)
    FCmp,       // crfD, frA, frB
    Mffs,       // frD               (Rc)
    Mtfsf,      // FM, frB           (Rc)
    Mtfsfi,     // crfD, IMM         (Rc)
    Mtfsb,      // crbD              (Rc)
};

// X(enum name, mnemonic, form)
#define PPC_OPCODES(X)                                   \
    X(Invalid,  "(invalid)", None)                       \
    /* Integer arithmetic */                             \
    X(Add,      "add",      DAB)                         \
    X(Addc,     "addc",     DAB)                         \
    X(Adde,     "adde",     DAB)                         \
    X(Addi,     "addi",     DASimm)                      \
    X(Addic,    "addic",    DASimm)                      \
    X(AddicRc,  "addic.",   DASimm)                      \
    X(Addis,    "addis",    DASimm)                      \
    X(Addme,    "addme",    DA)                          \
    X(Addze,    "addze",    DA)                          \
    X(Divw,     "divw",     DAB)                         \
    X(Divwu,    "divwu",    DAB)                         \
    X(Mulhw,    "mulhw",    DABNoOE)                     \
    X(Mulhwu,   "mulhwu",   DABNoOE)                     \
    X(Mulli,    "mulli",    DASimm)                      \
    X(Mullw,    "mullw",    DAB)                         \
    X(Neg,      "neg",      DA)                          \
    X(Subf,     "subf",     DAB)                         \
    X(Subfc,    "subfc",    DAB)                         \
    X(Subfe,    "subfe",    DAB)                         \
    X(Subfic,   "subfic",   DASimm)                      \
    X(Subfme,   "subfme",   DA)                          \
    X(Subfze,   "subfze",   DA)                          \
    /* Integer compare */                                \
    X(Cmp,      "cmp",      Cmp)                         \
    X(Cmpi,     "cmpi",     CmpSimm)                     \
    X(Cmpl,     "cmpl",     Cmp)                         \
    X(Cmpli,    "cmpli",    CmpUimm)                     \
    /* Integer logical */                                \
    X(And,      "and",      ASB)                         \
    X(Andc,     "andc",     ASB)                         \
    X(AndiRc,   "andi.",    ASUimm)                      \
    X(AndisRc,  "andis.",   ASUimm)                      \
    X(Cntlzw,   "cntlzw",   AS)                          \
    X(Eqv,      "eqv",      ASB)                         \
    X(Extsb,    "extsb",    AS)                          \
    X(Extsh,    "extsh",    AS)                          \
    X(Nand,     "nand",     ASB)                         \
    X(Nor,      "nor",      ASB)                         \
    X(Or,       "or",       ASB)                         \
    X(Orc,      "orc",      ASB)                         \
    X(Ori,      "ori",      ASUimm)                      \
    X(Oris,     "oris",     ASUimm)                      \
    X(Xor,      "xor",      ASB)                         \
    X(Xori,     "xori",     ASUimm)                      \
    X(Xoris,    "xoris",    ASUimm)                      \
    /* Rotate and shift */                               \
    X(Rlwimi,   "rlwimi",   Rlwi)                        \
    X(Rlwinm,   "rlwinm",   Rlwi)                        \
    X(Rlwnm,    "rlwnm",    Rlwnm)                       \
    X(Slw,      "slw",      ASB)                         \
    X(Sraw,     "sraw",     ASB)                         \
    X(Srawi,    "srawi",    ASSh)                        \
    X(Srw,      "srw",      ASB)                         \
    /* Integer load */                                   \
    X(Lbz,      "lbz",      LoadStoreD)                  \
    X(Lbzu,     "lbzu",     LoadStoreD)                  \
    X(Lbzux,    "lbzux",    LoadStoreX)                  \
    X(Lbzx,     "lbzx",     LoadStoreX)                  \
    X(Lha,      "lha",      LoadStoreD)                  \
    X(Lhau,     "lhau",     LoadStoreD)                  \
    X(Lhaux,    "lhaux",    LoadStoreX)                  \
    X(Lhax,     "lhax",     LoadStoreX)                  \
    X(Lhbrx,    "lhbrx",    LoadStoreX)                  \
    X(Lhz,      "lhz",      LoadStoreD)                  \
    X(Lhzu,     "lhzu",     LoadStoreD)                  \
    X(Lhzux,    "lhzux",    LoadStoreX)                  \
    X(Lhzx,     "lhzx",     LoadStoreX)                  \
    X(Lmw,      "lmw",      LoadStoreD)                  \
    X(Lswi,     "lswi",     LoadStoreNB)                 \
    X(Lswx,     "lswx",     LoadStoreX)                  \
    X(Lwarx,    "lwarx",    LoadStoreX)                  \
    X(Lwbrx,    "lwbrx",    LoadStoreX)                  \
    X(Lwz,      "lwz",      LoadStoreD)                  \
    X(Lwzu,     "lwzu",     LoadStoreD)                  \
    X(Lwzux,    "lwzux",    LoadStoreX)                  \
    X(Lwzx,     "lwzx",     LoadStoreX)                  \
    /* Integer store */                                  \
    X(Stb,      "stb",      LoadStoreD)                  \
    X(Stbu,     "stbu",     LoadStoreD)                  \
    X(Stbux,    "stbux",    LoadStoreX)                  \
    X(Stbx,     "stbx",     LoadStoreX)                  \
    X(Sth,      "sth",      LoadStoreD)                  \
    X(Sthbrx,   "sthbrx",   LoadStoreX)                  \
    X(Sthu,     "sthu",     LoadStoreD)                  \
    X(Sthux,    "sthux",    LoadStoreX)                  \
    X(Sthx,     "sthx",     LoadStoreX)                  \
    X(Stmw,     "stmw",     LoadStoreD)                  \
    X(Stswi,    "stswi",    LoadStoreNB)                 \
    X(Stswx,    "stswx",    LoadStoreX)                  \
    X(Stw,      "stw",      LoadStoreD)                  \
    X(Stwbrx,   "stwbrx",   LoadStoreX)                  \
    X(StwcxRc,  "stwcx.",   LoadStoreX)                  \
    X(Stwu,     "stwu",     LoadStoreD)                  \
    X(Stwux,    "stwux",    LoadStoreX)                  \
    X(Stwx,     "stwx",     LoadStoreX)                  \
    /* External control */                               \
    X(Eciwx,    "eciwx",    LoadStoreX)                  \
    X(Ecowx,    "ecowx",    LoadStoreX)                  \
    /* Floating-point load/store */                      \
    X(Lfd,      "lfd",      FLoadStoreD)                 \
    X(Lfdu,     "lfdu",     FLoadStoreD)                 \
    X(Lfdux,    "lfdux",    FLoadStoreX)                 \
    X(Lfdx,     "lfdx",     FLoadStoreX)                 \
    X(Lfs,      "lfs",      FLoadStoreD)                 \
    X(Lfsu,     "lfsu",     FLoadStoreD)                 \
    X(Lfsux,    "lfsux",    FLoadStoreX)                 \
    X(Lfsx,     "lfsx",     FLoadStoreX)                 \
    X(Stfd,     "stfd",     FLoadStoreD)                 \
    X(Stfdu,    "stfdu",    FLoadStoreD)                 \
    X(Stfdux,   "stfdux",   FLoadStoreX)                 \
    X(Stfdx,    "stfdx",    FLoadStoreX)                 \
    X(Stfiwx,   "stfiwx",   FLoadStoreX)                 \
    X(Stfs,     "stfs",     FLoadStoreD)                 \
    X(Stfsu,    "stfsu",    FLoadStoreD)                 \
    X(Stfsux,   "stfsux",   FLoadStoreX)                 \
    X(Stfsx,    "stfsx",    FLoadStoreX)                 \
    /* Floating-point arithmetic */                      \
    X(Fabs,     "fabs",     FDB)                         \
    X(Fadd,     "fadd",     FDAB)                        \
    X(Fadds,    "fadds",    FDAB)                        \
    X(Fcmpo,    "fcmpo",    FCmp)                        \
    X(Fcmpu,    "fcmpu",    FCmp)                        \
    X(Fctiw,    "fctiw",    FDB)                         \
    X(Fctiwz,   "fctiwz",   FDB)                         \
    X(Fdiv,     "fdiv",     FDAB)                        \
    X(Fdivs,    "fdivs",    FDAB)                        \
    X(Fmadd,    "fmadd",    FDACB)                       \
    X(Fmadds,   "fmadds",   FDACB)                       \
    X(Fmr,      "fmr",      FDB)                         \
    X(Fmsub,    "fmsub",    FDACB)                       \
    X(Fmsubs,   "fmsubs",   FDACB)                       \
    X(Fmul,     "fmul",     FDAC)                        \
    X(Fmuls,    "fmuls",    FDAC)                        \
    X(Fnabs,    "fnabs",    FDB)                         \
    X(Fneg,     "fneg",     FDB)                         \
    X(Fnmadd,   "fnmadd",   FDACB)                       \
    X(Fnmadds,  "fnmadds",  FDACB)                       \
    X(Fnmsub,   "fnmsub",   FDACB)                       \
    X(Fnmsubs,  "fnmsubs",  FDACB)                       \
    X(Fres,     "fres",     FDB)                         \
    X(Frsp,     "frsp",     FDB)                         \
    X(Frsqrte,  "frsqrte",  FDB)                         \
    X(Fsel,     "fsel",     FDACB)                       \
    X(Fsqrt,    "fsqrt",    FDB)                         \
    X(Fsqrts,   "fsqrts",   FDB)                         \
    X(Fsub,     "fsub",     FDAB)                        \
    X(Fsubs,    "fsubs",    FDAB)                        \
    /* FPSCR */                                          \
    X(Mcrfs,    "mcrfs",    Mcrf)                        \
    X(Mffs,     "mffs",     Mffs)                        \
    X(Mtfsb0,   "mtfsb0",   Mtfsb)                       \
    X(Mtfsb1,   "mtfsb1",   Mtfsb)                       \
    X(Mtfsf,    "mtfsf",    Mtfsf)                       \
    X(Mtfsfi,   "mtfsfi",   Mtfsfi)                      \
    /* Branch and flow control */                        \
    X(B,        "b",        Branch)                      \
    X(Bc,       "bc",       BranchCond)                  \
    X(Bcctr,    "bcctr",    BranchCTR)                   \
    X(Bclr,     "bclr",     BranchLR)                    \
    X(Rfi,      "rfi",      None)                        \
    X(Sc,       "sc",       None)                        \
    X(Tw,       "tw",       Trap)                        \
    X(Twi,      "twi",      TrapImm)                     \
    /* Condition register */                             \
    X(Crand,    "crand",    CrBit)                       \
    X(Crandc,   "crandc",   CrBit)                       \
    X(Creqv,    "creqv",    CrBit)                       \
    X(Crnand,   "crnand",   CrBit)                       \
    X(Crnor,    "crnor",    CrBit)                       \
    X(Cror,     "cror",     CrBit)                       \
    X(Crorc,    "crorc",    CrBit)                       \
    X(Crxor,    "crxor",    CrBit)                       \
    X(Mcrf,     "mcrf",     Mcrf)                        \
    X(Mcrxr,    "mcrxr",    Mcrxr)                       \
    X(Mfcr,     "mfcr",     MfReg)                       \
    X(Mtcrf,    "mtcrf",    MtCrf)                       \
    /* Processor control / supervisor */                 \
    X(Mfmsr,    "mfmsr",    MfReg)                       \
    X(Mfspr,    "mfspr",    MfSpr)                       \
    X(Mfsr,     "mfsr",     MfSr)                        \
    X(Mfsrin,   "mfsrin",   MfSrin)                      \
    X(Mftb,     "mftb",     MfTb)                        \
    X(Mtmsr,    "mtmsr",    MtReg)                       \
    X(Mtspr,    "mtspr",    MtSpr)                       \
    X(Mtsr,     "mtsr",     MtSr)                        \
    X(Mtsrin,   "mtsrin",   MtSrin)                      \
    /* Synchronization */                                \
    X(Eieio,    "eieio",    None)                        \
    X(Isync,    "isync",    None)                        \
    X(Sync,     "sync",     None)                        \
    /* Cache and TLB management */                       \
    X(Dcbf,     "dcbf",     Cache)                       \
    X(Dcbi,     "dcbi",     Cache)                       \
    X(Dcbst,    "dcbst",    Cache)                       \
    X(Dcbt,     "dcbt",     Cache)                       \
    X(Dcbtst,   "dcbtst",   Cache)                       \
    X(Dcbz,     "dcbz",     Cache)                       \
    X(Icbi,     "icbi",     Cache)                       \
    X(Tlbia,    "tlbia",    None)                        \
    X(Tlbie,    "tlbie",    RB)                          \
    X(Tlbld,    "tlbld",    RB)  /* 603 only */          \
    X(Tlbli,    "tlbli",    RB)  /* 603 only */          \
    X(Tlbsync,  "tlbsync",  None)

enum class Op : uint16_t {
#define PPC_X(name, mnem, form) name,
    PPC_OPCODES(PPC_X)
#undef PPC_X
    Count
};

const char* mnemonic(Op op);
Form form(Op op);

struct Instruction {
    uint32_t raw = 0;
    Op op = Op::Invalid;

    constexpr bool valid() const { return op != Op::Invalid; }

    // Register / field extraction. Several names alias the same bits
    // (e.g. rD, rS, frD, BO and TO all live in bits 6-10).
    constexpr uint32_t opcd() const { return raw >> 26; }
    constexpr uint32_t rd() const { return (raw >> 21) & 31; }
    constexpr uint32_t rs() const { return rd(); }
    constexpr uint32_t ra() const { return (raw >> 16) & 31; }
    constexpr uint32_t rb() const { return (raw >> 11) & 31; }
    constexpr uint32_t rc() const { return (raw >> 6) & 31; } // frC (A-form)
    constexpr uint32_t bo() const { return rd(); }
    constexpr uint32_t bi() const { return ra(); }
    constexpr uint32_t to() const { return rd(); }
    constexpr uint32_t sh() const { return rb(); }
    constexpr uint32_t mb() const { return (raw >> 6) & 31; }
    constexpr uint32_t me() const { return (raw >> 1) & 31; }
    constexpr uint32_t nb() const { return rb(); }
    constexpr uint32_t crfd() const { return (raw >> 23) & 7; }
    constexpr uint32_t crfs() const { return (raw >> 18) & 7; }
    constexpr uint32_t crm() const { return (raw >> 12) & 0xFF; }
    constexpr uint32_t fm() const { return (raw >> 17) & 0xFF; }
    constexpr uint32_t sr() const { return (raw >> 16) & 15; }
    constexpr uint32_t fpimm() const { return (raw >> 12) & 15; }
    // SPR/TBR numbers are encoded with their two 5-bit halves swapped.
    constexpr uint32_t spr() const { return ((raw >> 16) & 31) | (((raw >> 11) & 31) << 5); }

    constexpr bool record() const { return raw & 1; } // Rc
    constexpr bool oe() const { return (raw >> 10) & 1; }
    constexpr bool lk() const { return raw & 1; }
    constexpr bool aa() const { return (raw >> 1) & 1; }

    constexpr int32_t simm() const { return static_cast<int16_t>(raw & 0xFFFF); }
    constexpr uint32_t uimm() const { return raw & 0xFFFF; }
    // Branch displacements, already sign-extended and in bytes.
    constexpr int32_t li() const {
        uint32_t v = raw & 0x03FFFFFC;
        return static_cast<int32_t>(v & 0x02000000 ? v | 0xFC000000 : v);
    }
    constexpr int32_t bd() const { return static_cast<int16_t>(raw & 0xFFFC); }
};

Instruction decode(uint32_t raw);

// Target of a direct branch (b / bc) at address `pc`; nullopt for anything
// else, including bclr/bcctr whose target is only known at runtime.
std::optional<uint32_t> branch_target(const Instruction& insn, uint32_t pc);

} // namespace ppc
