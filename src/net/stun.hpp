// STUN (RFC 5389) and a TURN client (RFC 5766), on the game's own UDP
// socket, for internet play:
// - STUN: a Binding request to a STUN server (Cloudflare's) answers with
//   the address our datagrams come from as the internet sees them (our
//   router's): another machine can try that one.
// - TURN: when two routers won't let datagrams through to each other, one
//   machine takes an address on the relay (Cloudflare's TURN, with
//   short-lived credentials from the lobby). The others send to it as to
//   any address; the relay passes their datagrams on to us (Data
//   indications) and ours to them (Send indications).
// Only UDP, IPv4, long-term credentials; no channels (36 bytes more a datagram).
#pragma once

#include "net/udp.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace net {

// A STUN message: its type, transaction and attributes (values as sent).
struct StunMessage {
    uint16_t type = 0;
    std::array<uint8_t, 12> id{};
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> attributes;

    const std::vector<uint8_t>* find(uint16_t attribute) const;
    std::optional<Address> xor_address(uint16_t attribute) const; // XOR-MAPPED/RELAYED/PEER-ADDRESS
    int error() const;                                              // ERROR-CODE (0: none)
};

namespace stun {
constexpr uint16_t kBindingRequest = 0x0001, kBindingSuccess = 0x0101;
constexpr uint16_t kXorMappedAddress = 0x0020;
// The first byte's top two bits clear and the magic cookie: STUN, not ours.
bool is_stun(std::span<const uint8_t> data);
std::optional<StunMessage> parse(std::span<const uint8_t> data);
std::vector<uint8_t> binding_request(const std::array<uint8_t, 12>& id);
std::array<uint8_t, 12> new_transaction();
} // namespace stun

class TurnClient {
public:
    using Send = std::function<void(const Address& to, std::span<const uint8_t> data)>;
    using Clock = std::chrono::steady_clock;

    TurnClient(Address server, std::string username, std::string password);
    const Address& server() const { return server_; }
    // The relay address the others can send to, once allocated.
    std::optional<Address> relayed() const { return relayed_; }
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

    // Let datagrams from `ip` through (each peer's addresses).
    void permit(uint32_t ip);
    // Requests due now (the allocation, its refresh, permissions).
    void tick(const Send& send);
    // A datagram from the server: a peer's datagram it relayed, if it was one.
    std::optional<UdpSocket::Datagram> received(std::span<const uint8_t> data, const Send& send);
    // A datagram to a peer through the relay.
    void send_to(const Address& peer, std::span<const uint8_t> data, const Send& send);

private:
    std::vector<uint8_t> request(uint16_t type, const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& attributes,
                                 bool authenticated);
    void allocate(const Send& send);
    void refresh(const Send& send);
    void create_permissions(const Send& send);
    static bool is_public(uint32_t ip);

    Address server_;
    std::string username_, password_, realm_, nonce_;
    std::array<uint8_t, 16> key_{};
    std::optional<Address> relayed_;
    bool failed_ = false;
    std::string error_;
    std::array<uint8_t, 12> pending_{};
    uint16_t pending_type_ = 0;
    Clock::time_point sent_{}, refresh_due_{}, permissions_due_{};
    int tries_ = 0;
    std::vector<uint32_t> permitted_; // every peer address let through (again every few minutes)
    std::deque<uint32_t> to_permit_;  // (one request each, in turn)
};

} // namespace net
