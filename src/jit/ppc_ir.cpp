#include "jit/ppc_ir.hpp"

#include "ppc/cpu_state.hpp"
#include "ppc/instruction.hpp"
#include "ppc/timing.hpp"
#include "runtime/memory_map.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <map>

namespace jit {

uint64_t Layout::hash() const {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint32_t v[] = {ctx_cpu,         ctx_bus,          ctx_returned_to,   ctx_resume_at, ctx_blocks,
                          ctx_cycles,      ctx_next_check,   ctx_recent,        ctx_recent_pos, ctx_return_stack,
                          ctx_return_depth, return_stack_size, bus_ram_data,    bus_fast_limit, bus_marks,
                          mark_shift};
    for (uint32_t x : v) h = (h ^ x) * 0x100000001B3ull;
    return h;
}

namespace {

using analysis::Block;
using analysis::Exit;
using analysis::Function;
using ppc::CpuState;
using ppc::Instruction;
using ppc::Op;
using llvm::BasicBlock;
using llvm::Value;

uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    const uint32_t from = 0xFFFFFFFFu >> mb;
    const uint32_t to = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (from & to) : (from | to);
}

// The CpuState field of an SPR (as the C++ lifter's spr_field), or -1.
int spr_offset(uint32_t spr) {
    switch (spr) {
    case 1:    return offsetof(CpuState, xer);
    case 8:    return offsetof(CpuState, lr);
    case 9:    return offsetof(CpuState, ctr);
    case 18:   return offsetof(CpuState, dsisr);
    case 19:   return offsetof(CpuState, dar);
    case 22:   return offsetof(CpuState, dec);
    case 25:   return offsetof(CpuState, sdr1);
    case 26:   return offsetof(CpuState, srr0);
    case 27:   return offsetof(CpuState, srr1);
    case 282:  return offsetof(CpuState, ear);
    case 287:  return offsetof(CpuState, pvr);
    case 976:  return offsetof(CpuState, dmiss);
    case 977:  return offsetof(CpuState, dcmp);
    case 978:  return offsetof(CpuState, hash1);
    case 979:  return offsetof(CpuState, hash2);
    case 980:  return offsetof(CpuState, imiss);
    case 981:  return offsetof(CpuState, icmp);
    case 982:  return offsetof(CpuState, rpa);
    case 1008: return offsetof(CpuState, hid0);
    case 1009: return offsetof(CpuState, hid1);
    case 1010: return offsetof(CpuState, iabr);
    default:   break;
    }
    if (spr >= 272 && spr <= 275) return static_cast<int>(offsetof(CpuState, sprg) + 4 * (spr - 272));
    if (spr >= 528 && spr <= 535) return static_cast<int>(offsetof(CpuState, ibat) + 4 * (spr - 528));
    if (spr >= 536 && spr <= 543) return static_cast<int>(offsetof(CpuState, dbat) + 4 * (spr - 536));
    return -1;
}

std::string fn_name(const std::string& prefix, uint32_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "_f_%08x", addr);
    return prefix + buf;
}

// The state shared by every module of one lift (as the C++ lifter's
// resume_sites / block_sites, shared by the emitters of all files).
struct LiftState {
    const LiftInput* in;
    std::map<uint32_t, uint32_t> resume_sites; // site -> the first function that has it
    std::map<uint32_t, uint32_t> block_sites;  // block start -> the first function that has it
};

class Emitter {
public:
    Emitter(llvm::Module& m, const Layout& layout, recomp::LiftStats& stats)
        : C(m.getContext()), M(m), b(C), L(layout), stats_(stats) {
        i1 = b.getInt1Ty(), i8 = b.getInt8Ty(), i16 = b.getInt16Ty(), i32 = b.getInt32Ty(), i64 = b.getInt64Ty();
        f32 = b.getFloatTy(), f64 = b.getDoubleTy();
        ptr = llvm::PointerType::get(C, 0);
        void_fn = llvm::FunctionType::get(b.getVoidTy(), {ptr}, false);
        unlikely_ = llvm::MDBuilder(C).createBranchWeights(1, 2000);
    }

    llvm::Function* function_decl(const std::string& prefix, uint32_t entry) {
        const std::string name = fn_name(prefix, entry);
        llvm::Function* f = M.getFunction(name);
        if (!f) {
            f = llvm::Function::Create(void_fn, llvm::GlobalValue::ExternalLinkage, name, M);
            f->setDSOLocal(true);
        }
        return f;
    }

