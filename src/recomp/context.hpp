// What recompiled code runs against: the CPU state, the bus, and a table of
// the recompiled functions for indirect calls and jumps.
//
// Recompiled functions are plain C++ functions taking the context. `bl` is a
// C++ call, `blr` a C++ return; anything the code can't follow statically
// (function pointers, computed jumps, rfi) goes through call()/jump(), which
// look the target up in the table. Reaching code that wasn't recompiled, an
// unimplemented instruction, or a return to the wrong place throws, so a run
// stops at the first thing the recompiler got wrong instead of drifting.
#pragma once

#include "ppc/cpu_state.hpp"
#include "runtime/bus.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace recomp {

class Context;
class Scheduler;
using GuestFunction = void (*)(Context&);

// A recompiled function, or a place inside one to resume at: `resume` is an
// instruction right after a call in `fn` (a return site), which code can
// return to without a C++ frame waiting there (a task resumed on its own
// stack returns into the calls it made before it yielded).
struct FunctionEntry {
    uint32_t addr;
    GuestFunction fn;
    uint32_t resume = 0; // == addr for a return site; 0 for a function entry
};

// Execution reached an address with no recompiled function.
struct MissingCode : std::runtime_error {
    explicit MissingCode(uint32_t a);
    uint32_t addr;
};

// Something the runtime can't handle (yet): stops the run.
struct GuestError : std::runtime_error {
    GuestError(uint32_t at, const std::string& what);
    uint32_t pc;
};

// The block limit was reached (an endless loop, or a run's limit).
struct OutOfBudget : GuestError {
    explicit OutOfBudget(uint32_t at);
};

class Context {
public:
    // `functions` must be sorted by address.
    Context(m3::Bus& bus, std::span<const FunctionEntry> functions);

    ppc::CpuState cpu;
    m3::Bus& bus;

    // Where the last `blr` returned to; checked after every call.
    uint32_t returned_to = 0;

    // Built with SPIKEOUT_FP_TRAP, the recompiled code reports every FP
    // result here: the first NaN found (address and bits) is kept, to find
    // where one starts.
    void fp_check(uint32_t pc, uint64_t bits) {
        if (first_nan_pc == 0 && (bits & 0x7FF0000000000000u) == 0x7FF0000000000000u && (bits & 0x000FFFFFFFFFFFFFu)) {
            first_nan_pc = pc;
            first_nan_bits = bits;
        }
    }
    uint32_t first_nan_pc = 0;
    uint64_t first_nan_bits = 0;

    // ---- Clock ----
    // Each basic block adds the 603e cycles the lifter worked out for it
    // (ppc::block_cycles: dispatch, units and register dependencies, every
    // access a cache hit). Counted, not measured, so every run with the same
    // inputs is exactly the same (save states, rollback netplay).
    uint64_t blocks = 0;
    uint64_t cycles() const { return cycles_; }
    // Stop with OutOfBudget after `n` more blocks, or `n` more cycles.
    void limit_blocks(uint64_t n) {
        block_limit_ = blocks + n;
        recheck();
    }
    void limit_cycles(uint64_t n) {
        cycle_limit_ = cycles_ + n;
        recheck();
    }
    // Board timing (frames, interrupts): called whenever it's due.
    void set_scheduler(Scheduler* s) {
        scheduler_ = s;
        scheduler_due_ = 0;
        recheck();
    }
    // A device's next event may have moved earlier: ask the scheduler again.
    void reschedule() {
        scheduler_due_ = 0;
        recheck();
    }

    // Time base and decrementer, at the bus clock / 4 (16.5 MHz with a
    // 166 MHz core). Each TBL read also moves the time base on by `tb_step`,
    // so back-to-back reads differ.
    uint32_t tb_step = 1;
    uint64_t time_base();
    uint32_t read_tbl();
    uint32_t read_tbu() { return static_cast<uint32_t>(time_base() >> 32); }
    void write_tbl(uint32_t v);
    void write_tbu(uint32_t v);
    uint32_t read_dec();
    void write_dec(uint32_t v);

    // Something changed that may let an interrupt in (MSR, the IRQ line):
    // look again at the start of the next block.
    void recheck() { next_check_ = cycles_; }
    void msr_changed() { recheck(); }

    uint64_t external_interrupts = 0;    // taken
    uint64_t decrementer_interrupts = 0; // taken

