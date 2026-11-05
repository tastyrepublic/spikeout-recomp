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
// Controls (player 1's keys or pad buttons), Game (reset, the arcade's test
// menu, a service credit) and Quit. Its choices are kept in settings.ini
// (tools/settings.hpp), written when the menu closes; options on the command
// line win over them for that run. Without a GPU (--no-gpu) there is no menu
// and Esc quits.
//
// Keys (as set in Controls):  player 1: 5 coin, 1 start, arrows, Z beat,
// X charge, C jump, V shift;  player 2: 6 coin, 2 start, I J K L, A beat,
// S charge, D jump, F shift. F2 test, F1 service, Tab (held) as fast as it
// goes, F12 a screenshot (spikeout-<frame>.png).
// Pads (as set in Controls; player 1 the one chosen there, else the first,
// player 2 the next): d-pad or left stick, A beat, X charge, B jump, Y shift,
// Start, Back coin. (The menu shows player 1's controls only; player 2's
// stay in settings.ini, for --boards 2.)
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
// The window draws through the GPU (Vulkan, Direct3D 12, Metal); without one
// it says so and quits (the owner's choice); --no-gpu draws through SDL's 2D
// renderer (developers: no menu then). On the GPU the
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
// the last one saved; --load-state FILE starts from FILE. In a network game
// F9 saves every cabinet together (spikeout-net-<frame>.state, as gamerun
// --save-state writes linked boards: gamerun --boards N --parallel
// --load-state FILE runs on from it); F10 doesn't load those.
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
// "quad rendering" option. --fog-over-nothing draws scroll fog in frames
// with no 3D too, as Supermodel (the ranking after the staff credits:
// orange). --midi-burst sends the sound board its bytes with
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
#include "game_thread.hpp"
#include "recording.hpp"
#include "png_write.hpp"
#include "arcade_settings.hpp"
#include "netplay.hpp"
#include "net/compress.hpp"
#include "net/update.hpp"
#include "one_board.hpp"
#include "rollback.hpp"
#include "settings.hpp"
#include "ui/assets.hpp" // (the app's icon)
#ifdef SPIKEOUT_JIT
#include "jit/jit.hpp" // (spikeout-jit: the game translated from the ROM set at the first start, LLVM)
#endif
#ifdef SPIKEOUT_GPU
#include "gpu/present.hpp"
#include "gpu/real3d_gpu.hpp"
#include "ui/menu.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // (the console: main)
#endif

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
#include <map>
#include <memory>
#include <mutex>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <set>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = m3::TilegenRenderer::kWidth, kHeight = m3::TilegenRenderer::kHeight;
constexpr std::size_t kMaxBoards = 4;
constexpr uint32_t kTurnLines = 8; // display lines a linked board runs before passing the turn on
// Rollback's time sync: frames ahead of another player before this machine slows a little, behind the one furthest
// on before it runs a little faster, and before it runs unpaced to catch up (a stall).
constexpr int64_t kSyncAhead = 4, kSyncBehind = 8, kSyncFar = 60;

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
    // (the owner's safeguards, 2026-11-04: each state's file says its format and the game code it was made by
    // (tools::state_file_bytes); the next save to `save_to` instead (the undo's, before a load); saves made so far; a short
    // notice at the bottom of the window (STATE SAVED, CAN'T LOAD: why), under `lock`)

    std::string save_to;
    std::atomic<uint64_t> saves_done{0};
    std::string notice;
    std::chrono::steady_clock::time_point notice_until{};
    void say(const std::string& text, int seconds = 3) {
        std::lock_guard<std::mutex> guard(lock);
        notice = text, notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    }
    bool net_save_key = false; // --net-save-key: F9 in network play saves every cabinet (spikeout-net-<frame>.state), for gamerun
    bool real_netboard = true;
    bool old_link = false; // --net-old-link: the link before the ring model (docs/fidelity.md)
    bool fog_over_nothing = false; // --fog-over-nothing: scroll fog with no 3D too, as Supermodel (docs/fidelity.md)
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
        uint64_t version = 0; // the same frame made again (paused, for a new picture shape): drawn again
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

    // Network play (Mode B, docs/netplay.md): every board of the session
    // runs here, at once (meeting every kTurnLines lines, as gamerun
    // --parallel), only this player's (`local`) drawn and heard; every
    // board's inputs come from the table, this player's sent to the others
    // `input_delay` frames ahead. The boards start as the host started them.
    tools::NetPlay* netplay = nullptr;
    tools::InputTable* inputs = nullptr;
    std::size_t local = 0;
    std::atomic<uint64_t> input_delay{0}; // (Auto: changed in play, read by the boards each frame)
    net::StartInfo net_start;
    // Every board's state: saved for a machine that asks (a player joining
    // the game in progress, or one out of step), or (`net_state`, unpacked)
    // the host's to run on from; then the boards run unpaced until they've
    // caught up with the others (and a cabinet joined in progress is taken).
    tools::LinkedState linked;
    std::atomic<bool> net_save_request{false}; // F9 in network play: every board's state in a file
    std::vector<uint8_t> net_state;
    std::atomic<bool> catching_up{false};
    std::atomic<uint64_t> catch_up_behind{0}; // (frames still to catch up, the shown board's; 0 not known yet)
    std::atomic<uint64_t> catch_up_start{0};  // (and as many when catching up started: the loading bar)
    // A spectator (docs/netplay.md, "Spectators"): no seat; every board run here on inputs all in (no one board, no
    // rollback), net::kWatchLag frames behind the players; the board shown (`local`) changed with the arrows or a pad's
    // shoulders. `watched`: the board threads' copy, each taking it at its next frame's start.
    bool spectating = false;
    std::atomic<std::size_t> watched{0};
    // On one board, another player chosen (docs/netplay.md, "Switching on one board"): every other cabinet made whole
    // (its player's backup run on here: the module's "every board again"), then the module set up again with that
    // player's board staying. ~0: none under way.
    std::atomic<std::size_t> switch_to{~std::size_t{0}};
    std::atomic<bool> switch_started{false};
    std::chrono::steady_clock::time_point switch_started_at{};
    // One board a PC (docs/netplay.md, "One board a PC in the window"): only this player's board
    // run whole here, the other cabinets made from their players' buttons (tools/one_board.hpp),
    // each run whole as a shadow where that can't follow it (held whole until every player is in
    // and the game is calm; a time over, a coin), a shadow from its machine's real board. Auto (the
    // default; --net-auto-board): every board, none of it, until this PC can't run them at full
    // speed; --net-one-board from the start; --net-every-board never. Each board registers its part
    // as it starts; the module is set up
    // where they meet, every board stopped. Nothing sent differs: the others needn't do the same.
    enum class OneBoard { automatic, always, never } one_board = OneBoard::automatic;
    // Rollback (--net-rollback N, tools::rollback): every board running up to N frames ahead on guessed inputs, going
    // back together when a guess was wrong (0: none, as delay-based play). The run's, while it runs.
    uint64_t rollback_frames = 0, rollback_wanted = 0; // (wanted: --net-rollback's; the run's what this machine's memory can keep)
    std::unique_ptr<tools::rollback::Linked> rollback;
    std::atomic<uint64_t> sync_slowed{0}, sync_sooner{0}, sync_free{0}; // (rollback's time sync: frames slowed, sooner, unpaced)
    std::atomic<uint64_t> rollback_from{0}; // (rollback's snapshots and guesses from this frame on: a joiner's, once caught up)
    std::unique_ptr<tools::one_board::Game> one_board_game; // (this run's, else none)
    std::array<tools::one_board::Board*, kMaxBoards> one_board_parts{};
    std::atomic<bool> one_board_wanted{false};            // (asked: set up at the next meeting)
    std::atomic<bool> one_board_back{false};              // (every board again: the module gone at a meeting, once it can be)
    m3::LinkRing* ring = nullptr;                         // (the run's: a board's packets counted with its state)
    std::atomic<uint64_t> one_board_frame{~0ull};         // the hand-over (~0: none yet)
    // Leaving a game this machine runs one board of: the others told of a frame half a second ahead, this board's
    // state at its end sent to each (they run the cabinet on whole from it), then the goodbye.
    std::atomic<bool> parting_wanted{false}, parted{false};
    uint64_t parting_frame = 0;
    bool state_pending = false; // (every board's state asked for: once every board runs whole here)
    uint64_t whole_until = 0;
    // This board's backups on the other machines (one board here: tools::backup), the seats sent one (changes from it
    // next); and the game's ROMs (a board whose player went run on from its backup).
    tools::backup::Maker backups;
    std::set<uint8_t> backup_bases;
    const m3::RomSet* roms = nullptr;
};

// The input bits a player's actions are (bank 0: coin, start; the controls byte).
struct InputBit {
    bool bank0;
    int bit;
};
constexpr std::array<InputBit, tools::kActions> kActionBits = {{
    {true, 0}, {true, 4}, {false, 5}, {false, 4}, {false, 7}, {false, 6}, {false, 0}, {false, 1}, {false, 3}, {false, 2},
}};

// An axis past this counts as pressed (a stick's direction, a trigger).
constexpr int kAxisPressed = 16000;

// A player's pad: which of the actions it holds, by their buttons (tools::Settings::pad_buttons).
// The left stick moves too, unless one of the actions is set to one of its axes.
std::array<bool, tools::kActions> pad_actions(SDL_Gamepad* pad, const std::array<int, tools::kActions>& buttons) {
    std::array<bool, tools::kActions> held{};
    if (!pad) return held;
    bool stick_used = false;
    for (std::size_t a = 0; a < tools::kActions; ++a) {
        const int code = buttons[a];
        if (code >= tools::kPadAxis) {
            const auto axis = static_cast<SDL_GamepadAxis>((code - tools::kPadAxis) / 2);
            const int v = SDL_GetGamepadAxis(pad, axis);
            held[a] = (code - tools::kPadAxis) % 2 ? v > kAxisPressed : v < -kAxisPressed;
            stick_used = stick_used || axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY;
        } else if (code > 0) {
            held[a] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(code - 1));
        }
    }
    if (!stick_used) {
        const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        using A = tools::Action;
        auto set = [&](A a, bool on) { held[static_cast<std::size_t>(a)] = held[static_cast<std::size_t>(a)] || on; };
        set(A::Up, y < -kAxisPressed);
        set(A::Down, y > kAxisPressed);
        set(A::Left, x < -kAxisPressed);
        set(A::Right, x > kAxisPressed);
    }
    return held;
}

