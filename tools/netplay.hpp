// Network play for the frontends (gamerun, the window): a net::Session
// joined to the boards' input table. Every machine runs every board; the
// local player's inputs go into the table and out to the others, the
// others' come in from the session; a player who drops stands idle.
#pragma once

#include "arcade_settings.hpp"
#include "input_table.hpp"
#include "net/lobby.hpp"
#include "net/session.hpp"
#include "runtime/rom.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tools {

// What two machines must share to play together: the ROMs (sampled) and
// the recompiled game (its functions), and the protocol.
inline uint64_t build_id(const m3::RomSet& roms, std::size_t functions) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(net::kProtocol), mix(functions);
    for (std::size_t r = 0; r < static_cast<std::size_t>(m3::RomRegionId::Count); ++r) {
        const auto& region = roms.region(static_cast<m3::RomRegionId>(r));
        mix(region.size());
        for (std::size_t i = 0; i < region.size(); i += 61) mix(region[i]);
    }
    return h;
}

// A settings EEPROM's 128 bytes (high byte first) as words, and back.
inline std::array<uint16_t, 64> eeprom_words(const std::vector<uint8_t>& bytes) {
    std::array<uint16_t, 64> w{};
    for (std::size_t i = 0; i < 64 && 2 * i + 1 < bytes.size(); ++i) w[i] = static_cast<uint16_t>(bytes[2 * i] << 8 | bytes[2 * i + 1]);
    return w;
}
inline std::vector<uint8_t> eeprom_bytes(const std::array<uint16_t, 64>& w) {
    std::vector<uint8_t> out;
    for (uint16_t v : w) out.push_back(static_cast<uint8_t>(v >> 8)), out.push_back(static_cast<uint8_t>(v));
    return out;
}

// Seat `seat`'s settings EEPROM as every machine starts its board: the
// player's own cabinet's, with the host's country (linked cabinets must
// share one, or the slaves show COUNTRY SETTING DIFFERENT; difficulty
// follows the master, coins and free play stay each cabinet's own:
// docs/fidelity.md). A new cabinet (none yet) starts as the host's.
inline std::vector<uint8_t> seat_eeprom(const net::StartInfo& start, std::size_t seat) {
    const std::vector<uint8_t>& host = start.eeprom.empty() ? std::vector<uint8_t>() : start.eeprom[0];
    std::vector<uint8_t> mine = seat < start.eeprom.size() ? start.eeprom[seat] : std::vector<uint8_t>();
    auto host_words = eeprom_words(host);
    const ArcadeSettings host_settings{host_words};
    if (host.size() != 128 || !host_settings.valid()) return mine;
    auto words = eeprom_words(mine);
    ArcadeSettings settings{words};
    if (mine.size() != 128 || !settings.valid()) return host;
    if (settings.country() != host_settings.country()) settings.set_country(host_settings.country());
    return eeprom_bytes(words);
}

// A settings EEPROM with its country put back (a cabinet's own, kept after
// a game that played it as the host's).
inline std::vector<uint8_t> with_country(const std::vector<uint8_t>& eeprom, int country) {
    auto words = eeprom_words(eeprom);
    ArcadeSettings settings{words};
    if (eeprom.size() != 128 || !settings.valid() || settings.country() == country) return eeprom;
    settings.set_country(country);
    return eeprom_bytes(words);
}

