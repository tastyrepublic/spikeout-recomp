// spikeout: the recompiled game in a window (SDL3).
//
//   spikeout <rom_dir> [--scale N] [--netboard sim] [--frames N] [--mute] [--quads]
//                     [--midi-burst] [--dma-instant] [--sound-volume N] [--music-volume N]
//                     [--boards N] [--no-gpu] [--soft-3d] [--render-scale N] [--fifo]
//                     [--sleep-pacing]
//
// The game runs on its own thread, paced to the Model 3's 57.524 Hz (waiting
// busy between frames; --sleep-pacing sleeps, saving power but letting the
// CPU slow down, which made some frames late); the
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
// and averages it down (the 2D layers stay 496x384). Each frame is shown as
// it finishes, its 3D drawn once, presented in mailbox mode where the GPU
// has it (no tearing; --fifo: plain vsync, which on some Windows setups
// shows only every second or third refresh; gpu::Device::open).
//
// --record FILE records the run from power-on (tools::Recording: the
// inputs each frame, and main RAM's hash now and then), to play again with
// gamerun --replay FILE; a restart starts FILE-2 and so on.
//
// Save states (one board): F9 saves the game as it is at the end of the
// frame (spikeout-<frame>.state, the same as gamerun --save-state writes,
// so gamerun --load-state runs on from it exactly); F10 starts again from
// the last one saved; --load-state FILE starts from FILE.
//
// Smooth motion (the menu, or --smooth; the GPU 3D only): a picture each
// refresh of the display (at most 120 a second, timed as the game's frames
// are), its 3D placed between the last two frames (m3::interpolate) by an
// even display clock a frame behind the game's, pulled a tenth of the way to
// it each picture (after vf3-recomp's: the pictures drawn when a pass
// happened to run would show the motion unevenly). The 2D is the newest
// frame's.
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
#include "runtime/widescreen.hpp"
#include "runtime/real3d_interp.hpp"
#include "game_setup.hpp"
#include "recording.hpp"
#include "png_write.hpp"
#include "arcade_settings.hpp"
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
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
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
    // The board's memory that a cabinet keeps switched off: the settings
    // EEPROM (64 words) and the backup RAM (rankings). The game takes them as
    // it starts and gives them back as it stops; the window keeps them in
    // files (empty: none yet, a new board).
    std::vector<uint16_t> eeprom;
    std::vector<uint8_t> backup;
    m3::Bus* bus = nullptr; // while the game runs (the menu reads the settings with the game paused)
};

// What the window and the game threads share.
struct Shared {
    std::mutex lock;
    std::array<Screen, kMaxBoards> screens;
    std::size_t boards = 1;
    std::atomic<bool> quit{false}, fast{false}, paused{false};
    uint64_t max_frames = 0; // 0: run until the window is closed
    // --record FILE: board 0's run recorded (tools::Recording), from power-on;
    // each restart a file of its own (FILE, then FILE-2 ...)
    std::string record_path;
    int recordings = 0;
    // Save states (one board): F9 asks board 0 to save at the end of its
    // frame (spikeout-<frame>.state, as gamerun --save-state writes them);
    // `load_path` is loaded as board 0 starts (F10 restarts it with the last
    // one saved, or --load-state's).
    std::atomic<bool> save_request{false};
    std::string load_path, last_state; // (under `lock`)
    bool real_netboard = true;
    std::atomic<bool> quads{false}; // draw quads whole
    bool midi_burst = false;        // Supermodel's MIDI timing
    bool dma_instant = false;       // Real3D DMA done at once (Supermodel, MAME)
    bool sleep_pacing = false;      // board 0 sleeps between frames, not waits busy
    Sound* sound = nullptr;         // nullptr: muted

    // The 3D drawn by the GPU (one board): the game thread leaves each
    // frame's scene and 2D layers here, and the window draws them.
    std::atomic<bool> gpu_3d{false};
    // Widescreen: how far the full-screen viewports are opened out each side,
    // in screen pixels (m3::Real3dSceneBuilder::set_extend; 0: the arcade's).
    std::atomic<int> extend{0};
    struct GpuFrame {
        m3::Real3dScene scene;
        std::vector<uint32_t> bottom, top;
        // widescreen: the layers as wide as the 3D (m3::WideLayer)
        std::vector<uint32_t> bottom_wide, top_wide;
        // the tile generator's control (which layers are above the 3D) and
        // scroll registers at the frame's end: smooth motion slides the sky
        uint32_t tile_control = 0;
        std::array<uint32_t, 4> tile_scroll{};
        // The 2D above the 3D opaque everywhere (the sides too): the 3D can't
        // be seen (the stage the game draws behind its logo), so isn't drawn.
        bool covered = false;
        uint64_t number = 0;
        int extend = 0; // the widescreen extension the scene was built with
    } gpu_frame;
    // Texture RAM rows the game wrote, copied for the window (its copy kept
    // up to date with them; the GPU given only them).
    struct TextureRows {
        uint32_t first, count;
        std::vector<uint16_t> texels;
    };
    std::vector<TextureRows> texture_rows;

    // Frames the boards have finished (all of them together): the window
    // waits on it, to show each frame as it is done.
    uint64_t frames_done = 0;
    std::condition_variable frame_done;
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
// The sound outputs (Settings::speakers): the cabinet's front and rear pairs
// mixed into stereo, both on one output of four channels (front left and
// right, rear left and right: SDL's quad; SDL fits them to an output of
// other channels, mixing them down on a stereo one), or each pair on an
// output of its own (named; empty: the default).
struct SpeakerSetup {
    int mode = 0; // 0 stereo, 1 four channels, 2 two outputs
    std::string front, rear;
};

// An output by its name (empty or not there: the default).
SDL_AudioDeviceID audio_device(const std::string& name) {
    SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (name.empty()) return found;
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; i < n; ++i)
        if (const char* d = SDL_GetAudioDeviceName(ids[i]); d && name == d) found = ids[i];
    SDL_free(ids);
    return found;
}

class Sound {
public:
    Sound(const m3::RomSet& roms, int sound_volume, int music_volume, const SpeakerSetup& speakers)
        : board_(roms.region(m3::RomRegionId::SoundProgram), roms.region(m3::RomRegionId::SoundSamples)) {
        tools::attach_music(board_, roms);
        board_.set_volumes(sound_volume, music_volume);
        open(speakers);
    }
    ~Sound() { close(); }

