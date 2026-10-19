// Sega 315-5881 decrypter, as on the Model 3 security board: the game puts
// encrypted words in the board's RAM, sets a sequence key, and reads the
// plaintext back one 16-bit word at a time. The cipher (two 4-round Feistel
// networks in counter mode, keyed by a per-game key and the sequence key,
// with an optional line-based compression on top) was worked out by Andreas
// Naive, Olivier Galibert and David Haywood for MAME
// (src/mame/sega/315-5881_crypt.cpp, BSD-3-Clause); the tables and the
// algorithm in crypt315_5881.cpp are theirs, as used by Supermodel
// (Src/Model3/Crypto.cpp). Their notice is kept in that file.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace m3 {

class Crypt315_5881 {
public:
    // `read` returns the encrypted word at a word address in the board's RAM.
    using ReadWord = std::function<uint16_t(uint32_t)>;

    Crypt315_5881() { reset(); }
    void set_key(uint32_t game_key) { key_ = game_key; }
    uint32_t key() const { return key_; }
    void set_reader(ReadWord read) { read_ = std::move(read); }
    void reset();

    // Next plaintext word. With no key (a board without the chip) all ones.
    uint16_t decrypt();
    void set_address_low(uint16_t data);
    void set_address_high(uint16_t data);
    void set_subkey(uint16_t data);

    // The block cipher alone: one encrypted word at `counter`.
    static uint16_t block_decrypt(uint32_t game_key, uint16_t sequence_key, uint16_t counter, uint16_t data);

private:
    static constexpr int kBufferSize = 2, kLineSize = 512;
    static constexpr std::size_t kLineCapacity = kLineSize + 8; // a run may end up to 7 bytes past the line
    static constexpr uint32_t kFlagCompressed = 0x20000;

    struct Sbox {
        uint8_t table[64];
        int inputs[6];  // positions of the input bits, -1 means no input except from the key
        int outputs[2]; // positions of the output bits
    };
    static constexpr int kFn1GameKeyBits = 38, kFn2GameKeyBits = 32;
    static const Sbox fn1_sboxes[4][4];
    static const Sbox fn2_sboxes[4][4];
    static const int fn1_game_key_scheduling[kFn1GameKeyBits][2];
    static const int fn2_game_key_scheduling[kFn2GameKeyBits][2];
    static const int fn1_sequence_key_scheduling[20][2];
    static const int fn2_sequence_key_scheduling[16];
    static const int fn2_middle_result_scheduling[16];
    static const uint8_t trees[9][2][32];
    static int feistel_function(int input, const Sbox* sboxes, uint32_t subkeys);

    uint16_t get_decrypted_16();
    int get_compressed_bit();
    void enc_start();
    void enc_fill();
    void line_fill();

    ReadWord read_;
    uint32_t key_ = 0;
    std::array<uint8_t, kBufferSize> buffer_{};
    std::array<uint8_t, kLineCapacity> line_buffer_{}, line_buffer_prev_{};
    uint32_t prot_cur_address_ = 0;
    uint16_t subkey_ = 0, dec_hist_ = 0;
    uint32_t dec_header_ = 0;
    bool enc_ready_ = false;
    int buffer_pos_ = 0, line_buffer_pos_ = 0, line_buffer_size_ = 0, buffer_bit_ = 0, buffer_bit2_ = 15;
    uint8_t buffer2_[2] = {};
    uint16_t buffer2a_ = 0;
    int block_size_ = 0, block_pos_ = 0, block_numlines_ = 0;
    bool done_compression_ = false;
};

} // namespace m3
