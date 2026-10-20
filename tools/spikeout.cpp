// spikeout: the recompiled game in a window (SDL3).
//
//   spikeout <rom_dir> [--scale N] [--netboard sim] [--frames N] [--mute] [--quads]
//                     [--midi-burst] [--dma-instant] [--sound-volume N] [--music-volume N]
//                     [--boards N] [--no-gpu]
//
// The game runs on its own thread, paced to the Model 3's 57.524 Hz; the
// window shows the tile generator's 2D layers and the Real3D's picture
// (drawn in software for now). The sound and music boards play through the
// default audio device (--mute: not at all).
// The keyboard goes to the I/O board:
//
//   5 coin      1 start      F2 test      F1 service
//   arrows      joystick
//   Z beat      X charge     C jump       V shift
//   Tab (held)  run as fast as possible
//   F12         screenshot (spikeout-<frame>.png)
//   Esc         quit
//
// --boards N (2-4) runs N cabinets linked in a ring, as in an arcade: each
// is a complete board with its own player, the window split between them
// (side by side, or 2 x 2). Board 0 is set up as the master and the others
// as slaves (tools::kLinkSettingAddr). They take turns on their threads
// every few display lines (gamerun --boards). The keys above drive board 0
// (F5-F8 move them to board 1-4); a second set drives board 1:
//
//   6 coin      2 start      I J K L    joystick
//   A beat      S charge     D jump     F shift
//
// Only board 0's sound plays.
//
// The window draws through the GPU (Vulkan, Metal) when SDL finds one it
// can use, else, or with --no-gpu, through SDL's 2D renderer.
//
// --frames N quits after N frames and writes the last one to
// spikeout-<N>.png (for testing). --quads draws quads whole, as Supermodel's
// "quad rendering" option (slower in software). --midi-burst sends the sound
// board its bytes with Supermodel's timing instead of the serial chip's;
// --dma-instant finishes Real3D DMA copies at once (Supermodel, MAME).
// --sound-volume and --music-volume set the sound effects (the two SCSPs) and
// the music board apart, in percent (0-200, default 100).
//
// Built only when CMake's SPIKEOUT_ROM_DIR is set (it links the recompiled
// game).
#include "games/games.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/rom.hpp"
#include "runtime/real3d_render.hpp"
#include "runtime/tilegen.hpp"
#include "game_setup.hpp"
#include "png_write.hpp"
#ifdef SPIKEOUT_GPU
#include "gpu/present.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = m3::TilegenRenderer::kWidth, kHeight = m3::TilegenRenderer::kHeight;
constexpr std::size_t kMaxBoards = 4;
constexpr uint32_t kTurnLines = 8; // display lines a linked board runs before passing the turn on

struct Stop {}; // thrown from the frame callback to end the run

class Sound;

// One board's picture and inputs.
struct Screen {
    std::vector<uint32_t> frame = std::vector<uint32_t>(kWidth * kHeight, 0xFF000000u); // last finished frame
    uint64_t frame_number = 0;
    std::string stopped; // why the game stopped, once it has
    // Inputs, active low, as the I/O board shows them.
    std::atomic<uint8_t> bank0{0xFF}, bank1{0xFF}, controls{0xFF};
};

// What the window and the game threads share.
struct Shared {
    std::mutex lock;
    std::array<Screen, kMaxBoards> screens;
    std::size_t boards = 1;
    std::atomic<bool> quit{false}, fast{false};
    uint64_t max_frames = 0; // 0: run until the window is closed
    bool real_netboard = true;
    bool quads = false;      // draw quads whole (slower)
    bool midi_burst = false; // Supermodel's MIDI timing
    bool dma_instant = false; // Real3D DMA done at once (Supermodel, MAME)
    Sound* sound = nullptr; // nullptr: muted
};

