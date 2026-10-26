// SHA-1, HMAC-SHA1 and MD5, as STUN and TURN's long-term credentials need
// them (RFC 5389: MESSAGE-INTEGRITY is HMAC-SHA1 keyed with
// MD5(username:realm:password)). Written from RFC 3174, 2104 and 1321.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace net {

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);
std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data);
std::array<uint8_t, 16> md5(std::span<const uint8_t> data);

inline std::span<const uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

} // namespace net
