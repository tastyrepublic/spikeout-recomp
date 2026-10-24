// license:BSD-3-Clause
// copyright-holders:Andreas Naive, Olivier Galibert, David Haywood
//
// Adapted from Supermodel's Src/Model3/Crypto.cpp (Supermodel, a Sega Model 3
// arcade emulator; Copyright 2011-2016 Bart Trzynadlowski, Nik Henson;
// GPL-3.0-or-later), which carries MAME's code under the notice above.
//
// Sega 315-5881 decryption, from MAME (src/mame/sega/315-5881_crypt.cpp) by
// way of Supermodel. The s-boxes, key schedules,
// block cipher and decompression trees are theirs, unchanged; the stream
// handling around them is rewritten for this runtime (no save states, fixed
// buffers) but follows Supermodel's version step for step.
//
// From MAME's notes: the encryption is a stream cipher in counter mode around
// a 16-bit block cipher: two 4-round Feistel networks, the first turning the
// counter, game key and sequence key into a middle result that keys the
// second, which decrypts the data word. Each round uses four s-boxes with 6
// inputs and 2 outputs. Every substream starts with an 18-bit header (1+9+8:
// compressed flag, then a length as (A+1)*(B+1)), so plaintext words sit 2
// bits off the cipher's word boundaries. Some s-box entries are unknown (255).
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#include "runtime/crypt315_5881.hpp"
#include "runtime/state.hpp"

#include <algorithm>
#include <utility>

