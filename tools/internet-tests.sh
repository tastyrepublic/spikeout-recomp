#!/usr/bin/env bash
# Network play over the real internet (docs/netplay.md, "The first tests over the internet"): two GitHub Codespaces
# (UK South and Central India: other networks, with their own public addresses) and this PC play each other through
# the lobby, every machine at the arcade's pace with rollback, as players' windows would; each case's machines must
# all end with "0 hash checks differed", the joiners take their cabinets, and the worst gaps between frames are
# reported (a vanished or stalled player's others must not hold). Run it before committing network code: a run takes
# about 25 minutes and two 4-core Codespaces' time (stopped at the end; KEEP=1 leaves them on).
#
#   tools/internet-tests.sh [case ...]   cases: two relay three four vanish stall spectator spectators oneboard (all)
#
# Needs: gh signed in with the codespace scope; the Codespaces made from .devcontainer and built once with
# tools/codespace.sh build (the ROM set in their home folder, never in the repo); this PC's build/gamerun.exe (or
# build/gamerun) built; the ROM set at ROMS (default the main checkout's build/roms/spikeofe). Each Codespace gets
# this checkout's changed sources (against its own commit) and is rebuilt first, so all three run the same code.
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"
roms="${ROMS:-$(cd "$repo/../../.." 2>/dev/null && pwd)/build/roms/spikeofe}"
[ -d "$roms" ] || roms="$repo/build/roms/spikeofe"
gamerun="$repo/build/gamerun.exe"; [ -x "$gamerun" ] || gamerun="$repo/build/gamerun"
lobby="wss://spikeout-lobby.spikeout-lobby.workers.dev/v1/lobby"
out="$repo/build/internet-tests"; mkdir -p "$out"
cases="${*:-two relay three four vanish stall spectator spectators oneboard}"
by_location() { gh api user/codespaces --jq ".codespaces[] | select(.location==\"$1\") | .name" | head -1; }
UK="${UK:-$(by_location UkSouth)}"
IN="${IN:-$(by_location CentralIndia)}"
[ -n "$UK" ] && [ -n "$IN" ] || { echo "no Codespaces in UK South and Central India (gh codespace create ... -l)"; exit 2; }
failed=0
say() { printf '%s\n' "$*"; }
# (one run at a time: two at once kill each other's players between cases)
lock="$out/.running"
if [ -e "$lock" ] && kill -0 "$(cat "$lock")" 2>/dev/null; then echo "a run is going already (process $(cat "$lock"))"; exit 2; fi
echo $$ > "$lock"
trap 'rm -f "$lock"' EXIT

# (a command on a Codespace, starting it if stopped; given up after 3 minutes: a connection that hangs stalls nothing)
cs() { timeout 180 gh codespace ssh -c "$1" -- "$2" 2>/dev/null; }

