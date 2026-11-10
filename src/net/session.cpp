#include "net/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>

namespace net {

namespace {

using Clock = std::chrono::steady_clock;

// Every datagram: "SK", the protocol, its type, the session, the sender's seat (13 bytes).
// (kGoodbye: a player leaving, as a datagram of its own too, sent beside the reliable goodbye. In an internet test
// (2026-11-06) a one-board player's reliable goodbye, sent in one burst as it quit, never came (2 pieces queued, so
// not its window), and the others held kDropInGame for it; with this datagram too it came. A build without it
// ignores the type.)
// (kUrgent: a drop's messages (kSuspect, kHave, kIdle) as datagrams too, beside the reliable ones: in internet tests a
// player's goodbye held the others 2-5 s, those waiting behind one-board backups' pieces in the reliable channel,
// 2026-11-07; each harmless twice)
enum Type : uint8_t { kBeacon = 1, kJoin = 2, kReliable = 3, kInput = 4, kRefuse = 5, kPunch = 6, kGoodbye = 7, kNodeData = 8, kUrgent = 9 };
constexpr uint8_t kNoSeat = 0xFF;

// Control messages, on the reliable channel.
enum Message : uint8_t { kWelcome = 1, kRoster = 2, kStart = 3, kSuspect = 4, kHave = 5, kIdle = 6, kHash = 7, kBye = 8, kCabinet = 9,
                        kState = 10, kWantState = 11, kTake = 12, kTaken = 13, kWantBoard = 14, kBoard = 15, kParting = 16, kBackup = 17,
                        kWantBackup = 18, kStatus = 19,
                        kWatchWelcome = 20, // (the host to a spectator: its number, the seats, whether the game is on)
                        kWatchInfo = 21,    // (the host to its spectators: every seat as the host sees it, and their count)
                        kWatchers = 22,     // (the host to the players: how many watch, and who)
                        kWatchMode = 23,    // (a spectator to the host: it runs one board: the players' backups wanted)
                        kStateSize = 24,    // (before kState: its size, for the receiver's progress)
                        kNode = 25 };       // (its own board's part of a link packet, for one board a PC's check: Session::send_node)

// Spectators (docs/netplay.md, "Spectators"): each talks to the room's host only, sending as kWatcherBase + its
// number; the players never wait for them, nor count them. They take every seat's inputs from the host and run each
// frame once every seat's are in kWatchLag frames beyond it (about 2 s behind, so a late input never stalls them).
constexpr uint8_t kWatcherBase = 0x10;
constexpr auto kWatchInfoEvery = std::chrono::milliseconds(1000);

constexpr std::size_t kFragment = 1000;    // reliable payload a datagram
constexpr std::size_t kWindow = 256;       // reliable fragments in flight (a save state: some MB)
constexpr uint64_t kTakeMargin = 8;        // frames ahead a cabinet is handed to the player taking it
constexpr auto kResend = std::chrono::milliseconds(150);
constexpr std::size_t kChunkFrames = 64;   // input frames a seat a datagram
constexpr auto kInputResend = std::chrono::milliseconds(4);
constexpr auto kKeepAlive = std::chrono::milliseconds(100);
constexpr auto kJoinRetry = std::chrono::milliseconds(400);
constexpr auto kTakeRetry = std::chrono::seconds(2); // a cabinet asked for again, until it's handed over
constexpr auto kBeaconEvery = std::chrono::milliseconds(1000);

class Writer {
public:
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void bytes(const std::vector<uint8_t>& b) {
        u32(static_cast<uint32_t>(b.size()));
        out.insert(out.end(), b.begin(), b.end());
    }
    void str(const std::string& s) {
        u8(static_cast<uint8_t>(std::min<std::size_t>(s.size(), 255)));
        out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(s.size(), 255)));
    }
};

