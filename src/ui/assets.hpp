// The menu's files, embedded in the program at build time from assets/
// (cmake/embed_file.cmake): its fonts and its page and style sheet.
#pragma once

#include <cstddef>
#include <string_view>

namespace ui {

struct Asset {
    const unsigned char* data;
    std::size_t size; // a 0 follows the data
    std::string_view text() const { return {reinterpret_cast<const char*>(data), size}; }
};

namespace assets {
extern const Asset menu_rml, menu_rcss;
extern const Asset font_medium, font_semibold, font_bold_italic; // Barlow Condensed (SIL OFL)
// Every country's flag (the network seat list) and its RmlUi sprite sheet ("flag-au" and so on), as vf3-recomp has
// them (its tools/flags/make_flags.py: flag-icons by Panayiotis Lipiridis, MIT, assets/ui/flags-LICENSE.txt)
extern const Asset flags_png, flags_rcss;
} // namespace assets

} // namespace ui