    uint8_t read8(uint32_t addr) { return bus.read8(addr); }
    uint16_t read16(uint32_t addr) { return bus.read16(addr); }
    uint32_t read32(uint32_t addr) { return bus.read32(addr); }
    uint64_t read64(uint32_t addr) { return bus.read64(addr); }
    void write8(uint32_t addr, uint8_t v) { bus.write8(addr, v); }
    void write16(uint32_t addr, uint16_t v) { bus.write16(addr, v); }
    void write32(uint32_t addr, uint32_t v) { bus.write32(addr, v); }
    void write64(uint32_t addr, uint64_t v) { bus.write64(addr, v); }

    // Start of every basic block, with the block's cost: one compare unless
    // something is due (an event, an interrupt to deliver, a limit). The
    // block's cycles count once it runs, after any interrupt taken first.
    void tick(uint32_t pc, uint32_t block_cycles) {
        cpu.pc = pc;
        recent[recent_pos++ & (recent.size() - 1)] = pc;
        ++blocks;
        if (cycles_ >= next_check_) slow_tick(pc);
        cycles_ += block_cycles;
    }

    // The last blocks run, oldest first: where a stopped run came from.
    std::array<uint32_t, 16> recent{};
    uint32_t recent_pos = 0;
    std::array<uint32_t, 16> recent_blocks() const {
        std::array<uint32_t, 16> out{};
        for (uint32_t i = 0; i < out.size(); ++i) out[i] = recent[(recent_pos + i) & (recent.size() - 1)];
        return out;
    }

    const FunctionEntry* find(uint32_t addr) const;
    // Run the function at `addr` (an indirect call; the caller sets LR), or
    // resume the function containing return site `addr`.
    void call(uint32_t addr);
    // Set by call() for a return site; the function's prologue takes it
    // (and clears it) and continues at that site.
    uint32_t resume_at = 0;
    // Continue at `addr` (an indirect jump or tail call). Same as call():
    // the C++ stack only unwinds when the guest code returns.
    void jump(uint32_t addr) { call(addr); }

    // Save states: every block start (the generated *_blocks() tables,
    // sorted), to resume at.
    void set_blocks(std::span<const FunctionEntry> table) { blocks_ = table; }
    // Run on from the block at `pc` (cpu as loaded, outside any exception),
    // with no C++ frames waiting above it: each guest return goes on into
    // the caller's return site, as the CPU would, for as long as the game
    // runs. Throws as call() does.
    [[noreturn]] void resume_block(uint32_t pc);
    // Game-specific observers (m3::GameSpec::hooks): the generated code calls
    // hook(pc) at the start of each such block, after its tick, before its
    // instructions. They read the guest's state and never change it, so the
    // game runs the same with or without them.
    using Hook = void (*)(Context& c, uint32_t pc, void* user);
    void set_hook(Hook fn, void* user) {
        hook_ = fn;
        hook_user_ = user;
    }
    void hook(uint32_t pc) {
        if (hook_) hook_(*this, pc, hook_user_);
    }

    // Debug aid (gamerun --camera-test): the CPU and the clock set aside, to
    // run guest code on the side (no scheduler, no interrupts) and then put
    // everything back as it was; the caller keeps and restores memory.
    struct Aside {
        ppc::CpuState cpu;
        uint64_t blocks, cycles, next_check, scheduler_due, tb_offset, dec_tb, dec_due_tb;
        uint32_t dec, returned_to, resume_at;
        bool dec_pending;
        size_t return_depth;
        Scheduler* scheduler;
        uint64_t block_limit, cycle_limit;
    };
    Aside set_aside() const {
        return {cpu, blocks, cycles_, next_check_, scheduler_due_, tb_offset_, dec_tb_, dec_due_tb_, dec_, returned_to,
                resume_at, dec_pending_, return_depth_, scheduler_, block_limit_, cycle_limit_};
    }
    void put_back(const Aside& a) {
        cpu = a.cpu, blocks = a.blocks, cycles_ = a.cycles, next_check_ = a.next_check, scheduler_due_ = a.scheduler_due;
        tb_offset_ = a.tb_offset, dec_tb_ = a.dec_tb, dec_due_tb_ = a.dec_due_tb, dec_ = a.dec, returned_to = a.returned_to;
        resume_at = a.resume_at, dec_pending_ = a.dec_pending, return_depth_ = a.return_depth, scheduler_ = a.scheduler;
        block_limit_ = a.block_limit, cycle_limit_ = a.cycle_limit;
        recheck();
    }
    // For code run aside (between set_aside and put_back): the run's own limits instead of its caller's.
    void limit_aside(uint64_t cycles) {
        block_limit_ = UINT64_MAX - 1;
        limit_cycles(cycles);
    }