    // The outputs again (the board keeps playing).
    void open(const SpeakerSetup& speakers) {
        close();
        mode_ = speakers.mode;
        const int channels = mode_ == 1 ? 4 : 2;
        const SDL_AudioSpec spec{SDL_AUDIO_S16, channels, m3::Scsp::kSampleRate};
        stream_ = SDL_OpenAudioDeviceStream(audio_device(mode_ == 0 ? "" : speakers.front), &spec, &Sound::feed, this);
        if (!stream_) {
            std::fprintf(stderr, "spikeout: no sound: %s\n", SDL_GetError());
            return;
        }
        if (mode_ == 2) { // the rear pair, fed from the front's callback
            const SDL_AudioSpec rear{SDL_AUDIO_S16, 2, m3::Scsp::kSampleRate};
            rear_ = SDL_OpenAudioDeviceStream(audio_device(speakers.rear), &rear, nullptr, nullptr);
            if (!rear_) std::fprintf(stderr, "spikeout: no rear output: %s\n", SDL_GetError());
        }
        if (!paused_) {
            SDL_ResumeAudioStreamDevice(stream_);
            if (rear_) SDL_ResumeAudioStreamDevice(rear_);
        }
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
    // Save states, as tools::FrameSound keeps them: the board, and the MIDI
    // bytes it hasn't played yet (loaded, they play as soon as they can;
    // `cycle`: the game's time now).
    void state(m3::State& s, uint64_t cycle) {
        std::lock_guard<std::mutex> guard(lock_);
        board_.state(s);
        double due = 0;
        s.io(due);
        std::vector<uint8_t> waiting;
        for (const Byte& b : midi_) waiting.push_back(b.value);
        s.io(waiting);
        if (s.loading()) {
            midi_.clear();
            for (uint8_t b : waiting) midi_.push_back({b, static_cast<double>(cycle) * kSamplesPerCycle});
            anchored_ = false;
        }
    }
    // Silent and still while the game is paused (the menu).
    void pause(bool on) {
        paused_ = on;
        for (SDL_AudioStream* s : {stream_, rear_}) {
            if (!s) continue;
            if (on) SDL_PauseAudioStreamDevice(s);
            else SDL_ResumeAudioStreamDevice(s);
        }
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

    void close() {
        if (stream_) SDL_DestroyAudioStream(stream_); // stops the callbacks first
        if (rear_) SDL_DestroyAudioStream(rear_);
        stream_ = rear_ = nullptr;
    }

    void play(SDL_AudioStream* stream, int bytes) {
        const int channels = mode_ == 1 ? 4 : 2, rendered = mode_ == 0 ? 2 : 4;
        const auto frames = static_cast<std::size_t>(bytes + 2 * channels - 1) / static_cast<std::size_t>(2 * channels);
        buffer_.resize(static_cast<std::size_t>(rendered) * frames);
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
            board_.render(buffer_.data() + static_cast<std::size_t>(rendered) * done, until - done, rendered);
            done = until;
        }
        played_ += frames;
        if (mode_ != 2) {
            SDL_PutAudioStreamData(stream, buffer_.data(), static_cast<int>(frames * 2 * static_cast<std::size_t>(channels)));
            return;
        }
        // Two outputs: the front pair here, the rear pair to the other one
        // (its own clock: what it hasn't played after a few frames is dropped,
        // so it can't fall behind).
        front_.resize(2 * frames);
        rear_buffer_.resize(2 * frames);
        for (std::size_t i = 0; i < frames; ++i) {
            front_[2 * i] = buffer_[4 * i];
            front_[2 * i + 1] = buffer_[4 * i + 1];
            rear_buffer_[2 * i] = buffer_[4 * i + 2];
            rear_buffer_[2 * i + 1] = buffer_[4 * i + 3];
        }
        SDL_PutAudioStreamData(stream, front_.data(), static_cast<int>(frames * 4));
        if (rear_) {
            if (SDL_GetAudioStreamQueued(rear_) > static_cast<int>(4 * kLead * 4)) SDL_ClearAudioStream(rear_);
            SDL_PutAudioStreamData(rear_, rear_buffer_.data(), static_cast<int>(frames * 4));
        }
    }

    m3::SoundBoard board_;
    std::mutex lock_;
    std::deque<Byte> midi_;
    uint64_t played_ = 0; // samples
    double offset_ = 0;   // device samples minus emulated samples
    bool anchored_ = false;
    std::vector<int16_t> buffer_, front_, rear_buffer_;
    SDL_AudioStream* stream_ = nullptr; // (with the callback)
    SDL_AudioStream* rear_ = nullptr;   // two outputs: the rear pair's
    int mode_ = 0;                      // SpeakerSetup::mode
    bool paused_ = false;
};

// Board `index`: run until quit. Board 0 paces the run (the others take
// turns with it); with one board there are no turns.
void run_game(const m3::RomSet& roms, std::size_t index, m3::LinkRing* ring, tools::Turns& turns, Shared& shared) {
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();
    Screen& out = shared.screens[index];
    m3::Bus bus(roms);
    bus.set_security_key(game.security_key);
    const int64_t clock_start = tools::set_rtc_to_host(bus);
    tools::RecordingWriter recording;
    {
        std::lock_guard<std::mutex> guard(shared.lock);
        if (out.eeprom.size() == 64) std::copy(out.eeprom.begin(), out.eeprom.end(), bus.eeprom().words().begin());
        if (out.backup.size() == m3::Bus::kBackupRamSize) std::copy(out.backup.begin(), out.backup.end(), bus.backup_ram().begin());
        out.bus = &bus;
        if (index == 0 && !shared.record_path.empty()) {
            std::string path = shared.record_path;
            if (++shared.recordings > 1) {
                const size_t dot = path.find_last_of('.');
                const std::string n = "-" + std::to_string(shared.recordings);
                path = dot == std::string::npos ? path + n : path.substr(0, dot) + n + path.substr(dot);
            }
            if (recording.open(path, clock_start, bus.eeprom().words(), bus.backup_ram()))
                std::printf("spikeout: recording to %s\n", path.c_str());
            else
                std::printf("spikeout: can't record to %s\n", path.c_str());
        }
    }
    if (shared.real_netboard) bus.use_netboard(std::make_unique<m3::CommBoard>(ring ? ring->port(index) : nullptr));
    else if (ring) bus.use_netboard(std::make_unique<m3::SimNetBoard>(ring->port(index)));
    bus.midi_burst = shared.midi_burst;
    bus.dma_timed = !shared.dma_instant;
    const bool linked = ring != nullptr;
    const uint8_t link_setting = index == 0 ? 1 : 2; // master, slave

    recomp::Context c(bus, functions);
    const std::vector<recomp::FunctionEntry> blocks = tools::game_blocks();
    c.set_blocks(blocks);
    recomp::FrameTiming timing;
    c.set_scheduler(&timing);
    const std::function<void(m3::State&)> sound_state =
        shared.sound && index == 0 ? std::function<void(m3::State&)>([&](m3::State& s) { shared.sound->state(s, c.cycles()); })
                                   : std::function<void(m3::State&)>();
    bool loaded = false;    // from a save state: the run resumes there
    std::string load_error; // a state that doesn't fit stops the board, saying why
    if (index == 0) {
        std::string path;
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            path.swap(shared.load_path);
        }
        if (!path.empty()) {
            try {
                m3::State s(tools::read_file(path));
                tools::board_state(s, c, timing, sound_state);
                loaded = true;
                std::printf("spikeout: loaded %s (frame %llu)\n", path.c_str(), static_cast<unsigned long long>(timing.frames()));
            } catch (const std::exception& e) {
                load_error = path + ": " + e.what();
                std::printf("spikeout: can't load %s\n", load_error.c_str());
            }
            if (recording.is_open()) {
                recording.close();
                std::printf("spikeout: not recording (a recording starts at power-on)\n");
            }
        }
    }
    m3::TilegenRenderer tilegen;
    m3::Real3dRenderer real3d;
    std::vector<uint32_t> screen(kWidth * kHeight);

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    uint64_t paced_from = timing.frames(); // frame count at `start` (a save state's, loaded)
    timing.on_display_line = [&](uint32_t line) {
        if (line == 0) tilegen.begin_frame();
        tilegen.draw_line(bus, static_cast<int>(line));
        if (linked && line % kTurnLines == kTurnLines - 1) {
            bus.netboard().sync(c.cycles());
            turns.pass(index);
        }
    };
    // Board 0 paces them all to the Model 3's refresh: it holds each finished
    // frame until it is due, so frames reach the window evenly spaced however
    // long each took to run. (Tab runs free; after it, or the menu, pacing
    // starts again from there.)
    bool repace = false;
    auto pace = [&] {
        if (index != 0) return;
        if (shared.fast || repace) {
            start = clock::now();
            paced_from = timing.frames();
            repace = shared.fast;
            return;
        }
        const auto due = start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(
                                     static_cast<double>(timing.frames() - paced_from) / recomp::FrameTiming::kRefreshHz));
        auto now = clock::now();
        if (now - due > std::chrono::milliseconds(250)) { // too far behind: don't try to catch up
            start = now;
            paced_from = timing.frames();
            return;
        }
        // Waited out busy (yielding): a thread that sleeps most of each frame
        // lets the CPU clock its core down (Windows' balanced power plan),
        // and one frame in twenty then ran 15-27 ms instead of 5, showing as
        // a hitch. --sleep-pacing sleeps (saves power; sleeping also wakes up
        // to a millisecond or two late, so the last of it is waited out).
        if (shared.sleep_pacing && due - now > std::chrono::milliseconds(2))
            std::this_thread::sleep_until(due - std::chrono::milliseconds(2));
        while (clock::now() < due) std::this_thread::yield();
    };
    m3::Real3dSceneBuilder builder;
    m3::WideLayer wide_bottom, wide_top;
    Shared::GpuFrame gpu_frame;
    bool textures_whole = true; // the next frame sends all of texture RAM
    timing.on_frame_end = [&] {
        if (shared.gpu_3d && index == 0) { // the scene and layers for the window to draw
            const m3::Real3d& mem = bus.real3d();
            builder.set_extend(shared.extend);
            builder.build(mem, gpu_frame.scene);
            gpu_frame.extend = builder.extend();
            gpu_frame.bottom.assign(tilegen.bottom().begin(), tilegen.bottom().end());
            gpu_frame.top.assign(tilegen.top().begin(), tilegen.top().end());
            if (gpu_frame.extend > 0) {
                const bool drawn = tilegen.extend() == gpu_frame.extend; // the tile generator's own sides
                wide_bottom.build(tilegen.bottom(), drawn ? tilegen.bottom_side() : std::span<const uint32_t>(),
                                  gpu_frame.extend, gpu_frame.bottom_wide);
                wide_top.build(tilegen.top(), drawn ? tilegen.top_side() : std::span<const uint32_t>(), gpu_frame.extend,
                               gpu_frame.top_wide, true);
            }
            tilegen.set_extend(gpu_frame.extend);
            {
                const std::vector<uint32_t>& above = gpu_frame.extend > 0 ? gpu_frame.top_wide : gpu_frame.top;
                gpu_frame.covered = !above.empty() && std::all_of(above.begin(), above.end(), [](uint32_t p) { return (p >> 24) != 0; });
            }
            gpu_frame.number = timing.frames();
            gpu_frame.tile_control = bus.tile_reg(0x20);
            for (uint32_t n = 0; n < 4; ++n) gpu_frame.tile_scroll[n] = bus.tile_reg(0x60 + 4 * n);
            // The texture RAM rows written since the last frame, copied for
            // the window (all of them the first time, or after the software 3D).
            std::vector<Shared::TextureRows> rows;
            {
                auto runs = bus.real3d().take_written_rows();
                if (textures_whole) runs.assign(1, {0u, m3::Real3d::kTextureHeight});
                textures_whole = false;
                for (const auto& [first, count] : runs) {
                    const auto from = mem.texture_ram().begin() + static_cast<std::ptrdiff_t>(size_t{first} * m3::Real3d::kTextureWidth);
                    rows.push_back({first, count, std::vector<uint16_t>(from, from + static_cast<std::ptrdiff_t>(size_t{count} * m3::Real3d::kTextureWidth))});
                }
            }
            pace();
            std::lock_guard<std::mutex> guard(shared.lock);
            std::swap(shared.gpu_frame, gpu_frame);
            for (auto& r : rows) shared.texture_rows.push_back(std::move(r));
            out.frame_number = timing.frames();
        } else {
            real3d.set_quads(shared.quads);
            real3d.render(bus.real3d());
            tilegen.composite(screen, real3d.image());
            textures_whole = true; // (the GPU, if turned on again, takes texture RAM afresh)
            pace();
            std::lock_guard<std::mutex> guard(shared.lock);
            out.frame.swap(screen);
            out.frame_number = timing.frames();
        }
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            ++shared.frames_done;
        }
        shared.frame_done.notify_one();
        if (shared.sound && index == 0) shared.sound->send(bus.midi_out());
        else bus.midi_out().clear();
        if (linked && timing.frames() < tools::kLinkSettingFrames) bus.write8(tools::kLinkSettingAddr, link_setting);
        // The menu: held here, between frames, until it closes.
        while (shared.paused && !shared.quit) {
            repace = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        bus.inputs.bank0 = out.bank0;
        bus.inputs.bank1 = out.bank1;
        bus.inputs.controls = out.controls;
        if (index == 0 && shared.save_request.exchange(false))
            c.request_snapshot([&](uint32_t) {
                const std::string path = "spikeout-" + std::to_string(timing.frames()) + ".state";
                try {
                    m3::State s;
                    tools::board_state(s, c, timing, sound_state);
                    if (!tools::write_file(path, s.data())) throw std::runtime_error("can't write it");
                    std::lock_guard<std::mutex> guard(shared.lock);
                    shared.last_state = path;
                    std::printf("spikeout: saved %s\n", path.c_str());
                } catch (const std::exception& e) {
                    std::printf("spikeout: no save state %s: %s\n", path.c_str(), e.what());
                }
            });
        if (recording.is_open()) {
            const auto frame = static_cast<uint32_t>(timing.frames());
            if (frame % tools::Recording::kHashEvery == 0) recording.hash(frame, tools::Recording::ram_hash(bus.ram()));
            recording.inputs(frame, {bus.inputs.bank0, bus.inputs.bank1, bus.inputs.controls, bus.inputs.controls2});
        }
        if (shared.max_frames && timing.frames() >= shared.max_frames) shared.quit = true;
        if (shared.quit) throw Stop{};
    };

    std::string why;
    turns.wait(index);
    try {
        if (!load_error.empty()) throw recomp::GuestError(c.cpu.pc, "save state " + load_error);
        if (loaded) c.resume_block(c.cpu.pc);
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
    out.eeprom.assign(bus.eeprom().words().begin(), bus.eeprom().words().end());
    out.backup.assign(bus.backup_ram().begin(), bus.backup_ram().end());
    out.bus = nullptr;
}

