#include "test.hpp"

#include "net/digest.hpp"
#include "net/session.hpp"
#include "net/stun.hpp"

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

    host.report_hash(120, 0xABC);
    guest->report_hash(120, 0xABD); // (different)
    CHECK(host_seen.wait([&] { return host_seen.desyncs > 0; }));
    CHECK(guest_seen.wait([&] { return guest_seen.desyncs > 0; }));

    // The guest leaves: its seat idle from the frame after its last.
    guest.reset();
    CHECK(host_seen.wait([&] { return host_seen.idle[1].has_value(); }));
    CHECK_EQ(*host_seen.idle[1], 210u);
}