// A pad input's name in the menu (a pad given: its face buttons as that pad labels them, A or Cross, ...).
std::string pad_input_name(int code, SDL_Gamepad* pad) {
    if (code <= 0) return "None";
    if (code >= tools::kPadAxis) {
        const int axis = (code - tools::kPadAxis) / 2;
        const bool positive = (code - tools::kPadAxis) % 2;
        static constexpr const char* kAxes[] = {"Left stick", "Left stick", "Right stick", "Right stick", "LT", "RT"};
        static constexpr const char* kDirs[] = {"left", "right", "up", "down", "left", "right", "up", "down"};
        if (axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return axis < 6 ? kAxes[axis] : "Axis " + std::to_string(axis);
        return std::string(kAxes[axis]) + " " + kDirs[axis * 2 + (positive ? 1 : 0)];
    }
    const auto b = static_cast<SDL_GamepadButton>(code - 1);
    if (b <= SDL_GAMEPAD_BUTTON_NORTH) {
        const SDL_GamepadButtonLabel label =
            pad ? SDL_GetGamepadButtonLabel(pad, b) : SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_XBOX360, b);
        switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: break;
        }
    }
    static constexpr const char* kNames[] = {"South",       "East",     "West",      "North",        "Back",
                                             "Guide",       "Start",    "Left stick press", "Right stick press", "LB",
                                             "RB",          "D-pad up", "D-pad down", "D-pad left",  "D-pad right",
                                             "Misc",        "Right paddle 1", "Left paddle 1", "Right paddle 2", "Left paddle 2",
                                             "Touchpad"};
    const auto i = static_cast<std::size_t>(b);
    return i < std::size(kNames) ? kNames[i] : "Button " + std::to_string(i);
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
        const auto held = pad_actions(pads[player], settings.pad_buttons[player]);
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
    // What the music board plays (SPIKEOUT_NET_DEBUG's reports).
    std::optional<m3::Dsb2::NowPlaying> music_now() {
        std::lock_guard<std::mutex> guard(lock_);
        if (const m3::Dsb2* dsb = board_.music()) return dsb->now_playing();
        return std::nullopt;
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
// Tests (SPIKEOUT_TEST_PRESS): a button held from a frame, as gamerun --press holds it.
struct TestPress {
    bool bank0 = false; // (else the controls byte)
    int bit = 0;
    uint64_t frame = 0, frames = 6;
};
std::vector<TestPress> parse_test_presses(const char* list) {
    static const struct {
        const char* name;
        bool bank0;
        int bit;
    } kButtons[] = {{"coin", true, 0},   {"start", true, 4},  {"beat", false, 0}, {"charge", false, 1},
                    {"shift", false, 2}, {"jump", false, 3},  {"down", false, 4}, {"up", false, 5},
                    {"right", false, 6}, {"left", false, 7}};
    std::vector<TestPress> out;
    if (!list) return out;
    const std::string text = list;
    for (size_t at = 0; at < text.size();) {
        const size_t comma = std::min(text.find(',', at), text.size());
        const std::string one = text.substr(at, comma - at);
        at = comma + 1;
        const size_t sign = one.find('@');
        if (sign == std::string::npos) continue;
        TestPress p;
        const std::string name = one.substr(0, sign);
        const auto button = std::find_if(std::begin(kButtons), std::end(kButtons), [&](const auto& b) { return name == b.name; });
        if (button == std::end(kButtons)) continue;
        p.bank0 = button->bank0, p.bit = button->bit;
        char* end = nullptr;
        p.frame = std::strtoull(one.c_str() + sign + 1, &end, 0);
        if (end && *end == ':') p.frames = std::strtoull(end + 1, nullptr, 0);
        out.push_back(p);
    }
    return out;
}

void run_game(const m3::RomSet& roms, std::size_t index, m3::LinkRing* ring, tools::Turns& turns, Shared& shared) {
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();
    Screen& out = shared.screens[index];
    m3::Bus bus(roms);
    bus.set_security_key(game.security_key);
    const int64_t clock_start = tools::set_rtc_to_host(bus);
    tools::RecordingWriter recording;
    // Network play: every machine's boards the same from power-on (the
    // host's clock, settings and backup RAM); only this player's board drawn and heard.
    const bool networked = shared.netplay != nullptr;
    std::size_t shown = networked ? shared.local : 0; // the board the window shows, and plays (a spectator: changes)
    bool pictured = !networked || index == shown;
    bool came_into_view = false; // (a spectator's: this board shown from this frame: texture RAM taken afresh)
    {
        std::lock_guard<std::mutex> guard(shared.lock);
        if (out.eeprom.size() == 64) std::copy(out.eeprom.begin(), out.eeprom.end(), bus.eeprom().words().begin());
        if (out.backup.size() == m3::Bus::kBackupRamSize) std::copy(out.backup.begin(), out.backup.end(), bus.backup_ram().begin());
        if (networked) { // each board from its player's own cabinet (the host's country)
            const net::StartInfo& start = shared.net_start;
            bus.rtc().set_time(start.clock, 0);
            const std::vector<uint8_t> eeprom = tools::seat_eeprom(start, index);
            const auto words = tools::eeprom_words(eeprom);
            if (eeprom.size() == 128) std::copy(words.begin(), words.end(), bus.eeprom().words().begin());
            else bus.eeprom().erase_all(); // (a cabinet with none (gamerun's): a blank chip, as a fresh board has it, and gamerun)
            if (index < start.backup.size() && start.backup[index].size() == m3::Bus::kBackupRamSize)
                std::copy(start.backup[index].begin(), start.backup[index].end(), bus.backup_ram().begin());
            else
                std::fill(bus.backup_ram().begin(), bus.backup_ram().end(), uint8_t{0});
        }
        out.bus = &bus;
        if (index == 0 && !networked && !shared.record_path.empty()) {
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
    if (shared.real_netboard) {
        m3::CommBoardOptions options;
        if (shared.old_link) options.use_old_link();
        bus.use_netboard(std::make_unique<m3::CommBoard>(ring ? ring->port(index) : nullptr, options));
    }
    else if (ring) bus.use_netboard(std::make_unique<m3::SimNetBoard>(ring->port(index)));
    bus.midi_burst = shared.midi_burst;
    bus.dma_timed = !shared.dma_instant;
    const bool linked = ring != nullptr;
    const uint8_t link_setting = index == 0 ? 1 : 2; // master, slave

    recomp::Context c(bus, functions);
    const std::vector<recomp::FunctionEntry> blocks = tools::game_blocks();
    c.set_blocks(blocks);
    m3::games::install_spikeout_hooks(c);
    recomp::FrameTiming timing;
    c.set_scheduler(&timing);
    const std::function<void(m3::State&)> sound_state =
        shared.sound && index == shown ? std::function<void(m3::State&)>([&](m3::State& s) { shared.sound->state(s, c.cycles()); })
                                   : std::function<void(m3::State&)>();
    bool loaded = false;    // from a save state: the run resumes there
    std::map<uint64_t, uint64_t> hash_pending; // (rollback: RAM hashes for the others, once their frames are settled)
    uint64_t catch_up_frame = 0; // (run on from a state: where catching up started, and when)
    std::chrono::steady_clock::time_point catch_up_at{};
    uint64_t catch_up_start_behind = 0; // (frames behind then)
    std::string load_error; // a state that doesn't fit stops the board, saying why
    if (index == 0 && !networked) {
        std::string path;
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            path.swap(shared.load_path);
        }
        if (!path.empty()) {
            try {
                tools::StateFile f = tools::read_state_file(tools::read_file(path));
                const bool other = !f.matches(); // (another version's, or from before the header: tried, said)
                m3::State s(std::move(f.state));
                tools::board_state(s, c, timing, sound_state);
                loaded = true;
                std::printf("spikeout: loaded %s (frame %llu)%s\n", path.c_str(), static_cast<unsigned long long>(timing.frames()),
                            other ? " (a save from another version of the game, or from before saves said theirs: tried)" : "");
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
    // Network play: every board as the host had them (a game joined in
    // progress, or ours put back in step).
    if (networked && index < shared.linked.loaded.size() && !shared.linked.loaded[index].empty()) {
        try {
            m3::State s(std::move(shared.linked.loaded[index]));
            tools::board_state(s, c, timing, sound_state);
            loaded = true;
        } catch (const std::exception& e) {
            load_error = std::string("the host's state: ") + e.what();
            std::printf("spikeout: can't run on from %s\n", load_error.c_str());
        }
    }
    // One board a PC: this board's part, registered (set up at a meeting, when it's wanted: set_up_one_board).
    tools::one_board::Board part(c, bus, timing, index);
    part.inputs = shared.inputs;
    tools::one_board::Game* const one_board = shared.one_board_game.get();
    if (one_board) {
        std::lock_guard guard(shared.lock);
        shared.one_board_parts[index] = &part;
    }
    const bool goes = one_board && index != shown; // (handed over at shared.one_board_frame, if it comes)
    const uint64_t auto_from = timing.frames(); // (Auto: this board's frames timed from 600 on)
    double auto_ms = 0, auto_every_ms = 0; // (and every board's, when one board was taken for it)
    uint64_t auto_frames = 0;
    bool auto_decided = false; // (one board tried: kept, or every board again)
    m3::TilegenRenderer tilegen;
    m3::Real3dRenderer real3d;
    real3d.set_fog_over_nothing(shared.fog_over_nothing);
    std::vector<uint32_t> screen(kWidth * kHeight);

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    uint64_t paced_from = timing.frames(); // frame count at `start` (a save state's, loaded)
    static const bool meet_timing = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr; // (its reports, below)
    double meet_ms = 0, pass_ms = 0, side_ms_before = 0;
    timing.on_display_line = [&](uint32_t line) {
        if (line == 0 && shared.spectating && shared.watched != shown) { // (a spectator's view changed: from this frame)
            shown = shared.watched;
            const bool now = index == shown;
            came_into_view = now && !pictured;
            pictured = now;
        }
        if (pictured && !shared.catching_up && !(shared.rollback && shared.rollback->quiet(index, timing.frames() + 1))) { // (a board not shown needn't draw its 2D: the game never reads it back)
            if (line == 0) tilegen.begin_frame();
            tilegen.draw_line(bus, static_cast<int>(line));
        }
        if (linked && line % kTurnLines == kTurnLines - 1) {
            bus.netboard().sync(c.cycles());
            const auto met = meet_timing ? clock::now() : clock::time_point{};
            tools::one_board::Game::meet(part, line); // (--net-one-board: the other cabinet's blocks due)
            const auto passing = meet_timing ? clock::now() : clock::time_point{};
            turns.pass(index);
            if (meet_timing) // (SPIKEOUT_NET_DEBUG: the module's link runs at the meetings, and the meetings' waits)
                meet_ms += std::chrono::duration<double, std::milli>(passing - met).count(),
                    pass_ms += std::chrono::duration<double, std::milli>(clock::now() - passing).count();
            if (shared.rollback && shared.rollback->rewind(index)) throw tools::rollback::Rewound{}; // (gone back, with the others)
        }
    };
    // Board 0 paces them all to the Model 3's refresh: it holds each finished
    // frame until it is due, so frames reach the window evenly spaced however
    // long each took to run. (Tab runs free; after it, or the menu, pacing
    // starts again from there.)
    bool repace = false;
    auto pace = [&] {
        if (index != shown) return;
        if (shared.fast || shared.catching_up || repace) { // (catching up: run on from a state, behind the others)
            start = clock::now();
            paced_from = timing.frames();
            repace = shared.fast || shared.catching_up;
            return;
        }
        // Rollback's time sync: ahead of another player by more than a few frames (as their inputs show): this frame
        // a little later (more the further ahead: the game at 50-95% until they are close); well behind the one furthest on:
        // a little sooner (the game at up to 110%), or, far behind, not held to the pace (catching up after a stall).
        // Without it a machine runs on ahead of a slower or stalled one
        // as far as the window, and every input of the other's then takes it back that far.
        if (shared.rollback && shared.netplay) {
            const auto lead = shared.netplay->lead(shown);
            if (std::getenv("SPIKEOUT_NET_DEBUG") && timing.frames() % 30 == 0) // (SPIKEOUT_NET_DEBUG: the lead it goes by)
                std::printf("spikeout: frame %llu: ahead %lld, behind %lld\n", static_cast<unsigned long long>(timing.frames()),
                            static_cast<long long>(lead.ahead), static_cast<long long>(lead.behind));
            // (behind: this frame a little earlier, a fiftieth of a frame for each frame beyond, up to a tenth (the game's
            // sound commands come that much faster: run unpaced, the music raced); far behind (a stall): unpaced)
            if (lead.behind < -kSyncFar) {
                ++shared.sync_free;
                start = clock::now();
                paced_from = timing.frames();
                return;
            }
            if (lead.behind < -kSyncBehind) {
                ++shared.sync_sooner;
                const double sooner = std::min(0.1, 0.02 * static_cast<double>(-lead.behind - kSyncBehind));
                start -= std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(sooner / recomp::FrameTiming::kRefreshHz));
            }
            if (lead.ahead > kSyncAhead) { // (the further ahead, the more: a twentieth of a frame for each frame, up to half)
                ++shared.sync_slowed;
                const double later = std::min(0.5, 0.05 * static_cast<double>(lead.ahead - kSyncAhead + 1));
                start += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(later / recomp::FrameTiming::kRefreshHz));
            }
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
    builder.set_fog_over_nothing(shared.fog_over_nothing);
    m3::WideLayer wide_bottom, wide_top;
    Shared::GpuFrame gpu_frame;
    int built_extend = 0;  // the widescreen extension the last frame was made with (asked for)
    uint64_t rebuilt = 0;  // frames made again, paused (GpuFrame::version)
    // The frame for the window: its scene and 2D layers (widescreen: made as wide).
    auto build_gpu_frame = [&] {
        built_extend = shared.extend;
        builder.set_extend(built_extend);
        builder.build(bus.real3d(), gpu_frame.scene);
        gpu_frame.extend = builder.extend();
        gpu_frame.bottom.assign(tilegen.bottom().begin(), tilegen.bottom().end());
        gpu_frame.top.assign(tilegen.top().begin(), tilegen.top().end());
        if (gpu_frame.extend > 0) {
            const bool drawn = tilegen.extend() == gpu_frame.extend; // the tile generator's own sides
            wide_bottom.build(tilegen.bottom(), drawn ? tilegen.bottom_side() : std::span<const uint32_t>(),
                              gpu_frame.extend, gpu_frame.bottom_wide, false,
                              drawn ? tilegen.bottom_own() : std::span<const uint32_t>(),
                              drawn ? tilegen.bottom_real() : std::span<const uint8_t>());
            wide_top.build(tilegen.top(), drawn ? tilegen.top_side() : std::span<const uint32_t>(), gpu_frame.extend,
                           gpu_frame.top_wide, true, drawn ? tilegen.top_own() : std::span<const uint32_t>(),
                           drawn ? tilegen.top_real() : std::span<const uint8_t>());
        }
        tilegen.set_extend(gpu_frame.extend);
        {
            const std::vector<uint32_t>& above = gpu_frame.extend > 0 ? gpu_frame.top_wide : gpu_frame.top;
            gpu_frame.covered = !above.empty() && std::all_of(above.begin(), above.end(), [](uint32_t p) { return (p >> 24) != 0; });
        }
        gpu_frame.number = timing.frames();
        gpu_frame.version = rebuilt;
        gpu_frame.tile_control = bus.tile_reg(0x20);
        for (uint32_t n = 0; n < 4; ++n) gpu_frame.tile_scroll[n] = bus.tile_reg(0x60 + 4 * n);
    };
    bool textures_whole = true; // the next frame sends all of texture RAM
    bool inputs_started = false; // (network play: the frames before the first, with an input delay, set to none)
    // Tests: SPIKEOUT_NET_DEBUG=1 prints, every 300 frames, how long the
    // shown board's frames took to run (from the end of one frame's waits
    // to the end of the next frame), and its waits for the inputs.
    static const bool timing_debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
    auto run_started = clock::now(), frame_ended = clock::now();
    double run_ms = 0, input_ms = 0, frame_ms = 0, end_ms = 0;
    timing.on_frame_end = [&] {
        // (rollback: a frame run again after going back, drawn and heard nothing: shown and heard the first time)
        const bool quiet = shared.rollback && shared.rollback->quiet(index, timing.frames());
        // (and catching up with the others, run on from a state: nobody looks; the sound still plays, so the music goes on)
        const bool unseen = quiet || shared.catching_up;
        tools::one_board::Game::end_frame(part); // (one board: the other cabinet's frame ends with this board's)
        // One board, Auto: this board's frames timed (from the end of one frame's waits to the end of the next: its
        // own work and its meetings with the others, not the waits for inputs or the pacing), 300 at a time; over
        // 97% of a frame on average, this PC can't run every board at full speed: one board from the next meeting.
        // Then, once the other cabinet is made by side runs, timed again: one board kept only if it is 5% faster than
        // every board was, else the other cabinet run whole again (a shadow never handed back) for the rest of the
        // game (a PC with cores to spare runs every board faster; one busy with something else, neither helps).
        // (Not while running free (Tab) or catching up, nor in the first 600 frames: the boot's loads.)
        // (a spectator on one board switching players: every other cabinet whole first, from their backups; set up
        // again around the new one once the module is gone, in the turns' meeting)
        if (one_board && index == shown && shared.switch_to != ~std::size_t{0} && !shared.switch_started &&
            shared.one_board_frame != ~0ull) {
            shared.switch_started = true;
            shared.switch_started_at = std::chrono::steady_clock::now();
            std::printf("spikeout: switching to seat %zu: every cabinet whole here first (from their backups)\n",
                        shared.switch_to.load() + 1);
            tools::one_board::Game::keep_whole(part), shared.one_board_back = true;
        }
        if (one_board && index == shown && shared.one_board == Shared::OneBoard::automatic && !auto_decided) {
            const bool switched = shared.one_board_frame != ~0ull;
            const bool timed = switched ? tools::one_board::Game::on_side_runs(part) : !shared.one_board_wanted;
            if (!timed || shared.fast || shared.catching_up || timing.frames() < auto_from + 600) {
                auto_ms = 0, auto_frames = 0;
            } else {
                auto_ms += std::chrono::duration<double, std::milli>(clock::now() - run_started).count();
                if (++auto_frames == 300) {
                    const double budget = 1000.0 / recomp::FrameTiming::kRefreshHz, ms = auto_ms / 300;
                    // (tests: SPIKEOUT_TEST_AUTO_MS=N, one board once frames take over N ms: a PC too slow, played)
                    static const char* test_ms = std::getenv("SPIKEOUT_TEST_AUTO_MS");
                    if (!switched && ms > (test_ms ? std::atof(test_ms) : 0.97 * budget)) {
                        std::printf("spikeout: every board takes %.2f ms a frame here (a frame is %.2f): one board from now\n", ms,
                                    budget);
                        auto_every_ms = ms;
                        shared.one_board_wanted = true;
                    } else if (switched) {
                        auto_decided = true;
                        const bool keep = ms < 0.95 * auto_every_ms;
                        std::printf("spikeout: one board takes %.2f ms a frame here (every board %.2f): %s\n", ms, auto_every_ms,
                                    keep ? "kept" : "every board again, for the rest of the game");
                        if (!keep) tools::one_board::Game::keep_whole(part), shared.one_board_back = true;
                    }
                    auto_ms = 0, auto_frames = 0;
                }
            }
        }
        if (timing_debug && timing.frames() % 300 == 0) {
            const uint32_t logic = bus.read32(0x1000A8);
            static thread_local uint32_t logic_before2 = 0;
            std::printf("spikeout: board %zu at %llu: game logic +%u; inputs %02x %02x %02x %02x; %s\n", index,
                        static_cast<unsigned long long>(timing.frames()), logic - logic_before2, bus.inputs.bank0, bus.inputs.bank1,
                        bus.inputs.controls, bus.inputs.controls2, bus.netboard().describe().c_str());
            logic_before2 = logic;
            std::fflush(stdout);
        }
        if (timing_debug && index == shown) {
            const auto now = clock::now();
            run_ms += std::chrono::duration<double, std::milli>(now - run_started).count();
            frame_ms += std::chrono::duration<double, std::milli>(now - frame_ended).count();
            frame_ended = now;
            if (timing.frames() % 300 == 0) {
                // (and the game's own logic counter: 300 more a 300 frames at full speed; docs/fidelity.md)
                const uint32_t logic = bus.read32(0x1000A8);
                static thread_local uint32_t logic_before = 0;
                const double side_ms = part.cabinets ? 1000 * part.cabinets->whole_seconds() - side_ms_before : 0;
                side_ms_before += side_ms;
                std::printf("spikeout: board %zu, frames to %llu: %.2f ms a frame (%.2f running, %.2f its end, %.2f waiting for "
                            "inputs; of the running %.2f side runs, %.2f at meetings, %.2f waiting at them), game logic +%u\n",
                            index, static_cast<unsigned long long>(timing.frames()), frame_ms / 300, run_ms / 300, end_ms / 300,
                            input_ms / 300, side_ms / 300, meet_ms / 300, pass_ms / 300, logic - logic_before);
                meet_ms = pass_ms = 0;
                logic_before = logic;
                if (shared.sound)
                    if (const auto m = shared.sound->music_now())
                        std::printf("spikeout: board %zu at %llu: music %s %06x-%06x at %06x, volume %02x %02x\n", index,
                                    static_cast<unsigned long long>(timing.frames()), m->playing ? "playing" : "stopped", m->start,
                                    m->end, m->pos, m->volume_left, m->volume_right);
                std::fflush(stdout);
                run_ms = input_ms = frame_ms = end_ms = 0;
            }
        }
        if (came_into_view) textures_whole = true, came_into_view = false; // (a spectator's: another board's textures before)
        if (unseen) {
            textures_whole = true; // (texture RAM put back as it was: the GPU takes it afresh)
        } else if (!pictured) {
            pace(); // (no picture: another player's board)
        } else if (shared.gpu_3d && index == shown) { // the scene and layers for the window to draw
            const m3::Real3d& mem = bus.real3d();
            build_gpu_frame();
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
        if (pictured && !unseen) { // (network play: only the shown board; another's frame would wake the window for nothing to show)
            {
                std::lock_guard<std::mutex> guard(shared.lock);
                ++shared.frames_done;
            }
            shared.frame_done.notify_one();
        }
        if (shared.sound && index == shown && !quiet) shared.sound->send(bus.midi_out());
        else bus.midi_out().clear();
        if (linked && timing.frames() < tools::kLinkSettingFrames) bus.write8(tools::kLinkSettingAddr, link_setting);
        // The menu: held here, between frames, until it closes. The picture
        // shape changed meanwhile: this frame made again for it (its 3D from
        // the Real3D's memory as it is, its 2D's sides from the layers; those
        // the tile generator draws itself come with the next frame).
        while (shared.paused && !networked && !shared.quit) { // (network play goes on: the others play)
            repace = true;
            if (shared.gpu_3d && index == shown && shared.extend != built_extend) {
                build_gpu_frame();
                gpu_frame.version = ++rebuilt;
                std::lock_guard<std::mutex> guard(shared.lock);
                std::swap(shared.gpu_frame, gpu_frame);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (networked) { // this player's inputs in (and out), then every board's for the next frame from the table
            const uint64_t frame = timing.frames();
            // Every board's state, asked for: saved at the end of a frame
            // every board is yet to finish (the last to save sends it: start_games).
            // One board a PC: another machine's shadow of this player's board wants its state at the end of this frame
            // (taken where a save state is, without the sound board; sent compressed, with its packets' counts).
            // This board's backup on every other machine now and then, for one board a PC's (any of them may run this
            // board on side runs): the pages changed since the last, or whole to a machine with none of it; packed and
            // sent off this thread (tools::backup).
            // (at the snapshot point; a shadow's request too: the backups first)
            const auto backup_now = [&](uint64_t at) {
                if (shared.backups.frame() == at) return; // (taken already: the backups' turn this frame)
                for (const uint8_t s : shared.netplay->take_whole_wanted()) shared.backup_bases.erase(s);
                m3::LinkRing* r = shared.ring;
                shared.backups.take(at, [&](m3::State& st) { tools::board_state(st, c, timing, {}); }, r ? r->taken(index) : 0,
                                    r ? r->sent(index) : 0);
                if (r) r->abandon_cut(index); // (one board's state alone: no cut under way)
                std::vector<uint8_t> to_changes, to_whole;
                std::vector<uint8_t> to = shared.netplay->session().backup_watchers(); // (spectators on one board too)
                for (uint8_t s = 0; s < shared.boards; ++s) // (a player still joining: none yet, Session::joining)
                    if (s == index) continue;
                    else if (shared.netplay->session().joining(s)) shared.backup_bases.erase(s); // (whole once seated)
                    else to.push_back(s);
                for (const uint8_t s : to) {
                    (shared.backups.changes_possible() && shared.backup_bases.count(s) ? to_changes : to_whole).push_back(s);
                    shared.backup_bases.insert(s);
                }
                if (!to_changes.empty()) shared.netplay->send_backup(at, shared.backups.changes(), std::move(to_changes));
                if (!to_whole.empty()) shared.netplay->send_backup(at, shared.backups.whole(), std::move(to_whole));
            };
            if (index == shown && shared.real_netboard && frame >= shared.backups.frame() + tools::backup::kEvery && shared.netplay->session().seated() &&
                (!shared.rollback || (shared.inputs->all_set(frame, shared.boards) && shared.inputs->wrong_from() > frame))) // (rollback: a settled frame's)
                c.request_snapshot([backup_now, frame](uint32_t) { backup_now(frame); });
            // Leaving (Shared::parting_wanted): the others told of a frame a little ahead; at its end, this board's state
            // to every one of them.
            if (index == shown && one_board && shared.parting_wanted && !shared.parting_frame) {
                shared.parting_frame = frame + tools::one_board::Game::kAskAhead;
                shared.netplay->session().announce_parting(shared.parting_frame);
                std::printf("spikeout: leaving after frame %llu\n", static_cast<unsigned long long>(shared.parting_frame));
            }
            if (index == shown) {
                std::vector<uint8_t> seats = shared.netplay->take_board_requests(frame);
                const bool parting = shared.parting_frame && frame == shared.parting_frame;
                if (parting)
                    for (uint8_t s = 0; s < shared.boards; ++s)
                        if (s != index && std::find(seats.begin(), seats.end(), s) == seats.end()) seats.push_back(s);
                if (!seats.empty()) // (one state for all who asked)
                    c.request_snapshot([&, seats, frame, parting, backup_now](uint32_t) {
                        // (rollback: only a frame that won't be run again: the inputs it ran on waited for (those up
                        // to the frame before: a frame's are set at its end, for the next; the machine that asked sets
                        // its own of this frame only once its shadow is loaded); one guessed wrong, asked again and
                        // taken when the frame is run again, from the next meeting)
                        // (nor while a shadow here waits for a state of a frame before it, come late: every board goes
                        // back there, this frame run again with that cabinet whole)
                        if (shared.rollback && !parting && frame &&
                            (!shared.inputs->wait_all(frame - 1, shared.boards, shown) || shared.inputs->wrong_from() < frame ||
                             (one_board && one_board->late_before(frame)))) {
                            for (const uint8_t seat : seats) shared.netplay->board_request_again(seat, frame);
                            return;
                        }
                        // (as its backup of this frame: the pages written since the last one, sent to every machine off
                        // this thread, then "my backup" to those who asked; leaving, the whole state, sent at once)
                        if (!parting && shared.real_netboard) {
                            backup_now(frame);
                            std::printf("spikeout: this board's state at frame %llu to %zu seat(s): its backup\n",
                                        static_cast<unsigned long long>(frame), seats.size());
                            shared.netplay->send_board_after_backups(frame, tools::one_board::pack_remote(0, 0, {}), seats);
                            return;
                        }
                        const auto t0 = std::chrono::steady_clock::now();
                        m3::State s;
                        tools::board_state(s, c, timing, {});
                        m3::LinkRing* r = shared.ring;
                        const uint64_t taken = r ? r->taken(index) : 0, sent = r ? r->sent(index) : 0;
                        if (r) r->abandon_cut(index); // (one board's state alone: no cut under way)
                        const auto t1 = std::chrono::steady_clock::now();
                        const std::vector<uint8_t> packed = tools::one_board::pack_remote(taken, sent, net::compress(s.data()));
                        const auto t2 = std::chrono::steady_clock::now();
                        for (const uint8_t seat : seats) {
                            std::printf("spikeout: this board's state at frame %llu to seat %u (%zu KB packed, %zu KB; saved in "
                                        "%.1f ms, compressed in %.1f ms)\n",
                                        static_cast<unsigned long long>(frame), seat + 1u, packed.size() / 1024, s.data().size() / 1024,
                                        std::chrono::duration<double, std::milli>(t1 - t0).count(),
                                        std::chrono::duration<double, std::milli>(t2 - t1).count());
                            shared.netplay->session().send_board(seat, frame, packed);
                        }
                        if (parting) shared.parted = true;
                    });
            }
            // (one board: from the hand-over every other board runs whole here (a shadow from its machine's real board,
            // asked for: Game::want_whole) before every board's state is saved; a board whose player went without a
            // parting can't be)
            const bool alone = one_board && shared.one_board_frame != ~0ull;
            if (index == shown && alone) {
                for (const auto& [seat, at] : shared.netplay->take_partings()) one_board->parting(seat, at);
                for (const uint8_t seat : shared.netplay->take_gone()) one_board->seat_gone(seat);
                for (const uint8_t seat : shared.netplay->take_taken()) one_board->seat_taken(seat);
            }
            if (index == shown && shared.linked.capture_frame == 0 && shared.netplay->take_state_request()) {
                if (!alone || one_board->all_whole()) {
                    shared.linked.capture_frame = frame + 2;
                } else if (one_board->can_be_whole()) {
                    std::printf("spikeout: every board's state asked for at frame %llu: every board whole here first\n",
                                static_cast<unsigned long long>(frame));
                    one_board->want_whole(true);
                    shared.state_pending = true;
                } else {
                    std::printf("spikeout: every board's state asked for at frame %llu: a board whose player went is had "
                                "nowhere whole, none sent\n",
                                static_cast<unsigned long long>(frame));
                }
            }
            if (index == shown && alone && shared.state_pending && shared.linked.capture_frame == 0 && one_board->all_whole()) {
                shared.linked.capture_frame = frame + 2;
                shared.state_pending = false, shared.whole_until = frame + 10;
            }
            if (index == shown && alone && shared.whole_until && frame >= shared.whole_until)
                one_board->want_whole(false), shared.whole_until = 0;
            // F9: every board's state in a file, the same way (gamerun --boards N
            // --parallel --load-state runs on from it).
            if (index == shown && shared.linked.capture_frame == 0 && shared.net_save_request.exchange(false)) {
                if (alone && !one_board->all_whole()) {
                    std::printf("spikeout: no save state: not every board runs whole here (one board)\n");
                } else {
                    shared.linked.to_file = true;
                    shared.linked.path = "spikeout-net-" + std::to_string(frame + 2) + ".state";
                    shared.linked.capture_frame = frame + 2;
                }
            }
            // (rollback: only a frame that won't be run again: every input of it in, none guessed wrong; else when it is)
            if (shared.linked.capture_frame != 0 && frame == shared.linked.capture_frame &&
                (!shared.rollback || (shared.inputs->wait_all(frame, shared.boards, shown) && shared.inputs->wrong_from() > frame)))
                c.request_snapshot([&](uint32_t) {
                    m3::State s;
                    // A cabinet whose sound this machine doesn't run carries this machine's own
                    // sound board: whoever takes that cabinet (a player joining, or put back in
                    // step) then has music (the owner's choice, 2026-10-30). In the game every
                    // cabinet's music board plays the same track, at most two MPEG frames apart
                    // (gamerun --music-hash), so a cabinet in the game is in step at once; one
                    // still waiting (attract, select) plays this machine's track until its own
                    // game sends its tune (coin, the select, entering the game: it answers them,
                    // where a sound board that never ran didn't, the start-up exchange made only
                    // at power-on missing: no music until a reload). A moment of this machine's
                    // own sound effects may come with it.
                    const bool carry = !sound_state && shared.sound;
                    const std::function<void(m3::State&)> carried =
                        carry ? std::function<void(m3::State&)>([&](m3::State& st) { shared.sound->state(st, c.cycles()); })
                              : sound_state;
                    tools::board_state(s, c, timing, carried);
                    std::lock_guard guard(shared.linked.lock);
                    shared.linked.boards[index] = std::move(s.data());
                    ++shared.linked.saved;
                });
            if (shared.netplay->resync_pending()) throw Stop{}; // (out of step: again from the host's state)
            // Tests: SPIKEOUT_TEST_POKE=B:ADDR=VALUE@FRAME (hex address and
            // value) writes a byte of board B's RAM at the end of FRAME, on
            // this machine only: its boards then differ from the others'.
            static const char* test_poke = std::getenv("SPIKEOUT_TEST_POKE");
            if (test_poke) {
                unsigned b = 0, addr = 0, value = 0;
                unsigned long long at = 0;
                if (std::sscanf(test_poke, "%u:%x=%x@%llu", &b, &addr, &value, &at) == 4 && b == index && at == frame) {
                    bus.write8(addr, static_cast<uint8_t>(value));
                    std::printf("spikeout: test poke, board %u %06x = %02x at frame %llu\n", b, addr, value, at);
                }
            }
            // Run on from a state: caught up with the others (as far as the
            // furthest of their inputs: one seat alone may be a cabinet whose
            // player left, its inputs stopped where they did), our cabinet then.
            if (index == shown && shared.catching_up) {
                uint64_t front = 0;
                for (std::size_t s = 0; s < shared.boards; ++s)
                    if (s != shown || shared.spectating) front = std::max(front, shared.netplay->session().inputs_known(static_cast<uint8_t>(s)));
                if (shared.spectating) front = front > net::kWatchLag ? front - net::kWatchLag : 0; // (it runs that far behind them)
                shared.catch_up_behind = front > frame ? front - frame : 0; // (for the loading screen)
                // (tests: SPIKEOUT_TEST_CATCHUP_MS=N, each frame caught up N ms longer: a slow PC's join, to see it)
                static const int test_catchup_ms = std::getenv("SPIKEOUT_TEST_CATCHUP_MS") ? std::atoi(std::getenv("SPIKEOUT_TEST_CATCHUP_MS")) : 0;
                if (test_catchup_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(test_catchup_ms));
                // (how long it takes: from the first frame run here to the others)
                if (!catch_up_frame) {
                    catch_up_frame = frame, catch_up_at = clock::now(), catch_up_start_behind = front > frame ? front - frame : 0;
                    shared.catch_up_start = catch_up_start_behind;
                    std::printf("spikeout: catching up from frame %llu, the others at %llu\n", static_cast<unsigned long long>(frame),
                                static_cast<unsigned long long>(front));
                }
                // (not gaining on them after a few seconds (a PC that can't run every board faster than the game): the
                // cabinet taken anyway; the others then wait for this machine as for any slow player, and Auto may go to
                // one board)
                const double so_far = std::chrono::duration<double>(clock::now() - catch_up_at).count();
                const bool given_up = so_far > 5 && front > frame + 4 && front - frame + 30 >= catch_up_start_behind;
                if (given_up)
                    std::printf("spikeout: not gaining on the others (%llu frames behind after %.0f s): the cabinet taken anyway\n",
                                static_cast<unsigned long long>(front - frame), so_far);
                if (frame + 4 >= front || given_up) {
                    const double s = std::chrono::duration<double>(clock::now() - catch_up_at).count();
                    std::printf("spikeout: caught up at frame %llu: %llu frames in %.1f s (%.2fx the game's speed)\n",
                                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(frame - catch_up_frame), s,
                                s > 0 ? static_cast<double>(frame - catch_up_frame) / s / recomp::FrameTiming::kRefreshHz : 0.0);
                    catch_up_frame = 0;
                    shared.rollback_from = frame + 2; // (rollback: snapshots and guesses again, every board from there)
                    shared.catching_up = false;
                    shared.netplay->session().take_seat(); // (a cabinet joined in progress; else nothing)
                }
            }
            if (index == shown) {
                tools::InputTable::Inputs in{out.bank0, out.bank1, out.controls, 0xFF};
                // Tests: SPIKEOUT_TEST_PRESS=NAME@FRAME[:N],... (gamerun --press's names) holds this player's
                // NAME for N frames (6) from FRAME instead of the keys and pads: a run gamerun can repeat.
                static const std::vector<TestPress> test_presses = parse_test_presses(std::getenv("SPIKEOUT_TEST_PRESS"));
                if (!test_presses.empty()) {
                    in.bank0 = in.controls = 0xFF;
                    for (const TestPress& p : test_presses)
                        if (frame >= p.frame && frame < p.frame + p.frames)
                            (p.bank0 ? in.bank0 : in.controls) &= static_cast<uint8_t>(~(1u << p.bit));
                }
                if (shared.input_delay && !inputs_started) shared.netplay->local(index, frame, {});
                inputs_started = true;
                shared.netplay->local(index, frame + shared.input_delay, in);
            }
            // (--net-one-board: a shadow of another cabinet is exact in what it sends, not in all of its RAM)
            // (rollback: once every board's inputs up to that frame are in and none guessed wrong, and this board has
            // run it again if it went back: kept until then)
            if (frame % 120 == 0 && !(goes && frame > shared.one_board_frame)) {
                if (shared.rollback) hash_pending[frame] = tools::Recording::ram_hash(bus.ram());
                else shared.netplay->hash(frame, index, tools::Recording::ram_hash(bus.ram()));
            }
            while (!hash_pending.empty() && hash_pending.begin()->first <= frame && shared.inputs->all_set(hash_pending.begin()->first, shared.boards) &&
                   shared.inputs->wrong_from() > hash_pending.begin()->first) {
                shared.netplay->hash(hash_pending.begin()->first, index, hash_pending.begin()->second);
                hash_pending.erase(hash_pending.begin());
            }
            const auto waited = clock::now();
            bool guessed = false; // (rollback: a guess, a few frames ahead at most)
            // (one board: not around a switch between side runs and whole, Game::may_guess)
            const bool may_guess = shared.rollback && frame >= shared.rollback_from && (!one_board || one_board->may_guess(frame));
            const auto got = may_guess ? shared.inputs->guess(index, frame, shared.rollback_frames, guessed)
                                             : shared.inputs->wait(index, frame);
            if (timing_debug) input_ms += std::chrono::duration<double, std::milli>(clock::now() - waited).count();
            if (!got || shared.quit) throw Stop{};
            bus.inputs.bank0 = got->bank0;
            bus.inputs.bank1 = got->bank1;
            bus.inputs.controls = got->controls;
            bus.inputs.controls2 = got->controls2;
            // (rollback: the frames it may run again keep theirs)
            const uint64_t keep = shared.rollback ? shared.rollback_frames + 2 : 0;
            if (frame > keep) shared.inputs->forget_before(index, frame - keep);
        } else {
            bus.inputs.bank0 = out.bank0;
            bus.inputs.bank1 = out.bank1;
            bus.inputs.controls = out.controls;
        }
        if (index == 0 && !networked && shared.save_request.exchange(false))
            c.request_snapshot([&](uint32_t) {
                std::string path = "spikeout-" + std::to_string(timing.frames()) + ".state";
                bool undo = false; // (the game as it was before a load: F9 goes back to it a few seconds)
                {
                    std::lock_guard<std::mutex> guard(shared.lock);
                    if (!shared.save_to.empty()) path = std::exchange(shared.save_to, {}), undo = true;
                }
                try {
                    m3::State s;
                    tools::board_state(s, c, timing, sound_state);
                    if (!tools::write_file(path, tools::state_file_bytes(timing.frames(), s.data())))
                        throw std::runtime_error("can't write it");
                    if (!undo) {
                        {
                            std::lock_guard<std::mutex> guard(shared.lock);
                            shared.last_state = path;
                        }
                        shared.say("STATE SAVED (FRAME " + std::to_string(timing.frames()) + ")");
                    }
                    std::printf("spikeout: saved %s\n", path.c_str());
                } catch (const std::exception& e) {
                    std::printf("spikeout: no save state %s: %s\n", path.c_str(), e.what());
                    if (!undo) shared.say("CAN'T SAVE: " + std::string(e.what()));
                }
                ++shared.saves_done;
            });
        if (recording.is_open()) {
            const auto frame = static_cast<uint32_t>(timing.frames());
            if (frame % tools::Recording::kHashEvery == 0) recording.hash(frame, tools::Recording::ram_hash(bus.ram()));
            recording.inputs(frame, {bus.inputs.bank0, bus.inputs.bank1, bus.inputs.controls, bus.inputs.controls2});
        }
        // (rollback: this board's snapshot after the frame, the last asked: another state taken here leaves no cut)
        // (not while catching up: every input of those frames is in, none guessed; all the boards from the same frame on)
        if (shared.rollback && timing.frames() >= shared.rollback_from)
            c.request_snapshot([&, frame = timing.frames()](uint32_t) { shared.rollback->save(index, frame); });
        // (--frames in a network game run on one board: this board handed over before the end, as leaving does)
        if (shared.max_frames && networked && one_board && index == shown && shared.one_board_frame != ~0ull &&
            timing.frames() + tools::one_board::Game::kAskAhead + 5 == shared.max_frames)
            shared.parting_wanted = true;
        if (shared.max_frames && timing.frames() >= shared.max_frames) shared.quit = true;
        if (shared.quit) throw Stop{};
        run_started = clock::now(); // (SPIKEOUT_NET_DEBUG: the next frame runs from here)
        if (timing_debug && index == shown) end_ms += std::chrono::duration<double, std::milli>(run_started - frame_ended).count();
    };

    // Rollback: this board's snapshots (its state where it starts kept first: a first guess's frame before)
    if (shared.rollback) {
        shared.rollback->attach(index, [&](m3::State& st) {
            // (one board: the boards on side runs, made by this one's module, their network boards with it; first:
            // board_state ends with its check that nothing follows it)
            if (part.cabinets)
                for (std::size_t b = 0; b < shared.boards; ++b)
                    if (b != index && shared.one_board_parts[b] && !shared.rollback->active(b))
                        shared.one_board_parts[b]->bus.netboard().state(st);
            tools::board_state(st, c, timing, {});
        });
        shared.rollback->save(index, timing.frames());
    }
    std::string why;
    turns.wait(index);
    for (bool resume = loaded;;) {
        try {
            if (!load_error.empty()) throw recomp::GuestError(c.cpu.pc, "save state " + load_error);
            if (resume) c.resume_block(c.cpu.pc);
            c.call(c.cpu.pc);
            why = "the reset code returned";
        } catch (const tools::rollback::Rewound&) { // (rollback: on from this board's snapshot)
            resume = true;
            continue;
        } catch (const tools::one_board::HandedBack& h) { // (--net-one-board: the other cabinet's shadow stopped, made on from there)
            if (one_board->handed_back(part, turns, h.count, why)) { // (a shadow again later)
                why.clear();
                resume = true;
                continue;
            }
        } catch (const Stop&) {
        } catch (const recomp::MissingCode& e) {
            why = e.what();
        } catch (const recomp::GuestError& e) {
            why = e.what();
        }
        break;
    }
    tools::one_board::Game::stopped(part); // (--net-one-board: this player's board stopped, the other's shadow with it)
    if (part.cabinets && one_board) // (one board: what the other cabinets' side runs cost, and the waits for their buttons)
        std::printf("spikeout: other cabinets: %llu side runs, %.2f s in them (%.2f s with their set-ups); their buttons "
                    "waited for %llu times, %.2f s\n%s",
                    static_cast<unsigned long long>(part.cabinets->side_runs()), part.cabinets->side_seconds(),
                    part.cabinets->whole_seconds(), static_cast<unsigned long long>(one_board->held_waits()),
                    one_board->held_wait_seconds(), part.cabinets->profile().c_str());
    if (shared.rollback && index == shown)
        std::printf("spikeout: rollback: %s; time sync: %llu frames slowed, %llu sooner, %llu run unpaced\n",
                    shared.rollback->report().c_str(), static_cast<unsigned long long>(shared.sync_slowed.load()),
                    static_cast<unsigned long long>(shared.sync_sooner.load()), static_cast<unsigned long long>(shared.sync_free.load()));
    turns.finish(index);
    std::lock_guard<std::mutex> guard(shared.lock);
    out.stopped = why.empty() ? "quit" : why;
    out.eeprom.assign(bus.eeprom().words().begin(), bus.eeprom().words().end());
    out.backup.assign(bus.backup_ram().begin(), bus.backup_ram().end());
    out.bus = nullptr;
}

// One board a PC, wanted: set up where the boards meet (every one stopped there), this player's board
// staying, the others held whole until the rules find the game calm.
void set_up_one_board(Shared& shared) {
    shared.one_board_wanted = false;
    tools::one_board::Board& stays = *shared.one_board_parts[shared.local];
    tools::one_board::Game::Settings s;
    s.stays = shared.local;
    for (std::size_t b = 0; b < shared.boards; ++b)
        if (b != shared.local) s.going.push_back(b);
    s.frame = stays.timing.frames();
    s.tasks = s.rules = true;
    s.slice = kTurnLines;
    s.net = true;
    // (a shadow from the other machine's real board, asked of it: docs/netplay.md)
    tools::NetPlay* net = shared.netplay;
    s.ask_board = [net](std::size_t seat, uint64_t frame) { net->session().want_board(static_cast<uint8_t>(seat), frame); };
    s.wait_board = [net](std::size_t seat, uint64_t frame) -> std::optional<tools::one_board::RemoteBoard> {
        const auto t0 = std::chrono::steady_clock::now();
        auto packed = net->wait_board(static_cast<uint8_t>(seat), frame, std::chrono::seconds(30));
        if (!packed) return std::nullopt;
        const auto t1 = std::chrono::steady_clock::now();
        auto board = tools::one_board::unpack_remote(*packed, [](std::span<const uint8_t> d) { return net::decompress(d); });
        const bool backup = board && board->state.empty();
        if (backup) { // ("my backup of that frame": from the copy kept here)
            auto copy = net->backup_at(static_cast<uint8_t>(seat), frame);
            if (!copy) {
                std::printf("spikeout: board %zu's backup of frame %llu not here\n", seat, (unsigned long long)frame);
                return std::nullopt;
            }
            board->regions = std::move(copy->regions), board->small = std::move(copy->small);
            board->taken = copy->taken, board->sent = copy->sent;
        }
        std::printf("spikeout: board %zu's state for frame %llu%s: waited %.1f ms for it, unpacked in %.1f ms\n", seat,
                    (unsigned long long)frame, backup ? " (its backup)" : "", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
        return board;
    };
    s.rewind_link = [ring = shared.ring](std::size_t seat, const tools::one_board::RemoteBoard& b) {
        if (ring)
            std::printf("one board: board %zu's port there: taken %llu, sent %llu; here: taken %llu, sent %llu (board 0 here: "
                        "taken %llu, sent %llu)\n",
                        seat, (unsigned long long)b.taken, (unsigned long long)b.sent, (unsigned long long)ring->taken(seat),
                        (unsigned long long)ring->sent(seat), (unsigned long long)ring->taken(0), (unsigned long long)ring->sent(0));
        if (!ring || (ring->taken(seat) == b.taken && ring->sent(seat) == b.sent)) return;
        ring->rewind(seat, b.taken);
        ring->unsend(seat, b.sent);
    };
    // (a board whose player went without a parting: its last backup run on here, the packets to each board and its
    // player's inputs kept since its backup for that: tools::backup)
    if (shared.real_netboard && shared.roms) {
        s.backup_frame = [net](std::size_t seat) { return net->keeper().frame(seat); };
        s.replay = [net, &shared](std::size_t seat, uint64_t until,
                                  std::function<bool(uint64_t)> live) -> std::optional<tools::one_board::RemoteBoard> {
            // (a spectator: the newest of the backups kept at or before the frame: its players' are taken ahead of it)
            const tools::backup::Copy copy = shared.spectating ? net->keeper().copy_for(seat, until) : net->keeper().copy(seat);
            if (!copy.valid() || copy.frame > until) {
                std::printf("spikeout: board %zu: no backup of it at or before frame %llu here\n", seat, (unsigned long long)until);
                return std::nullopt;
            }
            m3::CommBoardOptions options;
            if (shared.old_link) options.use_old_link();
            tools::backup::Replay r{*shared.roms, m3::games::kSpikeoutFinalEdition, options,
                                    [&shared](m3::Bus& bus) { bus.midi_burst = shared.midi_burst, bus.dma_timed = !shared.dma_instant; },
                                    [](recomp::Context& c) { m3::games::install_spikeout_hooks(c); }, kTurnLines, copy,
                                    net->keeper().log(seat),
                                    [net, seat](uint64_t frame) { return net->keeper().inputs(seat, frame); }, std::move(live), {}};
            try {
                auto result = r.run(until);
                if (!result) return std::nullopt;
                std::printf("spikeout: board %zu run on from its backup at frame %llu to %llu in %.1f s\n", seat,
                            static_cast<unsigned long long>(copy.frame), static_cast<unsigned long long>(until), result->seconds);
                return tools::one_board::RemoteBoard{std::move(result->state), result->taken, result->sent, {}, {}};
            } catch (const std::exception& e) {
                std::printf("spikeout: board %zu's backup can't be run on: %s\n", seat, e.what());
                return std::nullopt;
            }
        };
        if (shared.ring)
            shared.ring->on_departure([net](std::size_t to, uint64_t seq, uint64_t at, const std::vector<uint8_t>& data) {
                net->keeper().packet(to, seq, at, data);
            });
        // A spectator on one board (docs/netplay.md, "Spectators on one board"): no machine to ask for a cabinet's state;
        // its shadow is its player's backup run on here to the frame (the backups sent to it from now on, within the
        // players' spare upload: Session::set_watch_one_board; a few kept, its frames behind theirs).
        if (shared.spectating) {
            net->keeper().set_keep(3);
            net->session().set_watch_one_board(true);
            s.ask_board = [](std::size_t, uint64_t) {};
            s.wait_board = [replay = s.replay](std::size_t seat, uint64_t frame) -> std::optional<tools::one_board::RemoteBoard> {
                return replay(seat, frame, [](uint64_t) { return true; }); // (its packets in the log: this board met it there)
            };
        }
    }
    // (rollback: the switches only on frames that won't be run again, Game::may_guess; a board on side runs no part of
    // its own, its network board in this one's snapshots; the module's state kept and put back with this board's)
    if (tools::rollback::Linked* rb = shared.rollback.get()) {
        s.guess_ahead = shared.rollback_frames;
        tools::InputTable* table = shared.inputs;
        const std::size_t boards = shared.boards;
        s.settled = [table, boards](uint64_t frame) { return table->all_set(frame, boards) && table->wrong_from() > frame; };
        s.wait_settled = [table, boards, stays = shared.local](uint64_t frame) { // (this machine's own never guessed)
            return table->wait_all(frame, boards, stays) && table->wrong_from() > frame;
        };
        s.part_whole = [rb](std::size_t board, bool whole, uint64_t) { rb->set_active(board, whole); };
        s.rewound = [rb](std::size_t board) { return rb->rewound_to(board); };
        s.pin = [rb](uint64_t frame) { rb->pin(frame); };
        s.has_board = [net](std::size_t seat, uint64_t frame, uint32_t ms) {
            return net->has_board(static_cast<uint8_t>(seat), frame, std::chrono::milliseconds(ms));
        };
        s.run_again_from = [table](uint64_t frame) { table->run_again_from(frame); };
    }
    tools::one_board::Game& game = *shared.one_board_game;
    game.set_up({shared.one_board_parts.begin(), shared.one_board_parts.begin() + shared.boards}, std::move(s));
    for (std::size_t b = 0; b < shared.boards; ++b)
        if (b != shared.local) game.hand_over(*shared.one_board_parts[b], true);
    if (tools::rollback::Linked* rb = shared.rollback.get()) {
        tools::remote::Cabinets* cab = stays.cabinets.get();
        rb->attach_extra(
            shared.local, [cab] { return std::shared_ptr<const void>(std::make_shared<tools::remote::Cabinets::Kept>(cab->keep())); },
            [cab](const void* k) { cab->put_back(*static_cast<const tools::remote::Cabinets::Kept*>(k)); });
        // (a switch: set up only on a settled frame, no guesses through the next, nothing gone back to before that one:
        // the module's state kept from its save on)
        game.hold_guesses(stays.timing.frames() + 1);
        rb->pin(stays.timing.frames() + 1);
    }
    shared.one_board_frame = stays.timing.frames();
    std::printf("spikeout: one board here from frame %llu: the other cabinet held whole until the game is calm, then made from "
                "its player's buttons\n",
                static_cast<unsigned long long>(shared.one_board_frame.load()));
}

// No GPU (the owner's choice, 2026-11-02: such a device doesn't run the game): said in the window in the menu's
// colours, drawn with SDL's 2D renderer (the menu itself needs the GPU) in its built-in 8x8 font, in plain words (the
// technical reason only in the log); until a key, a button or the window closed.
void show_no_gpu(SDL_Window* window) {
    SDL_Renderer* r = SDL_CreateRenderer(window, nullptr);
    if (!r) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Spikeout",
                                 "Spikeout needs a graphics card with Vulkan, Direct3D 12 or Metal, and its driver.", window);
        return;
    }
    SDL_ShowWindow(window);
    auto line = [&](const std::string& text, float y, float scale, SDL_Color c) {
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(r, &w, &h);
        SDL_SetRenderScale(r, scale, scale);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        const float x = (static_cast<float>(w) / scale - static_cast<float>(text.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 2;
        SDL_RenderDebugText(r, x, static_cast<float>(h) * y / scale, text.c_str());
        SDL_SetRenderScale(r, 1, 1);
    };
    for (bool done = false; !done;) {
        for (SDL_Event e; SDL_PollEvent(&e);)
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                done = true;
        SDL_SetRenderDrawColor(r, 0x0b, 0x0d, 0x12, 255);
        SDL_RenderClear(r);
        line("NO GRAPHICS CARD", 0.36f, 4, {0xff, 0xd3, 0x4a, 255});
        line("Spikeout needs a graphics card with Vulkan,", 0.50f, 2, {0xc8, 0xcc, 0xd4, 255});
        line("Direct3D 12 or Metal, and its driver.", 0.55f, 2, {0xc8, 0xcc, 0xd4, 255});
        line("Press any key to quit", 0.75f, 2, {0xff, 0xff, 0xff, 255});
        SDL_RenderPresent(r);
        SDL_Delay(30);
    }
    SDL_DestroyRenderer(r);
}

// Paths from and to UTF-8 (settings.ini, SDL's dialogs).
std::filesystem::path utf8_path(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string path_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// What's wrong with a ROM set's zip (or a folder of its files, given on the command line), in plain words ("": it
// has every file, the right sizes; checksums aren't read).
std::string rom_problem(const std::filesystem::path& path) {
    const auto& game = m3::games::kSpikeoutFinalEdition;
    auto count = [&](const std::filesystem::path& p, std::size_t& missing, std::size_t& wrong) {
        missing = wrong = 0;
        for (const m3::RomFileReport& r : m3::survey_rom_set(game, p)) {
            missing += r.status == m3::RomFileStatus::Missing;
            wrong += r.status == m3::RomFileStatus::WrongSize || r.status == m3::RomFileStatus::WrongChecksum;
        }
    };
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return "That file isn't there any more.";
    std::size_t missing = 0, wrong = 0;
    count(path, missing, wrong);
    std::size_t files = 0;
    for (const m3::RomRegionSpec& r : game.regions) files += r.files.size();
    if (missing == files) return "That isn't Spikeout: Final Edition's set.";
    if (missing != 0)
        return std::to_string(missing) + " of the set's " + std::to_string(files) + " files aren't in it.";
    if (wrong != 0) return std::to_string(wrong) + (wrong == 1 ? " file differs" : " files differ") + " from the real set (a bad dump?).";
    return {};
}

// A folder for a menu line, UTF-8: a long one as its root and its last two folders (a path has no spaces to wrap at).
[[maybe_unused]] std::string short_path(const std::filesystem::path& p) {
    const std::string whole = path_utf8(p);
    if (whole.size() <= 48) return whole;
    std::vector<std::filesystem::path> parts(p.begin(), p.end());
    while (!parts.empty() && parts.back().empty()) parts.pop_back();
    if (parts.size() < 4) return whole;
    const char sep = static_cast<char>(std::filesystem::path::preferred_separator);
    return path_utf8(p.root_path()) + "..." + sep + path_utf8(parts[parts.size() - 2]) + sep + path_utf8(parts.back());
}

// The ROM set chosen in the window (no location set yet, or the one set can't be used), drawn as the no-GPU screen is:
// SDL's 2D renderer, its 8x8 font, the menu's colours. The keys, a pad or the mouse choose the zip with the
// system's own dialog; checked at once. The renderer is let go before the GPU takes the window.
class RomScreen {
public:
    explicit RomScreen(SDL_Window* window) : window_(window) {}
    ~RomScreen() { close(); }
    void close() {
        if (r_) SDL_DestroyRenderer(r_);
        r_ = nullptr;
    }

    // A location chosen and found good (nothing: the player quit); `why`: the last one's problem ("": none set).
    std::optional<std::filesystem::path> choose(std::string why) {
        if (!open()) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Spikeout", "Spikeout needs its ROM set, spikeofe.zip.", window_);
            return std::nullopt;
        }
        int selected = 0;
        for (;;) {
            if (auto picked = take_picked(); picked && !picked->empty()) { // (empty: cancelled)
                std::filesystem::path p = utf8_path(*picked);
                why = rom_problem(p);
                if (why.empty()) return p;
            }
            for (SDL_Event e; SDL_PollEvent(&e);) {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return std::nullopt;
                if (dialog_open_) continue;
                int act = -1;
                if (e.type == SDL_EVENT_KEY_DOWN) {
                    const SDL_Scancode k = e.key.scancode;
                    if (k == SDL_SCANCODE_UP || k == SDL_SCANCODE_LEFT) selected = (selected + kButtons - 1) % kButtons;
                    if (k == SDL_SCANCODE_DOWN || k == SDL_SCANCODE_RIGHT || k == SDL_SCANCODE_TAB) selected = (selected + 1) % kButtons;
                    if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE) act = selected;
                    if (k == SDL_SCANCODE_ESCAPE) return std::nullopt;
                } else if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                    SDL_OpenGamepad(e.gdevice.which);
                } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    const auto b = static_cast<SDL_GamepadButton>(e.gbutton.button);
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) selected = (selected + kButtons - 1) % kButtons;
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) selected = (selected + 1) % kButtons;
                    if (b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_START) act = selected;
                } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    float x = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x;
                    float y = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
                    SDL_RenderCoordinatesFromWindow(r_, x, y, &x, &y);
                    const SDL_FPoint at{x, y};
                    for (int i = 0; i < kButtons; ++i)
                        if (SDL_PointInRectFloat(&at, &boxes_[static_cast<std::size_t>(i)])) {
                            selected = i;
                            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) act = i;
                        }
                }
                if (act == 0) pick();
                if (act == 1) return std::nullopt;
            }
            draw(why, selected);
            SDL_Delay(16);
        }
    }

    // "Loading" over the window while the set is read (a few seconds from a zip).
    void loading() {
        if (!r_) return;
        clear();
        line("LOADING", 0.42f, 4, kTitle);
        line("Reading the ROM set...", 0.56f, 2, kText);
        SDL_RenderPresent(r_);
    }

    // The JIT build's first start: the game being translated from the ROM set, how far (a share, 0-1) and what
    // it's doing; once only, later starts load it.
    void preparing(float share, const std::string& doing) {
        if (!r_ && !open()) return;
        clear();
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(r_, &w, &h);
        const float small = h < 600 ? 1.0f : 2.0f;
        line("PREPARING THE GAME", 0.30f, small * 2, kTitle);
        line("Translating the game from your ROM set.", 0.42f, small, kText);
        line("This happens on the first start only.", 0.47f, small, kText);
        const float bw = static_cast<float>(w) * 0.6f, bh = 12 * small;
        const SDL_FRect frame{(static_cast<float>(w) - bw) / 2, static_cast<float>(h) * 0.56f, bw, bh};
        SDL_FRect bar{frame.x + 2, frame.y + 2, (frame.w - 4) * std::clamp(share, 0.0f, 1.0f), frame.h - 4};
        SDL_SetRenderDrawColor(r_, kTitle.r, kTitle.g, kTitle.b, 255);
        SDL_RenderFillRect(r_, &bar);
        SDL_SetRenderDrawColor(r_, kText.r, kText.g, kText.b, 255);
        SDL_RenderRect(r_, &frame);
        line(doing, 0.56f + bh / static_cast<float>(h) + 0.03f, small, kText);
        SDL_RenderPresent(r_);
    }

private:
    static constexpr int kButtons = 2;
    static constexpr SDL_Color kTitle{0xff, 0xd3, 0x4a, 255}, kText{0xc8, 0xcc, 0xd4, 255}, kWhite{0xff, 0xff, 0xff, 255},
        kProblem{0xff, 0x7a, 0x6a, 255};

    bool open() {
        if (!r_) r_ = SDL_CreateRenderer(window_, nullptr);
        if (r_) SDL_SetRenderVSync(r_, 1);
        SDL_ShowWindow(window_);
        return r_ != nullptr;
    }
    void clear() {
        SDL_SetRenderDrawColor(r_, 0x0b, 0x0d, 0x12, 255);
        SDL_RenderClear(r_);
    }
    // A centred line at a height (a share of the window's), `scale` times the 8x8 font; its box in render pixels.
    SDL_FRect line(const std::string& text, float y, float scale, SDL_Color c) {
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(r_, &w, &h);
        const float tw = static_cast<float>(text.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
        while (scale > 1 && tw * scale > static_cast<float>(w) * 0.95f) scale -= 1; // (a line wider than the window: smaller)
        SDL_SetRenderScale(r_, scale, scale);
        SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, 255);
        const float x = (static_cast<float>(w) / scale - tw) / 2, ty = static_cast<float>(h) * y / scale;
        SDL_RenderDebugText(r_, x, ty, text.c_str());
        SDL_SetRenderScale(r_, 1, 1);
        return {x * scale, ty * scale, tw * scale, SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale};
    }
    void draw(const std::string& why, int selected) {
        clear();
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(r_, &w, &h);
        const float small = h < 600 ? 1.0f : 2.0f;
        line("CHOOSE THE ROM SET", 0.22f, small * 2, kTitle);
        line("Spikeout needs the arcade's ROM set,", 0.36f, small, kText);
        line("spikeofe.zip, as MAME and Supermodel have it.", 0.41f, small, kText);
        if (!why.empty()) line(why, 0.48f, small, kProblem);
        static const char* kLabels[kButtons] = {"Choose the zip file", "Quit"};
        for (int i = 0; i < kButtons; ++i) {
            const SDL_FRect t = line(kLabels[i], 0.58f + 0.08f * static_cast<float>(i), small, i == selected ? kWhite : kText);
            const float pad = 8 * small;
            SDL_FRect box{t.x - 2 * pad, t.y - pad, t.w + 4 * pad, t.h + 2 * pad};
            boxes_[static_cast<std::size_t>(i)] = box;
            if (i == selected) {
                SDL_SetRenderDrawColor(r_, kTitle.r, kTitle.g, kTitle.b, 255);
                SDL_RenderRect(r_, &box);
            }
        }
        if (dialog_open_) line("Choose it in the window that opened.", 0.86f, small, kText);
        else line("Arrows and Enter, a pad, or the mouse", 0.86f, small, kText);
        SDL_RenderPresent(r_);
    }
    // The system's dialog (its answer comes on another thread: take_picked).
    void pick() {
        dialog_open_ = true;
        static const SDL_DialogFileFilter kZip[] = {{"Zip files", "zip"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(&RomScreen::picked, this, window_, kZip, 2, nullptr, false);
    }
    static void SDLCALL picked(void* self, const char* const* files, int) {
        auto* s = static_cast<RomScreen*>(self);
        std::lock_guard guard(s->lock_);
        s->picked_ = files && files[0] ? std::string(files[0]) : std::string();
    }
    std::optional<std::string> take_picked() {
        std::lock_guard guard(lock_);
        auto p = std::move(picked_);
        picked_.reset();
        if (p) dialog_open_ = false;
        return p;
    }

    SDL_Window* window_;
    SDL_Renderer* r_ = nullptr;
    std::array<SDL_FRect, kButtons> boxes_{};
    std::mutex lock_;
    std::optional<std::string> picked_; // (empty: cancelled)
    bool dialog_open_ = false;
};

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

// Network play from the menu: a game offered or joined, on this network
// (LAN discovery) or through the internet lobby, its players as they come,
// until it starts (then the window runs every board of it). Setting up
// through the lobby takes a few seconds, so it goes on a thread of its own.
struct NetGame {
    tools::InputTable table{net::kMaxSeats};
    std::unique_ptr<tools::NetPlay> play;
    net::LobbyClient lobby;
    std::thread worker;
    bool host = false, internet = false, running = false;
    bool spectator = false; // (watching it: Session::watch)
    std::size_t listed_taken = 1; // (the host, on the internet: the players the lobby last heard of)
    std::atomic<bool> ready{false}, failed{false};
    mutable std::mutex lock;
    std::string status, code;

    NetGame(uint64_t build) {
        play = std::make_unique<tools::NetPlay>(table, build, [this](const std::string& s) {
            std::printf("net: %s\n", s.c_str());
            set_status(s);
        });
    }
    ~NetGame() {
        if (worker.joinable()) worker.join();
        play.reset(); // (its thread first: it calls back into the table and the status)
    }
    net::Session& session() { return play->session(); }
    void set_status(const std::string& s) {
        std::lock_guard guard(lock);
        status = s;
    }
    std::string get_status() const {
        std::lock_guard guard(lock);
        return status;
    }
    void fail(const std::string& why) {
        set_status(why);
        failed = true;
    }
};

// This computer's player's name for the others: the one set in the menu,
// else its user's name (tools::clean_name's rules both).
std::string player_name(const tools::Settings& settings) {
    if (!settings.player_name.empty()) return settings.player_name;
    for (const char* var : {"USERNAME", "USER"})
        if (const char* v = std::getenv(var); v && *v)
            if (std::string name = tools::clean_name(v); !name.empty()) return name;
    return "PLAYER";
}

// The effects' and the music's volumes (percent), under the overall one.
int effects_volume(const tools::Settings& s) { return s.sound_volume * s.volume / 100; }
int music_volume(const tools::Settings& s) { return s.music_volume * s.volume / 100; }

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // A Windows program (no console window of its own: WIN32_EXECUTABLE). Its messages go where they're sent (a
    // script's file), else to the console it was started from, if any; started from Explorer, nowhere.
    auto std_handle_set = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    if (!std_handle_set(STD_OUTPUT_HANDLE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        if (!std_handle_set(STD_ERROR_HANDLE)) freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
    const char* dir = nullptr;
    std::string record_path, load_state;
    int scale = 0, cli_render_scale = 0, cli_sound = -1, cli_music = -1;
    bool real_netboard = true, old_link = false, fog_over_nothing = false;
    bool net_save_key = false; // --net-save-key
    Shared::OneBoard net_one_board = Shared::OneBoard::automatic; // --net-every-board, --net-one-board (Auto: the default)
    uint64_t net_rollback = 0; // --net-rollback N (tools::rollback; every board here: no one board then)
    bool net_rollback_forced = false; // (--net-rollback given: not chosen by itself)
    int cli_net_delay = -1;           // --net-delay N: the input delay forced (tests; -1: Auto)
    uint64_t max_frames = 0;
    bool mute = false, cli_quads = false, cli_smooth = false, cli_smooth_log = false, midi_burst = false, dma_instant = false, use_gpu = true, cli_soft_3d = false, fifo = false,
         sleep_pacing = false, list_audio = false;
    std::size_t boards = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--netboard" && i + 1 < argc) real_netboard = std::string(argv[++i]) == "real";
        else if (arg == "--net-old-link") old_link = true;
        else if (arg == "--net-save-key") net_save_key = true; // (F9 in network play: every cabinet in a file, for gamerun)
        else if (arg == "--net-one-board") net_one_board = Shared::OneBoard::always;
        else if (arg == "--net-every-board") net_one_board = Shared::OneBoard::never;
        else if (arg == "--net-auto-board") net_one_board = Shared::OneBoard::automatic;
        else if (arg == "--net-rollback" && i + 1 < argc) net_rollback = std::strtoull(argv[++i], nullptr, 0), net_rollback_forced = true;
        else if (arg == "--net-delay" && i + 1 < argc) cli_net_delay = std::clamp(std::atoi(argv[++i]), 0, 8);
        else if (arg == "--no-idle-skip") m3::games::set_idle_skip(false), m3::CommBoard::set_wait_skip(false); // (the vblank wait and the network board's waits spun, as before)
        else if (arg == "--no-net-idle-skip") m3::CommBoard::set_wait_skip(false); // (the network board's only)
        else if (arg == "--fog-over-nothing") fog_over_nothing = true;
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
    // (usage: spikeout [<rom zip or folder>] [--scale N] [--netboard sim|real] [--frames N] [--mute] [--quads]
    //   [--midi-burst] [--dma-instant] [--sound-volume N] [--music-volume N] [--boards N] [--no-gpu] [--soft-3d]
    //   [--render-scale N] [--fifo] [--sleep-pacing]; spikeout --list-audio. No ROM given: the one chosen before, in
    //   settings.ini, else chosen in the window)
    if (scale == 0) scale = boards == 1 ? 2 : 1;

    const int cols = grid_columns(boards), rows = grid_rows(boards);
    const int width = kWidth * cols, height = kHeight * rows;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "spikeout: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // Tests: SPIKEOUT_TEST_VIRTUAL_PAD=1 connects a pad that isn't there (SDL's virtual joystick: no buttons
    // pressed), for the menu's pad pages on a computer with none.
    if (std::getenv("SPIKEOUT_TEST_VIRTUAL_PAD")) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Virtual pad (test)";
        if (!SDL_AttachVirtualJoystick(&desc)) std::fprintf(stderr, "spikeout: virtual pad: %s\n", SDL_GetError());
    }

    // The settings, and the command line over them.
    const std::string settings_path = tools::data_folder() + "settings.ini";
    tools::Settings settings = tools::load_settings(settings_path);
    // Whether a newer version is out (GitHub's release list, on its own thread; the menu says so). Not for builds that
    // aren't releases ("-dev"); SPIKEOUT_UPDATE_AS=VERSION: as if this were that version (tests).
    std::string version = net::kVersion;
    if (const char* e = std::getenv("SPIKEOUT_UPDATE_AS"); e && *e) version = e;
    net::UpdateCheck update_check;
    std::optional<net::Release> newer_version;
    bool update_told = false;
    std::printf("spikeout: version %s\n", net::kVersion);
    if (settings.check_updates && !net::dev_build(version)) update_check.start(version);
    if (cli_render_scale) settings.render_scale = cli_render_scale;
    if (cli_quads) settings.quads = true;
    if (cli_smooth) settings.smooth_motion = true;
    if (cli_soft_3d) settings.gpu_3d = false;
    if (cli_sound >= 0) settings.sound_volume = cli_sound;
    if (cli_music >= 0) settings.music_volume = cli_music;
    const bool test_run = max_frames != 0; // (writes no settings)

    // (hidden until the ROM set is read, as before it was made after; shown at once if one must be chosen)
    SDL_Window* window =
        SDL_CreateWindow("Spikeout: Final Edition", width * scale, height * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "spikeout: %s\n", SDL_GetError());
        return 1;
    }
    // The app's icon (its title bar and taskbar; macOS's Dock too; the .exe's own is its resource on Windows).
    if (SDL_Surface* icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(ui::assets::app_icon_png.data, ui::assets::app_icon_png.size), true)) {
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
    if (settings.fullscreen && !test_run) SDL_SetWindowFullscreen(window, true);

    // The ROM set: the command line's, else the one chosen before (settings.ini), else chosen here (the release
    // checklist's picker: a zip or a folder, remembered). One that can't be used: chosen again, said why.
    m3::RomSet roms;
    std::string rom_in_use; // (its location, UTF-8: the menu shows it)
    std::filesystem::path jit_cache_in_use; // (the JIT build: the translation's folder; empty: not kept, or the C++ build)
    {
        const bool given = dir != nullptr;
        std::filesystem::path rom_path = given ? std::filesystem::path(dir) : utf8_path(settings.rom_path);
        RomScreen screen(window);
        std::string why;
        for (bool chosen = false;; chosen = true) {
            if (!rom_path.empty()) {
                why = rom_problem(rom_path);
                if (why.empty()) {
                    screen.loading();
                    try {
                        roms = m3::load_rom_set(m3::games::kSpikeoutFinalEdition, rom_path);
                        // (refused unless every file is the known set's, by CRC: the game's code is compiled from
                        // that program ROM, and a different one would run the wrong code, unnoticed; the owner's
                        // choice, 2026-11-04. gamerun and romcheck still load such sets, as before)
                        std::size_t differ = 0;
                        for (const m3::RomFileReport& r : roms.report) {
                            differ += r.status == m3::RomFileStatus::WrongChecksum;
                            if (r.status == m3::RomFileStatus::WrongChecksum)
                                std::printf("spikeout: %s's CRC is %08x, not %08x\n", r.name.c_str(), r.actual_crc, r.expected_crc);
                        }
                        if (roms.complete() && differ == 0) break;
                        why = differ != 0 ? std::to_string(differ) + (differ == 1 ? " file differs" : " files differ") +
                                                " from the real set (a bad dump?)."
                                          : "Some of its files can't be read.";
                    } catch (const std::exception& e) {
                        std::printf("spikeout: %s\n", e.what());
                        why = "It can't be read (a damaged zip?).";
                    }
                }
                std::printf("spikeout: the ROM set at %s: %s\n", path_utf8(rom_path).c_str(), why.c_str());
            }
            if (given && !chosen) { // (a script's or a shortcut's: said, not asked)
                std::fprintf(stderr, "spikeout: ROM set incomplete; run romcheck for details\n");
                return 1;
            }
            const auto picked = screen.choose(why);
            if (!picked) {
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 0;
            }
            rom_path = *picked;
        }
        const std::string chosen = path_utf8(rom_path);
        if (!given && chosen != settings.rom_path) { // (one chosen here kept; the command line's never written)
            settings.rom_path = chosen;
            if (!test_run) tools::save_settings(settings_path, settings);
        }
        std::printf("spikeout: ROM set %s\n", chosen.c_str());
        rom_in_use = chosen;
#ifdef SPIKEOUT_JIT
        // The game's code, translated from this ROM set by the JIT on another thread: the first start (or a new
        // set or version) ~15 s on every core, behind a screen with how far it is; later starts load it from the
        // cache folder in a fraction of a second (no screen: shown only after 0.3 s). Kept in the data folder's
        // jit-cache unless the settings or SPIKEOUT_JIT_CACHE say elsewhere; translations of other sets deleted.
        std::atomic<int> step{0};
        std::atomic<unsigned> done{0}, total{0};
        std::atomic<bool> cancel{false}, finished{false};
        std::string error;
        jit::Options jo;
        jo.cancel = &cancel;
        jo.prune = true;
        if (!settings.jit_cache.empty()) jo.cache_dir = utf8_path(settings.jit_cache);
        else if (const char* e = std::getenv("SPIKEOUT_JIT_CACHE"); !e || !*e) jo.cache_dir = utf8_path(tools::data_folder() + "jit-cache");
        jo.progress = [&](const jit::Progress& p) {
            step = static_cast<int>(p.step);
            if (p.step == jit::Step::Compile) total = p.total, done = p.done;
        };
        std::thread worker([&] {
            try {
                const jit::Report report = jit::prepare(m3::games::kSpikeoutFinalEdition, roms, jo);
                jit_cache_in_use = report.cache_path;
            } catch (const jit::Cancelled&) {
            } catch (const std::exception& e) {
                error = e.what();
            }
            finished = true;
        });
        const auto began = std::chrono::steady_clock::now();
        bool quit = false;
        while (!finished) {
            for (SDL_Event e; SDL_PollEvent(&e);)
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true, cancel = true;
            if (!quit && std::chrono::steady_clock::now() - began > std::chrono::milliseconds(300)) {
                // (the compile is nearly all of the time: the analysis and IR take the first 10%, linking the last 2%)
                const unsigned n = total, d = done;
                const auto s = static_cast<jit::Step>(step.load());
                const float share = s == jit::Step::Compile && n ? 0.10f + 0.88f * static_cast<float>(d) / static_cast<float>(n)
                                    : s == jit::Step::Load   ? 0.98f
                                    : s == jit::Step::Ir     ? 0.05f
                                                             : 0.0f;
                const std::string doing = s == jit::Step::Compile ? "Part " + std::to_string(d) + " of " + std::to_string(n)
                                          : s == jit::Step::Load  ? "Starting..."
                                                                  : "Reading the game's code...";
                screen.preparing(share, doing);
            }
            SDL_Delay(16);
        }
        worker.join();
        if (quit) {
            screen.close();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        if (!error.empty()) {
            std::fprintf(stderr, "spikeout: the game couldn't be translated: %s\n", error.c_str());
            const std::string text = "The game couldn't be translated from the ROM set:\n" + error;
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Spikeout", text.c_str(), window);
            screen.close();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
#endif
        screen.close(); // (the window given to the GPU)
    }
    SDL_ShowWindow(window);
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
        else { // (the owner's choice, 2026-11-02: a device without one doesn't run the game; --no-gpu still draws without)
            const std::string why = gpu_device->error();
            std::printf("spikeout: no GPU (%s)\n", why.c_str());
            presenter.reset(); // (the window given back, for SDL's 2D renderer)
            gpu_device.reset();
            show_no_gpu(window);
            return 1;
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
    shared.roms = &roms;
    shared.max_frames = max_frames;
    shared.record_path = record_path;
    shared.load_path = shared.last_state = load_state;
    shared.real_netboard = real_netboard;
    shared.old_link = old_link;
    shared.net_save_key = net_save_key;
    shared.one_board = net_one_board;
    shared.rollback_frames = shared.rollback_wanted = net_rollback;
    shared.fog_over_nothing = fog_over_nothing;
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
        const bool want = on_gpu && (boards == 1 || shared.netplay) && settings.gpu_3d; // (network play: the local board)
        const int s = wanted_scale(), e = want ? wanted_extend() : 0;
        drawn_number = 0;
        if (e != shared.extend || s != draw_scale) { // (a log of what the player changed, for reports)
            std::size_t board = shared.netplay ? shared.local : 0;
            std::printf("spikeout: picture shape %d, %d pixels each side, render scale %d (board %zu at frame %llu)\n",
                        settings.picture_shape, e, s, board + 1,
                        static_cast<unsigned long long>(shared.screens[board].frame_number));
            std::fflush(stdout);
        }
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
            menu.set_footer(std::string("Spikeout: Final Edition recomp ") + net::kVersion);
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
    std::vector<tools::GameThread> games;
    auto start_games = [&] {
        ring.reset();
        if (boards > 1) ring = std::make_unique<m3::LinkRing>(boards);
        shared.ring = ring.get();
        if (shared.netplay) { // network play: the boards at once, the link's packets a slice late (as gamerun --parallel)
            ring->set_deferred(true);
            ring->set_latency((recomp::FrameTiming().frame_cycles() * kTurnLines + recomp::FrameTiming::kLines - 1) /
                              recomp::FrameTiming::kLines);
            // Every board's state: saved when asked for, sent once the last has
            // saved (all stopped: where the slices meet); or the host's, run on from.
            tools::LinkedState& linked = shared.linked;
            linked.ring = ring.get();
            linked.boards.assign(boards, {});
            linked.loaded.clear();
            linked.capture_frame = 0, linked.saved = 0, linked.written = false;
            linked.cut.clear();
            linked.sink = [&](uint64_t frame, std::vector<uint8_t> state) {
                std::vector<uint8_t> packed = net::compress(state);
                std::printf("spikeout: every board's state at frame %llu: %zu KB packed\n", static_cast<unsigned long long>(frame),
                            packed.size() / 1024);
                shared.netplay->session().send_state(frame, std::move(packed));
            };
            if (!shared.net_state.empty()) {
                try {
                    m3::State s(std::move(shared.net_state));
                    linked.io(s);
                    linked.loaded = std::move(linked.boards);
                } catch (const std::exception& e) {
                    std::printf("spikeout: the host's state: %s\n", e.what());
                }
                linked.boards.assign(boards, {});
                shared.net_state.clear();
            }
            // (rollback: every board's snapshots and the ring's cuts; each board keeps its state where it starts first;
            // the window this machine's memory can keep, tools::rollback::fit_window)
            shared.rollback_frames = tools::rollback::fit_window(shared.rollback_wanted, boards, tools::rollback::free_for_window());
            if (shared.netplay && shared.rollback_frames < shared.rollback_wanted)
                std::printf("spikeout: rollback over %llu frames, not %llu: what this machine's memory can keep\n",
                            (unsigned long long)shared.rollback_frames, (unsigned long long)shared.rollback_wanted);
            shared.rollback = shared.netplay && shared.rollback_frames && !shared.spectating // (a spectator: inputs all in)
                                  ? std::make_unique<tools::rollback::Linked>(boards, shared.rollback_frames, ring.get(), *shared.inputs)
                                  : nullptr;
            turns = std::make_unique<tools::Turns>(boards, true, [&] {
                ring->exchange();
                uint64_t back = 0;
                if (shared.rollback) { // (the ring's part of the frame every board saved; back, if a guess was wrong)
                    const bool capturing = !shared.linked.boards.empty() && shared.linked.saved == shared.linked.boards.size();
                    back = shared.rollback->between(capturing ? &shared.linked.cut : nullptr);
                }
                shared.linked.finish();
                // (one board, wanted: set up here, every board stopped; not while their states are being saved; rollback:
                // not where going back, and only on a frame that won't be run again, every input of it in)
                const auto settled = [&] {
                    if (!shared.rollback) return true;
                    const uint64_t frame = shared.one_board_parts[shared.local] ? shared.one_board_parts[shared.local]->timing.frames() : 0;
                    return back == 0 && frame >= shared.rollback_from && shared.inputs->all_set(frame, shared.boards) &&
                           shared.inputs->wrong_from() > frame;
                };
                // (wanted: no guesses meanwhile, so that a frame settles soon: running ahead on guesses, none would)
                if (shared.one_board_game && shared.one_board_wanted && shared.rollback) shared.one_board_game->hold_guesses(~0ull);
                if (shared.one_board_game && shared.one_board_wanted && shared.linked.capture_frame == 0 && settled())
                    set_up_one_board(shared);
                // (every board again: once the other's shadow runs, the module gone)
                // (tests: SPIKEOUT_TEST_SWITCH_MS=N, a spectator's switch held that long, as a slow device's: its cover seen)
                static const long test_switch_ms = std::getenv("SPIKEOUT_TEST_SWITCH_MS") ? std::atol(std::getenv("SPIKEOUT_TEST_SWITCH_MS")) : 0;
                const bool switch_held = test_switch_ms > 0 && shared.switch_started &&
                                         std::chrono::steady_clock::now() - shared.switch_started_at < std::chrono::milliseconds(test_switch_ms);
                if (shared.one_board_game && shared.one_board_back && !switch_held && shared.one_board_game->tear_down()) {
                    shared.one_board_back = false;
                    shared.one_board_frame = ~0ull; // (every board here: the status list's, a spectator's switching)
                    if (const std::size_t to = shared.switch_to; to != ~std::size_t{0}) {
                        // (a spectator's switch: the new player's board whole now: shown, and staying from the next
                        // meeting, the one shown before going (Game::again: the module's last set-up let go))
                        shared.one_board_game->again();
                        shared.local = to, shared.watched = to;
                        shared.one_board_wanted = true;
                        shared.switch_to = ~std::size_t{0}, shared.switch_started = false;
                        std::printf("spikeout: watching seat %zu, every cabinet whole: one board again around it\n", to + 1);
                    } else {
                        std::printf("spikeout: every board here again (the other cabinet run whole), as plain network play\n");
                        if (shared.spectating) shared.netplay->session().set_watch_one_board(false); // (no backups wanted now)
                    }
                }
            });
        } else {
            turns = std::make_unique<tools::Turns>(boards);
        }
        // One board a PC: a run's own (any number of players: the rules follow each other cabinet, its shadows from its
        // machine's real board); --net-one-board's wanted at once
        shared.one_board_game.reset();
        shared.one_board_parts = {}, shared.one_board_wanted = false, shared.one_board_back = false, shared.one_board_frame = ~0ull;
        shared.backup_bases.clear(); // (every machine sent its whole first again)
        if (shared.netplay && shared.one_board != Shared::OneBoard::never && boards >= 2) { // (a spectator's too: by its power)
            shared.one_board_game = std::make_unique<tools::one_board::Game>();
            shared.one_board_game->set_turns(turns.get()); // (a shadow's hand-back waited for at the meetings)
            shared.one_board_wanted = shared.one_board == Shared::OneBoard::always;
        }
        for (std::size_t i = 0; i < boards; ++i)
            games.emplace_back([&, i] { run_game(roms, i, ring.get(), *turns, shared); });
    };
    auto stop_games = [&] {
        shared.quit = true;
        if (shared.inputs) shared.inputs->close(); // (a board waiting for another player's inputs)
        for (tools::GameThread& g : games) g.join();
        games.clear();
    };
    // The boards' EEPROM and backup RAM from the last run (not for test runs).
    const std::string data_folder = tools::data_folder();
    if (!test_run) load_nvram(data_folder, shared, boards);
    start_games();

    // Pads: players 1 and 2 each the one chosen in Controls (settings.pads, by GUID) when it's connected, else the
    // first connected that no player has.
    std::vector<SDL_Gamepad*> gamepads;
    auto pad_guid = [](SDL_Gamepad* pad) {
        char text[64] = {};
        SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(pad)), text, sizeof text);
        return std::string(text);
    };
    auto player_pads = [&] {
        std::array<SDL_Gamepad*, 2> p{};
        std::vector<bool> taken(gamepads.size());
        for (std::size_t i = 0; i < 2; ++i)
            for (std::size_t g = 0; g < gamepads.size() && !settings.pads[i].empty(); ++g)
                if (!taken[g] && pad_guid(gamepads[g]) == settings.pads[i]) {
                    p[i] = gamepads[g];
                    taken[g] = true;
                    break;
                }
        for (std::size_t i = 0; i < 2; ++i)
            for (std::size_t g = 0; g < gamepads.size() && !p[i]; ++g)
                if (!taken[g]) {
                    p[i] = gamepads[g];
                    taken[g] = true;
                }
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

    // ---- Network play (Mode B, docs/netplay.md) ----
    const std::size_t own_boards = boards; // (--boards: what runs when not in a network game)
    m3::CommBoardOptions link; // (as run_game makes the boards')
    if (shared.old_link) link.use_old_link();
    const uint64_t build = tools::build_id(roms, tools::game_functions().size(), shared.real_netboard ? &link : nullptr);
    std::unique_ptr<NetGame> netgame;
    std::string net_notice; // (a line where the room's was, for a while: the host closed it)
    std::chrono::steady_clock::time_point net_notice_until{};
    std::chrono::steady_clock::time_point watch_banner_until{}; // (a spectator's banner: whom it watches, the keys)
    bool join_shown = false; // (a game joined in progress: the room page closed once for the joining screen)
    // Save states' safeguards (F9 / F10 below): F10's first press arms it a few seconds; after a load, F9 goes back to
    // the game kept just before it (kUndoState) a few seconds. A state checked before anything is reset: read, then
    // loaded into a spare board, all of it (CPU, frame timing, the bus and its devices, the sound board and its music);
    // "" if it will load, else why not, in a few words; `other`: from another version of the game (a format or code
    // that differs, or none said: tools::StateFile::matches), offered with a warning (the owner's choice).
    static constexpr const char* kUndoState = "spikeout-undo.state";
    std::chrono::steady_clock::time_point load_armed_until{}, undo_until{};
    auto check_state = [&](const std::string& path, std::optional<uint64_t>& frame, bool& other) -> std::string {
        tools::StateFile f;
        try {
            f = tools::read_state_file(tools::read_file(path));
        } catch (const std::exception& e) {
            const std::string what = e.what();
            return what.find("damaged") != std::string::npos   ? "THE FILE IS DAMAGED"
                   : what.find("version") != std::string::npos ? "A SAVE FROM A NEWER VERSION"
                                                                : "THE FILE CAN'T BE READ";
        }
        if (f.state.empty()) return "THE FILE CAN'T BE READ";
        other = !f.matches();
        if (f.format) frame = f.frame;
        try {
            const std::vector<recomp::FunctionEntry> functions = tools::game_functions();
            m3::Bus spare(roms);
            if (shared.real_netboard) { // (its network board as run_game gives the boards: the state has its part)
                m3::CommBoardOptions options;
                if (shared.old_link) options.use_old_link();
                spare.use_netboard(std::make_unique<m3::CommBoard>(nullptr, options));
            }
            recomp::Context c(spare, functions);
            recomp::FrameTiming timing;
            c.set_scheduler(&timing);
            // (a spare sound board as the window's (Sound), its music board with it; Sound::state's own part after)
            m3::SoundBoard spare_sound(roms.region(m3::RomRegionId::SoundProgram), roms.region(m3::RomRegionId::SoundSamples));
            tools::attach_music(spare_sound, roms);
            const auto sound_part = [&spare_sound](m3::State& st) {
                spare_sound.state(st);
                double due = 0;
                std::vector<uint8_t> waiting;
                st.io(due), st.io(waiting);
            };
            m3::State s(std::move(f.state));
            tools::board_state(s, c, timing, sound_part);
            if (!f.format) frame = timing.frames();
        } catch (const std::exception& e) {
            std::printf("spikeout: %s doesn't load: %s\n", path.c_str(), e.what());
            return "IT'S DAMAGED, OR MADE BY A VERSION THIS ONE CAN'T READ";
        }
        return {};
    };
    int net_seats = 2;      // a game offered: its players
    bool net_listed = true; // on the internet: in the lobby's list, or by its code only
    int net_where = 0;      // a game offered: 0 this network, 1 the internet (listed), 2 the internet (private)
    int net_delay = cli_net_delay; // input delay in frames (-1: Auto, the default: from the furthest player, kept up in play)
    // The network settings' Auto (net_begin, and the delay in play): no rollback within a LAN's reach (each way, ms),
    // its window otherwise (frames, what memory allows); the delay the frames the furthest player is away past a
    // frame's slack, half of them (two at most) with rollback, which covers the rest.
    static constexpr int kLanOneWayMs = 10;
    static constexpr uint64_t kAutoRollback = 12;
    auto furthest_one_way = [&](std::size_t me, std::size_t seats) {
        int one_way = -1;
        const auto trips = netgame->session().round_trips();
        for (std::size_t s = 0; s < seats && s < trips.size(); ++s)
            if (s != me && trips[s] >= 0) one_way = std::max(one_way, trips[s] / 2);
        return one_way;
    };
    auto auto_delay = [](int one_way, bool rollback) -> uint64_t {
        if (one_way <= 8) return 0;
        const auto frames = static_cast<uint64_t>(std::ceil((one_way - 8) / (1000.0 / recomp::FrameTiming::kRefreshHz)));
        return rollback ? std::min<uint64_t>(2, frames / 2) : frames;
    };
    std::unique_ptr<net::Discovery> discovery; // games on this network, while looked for
    std::unique_ptr<net::LobbyClient> browser; // the lobby's list, while looked at
    std::vector<net::Json> internet_games;
    int internet_from = 0, internet_total = 0; // the lobby's list: the page shown (from this game), and how many there are
    int internet_others = 0;                   // and how many public games are on other versions
    std::vector<net::GameInfo> nearby_games;
    // This computer's cabinet (its first: board 0's settings and backup RAM,
    // as the board has them now), what it brings to a network game; its
    // country, put back when the game's version of it is kept.
    int own_country = 0;
    auto new_netgame = [&] {
        netgame = std::make_unique<NetGame>(build);
        std::vector<uint8_t> eeprom, backup;
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            const Screen& s = shared.screens[0];
            std::vector<uint16_t> words = s.bus ? std::vector<uint16_t>(s.bus->eeprom().words().begin(), s.bus->eeprom().words().end()) : s.eeprom;
            for (uint16_t w : words) eeprom.push_back(static_cast<uint8_t>(w >> 8)), eeprom.push_back(static_cast<uint8_t>(w));
            backup = s.bus ? std::vector<uint8_t>(s.bus->backup_ram().begin(), s.bus->backup_ram().end()) : s.backup;
        }
        auto words = tools::eeprom_words(eeprom);
        const tools::ArcadeSettings own{words};
        own_country = eeprom.size() == 128 && own.valid() ? own.country() : 0;
        netgame->session().set_cabinet(std::move(eeprom), std::move(backup));
    };
    // After a network game: this player's board (its settings, its country
    // back as its own, and its backup RAM: the rankings) kept as this
    // computer's first cabinet, as a cabinet keeps what it played.
    auto keep_seat = [&] {
        if (test_run || !shared.netplay) return;
        if (shared.net_start.state_frame) return; // (a cabinet joined in progress: set up as it was, not from ours: not kept)
        const Screen& s = shared.screens[shared.local];
        Screen& own = shared.screens[0];
        std::vector<uint8_t> bytes;
        for (uint16_t w : s.eeprom) bytes.push_back(static_cast<uint8_t>(w >> 8)), bytes.push_back(static_cast<uint8_t>(w));
        if (own_country) bytes = tools::with_country(bytes, own_country);
        const auto words = tools::eeprom_words(bytes);
        std::vector<uint16_t> eeprom = bytes.size() == 128 ? std::vector<uint16_t>(words.begin(), words.end()) : std::vector<uint16_t>();
        std::vector<uint8_t> backup = s.backup;
        own.eeprom = std::move(eeprom);
        own.backup = std::move(backup);
        save_nvram(data_folder, shared, 1);
        std::printf("spikeout: seat %zu's board kept as this computer's cabinet\n", shared.local + 1);
    };
    auto net_host = [&](bool internet) {
        new_netgame();
        netgame->host = true;
        netgame->internet = internet;
        const std::string name = player_name(settings);
        if (!internet) {
            if (netgame->session().host(name, static_cast<uint8_t>(net_seats))) {
                netgame->set_status("Offered on this network: waiting for players");
                netgame->ready = true;
            } else {
                netgame->fail("Can't offer a game: " + netgame->session().error());
            }
            return;
        }
        netgame->set_status("Connecting to the lobby...");
        NetGame* g = netgame.get();
        g->worker = std::thread([g, name, seats = net_seats, listed = net_listed, build] {
            const auto welcome = tools::lobby_hello(g->lobby, build, name, net::kLobbyUrl);
            if (!welcome) return g->fail("No lobby: " + g->lobby.error());
            g->session().set_country((*welcome)["country"].as_string()); // (each player's flag in the seat list)
            if (!g->session().host(name, static_cast<uint8_t>(seats))) return g->fail("Can't offer a game: " + g->session().error());
            g->set_status("Finding our address on the internet...");
            tools::find_addresses(g->lobby, g->session(), (*welcome)["turn"].as_bool(), {});
            const auto code = tools::lobby_offer(g->lobby, g->session(), name, static_cast<uint8_t>(seats), listed);
            if (!code) return g->fail("The lobby didn't take the game");
            {
                std::lock_guard guard(g->lock);
                g->code = *code;
            }
            g->set_status(std::string(listed ? "Listed on the internet" : "Private") + ": waiting for players");
            g->ready = true;
        });
    };
    auto net_join_nearby = [&](const net::GameInfo& game) {
        new_netgame();
        if (netgame->session().join(game, player_name(settings))) netgame->set_status("Joining " + game.name + "...");
        else netgame->fail("Can't join: " + netgame->session().error());
        netgame->ready = true;
    };
    // Watching (a spectator: docs/netplay.md, "Spectators"): as joining, but no seat.
    auto net_watch_nearby = [&](const net::GameInfo& game) {
        new_netgame();
        netgame->spectator = true;
        if (netgame->session().watch(game, player_name(settings))) netgame->set_status("Watching " + game.name + "...");
        else netgame->fail("Can't watch: " + netgame->session().error());
        netgame->ready = true;
    };
    auto net_by_code = [&](const std::string& code, bool watch) {
        new_netgame();
        netgame->internet = true;
        netgame->spectator = watch;
        netgame->code = code;
        netgame->set_status("Connecting to the lobby...");
        NetGame* g = netgame.get();
        g->worker = std::thread([g, code, watch, name = player_name(settings), build] {
            const auto welcome = tools::lobby_hello(g->lobby, build, name, net::kLobbyUrl);
            if (!welcome) return g->fail("No lobby: " + g->lobby.error());
            g->session().set_country((*welcome)["country"].as_string());
            if (!g->session().listen()) return g->fail(g->session().error());
            g->set_status("Finding our address on the internet...");
            tools::find_addresses(g->lobby, g->session(), (*welcome)["turn"].as_bool(), {});
            std::string error;
            if (!tools::lobby_join(g->lobby, g->session(), build, code, name, error, watch))
                return g->fail(std::string(watch ? "Can't watch " : "Can't join ") + code + ": " + error);
            g->set_status(std::string(watch ? "Watching " : "Joining ") + code + "...");
            g->ready = true;
        });
    };
    auto net_join_code = [&](const std::string& code) { net_by_code(code, false); };
    auto net_watch_code = [&](const std::string& code) { net_by_code(code, true); };
    // The host's start: every board from power-on, each from its player's cabinet.
    auto net_start = [&] {
        if (!netgame || !netgame->host || !netgame->ready) return;
        net::StartInfo info;
        {
            m3::Bus probe(roms);
            info.clock = tools::set_rtc_to_host(probe);
        }
        netgame->session().start(info);
        if (netgame->internet) { // (the seats with players: a game started alone has free ones to join)
            std::size_t taken = 0;
            for (const std::string& name : netgame->session().players()) taken += name.empty() ? 0 : 1;
            netgame->listed_taken = taken;
            net::Json m = net::Json::object();
            m.set("t", "update"), m.set("taken", static_cast<int>(taken)), m.set("started", true);
            netgame->lobby.send(m);
        }
    };
    std::function<void()> set_up_display; // (the 3D renderer for what the window shows: below, with the GPU)
    // The game started (ours or the host's): this window runs all its boards.
    auto net_begin = [&](const net::StartInfo& start) {
        // (the network settings chosen by themselves, nothing to set (the owner's choice, 2026-11-04): rollback for a
        // game through the internet or with a player further than a LAN's; the input delay from the furthest player,
        // kept up to date in play (below); the board mode by this PC's speed, as before. --net-delay N and
        // --net-rollback N force them, for tests.)
        const bool spectator = start.my_seat >= start.seats;
        const int one_way = furthest_one_way(start.my_seat, start.seats);
        if (!net_rollback_forced)
            shared.rollback_wanted = !spectator && (netgame->internet || one_way > kLanOneWayMs) ? kAutoRollback : 0;
        const uint64_t delay = net_delay >= 0 ? static_cast<uint64_t>(net_delay) : auto_delay(one_way, shared.rollback_wanted > 0);
        std::printf("spikeout: network settings: %s, input delay %llu (the furthest player %d ms away one way)\n",
                    shared.rollback_wanted ? ("rollback over " + std::to_string(shared.rollback_wanted) + " frames").c_str()
                                           : "waiting for inputs",
                    static_cast<unsigned long long>(delay), one_way);
        std::vector<uint8_t> state; // (a game joined in progress: every board as the host had them)
        if (start.state_frame) {
            if (auto unpacked = net::decompress(start.state)) state = std::move(*unpacked);
            else std::printf("spikeout: the host's state doesn't unpack\n");
        }
        reset_game([&] {
            shared.netplay = netgame->play.get();
            // (tests: SPIKEOUT_TEST_HOLD_INPUTS=N, this machine's inputs sent N frames' time late, as gamerun
            // --net-hold-inputs: the others go on on guesses meanwhile, with rollback)
            if (const char* hold = std::getenv("SPIKEOUT_TEST_HOLD_INPUTS"))
                shared.netplay->session().hold_inputs(std::strtoull(hold, nullptr, 0));
            shared.inputs = &netgame->table;
            shared.spectating = start.my_seat >= start.seats; // (a spectator: no seat; the first board shown)
            shared.local = shared.spectating ? 0 : start.my_seat;
            shared.watched = shared.local;
            shared.net_start = start;
            shared.input_delay = delay;
            shared.net_state = std::move(state);
            shared.catching_up = start.state_frame.has_value();
            shared.catch_up_behind = 0, shared.catch_up_start = 0;
            shared.rollback_from = start.state_frame ? ~0ull : 0; // (joined in progress: none until caught up)
            boards = start.seats;
            shared.boards = boards;
            focus = shared.local;
        });
        netgame->running = true;
        if (shared.spectating)
            std::printf("spikeout: watching a network game of %u cabinets%s\n", start.seats,
                        start.state_frame ? " (in progress)" : "");
        else
            std::printf("spikeout: network game, seat %u of %u, input delay %llu%s\n", start.my_seat + 1, start.seats,
                        static_cast<unsigned long long>(delay), start.state_frame ? " (joined in progress)" : "");
        if (set_up_display) set_up_display();
    };
    // Ours out of step with the host's boards: every board again from its
    // state, the inputs since fed again, run unpaced until caught up.
    auto net_resync = [&](const net::StartInfo& again) {
        auto unpacked = net::decompress(again.state);
        if (!unpacked || !again.state_frame) return;
        reset_game([&] {
            shared.net_state = std::move(*unpacked);
            netgame->table.reset();
            netgame->session().feed_inputs(*again.state_frame);
            shared.rollback_from = ~0ull; // (rollback: none until caught up)
            shared.catching_up = true;
        });
        std::printf("spikeout: put back in step with the host (frame %llu)\n", static_cast<unsigned long long>(*again.state_frame));
    };
    // Leaving: back to this computer's own cabinets, as they were.
    // (one board here: its state to the others first, so that each runs this cabinet on whole; a few seconds at most)
    auto parting = [&] {
        if (!netgame || !netgame->running || !shared.one_board_game || shared.one_board_frame == ~0ull) return;
        shared.parting_wanted = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!shared.parted && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        while (netgame->session().sending() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::printf("spikeout: %s\n", shared.parted ? "this board's state sent, leaving" : "leaving without this board's state sent");
        shared.parting_wanted = false, shared.parted = false, shared.parting_frame = 0;
    };
    auto net_leave = [&] {
        if (!netgame) return;
        parting();
        netgame->session().leave();
        if (netgame->running)
            reset_game([&] {
                if (!shared.spectating) keep_seat(); // (this player's board, as this computer's cabinet: while still in the game)
                shared.spectating = false, shared.watched = 0;
                shared.netplay = nullptr;
                shared.inputs = nullptr;
                shared.catching_up = false; // (left before catching up: paced again)
                boards = own_boards;
                shared.boards = boards;
                focus = 0;
                if (!test_run) load_nvram(data_folder, shared, boards);
            });
        netgame.reset();
        if (set_up_display) set_up_display();
    };
#ifdef SPIKEOUT_GPU
    set_up_display = set_up_3d;
#endif

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
        p.items.push_back({"Network status",
                           "In a network game, each seat under the frame rate: the players, their connection (bars "
                           "and round trip), and who joined, left or dropped.",
                           [&] { return on_off(settings.show_net); },
                           [&](int) {
                               settings.show_net = !settings.show_net;
                               changed();
                           }});
        p.items.push_back(back_item());
        return p;
    };
    // The outputs opened again for the speaker settings (here, not in the
    // page: its items run it after the page's making has returned).
    auto reopen_sound = [&] {
        if (sound) sound->open(speaker_setup());
        changed();
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
        // (The items keep a copy of `channels`: they're used after this page's
        // making has returned, when a setting changes.)
        static constexpr const char* kSpeakerNames[] = {"Stereo", "Four channels", "Two outputs"};
        p.items.push_back(
            {"Speakers",
             "The cabinet's front pair (first sound chip, music) and rear pair (second chip). Stereo mixes them. Four "
             "channels: both pairs to one output, for quad, 5.1 or 7.1 speakers set up in your system (else mixed to stereo). "
             "Two outputs: each pair to an output of its own.",
             [&, channels] {
                 std::string v = kSpeakerNames[settings.speakers];
                 if (settings.speakers == 1) {
                     const int c = channels(settings.front_output);
                     if (c > 0 && c < 4) v += " (" + std::to_string(c) + " here: mixed)";
                 }
                 return v;
             },
             [&](int d) {
                 settings.speakers = (settings.speakers + d + 3) % 3;
                 reopen_sound();
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
                                    if (settings.speakers != 0) reopen_sound();
                                }};
        };
        p.items.push_back(output_item("Front output", "Where the front pair (or all four channels) goes.", settings.front_output));
        p.items.push_back(output_item("Rear output", "Where the rear pair goes, with front and rear on two outputs.",
                                      settings.rear_output));
        p.items.push_back(back_item());
        return p;
    };
    // Controls: one page for a player (the menu shows player 1's only; player 2's settings stay, for a second cabinet
    // with --boards 2 or several pads on one computer later). The device at the top picks whose buttons the rows
    // show and set: the keyboard or a connected pad (choosing a pad makes it the player's). Both always play.
    SDL_JoystickID controls_device = 0; // (0: the keyboard)
    auto device_pad = [&]() -> SDL_Gamepad* {
        for (SDL_Gamepad* pad : gamepads)
            if (SDL_GetGamepadID(pad) == controls_device) return pad;
        controls_device = 0; // (unplugged: back to the keyboard)
        return nullptr;
    };
    auto controls_page = [&](std::size_t player) {
        ui::MenuPage p{"Controls", "Enter: change. Delete: none.", {}};
        p.items.push_back(
            {"Device",
             "The keyboard or a pad: whose buttons to set below. Both play; the pad chosen here is this player's. A pad's "
             "left stick moves too, and Guide (or Back + Start) opens the menu.",
             [&] {
                 SDL_Gamepad* pad = device_pad();
                 if (pad) {
                     const char* name = SDL_GetGamepadName(pad);
                     return std::string(name ? name : "Pad");
                 }
                 return std::string(gamepads.empty() ? "Keyboard (no pad connected)" : "Keyboard");
             },
             [&, player](int d) {
                 device_pad();
                 // The keyboard, then each connected pad, round.
                 const auto n = static_cast<int>(gamepads.size()) + 1;
                 int at = 0;
                 for (std::size_t g = 0; g < gamepads.size(); ++g)
                     if (SDL_GetGamepadID(gamepads[g]) == controls_device) at = static_cast<int>(g) + 1;
                 at = (at + d + n) % n;
                 controls_device = at == 0 ? 0 : SDL_GetGamepadID(gamepads[static_cast<std::size_t>(at - 1)]);
                 if (at > 0) {
                     settings.pads[player] = pad_guid(gamepads[static_cast<std::size_t>(at - 1)]);
                     changed();
                 }
                 menu.refresh();
             }});
        for (std::size_t a = 0; a < tools::kActions; ++a) {
            p.items.push_back(
                {tools::kActionNames[a], "",
                 [&, player, a] {
                     if (SDL_Gamepad* pad = device_pad()) return pad_input_name(settings.pad_buttons[player][a], pad);
                     const int key = settings.keys[player][a];
                     return key > 0 ? std::string(SDL_GetScancodeName(static_cast<SDL_Scancode>(key))) : std::string("None");
                 },
                 {},
                 [&, player, a] {
                     const std::string action = tools::kActionNames[a];
                     if (!device_pad()) {
                         menu.capture("Press a key for " + action + " (Esc: cancel)", [&, player, a](const SDL_Event& e) {
                             if (e.type != SDL_EVENT_KEY_DOWN) return false;
                             settings.keys[player][a] = e.key.scancode;
                             changed();
                             return true;
                         });
                         return;
                     }
                     const SDL_JoystickID id = controls_device;
                     menu.capture("Press a button for " + action + " (Guide or Esc: cancel)",
                                  [&, player, a, id](const SDL_Event& e) {
                                      int code = 0;
                                      if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.which == id) {
                                          if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) return true; // (cancelled)
                                          code = tools::pad_button(static_cast<SDL_GamepadButton>(e.gbutton.button));
                                      } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.which == id &&
                                                 (e.gaxis.value > 24000 || e.gaxis.value < -24000)) {
                                          code = tools::pad_axis(static_cast<SDL_GamepadAxis>(e.gaxis.axis), e.gaxis.value > 0);
                                      }
                                      if (code == 0) return false;
                                      settings.pad_buttons[player][a] = code;
                                      changed();
                                      return true;
                                  });
                 },
                 false,
                 [&, player, a] {
                     if (device_pad()) settings.pad_buttons[player][a] = 0;
                     else settings.keys[player][a] = 0;
                     changed();
                 }});
        }
        p.items.push_back({"Reset to defaults", "This device's buttons as they were.", {}, {}, [&, player] {
                               if (device_pad()) settings.pad_buttons[player] = tools::Settings::defaults().pad_buttons[player];
                               else settings.keys[player] = tools::Settings::defaults().keys[player];
                               changed();
                               menu.refresh();
                           }});
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
    // The ROM set's location, changed from the menu: the system's dialog (its answer comes on another thread, taken
    // in the loop below: checked, kept in the settings, used from the next start).
    struct RomPick {
        std::mutex lock;
        std::optional<std::string> path; // (empty: cancelled)
        std::string status;
        std::optional<std::string> jit_folder; // (the JIT build: the translation's folder chosen; empty: cancelled)
        std::string jit_status;
    } rom_pick;
    auto rom_dialog = [&] {
        auto answer = +[](void* u, const char* const* files, int) {
            auto* pick = static_cast<RomPick*>(u);
            std::lock_guard guard(pick->lock);
            pick->path = files && files[0] ? std::string(files[0]) : std::string();
        };
        static const SDL_DialogFileFilter kZip[] = {{"Zip files", "zip"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(answer, &rom_pick, window, kZip, 2, nullptr, false);
    };
    auto rom_page = [&] {
        ui::MenuPage p{"ROM set", "", {}};
        p.items.push_back({"In use", "", [&] {
                               if (!rom_pick.status.empty()) return rom_pick.status;
                               return path_utf8(utf8_path(rom_in_use).filename());
                           },
                           {}, {}, true});
        p.items.push_back({"Choose the zip file", "spikeofe.zip, as MAME and Supermodel have it. Used from the next start.", {},
                           {}, [&] { rom_dialog(); }});
#ifdef SPIKEOUT_JIT
        // The game translated from the set at the first start, kept in a folder: its size, the folder, and another
        // chosen (used from the next start, translated there again).
        p.items.push_back({"Translated game", "", [&] {
                               if (!rom_pick.jit_status.empty()) return rom_pick.jit_status;
                               if (jit_cache_in_use.empty()) return std::string("Not kept");
                               std::error_code ec;
                               std::uintmax_t bytes = 0;
                               for (const auto& f : std::filesystem::directory_iterator(jit_cache_in_use, ec))
                                   if (f.is_regular_file(ec)) bytes += f.file_size(ec);
                               return std::to_string((bytes + 500000) / 1000000) + " MB";
                           },
                           {}, {}, true});
        p.items.push_back({"Choose its folder",
                           "Where the game translated from the ROM set is kept (about 12 MB)" +
                               (jit_cache_in_use.empty() ? std::string()
                                                         : ", now " + short_path(jit_cache_in_use.parent_path())) +
                               ". Used from the next start, translated there again.",
                           {}, {}, [&] {
                               auto answer = +[](void* u, const char* const* files, int) {
                                   auto* pick = static_cast<RomPick*>(u);
                                   std::lock_guard guard(pick->lock);
                                   pick->jit_folder = files && files[0] ? std::string(files[0]) : std::string();
                               };
                               SDL_ShowOpenFolderDialog(answer, &rom_pick, window, nullptr, false);
                           }});
#endif
        p.items.push_back(back_item());
        return p;
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
        p.items.push_back({"ROM set", "The game's ROM set, spikeofe.zip: which one is used.", {}, {},
                           [&] { menu.push(rom_page()); }});
        if (net::UpdateCheck::available())
            p.items.push_back({"Check for updates",
                               "At each start, asks GitHub whether a newer version is out, and says so in this menu (the game "
                               "doesn't update itself). This is version " + std::string(net::kVersion) + ".",
                               [&] { return on_off(settings.check_updates); },
                               [&](int) {
                                   settings.check_updates = !settings.check_updates;
                                   changed();
                               }});
        p.items.push_back(back_item());
        return p;
    };
    // Network play: a game offered or joined, as it gathers its players.
    std::function<ui::MenuPage()> make_root; // (the main menu: root_page, below)
    auto lobby_page = [&] {
        ui::MenuPage p{"Network game", "", {}};
        p.items.push_back({"Status", "", [&] { return netgame ? netgame->get_status() : std::string("Left"); }, {}, {}, true});
        if (netgame && netgame->spectator) { // (watching: who plays, whose screen this is, and stopping)
            for (std::size_t s = 0; s < net::kMaxSeats; ++s)
                p.items.push_back({"Seat " + std::to_string(s + 1), "Left / Right (a pad's shoulders) change whose screen you watch.",
                                   [&, s] {
                                       if (!netgame) return std::string();
                                       const auto seats = netgame->session().seats();
                                       using State = net::Session::SeatView::State;
                                       if (seats[s].state == State::none) return std::string("-");
                                       const std::string who = seats[s].name.empty() ? std::string("(free)") : seats[s].name;
                                       return who + (netgame->running && shared.spectating && shared.local == s ? "  (watching)" : "");
                                   },
                                   {}, {}, true});
            p.items.push_back({"Stop watching", "Leave the game you watch.", {}, {}, [&] {
                                   net_leave();
                                   menu.open(make_root());
                               }});
            return p;
        }
        if (netgame && netgame->internet)
            p.items.push_back({"Game code", "The others join with this code (Network play, Join on the internet).",
                               [&] {
                                   std::lock_guard guard(netgame->lock);
                                   return netgame->code.empty() ? std::string("...") : netgame->code;
                               },
                               {}, {}, true});
        for (std::size_t s = 0; s < net::kMaxSeats; ++s)
            p.items.push_back({"Seat " + std::to_string(s + 1), "Seat 1 is the host's cabinet, the link's master.",
                               [&, s] {
                                   if (!netgame) return std::string();
                                   const auto players = netgame->session().players();
                                   if (s >= players.size()) return std::string("-");
                                   return players[s].empty() ? std::string("(free)") : players[s];
                               },
                               {}, {}, true});
        const bool started = netgame && netgame->running;
        if (netgame && netgame->host && !started) // (once it has started, nothing to start)
            p.items.push_back({"Start game",
                               "Every cabinet from power-on, each player's from their own cabinet (the host's country). "
                               "Alone too: the empty cabinets stand in attract mode until players join the game in progress.",
                               {}, {}, [&] {
                                   if (!netgame || netgame->session().running()) return;
                                   std::size_t present = 0;
                                   for (const std::string& name : netgame->session().players()) present += name.empty() ? 0 : 1;
                                   if (netgame->session().ready_players() >= present) net_start(); // (each player's cabinet in)
                               }});
        // What leaving does here: a room not started (the host's closes it), or
        // a game (the others go on, this player's character standing idle).
        const bool host = netgame && netgame->host;
        p.items.push_back({started ? "Leave game" : host ? "Close room" : "Leave room",
                           started ? "Leave the game: the others go on, your character standing idle; your own cabinet "
                                     "comes back here."
                           : host  ? "Close the room: nobody can join it any more."
                                   : "Leave the room, before the game starts.",
                           {}, {}, [&] {
                               net_leave();
                               menu.open(make_root()); // (out of the room: the main menu as it is now)
                           }});
        return p;
    };
    // Into a room (hosted or joined): its page straight over the main menu, so
    // Back from it never comes to Network play's Host / Join (which would
    // start another room, closing this one).
    // The main menu made again (it shows "Your room" in a room only).
    auto to_room = [&] {
        menu.open(make_root());
        menu.push(lobby_page());
    };
    // Looking for games on the networks this computer is on (a broadcast; no
    // internet), from when the Network play page opens.
    auto look_nearby = [&] {
        if (discovery) return;
        discovery = std::make_unique<net::Discovery>();
        if (!discovery->open()) std::printf("spikeout: can't look for games: %s\n", discovery->error().c_str());
    };
    // A game's code: typed (or pasted, Ctrl+V), or picked from an on-screen
    // keyboard with a pad or the mouse (ui::CodeInput). The lobby's codes
    // leave out I, O, 0 and 1 (too like each other).
    // (`watch`: to watch it, a spectator)
    auto code_page = [&](bool watch) {
        ui::MenuPage p{watch ? "Watch by code" : "Join by code",
                       std::string("Type the code the host's lobby page shows (Ctrl+V pastes it), or pick its letters below. "
                                   "Backspace / Y: delete. Enter / Start: ") +
                           (watch ? "watch." : "join."),
                       {}};
        p.code = std::make_shared<ui::CodeInput>();
        p.code->alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        p.code->length = 6;
        if (watch) p.code->done_label = "WATCH";
        p.code->done = [&, watch](const std::string& code) {
            if (watch) net_watch_code(code);
            else net_join_code(code);
            to_room();
        };
        return p;
    };
    // A listed game chosen: join it (a cabinet free), or watch it (fewer than net::kMaxWatchers watching).
    auto game_choice = [&](const std::string& name, bool can_join, bool can_watch, std::function<void()> join,
                           std::function<void()> watch) {
        ui::MenuPage p{name, "", {}};
        if (can_join) p.items.push_back({"Join", "Play in this game: a free cabinet is yours.", {}, {}, [join] { join(); }});
        else p.items.push_back({"Join", "", [] { return std::string("Full"); }, {}, {}, true});
        if (can_watch)
            p.items.push_back({"Watch", "Watch the players, about 2 seconds behind them, any player's screen (Left / Right "
                                        "change it). Nobody waits for you.",
                               {}, {}, [watch] { watch(); }});
        else p.items.push_back({"Watch", "", [] { return std::to_string(net::kMaxWatchers) + " watching already"; }, {}, {}, true});
        p.items.push_back(back_item());
        menu.push(std::move(p));
    };
    // Every game there is to join, in one list (the owner's choice): this
    // network's first (found by its broadcasts, no internet), then the
    // internet lobby's listed ones (private ones: Join by code). A game in
    // both (the same session) is shown once, nearby.
    std::function<ui::MenuPage()> join_page = [&] {
        ui::MenuPage p{"Join a game", "Games on this network and on the internet.", {}};
        look_nearby();
        nearby_games = discovery->poll();
        const char* const kInProgress = "Join this game in progress: an empty cabinet is yours once you've caught up.";
        auto label = [](const std::string& name, int taken, int seats, const std::string& where, bool playing) {
            return name + "  (" + std::to_string(taken) + "/" + std::to_string(seats) + ", " + where + (playing ? ", playing)" : ")");
        };
        for (const net::GameInfo& g : nearby_games) {
            const std::string text = label(g.name, g.taken, g.seats, "nearby", g.started);
            if (g.build != build) {
                p.items.push_back({text, "Another version of the game, or another ROM set: players need the same ones to play together.", [] { return std::string(); }, {}, {}, true});
            } else {
                ui::MenuItem it{text, g.taken >= g.seats ? "Full: watch it." : g.started ? kInProgress : "Join or watch this game (on this network).",
                                {}, {}, [&, g] {
                                    game_choice(g.name, g.taken < g.seats, g.watchers < net::kMaxWatchers,
                                                [&, g] { net_join_nearby(g), to_room(); },
                                                [&, g] { net_watch_nearby(g), to_room(); });
                                }};
                it.watchers = g.watchers; // (an eye and the count)
                p.items.push_back(std::move(it));
            }
        }
        std::string lobby_note;
        if (net::LobbyClient::available()) {
            if (!browser) {
                browser = std::make_unique<net::LobbyClient>();
                browser->connect(tools::hex64(build), player_name(settings), net::kLobbyUrl);
            }
            // A page of the lobby's list (it sends 50 at a time: waiting games
            // first, then those in progress, the newest first in each).
            net::Json list = net::Json::object();
            list.set("t", "list"), list.set("from", internet_from);
            browser->send(list);
            if (auto answer = browser->wait_for("games", 3000); answer && (*answer)["t"].as_string() == "games") {
                internet_games = (*answer)["games"].items();
                internet_total = static_cast<int>((*answer)["total"].as_number()); // (0 from an older lobby: no pages)
                internet_others = static_cast<int>((*answer)["others"].as_number(0));
            }
            if (browser->failed()) lobby_note = "No internet lobby: " + browser->error();
            for (const net::Json& g : internet_games) {
                const std::string name = g["name"].as_string();
                const std::string session = g["session"].as_string(); // (hex; none from an older lobby)
                if (!session.empty() && std::any_of(nearby_games.begin(), nearby_games.end(), [&](const net::GameInfo& n) {
                        return tools::hex64(n.session) == session;
                    }))
                    continue; // (shown nearby)
                const std::string code = g["code"].as_string();
                const bool playing = g["started"].as_bool();
                std::string country = g["country"].as_string(); // (the host's, as Cloudflare sees it: its flag)
                for (char& c : country) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const int taken = static_cast<int>(g["taken"].as_number()), seats = static_cast<int>(g["seats"].as_number());
                const int watchers = static_cast<int>(g["watchers"].as_number(0));
                ui::MenuItem it{label(name, taken, seats, "internet", playing),
                                std::string(taken >= seats ? "Full: watch it." : playing ? kInProgress : "Join or watch this game (through the internet).") +
                                    " The speed (~) is an estimate, from how far away its host is; the real one shows in the game.",
                                {}, {}, [&, code, name, taken, seats, watchers] {
                                    game_choice(name, taken < seats, watchers < static_cast<int>(net::kMaxWatchers),
                                                [&, code] { net_join_code(code), to_room(); },
                                                [&, code] { net_watch_code(code), to_room(); });
                                }};
                it.country = country;
                it.watchers = watchers;
                it.estimate_ms = static_cast<int>(g["ms"].as_number(-1)); // (-1: the lobby doesn't know where one of you is)
                p.items.push_back(std::move(it));
            }
        }
        if (p.items.empty()) p.items.push_back({"No games found yet", "", {}, {}, {}, true});
        if (internet_others > 0) {
            // Games the lobby has on other versions: not listed, but said, so a player whose friends updated knows why.
            std::string help = "Their players have another version of the game (this is " + std::string(net::kVersion) +
                               "): players need the same one to play together.";
            if (newer_version) help += " Version " + newer_version->version + " is out: Enter opens its page.";
            p.items.push_back({std::to_string(internet_others) + (internet_others == 1 ? " game" : " games") + " on another version",
                               help, {}, {}, [&] {
                                   if (newer_version) SDL_OpenURL(newer_version->page.c_str());
                               }});
        }
        if (!lobby_note.empty()) p.items.push_back({lobby_note, "", {}, {}, {}, true});
        constexpr int kPage = 50; // (the lobby's)
        if (internet_from + kPage < internet_total)
            p.items.push_back({"More games (" + std::to_string(internet_from + kPage + 1) + "-" +
                                   std::to_string(std::min(internet_from + 2 * kPage, internet_total)) + " of " +
                                   std::to_string(internet_total) + ")",
                               "The lobby's next games.", {}, {}, [&] {
                                   internet_from += kPage;
                                   menu.pop();
                                   menu.push(join_page());
                               }});
        if (internet_from > 0)
            p.items.push_back({"Earlier games", "The lobby's games before these.", {}, {}, [&] {
                                   internet_from = std::max(0, internet_from - kPage);
                                   menu.pop();
                                   menu.push(join_page());
                               }});
        if (net::LobbyClient::available()) {
            p.items.push_back({"Join by code", "A private internet game, or one you were told the code of.", {}, {},
                               [&] { menu.push(code_page(false)); }});
            p.items.push_back({"Watch by code", "Watch a private internet game, or one you were told the code of.", {}, {},
                               [&] { menu.push(code_page(true)); }});
        }
        p.items.push_back({"Refresh", "Look again (a game nearby shows within a second or two).", {}, {}, [&] {
                               menu.pop();
                               menu.push(join_page());
                           }});
        p.items.push_back(back_item());
        return p;
    };
    // Offering a game: how many cabinets, and where (this network, or the
    // internet lobby: in its list, or only by its code).
    auto host_page = [&] {
        ui::MenuPage p{"Host a game", "", {}};
        p.items.push_back({"Players", "How many cabinets your game has (one for each player).",
                           [&] { return std::to_string(net_seats); },
                           [&](int d) { net_seats = std::clamp(net_seats + d, 2, static_cast<int>(net::kMaxSeats)); }});
        if (net::LobbyClient::available())
            p.items.push_back({"Where",
                               "This network: computers on your network find it (no internet needed). Internet (listed): in "
                               "the lobby's list for anyone. Internet (private): only by its code.",
                               [&] {
                                   return std::string(net_where == 0 ? "This network" : net_where == 1 ? "Internet (listed)"
                                                                                                       : "Internet (private)");
                               },
                               [&](int d) { net_where = (net_where + d + 3) % 3; }});
        p.items.push_back({"Start hosting", "Offer the game; it starts when you choose (players can also join it later).", {}, {},
                           [&] {
                               net_listed = net_where != 2;
                               net_host(net_where != 0 && net::LobbyClient::available());
                               to_room();
                           }});
        p.items.push_back(back_item());
        return p;
    };
    // The name the others see: typed, or picked like a game's code (the
    // owner's choice: up to 10 of A-Z, 0-9, space, - _ .).
    auto name_page = [&] {
        ui::MenuPage p{"Player name",
                       "The name the other players see (up to 10: A-Z, 0-9, space, - _ .). Type it, or pick its letters "
                       "below. Backspace / Y: delete. Enter / Start: OK.",
                       {}};
        p.code = std::make_shared<ui::CodeInput>();
        p.code->alphabet = tools::kNameLetters;
        p.code->length = tools::kNameLength;
        p.code->min_length = 1;
        p.code->done_label = "OK";
        p.code->text = player_name(settings);
        p.code->done = [&](const std::string& name) {
            if (const std::string clean = tools::clean_name(name); !clean.empty()) {
                settings.player_name = clean;
                changed();
            }
            menu.pop();
        };
        return p;
    };
    auto network_page = [&] {
        ui::MenuPage p{"Network play",
                       "Each player on their own computer: every computer runs every cabinet, only the controls go between them.",
                       {}};
        look_nearby(); // (so the games nearby are found by the time Join a game opens)
        p.items.push_back({"Join a game", "Games on this network and on the internet, or one by its code.", {}, {},
                           [&] {
                               if (netgame) return to_room(); // (in a room already: that one)
                               internet_from = 0; // (the lobby's first games)
                               menu.push(join_page());
                           }});
        p.items.push_back({"Host a game", "Offer a game on this network or on the internet.", {}, {}, [&] {
                               if (netgame) return to_room(); // (in a room already)
                               menu.push(host_page());
                           }});
        if (netgame) p.items.erase(p.items.begin(), p.items.begin() + 2); // (in a room: no other until it's left)
        p.items.push_back({"Player name", "The name the other players see. Enter: change it.", [&] { return player_name(settings); },
                           {}, [&] { menu.push(name_page()); }});
        // (no input delay, board mode or rollback to set: chosen by themselves, net_begin; the owner's choice)
        p.items.push_back(back_item());
        return p;
    };
    auto root_page = [&] {
        ui::MenuPage p{"", "", {}};
        // In a room (the menu closed with Esc doesn't leave it): first, and
        // selected when the menu opens, so Enter goes back to it.
        if (netgame)
            p.items.push_back({"Your room",
                               "You're still in this network game's room: Enter goes back to it (to start the game, or leave).",
                               [&] {
                                   if (!netgame) return std::string();
                                   if (netgame->spectator)
                                       return netgame->running && shared.spectating ? "Watching " + std::to_string(shared.local + 1) + "P"
                                                                                    : std::string("Watching");
                                   std::size_t taken = 0;
                                   const auto players = netgame->session().players();
                                   for (const std::string& name : players) taken += name.empty() ? 0 : 1;
                                   const std::string count = std::to_string(taken) + "/" + std::to_string(players.size());
                                   if (netgame->running) {
                                       const auto seats = netgame->session().seats();
                                       for (std::size_t s = 0; s < seats.size(); ++s)
                                           if (seats[s].state == net::Session::SeatView::State::me)
                                               return "Playing as " + std::to_string(s + 1) + "P";
                                       return std::string("Playing");
                                   }
                                   return std::string(netgame->host ? "Hosting, " : "Joined, ") + count;
                               },
                               {}, [&] { menu.push(lobby_page()); }});
        p.items.push_back({"Resume", "Back to the game.", {}, {}, [&] { menu.close(); }});
        if (newer_version)
            p.items.push_back({"Version " + newer_version->version + " is out",
                               "This is version " + std::string(net::kVersion) +
                                   ". Opens its page on GitHub, with the downloads. Players need the same version to play together.",
                               {}, {}, [&] { SDL_OpenURL(newer_version->page.c_str()); }});
        p.items.push_back({"Network play", netgame ? "The game you're in." : "Play with others, on this network or the internet.", {},
                           {}, [&] { menu.push(netgame ? lobby_page() : network_page()); }});
        p.items.push_back({"Display", "Render scale, antialiasing, quads, the 3D renderer, CRT colours, screen filter, fullscreen.", {}, {},
                           [&] { menu.push(display_page()); }});
        p.items.push_back({"Sound", "The volumes, and the speakers: stereo, or the cabinet's front and rear apart.", {}, {}, [&] { menu.push(sound_page()); }});
        p.items.push_back({"Controls", "Your keys or pad buttons.", {}, {}, [&] { menu.push(controls_page(0)); }});
        if (!(netgame && netgame->running)) // (not in a network game: the same for everyone)
            p.items.push_back({"Game", "Reset, the arcade's test menu, a service credit.", {}, {}, [&] { menu.push(game_page()); }});
        p.items.push_back({"Quit", "Saves the settings and closes the game.", {}, {}, [&] {
                               menu.close();
                               running = false;
                           }});
        return p;
    };
    make_root = root_page;
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
    bool test_key_ready = false; // (frame 300 reached once)
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
        // (from the game's frame 300: the board shown, in a network game this player's; and then on, through a
        // network game starting: its frames begin again)
        if (test_key < test_keys.size() && (test_key_ready || shown_number[shared.netplay ? shared.local : 0] >= 300) && std::chrono::steady_clock::now() >= test_key_due) {
            test_key_ready = true;
            SDL_Event key{};
            key.type = SDL_EVENT_KEY_DOWN;
            key.key.scancode = test_keys[test_key++];
            key.key.key = SDL_GetKeyFromScancode(key.key.scancode, SDL_KMOD_NONE, false);
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
                const std::string path = "spikeout-" + std::to_string(shown_number[shared.netplay ? shared.local : 0]) + ".png";
#ifdef SPIKEOUT_GPU
                if (shared.gpu_3d) shown = capture();
#endif
                if (tools::write_png(path, shown, width, height)) std::printf("screenshot: %s\n", path.c_str());
            }
            // Save states (the owner's safeguards, 2026-11-04). F9 saves (a network game's every cabinet only with
            // --net-save-key: for gamerun, nothing a player can load); a few seconds after a load, F9 goes back to the
            // game as it was before it instead. F10 loads the last saved: pressed twice (the first says what it would
            // load), the file checked first (its build, then loaded into a spare board), the game as it is saved first.
            // Not in network play; the menu takes the keys while open.
            const auto now_key = std::chrono::steady_clock::now();
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F9 && !e.key.repeat) {
                if (shared.netplay) {
                    if (shared.net_save_key) {
                        shared.net_save_request = true;
                        std::printf("spikeout: saving every cabinet at the end of a frame (spikeout-net-<frame>.state)\n");
                    } else {
                        shared.say("SAVE STATES ARE FOR SINGLE-PLAYER GAMES");
                    }
                } else if (boards == 1 && now_key < undo_until) {
                    undo_until = {};
                    reset_game([&] { shared.load_path = kUndoState; });
                    shared.say("BACK TO THE GAME BEFORE THE LOAD");
                    std::printf("spikeout: back to the game before the load (%s)\n", kUndoState);
                } else if (boards == 1) {
                    shared.save_request = true;
                    std::printf("spikeout: saving at the end of the frame\n");
                } else {
                    shared.say("SAVE STATES ARE FOR ONE CABINET");
                }
                std::fflush(stdout);
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F10 && !e.key.repeat && boards == 1 && !shared.netplay) {
                std::string last;
                {
                    std::lock_guard<std::mutex> guard(shared.lock);
                    last = shared.last_state;
                }
                std::optional<uint64_t> frame; // (the file's, from its header)
                bool other = false;            // (from another version of the game: offered, said)
                const std::string why = last.empty() ? std::string() : check_state(last, frame, other);
                const std::string which = frame ? "THE STATE FROM FRAME " + std::to_string(*frame) : "THE LAST SAVE STATE";
                if (last.empty()) {
                    shared.say("NO SAVE STATE YET: F9 SAVES ONE");
                } else if (!why.empty()) {
                    load_armed_until = {};
                    shared.say("CAN'T LOAD: " + why, 5);
                    std::printf("spikeout: can't load %s: %s\n", last.c_str(), why.c_str());
                } else if (now_key >= load_armed_until) { // (the first press: what it would do)
                    load_armed_until = now_key + std::chrono::seconds(3);
                    shared.say(other ? "THIS SAVE IS FROM ANOTHER VERSION: PRESS F10 AGAIN TO TRY IT" : "PRESS F10 AGAIN TO LOAD " + which);
                } else { // (the second: the game as it is kept first, then the load)
                    load_armed_until = {};
                    const uint64_t before = shared.saves_done;
                    {
                        std::lock_guard<std::mutex> guard(shared.lock);
                        shared.save_to = kUndoState;
                    }
                    shared.save_request = true;
                    for (int k = 0; k < 200 && shared.saves_done == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    const bool kept = shared.saves_done != before;
                    reset_game([&] { shared.load_path = last; });
                    undo_until = kept ? std::chrono::steady_clock::now() + std::chrono::seconds(5) : std::chrono::steady_clock::time_point{};
                    shared.say(kept ? "LOADED " + which + "  \xC2\xB7  F9 GOES BACK" : "LOADED " + which, 5);
                    std::printf("spikeout: loading %s%s%s%s\n", last.c_str(), kept ? " (the game before it in " : "", kept ? kUndoState : "",
                                kept ? ")" : "");
                }
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode >= SDL_SCANCODE_F5 && e.key.scancode <= SDL_SCANCODE_F8) {
                const auto board = static_cast<std::size_t>(e.key.scancode - SDL_SCANCODE_F5);
                if (board < boards) focus = board;
            }
            // A spectator: the cabinet watched, the next or the one before (Left / Right, a pad's shoulders; 1-4 for
            // that seat), each board taking it at its next frame.
            if (shared.spectating && boards > 1) {
                int step = 0;
                std::size_t to = shared.local;
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                    if (e.key.scancode == SDL_SCANCODE_LEFT) step = -1;
                    if (e.key.scancode == SDL_SCANCODE_RIGHT) step = 1;
                    if (e.key.scancode >= SDL_SCANCODE_1 && e.key.scancode <= SDL_SCANCODE_4 &&
                        static_cast<std::size_t>(e.key.scancode - SDL_SCANCODE_1) < boards)
                        to = static_cast<std::size_t>(e.key.scancode - SDL_SCANCODE_1);
                } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) step = -1;
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) step = 1;
                }
                if (step > 0) to = (shared.local + 1) % boards;
                if (step < 0) to = (shared.local + boards - 1) % boards;
                // (on one board, this device too slow for every board: only the cabinet it watches runs whole here; the
                // other made whole first from its player's backup, a second or so: Shared::switch_to; one at a time)
                if (to != shared.local && (shared.one_board_frame != ~0ull || shared.switch_to != ~std::size_t{0})) {
                    if (shared.switch_to == ~std::size_t{0}) {
                        shared.switch_to = to;
                        std::printf("spikeout: watching seat %zu asked for: on one board here, its board made whole first\n", to + 1);
                    }
                    to = shared.local;
                }
                if (to != shared.local) {
                    shared.local = to, focus = to;
                    shared.watched = to;
                    watch_banner_until = std::chrono::steady_clock::now() + std::chrono::seconds(5); // (the banner again)
                    std::printf("spikeout: watching seat %zu\n", to + 1);
                }
            }
#ifdef SPIKEOUT_GPU
            // Auto render scale and the window's picture shape: the window's size decides them.
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && (settings.render_scale == 0 || settings.picture_shape == 1))
                set_up_3d();
