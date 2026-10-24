// The minimp3 implementation, compiled once (third_party/minimp3/minimp3.h,
// CC0, https://github.com/lieff/minimp3 at ea99364). One local change:
// mp3dec_decode_frame clears its scratch (marked "spikeout-recomp").
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
