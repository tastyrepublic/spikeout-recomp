// After vf3-recomp's src/host/menu.cpp (MIT License, Copyright (c) 2026
// mr-finster: https://github.com/mr-finster/vf3-recomp), drawn through
// RmlUi's SDL GPU renderer (third_party/rmlui_sdl_gpu) instead of an SDL
// renderer, with its page, style and fonts embedded.
#include "ui/menu.hpp"

#include "ui/assets.hpp"

#include "RmlUi_Renderer_SDL_GPU.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

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

// RmlUi's files: the flags' picture (flags.rcss's "src: flags.png") from the program itself, anything else
// from the disk as RmlUi's own would.
struct AssetFiles : Rml::FileInterface {
    struct Open {
        const Asset* asset = nullptr;
        std::FILE* file = nullptr;
        std::size_t at = 0;
    };
    Rml::FileHandle Open(const Rml::String& path) override {
        auto* f = new struct Open;
        if (path.size() >= 9 && path.compare(path.size() - 9, 9, "flags.png") == 0) f->asset = &assets::flags_png;
        else if (path.size() >= 13 && path.compare(path.size() - 13, 13, "platforms.png") == 0) f->asset = &assets::platforms_png;
        else if (!(f->file = std::fopen(path.c_str(), "rb"))) return delete f, 0;
        return reinterpret_cast<Rml::FileHandle>(f);
    }
    void Close(Rml::FileHandle h) override {
        auto* f = reinterpret_cast<struct Open*>(h);
        if (f->file) std::fclose(f->file);
        delete f;
    }
    size_t Read(void* buffer, size_t size, Rml::FileHandle h) override {
        auto* f = reinterpret_cast<struct Open*>(h);
        if (f->file) return std::fread(buffer, 1, size, f->file);
        const std::size_t n = std::min(size, f->asset->size - std::min(f->at, f->asset->size));
        std::memcpy(buffer, f->asset->data + f->at, n);
        f->at += n;
        return n;
    }
    bool Seek(Rml::FileHandle h, long offset, int origin) override {
        auto* f = reinterpret_cast<struct Open*>(h);
        if (f->file) return std::fseek(f->file, offset, origin) == 0;
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? static_cast<long>(f->at) : static_cast<long>(f->asset->size);
        if (base + offset < 0) return false;
        f->at = static_cast<std::size_t>(base + offset);
        return true;
    }
    size_t Tell(Rml::FileHandle h) override {
        auto* f = reinterpret_cast<struct Open*>(h);
        return f->file ? static_cast<size_t>(std::ftell(f->file)) : f->at;
    }
};

// The two-letter codes flags.rcss has a flag for ("flag-au: ...").
std::set<std::string> flag_codes() {
    std::set<std::string> out;
    const std::string_view s = assets::flags_rcss.text();
    for (std::size_t at = s.find("flag-"); at != std::string_view::npos; at = s.find("flag-", at + 1))
        if (at + 7 < s.size() && s[at + 7] == ':') out.emplace(s.substr(at + 5, 2));
    return out;
}

} // namespace

