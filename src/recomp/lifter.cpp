#include "recomp/lifter.hpp"

#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "ppc/timing.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <vector>

namespace recomp {

namespace {

using analysis::Block;
using analysis::Exit;
using analysis::Function;
using ppc::Instruction;
using ppc::Op;

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08xu", v);
    return buf;
}

std::string label(uint32_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "L_%08x", addr);
    return buf;
}

std::string function_name(uint32_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "f_%08x", addr);
    return buf;
}

std::string gpr(uint32_t r) { return "s.gpr[" + std::to_string(r) + "]"; }

// PowerPC MASK(mb, me): ones from bit mb through bit me (bit 0 = MSB), wrapping.
uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    const uint32_t from = 0xFFFFFFFFu >> mb;
    const uint32_t to = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (from & to) : (from | to);
}

// CpuState member for an SPR, or nullptr. The time base is handled apart.
std::string spr_field(uint32_t spr) {
    switch (spr) {
    case 1:    return "s.xer";
    case 8:    return "s.lr";
    case 9:    return "s.ctr";
    case 18:   return "s.dsisr";
    case 19:   return "s.dar";
    case 22:   return "s.dec";
    case 25:   return "s.sdr1";
    case 26:   return "s.srr0";
    case 27:   return "s.srr1";
    case 282:  return "s.ear";
    case 287:  return "s.pvr";
    case 976:  return "s.dmiss";
    case 977:  return "s.dcmp";
    case 978:  return "s.hash1";
    case 979:  return "s.hash2";
    case 980:  return "s.imiss";
    case 981:  return "s.icmp";
    case 982:  return "s.rpa";
    case 1008: return "s.hid0";
    case 1009: return "s.hid1";
    case 1010: return "s.iabr";
    default:   break;
    }
    if (spr >= 272 && spr <= 275) return "s.sprg[" + std::to_string(spr - 272) + "]";
    if (spr >= 528 && spr <= 535) return "s.ibat[" + std::to_string(spr - 528) + "]";
    if (spr >= 536 && spr <= 543) return "s.dbat[" + std::to_string(spr - 536) + "]";
    return {};
}

class Emitter {
public:
    // `resume_sites`: return sites found so far, each with the first function
    // that has it (shared by the emitters of all files).
    // `block_sites`: every block start found so far, with the first function
    // that has it (save states resume at any block).
    Emitter(const analysis::CodeImage& image, const analysis::Analysis& an, const std::set<uint32_t>& compiled,
            LiftStats& stats, std::map<uint32_t, uint32_t>& resume_sites, std::map<uint32_t, uint32_t>& block_sites)
        : image_(image), an_(an), compiled_(compiled), stats_(stats), resume_sites_(resume_sites),
          block_sites_(block_sites) {}

    void function(const Function& fn) {
        blocks_.clear();
        blocks_.insert(fn.blocks.begin(), fn.blocks.end());
        sites_.clear();
        ++stats_.functions;

        const std::string head = "\n// " + (fn.name.empty() ? function_name(fn.entry) : fn.name) + "\n" +
                                 "void " + function_name(fn.entry) + "(Context& c) {\n    auto& s = c.cpu;\n    (void)s;\n";
        std::string body;
        body.swap(out_);
        for (uint32_t start : fn.blocks) block(an_.blocks.at(start));
        body.swap(out_);

        out_ += head;
        // Resuming at a return site (Context::call sets resume_at), or at any
        // block (a save state loaded: Context::resume_block): go to it.
        out_ += "    if (c.resume_at) [[unlikely]] {\n        const uint32_t at = c.resume_at;\n"
                "        c.resume_at = 0;\n        switch (at) {\n";
        std::set<uint32_t> cases;
        for (const auto& [site, name] : sites_) {
            out_ += "        case " + hex(site) + ": goto " + name + ";\n";
            resume_sites_.emplace(site, fn.entry);
            cases.insert(site);
        }
        for (uint32_t start : fn.blocks) {
            if (!cases.insert(start).second) continue;
            out_ += "        case " + hex(start) + ": goto " + label(start) + ";\n";
            block_sites_.emplace(start, fn.entry);
        }
        out_ += "        default: break;\n        }\n    }\n";
        if (fn.blocks.front() != fn.entry) out_ += "    goto " + label(fn.entry) + ";\n";
        out_ += body;
        out_ += "}\n";
    }

    std::string take() { return std::move(out_); }

private:
    void line(const std::string& code, const std::string& comment = {}) {
        out_ += "    " + code;
        if (!comment.empty()) out_ += "  // " + comment;
        out_ += "\n";
    }

