#include "jit/jit.hpp"

#include "analysis/boot_image.hpp"
#include "analysis/control_flow.hpp"
#include "jit/ppc_ir.hpp"
#include "recomp/guest.hpp"
#include "runtime/bus.hpp"
#include "runtime/memory_map.hpp"

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// Bump when the IR back end or the helpers change what code they make: old caches are then not used.
static constexpr const char* kTranslatorVersion = "spikeout-jit 1";
static constexpr uint32_t kParts = 48; // modules, compiled side by side (independent of the thread count: same objects)

namespace recomp {

// What the generated IR reads and writes in Context and Bus, measured here.
struct JitAccess {
    static jit::Layout layout() {
        m3::RomSet roms;
        auto bus = std::make_unique<m3::Bus>(roms);
        auto c = std::make_unique<Context>(*bus, std::span<const FunctionEntry>{});
        const char* cb = reinterpret_cast<const char*>(c.get());
        const char* bb = reinterpret_cast<const char*>(bus.get());
        auto in_ctx = [&](const void* p) { return static_cast<uint32_t>(static_cast<const char*>(p) - cb); };
        auto in_bus = [&](const void* p) { return static_cast<uint32_t>(static_cast<const char*>(p) - bb); };
        jit::Layout l{};
        l.ctx_cpu = in_ctx(&c->cpu);
        // The bus is a reference member: found by its address.
        l.ctx_bus = UINT32_MAX;
        for (uint32_t off = 0; off + sizeof(void*) <= sizeof(Context); off += alignof(void*)) {
            const void* p = nullptr;
            std::memcpy(&p, cb + off, sizeof p);
            if (p == bus.get()) {
                l.ctx_bus = off;
                break;
            }
        }
        l.ctx_returned_to = in_ctx(&c->returned_to);
        l.ctx_resume_at = in_ctx(&c->resume_at);
        l.ctx_blocks = in_ctx(&c->blocks);
        l.ctx_cycles = in_ctx(&c->cycles_);
        l.ctx_next_check = in_ctx(&c->next_check_);
        l.ctx_recent = in_ctx(c->recent.data());
        l.ctx_recent_pos = in_ctx(&c->recent_pos);
        l.ctx_return_stack = in_ctx(c->return_stack_.data());
        l.ctx_return_depth = in_ctx(&c->return_depth_);
        l.return_stack_size = static_cast<uint32_t>(c->return_stack_.size());
        // Work RAM's data pointer: the vector's first word (libstdc++, libc++ and MSVC alike), checked.
        l.bus_ram_data = in_bus(&bus->ram_);
        const uint8_t* data = nullptr;
        std::memcpy(&data, bb + l.bus_ram_data, sizeof data);
        l.bus_fast_limit = in_bus(bus->fast_limit_.data());
        l.bus_marks = in_bus(&bus->marks_);
        l.mark_shift = m3::Bus::kMarkShift;
        static_assert(sizeof(c->recent) == 16 * sizeof(uint32_t));
        static_assert(sizeof(c->return_depth_) == 8 && sizeof(c->cycles_) == 8 && sizeof(c->next_check_) == 8);
        if (l.ctx_bus == UINT32_MAX || data != bus->ram_.data())
            throw std::runtime_error("jit: can't find the context's bus or work RAM in this build");
        return l;
    }
    static void slow_tick(Context& c, uint32_t pc) { c.slow_tick(pc); }
};

} // namespace recomp