#endif
        }
#ifdef SPIKEOUT_GPU
        // The menu pauses the game (not a network game: the others play on);
        // closing it keeps the settings.
        {
            std::optional<std::string> picked;
            {
                std::lock_guard guard(rom_pick.lock);
                picked.swap(rom_pick.path);
            }
            if (picked && !picked->empty()) {
                std::filesystem::path p = utf8_path(*picked);
                const std::string why = rom_problem(p);
                if (why.empty()) {
                    settings.rom_path = path_utf8(p);
                    changed();
                    rom_pick.status = path_utf8(p.filename()) + ", from the next start";
                } else {
                    rom_pick.status = why;
                }
                std::printf("spikeout: ROM set chosen: %s (%s)\n", path_utf8(p).c_str(), why.empty() ? "good" : why.c_str());
                if (menu_ready && menu.is_open()) menu.refresh();
            }
#ifdef SPIKEOUT_JIT
            std::optional<std::string> folder;
            {
                std::lock_guard guard(rom_pick.lock);
                folder.swap(rom_pick.jit_folder);
            }
            if (folder && !folder->empty()) {
                settings.jit_cache = *folder;
                changed();
                rom_pick.jit_status = path_utf8(utf8_path(*folder).filename()) + ", from the next start";
                std::printf("spikeout: the translation's folder chosen: %s\n", folder->c_str());
                if (menu_ready && menu.is_open()) menu.refresh();
            }
#endif
        }
        const bool menu_open = menu_ready && menu.is_open();
        if (menu_open != menu_was_open) {
            {
                const std::size_t board = shared.netplay ? shared.local : 0;
                std::printf("spikeout: menu %s (board %zu at frame %llu)\n", menu_open ? "opened" : "closed", board + 1,
                            static_cast<unsigned long long>(shared.screens[board].frame_number));
                std::fflush(stdout);
            }
            shared.paused = menu_open;
            // (closing it always plays the sound again: a network game started from the menu was paused before it began)
            if (sound && (!menu_open || !shared.netplay)) sound->pause(menu_open);
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
        // Network play: the game, as it gathers its players, starts; the
        // lobby told of players joining; the menu's lobby page kept up to
        // date; the controls let go while the menu is open (the game goes on).
        static auto net_refreshed = now;
        // The update check answered: a newer version said once at the bottom (the menu's first items say it from now on).
        if (!update_told && update_check.done()) {
            update_told = true;
            newer_version = update_check.newer();
            if (newer_version) {
                std::printf("spikeout: version %s is out (%s)\n", newer_version->version.c_str(), newer_version->page.c_str());
                std::string upper = newer_version->version;
                for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                shared.say("VERSION " + upper + " IS OUT: SEE THE MENU (ESC)", 8);
            } else if (const std::string why = update_check.error(); !why.empty()) {
                std::printf("spikeout: the update check: %s\n", why.c_str());
            }
        }
        // The host closed the room before the game started: out of it, and told.
        // A spectator's game ended (its host gone, no new one): out of it too.
        if (netgame && (!netgame->running || shared.spectating) && !netgame->host && netgame->session().room_closed()) {
            const bool watched = shared.spectating;
            net_leave();
            net_notice = watched ? "THE GAME YOU WATCHED ENDED" : "THE HOST CLOSED THE ROOM";
            net_notice_until = now + std::chrono::seconds(6);
#ifdef SPIKEOUT_GPU
            if (menu_ready && menu.is_open()) menu.open(make_root());
#endif
        }
        // The host left the game in progress and this machine hosts the room
        // now (host migration): on the internet, the room listed again under
        // its code (the lobby still knows it), so the others can join it.
        // The input delay in play (Auto): each second, what the furthest player's round trip asks for now; moved a frame
        // towards it once it has asked for the same for 3 seconds (a moment's spike doesn't move it; a raised delay
        // leaves a frame held, a lowered one drops one: the session's stream takes either).
        static auto delay_checked = now;
        static int delay_asked = 0;
        if (netgame && netgame->running && !shared.spectating && net_delay < 0 && now - delay_checked >= std::chrono::seconds(1)) {
            delay_checked = now;
            const int one_way = furthest_one_way(shared.local, shared.boards);
            const uint64_t want = auto_delay(one_way, shared.rollback != nullptr), have = shared.input_delay;
            delay_asked = want == have ? 0 : delay_asked + 1;
            if (delay_asked >= 3 && one_way >= 0) {
                delay_asked = 0;
                shared.input_delay = want > have ? have + 1 : have - 1;
                std::printf("spikeout: input delay %llu (the furthest player %d ms away one way)\n",
                            static_cast<unsigned long long>(shared.input_delay.load()), one_way);
            }
        }
        if (netgame && netgame->running && !netgame->host && netgame->session().hosting()) {
            netgame->host = true;
            netgame->set_status("The host left: you host the room now");
            if (netgame->internet) {
                netgame->ready = false; // (the listing below waits for this)
                if (netgame->worker.joinable()) netgame->worker.join();
                NetGame* g = netgame.get();
                g->worker = std::thread([g, name = player_name(settings)] {
                    std::string code;
                    {
                        std::lock_guard guard(g->lock);
                        code = g->code;
                    }
                    const auto seats = static_cast<uint8_t>(g->session().players().size());
                    // (listed or not as it was, if the lobby takes it over; else by its new code only)
                    const auto again = tools::lobby_offer(g->lobby, g->session(), name, seats, false, code);
                    {
                        std::lock_guard guard(g->lock);
                        g->code = again ? *again : std::string();
                    }
                    g->listed_taken = 0; // (the lobby told who's in, below)
                    g->set_status(again ? "You host the room now" : "You host the room now (the lobby didn't answer)");
                    g->ready = true;
                });
            }
        }
        if (netgame) {
            if (netgame->host && netgame->internet && netgame->ready) { // (a game in progress too: players join it)
                tools::lobby_pump(netgame->lobby, netgame->session());
                std::size_t taken = 0;
                for (const std::string& name : netgame->session().players()) taken += name.empty() ? 0 : 1;
                if (taken != netgame->listed_taken) { // the lobby's list up to date
                    netgame->listed_taken = taken;
                    net::Json m = net::Json::object();
                    m.set("t", "update"), m.set("taken", static_cast<int>(taken)), m.set("started", netgame->running);
                    netgame->lobby.send(m);
                }
            }
            if (!netgame->running)
                if (const auto start = netgame->play->wait_start(std::chrono::milliseconds(0))) {
                    net_begin(*start);
#ifdef SPIKEOUT_GPU
                    if (menu_ready) menu.close();
#endif
                }
            if (netgame->running)
                if (const auto again = netgame->play->take_resync()) net_resync(*again);
            if (netgame && netgame->running && shared.paused) { // (the menu open)
                Screen& s = shared.screens[shared.local];
                s.bank0 = 0xFF, s.bank1 = 0xFF, s.controls = 0xFF;
            }
        }
        if (discovery) discovery->poll(); // (games heard of, for the nearby list)
#ifdef SPIKEOUT_GPU
        // Network status (Settings::show_net), 4 times a second: each seat
        // under the frame rate. A seat's news (joined, left) shows in its
        // row for a few seconds in place of its connection. In a room whose
        // game hasn't started, a line above them says so (and its code): the
        // menu can be closed without leaving the room, so it's always shown.
        static auto net_status_at = now;
        static std::array<net::Session::SeatView::State, net::kMaxSeats> seat_was{};
        static std::array<std::chrono::steady_clock::time_point, net::kMaxSeats> seat_news{};
        if (menu_ready && now - net_status_at > std::chrono::milliseconds(250)) {
            net_status_at = now;
            std::vector<ui::NetRow> net_rows;
            std::string net_header = !netgame && now < net_notice_until ? net_notice : std::string();
            ui::NetPills net_pills;
            using State = net::Session::SeatView::State;
            using Status = net::Session::Status;
            // (each machine's platform and how it plays, for the others' lists: sent only as it changes, and repeated)
            if (netgame && netgame->running && !shared.spectating) {
                Status me;
#if defined(__ANDROID__)
                me.platform = Status::Platform::android;
#elif defined(_WIN32)
                me.platform = Status::Platform::windows;
#elif defined(__APPLE__)
                me.platform = Status::Platform::macos;
#elif defined(__linux__)
                me.platform = Status::Platform::linux_os;
#endif
                me.boards = shared.one_board_frame != ~0ull ? Status::Boards::one : Status::Boards::every;
                me.net = shared.rollback ? Status::Net::rollback : Status::Net::delay;
                me.delay = static_cast<int16_t>(shared.input_delay), me.auto_delay = net_delay < 0;
                if (shared.catching_up) { // (joined in progress: how far caught up, for the host's seat list; by fives)
                    const uint64_t behind = shared.catch_up_behind, start = shared.catch_up_start;
                    const int percent = start && behind <= start ? static_cast<int>(100 * (start - behind) / start) : 0;
                    me.catch_up = static_cast<int8_t>(percent / 5 * 5);
                }
                netgame->session().set_status(me);
            }
            const auto platform_icon = [](const Status& s) -> std::string {
                static const char* const kNames[] = {"", "windows", "macos", "linux", "android"};
                return kNames[static_cast<int>(s.platform)];
            };
            const auto mode_tag = [](const Status& s) -> std::string {
                if (s.boards == Status::Boards::unknown) return {};
                // ("1 BOARD · RB · AUTO 2": its boards, rollback or waiting for inputs, its input delay (frames) and
                // whether Auto chose it)
                std::string t = s.boards == Status::Boards::one ? "1 BOARD" : "ALL BOARDS";
                if (s.net != Status::Net::unknown) t += s.net == Status::Net::rollback ? " \xC2\xB7 RB" : " \xC2\xB7 WAIT";
                if (s.delay >= 0) t += std::string(s.auto_delay ? " \xC2\xB7 AUTO " : " \xC2\xB7 DELAY ") + std::to_string(s.delay);
                return t;
            };
            // (rollback: how often each other player's inputs were guessed wrong here in the last 10 s)
            const uint64_t frame_now = shared.inputs ? shared.inputs->next(shared.local) : 0;
            const uint64_t lately = frame_now > 600 ? frame_now - 600 : 0;
            if (netgame) {
                const auto seats = netgame->session().seats();
                if (!netgame->running) {
                    std::string code;
                    {
                        std::lock_guard guard(netgame->lock);
                        code = netgame->code;
                    }
                    const bool in = std::any_of(seats.begin(), seats.end(), [&](const auto& v) { return v.state == State::me; });
                    net_header = "ROOM " + (code.empty() ? std::string() : code + "  ") +
                                 (netgame->host ? "(HOSTING)" : netgame->spectator ? "(WATCHING)" : in ? "(JOINED)" : "(JOINING...)");
                } else if (settings.show_net) { // (playing: the game's name and its room; each player's delay in its badges)
                    std::string code;
                    {
                        std::lock_guard guard(netgame->lock);
                        code = netgame->code;
                    }
                    // ("HOST DON · ROOM ABCD": who hosts now (host migration: not the game's own name then), YOU if
                    // this machine; a game on the LAN has no room code)
                    const uint8_t host = netgame->session().host_seat();
                    const std::string host_name = netgame->session().hosting() ? std::string("YOU")
                                                  : host < seats.size() && !seats[host].name.empty() ? tools::clean_name(seats[host].name)
                                                  : tools::clean_name(netgame->session().game_name());
                    net_header = "HOST " + host_name + (code.empty() ? std::string(" \xC2\xB7 LAN") : " \xC2\xB7 ROOM " + code);
                    if (shared.spectating) net_header = "WATCHING \xC2\xB7 " + net_header;
                }
                const bool hosting = netgame->session().hosting();
                for (std::size_t s = 0; s < seats.size(); ++s) {
                    const auto& v = seats[s];
                    if (v.state == State::none) continue;
                    const State was = seat_was[s];
                    if (was != State::none && was != v.state &&
                        (v.state == State::playing || v.state == State::gone)) // (news: joined, left)
                        seat_news[s] = now + std::chrono::seconds(4);
                    seat_was[s] = v.state;
                    const bool news = now < seat_news[s];
                    ui::NetRow r;
                    r.seat = std::to_string(s + 1) + "P";
                    r.name = tools::clean_name(v.name); // (as ours: what the menu's font has)
                    r.country = v.country;
                    // (tests: SPIKEOUT_TEST_FLAGS=gb,jp,... each seat's flag as if the lobby had said its country: a LAN
                    // game's list seen as an internet one's)
                    if (static const char* flags = std::getenv("SPIKEOUT_TEST_FLAGS"); flags && std::strlen(flags) >= 3 * s + 2)
                        r.country = std::string(flags + 3 * s, 2);
                    if (v.state == State::me || v.state == State::playing || v.state == State::joining)
                        r.platform = platform_icon(v.status), r.mode = mode_tag(v.status);
                    // (how this machine reaches each other player, first in its tag: the owner's ask, 2026-11-05)
                    using Link = net::Session::SeatView::Link;
                    if (v.link != Link::none && !r.mode.empty())
                        r.mode = std::string(v.link == Link::lan ? "LAN" : v.link == Link::relay ? "RELAY" : "DIRECT") +
                                 " \xC2\xB7 " + r.mode;
                    if (v.state == State::playing && shared.rollback && shared.inputs && netgame->running)
                        r.rollbacks = static_cast<int>(shared.inputs->wrongs_since(s, lately));
                    switch (v.state) {
                    case State::me: r.name = "YOU", r.look = ui::NetRow::Look::you; break;
                    case State::open: r.status = hosting ? "WAITING" : "OPEN", r.look = ui::NetRow::Look::dim; break;
                    case State::joining: // (as far as the host knows it: its state received, then its catching up)
                        using Joining = net::Session::SeatView::Joining;
                        r.status = v.joining == Joining::receiving     ? "RECEIVING " + std::to_string(v.progress) + "%"
                                   : v.joining == Joining::catching_up ? "CATCHING UP " + std::to_string(v.progress) + "%"
                                                                       : std::string("JOINING");
                        if (v.joining != Joining::none) r.progress = v.progress;
                        r.look = ui::NetRow::Look::fresh;
                        break;
                    case State::gone:
                        r.status = !v.left ? "IDLE" : news ? "LEFT" : hosting ? "WAITING" : "OPEN";
                        if (v.left && !news) r.name.clear();
                        r.look = ui::NetRow::Look::dim;
                        break;
                    case State::playing:
                        if (news) {
                            r.status = "JOINED", r.look = ui::NetRow::Look::fresh;
                        } else {
                            const int ms = v.round_trip;
                            r.ms = ms;
                            r.bars = ms < 0 ? 0 : ms < 40 ? 4 : ms < 80 ? 3 : ms < 140 ? 2 : 1;
                        }
                        break;
                    default: break;
                    }
                    if (shared.spectating && netgame->running && s == shared.local) r.look = ui::NetRow::Look::you; // (the one watched)
                    net_rows.push_back(std::move(r));
                }
                // (spectators: an eye and how many after the header, in the room and in the game: the owner's choice)
                net_pills.watchers = static_cast<int>(netgame->session().watching());
                // (a spectator: whom it watches and how to change it, at the bottom for 5 s once its game shows and
                // after each change: the owner's choice)
                static bool banner_started = false;
                if (!shared.spectating || !netgame->running) banner_started = false;
                else if (!shared.catching_up && !banner_started && shown_number[shared.local] >= 600) // (past the boot's screens)
                    banner_started = true, watch_banner_until = now + std::chrono::seconds(5);
                const auto who_at = [&](std::size_t s) {
                    const std::string seat = std::to_string(s + 1) + "P";
                    return seats[s].name.empty() ? seat : tools::clean_name(seats[s].name) + " (" + seat + ")";
                };
                // (on one board, a switch under way: its board made whole from its backup first; then the banner again)
                static bool was_switching = false;
                static std::chrono::steady_clock::time_point switch_asked{};
                const std::size_t switching = shared.switch_to;
                if (was_switching && switching == ~std::size_t{0}) watch_banner_until = now + std::chrono::seconds(5);
                if (!was_switching && switching != ~std::size_t{0}) switch_asked = now;
                was_switching = switching != ~std::size_t{0};
                // (one taking over half a second: the game dimmed under the new player's name, still running behind it:
                // the owner's choice)
                const bool covered = shared.spectating && was_switching && switching < seats.size() &&
                                     now - switch_asked > std::chrono::milliseconds(500);
                menu.set_switch_cover(covered ? "SWITCHING TO " + who_at(switching) : std::string());
                if (covered) {
                    menu.set_watch_banner(""); // (the cover says it)
                } else if (shared.spectating && netgame->running && was_switching && switching < seats.size()) {
                    menu.set_watch_banner({}, "SWITCHING TO " + who_at(switching) + "...");
                } else if (shared.spectating && netgame->running && !shared.catching_up && now < watch_banner_until) {
                    menu.set_watch_banner(who_at(shared.local));
                } else {
                    menu.set_watch_banner("");
                }
            } else {
                seat_was.fill(State::none);
                menu.set_watch_banner(""), menu.set_switch_cover("");
                seat_news.fill({});
            }
            { // (a save state's notice, at the bottom a few seconds: over a spectator's banner)
                std::lock_guard<std::mutex> guard(shared.lock);
                if (now < shared.notice_until) menu.set_watch_banner({}, shared.notice);
                else if (!netgame) menu.set_watch_banner("");
            }
            // (a game joined in progress: a loading screen through each step, the game's picture not real until the
            // cabinet is ours: receiving every board's state, catching up with the others, taking the cabinet; the
            // owner's design, 2026-11-05. A spectator's: catching up only, as before)
            const auto join = netgame && !netgame->spectator ? netgame->session().join_progress() : net::Session::JoinProgress{};
            using Stage = net::Session::JoinProgress::Stage;
            const bool catching = netgame && netgame->running && shared.catching_up;
            if (join.stage == Stage::receiving || join.stage == Stage::seating || catching) {
                if (join.stage == Stage::receiving && !join_shown) { // (once: the room page closed for the screen)
                    join_shown = true;
                    if (menu_ready && menu.is_open()) menu.close();
                }
                std::string code;
                {
                    std::lock_guard guard(netgame->lock);
                    code = netgame->code;
                }
                const auto seats = netgame->session().seats();
                const uint8_t host = netgame->session().host_seat();
                std::size_t players = 0;
                for (const auto& v : seats) players += v.state == State::me || v.state == State::playing || v.state == State::joining;
                std::string sub;
                if (host < seats.size() && !seats[host].name.empty()) sub = "HOST " + tools::clean_name(seats[host].name) + " \xC2\xB7 ";
                sub += std::to_string(players) + " PLAYERS";
                if (host < seats.size()) {
                    using Link = net::Session::SeatView::Link;
                    const Link l = seats[host].link;
                    if (l != Link::none) sub += l == Link::relay ? " \xC2\xB7 THROUGH THE RELAY" : l == Link::lan ? " \xC2\xB7 ON THIS NETWORK" : " \xC2\xB7 DIRECT";
                }
                const std::string title = shared.spectating ? "WATCHING THE GAME" : code.empty() ? "JOINING THE GAME" : "JOINING ROOM " + code;
                static const std::vector<std::string> kSteps = {"JOINED", "RECEIVING THE GAME", "CATCHING UP", "YOUR CABINET"};
                const auto mb = [](std::size_t b) {
                    char t[16];
                    std::snprintf(t, sizeof t, "%.1f", static_cast<double>(b) / (1024.0 * 1024.0));
                    return std::string(t);
                };
                if (catching) {
                    // (the bar: how much of the gap there was as catching up started has been closed)
                    const uint64_t behind = shared.catch_up_behind, start = shared.catch_up_start;
                    const int percent = start && behind <= start ? static_cast<int>(100 * (start - behind) / start) : 0;
                    menu.set_loading(title, percent, "Catching up with the others", sub, shared.spectating ? std::vector<std::string>{} : kSteps, 2);
                } else if (join.stage == Stage::receiving) {
                    const int percent = join.total ? static_cast<int>(100 * join.bytes / join.total) : -1;
                    menu.set_loading(title, percent,
                                     join.total ? "Receiving the game: " + mb(join.bytes) + " of " + mb(join.total) + " MB"
                                                : std::string("Waiting for the game..."),
                                     sub, kSteps, 1);
                } else {
                    menu.set_loading(title, 100, "Taking your cabinet", sub, kSteps, 3);
                }
            } else {
                menu.set_loading("");
                if (!netgame) join_shown = false;
            }
            // (Network status off: the seats left out, the room's line still shown)
            menu.set_net(settings.show_net ? net_rows : std::vector<ui::NetRow>(), net_header,
                         settings.show_net ? net_pills : ui::NetPills{});
        }
        if (menu_open && now - net_refreshed > std::chrono::milliseconds(250)) {
            net_refreshed = now;
            if (netgame) menu.refresh();
        }
#endif
        {
            std::lock_guard<std::mutex> guard(shared.lock);
            for (std::size_t b = 0; b < boards; ++b) {
                if (shared.netplay && b != shared.local) continue; // (network play: only this player's board shows)
                Screen& s = shared.screens[b];
                if (s.frame_number != shown_number[b]) {
                    place(shared.netplay ? 0 : b, s.frame);
                    if (b == (shared.netplay ? shared.local : 0)) {
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
            const bool remade = shared.gpu_frame.number == gpu_frame.number && shared.gpu_frame.version > gpu_frame.version;
            if (shared.gpu_3d && (shared.gpu_frame.number > gpu_frame.number || remade)) {
                if (remade) drawn_number = 0; // (its 3D drawn again)
                const bool keep = settings.smooth_motion && !remade;
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
            const bool all_reported = shared.netplay ? (reported & (1u << shared.local)) != 0 : reported == (1u << boards) - 1;
            if (max_frames && all_reported) { // a test run: keep the last frames and close
                for (std::size_t b = 0; b < boards; ++b)
                    if (!shared.netplay || b == shared.local) place(shared.netplay ? 0 : b, shared.screens[b].frame);
#ifdef SPIKEOUT_GPU
                if (shared.gpu_3d) shown = capture();
#endif
                const std::string path = "spikeout-" + std::to_string(shown_number[shared.netplay ? shared.local : 0]) + ".png";
                if (tools::write_png(path, shown, width, height)) std::printf("screenshot: %s\n", path.c_str());
                running = false;
            }
        }
        if (!reported && now - fps_start >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(now - fps_start).count();
            char title[96];
            std::snprintf(title, sizeof title, "Spikeout: Final Edition  (%.1f fps)", static_cast<double>(fps_frames) / secs);
            static const bool shown_debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr; // (tests: frames run and shown a second)
            if (shown_debug) {
                std::printf("spikeout: %.1f frames run, %.1f shown a second\n", static_cast<double>(fps_frames) / secs,
                            static_cast<double>(frames_shown) / secs);
                std::fflush(stdout);
            }
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
    if (netgame) parting(), netgame->session().leave();
    stop_games();
    if (!shared.spectating) keep_seat(); // (a network game: only this player's board is ours to keep; a spectator's: none)
    if (!test_run && !shared.netplay) save_nvram(data_folder, shared, boards); // (a network game's boards aren't ours to keep)
    netgame.reset();
    browser.reset();
    sound.reset();
    for (SDL_Gamepad* pad : gamepads) SDL_CloseGamepad(pad);
#ifdef SPIKEOUT_GPU
    if (presenter) presenter->overlay = nullptr; // (none without a GPU: SDL's renderer)
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
