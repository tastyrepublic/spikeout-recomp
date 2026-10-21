// After vf3-recomp's src/host/menu.cpp (MIT License, Copyright (c) 2026
// mr-finster: https://github.com/mr-finster/vf3-recomp), drawn through
// RmlUi's SDL GPU renderer (third_party/rmlui_sdl_gpu) instead of an SDL
// renderer, with its page, style and fonts embedded.
#include "ui/menu.hpp"

#include "ui/assets.hpp"

#include "RmlUi_Renderer_SDL_GPU.h"

#include <RmlUi/Core.h>

#include <cstdlib>

namespace ui {

namespace {

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

int first_selectable(const MenuPage& p) {
    for (size_t i = 0; i < p.items.size(); i++)
        if (!p.items[i].info) return static_cast<int>(i);
    return 0;
}

Rml::Span<const Rml::byte> bytes(const Asset& a) { return {a.data, a.size}; }

} // namespace

// The mouse over a row selects it; a click selects and uses it.
struct MenuListener : Rml::EventListener {
    Menu* menu = nullptr;
    void ProcessEvent(Rml::Event& ev) override {
        Rml::Element* e = ev.GetTargetElement();
        while (e && !e->IsClassSet("row")) e = e->GetParentNode();
        if (!e || e->IsClassSet("info")) return;
        const int i = std::atoi(e->GetId().c_str() + 1);
        menu->select(i);
        if (ev.GetId() == Rml::EventId::Click) {
            const MenuItem& it = menu->stack_.back().items[static_cast<size_t>(i)];
            const Rml::Element* t = ev.GetTargetElement();
            if (it.activate) menu->activate();
            else if (it.change) menu->click_change(t && t->IsClassSet("back") ? -1 : +1); // the "<": down
        }
    }
};

struct Menu::Impl {
    SDL_Window* window = nullptr;
    std::unique_ptr<RenderInterface_SDL_GPU> gfx;
    Rml::Context* ctx = nullptr;
    Rml::ElementDocument* doc = nullptr;
    Rml::ElementDocument* fps = nullptr; // the frame rate (Menu::set_fps)
    MenuListener listener;
};

Menu::Menu() : impl_(std::make_unique<Impl>()) {}

Menu::~Menu() { shutdown(); }

void Menu::shutdown() {
    if (impl_->ctx) {
        Rml::RemoveContext(impl_->ctx->GetName());
        impl_->ctx = nullptr;
        impl_->doc = nullptr;
        impl_->fps = nullptr;
    }
    if (impl_->gfx) {
        Rml::Shutdown();
        impl_->gfx->Shutdown();
        impl_->gfx.reset();
    }
    open_ = false;
}

bool Menu::init(SDL_GPUDevice* device, SDL_Window* window) {
    impl_->window = window;
    impl_->gfx = std::make_unique<RenderInterface_SDL_GPU>(device, window);
    Rml::SetRenderInterface(impl_->gfx.get());
    if (!Rml::Initialise()) {
        error_ = "RmlUi didn't start";
        return false;
    }
    if (!Rml::LoadFontFace(bytes(assets::font_medium), "Barlow Condensed", Rml::Style::FontStyle::Normal,
                           Rml::Style::FontWeight(500)) ||
        !Rml::LoadFontFace(bytes(assets::font_semibold), "Barlow Condensed", Rml::Style::FontStyle::Normal,
                           Rml::Style::FontWeight(600)) ||
        !Rml::LoadFontFace(bytes(assets::font_bold_italic), "Barlow Condensed", Rml::Style::FontStyle::Italic,
                           Rml::Style::FontWeight::Bold)) {
        error_ = "the menu's fonts didn't load";
        return false;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    impl_->ctx = Rml::CreateContext("menu", Rml::Vector2i(w, h));
    if (!impl_->ctx) {
        error_ = "no RmlUi context";
        return false;
    }
    // The page with its style sheet inside (both embedded).
    std::string page(assets::menu_rml.text());
    const size_t head = page.find("</head>");
    if (head != std::string::npos) page.insert(head, "<style>" + std::string(assets::menu_rcss.text()) + "</style>");
    impl_->doc = impl_->ctx->LoadDocumentFromMemory(page);
    if (!impl_->doc) {
        error_ = "the menu's page didn't load";
        return false;
    }
    impl_->listener.menu = this;
    impl_->doc->AddEventListener(Rml::EventId::Mouseover, &impl_->listener);
    impl_->doc->AddEventListener(Rml::EventId::Click, &impl_->listener);
    // The frame rate, top right, over the game (and the menu).
    impl_->fps = impl_->ctx->LoadDocumentFromMemory(
        "<rml><head><style>body { width: 100%; height: 100%; font-family: \"Barlow Condensed\"; font-weight: 600; } "
        "#fps { position: absolute; right: 16dp; top: 10dp; font-size: 26dp; color: #ffd34a; "
        "font-effect: outline(2dp #000000); }</style></head><body><div id=\"fps\"></div></body></rml>");
    return true;
}

void Menu::set_footer(const std::string& text) {
    if (impl_->doc)
        if (Rml::Element* e = impl_->doc->GetElementById("ver")) e->SetInnerRML(escape(text));
}

void Menu::set_fps(const std::string& text) {
    if (!impl_->fps) return;
    if (text.empty()) {
        impl_->fps->Hide();
    } else {
        if (Rml::Element* e = impl_->fps->GetElementById("fps")) e->SetInnerRML(escape(text));
        impl_->fps->Show();
    }
    fps_shown_ = !text.empty();
}

void Menu::open(MenuPage root) {
    if (!impl_->doc) return;
    stack_ = {std::move(root)};
    selected_stack_.clear();
    selected_ = first_selectable(stack_.back());
    build();
    impl_->doc->Show();
    open_ = true;
    SDL_ShowCursor();
}

void Menu::close() {
    if (!impl_->doc) return;
    capture_ = nullptr;
    impl_->doc->Hide();
    open_ = false;
    stack_.clear();
    SDL_HideCursor();
}

void Menu::push(MenuPage page) {
    selected_stack_.push_back(selected_);
    stack_.push_back(std::move(page));
    selected_ = first_selectable(stack_.back());
    build();
}

void Menu::pop() {
    capture_ = nullptr;
    if (stack_.size() <= 1) {
        close();
        return;
    }
    stack_.pop_back();
    selected_ = selected_stack_.back();
    selected_stack_.pop_back();
    build();
}

void Menu::refresh() {
    if (open_) build();
}

void Menu::build() {
    const MenuPage& p = stack_.back();
    std::string crumb;
    for (size_t i = 1; i < stack_.size(); i++) crumb += (i > 1 ? "  /  " : "") + stack_[i].name;
    impl_->doc->GetElementById("crumb")->SetInnerRML(escape(crumb));
    std::string rml;
    for (size_t i = 0; i < p.items.size(); i++) {
        const MenuItem& it = p.items[i];
        rml += "<div class=\"row" + std::string(it.info ? " info" : "") + (static_cast<int>(i) == selected_ ? " selected" : "") +
               "\" id=\"r" + std::to_string(i) + "\"><span class=\"label\">" + escape(it.label) + "</span>";
        if (it.value) {
            rml += "<span class=\"value\">";
            if (it.change) rml += "<span class=\"arrow back\">&lt;&nbsp;&nbsp;</span>";
            rml += escape(it.value());
            if (it.change) rml += "<span class=\"arrow\">&nbsp;&nbsp;&gt;</span>";
            rml += "</span>";
        }
        rml += "</div>";
    }
    impl_->doc->GetElementById("rows")->SetInnerRML(rml);
    scroll_to_selected();
    show_help();
}

// A long page scrolls inside #rows: the selected row kept in view.
void Menu::scroll_to_selected() {
    Rml::Element *rows = impl_->doc->GetElementById("rows"), *e = impl_->doc->GetElementById("r" + std::to_string(selected_));
    if (!rows || !e) return;
    impl_->doc->UpdateDocument(); // the rows' layout, for their positions
    const float top = e->GetOffsetTop(), h = e->GetOffsetHeight(), view = rows->GetClientHeight();
    float st = rows->GetScrollTop();
    if (top < st) st = top;
    else if (top + h > st + view) st = top + h - view;
    rows->SetScrollTop(st);
}

void Menu::capture(std::string prompt, std::function<bool(const SDL_Event&)> on_event) {
    capture_prompt_ = std::move(prompt);
    capture_ = std::move(on_event);
    show_help();
}

void Menu::show_help() {
    if (capture_) {
        impl_->doc->GetElementById("note")->SetInnerRML("<span class=\"key\">" + escape(capture_prompt_) + "</span>");
        return;
    }
    const MenuPage& p = stack_.back();
    const auto sel = static_cast<size_t>(selected_);
    const std::string& h = sel < p.items.size() && !p.items[sel].help.empty() ? p.items[sel].help : p.note;
    impl_->doc->GetElementById("note")->SetInnerRML(escape(h));
}

void Menu::select(int i) {
    const MenuPage& p = stack_.back();
    if (i < 0 || i >= static_cast<int>(p.items.size()) || p.items[static_cast<size_t>(i)].info || i == selected_) return;
    if (auto* old = impl_->doc->GetElementById("r" + std::to_string(selected_))) old->SetClass("selected", false);
    selected_ = i;
    if (auto* e = impl_->doc->GetElementById("r" + std::to_string(selected_))) e->SetClass("selected", true);
    scroll_to_selected();
    show_help();
}

void Menu::move(int d) {
    const MenuPage& p = stack_.back();
    const int n = static_cast<int>(p.items.size());
    int i = selected_;
    for (int k = 0; k < n; k++) {
        i = (i + d + n) % n;
        if (!p.items[static_cast<size_t>(i)].info) {
            select(i);
            return;
        }
    }
}

void Menu::change(int d) {
    if (stack_.empty() || static_cast<size_t>(selected_) >= stack_.back().items.size()) return; // closed by a change
    auto f = stack_.back().items[static_cast<size_t>(selected_)].change; // a copy: it may rebuild the pages
    if (!f) return;
    f(d);
    refresh();
}

// A click: a step (the "<" down, anywhere else up), and from either end round
// to the other, so a choice that stops at its ends can't look stuck to a mouse.
void Menu::click_change(int d) {
    auto value = [&] {
        if (stack_.empty() || static_cast<size_t>(selected_) >= stack_.back().items.size()) return std::string("\x01gone");
        const MenuItem& it = stack_.back().items[static_cast<size_t>(selected_)];
        return it.value ? it.value() : std::string();
    };
    const std::string was = value();
    change(d);
    if (value() != was) return;
    for (int n = 0; n < 64; n++) { // at an end: to the other one
        const std::string at = value();
        if (at == "\x01gone") break;
        change(-d);
        if (value() == at) break;
    }
}

// A binding back to none: Delete on a keyboard, Y on a pad.
void Menu::clear() {
    if (stack_.empty() || static_cast<size_t>(selected_) >= stack_.back().items.size()) return;
    auto f = stack_.back().items[static_cast<size_t>(selected_)].clear;
    if (!f) return;
    f();
    refresh();
}

void Menu::activate() {
    const MenuItem& it = stack_.back().items[static_cast<size_t>(selected_)];
    if (it.activate) {
        auto f = it.activate; // a copy: it may push, pop or close
        f();
        if (open_) refresh();
    } else if (it.change) {
        change(+1);
    }
}

bool Menu::handle_event(const SDL_Event& e) {
    if (!open_) return false;
    if (capture_) {
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) return true;
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                capture_ = nullptr;
                build();
                return true;
            }
            [[fallthrough]];
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            auto f = capture_;
            if (f(e)) {
                capture_ = nullptr;
                build();
            }
            return true;
        }
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL: return true;
        default: break;
        }
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        switch (e.key.scancode) {
        case SDL_SCANCODE_UP: move(-1); break;
        case SDL_SCANCODE_DOWN: move(+1); break;
        case SDL_SCANCODE_LEFT: change(-1); break;
        case SDL_SCANCODE_RIGHT: change(+1); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            if (!e.key.repeat) activate();
            break;
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_BACKSPACE:
            if (!e.key.repeat) pop();
            break;
        case SDL_SCANCODE_DELETE:
            if (!e.key.repeat) clear();
            break;
        default: break;
        }
        return true;
    case SDL_EVENT_KEY_UP: return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: move(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(+1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: change(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: change(+1); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: activate(); break;
        case SDL_GAMEPAD_BUTTON_EAST: pop(); break;
        case SDL_GAMEPAD_BUTTON_NORTH: clear(); break;
        case SDL_GAMEPAD_BUTTON_START:
        case SDL_GAMEPAD_BUTTON_BACK:
        case SDL_GAMEPAD_BUTTON_GUIDE:
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: close(); break;
        default: break;
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP: return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        // The left stick moves like the d-pad, once per push.
        const int dir = e.gaxis.value < -16000 ? -1 : e.gaxis.value > 16000 ? 1 : 0;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            if (dir && dir != stick_y_) move(dir);
            stick_y_ = dir;
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            if (dir && dir != stick_x_) change(dir);
            stick_x_ = dir;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        const float d = SDL_GetWindowPixelDensity(impl_->window);
        impl_->ctx->ProcessMouseMove(static_cast<int>(e.motion.x * d), static_cast<int>(e.motion.y * d), 0);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT) impl_->ctx->ProcessMouseButtonDown(0, 0);
        else if (e.button.button == SDL_BUTTON_RIGHT) pop();
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT) impl_->ctx->ProcessMouseButtonUp(0, 0);
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e.wheel.y != 0) move(e.wheel.y > 0 ? -1 : +1);
        return true;
    default: return false;
    }
}

void Menu::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain, uint32_t width, uint32_t height) {
    if ((!open_ && !fps_shown_) || !impl_->ctx) return;
    impl_->ctx->SetDimensions(Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    impl_->ctx->SetDensityIndependentPixelRatio(static_cast<float>(height) / 720.0f);
    impl_->ctx->Update();
    impl_->gfx->BeginFrame(cmd, swapchain, width, height);
    impl_->ctx->Render();
    impl_->gfx->EndFrame();
}

} // namespace ui
