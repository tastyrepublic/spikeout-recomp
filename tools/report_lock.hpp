// A report's network game recording, locked (the owner's choice, 2026-11-11): a recording's state is the game's
// memory, pieces of the ROM in it (textures, models), and a report's zip may be posted where anyone can download it.
// So the game locks those files to the project owner's key before they go into the zip: locked with the owner's
// public key (kOwnerKey below: it can only lock, so it may be seen), opened only with the owner's secret key, kept on
// the owner's computer and never in the repository. gamerun --unlock FILE.locked --report-key KEYFILE opens one;
// gamerun --make-report-key KEYFILE makes a new pair (the public half printed, for kOwnerKey).
//
// Monocypher (third_party/monocypher): X25519 with a new key for each file, BLAKE2b of the shared secret and both
// public keys as the key, XChaCha20-Poly1305 (crypto_aead_lock) over the bytes. A locked file:
//   "SPKLOCK1", the file's own public key (32 bytes), the nonce (24), the MAC (16), then the locked bytes.
// The magic and the file's public key are the MAC's additional data.
#pragma once

#include <monocypher.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h> // (arc4random_buf)
#else
#include <sys/random.h>
#endif

namespace tools::lock {

using Key = std::array<uint8_t, 32>;
inline constexpr char kMagic[8] = {'S', 'P', 'K', 'L', 'O', 'C', 'K', '1'};
inline constexpr std::size_t kHead = 8 + 32 + 24 + 16;

// The project owner's public key (hex; made by gamerun --make-report-key, 2026-11-11). Empty: reports carry no
// recording.
inline constexpr const char* kOwnerKey = "b92fd0103c717d7d8e602f6df40a57a88247fd73ede406a7aee1a3d8623f144d";

// Bytes from the system's own secure source. False if it gave none.
inline bool random_bytes(uint8_t* out, std::size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
    arc4random_buf(out, n);
    return true;
#else
    while (n) {
        const ssize_t got = getrandom(out, n, 0);
        if (got <= 0) return false;
        out += got, n -= static_cast<std::size_t>(got);
    }
    return true;
#endif
}

inline std::string hex(const uint8_t* p, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s += digits[p[i] >> 4], s += digits[p[i] & 15];
    return s;
}
// 64 hex digits (spaces and line ends around them skipped) to a key.
inline std::optional<Key> key_from_hex(std::string_view text) {
    std::string digits;
    for (char c : text)
        if (std::isxdigit(static_cast<unsigned char>(c))) digits += c;
        else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return std::nullopt;
    if (digits.size() != 64) return std::nullopt;
    Key k{};
    for (std::size_t i = 0; i < 32; ++i) k[i] = static_cast<uint8_t>(std::stoi(digits.substr(2 * i, 2), nullptr, 16));
    return k;
}
inline std::optional<Key> owner_key() { return key_from_hex(kOwnerKey); }

namespace detail {
inline Key shared_key(const uint8_t raw[32], const uint8_t file_public[32], const uint8_t owner_public[32]) {
    uint8_t in[96];
    std::memcpy(in, raw, 32), std::memcpy(in + 32, file_public, 32), std::memcpy(in + 64, owner_public, 32);
    Key k{};
    crypto_blake2b(k.data(), k.size(), in, sizeof in);
    crypto_wipe(in, sizeof in);
    return k;
}
} // namespace detail

inline bool is_locked(const std::vector<uint8_t>& data) {
    return data.size() >= kHead && std::memcmp(data.data(), kMagic, 8) == 0;
}

// `data` locked to `owner` (a public key). Empty if the system gave no random bytes.
inline std::vector<uint8_t> seal(const std::vector<uint8_t>& data, const Key& owner) {
    uint8_t secret[32], nonce[24];
    if (!random_bytes(secret, sizeof secret) || !random_bytes(nonce, sizeof nonce)) return {};
    std::vector<uint8_t> out(kHead + data.size());
    std::memcpy(out.data(), kMagic, 8);
    uint8_t* file_public = out.data() + 8;
    crypto_x25519_public_key(file_public, secret);
    uint8_t raw[32];
    crypto_x25519(raw, secret, owner.data());
    Key key = detail::shared_key(raw, file_public, owner.data());
    std::memcpy(out.data() + 40, nonce, 24);
    crypto_aead_lock(out.data() + kHead, out.data() + 64, key.data(), nonce, out.data(), 40, data.data(), data.size());
    crypto_wipe(secret, sizeof secret), crypto_wipe(raw, sizeof raw), crypto_wipe(key.data(), key.size());
    return out;
}

// A locked file opened with the owner's secret key: nothing if it isn't one, or the key isn't the one it was locked
// to, or it was changed since.
inline std::optional<std::vector<uint8_t>> open(const std::vector<uint8_t>& locked, const Key& secret) {
    if (!is_locked(locked)) return std::nullopt;
    Key owner_public{};
    crypto_x25519_public_key(owner_public.data(), secret.data());
    const uint8_t* file_public = locked.data() + 8;
    uint8_t raw[32];
    crypto_x25519(raw, secret.data(), file_public);
    Key key = detail::shared_key(raw, file_public, owner_public.data());
    std::vector<uint8_t> out(locked.size() - kHead);
    const int bad = crypto_aead_unlock(out.data(), locked.data() + 64, key.data(), locked.data() + 40, locked.data(), 40,
                                       locked.data() + kHead, out.size());
    crypto_wipe(raw, sizeof raw), crypto_wipe(key.data(), key.size());
    if (bad) return std::nullopt;
    return out;
}

// A new key pair: the secret's hex (for the key file) and the public's (for kOwnerKey). Nothing if no random bytes.
inline std::optional<std::pair<std::string, std::string>> make_pair() {
    Key secret{}, pub{};
    if (!random_bytes(secret.data(), secret.size())) return std::nullopt;
    crypto_x25519_public_key(pub.data(), secret.data());
    auto r = std::make_pair(hex(secret.data(), 32), hex(pub.data(), 32));
    crypto_wipe(secret.data(), secret.size());
    return r;
}

} // namespace tools::lock
