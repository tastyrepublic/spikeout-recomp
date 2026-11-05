// The lobby client when built without libcurl: always unavailable (LAN play only).
#include "net/lobby.hpp"

namespace net {

bool LobbyClient::available() { return false; }
LobbyClient::LobbyClient() = default;
LobbyClient::~LobbyClient() = default;
std::string LobbyClient::error() const { return "built without the internet lobby"; }
void LobbyClient::connect(const std::string&, const std::string&, const std::string&) { failed_ = true; }
void LobbyClient::send(const Json&) {}
std::optional<Json> LobbyClient::next(int) { return std::nullopt; }
std::optional<Json> LobbyClient::wait_for(const std::string&, int) { return std::nullopt; }
void LobbyClient::run(std::string, std::string) {}

} // namespace net

// The update check too (it needs libcurl): never done.
#include "net/update.hpp"

namespace net {

bool UpdateCheck::available() { return false; }
UpdateCheck::~UpdateCheck() = default;
void UpdateCheck::start(std::string, std::string) {}
std::optional<Release> UpdateCheck::newer() const { return std::nullopt; }
std::string UpdateCheck::error() const { return "built without it"; }
void UpdateCheck::run(std::shared_ptr<State>, std::string, std::string) {}

} // namespace net
