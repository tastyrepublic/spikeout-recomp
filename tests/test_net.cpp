#include "test.hpp"

#include "net/digest.hpp"
#include "net/session.hpp"
#include "net/stun.hpp"
#include "net/update.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace net;

namespace {

// What a session's callbacks saw, for the test to wait on.
struct Seen {
    std::mutex lock;
    std::condition_variable changed;
    bool started = false;
    StartInfo start;
    std::vector<std::pair<uint64_t, InputBytes>> inputs[kMaxSeats];
    std::optional<uint64_t> idle[kMaxSeats];
    int desyncs = 0;

    Session::Callbacks callbacks() {
        Session::Callbacks cb;
        cb.started = [this](const StartInfo& i) {
            std::lock_guard g(lock);
            started = true, start = i;
            changed.notify_all();
        };
        cb.input = [this](uint8_t seat, uint64_t frame, const InputBytes& in) {
            std::lock_guard g(lock);
            inputs[seat].push_back({frame, in});
            changed.notify_all();
        };
        cb.idle = [this](uint8_t seat, uint64_t frame) {
            std::lock_guard g(lock);
            idle[seat] = frame;
            changed.notify_all();
        };
        cb.desync = [this](uint8_t, uint64_t) {
            std::lock_guard g(lock);
            ++desyncs;
            changed.notify_all();
        };
        return cb;
    }
    template <class F> bool wait(F done, int ms = 5000) {
        std::unique_lock g(lock);
        return changed.wait_for(g, std::chrono::milliseconds(ms), done);
    }
};

} // namespace