    void block(const Block& b) {
        ++stats_.blocks;
        out_ += label(b.start) + ":\n";
        std::vector<ppc::Instruction> code;
        for (uint32_t pc = b.start; pc < b.end; pc += 4) code.push_back(ppc::decode(image_.read32(pc).value_or(0)));
        const uint32_t cycles = ppc::block_cycles(code);
        stats_.cycles += cycles;
        line("c.tick(" + hex(b.start) + ", " + std::to_string(cycles) + ");");
        for (uint32_t pc = b.start; pc < b.end; pc += 4) {
            const uint32_t raw = image_.read32(pc).value_or(0);
            const Instruction in = ppc::decode(raw);
            ++stats_.instructions;
            const std::string text = ppc::disassemble(in, pc);
            switch (in.op) {
            case Op::B: case Op::Bc: case Op::Bclr: case Op::Bcctr: case Op::Rfi:
                // Branches can be several lines: the comment goes on a line of its own.
                line("// " + text);
                line(branch(in, pc, b));
                if (in.lk() && in.op != Op::Rfi) return_site(pc + 4, b);
                break;
            default:
                line(instruction(in, pc), text);
                break;
            }
        }
        switch (b.exit) {
        case Exit::Fallthrough: line(edge(b.end)); break;
        case Exit::OutOfImage:  line("c.missing(" + hex(b.end) + ");"); break;
        // A handler may resume after an unconditional trap; nothing there was recompiled.
        case Exit::Trap:        line("c.missing(" + hex(b.end) + ");"); break;
        default: break;
        }
    }

    // After a call at `site - 4`: a label to resume at, inside this block or
    // at the next one (when it is in this function).
    void return_site(uint32_t site, const Block& b) {
        if (site < b.end) {
            const std::string name = "R_" + label(site).substr(2);
            out_ += name + ":\n";
            sites_.emplace(site, name);
        } else if (blocks_.contains(site)) {
            sites_.emplace(site, label(site));
        }
    }

    // Transfer to `target`: a goto inside the function, otherwise a tail call.
    std::string edge(uint32_t target) const {
        if (blocks_.contains(target)) return "goto " + label(target) + ";";
        if (compiled_.contains(target)) return function_name(target) + "(c); return;";
        return "c.jump(" + hex(target) + "); return;";
    }

    // After a call: if the callee returned elsewhere, follow it there, or
    // return at once when an outer call waits for that address.
    static std::string after_call(uint32_t next) {
        return "if (c.returned_to != " + hex(next) + " && c.follow_return(" + hex(next) + ")) return;";
    }

    std::string call(uint32_t target, uint32_t next) const {
        const std::string invoke =
            compiled_.contains(target) ? function_name(target) + "(c);" : "c.call(" + hex(target) + ");";
        return "c.push_return(" + hex(next) + "); " + invoke + " c.pop_return(); " + after_call(next);
    }

    // Branch condition of a bc-type instruction, "" when it always branches.
    // CTR is decremented before the CR bit is tested, as the hardware does.
    static std::string condition(const Instruction& in) {
        const uint32_t bo = in.bo();
        std::string ctr, cr;
        if (!(bo & 0x04)) ctr = (bo & 0x02) ? "--s.ctr == 0" : "--s.ctr != 0";
        if (!(bo & 0x10)) cr = std::string((bo & 0x08) ? "" : "!") + "s.crbit(" + std::to_string(in.bi()) + ")";
        if (ctr.empty()) return cr;
        if (cr.empty()) return "(" + ctr + ")";
        return "(" + ctr + ") && " + cr;
    }

    static std::string guarded(const std::string& cond, const std::string& body) {
        return cond.empty() ? body : "if (" + cond + ") { " + body + " }";
    }

    std::string branch(const Instruction& in, uint32_t pc, const Block& b) const {
        const uint32_t next = pc + 4;
        const std::string cond = in.op == Op::B || in.op == Op::Rfi ? std::string() : condition(in);
        const std::string set_lr = "s.lr = " + hex(next) + ";";

        if (in.op == Op::Rfi) return "c.rfi(); return;";

        if (in.op == Op::B || in.op == Op::Bc) {
            const uint32_t target = *ppc::branch_target(in, pc);
            if (in.lk()) {
                // LR is written whether or not the branch is taken. bl $+4 reads the PC.
                if (target == next) return cond.empty() ? set_lr : set_lr + " (void)(" + cond + ");";
                return set_lr + " " + guarded(cond, call(target, next));
            }
            if (cond.empty()) return edge(target);
            return "if (" + cond + ") { " + edge(target) + " }\n    " + edge(next);
        }

        // bclr / bcctr
        const std::string reg = in.op == Op::Bclr ? "s.lr" : "s.ctr";
        if (in.lk()) {
            const std::string body = "c.push_return(" + hex(next) + "); c.call(t); c.pop_return(); " + after_call(next);
            return "{ const uint32_t t = " + reg + " & ~3u; " + set_lr + " " + guarded(cond, body) + " }";
        }
        std::string act;
        switch (b.exit) {
        case Exit::Return:
            act = "c.returned_to = s.lr; return;";
            break;
        case Exit::Jump:
        case Exit::TailCall:
            // A constant target outside the image has no successor: jump to what the register holds.
            act = b.successors.empty() ? "c.jump(" + reg + " & ~3u); return;" : edge(b.successors[0]);
            break;
        case Exit::JumpTable: {
            act = "switch (" + reg + " & ~3u) {";
            const size_t targets = b.successors.size() - (cond.empty() ? 0 : 1);
            for (size_t i = 0; i < targets; ++i)
                act += " case " + hex(b.successors[i]) + ": " + edge(b.successors[i]);
            act += " default: c.jump(" + reg + " & ~3u); return; }";
            break;
        }
        default:
            act = "c.jump(" + reg + " & ~3u); return;";
            break;
        }
        if (cond.empty()) return act;
        return "if (" + cond + ") { " + act + " }\n    " + edge(next);
    }

