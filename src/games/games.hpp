#pragma once

#include "runtime/rom.hpp"

#include <span>
#include <string_view>

namespace m3::games {

extern const GameSpec kSpikeoutFinalEdition;

std::span<const GameSpec* const> all();
const GameSpec* find(std::string_view id);

} // namespace m3::games