// Keyboard state to the I/O boards' input bytes (bits clear while pressed).
void read_keys(Shared& shared, std::size_t focus) {
    const bool* k = SDL_GetKeyboardState(nullptr);
    auto bits = [&](std::initializer_list<std::pair<SDL_Scancode, int>> map) {
        uint8_t v = 0xFF;
        for (const auto& [key, bit] : map)
            if (k[key]) v = static_cast<uint8_t>(v & ~(1u << bit));
        return v;
    };
    for (Screen& s : shared.screens) {
        s.bank0 = 0xFF;
        s.controls = 0xFF;
    }
    // The second player's keys first: board 1, unless the first player's
    // keys are there too.
    if (shared.boards > 1 && focus != 1) {
        Screen& p2 = shared.screens[1];
        p2.bank0 = bits({{SDL_SCANCODE_6, 0}, {SDL_SCANCODE_2, 4}});
        p2.controls = bits({{SDL_SCANCODE_A, 0}, {SDL_SCANCODE_S, 1}, {SDL_SCANCODE_F, 2}, {SDL_SCANCODE_D, 3},
                            {SDL_SCANCODE_K, 4}, {SDL_SCANCODE_I, 5}, {SDL_SCANCODE_L, 6}, {SDL_SCANCODE_J, 7}});
    }
    Screen& p1 = shared.screens[focus];
    p1.bank0 = bits({{SDL_SCANCODE_5, 0}, {SDL_SCANCODE_F2, 2}, {SDL_SCANCODE_F1, 3}, {SDL_SCANCODE_1, 4}});
    p1.bank1 = 0xFF;
    p1.controls = bits({{SDL_SCANCODE_Z, 0}, {SDL_SCANCODE_X, 1}, {SDL_SCANCODE_V, 2}, {SDL_SCANCODE_C, 3},
                        {SDL_SCANCODE_DOWN, 4}, {SDL_SCANCODE_UP, 5}, {SDL_SCANCODE_RIGHT, 6},
                        {SDL_SCANCODE_LEFT, 7}});
    shared.fast = k[SDL_SCANCODE_TAB];
}

// The sound board plays on the audio device's clock, as the real one runs
// on its own crystal: SDL asks for samples as it needs them, and the board
// takes the game's MIDI bytes as they arrive. A slow frame on the main
// board (the software 3D) then delays a sound command by that much instead
// of starving the device, which crackled. The bytes keep the spacing the
// serial line gave them: each plays at its arrival time plus an offset,
// set a frame ahead of the device whenever a byte would be late (or far
// ahead, running fast with Tab).
class Sound {
public:
    Sound(const m3::RomSet& roms, int sound_volume, int music_volume)
        : board_(roms.region(m3::RomRegionId::SoundProgram), roms.region(m3::RomRegionId::SoundSamples)) {
        tools::attach_music(board_, roms);
        board_.set_volumes(sound_volume, music_volume);
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, m3::Scsp::kSampleRate};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Sound::feed, this);
        if (stream_) SDL_ResumeAudioStreamDevice(stream_);
        else std::fprintf(stderr, "spikeout: no sound: %s\n", SDL_GetError());
    }
    ~Sound() {
        if (stream_) SDL_DestroyAudioStream(stream_); // stops the callbacks first
    }

    // From the game thread: the MIDI bytes that arrived during a frame
    // (empties the list).
    void send(std::vector<m3::SerialByte>& midi) {
        if (stream_) {
            std::lock_guard<std::mutex> guard(lock_);
            for (const m3::SerialByte& b : midi) midi_.push_back({b.value, static_cast<double>(b.cycle) * kSamplesPerCycle});
        }
        midi.clear();
    }

