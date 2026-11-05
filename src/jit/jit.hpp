// The LLVM JIT (prototype): the game's PowerPC code translated from the
// player's own ROM at start-up, instead of the recompiled C++ built into the
// executable. The same analysis and the same lifter semantics as ppcrecomp
// (src/jit/ppc_ir.cpp is the lifter's IR back end); the result is cached on
// disk, keyed by the program ROM, this translator's version, LLVM's and the
// host CPU's, so later starts only load it.
//
// Linking this library gives generated::game_ram_functions() and the other
// table functions game_setup.hpp declares (the boot ROM's are in the same
// tables), valid after prepare().
#pragma once

#include "recomp/context.hpp"
#include "runtime/rom.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>

namespace jit {

// How far a translation is (for a "preparing the game" screen): the step, and parts done of all of them while
// compiling (most of the time).
enum class Step { Cache, Analysis, Ir, Compile, Load };
struct Progress {
    Step step = Step::Cache;
    unsigned done = 0, total = 0; // (parts compiled: Step::Compile only)
};

struct Options {
    std::function<void(const Progress&)> progress; // called on any thread, often: keep it quick
    const std::atomic<bool>* cancel = nullptr;      // set: prepare() stops soon and throws Cancelled
    std::filesystem::path cache_dir; // empty: $SPIKEOUT_JIT_CACHE, else ./jit-cache
    unsigned threads = 0;  // 0: $SPIKEOUT_JIT_THREADS, else every core
    int opt_level = -1;    // 0-3; -1: $SPIKEOUT_JIT_OPT, else 1 (the code as fast as -O2's, made ~15% sooner)
    bool use_cache = true; // false (or SPIKEOUT_JIT_NO_CACHE=1): translate, don't read or write the cache
    bool verbose = true;   // what was done and its times, on stdout
    bool prune = false;    // other translations in cache_dir (older ROMs or versions) deleted once this one is kept
};

struct Report {
    bool from_cache = false;
    std::filesystem::path cache_path; // this translation's folder (empty: not kept)
    double analysis_s = 0, ir_s = 0, compile_s = 0, load_s = 0, total_s = 0;
    unsigned threads = 0;
    size_t functions = 0, blocks = 0, object_bytes = 0;
};

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("the translation was stopped") {}
};

// Throws std::runtime_error when it can't (Cancelled when Options::cancel was set).
Report prepare(const m3::GameSpec& game, const m3::RomSet& roms, Options options = {});

std::span<const recomp::FunctionEntry> functions(); // sorted
std::span<const recomp::FunctionEntry> blocks();    // sorted

} // namespace jit
