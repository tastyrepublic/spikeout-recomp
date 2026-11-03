// The memory this machine has free now, for what a long history may take (rollback's window: tools/rollback.hpp).
#pragma once

#include <cstdint>

namespace m3 {

// Bytes free now, as the system counts it (what it can give without taking from others: Windows' available physical
// memory, Linux's and Android's MemAvailable, macOS's and iOS's free and inactive pages); 0: not known.
uint64_t memory_free_bytes();

} // namespace m3
