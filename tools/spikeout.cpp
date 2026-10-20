// spikeout: the recompiled game in a window (SDL3).
//
//   spikeout <rom_dir> [--scale N] [--netboard sim] [--frames N] [--mute] [--quads]
//                     [--midi-burst] [--dma-instant] [--sound-volume N] [--music-volume N]
//                     [--boards N] [--no-gpu] [--soft-3d] [--render-scale N]
//
// The game runs on its own thread, paced to the Model 3's 57.524 Hz; the
// window shows the tile generator's 2D layers and the Real3D's picture. The
// sound and music boards play through the default audio device (--mute: not
// at all).
//
// Esc (or a pad's Guide, its touchpad, or Back + Start) opens the menu, which
// pauses the game: Display (render scale, antialiasing, quads, the 3D
// renderer, fullscreen), Sound (the effects' and the music's volumes),
// Controls (each player's keys; the pads), Game (reset, the arcade's test
// menu, a service credit) and Quit. Its choices are kept in settings.ini
// (tools/settings.hpp), written when the menu closes; options on the command
// line win over them for that run. Without a GPU (--no-gpu) there is no menu
// and Esc quits.
//
// Keys (as set in Controls):  player 1: 5 coin, 1 start, arrows, Z beat,
// X charge, C jump, V shift;  player 2: 6 coin, 2 start, I J K L, A beat,
// S charge, D jump, F shift. F2 test, F1 service, Tab (held) as fast as it
// goes, F12 a screenshot (spikeout-<frame>.png).
// Pads (the first for player 1, the second for player 2): d-pad or left
// stick, A beat, X charge, B jump, Y shift, Start, Back coin.
//
// --boards N (2-4) runs N cabinets linked in a ring, as in an arcade: each
// is a complete board with its own player, the window split between them
// (side by side, or 2 x 2). Board 0 is set up as the master and the others
// as slaves (tools::kLinkSettingAddr). They take turns on their threads
// every few display lines (gamerun --boards). Player 1's keys drive board 0
// (F5-F8 move them to board 1-4); player 2's drive board 1. Only board 0's
// sound plays. A testing aid: linked play is meant to be one cabinet per
// computer.
//
// The window draws through the GPU (Vulkan, Metal) when SDL finds one it
// can use, else, or with --no-gpu, through SDL's 2D renderer. On the GPU the
// 3D is drawn there too (gpu::Real3dGpu), one board only; --soft-3d (or the
// menu) keeps it in software. The render scale draws the 3D N times as large
// each way (Auto: as large as the window shows it), antialiasing that again
// and averages it down (the 2D layers stay 496x384).
//
// --frames N quits after N frames and writes the last one to
// spikeout-<N>.png (for testing). --quads draws quads whole, as Supermodel's
// "quad rendering" option. --midi-burst sends the sound board its bytes with
// Supermodel's timing instead of the serial chip's; --dma-instant finishes
// Real3D DMA copies at once (Supermodel, MAME). --sound-volume and
// --music-volume set the sound effects (the two SCSPs) and the music board
// apart, in percent (0-200, default 100).
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
#include "settings.hpp"
#ifdef SPIKEOUT_GPU
#include "gpu/present.hpp"
#include "gpu/real3d_gpu.hpp"
#include "ui/menu.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
    std::atomic<bool> quit{false}, fast{false}, paused{false};
    uint64_t max_frames = 0; // 0: run until the window is closed
    bool real_netboard = true;
    std::atomic<bool> quads{false}; // draw quads whole
    bool midi_burst = false;        // Supermodel's MIDI timing
    bool dma_instant = false;       // Real3D DMA done at once (Supermodel, MAME)
    Sound* sound = nullptr;         // nullptr: muted

    // The 3D drawn by the GPU (one board): the game thread leaves each
    // frame's scene and 2D layers here, and the window draws them.
    std::atomic<bool> gpu_3d{false};
    struct GpuFrame {
        m3::Real3dScene scene;
        std::vector<uint32_t> bottom, top;
        uint64_t number = 0;
    } gpu_frame;
    // Texture RAM as of the last upload, and which upload that was.
    std::shared_ptr<const std::vector<uint16_t>> textures;
    uint64_t textures_version = 0;
};

