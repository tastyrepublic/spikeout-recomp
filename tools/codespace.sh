#!/usr/bin/env bash
# Network play over the internet with a GitHub Codespace as the second network (its own public address, which one PC
# can't give itself): the Codespace runs gamerun, headless, against a window on a PC. .devcontainer/ sets it up.
#
#   1. Upload your spikeofe.zip into the Codespace's home folder (drag it onto the Explorer, then move it:
#      mv /workspaces/*/spikeofe.zip ~/ ; zips are git-ignored, but keep it out of the repo anyway).
#   2. tools/codespace.sh build ~/spikeofe.zip       unpacks it to ~/roms/spikeofe and builds gamerun (a few minutes)
#   3. On the PC, host a game in the window (Network play > Host, internet), and here:
#      tools/codespace.sh join CODE                  joins it by its room code; run it again in another terminal for
#                                                    a third or fourth player (they share the Codespace's address)
#      or tools/codespace.sh host [SEATS]            hosts here (private, by code: it prints the code to join with)
#   Extra gamerun options go after (e.g. --frames 30000); RELAY=1 before the command offers only the relay's address
#   (Cloudflare's TURN), to test the relay; the window's seat list shows RELAY, DIRECT or LAN for each player.
#
# Both sides must be the same version (a build of the same commit, or the same release): the lobby lists games only
# to players with the same build. The run ends after FRAMES frames (default 20,000, about 6 minutes) and prints
# "Network: N hash checks differed" (0: every machine's game the same).
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
roms="$HOME/roms/spikeofe"
build="$repo/build-codespace"
lobby="wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby"
gamerun="$build/gamerun"

run() {
    [ -x "$gamerun" ] || { echo "build it first: tools/codespace.sh build ~/spikeofe.zip"; exit 1; }
    if [ "${RELAY:-0}" = 1 ]; then export SPIKEOUT_TEST_RELAY_ONLY=1; fi
    exec "$gamerun" "$roms" --blocks 900000000000 --no-sound --no-2d --pace --frames "${FRAMES:-20000}" --lobby "$lobby" "$@"
}

case "${1:-}" in
build)
    zip="${2:?the ROM set: tools/codespace.sh build ~/spikeofe.zip}"
    mkdir -p "$roms"
    if command -v unzip >/dev/null; then unzip -o -q "$zip" -d "$roms"; else python3 -m zipfile -e "$zip" "$roms"; fi
    cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIKEOUT_FRONTEND=OFF -DSPIKEOUT_GPU=OFF \
        -DSPIKEOUT_BUILD_TESTS=OFF -DSPIKEOUT_ROM_DIR="$roms"
    cmake --build "$build" --target gamerun
    echo "built: $gamerun"
    ;;
join)
    code="${2:?the room code: tools/codespace.sh join CODE}"
    shift 2
    run --net-join "$code" "$@"
    ;;
host)
    seats="${2:-2}"
    shift $(($# >= 2 ? 2 : 1))
    run --net-host Codespace --seats "$seats" --net-start-alone --private "$@"
    ;;
*)
    sed -n '2,20p' "$0"
    exit 1
    ;;
esac