// The boards' screens side by side (2) or 2 x 2 (3, 4).
int grid_columns(std::size_t boards) { return boards == 1 ? 1 : 2; }
int grid_rows(std::size_t boards) { return boards <= 2 ? 1 : 2; }

std::string on_off(bool on) { return on ? "On" : "Off"; }

// The boards' EEPROM and backup RAM, kept between runs as a cabinet keeps
// them: nvram/board<N>.eeprom (64 words, high byte first) and
// nvram/board<N>.backup in the data folder. A missing or short file: a new
// board (the game sets its EEPROM up as it first starts).
void load_nvram(const std::string& folder, Shared& shared, std::size_t boards) {
    for (std::size_t b = 0; b < boards; ++b) {
        Screen& s = shared.screens[b];
        const std::string base = folder + "nvram/board" + std::to_string(b);
        std::ifstream e(base + ".eeprom", std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(e)), std::istreambuf_iterator<char>());
        s.eeprom.clear();
        if (bytes.size() == 128)
            for (std::size_t i = 0; i < 64; ++i)
                s.eeprom.push_back(static_cast<uint16_t>(static_cast<uint8_t>(bytes[2 * i]) << 8 | static_cast<uint8_t>(bytes[2 * i + 1])));
        std::ifstream r(base + ".backup", std::ios::binary);
        s.backup.assign(std::istreambuf_iterator<char>(r), std::istreambuf_iterator<char>());
        if (s.backup.size() != m3::Bus::kBackupRamSize) s.backup.clear();
    }
}