namespace {

using recomp::Context;
using recomp::FunctionEntry;

// ---- The helpers the generated code calls (the runtime's own code) ----
uint32_t spk_read8(Context* c, uint32_t a) { return c->read8(a); }
uint32_t spk_read16(Context* c, uint32_t a) { return c->read16(a); }
uint32_t spk_read32(Context* c, uint32_t a) { return c->read32(a); }
uint64_t spk_read64(Context* c, uint32_t a) { return c->read64(a); }
void spk_write8(Context* c, uint32_t a, uint32_t v) { c->write8(a, static_cast<uint8_t>(v)); }
void spk_write16(Context* c, uint32_t a, uint32_t v) { c->write16(a, static_cast<uint16_t>(v)); }
void spk_write32(Context* c, uint32_t a, uint32_t v) { c->write32(a, v); }
void spk_write64(Context* c, uint32_t a, uint64_t v) { c->write64(a, v); }
void spk_slow_tick(Context* c, uint32_t pc) { recomp::JitAccess::slow_tick(*c, pc); }
void spk_hook(Context* c, uint32_t pc) { c->hook(pc); }
void spk_call(Context* c, uint32_t a) { c->call(a); }
void spk_jump(Context* c, uint32_t a) { c->jump(a); }
uint32_t spk_follow_return(Context* c, uint32_t next) { return c->follow_return(next) ? 1u : 0u; }
void spk_rfi(Context* c) { c->rfi(); }
[[noreturn]] void spk_missing(Context* c, uint32_t a) { c->missing(a); }
[[noreturn]] void spk_unimplemented(Context* c, uint32_t pc, uint32_t raw) { c->unimplemented(pc, raw); }
[[noreturn]] void spk_invalid(Context* c, uint32_t pc, uint32_t raw) { c->invalid(pc, raw); }
[[noreturn]] void spk_unknown_spr(Context* c, uint32_t pc, uint32_t spr) { c->unknown_spr(pc, spr); }
uint32_t spk_read_tbl(Context* c) { return c->read_tbl(); }
uint32_t spk_read_tbu(Context* c) { return c->read_tbu(); }
uint32_t spk_read_dec(Context* c) { return c->read_dec(); }
void spk_write_tbl(Context* c, uint32_t v) { c->write_tbl(v); }
void spk_write_tbu(Context* c, uint32_t v) { c->write_tbu(v); }
void spk_write_dec(Context* c, uint32_t v) { c->write_dec(v); }
void spk_dcbz(Context* c, uint32_t ea) { c->dcbz(ea); }
void spk_load_string(Context* c, uint32_t rd, uint32_t ea, uint32_t nb) { c->load_string(rd, ea, nb); }
void spk_store_string(Context* c, uint32_t rs, uint32_t ea, uint32_t nb) { c->store_string(rs, ea, nb); }
void spk_trap(Context* c, uint32_t pc) { c->trap(pc); }
void spk_system_call(Context* c, uint32_t pc) { c->system_call(pc); }
uint64_t spk_fctiw(Context* c, uint64_t b, uint32_t toward_zero) { return recomp::ops::fctiw(c->cpu, b, toward_zero != 0); }
void spk_fpscr_written(Context* c) { recomp::ops::fpscr_written(c->cpu); }

struct Helper {
    const char* name;
    void* address;
};
template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }
const Helper kHelpers[] = {
    {"spk_read8", addr(spk_read8)},       {"spk_read16", addr(spk_read16)},
    {"spk_read32", addr(spk_read32)},     {"spk_read64", addr(spk_read64)},
    {"spk_write8", addr(spk_write8)},     {"spk_write16", addr(spk_write16)},
    {"spk_write32", addr(spk_write32)},   {"spk_write64", addr(spk_write64)},
    {"spk_slow_tick", addr(spk_slow_tick)}, {"spk_hook", addr(spk_hook)},
    {"spk_call", addr(spk_call)},         {"spk_jump", addr(spk_jump)},
    {"spk_follow_return", addr(spk_follow_return)}, {"spk_rfi", addr(spk_rfi)},
    {"spk_missing", addr(spk_missing)},   {"spk_unimplemented", addr(spk_unimplemented)},
    {"spk_invalid", addr(spk_invalid)},   {"spk_unknown_spr", addr(spk_unknown_spr)},
    {"spk_read_tbl", addr(spk_read_tbl)}, {"spk_read_tbu", addr(spk_read_tbu)},
    {"spk_read_dec", addr(spk_read_dec)}, {"spk_write_tbl", addr(spk_write_tbl)},
    {"spk_write_tbu", addr(spk_write_tbu)}, {"spk_write_dec", addr(spk_write_dec)},
    {"spk_dcbz", addr(spk_dcbz)},         {"spk_load_string", addr(spk_load_string)},
    {"spk_store_string", addr(spk_store_string)}, {"spk_trap", addr(spk_trap)},
    {"spk_system_call", addr(spk_system_call)}, {"spk_fctiw", addr(spk_fctiw)},
    {"spk_fpscr_written", addr(spk_fpscr_written)},
};

