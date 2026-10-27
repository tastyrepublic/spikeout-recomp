#include "net/session.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>

namespace net {

namespace {

using Clock = std::chrono::steady_clock;

// Every datagram: "SK", the protocol, its type, the session, the sender's seat.
enum Type : uint8_t { kBeacon = 1, kJoin = 2, kReliable = 3, kInput = 4, kRefuse = 5, kPunch = 6 };
constexpr uint8_t kNoSeat = 0xFF;
constexpr std::size_t kHeader = 13;

// Control messages, on the reliable channel.
enum Message : uint8_t { kWelcome = 1, kRoster = 2, kStart = 3, kSuspect = 4, kHave = 5, kIdle = 6, kHash = 7, kBye = 8, kCabinet = 9,
                        kState = 10, kWantState = 11, kTake = 12, kTaken = 13 };

constexpr std::size_t kFragment = 1000;    // reliable payload a datagram
constexpr std::size_t kWindow = 256;       // reliable fragments in flight (a save state: some MB)
constexpr uint64_t kTakeMargin = 8;        // frames ahead a cabinet is handed to the player taking it
constexpr auto kResend = std::chrono::milliseconds(150);
constexpr std::size_t kChunkFrames = 64;   // input frames a seat a datagram
constexpr auto kInputResend = std::chrono::milliseconds(4);
constexpr auto kKeepAlive = std::chrono::milliseconds(100);
constexpr auto kJoinRetry = std::chrono::milliseconds(400);
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
    void acked(uint32_t ack) {
        while (!out.empty() && static_cast<int32_t>(out.front().seq - ack) < 0) out.pop_front();
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
    Clock::time_point last_punch{};
    Clock::time_point last_join{}, last_beacon{};
    std::vector<Address> beacon_targets; // (the host: every network's broadcast address)
    Clock::time_point targets_asked{};

    struct Peer {
        bool present = false;   // a player in this seat (not me)
        bool connected = false; // heard from, not dropped
        bool dropped = false;
        Address address;
        std::string name;
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
    };
    std::array<Peer, kMaxSeats> peers;
    std::array<Stream, kMaxSeats> streams;
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
    bool late = false, want_state = false;
    std::optional<StartInfo> late_start;
    std::optional<uint64_t> mine_from;
    std::optional<uint64_t> state_at; // (the frame of the last state we ran on from: hashes before it not compared)
    std::map<uint64_t, InputBytes> pending_local;

    struct Drop {
        bool suspected = false;
        uint8_t asked_by = kNoSeat;
        std::map<uint8_t, uint64_t> answers;
        bool announced = false;
    };
    std::array<Drop, kMaxSeats> drops;
    // A seat whose player went (Session::seats): their name, and whether they said goodbye.
    std::array<std::string, kMaxSeats> gone_name;
    std::array<bool, kMaxSeats> said_bye{};
    std::map<uint64_t, uint64_t> my_hashes;
    std::map<std::pair<uint64_t, uint8_t>, uint64_t> their_hashes;

    Impl(Session& s, Callbacks c, uint64_t b) : owner(s), cb(std::move(c)), build(b) {}

    uint32_t now_ms() const {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count());
    }
    void log(const std::string& s) {
        if (cb.log) cb.log(s);
    }

    // ---- Sending ----

    void send(const Address& to, const Writer& w) { send_raw(to, w.out); }
    void send_raw(const Address& to, std::span<const uint8_t> data) {
        if (to.relayed && turn) turn->send_to(Address{to.ip, to.port}, data, turn_send());
        else socket.send_to(to, data);
    }
    TurnClient::Send turn_send() {
        return [this](const Address& a, std::span<const uint8_t> d) { socket.send_to(a, d); };
    }
    // To a peer: where it answered from, or (not yet) everywhere it may be.
    void send_peer(const Peer& p, const Writer& w) {
        if (p.reached) return send(p.address, w);
        send(p.address, w);
        for (const Address& a : p.candidates)
            if (!a.same_place(p.address)) send(a, w);
    }
    void permit(const Address& a) {
        if (turn) turn->permit(a.ip);
    }

    void send_reliable(uint8_t seat, const std::vector<uint8_t>& message) { peers[seat].rel.queue(message); }
    void send_all(const std::vector<uint8_t>& message, uint8_t except = kNoSeat) {
        for (uint8_t s = 0; s < kMaxSeats; ++s)
            if (s != except && s != my_seat && peers[s].present && peers[s].connected) send_reliable(s, message);
    }