void save_nvram(const std::string& folder, const Shared& shared, std::size_t boards) {
    SDL_CreateDirectory((folder + "nvram").c_str());
    for (std::size_t b = 0; b < boards; ++b) {
        const Screen& s = shared.screens[b];
        const std::string base = folder + "nvram/board" + std::to_string(b);
        if (s.eeprom.size() == 64) {
            std::ofstream e(base + ".eeprom", std::ios::binary | std::ios::trunc);
            for (uint16_t w : s.eeprom) e.put(static_cast<char>(w >> 8)).put(static_cast<char>(w & 0xFF));
        }
        if (s.backup.size() == m3::Bus::kBackupRamSize) {
            std::ofstream r(base + ".backup", std::ios::binary | std::ios::trunc);
            r.write(reinterpret_cast<const char*>(s.backup.data()), static_cast<std::streamsize>(s.backup.size()));
        }
    }
}

// The effects' and the music's volumes (percent), under the overall one.
int effects_volume(const tools::Settings& s) { return s.sound_volume * s.volume / 100; }
int music_volume(const tools::Settings& s) { return s.music_volume * s.volume / 100; }

} // namespace

int main(int argc, char** argv) {
    const char* dir = nullptr;
    std::string record_path, load_state;
    int scale = 0, cli_render_scale = 0, cli_sound = -1, cli_music = -1;
    bool real_netboard = true;
    uint64_t max_frames = 0;
    bool mute = false, cli_quads = false, cli_smooth = false, cli_smooth_log = false, midi_burst = false, dma_instant = false, use_gpu = true, cli_soft_3d = false, fifo = false,
         sleep_pacing = false, list_audio = false;
    std::size_t boards = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--netboard" && i + 1 < argc) real_netboard = std::string(argv[++i]) == "real";
        else if (arg == "--frames" && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--boards" && i + 1 < argc)
            boards = std::clamp<std::size_t>(std::strtoull(argv[++i], nullptr, 0), 1, kMaxBoards);
        else if (arg == "--quads") cli_quads = true;
        else if (arg == "--smooth") cli_smooth = true;
        else if (arg == "--record" && i + 1 < argc) record_path = argv[++i];
        else if (arg == "--load-state" && i + 1 < argc) load_state = argv[++i];
        else if (arg == "--smooth-log") cli_smooth = cli_smooth_log = true;
        else if (arg == "--midi-burst") midi_burst = true;
        else if (arg == "--dma-instant") dma_instant = true;
        else if (arg == "--mute") mute = true;
        else if (arg == "--no-gpu") use_gpu = false;
        else if (arg == "--soft-3d") cli_soft_3d = true;
        else if (arg == "--fifo") fifo = true;
        else if (arg == "--sleep-pacing") sleep_pacing = true;
        else if (arg == "--list-audio") list_audio = true;
        else if (arg == "--render-scale" && i + 1 < argc) cli_render_scale = std::clamp(std::atoi(argv[++i]), 1, 6);
        else if (arg == "--sound-volume" && i + 1 < argc) cli_sound = std::clamp(std::atoi(argv[++i]), 0, 200);
        else if (arg == "--music-volume" && i + 1 < argc) cli_music = std::clamp(std::atoi(argv[++i]), 0, 200);
        else if (!dir) dir = argv[i];
    }
    if (list_audio) { // the sound outputs, with the channels each has (front and rear apart need 4)
        if (!SDL_Init(SDL_INIT_AUDIO)) return 1;
        int n = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
        for (int i = 0; i < n; ++i) {
            SDL_AudioSpec spec{};
            int frames = 0;
            const bool known = SDL_GetAudioDeviceFormat(ids[i], &spec, &frames);
            std::printf("%s: %d channels\n", SDL_GetAudioDeviceName(ids[i]), known ? spec.channels : 0);
        }
        SDL_free(ids);
        SDL_AudioSpec spec{};
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames))
            std::printf("(the default output: %d channels)\n", spec.channels);
        SDL_Quit();
        return 0;
    }
    if (!dir) {
        std::fprintf(stderr, "usage: spikeout <rom_dir> [--scale N] [--netboard sim|real] [--frames N] [--mute] [--quads]\n"
                             "                [--midi-burst] [--dma-instant] [--sound-volume N] [--music-volume N]\n"
                             "                [--boards N] [--no-gpu] [--soft-3d] [--render-scale N] [--fifo]\n"
                             "                [--sleep-pacing]\n"
                             "       spikeout --list-audio\n");
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
    if (cli_smooth) settings.smooth_motion = true;
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
        on_gpu = gpu_device->open(window, fifo) && presenter->open(*gpu_device);
        if (on_gpu) presenter->set_look(settings.crt_colours, settings.screen_filter);
        if (on_gpu)
            std::printf("spikeout: drawing with the GPU (%s, %s)\n", gpu_device->driver(),
                        gpu_device->mailbox() ? "mailbox" : "vsync");
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
    shared.record_path = record_path;
    shared.load_path = shared.last_state = load_state;
    shared.real_netboard = real_netboard;
    shared.quads = settings.quads;
    shared.midi_burst = midi_burst;
    shared.dma_instant = dma_instant;
    shared.sleep_pacing = sleep_pacing;
    std::unique_ptr<Sound> sound;
    auto speaker_setup = [&] { return SpeakerSetup{settings.speakers, settings.front_output, settings.rear_output}; };
    if (!mute) sound = std::make_unique<Sound>(roms, effects_volume(settings), music_volume(settings), speaker_setup());
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
    // Widescreen: the screen pixels the gameplay's 3D is opened out each side
    // for the picture shape (Settings::picture_shape; the window's: its shape,
    // from the arcade's up to 21:9). 0 for the arcade's, and without the GPU.
    auto wanted_extend = [&] {
        static constexpr double kShapes[] = {0, 0, 16.0 / 10.0, 16.0 / 9.0, 64.0 / 27.0};
        double shape = kShapes[std::clamp(settings.picture_shape, 0, 4)];
        if (settings.picture_shape == 1) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            shape = ph > 0 ? std::min(static_cast<double>(pw) / ph, 64.0 / 27.0) : 0;
        }
        const double extra = (shape * kHeight - kWidth) / 2;
        return shape > 0 && extra >= 1 ? static_cast<int>(std::lround(extra)) : 0;
    };
    // The game frame whose 3D the GPU last drew (0: none; draw it again). The
    // window shows each frame for one or more of the display's refreshes and
    // draws its 3D only once, for the first.
    uint64_t drawn_number = 0;
    auto set_up_3d = [&] {
        const bool want = on_gpu && boards == 1 && settings.gpu_3d;
        const int s = wanted_scale(), e = want ? wanted_extend() : 0;
        drawn_number = 0;
        shared.extend = e;
        if (want && real3d_gpu && s == draw_scale && e == real3d_gpu->extend()) {
            real3d_gpu->set_quads(settings.quads);
            return;
        }
        real3d_gpu.reset();
        draw_scale = 0;
        if (want) {
            real3d_gpu = std::make_unique<gpu::Real3dGpu>();
            real3d_gpu->set_quads(settings.quads);
            if (real3d_gpu->open(*gpu_device, s, e)) {
                draw_scale = s;
            } else {
                std::printf("spikeout: 3D in software (%s)\n", real3d_gpu->error().c_str());
                real3d_gpu.reset();
            }
        }
        shared.gpu_3d = real3d_gpu != nullptr;
        if (!real3d_gpu) shared.extend = 0;
    };
    set_up_3d();
    Shared::GpuFrame gpu_frame; // the frame the window shows
    // Smooth motion: the frame before it (its scene and number), when the
    // newest came, the display clock (in frames), the last picture's time,
    // and the scene between the two.
    struct Smooth {
        m3::Real3dScene before, between;
        uint64_t before_number = 0;
        // The sky (the layers below the 3D) as it moved from the frame before
        // (m3::TilegenRenderer::find_move): drawn moved back part of the way.
        std::vector<uint32_t> before_bottom;
        int move_x = 0, move_y = 0;
        std::chrono::steady_clock::time_point arrived{}, last_picture{}, due{};
        double clock = 0;
        double picture_seconds = 1.0 / 60; // the display's refresh, at most 120 a second
        double picture_average = 1.0 / 60; // the pictures' time apart, averaged: the clock's step
    } smooth;
    // Texture RAM as the game thread's rows have brought it (`textures`: some
    // came); a renderer made afresh takes it whole (its version differs), then
    // only the rows that change.
    std::vector<uint16_t> texture_copy(size_t{m3::Real3d::kTextureWidth} * m3::Real3d::kTextureHeight);
    bool textures = false;
    const uint64_t textures_version = 1;
    // Reads the GPU's 3D back and composes the screen (F12, the end of a test
    // run), at 496x384: drawn larger, every draw_scale-th pixel.
    auto capture = [&] {
        std::vector<uint32_t> drawn, three_d(static_cast<std::size_t>(kWidth) * kHeight);
        if (textures && real3d_gpu) real3d_gpu->render(gpu_frame.scene, texture_copy, textures_version, &drawn);
        const auto s = static_cast<std::size_t>(std::max(draw_scale, 1));
        // (widescreen: the arcade's screen from the middle)
        const auto e = static_cast<std::size_t>(real3d_gpu ? real3d_gpu->extend() : 0);
        const auto stride = (static_cast<std::size_t>(kWidth) + 2 * e) * s;
        for (std::size_t y = 0; y < static_cast<std::size_t>(kHeight) && !drawn.empty(); ++y)
            for (std::size_t x = 0; x < static_cast<std::size_t>(kWidth); ++x)
                three_d[y * kWidth + x] = drawn[y * s * stride + (x + e) * s];
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
    // The boards' EEPROM and backup RAM from the last run (not for test runs).
    const std::string data_folder = tools::data_folder();
    if (!test_run) load_nvram(data_folder, shared, boards);
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
    uint64_t fps_frames = 0, frames_shown = 0, pictures_shown = 0; // in the last second: frames run, shown, and (smooth motion) pictures
    // Tests: SPIKEOUT_SMOOTH_LOG=1 (or --smooth-log, into smooth_log.txt by
    // the program) prints smooth motion's pictures a second, how evenly its
    // clock went from one to the next, and what the pictures cost.
    std::FILE* smooth_log_file = stdout;
    if (cli_smooth_log)
        if (const char* base = SDL_GetBasePath())
            if (std::FILE* f = std::fopen((std::string(base) + "smooth_log.txt").c_str(), "w")) smooth_log_file = f;
    const bool smooth_log = cli_smooth_log || std::getenv("SPIKEOUT_SMOOTH_LOG") != nullptr;
    double smooth_log_clock = 0, smooth_log_sum = 0, smooth_log_sq = 0, smooth_log_draw = 0, smooth_log_present = 0,
           smooth_log_longest = 0;
    uint64_t smooth_log_n = 0, smooth_log_between = 0;
    // The arcade's test and service buttons, held for a moment from the menu.
    auto test_until = std::chrono::steady_clock::time_point{}, service_until = test_until;

    // A restart: the boards from power on, the settings kept.
    // (`while_off`: done with the boards switched off, before they start again.)
    auto reset_game = [&](const std::function<void()>& while_off = {}) {
        stop_games();
        if (while_off) while_off();
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
        drawn_number = 0;
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
        static constexpr const char* kShapeNames[] = {"Arcade", "The window's", "16:10", "16:9", "21:9"};
        p.items.push_back({"Picture shape",
                           "Arcade: the screen as the cabinet's. Wider shapes open the gameplay's 3D out to the "
                           "sides (the same height, as Supermodel's widescreen); the 2D (scores, text) stays in the "
                           "middle, its plain edges carried across. The game decides what's there for its own "
                           "screen, so things can appear late at the far sides.",
                           [&] { return std::string(kShapeNames[std::clamp(settings.picture_shape, 0, 4)]); },
                           [&](int d) {
                               settings.picture_shape = (settings.picture_shape + d + 5) % 5;
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
        p.items.push_back({"CRT colours",
                           "The colours as an arcade monitor showed them (Supermodel's CRT colours): Sega arcade is a "
                           "Japanese cabinet's, bluer whites and richer reds; then a Sony studio monitor and the TV "
                           "standards of Japan, the US and Europe. Off: the game's colours as they are.",
                           [&] { return std::string(gpu::Presenter::kCrtColourNames[settings.crt_colours]); },
                           [&](int d) {
                               settings.crt_colours = (settings.crt_colours + d + 6) % 6;
                               presenter->set_look(settings.crt_colours, settings.screen_filter);
                               changed();
                           }});
        p.items.push_back({"Screen filter",
                           "Scanlines: the dark lines between the arcade monitor's rows. CRT: those, its red, green "
                           "and blue stripes and slightly darker corners. The lines need a large window or "
                           "fullscreen to show.",
                           [&] { return std::string(gpu::Presenter::kFilterNames[settings.screen_filter]); },
                           [&](int d) {
                               settings.screen_filter = (settings.screen_filter + d + 3) % 3;
                               presenter->set_look(settings.crt_colours, settings.screen_filter);
                               changed();
                           }});
        p.items.push_back({"Show FPS",
                           "The frames shown a second, in the top right corner (the arcade's are 57.5).",
                           [&] { return on_off(settings.show_fps); },
                           [&](int) {
                               settings.show_fps = !settings.show_fps;
                               if (!settings.show_fps) menu.set_fps("");
                               changed();
                           }});
        p.items.push_back(back_item());
        return p;
    };
    auto sound_page = [&] {
        ui::MenuPage p{"Sound", "", {}};
        auto volume = [&](const char* label, const char* help, int& v, int most) {
            return ui::MenuItem{label, help, [&v] { return std::to_string(v) + "%"; },
                                [&, most](int d) {
                                    v = std::clamp(v + 10 * d, 0, most);
                                    if (sound) sound->set_volumes(effects_volume(settings), music_volume(settings));
                                    changed();
                                }};
        };
        p.items.push_back(volume("Volume", "Everything: the effects and the music together.", settings.volume, 100));
        p.items.push_back(
            volume("Sound effects", "Hits, voices and effects: the sound board's two SCSPs.", settings.sound_volume, 200));
        p.items.push_back(volume("Music", "The music board's tunes.", settings.music_volume, 200));
        // The outputs SDL has, by name ("" the default), and how many channels one has.
        auto outputs = [] {
            std::vector<std::string> names{""};
            int n = 0;
            SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
            for (int i = 0; i < n; ++i)
                if (const char* d = SDL_GetAudioDeviceName(ids[i])) names.emplace_back(d);
            SDL_free(ids);
            return names;
        };
        auto channels = [](const std::string& name) {
            SDL_AudioSpec spec{};
            int frames = 0;
            return SDL_GetAudioDeviceFormat(audio_device(name), &spec, &frames) ? spec.channels : 0;
        };
        auto reopen = [&] {
            if (sound) sound->open(speaker_setup());
            changed();
        };
        static constexpr const char* kSpeakerNames[] = {"Stereo", "Four channels", "Two outputs"};
        p.items.push_back(
            {"Speakers",
             "The cabinet's front pair (first sound chip, music) and rear pair (second chip). Stereo mixes them. Four "
             "channels: both pairs to one output, for quad, 5.1 or 7.1 speakers set up in your system (else mixed to stereo). "
             "Two outputs: each pair to an output of its own.",
             [&] {
                 std::string v = kSpeakerNames[settings.speakers];
                 if (settings.speakers == 1) {
                     const int c = channels(settings.front_output);
                     if (c > 0 && c < 4) v += " (" + std::to_string(c) + " here: mixed)";
                 }
                 return v;
             },
             [&](int d) {
                 settings.speakers = (settings.speakers + d + 3) % 3;
                 reopen();
                 menu.refresh();
             }});
        auto output_item = [&](const char* label, const char* help, std::string& chosen) {
            return ui::MenuItem{label, help,
                                [&chosen] { return chosen.empty() ? std::string("Default") : chosen; },
                                [&, outputs](int d) {
                                    const std::vector<std::string> names = outputs();
                                    std::size_t at = 0;
                                    for (std::size_t i = 0; i < names.size(); ++i)
                                        if (names[i] == chosen) at = i;
                                    const auto n = static_cast<int>(names.size());
                                    chosen = names[static_cast<std::size_t>((static_cast<int>(at) + d + n) % n)];
                                    if (settings.speakers != 0) reopen();
                                }};
        };
        p.items.push_back(output_item("Front output", "Where the front pair (or all four channels) goes.", settings.front_output));
        p.items.push_back(output_item("Rear output", "Where the rear pair goes, with front and rear on two outputs.",
                                      settings.rear_output));
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
    // The arcade's settings (tools::ArcadeSettings), edited in a copy of board
    // 0's EEPROM, read as the page opens (the game paused); a change goes to
    // every board's as the menu closes, and restarts the game, which reads
    // them as it starts.
    std::array<uint16_t, 64> arcade_words{};
    bool arcade_changed = false;
    auto arcade_page = [&] {
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            if (m3::Bus* b = shared.screens[0].bus)
                std::copy(b->eeprom().words().begin(), b->eeprom().words().end(), arcade_words.begin());
            else
                arcade_words.fill(0xFFFF);
        }
        const bool valid = tools::ArcadeSettings{arcade_words}.valid();
        ui::MenuPage p{"Arcade settings",
                       valid ? "A change restarts the game when the menu closes."
                             : "Not set up yet: the game sets them up as it first starts.",
                       {}};
        if (valid) {
            p.items.push_back({"Difficulty", "How hard the enemies fight (the test menu's GAME DIFFICULTY).",
                               [&] {
                                   const int d = tools::ArcadeSettings{arcade_words}.difficulty();
                                   return d >= 0 && d < 5 ? std::string(tools::ArcadeSettings::kDifficultyNames[static_cast<std::size_t>(d)])
                                                          : std::string("-");
                               },
                               [&](int d) {
                                   tools::ArcadeSettings s{arcade_words};
                                   const int now = s.difficulty();
                                   if (now < 0 || now > 4) return;
                                   s.set_difficulty(std::clamp(now + d, 0, 4));
                                   arcade_changed = true;
                               }});
            p.items.push_back({"Region",
                               "The arcade's country: Japan, U.S.A., Export (this ROM set's own) or Australia. The "
                               "game's own setting, which its test menu shows but doesn't let the operator change.",
                               [&] {
                                   const int c = tools::ArcadeSettings{arcade_words}.country();
                                   return c >= 1 && c <= 4 ? std::string(tools::ArcadeSettings::kCountryNames[static_cast<std::size_t>(c)])
                                                           : std::string("-");
                               },
                               [&](int d) {
                                   tools::ArcadeSettings s{arcade_words};
                                   const int now = s.country();
                                   if (now < 1 || now > 4) return;
                                   s.set_country(std::clamp(now + d, 1, 4));
                                   arcade_changed = true;
                               }});
            p.items.push_back({"Free play",
                               "Start and continue without coins: the arcade's own COIN/CREDIT SETTING #27. Off, "
                               "back to its default, a coin a credit.",
                               [&] { return on_off(tools::ArcadeSettings{arcade_words}.free_play()); },
                               [&](int) {
                                   tools::ArcadeSettings s{arcade_words};
                                   s.set_free_play(!s.free_play());
                                   arcade_changed = true;
                               }});
            p.items.push_back({"Demo sound", "Sound in the demo between games (the test menu's ADVERTISE SOUND).",
                               [&] { return on_off(tools::ArcadeSettings{arcade_words}.advertise_sound()); },
                               [&](int) {
                                   tools::ArcadeSettings s{arcade_words};
                                   s.set_advertise_sound(!s.advertise_sound());
                                   arcade_changed = true;
                               }});
        }
        p.items.push_back(back_item());
        return p;
    };
    // The settings into every board's EEPROM (the boards off), from the copy.
    auto apply_arcade_settings = [&] {
        const tools::ArcadeSettings from{arcade_words};
        for (std::size_t b = 0; b < boards; ++b) {
            Screen& s = shared.screens[b];
            if (s.eeprom.size() != 64) continue;
            tools::ArcadeSettings to{std::span<uint16_t, 64>(s.eeprom.data(), 64)};
            if (!to.valid()) continue;
            to.set_difficulty(from.difficulty());
            to.set_advertise_sound(from.advertise_sound());
            if (from.country() >= 1 && from.country() <= 4) to.set_country(from.country());
            if (from.free_play() != to.free_play()) to.set_free_play(from.free_play());
        }
        if (!test_run) save_nvram(data_folder, shared, boards);
    };
    auto game_page = [&] {
        ui::MenuPage p{"Game", "", {}};
        p.items.push_back({"Reset game", "Restarts the game, as switching the arcade off and on.", {}, {}, [&] {
                               menu.close();
                               reset_game();
                           }});
        p.items.push_back({"Arcade settings",
                           "The arcade's own settings: difficulty, region, free play and the demo's sound. A change restarts the "
                           "game when the menu closes.",
                           {}, {}, [&] { menu.push(arcade_page()); }});
        p.items.push_back({"Arcade test menu",
                           "Opens the arcade's own test menu: its settings, the network link, the tests. In it, "
                           "the service button (F1) moves and the test button (F2) chooses.",
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
        p.items.push_back({"Display", "Render scale, antialiasing, quads, the 3D renderer, CRT colours, screen filter, fullscreen.", {}, {},
                           [&] { menu.push(display_page()); }});
        p.items.push_back({"Sound", "The volumes, and the speakers: stereo, or the cabinet's front and rear apart.", {}, {}, [&] { menu.push(sound_page()); }});
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

    uint64_t frames_seen = 0;
    while (running) {
        // Each frame shown as soon as it is finished (and, with the game
        // paused or stopped, the menu and events 50 times a second: longer
        // than a frame, 17.4 ms).
        // Smooth motion: a picture each refresh of the display instead, its
        // time held to (slept to 1 ms before with SDL's precise delay, then
        // waited for: Windows' plain sleep wakes on its 15.6 ms tick, which
        // held the pictures to 64 a second on a 99 Hz screen).
#ifdef SPIKEOUT_GPU
        const bool smoothing = settings.smooth_motion && on_gpu && shared.gpu_3d && real3d_gpu;
#else
        const bool smoothing = false;
#endif
        if (smoothing) {
            using clock = std::chrono::steady_clock;
            // (each picture's time a refresh after the last's; fallen behind
            // by more than a refresh: from now)
            const auto period =
                std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(smooth.picture_seconds));
            smooth.due += period;
            if (smooth.due + period < clock::now()) smooth.due = clock::now();
            const auto due = smooth.due;
            const auto rest = due - clock::now() - std::chrono::milliseconds(1);
            if (rest > clock::duration::zero())
                SDL_DelayPrecise(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(rest).count()));
            while (clock::now() < due) std::this_thread::yield();
            std::lock_guard<std::mutex> guard(shared.lock);
            frames_seen = shared.frames_done;
        } else {
            std::unique_lock<std::mutex> guard(shared.lock);
            shared.frame_done.wait_for(guard, std::chrono::milliseconds(20),
                                       [&] { return shared.frames_done != frames_seen; });
            frames_seen = shared.frames_done;
        }
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
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F9 && !e.key.repeat) {
                if (boards == 1) {
                    shared.save_request = true;
                    std::printf("spikeout: saving at the end of the frame\n");
                } else {
                    std::printf("spikeout: save states are for one board\n");
                }
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F10 && !e.key.repeat && boards == 1) {
                std::string last;
                {
                    std::lock_guard<std::mutex> guard(shared.lock);
                    last = shared.last_state;
                }
                if (last.empty()) std::printf("spikeout: no save state yet (F9 saves one)\n");
                else reset_game([&] { shared.load_path = last; });
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode >= SDL_SCANCODE_F5 && e.key.scancode <= SDL_SCANCODE_F8) {
                const auto board = static_cast<std::size_t>(e.key.scancode - SDL_SCANCODE_F5);
                if (board < boards) focus = board;
            }
#ifdef SPIKEOUT_GPU
            // Auto render scale and the window's picture shape: the window's size decides them.
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && (settings.render_scale == 0 || settings.picture_shape == 1))
                set_up_3d();
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
            if (!menu_open && arcade_changed) {
                arcade_changed = false;
                shared.paused = false;
                reset_game(apply_arcade_settings);
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
                    if (b == 0) {
                        fps_frames += s.frame_number - shown_number[b];
                        ++frames_shown;
                    }
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
                const bool keep = settings.smooth_motion;
                if (keep) { // kept, to draw between it and the newest
                    smooth.before = gpu_frame.scene;
                    smooth.before_number = gpu_frame.number;
                    smooth.before_bottom = gpu_frame.bottom;
                    smooth.arrived = now;
                }
                const uint32_t sky_before = gpu_frame.tile_scroll[3];
                std::swap(gpu_frame, shared.gpu_frame);
                // The sky's move: layer 3 below the 3D, its scroll the register's (not a value a line).
                smooth.move_x = smooth.move_y = 0;
                const uint32_t sky = gpu_frame.tile_scroll[3];
                if (keep && smooth.before_number + 1 == gpu_frame.number && (sky & 0x80000000u) && !(sky & 0x8000) &&
                    !(gpu_frame.tile_control & 0x800))
                    m3::TilegenRenderer::find_move(smooth.before_bottom, gpu_frame.bottom, sky_before, sky, smooth.move_x,
                                                   smooth.move_y);
                for (const auto& r : shared.texture_rows) { // into the copy, and for the GPU
                    std::copy(r.texels.begin(), r.texels.end(),
                              texture_copy.begin() + static_cast<std::ptrdiff_t>(size_t{r.first} * m3::Real3d::kTextureWidth));
                    if (real3d_gpu) real3d_gpu->textures_changed(r.first, r.count);
                    textures = true;
                }
                shared.texture_rows.clear();
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
#ifdef SPIKEOUT_GPU
            // Show FPS: the frames the window showed (the title: the frames the game ran).
            // With smooth motion: the pictures, each between two frames.
            if (menu_ready) {
                char shown_rate[32] = "";
                const uint64_t shown_count = smoothing ? pictures_shown : frames_shown;
                if (settings.show_fps)
                    std::snprintf(shown_rate, sizeof shown_rate, "%.1f FPS", static_cast<double>(shown_count) / secs);
                menu.set_fps(shown_rate);
            }
            // The display's refresh (it may have moved to another): smooth
            // motion's pictures, at most 120 a second.
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
                smooth.picture_seconds = 1.0 / std::clamp(mode->refresh_rate > 0 ? static_cast<double>(mode->refresh_rate) : 60.0,
                                                          30.0, 120.0);
#endif
            if (smooth_log && smoothing && smooth_log_n > 1) {
                const double mean = smooth_log_sum / static_cast<double>(smooth_log_n);
                const double spread = std::sqrt(std::max(0.0, smooth_log_sq / static_cast<double>(smooth_log_n) - mean * mean));
                const double pictures = static_cast<double>(std::max<uint64_t>(pictures_shown, 1));
                std::fprintf(smooth_log_file, "smooth motion: %llu pictures (%llu between frames) a second; the clock %.3f frames a picture, "
                            "spread %.3f; a picture %.2f ms drawing the 3D, %.2f presenting, %.1f apart at most; the display "
                            "%.1f Hz; frames %llu; %zu polygons, render scale %d\n",
                            static_cast<unsigned long long>(pictures_shown), static_cast<unsigned long long>(smooth_log_between),
                            mean, spread, 1000.0 * smooth_log_draw / pictures, 1000.0 * smooth_log_present / pictures,
                            1000.0 * smooth_log_longest, 1.0 / smooth.picture_seconds,
                            static_cast<unsigned long long>(fps_frames), gpu_frame.scene.polygons.size(), draw_scale);
                std::fflush(smooth_log_file);
            }
            smooth_log_sum = smooth_log_sq = smooth_log_draw = smooth_log_present = smooth_log_longest = 0;
            smooth_log_n = smooth_log_between = 0;
            fps_start = now;
            fps_frames = 0;
            frames_shown = 0;
            pictures_shown = 0;
        }
#ifdef SPIKEOUT_GPU
        if (on_gpu && shared.gpu_3d && real3d_gpu) {
            if (textures && !gpu_frame.bottom.empty()) {
                bool between = false;
                float sky_shift_x = 0, sky_shift_y = 0;
                if (smoothing) {
                    // The display clock: the pictures' average time apart on
                    // (an even step: each picture's own time would carry its
                    // wake-up's wobble), pulled a tenth of the way to where the
                    // newest frame says it is (it shows, as it comes, the frame
                    // before; a frame on, itself). Off by more than 2 frames (a
                    // pause, a stall): taken whole.
                    const auto at = std::chrono::steady_clock::now();
                    constexpr double kFrame = 1.0 / recomp::FrameTiming::kRefreshHz;
                    const double newest = static_cast<double>(gpu_frame.number);
                    const double target = newest - 1 + std::chrono::duration<double>(at - smooth.arrived).count() / kFrame;
                    const double apart = std::chrono::duration<double>(at - smooth.last_picture).count();
                    if (smooth_log && apart < 1) smooth_log_longest = std::max(smooth_log_longest, apart);
                    if (apart < 3 * smooth.picture_seconds) smooth.picture_average += 0.05 * (apart - smooth.picture_average);
                    smooth.clock += smooth.picture_average / kFrame;
                    smooth.clock = std::fabs(smooth.clock - target) > 2 ? target : smooth.clock + 0.1 * (target - smooth.clock);
                    smooth.last_picture = at;
                    const double t = smooth.clock - (newest - 1);
                    if (smooth_log) { // tests: how evenly the clock went, picture to picture
                        const double step = smooth.clock - smooth_log_clock;
                        smooth_log_clock = smooth.clock;
                        if (step > 0 && step < 2) {
                            smooth_log_sum += step;
                            smooth_log_sq += step * step;
                            ++smooth_log_n;
                        }
                    }
                    if (smooth.before_number + 1 == gpu_frame.number && t < 1 && !gpu_frame.covered) {
                        between = m3::interpolate(smooth.before, gpu_frame.scene, static_cast<float>(std::max(t, 0.0)),
                                                  smooth.between);
                        if (between) {
                            real3d_gpu->render(smooth.between, texture_copy, textures_version);
                            drawn_number = 0; // the frame itself drawn again when it's shown
                            ++smooth_log_between;
                            // the sky where it was then: now's, back by the rest of the move
                            const double back = 1 - std::max(t, 0.0);
                            sky_shift_x = static_cast<float>(back * smooth.move_x);
                            sky_shift_y = static_cast<float>(back * smooth.move_y);
                        }
                    }
                    ++pictures_shown;
                }
                if (!between && gpu_frame.number != drawn_number) {
                    if (!gpu_frame.covered) real3d_gpu->render(gpu_frame.scene, texture_copy, textures_version);
                    drawn_number = gpu_frame.number;
                }
                const auto drawn_at = std::chrono::steady_clock::now();
                const int extend = real3d_gpu->extend();
                const bool wide = extend > 0 && extend == gpu_frame.extend &&
                                  gpu_frame.bottom_wide.size() == static_cast<std::size_t>(kWidth + 2 * extend) * kHeight;
                if (wide)
                    presenter->present_layers(gpu_frame.bottom_wide, gpu_frame.top_wide, real3d_gpu->display_image(), kWidth,
                                              kHeight, extend, sky_shift_x, sky_shift_y);
                else
                    presenter->present_layers(gpu_frame.bottom, gpu_frame.top, real3d_gpu->display_image(), kWidth, kHeight, 0,
                                              sky_shift_x, sky_shift_y);
                if (smooth_log && smoothing) {
                    const auto presented_at = std::chrono::steady_clock::now();
                    smooth_log_draw += std::chrono::duration<double>(drawn_at - smooth.last_picture).count();
                    smooth_log_present += std::chrono::duration<double>(presented_at - drawn_at).count();
                }
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
    if (!test_run) save_nvram(data_folder, shared, boards);
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
