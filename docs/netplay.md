# Online play design

## Two ways to network Spikeout

**Cabinet link (original).** Arcade Spikeout links up to four cabinets, one
player each, through the Model 3 network board (mapped at `0xC0000000`).
We could emulate that board and carry its traffic over UDP. It needs almost
no game-specific work, but the game expects cabinets on a local cable, so on
real internet connections it will stall or desync. We'll keep it as a **LAN
mode** and as a way to check accuracy.

**Rollback (primary).** Every player runs the complete game in single-cabinet
four-player mode. Only inputs are exchanged. Each machine predicts missing
remote inputs (repeat the last one), runs ahead, and when the real input
arrives and differs, restores the last confirmed state and replays the frames
in between. This is the GGPO approach used by modern fighting games.

## What rollback needs from the runtime

1. **Determinism.** The same starting state plus the same inputs must
   produce bit-identical results on Windows, macOS and Linux, on x86-64 and
   ARM64. For the recompiled code this means:
   - PowerPC floating point emulated exactly (rounding modes, `fres`/`frsqrte`
     estimates, `fmadd` fused vs. unfused) rather than whatever the host
     compiler chooses. Build with `-ffp-contract=off` and no fast-math.
   - No host timing, threads or uninitialized memory leaking into game state.
   - Sound may run on its own thread *only* if it never feeds back into the
     main CPU's state, or is stepped in lockstep with the frame.
2. **Fast save/restore.** State = CPU registers (`ppc::CpuState`, a plain
   struct), 8 MB work RAM, backup RAM, Real3D/Tilegen memories and device
   registers. Rolling back up to ~8 frames at 60 fps means saving every frame
   and restoring plus re-simulating several frames inside one 16.7 ms frame.
   Strategy: dirty-page tracking on work RAM so a save copies only what
   changed.
3. **Render separation.** Re-simulated frames must not be drawn or play
   sound. The renderer consumes the display list of the final frame only.
4. **A desync detector.** Peers exchange a hash of game state every N frames;
   a mismatch is logged with both states for debugging.

## Session model

- 2–4 players plus spectators (spectators receive confirmed inputs only).
- Peer-to-peer UDP with a relay fallback; candidate libraries: GekkoNet or
  GGPO (MIT), with Steam Networking Sockets as an optional transport.
- The host's ROM set CRCs and build version are compared at connect time,
  since any difference guarantees a desync.