    void function(LiftState& ls, const Function& fn) {
        ls_ = &ls;
        const LiftInput& in = *ls.in;
        blocks_.clear();
        blocks_.insert(fn.blocks.begin(), fn.blocks.end());
        sites_.clear();
        labels_.clear();
        ++stats_.functions;

        F = function_decl(in.prefix, fn.entry);
        F->addFnAttr(llvm::Attribute::getWithUWTableKind(C, llvm::UWTableKind::Async));
        ctx = F->getArg(0);
        BasicBlock* entry = BasicBlock::Create(C, "entry", F);
        BasicBlock* resume = BasicBlock::Create(C, "resume", F);
        for (uint32_t start : fn.blocks) labels_[start] = BasicBlock::Create(C, label_name("L", start), F);

        b.SetInsertPoint(entry);
        Value* at = ld(i32, L.ctx_resume_at);
        b.CreateCondBr(b.CreateICmpNE(at, b.getInt32(0)), resume, labels_.at(fn.entry), unlikely_);

        // The body, from a block of its own (the blocks' labels can't be the entry).
        b.SetInsertPoint(BasicBlock::Create(C, "body", F));
        for (uint32_t start : fn.blocks) block(ls.in->analysis->blocks.at(start));
        if (!terminated()) b.CreateRetVoid();

        // Resuming at a return site (Context::call sets resume_at), or at any
        // block (a save state loaded: Context::resume_block): go to it.
        b.SetInsertPoint(resume);
        st(b.getInt32(0), L.ctx_resume_at);
        std::set<uint32_t> cases;
        std::vector<std::pair<uint32_t, BasicBlock*>> targets;
        for (const auto& [site, bb] : sites_) {
            targets.emplace_back(site, bb);
            ls.resume_sites.emplace(site, fn.entry);
            cases.insert(site);
        }
        for (uint32_t start : fn.blocks) {
            if (!cases.insert(start).second) continue;
            targets.emplace_back(start, labels_.at(start));
            ls.block_sites.emplace(start, fn.entry);
        }
        llvm::SwitchInst* sw = b.CreateSwitch(at, labels_.at(fn.entry), static_cast<unsigned>(targets.size()));
        for (const auto& [addr, bb] : targets) sw->addCase(b.getInt32(addr), bb);

        // Blocks no code reaches (after a noreturn call, say) are left for the optimizer; each must end.
        for (BasicBlock& bb : *F)
            if (!bb.getTerminator()) {
                b.SetInsertPoint(&bb);
                b.CreateUnreachable();
            }
    }

private:
    static std::string label_name(const char* kind, uint32_t addr) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%s_%08x", kind, addr);
        return buf;
    }

    // ---- Basics ----
    bool terminated() { return b.GetInsertBlock()->getTerminator() != nullptr; }
    // Code after a terminator (C++ code after a return): a block nothing reaches.
    void reopen() {
        if (terminated()) b.SetInsertPoint(BasicBlock::Create(C, "", F));
    }
    // A label: the code before falls into it, as in C++.
    void place(BasicBlock* bb) {
        if (!terminated()) b.CreateBr(bb);
        b.SetInsertPoint(bb);
    }
    BasicBlock* new_bb(const char* name = "") { return BasicBlock::Create(C, name, F); }

    Value* at(uint32_t off) { return b.CreateConstInBoundsGEP1_32(i8, ctx, off); }
    Value* ld(llvm::Type* t, uint32_t off) { return b.CreateLoad(t, at(off)); }
    void st(Value* v, uint32_t off) { b.CreateStore(v, at(off)); }
    uint32_t cpu(size_t off) const { return L.ctx_cpu + static_cast<uint32_t>(off); }

    Value* G(uint32_t r) { return ld(i32, cpu(offsetof(CpuState, gpr) + 4 * r)); }
    void SG(uint32_t r, Value* v) { st(v, cpu(offsetof(CpuState, gpr) + 4 * r)); }
    Value* A0(const Instruction& in) { return in.ra() == 0 ? b.getInt32(0) : G(in.ra()); }
    Value* FR(uint32_t r) { return ld(i64, cpu(offsetof(CpuState, fpr) + 8 * r)); }
    void SFR(uint32_t r, Value* v) { st(v, cpu(offsetof(CpuState, fpr) + 8 * r)); }
    Value* field(size_t off) { return ld(i32, cpu(off)); }
    void set_field(size_t off, Value* v) { st(v, cpu(off)); }
    Value* xer() { return field(offsetof(CpuState, xer)); }
    void set_xer(Value* v) { set_field(offsetof(CpuState, xer), v); }
    Value* cr() { return field(offsetof(CpuState, cr)); }
    void set_cr(Value* v) { set_field(offsetof(CpuState, cr), v); }
    Value* fpscr() { return field(offsetof(CpuState, fpscr)); }
    void set_fpscr(Value* v) { set_field(offsetof(CpuState, fpscr), v); }
    Value* k(uint32_t v) { return b.getInt32(v); }
    Value* k64(uint64_t v) { return b.getInt64(v); }

    // The runtime's helpers (src/jit/jit.cpp), by name: (Context*, the arguments' types).
    Value* call(const char* name, llvm::Type* ret, std::initializer_list<Value*> args) {
        std::vector<llvm::Type*> params{ptr};
        for (Value* v : args) params.push_back(v->getType());
        llvm::FunctionCallee f = M.getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
        if (auto* fn = llvm::dyn_cast<llvm::Function>(f.getCallee())) fn->setDSOLocal(true);
        std::vector<Value*> a{ctx};
        a.insert(a.end(), args.begin(), args.end());
        return b.CreateCall(f, a);
    }
    void call_void(const char* name, std::initializer_list<Value*> args) { call(name, b.getVoidTy(), args); }
    void call_noreturn(const char* name, std::initializer_list<Value*> args) {
        call_void(name, args);
        if (auto* fn = M.getFunction(name)) fn->setDoesNotReturn();
        b.CreateUnreachable();
    }

    // ---- Flags (recomp/guest.hpp's ops) ----
    Value* so() { return b.CreateLShr(xer(), 31); }
    Value* ca() { return b.CreateAnd(b.CreateLShr(xer(), 29), 1); }
    void set_ca(Value* carry) {
        Value* x = xer();
        set_xer(b.CreateSelect(carry, b.CreateOr(x, CpuState::kXerCA), b.CreateAnd(x, ~CpuState::kXerCA)));
    }
    void set_ov(Value* overflow) {
        Value* x = xer();
        set_xer(b.CreateSelect(overflow, b.CreateOr(x, CpuState::kXerOV | CpuState::kXerSO),
                               b.CreateAnd(x, ~CpuState::kXerOV)));
    }
    void set_crf(uint32_t n, Value* v) {
        const uint32_t shift = 28 - 4 * n;
        Value* c = cr();
        set_cr(b.CreateOr(b.CreateAnd(c, ~(0xFu << shift)), b.CreateShl(b.CreateAnd(v, 0xF), shift)));
    }
    Value* crf(uint32_t n) { return b.CreateAnd(b.CreateLShr(cr(), 28 - 4 * n), 0xF); }
    Value* crbit(uint32_t n) { return b.CreateTrunc(b.CreateLShr(cr(), 31 - n), i1); }
    void set_crbit(uint32_t n, Value* v) {
        const uint32_t mask = 1u << (31 - n);
        Value* c = cr();
        set_cr(b.CreateSelect(v, b.CreateOr(c, mask), b.CreateAnd(c, ~mask)));
    }
    // cmps / cmpu: LT GT EQ | SO.
    void cmp(uint32_t field, Value* a, Value* bb, bool is_signed) {
        Value* lt = is_signed ? b.CreateICmpSLT(a, bb) : b.CreateICmpULT(a, bb);
        Value* gt = is_signed ? b.CreateICmpSGT(a, bb) : b.CreateICmpUGT(a, bb);
        Value* v = b.CreateSelect(lt, k(8), b.CreateSelect(gt, k(4), k(2)));
        set_crf(field, b.CreateOr(v, so()));
    }
    void set_cr0(Value* r) { cmp(0, r, k(0), true); }

    Value* addx(Value* x, Value* y, Value* cin, bool record_ca, bool oe) {
        Value* wide = b.CreateAdd(b.CreateAdd(b.CreateZExt(x, i64), b.CreateZExt(y, i64)), b.CreateZExt(cin, i64));
        Value* r = b.CreateTrunc(wide, i32);
        if (record_ca) set_ca(b.CreateICmpNE(b.CreateLShr(wide, 32), k64(0)));
        if (oe) set_ov(b.CreateTrunc(b.CreateLShr(b.CreateAnd(b.CreateXor(x, r), b.CreateXor(y, r)), 31), i1));
        return r;
    }
    Value* rotl(Value* x, Value* n) {
        llvm::Function* fshl = llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::fshl, {i32});
        return b.CreateCall(fshl, {x, x, n}); // (n mod 32; 0 leaves x)
    }
    Value* sraw(Value* value, Value* n) { // n < 64
        Value* neg = b.CreateICmpSLT(value, k(0));
        Value* big = b.CreateICmpUGE(n, k(32));
        Value* zero = b.CreateICmpEQ(n, k(0));
        Value* nn = b.CreateAnd(n, 31);
        Value* mask = b.CreateSub(b.CreateShl(k(1), nn), k(1));
        Value* lost = b.CreateICmpNE(b.CreateAnd(value, mask), k(0));
        Value* carry = b.CreateSelect(big, neg, b.CreateSelect(zero, b.getFalse(), b.CreateAnd(neg, lost)));
        set_ca(carry);
        return b.CreateSelect(big, b.CreateSelect(neg, k(0xFFFFFFFFu), k(0)), b.CreateAShr(value, nn));
    }
    Value* bswap(Value* v) {
        return b.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::bswap, {v->getType()}), {v});
    }
    Value* bswap16(Value* v) { // ((v & 0xFF) << 8) | ((v >> 8) & 0xFF)
        return b.CreateOr(b.CreateShl(b.CreateAnd(v, 0xFF), 8), b.CreateAnd(b.CreateLShr(v, 8), 0xFF));
    }

    // ---- Memory (Bus's inline fast paths; anything else the bus's own code) ----
    Value* bus() { return ld(ptr, L.ctx_bus); }
    Value* ram_ptr(Value* bus, Value* ea) {
        Value* base = b.CreateLoad(ptr, b.CreateConstInBoundsGEP1_32(i8, bus, L.bus_ram_data));
        return b.CreateInBoundsGEP(i8, base, b.CreateZExt(ea, i64));
    }
    Value* read(unsigned bytes, Value* ea) {
        static const char* names[] = {nullptr, "spk_read8", "spk_read16", nullptr, "spk_read32"};
        const uint32_t limit = m3::kWorkRamSize - (bytes - 1);
        BasicBlock *fast = new_bb(), *slow = new_bb(), *join = new_bb();
        b.CreateCondBr(b.CreateICmpULT(ea, k(limit)), fast, slow);
        b.SetInsertPoint(fast);
        llvm::Type* t = bytes == 1 ? i8 : bytes == 2 ? i16 : i32;
        Value* v = b.CreateAlignedLoad(t, ram_ptr(bus(), ea), llvm::MaybeAlign(1));
        if (bytes > 1) v = bswap(v);
        v = b.CreateZExt(v, i32);
        BasicBlock* fast_end = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        Value* s = call(names[bytes], i32, {ea});
        BasicBlock* slow_end = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        llvm::PHINode* phi = b.CreatePHI(i32, 2);
        phi->addIncoming(v, fast_end);
        phi->addIncoming(s, slow_end);
        return phi;
    }
    Value* read8(Value* ea) { return read(1, ea); }
    Value* read16(Value* ea) { return read(2, ea); }
    Value* read32(Value* ea) { return read(4, ea); }
    // Two 32-bit reads, high word first (Bus::read64).
    Value* read64(Value* ea) {
        BasicBlock *fast = new_bb(), *slow = new_bb(), *join = new_bb();
        b.CreateCondBr(b.CreateICmpULT(ea, k(m3::kWorkRamSize - 7)), fast, slow);
        b.SetInsertPoint(fast);
        Value* v = bswap(b.CreateAlignedLoad(i64, ram_ptr(bus(), ea), llvm::MaybeAlign(1)));
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        Value* s = call("spk_read64", i64, {ea});
        b.CreateBr(join);
        b.SetInsertPoint(join);
        llvm::PHINode* phi = b.CreatePHI(i64, 2);
        phi->addIncoming(v, fast);
        phi->addIncoming(s, slow);
        return phi;
    }
    void mark(Value* bus, std::initializer_list<Value*> addrs) {
        Value* marks = b.CreateLoad(ptr, b.CreateConstInBoundsGEP1_32(i8, bus, L.bus_marks));
        BasicBlock *set = new_bb(), *done = new_bb();
        b.CreateCondBr(b.CreateIsNotNull(marks), set, done, unlikely_);
        b.SetInsertPoint(set);
        for (Value* a : addrs)
            b.CreateStore(b.getInt8(1), b.CreateInBoundsGEP(i8, marks, b.CreateZExt(b.CreateLShr(a, L.mark_shift), i64)));
        b.CreateBr(done);
        b.SetInsertPoint(done);
    }
    void write(unsigned bytes, Value* ea, Value* value) { // value: i32
        static const char* names[] = {nullptr, "spk_write8", "spk_write16", nullptr, "spk_write32"};
        const uint32_t index = bytes == 1 ? 0 : bytes == 2 ? 1 : 2;
        BasicBlock *fast = new_bb(), *slow = new_bb(), *join = new_bb();
        Value* bs = bus();
        Value* limit = b.CreateLoad(i32, b.CreateConstInBoundsGEP1_32(i8, bs, L.bus_fast_limit + 4 * index));
        b.CreateCondBr(b.CreateICmpULT(ea, limit), fast, slow);
        b.SetInsertPoint(fast);
        llvm::Type* t = bytes == 1 ? i8 : bytes == 2 ? i16 : i32;
        Value* v = b.CreateTrunc(value, t);
        if (bytes > 1) v = bswap(v);
        b.CreateAlignedStore(v, ram_ptr(bs, ea), llvm::MaybeAlign(1));
        if (bytes == 1) mark(bs, {ea});
        else mark(bs, {ea, b.CreateAdd(ea, k(bytes - 1))});
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        call_void(names[bytes], {ea, value});
        b.CreateBr(join);
        b.SetInsertPoint(join);
    }
    void write8(Value* ea, Value* v) { write(1, ea, v); }
    void write16(Value* ea, Value* v) { write(2, ea, v); }
    void write32(Value* ea, Value* v) { write(4, ea, v); }
    // Two 32-bit writes, high word first (Bus::write64).
    void write64(Value* ea, Value* value) {
        BasicBlock *fast = new_bb(), *slow = new_bb(), *join = new_bb();
        Value* bs = bus();
        Value* limit = b.CreateLoad(i32, b.CreateConstInBoundsGEP1_32(i8, bs, L.bus_fast_limit + 8));
        Value* lo = b.CreateAdd(ea, k(4));
        b.CreateCondBr(b.CreateAnd(b.CreateICmpULT(ea, limit), b.CreateICmpULT(lo, limit)), fast, slow);
        b.SetInsertPoint(fast);
        b.CreateAlignedStore(bswap(value), ram_ptr(bs, ea), llvm::MaybeAlign(1));
        mark(bs, {ea, b.CreateAdd(ea, k(3)), lo, b.CreateAdd(ea, k(7))});
        b.CreateBr(join);
        b.SetInsertPoint(slow);
        call_void("spk_write64", {ea, value});
        b.CreateBr(join);
        b.SetInsertPoint(join);
    }

    // ---- Clock (Context::tick) ----
    void tick(uint32_t pc, uint32_t cycles) {
        set_field(offsetof(CpuState, pc), k(pc));
        Value* pos = ld(i32, L.ctx_recent_pos);
        st(b.CreateAdd(pos, k(1)), L.ctx_recent_pos);
        b.CreateStore(k(pc), b.CreateInBoundsGEP(i32, at(L.ctx_recent), b.CreateZExt(b.CreateAnd(pos, 15), i64)));
        st(b.CreateAdd(ld(i64, L.ctx_blocks), k64(1)), L.ctx_blocks);
        BasicBlock *slow = new_bb(), *go = new_bb();
        b.CreateCondBr(b.CreateICmpUGE(ld(i64, L.ctx_cycles), ld(i64, L.ctx_next_check)), slow, go, unlikely_);
        b.SetInsertPoint(slow);
        call_void("spk_slow_tick", {k(pc)});
        b.CreateBr(go);
        b.SetInsertPoint(go);
        st(b.CreateAdd(ld(i64, L.ctx_cycles), k64(cycles)), L.ctx_cycles);
    }
    void recheck() { st(ld(i64, L.ctx_cycles), L.ctx_next_check); }

    // ---- Control flow ----
    void block(const Block& bl) {
        ++stats_.blocks;
        place(labels_.at(bl.start));
        std::vector<Instruction> code;
        for (uint32_t pc = bl.start; pc < bl.end; pc += 4)
            code.push_back(ppc::decode(ls_->in->image->read32(pc).value_or(0)));
        const uint32_t cycles = ppc::block_cycles(code);
        stats_.cycles += cycles;
        tick(bl.start, cycles);
        if (ls_->in->hooks.contains(bl.start)) call_void("spk_hook", {k(bl.start)});
        for (uint32_t pc = bl.start; pc < bl.end; pc += 4) {
            const Instruction in = code[(pc - bl.start) / 4];
            ++stats_.instructions;
            reopen();
            switch (in.op) {
            case Op::B: case Op::Bc: case Op::Bclr: case Op::Bcctr: case Op::Rfi:
                branch(in, pc, bl);
                if (in.lk() && in.op != Op::Rfi) return_site(pc + 4, bl);
                break;
            default:
                instruction(in, pc);
                break;
            }
        }
        reopen();
        switch (bl.exit) {
        case Exit::Fallthrough: edge(bl.end); break;
        case Exit::OutOfImage:
        case Exit::Trap:        call_noreturn("spk_missing", {k(bl.end)}); break;
        default: break;
        }
    }

    void return_site(uint32_t site, const Block& bl) {
        if (site < bl.end) {
            BasicBlock* r = new_bb(label_name("R", site).c_str());
            place(r);
            sites_.emplace(site, r);
        } else if (blocks_.contains(site)) {
            sites_.emplace(site, labels_.at(site));
        }
    }

    bool compiled(uint32_t target) const { return ls_->in->functions.contains(target); }
    void tail_call(uint32_t target) {
        llvm::CallInst* c = b.CreateCall(function_decl(ls_->in->prefix, target), {ctx});
        c->setTailCall();
        b.CreateRetVoid();
    }
    void jump(Value* target) { // c.jump(x); return;
        llvm::CallInst* c = llvm::cast<llvm::CallInst>(call("spk_jump", b.getVoidTy(), {target}));
        c->setTailCall();
        b.CreateRetVoid();
    }
    void edge(uint32_t target) {
        if (blocks_.contains(target)) b.CreateBr(labels_.at(target));
        else if (compiled(target)) tail_call(target);
        else jump(k(target));
    }
    void after_call(uint32_t next) {
        BasicBlock *check = new_bb(), *ret = new_bb(), *go = new_bb();
        b.CreateCondBr(b.CreateICmpNE(ld(i32, L.ctx_returned_to), k(next)), check, go, unlikely_);
        b.SetInsertPoint(check);
        Value* r = call("spk_follow_return", i32, {k(next)});
        b.CreateCondBr(b.CreateICmpNE(r, k(0)), ret, go);
        b.SetInsertPoint(ret);
        b.CreateRetVoid();
        b.SetInsertPoint(go);
    }
    void push_return(uint32_t next) {
        Value* depth = ld(i64, L.ctx_return_depth);
        BasicBlock *put = new_bb(), *go = new_bb();
        b.CreateCondBr(b.CreateICmpULT(depth, k64(L.return_stack_size)), put, go);
        b.SetInsertPoint(put);
        b.CreateStore(k(next), b.CreateInBoundsGEP(i32, at(L.ctx_return_stack), depth));
        b.CreateBr(go);
        b.SetInsertPoint(go);
        st(b.CreateAdd(depth, k64(1)), L.ctx_return_depth);
    }
    void pop_return() { st(b.CreateSub(ld(i64, L.ctx_return_depth), k64(1)), L.ctx_return_depth); }
    // c.push_return(next); f(c) or c.call(target); c.pop_return(); after_call(next)
    void call_to(uint32_t target, uint32_t next) {
        push_return(next);
        if (compiled(target)) b.CreateCall(function_decl(ls_->in->prefix, target), {ctx});
        else call_void("spk_call", {k(target)});
        pop_return();
        after_call(next);
    }
    void call_indirect(Value* t, uint32_t next) {
        push_return(next);
        call_void("spk_call", {t});
        pop_return();
        after_call(next);
    }

    // The bc condition, nullptr when it always branches. CTR is decremented first.
    Value* condition(const Instruction& in) {
        const uint32_t bo = in.bo();
        Value *c_ctr = nullptr, *c_cr = nullptr;
        if (!(bo & 0x04)) {
            Value* ctr = b.CreateSub(field(offsetof(CpuState, ctr)), k(1));
            set_field(offsetof(CpuState, ctr), ctr);
            c_ctr = (bo & 0x02) ? b.CreateICmpEQ(ctr, k(0)) : b.CreateICmpNE(ctr, k(0));
        }
        if (!(bo & 0x10)) {
            Value* bit = crbit(in.bi());
            c_cr = (bo & 0x08) ? bit : b.CreateNot(bit);
        }
        if (!c_ctr) return c_cr;
        if (!c_cr) return c_ctr;
        return b.CreateAnd(c_ctr, c_cr);
    }
    // if (cond) { body } — body may end in a return.
    template <class Body> void guarded(Value* cond, Body body) {
        if (!cond) {
            body();
            return;
        }
        BasicBlock *then = new_bb(), *after = new_bb();
        b.CreateCondBr(cond, then, after);
        b.SetInsertPoint(then);
        body();
        place(after);
    }

    void branch(const Instruction& in, uint32_t pc, const Block& bl) {
        const uint32_t next = pc + 4;
        auto set_lr = [&] { set_field(offsetof(CpuState, lr), k(next)); };

        if (in.op == Op::Rfi) {
            call_void("spk_rfi", {});
            b.CreateRetVoid();
            return;
        }

        if (in.op == Op::B || in.op == Op::Bc) {
            const uint32_t target = *ppc::branch_target(in, pc);
            if (in.lk()) {
                set_lr();
                Value* cond = in.op == Op::B ? nullptr : condition(in);
                if (target == next) return; // bl $+4 reads the PC (the condition still evaluated)
                guarded(cond, [&] { call_to(target, next); });
                return;
            }
            Value* cond = in.op == Op::B ? nullptr : condition(in);
            if (!cond) {
                edge(target);
                return;
            }
            guarded(cond, [&] { edge(target); });
            edge(next);
            return;
        }

        // bclr / bcctr
        const size_t reg = in.op == Op::Bclr ? offsetof(CpuState, lr) : offsetof(CpuState, ctr);
        if (in.lk()) {
            Value* t = b.CreateAnd(field(reg), ~3u);
            set_lr();
            Value* cond = condition(in);
            guarded(cond, [&] { call_indirect(t, next); });
            return;
        }
        Value* cond = condition(in);
        auto act = [&] {
            switch (bl.exit) {
            case Exit::Return:
                st(field(offsetof(CpuState, lr)), L.ctx_returned_to);
                b.CreateRetVoid();
                break;
            case Exit::Jump:
            case Exit::TailCall:
                if (bl.successors.empty()) jump(b.CreateAnd(field(reg), ~3u));
                else edge(bl.successors[0]);
                break;
            case Exit::JumpTable: {
                Value* v = b.CreateAnd(field(reg), ~3u);
                BasicBlock* def = new_bb();
                const size_t targets = bl.successors.size() - (cond ? 1 : 0);
                llvm::SwitchInst* sw = b.CreateSwitch(v, def, static_cast<unsigned>(targets));
                for (size_t i = 0; i < targets; ++i) {
                    const uint32_t t = bl.successors[i];
                    if (blocks_.contains(t)) {
                        sw->addCase(b.getInt32(t), labels_.at(t));
                    } else {
                        BasicBlock* c = new_bb();
                        sw->addCase(b.getInt32(t), c);
                        b.SetInsertPoint(c);
                        edge(t);
                    }
                }
                b.SetInsertPoint(def);
                jump(v);
                break;
            }
            default:
                jump(b.CreateAnd(field(reg), ~3u));
                break;
            }
        };
        if (!cond) {
            act();
            return;
        }
        guarded(cond, act);
        edge(next);
    }

    void unimplemented(const Instruction& in, uint32_t pc) {
        ++stats_.unimplemented[ppc::mnemonic(in.op)];
        call_noreturn("spk_unimplemented", {k(pc), k(in.raw)});
    }

    // ---- Instructions ----
    void instruction(const Instruction& in, uint32_t pc) {
        const uint32_t d = in.rd(), a = in.ra(), bb = in.rb(), s = in.rs();
        const bool rc = in.record();
        Value* simm = k(static_cast<uint32_t>(in.simm()));
        Value* uimm = k(in.uimm());
        auto cr0 = [&](uint32_t reg) {
            if (rc) set_cr0(G(reg));
        };
        auto arith = [&](Value* x, Value* y, Value* cin, bool record_ca) {
            SG(d, addx(x, y, cin, record_ca, in.oe()));
            cr0(d);
        };
        auto logical = [&](Value* v) {
            SG(a, v);
            cr0(a);
        };
        auto ea_d = [&] { return b.CreateAdd(A0(in), simm); };
        auto ea_x = [&] { return b.CreateAdd(A0(in), G(bb)); };
        auto load = [&](Value* ea, auto value, bool update) {
            SG(d, value(ea));
            if (update) SG(a, ea);
        };
        auto store = [&](Value* ea, auto action, bool update) {
            action(ea);
            if (update) SG(a, ea);
        };
        auto r8 = [&](Value* ea) { return read8(ea); };
        auto r16 = [&](Value* ea) { return read16(ea); };
        auto r16s = [&](Value* ea) { return b.CreateSExt(b.CreateTrunc(read16(ea), i16), i32); };
        auto r32 = [&](Value* ea) { return read32(ea); };
        auto w8 = [&](Value* ea) { write8(ea, G(s)); };
        auto w16 = [&](Value* ea) { write16(ea, G(s)); };
        auto w32 = [&](Value* ea) { write32(ea, G(s)); };
        auto crbits = [&](Value* v) { set_crbit(d, v); };
        auto ba = [&] { return crbit(a); };
        auto bb_ = [&] { return crbit(bb); };
        const uint32_t crfd = in.crfd();

        switch (in.op) {
        // Integer arithmetic
        case Op::Addi:    SG(d, b.CreateAdd(A0(in), simm)); return;
        case Op::Addis:   SG(d, b.CreateAdd(A0(in), k(static_cast<uint32_t>(in.simm()) << 16))); return;
        case Op::Addic:   SG(d, addx(G(a), simm, k(0), true, false)); return;
        case Op::AddicRc: SG(d, addx(G(a), simm, k(0), true, false)); set_cr0(G(d)); return;
        case Op::Subfic:  SG(d, addx(b.CreateNot(G(a)), simm, k(1), true, false)); return;
        case Op::Mulli:   SG(d, b.CreateMul(G(a), simm)); return;
        case Op::Add:     arith(G(a), G(bb), k(0), false); return;
        case Op::Addc:    arith(G(a), G(bb), k(0), true); return;
        case Op::Adde:    arith(G(a), G(bb), ca(), true); return;
        case Op::Addme:   arith(G(a), k(0xFFFFFFFFu), ca(), true); return;
        case Op::Addze:   arith(G(a), k(0), ca(), true); return;
        case Op::Subf:    arith(b.CreateNot(G(a)), G(bb), k(1), false); return;
        case Op::Subfc:   arith(b.CreateNot(G(a)), G(bb), k(1), true); return;
        case Op::Subfe:   arith(b.CreateNot(G(a)), G(bb), ca(), true); return;
        case Op::Subfme:  arith(b.CreateNot(G(a)), k(0xFFFFFFFFu), ca(), true); return;
        case Op::Subfze:  arith(b.CreateNot(G(a)), k(0), ca(), true); return;
        case Op::Neg:     arith(b.CreateNot(G(a)), k(0), k(1), false); return;
        case Op::Mullw: {
            Value* p = b.CreateMul(b.CreateSExt(G(a), i64), b.CreateSExt(G(bb), i64));
            if (in.oe()) set_ov(b.CreateICmpNE(p, b.CreateSExt(b.CreateTrunc(p, i32), i64)));
            SG(d, b.CreateTrunc(p, i32));
            cr0(d);
            return;
        }
        case Op::Mulhw:
            SG(d, b.CreateTrunc(b.CreateLShr(b.CreateMul(b.CreateSExt(G(a), i64), b.CreateSExt(G(bb), i64)), 32), i32));
            cr0(d);
            return;
        case Op::Mulhwu:
            SG(d, b.CreateTrunc(b.CreateLShr(b.CreateMul(b.CreateZExt(G(a), i64), b.CreateZExt(G(bb), i64)), 32), i32));
            cr0(d);
            return;
        case Op::Divw: {
            Value *x = G(a), *y = G(bb);
            Value* invalid = b.CreateOr(b.CreateICmpEQ(y, k(0)),
                                        b.CreateAnd(b.CreateICmpEQ(x, k(0x80000000u)), b.CreateICmpEQ(y, k(0xFFFFFFFFu))));
            if (in.oe()) set_ov(invalid);
            Value* q = b.CreateSDiv(x, b.CreateSelect(invalid, k(1), y));
            SG(d, b.CreateSelect(invalid, b.CreateSelect(b.CreateICmpSLT(x, k(0)), k(0xFFFFFFFFu), k(0)), q));
            cr0(d);
            return;
        }
        case Op::Divwu: {
            Value *x = G(a), *y = G(bb);
            Value* zero = b.CreateICmpEQ(y, k(0));
            if (in.oe()) set_ov(zero);
            SG(d, b.CreateSelect(zero, k(0), b.CreateUDiv(x, b.CreateSelect(zero, k(1), y))));
            cr0(d);
            return;
        }

        // Compare
        case Op::Cmp:   cmp(crfd, G(a), G(bb), true); return;
        case Op::Cmpi:  cmp(crfd, G(a), simm, true); return;
        case Op::Cmpl:  cmp(crfd, G(a), G(bb), false); return;
        case Op::Cmpli: cmp(crfd, G(a), uimm, false); return;

        // Logical
        case Op::And:     logical(b.CreateAnd(G(s), G(bb))); return;
        case Op::Andc:    logical(b.CreateAnd(G(s), b.CreateNot(G(bb)))); return;
        case Op::Or:      logical(s == bb ? G(s) : b.CreateOr(G(s), G(bb))); return;
        case Op::Orc:     logical(b.CreateOr(G(s), b.CreateNot(G(bb)))); return;
        case Op::Xor:     logical(b.CreateXor(G(s), G(bb))); return;
        case Op::Nand:    logical(b.CreateNot(b.CreateAnd(G(s), G(bb)))); return;
        case Op::Nor:     logical(b.CreateNot(b.CreateOr(G(s), G(bb)))); return;
        case Op::Eqv:     logical(b.CreateNot(b.CreateXor(G(s), G(bb)))); return;
        case Op::AndiRc:  SG(a, b.CreateAnd(G(s), uimm)); set_cr0(G(a)); return;
        case Op::AndisRc: SG(a, b.CreateAnd(G(s), k(in.uimm() << 16))); set_cr0(G(a)); return;
        case Op::Ori:     SG(a, b.CreateOr(G(s), uimm)); return;
        case Op::Oris:    SG(a, b.CreateOr(G(s), k(in.uimm() << 16))); return;
        case Op::Xori:    SG(a, b.CreateXor(G(s), uimm)); return;
        case Op::Xoris:   SG(a, b.CreateXor(G(s), k(in.uimm() << 16))); return;
        case Op::Cntlzw:
            logical(b.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::ctlz, {i32}), {G(s), b.getFalse()}));
            return;
        case Op::Extsb: logical(b.CreateSExt(b.CreateTrunc(G(s), i8), i32)); return;
        case Op::Extsh: logical(b.CreateSExt(b.CreateTrunc(G(s), i16), i32)); return;

        // Rotate and shift
        case Op::Rlwinm: {
            Value* v = in.sh() ? rotl(G(s), k(in.sh())) : G(s);
            logical(b.CreateAnd(v, ppc_mask(in.mb(), in.me())));
            return;
        }
        case Op::Rlwimi: {
            const uint32_t m = ppc_mask(in.mb(), in.me());
            Value* r = rotl(G(s), k(in.sh()));
            logical(b.CreateOr(b.CreateAnd(r, m), b.CreateAnd(G(a), ~m)));
            return;
        }
        case Op::Rlwnm: logical(b.CreateAnd(rotl(G(s), G(bb)), ppc_mask(in.mb(), in.me()))); return;
        case Op::Slw: {
            Value *v = G(s), *n = G(bb);
            logical(b.CreateSelect(b.CreateICmpNE(b.CreateAnd(n, 0x20), k(0)), k(0), b.CreateShl(v, b.CreateAnd(n, 31))));
            return;
        }
        case Op::Srw: {
            Value *v = G(s), *n = G(bb);
            logical(b.CreateSelect(b.CreateICmpNE(b.CreateAnd(n, 0x20), k(0)), k(0), b.CreateLShr(v, b.CreateAnd(n, 31))));
            return;
        }
        case Op::Sraw: {
            Value *v = G(s), *n = b.CreateAnd(G(bb), 0x3F);
            logical(sraw(v, n));
            return;
        }
        case Op::Srawi: logical(sraw(G(s), k(in.sh()))); return;

        // Loads
        case Op::Lbz:   load(ea_d(), r8, false); return;
        case Op::Lbzu:  load(ea_d(), r8, true); return;
        case Op::Lbzx:  load(ea_x(), r8, false); return;
        case Op::Lbzux: load(ea_x(), r8, true); return;
        case Op::Lhz:   load(ea_d(), r16, false); return;
        case Op::Lhzu:  load(ea_d(), r16, true); return;
        case Op::Lhzx:  load(ea_x(), r16, false); return;
        case Op::Lhzux: load(ea_x(), r16, true); return;
        case Op::Lha:   load(ea_d(), r16s, false); return;
        case Op::Lhau:  load(ea_d(), r16s, true); return;
        case Op::Lhax:  load(ea_x(), r16s, false); return;
        case Op::Lhaux: load(ea_x(), r16s, true); return;
        case Op::Lwz:   load(ea_d(), r32, false); return;
        case Op::Lwzu:  load(ea_d(), r32, true); return;
        case Op::Lwzx:  load(ea_x(), r32, false); return;
        case Op::Lwzux: load(ea_x(), r32, true); return;
        case Op::Lhbrx: load(ea_x(), [&](Value* ea) { return bswap16(read16(ea)); }, false); return;
        case Op::Lwbrx: load(ea_x(), [&](Value* ea) { return bswap(read32(ea)); }, false); return;
        case Op::Lwarx: {
            Value* ea = ea_x();
            st(b.getInt8(1), cpu(offsetof(CpuState, reserve)));
            set_field(offsetof(CpuState, reserve_addr), ea);
            SG(d, read32(ea));
            return;
        }
        case Op::Lmw: {
            Value* ea = ea_d();
            for (uint32_t r = d; r < 32; ++r, ea = b.CreateAdd(ea, k(4))) SG(r, read32(ea));
            return;
        }
        case Op::Lswi: call_void("spk_load_string", {k(d), A0(in), k(in.nb() ? in.nb() : 32)}); return;
        case Op::Lswx: {
            Value* ea = b.CreateAdd(A0(in), G(bb));
            call_void("spk_load_string", {k(d), ea, b.CreateAnd(xer(), 0x7F)});
            return;
        }

        // Stores
        case Op::Stb:    store(ea_d(), w8, false); return;
        case Op::Stbu:   store(ea_d(), w8, true); return;
        case Op::Stbx:   store(ea_x(), w8, false); return;
        case Op::Stbux:  store(ea_x(), w8, true); return;
        case Op::Sth:    store(ea_d(), w16, false); return;
        case Op::Sthu:   store(ea_d(), w16, true); return;
        case Op::Sthx:   store(ea_x(), w16, false); return;
        case Op::Sthux:  store(ea_x(), w16, true); return;
        case Op::Stw:    store(ea_d(), w32, false); return;
        case Op::Stwu:   store(ea_d(), w32, true); return;
        case Op::Stwx:   store(ea_x(), w32, false); return;
        case Op::Stwux:  store(ea_x(), w32, true); return;
        case Op::Sthbrx: store(ea_x(), [&](Value* ea) { write16(ea, bswap16(G(s))); }, false); return;
        case Op::Stwbrx: store(ea_x(), [&](Value* ea) { write32(ea, bswap(G(s))); }, false); return;
        case Op::StwcxRc: {
            Value* ea = ea_x();
            Value* reserved = b.CreateICmpNE(ld(i8, cpu(offsetof(CpuState, reserve))), b.getInt8(0));
            BasicBlock *yes = new_bb(), *no = new_bb(), *done = new_bb();
            b.CreateCondBr(reserved, yes, no);
            b.SetInsertPoint(yes);
            write32(ea, G(s));
            set_crf(0, b.CreateOr(k(2), so()));
            b.CreateBr(done);
            b.SetInsertPoint(no);
            set_crf(0, so());
            b.CreateBr(done);
            b.SetInsertPoint(done);
            st(b.getInt8(0), cpu(offsetof(CpuState, reserve)));
            return;
        }
        case Op::Stmw: {
            Value* ea = ea_d();
            for (uint32_t r = s; r < 32; ++r, ea = b.CreateAdd(ea, k(4))) write32(ea, G(r));
            return;
        }
        case Op::Stswi: call_void("spk_store_string", {k(s), A0(in), k(in.nb() ? in.nb() : 32)}); return;
        case Op::Stswx: {
            Value* ea = b.CreateAdd(A0(in), G(bb));
            call_void("spk_store_string", {k(s), ea, b.CreateAnd(xer(), 0x7F)});
            return;
        }

        // Caches, TLB and ordering
        case Op::Dcbz: call_void("spk_dcbz", {b.CreateAdd(A0(in), G(bb))}); return;
        case Op::Dcbf: case Op::Dcbi: case Op::Dcbst: case Op::Dcbt: case Op::Dcbtst: case Op::Icbi:
        case Op::Eieio: case Op::Isync: case Op::Sync:
        case Op::Tlbia: case Op::Tlbie: case Op::Tlbld: case Op::Tlbli: case Op::Tlbsync:
            return;

        // Condition register
        case Op::Crand:  crbits(b.CreateAnd(ba(), bb_())); return;
        case Op::Crandc: crbits(b.CreateAnd(ba(), b.CreateNot(bb_()))); return;
        case Op::Creqv:  crbits(b.CreateICmpEQ(ba(), bb_())); return;
        case Op::Crnand: crbits(b.CreateNot(b.CreateAnd(ba(), bb_()))); return;
        case Op::Crnor:  crbits(b.CreateNot(b.CreateOr(ba(), bb_()))); return;
        case Op::Cror:   crbits(b.CreateOr(ba(), bb_())); return;
        case Op::Crorc:  crbits(b.CreateOr(ba(), b.CreateNot(bb_()))); return;
        case Op::Crxor:  crbits(b.CreateICmpNE(ba(), bb_())); return;
        case Op::Mcrf:   set_crf(crfd, crf(in.crfs())); return;
        case Op::Mcrxr:
            set_crf(crfd, b.CreateLShr(xer(), 28));
            set_xer(b.CreateAnd(xer(), 0x0FFFFFFFu));
            return;
        case Op::Mfcr: SG(d, cr()); return;
        case Op::Mtcrf: {
            uint32_t mask = 0;
            for (uint32_t f = 0; f < 8; ++f)
                if (in.crm() & (0x80u >> f)) mask |= 0xF0000000u >> (4 * f);
            set_cr(b.CreateOr(b.CreateAnd(cr(), ~mask), b.CreateAnd(G(s), mask)));
            return;
        }

        // Processor control
        case Op::Mfmsr:  SG(d, field(offsetof(CpuState, msr))); return;
        case Op::Mtmsr:  set_field(offsetof(CpuState, msr), G(s)); recheck(); return;
        case Op::Mfsr:   SG(d, field(offsetof(CpuState, sr) + 4 * in.sr())); return;
        case Op::Mtsr:   set_field(offsetof(CpuState, sr) + 4 * in.sr(), G(s)); return;
        case Op::Mfsrin:
        case Op::Mtsrin: {
            Value* idx = b.CreateZExt(b.CreateLShr(G(bb), 28), i64);
            Value* p = b.CreateInBoundsGEP(i32, at(cpu(offsetof(CpuState, sr))), idx);
            if (in.op == Op::Mfsrin) SG(d, b.CreateLoad(i32, p));
            else b.CreateStore(G(s), p);
            return;
        }
        case Op::Mftb:
        case Op::Mfspr: {
            const uint32_t spr = in.spr();
            if (spr == 268) { SG(d, call("spk_read_tbl", i32, {})); return; }
            if (spr == 22 && in.op == Op::Mfspr) { SG(d, call("spk_read_dec", i32, {})); return; }
            if (spr == 269) { SG(d, call("spk_read_tbu", i32, {})); return; }
            const int off = in.op == Op::Mfspr ? spr_offset(spr) : -1;
            if (off < 0) { call_noreturn("spk_unknown_spr", {k(pc), k(spr)}); return; }
            SG(d, field(static_cast<size_t>(off)));
            return;
        }
        case Op::Mtspr: {
            const uint32_t spr = in.spr();
            if (spr == 268 || spr == 284) { call_void("spk_write_tbl", {G(s)}); return; }
            if (spr == 22) { call_void("spk_write_dec", {G(s)}); return; }
            if (spr == 269 || spr == 285) { call_void("spk_write_tbu", {G(s)}); return; }
            if (spr == 287) return;
            const int off = spr_offset(spr);
            if (off < 0) { call_noreturn("spk_unknown_spr", {k(pc), k(spr)}); return; }
            set_field(static_cast<size_t>(off), G(s));
            return;
        }

        // Traps and system calls
        case Op::Tw:
        case Op::Twi: {
            if (in.to() == 31) { call_void("spk_trap", {k(pc)}); return; }
            Value* x = G(a);
            Value* y = in.op == Op::Tw ? G(bb) : simm;
            const uint32_t to = in.to();
            Value* t = b.getFalse();
            if (to & 16) t = b.CreateOr(t, b.CreateICmpSLT(x, y));
            if (to & 8) t = b.CreateOr(t, b.CreateICmpSGT(x, y));
            if (to & 4) t = b.CreateOr(t, b.CreateICmpEQ(x, y));
            if (to & 2) t = b.CreateOr(t, b.CreateICmpULT(x, y));
            if (to & 1) t = b.CreateOr(t, b.CreateICmpUGT(x, y));
            guarded(t, [&] { call_void("spk_trap", {k(pc)}); });
            return;
        }
        case Op::Sc: call_void("spk_system_call", {k(pc)}); return;

        case Op::Invalid: call_noreturn("spk_invalid", {k(pc), k(in.raw)}); return;

        default:
            floating_point(in, pc);
            return;
        }
    }

    // ---- Floating point (recomp/guest.hpp) ----
    Value* f64of(Value* bits) { return b.CreateBitCast(bits, f64); }
    Value* bitsof(Value* d) { return b.CreateBitCast(d, i64); }
    Value* fintr(llvm::Intrinsic::ID id, std::initializer_list<Value*> args) {
        return b.CreateCall(llvm::Intrinsic::getDeclaration(&M, id, {f64}), std::vector<Value*>(args));
    }
    Value* round_single(Value* d) { return bitsof(b.CreateFPExt(b.CreateFPTrunc(d, f32), f64)); }
    void set_fprf(Value* result, bool single) {
        Value* d = f64of(result);
        Value* neg = b.CreateICmpNE(b.CreateLShr(result, 63), k64(0));
        Value* fabs = fintr(llvm::Intrinsic::fabs, {d});
        auto pick = [&](uint32_t if_neg, uint32_t if_pos) { return b.CreateSelect(neg, k(if_neg), k(if_pos)); };
        Value* c = pick(0x08, 0x04);
        const double tiny = single ? 1.17549435e-38 : 2.2250738585072014e-308;
        c = b.CreateSelect(b.CreateFCmpOLT(fabs, llvm::ConstantFP::get(f64, tiny)), pick(0x18, 0x14), c);
        c = b.CreateSelect(b.CreateFCmpOEQ(fabs, llvm::ConstantFP::getInfinity(f64)), pick(0x09, 0x05), c);
        c = b.CreateSelect(b.CreateFCmpOEQ(d, llvm::ConstantFP::get(f64, 0.0)), pick(0x12, 0x02), c);
        c = b.CreateSelect(b.CreateFCmpUNO(d, d), k(0x11), c);
        set_fpscr(b.CreateOr(b.CreateAnd(fpscr(), ~0x1F000u), b.CreateShl(c, 12)));
    }
    Value* single_to_double(Value* w) {
        Value* sign = b.CreateShl(b.CreateZExt(b.CreateLShr(w, 31), i64), 63);
        Value* exp = b.CreateAnd(b.CreateLShr(w, 23), 0xFF);
        Value* frac = b.CreateAnd(w, 0x7FFFFF);
        Value* special = b.CreateOr(b.CreateOr(sign, k64(0x7FF0000000000000u)), b.CreateShl(b.CreateZExt(frac, i64), 29));
        Value* normal = bitsof(b.CreateFPExt(b.CreateBitCast(w, f32), f64));
        return b.CreateSelect(b.CreateICmpEQ(exp, k(0xFF)), special, normal);
    }
    Value* double_to_single(Value* dd) {
        Value* exp = b.CreateAnd(b.CreateTrunc(b.CreateLShr(dd, 52), i32), 0x7FF);
        Value* hi = b.CreateTrunc(b.CreateLShr(dd, 32), i32);
        Value* sign = b.CreateAnd(hi, 0x80000000u);
        Value* plain = b.CreateOr(b.CreateAnd(hi, 0xC0000000u), b.CreateAnd(b.CreateTrunc(b.CreateLShr(dd, 29), i32), 0x3FFFFFFFu));
        Value* is_plain = b.CreateOr(b.CreateICmpUGT(exp, k(896)), b.CreateICmpEQ(b.CreateAnd(dd, k64(0x7FFFFFFFFFFFFFFFu)), k64(0)));
        Value* mantissa = b.CreateOr(k64(0x0010000000000000u), b.CreateAnd(dd, k64(0x000FFFFFFFFFFFFFu)));
        Value* shift = b.CreateAnd(b.CreateZExt(b.CreateSub(k(29 + 897), exp), i64), k64(63));
        Value* denormal = b.CreateOr(sign, b.CreateTrunc(b.CreateLShr(mantissa, shift), i32));
        Value* small = b.CreateSelect(b.CreateICmpUGE(exp, k(874)), denormal, sign);
        return b.CreateSelect(is_plain, plain, small);
    }
    void fcmp(uint32_t field_n, Value* abits, Value* bbits) {
        Value *x = f64of(abits), *y = f64of(bbits);
        Value* c = b.CreateSelect(b.CreateFCmpUNO(x, y), k(1),
                                  b.CreateSelect(b.CreateFCmpOLT(x, y), k(8), b.CreateSelect(b.CreateFCmpOGT(x, y), k(4), k(2))));
        set_crf(field_n, c);
        set_fpscr(b.CreateOr(b.CreateAnd(fpscr(), ~0xF000u), b.CreateShl(c, 12)));
    }

    void floating_point(const Instruction& in, uint32_t pc) {
        const uint32_t fd = in.rd(), fa = in.ra(), fb = in.rb(), fc = in.rc();
        const bool rc = in.record();
        auto cr1 = [&] {
            if (rc) set_crf(1, b.CreateLShr(fpscr(), 28));
        };
        auto ea_d = [&] { return b.CreateAdd(A0(in), k(static_cast<uint32_t>(in.simm()))); };
        auto ea_x = [&] { return b.CreateAdd(A0(in), G(in.rb())); };
        auto mem = [&](bool indexed, auto action, bool update) {
            Value* ea = indexed ? ea_x() : ea_d();
            action(ea);
            if (update) SG(in.ra(), ea);
        };
        auto A = [&] { return f64of(FR(fa)); };
        auto B = [&] { return f64of(FR(fb)); };
        auto Cc = [&] { return f64of(FR(fc)); };
        auto dbl = [&](Value* expr) {
            SFR(fd, bitsof(expr));
            set_fprf(FR(fd), false);
            cr1();
        };
        auto sgl = [&](Value* expr) {
            SFR(fd, round_single(expr));
            set_fprf(FR(fd), true);
            cr1();
        };
        auto raw = [&](Value* v) {
            SFR(fd, v);
            cr1();
        };
        auto lfs = [&](Value* ea) { SFR(fd, single_to_double(read32(ea))); };
        auto lfd = [&](Value* ea) { SFR(fd, read64(ea)); };
        auto stfs = [&](Value* ea) { write32(ea, double_to_single(FR(fd))); };
        auto stfd = [&](Value* ea) { write64(ea, FR(fd)); };
        auto fma = [&](Value* x, Value* y, Value* z) { return fintr(llvm::Intrinsic::fma, {x, y, z}); };
        auto neg = [&](Value* v) { return b.CreateFNeg(v); };
        Value* one = llvm::ConstantFP::get(f64, 1.0);

        switch (in.op) {
        case Op::Lfs:    mem(false, lfs, false); return;
        case Op::Lfsu:   mem(false, lfs, true); return;
        case Op::Lfsx:   mem(true, lfs, false); return;
        case Op::Lfsux:  mem(true, lfs, true); return;
        case Op::Lfd:    mem(false, lfd, false); return;
        case Op::Lfdu:   mem(false, lfd, true); return;
        case Op::Lfdx:   mem(true, lfd, false); return;
        case Op::Lfdux:  mem(true, lfd, true); return;
        case Op::Stfs:   mem(false, stfs, false); return;
        case Op::Stfsu:  mem(false, stfs, true); return;
        case Op::Stfsx:  mem(true, stfs, false); return;
        case Op::Stfsux: mem(true, stfs, true); return;
        case Op::Stfd:   mem(false, stfd, false); return;
        case Op::Stfdu:  mem(false, stfd, true); return;
        case Op::Stfdx:  mem(true, stfd, false); return;
        case Op::Stfdux: mem(true, stfd, true); return;
        case Op::Stfiwx: mem(true, [&](Value* ea) { write32(ea, b.CreateTrunc(FR(fd), i32)); }, false); return;

        case Op::Fadd:    dbl(b.CreateFAdd(A(), B())); return;
        case Op::Fadds:   sgl(b.CreateFAdd(A(), B())); return;
        case Op::Fsub:    dbl(b.CreateFSub(A(), B())); return;
        case Op::Fsubs:   sgl(b.CreateFSub(A(), B())); return;
        case Op::Fmul:    dbl(b.CreateFMul(A(), Cc())); return;
        case Op::Fmuls:   sgl(b.CreateFMul(A(), Cc())); return;
        case Op::Fdiv:    dbl(b.CreateFDiv(A(), B())); return;
        case Op::Fdivs:   sgl(b.CreateFDiv(A(), B())); return;
        case Op::Fmadd:   dbl(fma(A(), Cc(), B())); return;
        case Op::Fmadds:  sgl(fma(A(), Cc(), B())); return;
        case Op::Fmsub:   dbl(fma(A(), Cc(), neg(B()))); return;
        case Op::Fmsubs:  sgl(fma(A(), Cc(), neg(B()))); return;
        case Op::Fnmadd:  dbl(neg(fma(A(), Cc(), B()))); return;
        case Op::Fnmadds: sgl(neg(fma(A(), Cc(), B()))); return;
        case Op::Fnmsub:  dbl(neg(fma(A(), Cc(), neg(B())))); return;
        case Op::Fnmsubs: sgl(neg(fma(A(), Cc(), neg(B())))); return;
        case Op::Fsqrt:   dbl(fintr(llvm::Intrinsic::sqrt, {B()})); return;
        case Op::Fsqrts:  sgl(fintr(llvm::Intrinsic::sqrt, {B()})); return;
        case Op::Fres:    sgl(b.CreateFDiv(one, B())); return;
        case Op::Frsqrte: dbl(b.CreateFDiv(one, fintr(llvm::Intrinsic::sqrt, {B()}))); return;
        case Op::Fsel: {
            Value* ge = b.CreateFCmpOGE(A(), llvm::ConstantFP::get(f64, 0.0));
            raw(b.CreateSelect(ge, FR(fc), FR(fb)));
            return;
        }

        case Op::Fmr:   raw(FR(fb)); return;
        case Op::Fneg:  raw(b.CreateXor(FR(fb), k64(0x8000000000000000u))); return;
        case Op::Fabs:  raw(b.CreateAnd(FR(fb), k64(0x7FFFFFFFFFFFFFFFu))); return;
        case Op::Fnabs: raw(b.CreateOr(FR(fb), k64(0x8000000000000000u))); return;

        case Op::Frsp:   sgl(B()); return;
        case Op::Fctiw:  raw(call("spk_fctiw", i64, {FR(fb), k(0)})); return;
        case Op::Fctiwz: raw(call("spk_fctiw", i64, {FR(fb), k(1)})); return;
        case Op::Fcmpu:
        case Op::Fcmpo:  fcmp(in.crfd(), FR(fa), FR(fb)); return;

        case Op::Mffs: raw(b.CreateOr(k64(0xFFF8000000000000u), b.CreateZExt(fpscr(), i64))); return;
        case Op::Mcrfs: {
            const uint32_t shift = 28 - 4 * in.crfs();
            set_crf(in.crfd(), b.CreateAnd(b.CreateLShr(fpscr(), shift), 0xF));
            set_fpscr(b.CreateAnd(fpscr(), ~(0x9FF80700u & (0xFu << shift))));
            return;
        }
        case Op::Mtfsf: {
            uint32_t mask = 0;
            for (uint32_t f = 0; f < 8; ++f)
                if (in.fm() & (0x80u >> f)) mask |= 0xF0000000u >> (4 * f);
            set_fpscr(b.CreateOr(b.CreateAnd(fpscr(), ~mask), b.CreateAnd(b.CreateTrunc(FR(fb), i32), mask)));
            call_void("spk_fpscr_written", {});
            cr1();
            return;
        }
        case Op::Mtfsfi: {
            const uint32_t shift = 28 - 4 * in.crfd();
            set_fpscr(b.CreateOr(b.CreateAnd(fpscr(), ~(0xFu << shift)), k(in.fpimm() << shift)));
            call_void("spk_fpscr_written", {});
            cr1();
            return;
        }
        case Op::Mtfsb0:
            set_fpscr(b.CreateAnd(fpscr(), ~(0x80000000u >> in.rd())));
            call_void("spk_fpscr_written", {});
            cr1();
            return;
        case Op::Mtfsb1:
            set_fpscr(b.CreateOr(fpscr(), 0x80000000u >> in.rd()));
            call_void("spk_fpscr_written", {});
            cr1();
            return;

        default:
            unimplemented(in, pc);
            return;
        }
    }

    llvm::LLVMContext& C;
    llvm::Module& M;
    llvm::IRBuilder<> b;
    const Layout& L;
    recomp::LiftStats& stats_;
    llvm::Type *i1, *i8, *i16, *i32, *i64, *f32, *f64;
    llvm::PointerType* ptr;
    llvm::FunctionType* void_fn;
    llvm::MDNode* unlikely_;

    LiftState* ls_ = nullptr;
    llvm::Function* F = nullptr;
    Value* ctx = nullptr;
    std::set<uint32_t> blocks_;
    std::map<uint32_t, BasicBlock*> labels_;
    std::map<uint32_t, BasicBlock*> sites_;
};

} // namespace