class Reader {
public:
    Reader(const uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    std::size_t left() const { return n_ - pos_; }
    uint8_t u8() { return static_cast<uint8_t>(take(1)); }
    uint16_t u16() { return static_cast<uint16_t>(take(2)); }
    uint32_t u32() { return static_cast<uint32_t>(take(4)); }
    uint64_t u64() { return take(8); }
    std::vector<uint8_t> bytes() {
        const uint32_t n = u32();
        if (!ok_ || n > left()) return fail<std::vector<uint8_t>>();
        std::vector<uint8_t> b(p_ + pos_, p_ + pos_ + n);
        pos_ += n;
        return b;
    }
    std::string str() {
        const uint8_t n = u8();
        if (!ok_ || n > left()) return fail<std::string>();
        std::string s(reinterpret_cast<const char*>(p_ + pos_), n);
        pos_ += n;
        return s;
    }
    const uint8_t* here() const { return p_ + pos_; }
    void skip(std::size_t n) {
        if (n > left()) ok_ = false;
        else pos_ += n;
    }

private:
    uint64_t take(int n) {
        if (!ok_ || static_cast<std::size_t>(n) > left()) {
            ok_ = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint64_t{p_[pos_ + static_cast<std::size_t>(i)]} << (8 * i);
        pos_ += static_cast<std::size_t>(n);
        return v;
    }
    template <class T> T fail() {
        ok_ = false;
        return T{};
    }
    const uint8_t* p_;
    std::size_t n_, pos_ = 0;
    bool ok_ = true;
};

Writer header(Type type, uint64_t session, uint8_t from) {
    Writer w;
    w.u8('S'), w.u8('K'), w.u8(kProtocol), w.u8(type), w.u64(session), w.u8(from);
    return w;
}

// Messages split into fragments, each sent until acknowledged, delivered
// in order (go-back-N with a window).
struct Reliable {
    struct Fragment {
        uint32_t seq;
        uint8_t flags; // 1: a message's first, 2: its last
        std::vector<uint8_t> data;
        Clock::time_point sent{};
        bool ever_sent = false;
        bool sacked = false; // (the receiver has it, beyond a gap: not sent again)
    };
    std::deque<Fragment> out;
    uint32_t next_seq = 0;
    // Receiving
    uint32_t rx_next = 0;
    std::map<uint32_t, std::pair<uint8_t, std::vector<uint8_t>>> early;
    std::vector<uint8_t> assembling;
    bool ack_due = false;

    void queue(const std::vector<uint8_t>& message) {
        for (std::size_t at = 0; at < message.size() || at == 0; at += kFragment) {
            const std::size_t n = std::min(kFragment, message.size() - at);
            Fragment f{next_seq++, 0, std::vector<uint8_t>(message.begin() + static_cast<std::ptrdiff_t>(at),
                                                         message.begin() + static_cast<std::ptrdiff_t>(at + n))};
            if (at == 0) f.flags |= 1;
            if (at + n >= message.size()) f.flags |= 2;
            out.push_back(std::move(f));
            if (message.empty()) break;
        }
    }
    // Acknowledged: everything before `ack`, and of the 64 pieces after it, those `sack`'s bits name (bit i: ack + 1
    // + i) that came beyond the gap: only the pieces really lost are sent again (one lost piece no longer brings its
    // whole window again, which a slow line or a relay drops in turn).
    void acked(uint32_t ack, uint64_t sack) {
        while (!out.empty() && static_cast<int32_t>(out.front().seq - ack) < 0) out.pop_front();
        if (!sack) return;
        for (Fragment& f : out) {
            const int32_t d = static_cast<int32_t>(f.seq - ack) - 1;
            if (d < 0) continue;
            if (d >= 64) break;
            if (sack >> d & 1) f.sacked = true;
        }
    }
    // What we have beyond the next piece expected, for the sender (acked's `sack`).
    uint64_t sack_bits() const {
        uint64_t bits = 0;
        for (const auto& [seq, piece] : early) {
            const int32_t d = static_cast<int32_t>(seq - rx_next) - 1;
            if (d >= 0 && d < 64) bits |= uint64_t{1} << d;
        }
        return bits;
    }
    // A fragment in: the messages it completes, in order.
    std::vector<std::vector<uint8_t>> receive(uint32_t seq, uint8_t flags, std::vector<uint8_t> data) {
        std::vector<std::vector<uint8_t>> done;
        ack_due = true;
        if (static_cast<int32_t>(seq - rx_next) < 0) return done; // (seen)
        early[seq] = {flags, std::move(data)};
        for (auto it = early.find(rx_next); it != early.end(); it = early.find(rx_next)) {
            if (it->second.first & 1) assembling.clear();
            assembling.insert(assembling.end(), it->second.second.begin(), it->second.second.end());
            if (it->second.first & 2) done.push_back(std::move(assembling)), assembling.clear();
            early.erase(it);
            ++rx_next;
        }
        return done;
    }
};

struct Stream {
    bool known = false;
    uint64_t first = 0;
    std::vector<InputBytes> frames;
    uint64_t next() const { return known ? first + frames.size() : 0; }
};

// A country as the lobby says it (Cloudflare's two letters), lower case; anything else none.
std::string two_letters(const std::string& country) {
    if (country.size() != 2 || !std::isalpha(static_cast<unsigned char>(country[0])) || !std::isalpha(static_cast<unsigned char>(country[1])))
        return {};
    return {static_cast<char>(std::tolower(static_cast<unsigned char>(country[0]))),
            static_cast<char>(std::tolower(static_cast<unsigned char>(country[1])))};
}

// An address on a network of its own, not the internet (RFC 1918's 10/8, 172.16/12, 192.168/16, link-local
// 169.254/16, this PC's 127/8): a player there is on the same network (a LAN, a hotspot) or this machine.
bool private_address(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 0xFF;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

} // namespace

struct Session::Impl {
    Session& owner;
    Callbacks cb;
    uint64_t build;
    UdpSocket socket;
    std::thread thread;
    std::atomic<bool> stop{false};
    mutable std::mutex lock;
    Clock::time_point epoch = Clock::now();

    bool is_host = false;
    // The room's host: seat 0 until it leaves a game in progress, then the
    // lowest seat still connected (every machine works it out alike: host
    // migration). Only its welcome, roster and start are taken.
    uint8_t host_seat = 0;
    bool room_closed = false; // (a joiner: the host left before the game started)
    uint64_t session = 0;
    std::string game_name, my_name;
    std::string my_country; // (the lobby's two letters for this player, Session::set_country; "" none)
    Session::Status my_status;            // (set_status: sent as it changes and every kStatusEvery)
    Clock::time_point status_sent{};
    static constexpr auto kStatusEvery = std::chrono::seconds(2);
    void send_status() {
        Writer w;
        w.u8(kStatus), w.u8(static_cast<uint8_t>(my_status.platform)), w.u8(static_cast<uint8_t>(my_status.boards)),
            w.u8(static_cast<uint8_t>(my_status.net)), w.u16(static_cast<uint16_t>(my_status.delay)), w.u8(my_status.auto_delay ? 1 : 0);
        w.u8(static_cast<uint8_t>(my_status.catch_up + 1)); // (0: not catching up)
        send_all(w.out);
        status_sent = Clock::now();
    }
    std::vector<uint8_t> my_eeprom, my_backup; // this player's cabinet (Session::set_cabinet)
    uint8_t seats = 0, my_seat = kNoSeat;
    bool started = false, leaving = false, joining = false;
    Address host_address;
    std::vector<Address> host_candidates; // (a joiner: every address the host may be at)
    // Internet play: our router's address (STUN), the relay (TURN), routers to open
    std::optional<Address> stun_server, public_address;
    std::array<uint8_t, 12> stun_id{};
    Clock::time_point stun_sent{};
    int stun_tries = 0;
    std::optional<TurnClient> turn;
    struct Punch {
        std::vector<Address> to;
        Clock::time_point until;
    };
    std::vector<Punch> punches;
    // (the host: a joiner's country as the lobby said it, by the addresses it said the joiner is at)
    struct CountrySaid {
        std::vector<Address> at;
        std::string country;
        Clock::time_point until;
    };
    std::vector<CountrySaid> countries_said;
    Clock::time_point last_punch{};
    Clock::time_point last_join{}, last_beacon{};
    std::vector<Address> beacon_targets; // (the host: every network's broadcast address)
    Clock::time_point targets_asked{};

    // A rate for bulk sending (Impl::cap_take): bytes a second, and the bytes allowed now.
    struct Cap {
        double rate = 256e3, tokens = 0; // bytes a second; bytes allowed now
        Clock::time_point at{}, paused_until{}, changed{};
    };

    struct Peer {
        bool present = false;   // a player in this seat (not me)
        bool connected = false; // heard from, not dropped
        bool dropped = false;
        Address address;
        std::string name, country;
        uint64_t player = 0;
        bool cabinet = false; // (the host: its cabinet in, as it was)
        std::vector<uint8_t> eeprom, backup;
        Clock::time_point heard{};
        Reliable rel;
        std::array<uint64_t, kMaxSeats> acks{}; // the next frame of each seat they need
        Clock::time_point inputs_sent{};
        bool inputs_due = false;
        uint32_t their_time = 0; // their last send time, and when it came (round trips)
        Clock::time_point their_time_at{};
        int rtt = -1;
        std::vector<Address> candidates; // where it may be reached (tried until it answers)
        bool reached = false;
        Clock::time_point direct_heard{}; // (last heard not through our relay)
        bool wants_state = false;         // (the host: it waits for every board's state)
        Session::Status status;           // (as it last said: kStatus)
        int rtt_floor = -1;               // (its least round trip so far: Cap's guard)
        // Its reliable channel paced (a joiner's every-board state: MBs a relay or a slow line drops in a burst):
        // the rate grows a quarter each second nothing was sent again, halves when pieces had to be.
        Cap pace;
        unsigned resent = 0, sent = 0; // (pieces this second)
        // (the host: every board's state on its way to it, its pieces' sequence numbers [state_first, state_end))
        uint32_t state_first = 0, state_end = 0;
        bool state_going = false;
    };
    std::array<Peer, kMaxSeats> peers;
    std::array<Stream, kMaxSeats> streams;

    // ---- Spectators ----
    // The host: each spectator (none of a seat's: never waited for, never counted by the players).
    struct Watcher {
        bool present = false;
        Address address;
        std::vector<Address> candidates;
        bool reached = true;
        std::string name, country;
        uint64_t player = 0;
        Clock::time_point heard{}, inputs_sent{}, info_sent{};
        Reliable rel;
        std::array<uint64_t, kMaxSeats> acks{}; // (the next frame of each seat it needs)
        bool wants_state = false;               // (no inputs until its state has gone: they start after it)
        bool wants_backups = false;             // (it runs one board: every player's backups to it, kWatchMode)
        uint32_t their_time = 0;
        Clock::time_point their_time_at{};
    };
    std::array<Watcher, kMaxWatchers> watchers;
    // A player (not the host): its channel to each spectator on one board, for its own board's backups (the host
    // sends them on its Watcher channel). Taken from the host's kWatchers list.
    struct Bulk {
        bool present = false;
        uint64_t player = 0;
        Address address;
        std::vector<Address> candidates;
        bool reached = false;
        Clock::time_point heard{};
        Reliable rel;
    };
    std::array<Bulk, kMaxWatchers> bulk;
    bool watch_one_board = false; // (a spectator: it runs one board, Session::set_watch_one_board)
    Clock::time_point watch_punched{};
    std::size_t watchers_told = 0;    // (the count the players were last told)
    Clock::time_point watchers_sent{};
    std::size_t watching_count = 0;   // (a player or a spectator: as the host says)
    // A spectator: its number (kNoSeat: not one, or not let in yet), the frames run so far (each frame's inputs
    // given out once every seat's are in kWatchLag beyond it), and every seat as the host sees it.
    bool watch_mode = false;
    uint8_t my_watch = kNoSeat;
    std::optional<uint64_t> watch_next;
    std::array<Session::SeatView, kMaxSeats> watch_view{};
    Clock::time_point watch_acked{};
    uint8_t me() const { return my_watch != kNoSeat ? static_cast<uint8_t>(kWatcherBase + my_watch) : my_seat; }
    StartInfo start_info;

    // A cabinet no player has (never taken, or its player dropped): from
    // frame `from` its inputs are none, made by the authority (the lowest
    // seat still connected) as it makes its own, and sent as anyone's, so
    // every machine has the same; up to `until`, where a player taking it
    // carries on.
    struct Seat {
        bool open = false;
        uint64_t from = 0;
        std::optional<uint64_t> until;
    };
    std::array<Seat, kMaxSeats> cabinets;
    // A game joined in progress (or a machine put back in step): its start
    // (and then every board's state) from the host; our cabinet ours from
    // `mine_from`, our inputs before then kept until they can go out.
    uint64_t send_hold = 0; // (tests: our inputs sent this many frames' time late, Session::hold_inputs)
    std::vector<Clock::time_point> set_at; // (then: when each of our frames was set, from our stream's first)
    uint64_t send_failures = 0, datagrams_in = 0; // (told with a peer not answering)
    bool late = false, want_state = false;
    uint32_t state_size = 0; // (a joiner: the coming kState's size, from kStateSize: its progress)
    std::optional<StartInfo> late_start;
    std::optional<uint64_t> mine_from;
    // Our cabinet asked for (kTake), until it's handed over (kTaken): asked
    // again of whoever is the authority then, now and then and whenever the
    // authority changes, since an authority that has left, or that a
    // machine only took for one a moment, never answers.
    bool take_wanted = false;
    uint8_t take_asked = kNoSeat;
    Clock::time_point take_sent{};
    std::optional<uint64_t> state_at; // (the frame of the last state we ran on from: hashes before it not compared)
    std::map<uint64_t, InputBytes> pending_local;
    std::array<bool, kMaxSeats> quiet_told{}; // (Callbacks::quiet: as last told, each seat)

    struct Drop {
        bool suspected = false;
        uint8_t asked_by = kNoSeat;
        std::map<uint8_t, uint64_t> answers;
        bool announced = false;
    };
    std::array<Drop, kMaxSeats> drops;
    // A seat whose player went (Session::seats): their name and country, and whether they said goodbye.
    std::array<std::string, kMaxSeats> gone_name, gone_country;
    std::array<bool, kMaxSeats> said_bye{};
    std::map<uint64_t, uint64_t> my_hashes;
    std::map<std::pair<uint64_t, uint8_t>, uint64_t> their_hashes;

    Impl(Session& s, Callbacks c, uint64_t b) : owner(s), cb(std::move(c)), build(b) {}
    Session::SeatView view(uint8_t s) const; // (a seat for the overlay, as this machine sees it)

    uint32_t now_ms() const {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count());
    }
    void log(const std::string& s) {
        if (cb.log) cb.log(s);
    }

    // ---- Sending ----

    void send(const Address& to, const Writer& w) { send_raw(to, w.out); }
    // Tests: SPIKEOUT_TEST_LATENCY=MS[,MS@SECONDS...] holds every datagram this machine sends that long, as a network
    // that far away would (two windows on one PC as players apart: the network settings' Auto chooses by it); later
    // values from that many seconds after the session opened (the delay in play following a change).
    struct Held {
        Clock::time_point due;
        Address to;
        std::vector<uint8_t> data;
    };
    std::deque<Held> held_out;
    int test_latency_ms(Clock::time_point now) const {
        static const std::vector<std::pair<int, int>> steps = [] { // (seconds from, ms)
            std::vector<std::pair<int, int>> out;
            if (const char* v = std::getenv("SPIKEOUT_TEST_LATENCY"))
                for (const char* p = v; *p;) {
                    char* end = nullptr;
                    const int ms = static_cast<int>(std::strtol(p, &end, 10));
                    if (end == p) break; // (not a number: the rest left)
                    int from = 0;
                    if (*end == '@') from = static_cast<int>(std::strtol(end + 1, &end, 10));
                    out.push_back({from, ms});
                    if (*end != ',') break;
                    p = end + 1;
                }
            return out;
        }();
        int ms = 0;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - epoch).count();
        for (const auto& [from, v] : steps)
            if (seconds >= from) ms = v;
        return ms;
    }
    void send_held(Clock::time_point now) {
        while (!held_out.empty() && held_out.front().due <= now) {
            Held h = std::move(held_out.front());
            held_out.pop_front();
            send_now(h.to, h.data);
        }
    }
    // Tests: SPIKEOUT_TEST_LINK=KB[,LOSS] this machine's uplink as a slow line: KB kilobytes a second, a second's
    // queue at most (more is dropped, as a router's full buffer drops), and LOSS percent of datagrams lost on the
    // way. With SPIKEOUT_TEST_LATENCY, a far and slow connection on one PC (the join's pacing and progress).
    struct TestLink {
        double bytes_per_s = 0, loss = 0;
    };
    static const TestLink& test_link() {
        static const TestLink link = [] {
            TestLink l;
            if (const char* v = std::getenv("SPIKEOUT_TEST_LINK")) {
                char* end = nullptr;
                l.bytes_per_s = std::strtod(v, &end) * 1024;
                if (*end == ',') l.loss = std::strtod(end + 1, nullptr) / 100;
            }
            return l;
        }();
        return link;
    }
    Clock::time_point link_free{};
    std::minstd_rand link_random{12345};
    std::array<uint64_t, 256> sent_bytes{}; // (SPIKEOUT_NET_DEBUG: bytes sent by datagram type, every 5 s)
    Clock::time_point sent_shown{};
    void send_raw(const Address& to, std::span<const uint8_t> data) {
        const auto now = Clock::now();
        static const bool debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
        if (debug) {
            sent_bytes[data.size() > 3 ? data[3] : 255] += data.size() + 28;
            if (now - sent_shown > std::chrono::seconds(5)) {
                std::string line;
                for (std::size_t t = 0; t < sent_bytes.size(); ++t)
                    if (sent_bytes[t]) line += " type " + std::to_string(t) + ": " + std::to_string(sent_bytes[t] / 5120) + " KB/s;";
                line += " inputs known:";
                for (uint8_t s = 0; s < seats && s < kMaxSeats; ++s) line += " " + std::to_string(streams[s].next());
                line += ";";
                if (late) // (a cabinet joined in progress: ours from, and our inputs held back)
                    line += " ours from " + (mine_from ? std::to_string(*mine_from) : std::string("?")) + ", " +
                            std::to_string(pending_local.size()) + " held" +
                            (pending_local.empty() ? std::string() : " (" + std::to_string(pending_local.begin()->first) + "-" +
                                                                         std::to_string(pending_local.rbegin()->first) + ")") + ";";
                for (uint8_t s = 0; s < kMaxSeats; ++s)
                    if (peers[s].present)
                        line += " seat " + std::to_string(s + 1) + ": " + std::to_string(peers[s].rel.out.size()) + " pieces queued, pace " +
                                std::to_string(static_cast<int>(peers[s].pace.rate / 1024)) + " KB/s, rtt " + std::to_string(peers[s].rtt) +
                                (peers[s].rel.out.empty() ? std::string() : ", front " + std::to_string(peers[s].rel.out.front().seq) + " type " +
                                 std::to_string(peers[s].rel.out.front().data.empty() ? -1 : peers[s].rel.out.front().data[0])) + ";";
                if (sent_shown != Clock::time_point{}) std::fprintf(stderr, "sent:%s\n", line.c_str());
                sent_bytes.fill(0), sent_shown = now;
            }
        }
        // (tests: SPIKEOUT_TEST_SILENT=SECONDS,MS: nothing sent for MS ms from that many seconds after the session
        // opened, then as before: a machine stalled or its network gone a moment, back; the others' game goes on)
        static const std::pair<double, double> silent = [] {
            double from = 0, ms = 0;
            if (const char* v = std::getenv("SPIKEOUT_TEST_SILENT")) std::sscanf(v, "%lf,%lf", &from, &ms);
            return std::pair{from, ms};
        }();
        if (silent.second > 0) {
            const double t = std::chrono::duration<double>(now - epoch).count();
            if (t >= silent.first && t < silent.first + silent.second / 1000) return;
        }
        const TestLink& link = test_link();
        auto due = now + std::chrono::milliseconds(test_latency_ms(now));
        if (link.loss > 0 && std::uniform_real_distribution<double>(0, 1)(link_random) < link.loss) return; // (lost)
        if (link.bytes_per_s > 0) {
            if (link_free < now) link_free = now;
            if (link_free - now > std::chrono::seconds(1)) return; // (the line's queue full: dropped)
            link_free += std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(static_cast<double>(data.size() + 28) / link.bytes_per_s));
            due = std::max(due, link_free);
        }
        if (due > now) {
            held_out.push_back({due, to, std::vector<uint8_t>(data.begin(), data.end())});
            return;
        }
        send_now(to, data);
    }
    void send_now(const Address& to, std::span<const uint8_t> data) {
        // (tests: SPIKEOUT_TEST_RELAY_ONLY: the game's datagrams out through our relay too, as in comes only through it:
        // a router letting nothing through between the players, every exchange relay to relay; else a direct send to
        // the other's relay had its answer, straight back from that relay, refused here)
        static const bool relay_only = std::getenv("SPIKEOUT_TEST_RELAY_ONLY") != nullptr;
        const bool through_relay = to.relayed || (relay_only && !stun::is_stun(data) && turn && turn->relayed());
        if (through_relay && turn) turn->send_to(Address{to.ip, to.port}, data, turn_send());
        else if (!socket.send_to(to, data)) ++send_failures;
    }
    TurnClient::Send turn_send() {
        return [this](const Address& a, std::span<const uint8_t> d) { socket.send_to(a, d); };
    }
    // To a peer: where it answered from, or (not yet) everywhere it may be.
    template <class P> void send_peer(const P& p, const Writer& w) {
        if (p.reached) return send(p.address, w);
        send(p.address, w);
        for (const Address& a : p.candidates)
            if (!a.same_place(p.address)) send(a, w);
    }
    void permit(const Address& a) {
        if (turn) turn->permit(a.ip);
    }

    void send_reliable(uint8_t seat, const std::vector<uint8_t>& message) {
        static const bool debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
        if (debug && message.size() > 8000)
            std::fprintf(stderr, "reliable to seat %u: message %u, %zu bytes\n", seat + 1, message.empty() ? 0u : message[0], message.size());
        peers[seat].rel.queue(message);
    }
    void send_all(const std::vector<uint8_t>& message, uint8_t except = kNoSeat) {
        for (uint8_t s = 0; s < kMaxSeats; ++s)
            if (s != except && s != my_seat && peers[s].present && peers[s].connected) send_reliable(s, message);
    }
    // (a drop's message: reliable, and three datagrams of its own that no queued piece holds back)
    void send_urgent(uint8_t seat, const std::vector<uint8_t>& message) {
        send_reliable(seat, message);
        Writer w = header(kUrgent, session, me());
        w.bytes(message);
        for (int k = 0; k < 3; ++k) send_peer(peers[seat], w);
    }
    void send_all_urgent(const std::vector<uint8_t>& message, uint8_t except = kNoSeat) {
        for (uint8_t s = 0; s < kMaxSeats; ++s)
            if (s != except && s != my_seat && peers[s].present && peers[s].connected) send_urgent(s, message);
    }

    // Spectator traffic's speed (the owner's rules, 2026-11-04): never before the players' own, held to a rate shared by
    // every spectator this machine sends to (its uplink is one), which grows while the players' round trips stay as
    // they were and halves, pausing a second, as soon as one rises (a full uplink delays the inputs first). The same
    // for the host's channel to each spectator (its every-board state too) and a player's backups to one.
    Cap spectator_cap;
    static constexpr double kCapMin = 16e3, kCapMax = 4e6;
    bool cap_take(Cap& c, std::size_t bytes, Clock::time_point now) {
        if (now < c.paused_until) return false;
        if (c.at != Clock::time_point{})
            c.tokens = std::min(c.rate * 0.1, c.tokens + c.rate * std::chrono::duration<double>(now - c.at).count());
        c.at = now;
        if (c.tokens < static_cast<double>(bytes)) return false;
        c.tokens -= static_cast<double>(bytes);
        return true;
    }
    // (each turn: a player's round trip above its least so far by 40 ms: the uplink is filling)
    void guard_spectator_cap(Clock::time_point now) {
        bool rising = false;
        for (uint8_t s = 0; s < seats && s < kMaxSeats; ++s) {
            Peer& p = peers[s];
            if (s == my_seat || !p.present || p.dropped || p.rtt < 0) continue;
            p.rtt_floor = p.rtt_floor < 0 ? p.rtt : std::min(p.rtt_floor, p.rtt);
            if (p.rtt > p.rtt_floor + 40) rising = true;
        }
        Cap& c = spectator_cap;
        if (rising && now - c.changed > std::chrono::seconds(1)) {
            c.rate = std::max(kCapMin, c.rate / 2), c.paused_until = now + std::chrono::seconds(1), c.changed = now, c.tokens = 0;
        } else if (!rising && now - c.changed > std::chrono::seconds(1)) {
            c.rate = std::min(kCapMax, c.rate * 1.25), c.changed = now;
        }
    }
    // The host: how many of a peer's every-board state pieces (Session::send_state) it hasn't acknowledged yet.
    static std::size_t state_left(const Peer& p) {
        if (!p.state_going || p.rel.out.empty()) return 0;
        const uint32_t front = p.rel.out.front().seq;
        if (static_cast<int32_t>(front - p.state_end) >= 0) return 0;
        const uint32_t from = static_cast<int32_t>(front - p.state_first) > 0 ? front : p.state_first;
        return p.state_end - from;
    }
    // A player's reliable channel's rate, each second: up a quarter while what's sent gets through (a busy second
    // with almost nothing sent again), halved when more had to be (a relay or a line dropping the rest), from 1 MB/s.
    static constexpr double kPaceStart = 1e6, kPaceMin = 32e3, kPaceMax = 16e6;
    void pace(Peer& p, Clock::time_point now) {
        Cap& c = p.pace;
        if (c.changed == Clock::time_point{}) // (the same network: a line many times faster, so from far higher)
            c.rate = !p.address.relayed && private_address(p.address.ip) ? kPaceMax / 2 : kPaceStart, c.changed = now;
        if (now - c.changed < std::chrono::seconds(1)) return;
        // (and halved, as the spectators' rate is, when a player's round trip rises: the uplink filling delays the
        // players' inputs before anything is lost, and they come first)
        bool rising = false;
        for (uint8_t s = 0; s < seats && s < kMaxSeats; ++s) {
            Peer& o = peers[s];
            // (the peer sent to counts too, once it plays: its own inputs wait behind what fills the line to it; a joiner
            // still taking in the state isn't playing yet)
            if (s == my_seat || !o.present || o.dropped || o.rtt < 0 || (&o == &p && state_left(p) > 0)) continue;
            o.rtt_floor = o.rtt_floor < 0 ? o.rtt : std::min(o.rtt_floor, o.rtt);
            if (o.rtt > o.rtt_floor + 40) rising = true;
        }
        if (p.resent > std::max(4u, p.sent / 20) || (rising && p.sent > 0)) c.rate = std::max(kPaceMin, c.rate / 2);
        else if (p.sent >= 16) c.rate = std::min(kPaceMax, c.rate * 1.25); // (only a busy second shows the line takes more)
        c.changed = now, p.resent = 0, p.sent = 0;
    }
    template <class P> void pump_reliable(uint8_t seat, P& p, Cap* cap = nullptr) {
        const auto now = Clock::now();
        bool sent_any = false;
        std::size_t n = 0;
        // (a player's: sent again after twice its round trip and a tenth of a second: a far one's pieces, or ones queued
        // on a full line, aren't lost yet, and sent again too soon they'd count as lost and halve its pace)
        Clock::duration resend = kResend;
        if constexpr (requires { p.pace; })
            if (p.rtt > 0) resend = std::max<Clock::duration>(kResend, std::chrono::milliseconds(p.rtt * 2 + 100));
        for (auto& f : p.rel.out) {
            if (n++ >= kWindow) break;
            if (f.sacked || (f.ever_sent && now - f.sent < resend)) continue;
            if (cap && !cap_take(*cap, f.data.size() + 32, now)) break; // (spectator traffic: its share only)
            if constexpr (requires { p.resent; }) ++(f.ever_sent ? p.resent : p.sent);
            Writer w = header(kReliable, session, me());
            w.u32(p.rel.rx_next), w.u64(p.rel.sack_bits()), w.u8(1), w.u32(f.seq), w.u8(f.flags);
            w.out.insert(w.out.end(), f.data.begin(), f.data.end());
            send_peer(p, w);
            f.sent = now, f.ever_sent = true;
            sent_any = true;
        }
        if (p.rel.ack_due && !sent_any) {
            Writer w = header(kReliable, session, me());
            w.u32(p.rel.rx_next), w.u64(p.rel.sack_bits()), w.u8(0);
            send_peer(p, w);
        }
        p.rel.ack_due = false;
        (void)seat;
    }

    // Inputs: every seat's frames this peer hasn't acknowledged (to seat `to`).
    void send_inputs(uint8_t to, Peer& p) {
        Writer w = header(kInput, session, my_seat);
        const auto now = Clock::now();
        const uint32_t held = p.their_time_at == Clock::time_point{} ? 0 :
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - p.their_time_at).count());
        w.u32(now_ms()), w.u32(p.their_time), w.u16(static_cast<uint16_t>(std::min<uint32_t>(held, 65535)));
        w.u8(seats);
        for (uint8_t s = 0; s < seats; ++s) w.u64(s == my_seat ? held_back(streams[s].next()) : streams[s].next());
        uint8_t chunks = 0;
        const std::size_t count_at = w.out.size();
        w.u8(0);
        for (uint8_t s = 0; s < seats; ++s) {
            const Stream& st = streams[s];
            const uint64_t end = sendable(s, to);
            if (!st.known) continue;
            const uint64_t from = std::max(p.acks[s], st.first);
            if (from >= end) continue;
            const uint64_t n = std::min<uint64_t>(end - from, kChunkFrames);
            w.u8(s), w.u64(from), w.u8(static_cast<uint8_t>(n));
            for (uint64_t f = from; f < from + n; ++f)
                w.out.insert(w.out.end(), st.frames[static_cast<std::size_t>(f - st.first)].begin(),
                             st.frames[static_cast<std::size_t>(f - st.first)].end());
            ++chunks;
        }
        w.out[count_at] = chunks;
        send_peer(p, w);
        p.inputs_sent = now;
        p.inputs_due = false;
    }
    bool inputs_pending(uint8_t to, const Peer& p) const {
        for (uint8_t s = 0; s < seats; ++s)
            if (streams[s].known && std::max(p.acks[s], streams[s].first) < sendable(s, to)) return true;
        return false;
    }
    // Seat s's frames to send to `to`: up to this. Its own seat's only while that cabinet had no player (a game
    // joined in progress: the frames before it took it over); ours held back (tests: hold_inputs).
    uint64_t sendable(uint8_t s, uint8_t to) const {
        const Stream& st = streams[s];
        const uint64_t end = s != to ? st.next() : cabinets[s].until ? std::min(st.next(), *cabinets[s].until)
                                                 : cabinets[s].open ? st.next() : 0;
        return s == my_seat ? held_back(end) : end;
    }
    // (by time, as a network's latency is: a count of frames would make the others wait on our own frames going on,
    // and where every machine waits for the others' real inputs (one board a PC's switches), none would go on)
    uint64_t held_back(uint64_t end) const {
        if (!send_hold) return end;
        const Stream& st = streams[my_seat];
        const auto due = Clock::now() - std::chrono::microseconds(send_hold * 16667);
        uint64_t f = std::min<uint64_t>(end, st.first + set_at.size());
        while (f > st.first && set_at[static_cast<std::size_t>(f - 1 - st.first)] > due) --f;
        return f;
    }

    // The host to a spectator: every seat's frames it hasn't acknowledged (as far as the host has them: each seat's
    // are only those every player has had, since the host's own go out as they're set).
    void send_watch_inputs(Watcher& v) {
        Writer w = header(kInput, session, my_seat);
        const auto now = Clock::now();
        const uint32_t held = v.their_time_at == Clock::time_point{} ? 0 :
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - v.their_time_at).count());
        w.u32(now_ms()), w.u32(v.their_time), w.u16(static_cast<uint16_t>(std::min<uint32_t>(held, 65535)));
        w.u8(seats);
        for (uint8_t s = 0; s < seats; ++s) w.u64(watch_end(s));
        const std::size_t count_at = w.out.size();
        w.u8(0);
        uint8_t chunks = 0;
        if (!v.wants_state)
            for (uint8_t s = 0; s < seats; ++s) {
                const Stream& st = streams[s];
                if (!st.known) continue;
                const uint64_t from = std::max(v.acks[s], st.first), end = watch_end(s);
                if (from >= end) continue;
                const uint64_t n = std::min<uint64_t>(end - from, kChunkFrames);
                w.u8(s), w.u64(from), w.u8(static_cast<uint8_t>(n));
                for (uint64_t f = from; f < from + n; ++f)
                    w.out.insert(w.out.end(), st.frames[static_cast<std::size_t>(f - st.first)].begin(),
                                 st.frames[static_cast<std::size_t>(f - st.first)].end());
                ++chunks;
            }
        w.out[count_at] = chunks;
        send_peer(v, w);
        v.inputs_sent = now;
    }
    uint64_t watch_end(uint8_t s) const { return s == my_seat ? held_back(streams[s].next()) : streams[s].next(); }
    bool watch_pending(const Watcher& v) const {
        if (v.wants_state) return false;
        for (uint8_t s = 0; s < seats; ++s)
            if (streams[s].known && std::max(v.acks[s], streams[s].first) < watch_end(s)) return true;
        return false;
    }
    std::size_t watcher_count() const {
        return static_cast<std::size_t>(std::count_if(watchers.begin(), watchers.end(), [](const Watcher& v) { return v.present; }));
    }
    // Every seat as the host sees it (its own a player's), and how many watch: for the spectators' overlay.
    std::vector<uint8_t> watch_info() const {
        Writer w;
        w.u8(kWatchInfo), w.u8(seats), w.u8(static_cast<uint8_t>(watcher_count()));
        for (uint8_t s = 0; s < seats; ++s) {
            Session::SeatView v = view(s);
            if (v.state == Session::SeatView::State::me) v.state = Session::SeatView::State::playing, v.round_trip = 0;
            w.u8(static_cast<uint8_t>(v.state)), w.str(v.name), w.str(v.country), w.u16(static_cast<uint16_t>(v.round_trip + 1));
            w.u8(v.left ? 1 : 0), w.u8(static_cast<uint8_t>(v.status.platform)), w.u8(static_cast<uint8_t>(v.status.boards));
            w.u8(static_cast<uint8_t>(v.status.net)), w.u16(static_cast<uint16_t>(v.status.delay)), w.u8(v.status.auto_delay ? 1 : 0);
            // (where each other player may be reached: a spectator on one board takes their backups straight from them)
            std::vector<Address> where;
            if (s != my_seat && peers[s].present && !peers[s].dropped) {
                where.push_back(Address{peers[s].address.ip, peers[s].address.port});
                for (const Address& a : peers[s].candidates)
                    if (!a.same_place(where.front())) where.push_back(a);
            }
            put_addresses(w, where);
        }
        return w.out;
    }
    void on_watch_info(Reader& r) {
        const uint8_t n = r.u8();
        const uint8_t count = r.u8();
        std::array<Session::SeatView, kMaxSeats> out{};
        for (uint8_t s = 0; s < n && r.ok(); ++s) {
            Session::SeatView v;
            v.state = static_cast<Session::SeatView::State>(std::min<uint8_t>(r.u8(), 5));
            v.name = r.str(), v.country = r.str();
            v.round_trip = static_cast<int>(r.u16()) - 1;
            v.left = r.u8() != 0;
            v.status.platform = static_cast<Session::Status::Platform>(std::min<uint8_t>(r.u8(), 4));
            v.status.boards = static_cast<Session::Status::Boards>(std::min<uint8_t>(r.u8(), 2));
            v.status.net = static_cast<Session::Status::Net>(std::min<uint8_t>(r.u8(), 2));
            v.status.delay = static_cast<int16_t>(r.u16());
            v.status.auto_delay = r.u8() != 0;
            std::vector<Address> where = get_addresses(r);
            if (s < kMaxSeats) out[s] = v;
            // (a player other than the host: reachable for its backups, our router opened to it: watch_punch)
            if (r.ok() && s < kMaxSeats && s != host_seat && !where.empty()) {
                Peer& p = peers[s];
                if (!p.present || !p.reached) p.present = true, p.address = where.front();
                p.candidates = std::move(where);
                for (const Address& a : p.candidates) permit(a);
            }
        }
        if (r.ok()) watch_view = out, watching_count = count;
    }
    // A spectator on one board: datagrams to every player now and then (our router opened to their backups), and
    // their channels' acknowledgements (the backups' wants, Session::want_backup, on them too).
    void watch_punch(Clock::time_point now) {
        for (uint8_t s = 0; s < seats && s < kMaxSeats; ++s) {
            Peer& p = peers[s];
            if (s == host_seat || !p.present) continue;
            if (now - watch_punched > std::chrono::milliseconds(500)) {
                const Writer w = header(kPunch, session, me());
                send_peer(p, w);
            }
            pump_reliable(s, p);
        }
        if (now - watch_punched > std::chrono::milliseconds(500)) watch_punched = now;
    }

    // A spectator: each frame's inputs given out (Callbacks::input, every seat) once every seat's are in kWatchLag
    // frames beyond it.
    void deliver_watched(bool all = false) {
        if (!started || seats == 0) return;
        uint64_t first = 0, end = UINT64_MAX;
        for (uint8_t s = 0; s < seats; ++s) {
            if (!streams[s].known) return;
            first = std::max(first, streams[s].first);
            end = std::min(end, streams[s].next());
        }
        if (!watch_next) watch_next = first;
        while (*watch_next + (all ? 0 : kWatchLag) < end) {
            const uint64_t f = (*watch_next)++;
            for (uint8_t s = 0; s < seats; ++s)
                if (cb.input && f >= streams[s].first) cb.input(s, f, streams[s].frames[static_cast<std::size_t>(f - streams[s].first)]);
        }
    }
    // A spectator to the host: how far it has each seat (the host sends on from there), and its time (round trips).
    void send_watch_acks() {
        Peer& h = peers[host_seat];
        Writer w = header(kInput, session, me());
        const auto now = Clock::now();
        const uint32_t held = h.their_time_at == Clock::time_point{} ? 0 :
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - h.their_time_at).count());
        w.u32(now_ms()), w.u32(h.their_time), w.u16(static_cast<uint16_t>(std::min<uint32_t>(held, 65535)));
        w.u8(seats);
        for (uint8_t s = 0; s < seats; ++s) w.u64(streams[s].next());
        w.u8(0);
        send_peer(h, w);
        watch_acked = now;
    }

    // The host: a datagram from a spectator.
    void on_watcher(uint8_t id, const UdpSocket::Datagram& d, Reader& r, uint8_t type) {
        Watcher& v = watchers[id];
        if (!v.present) return;
        v.address = d.from, v.heard = Clock::now(), v.reached = true;
        if (type == kReliable) {
            const uint32_t ack = r.u32();
            const uint64_t sack = r.u64();
            const uint8_t has = r.u8();
            if (!r.ok()) return;
            v.rel.acked(ack, sack);
            if (!has) return;
            const uint32_t seq = r.u32();
            const uint8_t flags = r.u8();
            if (!r.ok()) return;
            std::vector<uint8_t> data(r.here(), r.here() + r.left());
            for (auto& m : v.rel.receive(seq, flags, std::move(data))) {
                if (m.empty()) continue;
                if (m[0] == kBye) {
                    log(v.name + " stopped watching");
                    v = Watcher{};
                    return;
                }
                if (m[0] == kWatchMode && m.size() >= 2) { // (it runs one board, or every board again)
                    if (v.wants_backups != (m[1] != 0))
                        log(v.name + (m[1] ? " watches on one board: the backups to it" : " watches on every board again"));
                    v.wants_backups = m[1] != 0;
                    watchers_sent = Clock::time_point{}; // (the players told at once)
                }
                if (m[0] == kWantBackup && cb.backup_wanted) cb.backup_wanted(static_cast<uint8_t>(kWatcherBase + id));
            }
        } else if (type == kInput) {
            v.their_time = r.u32(), v.their_time_at = Clock::now();
            r.u32(), r.u16();
            const uint8_t n = r.u8();
            for (uint8_t s = 0; s < n && s < kMaxSeats && r.ok(); ++s) {
                const uint64_t next = r.u64();
                if (r.ok()) v.acks[s] = v.acks[s] == UINT64_MAX ? next : std::max(v.acks[s], next); // (MAX: taken over, not heard yet)
            }
        }
    }
    // The host: a spectator asks to watch.
    void on_watch_join(const Address& from, const uint64_t player, const std::string& name, std::vector<Address> where,
                       const std::string& country) {
        for (Watcher& v : watchers)
            if (v.present && v.player == player) return; // (already in: our welcome is on its way)
        uint8_t id = kNoSeat;
        for (uint8_t i = 0; i < kMaxWatchers && id == kNoSeat; ++i)
            if (!watchers[i].present) id = i;
        if (id == kNoSeat) {
            Writer w = header(kRefuse, session, my_seat);
            w.str(std::to_string(kMaxWatchers) + " are watching already");
            send(from, w);
            return;
        }
        Watcher& v = watchers[id];
        v = Watcher{};
        v.present = true, v.address = from, v.candidates = std::move(where), v.name = name, v.country = country, v.player = player;
        v.heard = Clock::now();
        permit(from);
        for (const Address& a : v.candidates) permit(a);
        log(name + " is watching" + (started ? " (the game in progress)" : ""));
        Writer w;
        w.u8(kWatchWelcome), w.u8(id), w.u8(seats), w.u8(started ? 1 : 0);
        v.rel.queue(w.out);
        if (started) { // the start as it was, then every board as it is now (Callbacks::state_wanted)
            v.rel.queue(start_message(start_info));
            v.wants_state = true;
            if (cb.state_wanted) cb.state_wanted();
        }
    }
    // The host, each turn of the loop: the spectators' inputs, overlay and reliable messages; one gone quiet let go;
    // the players told how many watch.
    void serve_watchers(Clock::time_point now) {
        for (Watcher& v : watchers) {
            if (!v.present) continue;
            if (now - v.heard > kDropAfter) {
                log(v.name + " stopped watching (not answering)");
                v = Watcher{};
                continue;
            }
            if (started && ((watch_pending(v) && now - v.inputs_sent >= kInputResend) || now - v.inputs_sent >= kKeepAlive))
                send_watch_inputs(v);
            if (now - v.info_sent >= kWatchInfoEvery) {
                v.rel.queue(watch_info());
                v.info_sent = now;
            }
        }
        pump_spectators(now);
        const std::size_t n = watcher_count();
        if (n != watchers_told || now - watchers_sent > std::chrono::seconds(2)) {
            // (the count for the overlay, and each spectator: whoever hosts the room next takes them over)
            Writer w;
            w.u8(kWatchers), w.u8(static_cast<uint8_t>(n));
            for (uint8_t i = 0; i < kMaxWatchers; ++i) {
                const Watcher& v = watchers[i];
                if (!v.present) continue;
                w.u8(i), w.u64(v.player), w.str(v.name), w.str(v.country);
                std::vector<Address> where{Address{v.address.ip, v.address.port}};
                for (const Address& a : v.candidates)
                    if (!a.same_place(where.front())) where.push_back(a);
                put_addresses(w, where);
                w.u8(v.wants_backups ? 1 : 0);
            }
            send_all(w.out);
            watchers_told = n, watchers_sent = now, watching_count = n;
        }
    }
    // Every channel to a spectator, after the players' own (the run loop's last), within spectator_cap: the host's to
    // each spectator, a player's backups to each on one board.
    void pump_spectators(Clock::time_point now) {
        guard_spectator_cap(now);
        if (is_host)
            for (Watcher& v : watchers)
                if (v.present) pump_reliable(0, v, &spectator_cap);
        if (!is_host)
            for (Bulk& b : bulk)
                if (b.present) pump_reliable(0, b, &spectator_cap);
    }
    // A player: the host's spectators as it last told them (taken over if this machine comes to host the room).
    struct KnownWatcher {
        uint8_t id = 0;
        uint64_t player = 0;
        std::string name, country;
        std::vector<Address> where;
        bool wants_backups = false;
    };
    std::vector<KnownWatcher> known_watchers;
    void on_watchers(Reader& r) {
        const uint8_t n = r.u8();
        std::vector<KnownWatcher> list;
        for (uint8_t k = 0; k < n && r.ok(); ++k) {
            KnownWatcher v;
            v.id = r.u8(), v.player = r.u64(), v.name = r.str(), v.country = r.str();
            v.where = get_addresses(r);
            v.wants_backups = r.u8() != 0;
            if (r.ok() && v.id < kMaxWatchers) list.push_back(std::move(v));
        }
        if (!r.ok()) return;
        watching_count = n;
        known_watchers = std::move(list);
        // (a channel to each spectator on one board, kept while it's the same one; the others let go)
        for (uint8_t i = 0; i < kMaxWatchers; ++i) {
            const auto it = std::find_if(known_watchers.begin(), known_watchers.end(),
                                         [&](const KnownWatcher& v) { return v.id == i && v.wants_backups; });
            Bulk& b = bulk[i];
            if (it == known_watchers.end()) {
                b = Bulk{};
                continue;
            }
            if (b.present && b.player == it->player) continue;
            b = Bulk{};
            b.present = true, b.player = it->player, b.candidates = it->where, b.heard = Clock::now();
            b.address = it->where.empty() ? Address{} : it->where.front();
            for (const Address& a : it->where) permit(a);
        }
    }
    // A player (not the host): a datagram from a spectator on one board (its acknowledgements, a whole backup asked for).
    void on_bulk(uint8_t id, const UdpSocket::Datagram& d, Reader& r, uint8_t type) {
        Bulk& b = bulk[id];
        if (!b.present) return;
        b.address = d.from, b.reached = true, b.heard = Clock::now();
        if (type != kReliable) return;
        const uint32_t ack = r.u32();
        const uint64_t sack = r.u64();
        const uint8_t has = r.u8();
        if (!r.ok()) return;
        b.rel.acked(ack, sack);
        if (!has) return;
        const uint32_t seq = r.u32();
        const uint8_t flags = r.u8();
        if (!r.ok()) return;
        std::vector<uint8_t> data(r.here(), r.here() + r.left());
        for (auto& m : b.rel.receive(seq, flags, std::move(data)))
            if (!m.empty() && m[0] == kWantBackup && cb.backup_wanted) cb.backup_wanted(static_cast<uint8_t>(kWatcherBase + id));
    }
    // This machine hosts the room now (the host left a game in progress): its spectators are ours, greeted again
    // (they run on from where they are: inputs from their next acknowledgement).
    void adopt_watchers() {
        for (const KnownWatcher& k : known_watchers) {
            Watcher& v = watchers[k.id];
            v = Watcher{};
            v.present = true, v.player = k.player, v.name = k.name, v.country = k.country;
            v.address = k.where.empty() ? Address{} : k.where.front();
            v.candidates = k.where;
            v.reached = false; // (to every address it may be at, until it answers)
            v.heard = Clock::now();
            v.acks.fill(UINT64_MAX); // (nothing sent until it says how far it is)
            for (const Address& a : k.where) permit(a);
            Writer w;
            w.u8(kWatchWelcome), w.u8(k.id), w.u8(seats), w.u8(1);
            v.rel.queue(w.out);
            log(k.name + " goes on watching, through this machine");
        }
        known_watchers.clear();
    }

    static void put_addresses(Writer& w, const std::vector<Address>& list) {
        const std::size_t n = std::min<std::size_t>(list.size(), 16);
        w.u8(static_cast<uint8_t>(n));
        for (std::size_t i = 0; i < n; ++i) w.u32(list[i].ip), w.u16(list[i].port);
    }
    static std::vector<Address> get_addresses(Reader& r) {
        std::vector<Address> out;
        const uint8_t n = r.u8();
        for (uint8_t i = 0; i < n && r.ok(); ++i) {
            const uint32_t ip = r.u32();
            const uint16_t port = r.u16();
            if (r.ok()) out.push_back({ip, port});
        }
        return out;
    }

    // The host's list of seats: each player's name and where it may be reached.
    std::vector<uint8_t> roster_message() const {
        Writer w;
        w.u8(kRoster), w.u8(seats);
        for (uint8_t s = 0; s < seats; ++s) {
            const bool present = s == my_seat || peers[s].present;
            w.u8(present ? 1 : 0);
            w.str(s == my_seat ? my_name : peers[s].name);
            w.str(s == my_seat ? my_country : peers[s].country);
            std::vector<Address> where;
            if (s != my_seat && present) {
                where.push_back(Address{peers[s].address.ip, peers[s].address.port}); // (as we see it)
                for (const Address& a : peers[s].candidates)
                    if (!a.same_place(where.front())) where.push_back(a);
            }
            put_addresses(w, where);
        }
        return w.out;
    }

    // ---- Receiving ----

    void received(const UdpSocket::Datagram& d) {
        // STUN and TURN: our router's address, and what the relay passes on.
        if (stun::is_stun(d.data)) {
            {
                static const bool debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
                static int shown = 0;
                if (debug && shown < 40) {
                    ++shown;
                    const auto m = stun::parse(d.data);
                    std::fprintf(stderr, "stun in from %s (relay server %s): type %04x, %zu bytes\n", d.from.str().c_str(),
                                 turn ? turn->server().str().c_str() : "none", m ? m->type : 0u, d.data.size());
                }
            }
            if (turn && d.from.same_place(turn->server())) {
                if (auto inner = turn->received(d.data, turn_send())) received(*inner);
                return;
            }
            if (const auto m = stun::parse(d.data); m && m->type == stun::kBindingSuccess && m->id == stun_id)
                if (const auto a = m->xor_address(stun::kXorMappedAddress)) {
                    if (!public_address) log("our address on the internet: " + a->str());
                    public_address = a;
                }
            return;
        }
        static const bool relay_only = std::getenv("SPIKEOUT_TEST_RELAY_ONLY") != nullptr;
        if (relay_only && !d.from.relayed) return; // (tests: through our relay only, see my_candidates)
        Reader r(d.data.data(), d.data.size());
        if (r.u8() != 'S' || r.u8() != 'K' || r.u8() != kProtocol) return;
        const uint8_t type = r.u8();
        const uint64_t sess = r.u64();
        const uint8_t from = r.u8();
        if (!r.ok() || type == kBeacon || type == kPunch) return;
        if (type == kJoin) {
            if (is_host && sess == session) on_join(d.from, r);
            return;
        }
        if (type == kRefuse) {
            if (!is_host && sess == session && my_seat == kNoSeat) {
                log("the host refused: " + r.str());
                leaving = true;
            }
            return;
        }
        if (from >= kWatcherBase && from < kWatcherBase + kMaxWatchers) { // (a spectator's: the host's, or a backup channel's)
            if (sess != session) return;
            if (is_host) on_watcher(static_cast<uint8_t>(from - kWatcherBase), d, r, type);
            else on_bulk(static_cast<uint8_t>(from - kWatcherBase), d, r, type);
            return;
        }
        if (sess != session || from >= kMaxSeats || from == my_seat) return;
        // A spectator hears only from the room's host: the first, or one that greets it later (host migration).
        if (watch_mode && from != host_seat && !(type == kReliable && my_watch != kNoSeat)) return;
        Peer& p = peers[from];
        // A player dropped: what still comes is ignored (first: the host's
        // last goodbyes, coming after it was dropped, would otherwise bring it
        // back as present below, take charge of the empty cabinets from us
        // and leave its inputs to no one).
        if (p.dropped) return;
        if (!p.present) {
            // (the host, before its welcome: from any of its addresses; joining a
            // game whose host has moved on, the seat it hosts from is learnt then)
            if (is_host || (my_seat != kNoSeat && from != host_seat)) return;
            p.present = true, p.connected = true, p.address = d.from;
            p.candidates = host_candidates;
        }
        // Where it answers from is where to send (a router may move it);
        // straight to it rather than through our relay, once that works.
        const auto now = Clock::now();
        if (!p.reached || !d.from.relayed || now - p.direct_heard > std::chrono::seconds(1)) p.address = d.from;
        if (!d.from.relayed) p.direct_heard = now;
        p.reached = true;
        if (!said_bye[from] && !(watch_mode && from != host_seat)) p.heard = now; // (after a goodbye, what still comes isn't a sign of life: dropped at once)
        p.connected = true;
        if (type == kReliable) on_reliable(from, p, r);
        else if (type == kInput) on_inputs(from, p, r);
        else if (type == kGoodbye && !said_bye[from]) on_bye(from);
        else if (type == kUrgent) { // (a drop's message, as its own datagram: send_urgent)
            std::vector<uint8_t> m = r.bytes();
            if (r.ok() && !m.empty() && (m[0] == kSuspect || m[0] == kHave || m[0] == kIdle)) on_message(from, m);
        } else if (type == kNodeData && cb.node) { // (a node, as its own datagram: Session::send_node)
            const uint64_t seq = r.u64();
            std::vector<uint8_t> node = r.bytes();
            if (r.ok()) cb.node(from, seq, std::move(node));
        }
    }

    void on_join(const Address& from, Reader& r) {
        auto refuse = [&](const std::string& why) {
            Writer w = header(kRefuse, session, my_seat);
            w.str(why);
            send(from, w);
        };
        const uint64_t their_build = r.u64(), player = r.u64();
        const std::string name = r.str();
        std::vector<Address> where = get_addresses(r);
        if (!r.ok()) return;
        const bool watch = r.left() > 0 && r.u8() == 1;
        // (its country: as the lobby said, by where it is (from here, or an address it gives); none on a LAN)
        std::string country;
        for (const CountrySaid& c : countries_said)
            for (const Address& a : c.at)
                if (a.same_place(from) || std::any_of(where.begin(), where.end(), [&](const Address& b) { return a.same_place(b); }))
                    country = c.country;
        for (uint8_t s = 0; s < seats; ++s)
            if (s != my_seat && peers[s].present && peers[s].player == player) return; // (already in: our welcome is on its way)
        if (their_build != build) return refuse("a different game or build");
        if (watch) return on_watch_join(from, player, name, std::move(where), country);
        uint8_t seat = kNoSeat; // (a game in progress: a cabinet no player has)
        for (uint8_t s = 0; s < seats && seat == kNoSeat; ++s) // (the old host's too, once the room has moved on)
            if (s != my_seat && !peers[s].present && (!started || (cabinets[s].open && !cabinets[s].until))) seat = s;
        if (seat == kNoSeat) return refuse("the game is full");
        Peer& p = peers[seat];
        p = Peer{};
        said_bye[seat] = false, gone_name[seat].clear();
        p.present = p.connected = true;
        p.address = from, p.name = name, p.country = country, p.player = player, p.heard = Clock::now();
        p.reached = true, p.direct_heard = from.relayed ? Clock::time_point{} : p.heard;
        p.candidates = std::move(where);
        permit(from);
        for (const Address& a : p.candidates) permit(a);
        log(name + " joined, seat " + std::to_string(seat + 1) + (started ? " (the game in progress)" : ""));
        Writer w;
        w.u8(kWelcome), w.u8(seat), w.u8(seats), w.u8(started ? 1 : 0);
        send_reliable(seat, w.out);
        send_all(roster_message());
        if (started) { // the start as it was, then every board as it is now (Callbacks::state_wanted)
            start_info.names[seat] = name;
            send_reliable(seat, start_message(start_info));
            p.wants_state = true;
            if (cb.state_wanted) cb.state_wanted();
        }
    }

    std::vector<uint8_t> start_message(const StartInfo& info) const {
        Writer w;
        w.u8(kStart), w.u8(info.seats), w.u64(static_cast<uint64_t>(info.clock));
        for (uint8_t s = 0; s < info.seats; ++s) w.str(info.names[s]), w.bytes(info.eeprom[s]), w.bytes(info.backup[s]);
        for (uint8_t s = 0; s < info.seats; ++s) { // (the cabinets no player has, and from when)
            w.u8(cabinets[s].open ? 1 : 0), w.u64(cabinets[s].from);
        }
        return w.out;
    }

    // The authority (Seat): inputs of none for the cabinets no player has,
    // up to `upto` (as far as its own).
    void fill_open(uint64_t upto) {
        if (authority() != my_seat) return;
        const InputBytes none{0xFF, 0xFF, 0xFF, 0xFF};
        for (uint8_t s = 0; s < seats; ++s) {
            Seat& c = cabinets[s];
            if (s == my_seat || !c.open) continue;
            Stream& st = streams[s];
            if (!st.known) {
                if (c.from > 0 || !streams[my_seat].known) continue; // (its frames before `from` still to come)
                st.known = true, st.first = streams[my_seat].first;
            }
            const uint64_t last = c.until ? std::min(upto, *c.until - 1) : upto;
            while (st.next() >= c.from && st.next() <= last && (!c.until || st.next() < *c.until)) {
                const uint64_t f = st.next();
                st.frames.push_back(none);
                if (cb.input) cb.input(s, f, none);
            }
            if (c.until && st.next() >= *c.until) c.open = false; // (the player's from here)
        }
    }

    // (as far as our own inputs go)
    void fill_open_to_mine() {
        const Stream& mine = streams[my_seat];
        if (my_seat < kMaxSeats && mine.known && mine.next() > 0) fill_open(mine.next() - 1);
    }

    // A late joiner's (or our) inputs, held until the frames before them are in.
    void flush_pending() {
        Stream& st = streams[my_seat];
        while (!pending_local.empty() && mine_from && st.known && st.next() >= *mine_from) {
            const auto [frame, in] = *pending_local.begin();
            pending_local.erase(pending_local.begin());
            append_local(frame, in);
        }
    }
    void append_local(uint64_t frame, const InputBytes& in) {
        Stream& st = streams[my_seat];
        if (!st.known) st.known = true, st.first = frame;
        if (frame < st.next()) return;
        while (st.next() < frame) { // (frames skipped hold the last)
            const uint64_t f = st.next();
            const InputBytes held = st.frames.empty() ? InputBytes{0xFF, 0xFF, 0xFF, 0xFF} : st.frames.back();
            st.frames.push_back(held);
            if (cb.input) cb.input(my_seat, f, held);
        }
        st.frames.push_back(in);
        if (send_hold) set_at.resize(st.frames.size(), Clock::now());
        if (cb.input) cb.input(my_seat, frame, in);
    }

    void on_reliable(uint8_t from, Peer& p, Reader& r) {
        const uint32_t ack = r.u32();
        const uint64_t sack = r.u64();
        const uint8_t has = r.u8();
        if (!r.ok()) return;
        p.rel.acked(ack, sack);
        if (!has) return;
        const uint32_t seq = r.u32();
        const uint8_t flags = r.u8();
        if (!r.ok()) return;
        std::vector<uint8_t> data(r.here(), r.here() + r.left());
        for (auto& m : p.rel.receive(seq, flags, std::move(data))) on_message(from, m);
    }

    // A player's goodbye (kBye in the reliable channel, or its kGoodbye datagram, whichever comes first).
    void on_bye(uint8_t from) {
        // (as if silent for long: dropped next; a spectator waits from now for a new host to greet it)
        peers[from].heard = watch_mode ? Clock::now() : Clock::time_point{};
        if (watch_mode && from == host_seat) deliver_watched(true); // (every frame complete so far: nothing more may come)
        said_bye[from] = true;
        log(peers[from].name + " left");
        if (!started && !is_host && from == host_seat && !room_closed) { // (before the start: the room goes with it)
            room_closed = true;
            log("the host closed the room");
        }
    }

    void on_message(uint8_t from, const std::vector<uint8_t>& m) {
        Reader r(m.data(), m.size());
        switch (r.u8()) {
        case kWelcome:
            if (my_seat == kNoSeat && !is_host) {
                host_seat = from; // (0, or whoever hosts the room now)
                my_seat = r.u8();
                seats = r.u8();
                late = r.u8() != 0;
                log("joined " + game_name + ", seat " + std::to_string(my_seat + 1) + (late ? " (the game in progress)" : ""));
                Writer c; // our cabinet, for every machine to start our board from
                c.u8(kCabinet), c.bytes(my_eeprom), c.bytes(my_backup);
                send_reliable(host_seat, c.out);
            }
            break;
        case kStateSize: {
            const uint32_t size = r.u32();
            if (r.ok()) state_size = size;
            break;
        }
        case kWatchWelcome: // (a spectator let in; or greeted by the room's new host, the old one gone)
            if (watch_mode && !is_host) {
                const uint8_t id = r.u8(), n = r.u8();
                const bool on = r.u8() != 0;
                if (!r.ok() || id >= kMaxWatchers) break;
                if (my_watch == kNoSeat) {
                    my_watch = id, seats = n, late = on, host_seat = from;
                    log("watching " + game_name + (on ? " (the game in progress)" : ""));
                } else if (from != host_seat) {
                    log("the host left: watching through " + (peers[from].name.empty() ? "seat " + std::to_string(from + 1) : peers[from].name));
                    peers[host_seat] = Peer{};
                    host_seat = from, my_watch = id;
                }
                peers[from].present = peers[from].connected = true;
                watch_acked = Clock::time_point{}; // (how far we are, at once)
            }
            break;
        case kWatchInfo:
            if (watch_mode && from == host_seat) on_watch_info(r);
            break;
        case kWatchers:
            if (!watch_mode && from == host_seat) on_watchers(r);
            break;
        case kRoster: // (a game in progress too: a player joining)
            if (from == host_seat && !is_host) {
                const uint8_t n = r.u8();
                for (uint8_t s = 0; s < n && s < kMaxSeats && r.ok(); ++s) {
                    const bool present = r.u8() != 0;
                    const std::string name = r.str(), country = r.str();
                    std::vector<Address> where = get_addresses(r);
                    if (s == my_seat || s == host_seat) {
                        if (s == host_seat) peers[host_seat].name = name, peers[host_seat].country = country;
                        continue;
                    }
                    Peer& p = peers[s];
                    if (p.dropped && !(present && cabinets[s].open)) continue;
                    if (p.dropped) p = Peer{}; // (a new player in a cabinet whose player dropped)
                    const bool was = p.present;
                    p.present = present && !where.empty(), p.name = name, p.country = country;
                    if (p.present && !p.reached) p.address = where.front();
                    if (started && p.present && !was) p.connected = true, p.heard = Clock::now(); // (joined in progress)
                    if (p.present && !was) said_bye[s] = false, gone_name[s].clear();
                    for (const Address& a : where) permit(a);
                    p.candidates = std::move(where);
                }
            }
            break;
        case kStart:
            if (from == host_seat && !is_host && !started) {
                StartInfo info;
                info.seats = r.u8();
                info.clock = static_cast<int64_t>(r.u64());
                for (uint8_t s = 0; s < info.seats; ++s) {
                    info.names.push_back(r.str());
                    info.eeprom.push_back(r.bytes());
                    info.backup.push_back(r.bytes());
                }
                for (uint8_t s = 0; s < info.seats && s < kMaxSeats; ++s) {
                    cabinets[s].open = r.u8() != 0;
                    cabinets[s].from = r.u64();
                }
                if (!r.ok()) break;
                info.my_seat = my_seat;
                seats = info.seats;
                for (uint8_t s = 0; s < seats && s < kMaxSeats; ++s) // (a spectator has no roster: the names from here)
                    if (peers[s].name.empty()) peers[s].name = info.names[s];
                const auto now = Clock::now();
                for (uint8_t s = 0; s < seats; ++s)
                    if (s != my_seat && peers[s].present) peers[s].connected = true, peers[s].heard = now;
                if (late) late_start = info; // (and every board's state next)
                else begin(info);
            }
            break;
        case kState: { // every board as the host has them: a game joined in progress, or ours put back in step
            const uint64_t frame = r.u64();
            std::vector<uint8_t> state = r.bytes();
            if (from != authority() && from != host_seat) break;
            if (!r.ok()) break;
            want_state = false;
            state_at = frame;
            my_hashes.clear(), their_hashes.clear();
            if (late && !started && late_start) {
                StartInfo info = *late_start;
                info.state_frame = frame, info.state = std::move(state);
                log("every board's state from the host: frame " + std::to_string(frame));
                begin(info);
            } else if (started && cb.resync) {
                StartInfo info = start_info;
                info.state_frame = frame, info.state = std::move(state);
                log("put back in step with the host's boards at frame " + std::to_string(frame));
                cb.resync(info);
            }
            break;
        }
        case kWantState: // (out of step: our boards' state wanted)
            peers[from].wants_state = true;
            if (cb.state_wanted) cb.state_wanted();
            break;
        case kWantBoard: { // (one board a PC: our own board's state at the end of a frame, for the asker's shadow of it)
            const uint64_t frame = r.u64();
            if (r.ok() && from < seats && cb.board_wanted) cb.board_wanted(from, frame);
            break;
        }
        case kStatus: { // (its machine and how it plays: the overlay's)
            Session::Status s;
            s.platform = static_cast<Session::Status::Platform>(std::min<uint8_t>(r.u8(), 4));
            s.boards = static_cast<Session::Status::Boards>(std::min<uint8_t>(r.u8(), 2));
            s.net = static_cast<Session::Status::Net>(std::min<uint8_t>(r.u8(), 2));
            s.delay = static_cast<int16_t>(r.u16());
            s.auto_delay = r.u8() != 0;
            if (r.ok() && r.left() > 0) s.catch_up = static_cast<int8_t>(std::min<int>(r.u8(), 101) - 1);
            if (r.ok() && from < seats) peers[from].status = s;
            break;
        }
        case kParting: { // (it leaves after this frame, its board's state at its end to come: a shadow of it from it)
            const uint64_t frame = r.u64();
            if (r.ok() && from < seats && cb.parting) cb.parting(from, frame);
            break;
        }
        case kBoard: { // (its own board's state, as we asked)
            const uint64_t frame = r.u64();
            std::vector<uint8_t> state = r.bytes();
            if (r.ok() && from < seats && cb.board) cb.board(from, frame, std::move(state));
            break;
        }
        case kBackup: { // (its own board's backup: the pages changed since its last, or whole)
            const uint64_t frame = r.u64();
            std::vector<uint8_t> backup = r.bytes();
            if (r.ok() && from < seats && cb.backup) cb.backup(from, frame, std::move(backup));
            break;
        }
        case kNode: { // (its own board's part of a link packet: tools::NodeCheck)
            const uint64_t seq = r.u64();
            std::vector<uint8_t> node = r.bytes();
            if (r.ok() && from < seats && cb.node) cb.node(from, seq, std::move(node));
            break;
        }
        case kWantBackup: // (ours whole next: it hasn't what the changes are from)
            if (from < seats && cb.backup_wanted) cb.backup_wanted(from);
            break;
        case kTake: { // (the authority) a player joined in progress takes its cabinet
            if (authority() != my_seat || from >= seats) break; // (it asks the authority again)
            if (cabinets[from].until) { // already handed over (asked again: the answer lost with a host that left)
                Writer w;
                w.u8(kTaken), w.u8(from), w.u64(*cabinets[from].until);
                send_reliable(from, w.out);
                break;
            }
            if (!cabinets[from].open) break;
            const uint64_t x = streams[from].next() + kTakeMargin;
            Writer w;
            w.u8(kTaken), w.u8(from), w.u64(x);
            send_all(w.out);
            taken(from, x);
            break;
        }
        case kTaken: {
            const uint8_t s = r.u8();
            const uint64_t x = r.u64();
            if (r.ok() && s < seats) taken(s, x);
            break;
        }
        case kSuspect: {
            const uint8_t s = r.u8();
            if (!r.ok() || s >= seats || s == my_seat) break;
            Drop& d = drops[s];
            d.suspected = true;
            Writer w;
            w.u8(kHave), w.u8(s), w.u64(streams[s].next());
            send_urgent(from, w.out);
            break;
        }
        case kHave: {
            const uint8_t s = r.u8();
            const uint64_t next = r.u64();
            if (r.ok() && s < seats) drops[s].answers[from] = next;
            break;
        }
        case kIdle: {
            const uint8_t s = r.u8();
            const uint64_t frame = r.u64();
            if (r.ok() && s < seats && s != my_seat) set_idle(s, frame);
            break;
        }
        case kHash: {
            const uint64_t frame = r.u64(), hash = r.u64();
            if (!r.ok()) break;
            if (auto it = my_hashes.find(frame); it != my_hashes.end()) {
                if (it->second != hash) {
                    if (cb.desync) cb.desync(from, frame);
                    out_of_step(from, frame);
                }
            } else {
                their_hashes[{frame, from}] = hash;
            }
            break;
        }
        case kCabinet:
            if (is_host && !started && from < seats && peers[from].present) {
                Peer& p = peers[from];
                p.eeprom = r.bytes();
                p.backup = r.bytes();
                p.cabinet = r.ok();
            }
            break;
        case kBye:
            if (!said_bye[from]) on_bye(from); // (its datagram may have come first)
            break;
        default: break;
        }
    }

    void on_inputs(uint8_t from, Peer& p, Reader& r) {
        const uint32_t their_time = r.u32(), echo = r.u32();
        const uint16_t held = r.u16();
        const uint8_t n = r.u8();
        if (!r.ok()) return;
        p.their_time = their_time, p.their_time_at = Clock::now();
        if (echo) p.rtt = static_cast<int>(now_ms() - echo) - held;
        for (uint8_t s = 0; s < n && s < kMaxSeats; ++s) p.acks[s] = r.u64();
        const uint8_t chunks = r.u8();
        for (uint8_t c = 0; c < chunks && r.ok(); ++c) {
            const uint8_t s = r.u8();
            const uint64_t first = r.u64();
            const uint8_t count = r.u8();
            if (!r.ok() || s >= seats || count * 4u > r.left()) return;
            const uint8_t* data = r.here();
            r.skip(count * 4u);
            if (watch_mode) { // (a spectator: kept, given out kWatchLag frames later: deliver_watched)
                Stream& st = streams[s];
                if (!st.known) st.known = true, st.first = first;
                for (uint8_t k = 0; k < count; ++k)
                    if (first + k == st.next()) {
                        InputBytes in;
                        std::memcpy(in.data(), data + 4 * k, 4);
                        st.frames.push_back(in);
                    }
                continue;
            }
            // Our own: only a cabinet taken in progress, its frames before it was ours (none).
            if (s == my_seat && !late) continue;
            if (s == from && drops[s].suspected) continue; // (frozen while its drop is settled)
            Stream& st = streams[s];
            if (!st.known) st.known = true, st.first = first;
            for (uint8_t k = 0; k < count; ++k) {
                const uint64_t f = first + k;
                if (f != st.next()) continue;
                if (s == my_seat && mine_from && f >= *mine_from) break;
                InputBytes in;
                std::memcpy(in.data(), data + 4 * k, 4);
                st.frames.push_back(in);
                if (cb.input) cb.input(s, f, in);
            }
            if (s == my_seat) flush_pending();
        }
        if (watch_mode) return deliver_watched();
        fill_open_to_mine(); // (a new authority's, catching up)
        // Anything new to pass on goes out soon; our acks too.
        for (uint8_t s = 0; s < kMaxSeats; ++s)
            if (peers[s].present) peers[s].inputs_due = true;
    }

    // ---- Drops ----

    // A player dropped: its cabinet no player's from `frame` (its inputs
    // none from there, made by the authority: Seat), free to be taken again.
    void set_idle(uint8_t s, uint64_t frame) {
        if (peers[s].dropped) return;
        peers[s].connected = false, peers[s].dropped = true;
        drops[s].suspected = true;
        cabinets[s] = Seat{true, frame, std::nullopt};
        log((peers[s].name.empty() ? "seat " + std::to_string(s + 1) : peers[s].name) + " dropped: idle from frame " +
            std::to_string(frame));
        if (cb.idle) cb.idle(s, frame);
        // The seat free again (a new player may join it): this player is gone.
        Peer& p = peers[s];
        gone_name[s] = p.name, gone_country[s] = p.country;
        p = Peer{};
        p.dropped = true; // (its datagrams, if any still come, ignored; a new player resets it)
        drops[s] = Drop{};
        fill_open_to_mine();
        // The host gone from a game in progress: the room goes on, hosted by
        // the lowest seat still connected (the authority, the same on every
        // machine): it lets players in (welcome, roster, the start, the
        // boards' state), and is found on the networks (beacons) and the lobby
        // (the frontend, Session::hosting).
        if (started && s == host_seat) {
            host_seat = authority();
            if (host_seat == my_seat) {
                is_host = true;
                targets_asked = Clock::time_point{}; // (the networks to announce it on, at once)
                adopt_watchers();
                log("the host left: this machine hosts the room now (port " + std::to_string(socket.port()) + ")");
            } else {
                log("the host left: " + (peers[host_seat].name.empty() ? "seat " + std::to_string(host_seat + 1) : peers[host_seat].name) +
                    " hosts the room now");
            }
        }
    }

    // A cabinet a player has from frame `x` (taken in a game in progress).
    void taken(uint8_t s, uint64_t x) {
        if (cabinets[s].until) return;
        cabinets[s].until = x;
        log("seat " + std::to_string(s + 1) + " taken from frame " + std::to_string(x));
        if (s == my_seat) {
            mine_from = x;
            take_wanted = false;
            pending_local.erase(pending_local.begin(), pending_local.lower_bound(x)); // (kept before it was known: not ours)
            flush_pending();
        }
        fill_open_to_mine();
        if (cb.taken) cb.taken(s, x);
    }

    // Our cabinet, asked of the authority (not ourselves: one moment with
    // no one else connected; asked again once someone is).
    void ask_take(Clock::time_point now) {
        const uint8_t a = authority();
        take_asked = a, take_sent = now;
        if (a == my_seat || a >= seats) return;
        Writer w;
        w.u8(kTake);
        send_reliable(a, w.out);
    }

    // Our boards differ from the authority's: ask it for its state.
    void out_of_step(uint8_t from, uint64_t key) {
        if (is_host || from != authority() || want_state || !started) return;
        if (state_at && key / kMaxSeats <= *state_at) return; // (before the state we run on from)
        want_state = true;
        log("out of step with " + (peers[from].name.empty() ? "the host" : peers[from].name) + ": asking for its boards");
        Writer w;
        w.u8(kWantState);
        send_reliable(from, w.out);
    }

    // Who makes the empty cabinets' inputs, settles drops and hands cabinets
    // over: the room's host while it's there; else the lowest seat still
    // connected, leaving out a player who joined the game in progress and
    // hasn't its cabinet yet (it isn't running the game with the others).
    uint8_t authority() const {
        auto connected = [&](uint8_t s) {
            return s == my_seat || (peers[s].present && peers[s].connected && !drops[s].suspected);
        };
        if (host_seat < seats && connected(host_seat)) return host_seat;
        for (uint8_t s = 0; s < seats; ++s)
            if (connected(s) && !(cabinets[s].open && !cabinets[s].until)) return s;
        return my_seat;
    }

    void check_drops() {
        const auto now = Clock::now();
        for (uint8_t s = 0; s < seats; ++s) { // (quiet or heard again: told as it changes)
            Peer& p = peers[s];
            const bool quiet = s != my_seat && p.present && !p.dropped && now - p.heard > kQuietAfter;
            if (quiet != quiet_told[s]) {
                quiet_told[s] = quiet;
                if (cb.quiet) cb.quiet(s, quiet);
            }
        }
        for (uint8_t s = 0; s < seats; ++s) {
            Peer& p = peers[s];
            if (s == my_seat || !p.present || p.dropped) continue;
            // (a joiner still taking in every board's state: a slow line's or relay's acknowledgements can lag)
            // (heard: any datagram from it, its session's thread's, which goes on while its game stalls; a game going
            // on: a second of silence)
            const auto quiet = state_left(p) > 0 ? kDropAfter * 5 : started ? kDropInGame : kDropAfter;
            if (now - p.heard > quiet && !drops[s].suspected) {
                drops[s].suspected = true; // (its inputs frozen here)
                const auto ms = [&](Clock::time_point t) { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count()); };
                log((p.name.empty() ? "seat " + std::to_string(s + 1) : p.name) + " isn't answering (heard " + ms(p.heard) +
                    " ms ago; our inputs to it " + ms(p.inputs_sent) + " ms ago, " + std::to_string(p.rel.out.size()) +
                    " reliable pieces waiting; " + std::to_string(datagrams_in) + " datagrams in, " + std::to_string(send_failures) +
                    " sends failed, in all)");
            }
        }
        if (authority() != my_seat) return;
        for (uint8_t s = 0; s < seats; ++s) {
            Drop& d = drops[s];
            if (s == my_seat || !peers[s].present || !d.suspected || d.announced) continue;
            if (d.asked_by != my_seat) { // ask the others how far they have its inputs
                d.asked_by = my_seat;
                d.answers.clear();
                Writer w;
                w.u8(kSuspect), w.u8(s);
                send_all_urgent(w.out, s);
            }
            uint64_t x = streams[s].next();
            bool all = true;
            for (uint8_t o = 0; o < seats; ++o) {
                if (o == my_seat || o == s || !peers[o].present || peers[o].dropped || drops[o].suspected) continue;
                const auto it = d.answers.find(o);
                if (it == d.answers.end()) all = false;
                else x = std::max(x, it->second);
            }
            if (!all || streams[s].next() < x) continue; // (still to hear, or the frames still to come)
            d.announced = true;
            Writer w;
            w.u8(kIdle), w.u8(s), w.u64(x);
            send_all_urgent(w.out, s);
            set_idle(s, x);
        }
    }

    // ---- The loop ----

    void begin(const StartInfo& info) {
        start_info = info;
        started = true;
        owner.started_ = true;
        log("the game starts: " + std::to_string(info.seats) + " cabinets" + (info.state_frame ? " (joined in progress)" : ""));
        if (cb.started) cb.started(info);
    }

    void run() {
        while (!stop) {
            socket.wait(1);
            // The networks this machine is on, asked of the system now and then,
            // outside the lock (it takes milliseconds: a board would wait for it).
            if (is_host && Clock::now() - targets_asked > std::chrono::seconds(5)) {
                auto targets = broadcast_addresses(kDiscoveryPort);
                std::lock_guard guard(lock);
                beacon_targets = std::move(targets);
                targets_asked = Clock::now();
            }
            std::lock_guard guard(lock);
            while (auto d = socket.receive()) ++datagrams_in, received(*d);
            const auto now = Clock::now();
            send_held(now); // (tests: SPIKEOUT_TEST_LATENCY's, due)
            if (is_host && now - last_beacon > kBeaconEvery) beacon(now);
            if (joining && my_seat == kNoSeat && my_watch == kNoSeat && !leaving && now - last_join > kJoinRetry) {
                last_join = now; // (to every address the host may be at: the first to answer is where it is)
                Writer w = header(kJoin, session, kNoSeat);
                w.u64(build), w.u64(player_id()), w.str(my_name);
                put_addresses(w, my_candidates());
                w.u8(watch_mode ? 1 : 0);
                send(host_address, w);
                for (const Address& a : host_candidates)
                    if (!a.same_place(host_address)) send(a, w);
            }
            // Routers opened to players joining, our address asked of STUN, the relay kept.
            if (!punches.empty() && now - last_punch > std::chrono::milliseconds(200)) {
                last_punch = now;
                std::erase_if(punches, [&](const Punch& p) { return now > p.until; });
                const Writer w = header(kPunch, session, my_seat);
                for (const Punch& p : punches)
                    for (const Address& a : p.to) send(a, w);
            }
            if (stun_server && !public_address && stun_tries < 10 && now - stun_sent > std::chrono::milliseconds(300)) {
                ++stun_tries;
                stun_sent = now;
                stun_id = stun::new_transaction();
                socket.send_to(*stun_server, stun::binding_request(stun_id));
            }
            if (turn) turn->tick(turn_send());
            if (watch_mode) { // (a spectator: the host only; gone quiet, or gone and no new host greeting us: it ends)
                if (my_watch != kNoSeat && !leaving) {
                    Peer& h = peers[host_seat];
                    if (now - watch_acked >= std::chrono::milliseconds(50)) send_watch_acks();
                    pump_reliable(host_seat, h);
                    if (watch_one_board) watch_punch(now);
                    if (!room_closed && now - h.heard > kDropAfter + std::chrono::seconds(7)) {
                        room_closed = true;
                        log("the host isn't answering: watching ended");
                    }
                } else if (leaving && my_watch != kNoSeat) {
                    pump_reliable(host_seat, peers[host_seat]);
                }
                continue;
            }
            if (is_host && !leaving) serve_watchers(now);
            if (started && !leaving) check_drops();
            if (my_seat != kNoSeat && !leaving && my_status != Session::Status{} && now - status_sent > kStatusEvery) send_status();
            if (take_wanted && !mine_from && !leaving && (authority() != take_asked || now - take_sent > kTakeRetry))
                ask_take(now);
            if (!started && !is_host && my_seat != kNoSeat && !room_closed && peers[host_seat].present &&
                now - peers[host_seat].heard > kDropAfter) { // (before the start, a host gone quiet: the room is gone)
                room_closed = true;
                log("the host isn't answering: the room is closed");
            }
            if (is_host && !started) // before the start, a player gone quiet frees the seat
                for (uint8_t s = 1; s < seats; ++s)
                    if (peers[s].present && now - peers[s].heard > kDropAfter) {
                        log(peers[s].name + " left");
                        peers[s] = Peer{};
                        send_all(roster_message());
                    }
            for (uint8_t s = 0; s < kMaxSeats; ++s) {
                Peer& p = peers[s];
                if (s == my_seat || !p.present || p.dropped) continue;
                if (leaving) { // (gone: only the goodbye still to get through)
                    pump_reliable(s, p);
                    continue;
                }
                if ((p.inputs_due || inputs_pending(s, p)) && now - p.inputs_sent >= kInputResend) send_inputs(s, p);
                else if (now - p.inputs_sent >= kKeepAlive) send_inputs(s, p);
                pace(p, now);
                pump_reliable(s, p, &p.pace);
            }
            if (!is_host && !leaving) pump_spectators(now); // (a player's backups to spectators on one board: last)
            if (!is_host && !started && my_seat == kNoSeat && leaving) break;
        }
    }

    std::vector<Address> my_candidates() const {
        std::vector<Address> out;
        // Tests: SPIKEOUT_TEST_RELAY_ONLY=1 offers only our relay address
        // (two machines on one network would otherwise never use it).
        static const bool relay_only = std::getenv("SPIKEOUT_TEST_RELAY_ONLY") != nullptr;
        if (relay_only) {
            if (turn && turn->relayed()) out.push_back(*turn->relayed());
            return out;
        }
        for (uint32_t ip : local_addresses()) out.push_back({ip, socket.port()});
        if (public_address) out.push_back(*public_address);
        if (turn && turn->relayed()) out.push_back(*turn->relayed());
        return out;
    }

    void beacon(Clock::time_point now) {
        last_beacon = now;
        Writer w = header(kBeacon, session, my_seat);
        uint8_t taken = 1; // (ours, and the players present)
        for (uint8_t s = 0; s < seats; ++s) taken = static_cast<uint8_t>(taken + (s != my_seat && peers[s].present ? 1 : 0));
        w.u64(build), w.str(game_name), w.u8(seats), w.u8(taken), w.u8(started ? 1 : 0), w.u16(socket.port());
        w.u8(static_cast<uint8_t>(watcher_count()));
        for (const Address& a : beacon_targets) send(a, w);
    }

    // This session's player, told to the host with each join (a join asked
    // again isn't taken for another player). One a session, not a process:
    // tests run several players in one.
    uint64_t player_id() { return my_player; }
    const uint64_t my_player = std::random_device{}() * 0x9E3779B97F4A7C15ull ^ std::random_device{}() ^
                            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    bool open() {
        if (socket.is_open()) return true; // (opened to find our addresses first: Session::listen)
        if (!socket.open(0)) return false;
        thread = std::thread([this] { run(); });
        return true;
    }
};

