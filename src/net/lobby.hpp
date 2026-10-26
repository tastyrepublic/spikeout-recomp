// The internet lobby's client: a WebSocket to the lobby Worker (lobby/src/
// index.js, which lists its messages), over TLS through libcurl, on a thread
// of its own. Built with SPIKEOUT_LOBBY (CMake finds libcurl); the address
// is the build's (SPIKEOUT_LOBBY_URL), not the player's to change.
#pragma once

#include "net/json.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace net {

#ifdef SPIKEOUT_LOBBY
constexpr const char* kLobbyUrl = SPIKEOUT_LOBBY_URL;
#else
constexpr const char* kLobbyUrl = "";
#endif

class LobbyClient {
public:
    LobbyClient();
    ~LobbyClient();
    static bool available(); // built with the lobby

    // Connects (in the background) and says hello. `url`: the build's
    // lobby unless a test gives its own.
    void connect(const std::string& build, const std::string& name, const std::string& url = kLobbyUrl);
    bool connected() const { return connected_; }
    bool failed() const { return failed_; }
    std::string error() const;

    void send(const Json& message);
    // The next message from the lobby, waiting up to `ms` for one.
    std::optional<Json> next(int ms = 0);
    // Waits for a message of type `t` (others are kept for next()), or an
    // error message (returned instead), up to `ms`.
    std::optional<Json> wait_for(const std::string& t, int ms);

private:
    void run(std::string url, std::string hello);

    std::thread thread_;
    std::atomic<bool> stop_{false}, connected_{false}, failed_{false};
    mutable std::mutex lock_;
    std::condition_variable changed_;
    std::deque<std::string> out_;
    std::deque<Json> in_;
    std::string error_;
};

} // namespace net