std::vector<IrPart> build_ir(const std::vector<LiftInput>& lifts, const Layout& layout, uint32_t parts,
                             const std::string& data_layout, const std::string& triple, recomp::LiftStats* stats_out) {
    parts = std::max(1u, parts);
    auto new_part = [&](const char* name) {
        IrPart p;
        p.context = std::make_unique<llvm::LLVMContext>();
        p.module = std::make_unique<llvm::Module>(name, *p.context);
        p.module->setDataLayout(data_layout);
        p.module->setTargetTriple(triple);
        return p;
    };

    // Every function of every lift, in order, spread over the parts by instruction count.
    uint64_t total = 0;
    for (const LiftInput& l : lifts)
        for (uint32_t entry : l.functions) total += l.analysis->functions.at(entry).instructions;

    std::vector<IrPart> out;
    for (uint32_t i = 0; i < parts; ++i) out.push_back(new_part(("spikeout_part" + std::to_string(i)).c_str()));
    std::vector<std::unique_ptr<Emitter>> emitters;
    recomp::LiftStats stats;
    for (IrPart& p : out) emitters.push_back(std::make_unique<Emitter>(*p.module, layout, stats));

    std::vector<LiftState> states;
    for (const LiftInput& l : lifts) states.push_back({&l, {}, {}});
    uint64_t done = 0;
    for (LiftState& ls : states) {
        for (uint32_t entry : ls.in->functions) {
            const Function& fn = ls.in->analysis->functions.at(entry);
            if (fn.blocks.empty()) continue;
            const size_t part = total ? std::min<size_t>(parts - 1, static_cast<size_t>(done * parts / total)) : 0;
            emitters[part]->function(ls, fn);
            out[part].instructions += fn.instructions;
            done += fn.instructions;
        }
    }
    emitters.clear();

    // The tables (as the C++ lifter's kFunctions and kBlocks, every lift's, sorted by address).
    IrPart table = new_part("spikeout_tables");
    llvm::LLVMContext& C = *table.context;
    llvm::Module& M = *table.module;
    llvm::Type* i32 = llvm::Type::getInt32Ty(C);
    llvm::PointerType* ptr = llvm::PointerType::get(C, 0);
    llvm::StructType* entry_t = llvm::StructType::get(C, {i32, ptr, i32});
    llvm::FunctionType* void_fn = llvm::FunctionType::get(llvm::Type::getVoidTy(C), {ptr}, false);
    auto decl = [&](const std::string& prefix, uint32_t entry) {
        const std::string name = fn_name(prefix, entry);
        llvm::Function* f = M.getFunction(name);
        if (!f) {
            f = llvm::Function::Create(void_fn, llvm::GlobalValue::ExternalLinkage, name, M);
            f->setDSOLocal(true);
        }
        return f;
    };
    std::map<uint32_t, llvm::Constant*> rows, block_rows;
    auto row = [&](uint32_t addr, llvm::Function* f, uint32_t resume) {
        return llvm::ConstantStruct::get(entry_t, {llvm::ConstantInt::get(i32, addr), f, llvm::ConstantInt::get(i32, resume)});
    };
    for (const LiftState& ls : states) {
        std::map<uint32_t, llvm::Constant*> lift_rows;
        for (uint32_t entry : ls.in->functions)
            if (!ls.in->analysis->functions.at(entry).blocks.empty()) lift_rows.emplace(entry, row(entry, decl(ls.in->prefix, entry), 0));
        for (const auto& [site, fn] : ls.resume_sites) lift_rows.emplace(site, row(site, decl(ls.in->prefix, fn), site));
        stats.return_sites += static_cast<uint32_t>(ls.resume_sites.size());
        for (const auto& [start, fn] : ls.block_sites)
            if (!lift_rows.contains(start)) block_rows.emplace(start, row(start, decl(ls.in->prefix, fn), start));
        rows.insert(lift_rows.begin(), lift_rows.end());
    }
    auto array = [&](const char* name, const std::map<uint32_t, llvm::Constant*>& r) {
        std::vector<llvm::Constant*> v;
        for (const auto& [addr, c] : r) v.push_back(c);
        llvm::ArrayType* t = llvm::ArrayType::get(entry_t, v.size());
        auto* g = new llvm::GlobalVariable(M, t, true, llvm::GlobalValue::ExternalLinkage, llvm::ConstantArray::get(t, v), name);
        g->setDSOLocal(true);
        auto* n = new llvm::GlobalVariable(M, llvm::Type::getInt64Ty(C), true, llvm::GlobalValue::ExternalLinkage,
                                           llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), v.size()),
                                           std::string(name) + "_n");
        n->setDSOLocal(true);
    };
    array("spk_functions", rows);
    array("spk_blocks", block_rows);
    out.push_back(std::move(table));
    if (stats_out) *stats_out = std::move(stats);
    return out;
}

} // namespace jit
