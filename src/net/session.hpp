// A network play session, Mode B (docs/netplay.md): every machine runs every
// cabinet (board), only the players' inputs go between them, and each
// board reads each frame's inputs once every player's are in (delay-based:
// a player's inputs are set a few frames ahead, `input delay`, so they can
// arrive in time; 0 is as the arcade, a frame held until they do).
//
// Seats are the cabinets: seat 0 (shown as 1) is the host's and the link's
// master; others take the next free seat as they join. Games are found by
// LAN discovery (a broadcast on every network this machine is on, kDiscoveryPort),
// never by an address a player types.
//
// Everything goes over UDP. Inputs: each machine sends every other machine
// the frames it has (its own seat's, and the others' it holds, so a drop
// can be settled) that they haven't acknowledged, again and again until
// they have. Control messages (joining, the start, drops, state hashes) go
// on a small reliable channel to each peer.
//
// A player who drops (nothing heard for kDropAfter, or a goodbye): the
// authority (the lowest seat still connected) tells everyone to stop
// taking that seat's inputs from it, asks how far each has them, waits
// until it has them as far as anyone, and announces the frame from which
// the seat's inputs are none: its character stands idle, and every machine
// runs the same frames.
//
// Every cabinet of a game is powered on at the start, whether a player has
// it or not (the host may start alone). A cabinet no player has (never
// taken, or its player dropped) gets inputs of none from the authority,
// sent as anyone's. A player may join the game in progress: the host sends
// it the start and every board's state (a linked save state, compressed),
// the inputs since come as usual, and once its boards have caught up it
// asks for the cabinet: the authority names a frame from which the inputs
// are the player's. A machine whose boards differ from the host's (the
// state hashes) gets every board's state the same way and runs on from it.
//
// Thread-safe: the session runs its own network thread; the calls below
// can come from any thread, the callbacks come from the network thread.
#pragma once

#include "net/stun.hpp"
#include "net/udp.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace net {

constexpr uint16_t kDiscoveryPort = 47326;
constexpr uint8_t kProtocol = 1;
constexpr std::size_t kMaxSeats = 4;
constexpr std::chrono::milliseconds kDropAfter{3000};

using InputBytes = std::array<uint8_t, 4>; // bank 0, bank 1, controls, controls 2 (as tools::Recording::Inputs)

// A game being offered, as discovery finds it.
struct GameInfo {
    uint64_t session = 0;
    uint64_t build = 0; // must equal ours to join (the same game, ROMs and build)
    std::string name;   // the host's
    uint8_t seats = 0, taken = 0;
    bool started = false;
    Address host;       // its game port
    std::vector<Address> candidates; // (from the lobby) every address it may be reached at
};

// Listens for games on the networks this machine is on.
class Discovery {
public:
    bool open(); // false: the port can't be had (error())
    const std::string& error() const { return socket_.error(); }
    // The games heard from in the last few seconds (call it now and then).
    std::vector<GameInfo> poll();

private:
    UdpSocket socket_;
    struct Heard {
        GameInfo info;
        std::chrono::steady_clock::time_point at;
    };
    std::map<uint64_t, Heard> games_;
};

// What every machine starts its boards from: the same for all.
struct StartInfo {
    uint8_t seats = 0;  // boards
    uint8_t my_seat = 0;
    int64_t clock = 0;  // the boards' clocks at power-on (seconds, as Rtc72421::set_time)
    // By seat, each player's own cabinet as it was (Session::set_cabinet): its
    // settings EEPROM (128 bytes) and backup RAM (empty: blank, a new cabinet).
    std::vector<std::vector<uint8_t>> eeprom, backup;
    // A game joined in progress (or a machine put back in step): every
    // board as it was at the end of `state_frame` (a linked save state,
    // zstd-compressed), to run on from; the inputs from then on come as usual.
    std::optional<uint64_t> state_frame;
    std::vector<uint8_t> state;
    std::vector<std::string> names; // by seat
};

class Session {
public:
    struct Callbacks {
        std::function<void(const StartInfo&)> started;
        // Seat `seat`'s inputs set at the end of `frame` (each frame once, in order).
        std::function<void(uint8_t seat, uint64_t frame, const InputBytes&)> input;
        // Seat `seat` dropped: its inputs from `frame` on are none (its
        // character stands idle; the cabinet is free to be taken again).
        std::function<void(uint8_t seat, uint64_t frame)> idle;
        // Another machine's boards differ from ours at the end of `frame`.
        std::function<void(uint8_t seat, uint64_t frame)> desync;
        // The host: a machine needs every board's state (a player joining
        // the game in progress, or one out of step): call send_state soon.
        std::function<void()> state_wanted;
        // Ours were out of step: every board as the host had them (run on
        // from there; the inputs since come again through `input`).
        std::function<void(const StartInfo&)> resync;
        // Seat `seat` is a player's from `frame` on (a cabinet taken).
        std::function<void(uint8_t seat, uint64_t frame)> taken;
        std::function<void(const std::string&)> log; // what happened, for the frontend
    };