#ifdef _WIN32
// Windows: C++ exceptions (rfi's return, MissingCode, OutOfBudget...) unwind through the generated code with the
// system's unwinder, which only knows code whose unwind table (.pdata) is registered. LLVM 18 loads COFF objects with
// RuntimeDyld, which hands each object's .pdata to the memory manager as if it were a DWARF .eh_frame; this one
// registers it with the system instead (RtlAddFunctionTable), relative to the base the loader used for its 32-bit
// image-relative addresses: the lowest address of the object's loaded sections (RuntimeDyldCOFFX86_64's
// getImageBase), each of them allocated here. One memory manager an object (RTDyldObjectLinkingLayer makes one each).
extern "C" __declspec(dllimport) unsigned char __stdcall RtlAddFunctionTable(void* table, unsigned long count,
                                                                            unsigned long long base);
class SehMemoryManager : public llvm::SectionMemoryManager {
public:
    uint8_t* allocateCodeSection(uintptr_t size, unsigned align, unsigned id, llvm::StringRef name) override {
        return note(llvm::SectionMemoryManager::allocateCodeSection(size, align, id, name), size);
    }
    uint8_t* allocateDataSection(uintptr_t size, unsigned align, unsigned id, llvm::StringRef name,
                                 bool read_only) override {
        return note(llvm::SectionMemoryManager::allocateDataSection(size, align, id, name, read_only), size);
    }
    void registerEHFrames(uint8_t* addr, uint64_t, size_t size) override {
        constexpr size_t kEntry = 12; // RUNTIME_FUNCTION: begin, end, unwind info (image-relative)
        if (size >= kEntry && base_ != UINT64_MAX &&
            !RtlAddFunctionTable(addr, static_cast<unsigned long>(size / kEntry), base_))
            throw std::runtime_error("jit: RtlAddFunctionTable failed");
    }
    void deregisterEHFrames() override {} // (the code stays for the whole run)

private:
    uint8_t* note(uint8_t* p, uintptr_t size) {
        (void)size; // (every section loaded counts, as in getImageBase: a non-zero address)
        if (p) base_ = std::min<uint64_t>(base_, reinterpret_cast<uint64_t>(p));
        return p;
    }
    uint64_t base_ = UINT64_MAX;
};
#endif

// ---- State ----
std::unique_ptr<llvm::orc::LLJIT> g_jit;
std::vector<FunctionEntry> g_functions, g_blocks;

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

struct Hash {
    uint64_t h = 0xCBF29CE484222325ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
    }
    void str(const std::string& s) {
        bytes(s.data(), s.size());
        u64(s.size());
    }
    void u64(uint64_t v) { bytes(&v, sizeof v); }
};

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("jit: " + what); }

template <class T> T take(llvm::Expected<T> e, const char* what) {
    if (!e) fail(std::string(what) + ": " + llvm::toString(e.takeError()));
    return std::move(*e);
}
void check(llvm::Error e, const char* what) {
    if (e) fail(std::string(what) + ": " + llvm::toString(std::move(e)));
}

std::vector<char> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A path for the log, UTF-8 (string() can throw on Windows for names outside the system's code page).
std::string shown(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

// The other translations in a cache folder deleted (a ROM set or a version before): only folders named as this
// translator names them (16 hex digits) holding its files, whatever else the folder has kept.
void prune_others(const std::filesystem::path& cache, const std::filesystem::path& keep) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(cache, ec)) {
        const std::string name = e.path().filename().string();
        if (!e.is_directory(ec) || e.path() == keep || name.size() != 16 ||
            name.find_first_not_of("0123456789abcdef") != std::string::npos)
            continue;
        if (!std::filesystem::exists(e.path() / "part0.o", ec)) continue;
        bool only_ours = true;
        for (const auto& f : std::filesystem::directory_iterator(e.path(), ec)) {
            const std::string n = f.path().filename().string();
            only_ours &= f.is_regular_file(ec) && (n == "complete" || (n.rfind("part", 0) == 0 && f.path().extension() == ".o"));
        }
        if (only_ours) std::filesystem::remove_all(e.path(), ec);
    }
}

} // namespace

