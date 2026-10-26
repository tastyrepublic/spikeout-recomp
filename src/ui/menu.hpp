// The in-game menu, drawn with RmlUi over the paused game through SDL GPU
// (the page and style in assets/ui, embedded). The window builds the pages;
// the menu handles the keyboard, gamepads and mouse.
//
// After vf3-recomp's src/host/menu.h and menu.cpp (MIT, Copyright (c) 2026
// mr-finster): pages of items, each with its help text, its value and what
// Left / Right, Enter and Delete do; a capture mode for binding keys.
#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// One line of a page.
struct MenuItem {
    MenuItem(std::string label_, std::string help_ = {}, std::function<std::string()> value_ = {},
             std::function<void(int)> change_ = {}, std::function<void()> activate_ = {}, bool info_ = false,
             std::function<void()> clear_ = {})
        : label(std::move(label_)), help(std::move(help_)), value(std::move(value_)), change(std::move(change_)),
          activate(std::move(activate_)), info(info_), clear(std::move(clear_)) {}

    std::string label;
    std::string help;                   // under the items while this one is selected
    std::function<std::string()> value; // shown on the right (choices); none for buttons
    std::function<void(int)> change;    // Left / Right: -1 / +1 (choices)
    std::function<void()> activate;     // Enter / A / a click (buttons)
    bool info = false;                  // a line of text: not selectable
    std::function<void()> clear;        // Delete / Y: back to none (a key binding)
};

// A seat of a network game, in the list under the frame rate.
struct NetRow {
    enum class Look : uint8_t { you, player, fresh, dim }; // yellow, white, green (just joined), grey
    std::string seat;   // "2P"
    std::string name;
    std::string status; // in place of the bars and round trip (JOINED, LEFT, IDLE, ...); empty: them
    int bars = -1;      // 0-4 (-1: none)
    int ms = -1;        // the round trip (-1: none)
    Look look = Look::player;
};

// A page for typing a short code (a game's): a box of its letters and an
// on-screen keyboard of the letters it may have, for pads and the mouse;
// a keyboard types into it (and pastes, Ctrl+V).
struct CodeInput {
    std::string alphabet; // the letters a code may have (typed lower case: upper)
    std::size_t length = 6;      // at most
    std::size_t min_length = 6;  // for the done key (a name: 1)
    std::string done_label = "JOIN";
    std::string text;     // so far
    int key = 0;          // the on-screen key selected: the letters, then Delete, the done key
    std::function<void(const std::string&)> done; // the done key (Enter, Start), the text long enough
};

struct MenuPage {
    MenuPage(std::string name_ = {}, std::string note_ = {}, std::vector<MenuItem> items_ = {})
        : name(std::move(name_)), note(std::move(note_)), items(std::move(items_)) {}

    std::string name;
    std::string note; // under the items when the selected one has no help
    std::vector<MenuItem> items;
    std::shared_ptr<CodeInput> code; // a code page instead of items
};

class Menu {
public:
    Menu();
    ~Menu();
    // On the window's GPU device; false (error() says why) if RmlUi or the
    // fonts can't start.
    bool init(SDL_GPUDevice* device, SDL_Window* window);
    // Lets go of RmlUi and what it holds on the device (before the device goes).
    void shutdown();
    const std::string& error() const { return error_; }
    void set_footer(const std::string& text); // the small line at the bottom (version)
    // The frame rate in the top right corner, shown with the menu closed
    // too (empty: none).
    void set_fps(const std::string& text);
    // A network game's seats, under the frame rate (empty: none).
    void set_net(const std::vector<NetRow>& rows);

    bool is_open() const { return open_; }
    void open(MenuPage root);
    void close();
    void push(MenuPage page); // a sub-page
    void pop();               // back a page; closes at the top
    void refresh();           // the values again (changed from outside)
    // Waits for a key or pad button: on_event sees each input event until it
    // returns true. Esc cancels. The prompt shows under the items meanwhile.
    void capture(std::string prompt, std::function<bool(const SDL_Event&)> on_event);

    // Takes the event when the menu is open; true when it used it.
    bool handle_event(const SDL_Event& e);
    // Draws the menu (when open) and the frame rate over the swapchain, in the command buffer
    // the window's picture was drawn in (gpu::Presenter's overlay).
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain, uint32_t width, uint32_t height);

    struct Impl;

private:
    friend struct MenuListener;
    void build();
    void show_help();
    void scroll_to_selected();
    void move(int d);
    void select(int i);
    void change(int d);
    void click_change(int d);
    void activate();
    void clear();
    // A code page (MenuPage::code).
    void build_code();
    bool code_event(const SDL_Event& e);
    void code_move(int dx, int dy);
    void code_press(int key);
    void code_type(const std::string& letters);

    std::unique_ptr<Impl> impl_;
    std::string error_;
    bool open_ = false;
    bool fps_shown_ = false, net_shown_ = false;
    std::string net_rml_; // (what the seat list shows: not set again unchanged)
    void show_corner();   // the corner page shown while either is
    std::vector<MenuPage> stack_;
    std::vector<int> selected_stack_;
    int selected_ = 0;
    int stick_x_ = 0, stick_y_ = 0; // the left stick's last direction, for its edges
    bool mouse_moved_ = false;      // the mouse moved since the last key or button (a code page: it selects)
    std::function<bool(const SDL_Event&)> capture_;
    std::string capture_prompt_;
};

} // namespace ui
