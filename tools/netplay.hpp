// Network play for the frontends (gamerun, the window): a net::Session
// joined to the boards' input table. Every machine runs every board; the
// local player's inputs go into the table and out to the others, the
// others' come in from the session; a player who drops stands idle.
#pragma once

#include "arcade_settings.hpp"
#include "board_backup.hpp"
#include "input_table.hpp"
#include "net/lobby.hpp"
#include "net/session.hpp"
#include "runtime/commboard.hpp"
#include "runtime/rom.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tools {

// What two machines must share to play together: the ROMs (sampled) and
// the recompiled game (its functions), and the protocol.
// The network board too (`link`, null for the simulated one): its options
// change what the game does, so two machines must have the same.
inline uint64_t build_id(const m3::RomSet& roms, std::size_t functions, const m3::CommBoardOptions* link) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    auto mixd = [&](double v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        mix(bits);
    };
    mix(net::kProtocol), mix(functions);
    mix(link != nullptr);
    if (link) {
        mix(link->comm_ram_xor), mix(link->host_sees_other_bank), mix(link->cpu_hz), mix(link->irq5_hz);
        mix(link->irq5_per_frame), mixd(link->irq5_phase), mix(link->irq5_on_e0), mix(link->slave_irq5_per_frame);
        mixd(link->link_cycles_per_byte), mix(link->send_waits_for_receive), mix(link->ring_forwarding);
        mix(link->double_buffer), mixd(link->main_frame_hz), mix(link->main_hz), mix(link->link_status);
    }
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
// docs/fidelity.md). A new cabinet (none yet) starts as the host's. A host
// with none (gamerun's, a blank chip) doesn't set the country; its own
// cabinet then starts as the first other player's: a blank chip on the
// master with a slave's set up never linked (the network check cancelled;
// docs/netplay.md, "gamerun and the window in one game"). Every cabinet blank:
// as they are.
inline std::vector<uint8_t> seat_eeprom(const net::StartInfo& start, std::size_t seat) {
    auto valid = [](const std::vector<uint8_t>& bytes) {
        auto w = eeprom_words(bytes);
        return bytes.size() == 128 && ArcadeSettings{w}.valid();
    };
    const std::vector<uint8_t>& host = start.eeprom.empty() ? std::vector<uint8_t>() : start.eeprom[0];
    std::vector<uint8_t> mine = seat < start.eeprom.size() ? start.eeprom[seat] : std::vector<uint8_t>();
    auto host_words = eeprom_words(host);
    const ArcadeSettings host_settings{host_words};
    if (!valid(host)) {
        if (valid(mine)) return mine;
        for (const std::vector<uint8_t>& other : start.eeprom)
            if (valid(other)) return other;
        return mine;
    }
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
            keeper_.input(seat, frame, {in[0], in[1], in[2], in[3]});
        };
        cb.idle = [this](uint8_t seat, uint64_t frame) { // (its inputs none from there come as anyone's)
            // (and are none here at once: they come as far as the authority's own, and one board a PC reads the other
            // cabinets' a frame or so ahead of its own: with no input delay, a wait for frames made only as ours are)
            table_.hold_from(seat, frame, {});
            keeper_.hold_from(seat, frame);
            {
                std::lock_guard guard(lock_);
                gone_.push_back(seat);
                if (seat < absent_.size()) absent_[seat] = true;
            }
            changed_.notify_all(); // (a wait for its board ends)
        };
        cb.taken = [this](uint8_t seat, uint64_t frame) { // (a player's again: its board asked of its machine as any)
            table_.hold_until(seat, frame);
            keeper_.hold_until(seat, frame);
            std::lock_guard guard(lock_);
            taken_.push_back(seat);
            if (seat < absent_.size()) absent_[seat] = false;
        };
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
        cb.board_wanted = [this](uint8_t seat, uint64_t frame) {
            std::lock_guard guard(lock_);
            boards_wanted_.push_back({seat, frame});
        };
        cb.parting = [this](uint8_t seat, uint64_t frame) {
            std::lock_guard guard(lock_);
            partings_.push_back({seat, frame});
        };
        cb.board = [this](uint8_t seat, uint64_t frame, std::vector<uint8_t> state) {
            // ("my backup of that frame" (one_board::pack_remote with no state: its 16 bytes of counts): that backup came
            // just before it; the copy kept then, as another backup (for another machine's shadow) may follow at once)
            std::optional<backup::Copy> copy;
            if (state.size() == 16)
                if (backup::Copy c = keeper_.copy(seat); c.frame == frame) copy = std::move(c);
            {
                std::lock_guard guard(lock_);
                boards_[{seat, frame}] = std::move(state);
                if (copy) {
                    for (auto it = board_copies_.begin(); it != board_copies_.end();) // (any never taken, long gone by)
                        it = it->first.first == seat && it->first.second + 600 < frame ? board_copies_.erase(it) : std::next(it);
                    board_copies_[{seat, frame}] = std::move(*copy);
                }
            }
            changed_.notify_all();
        };
        cb.backup = [this](uint8_t seat, uint64_t, std::vector<uint8_t> backup) {
            if (keeper_.backup(seat, backup)) return;
            std::lock_guard guard(lock_); // (changes from one not had: its whole asked for next, not from in here)
            backup_missing_.push_back(seat);
        };
        cb.backup_wanted = [this](uint8_t seat) {
            std::lock_guard guard(lock_);
            whole_wanted_.push_back(seat);
        };
        session_ = std::make_unique<net::Session>(std::move(cb), build);
    }

    // One board a PC: every other board's last backup, the packets to it and its player's inputs since
    // (tools::backup); and the seats wanting this machine's own board's next backup whole, since the last asked.
    backup::Keeper& keeper() { return keeper_; }
    // This machine's own board's backup out (backup::Maker's, not packed yet) to each of `seats`: packed and sent on
    // a thread of its own (the whole board takes 100 ms or so), every backup in the order given.
    void send_backup(uint64_t frame, std::vector<uint8_t> raw, std::vector<uint8_t> seats) {
        {
            std::lock_guard guard(backup_lock_);
            backups_out_.push_back({frame, std::move(raw), std::move(seats), {}});
            if (!backup_sender_.joinable())
                backup_sender_ = std::jthread([this](std::stop_token stop) {
                    for (;;) {
                        BackupOut b;
                        {
                            std::unique_lock guard2(backup_lock_);
                            backup_queued_.wait(guard2, stop, [&] { return !backups_out_.empty(); });
                            if (stop.stop_requested()) return;
                            b = std::move(backups_out_.front());
                            backups_out_.pop_front();
                        }
                        if (b.raw.empty()) { // (send_board_after_backups)
                            for (const uint8_t seat : b.seats) session_->send_board(seat, b.frame, b.board);
                            continue;
                        }
                        const std::vector<uint8_t> packed = net::compress(b.raw);
                        for (const uint8_t seat : b.seats) session_->send_backup(seat, b.frame, packed);
                    }
                });
        }
        backup_queued_.notify_all();
    }
    // One board a PC: a board's state for other machines' shadows, sent after the backups queued before it (from
    // their thread: it may say "my backup of this frame", which must come first).
    void send_board_after_backups(uint64_t frame, std::vector<uint8_t> board, std::vector<uint8_t> seats) {
        {
            std::lock_guard guard(backup_lock_);
            backups_out_.push_back({frame, {}, std::move(seats), std::move(board)});
        }
        backup_queued_.notify_all();
    }
    std::vector<uint8_t> take_whole_wanted() {
        std::lock_guard guard(lock_);
        return std::exchange(whole_wanted_, {});
    }

    // One board a PC (tools/one_board.hpp): the seats that want this machine's own board's state at the end of
    // `frame` (taken: answer each with session().send_board); and a seat's board's state at the end of `frame`, as
    // asked with session().want_board, waited for (nothing: the time is up, or the session closed).
    // (each frame's: the backups to be asked for whole are asked for here, not from the session's own thread)
    std::vector<uint8_t> take_board_requests(uint64_t frame) {
        std::vector<uint8_t> missing;
        {
            std::lock_guard guard(lock_);
            missing = std::exchange(backup_missing_, {});
        }
        for (const uint8_t seat : missing) session_->want_backup(seat);
        std::lock_guard guard(lock_);
        std::vector<uint8_t> seats;
        for (auto it = boards_wanted_.begin(); it != boards_wanted_.end();)
            if (it->second <= frame) {
                if (it->second == frame) seats.push_back(it->first); // (one asked for a frame gone by: too late, dropped)
                it = boards_wanted_.erase(it);
            } else {
                ++it;
            }
        for (auto it = boards_again_.begin(); it != boards_again_.end();) // (kept while that frame may be run again)
            if (it->second == frame || it->second + kAgainFrames < frame) {
                if (it->second == frame) seats.push_back(it->first);
                it = boards_again_.erase(it);
            } else {
                ++it;
            }
        return seats;
    }
    // (rollback: one taken for a frame run on a wrong guess: asked again, for when that frame is run again: kept that
    // long, not dropped as gone by at the next frame)
    void board_request_again(uint8_t seat, uint64_t frame) {
        std::lock_guard guard(lock_);
        boards_again_.push_back({seat, frame});
    }
    // The seats leaving, each after a frame, their boards' states at its end to come (Session::announce_parting); and
    // those whose player has gone (left or dropped), since the last asked.
    std::vector<std::pair<uint8_t, uint64_t>> take_partings() {
        std::lock_guard guard(lock_);
        return std::exchange(partings_, {});
    }
    std::vector<uint8_t> take_gone() {
        std::lock_guard guard(lock_);
        return std::exchange(gone_, {});
    }
    // And those taken by a player (a game joined in progress), since the last asked.
    std::vector<uint8_t> take_taken() {
        std::lock_guard guard(lock_);
        return std::exchange(taken_, {});
    }
    // One board a PC: a board's state for a shadow said to be its backup of `frame` (send_board_after_backups): the
    // copy kept here of that board, once that backup is in (sent before it; a few seconds at most).
    std::optional<backup::Copy> backup_at(uint8_t seat, uint64_t frame) {
        {
            std::lock_guard guard(lock_);
            if (const auto it = board_copies_.find({seat, frame}); it != board_copies_.end()) {
                backup::Copy copy = std::move(it->second);
                board_copies_.erase(it);
                return copy;
            }
        }
        for (int k = 0; k < 5000; ++k) {
            const auto f = keeper_.frame(seat);
            if (f && *f >= frame) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        backup::Copy copy = keeper_.copy(seat);
        if (copy.frame != frame) return std::nullopt;
        return copy;
    }
    // Whether that state has come (waited for up to `timeout`), left for wait_board.
    bool has_board(uint8_t seat, uint64_t frame, std::chrono::milliseconds timeout) {
        std::unique_lock guard(lock_);
        return changed_.wait_for(guard, timeout, [&] { return boards_.count({seat, frame}) != 0; });
    }
    std::optional<std::vector<uint8_t>> wait_board(uint8_t seat, uint64_t frame, std::chrono::milliseconds timeout) {
        std::unique_lock guard(lock_);
        changed_.wait_for(guard, timeout, [&] { return boards_.count({seat, frame}) != 0 || (seat < absent_.size() && absent_[seat]); });
        const auto it = boards_.find({seat, frame});
        if (it == boards_.end()) return std::nullopt;
        std::vector<uint8_t> state = std::move(it->second);
        boards_.erase(it);
        return state;
    }

    net::Session& session() { return *session_; }

    // Rollback's time sync: how far this machine (seat `me`) runs ahead of the others, in frames, as their inputs
    // show it (each frame's inputs go out as that machine sets them, the input delay on: the same for all is
    // assumed), half the round trip allowed for their way here. `ahead`: of the furthest behind, `behind`: of the
    // furthest ahead (negative: behind it). Machines not heard from for a while (gone silent: a crash, a stall) left
    // out: the game runs on with guesses for them rather than waiting. None to go by: both 0.
    struct Lead {
        int64_t ahead = 0, behind = 0;
    };
    Lead lead(std::size_t me) const {
        const auto t = session_->timing();
        const int64_t mine = static_cast<int64_t>(t[me].next);
        Lead l;
        bool any = false;
        for (std::size_t s = 0; s < t.size(); ++s) {
            if (s == me || t[s].quiet_ms < 0 || t[s].quiet_ms > 300 || t[s].next == 0) continue;
            const int64_t transit = t[s].rtt > 0 ? static_cast<int64_t>(t[s].rtt / 2 / 17.4 + 0.5) : 0;
            const int64_t d = mine - (static_cast<int64_t>(t[s].next) + transit);
            l.ahead = any ? std::max(l.ahead, d) : d;
            l.behind = any ? std::min(l.behind, d) : d;
            any = true;
        }
        return l;
    }

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
    std::vector<std::pair<uint8_t, uint64_t>> boards_wanted_, partings_;     // (seat, frame)
    std::vector<std::pair<uint8_t, uint64_t>> boards_again_;                 // (seat, frame: rollback's, board_request_again)
    static constexpr uint64_t kAgainFrames = 256;                             // (longer than rollback's window)
    std::vector<uint8_t> gone_, taken_, whole_wanted_, backup_missing_;
    std::array<bool, net::kMaxSeats> absent_{}; // (by seat: its player gone, nobody's since)
    std::map<std::pair<uint8_t, uint64_t>, std::vector<uint8_t>> boards_; // (seat, frame): a board's state, compressed
    std::map<std::pair<uint8_t, uint64_t>, backup::Copy> board_copies_;    // (and "my backup": the copy as it came)
    backup::Keeper keeper_{net::kMaxSeats};
    std::atomic<uint64_t> desyncs_{0};
    std::atomic<bool> state_wanted_{false}, resync_pending_{false};
    std::unique_ptr<net::Session> session_; // (its thread, calling back into the above, stops before them)
    // (backups being packed and sent: stopped first, before the session they send through)
    struct BackupOut {
        uint64_t frame = 0;
        std::vector<uint8_t> raw, seats;
        std::vector<uint8_t> board; // (send_board_after_backups: a board's state, sent as is; raw empty)
    };
    std::mutex backup_lock_;
    std::condition_variable_any backup_queued_;
    std::deque<BackupOut> backups_out_;
    std::jthread backup_sender_;
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
// by the code returned). `takeover`: the code of the room this machine hosts
// now (its host left the game in progress): the lobby lists it under that
// code again, as it was, if it still remembers it (else a new code).
inline std::optional<std::string> lobby_offer(net::LobbyClient& lobby, net::Session& session, const std::string& name,
                                              uint8_t seats, bool listed, const std::string& takeover = {}) {
    net::Json m = net::Json::object();
    m.set("t", "host"), m.set("name", name), m.set("seats", static_cast<int>(seats)), m.set("public", listed);
    m.set("session", hex64(session.session_id())), m.set("port", static_cast<int>(session.port()));
    m.set("candidates", address_list(session.candidates()));
    if (!takeover.empty()) m.set("takeover", takeover);
    lobby.send(m);
    const auto hosted = lobby.wait_for("hosted", 10000);
    if (!hosted || (*hosted)["t"].as_string() != "hosted") return std::nullopt;
    return (*hosted)["code"].as_string();
}

// The host's part while players join: each joiner's addresses opened to, its country (the lobby's) kept.
inline void lobby_pump(net::LobbyClient& lobby, net::Session& session) {
    while (auto m = lobby.next())
        if ((*m)["t"].as_string() == "joiner") session.expect(parse_addresses((*m)["candidates"]), (*m)["country"].as_string());
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