namespace jit {

std::span<const recomp::FunctionEntry> functions() { return g_functions; }
std::span<const recomp::FunctionEntry> blocks() { return g_blocks; }

Report prepare(const m3::GameSpec& game, const m3::RomSet& roms, Options o) {
    const auto start = std::chrono::steady_clock::now();
    Report report;
    if (o.cache_dir.empty()) o.cache_dir = std::filesystem::path(env("SPIKEOUT_JIT_CACHE"));
    if (o.cache_dir.empty()) o.cache_dir = "jit-cache";
    if (o.threads == 0) o.threads = static_cast<unsigned>(std::atoi(env("SPIKEOUT_JIT_THREADS").c_str()));
    if (o.threads == 0) o.threads = std::max(1u, std::thread::hardware_concurrency());
    if (o.opt_level < 0) o.opt_level = env("SPIKEOUT_JIT_OPT").empty() ? 1 : std::atoi(env("SPIKEOUT_JIT_OPT").c_str());
    if (env("SPIKEOUT_JIT_NO_CACHE") == "1") o.use_cache = false;
    report.threads = o.threads;
    auto say = [&](Step step, unsigned done = 0, unsigned total = 0) {
        if (o.progress) o.progress({step, done, total});
    };
    auto stopped = [&] { return o.cancel && o.cancel->load(std::memory_order_relaxed); };
    say(Step::Cache);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::orc::JITTargetMachineBuilder jtmb = take(llvm::orc::JITTargetMachineBuilder::detectHost(), "host");
    jtmb.setRelocationModel(llvm::Reloc::PIC_);
    jtmb.setCodeModel(llvm::CodeModel::Small);
    const llvm::CodeGenOptLevel levels[] = {llvm::CodeGenOptLevel::None, llvm::CodeGenOptLevel::Less,
                                            llvm::CodeGenOptLevel::Default, llvm::CodeGenOptLevel::Aggressive};
    jtmb.setCodeGenOptLevel(levels[std::clamp(o.opt_level, 0, 3)]);
    // Separate multiplies and adds are never fused (as -ffp-contract=off for the C++ build).
    jtmb.getOptions().AllowFPOpFusion = llvm::FPOpFusion::Strict;

    const Layout layout = recomp::JitAccess::layout();
    const analysis::BootImage boot = analysis::make_boot_image(game, roms);

    // The cache key: what the code is made from, and how.
    Hash key;
    key.str(kTranslatorVersion), key.str(LLVM_VERSION_STRING), key.str(jtmb.getTargetTriple().str());
    key.str(jtmb.getCPU()), key.str(jtmb.getFeatures().getString());
    key.u64(static_cast<uint64_t>(o.opt_level)), key.u64(kParts), key.u64(layout.hash()), key.str(std::string(game.id));
    for (uint32_t h : game.hooks) key.u64(h);
    for (const analysis::Segment& seg : boot.image.segments()) {
        key.str(seg.name), key.u64(seg.base);
        key.bytes(seg.bytes.data(), seg.bytes.size());
    }
    for (const analysis::Entry& e : boot.entries) key.u64(e.addr);
    char key_hex[32];
    std::snprintf(key_hex, sizeof key_hex, "%016llx", static_cast<unsigned long long>(key.h));
    const std::filesystem::path dir = o.cache_dir / key_hex;
    auto part_path = [&](uint32_t i) { return dir / ("part" + std::to_string(i) + ".o"); };

    // The objects: from the cache, or made now.
    std::vector<std::vector<char>> objects;
    if (o.use_cache && std::filesystem::exists(dir / "complete")) {
        for (uint32_t i = 0; i <= kParts; ++i) {
            objects.push_back(read_file(part_path(i)));
            if (objects.back().empty()) {
                objects.clear();
                break;
            }
        }
        report.from_cache = !objects.empty();
        if (report.from_cache) report.cache_path = dir;
    }
    if (objects.empty()) {
        auto t = std::chrono::steady_clock::now();
        say(Step::Analysis);
        // As ppcrecomp: the boot ROM's functions from an analysis of the vectors, the game's with pointers scanned too.
        analysis::Options base;
        for (const analysis::Segment& seg : boot.image.segments())
            if (seg.base < m3::kWorkRamSize) base.pointer_segments.push_back(seg.name);
        analysis::Options ram_options = base;
        ram_options.scan_pointers = true;
        const analysis::Analysis crom_an = analysis::analyze(boot.image, boot.entries, base);
        const analysis::Analysis ram_an = analysis::analyze(boot.image, boot.entries, ram_options);
        std::vector<LiftInput> lifts(2);
        lifts[0] = {"boot_rom", &boot.image, &crom_an, {}, {}};
        lifts[1] = {"game_ram", &boot.image, &ram_an, {}, {}};
        const char* segment_of[] = {"crom", "ram"};
        for (size_t l = 0; l < 2; ++l) {
            lifts[l].hooks.insert(game.hooks.begin(), game.hooks.end());
            for (const auto& [entry, fn] : lifts[l].analysis->functions) {
                const analysis::Segment* seg = boot.image.segment_at(entry);
                if (seg && seg->name == segment_of[l] && !fn.blocks.empty()) lifts[l].functions.insert(entry);
            }
        }
        report.analysis_s = seconds_since(t);
        if (stopped()) throw Cancelled();
        say(Step::Ir);

        t = std::chrono::steady_clock::now();
        std::unique_ptr<llvm::TargetMachine> tm0 = take(jtmb.createTargetMachine(), "target machine");
        recomp::LiftStats stats;
        std::vector<IrPart> parts = build_ir(lifts, layout, kParts, tm0->createDataLayout().getStringRepresentation(),
                                             jtmb.getTargetTriple().str(), &stats);
        report.ir_s = seconds_since(t);
        if (stopped()) throw Cancelled();
        if (o.verbose)
            std::printf( "jit: %u functions, %u blocks, %u instructions, %u return sites\n", stats.functions,
                         stats.blocks, stats.instructions, stats.return_sites);

        t = std::chrono::steady_clock::now();
        objects.resize(parts.size());
        std::atomic<size_t> next{0};
        std::atomic<unsigned> compiled{0};
        say(Step::Compile, 0, static_cast<unsigned>(parts.size()));
        std::mutex error_mutex;
        std::string error;
        const bool verify = env("SPIKEOUT_JIT_VERIFY") == "1";
        const std::string dump = env("SPIKEOUT_JIT_DUMP"); // a folder: each part's IR before and after optimizing
        auto worker = [&] {
            try {
                std::unique_ptr<llvm::TargetMachine> tm = take(jtmb.createTargetMachine(), "target machine");
                for (size_t i; (i = next++) < parts.size();) {
                    if (stopped()) break;
                    llvm::Module& m = *parts[i].module;
                    if (verify && llvm::verifyModule(m, &llvm::errs())) fail("part " + std::to_string(i) + " doesn't verify");
                    auto write_ir = [&](const char* tag) {
                        if (dump.empty()) return;
                        std::error_code ec;
                        llvm::raw_fd_ostream out(dump + "/part" + std::to_string(i) + tag + ".ll", ec);
                        m.print(out, nullptr);
                    };
                    write_ir("");
                    if (o.opt_level > 0) {
                        llvm::LoopAnalysisManager lam;
                        llvm::FunctionAnalysisManager fam;
                        llvm::CGSCCAnalysisManager cgam;
                        llvm::ModuleAnalysisManager mam;
                        llvm::PassBuilder pb(tm.get());
                        pb.registerModuleAnalyses(mam);
                        pb.registerCGSCCAnalyses(cgam);
                        pb.registerFunctionAnalyses(fam);
                        pb.registerLoopAnalyses(lam);
                        pb.crossRegisterProxies(lam, fam, cgam, mam);
                        const llvm::OptimizationLevel ol = o.opt_level == 1   ? llvm::OptimizationLevel::O1
                                                           : o.opt_level == 2 ? llvm::OptimizationLevel::O2
                                                                              : llvm::OptimizationLevel::O3;
                        llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(ol);
                        mpm.run(m, mam);
                    }
                    write_ir("-opt");
                    llvm::SmallVector<char, 0> buffer;
                    llvm::raw_svector_ostream os(buffer);
                    llvm::legacy::PassManager cg;
                    if (tm->addPassesToEmitFile(cg, os, nullptr, llvm::CodeGenFileType::ObjectFile))
                        fail("can't emit an object file");
                    cg.run(m);
                    objects[i].assign(buffer.begin(), buffer.end());
                    parts[i].module.reset();
                    parts[i].context.reset();
                    say(Step::Compile, ++compiled, static_cast<unsigned>(parts.size()));
                }
            } catch (const std::exception& e) {
                std::lock_guard lock(error_mutex);
                error = e.what();
                next = parts.size();
            }
        };
        std::vector<std::thread> threads;
        for (unsigned n = 0; n < std::min<size_t>(o.threads, parts.size()); ++n) threads.emplace_back(worker);
        for (std::thread& th : threads) th.join();
        if (!error.empty()) throw std::runtime_error(error);
        if (stopped()) throw Cancelled();
        report.compile_s = seconds_since(t);

        if (o.use_cache) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            bool ok = !ec;
            for (uint32_t i = 0; ok && i < objects.size(); ++i) {
                std::ofstream out(part_path(i), std::ios::binary);
                out.write(objects[i].data(), static_cast<std::streamsize>(objects[i].size()));
                ok = static_cast<bool>(out);
            }
            if (ok) {
                std::ofstream(dir / "complete") << kTranslatorVersion << "\n";
                report.cache_path = dir;
                if (o.prune) prune_others(o.cache_dir, dir);
            } else if (o.verbose) std::printf( "jit: couldn't write the cache in %s\n", shown(dir).c_str());
        }
    }
    for (const auto& obj : objects) report.object_bytes += obj.size();

