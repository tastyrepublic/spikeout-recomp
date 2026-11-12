// Pads and arcade sticks SDL has no layout for (issue #11: a Mayflash F500 Elite on macOS, "NOT a gamepad to SDL", the
// game couldn't use it). Two things, both before the game opens its pads:
//   1. more layouts: the community's SDL_GameControllerDB (assets/controllers, built in; zlib licence), then the
//      player's own gamecontrollerdb.txt in the data folder if there is one (made with the community's tools);
//   2. a generic layout for any joystick still without one: its buttons in order (button 1 = A (South), 2 = B (East),
//      3 = X, 4 = Y, 5 = LB, 6 = RB, 7 = LT, 8 = RT, 9 = Back, 10 = Start, then the rest), its first hat the d-pad, its
//      first axes the left and right sticks. Every input of it can then be chosen in Controls, the menu naming them as
//      the stick has them ("Button 3", "Hat up", "Axis 2 +"), and the player's choices kept as for any pad.
// A stick set up that way and sent in a report (its settings.ini says which button is which) can be given a layout of
// its own here, for everyone after.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

namespace tools::pads {

// The generic layout's order: raw button n is this gamepad button.
inline constexpr const char* kGenericButtons[] = {"a",        "b",        "x",       "y",       "leftshoulder", "rightshoulder",
                                                  "lefttrigger", "righttrigger", "back", "start", "leftstick", "rightstick",
                                                  "guide",    "misc1",    "paddle1", "paddle2", "paddle3", "paddle4",
                                                  "touchpad", "misc2",    "misc3",   "misc4",   "misc5",   "misc6"};
inline constexpr const char* kGenericAxes[] = {"leftx", "lefty", "rightx", "righty"};

// The mapping SDL takes (SDL_AddGamepadMapping) for a joystick of that many buttons, axes and hats; "" if it has
// nothing to map.
inline std::string generic_mapping(const std::string& guid, const std::string& name, int buttons, int axes, int hats) {
    std::string m;
    const int b = std::min(buttons, static_cast<int>(std::size(kGenericButtons)));
    for (int i = 0; i < b; ++i) {
        // (a trigger from a button: SDL gives it as the axis full; the d-pad from the hat below)
        m += std::string(",") + kGenericButtons[i] + ":b" + std::to_string(i);
    }
    if (hats > 0) m += ",dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8";
    const int a = std::min(axes, static_cast<int>(std::size(kGenericAxes)));
    for (int i = 0; i < a; ++i) m += std::string(",") + kGenericAxes[i] + ":a" + std::to_string(i);
    if (m.empty()) return {};
    std::string clean = name;
    std::replace(clean.begin(), clean.end(), ',', ' '); // (the mapping's fields are comma separated)
    return guid + "," + clean + " (generic layout)" + m + ",";
}

// The joysticks given a generic layout here (by GUID): the menu names their inputs as the stick has them.
inline std::set<std::string>& generic_guids() {
    static std::set<std::string> guids;
    return guids;
}
inline std::string guid_text(SDL_GUID g) {
    char text[64] = {};
    SDL_GUIDToString(g, text, sizeof text);
    return text;
}

// Every connected joystick SDL has no layout for given the generic one (SDL then offers it as a gamepad). Cheap
// when there's nothing new: call it at start and as joysticks come.
inline void make_generic_layouts() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    if (!ids) return;
    for (int i = 0; i < n; ++i) {
        const SDL_JoystickID id = ids[i];
        if (SDL_IsGamepad(id)) continue;
        const std::string guid = guid_text(SDL_GetJoystickGUIDForID(id));
        if (generic_guids().count(guid)) continue;
        SDL_Joystick* j = SDL_OpenJoystick(id);
        if (!j) continue;
        const char* name = SDL_GetJoystickName(j);
        const std::string mapping = generic_mapping(guid, name ? name : "Joystick", SDL_GetNumJoystickButtons(j),
                                                    SDL_GetNumJoystickAxes(j), SDL_GetNumJoystickHats(j));
        const int buttons = SDL_GetNumJoystickButtons(j), axes = SDL_GetNumJoystickAxes(j), hats = SDL_GetNumJoystickHats(j);
        SDL_CloseJoystick(j);
        generic_guids().insert(guid); // (tried once, whatever comes of it)
        if (mapping.empty()) continue;
        if (SDL_AddGamepadMapping(mapping.c_str()) < 0) {
            std::printf("controller %u: no layout known, and a generic one wasn't taken: %s\n", static_cast<unsigned>(id), SDL_GetError());
            continue;
        }
        std::printf("controller %u: no layout known: a generic one made (buttons %d, axes %d, hats %d; Controls names them "
                    "as the stick has them)\n",
                    static_cast<unsigned>(id), buttons, axes, hats);
    }
    SDL_free(ids);
}

// More layouts: the built-in database (its text), then the player's own file (if there is one).
inline void load_layouts(const void* db, std::size_t db_size, const std::string& data_folder) {
    if (db && db_size) {
        const int n = SDL_AddGamepadMappingsFromIO(SDL_IOFromConstMem(db, db_size), true);
        std::printf("controllers: %d layouts from SDL_GameControllerDB\n", n);
    }
    const std::string own = data_folder + "gamecontrollerdb.txt";
    if (SDL_GetPathInfo(own.c_str(), nullptr)) {
        const int n = SDL_AddGamepadMappingsFromFile(own.c_str());
        std::printf("controllers: %d layouts from %s\n", n, own.c_str());
    }
}

// A gamepad on the generic layout: its input's name as the stick has it ("Button 3", "Hat up", "Axis 2 -"); "" if the
// pad has a real layout (the usual names then).
inline std::string generic_name(SDL_Gamepad* pad, int code, int axis_base) {
    if (!pad) return {};
    if (!generic_guids().count(guid_text(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(pad))))) return {};
    if (code >= axis_base) {
        const int axis = (code - axis_base) / 2;
        const bool positive = (code - axis_base) % 2;
        if (axis < static_cast<int>(std::size(kGenericAxes))) return "Axis " + std::to_string(axis + 1) + (positive ? " +" : " -");
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return "Button 7";
        if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return "Button 8";
        return {};
    }
    const auto b = static_cast<SDL_GamepadButton>(code - 1);
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "Hat up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "Hat down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "Hat left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "Hat right";
    default: break;
    }
    const char* key = SDL_GetGamepadStringForButton(b);
    for (std::size_t i = 0; key && i < std::size(kGenericButtons); ++i)
        if (std::string(key) == kGenericButtons[i]) return "Button " + std::to_string(i + 1);
    return {};
}

} // namespace tools::pads
