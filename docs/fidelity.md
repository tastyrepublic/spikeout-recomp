# Hardware fidelity backlog

Where our emulation of the board follows Supermodel rather than known
hardware behavior, and what could make it more faithful. Supermodel is the
reference because it runs the game; these are the places where it guesses,
works around a game, or simplifies. Change one only with evidence (a
datasheet, the firmware's own code, a recording or measurement of a real
board), and keep the old behavior selectable while it is being tested.

## Can be checked against documents

- **MPC106 PCI bridge**: reset values (Supermodel marks register `0xB8`
  "TO-DO: CHECK MANUAL") and the byte order of multi-byte config reads
  (Supermodel stores little-endian and reads back big-endian; its own
  comment calls that unreliable). Source: Motorola MPC106 user's manual.
- **603e exceptions and timers**: SRR1 bits per exception, the MSR on
  entry and `rfi`, decrementer behavior when written negative, time base
  rate. Source: PowerPC 603e user's manual. (We already run the time base
  and decrementer at the bus clock / 4, unlike Supermodel's per-game
  scaling, and re-check the IRQ line when the enable mask is written.)

## Needs real-board evidence

- **IRQ acknowledge** through Tilegen register `0x10`: Supermodel clears
  all 8 written bits, MAME only the low 4.
- **System register `0x1C`** reads `0xFF` because games "apparently expect"
  it; the real meaning is unknown.
- **IRQ `0x02` at the start of line 383**: assumed from System 24 (Lost
  World needs it); not measured on Model 3.
- **Real3D ping-pong flip** at the line in Tilegen register `0x08`: stands
  in for "the Real3D finished the frame", which really depends on how long
  rendering takes.
- **Real3D DMA**: copies finish instantly and the busy bit is never set;
  the completion IRQ is wired as non-maskable (`0x100`) by choice; the ID
  command answers Mercury's `0x16C311DB` (Supermodel: "make sure we're
  returning the correct value") while PCI config space says `0x178611DB`.
- **JTAG**: whether system register bit 7 is really TRST; 3D-RAM boundary
  scan isn't emulated (only BYPASS).
- **Frame loop**: Supermodel runs VBlank until IRQ `0x02` is acknowledged
  (emulator scheduling, not hardware); we don't, and should only if a game
  proves it needs it.
- **Clock**: our time is basic blocks × cycles per block, not real CPU
  cycles; how long code takes affects anything timed against the frame.

## Clock, sound link and 2D

- **Done:** the RTC runs on the emulated time from a start time the
  frontend gives it (Supermodel returns the host's clock on every read and
  ignores writes), so runs repeat, and the game can set it; 12/24 hours,
  STOP, RESET and 30-second adjust follow the datasheet. BUSY is never
  set and the interrupt output isn't emulated.
- **MIDI IRQ timing** is Supermodel's (every 1000 cycles at the start of
  VBlank, at most 129) and so is the order (Supermodel first waits for IRQ
  `0x02` to be acknowledged, which we don't). Real timing comes from the
  sound board's 68000 taking each byte; needs the sound board.
- **Tile generator**: lines are drawn at the start of each display line,
  as Supermodel does; the colour offset's channel order (red in the low
  byte) is Supermodel's code, which its own comment contradicts; which
  layer the line mask's first 16 pixels belong to (overscan) is a guess in
  both emulators. Composite is 2D only: black, the layers below the 3D,
  the layers above.

## Real3D (3D)