TEST_CASE("net digests: SHA-1, HMAC-SHA1 and MD5 as their RFCs' examples") {
    auto hex = [](std::span<const uint8_t> d) {
        std::string s;
        for (uint8_t b : d) s += "0123456789abcdef"[b >> 4], s += "0123456789abcdef"[b & 15];
        return s;
    };
    CHECK_EQ(hex(sha1(bytes_of("abc"))), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(hex(sha1(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    CHECK_EQ(hex(md5(bytes_of(""))), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(hex(md5(bytes_of("message digest"))), std::string("f96b697d7cb7938d525a2f31aaf161d0"));
    CHECK_EQ(hex(hmac_sha1(bytes_of("key"), bytes_of("The quick brown fox jumps over the lazy dog"))),
             std::string("de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9"));
}

TEST_CASE("net stun: a binding request, and an answer's address read back") {
    const auto id = stun::new_transaction();
    const auto request = stun::binding_request(id);
    CHECK(stun::is_stun(request));
    // An answer: 203.0.113.9:40000, XOR-MAPPED-ADDRESS as RFC 5389 has it.
    std::vector<uint8_t> answer = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
    answer.insert(answer.end(), id.begin(), id.end());
    const uint16_t port = 40000 ^ 0x2112;
    const uint32_t ip = (203u << 24 | 0u << 16 | 113u << 8 | 9u) ^ 0x2112A442u;
    const uint8_t attribute[] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(port),
                                 static_cast<uint8_t>(ip >> 24), static_cast<uint8_t>(ip >> 16), static_cast<uint8_t>(ip >> 8),
                                 static_cast<uint8_t>(ip)};
    answer.insert(answer.end(), std::begin(attribute), std::end(attribute));
    const auto m = stun::parse(answer);
    CHECK(m.has_value());
    CHECK_EQ(m->type, stun::kBindingSuccess);
    CHECK(m->id == id);
    const auto a = m->xor_address(stun::kXorMappedAddress);
    CHECK(a.has_value());
    CHECK_EQ(a->str(), std::string("203.0.113.9:40000"));
    CHECK(!stun::is_stun(std::vector<uint8_t>{'S', 'K', 1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK_EQ(parse_address("10.1.2.3:47000")->str(), std::string("10.1.2.3:47000"));
    CHECK(!parse_address("10.1.2:47000"));
}

TEST_CASE("net session: a host alone, a player joining the game in progress and taking the empty cabinet") {
    Seen host_seen, guest_seen;
    std::atomic<bool> wanted{false};
    auto host_cb = host_seen.callbacks();
    host_cb.state_wanted = [&] { wanted = true; };
    uint64_t taken_at = 0;
    auto guest_cb = guest_seen.callbacks();
    guest_cb.taken = [&](uint8_t seat, uint64_t frame) {
        std::lock_guard g(guest_seen.lock);
        if (seat == 1) taken_at = frame;
        guest_seen.changed.notify_all();
    };
    Session host(std::move(host_cb), 42);
    CHECK(host.host("host", 2));
    host.start(StartInfo{}); // alone: seat 2 empty
    CHECK(host_seen.wait([&] { return host_seen.started; }));
    for (uint64_t f = 1; f <= 100; ++f) host.local_input(f, {static_cast<uint8_t>(f), 0xFF, 0xFF, 0xFF});
    CHECK(host_seen.wait([&] { return host_seen.inputs[1].size() == 100; })); // (none for the empty cabinet, made by the host)
    CHECK(host_seen.inputs[1].back().second == (InputBytes{0xFF, 0xFF, 0xFF, 0xFF}));

    GameInfo game{host.session_id(), 42, "host", 2, 1, true, Address{0x7F000001u, host.port()}, {}};
    Session guest(std::move(guest_cb), 42);
    CHECK(guest.join(game, "guest"));
    for (int i = 0; i < 300 && !wanted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(wanted.load());
    host.send_state(60, std::vector<uint8_t>(5000, 0x77));
    CHECK(guest_seen.wait([&] { return guest_seen.started; }));
    CHECK_EQ(guest_seen.start.my_seat, 1);
    CHECK(guest_seen.start.state_frame.has_value() && *guest_seen.start.state_frame == 60u);
    CHECK(guest_seen.start.state == std::vector<uint8_t>(5000, 0x77));
    // Every input since the start comes to it: the host's, and none for its cabinet.
    CHECK(guest_seen.wait([&] { return guest_seen.inputs[0].size() == 100 && guest_seen.inputs[1].size() == 100; }));
    guest.local_input(101, {0x00, 0x00, 0x00, 0x00}); // (not its cabinet yet: dropped)
    guest.take_seat();
    CHECK(guest_seen.wait([&] { return taken_at != 0; }));
    CHECK_EQ(taken_at, 101u + 8u); // the host's next frame for it, and a margin
    for (uint64_t f = 101; f <= 130; ++f) {
        host.local_input(f, {static_cast<uint8_t>(f), 0xFF, 0xFF, 0xFF});
        guest.local_input(f, {0x10, 0xFF, 0xFE, 0xFF});
    }
    CHECK(host_seen.wait([&] { return host_seen.inputs[1].size() == 130; }));
    bool right = true;
    for (const auto& [f, in] : host_seen.inputs[1])
        right = right && (f < taken_at ? in == InputBytes{0xFF, 0xFF, 0xFF, 0xFF} : in == InputBytes{0x10, 0xFF, 0xFE, 0xFF});
    CHECK(right); // none up to the hand-over, the player's from it
}

TEST_CASE("net session: two machines on this one, inputs each frame, a hash, a goodbye") {
    Seen host_seen, guest_seen;
    Session host(host_seen.callbacks(), 42);
    CHECK(host.host("host", 2));
    GameInfo game{host.session_id(), 42, "host", 2, 1, false, Address{0x7F000001u, host.port()}, {}};
    {
        Session wrong(Seen{}.callbacks(), 43);
        CHECK(!wrong.join(game, "other build"));
    }
    // Each player's own cabinet: the host's, and the guest's (sent once it
    // has its seat; backup RAM in many fragments).
    std::vector<uint8_t> host_backup(128 * 1024), guest_backup(128 * 1024);
    for (std::size_t i = 0; i < host_backup.size(); ++i)
        host_backup[i] = static_cast<uint8_t>(i * 7), guest_backup[i] = static_cast<uint8_t>(i * 3 + 1);
    host.set_cabinet(std::vector<uint8_t>(128, 0x5A), host_backup);
    auto guest = std::make_unique<Session>(guest_seen.callbacks(), 42);
    guest->set_cabinet(std::vector<uint8_t>(128, 0xA5), guest_backup);
    CHECK(guest->join(game, "guest"));
    for (int i = 0; i < 300 && !host.all_seats_taken(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(host.all_seats_taken());
    CHECK_EQ(host.ready_players(), 2u);
    CHECK(host.players() == std::vector<std::string>({"host", "guest"}));

    StartInfo info;
    info.clock = 123456;
    host.start(info);
    CHECK(guest_seen.wait([&] { return guest_seen.started; }));
    CHECK_EQ(guest_seen.start.my_seat, 1);
    CHECK_EQ(guest_seen.start.seats, 2);
    CHECK_EQ(guest_seen.start.clock, 123456);
    CHECK(guest_seen.start.eeprom.size() == 2 && guest_seen.start.eeprom[0] == std::vector<uint8_t>(128, 0x5A) &&
          guest_seen.start.eeprom[1] == std::vector<uint8_t>(128, 0xA5));
    CHECK(guest_seen.start.backup.size() == 2 && guest_seen.start.backup[0] == host_backup &&
          guest_seen.start.backup[1] == guest_backup);
    CHECK(host_seen.wait([&] { return host_seen.started; }));
    CHECK(host_seen.start.backup.size() == 2 && host_seen.start.backup[1] == guest_backup);
    CHECK(guest_seen.start.names == std::vector<std::string>({"host", "guest"}));

    // Inputs: frames 10-209 from each, a frame skipped now and then (held).
    for (uint64_t f = 10; f < 210; ++f) {
        if (f % 37 == 0) continue;
        host.local_input(f, {static_cast<uint8_t>(f), 1, 2, 3});
        guest->local_input(f, {static_cast<uint8_t>(f + 100), 4, 5, 6});
    }
    CHECK(guest_seen.wait([&] { return guest_seen.inputs[0].size() == 200; }));
    CHECK(host_seen.wait([&] { return host_seen.inputs[1].size() == 200; }));
    bool in_order = true;
    for (std::size_t k = 0; k < 200; ++k) {
        const uint64_t f = 10 + k, held = f % 37 == 0 ? f - 1 : f;
        in_order = in_order && guest_seen.inputs[0][k].first == f && guest_seen.inputs[0][k].second[0] == static_cast<uint8_t>(held) &&
                   host_seen.inputs[1][k].first == f && host_seen.inputs[1][k].second[0] == static_cast<uint8_t>(held + 100);
    }
    CHECK(in_order);
    // How each reaches the other: this PC's address, so the same network; no link for itself.
    using Link = Session::SeatView::Link;
    CHECK(host.seats()[1].link == Link::lan);
    CHECK(guest->seats()[0].link == Link::lan);
    CHECK(host.seats()[0].link == Link::none);

    host.report_hash(120, 0xABC);
    guest->report_hash(120, 0xABD); // (different)
    CHECK(host_seen.wait([&] { return host_seen.desyncs > 0; }));
    CHECK(guest_seen.wait([&] { return guest_seen.desyncs > 0; }));

    // The guest leaves: its seat idle from the frame after its last.
    guest.reset();
    CHECK(host_seen.wait([&] { return host_seen.idle[1].has_value(); }));
    CHECK_EQ(*host_seen.idle[1], 210u);
}

TEST_CASE("net session: the host closing the room before the game starts takes the others out") {
    Session host(Seen{}.callbacks(), 42);
    CHECK(host.host("host", 2));
    Seen guest_seen;
    Session guest(guest_seen.callbacks(), 42);
    GameInfo game{host.session_id(), 42, "host", 2, 1, false, Address{0x7F000001u, host.port()}, {}};
    CHECK(guest.join(game, "guest"));
    for (int i = 0; i < 300 && host.ready_players() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(host.ready_players(), 2u);
    CHECK(!guest.room_closed());
    host.leave(); // (its goodbye)
    bool closed = false;
    for (int i = 0; i < 300 && !closed; ++i) {
        closed = guest.room_closed();
        if (!closed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(closed);
    CHECK(!guest.running());
}

TEST_CASE("net session: a late joiner asking for its cabinet as the host leaves gets it from the new host") {
    Seen host_seen, a_seen, b_seen;
    std::atomic<bool> host_wanted{false};
    auto host_cb = host_seen.callbacks();
    host_cb.state_wanted = [&] { host_wanted = true; };
    uint64_t b_taken = 0;
    auto b_cb = b_seen.callbacks();
    b_cb.taken = [&](uint8_t seat, uint64_t frame) {
        std::lock_guard g(b_seen.lock);
        if (seat == 2) b_taken = frame;
        b_seen.changed.notify_all();
    };
    auto host = std::make_unique<Session>(std::move(host_cb), 42);
    CHECK(host->host("host", 3));
    Session a(a_seen.callbacks(), 42);
    GameInfo game{host->session_id(), 42, "host", 3, 1, false, Address{0x7F000001u, host->port()}, {}};
    CHECK(a.join(game, "a"));
    for (int i = 0; i < 300 && host->ready_players() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    host->start(StartInfo{}); // (seat 3 empty)
    CHECK(a_seen.wait([&] { return a_seen.started; }));
    for (uint64_t f = 1; f <= 50; ++f) {
        host->local_input(f, {static_cast<uint8_t>(f), 0xFF, 0xFF, 0xFF});
        a.local_input(f, {0x22, 0xFF, 0xFF, 0xFF});
    }
    CHECK(a_seen.wait([&] { return a_seen.inputs[0].size() == 50 && a_seen.inputs[2].size() == 50; }));

    // b joins the game in progress through the host and runs on from its state.
    Session b(std::move(b_cb), 42);
    GameInfo running{host->session_id(), 42, "host", 3, 2, true, Address{0x7F000001u, host->port()}, {}};
    CHECK(b.join(running, "b"));
    for (int i = 0; i < 300 && !host_wanted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(host_wanted.load());
    host->send_state(40, std::vector<uint8_t>(3000, 0x55));
    CHECK(b_seen.wait([&] { return b_seen.started; }));
    CHECK_EQ(b_seen.start.my_seat, 2);

    // The host's machine goes as b asks it for its cabinet (no goodbye: b's
    // request is never answered); once it's dropped, b asks a, the room's
    // host then.
    host->vanish();
    b.take_seat();
    uint64_t fed = 50; // (a's own inputs go on: the frames it hands b's cabinet over from follow them)
    for (int i = 0; i < 750; ++i) {
        a.local_input(++fed, {0x22, 0xFF, 0xFF, 0xFF});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard g(b_seen.lock);
        if (b_taken) break;
    }
    CHECK(b_seen.wait([&] { return b_taken != 0; }, 1000));
    // and a takes b's inputs from then on
    const uint64_t first = b_taken;
    for (uint64_t f = 1; f <= first + 20; ++f) b.local_input(f, {0x44, 0xFF, 0xFF, 0xFF});
    while (fed < first + 20) a.local_input(++fed, {0x22, 0xFF, 0xFF, 0xFF});
    CHECK(a_seen.wait([&] {
        for (const auto& [f, in] : a_seen.inputs[2])
            if (f >= first && in == InputBytes{0x44, 0xFF, 0xFF, 0xFF}) return true;
        return false;
    }));
}

TEST_CASE("net session: the host leaving a game in progress, the room goes on: a player joins it through the new host") {
    Seen host_seen, a_seen, b_seen;
    std::atomic<bool> a_wanted{false};
    auto a_cb = a_seen.callbacks();
    a_cb.state_wanted = [&] { a_wanted = true; };
    uint64_t b_taken = 0;
    auto b_cb = b_seen.callbacks();
    b_cb.taken = [&](uint8_t seat, uint64_t frame) {
        std::lock_guard g(b_seen.lock);
        if (seat == 0) b_taken = frame;
        b_seen.changed.notify_all();
    };
    auto host = std::make_unique<Session>(host_seen.callbacks(), 42);
    CHECK(host->host("host", 2));
    Session a(std::move(a_cb), 42);
    GameInfo game{host->session_id(), 42, "host", 2, 1, false, Address{0x7F000001u, host->port()}, {}};
    CHECK(a.join(game, "a"));
    for (int i = 0; i < 300 && host->ready_players() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    host->start(StartInfo{});
    CHECK(a_seen.wait([&] { return a_seen.started; }));
    CHECK(!a.hosting());
    for (uint64_t f = 1; f <= 50; ++f) {
        host->local_input(f, {static_cast<uint8_t>(f), 0xFF, 0xFF, 0xFF});
        a.local_input(f, {0x22, 0xFF, 0xFF, 0xFF});
    }
    CHECK(a_seen.wait([&] { return a_seen.inputs[0].size() == 50; }));

    host.reset(); // the host leaves (its goodbye): seat 1 idle, and a hosts the room now
    CHECK(a_seen.wait([&] { return a_seen.idle[0].has_value(); }));
    CHECK_EQ(*a_seen.idle[0], 51u);
    bool hosting = false;
    for (int i = 0; i < 100 && !hosting; ++i) {
        hosting = a.hosting();
        if (!hosting) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(hosting);
    for (uint64_t f = 51; f <= 80; ++f) a.local_input(f, {0x22, 0xFF, 0xFF, 0xFF}); // (seat 1: none, made by a)

    // b finds the room at a (as its beacons or the lobby would say) and takes the old host's cabinet.
    Session b(std::move(b_cb), 42);
    GameInfo moved{a.session_id(), 42, "host", 2, 1, true, Address{0x7F000001u, a.port()}, {}};
    CHECK(b.join(moved, "b"));
    for (int i = 0; i < 300 && !a_wanted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(a_wanted.load()); // (b waits for every board's state, from a)
    a.send_state(70, std::vector<uint8_t>(3000, 0x33));
    CHECK(b_seen.wait([&] { return b_seen.started; }));
    CHECK_EQ(b_seen.start.my_seat, 0); // the old host's cabinet
    CHECK(b_seen.start.state_frame.has_value() && *b_seen.start.state_frame == 70u);
    CHECK(b_seen.wait([&] { return b_seen.inputs[1].size() == 80 && b_seen.inputs[0].size() >= 80; }));
    b.take_seat();
    CHECK(b_seen.wait([&] { return b_taken != 0; }));
    for (uint64_t f = 81; f <= 120; ++f) {
        a.local_input(f, {0x22, 0xFF, 0xFF, 0xFF});
        b.local_input(f, {0x44, 0xFF, 0xFF, 0xFF});
    }
    CHECK(a_seen.wait([&] { return a_seen.inputs[0].size() >= 120; }));
    bool right = true; // seat 1 on a: the host's, then none, then b's from the hand-over
    for (const auto& [f, in] : a_seen.inputs[0])
        right = right && (f <= 50   ? in[0] == static_cast<uint8_t>(f)
                          : f < b_taken ? in == InputBytes{0xFF, 0xFF, 0xFF, 0xFF}
                                        : in == InputBytes{0x44, 0xFF, 0xFF, 0xFF});
    CHECK(right);
}

TEST_CASE("net update: versions compared as the releases' tags, the newest newer release found in GitHub's list") {
    CHECK(compare_versions("0.1.0", "0.1.0") == 0);
    CHECK(compare_versions("v0.1.0", "0.1.0") == 0);
    CHECK(compare_versions("0.1.0-preview1", "0.1.0-preview2") < 0);
    CHECK(compare_versions("0.1.0-preview9", "0.1.0-preview10") < 0); // (numbers, not letters)
    CHECK(compare_versions("0.1.0-preview2", "0.1.0") < 0);           // (a test build before its release)
    CHECK(compare_versions("0.1.0", "0.1.1-preview1") < 0);
    CHECK(compare_versions("0.10.0", "0.9.3") > 0);
    CHECK(compare_versions("1.0.0", "0.99.99") > 0);
    CHECK(prerelease("0.1.0-preview1"));
    CHECK(!prerelease("0.1.0"));
    CHECK(dev_build("0.1.0-dev"));
    CHECK(!dev_build("0.1.0-preview1"));

    const auto list = Json::parse(R"([
        {"tag_name": "v0.2.0", "draft": true, "prerelease": false, "html_url": "draft"},
        {"tag_name": "v0.1.1-preview1", "draft": false, "prerelease": true, "html_url": "p1"},
        {"tag_name": "v0.1.0", "draft": false, "prerelease": false, "html_url": "r010"},
        {"tag_name": "v0.1.0-preview1", "draft": false, "prerelease": true, "html_url": "p0"}
    ])");
    CHECK(list.has_value());
    const auto a = newer_release(*list, "0.1.0-preview1"); // a test build: offered test builds too
    CHECK(a.has_value() && a->version == "0.1.1-preview1" && a->page == "p1");
    const auto b = newer_release(*list, "0.0.9"); // a release: only releases (never the draft)
    CHECK(b.has_value() && b->version == "0.1.0" && b->page == "r010");
    CHECK(!newer_release(*list, "0.1.0").has_value());
    CHECK(!newer_release(*list, "0.1.1-preview1").has_value());
}