// The input bits a player's actions are (bank 0: coin, start; the controls byte).
struct InputBit {
    bool bank0;
    int bit;
};
constexpr std::array<InputBit, tools::kActions> kActionBits = {{
    {true, 0}, {true, 4}, {false, 5}, {false, 4}, {false, 7}, {false, 6}, {false, 0}, {false, 1}, {false, 3}, {false, 2},
}};

// A player's pad: which of the actions it holds.
std::array<bool, tools::kActions> pad_actions(SDL_Gamepad* pad) {
    std::array<bool, tools::kActions> held{};
    if (!pad) return held;
    auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    using A = tools::Action;
    auto set = [&](A a, bool on) { held[static_cast<std::size_t>(a)] = held[static_cast<std::size_t>(a)] || on; };
    set(A::Coin, button(SDL_GAMEPAD_BUTTON_BACK));
    set(A::Start, button(SDL_GAMEPAD_BUTTON_START));
    set(A::Up, button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -16000);
    set(A::Down, button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 16000);
    set(A::Left, button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -16000);
    set(A::Right, button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 16000);
    set(A::Beat, button(SDL_GAMEPAD_BUTTON_SOUTH));
    set(A::Charge, button(SDL_GAMEPAD_BUTTON_WEST));
    set(A::Jump, button(SDL_GAMEPAD_BUTTON_EAST));
    set(A::Shift, button(SDL_GAMEPAD_BUTTON_NORTH));
    return held;
}

