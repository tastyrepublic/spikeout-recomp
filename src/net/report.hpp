// A bug report sent from the game to GitHub (the owner's design, 2026-11-09): an issue on the project's repository,
// the report's text in it and the game's logs in comments under it, opened by the player, logged in to GitHub from the
// game (GitHub's device flow: a code the player types on github.com/login/device; the project's GitHub App, its only
// permission "Issues: read and write", installed on the project's repository). Not logged in: a zip, attached by hand.
// Each call blocks (the window runs it on a thread of its own).
#pragma once

#include <string>
#include <vector>

namespace net::report {

#ifdef SPIKEOUT_GITHUB_CLIENT_ID
constexpr const char* kClientId = SPIKEOUT_GITHUB_CLIENT_ID; // (the GitHub App's: public, no secret with the device flow)
#else
constexpr const char* kClientId = "";
#endif
#ifdef SPIKEOUT_REPORT_REPO
constexpr const char* kRepo = SPIKEOUT_REPORT_REPO; // owner/name
#else
constexpr const char* kRepo = "tastyrepublic/spikeout-recomp";
#endif

struct Report {
    std::string title, body;           // the issue (Markdown)
    std::vector<std::string> comments; // the logs, each a comment (GitHub takes 65,536 characters a comment)
};
constexpr std::size_t kCommentLimit = 60000; // (characters: under GitHub's 65,536, room for the fences)
constexpr std::size_t kComments = 4;         // (the logs, the run before, the crash report, the settings)

struct Sent {
    bool ok = false;
    int number = 0;    // the issue's
    std::string url;   // its page
    std::string error; // why not, in plain words
};

bool available(); // built with libcurl (the internet)
bool can_log_in(); // a GitHub App's client ID built in

// GitHub's device flow: the code the player types on the page given, then asked for every `interval` seconds until
// they have said yes (or no), or it ran out.
struct DeviceCode {
    std::string device_code, user_code, verification_uri, error;
    int interval = 5, expires_in = 900;
};
DeviceCode login_start();
struct Poll {
    enum class State { pending, slow_down, done, denied, expired, failed } state = State::failed;
    std::string token, error;
};
Poll login_poll(const std::string& device_code);
std::string login_name(const std::string& token, std::string* error = nullptr); // the token's GitHub name ("": not valid)

// As the logged-in player, straight to GitHub.
Sent send_as_user(const Report& report, const std::string& token);

} // namespace net::report
