#include "net/stun.hpp"

#include "net/digest.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace net {

namespace {

constexpr uint32_t kMagic = 0x2112A442;
// Methods and classes (RFC 5766).
constexpr uint16_t kAllocate = 0x0003, kRefresh = 0x0004, kSendIndication = 0x0016, kDataIndication = 0x0017,
                   kCreatePermission = 0x0008;
constexpr uint16_t kSuccess = 0x0100, kErrorClass = 0x0110;
// Attributes.
constexpr uint16_t kUsername = 0x0006, kMessageIntegrity = 0x0008, kErrorCode = 0x0009, kLifetime = 0x000D,
                   kXorPeerAddress = 0x0012, kData = 0x0013, kRealm = 0x0014, kNonce = 0x0015, kXorRelayedAddress = 0x0016,
                   kRequestedTransport = 0x0019;

constexpr auto kRetry = std::chrono::milliseconds(500);
constexpr auto kRefreshEvery = std::chrono::minutes(5);    // (allocations last 10)
constexpr auto kPermissionsEvery = std::chrono::minutes(4); // (permissions last 5)

void be16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(static_cast<uint8_t>(v >> 8)), out.push_back(static_cast<uint8_t>(v)); }
void be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

std::vector<uint8_t> xor_address(const Address& a) {
    std::vector<uint8_t> v{0, 1}; // (reserved, IPv4)
    be16(v, static_cast<uint16_t>(a.port ^ (kMagic >> 16)));
    be32(v, a.ip ^ kMagic);
    return v;
}

void put_attribute(std::vector<uint8_t>& m, uint16_t type, const std::vector<uint8_t>& value) {
    be16(m, type), be16(m, static_cast<uint16_t>(value.size()));
    m.insert(m.end(), value.begin(), value.end());
    while (m.size() % 4) m.push_back(0);
}

std::vector<uint8_t> header(uint16_t type, const std::array<uint8_t, 12>& id) {
    std::vector<uint8_t> m;
    be16(m, type), be16(m, 0), be32(m, kMagic);
    m.insert(m.end(), id.begin(), id.end());
    return m;
}

void set_length(std::vector<uint8_t>& m, std::size_t length) {
    m[2] = static_cast<uint8_t>(length >> 8), m[3] = static_cast<uint8_t>(length);
}

std::vector<uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }

} // namespace

const std::vector<uint8_t>* StunMessage::find(uint16_t attribute) const {
    for (const auto& [kind, value] : attributes)
        if (kind == attribute) return &value;
    return nullptr;
}

std::optional<Address> StunMessage::xor_address(uint16_t attribute) const {
    const auto* v = find(attribute);
    if (!v || v->size() < 8 || (*v)[1] != 1) return std::nullopt;
    return Address{get32(v->data() + 4) ^ kMagic, static_cast<uint16_t>(get16(v->data() + 2) ^ (kMagic >> 16))};
}

int StunMessage::error() const {
    const auto* v = find(kErrorCode);
    if (!v || v->size() < 4) return 0;
    return ((*v)[2] & 7) * 100 + (*v)[3];
}

namespace stun {

bool is_stun(std::span<const uint8_t> d) { return d.size() >= 20 && (d[0] & 0xC0) == 0 && get32(d.data() + 4) == kMagic; }

std::optional<StunMessage> parse(std::span<const uint8_t> d) {
    if (!is_stun(d)) return std::nullopt;
    const std::size_t length = get16(d.data() + 2);
    if (20 + length > d.size()) return std::nullopt;
    StunMessage m;
    m.type = get16(d.data());
    std::memcpy(m.id.data(), d.data() + 8, 12);
    for (std::size_t at = 20; at + 4 <= 20 + length;) {
        const uint16_t type = get16(d.data() + at), n = get16(d.data() + at + 2);
        if (at + 4 + n > 20 + length) return std::nullopt;
        m.attributes.push_back({type, std::vector<uint8_t>(d.begin() + static_cast<std::ptrdiff_t>(at + 4),
                                                          d.begin() + static_cast<std::ptrdiff_t>(at + 4 + n))});
        at += 4 + ((n + 3u) & ~3u);
    }
    return m;
}

std::vector<uint8_t> binding_request(const std::array<uint8_t, 12>& id) { return header(kBindingRequest, id); }

std::array<uint8_t, 12> new_transaction() {
    static std::mt19937_64 rng(std::random_device{}());
    std::array<uint8_t, 12> id;
    for (auto& b : id) b = static_cast<uint8_t>(rng());
    return id;
}

} // namespace stun

// ---- TURN ----

TurnClient::TurnClient(Address server, std::string username, std::string password)
    : server_(server), username_(std::move(username)), password_(std::move(password)) {}

std::vector<uint8_t> TurnClient::request(uint16_t type, const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& attributes,
                                         bool authenticated) {
    pending_ = stun::new_transaction();
    pending_type_ = type;
    std::vector<uint8_t> m = header(type, pending_);
    for (const auto& [t, v] : attributes) put_attribute(m, t, v);
    if (authenticated) {
        put_attribute(m, kUsername, text(username_));
        put_attribute(m, kRealm, text(realm_));
        put_attribute(m, kNonce, text(nonce_));
        set_length(m, m.size() - 20 + 24); // (as if MESSAGE-INTEGRITY were there)
        const auto mac = hmac_sha1(key_, m);
        put_attribute(m, kMessageIntegrity, std::vector<uint8_t>(mac.begin(), mac.end()));
    }
    set_length(m, m.size() - 20);
    return m;
}