// Keys and pads to the I/O boards' input bytes (bits clear while pressed):
// player 1's onto board `focus`, player 2's onto board 1 when there is one.
void read_inputs(Shared& shared, std::size_t focus, const tools::Settings& settings, const std::array<SDL_Gamepad*, 2>& pads,
                 bool test, bool service) {
    const bool* k = SDL_GetKeyboardState(nullptr);
    for (Screen& s : shared.screens) {
        s.bank0 = 0xFF;
        s.bank1 = 0xFF;
        s.controls = 0xFF;
    }
    auto apply = [&](Screen& s, std::size_t player) {
        const auto held = pad_actions(pads[player]);
        uint8_t bank0 = s.bank0, controls = s.controls;
        for (std::size_t a = 0; a < tools::kActions; ++a) {
            const int key = settings.keys[player][a];
            if (!(held[a] || (key > 0 && k[key]))) continue;
            if (kActionBits[a].bank0) bank0 = static_cast<uint8_t>(bank0 & ~(1u << kActionBits[a].bit));
            else controls = static_cast<uint8_t>(controls & ~(1u << kActionBits[a].bit));
        }
        s.bank0 = bank0;
        s.controls = controls;
    };
    if (shared.boards > 1 && focus != 1) apply(shared.screens[1], 1);
    Screen& p1 = shared.screens[focus];
    apply(p1, 0);
    uint8_t bank0 = p1.bank0;
    if (k[SDL_SCANCODE_F2] || test) bank0 = static_cast<uint8_t>(bank0 & ~(1u << 2));
    if (k[SDL_SCANCODE_F1] || service) bank0 = static_cast<uint8_t>(bank0 & ~(1u << 3));
    p1.bank0 = bank0;
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
    void set_volumes(int sound, int music) {
        std::lock_guard<std::mutex> guard(lock_);
        board_.set_volumes(sound, music);
    }
    // Silent and still while the game is paused (the menu).
    void pause(bool on) {
        if (!stream_) return;
        if (on) SDL_PauseAudioStreamDevice(stream_);
        else SDL_ResumeAudioStreamDevice(stream_);
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
        std::lock_guard<std::mutex> guard(lock_);
        while (done < frames) {
            std::size_t until = frames;
            // Into the board's FIFO as it has room; the rest waits here.
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
    m3::Real3dSceneBuilder builder;
    Shared::GpuFrame gpu_frame;
    uint64_t textures_seen = UINT64_MAX;
    timing.on_frame_end = [&] {
        if (shared.gpu_3d && index == 0) { // the scene and layers for the window to draw
            const m3::Real3d& mem = bus.real3d();
            builder.build(mem, gpu_frame.scene);
            gpu_frame.bottom.assign(tilegen.bottom().begin(), tilegen.bottom().end());
            gpu_frame.top.assign(tilegen.top().begin(), tilegen.top().end());
            gpu_frame.number = timing.frames();
            std::shared_ptr<const std::vector<uint16_t>> textures;
            if (mem.textures_uploaded() != textures_seen) {
                textures_seen = mem.textures_uploaded();
                textures = std::make_shared<const std::vector<uint16_t>>(mem.texture_ram().begin(), mem.texture_ram().end());
            }
            std::lock_guard<std::mutex> guard(shared.lock);
            std::swap(shared.gpu_frame, gpu_frame);
            if (textures) {
                shared.textures = std::move(textures);
                ++shared.textures_version;
            }
            out.frame_number = timing.frames();
        } else {
            real3d.set_quads(shared.quads);
            real3d.render(bus.real3d());
            tilegen.composite(screen, real3d.image());
            textures_seen = UINT64_MAX; // (the GPU, if turned on again, takes texture RAM afresh)
            std::lock_guard<std::mutex> guard(shared.lock);
            out.frame.swap(screen);
            out.frame_number = timing.frames();
        }
        if (shared.sound && index == 0) shared.sound->send(bus.midi_out());
        else bus.midi_out().clear();
        if (linked && timing.frames() < tools::kLinkSettingFrames) bus.write8(tools::kLinkSettingAddr, link_setting);
        // The menu: held here, between frames, until it closes.
        bool was_paused = false;
        while (shared.paused && !shared.quit) {
            was_paused = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        bus.inputs.bank0 = out.bank0;
        bus.inputs.bank1 = out.bank1;
        bus.inputs.controls = out.controls;
        if (shared.max_frames && timing.frames() >= shared.max_frames) shared.quit = true;
        if (shared.quit) throw Stop{};
        if (index != 0) return; // board 0 paces them all
        if (shared.fast || was_paused) { // run free, and pace from here when Tab is let go or the menu closes
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

std::string on_off(bool on) { return on ? "On" : "Off"; }

} // namespace

int main(int argc, char** argv) {
    const char* dir = nullptr;
    int scale = 0, cli_render_scale = 0, cli_sound = -1, cli_music = -1;
    bool real_netboard = true;
    uint64_t max_frames = 0;
    bool mute = false, cli_quads = false, midi_burst = false, dma_instant = false, use_gpu = true, cli_soft_3d = false;
    std::size_t boards = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--netboard" && i + 1 < argc) real_netboard = std::string(argv[++i]) == "real";
        else if (arg == "--frames" && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--boards" && i + 1 < argc)
            boards = std::clamp<std::size_t>(std::strtoull(argv[++i], nullptr, 0), 1, kMaxBoards);
        else if (arg == "--quads") cli_quads = true;
        else if (arg == "--midi-burst") midi_burst = true;
        else if (arg == "--dma-instant") dma_instant = true;
        else if (arg == "--mute") mute = true;
        else if (arg == "--no-gpu") use_gpu = false;
        else if (arg == "--soft-3d") cli_soft_3d = true;
        else if (arg == "--render-scale" && i + 1 < argc) cli_render_scale = std::clamp(std::atoi(argv[++i]), 1, 6);
        else if (arg == "--sound-volume" && i + 1 < argc) cli_sound = std::clamp(std::atoi(argv[++i]), 0, 200);
        else if (arg == "--music-volume" && i + 1 < argc) cli_music = std::clamp(std::atoi(argv[++i]), 0, 200);
        else if (!dir) dir = argv[i];
    }
    if (!dir) {
        std::fprintf(stderr, "usage: spikeout <rom_dir> [--scale N] [--netboard sim|real] [--frames N] [--mute] [--quads]\n"
                             "                [--midi-burst] [--dma-instant] [--sound-volume N] [--music-volume N]\n"
                             "                [--boards N] [--no-gpu] [--soft-3d] [--render-scale N]\n");
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "spikeout: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // The settings, and the command line over them.
    const std::string settings_path = tools::data_folder() + "settings.ini";
    tools::Settings settings = tools::load_settings(settings_path);
    if (cli_render_scale) settings.render_scale = cli_render_scale;
    if (cli_quads) settings.quads = true;
    if (cli_soft_3d) settings.gpu_3d = false;
    if (cli_sound >= 0) settings.sound_volume = cli_sound;
    if (cli_music >= 0) settings.music_volume = cli_music;
    const bool test_run = max_frames != 0; // (writes no settings)

    SDL_Window* window = SDL_CreateWindow("Spikeout: Final Edition", width * scale, height * scale, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "spikeout: %s\n", SDL_GetError());
        return 1;
    }
    if (settings.fullscreen && !test_run) SDL_SetWindowFullscreen(window, true);
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
    shared.quads = settings.quads;
    shared.midi_burst = midi_burst;
    shared.dma_instant = dma_instant;
    std::unique_ptr<Sound> sound;
    if (!mute) sound = std::make_unique<Sound>(roms, settings.sound_volume, settings.music_volume);
    shared.sound = sound.get();

#ifdef SPIKEOUT_GPU
    // The 3D on the GPU too (one board; linked test runs draw it in software),
    // drawn at the render scale times the antialiasing.
    std::unique_ptr<gpu::Real3dGpu> real3d_gpu;
    int draw_scale = 0;
    auto wanted_scale = [&] {
        int base = settings.render_scale;
        if (base == 0) { // Auto: as large as the window shows it
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            const double fit = std::min(static_cast<double>(pw) / kWidth, static_cast<double>(ph) / kHeight);
            base = std::clamp(static_cast<int>(std::ceil(fit - 0.05)), 1, 6);
        }
        return std::clamp(base * settings.antialiasing, 1, 8);
    };
    auto set_up_3d = [&] {
        const bool want = on_gpu && boards == 1 && settings.gpu_3d;
        const int s = wanted_scale();
        if (want && real3d_gpu && s == draw_scale) {
            real3d_gpu->set_quads(settings.quads);
            return;
        }
        real3d_gpu.reset();
        draw_scale = 0;
        if (want) {
            real3d_gpu = std::make_unique<gpu::Real3dGpu>();
            real3d_gpu->set_quads(settings.quads);
            if (real3d_gpu->open(*gpu_device, s)) {
                draw_scale = s;
            } else {
                std::printf("spikeout: 3D in software (%s)\n", real3d_gpu->error().c_str());
                real3d_gpu.reset();
            }
        }
        shared.gpu_3d = real3d_gpu != nullptr;
    };
    set_up_3d();
    Shared::GpuFrame gpu_frame; // the frame the window shows
    std::shared_ptr<const std::vector<uint16_t>> textures;
    uint64_t textures_version = 0;
    // Reads the GPU's 3D back and composes the screen (F12, the end of a test
    // run), at 496x384: drawn larger, every draw_scale-th pixel.
    auto capture = [&] {
        std::vector<uint32_t> drawn, three_d(static_cast<std::size_t>(kWidth) * kHeight);
        if (textures && real3d_gpu) real3d_gpu->render(gpu_frame.scene, *textures, textures_version, &drawn);
        const auto s = static_cast<std::size_t>(std::max(draw_scale, 1));
        const auto stride = static_cast<std::size_t>(kWidth) * s;
        for (std::size_t y = 0; y < static_cast<std::size_t>(kHeight) && !drawn.empty(); ++y)
            for (std::size_t x = 0; x < static_cast<std::size_t>(kWidth); ++x)
                three_d[y * kWidth + x] = drawn[y * s * stride + x * s];
        std::vector<uint32_t> screen(static_cast<std::size_t>(kWidth) * kHeight);
        m3::TilegenRenderer::composite(screen, gpu_frame.bottom, gpu_frame.top, three_d);
        return screen;
    };

    // The menu, over the picture.
    ui::Menu menu;
    bool menu_ready = false;
    if (on_gpu) {
        menu_ready = menu.init(gpu_device->get(), window);
        if (menu_ready) {
            menu.set_footer("Spikeout: Final Edition recomp");
            presenter->overlay = [&](SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain, uint32_t w, uint32_t h) {
                menu.render(cmd, swapchain, w, h);
            };
        } else {
            std::printf("spikeout: no menu (%s)\n", menu.error().c_str());
        }
    }
#else
    const bool menu_ready = false;
#endif

    std::unique_ptr<m3::LinkRing> ring;
    std::unique_ptr<tools::Turns> turns;
    std::vector<std::thread> games;
    auto start_games = [&] {
        ring.reset();
        if (boards > 1) ring = std::make_unique<m3::LinkRing>(boards);
        turns = std::make_unique<tools::Turns>(boards);
        for (std::size_t i = 0; i < boards; ++i)
            games.emplace_back([&, i] { run_game(roms, i, ring.get(), *turns, shared); });
    };
    auto stop_games = [&] {
        shared.quit = true;
        for (std::thread& g : games) g.join();
        games.clear();
    };
    start_games();

    // Pads: the first two connected, for players 1 and 2.
    std::vector<SDL_Gamepad*> gamepads;
    auto player_pads = [&] {
        std::array<SDL_Gamepad*, 2> p{};
        for (std::size_t i = 0; i < 2 && i < gamepads.size(); ++i) p[i] = gamepads[i];
        return p;
    };

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
    // The arcade's test and service buttons, held for a moment from the menu.
    auto test_until = std::chrono::steady_clock::time_point{}, service_until = test_until;

    // A restart: the boards from power on, the settings kept.
    auto reset_game = [&] {
        stop_games();
        shared.quit = false;
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            for (Screen& s : shared.screens) {
                s.stopped.clear();
                s.frame_number = 0;
            }
            shared.gpu_frame.number = 0;
        }
        shown_number = {};
#ifdef SPIKEOUT_GPU
        gpu_frame.number = 0;
#endif
        reported = 0;
        start_games();
    };

#ifdef SPIKEOUT_GPU
    // ---- The menu's pages ----
    bool settings_changed = false;
    auto changed = [&] { settings_changed = true; };
    auto back_item = [&] { return ui::MenuItem{"Back", "Back to the previous page.", {}, {}, [&] { menu.pop(); }}; };
    auto display_page = [&] {
        ui::MenuPage p{"Display", "", {}};
        p.items.push_back({"Render scale",
                           "How sharp the 3D is: 1x is the arcade's own 384 lines. Auto draws it as large as the window "
                           "shows it. Higher needs a faster graphics card.",
                           [&] {
                               if (settings.render_scale == 0 && draw_scale == 0) return std::string("Auto");
                               return settings.render_scale == 0 ? "Auto (" + std::to_string(draw_scale / settings.antialiasing) + "x)"
                                                                 : std::to_string(settings.render_scale) + "x";
                           },
                           [&](int d) {
                               settings.render_scale = std::clamp(settings.render_scale + d, 0, 6);
                               changed();
                               set_up_3d();
                           }});
        p.items.push_back({"Antialiasing",
                           "Smooths jagged edges: the 3D drawn 2, 3 or 4 times as large again each way and averaged "
                           "down. Cleaner, but 4, 9 or 16 times the work.",
                           [&] { return settings.antialiasing == 1 ? std::string("Off") : std::to_string(settings.antialiasing) + "x"; },
                           [&](int d) {
                               settings.antialiasing = std::clamp(settings.antialiasing + d, 1, 4);
                               changed();
                               set_up_3d();
                           }});
        p.items.push_back({"Quads drawn whole",
                           "Draws four-sided polygons whole, as the Real3D does, rather than as two triangles "
                           "(Supermodel's quad rendering). Only bent or unevenly textured ones look different.",
                           [&] { return on_off(settings.quads); },
                           [&](int) {
                               settings.quads = !settings.quads;
                               shared.quads = settings.quads;
                               changed();
                               set_up_3d();
                           }});
        p.items.push_back({"3D renderer",
                           "Which part of your computer draws the 3D: the graphics card (GPU), or the processor "
                           "(CPU, the reference the GPU is checked against).",
                           [&] {
                               if (settings.gpu_3d && !real3d_gpu) return std::string("GPU (not available: CPU)");
                               return std::string(settings.gpu_3d ? "GPU" : "CPU");
                           },
                           [&](int) {
                               settings.gpu_3d = !settings.gpu_3d;
                               changed();
                               set_up_3d();
                           }});
        p.items.push_back({"Fullscreen", "Play fullscreen or in a window.", [&] { return on_off(settings.fullscreen); },
                           [&](int) {
                               settings.fullscreen = !settings.fullscreen;
                               SDL_SetWindowFullscreen(window, settings.fullscreen);
                               changed();
                           }});
        p.items.push_back(back_item());
        return p;
    };
    auto sound_page = [&] {
        ui::MenuPage p{"Sound", "", {}};
        auto volume = [&](const char* label, const char* help, int& v) {
            return ui::MenuItem{label, help, [&v] { return std::to_string(v) + "%"; },
                                [&](int d) {
                                    v = std::clamp(v + 10 * d, 0, 200);
                                    if (sound) sound->set_volumes(settings.sound_volume, settings.music_volume);
                                    changed();
                                }};
        };
        p.items.push_back(volume("Sound effects", "Hits, voices and effects: the sound board's two SCSPs.", settings.sound_volume));
        p.items.push_back(volume("Music", "The music board's tunes.", settings.music_volume));
        p.items.push_back(back_item());
        return p;
    };
    auto keys_page = [&](std::size_t player) {
        ui::MenuPage p{"Player " + std::to_string(player + 1) + " keys", "Enter: change a key. Delete: none.", {}};
        for (std::size_t a = 0; a < tools::kActions; ++a) {
            p.items.push_back({tools::kActionNames[a], "",
                               [&, player, a] {
                                   const int key = settings.keys[player][a];
                                   return key > 0 ? std::string(SDL_GetScancodeName(static_cast<SDL_Scancode>(key))) : std::string("None");
                               },
                               {},
                               [&, player, a] {
                                   menu.capture("Press a key for " + std::string(tools::kActionNames[a]) + " (Esc: cancel)",
                                                [&, player, a](const SDL_Event& e) {
                                                    if (e.type != SDL_EVENT_KEY_DOWN) return false;
                                                    settings.keys[player][a] = e.key.scancode;
                                                    changed();
                                                    return true;
                                                });
                               },
                               false,
                               [&, player, a] {
                                   settings.keys[player][a] = 0;
                                   changed();
                               }});
        }
        p.items.push_back({"Reset to defaults", "This player's keys as they were.", {}, {}, [&, player] {
                               settings.keys[player] = tools::Settings::defaults().keys[player];
                               changed();
                           }});
        p.items.push_back(back_item());
        return p;
    };
    auto controls_page = [&] {
        ui::MenuPage p{"Controls", "", {}};
        p.items.push_back({"Player 1 keys", "Change player 1's keys.", {}, {}, [&] { menu.push(keys_page(0)); }});
        p.items.push_back({"Player 2 keys", "Change player 2's keys (a second cabinet: --boards 2).", {}, {},
                           [&] { menu.push(keys_page(1)); }});
        for (std::size_t i = 0; i < 2; ++i) {
            const auto pads = player_pads();
            const char* name = pads[i] ? SDL_GetGamepadName(pads[i]) : nullptr;
            ui::MenuItem info{"Player " + std::to_string(i + 1) + " pad", "", [name] { return std::string(name ? name : "None"); }};
            info.info = true;
            p.items.push_back(info);
        }
        ui::MenuItem layout{"Pad buttons: A beat, X charge, B jump, Y shift, Back coin, Start; Guide or Back + Start: the menu", ""};
        layout.info = true;
        p.items.push_back(layout);
        p.items.push_back(back_item());
        return p;
    };
    auto game_page = [&] {
        ui::MenuPage p{"Game", "", {}};
        p.items.push_back({"Reset game", "Restarts the game, as switching the arcade off and on.", {}, {}, [&] {
                               menu.close();
                               reset_game();
                           }});
        p.items.push_back({"Arcade test menu",
                           "Opens the arcade's own test menu: its settings, the network link, the tests. In it, "
                           "player 1's Start chooses and the test button (F2) moves.",
                           {}, {}, [&] {
                               menu.close();
                               test_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                           }});
        p.items.push_back({"Service credit", "Adds a credit as the arcade's service button does.", {}, {}, [&] {
                               menu.close();
                               service_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                           }});
        p.items.push_back(back_item());
        return p;
    };
    auto root_page = [&] {
        ui::MenuPage p{"", "", {}};
        p.items.push_back({"Resume", "Back to the game.", {}, {}, [&] { menu.close(); }});
        p.items.push_back({"Display", "Render scale, antialiasing, quads, the 3D renderer and fullscreen.", {}, {},
                           [&] { menu.push(display_page()); }});
        p.items.push_back({"Sound", "The sound effects' and the music's volumes.", {}, {}, [&] { menu.push(sound_page()); }});
        p.items.push_back({"Controls", "Each player's keys, and the pads.", {}, {}, [&] { menu.push(controls_page()); }});
        p.items.push_back({"Game", "Reset, the arcade's test menu, a service credit.", {}, {}, [&] { menu.push(game_page()); }});
        p.items.push_back({"Quit", "Saves the settings and closes the game.", {}, {}, [&] {
                               menu.close();
                               running = false;
                           }});
        return p;
    };
    bool menu_was_open = false;
    auto open_menu = [&] {
        if (menu_ready && !menu.is_open()) menu.open(root_page());
    };
#endif

    // Tests: SPIKEOUT_MENU_KEYS=esc,down,enter,... presses these keys (esc,
    // up, down, left, right, enter, or SDL key names) a quarter of a second
    // apart, from the game's frame 300.
    std::vector<SDL_Scancode> test_keys;
    if (const char* keys = std::getenv("SPIKEOUT_MENU_KEYS")) {
        std::string list = keys;
        for (size_t at = 0; at <= list.size();) {
            const size_t comma = std::min(list.find(',', at), list.size());
            const std::string name = list.substr(at, comma - at);
            const SDL_Scancode sc = name == "esc" ? SDL_SCANCODE_ESCAPE
                                    : name == "enter" ? SDL_SCANCODE_RETURN
                                                      : SDL_GetScancodeFromName(name.c_str());
            if (sc != SDL_SCANCODE_UNKNOWN) test_keys.push_back(sc);
            at = comma + 1;
        }
    }
    std::size_t test_key = 0;
    auto test_key_due = std::chrono::steady_clock::time_point{};

    while (running) {
        if (test_key < test_keys.size() && shown_number[0] >= 300 && std::chrono::steady_clock::now() >= test_key_due) {
            SDL_Event key{};
            key.type = SDL_EVENT_KEY_DOWN;
            key.key.scancode = test_keys[test_key++];
            key.key.down = true;
            SDL_PushEvent(&key);
            test_key_due = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) gamepads.push_back(pad);
            }
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (auto it = gamepads.begin(); it != gamepads.end(); ++it)
                    if (SDL_GetGamepadID(*it) == e.gdevice.which) {
                        SDL_CloseGamepad(*it);
                        gamepads.erase(it);
                        break;
                    }
            }
#ifdef SPIKEOUT_GPU
            if (menu_ready && menu.handle_event(e)) continue;
            if (menu_ready && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
                const bool both = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
                                  SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD || both)
                    open_menu();
            }
#endif
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE && !e.key.repeat) {
#ifdef SPIKEOUT_GPU
                if (menu_ready) open_menu();
                else running = false;
#else
                running = false;
#endif
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat) {
                const std::string path = "spikeout-" + std::to_string(shown_number[0]) + ".png";
#ifdef SPIKEOUT_GPU
                if (shared.gpu_3d) shown = capture();
#endif
                if (tools::write_png(path, shown, width, height)) std::printf("screenshot: %s\n", path.c_str());
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode >= SDL_SCANCODE_F5 && e.key.scancode <= SDL_SCANCODE_F8) {
                const auto board = static_cast<std::size_t>(e.key.scancode - SDL_SCANCODE_F5);
                if (board < boards) focus = board;
            }
#ifdef SPIKEOUT_GPU
            // Auto render scale: the window's size decides it.
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && settings.render_scale == 0) set_up_3d();
#endif
        }