    std::string unimplemented(const Instruction& in, uint32_t pc) const {
        ++stats_.unimplemented[ppc::mnemonic(in.op)];
        return "c.unimplemented(" + hex(pc) + ", " + hex(in.raw) + ");";
    }

    std::string instruction(const Instruction& in, uint32_t pc) const {
        const std::string D = gpr(in.rd()), A = gpr(in.ra()), B = gpr(in.rb()), S = gpr(in.rs());
        const std::string A0 = in.ra() == 0 ? std::string("0u") : A; // (rA|0)
        const std::string simm = hex(static_cast<uint32_t>(in.simm()));
        const std::string uimm = hex(in.uimm());
        const std::string ea_d = "const uint32_t ea = " + A0 + " + " + simm + "; ";
        const std::string ea_x = "const uint32_t ea = " + A0 + " + " + B + "; ";
        const bool rc = in.record();
        auto cr0 = [&](const std::string& reg) { return rc ? " ops::set_cr0(s, " + reg + ");" : std::string(); };
        auto arith = [&](const std::string& x, const std::string& y, const std::string& cin, bool ca) {
            return D + " = ops::addx(s, " + x + ", " + y + ", " + cin + ", " + (ca ? "true" : "false") + ", " +
                   (in.oe() ? "true" : "false") + ");" + cr0(D);
        };
        auto logical = [&](const std::string& expr) { return A + " = " + expr + ";" + cr0(A); };
        auto load = [&](const std::string& ea, const std::string& value, bool update) {
            return "{ " + ea + D + " = " + value + ";" + (update ? " " + A + " = ea;" : "") + " }";
        };
        auto store = [&](const std::string& ea, const std::string& action, bool update) {
            return "{ " + ea + action + (update ? " " + A + " = ea;" : "") + " }";
        };
        auto crbits = [&](const std::string& expr) {
            return "s.set_crbit(" + std::to_string(in.rd()) + ", " + expr + ");";
        };
        const std::string ba = "s.crbit(" + std::to_string(in.ra()) + ")";
        const std::string bb = "s.crbit(" + std::to_string(in.rb()) + ")";
        const std::string crf = std::to_string(in.crfd());

        switch (in.op) {
        // Integer arithmetic
        case Op::Addi:    return D + " = " + A0 + " + " + simm + ";";
        case Op::Addis:   return D + " = " + A0 + " + " + hex(static_cast<uint32_t>(in.simm()) << 16) + ";";
        case Op::Addic:   return D + " = ops::addx(s, " + A + ", " + simm + ", 0, true, false);";
        case Op::AddicRc: return D + " = ops::addx(s, " + A + ", " + simm + ", 0, true, false); ops::set_cr0(s, " + D + ");";
        case Op::Subfic:  return D + " = ops::addx(s, ~" + A + ", " + simm + ", 1, true, false);";
        case Op::Mulli:   return D + " = " + A + " * " + simm + ";";
        case Op::Add:     return arith(A, B, "0", false);
        case Op::Addc:    return arith(A, B, "0", true);
        case Op::Adde:    return arith(A, B, "ops::ca(s)", true);
        case Op::Addme:   return arith(A, "0xFFFFFFFFu", "ops::ca(s)", true);
        case Op::Addze:   return arith(A, "0u", "ops::ca(s)", true);
        case Op::Subf:    return arith("~" + A, B, "1", false);
        case Op::Subfc:   return arith("~" + A, B, "1", true);
        case Op::Subfe:   return arith("~" + A, B, "ops::ca(s)", true);
        case Op::Subfme:  return arith("~" + A, "0xFFFFFFFFu", "ops::ca(s)", true);
        case Op::Subfze:  return arith("~" + A, "0u", "ops::ca(s)", true);
        case Op::Neg:     return arith("~" + A, "0u", "1", false);
        case Op::Mullw:   return D + " = ops::mullw(s, " + A + ", " + B + ", " + (in.oe() ? "true" : "false") + ");" + cr0(D);
        case Op::Mulhw:   return D + " = ops::mulhw(" + A + ", " + B + ");" + cr0(D);
        case Op::Mulhwu:  return D + " = ops::mulhwu(" + A + ", " + B + ");" + cr0(D);
        case Op::Divw:    return D + " = ops::divw(s, " + A + ", " + B + ", " + (in.oe() ? "true" : "false") + ");" + cr0(D);
        case Op::Divwu:   return D + " = ops::divwu(s, " + A + ", " + B + ", " + (in.oe() ? "true" : "false") + ");" + cr0(D);

        // Compare (the L bit selects 64-bit compares, which a 32-bit CPU doesn't have)
        case Op::Cmp:   return "ops::cmps(s, " + crf + ", static_cast<int32_t>(" + A + "), static_cast<int32_t>(" + B + "));";
        case Op::Cmpi:  return "ops::cmps(s, " + crf + ", static_cast<int32_t>(" + A + "), " + std::to_string(in.simm()) + ");";
        case Op::Cmpl:  return "ops::cmpu(s, " + crf + ", " + A + ", " + B + ");";
        case Op::Cmpli: return "ops::cmpu(s, " + crf + ", " + A + ", " + uimm + ");";

        // Logical
        case Op::And:     return logical(S + " & " + B);
        case Op::Andc:    return logical(S + " & ~" + B);
        case Op::Or:      return logical(in.rs() == in.rb() ? S : S + " | " + B);
        case Op::Orc:     return logical(S + " | ~" + B);
        case Op::Xor:     return logical(S + " ^ " + B);
        case Op::Nand:    return logical("~(" + S + " & " + B + ")");
        case Op::Nor:     return logical("~(" + S + " | " + B + ")");
        case Op::Eqv:     return logical("~(" + S + " ^ " + B + ")");
        case Op::AndiRc:  return A + " = " + S + " & " + uimm + "; ops::set_cr0(s, " + A + ");";
        case Op::AndisRc: return A + " = " + S + " & " + hex(in.uimm() << 16) + "; ops::set_cr0(s, " + A + ");";
        case Op::Ori:     return A + " = " + S + " | " + uimm + ";";
        case Op::Oris:    return A + " = " + S + " | " + hex(in.uimm() << 16) + ";";
        case Op::Xori:    return A + " = " + S + " ^ " + uimm + ";";
        case Op::Xoris:   return A + " = " + S + " ^ " + hex(in.uimm() << 16) + ";";
        case Op::Cntlzw:  return logical("ops::cntlzw(" + S + ")");
        case Op::Extsb:   return logical("static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(" + S + ")))");
        case Op::Extsh:   return logical("static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(" + S + ")))");

        // Rotate and shift
        case Op::Rlwinm: {
            const std::string m = hex(ppc_mask(in.mb(), in.me()));
            return logical((in.sh() ? "ops::rotl(" + S + ", " + std::to_string(in.sh()) + ")" : S) + " & " + m);
        }
        case Op::Rlwimi: {
            const uint32_t m = ppc_mask(in.mb(), in.me());
            return logical("(ops::rotl(" + S + ", " + std::to_string(in.sh()) + ") & " + hex(m) + ") | (" + A + " & " +
                           hex(~m) + ")");
        }
        case Op::Rlwnm: return logical("ops::rotl(" + S + ", " + B + ") & " + hex(ppc_mask(in.mb(), in.me())));
        case Op::Slw:   return logical("ops::slw(" + S + ", " + B + ")");
        case Op::Srw:   return logical("ops::srw(" + S + ", " + B + ")");
        case Op::Sraw:  return logical("ops::sraw(s, " + S + ", " + B + " & 0x3F)");
        case Op::Srawi: return logical("ops::sraw(s, " + S + ", " + std::to_string(in.sh()) + ")");

        // Loads
        case Op::Lbz:   return load(ea_d, "c.read8(ea)", false);
        case Op::Lbzu:  return load(ea_d, "c.read8(ea)", true);
        case Op::Lbzx:  return load(ea_x, "c.read8(ea)", false);
        case Op::Lbzux: return load(ea_x, "c.read8(ea)", true);
        case Op::Lhz:   return load(ea_d, "c.read16(ea)", false);
        case Op::Lhzu:  return load(ea_d, "c.read16(ea)", true);
        case Op::Lhzx:  return load(ea_x, "c.read16(ea)", false);
        case Op::Lhzux: return load(ea_x, "c.read16(ea)", true);
        case Op::Lha:   return load(ea_d, "static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))", false);
        case Op::Lhau:  return load(ea_d, "static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))", true);
        case Op::Lhax:  return load(ea_x, "static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))", false);
        case Op::Lhaux: return load(ea_x, "static_cast<uint32_t>(static_cast<int16_t>(c.read16(ea)))", true);
        case Op::Lwz:   return load(ea_d, "c.read32(ea)", false);
        case Op::Lwzu:  return load(ea_d, "c.read32(ea)", true);
        case Op::Lwzx:  return load(ea_x, "c.read32(ea)", false);
        case Op::Lwzux: return load(ea_x, "c.read32(ea)", true);
        case Op::Lhbrx: return load(ea_x, "ops::bswap16(c.read16(ea))", false);
        case Op::Lwbrx: return load(ea_x, "m3::bswap32(c.read32(ea))", false);
        case Op::Lwarx: return "{ " + ea_x + "s.reserve = true; s.reserve_addr = ea; " + D + " = c.read32(ea); }";
        case Op::Lmw:
            return "{ uint32_t ea = " + A0 + " + " + simm + "; for (uint32_t r = " + std::to_string(in.rd()) +
                   "; r < 32; ++r, ea += 4) s.gpr[r] = c.read32(ea); }";
        case Op::Lswi:
            return "c.load_string(" + std::to_string(in.rd()) + ", " + A0 + ", " + std::to_string(in.nb() ? in.nb() : 32) + ");";
        case Op::Lswx:
            return "c.load_string(" + std::to_string(in.rd()) + ", " + A0 + " + " + B + ", s.xer & 0x7F);";

        // Stores
        case Op::Stb:    return store(ea_d, "c.write8(ea, static_cast<uint8_t>(" + S + "));", false);
        case Op::Stbu:   return store(ea_d, "c.write8(ea, static_cast<uint8_t>(" + S + "));", true);
        case Op::Stbx:   return store(ea_x, "c.write8(ea, static_cast<uint8_t>(" + S + "));", false);
        case Op::Stbux:  return store(ea_x, "c.write8(ea, static_cast<uint8_t>(" + S + "));", true);
        case Op::Sth:    return store(ea_d, "c.write16(ea, static_cast<uint16_t>(" + S + "));", false);
        case Op::Sthu:   return store(ea_d, "c.write16(ea, static_cast<uint16_t>(" + S + "));", true);
        case Op::Sthx:   return store(ea_x, "c.write16(ea, static_cast<uint16_t>(" + S + "));", false);
        case Op::Sthux:  return store(ea_x, "c.write16(ea, static_cast<uint16_t>(" + S + "));", true);
        case Op::Stw:    return store(ea_d, "c.write32(ea, " + S + ");", false);
        case Op::Stwu:   return store(ea_d, "c.write32(ea, " + S + ");", true);
        case Op::Stwx:   return store(ea_x, "c.write32(ea, " + S + ");", false);
        case Op::Stwux:  return store(ea_x, "c.write32(ea, " + S + ");", true);
        case Op::Sthbrx: return store(ea_x, "c.write16(ea, static_cast<uint16_t>(ops::bswap16(" + S + ")));", false);
        case Op::Stwbrx: return store(ea_x, "c.write32(ea, m3::bswap32(" + S + "));", false);
        case Op::StwcxRc:
            return "{ " + ea_x + "if (s.reserve) { c.write32(ea, " + S +
                   "); s.set_crf(0, 2u | ops::so(s)); } else { s.set_crf(0, ops::so(s)); } s.reserve = false; }";
        case Op::Stmw:
            return "{ uint32_t ea = " + A0 + " + " + simm + "; for (uint32_t r = " + std::to_string(in.rs()) +
                   "; r < 32; ++r, ea += 4) c.write32(ea, s.gpr[r]); }";
        case Op::Stswi:
            return "c.store_string(" + std::to_string(in.rs()) + ", " + A0 + ", " + std::to_string(in.nb() ? in.nb() : 32) + ");";
        case Op::Stswx:
            return "c.store_string(" + std::to_string(in.rs()) + ", " + A0 + " + " + B + ", s.xer & 0x7F);";

        // Caches, TLB and ordering: nothing to do without a cache or an MMU model.
        case Op::Dcbz: return "c.dcbz(" + A0 + " + " + B + ");";
        case Op::Dcbf: case Op::Dcbi: case Op::Dcbst: case Op::Dcbt: case Op::Dcbtst: case Op::Icbi:
        case Op::Eieio: case Op::Isync: case Op::Sync:
        case Op::Tlbia: case Op::Tlbie: case Op::Tlbld: case Op::Tlbli: case Op::Tlbsync:
            return ";";

        // Condition register
        case Op::Crand:  return crbits(ba + " && " + bb);
        case Op::Crandc: return crbits(ba + " && !" + bb);
        case Op::Creqv:  return crbits(ba + " == " + bb);
        case Op::Crnand: return crbits("!(" + ba + " && " + bb + ")");
        case Op::Crnor:  return crbits("!(" + ba + " || " + bb + ")");
        case Op::Cror:   return crbits(ba + " || " + bb);
        case Op::Crorc:  return crbits(ba + " || !" + bb);
        case Op::Crxor:  return crbits(ba + " != " + bb);
        case Op::Mcrf:   return "s.set_crf(" + crf + ", s.crf(" + std::to_string(in.crfs()) + "));";
        case Op::Mcrxr:  return "s.set_crf(" + crf + ", s.xer >> 28); s.xer &= 0x0FFFFFFFu;";
        case Op::Mfcr:   return D + " = s.cr;";
        case Op::Mtcrf: {
            uint32_t mask = 0;
            for (uint32_t f = 0; f < 8; ++f)
                if (in.crm() & (0x80u >> f)) mask |= 0xF0000000u >> (4 * f);
            return "s.cr = (s.cr & " + hex(~mask) + ") | (" + S + " & " + hex(mask) + ");";
        }

        // Processor control
        case Op::Mfmsr:  return D + " = s.msr;";
        case Op::Mtmsr:  return "s.msr = " + S + "; c.msr_changed();";
        case Op::Mfsr:   return D + " = s.sr[" + std::to_string(in.sr()) + "];";
        case Op::Mtsr:   return "s.sr[" + std::to_string(in.sr()) + "] = " + S + ";";
        case Op::Mfsrin: return D + " = s.sr[" + B + " >> 28];";
        case Op::Mtsrin: return "s.sr[" + B + " >> 28] = " + S + ";";
        case Op::Mftb:
        case Op::Mfspr: {
            const uint32_t spr = in.spr();
            if (spr == 268) return D + " = c.read_tbl();";
            if (spr == 22 && in.op == Op::Mfspr) return D + " = c.read_dec();";
            if (spr == 269) return D + " = c.read_tbu();";
            const std::string field = in.op == Op::Mfspr ? spr_field(spr) : std::string();
            if (field.empty()) return "c.unknown_spr(" + hex(pc) + ", " + std::to_string(spr) + ");";
            return D + " = " + field + ";";
        }
        case Op::Mtspr: {
            const uint32_t spr = in.spr();
            if (spr == 268 || spr == 284) return "c.write_tbl(" + S + ");";
            if (spr == 22) return "c.write_dec(" + S + ");";
            if (spr == 269 || spr == 285) return "c.write_tbu(" + S + ");";
            if (spr == 287) return ";"; // PVR is read-only
            const std::string field = spr_field(spr);
            if (field.empty()) return "c.unknown_spr(" + hex(pc) + ", " + std::to_string(spr) + ");";
            return field + " = " + S + ";";
        }

        // Traps and system calls
        case Op::Tw:
            if (in.to() == 31) return "c.trap(" + hex(pc) + ");";
            return "if (ops::trap_if(" + std::to_string(in.to()) + ", " + A + ", " + B + ")) c.trap(" + hex(pc) + ");";
        case Op::Twi:
            if (in.to() == 31) return "c.trap(" + hex(pc) + ");";
            return "if (ops::trap_if(" + std::to_string(in.to()) + ", " + A + ", " + simm + ")) c.trap(" + hex(pc) + ");";
        case Op::Sc: return "c.system_call(" + hex(pc) + ");";

        case Op::Invalid: return "c.invalid(" + hex(pc) + ", " + hex(in.raw) + ");";

        default:
            return floating_point(in, pc);
        }
    }

