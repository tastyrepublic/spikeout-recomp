// The window's settings, as the menu changes them: kept in settings.ini
// ("key = value") in the data folder, which is the program's own folder in
// portable mode (a file named portable.txt beside the program), else the
// user's (SDL_GetPrefPath: %APPDATA%, ~/Library/Application Support,
// ~/.local/share). Options on the command line win over them for that run.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tools {

// What a player's keys and pad buttons do, in the I/O board's terms.
enum class Action { Coin, Start, Up, Down, Left, Right, Beat, Charge, Jump, Shift, Count };
constexpr std::size_t kActions = static_cast<std::size_t>(Action::Count);
constexpr std::array<const char*, kActions> kActionNames = {"Coin", "Start", "Up", "Down", "Left",
                                                            "Right", "Beat", "Charge", "Jump", "Shift"};
constexpr std::array<const char*, kActions> kActionKeys = {"coin", "start", "up", "down", "left",
                                                           "right", "beat", "charge", "jump", "shift"};

// A pad input an action is set to, as one number: 0 none, 1 + an SDL gamepad
// button, or kPadAxis + an axis * 2 (+ 1: its positive half; a trigger's only half).
constexpr int kPadAxis = 1000;
constexpr int pad_button(SDL_GamepadButton b) { return 1 + static_cast<int>(b); }
constexpr int pad_axis(SDL_GamepadAxis a, bool positive) { return kPadAxis + static_cast<int>(a) * 2 + (positive ? 1 : 0); }
constexpr std::array<int, kActions> kDefaultPad = {
    pad_button(SDL_GAMEPAD_BUTTON_BACK),      pad_button(SDL_GAMEPAD_BUTTON_START),
    pad_button(SDL_GAMEPAD_BUTTON_DPAD_UP),   pad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN),
    pad_button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), pad_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
    pad_button(SDL_GAMEPAD_BUTTON_SOUTH),     pad_button(SDL_GAMEPAD_BUTTON_WEST),
    pad_button(SDL_GAMEPAD_BUTTON_EAST),      pad_button(SDL_GAMEPAD_BUTTON_NORTH)};
constexpr bool valid_pad_input(int code) {
    return (code >= 0 && code <= SDL_GAMEPAD_BUTTON_COUNT) || (code >= kPadAxis && code < kPadAxis + SDL_GAMEPAD_AXIS_COUNT * 2);
}

struct Settings {
    // Display
    int render_scale = 0;  // the 3D's size: 0 Auto (fits the window), else 1-6 times the arcade's 384 lines
    // Picture shape: 0 the arcade's (496 x 384), 1 the window's, 2 16:10, 3 16:9,
    // 4 21:9 (widescreen: the gameplay's 3D wider, the 2D in the middle)
    int picture_shape = 0;
    int antialiasing = 1;  // drawn this many times larger each way and averaged down (1: off)
    bool quads = true;     // quads drawn whole (on by default: the owner's choice, 2026-11-05)
    bool gpu_3d = true;    // the 3D on the GPU (else in software)
    bool fullscreen = false;
    bool show_fps = false; // the frames shown a second, in a corner
    bool show_net = true;  // a network game's seats (players, their connection) under it
    bool check_updates = true; // at each start, ask GitHub whether a newer version is out (src/net/update.hpp)
    bool detailed_log = false; // the network and timing lines in spikeout.log too (SPIKEOUT_NET_DEBUG), from the next start
    // Network play's numbers after each game and the problems the game noticed sent to the project by the game itself
    // (tools/problem_report.hpp, part 2): -1 not asked yet, 0 no, 1 yes to network play's only (0.1.0-preview4's
    // question; asked again, as the question grew), 2 yes to any problem too (a crash, the start or a game alone standing
    // still; asked at the first start since 2026-11-12)
    int send_reports = -1;
    // Smooth motion: a picture each refresh of the screen (up to 120 a
    // second), the 3D placed between the arcade's last two frames, a frame
    // behind them (m3::interpolate). Off: the arcade's own frames. Still
    // being tested: not kept in settings.ini, only spikeout --smooth turns it on.
    bool smooth_motion = false;
    // Speakers (SpeakerSetup in spikeout.cpp): 0 stereo, 1 front and rear on one
    // output of four channels, 2 each pair on an output of its own (by name)
    int speakers = 0;
    std::string front_output, rear_output;
    int crt_colours = 0;   // the window's looks (gpu::Presenter::set_look): 0 off, 1-5 monitors
    int screen_filter = 0; // 0 off, 1 scanlines, 2 CRT
    // Sound, in percent: overall (0-100), and the effects and music (0-200) under it
    int volume = 100, sound_volume = 100, music_volume = 100;
    // Network play: the name the others see (clean_name's; empty: this computer's user name)
    std::string player_name;
    // The ROM set: its zip (spikeofe.zip) or the folder of its files, in UTF-8 (empty: not chosen yet; chosen at the
    // first start and in the menu; a folder on the command line goes over it)
    std::string rom_path;
    // The JIT build: the folder its translation of the game is kept in, UTF-8 (empty: jit-cache in the data folder)
    std::string jit_cache;
    // Controls: two players' keys (SDL scancodes, 0: none)
    std::array<std::array<int, kActions>, 2> keys{{
        {SDL_SCANCODE_5, SDL_SCANCODE_1, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
         SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V},
        {SDL_SCANCODE_6, SDL_SCANCODE_2, SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
         SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F},
    }};
    // Their pads' buttons (pad_input codes, 0: none; the left stick also moves unless an action has one of its axes)
    std::array<std::array<int, kActions>, 2> pad_buttons{{kDefaultPad, kDefaultPad}};
    // Which pad each player has (SDL's GUID as text; empty or not connected: the first connected not taken)
    std::array<std::string, 2> pads;
    static Settings defaults() { return {}; }
};