    void pump_reliable(uint8_t seat, Peer& p) {
        const auto now = Clock::now();
        bool sent_any = false;
        std::size_t n = 0;
        for (auto& f : p.rel.out) {
            if (n++ >= kWindow) break;
            if (f.ever_sent && now - f.sent < kResend) continue;
            Writer w = header(kReliable, session, my_seat);
            w.u32(p.rel.rx_next), w.u8(1), w.u32(f.seq), w.u8(f.flags);
            w.out.insert(w.out.end(), f.data.begin(), f.data.end());
            send_peer(p, w);
            f.sent = now, f.ever_sent = true;
            sent_any = true;
        }
        if (p.rel.ack_due && !sent_any) {
            Writer w = header(kReliable, session, my_seat);
            w.u32(p.rel.rx_next), w.u8(0);
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
        for (uint8_t s = 0; s < seats; ++s) w.u64(streams[s].next());
        uint8_t chunks = 0;
        const std::size_t count_at = w.out.size();
        w.u8(0);
        for (uint8_t s = 0; s < seats; ++s) {
            const Stream& st = streams[s];
            // Its own seat's only while that cabinet had no player (a game
            // joined in progress: the frames before it took it over).
            const uint64_t end = s != to ? st.next() : cabinets[s].until ? std::min(st.next(), *cabinets[s].until)
                                                     : cabinets[s].open ? st.next() : 0;
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
        for (uint8_t s = 0; s < seats; ++s) {
            const uint64_t end = s != to ? streams[s].next() : cabinets[s].until ? std::min(streams[s].next(), *cabinets[s].until)
                                                            : cabinets[s].open ? streams[s].next() : 0;
            if (streams[s].known && std::max(p.acks[s], streams[s].first) < end) return true;
        }
        return false;
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
        if (sess != session || from >= kMaxSeats || from == my_seat) return;
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
        if (!said_bye[from]) p.heard = now; // (after a goodbye, what still comes isn't a sign of life: dropped at once)
        p.connected = true;
        if (type == kReliable) on_reliable(from, p, r);
        else if (type == kInput) on_inputs(from, p, r);
    }

    void on_join(const Address& from, Reader& r) {
        const uint64_t their_build = r.u64(), player = r.u64();
        const std::string name = r.str();
        std::vector<Address> where = get_addresses(r);
        if (!r.ok()) return;
        for (uint8_t s = 0; s < seats; ++s)
            if (s != my_seat && peers[s].present && peers[s].player == player) return; // (already in: our welcome is on its way)
        auto refuse = [&](const std::string& why) {
            Writer w = header(kRefuse, session, my_seat);
            w.str(why);
            send(from, w);
        };
        if (their_build != build) return refuse("a different game or build");
        uint8_t seat = kNoSeat; // (a game in progress: a cabinet no player has)
        for (uint8_t s = 0; s < seats && seat == kNoSeat; ++s) // (the old host's too, once the room has moved on)
            if (s != my_seat && !peers[s].present && (!started || (cabinets[s].open && !cabinets[s].until))) seat = s;
        if (seat == kNoSeat) return refuse("the game is full");
        Peer& p = peers[seat];
        p = Peer{};
        said_bye[seat] = false, gone_name[seat].clear();
        p.present = p.connected = true;
        p.address = from, p.name = name, p.player = player, p.heard = Clock::now();
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
        if (cb.input) cb.input(my_seat, frame, in);
    }

    void on_reliable(uint8_t from, Peer& p, Reader& r) {
        const uint32_t ack = r.u32();
        const uint8_t has = r.u8();
        if (!r.ok()) return;
        p.rel.acked(ack);
        if (!has) return;
        const uint32_t seq = r.u32();
        const uint8_t flags = r.u8();
        if (!r.ok()) return;
        std::vector<uint8_t> data(r.here(), r.here() + r.left());
        for (auto& m : p.rel.receive(seq, flags, std::move(data))) on_message(from, m);
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
        case kRoster: // (a game in progress too: a player joining)
            if (from == host_seat && !is_host) {
                const uint8_t n = r.u8();
                for (uint8_t s = 0; s < n && s < kMaxSeats && r.ok(); ++s) {
                    const bool present = r.u8() != 0;
                    const std::string name = r.str();
                    std::vector<Address> where = get_addresses(r);
                    if (s == my_seat || s == host_seat) {
                        if (s == host_seat) peers[host_seat].name = name;
                        continue;
                    }
                    Peer& p = peers[s];
                    if (p.dropped && !(present && cabinets[s].open)) continue;
                    if (p.dropped) p = Peer{}; // (a new player in a cabinet whose player dropped)
                    const bool was = p.present;
                    p.present = present && !where.empty(), p.name = name;
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
        case kTake: { // (the authority) a player joined in progress takes its cabinet
            if (authority() != my_seat || from >= seats || !cabinets[from].open || cabinets[from].until) break;
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
            send_reliable(from, w.out);
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
            peers[from].heard = Clock::time_point{}; // (as if silent for long: dropped next)
            said_bye[from] = true;
            log(peers[from].name + " left");
            if (!started && !is_host && from == host_seat && !room_closed) { // (before the start: the room goes with it)
                room_closed = true;
                log("the host closed the room");
            }
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
        gone_name[s] = p.name;
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
                log("the host left: this machine hosts the room now");
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
            flush_pending();
        }
        fill_open_to_mine();
        if (cb.taken) cb.taken(s, x);
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
        for (uint8_t s = 0; s < seats; ++s) {
            Peer& p = peers[s];
            if (s == my_seat || !p.present || p.dropped) continue;
            if (now - p.heard > kDropAfter && !drops[s].suspected) {
                drops[s].suspected = true; // (its inputs frozen here)
                log((p.name.empty() ? "seat " + std::to_string(s + 1) : p.name) + " isn't answering");
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
                send_all(w.out, s);
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
            send_all(w.out, s);
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
            while (auto d = socket.receive()) received(*d);
            const auto now = Clock::now();
            if (is_host && now - last_beacon > kBeaconEvery) beacon(now);
            if (joining && my_seat == kNoSeat && !leaving && now - last_join > kJoinRetry) {
                last_join = now; // (to every address the host may be at: the first to answer is where it is)
                Writer w = header(kJoin, session, kNoSeat);
                w.u64(build), w.u64(player_id()), w.str(my_name);
                put_addresses(w, my_candidates());
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
            if (started && !leaving) check_drops();
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
                pump_reliable(s, p);
            }
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
        for (const Address& a : beacon_targets) send(a, w);
    }

    uint64_t player_id() {
        static const uint64_t id = std::random_device{}() * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(epoch.time_since_epoch().count());
        return id;
    }

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
    m.begin(info);
}

void Session::local_input(uint64_t frame, const InputBytes& in) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (m.my_seat == kNoSeat || !m.started) return;
    if (m.late && (!m.mine_from || frame < *m.mine_from)) return; // (not ours yet: none, as the others have it)
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
        m.send_reliable(s, w.out);
        m.log("every board's state (frame " + std::to_string(frame) + ", " + std::to_string(state.size() / 1024) + " KB) to " +
              (p.name.empty() ? "seat " + std::to_string(s + 1) : p.name));
    }
}

void Session::take_seat() {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    if (!m.late || m.mine_from || m.my_seat == kNoSeat) return;
    Writer w;
    w.u8(kTake);
    m.send_reliable(m.authority(), w.out);
}

uint64_t Session::inputs_known(uint8_t seat) const {
    std::lock_guard guard(impl_->lock);
    return seat < kMaxSeats ? impl_->streams[seat].next() : 0;
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
            if (s != m.my_seat && m.peers[s].present && !m.peers[s].dropped)
                for (int k = 0; k < 3; ++k) {
                    for (auto& f : m.peers[s].rel.out) f.ever_sent = false;
                    m.pump_reliable(s, m.peers[s]);
                }
    }
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

void Session::use_relay(const Address& turn_server, const std::string& username, const std::string& password) {
    Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    m.turn.emplace(turn_server, username, password);
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

std::array<int, kMaxSeats> Session::round_trips() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::array<int, kMaxSeats> out{};
    for (uint8_t s = 0; s < kMaxSeats; ++s) out[s] = s == m.my_seat ? 0 : m.peers[s].present ? m.peers[s].rtt : -1;
    return out;
}

std::array<Session::SeatView, kMaxSeats> Session::seats() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    std::array<SeatView, kMaxSeats> out{};
    using State = SeatView::State;
    for (uint8_t s = 0; s < m.seats && s < kMaxSeats; ++s) {
        SeatView& v = out[s];
        const auto& p = m.peers[s];
        const auto& cab = m.cabinets[s];
        if (s == m.my_seat) {
            v.state = State::me, v.name = m.my_name;
        } else if (p.present && !p.dropped) {
            v.name = p.name, v.round_trip = p.rtt;
            v.state = cab.open && !cab.until ? State::joining : State::playing;
        } else if (!m.gone_name[s].empty() || m.said_bye[s]) {
            v.state = State::gone, v.name = m.gone_name[s], v.left = m.said_bye[s];
        } else {
            v.state = State::open;
            if (s < m.start_info.names.size() && !cab.open) v.name = m.start_info.names[s]; // (before the drop is known)
        }
    }
    return out;
}

bool Session::hosting() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    return m.is_host;
}

bool Session::room_closed() const {
    const Impl& m = *impl_;
    std::lock_guard guard(m.lock);
    return m.room_closed;
}

} // namespace net