    std::string floating_point(const Instruction& in, uint32_t pc) const {
        auto fpr = [](uint32_t r) { return "s.fpr[" + std::to_string(r) + "]"; };
        const std::string FD = fpr(in.rd()), FA = fpr(in.ra()), FB = fpr(in.rb()), FC = fpr(in.rc());
        const std::string FS = FD; // frS of stores is in the same field
        const std::string A = gpr(in.ra()), B = gpr(in.rb());
        const std::string A0 = in.ra() == 0 ? std::string("0u") : A;
        const std::string ea_d = "const uint32_t ea = " + A0 + " + " + hex(static_cast<uint32_t>(in.simm())) + "; ";
        const std::string ea_x = "const uint32_t ea = " + A0 + " + " + B + "; ";
        const std::string cr1 = in.record() ? " s.set_crf(1, s.fpscr >> 28);" : "";
        const std::string a = "ops::f64(" + FA + ")", b = "ops::f64(" + FB + ")", cc = "ops::f64(" + FC + ")";
        auto mem = [&](bool indexed, const std::string& action, bool update) {
            return "{ " + (indexed ? ea_x : ea_d) + action + (update ? " " + A + " = ea;" : "") + " }";
        };
        // RECOMP_FP_CHECK: nothing unless built with SPIKEOUT_FP_TRAP (finds where NaNs start).
        const std::string check = " RECOMP_FP_CHECK(" + hex(pc) + ", " + std::to_string(in.rd()) + ");";
        // Arithmetic results set FPSCR[FPRF].
        auto dbl = [&](const std::string& expr) {
            return FD + " = ops::bits(" + expr + "); ops::set_fprf(s, " + FD + ", false);" + check + cr1;
        };
        auto sgl = [&](const std::string& expr) {
            return FD + " = ops::round_single(" + expr + "); ops::set_fprf(s, " + FD + ", true);" + check + cr1;
        };
        auto raw = [&](const std::string& expr) { return FD + " = " + expr + ";" + cr1; };
        const std::string lfs = FD + " = ops::single_to_double(c.read32(ea));";
        const std::string lfd = FD + " = c.read64(ea);";
        const std::string stfs = "c.write32(ea, ops::double_to_single(" + FS + "));";
        const std::string stfd = "c.write64(ea, " + FS + ");";

        switch (in.op) {
        // Loads and stores
        case Op::Lfs:    return mem(false, lfs, false);
        case Op::Lfsu:   return mem(false, lfs, true);
        case Op::Lfsx:   return mem(true, lfs, false);
        case Op::Lfsux:  return mem(true, lfs, true);
        case Op::Lfd:    return mem(false, lfd, false);
        case Op::Lfdu:   return mem(false, lfd, true);
        case Op::Lfdx:   return mem(true, lfd, false);
        case Op::Lfdux:  return mem(true, lfd, true);
        case Op::Stfs:   return mem(false, stfs, false);
        case Op::Stfsu:  return mem(false, stfs, true);
        case Op::Stfsx:  return mem(true, stfs, false);
        case Op::Stfsux: return mem(true, stfs, true);
        case Op::Stfd:   return mem(false, stfd, false);
        case Op::Stfdu:  return mem(false, stfd, true);
        case Op::Stfdx:  return mem(true, stfd, false);
        case Op::Stfdux: return mem(true, stfd, true);
        case Op::Stfiwx: return mem(true, "c.write32(ea, static_cast<uint32_t>(" + FS + "));", false);

        // Arithmetic (fmul and the multiply-adds take frC as the multiplier)
        case Op::Fadd:    return dbl(a + " + " + b);
        case Op::Fadds:   return sgl(a + " + " + b);
        case Op::Fsub:    return dbl(a + " - " + b);
        case Op::Fsubs:   return sgl(a + " - " + b);
        case Op::Fmul:    return dbl(a + " * " + cc);
        case Op::Fmuls:   return sgl(a + " * " + cc);
        case Op::Fdiv:    return dbl(a + " / " + b);
        case Op::Fdivs:   return sgl(a + " / " + b);
        case Op::Fmadd:   return dbl("std::fma(" + a + ", " + cc + ", " + b + ")");
        case Op::Fmadds:  return sgl("std::fma(" + a + ", " + cc + ", " + b + ")");
        case Op::Fmsub:   return dbl("std::fma(" + a + ", " + cc + ", -" + b + ")");
        case Op::Fmsubs:  return sgl("std::fma(" + a + ", " + cc + ", -" + b + ")");
        case Op::Fnmadd:  return dbl("-std::fma(" + a + ", " + cc + ", " + b + ")");
        case Op::Fnmadds: return sgl("-std::fma(" + a + ", " + cc + ", " + b + ")");
        case Op::Fnmsub:  return dbl("-std::fma(" + a + ", " + cc + ", -" + b + ")");
        case Op::Fnmsubs: return sgl("-std::fma(" + a + ", " + cc + ", -" + b + ")");
        case Op::Fsqrt:   return dbl("std::sqrt(" + b + ")");
        case Op::Fsqrts:  return sgl("std::sqrt(" + b + ")");
        case Op::Fres:    return sgl("1.0 / " + b);
        case Op::Frsqrte: return dbl("1.0 / std::sqrt(" + b + ")");
        case Op::Fsel:    return raw(a + " >= 0.0 ? " + FC + " : " + FB);

        // Moves and sign changes work on the bits (NaNs pass through unchanged)
        case Op::Fmr:   return raw(FB);
        case Op::Fneg:  return raw(FB + " ^ 0x8000000000000000u");
        case Op::Fabs:  return raw(FB + " & 0x7FFFFFFFFFFFFFFFu");
        case Op::Fnabs: return raw(FB + " | 0x8000000000000000u");

        // Conversion and compare
        case Op::Frsp:   return sgl(b);
        case Op::Fctiw:  return raw("ops::fctiw(s, " + FB + ", false)");
        case Op::Fctiwz: return raw("ops::fctiw(s, " + FB + ", true)");
        case Op::Fcmpu:
        case Op::Fcmpo:  return "ops::fcmp(s, " + std::to_string(in.crfd()) + ", " + FA + ", " + FB + ");";

        // FPSCR
        case Op::Mffs:   return raw("0xFFF8000000000000u | s.fpscr");
        case Op::Mcrfs:  return "ops::mcrfs(s, " + std::to_string(in.crfd()) + ", " + std::to_string(in.crfs()) + ");";
        case Op::Mtfsf: {
            uint32_t mask = 0;
            for (uint32_t f = 0; f < 8; ++f)
                if (in.fm() & (0x80u >> f)) mask |= 0xF0000000u >> (4 * f);
            return "s.fpscr = (s.fpscr & " + hex(~mask) + ") | (static_cast<uint32_t>(" + FB + ") & " + hex(mask) +
                   "); ops::fpscr_written(s);" + cr1;
        }
        case Op::Mtfsfi: {
            const uint32_t shift = 28 - 4 * in.crfd();
            return "s.fpscr = (s.fpscr & " + hex(~(0xFu << shift)) + ") | " + hex(in.fpimm() << shift) +
                   "; ops::fpscr_written(s);" + cr1;
        }
        case Op::Mtfsb0:
            return "s.fpscr &= " + hex(~(0x80000000u >> in.rd())) + "; ops::fpscr_written(s);" + cr1;
        case Op::Mtfsb1:
            return "s.fpscr |= " + hex(0x80000000u >> in.rd()) + "; ops::fpscr_written(s);" + cr1;

        // External control (eciwx/ecowx): not used by Model 3 games.
        default:
            return unimplemented(in, pc);
        }
    }