    // Load and link them.
    auto t = std::chrono::steady_clock::now();
    say(Step::Load);
    llvm::orc::LLJITBuilder builder;
    builder.setJITTargetMachineBuilder(jtmb);
#ifdef _WIN32
    builder.setObjectLinkingLayerCreator([](llvm::orc::ExecutionSession& es, const llvm::Triple&) {
        auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(es, [] { return std::make_unique<SehMemoryManager>(); });
        layer->setOverrideObjectFlagsWithResponsibilityFlags(true); // (as LLJIT does for COFF)
        layer->setAutoClaimResponsibilityForObjectSymbols(true);
        return llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>(std::move(layer));
    });
#endif
    g_jit = take(builder.create(), "LLJIT");
    llvm::orc::JITDylib& jd = g_jit->getMainJITDylib();
    llvm::orc::SymbolMap symbols;
    for (const Helper& h : kHelpers)
        symbols[g_jit->mangleAndIntern(h.name)] = {llvm::orc::ExecutorAddr::fromPtr(h.address),
                                                   llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    check(jd.define(llvm::orc::absoluteSymbols(std::move(symbols))), "helpers");
    // (the C library: memset, memcpy, fma on CPUs without it)
    jd.addGenerator(take(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                             g_jit->getDataLayout().getGlobalPrefix()),
                         "process symbols"));
    for (size_t i = 0; i < objects.size(); ++i)
        check(g_jit->addObjectFile(llvm::MemoryBuffer::getMemBufferCopy(
                  llvm::StringRef(objects[i].data(), objects[i].size()), "part" + std::to_string(i))),
              "adding an object");
    objects.clear();
    auto table = [&](const char* name) {
        const auto* rows = take(g_jit->lookup(name), name).toPtr<const FunctionEntry*>();
        const auto n = *take(g_jit->lookup(std::string(name) + "_n"), name).toPtr<const uint64_t*>();
        return std::vector<FunctionEntry>(rows, rows + n);
    };
    g_functions = table("spk_functions");
    g_blocks = table("spk_blocks");
    report.load_s = seconds_since(t);
    report.functions = g_functions.size();
    report.blocks = g_blocks.size();
    report.total_s = seconds_since(start);
    if (o.verbose) {
        if (report.from_cache)
            std::printf( "jit: loaded from the cache (%s) in %.2f s: %zu entries, %zu blocks, %.1f MB of code objects\n",
                         shown(dir).c_str(), report.total_s, report.functions, report.blocks, static_cast<double>(report.object_bytes) / 1e6);
        else
            std::printf(
                         "jit: translated in %.2f s (analysis %.2f, IR %.2f, compile -O%d on %u threads %.2f, link %.2f): "
                         "%zu entries, %zu blocks, %.1f MB of code objects\n",
                         report.total_s, report.analysis_s, report.ir_s, o.opt_level, o.threads, report.compile_s,
                         report.load_s, report.functions, report.blocks, static_cast<double>(report.object_bytes) / 1e6);
    }
    return report;
}

} // namespace jit

// The tables game_setup.hpp reads (the boot ROM's are in the game's).
namespace generated {
std::span<const recomp::FunctionEntry> game_ram_functions() { return jit::functions(); }
std::span<const recomp::FunctionEntry> game_ram_blocks() { return jit::blocks(); }
std::span<const recomp::FunctionEntry> boot_rom_functions() { return {}; }
std::span<const recomp::FunctionEntry> boot_rom_blocks() { return {}; }
} // namespace generated