    // Calls `fn` at the start of the next block run outside any exception
    // (where resume_block can start again), before anything else is done
    // there; it may save the state then. Several asked before then: each in
    // turn, the first asked first.
    void request_snapshot(std::function<void(uint32_t pc)> fn) {
        if (snapshot_)
            snapshot_ = [first = std::move(snapshot_), then = std::move(fn)](uint32_t pc) {
                first(pc);
                then(pc);
            };
        else
            snapshot_ = std::move(fn);
        recheck();
    }
    // The CPU, clock, time base and decrementer (the bus has the rest), as
    // at the start of the block a snapshot is taken at. Loaded, the run
    // goes on with resume_block(cpu.pc).
    void state(m3::State& s);

    // After a call, the callee's blr went somewhere other than `next`, the
    // instruction after the call. If an outer call is waiting for that
    // address, returns true: the caller returns at once, and so do the
    // frames above it, until the one that waits for it (a task that yields
    // by switching back to its scheduler's stack returns that way). Otherwise
    // the code went somewhere new on purpose (Spikeout's printf sets LR to
    // its own epilogue and branches to handlers that end in blr), so carry on
    // there, as the CPU would, until execution comes back to `next`.
    bool follow_return(uint32_t next);

    // The return addresses outer calls wait for (generated code pushes one
    // around each call), so follow_return can tell a return to an outer
    // caller from a jump.
    void push_return(uint32_t next) {
        if (return_depth_ < return_stack_.size()) return_stack_[return_depth_] = next;
        ++return_depth_;
    }
    void pop_return() { --return_depth_; }
    size_t return_depth() const { return return_depth_; }
    // follow_return() gave up: the code never came back.
    [[noreturn]] void bad_return(uint32_t expected);

    void dcbz(uint32_t ea);
    void load_string(uint32_t rd, uint32_t ea, uint32_t nb);  // lswi/lswx
    void store_string(uint32_t rs, uint32_t ea, uint32_t nb); // stswi/stswx

    // Exceptions. exception() enters a handler the way the 603e does (SRR0,
    // SRR1, MSR, vector at 0 or 0xFFF00000 by MSR[IP]) and runs it; it
    // returns when the handler's rfi goes back to `resume`, the address where
    // the recompiled code that raised it carries on. An rfi anywhere else is
    // a jump (the boot code enters the game that way).
    void exception(uint32_t offset, uint32_t srr0, uint32_t cause, uint32_t resume);
    void rfi();
    // Exceptions being handled, innermost last.
    size_t exception_depth() const { return resume_.size(); }

    void system_call(uint32_t pc) { exception(0xC00, pc + 4, 0, pc + 4); }
    // Program exception, trap type. SRR0 points at the trap; handlers step
    // past it, so the code resumes after it.
    void trap(uint32_t pc) { exception(0x700, pc, kSrr1Trap, pc + 4); }

    [[noreturn]] void unimplemented(uint32_t pc, uint32_t raw);
    [[noreturn]] void invalid(uint32_t pc, uint32_t raw);
    [[noreturn]] void missing(uint32_t addr) { throw MissingCode(addr); }
    [[noreturn]] void unknown_spr(uint32_t pc, uint32_t spr);

    static constexpr uint32_t kSrr1Trap = 1u << 17; // SRR1 bit 14
    static constexpr uint32_t kMsrEE = 1u << 15;

private:
    void slow_tick(uint32_t pc);
    uint64_t cycle_of_tb(uint64_t tb) const;

    std::span<const FunctionEntry> functions_;
    std::span<const FunctionEntry> blocks_;
    std::function<void(uint32_t)> snapshot_;
    Hook hook_ = nullptr;
    void* hook_user_ = nullptr;
    std::vector<uint32_t> resume_;
    std::array<uint32_t, 4096> return_stack_{};
    size_t return_depth_ = 0;

    uint64_t cycles_ = 0;
    uint64_t next_check_ = 0; // cycle at which tick() calls slow_tick()
    uint64_t block_limit_ = UINT64_MAX - 1;
    uint64_t cycle_limit_ = UINT64_MAX;
    Scheduler* scheduler_ = nullptr;
    uint64_t scheduler_due_ = UINT64_MAX; // cycle

    uint64_t tb_offset_ = 0; // time base = tb_offset_ + cycles * 33 / 332
    uint32_t dec_ = 0;       // DEC as written ...
    uint64_t dec_tb_ = 0;    // ... at this time base
    uint64_t dec_due_tb_ = 1; // when it next goes from 0 to -1 (it is 0 at reset)
    bool dec_pending_ = false;
};

// Board timing: raises interrupts and ends frames at the right times.
class Scheduler {
public:
    virtual ~Scheduler() = default;
    // Called once the clock reaches the cycle it last returned (0 at first):
    // does what's due and returns the cycle of its next event.
    virtual uint64_t update(Context& c) = 0;
};

} // namespace recomp
