#!/bin/bash
# The lazy-snapshot one-board run under ThreadSanitizer (no address randomization: TSan needs it off on this kernel).
cd ~/spikeout-asan
W=/mnt/z/Emu/spikeout-recomp/.claude/worktrees/lan-play-f60b31
export ROLLBACK_SIM_LAZY=1 ONEBOARD_NO_SHADOWS=1 TSAN_OPTIONS="halt_on_error=0:report_signal_unsafe=0:log_path=$HOME/tsan-report"
rm -f ~/tsan-report.*
timeout 5400 setarch x86_64 -R build-tsan/gamerun /mnt/z/Emu/spikeout-recomp/build/roms/spikeofe --boards 3 --parallel \
  --load-state $W/build-jit/bench/three-6000.state --frames 7500 --blocks 900000000000 --no-sound --no-2d \
  --no-frame-hashes --bot 1 --one-board 1@6010 --one-board 2@6010 --one-board-stays 0 --one-board-tasks \
  --rollback-sim 10 > ~/tsan-run.txt 2> ~/tsan-run.err
echo "exit $?" > ~/tsan-done.log