// A player's name as the others see it (the owner's choice): up to
// kNameLength of kNameLetters, lower case made upper, anything else left
// out, no spaces at the ends. The menu's font has only Latin letters.
constexpr std::size_t kNameLength = 10;
constexpr const char* kNameLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";

inline std::string clean_name(const std::string& name) {
    std::string out;
    for (char c : name) {
        const char up = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        if (up != '\0' && std::string(kNameLetters).find(up) != std::string::npos && out.size() < kNameLength) out += up;
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

// The data folder, ending in a separator.
inline std::string data_folder() {
    if (const char* base = SDL_GetBasePath()) {
        const std::string here = base;
        if (std::ifstream(here + "portable.txt")) return here;
    }
    std::string path;
    if (char* pref = SDL_GetPrefPath("spikeout-recomp", "Spikeout")) {
        path = pref;
        SDL_free(pref);
    }
    return path;
}

inline Settings load_settings(const std::string& path) {
    Settings s;
    std::ifstream in(path);
    std::map<std::string, std::string> v;
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3); // (a byte order mark: a file saved by an editor that adds one)
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        auto trim = [](std::string t) {
            t.erase(0, t.find_first_not_of(" \t\r"));
            t.erase(t.find_last_not_of(" \t\r") + 1);
            return t;
        };
        v[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    auto get = [&](const char* key, int& out, int lo, int hi) {
        if (auto it = v.find(key); it != v.end()) out = std::clamp(std::atoi(it->second.c_str()), lo, hi);
    };
    auto get_bool = [&](const char* key, bool& out) {
        if (auto it = v.find(key); it != v.end()) out = it->second == "1" || it->second == "on" || it->second == "true";
    };
    get("render_scale", s.render_scale, 0, 6);
    get("picture_shape", s.picture_shape, 0, 4);
    get("antialiasing", s.antialiasing, 1, 4);
    get_bool("quads", s.quads);
    get_bool("gpu_3d", s.gpu_3d);
    get_bool("fullscreen", s.fullscreen);
    get_bool("show_fps", s.show_fps);
    get_bool("show_net", s.show_net);
    get_bool("check_updates", s.check_updates);
    get_bool("detailed_log", s.detailed_log);
    get("send_reports", s.send_reports, -1, 2);
    get("crt_colours", s.crt_colours, 0, 5);
    get("speakers", s.speakers, 0, 2);
    if (auto it = v.find("front_output"); it != v.end()) s.front_output = it->second;
    if (auto it = v.find("rear_output"); it != v.end()) s.rear_output = it->second;
    if (auto it = v.find("player_name"); it != v.end()) s.player_name = clean_name(it->second);
    if (auto it = v.find("rom_path"); it != v.end()) s.rom_path = it->second;
    if (auto it = v.find("jit_cache"); it != v.end()) s.jit_cache = it->second;
    get("screen_filter", s.screen_filter, 0, 2);
    get("volume", s.volume, 0, 100);
    get("sound_volume", s.sound_volume, 0, 200);
    get("music_volume", s.music_volume, 0, 200);
    for (std::size_t p = 0; p < 2; ++p)
        for (std::size_t a = 0; a < kActions; ++a)
            get(("p" + std::to_string(p + 1) + "_" + kActionKeys[a]).c_str(), s.keys[p][a], 0, SDL_SCANCODE_COUNT - 1);
    for (std::size_t p = 0; p < 2; ++p) {
        const std::string pre = "p" + std::to_string(p + 1) + "_pad";
        if (auto it = v.find(pre); it != v.end()) s.pads[p] = it->second;
        for (std::size_t a = 0; a < kActions; ++a)
            if (auto it = v.find(pre + "_" + kActionKeys[a]); it != v.end()) {
                const int code = std::atoi(it->second.c_str());
                if (valid_pad_input(code)) s.pad_buttons[p][a] = code;
            }
    }
    return s;
}

// The settings as settings.ini has them
inline std::string settings_text(const Settings& s) {
    std::ostringstream o;
    o << "# Spikeout recomp settings (the in-game menu writes this file)\n";
    o << "render_scale = " << s.render_scale << "\n";
    o << "picture_shape = " << s.picture_shape << "\n";
    o << "antialiasing = " << s.antialiasing << "\n";
    o << "quads = " << (s.quads ? 1 : 0) << "\n";
    o << "gpu_3d = " << (s.gpu_3d ? 1 : 0) << "\n";
    o << "fullscreen = " << (s.fullscreen ? 1 : 0) << "\n";
    o << "show_fps = " << (s.show_fps ? 1 : 0) << "\n";
    o << "show_net = " << (s.show_net ? 1 : 0) << "\n";
    o << "check_updates = " << (s.check_updates ? 1 : 0) << "\n";
    o << "detailed_log = " << (s.detailed_log ? 1 : 0) << "\n";
    o << "send_reports = " << s.send_reports << "\n";
    o << "speakers = " << s.speakers << "\n";
    o << "front_output = " << s.front_output << "\n";
    o << "rear_output = " << s.rear_output << "\n";
    o << "player_name = " << s.player_name << "\n";
    o << "rom_path = " << s.rom_path << "\n";
    o << "jit_cache = " << s.jit_cache << "\n";
    o << "crt_colours = " << s.crt_colours << "\n";
    o << "screen_filter = " << s.screen_filter << "\n";
    o << "volume = " << s.volume << "\n";
    o << "sound_volume = " << s.sound_volume << "\n";
    o << "music_volume = " << s.music_volume << "\n";
    for (std::size_t p = 0; p < 2; ++p)
        for (std::size_t a = 0; a < kActions; ++a) o << "p" << p + 1 << "_" << kActionKeys[a] << " = " << s.keys[p][a] << "\n";
    for (std::size_t p = 0; p < 2; ++p) {
        o << "p" << p + 1 << "_pad = " << s.pads[p] << "\n";
        for (std::size_t a = 0; a < kActions; ++a)
            o << "p" << p + 1 << "_pad_" << kActionKeys[a] << " = " << s.pad_buttons[p][a] << "\n";
    }
    return o.str();
}

inline bool save_settings(const std::string& path, const Settings& s) {
    std::ofstream out(path, std::ios::trunc);
    out << settings_text(s);
    return static_cast<bool>(out);
}

// The settings that differ between two texts of them (settings_text), a line each: "key: old -> new" (the log: a
// report says what the player changed, issue #8)
inline std::vector<std::string> settings_changes(const std::string& before, const std::string& after) {
    auto read = [](const std::string& text) {
        std::vector<std::pair<std::string, std::string>> out;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            const auto eq = line.find(" = ");
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            out.push_back({line.substr(0, eq), line.substr(eq + 3)});
        }
        return out;
    };
    const auto a = read(before), b = read(after);
    std::vector<std::string> changes;
    for (const auto& [key, value] : b) {
        std::string old = "(none)";
        for (const auto& [k, v] : a)
            if (k == key) old = v;
        if (old != value) changes.push_back(key + ": " + old + " -> " + value);
    }
    return changes;
}

} // namespace tools