// ---- Discovery ----

bool Discovery::open() { return socket_.open(kDiscoveryPort, true); }

std::vector<GameInfo> Discovery::poll() {
    const auto now = Clock::now();
    while (auto d = socket_.receive()) {
        Reader r(d->data.data(), d->data.size());
        if (r.u8() != 'S' || r.u8() != 'K' || r.u8() != kProtocol || r.u8() != kBeacon) continue;
        GameInfo g;
        g.session = r.u64();
        r.u8(); // (seat)
        g.build = r.u64();
        g.name = r.str();
        g.seats = r.u8(), g.taken = r.u8(), g.started = r.u8() != 0;
        const uint16_t port = r.u16();
        if (!r.ok()) continue;
        g.watchers = r.left() ? r.u8() : 0;
        g.host = {d->from.ip, port};
        // The same game heard on several networks (this machine's loopback
        // and its LAN): keep one address, preferring loopback.
        auto it = games_.find(g.session);
        if (it != games_.end() && it->second.info.host.loopback() && !g.host.loopback()) g.host = it->second.info.host;
        games_[g.session] = {g, now};
    }
    std::vector<GameInfo> out;
    for (auto it = games_.begin(); it != games_.end();) {
        if (now - it->second.at > std::chrono::seconds(3)) {
            it = games_.erase(it);
        } else {
            out.push_back(it->second.info);
            ++it;
        }
    }
    return out;
}