private:
    static constexpr double kSamplesPerCycle =
        static_cast<double>(m3::Scsp::kSampleRate) / static_cast<double>(ppc::CpuState::kCoreClockHz);
    static constexpr double kLead = m3::Scsp::kSampleRate / recomp::FrameTiming::kRefreshHz; // a frame
    struct Byte {
        uint8_t value;
        double time; // in samples of emulated time
    };

    static void SDLCALL feed(void* self, SDL_AudioStream* stream, int additional, int) {
        static_cast<Sound*>(self)->play(stream, additional);
    }

    void play(SDL_AudioStream* stream, int bytes) {
        const auto frames = static_cast<std::size_t>(bytes + 3) / 4;
        buffer_.resize(2 * frames);
        std::size_t done = 0;
        while (done < frames) {
            std::size_t until = frames;
            {
                // Into the board's FIFO as it has room; the rest waits here.
                std::lock_guard<std::mutex> guard(lock_);
                while (!midi_.empty() && board_.midi_waiting() < 192) {
                    const double pos = static_cast<double>(played_ + done);
                    double at = midi_.front().time + offset_;
                    if (!anchored_ || at < pos || at > pos + 4 * kLead) {
                        offset_ = pos + kLead - midi_.front().time;
                        at = pos + kLead;
                        anchored_ = true;
                    }
                    const double into = at - static_cast<double>(played_);
                    if (into >= static_cast<double>(frames)) break;
                    if (const auto sample = static_cast<std::size_t>(into); sample > done) {
                        until = sample;
                        break;
                    }
                    board_.midi_in(midi_.front().value);
                    midi_.pop_front();
                }
            }
            board_.render(buffer_.data() + 2 * done, until - done);
            done = until;
        }
        played_ += frames;
        SDL_PutAudioStreamData(stream, buffer_.data(), static_cast<int>(frames * 4));
    }

    m3::SoundBoard board_;
    std::mutex lock_;
    std::deque<Byte> midi_;
    uint64_t played_ = 0; // samples
    double offset_ = 0;   // device samples minus emulated samples
    bool anchored_ = false;
    std::vector<int16_t> buffer_;
    SDL_AudioStream* stream_ = nullptr;
};

// Board `index`: run until quit. Board 0 paces the run (the others take
// turns with it); with one board there are no turns.
void run_game(const m3::RomSet& roms, std::size_t index, m3::LinkRing* ring, tools::Turns& turns, Shared& shared) {
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();
    Screen& out = shared.screens[index];
    m3::Bus bus(roms);
    bus.set_security_key(game.security_key);
    tools::set_rtc_to_host(bus);
    if (shared.real_netboard) bus.use_netboard(std::make_unique<m3::CommBoard>(ring ? ring->port(index) : nullptr));
    else if (ring) bus.use_netboard(std::make_unique<m3::SimNetBoard>(ring->port(index)));
    bus.midi_burst = shared.midi_burst;
    bus.dma_timed = !shared.dma_instant;
    const bool linked = ring != nullptr;
    const uint8_t link_setting = index == 0 ? 1 : 2; // master, slave

    recomp::Context c(bus, functions);
    recomp::FrameTiming timing;
    c.set_scheduler(&timing);
    m3::TilegenRenderer tilegen;
    m3::Real3dRenderer real3d;
    real3d.set_quads(shared.quads);
    std::vector<uint32_t> screen(kWidth * kHeight);

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    uint64_t paced_from = 0; // frame count at `start`
    timing.on_display_line = [&](uint32_t line) {
        if (line == 0) tilegen.begin_frame();
        tilegen.draw_line(bus, static_cast<int>(line));
        if (linked && line % kTurnLines == kTurnLines - 1) {
            bus.netboard().sync(c.cycles());
            turns.pass(index);
        }
    };
    timing.on_frame_end = [&] {
        real3d.render(bus.real3d());
        tilegen.composite(screen, real3d.image());
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            out.frame.swap(screen);
            out.frame_number = timing.frames();
        }
        if (shared.sound && index == 0) shared.sound->send(bus.midi_out());
        else bus.midi_out().clear();
        if (linked && timing.frames() < tools::kLinkSettingFrames) bus.write8(tools::kLinkSettingAddr, link_setting);
        bus.inputs.bank0 = out.bank0;
        bus.inputs.bank1 = out.bank1;
        bus.inputs.controls = out.controls;
        if (shared.max_frames && timing.frames() >= shared.max_frames) shared.quit = true;
        if (shared.quit) throw Stop{};
        if (index != 0) return; // board 0 paces them all
        if (shared.fast) { // run free, and pace from here when Tab is let go
            start = clock::now();
            paced_from = timing.frames();
            return;
        }
        const auto due = start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(
                                     static_cast<double>(timing.frames() - paced_from) / recomp::FrameTiming::kRefreshHz));
        const auto now = clock::now();
        if (due > now) std::this_thread::sleep_until(due);
        else if (now - due > std::chrono::milliseconds(250)) { // too far behind: don't try to catch up
            start = now;
            paced_from = timing.frames();
        }
    };

    std::string why;
    turns.wait(index);
    try {
        c.call(c.cpu.pc);
        why = "the reset code returned";
    } catch (const Stop&) {
    } catch (const recomp::MissingCode& e) {
        why = e.what();
    } catch (const recomp::GuestError& e) {
        why = e.what();
    }
    turns.finish(index);
    std::lock_guard<std::mutex> guard(shared.lock);
    out.stopped = why.empty() ? "quit" : why;
}