namespace m3 {

namespace {

constexpr int bit(uint32_t x, int n) { return static_cast<int>((x >> n) & 1); }

// Output bit 15 - i takes input bit b[i].
constexpr int bitswap16(uint32_t val, const std::array<int, 16>& b) {
    int r = 0;
    for (int i = 0; i < 16; ++i) r |= bit(val, b[static_cast<std::size_t>(i)]) << (15 - i);
    return r;
}

} // namespace

const Crypt315_5881::Sbox Crypt315_5881::fn1_sboxes[4][4] = {
    {   // 1st round
        {
            {
                0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,
                2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3,
            },
            {3,4,5,7,-1,-1},
            {0,4}
        },

        {
            {
                2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,
                0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2,
            },
            {0,1,2,5,6,7},
            {1,6}
        },

        {
            {
                0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,
                1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3,
            },
            {0,2,5,6,7,-1},
            {2,3}
        },

        {
            {
                3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,
                1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3,
            },
            {1,2,3,4,6,7},
            {5,7}
        },
    },
    {   // 2nd round
        {
            {
                3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,
                2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3,
            },
            {0,1,3,4,5,7},
            {0,4}
        },

        {
            {
                2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,
                3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1,
            },
            {0,1,3,4,6,-1},
            {1,5}
        },

        {
            {
                2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,
                3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2,
            },
            {1,2,5,6,7,6},
            {2,7}
        },

        {
            {
                0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,
                0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0,
            },
            {2,3,4,5,6,7},
            {3,6}
        },
    },
    {   // 3rd round
        {
            {
                0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,
                0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1,
            },
            {1,2,3,4,5,7},
            {0,5}
        },

        {
            {
                1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,
                1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1,
            },
            {0,2,4,5,6,7},
            {1,6}
        },

        {
            {
                0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,
                // unused?
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {0,2,4,6,7,-1},
            {2,3}
        },

        {
            {
                0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,
                2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3,
            },
            {0,1,2,3,6,-1},
            {4,7}
        },
    },
    {   // 4th round
        {
            {
                0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,
                3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1,
            },
            {0,1,3,4,5,6},
            {0,5}
        },

        {
            {
                0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,
                3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3,
            },
            {0,1,2,3,5,7},
            {1,7}
        },

        {
            {
                0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,
                3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0,
            },
            {2,3,4,5,6,7},
            {2,3}
        },

        {
            {
                0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,
                1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1,
            },
            {0,1,2,4,6,7},
            {4,6}
        },
    },
};


const Crypt315_5881::Sbox Crypt315_5881::fn2_sboxes[4][4] = {
    {   // 1st round
        {
            {
                3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,
                2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2,
            },
            {1,3,4,5,6,7},
            {0,7}
        },

        {
            {
                0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,
                3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0,
            },
            {0,3,5,6,5,0},
            {1,2}
        },

        {
            {
                0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,
                0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2,
            },
            {0,2,3,4,7,-1},
            {3,4}
        },

        {
            {
                2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,
                2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1,
            },
            {1,2,5,6,-1,-1},
            {5,6}
        },
    },
    {   // 2nd round
        {
            {
                2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,
                2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0,
            },
            {0,1,4,5,6,7},
            {0,7}
        },

        {
            {
                0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,
                1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3,
            },
            {0,2,3,6,7,-1},
            {1,5}
        },

        {
            {
                1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,
                1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3,
            },
            {2,3,4,6,7,2},
            {2,3}
        },

        {
            {
                2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,
                3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0,
            },
            {1,2,3,4,5,6},
            {4,6}
        },
    },
    {   // 3rd round
        {
            {
                0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,
                3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0,
            },
            {1,4,5,6,7,5},
            {0,5}
        },

        {
            {
                0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,
                2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2,
            },
            {0,2,3,4,6,7},
            {1,7}
        },

        {
            {
                2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,
                // unused?
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {1,2,4,7,2,-1},
            {2,4}
        },

        {
            {
                0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,
                3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3,
            },
            {0,1,2,3,5,7},
            {3,6}
        },
    },
    {   // 4th round
        {
            {
                0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,
                0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1,
            },
            {0,1,2,4,7,-1},
            {0,5}
        },

        {
            {
                2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,
                0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1,
            },
            {0,1,3,5,6,-1},
            {1,3}
        },

        {
            {
                0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,
                3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1,
            },
            {0,2,3,4,5,7},
            {2,7}
        },

        {
            {
                0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,
                2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2,
            },
            {2,3,4,5,6,7},
            {4,6}
        },
    },
};

const int Crypt315_5881::fn1_game_key_scheduling[kFn1GameKeyBits][2] = {
    {1,29},  {1,71},  {2,4},   {2,54},  {3,8},   {4,56},  {4,73},  {5,11},
    {6,51},  {7,92},  {8,89},  {9,9},   {9,39},  {9,58},  {10,90}, {11,6},
    {12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43},
    {19,82}, {20,81}, {21,32}, {22,5},  {23,66}, {24,13}, {24,45}, {25,12},
    {25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86}
};

const int Crypt315_5881::fn2_game_key_scheduling[kFn2GameKeyBits][2] = {
    {0,0},   {1,3},   {2,11},  {3,20},  {4,22},  {5,23},  {6,29},  {7,38},
    {8,39},  {9,55},  {9,86},  {9,87},  {10,50}, {11,57}, {12,59}, {13,61},
    {14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17},
    {22,6},  {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90}
};

const int Crypt315_5881::fn1_sequence_key_scheduling[20][2] = {
    {0,52},  {1,34},  {2,17},  {3,36}, {4,84},  {4,88},  {5,57},  {6,48},
    {6,68},  {7,76},  {8,83},  {9,30}, {10,22}, {10,41}, {11,38}, {12,55},
    {13,74}, {14,19}, {14,80}, {15,26}
};

const int Crypt315_5881::fn2_sequence_key_scheduling[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};

const int Crypt315_5881::fn2_middle_result_scheduling[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

/* node format
0xxxxxxx - next node index
1a0bbccc - end node
           a - 0 = repeat
               1 = fetch
           b - if a = 1
               00 - fetch  0
               01 - fetch  1
               11 - fetch -1
               if a = 0
               000
           c - repeat/fetch counter
               count = ccc + 1
11111111 - empty node
*/
const uint8_t Crypt315_5881::trees[9][2][32] = {
    {
        {0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,
            0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,
            0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,
            0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff,},
        {0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,
            0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff,},
    },
    {
        {0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,
            0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,
            0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,
            0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,
            0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,
            0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,
            0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
};

int Crypt315_5881::feistel_function(int input, const Sbox* sboxes, uint32_t subkeys) {
    int result = 0;
    for (int m = 0; m < 4; ++m) { // 4 s-boxes
        int aux = 0;
        for (int k = 0; k < 6; ++k)
            if (sboxes[m].inputs[k] != -1) aux |= bit(static_cast<uint32_t>(input), sboxes[m].inputs[k]) << k;
        aux = sboxes[m].table[(static_cast<uint32_t>(aux) ^ subkeys) & 0x3F];
        for (int k = 0; k < 2; ++k) result |= bit(static_cast<uint32_t>(aux), k) << sboxes[m].outputs[k];
        subkeys >>= 6;
    }
    return result;
}

uint16_t Crypt315_5881::block_decrypt(uint32_t game_key, uint16_t sequence_key, uint16_t counter, uint16_t data) {
    uint32_t fn1_subkeys[4] = {}, fn2_subkeys[4] = {};
    const auto set = [](uint32_t* subkeys, int pos) { subkeys[pos / 24] ^= 1u << (pos % 24); };

    // Game-key scheduling.
    for (const auto& s : fn1_game_key_scheduling)
        if (bit(game_key, s[0])) set(fn1_subkeys, s[1]);
    for (const auto& s : fn2_game_key_scheduling)
        if (bit(game_key, s[0])) set(fn2_subkeys, s[1]);
    // Sequence-key scheduling.
    for (const auto& s : fn1_sequence_key_scheduling)
        if (bit(sequence_key, s[0])) set(fn1_subkeys, s[1]);
    for (int j = 0; j < 16; ++j)
        if (bit(sequence_key, j)) set(fn2_subkeys, fn2_sequence_key_scheduling[j]);

    // First Feistel network: the counter.
    int aux = bitswap16(counter, {5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2});
    int b = aux >> 8;
    int a = (aux & 0xFF) ^ feistel_function(b, fn1_sboxes[0], fn1_subkeys[0]);
    b ^= feistel_function(a, fn1_sboxes[1], fn1_subkeys[1]);
    a ^= feistel_function(b, fn1_sboxes[2], fn1_subkeys[2]);
    b ^= feistel_function(a, fn1_sboxes[3], fn1_subkeys[3]);
    const uint32_t middle_result = static_cast<uint32_t>(b << 8 | a);

    // Middle-result scheduling.
    for (int j = 0; j < 16; ++j)
        if (bit(middle_result, j)) set(fn2_subkeys, fn2_middle_result_scheduling[j]);

    // Second Feistel network: the data.
    aux = bitswap16(data, {14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10});
    b = aux >> 8;
    a = (aux & 0xFF) ^ feistel_function(b, fn2_sboxes[0], fn2_subkeys[0]);
    b ^= feistel_function(a, fn2_sboxes[1], fn2_subkeys[1]);
    a ^= feistel_function(b, fn2_sboxes[2], fn2_subkeys[2]);
    b ^= feistel_function(a, fn2_sboxes[3], fn2_subkeys[3]);
    aux = b << 8 | a;
    return static_cast<uint16_t>(
        bitswap16(static_cast<uint32_t>(aux), {15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8}));
}

void Crypt315_5881::reset() {
    buffer_.fill(0);
    line_buffer_.fill(0);
    line_buffer_prev_.fill(0);
    prot_cur_address_ = 0;
    subkey_ = 0;
    dec_hist_ = 0;
    dec_header_ = 0;
    enc_ready_ = false;
    buffer_pos_ = 0;
    line_buffer_pos_ = 0;
    line_buffer_size_ = 0;
    buffer_bit_ = 0;
    buffer_bit2_ = 15;
}

uint16_t Crypt315_5881::decrypt() {
    if (key_ == 0) return 0xFFFF;
    if (!enc_ready_) enc_start();
    const uint8_t* p;
    if (dec_header_ & kFlagCompressed) {
        if (line_buffer_pos_ >= line_buffer_size_) { // nothing left in this line
            if (done_compression_) enc_start();
            line_fill();
        }
        p = line_buffer_.data() + line_buffer_pos_;
        line_buffer_pos_ += 2;
    } else {
        // >= rather than MAME's ==: after a compressed substream the bit
        // reader leaves buffer_pos_ odd, and MAME then reads past its buffer.
        if (buffer_pos_ + 2 > kBufferSize) enc_fill();
        p = buffer_.data() + buffer_pos_;
        buffer_pos_ += 2;
    }
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

void Crypt315_5881::set_address_low(uint16_t data) {
    prot_cur_address_ = (prot_cur_address_ & 0xFFFF0000) | data;
    enc_ready_ = false;
}

void Crypt315_5881::set_address_high(uint16_t data) {
    prot_cur_address_ = (prot_cur_address_ & 0x0000FFFF) | uint32_t{data} << 16;
    enc_ready_ = false;
    buffer_bit_ = 7;
    buffer_bit2_ = 15;
}

void Crypt315_5881::set_subkey(uint16_t data) {
    subkey_ = data;
    enc_ready_ = false;
}

uint16_t Crypt315_5881::get_decrypted_16() {
    const uint16_t enc = read_ ? read_(prot_cur_address_) : 0xFFFF;
    const uint16_t dec = block_decrypt(key_, subkey_, static_cast<uint16_t>(prot_cur_address_), enc);
    const uint16_t res = static_cast<uint16_t>((dec & 3) | (dec_hist_ & 0xFFFC));
    dec_hist_ = dec;
    ++prot_cur_address_;
    return res;
}

void Crypt315_5881::enc_start() {
    block_pos_ = 0;
    done_compression_ = false;
    buffer_pos_ = kBufferSize;
    if (buffer_bit2_ != 15) { // bits left over from the last compressed word start the header
        dec_header_ = uint32_t{buffer2a_ & 3u} << 16;
    } else {
        dec_hist_ = 0;
        dec_header_ = uint32_t{get_decrypted_16()} << 16;
    }
    dec_header_ |= get_decrypted_16();

    // Header: bit 17 compressed; bits 8-16 and 0-7 are two lengths minus one
    // whose product is the substream length (compressed: lines of the first).
    block_numlines_ = static_cast<int>(dec_header_ & 0xFF) + 1;
    const int blocky = static_cast<int>((dec_header_ >> 8) & 0x1FF) + 1;
    block_size_ = block_numlines_ * blocky;
    if (dec_header_ & kFlagCompressed) {
        line_buffer_size_ = std::min(blocky, kLineSize);
        line_buffer_pos_ = line_buffer_size_;
        buffer_bit_ = 7;
        buffer_bit2_ = 15;
    }
    enc_ready_ = true;
}

void Crypt315_5881::enc_fill() {
    for (std::size_t i = 0; i != kBufferSize; i += 2) {
        const uint16_t val = get_decrypted_16();
        buffer_[i] = static_cast<uint8_t>(val);
        buffer_[i + 1] = static_cast<uint8_t>(val >> 8);
        block_pos_ += 2;
        // At the end of an uncompressed substream the next header follows.
        if (!(dec_header_ & kFlagCompressed) && block_pos_ == block_size_) enc_start();
    }
    buffer_pos_ = 0;
}

int Crypt315_5881::get_compressed_bit() {
    if (buffer_bit2_ == 15) {
        buffer_bit2_ = 0;
        buffer2a_ = get_decrypted_16();
        buffer2_[0] = static_cast<uint8_t>(buffer2a_);
        buffer2_[1] = static_cast<uint8_t>(buffer2a_ >> 8);
        buffer_pos_ = 0;
    } else {
        ++buffer_bit2_;
    }
    const int res = (buffer2_[(buffer_pos_ & 1) ^ 1] >> buffer_bit_) & 1;
    if (--buffer_bit_ == -1) {
        buffer_bit_ = 7;
        ++buffer_pos_;
    }
    return res;
}

void Crypt315_5881::line_fill() {
    std::swap(line_buffer_, line_buffer_prev_);
    uint8_t* lc = line_buffer_.data();            // the line being built
    const uint8_t* lp = line_buffer_prev_.data(); // the one before it
    line_buffer_pos_ = 0;
    const int size = line_buffer_size_;
    const auto at = [](int i) { return static_cast<std::size_t>(i); };

    for (int i = 0; i < size;) {
        // Tree 0: start of a line; 1: inside it; 2-8: its last 7 bytes.
        const int slot = i ? (i < size - 7 ? 1 : (i & 7) + 1) : 0;
        uint32_t node = 0;
        while (!(node & 0x80)) node = trees[slot][get_compressed_bit()][node];
        if (node == 0xFF) continue; // an "empty node": nothing written
        const int count = static_cast<int>(node & 7) + 1;
        // A run may go on past the end of the line (as in MAME); with an odd
        // length that fills the other byte of the last word.
        if (node & 0x40) { // copy from the previous line, shifted by -1, 0 or +1
            static constexpr int kOffsets[4] = {0, 1, 0, -1};
            const int offset = kOffsets[(node & 0x18) >> 3];
            for (int j = 0; j != count; ++j, ++i)
                lc[at(i ^ 1)] = lp[at(((i + offset + size) % size) ^ 1)];
        } else { // a literal byte, repeated
            int byte = 0;
            for (int k = 0; k < 8; ++k) byte = byte << 1 | get_compressed_bit();
            for (int j = 0; j != count; ++j, ++i) lc[at(i ^ 1)] = static_cast<uint8_t>(byte);
        }
    }
    if (++block_pos_ == block_numlines_) done_compression_ = true;
}

void Crypt315_5881::state(State& s) {
    s.tag("crypt");
    s.io(buffer_), s.io(line_buffer_), s.io(line_buffer_prev_), s.io(prot_cur_address_), s.io(subkey_);
    s.io(dec_hist_), s.io(dec_header_), s.io(enc_ready_), s.io(buffer_pos_), s.io(line_buffer_pos_);
    s.io(line_buffer_size_), s.io(buffer_bit_), s.io(buffer_bit2_), s.io(buffer2_), s.io(buffer2a_);
    s.io(block_size_), s.io(block_pos_), s.io(block_numlines_), s.io(done_compression_);
}

} // namespace m3