// ---- Session ----

Session::Session(Callbacks callbacks, uint64_t build) : impl_(std::make_unique<Impl>(*this, std::move(callbacks), build)) {}

Session::~Session() {
    leave();
    impl_->stop = true;
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool Session::host(const std::string& name, uint8_t seats) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    m.is_host = true;
    m.game_name = m.my_name = name;
    m.seats = std::clamp<uint8_t>(seats, 2, kMaxSeats);
    m.my_seat = 0;
    std::mt19937_64 rng(std::random_device{}() ^ static_cast<uint64_t>(Clock::now().time_since_epoch().count()));
    m.session = rng() | 1;
    if (!m.open()) {
        error_ = m.socket.error();
        return false;
    }
    return true;
}

bool Session::join(const GameInfo& game, const std::string& name) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (game.build != m.build) {
        error_ = "a different game or build";
        return false;
    }
    m.is_host = false;
    m.game_name = game.name;
    m.my_name = name;
    m.session = game.session;
    m.seats = game.seats;
    m.host_address = game.host.ip || game.candidates.empty() ? game.host : game.candidates.front();
    m.host_candidates = game.candidates;
    for (const Address& a : game.candidates) m.permit(a);
    if (!m.open()) {
        error_ = m.socket.error();
        return false;
    }
    m.joining = true;
    return true;
}

