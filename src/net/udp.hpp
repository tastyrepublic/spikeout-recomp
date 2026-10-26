// UDP sockets for network play: Winsock on Windows, BSD sockets elsewhere.
// IPv4 for now. Non-blocking: receive() returns at once, wait() sleeps
// until a datagram comes in or the time is up.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace net {

// An IPv4 address and port (host byte order).
struct Address {
    uint32_t ip = 0;
    uint16_t port = 0;
    bool relayed = false; // reached through our TURN relay (net::TurnClient)
    bool operator==(const Address&) const = default;
    bool same_place(const Address& o) const { return ip == o.ip && port == o.port; }
    std::string str() const; // "192.168.1.5:47000"
    bool loopback() const { return (ip >> 24) == 127; }
};

// The broadcast address of each network this machine is on (a LAN, a
// hotspot it made or joined), and 255.255.255.255: a datagram sent to each
// reaches every machine on them. Loopback included (games on this machine).
std::vector<Address> broadcast_addresses(uint16_t port);

// This machine's own addresses on those networks (loopback first).
std::vector<uint32_t> local_addresses();

// A host name's IPv4 address (DNS), or a dotted address as it is.
std::optional<uint32_t> resolve(const std::string& host);
// "1.2.3.4:5678" to an address.
std::optional<Address> parse_address(const std::string& s);

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Bound to `port` on every interface (0: any free port). `shared`: other
    // sockets (other programs on this machine) may bind the same port, as
    // every listener for discovery broadcasts does. Broadcasts allowed.
    bool open(uint16_t port, bool shared = false);
    void close();
    bool is_open() const;
    uint16_t port() const { return port_; }
    const std::string& error() const { return error_; }

    bool send_to(const Address& to, std::span<const uint8_t> data);
    struct Datagram {
        Address from;
        std::vector<uint8_t> data;
    };
    std::optional<Datagram> receive();
    // Until a datagram is waiting (true) or `ms` milliseconds have passed.
    bool wait(int ms);

private:
    intptr_t fd_;
    uint16_t port_ = 0;
    std::string error_;
    std::vector<uint8_t> buffer_;
};

} // namespace net
