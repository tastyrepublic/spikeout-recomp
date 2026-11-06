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
constexpr uint8_t kProtocol = 3; // (2: spectators; 3: selective acknowledgements, the state's size, catch-up in the status)
constexpr std::size_t kMaxSeats = 4;
constexpr std::chrono::milliseconds kDropAfter{3000};
// A player in a game going on, heard nothing from for this long, is let go (its cabinet idle from the furthest of
// its inputs any machine has). 1 s was tried (2026-11-05: the others held less) and dropped a machine that was only
// stalled (four games run flat out on one PC, each making a board whole from its backup at once): 3 s, as before.
constexpr std::chrono::milliseconds kDropInGame{3000};
// A player's machine heard nothing from for this long is quiet (Callbacks::quiet): its datagrams come every frame or
// so (a keep-alive at least every 100 ms), so a few of them missed in a row.
constexpr std::chrono::milliseconds kQuietAfter{150};
constexpr std::size_t kMaxWatchers = 4; // spectators a game (the owner's choice)
constexpr uint64_t kWatchLag = 120;     // frames a spectator runs behind the furthest inputs (about 2 s)

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
    uint8_t watchers = 0; // spectators (up to kMaxWatchers)
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
        // A player's machine not heard from for kQuietAfter (true), or heard again (false), in a game going on: with
        // rollback its inputs are guessed further meanwhile (standing still, as the drop will have them), so the
        // game runs on rather than waiting for the drop. From the session's own thread.
        std::function<void(uint8_t seat, bool quiet)> quiet;
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
        // One board a PC: seat `seat` wants our own board's state at the end of `frame` (send_board), for its shadow
        // of it; and a seat's board's state, as we asked (want_board).
        std::function<void(uint8_t seat, uint64_t frame)> board_wanted;
        std::function<void(uint8_t seat, uint64_t frame, std::vector<uint8_t> state)> board;
        // Seat `seat` is leaving after `frame`, its board's state at its end sent to all (Callbacks::board).
        std::function<void(uint8_t seat, uint64_t frame)> parting;
        // One board a PC: seat `seat`'s backup of its own board at the end of `frame` (send_backup), and seat `seat`
        // wanting ours whole next (want_backup).
        std::function<void(uint8_t seat, uint64_t frame, std::vector<uint8_t> backup)> backup;
        std::function<void(uint8_t seat)> backup_wanted;
        std::function<void(const std::string&)> log; // what happened, for the frontend
    };

    explicit Session(Callbacks callbacks, uint64_t build);
    ~Session();

    // Offer a game of `seats` cabinets (2-4) as `name`; the host is seat 0.
    bool host(const std::string& name, uint8_t seats);
    // Join a game discovery (or the lobby) found, as `name`.
    bool join(const GameInfo& game, const std::string& name);
    // Watch it instead (a spectator: docs/netplay.md, "Spectators"): every board's inputs from the room's host, each
    // frame's given out kWatchLag frames after they're all in; no seat, nothing sent the players wait for. The start
    // (and, the game in progress, every board's state) come as a late joiner's; StartInfo::my_seat is 0xFF.
    // room_closed() once the host is gone and no new one greets it.
    bool watch(const GameInfo& game, const std::string& name);
    bool spectator() const;
    // A spectator running one board (docs/netplay.md, "Spectators on one board"): every player's own backups wanted
    // (Callbacks::backup, from each player's machine, at a speed that never holds the players back); false: every board.
    void set_watch_one_board(bool one);
    // A player: the spectators on one board its backups go to too, as send_backup's `seat` (kWatcherBase + their
    // number: 0x10-0x13), and Callbacks::backup_wanted's for one's whole.
    std::vector<uint8_t> backup_watchers() const;
    // Spectators of this game (the host's count: as it last told us).
    std::size_t watching() const;
    // Our game port opened before joining, to find our addresses first
    // (candidates(): STUN, the relay) and give them to the lobby.
    bool listen();
    const std::string& error() const { return error_; }
    uint16_t port() const; // the game port (a host's, for GameInfo::host)
    uint64_t session_id() const;

    // This player's cabinet (its settings EEPROM and backup RAM), before
    // hosting or joining: every machine starts this player's board from it.
    void set_cabinet(std::vector<uint8_t> eeprom, std::vector<uint8_t> backup);
    // This player's country (the lobby's welcome: Cloudflare's two letters; none on a LAN): its own seat's, and a
    // host's in the roster it sends (SeatView::country; a joiner's comes from the lobby through the host, expect).
    void set_country(const std::string& country);
    // The game's name (the host's: its player's name, or as the lobby listed it).
    std::string game_name() const;

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
    // One board a PC: ask seat `seat` for its own board's state at the end of `frame` (Callbacks::board); and send ours,
    // asked (Callbacks::board_wanted; compressed).
    void want_board(uint8_t seat, uint64_t frame);
    void send_board(uint8_t seat, uint64_t frame, std::vector<uint8_t> state);
    // Leaving a game in progress: this machine's board's state at the end of `frame` will go to every other (send_board),
    // then the goodbye (leave), so that each runs its cabinet on whole from it (Callbacks::parting).
    void announce_parting(uint64_t frame);
    // Something sent on the reliable channel not yet acknowledged by every machine still there (a parting state: wait
    // for it before leaving).
    bool sending() const;
    // One board a PC: our own board's backup at the end of `frame` to seat `seat` (whole, or the pages changed since
    // the last sent it: tools::backup), so that the others can run it on from there if this machine is lost; and
    // asking seat `seat` for its whole next (ours of it is missing what its changes are from).
    void send_backup(uint8_t seat, uint64_t frame, std::vector<uint8_t> backup);
    void want_backup(uint8_t seat);
    // Tests: our own inputs sent to the others `frames` frames' time (1/60 s each) after they're set, as a slow
    // network's (the others, with rollback, run on on guesses meanwhile).
    void hold_inputs(uint64_t frames);
    // A player joined in progress, its boards caught up with the others':
    // its cabinet is its own from a frame the authority chooses (until
    // then, none; Callbacks::taken).
    void take_seat();
    // Seat `seat`'s inputs are in up to (not including) this frame.
    uint64_t inputs_known(uint8_t seat) const;
    // Whether this machine makes seat `seat`'s inputs of `frame` itself (its own; an empty cabinet's, as the
    // authority): never guessed (rollback's lazy snapshots don't wait for them).
    bool fills_inputs(uint8_t seat, uint64_t frame) const;
    // Every input from `frame` on, again through Callbacks::input (a
    // machine running on from a state, its table made afresh).
    void feed_inputs(uint64_t frame);
    bool running() const { return started_; }
    // Our cabinet ours: from the start, or (joined in progress) once taken.
    bool seated() const;

    // This machine's player's inputs, set at the end of `frame` (in order;
    // frames skipped hold the last).
    void local_input(uint64_t frame, const InputBytes& in);
    // The boards' hash at the end of `frame` (compared between machines).
    void report_hash(uint64_t frame, uint64_t hash);
    // Leave (a goodbye to the others).
    void leave();
    // Tests: stop at once without a goodbye, as a machine that crashed or
    // lost its network (the others drop it after kDropAfter).
    void vanish();

    // Rollback's time sync (tools::NetPlay::lead): by seat, its inputs in up to (not including) this frame, the round
    // trip (ms, -1: none) and how long since anything came from it (ms; -1: not a machine in the game).
    struct PeerTiming {
        uint64_t next = 0;
        int rtt = -1, quiet_ms = -1;
    };
    std::array<PeerTiming, kMaxSeats> timing() const;
    // How it goes, for an overlay: round trip to each seat (ms, -1: none).
    std::array<int, kMaxSeats> round_trips() const;
    // What a player's machine is and how it plays, for the overlay (the network status list): sent to the others as
    // it changes (set_status) and every 2 seconds (a player who joins later has it soon).
    struct Status {
        enum class Platform : uint8_t { unknown, windows, macos, linux_os, android };
        enum class Boards : uint8_t { unknown, every, one };  // every board run here, or only its own (one board a PC)
        enum class Net : uint8_t { unknown, delay, rollback }; // inputs waited for, or guessed and gone back for
        Platform platform = Platform::unknown;
        Boards boards = Boards::unknown;
        Net net = Net::unknown;
        int16_t delay = -1;       // input delay, frames (-1: unknown)
        bool auto_delay = false;  // chosen by Auto
        int8_t catch_up = -1;     // joined in progress: percent caught up with the others (-1: not catching up)
        bool operator==(const Status&) const = default;
    };
    void set_status(const Status& status);
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
        std::string country; // theirs, two lower-case letters ("" unknown)
        bool left = false; // gone after saying goodbye
        int round_trip = -1; // ms (-1: not measured yet)
        Status status;       // as its machine last said (ours: set_status's)
        // How this machine reaches the player: on the same network (a private address: a LAN, a hotspot, this PC),
        // straight over the internet, or through the TURN relay (ours or theirs). none: ours, not reached yet, or a
        // spectator's view (the host's seats).
        enum class Link : uint8_t { none, lan, direct, relay };
        Link link = Link::none;
        // A player joining the game in progress (state joining): receiving the state (as far as this machine, the
        // host, has it acknowledged), then catching up (as its machine says), in percent; none: neither yet.
        enum class Joining : uint8_t { none, receiving, catching_up };
        Joining joining = Joining::none;
        int progress = -1;
    };
    std::array<SeatView, kMaxSeats> seats() const;
    // Joining a game (this machine): asking (the host not answered yet), receiving its every-board state (bytes of
    // total), then seating (the state in: catching up and taking the cabinet, until it's ours); none otherwise.
    struct JoinProgress {
        enum class Stage : uint8_t { none, asking, receiving, seating };
        Stage stage = Stage::none;
        std::size_t bytes = 0, total = 0; // (receiving; total 0: not known yet)
    };
    JoinProgress join_progress() const;
    // A player joined in progress whose cabinet isn't theirs yet (catching up): sent no board backups meanwhile,
    // which they don't run on, and which, MBs, would hold up the messages behind them (their cabinet's handover).
    bool joining(uint8_t seat) const;
    // This machine hosts the room: from the start, or since the host left a
    // game in progress (host migration: the lowest seat still connected).
    bool hosting() const;
    // The seat whose machine hosts the room now (host migration changes it; 0xFF: not known yet).
    uint8_t host_seat() const;
    // A player in a room whose game hadn't started: the host closed it (or
    // went quiet). The frontend leaves it.
    bool room_closed() const;

    // ---- Internet play (with the lobby, net::LobbyClient) ----
    // Our address as a STUN server sees it (our router's), found in the
    // background: another machine can try it.
    void find_public_address(const Address& stun_server);
    std::optional<Address> public_address() const;
    // A relay address on a TURN server (its short-lived credentials from the
    // lobby), for when routers won't let us through to each other.
    void use_relay(const std::vector<Address>& turn_servers, const std::string& username, const std::string& password);
    std::optional<Address> relay_address() const;
    // Every address another machine may reach us at (this machine's own on
    // its networks, our router's, the relay's), with our game port.
    std::vector<Address> candidates() const;
    // The host: a player joining from these addresses (the lobby says):
    // datagrams to them now open our router to theirs.
    void expect(const std::vector<Address>& addresses);
    // And its country as the lobby has it (Cloudflare's; never what a game says of itself): the player's who
    // joins from one of those addresses, which the roster passes on to every machine.
    void expect(const std::vector<Address>& addresses, const std::string& country);
    // Seats reached only through a relay (ours or theirs), for an overlay.
    std::array<bool, kMaxSeats> relayed() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string error_;
    std::atomic<bool> started_{false};
};

} // namespace net