bool Session::listen() {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (!m.open()) {
        error_ = m.socket.error();
        return false;
    }
    return true;
}

uint16_t Session::port() const { return impl_->socket.port(); }

uint64_t Session::session_id() const {
    std::lock_guard guard(impl_->lock);
    return impl_->session;
}

std::vector<std::string> Session::players() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::vector<std::string> out;
    for (uint8_t s = 0; s < m.seats; ++s)
        out.push_back(s == m.my_seat ? m.my_name : m.peers[s].present ? m.peers[s].name : std::string());
    return out;
}

bool Session::all_seats_taken() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (m.my_seat == kNoSeat) return false;
    for (uint8_t s = 0; s < m.seats; ++s)
        if (s != m.my_seat && (!m.peers[s].present || (m.is_host && !m.peers[s].cabinet))) return false;
    return true;
}

std::size_t Session::ready_players() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::size_t n = 1;
    while (n < m.seats && m.peers[n].present && m.peers[n].cabinet) ++n;
    return n;
}

void Session::set_cabinet(std::vector<uint8_t> eeprom, std::vector<uint8_t> backup) {
    std::lock_guard guard(impl_->lock);
    impl_->my_eeprom = std::move(eeprom);
    impl_->my_backup = std::move(backup);
}

void Session::set_country(const std::string& country) {
    std::lock_guard guard(impl_->lock);
    impl_->my_country = two_letters(country);
}

