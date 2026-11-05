// The update check: at start the window asks GitHub's release list
// (SPIKEOUT_RELEASES_URL) whether a newer version than this build's
// (SPIKEOUT_VERSION) is out, on a thread of its own, and says so in its
// menu. It only tells: the player downloads it. Builds whose version ends
// in "-dev" (any build that isn't a release) don't ask.
#pragma once

#include "net/json.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace net {

#ifdef SPIKEOUT_VERSION
constexpr const char* kVersion = SPIKEOUT_VERSION;
#else
constexpr const char* kVersion = "0.0.0-dev";
#endif

// Versions as the releases' tags have them ("v" optional): numbers, then a
// label ("0.1.0-preview2"); a label sorts before none (0.1.0-preview2 <
// 0.1.0), labels by their letters then their number. <0, 0, >0 as strcmp.
int compare_versions(std::string_view a, std::string_view b);
bool prerelease(std::string_view version); // a label ("-preview1")
bool dev_build(std::string_view version);  // "-dev": not a release

struct Release {
    std::string version; // without the "v"
    std::string page;    // its page on GitHub (the downloads)
};

// The newest release in GitHub's list (an array of releases) newer than
// `current`; drafts never, test builds (prereleases) only for a test build.
std::optional<Release> newer_release(const Json& releases, std::string_view current);

class UpdateCheck {
public:
    ~UpdateCheck(); // (doesn't wait for an answer: the question goes on alone, and is dropped)
    static bool available(); // built with libcurl
    // Asks in the background (`url`: the build's release list unless a test
    // gives its own; SPIKEOUT_UPDATE_URL too).
    void start(std::string current, std::string url = {});
    bool done() const { return state_->done; }
    std::optional<Release> newer() const; // once done: a newer release, if there is one
    std::string error() const;            // once done: why it couldn't tell ("" if it could)

private:
    struct State {
        std::atomic<bool> done{false};
        std::mutex lock;
        std::optional<Release> newer;
        std::string error;
    };
    static void run(std::shared_ptr<State> state, std::string current, std::string url);
    std::shared_ptr<State> state_ = std::make_shared<State>();
    bool started_ = false;
};

} // namespace net