class NetPlay {
public:
    NetPlay(InputTable& table, uint64_t build, std::function<void(const std::string&)> log)
        : table_(table), log_(std::move(log)) {
        net::Session::Callbacks cb;
        cb.started = [this](const net::StartInfo& info) {
            std::lock_guard guard(lock_);
            start_ = info;
            changed_.notify_all();
        };
        cb.input = [this](uint8_t seat, uint64_t frame, const net::InputBytes& in) {
            table_.set(seat, frame, {in[0], in[1], in[2], in[3]});
        };
        cb.idle = [](uint8_t, uint64_t) {}; // (its inputs none from there come as anyone's)
        cb.state_wanted = [this] { state_wanted_ = true; };
        cb.resync = [this](const net::StartInfo& info) {
            std::lock_guard guard(lock_);
            resync_ = info;
            resync_pending_ = true;
        };
        cb.desync = [this](uint8_t seat, uint64_t key) {
            ++desyncs_;
            if (log_) log_("board " + std::to_string(key % net::kMaxSeats) + " differs from seat " + std::to_string(seat + 1) +
                           "'s at frame " + std::to_string(key / net::kMaxSeats));
        };
        cb.log = [this](const std::string& s) {
            if (log_) log_(s);
        };
        session_ = std::make_unique<net::Session>(std::move(cb), build);
    }

    net::Session& session() { return *session_; }

    // Until the game starts (the host's start, or ours), or the time is up.
    std::optional<net::StartInfo> wait_start(std::chrono::milliseconds timeout) {
        std::unique_lock guard(lock_);
        changed_.wait_for(guard, timeout, [&] { return start_.has_value(); });
        return start_;
    }

    // The local player's inputs set at the end of `frame`: out to the others,
    // and into the table as the session takes them (a cabinet taken in a
    // game in progress: only from the frame it is ours).
    void local(std::size_t /*seat*/, uint64_t frame, const InputTable::Inputs& in) {
        session_->local_input(frame, {in.bank0, in.bank1, in.controls, in.controls2});
    }
    // The host: a machine waits for every board's state (send it with
    // session().send_state, compressed); true once a request.
    bool take_state_request() { return state_wanted_.exchange(false); }
    // Ours out of step: every board as the host has them, to run on from.
    std::optional<net::StartInfo> take_resync() {
        std::lock_guard guard(lock_);
        auto r = std::move(resync_);
        resync_.reset();
        resync_pending_ = false;
        return r;
    }
    // One has come (the boards stop, to run on from it).
    bool resync_pending() const { return resync_pending_; }
    // A board's hash at the end of `frame` (compared with the others').
    void hash(uint64_t frame, std::size_t board, uint64_t hash) { session_->report_hash(frame * net::kMaxSeats + board, hash); }
    uint64_t desyncs() const { return desyncs_; }

private:
    InputTable& table_;
    std::function<void(const std::string&)> log_;
    std::mutex lock_;
    std::condition_variable changed_;
    std::optional<net::StartInfo> start_, resync_;
    std::atomic<uint64_t> desyncs_{0};
    std::atomic<bool> state_wanted_{false}, resync_pending_{false};
    std::unique_ptr<net::Session> session_; // (last: its thread, calling back into the above, stops first)
};

// ---- Internet play: the lobby (net::LobbyClient) ----