// The mouse over a row selects it; a click selects and uses it.
struct MenuListener : Rml::EventListener {
    Menu* menu = nullptr;
    void ProcessEvent(Rml::Event& ev) override {
        Rml::Element* e = ev.GetTargetElement();
        if (!menu->stack_.empty() && menu->stack_.back().code) { // a code page's on-screen key
            while (e && !e->IsClassSet("ckey")) e = e->GetParentNode();
            if (!e) return;
            const int k = std::atoi(e->GetId().c_str() + 1);
            if (ev.GetId() == Rml::EventId::Click) {
                menu->code_press(k);
            } else if (menu->mouse_moved_ && menu->stack_.back().code->key != k) { // (not a key redrawn under a still pointer)
                menu->stack_.back().code->key = k;
                menu->build_code();
            }
            return;
        }
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
    AssetFiles files;
    std::set<std::string> flags = flag_codes();
    float ratio = 1; // (the dp scale, the window's height / 720: set as it's drawn)
    // A length in whole pixels (the signal bars: at a scale that isn't whole, each bar's dp rounded where it lies
    // made them differ in width).
    std::string px(double dp) const { return std::to_string(std::max(1L, std::lround(dp * ratio))) + "px"; }
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
    Rml::SetFileInterface(&impl_->files);
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
    if (head != std::string::npos)
        page.insert(head, "<style>" + std::string(assets::menu_rcss.text()) + std::string(assets::flags_rcss.text()) +
                              std::string(assets::platforms_rcss.text()) + "</style>");
    impl_->doc = impl_->ctx->LoadDocumentFromMemory(page);
    if (!impl_->doc) {
        error_ = "the menu's page didn't load";
        return false;
    }
    impl_->listener.menu = this;
    impl_->doc->AddEventListener(Rml::EventId::Mouseover, &impl_->listener);
    impl_->doc->AddEventListener(Rml::EventId::Click, &impl_->listener);
    // The frame rate, top right, over the game (and the menu), and a network
    // game's seats under it in the same style (each player's flag from
    // flags.rcss's sprite sheet).
    impl_->fps = impl_->ctx->LoadDocumentFromMemory(
        "<rml><head><style>" + std::string(assets::flags_rcss.text()) + std::string(assets::platforms_rcss.text()) +
        "body { width: 100%; height: 100%; font-family: \"Barlow Condensed\"; font-weight: 600; } "
        "#corner { position: absolute; right: 16dp; top: 10dp; } "
        "#fps { display: block; text-align: right; font-size: 26dp; color: #ffd34a; font-effect: outline(2dp #000000); } "
        "#net { position: absolute; right: 16dp; top: 35%; } " // (the seats down at the side, a little above the middle: the owner's choice)
        ".head { display: flex; justify-content: flex-end; align-items: center; height: 24dp; margin-bottom: 8dp; font-size: 20dp; color: #ffd34a; font-effect: outline(2dp #000000); } "
        ".seat { display: flex; justify-content: flex-end; align-items: flex-end; height: 24dp; font-size: 20dp; "
        "color: #ffd34a; font-effect: outline(2dp #000000); } "
        ".seat.player { color: #ffffff; } .seat.fresh { color: #7fe07f; } .seat.dim { color: #9a9a9a; } "
        ".seat .p { margin-right: 6dp; } .seat .name { white-space: nowrap; } " // (right-aligned, each as wide as it is: a long name the row out to the left)
        ".seat .st { margin-left: 8dp; white-space: nowrap; } "
        ".conn { display: flex; justify-content: flex-end; align-items: flex-end; margin-left: 4dp; } "
        ".bars { display: flex; align-items: flex-end; height: 16dp; margin-bottom: 3dp; margin-left: 5dp; } "
        ".bar { width: 3dp; margin-left: 1dp; background-color: #4a4a4a; border: 1dp #000000; } "
        ".g .on { background-color: #5fd35f; } .y .on { background-color: #ffd34a; } .r .on { background-color: #e05555; } "
        ".ms { width: 48dp; text-align: right; white-space: nowrap; } " // (as wide as "999 ms": the name stays put as it changes)
        ".flag { width: 19dp; height: 14dp; margin: 0 6dp 4dp 0; border: 1dp #000000; } " // (as tall as the 20dp text's capitals)
        ".noflag { width: 21dp; height: 14dp; margin: 0 6dp 4dp 0; } "
        ".head .eye { width: 22dp; height: 22dp; margin: 0 3dp 0 10dp; image-color: #ffd34a; } " // (the spectators' count)
        // (a spectator's banner: bottom middle, over the game, its keys as key caps)
        "#watchb { display: none; position: absolute; left: 0; width: 100%; bottom: 28dp; text-align: center; } "
        "#watchb { font-size: 22dp; color: #ffffff; font-effect: outline(2dp #000000); } "
        "#watchb .key { font-size: 15dp; padding: 0 6dp; margin: 0 2dp; border: 1dp #c8ccd4; border-radius: 4dp; "
        "background-color: #2a2e38; font-effect: none; } "
        "#watchb .pill { display: inline-block; padding: 4dp 14dp; background-color: #0b0d12d9; border: 1dp #000000; border-radius: 16dp; } "
        ".plat { width: 14dp; height: 14dp; margin: 0 6dp 4dp 0; } .noplat { width: 14dp; height: 14dp; margin: 0 6dp 4dp 0; } " // (as the flag: the capitals' height, on their line)
        ".badges { display: flex; justify-content: flex-end; height: 19dp; margin-bottom: 3dp; } " // (a seat's badges, under it)
        ".badge { font-size: 13dp; line-height: 16dp; height: 16dp; padding: 0 6dp; margin-left: 4dp; border-radius: 8dp; "
        "border: 1dp #000000; font-effect: none; } .badge.mode { background-color: #3a3f4a; color: #ffffff; } "
        ".badge.g { background-color: #5fd35f; color: #0d2a0d; } .badge.y { background-color: #ffd34a; color: #2a2000; } "
        ".badge.r { background-color: #e05555; color: #ffffff; } "
        ".pills { display: flex; justify-content: flex-end; height: 22dp; margin: 2dp 0 4dp 0; } "
        ".pill { font-size: 15dp; line-height: 18dp; height: 18dp; padding: 0 8dp; margin-left: 5dp; border-radius: 9dp; "
        "border: 1dp #000000; } "
        ".pill.delay { background-color: #3a3f4a; color: #ffffff; } "
        // (a spectator's switch taking a while: the game dimmed, still running, the new player named over it)
        "#swcover { display: none; position: absolute; left: 0; top: 0; width: 100%; height: 100%; background-color: #000000cc; } "
        "#swcover .t { position: absolute; left: 0; width: 100%; top: 44%; text-align: center; font-size: 40dp; color: #ffd34a; "
        "font-effect: outline(2dp #000000); } "
        "#loading { display: none; position: absolute; left: 0; top: 0; width: 100%; height: 100%; background-color: #0b0d12; } "
        "#loading .box { position: absolute; left: 0; width: 100%; top: 42%; text-align: center; } "
        "#loading .title { display: block; font-size: 44dp; color: #ffd34a; font-effect: outline(2dp #000000); } "
        "#loading .detail { display: block; margin-top: 10dp; font-size: 22dp; color: #c8ccd4; } "
        "#lbar { display: block; width: 420dp; height: 22dp; margin: 18dp auto 0 auto; background-color: #2a2e38; "
        "border: 2dp #000000; border-radius: 11dp; } "
        "#lfill { display: block; height: 100%; width: 0%; background-color: #ffd34a; border-radius: 9dp 0dp 0dp 9dp; } "
        "#lpct { display: block; margin-top: 8dp; font-size: 26dp; color: #ffffff; font-effect: outline(2dp #000000); } "
        "</style></head><body><div id=\"loading\"><div class=\"box\"><span class=\"title\" id=\"ltitle\"></span>"
        "<div id=\"lbar\"><div id=\"lfill\"></div></div><span id=\"lpct\"></span>"
        "<span class=\"detail\" id=\"ldetail\"></span></div></div>"
        "<div id=\"swcover\"><span class=\"t\" id=\"swtext\"></span></div><div id=\"corner\"><div id=\"fps\"></div></div><div id=\"net\"></div><div id=\"watchb\"></div></body></rml>");
    return true;
}

void Menu::show_corner() {
    if (!impl_->fps) return;
    if (fps_shown_ || net_shown_ || loading_shown_ || banner_shown_ || cover_shown_) impl_->fps->Show();
    else impl_->fps->Hide();
}

void Menu::set_net(const std::vector<NetRow>& rows, const std::string& header, const NetPills& pills) {
    if (!impl_->fps) return;
    // (the spectators: an eye and their count after the header, in its colour; the input delay: each player's own now,
    // in their badges)
    // (a row centred on the text's middle: the eye on it, not on the baseline; text in spans, as a flex box draws no
    // loose text)
    std::string rml = header.empty() ? std::string()
                                     : "<div class=\"head\"><span>" + escape(header) + "</span>" +
                                           (pills.watchers > 0 ? "<img class=\"eye\" sprite=\"eye\"/><span>" + std::to_string(pills.watchers) + "</span>"
                                                               : std::string()) +
                                           "</div>";
    // (a platform's icon, then a flag, between the seat and the name; the icon in the row's colour, as its text; each
    // row right-aligned, its parts as wide as they are)
    auto has_flag = [&](const NetRow& r) { return impl_->flags.count(r.country) != 0; };
    for (const NetRow& r : rows) {
        static const char* const kLooks[] = {"you", "player", "fresh", "dim"};
        static const char* const kTints[] = {"#ffd34a", "#ffffff", "#7fe07f", "#9a9a9a"};
        rml += "<div class=\"seat " + std::string(kLooks[static_cast<int>(r.look)]) + "\"><span class=\"p\">" + escape(r.seat) + "</span>";
        if (!r.platform.empty())
            rml += "<img class=\"plat\" sprite=\"platform-" + escape(r.platform) + "\" style=\"image-color: " +
                   kTints[static_cast<int>(r.look)] + ";\"/>";
        if (has_flag(r)) rml += "<img class=\"flag\" sprite=\"flag-" + r.country + "\"/>";
        rml += "<span class=\"name\">" + escape(r.name) + "</span>";
        if (!r.status.empty()) { // (our own row: none, its name at the right edge as the others' bars)
            rml += "<span class=\"st\">" + escape(r.status) + "</span>";
        } else if (r.bars >= 0) {
            rml += "<div class=\"conn\"><span class=\"ms\">" + (r.ms >= 0 ? std::to_string(r.ms) + " ms" : std::string()) +
                   "</span><div class=\"bars " + (r.bars >= 3 ? "g" : r.bars == 2 ? "y" : "r") + "\">";
            for (int b = 0; b < 4; ++b) // (whole pixels: Impl::px)
                rml += "<div class=\"bar" + std::string(b < r.bars ? " on" : "") + "\" style=\"height: " + impl_->px(5 + 3 * b) +
                       "; width: " + impl_->px(3) + "; margin-left: " + impl_->px(1) + "; border-width: " + impl_->px(1) + ";\"></div>";
            rml += "</div></div>";
        }
        rml += "</div>";
        // (its badges on a line of their own under it: how its machine plays, and how often it was gone back for lately
        // (few green, some yellow, many red: the bands to be tuned with real networks))
        if (!r.mode.empty() || r.rollbacks >= 0) {
            rml += "<div class=\"badges\">";
            if (!r.mode.empty()) rml += "<span class=\"badge mode\">" + escape(r.mode) + "</span>";
            if (r.rollbacks >= 0)
                rml += "<span class=\"badge " + std::string(r.rollbacks <= 2 ? "g" : r.rollbacks <= 15 ? "y" : "r") + "\">RB " +
                       std::to_string(r.rollbacks) + "</span>";
            rml += "</div>";
        }
    }
    net_shown_ = !rows.empty() || !header.empty();
    if (rml != net_rml_) {
        net_rml_ = rml;
        if (Rml::Element* e = impl_->fps->GetElementById("net")) e->SetInnerRML(rml);
    }
    show_corner();
}

void Menu::set_loading(const std::string& title, int percent, const std::string& detail) {
    if (!impl_->fps) return;
    const bool shown = !title.empty();
    if (Rml::Element* e = impl_->fps->GetElementById("loading")) e->SetProperty("display", shown ? "block" : "none");
    if (shown) {
        const int p = std::clamp(percent, 0, 100);
        if (Rml::Element* e = impl_->fps->GetElementById("ltitle")) e->SetInnerRML(escape(title));
        if (Rml::Element* e = impl_->fps->GetElementById("lbar")) e->SetProperty("display", percent >= 0 ? "block" : "none");
        if (Rml::Element* e = impl_->fps->GetElementById("lfill")) e->SetProperty("width", std::to_string(p) + "%");
        if (Rml::Element* e = impl_->fps->GetElementById("lpct"))
            e->SetInnerRML(percent >= 0 ? std::to_string(p) + "%" : std::string());
        if (Rml::Element* e = impl_->fps->GetElementById("ldetail")) e->SetInnerRML(escape(detail));
    }
    loading_shown_ = shown;
    show_corner();
}

void Menu::set_switch_cover(const std::string& text) {
    if (!impl_->fps) return;
    if (Rml::Element* e = impl_->fps->GetElementById("swtext")) e->SetInnerRML(escape(text));
    if (Rml::Element* e = impl_->fps->GetElementById("swcover")) e->SetProperty("display", text.empty() ? "none" : "block");
    cover_shown_ = !text.empty();
    show_corner();
}

void Menu::set_watch_banner(const std::string& who, const std::string& note) {
    if (!impl_->fps) return;
    const bool shown = !who.empty() || !note.empty();
    if (Rml::Element* e = impl_->fps->GetElementById("watchb")) {
        if (!note.empty())
            e->SetInnerRML("<span class=\"pill\">" + escape(note) + "</span>");
        else if (!who.empty())
            e->SetInnerRML("<span class=\"pill\">WATCHING " + escape(who) + "&nbsp;&nbsp;<span class=\"key\">LEFT</span><span class=\"key\">RIGHT</span>"
                           " / <span class=\"key\">LB</span><span class=\"key\">RB</span>&nbsp;another player</span>");
        e->SetProperty("display", shown ? "block" : "none");
    }
    banner_shown_ = shown;
    show_corner();
}

void Menu::set_footer(const std::string& text) {
    if (impl_->doc)
        if (Rml::Element* e = impl_->doc->GetElementById("ver")) e->SetInnerRML(escape(text));
}

void Menu::set_fps(const std::string& text) {
    if (!impl_->fps) return;
    if (Rml::Element* e = impl_->fps->GetElementById("fps")) {
        e->SetInnerRML(escape(text));
        e->SetProperty("display", text.empty() ? "none" : "block");
    }
    fps_shown_ = !text.empty();
    show_corner();
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
    if (p.code) {
        build_code();
        return;
    }
    std::string rml;
    for (size_t i = 0; i < p.items.size(); i++) {
        const MenuItem& it = p.items[i];
        rml += "<div class=\"row" + std::string(it.info ? " info" : "") + (static_cast<int>(i) == selected_ ? " selected" : "") +
               "\" id=\"r" + std::to_string(i) + "\"><span class=\"label\">" +
               (impl_->flags.count(it.country) ? "<img class=\"flag\" sprite=\"flag-" + it.country + "\"/>" : std::string()) +
               escape(it.label) +
               (it.watchers > 0 ? "<img class=\"eye\" sprite=\"eye\"/>" + std::to_string(it.watchers) : std::string()) + "</span>";
        if (it.estimate_ms >= 0) { // (the same bars as the seat list's, by the same bands)
            const int ms = it.estimate_ms, bars = ms < 40 ? 4 : ms < 80 ? 3 : ms < 140 ? 2 : 1;
            rml += "<span class=\"speed\"><span class=\"bars " + std::string(bars >= 3 ? "g" : bars == 2 ? "y" : "r") + "\">";
            for (int b = 0; b < 4; ++b)
                rml += "<span class=\"bar" + std::string(b < bars ? " on" : "") + "\" style=\"height: " + impl_->px(4 + 4 * b) +
                       "; width: " + impl_->px(4) + "; margin-left: " + impl_->px(2) + "; border-width: " + impl_->px(1) + ";\"></span>";
            rml += "</span><span>~" + std::to_string(ms) + " ms</span></span>"; // (its own span: loose text in a flex box isn't drawn)
        }
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

// ---- A code page (MenuPage::code) ----
// The code's box, then the on-screen keyboard: its letters kCodeColumns a
// row, and a row of Delete and Join (each half the width).
namespace {
constexpr int kCodeColumns = 8;
}

void Menu::build_code() {
    const CodeInput& c = *stack_.back().code;
    const int letters = static_cast<int>(c.alphabet.size());
    std::string rml = "<div id=\"codebox\">";
    for (std::size_t i = 0; i < c.length; ++i)
        rml += std::string("<span class=\"slot") + (i == c.text.size() ? " cur" : "") + "\">" +
               (i < c.text.size() ? escape(std::string(1, c.text[i])) : std::string("&nbsp;")) + "</span>";
    rml += "</div>";
    auto key = [&](int k, const std::string& text, const char* more) {
        rml += std::string("<span class=\"ckey") + more + (k == c.key ? " sel" : "") + "\" id=\"k" + std::to_string(k) + "\">" +
               escape(text) + "</span>";
    };
    for (int k = 0; k < letters; ++k) {
        if (k % kCodeColumns == 0) rml += "<div class=\"krow\">";
        const char letter = c.alphabet[static_cast<std::size_t>(k)];
        key(k, letter == ' ' ? std::string("SPACE") : std::string(1, letter), letter == ' ' ? " space" : "");
        if (k % kCodeColumns == kCodeColumns - 1 || k == letters - 1) rml += "</div>";
    }
    rml += "<div class=\"krow\">";
    key(letters, "DELETE", " wide");
    key(letters + 1, c.done_label, c.text.size() >= c.min_length ? " wide ready" : " wide");
    rml += "</div>";
    impl_->doc->GetElementById("rows")->SetInnerRML(rml);
    impl_->doc->GetElementById("note")->SetInnerRML(escape(stack_.back().note));
}

void Menu::code_move(int dx, int dy) {
    CodeInput& c = *stack_.back().code;
    const int letters = static_cast<int>(c.alphabet.size());
    const int rows = (letters + kCodeColumns - 1) / kCodeColumns + 1; // (the last: Delete, Join)
    int row = c.key < letters ? c.key / kCodeColumns : rows - 1;
    int col = c.key < letters ? c.key % kCodeColumns : c.key == letters ? 0 : kCodeColumns / 2;
    if (row == rows - 1) col = (col + dx * (kCodeColumns / 2) + kCodeColumns) % kCodeColumns; // (two keys)
    else col = (col + dx + kCodeColumns) % kCodeColumns;
    row = (row + dy + rows) % rows;
    if (row == rows - 1) c.key = col < kCodeColumns / 2 ? letters : letters + 1;
    else c.key = std::min(row * kCodeColumns + col, letters - 1);
    build_code();
}

void Menu::code_type(const std::string& typed) {
    CodeInput& c = *stack_.back().code;
    for (char ch : typed) {
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (c.text.size() < c.length && c.alphabet.find(up) != std::string::npos) c.text += up;
    }
    if (c.text.size() == c.length) c.key = static_cast<int>(c.alphabet.size()) + 1; // (full: on to the done key, for a pad)
    build_code();
}

void Menu::code_press(int k) {
    std::shared_ptr<CodeInput> c = stack_.back().code; // (kept: Join may pop the page)
    const int letters = static_cast<int>(c->alphabet.size());
    c->key = k;
    if (k < letters) {
        code_type(std::string(1, c->alphabet[static_cast<std::size_t>(k)]));
    } else if (k == letters) {
        if (!c->text.empty()) c->text.pop_back();
        build_code();
    } else if (c->text.size() >= c->min_length && c->done) {
        auto f = c->done;
        f(c->text);
    } else {
        build_code();
    }
}

// The keyboard types (and pastes), Backspace deletes a letter, Delete all
// of them, Enter joins; the arrows, a pad and the mouse use the on-screen
// keyboard. Esc / B: back.
bool Menu::code_event(const SDL_Event& e) {
    CodeInput& c = *stack_.back().code;
    const int letters = static_cast<int>(c.alphabet.size());
    if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) mouse_moved_ = false;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        switch (e.key.scancode) {
        case SDL_SCANCODE_ESCAPE: if (!e.key.repeat) pop(); return true;
        case SDL_SCANCODE_BACKSPACE: code_press(letters); return true;
        case SDL_SCANCODE_DELETE:
            c.text.clear();
            build_code();
            return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: if (!e.key.repeat) code_press(letters + 1); return true;
        case SDL_SCANCODE_SPACE: // (a space where the text may have one; else the on-screen key)
            if (c.alphabet.find(' ') != std::string::npos) code_type(" ");
            else if (!e.key.repeat) code_press(c.key);
            return true;
        case SDL_SCANCODE_LEFT: code_move(-1, 0); return true;
        case SDL_SCANCODE_RIGHT: code_move(+1, 0); return true;
        case SDL_SCANCODE_UP: code_move(0, -1); return true;
        case SDL_SCANCODE_DOWN: code_move(0, +1); return true;
        default: break;
        }
        const SDL_Keycode k = e.key.key ? e.key.key : SDL_GetKeyFromScancode(e.key.scancode, e.key.mod, false);
        if ((e.key.mod & SDL_KMOD_CTRL) && k == SDLK_V) {
            if (char* clip = SDL_GetClipboardText()) {
                code_type(clip);
                SDL_free(clip);
            }
        } else if (k >= SDLK_A && k <= SDLK_Z) {
            code_type(std::string(1, static_cast<char>('A' + (k - SDLK_A))));
        } else if (k >= SDLK_0 && k <= SDLK_9) {
            code_type(std::string(1, static_cast<char>('0' + (k - SDLK_0))));
        } else if (e.key.scancode >= SDL_SCANCODE_KP_1 && e.key.scancode <= SDL_SCANCODE_KP_9) {
            code_type(std::string(1, static_cast<char>('1' + (e.key.scancode - SDL_SCANCODE_KP_1))));
        } else if (e.key.scancode == SDL_SCANCODE_KP_0) {
            code_type("0");
        } else if (k == SDLK_MINUS || e.key.scancode == SDL_SCANCODE_KP_MINUS) {
            code_type((e.key.mod & SDL_KMOD_SHIFT) && k == SDLK_MINUS ? "_" : "-");
        } else if (k == SDLK_PERIOD || e.key.scancode == SDL_SCANCODE_KP_PERIOD) {
            code_type(".");
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: code_move(0, -1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: code_move(0, +1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: code_move(-1, 0); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: code_move(+1, 0); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: code_press(c.key); break;
        case SDL_GAMEPAD_BUTTON_EAST: pop(); break;
        case SDL_GAMEPAD_BUTTON_NORTH: code_press(letters); break;
        case SDL_GAMEPAD_BUTTON_START: code_press(letters + 1); break;
        case SDL_GAMEPAD_BUTTON_BACK:
        case SDL_GAMEPAD_BUTTON_GUIDE: close(); break;
        default: break;
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const int dir = e.gaxis.value < -16000 ? -1 : e.gaxis.value > 16000 ? 1 : 0;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            if (dir && dir != stick_y_) code_move(0, dir);
            stick_y_ = dir;
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            if (dir && dir != stick_x_) code_move(dir, 0);
            stick_x_ = dir;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: return true;
    default: return false; // (the mouse: the keys' own events)
    }
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
    if (!stack_.empty() && stack_.back().code && code_event(e)) return true;
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
        mouse_moved_ = true;
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
    if ((!open_ && !fps_shown_ && !net_shown_ && !loading_shown_ && !banner_shown_ && !cover_shown_) || !impl_->ctx) return;
    impl_->ctx->SetDimensions(Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    impl_->ratio = static_cast<float>(height) / 720.0f;
    impl_->ctx->SetDensityIndependentPixelRatio(static_cast<float>(height) / 720.0f);
    impl_->ctx->Update();
    impl_->gfx->BeginFrame(cmd, swapchain, width, height);
    impl_->ctx->Render();
    impl_->gfx->EndFrame();
}

} // namespace ui