void Session::set_status(const Status& status) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (status == m.my_status) return;
    m.my_status = status;
    if (m.my_seat != kNoSeat && !m.leaving) m.send_status(); // (changed: at once; else with the next repeat)
}

std::string Session::game_name() const {
    std::lock_guard guard(impl_->lock);
    return impl_->game_name;
}

void Session::start(StartInfo info) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (!m.is_host || m.started) return;
    // Every cabinet powered on: those with a player (its cabinet in) and
    // those standing empty (Seat: none of the inputs, until one is taken).
    info.seats = m.seats;
    info.my_seat = 0;
    info.names.clear(), info.eeprom.clear(), info.backup.clear();
    for (uint8_t s = 0; s < m.seats; ++s) {
        Impl::Peer& p = m.peers[s];
        const bool player = s == 0 || (p.present && p.cabinet);
        if (s != 0 && !player) p = Impl::Peer{};
        m.cabinets[s] = Impl::Seat{s != 0 && !player, 0, std::nullopt};
        info.names.push_back(s == 0 ? m.my_name : player ? p.name : std::string());
        info.eeprom.push_back(s == 0 ? m.my_eeprom : player ? p.eeprom : std::vector<uint8_t>());
        info.backup.push_back(s == 0 ? m.my_backup : player ? p.backup : std::vector<uint8_t>());
    }
    m.start_info = info;
    m.send_all(m.roster_message());
    m.send_all(m.start_message(info));
    for (Impl::Watcher& v : m.watchers) // (spectators in the room before the start: from power-on with the players)
        if (v.present) v.rel.queue(m.start_message(info));
    m.begin(info);
}