#ifdef SPIKEOUT_GPU
        // The menu pauses the game; closing it keeps the settings.
        const bool menu_open = menu_ready && menu.is_open();
        if (menu_open != menu_was_open) {
            shared.paused = menu_open;
            if (sound) sound->pause(menu_open);
            if (!menu_open && settings_changed && !test_run) {
                tools::save_settings(settings_path, settings);
                settings_changed = false;
            }
            menu_was_open = menu_open;
        }
#endif
        const auto now = std::chrono::steady_clock::now();
        read_inputs(shared, focus, settings, player_pads(), now < test_until, now < service_until);
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
#ifdef SPIKEOUT_GPU
            // A newer frame for the GPU to draw (the swap leaves the older one
            // in its place, for the game thread to fill next).
            if (shared.gpu_3d && shared.gpu_frame.number > gpu_frame.number) {
                std::swap(gpu_frame, shared.gpu_frame);
                if (shared.textures_version != textures_version) {
                    textures = shared.textures;
                    textures_version = shared.textures_version;
                }
            }
#endif
            if (max_frames && reported == (1u << boards) - 1) { // a test run: keep the last frames and close
                for (std::size_t b = 0; b < boards; ++b) place(b, shared.screens[b].frame);
#ifdef SPIKEOUT_GPU
                if (shared.gpu_3d) shown = capture();
#endif
                const std::string path = "spikeout-" + std::to_string(shown_number[0]) + ".png";
                if (tools::write_png(path, shown, width, height)) std::printf("screenshot: %s\n", path.c_str());
                running = false;
            }
        }
        if (!reported && now - fps_start >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(now - fps_start).count();
            char title[96];
            std::snprintf(title, sizeof title, "Spikeout: Final Edition  (%.1f fps)", static_cast<double>(fps_frames) / secs);
            SDL_SetWindowTitle(window, title);
            fps_start = now;
            fps_frames = 0;
        }
#ifdef SPIKEOUT_GPU
        if (on_gpu && shared.gpu_3d && real3d_gpu) {
            if (textures && !gpu_frame.bottom.empty()) {
                real3d_gpu->render(gpu_frame.scene, *textures, textures_version);
                presenter->present_layers(gpu_frame.bottom, gpu_frame.top, real3d_gpu->display_image(), kWidth, kHeight);
            } else {
                presenter->present(shown, width, height);
            }
            continue;
        }
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

#ifdef SPIKEOUT_GPU
    if (settings_changed && !test_run) tools::save_settings(settings_path, settings);
#else
    (void)menu_ready;
#endif
    shared.paused = false;
    stop_games();
    sound.reset();
    for (SDL_Gamepad* pad : gamepads) SDL_CloseGamepad(pad);
#ifdef SPIKEOUT_GPU
    presenter->overlay = nullptr;
    menu.shutdown();
    real3d_gpu.reset(); // before the device, and all before the window
    presenter.reset();
    gpu_device.reset();
#endif
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