    explicit Session(Callbacks callbacks, uint64_t build);
    ~Session();

    // Offer a game of `seats` cabinets (2-4) as `name`; the host is seat 0.
    bool host(const std::string& name, uint8_t seats);
    // Join a game discovery (or the lobby) found, as `name`.
    bool join(const GameInfo& game, const std::string& name);
    // Our game port opened before joining, to find our addresses first
    // (candidates(): STUN, the relay) and give them to the lobby.
    bool listen();
    const std::string& error() const { return error_; }
    uint16_t port() const; // the game port (a host's, for GameInfo::host)
    uint64_t session_id() const;

    // This player's cabinet (its settings EEPROM and backup RAM), before
    // hosting or joining: every machine starts this player's board from it.
    void set_cabinet(std::vector<uint8_t> eeprom, std::vector<uint8_t> backup);

    // The host: the players so far (seat, name), and starting the boards.
    std::vector<std::string> players() const;
    // Every seat taken, each player's cabinet in.
    bool all_seats_taken() const;
    // Seats 1.. that have joined and sent their cabinet.
    std::size_t ready_players() const;
    // Every seat's cabinet powered on (seats, names, cabinets and my_seat
    // filled in here). A seat nobody has taken yet stands empty, its
    // inputs none, until a player joins the game in progress and takes it.
    void start(StartInfo info);

    // ---- A game in progress ----
    // The host, asked (Callbacks::state_wanted): every board's state at
    // the end of `frame` (compressed), for the machines waiting for it.
    void send_state(uint64_t frame, std::vector<uint8_t> state);
    // A player joined in progress, its boards caught up with the others':
    // its cabinet is its own from a frame the authority chooses (until
    // then, none; Callbacks::taken).
    void take_seat();
    // Seat `seat`'s inputs are in up to (not including) this frame.
    uint64_t inputs_known(uint8_t seat) const;
    // Every input from `frame` on, again through Callbacks::input (a
    // machine running on from a state, its table made afresh).
    void feed_inputs(uint64_t frame);
    bool running() const { return started_; }

    // This machine's player's inputs, set at the end of `frame` (in order;
    // frames skipped hold the last).
    void local_input(uint64_t frame, const InputBytes& in);
    // The boards' hash at the end of `frame` (compared between machines).
    void report_hash(uint64_t frame, uint64_t hash);
    // Leave (a goodbye to the others).
    void leave();

    // How it goes, for an overlay: round trip to each seat (ms, -1: none).
    std::array<int, kMaxSeats> round_trips() const;
    // Each seat of a game in progress, for an overlay.
    struct SeatView {
        enum class State : uint8_t {
            none,    // not in this game (beyond its seats)
            open,    // no player has taken it yet
            me,      // this machine's player
            joining, // a player joined in progress, catching up (the cabinet not theirs yet)
            playing, // a player's
            gone,    // its player dropped (or left: `left`); the character stands idle
        };
        State state = State::none;
        std::string name; // the player's (gone: the one who was)
        bool left = false; // gone after saying goodbye
        int round_trip = -1; // ms (-1: not measured yet)
    };
    std::array<SeatView, kMaxSeats> seats() const;
    bool hosting() const;

    // ---- Internet play (with the lobby, net::LobbyClient) ----
    // Our address as a STUN server sees it (our router's), found in the
    // background: another machine can try it.
    void find_public_address(const Address& stun_server);
    std::optional<Address> public_address() const;
    // A relay address on a TURN server (its short-lived credentials from the
    // lobby), for when routers won't let us through to each other.
    void use_relay(const Address& turn_server, const std::string& username, const std::string& password);
    std::optional<Address> relay_address() const;
    // Every address another machine may reach us at (this machine's own on
    // its networks, our router's, the relay's), with our game port.
    std::vector<Address> candidates() const;
    // The host: a player joining from these addresses (the lobby says):
    // datagrams to them now open our router to theirs.
    void expect(const std::vector<Address>& addresses);
    // Seats reached only through a relay (ours or theirs), for an overlay.
    std::array<bool, kMaxSeats> relayed() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string error_;
    std::atomic<bool> started_{false};
};

} // namespace net
