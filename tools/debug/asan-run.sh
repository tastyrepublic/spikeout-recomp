#!/bin/bash
# The lazy-snapshot one-board run under AddressSanitizer, up to 4 times, stopping at the first error.
cd ~/spikeout-asan
W=/mnt/z/Emu/spikeout-recomp/.claude/worktrees/lan-play-f60b31
export ROLLBACK_SIM_LAZY=1 ONEBOARD_NO_SHADOWS=1 ASAN_OPTIONS=halt_on_error=1:detect_leaks=0
for k in 1 2 3 4; do
  timeout 2400 build-asan/gamerun /mnt/z/Emu/spikeout-recomp/build/roms/spikeofe --boards 3 --parallel \
    --load-state $W/build-jit/bench/three-6000.state --frames 7500 --blocks 900000000000 --no-sound --no-2d \
    --no-frame-hashes --bot 1 --one-board 1@6010 --one-board 2@6010 --one-board-stays 0 --one-board-tasks \
    --rollback-sim 10 > ~/asan-run$k.txt 2> ~/asan-run$k.err
  rc=$?
  echo "run $k: exit $rc" >> ~/asan-runs.log
  if grep -q "ERROR: AddressSanitizer" ~/asan-run$k.err; then echo "run $k: ASAN ERROR" >> ~/asan-runs.log; break; fi
done
echo finished >> ~/asan-runs.log