// The boards' screens side by side (2) or 2 x 2 (3, 4).
int grid_columns(std::size_t boards) { return boards == 1 ? 1 : 2; }
int grid_rows(std::size_t boards) { return boards <= 2 ? 1 : 2; }

} // namespace

int main(int argc, char** argv) {
    const char* dir = nullptr;
    int scale = 0;
    bool real_netboard = true;
    uint64_t max_frames = 0;
    bool mute = false, quads = false, midi_burst = false, dma_instant = false, use_gpu = true;
    int sound_volume = 100, music_volume = 100;
    std::size_t boards = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--netboard" && i + 1 < argc) real_netboard = std::string(argv[++i]) == "real";
        else if (arg == "--frames" && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--boards" && i + 1 < argc)
            boards = std::clamp<std::size_t>(std::strtoull(argv[++i], nullptr, 0), 1, kMaxBoards);
        else if (arg == "--quads") quads = true;
        else if (arg == "--midi-burst") midi_burst = true;
        else if (arg == "--dma-instant") dma_instant = true;
        else if (arg == "--mute") mute = true;
        else if (arg == "--no-gpu") use_gpu = false;
        else if (arg == "--sound-volume" && i + 1 < argc) sound_volume = std::atoi(argv[++i]);
        else if (arg == "--music-volume" && i + 1 < argc) music_volume = std::atoi(argv[++i]);
        else if (!dir) dir = argv[i];
    }
    if (!dir) {
        std::fprintf(stderr, "usage: spikeout <rom_dir> [--scale N] [--netboard sim|real] [--frames N] [--mute] [--quads]\n"
                             "                [--midi-burst] [--dma-instant] [--sound-volume N] [--music-volume N]\n"
                             "                [--boards N] [--no-gpu]\n");
        return 2;
    }
    if (scale == 0) scale = boards == 1 ? 2 : 1;
    const m3::RomSet roms = m3::load_rom_set(m3::games::kSpikeoutFinalEdition, dir);
    if (!roms.complete()) {
        std::fprintf(stderr, "spikeout: ROM set incomplete; run romcheck for details\n");
        return 1;
    }

    const int cols = grid_columns(boards), rows = grid_rows(boards);
    const int width = kWidth * cols, height = kHeight * rows;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "spikeout: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Spikeout: Final Edition", width * scale, height * scale, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "spikeout: %s\n", SDL_GetError());
        return 1;
    }
    // Shown through the GPU (Vulkan, Metal) when there is one SDL can use,
    // otherwise through SDL's 2D renderer.
#ifdef SPIKEOUT_GPU
    auto gpu_device = std::make_unique<gpu::Device>();
    auto presenter = std::make_unique<gpu::Presenter>();
    bool on_gpu = false;
    if (use_gpu) {
        on_gpu = gpu_device->open(window) && presenter->open(*gpu_device);
        if (on_gpu) std::printf("spikeout: drawing with the GPU (%s)\n", gpu_device->driver());
        else {
            std::printf("spikeout: no GPU (%s); drawing with SDL's renderer\n", gpu_device->error().c_str());
            presenter.reset(); // gives the window back
            gpu_device.reset();
        }
    }
#else
    const bool on_gpu = false;
    (void)use_gpu;