void Session::local_input(uint64_t frame, const InputBytes& in) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (m.my_seat == kNoSeat || !m.started) return;
    // A cabinet joined in progress: our inputs kept until the frame it's ours from is known (kTaken), since with an
    // input delay we make them before then and the first of them may be ours (thrown away, those frames were never
    // filled: every machine waited for them for good); the ones before it dropped then (Impl::taken).
    if (m.late && !m.mine_from) {
        m.pending_local[frame] = in;
        while (m.pending_local.size() > 1200) m.pending_local.erase(m.pending_local.begin()); // (a long wait: the latest)
        return;
    }
    if (m.late && frame < *m.mine_from) return; // (not ours: none, as the others have it)
    if (m.mine_from && (!m.streams[m.my_seat].known || m.streams[m.my_seat].next() < *m.mine_from)) {
        m.pending_local[frame] = in; // (the frames before it, none, still to come)
        return;
    }
    m.append_local(frame, in);
    m.fill_open(frame); // (the authority: the cabinets no player has, as far as ours)
    for (uint8_t s = 0; s < kMaxSeats; ++s) { // out at once
        Impl::Peer& p = m.peers[s];
        if (s != m.my_seat && p.present && !p.dropped) m.send_inputs(s, p);
    }
}

void Session::send_state(uint64_t frame, std::vector<uint8_t> state) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    Writer w;
    w.u8(kState), w.u64(frame), w.bytes(state);
    for (uint8_t s = 0; s < kMaxSeats; ++s) {
        Impl::Peer& p = m.peers[s];
        if (s == m.my_seat || !p.present || !p.wants_state) continue;
        p.wants_state = false;
        Writer size; // (its size first: the receiver's progress, Session::join_progress)
        size.u8(kStateSize), size.u32(static_cast<uint32_t>(w.out.size()));
        m.send_reliable(s, size.out);
        p.state_first = p.rel.next_seq;
        m.send_reliable(s, w.out);
        p.state_end = p.rel.next_seq, p.state_going = true;
        m.log("every board's state (frame " + std::to_string(frame) + ", " + std::to_string(state.size() / 1024) + " KB) to " +
              (p.name.empty() ? "seat " + std::to_string(s + 1) : p.name));
    }
    for (Impl::Watcher& v : m.watchers) { // (a spectator: its inputs from the frame after it)
        if (!v.present || !v.wants_state) continue;
        v.wants_state = false;
        v.acks.fill(frame + 1);
        v.rel.queue(w.out);
        m.log("every board's state (frame " + std::to_string(frame) + ", " + std::to_string(state.size() / 1024) + " KB) to " +
              v.name + ", watching");
    }
}

