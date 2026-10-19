#include "games/games.hpp"

#include <array>

namespace m3::games {

namespace {
const std::array<const GameSpec*, 1> kGames = {&kSpikeoutFinalEdition};
} // namespace

std::span<const GameSpec* const> all() { return kGames; }

const GameSpec* find(std::string_view id) {
    for (const GameSpec* g : kGames)
        if (g->id == id) return g;
    return nullptr;
}

} // namespace m3::games