inline std::string hex64(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

inline net::Json address_list(const std::vector<net::Address>& list) {
    net::Json out = net::Json::array();
    for (const net::Address& a : list) out.push(a.str());
    return out;
}

inline std::vector<net::Address> parse_addresses(const net::Json& list) {
    std::vector<net::Address> out;
    for (const net::Json& j : list.items())
        if (auto a = net::parse_address(j.as_string())) out.push_back(*a);
    return out;
}

// The lobby connected and greeted (its welcome: whether it offers a relay).
inline std::optional<net::Json> lobby_hello(net::LobbyClient& lobby, uint64_t build, const std::string& name,
                                            const std::string& url) {
    lobby.connect(hex64(build), name, url);
    auto welcome = lobby.wait_for("welcome", 10000);
    if (!welcome || (*welcome)["t"].as_string() != "welcome") return std::nullopt;
    return welcome;
}

// The session's addresses on the internet before it is offered or joins:
// our router's (Cloudflare's STUN), and a relay address (Cloudflare's TURN,
// credentials from the lobby) if the lobby has one. Up to a few seconds.
inline void find_addresses(net::LobbyClient& lobby, net::Session& session, bool relay,
                           const std::function<void(const std::string&)>& log) {
    if (const auto stun = net::resolve("stun.cloudflare.com")) session.find_public_address({*stun, 3478});
    if (relay) {
        lobby.send([] {
            net::Json m = net::Json::object();
            m.set("t", "turn");
            return m;
        }());
        if (const auto turn = lobby.wait_for("turn", 5000); turn && (*turn)["t"].as_string() == "turn") {
            for (const net::Json& server : (*turn)["iceServers"].items()) {
                const std::string& user = server["username"].as_string();
                for (const net::Json& u : server["urls"].items()) {
                    const std::string& url = u.as_string(); // turn:HOST:PORT?transport=udp
                    if (user.empty() || url.rfind("turn:", 0) != 0 || url.find("transport=udp") == std::string::npos) continue;
                    const std::size_t colon = url.find(':', 5), q = url.find('?');
                    const auto ip = net::resolve(url.substr(5, colon - 5));
                    const unsigned long port = std::strtoul(url.substr(colon + 1, q - colon - 1).c_str(), nullptr, 10);
                    if (ip && port) {
                        session.use_relay({*ip, static_cast<uint16_t>(port)}, user, server["credential"].as_string());
                        break;
                    }
                }
                if (session.relay_address() || !user.empty()) break;
            }
        } else if (log) {
            log("no relay from the lobby");
        }
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until &&
           (!session.public_address() || (relay && !session.relay_address())))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (log) log(std::string("internet: ") + (session.public_address() ? "our address " + session.public_address()->str() : "no public address") +
                 (session.relay_address() ? ", relay " + session.relay_address()->str() : ""));
}

// Offers the session's game on the lobby (public: in its list; either way
// by the code returned).
inline std::optional<std::string> lobby_offer(net::LobbyClient& lobby, net::Session& session, const std::string& name,
                                              uint8_t seats, bool listed) {
    net::Json m = net::Json::object();
    m.set("t", "host"), m.set("name", name), m.set("seats", static_cast<int>(seats)), m.set("public", listed);
    m.set("session", hex64(session.session_id())), m.set("port", static_cast<int>(session.port()));
    m.set("candidates", address_list(session.candidates()));
    lobby.send(m);
    const auto hosted = lobby.wait_for("hosted", 10000);
    if (!hosted || (*hosted)["t"].as_string() != "hosted") return std::nullopt;
    return (*hosted)["code"].as_string();
}

// The host's part while players join: each joiner's addresses opened to.
inline void lobby_pump(net::LobbyClient& lobby, net::Session& session) {
    while (auto m = lobby.next())
        if ((*m)["t"].as_string() == "joiner") session.expect(parse_addresses((*m)["candidates"]));
}

// Joins the game with this code (the session listening, its addresses found).
inline bool lobby_join(net::LobbyClient& lobby, net::Session& session, uint64_t build, const std::string& code,
                       const std::string& name, std::string& error) {
    net::Json m = net::Json::object();
    m.set("t", "join"), m.set("code", code), m.set("port", static_cast<int>(session.port()));
    m.set("candidates", address_list(session.candidates()));
    lobby.send(m);
    const auto joining = lobby.wait_for("joining", 10000);
    if (!joining || (*joining)["t"].as_string() != "joining") {
        error = joining ? (*joining)["why"].as_string() : "the lobby didn't answer";
        return false;
    }
    net::GameInfo game;
    game.session = std::strtoull((*joining)["session"].as_string().c_str(), nullptr, 16);
    game.build = build;
    game.name = (*joining)["name"].as_string();
    game.seats = static_cast<uint8_t>((*joining)["seats"].as_number(2));
    game.candidates = parse_addresses((*joining)["candidates"]);
    if (!session.join(game, name)) {
        error = session.error();
        return false;
    }
    return true;
}

} // namespace tools