void Session::want_board(uint8_t seat, uint64_t frame) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (seat >= kMaxSeats || seat == m.my_seat || !m.peers[seat].present) return;
    Writer w;
    w.u8(kWantBoard), w.u64(frame);
    m.send_reliable(seat, w.out);
}

bool Session::sending() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    for (uint8_t s = 0; s < kMaxSeats; ++s)
        if (s != m.my_seat && m.peers[s].present && !m.peers[s].dropped && !m.peers[s].rel.out.empty()) return true;
    return false;
}

void Session::announce_parting(uint64_t frame) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    Writer w;
    w.u8(kParting), w.u64(frame);
    m.send_all(w.out);
}

void Session::send_board(uint8_t seat, uint64_t frame, std::vector<uint8_t> state) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (seat >= kMaxSeats || seat == m.my_seat || !m.peers[seat].present) return;
    Writer w;
    w.u8(kBoard), w.u64(frame), w.bytes(state);
    m.send_reliable(seat, w.out);
}

void Session::send_backup(uint8_t seat, uint64_t frame, std::vector<uint8_t> backup) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    Writer w;
    w.u8(kBackup), w.u64(frame), w.bytes(backup);
    if (seat >= kWatcherBase && seat < kWatcherBase + kMaxWatchers) { // (a spectator on one board: its channel, capped)
        const uint8_t id = static_cast<uint8_t>(seat - kWatcherBase);
        if (m.is_host && m.watchers[id].present) m.watchers[id].rel.queue(w.out);
        else if (!m.is_host && m.bulk[id].present) m.bulk[id].rel.queue(w.out);
        return;
    }
    if (seat >= kMaxSeats || seat == m.my_seat || !m.peers[seat].present || m.peers[seat].dropped) return;
    m.send_reliable(seat, w.out);
}

void Session::send_node(uint64_t seq, std::vector<uint8_t> node) {
    // (a datagram of its own, twice, not the reliable channel: there it waited behind a one-board backup's pieces, a
    // second and more, past what rollback can go back for; one lost leaves that packet unchecked)
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    Writer w = header(kNodeData, m.session, m.me());
    w.u64(seq), w.bytes(node);
    for (uint8_t s = 0; s < kMaxSeats; ++s)
        if (s != m.my_seat && m.peers[s].present && m.peers[s].connected && !m.peers[s].dropped)
            for (int k = 0; k < 2; ++k) m.send_peer(m.peers[s], w);
}

bool Session::seated() const {
    std::lock_guard guard(impl_->lock);
    return !impl_->late || impl_->mine_from.has_value();
}

void Session::hold_inputs(uint64_t frames) {
    std::lock_guard guard(impl_->lock);
    impl_->send_hold = frames;
}

void Session::want_backup(uint8_t seat) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (seat >= kMaxSeats || seat == m.my_seat || !m.peers[seat].present) return;
    Writer w;
    w.u8(kWantBackup);
    m.send_reliable(seat, w.out);
}

void Session::take_seat() {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (!m.late || m.mine_from || m.my_seat == kNoSeat || m.take_wanted) return;
    m.take_wanted = true;
    m.ask_take(Clock::now());
}

uint64_t Session::seat_from() const {
    std::lock_guard guard(impl_->lock);
    return impl_->late && impl_->mine_from ? *impl_->mine_from : 0;
}

uint64_t Session::inputs_known(uint8_t seat) const {
    std::lock_guard guard(impl_->lock);
    return seat < kMaxSeats ? impl_->streams[seat].next() : 0;
}

bool Session::fills_inputs(uint8_t seat, uint64_t frame) const {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (seat >= kMaxSeats) return false;
    if (seat == m.my_seat) return true;
    if (m.authority() != m.my_seat) return false;
    const auto& c = m.cabinets[seat];
    return c.open && frame >= c.from && (!c.until || frame < *c.until); // (as fill_open has them)
}

void Session::feed_inputs(uint64_t frame) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (!m.cb.input) return;
    for (uint8_t s = 0; s < m.seats; ++s) {
        const Stream& st = m.streams[s];
        if (!st.known) continue;
        for (uint64_t f = std::max(frame, st.first); f < st.next(); ++f)
            m.cb.input(s, f, st.frames[static_cast<std::size_t>(f - st.first)]);
    }
}

void Session::report_hash(uint64_t frame, uint64_t hash) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    m.my_hashes[frame] = hash;
    while (m.my_hashes.size() > 64) m.my_hashes.erase(m.my_hashes.begin());
    for (auto it = m.their_hashes.begin(); it != m.their_hashes.end();) {
        if (it->first.first == frame) {
            if (it->second != hash) {
                if (m.cb.desync) m.cb.desync(it->first.second, frame);
                m.out_of_step(it->first.second, frame);
            }
            it = m.their_hashes.erase(it);
        } else if (it->first.first + 4096 < frame) {
            it = m.their_hashes.erase(it);
        } else {
            ++it;
        }
    }
    if (m.watch_mode) return; // (a spectator's boards are its own business: nobody waits on them)
    Writer w;
    w.u8(kHash), w.u64(frame), w.u64(hash);
    m.send_all(w.out);
}

void Session::leave() {
    Impl& m = *impl_;
    {
        std::lock_guard guard(m.lock);
        if (m.leaving || !m.socket.is_open()) return;
        m.leaving = true;
        Writer w;
        w.u8(kBye);
        m.send_all(w.out);
        for (uint8_t s = 0; s < kMaxSeats; ++s) // (no waiting for acknowledgements: sent a few times)
            if (s != m.my_seat && m.peers[s].present && !m.peers[s].dropped) {
                for (int k = 0; k < 3; ++k) {
                    for (auto& f : m.peers[s].rel.out) f.ever_sent = false;
                    m.pump_reliable(s, m.peers[s]);
                }
                if (m.my_watch == kNoSeat) // (a player's: its datagram too, past whatever fills the channel's window)
                    for (int k = 0; k < 3; ++k) m.send_peer(m.peers[s], header(kGoodbye, m.session, m.me()));
                if (const std::size_t queued = m.peers[s].rel.out.size(); queued > 1)
                    m.log("goodbye to " + m.peers[s].name + ": " + std::to_string(queued) + " reliable pieces still queued to it" +
                          (queued > kWindow ? " (past the window: the reliable goodbye not sent)" : ""));
            }
        if (m.is_host) // (the spectators: theirs too; with a game in progress they wait for the room's next host)
            for (Impl::Watcher& v : m.watchers) {
                if (!v.present) continue;
                v.rel.queue(w.out);
                for (int k = 0; k < 3; ++k) {
                    for (auto& f : v.rel.out) f.ever_sent = false;
                    m.pump_reliable(0, v);
                }
            }
        if (m.turn) m.turn->release(m.turn_send()); // (the relay's allocation given back: the next run may have one)
    }
}

void Session::vanish() {
    Impl& m = *impl_;
    m.stop = true;
    if (m.thread.joinable()) m.thread.join();
    std::lock_guard guard(m.lock);
    m.leaving = true; // (no goodbye from the destructor)
}

void Session::find_public_address(const Address& stun_server) {
    std::lock_guard guard(impl_->lock);
    impl_->stun_server = stun_server;
    impl_->stun_tries = 0;
}

std::optional<Address> Session::public_address() const {
    std::lock_guard guard(impl_->lock);
    return impl_->public_address;
}

void Session::use_relay(const std::vector<Address>& turn_servers, const std::string& username, const std::string& password) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (turn_servers.empty()) return;
    m.turn.emplace(turn_servers, username, password);
    for (const auto& p : m.peers) { // (the players known so far may send to it)
        if (!p.present) continue;
        m.permit(p.address);
        for (const Address& a : p.candidates) m.permit(a);
    }
    for (const Address& a : m.host_candidates) m.permit(a);
}

std::optional<Address> Session::relay_address() const {
    std::lock_guard guard(impl_->lock);
    return impl_->turn ? impl_->turn->relayed() : std::nullopt;
}

std::vector<Address> Session::candidates() const {
    std::lock_guard guard(impl_->lock);
    return impl_->my_candidates();
}

void Session::expect(const std::vector<Address>& addresses) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    for (const Address& a : addresses) m.permit(a);
    m.punches.push_back({addresses, Clock::now() + std::chrono::seconds(15)});
    m.last_punch = Clock::time_point{};
}

void Session::expect(const std::vector<Address>& addresses, const std::string& country) {
    expect(addresses);
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::erase_if(m.countries_said, [](const Impl::CountrySaid& c) { return c.until < Clock::now(); });
    m.countries_said.push_back({addresses, two_letters(country), Clock::now() + std::chrono::seconds(60)});
}

std::array<bool, kMaxSeats> Session::relayed() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::array<bool, kMaxSeats> out{};
    for (uint8_t s = 0; s < kMaxSeats; ++s) {
        const auto& p = m.peers[s];
        out[s] = p.present && p.reached && p.address.relayed;
    }
    return out;
}

std::array<Session::PeerTiming, kMaxSeats> Session::timing() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    const auto now = Clock::now();
    std::array<PeerTiming, kMaxSeats> out{};
    for (uint8_t s = 0; s < kMaxSeats && s < m.seats; ++s) {
        out[s].next = m.streams[s].next();
        out[s].delay = s == m.my_seat ? m.my_status.delay : m.peers[s].status.delay;
        if (s == m.my_seat || !m.peers[s].present || m.peers[s].dropped) continue;
        out[s].rtt = m.peers[s].rtt;
        out[s].quiet_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m.peers[s].heard).count());
    }
    return out;
}

std::array<int, kMaxSeats> Session::round_trips() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::array<int, kMaxSeats> out{};
    for (uint8_t s = 0; s < kMaxSeats; ++s) out[s] = s == m.my_seat ? 0 : m.peers[s].present ? m.peers[s].rtt : -1;
    return out;
}

Session::SeatView Session::Impl::view(uint8_t s) const {
    SeatView v;
    using State = SeatView::State;
    const auto& p = peers[s];
    const auto& cab = cabinets[s];
    if (s == my_seat) {
        v.state = State::me, v.name = my_name, v.country = my_country, v.status = my_status;
    } else if (p.present && !p.dropped) {
        v.name = p.name, v.country = p.country, v.round_trip = p.rtt, v.status = p.status;
        v.state = cab.open && !cab.until ? State::joining : State::playing;
        if (p.reached)
            v.link = p.address.relayed               ? SeatView::Link::relay
                     : private_address(p.address.ip) ? SeatView::Link::lan
                                                     : SeatView::Link::direct;
        if (v.state == State::joining) {
            if (const std::size_t left = state_left(p); left > 0) { // (the state on its way: the pieces it has acknowledged)
                const uint32_t all = p.state_end - p.state_first;
                v.joining = SeatView::Joining::receiving;
                v.progress = all ? static_cast<int>(100 * (all - left) / all) : 0;
            } else if (p.status.catch_up >= 0) {
                v.joining = SeatView::Joining::catching_up, v.progress = p.status.catch_up;
            }
        }
    } else if (!gone_name[s].empty() || said_bye[s]) {
        v.state = State::gone, v.name = gone_name[s], v.country = gone_country[s], v.left = said_bye[s];
    } else {
        v.state = State::open;
        if (s < start_info.names.size() && !cab.open) v.name = start_info.names[s]; // (before the drop is known)
    }
    return v;
}

bool Session::joining(uint8_t seat) const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    return seat < kMaxSeats && seat != m.my_seat && m.peers[seat].present && m.cabinets[seat].open && !m.cabinets[seat].until;
}

Session::JoinProgress Session::join_progress() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    JoinProgress out;
    if (m.is_host || m.watch_mode || m.leaving) return out;
    if (m.joining && m.my_seat == kNoSeat) {
        out.stage = JoinProgress::Stage::asking;
    } else if (m.late && !m.started) {
        out.stage = JoinProgress::Stage::receiving;
        const Reliable& rel = m.peers[m.host_seat].rel; // (what has come of it: in order, and the pieces beyond a gap)
        out.bytes = rel.assembling.size();
        for (const auto& [seq, piece] : rel.early) out.bytes += piece.second.size();
        out.total = m.state_size;
        if (out.total) out.bytes = std::min(out.bytes, out.total);
    } else if (m.late && m.started && !m.mine_from) {
        out.stage = JoinProgress::Stage::seating;
    }
    return out;
}

std::array<Session::SeatView, kMaxSeats> Session::seats() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (m.watch_mode) return m.watch_view; // (a spectator: as the host sees them)
    std::array<SeatView, kMaxSeats> out{};
    for (uint8_t s = 0; s < m.seats && s < kMaxSeats; ++s) out[s] = m.view(s);
    return out;
}

std::size_t Session::watching() const {
    std::lock_guard guard(impl_->lock);
    return impl_->is_host ? impl_->watcher_count() : impl_->watching_count;
}

void Session::set_watch_one_board(bool one) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (!m.watch_mode || m.watch_one_board == one || m.my_watch == kNoSeat) return;
    m.watch_one_board = one;
    Writer w;
    w.u8(kWatchMode), w.u8(one ? 1 : 0);
    m.send_reliable(m.host_seat, w.out);
}

std::vector<uint8_t> Session::backup_watchers() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::vector<uint8_t> out;
    for (uint8_t i = 0; i < kMaxWatchers; ++i)
        if (m.is_host ? m.watchers[i].present && m.watchers[i].wants_backups : m.bulk[i].present)
            out.push_back(static_cast<uint8_t>(kWatcherBase + i));
    return out;
}

bool Session::spectator() const {
    std::lock_guard guard(impl_->lock);
    return impl_->watch_mode;
}

bool Session::watch(const GameInfo& game, const std::string& name) {
    {
        std::lock_guard guard(impl_->lock); // (before the first join goes out)
        impl_->watch_mode = true;
    }
    return join(game, name);
}

bool Session::hosting() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    return m.is_host;
}

uint8_t Session::host_seat() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    return m.is_host ? m.my_seat : m.host_seat;
}

bool Session::room_closed() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    return m.room_closed;
}

} // namespace net
