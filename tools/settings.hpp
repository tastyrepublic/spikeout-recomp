// The window's settings, as the menu changes them: kept in settings.ini
// ("key = value") in the data folder, which is the program's own folder in
// portable mode (a file named portable.txt beside the program), else the
// user's (SDL_GetPrefPath: %APPDATA%, ~/Library/Application Support,
// ~/.local/share). Options on the command line win over them for that run.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace tools {

// What a player's keys and pad buttons do, in the I/O board's terms.
enum class Action { Coin, Start, Up, Down, Left, Right, Beat, Charge, Jump, Shift, Count };
constexpr std::size_t kActions = static_cast<std::size_t>(Action::Count);
constexpr std::array<const char*, kActions> kActionNames = {"Coin", "Start", "Up", "Down", "Left",
                                                            "Right", "Beat", "Charge", "Jump", "Shift"};
constexpr std::array<const char*, kActions> kActionKeys = {"coin", "start", "up", "down", "left",
                                                           "right", "beat", "charge", "jump", "shift"};

struct Settings {
    // Display
    int render_scale = 0;  // the 3D's size: 0 Auto (fits the window), else 1-6 times the arcade's 384 lines
    // Picture shape: 0 the arcade's (496 x 384), 1 the window's, 2 16:10, 3 16:9,
    // 4 21:9 (widescreen: the gameplay's 3D wider, the 2D in the middle)
    int picture_shape = 0;
    int antialiasing = 1;  // drawn this many times larger each way and averaged down (1: off)
    bool quads = false;    // quads drawn whole
    bool gpu_3d = true;    // the 3D on the GPU (else in software)
    bool fullscreen = false;
    bool show_fps = false; // the frames shown a second, in a corner
    bool show_net = true;  // a network game's seats (players, their connection) under it
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
    // Controls: two players' keys (SDL scancodes, 0: none)
    std::array<std::array<int, kActions>, 2> keys{{
        {SDL_SCANCODE_5, SDL_SCANCODE_1, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
         SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V},
        {SDL_SCANCODE_6, SDL_SCANCODE_2, SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
         SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F},
    }};
    static Settings defaults() { return {}; }
};

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
    get("crt_colours", s.crt_colours, 0, 5);
    get("speakers", s.speakers, 0, 2);
    if (auto it = v.find("front_output"); it != v.end()) s.front_output = it->second;
    if (auto it = v.find("rear_output"); it != v.end()) s.rear_output = it->second;
    get("screen_filter", s.screen_filter, 0, 2);
    get("volume", s.volume, 0, 100);
    get("sound_volume", s.sound_volume, 0, 200);
    get("music_volume", s.music_volume, 0, 200);
    for (std::size_t p = 0; p < 2; ++p)
        for (std::size_t a = 0; a < kActions; ++a)
            get(("p" + std::to_string(p + 1) + "_" + kActionKeys[a]).c_str(), s.keys[p][a], 0, SDL_SCANCODE_COUNT - 1);
    return s;
}

inline bool save_settings(const std::string& path, const Settings& s) {
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
    o << "speakers = " << s.speakers << "\n";
    o << "front_output = " << s.front_output << "\n";
    o << "rear_output = " << s.rear_output << "\n";
    o << "crt_colours = " << s.crt_colours << "\n";
    o << "screen_filter = " << s.screen_filter << "\n";
    o << "volume = " << s.volume << "\n";
    o << "sound_volume = " << s.sound_volume << "\n";
    o << "music_volume = " << s.music_volume << "\n";
    for (std::size_t p = 0; p < 2; ++p)
        for (std::size_t a = 0; a < kActions; ++a) o << "p" << p + 1 << "_" << kActionKeys[a] << " = " << s.keys[p][a] << "\n";
    std::ofstream out(path, std::ios::trunc);
    out << o.str();
    return static_cast<bool>(out);
}

} // namespace tools
