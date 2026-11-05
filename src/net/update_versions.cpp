// The update check's version rules (src/net/update.hpp), built with or
// without libcurl (the unit tests use them).
#include "net/update.hpp"

#include <cctype>
#include <cstdlib>

namespace net {

namespace {

struct Parsed {
    unsigned long numbers[3] = {0, 0, 0};
    std::string label;       // its letters ("preview")
    unsigned long count = 0; // its number (1 of "preview1")
    bool labelled = false;
};

Parsed parse(std::string_view v) {
    Parsed p;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
    std::size_t at = 0;
    for (int i = 0; i < 3 && at < v.size(); ++i) {
        unsigned long n = 0;
        while (at < v.size() && std::isdigit(static_cast<unsigned char>(v[at]))) n = n * 10 + static_cast<unsigned long>(v[at++] - '0');
        p.numbers[i] = n;
        if (at < v.size() && v[at] == '.') ++at;
        else break;
    }
    if (at < v.size() && (v[at] == '-' || v[at] == '+')) {
        p.labelled = v[at] == '-';
        ++at;
        while (at < v.size() && !std::isdigit(static_cast<unsigned char>(v[at])))
            p.label += static_cast<char>(std::tolower(static_cast<unsigned char>(v[at++])));
        while (at < v.size() && std::isdigit(static_cast<unsigned char>(v[at]))) p.count = p.count * 10 + static_cast<unsigned long>(v[at++] - '0');
    }
    return p;
}

} // namespace

int compare_versions(std::string_view a, std::string_view b) {
    const Parsed x = parse(a), y = parse(b);
    for (int i = 0; i < 3; ++i)
        if (x.numbers[i] != y.numbers[i]) return x.numbers[i] < y.numbers[i] ? -1 : 1;
    if (x.labelled != y.labelled) return x.labelled ? -1 : 1; // (a test build before its release)
    if (const int c = x.label.compare(y.label); c != 0) return c < 0 ? -1 : 1;
    if (x.count != y.count) return x.count < y.count ? -1 : 1;
    return 0;
}

bool prerelease(std::string_view version) { return parse(version).labelled; }

bool dev_build(std::string_view version) { return parse(version).label == "dev"; }

std::optional<Release> newer_release(const Json& releases, std::string_view current) {
    std::optional<Release> best;
    for (const Json& r : releases.items()) {
        if (r["draft"].as_bool()) continue;
        std::string tag = r["tag_name"].as_string();
        if (tag.empty()) continue;
        if (tag[0] == 'v' || tag[0] == 'V') tag.erase(0, 1);
        if (r["prerelease"].as_bool() && !prerelease(current)) continue; // (test builds only offered to test builds)
        if (compare_versions(tag, current) <= 0) continue;
        if (best && compare_versions(tag, best->version) <= 0) continue;
        best = Release{tag, r["html_url"].as_string()};
    }
    return best;
}

} // namespace net