# This checkout's changed sources onto each Codespace (against its own commit), its gamerun rebuilt.
sync() {
    for c in "$UK" "$IN"; do
        local base files
        base=$(cs "$c" "git -C /workspaces/spikeout-recomp rev-parse HEAD" | tr -d '\r')
        files=$( (git diff --name-only "$base" -- src tools games CMakeLists.txt cmake; git ls-files --others --exclude-standard -- src tools games) | sort -u)
        for f in $files; do [ -f "$f" ] && timeout 120 gh codespace cp -c "$c" "$f" "remote:/workspaces/spikeout-recomp/$f" >/dev/null 2>&1; done
        say "$c: $(echo $files | wc -w) files, $(cs "$c" "cd /workspaces/spikeout-recomp && sed -i 's/\r\$//' $(echo $files) 2>/dev/null; cmake --build build-codespace --target gamerun 2>&1 | grep -E 'error|Linking CXX executable|no work' | tail -1")"
    done
}
stop_all() { cs "$UK" "pkill gamerun; true" >/dev/null; cs "$IN" "pkill gamerun; true" >/dev/null; pkill -f "gamerun" 2>/dev/null; taskkill //F //IM gamerun.exe >/dev/null 2>&1; true; }

# A player on a Codespace (tools/codespace.sh host|join ...), in the background; its log ~/TAG.log there.
cs_play() { # CODESPACE TAG ENV... -- MODE ARGS...
    local c=$1 tag=$2; shift 2
    local env=""
    while [ "$1" != "--" ]; do env="$env $1"; shift; done
    shift
    # (its debug lines apart, in ~/TAG.err: mixed in, one cut a summary line in two)
    cs "$c" "cd /workspaces/spikeout-recomp && (nohup env SPIKEOUT_NET_DEBUG=1 $env tools/codespace.sh $* > ~/$tag.log 2> ~/$tag.err &)" >/dev/null
}
room_of() { # CODESPACE TAG: the room code its host printed
    local code=""
    for _ in $(seq 1 60); do
        code=$(cs "$1" "grep -o 'on the lobby as [A-Z2-9]*' ~/$2.log" | awk '{print $5}' | tr -d '\r')
        [ -n "$code" ] && break; sleep 2
    done
    echo "$code"
}
seat_taken() { # CODESPACE TAG N: until its host says seat N taken
    for _ in $(seq 1 90); do cs "$1" "grep -q 'seat $3 taken' ~/$2.log" && return 0; sleep 2; done; return 1
}
# This PC's player, in the background (PC_PID); its log $out/TAG.txt.
pc_play() { # TAG ENV... -- ARGS...
    local tag=$1; shift
    local env=()
    while [ "$1" != "--" ]; do env+=("$1"); shift; done
    shift
    env SPIKEOUT_NET_DEBUG=1 "${env[@]}" "$gamerun" "$roms" --blocks 900000000000 --no-sound --no-2d --pace --lobby "$lobby" "$@" \
        > "$out/$tag.txt" 2> "$out/$tag.err" &
    PC_PID=$!
}
cs_done() { # CODESPACE TAG: until its run has ended (or 6 minutes)
    for _ in $(seq 1 120); do cs "$1" "grep -qE '^CPU time|^gamerun:|^net: gone at frame' ~/$2.log ~/$2.err" && return 0; sleep 3; done; return 1
}
# A machine's lines: its hash checks, links, pace, how it joined; fails the case unless 0 checks differed.
judge() { # NAME FILE-TEXT [MAX_GAP_MS]
    local name=$1 text=$2 max=${3:-0}
    local diff gap
    diff=$(printf '%s\n' "$text" | grep -oE 'Network: [0-9]+ hash checks differed' | grep -oE '[0-9]+' | head -1)
    gap=$(printf '%s\n' "$text" | grep -oE 'the longest gap [0-9]+ ms' | grep -oE '[0-9]+' | head -1)
    local verdict=ok
    [ "$diff" = 0 ] || verdict=FAILED
    if [ "$max" -gt 0 ] && [ -n "$gap" ] && [ "$gap" -gt "$max" ]; then verdict="FAILED (a gap over $max ms)"; fi
    [ "$verdict" = ok ] || failed=$((failed + 1))
    say "  $name: $verdict; ${diff:-?} hash checks differed; longest gap ${gap:-?} ms; $(printf '%s\n' "$text" | grep -oE '^Network: links:.*' | head -1 | sed 's/^Network: //'); $(printf '%s\n' "$text" | grep -oE '(caught up at frame [0-9]+: [^)]*\)?|seat [0-9] taken from frame [0-9]+)' | head -1)"
}
cs_text() { cs "$1" "grep -E '^Network|^Pace|caught up|seat [0-9] taken|^Rollback: went' ~/$2.log"; }

rb="--net-rollback 12 --input-delay 1" # (as the window's Auto for players this far apart)
run_case() {
    local name=$1
    say "== $name"
    stop_all
    case "$name" in
    two | relay)
        local env=""; [ "$name" = relay ] && env="RELAY=1"
        cs_play "$UK" h-$name FRAMES=4800 $env -- host 2 $rb --press 0:coin@1200 --press 0:start@1300 --press 0:beat@1700
        local code; code=$(room_of "$UK" h-$name)
        local penv=(); [ "$name" = relay ] && penv=(SPIKEOUT_TEST_RELAY_ONLY=1)
        pc_play pc-$name "${penv[@]}" -- --frames 4200 $rb --net-join "$code" --net-catch-up --press 1:coin@2400 --press 1:start@2500 --press 1:beat@2900
        wait $PC_PID; cs_done "$UK" h-$name
        judge "UK (host)" "$(cs_text "$UK" h-$name)"; judge "this PC" "$(cat "$out/pc-$name.txt")" ;;
    three | four | vanish)
        local seats=3; [ "$name" = four ] && seats=4
        cs_play "$UK" h-$name FRAMES=6000 -- host $seats $rb --press 0:coin@1200 --press 0:start@1300 --press 0:beat@1700
        local code; code=$(room_of "$UK" h-$name)
        local leave=""; [ "$name" = vanish ] && leave="--net-vanish-at 3600"
        cs_play "$IN" i-$name FRAMES=5600 -- join "$code" $rb $leave --press 1:coin@2400 --press 1:start@2500 --press 1:beat@2900
        seat_taken "$UK" h-$name 2
        pc_play pc-$name -- --frames 5400 $rb --net-join "$code" --net-catch-up --press 2:coin@3000 --press 2:start@3100
        if [ "$name" = four ]; then seat_taken "$UK" h-$name 3; cs_play "$IN" j-$name FRAMES=5400 -- join "$code" $rb; fi
        wait $PC_PID; cs_done "$UK" h-$name; cs_done "$IN" i-$name
        local max=0; [ "$name" = vanish ] && max=500 # (the others go on as it vanishes: no hold)
        judge "UK (host)" "$(cs_text "$UK" h-$name)" $max; judge "this PC" "$(cat "$out/pc-$name.txt")" $max
        [ "$name" = vanish ] || judge "India" "$(cs_text "$IN" i-$name)"
        if [ "$name" = four ]; then cs_done "$IN" j-$name; judge "India, 4P" "$(cs_text "$IN" j-$name)"; fi ;;
    stall) # (this PC sends nothing for 1.5 s while pressing buttons, then is back: the host goes back for them, no hold)
        cs_play "$UK" h-$name FRAMES=4800 -- host 2 $rb
        local code; code=$(room_of "$UK" h-$name)
        local presses=""; for f in 1590 1605 1620 1635 1650 1665 1680 1695; do presses="$presses --press 1:beat@$f"; done
        pc_play pc-$name SPIKEOUT_TEST_SILENT=30,1500 -- --frames 4200 $rb --net-join "$code" --net-catch-up $presses
        wait $PC_PID; cs_done "$UK" h-$name
        judge "UK (host)" "$(cs_text "$UK" h-$name)" 300; judge "this PC (stalled)" "$(cat "$out/pc-$name.txt")" ;;
    spectator)
        cs_play "$UK" h-$name FRAMES=4800 -- host 2 $rb --press 0:coin@1200 --press 0:start@1300
        local code; code=$(room_of "$UK" h-$name)
        pc_play pc-$name -- --frames 4200 $rb --net-join "$code" --net-catch-up --press 1:coin@2400 --press 1:start@2500
        seat_taken "$UK" h-$name 2
        cs_play "$IN" w-$name FRAMES=4000 -- join "$code" --net-spectate
        wait $PC_PID; cs_done "$UK" h-$name; cs_done "$IN" w-$name
        judge "UK (host)" "$(cs_text "$UK" h-$name)" 300; judge "this PC" "$(cat "$out/pc-$name.txt")" 300
        judge "India (watching)" "$(cs_text "$IN" w-$name)" ;;
    spectators) # (four at once, the most a game has (net::kMaxWatchers): two in India, two in the UK; a fifth turned away)
        cs_play "$UK" h-$name FRAMES=5400 -- host 2 $rb --press 0:coin@1200 --press 0:start@1300
        local code; code=$(room_of "$UK" h-$name)
        pc_play pc-$name -- --frames 4800 $rb --net-join "$code" --net-catch-up --press 1:coin@2400 --press 1:start@2500
        seat_taken "$UK" h-$name 2
        # (their frames to the host's end, less their lag: all four still watching when the fifth asks)
        cs_play "$IN" w1-$name FRAMES=5200 -- join "$code" --net-spectate; sleep 8
        cs_play "$IN" w2-$name FRAMES=5200 -- join "$code" --net-spectate; sleep 8
        cs_play "$UK" w3-$name FRAMES=5200 -- join "$code" --net-spectate; sleep 8
        cs_play "$UK" w4-$name FRAMES=5200 -- join "$code" --net-spectate; sleep 12
        cs_play "$IN" w5-$name FRAMES=5200 -- join "$code" --net-spectate
        wait $PC_PID; cs_done "$UK" h-$name
        judge "UK (host)" "$(cs_text "$UK" h-$name)" 300; judge "this PC" "$(cat "$out/pc-$name.txt")" 300
        for w in w1 w2; do cs_done "$IN" $w-$name; judge "India, watching ($w)" "$(cs_text "$IN" $w-$name)"; done
        for w in w3 w4; do cs_done "$UK" $w-$name; judge "UK, watching ($w)" "$(cs_text "$UK" $w-$name)"; done
        cs_done "$IN" w5-$name
        if cs "$IN" "grep -qiE 'refused|full|too many|can.t (join|watch)' ~/w5-$name.log ~/w5-$name.err"; then
            say "  a fifth spectator: turned away, ok ($(cs "$IN" "grep -iE 'refused|full|too many|can.t' ~/w5-$name.log ~/w5-$name.err | head -1" | tr -d '\r'))"
        else
            say "  a fifth spectator: FAILED (let in, or no word why)"; failed=$((failed + 1))
        fi ;;
    oneboard) # (one board a PC on every machine, as weak devices play: no rollback, a delay of 2)
        local ob="--input-delay 2 --one-board-auto --one-board-held --net-one-board 100"
        cs_play "$UK" h-$name FRAMES=6000 -- host 3 $ob --press 0:coin@1200 --press 0:start@1300 --press 0:beat@1700
        local code; code=$(room_of "$UK" h-$name)
        cs_play "$IN" i-$name FRAMES=5600 -- join "$code" $ob --press 1:coin@2400 --press 1:start@2500
        seat_taken "$UK" h-$name 2
        pc_play pc-$name -- --frames 5400 $ob --net-join "$code" --net-catch-up --press 2:coin@3000 --press 2:start@3100
        wait $PC_PID; cs_done "$UK" h-$name; cs_done "$IN" i-$name
        judge "UK (host)" "$(cs_text "$UK" h-$name)"; judge "India" "$(cs_text "$IN" i-$name)"; judge "this PC" "$(cat "$out/pc-$name.txt")" ;;
    *) say "  (no such case)"; failed=$((failed + 1)) ;;
    esac
}

say "Codespaces: $UK (UK South), $IN (Central India); this PC: $gamerun"
sync
for c in $cases; do run_case "$c"; done
stop_all
[ "${KEEP:-0}" = 1 ] || { gh codespace stop -c "$UK" >/dev/null 2>&1; gh codespace stop -c "$IN" >/dev/null 2>&1; }
if [ "$failed" -gt 0 ]; then say "$failed failed"; exit 1; fi
say "all the same"