    const analysis::CodeImage& image_;
    const analysis::Analysis& an_;
    const std::set<uint32_t>& compiled_;
    LiftStats& stats_;
    std::map<uint32_t, uint32_t>& resume_sites_;
    std::map<uint32_t, uint32_t>& block_sites_;
    std::set<uint32_t> blocks_;
    std::map<uint32_t, std::string> sites_; // this function's return sites and their labels
    std::string out_;
};

} // namespace

std::vector<std::string> lift(const analysis::CodeImage& image, const analysis::Analysis& an,
                              const LiftOptions& options, LiftStats* stats_out) {
    std::set<uint32_t> compiled;
    for (const auto& [entry, fn] : an.functions)
        if (!fn.blocks.empty() && (options.functions.empty() || options.functions.contains(entry)))
            compiled.insert(entry);

    // Functions have external linkage in their own namespace so the files
    // can call each other; every file declares all of them.
    const std::string ns = "generated::" + options.name;
    std::string head = "// Generated by spikeout-recomp's lifter";
    if (!options.source.empty()) head += " from " + options.source;
    head += ".\n// Derived from ROM data: do not edit, do not commit.\n";
    head += "#include \"recomp/guest.hpp\"\n\n#include <span>\n\n";
    head += "#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Wunused-label\"\n#endif\n";
    head += "#if defined(_MSC_VER)\n#pragma warning(disable : 4102) // unreferenced label\n#endif\n\n";
    head += "namespace " + ns + " {\n\nusing recomp::Context;\nnamespace ops = recomp::ops;\n\n";
    for (uint32_t entry : compiled) head += "void " + function_name(entry) + "(Context& c);\n";
    const std::string tail = "\n} // namespace " + ns + "\n";

    // Split the functions over the files by instruction count.
    const uint32_t parts = std::max(1u, options.files);
    uint64_t total = 0;
    for (uint32_t entry : compiled) total += an.functions.at(entry).instructions;
    std::vector<std::vector<uint32_t>> groups(parts);
    uint64_t done = 0;
    for (uint32_t entry : compiled) {
        const size_t part = total ? std::min<size_t>(parts - 1, static_cast<size_t>(done * parts / total)) : 0;
        groups[part].push_back(entry);
        done += an.functions.at(entry).instructions;
    }

    LiftStats stats;
    std::map<uint32_t, uint32_t> resume_sites, block_sites;
    std::vector<std::string> bodies;
    for (const auto& group : groups) {
        Emitter emitter(image, an, compiled, stats, resume_sites, block_sites);
        for (uint32_t entry : group) emitter.function(an.functions.at(entry));
        bodies.push_back(emitter.take());
    }

    // The table, sorted by address: functions, and return sites that aren't
    // function entries themselves (the runtime resumes those inside the
    // function that has them).
    std::map<uint32_t, std::string> rows;
    for (uint32_t entry : compiled) rows.emplace(entry, "    {" + hex(entry) + ", " + function_name(entry) + "},\n");
    for (const auto& [site, fn] : resume_sites)
        rows.emplace(site, "    {" + hex(site) + ", " + function_name(fn) + ", " + hex(site) + "},\n");
    stats.return_sites = static_cast<uint32_t>(resume_sites.size());
    std::string table = head + "\nconst recomp::FunctionEntry kFunctions[] = {\n";
    for (const auto& [addr, row] : rows) table += row;
    table += "};\n";
    // Every other block start, for save states only (Context::resume_block):
    // kept apart, so a jump the analysis didn't foresee still stops the run.
    table += "\nconst recomp::FunctionEntry kBlocks[] = {\n";
    for (const auto& [start, fn] : block_sites)
        if (!rows.contains(start)) table += "    {" + hex(start) + ", " + function_name(fn) + ", " + hex(start) + "},\n";
    table += "    {0xffffffffu, nullptr, 0},\n};\n" + tail; // (never empty)
    table += "\nnamespace generated {\nstd::span<const recomp::FunctionEntry> " + options.name +
             "_functions() { return " + options.name + "::kFunctions; }\n" +
             "std::span<const recomp::FunctionEntry> " + options.name + "_blocks() { return " + options.name +
             "::kBlocks; }\n} // namespace generated\n";

    std::vector<std::string> files;
    files.push_back(std::move(table));
    for (std::string& body : bodies) {
        if (parts == 1) files[0].insert(files[0].find("\nconst recomp::FunctionEntry"), body);
        else files.push_back(head + body + tail);
    }
    if (stats_out) *stats_out = std::move(stats);
    return files;
}

} // namespace recomp
