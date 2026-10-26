#include "net/udp.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr intptr_t kNone = static_cast<intptr_t>(INVALID_SOCKET);
// Winsock started once, for the program's life.
struct Winsock {
    Winsock() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~Winsock() { WSACleanup(); }
};
void start() { static Winsock winsock; }
std::string last_error() { return "error " + std::to_string(WSAGetLastError()); }
void close_fd(intptr_t fd) { closesocket(static_cast<SOCKET>(fd)); }
#else
using socket_t = int;
constexpr intptr_t kNone = -1;
void start() {}
std::string last_error() { return std::strerror(errno); }
void close_fd(intptr_t fd) { ::close(static_cast<int>(fd)); }
#endif

socket_t sock(intptr_t fd) { return static_cast<socket_t>(fd); }

sockaddr_in to_sockaddr(const Address& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(a.ip);
    s.sin_port = htons(a.port);
    return s;
}

constexpr std::size_t kMaxDatagram = 65536;

} // namespace

std::string Address::str() const {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
    return buf;
}

std::vector<Address> broadcast_addresses(uint16_t port) {
    start();
    std::vector<Address> out{{0xFFFFFFFFu, port}, {0x7FFFFFFFu, port}}; // everywhere, and this machine (127.255.255.255)
    auto add = [&](uint32_t ip, uint32_t mask) {
        const Address a{ip | ~mask, port};
        if (mask && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    };
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             list, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, list, &size) != NO_ERROR)
            return out;
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            const unsigned bits = u->OnLinkPrefixLength;
            const uint32_t mask = bits == 0 ? 0 : bits >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1);
            add(ip, mask);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_netmask || !(a->ifa_flags & IFF_UP)) continue;
        const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
        const uint32_t mask = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
        add(ip, mask);
    }
    freeifaddrs(list);
#endif
    return out;
}

std::vector<uint32_t> local_addresses() {
    std::vector<uint32_t> out{0x7F000001u};
#ifdef _WIN32
    start();
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, list, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 list, &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
                if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
            }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && (a->ifa_flags & IFF_UP)) {
            const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
            if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
        }
    freeifaddrs(list);
#endif
    return out;
}

std::optional<uint32_t> resolve(const std::string& host) {
    start();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || !list) return std::nullopt;
    const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(list->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(list);
    return ip;
}

std::optional<Address> parse_address(const std::string& s) {
    unsigned a, b, c, d, port;
    char end;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &end) != 5 || a > 255 || b > 255 || c > 255 ||
        d > 255 || port == 0 || port > 65535)
        return std::nullopt;
    return Address{a << 24 | b << 16 | c << 8 | d, static_cast<uint16_t>(port)};
}

UdpSocket::UdpSocket() : fd_(kNone), buffer_(kMaxDatagram) { start(); }
UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::is_open() const { return fd_ != kNone; }

void UdpSocket::close() {
    if (fd_ != kNone) close_fd(fd_);
    fd_ = kNone;
    port_ = 0;
}

bool UdpSocket::open(uint16_t port, bool shared) {
    close();
    const socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (static_cast<intptr_t>(s) == kNone) {
        error_ = "socket: " + last_error();
        return false;
    }
    fd_ = static_cast<intptr_t>(s);
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    if (shared) {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&on), sizeof on);
#endif
    }
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    // An ICMP "port unreachable" from a peer that has gone would otherwise
    // fail the next receive (WSAECONNRESET) on Windows.
    BOOL report = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &report, sizeof report, nullptr, 0, &bytes, nullptr, nullptr); // SIO_UDP_CONNRESET
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in a = to_sockaddr({0, port});
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        error_ = "bind to port " + std::to_string(port) + ": " + last_error();
        close();
        return false;
    }
    socklen_t len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    return true;
}

bool UdpSocket::send_to(const Address& to, std::span<const uint8_t> data) {
    if (fd_ == kNone) return false;
    const sockaddr_in a = to_sockaddr(to);
    const auto sent = ::sendto(sock(fd_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                               reinterpret_cast<const sockaddr*>(&a), sizeof a);
    return sent >= 0 && static_cast<std::size_t>(sent) == data.size();
}

std::optional<UdpSocket::Datagram> UdpSocket::receive() {
    if (fd_ == kNone) return std::nullopt;
    for (;;) {
        sockaddr_in a{};
        socklen_t len = sizeof a;
        const auto n = ::recvfrom(sock(fd_), reinterpret_cast<char*>(buffer_.data()), static_cast<int>(buffer_.size()), 0,
                                  reinterpret_cast<sockaddr*>(&a), &len);
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEMSGSIZE) continue; // (too big: dropped)
#endif
            return std::nullopt; // nothing waiting
        }
        Datagram d;
        d.from = {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
        d.data.assign(buffer_.begin(), buffer_.begin() + n);
        return d;
    }
}

bool UdpSocket::wait(int ms) {
    if (fd_ == kNone) return false;
#ifdef _WIN32
    WSAPOLLFD p{sock(fd_), POLLRDNORM, 0};
    return WSAPoll(&p, 1, ms) > 0;
#else
    pollfd p{sock(fd_), POLLIN, 0};
    return ::poll(&p, 1, ms) > 0;
#endif
}

} // namespace net