void TurnClient::allocate(const Send& send) {
    send(server_, request(kAllocate, {{kRequestedTransport, {17, 0, 0, 0}}}, !nonce_.empty()));
    sent_ = Clock::now();
}

void TurnClient::refresh(const Send& send) {
    std::vector<uint8_t> lifetime;
    be32(lifetime, 600);
    send(server_, request(kRefresh, {{kLifetime, lifetime}}, true));
    sent_ = Clock::now();
}

void TurnClient::create_permissions(const Send& send) {
    if (to_permit_.empty()) return;
    send(server_, request(kCreatePermission, {{kXorPeerAddress, xor_address({to_permit_.front(), 0})}}, true));
    sent_ = Clock::now();
}

// A relay lets through only addresses on the internet (it refuses a request
// naming a private one: one request an address, so one refused stops none).
bool TurnClient::is_public(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 0xFF;
    return !(a == 0 || a == 10 || a == 127 || a >= 224 || (a == 100 && (b & 0xC0) == 64) || (a == 169 && b == 254) ||
             (a == 172 && (b & 0xF0) == 16) || (a == 192 && b == 168));
}

void TurnClient::permit(uint32_t ip) {
    if (!is_public(ip)) return;
    if (std::find(permitted_.begin(), permitted_.end(), ip) != permitted_.end()) return;
    permitted_.push_back(ip);
    to_permit_.push_back(ip);
}

void TurnClient::tick(const Send& send) {
    if (failed_) return;
    const auto now = Clock::now();
    if (pending_type_ != 0) { // a request unanswered
        if (now - sent_ < kRetry) return;
        if (++tries_ > 6) {
            failed_ = !relayed_;
            error_ = "the relay doesn't answer";
            if (pending_type_ == kCreatePermission && !to_permit_.empty()) to_permit_.pop_front(); // (that one given up)
            pending_type_ = 0;
            return;
        }
    } else {
        tries_ = 0;
    }
    if (!relayed_) return allocate(send);
    if (now >= refresh_due_) return refresh(send);
    if (to_permit_.empty() && now >= permissions_due_) { // (all again before they lapse)
        to_permit_.assign(permitted_.begin(), permitted_.end());
        permissions_due_ = now + kPermissionsEvery;
    }
    create_permissions(send);
}

std::optional<UdpSocket::Datagram> TurnClient::received(std::span<const uint8_t> data, const Send& send) {
    const auto m = stun::parse(data);
    if (!m) return std::nullopt;
    if (m->type == kDataIndication) {
        const auto peer = m->xor_address(kXorPeerAddress);
        const auto* payload = m->find(kData);
        if (!peer || !payload) return std::nullopt;
        Address from = *peer;
        from.relayed = true;
        return UdpSocket::Datagram{from, *payload};
    }
    if (m->id != pending_ || pending_type_ == 0) return std::nullopt;
    const uint16_t method = m->type & 0xFEEF, kind = m->type & 0x0110;
    if (method != (pending_type_ & 0xFEEF)) return std::nullopt;
    if (method == kCreatePermission && kind != kErrorClass && !to_permit_.empty()) to_permit_.pop_front();
    if (method == kCreatePermission && kind == kErrorClass && m->error() != 401 && m->error() != 438 && !to_permit_.empty())
        to_permit_.pop_front(); // (refused: the next)
    pending_type_ = 0;
    static const bool debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
    if (debug) std::fprintf(stderr, "turn: answer to method %u: %s %d\n", method, kind == kErrorClass ? "error" : "success", m->error());
    if (kind == kErrorClass) {
        const int code = m->error();
        if (code == 401 || code == 438) { // credentials wanted (or a stale nonce): again with them
            if (const auto* r = m->find(kRealm)) realm_.assign(r->begin(), r->end());
            if (const auto* n = m->find(kNonce)) nonce_.assign(n->begin(), n->end());
            key_ = md5(bytes_of(username_ + ":" + realm_ + ":" + password_));
            if (++tries_ > 4) {
                failed_ = true;
                error_ = "the relay refused its credentials";
                return std::nullopt;
            }
            if (method == kAllocate) allocate(send);
            else if (method == (kRefresh & 0xFEEF)) refresh(send);
            else create_permissions(send);
            return std::nullopt;
        }
        if (method == kAllocate && !relayed_) {
            failed_ = true;
            error_ = "the relay said " + std::to_string(code);
        }
        return std::nullopt;
    }
    if (kind == kSuccess && method == kAllocate) {
        relayed_ = m->xor_address(kXorRelayedAddress);
        refresh_due_ = Clock::now() + kRefreshEvery;
        permissions_due_ = Clock::time_point{};
        tick(send);
    } else if (kind == kSuccess && method == kRefresh) {
        refresh_due_ = Clock::now() + kRefreshEvery;
    }
    return std::nullopt;
}

void TurnClient::send_to(const Address& peer, std::span<const uint8_t> data, const Send& send) {
    std::vector<uint8_t> m = header(kSendIndication, stun::new_transaction());
    put_attribute(m, kXorPeerAddress, xor_address(peer));
    put_attribute(m, kData, std::vector<uint8_t>(data.begin(), data.end()));
    set_length(m, m.size() - 20);
    send(server_, m);
}

} // namespace net
