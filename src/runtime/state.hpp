// Save states. Each device has one `state(State&)` that walks everything it
// holds, the same code for saving and loading, so the two can't drift
// apart: saving appends each field's bytes, loading reads them back in the
// same order. Sections are tagged, so a state from a build that walks
// something differently stops at the first device that doesn't match
// instead of loading garbage.
//
// Only what decides what the game does next is kept: no settings the
// frontend chose (volumes, timing options), no ROMs, no counters kept for
// reports alone.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace m3 {

// A value whose bytes are all its value (no padding).
template <class T>
struct Plain
    : std::bool_constant<std::is_arithmetic_v<T> || std::is_enum_v<T> || std::has_unique_object_representations_v<T>> {};
template <class T, std::size_t N> struct Plain<T[N]> : Plain<T> {};
template <class T, std::size_t N> struct Plain<std::array<T, N>> : Plain<T> {};

class State {
public:
    // Saving: fields are appended to data().
    State() = default;
    // Loading `data`.
    explicit State(std::vector<uint8_t> data) : data_(std::move(data)), loading_(true) {}

    bool loading() const { return loading_; }
    std::vector<uint8_t>& data() { return data_; }
    // Loading: all of it read.
    bool done() const { return pos_ == data_.size(); }

    void bytes(void* p, std::size_t n) {
        if (loading_) {
            if (n > data_.size() - pos_) throw std::runtime_error("save state: ends early");
            std::memcpy(p, data_.data() + pos_, n);
            pos_ += n;
        } else {
            const auto* b = static_cast<const uint8_t*>(p);
            data_.insert(data_.end(), b, b + n);
        }
    }

    // Values with no padding are kept as their bytes. A struct with padding
    // is walked by its fields (a state_fields(State&, T&) found by ADL), so
    // two equal states are equal byte for byte.
    template <class T>
        requires Plain<T>::value
    void io(T& v) {
        bytes(&v, sizeof v);
    }
    template <class T>
        requires(!Plain<T>::value)
    void io(T& v) {
        state_fields(*this, v);
    }
    template <class T> void io(std::vector<T>& v) { sequence(v); }
    template <class T> void io(std::deque<T>& v) { sequence(v); }
    template <class T, std::size_t N>
        requires(!Plain<T>::value)
    void io(std::array<T, N>& v) {
        for (T& e : v) io(e);
    }
    // A buffer whose size is fixed by the hardware: loading checks it.
    template <class T>
        requires Plain<T>::value
    void fixed(std::vector<T>& v) {
        uint64_t n = v.size();
        io(n);
        if (n != v.size()) throw std::runtime_error("save state: a buffer has the wrong size");
        bytes(v.data(), v.size() * sizeof(T));
    }

    // The start of a device's fields.
    void tag(const char* name) {
        const std::size_t n = std::strlen(name);
        if (!loading_) {
            bytes(const_cast<char*>(name), n + 1);
            return;
        }
        if (n + 1 > data_.size() - pos_ || std::memcmp(data_.data() + pos_, name, n + 1) != 0)
            throw std::runtime_error(std::string("save state: expected ") + name);
        pos_ += n + 1;
    }

private:
    template <class C> void sequence(C& c) {
        uint64_t n = c.size();
        io(n);
        if (loading_) {
            if (n > data_.size() - pos_) throw std::runtime_error("save state: ends early");
            c.resize(static_cast<std::size_t>(n));
        }
        if constexpr (Plain<typename C::value_type>::value &&
                      std::is_same_v<C, std::vector<typename C::value_type>>) {
            bytes(c.data(), c.size() * sizeof(typename C::value_type));
        } else {
            for (auto& e : c) io(e);
        }
    }

    std::vector<uint8_t> data_;
    std::size_t pos_ = 0;
    bool loading_ = false;
};

} // namespace m3