The first renderer (`m3::Real3dRenderer`, software) draws the scene in
memory at the end of each frame, with fog (per viewport, by each
polygon's fog intensity). Not modelled yet, roughly in order of what
you'd notice: the spotlight, specular highlights, blending between LOD
levels (the nearer level is drawn whole), frustum culling (the rasterizer
clips instead, which only costs time), translucency order (drawn in scene
order, not sorted), texture filtering (nearest texel), fixed shading and
the Step 1.5 luminous rules.

Timing: writes to high culling RAM and polygon RAM after the ping-pong
flip go into the update buffers in low culling RAM and are applied at
VBlank, after the frame is drawn, as Supermodel does (`m3::Real3d`).
Supermodel also waits for both the command port and the tile generator to
say a frame is ready before drawing; we draw every frame.

## Settings EEPROM and security board

- **Done (datasheet):** the 93C46's dummy 0 bit before READ data. Supermodel
  leaves it out (its notes say shifting in an extra bit "causes problems");
  we show it on DO after the last address bit, and D15 comes with the next
  clock, as the datasheet has it. A write refused because writes are
  disabled starts no programming cycle.
- **EEPROM programming time** is a number of DO reads (5) showing busy, as
  in Supermodel, not the datasheet's milliseconds; needs a time base.
- **Security board status** always reads 0 (never busy) and the first data
  read after a restart gives `0xFFFF` (Supermodel); the game polls the
  busy bit, so a real board takes time per word. Needs a real board.
- **315-5881 stream on malformed data**: where MAME's code reads or writes
  outside its buffers (a compressed line whose last run goes past the end,
  a -1 shift at the start of a line, a plain substream after a compressed
  one), ours stays in bounds; on valid data the output is the same
  (checked against Supermodel's copy of MAME's code on 3,000 random
  streams). Some s-box entries are unknown in MAME too (255).

## Network board

- **Done:** `m3::CommBoard` runs the board's own 68000 (Moira) on the
  firmware the game uploads; it is what `gamerun` uses. The behavioral
  simulation (`m3::SimNetBoard`, Supermodel's `SimNetBoard`, which fakes the
  firmware's results) stays available with `--netboard sim`.
- Evidence from the firmware so far: as master on a ring of one it reports
  status 1 = `0x2021` (one machine, index 0), the same value Supermodel's
  simulation hard-codes; as a slave it waits for a master's frame and, after
  about 0.55 s, reports "no carrier" (status 1 bit 11), which the game shows
  as NO CARRIER! CHECK NETWORK CABLE. That is the correct behavior for a lone
  slave, so a lone cabinet needs its link setting to say master.
- How a lone cabinet's link behaves (Supermodel requires a TCP loopback).

### Real board: what is known (in progress)

Board 837-11861 (MAME `m3comm.cpp`): 68000FN12, 40 MHz crystal, 315-5804,
two 315-5917 link chips, Lattice 315-5958 host interface, 64 KB 68000 RAM,
128 KB comm RAM in two banks. MAME and Supermodel's `CNetBoard` agree on
the 68000 map: `0x00000` RAM (the PowerPC's net RAM at `0xC0020000`, one
byte-swapped 68000 word per PowerPC word: this is where the game uploads
the firmware), `0x40000` control (`0x00` comm bank, `0x40` IRQ 5 ack,
`0xA0` IRQ 2 ack, `0x80` LEDs, `0xC0` node ID?, `0xE0` ?), `0x80000` comm
RAM, `0xC0000` I/O registers shared with the PowerPC at `0xC0010000`
(`0x88`/`0x8A` status mailbox, `0xC0` holds the 68000 in reset while 0;
link chip: `0x10` rx status, `0x14`/`0x16` rx enable / IRQ 6, `0x18` tx
status, `0x1A`/`0x1C` tx enable / IRQ 4, `0x40`-`0x46` rx/tx offset and
size shifted in a byte at a time).

Unknown in both (they guess): 68000 clock (MAME "10 MHz random",
Supermodel 4 MHz), IRQ 5 source (MAME a 100 Hz timer, Supermodel 4 per
frame), IRQ 2 (MAME: host write to `0x88`), link status values (both
return 5), I/O register 0 (MAME returns 1; Supermodel sets bit 0 on IRQ 2
ack). They **disagree** on comm RAM byte order seen by the PowerPC
(Supermodel: byte `n` ↔ 68000 byte `n ^ 3`; MAME: `n ^ 2`) and on banks
(MAME: both CPUs see the bank the 68000 selects).

Our choices (`m3::CommBoardOptions`), still to confirm from the firmware
or a real board: 68000 at 10 MHz; IRQ 5 every 10 ms (the firmware's IRQ 5
handler acks it and toggles the LEDs, so it is a tick, not a frame
signal); link status registers read 5; comm RAM byte order Supermodel's
(`^3`); both CPUs see the selected bank; I/O register 0 bit 0 set by the
IRQ 2 ack (the game waits for it after each command, so it has to come
from somewhere). The link chip: writing `0x80` to `0x1A` sends
`[tx offset, tx size)`, `0x8C` to `0x1C` holds IRQ 4 (transmit complete);
`0x80` to `0x14` and `0x8C` to `0x16` take the next frame into
`[rx offset, ...)` and raise IRQ 6. The firmware's handlers (IRQ 2 at
`0x22AA`, 4 at `0x2260`, 5 at `0x2056`, 6 at `0x20C0`) read `0x82`/`0x83`
(interrupt status) and `0x10`/`0x11`, `0x18`/`0x19` (link status) and
branch on bits of them: those branches are where to look for what the
real chips report.