#endif
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    if (!on_gpu) {
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            std::fprintf(stderr, "spikeout: %s\n", SDL_GetError());
            return 1;
        }
        SDL_SetRenderVSync(renderer, 1);
        SDL_SetRenderLogicalPresentation(renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }

    Shared shared;
    shared.boards = boards;
    shared.max_frames = max_frames;
    shared.real_netboard = real_netboard;
    shared.quads = quads;
    shared.midi_burst = midi_burst;
    shared.dma_instant = dma_instant;
    std::unique_ptr<Sound> sound;
    if (!mute) sound = std::make_unique<Sound>(roms, sound_volume, music_volume);
    shared.sound = sound.get();
    std::unique_ptr<m3::LinkRing> ring;
    if (boards > 1) ring = std::make_unique<m3::LinkRing>(boards);
    tools::Turns turns(boards);
    std::vector<std::thread> games;
    for (std::size_t i = 0; i < boards; ++i)
        games.emplace_back([&, i] { run_game(roms, i, ring.get(), turns, shared); });

    // What the window shows: every board's last frame in the grid.
    std::vector<uint32_t> shown(static_cast<std::size_t>(width) * height, 0xFF000000u);
    auto place = [&](std::size_t board, const std::vector<uint32_t>& frame) {
        const int x0 = static_cast<int>(board) % cols * kWidth, y0 = static_cast<int>(board) / cols * kHeight;
        for (int y = 0; y < kHeight; ++y)
            std::copy_n(frame.begin() + y * kWidth, kWidth, shown.begin() + (y0 + y) * width + x0);
    };
    std::array<uint64_t, kMaxBoards> shown_number{};
    std::size_t focus = 0; // the board the first player's keys drive
    bool running = true;
    std::size_t reported = 0;
    auto fps_start = std::chrono::steady_clock::now();
    uint64_t fps_frames = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat) {
                const std::string path = "spikeout-" + std::to_string(shown_number[0]) + ".png";
                if (tools::write_png(path, shown, width, height)) std::printf("screenshot: %s\n", path.c_str());
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode >= SDL_SCANCODE_F5 && e.key.scancode <= SDL_SCANCODE_F8) {
                const auto board = static_cast<std::size_t>(e.key.scancode - SDL_SCANCODE_F5);
                if (board < boards) focus = board;
            }
        }
        read_keys(shared, focus);
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            for (std::size_t b = 0; b < boards; ++b) {
                Screen& s = shared.screens[b];
                if (s.frame_number != shown_number[b]) {
                    place(b, s.frame);
                    if (b == 0) fps_frames += s.frame_number - shown_number[b];
                    shown_number[b] = s.frame_number;
                }
                if (!s.stopped.empty() && !(reported & (1u << b))) {
                    std::printf("spikeout: board %zu stopped at frame %llu: %s\n", b,
                                static_cast<unsigned long long>(shown_number[b]), s.stopped.c_str());
                    SDL_SetWindowTitle(window, ("Spikeout: stopped (" + s.stopped + ")").c_str());
                    reported |= 1u << b;
                }
            }
            if (max_frames && reported == (1u << boards) - 1) { // a test run: keep the last frames and close
                for (std::size_t b = 0; b < boards; ++b) place(b, shared.screens[b].frame);
                const std::string path = "spikeout-" + std::to_string(shown_number[0]) + ".png";
                if (tools::write_png(path, shown, width, height)) std::printf("screenshot: %s\n", path.c_str());
                running = false;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!reported && now - fps_start >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(now - fps_start).count();
            char title[96];
            std::snprintf(title, sizeof title, "Spikeout: Final Edition  (%.1f fps)", static_cast<double>(fps_frames) / secs);
            SDL_SetWindowTitle(window, title);
            fps_start = now;
            fps_frames = 0;
        }
#ifdef SPIKEOUT_GPU
        if (on_gpu) {
            presenter->present(shown, width, height); // waits for the display (vsync)
            continue;
        }
#endif
        SDL_UpdateTexture(texture, nullptr, shown.data(), width * 4);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    shared.quit = true;
    for (std::thread& g : games) g.join();
    sound.reset();
#ifdef SPIKEOUT_GPU
    presenter.reset(); // before the device, and both before the window
    gpu_device.reset();
#endif
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
