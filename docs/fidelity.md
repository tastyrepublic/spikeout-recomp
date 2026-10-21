# Hardware fidelity backlog

Where our emulation of the board follows Supermodel rather than known
hardware behavior, and what could make it more faithful. Supermodel is the
reference because it runs the game; these are the places where it guesses,
works around a game, or simplifies. Change one only with evidence (a
datasheet, the firmware's own code, a recording or measurement of a real
board), and keep the old behavior selectable while it is being tested.

## Can be checked against documents

- **Checked: MPC106 PCI bridge** against its user's manual (MPC106UM/AD
  Rev. 2 and the addendum). Byte order: the registers are little-endian,
  and in big-endian mode the byte the CPU puts at `CONFIG_DATA + i` is
  register byte `offset + i` (the manual's worked examples), which is what
  we do. Reset values now come from the manual: Supermodel's (and so our
  old ones) are the same numbers laid out the other way round, halves
  swapped (PICR1 `0xFF000010` read as `0x0010FF00`, MCCR1, ESCR1/2, MCCR2/4,
  PICR2) and single bytes one register off (class code, cache line size,
  ODCR `0xCD` at `0x73`, its `0xB8` "check manual" byte, which is the
  alternate OS-visible register at `0xBA`, ErrEnR1 at `0xC0`). It made no
  difference to Spikeout: its boot code writes every bridge register before
  it reads one back, except the IDs and the revision, and its own writes
  (`0x0FFF0042` to `0xE0`, `0x20` to `0xE8`, the ODCR at `0x73`) match the
  manual's layout. Unknown: the chip's revision ID and MCCR1 bits 16-19
  (configuration pins), both 0.
- **Checked: 603e exceptions and timers** against the 603e user's manual:
  SRR1[16-31] from the MSR with the cause above; on entry the MSR keeps
  ME, IP and ILE, takes LE from ILE and clears the rest; `rfi` restores
  MSR[16-23, 25-27, 30-31]; the trap bit; DEC raising an exception when
  bit 0 goes from 0 to 1, by counting or by a write. The time base and
  decrementer run at the bus clock / 4 (Supermodel scales them per game).
- **Checked: address translation.** The game runs with MSR[IR, DR] on. Its
  BATs map `FE000000`-`FFFFFFFF` and the tile generator's `F1000000` to
  themselves; everything else goes through page tables, which it keeps in
  two halves: instructions at SDR1's `0x740000`, data 128 KB above (its own
  TLB miss handlers, installed over the boot ROM's at `0x1000`-`0x1200`,
  are the 603e manual's with `addis 2` for data). Every valid entry maps a
  page to itself (126 instruction pages; 1,743 data pages up to
  `C2000FFF`), and every data access in the attract mode finds one, so
  treating addresses as physical gives the hardware's results (`gamerun`
  prints the BATs and both tables). Not modelled: the TLBs themselves (64
  entries, 2-way): a model of the data TLB misses about 290 times a frame,
  about 10,000 cycles in the miss handler, under 0.5% of the frame; and
  the referenced/changed bits those handlers set in the tables.

## Needs real-board evidence

- **IRQ acknowledge** through Tilegen register `0x10`: Supermodel clears
  all 8 written bits, MAME only the low 4. Moot for Spikeout: it writes
  `0x6E` once at start-up, when none of the upper bits is pending, and then
  only `0x02`, `0x04` and `0x08`. It never acknowledges `0x01`, which stays
  pending, outside the enable mask (`0x6E`), for the whole run.
- **System register `0x1C`** reads `0xFF` because games "apparently expect"
  it; the real meaning is unknown.
- **IRQ `0x02` at the start of line 383**: assumed from System 24 (Lost
  World needs it); not measured on Model 3.
- **Real3D ping-pong flip** at the line in Tilegen register `0x08`: stands
  in for "the Real3D finished the frame", which really depends on how long
  rendering takes.
- **Real3D DMA** (improved): the data moves at once, but the copy only
  completes (busy bit 7 drops, the IRQ if asked for) after the time the PCI
  bus needs to carry it, one 32-bit word per 33 MHz clock at best; a copy
  started while one is going follows it. Supermodel and MAME complete it at
  once (`--dma-instant`; MAME holds busy 50 µs after a DMA command). This
  is a lower bound: the real engine's rate isn't known. Spikeout copies
  about 27 KB a frame (up to 375 KB while booting) and reads the status
  register about 2.5 times a copy. The completion IRQ is non-maskable
  (`0x100`), as both emulators have it (Supermodel: NMI; MAME: the SCSI
  line, outside the enable mask). The ID command answers Mercury's
  `0x16C311DB` while PCI config space says `0x178611DB` (Supermodel's
  notes: Step 2.x games by AM3 expect this).
- **JTAG**: whether system register bit 7 is really TRST; 3D-RAM boundary
  scan isn't emulated (only BYPASS).
- **Frame loop**: Supermodel runs VBlank until IRQ `0x02` is acknowledged
  (emulator scheduling, not hardware); we don't, and should only if a game
  proves it needs it.
- **Clock** (improved): each basic block costs the cycles a model of the
  603e's dispatch and execution units gives it (`ppc::block_cycles`), not
  a flat 8 a block (Supermodel counts one cycle an instruction at a lower
  clock). Checked against a real board's recording of the attract mode:
  the black load between the title and the first demo room lasts about
  0.25 s there, 0.1 s with this model and 1.5 s with the flat count. Not
  modelled: cache misses, the time device accesses take on the bus,
  results that cross into the next block; all would make it slower, which
  is the way it still differs.

## Clock, sound link and 2D

- **Done:** the RTC runs on the emulated time from a start time the
  frontend gives it (Supermodel returns the host's clock on every read and
  ignores writes), so runs repeat, and the game can set it; 12/24 hours,
  STOP, RESET and 30-second adjust follow the datasheet. BUSY is never
  set and the interrupt output isn't emulated.
- **Done: MIDI IRQ timing** from the port's serial chip (`m3::Usart8251`,
  the uPD71051): IRQ `0x40` is its TxRDY pin, a byte goes out every 320 µs
  and reaches the sound board with that spacing. Evidence: Spikeout
  programs the chip by the 8251 datasheet (reset sequence, mode `0x4E`) and
  turns the transmitter off when its queue is empty, which only makes sense
  if the IRQ is TxRDY. Supermodel raises the IRQ every 1000 cycles at the
  start of VBlank (at most 129) and sends each byte at once; that stays
  as `--midi-burst`. Assumed: CTS held low (MAME); IRQ `0x40` stays
  pending while TxRDY is up, even when acknowledged. Turning the
  transmitter off doesn't stop a byte already written (the 8251's
  behaviour, and the game writes its last byte and turns it off at once:
  holding that byte back left the attract mode's music muted for 3.7 s).
- **Tile generator**: lines are drawn at the start of each display line,
  as Supermodel does; the colour offset's channel order (red in the low
  byte, Supermodel's code against its own comment) is confirmed by a real
  board's recording: the title fades in from a white layer with offset
  `0x00808000`, which is pure red this way and would be pure blue the
  other, and the recording shows red; which
  layer the line mask's first 16 pixels belong to (overscan) is a guess in
  both emulators. Composite is 2D only: black, the layers below the 3D,
  the layers above.

## Real3D (3D)

The software renderer (`m3::Real3dRenderer`) draws the scene in memory at
the end of each frame. It follows Supermodel's New3D engine, compared
frame by frame against Supermodel 0.3a on the attract mode (both from
empty NVRAM, so the demo runs the same):

- **Scene walk:** frustum culling by each node's bounding sphere against
  the viewport's planes (words `0x0C`-`0x13`); level of detail with the
  node fading out past its delete size and two neighbouring levels
  cross-faded; model scale (node word 1) applied to the cull and blend
  radii.
- **Polygons:** one-sided polygons facing away aren't drawn; two-sided
  ones show their back, lit from that side. Texture alpha cuts out texels
  only with the contour bit (alpha test below 32/255) and blends only with
  the polygon's texture-alpha bits; otherwise the texture is opaque. (Our
  first renderer cut out every format-0 texel with its T bit clear, which
  riddled the fighters' skin with holes.) Triangles are clipped against
  the near plane and the view's sides (with a margin), as a GPU does; our
  first renderer clipped only at the near plane and dropped the huge
  triangles that left, which lost ceilings, floors and walls close to the
  camera.
- **Drawing order:** each priority is drawn twice, its ordinary and then
  its high-priority polygons (header 6 bit 4), each from a fresh depth
  buffer: the opaque pass, with a viewport's layered polygons (decals,
  bit 3) after the rest of it, depth scaled by 1 + 1/512 and one per pixel;
  then the translucent polygons into one of two translucency layers (bit
  17). Within a layer the nearest translucent polygon replaces what is
  there, without blending (the chip's translucency is two screen-door
  patterns), so overlapping shadows don't darken each other; an opaque
  pixel clears both layers; at the end one layer blends by its alpha and
  both together mix to an opaque colour.
- **Textures:** bilinear, with mipmaps from the levels stored in texture
  memory, blended between two levels; the level from the polygon's
  texture NP value and its distance, as the hardware does; repeat, clamp
  and mirror per axis.
- **Lighting:** per pixel, from the interpolated normal: sun, ambient,
  fixed shading, specular (smooth: a power of the sun's angle by
  shininess; flat: the reflected sun's z), no upper limit before the
  final colour clamp (Step 2); the spotlight (a screen ellipse with a
  depth range) and the spotlight on fog. Spikeout leaves the spotlight's
  colours at 0 (off) in the attract mode and the first stage.
- Fog per viewport by each polygon's fog intensity (with a fog start
  alone, too). Scroll fog and ambient fog are drawn over the 2D layers
  before the scene (Supermodel's guess at the hardware's rule for which
  viewport supplies them); Spikeout uses scroll fog in most frames.
- Also as Supermodel: normals divided by the node's model scale rather
  than normalized, signed bytes read as (2b + 1) / 255, the translator map
  (colour x 16), inverted textures, microtextures, format 7 without the
  alpha test, viewports in whole pixels, translucency layers over the 2D
  with straight alpha. `--quads` draws quads whole with mean value
  coordinates (Supermodel's optional quad rendering); by default they are
  two triangles, the same picture for flat, linearly mapped quads.

Checked again line by line against Supermodel's New3D (shaders, polygon
header, mesh pass selection, node walk, viewports, frame buffers) on
2026-10-20; three fixes, in both renderers (they share the scene):

- **Texture level, sign:** Supermodel's level is
  `-log2(gl_FragCoord.w^2 * base)`, `gl_FragCoord.w` being 1 / distance,
  so one level coarser each time the distance doubles. We had transcribed
  it with the distance squared instead of its inverse: close-ups took a
  coarser level than Supermodel (a fighter's face in the attract mode at
  level 1 instead of 0, visibly blurred) and far surfaces a finer one. A
  bug, so no switch for the old way.
- **Pass of a polygon with one discard bit:** it is half as opaque, but
  only the header's transparency bit and node alpha send a polygon to the
  translucency layers (Supermodel's `Mesh::Render`); we also sent the
  half-opaque ones.
- **Fade without a four-level LOD table:** used as it is; Supermodel
  snaps a fade to 0 or 1 within 1/32 of them only for four-level tables.

Found on 2026-10-21, comparing the character select screen with a
Supermodel build (commit 77d28ee, the release in use) whose shader wrote each
pixel's texture level, then its texture coordinates, then its raw texture
colour, frame-matched by Spike's outline:

- **Texture level, two too coarse:** levels and coordinates matched
  Supermodel exactly, but the colours did not. Our quick log2 (both
  renderers) took the float's exponent less 127 where its polynomial already
  adds 1, so every log2 was 1 too high and every texture level 2 too coarse:
  posters, trees and small lettering blurred next to Supermodel. With the
  exponent less 128, Spike's pixels match Supermodel's to 0.25 of 255 on
  average (2.5 before). A bug, so no switch for the old way.

Left as they are: Supermodel stores a polygon's transparency as a byte
(`t * 255 / 32`, so `t = 1` falls under its 1/32 cut-off); we keep
`t / 32`, as nothing says the hardware rounds that way. Supermodel wraps
texel addresses within each mip level's part of a page, we within the
whole texture sheet (a difference only for textures at a page's edge).
Supermodel draws a model's polygons grouped by their attributes, for
OpenGL; we draw them in memory order. Where a pixel centre lies exactly on
an edge shared by two polygons, the software renderer draws both and the
GPU (as OpenGL in Supermodel) one.

Still different: Supermodel's lighting is its own estimate of the
hardware (per-pixel; the real chips may light per vertex); the Step 1.5
luminous rules; microtextures; translucency patterns; the polygon RAM's
"layered" polygons; line-of-sight reads. `gamerun --probe X,Y@FRAME`
lists every triangle at one pixel and what became of it, for comparing.

Timing: writes to high culling RAM and polygon RAM after the ping-pong
flip go into the update buffers in low culling RAM and are applied at
VBlank, after the frame is drawn, as Supermodel does (`m3::Real3d`).
Frame signals, as current Supermodel has them: the command port (the CPU
wrote the frame) and a write to tile generator register `0x0C` (it draws a
frame). When both come before the flip, or at VBlank for the ones after,
the frame takes block culling from the Mercury ASIC's mode word (bit 2,
set through JTAG), which replaces the 3D with white, as Supermodel draws it.
Both emulators, and we, still draw every frame. Spikeout sets block culling
three times in the attract mode and the start of a game (boot, the "TEAM
SPIKEOUT PRESENTS" title, the load before the stage), each time under a
full-screen 2D layer, so it doesn't show. The Mars mode word never sets
bit `0x40000`, so the sun stays clamped at 0 (Supermodel's sun clamp), as
our lighting always has it. `gamerun` prints the five mode words.

## Sound board

`m3::SoundBoard` and `m3::Scsp` are adapted from Supermodel's SCSP (ElSemi's,
through MAME's conversion). Changes made on the evidence of the board or
the chip's documented register layout:

- **Done:** 512 KB of RAM per SCSP (MAME's map of the board; Supermodel
  has 1 MB). MDL and PLFOS read at their documented widths (Supermodel
  widens each by a bit as tuning: "a value of 30 is somehow needed for
  correct FM mix in VF3"); FM depth uses MAME's scale for the real field.
  Reading the MIDI status byte doesn't take a byte from the FIFO, and the
  FIFO's empty/full flags read correctly (Supermodel takes a byte on either
  half and never sets the flags). Timers reload all 8 bits (Supermodel
  drops bit 0). Idle slots start released, so the first key-on starts them.
  The LFO noise table is a fixed sequence (Supermodel uses `rand()`), so
  runs repeat.
- **Done: MIDI timing:** each byte reaches the SCSP's FIFO when its stop bit
  has gone out on the main board's serial line, 320 µs apart (`gamerun`
  runs the sound board a frame behind the main board, each byte at the
  same point of its frame; the window keeps their spacing on the audio
  device's clock). Supermodel passes a frame's bytes at the start of the
  sound board's frame. One byte at a time showed two bugs in the SCSP
  taken from Supermodel, both fixed as MAME has it: the MIDI interrupt
  stayed pending only until its level was next worked out, not while the
  FIFO holds a byte; and the register read gave the FIFO's flags after
  taking the byte, so the last byte read as "empty". The 68000's interrupt
  line is also updated as soon as it touches the interrupt registers or the
  FIFO, not only at the next sample, so a handler that returns quickly
  isn't taken a second time (that read an empty FIFO as a byte).
- **Sync:** in the window the sound board runs on the audio device's
  clock, as the real one runs on its own crystal, and takes each frame's
  MIDI bytes when the main board finishes the frame: a command plays up to
  a frame (plus the device's buffer) late, and a slow main-board frame
  delays commands instead of making the sound crackle. `gamerun` runs it a
  frame at a time after each main-board frame (as Supermodel does), so runs
  repeat exactly.
- **Checked against Sega's SCSP user's manual** (1997): timers count up
  every 1-128 samples and interrupt at the overflow, `256 - TIM` counts
  (5.805 ms from `00` at one a sample); timers B and C, MIDI out and the
  per-sample interrupt share the level in bit 7 of SCILV0-2 (Supermodel's
  "B and C share B's level"); MIDI input is pending until the FIFO is
  empty; KRS `0xF` turns key-rate scaling off; LPSLNK holds the envelope at
  its peak until the loop start. Fixed from it: SCIPD takes only bit 5 (the
  CPU interrupt) from a write, where the whole word was stored; the level
  comes from every pending, enabled source (bits 0-10, MIDI out empty and
  each sample now pending too), not just MIDI and timers; LPSLNK reaching
  the loop start under the decay level stays in decay 1 (Fig. 4.11).
  Spikeout enables only MIDI in (level 3) and timers A (2) and B (1), so
  its sound is unchanged.
- **Real-hardware evidence through MAME:** acknowledging (SCIRE) a timer
  that has overflowed and not been reloaded makes it pending again at once
  (MAME's note, from Sakura Taisen on a Saturn), which is how we and
  Supermodel have it; the timer stays at `0xFF` until reloaded.
- **Also fixed from the manual:** a rate of 0 is no change whatever the
  key-rate scale ("when AR = 00H the amount of change is 0"); Supermodel and
  MAME added the scale to it. SSCTL 1 now plays noise and 2 silence through
  the slot's usual envelope and levels (Supermodel played nothing and left
  the slot's envelope stopped, so it never ended); the noise algorithm
  isn't known (MAME: random numbers; here a fixed sequence). Spikeout keys
  no noise slots, and its rate-0 stages already came out at 0, so its sound
  is the same.
- **Unverified:** the DSP's memory access only on odd steps.
- **Outputs:** the board has two stereo outputs, SCSP 1 at the front (CN8,
  with the music) and SCSP 2 at the rear (CN7), from Supermodel's notes on
  the cabinet diagrams of other Model 3 games; Spikeout's own wiring isn't
  known. `SoundBoard::render` gives the four channels, or their average
  as stereo (Supermodel's downmix), which is what plays for now.

## Music board (DSB2)

`m3::Dsb2`: the board's own 68000 runs its program; the decoder is a
black box modelled by its commands (Supermodel's decoding) and minimp3.

- **Done (board):** commands come over the 31,250-baud serial line, one
  byte every 320 µs, each raising IRQ 1 until the 68000 reads it from the
  8251 (MAME's map of the board); Supermodel hands the board a frame's
  bytes in a burst, 500 cycles apart, and its status register always says
  a byte is there.
- **Clock:** 11 MHz, from the board's 33 MHz crystal / 3 (MAME's board
  notes; Supermodel uses 11 MHz too, MAME's device 8 MHz "unknown").
- **Kept from Supermodel, unverified:** IRQ 2 as a 1 kHz timer; the
  meaning of the decoder's volume and stereo commands (`0xB0`/`0xA1` and
  others: which speaker each sets is marked "?" there); a start address
  sent while a track plays makes the loop; decoder status always 1.
- **Mixing (Supermodel's):** the music is added to SCSP 1's output, the
  front pair, at the decoder's volume. We first routed it through the
  SCSPs' external inputs (MAME's note that it "should be chained with SCSP
  EXTS"), but Spikeout never sets the effect levels of slots 16/17 that
  carry those inputs: the "levels" seen there were sound effects playing on
  voices 16/17, so the music came and went with them. The game starts the
  attract mode's first track at 12.1 s, as "TEAM SPIKEOUT PRESENTS" fades
  out (a real board's recording agrees), with every decoder volume at 0,
  and sets them to `0x5A` in the same burst of commands.
- **Balance:** unknown on a real cabinet (each board has its own output to
  the amplifiers). Supermodel takes the SCSP's 18-bit output (DAC18B, which
  Spikeout sets) unscaled, four times MAME's level, so its sound effects
  sit 12 dB higher against the music than MAME's; we follow Supermodel.
  `--sound-volume` / `--music-volume` set them apart.
- Linear interpolation from 32 to 44.1 kHz.

## Settings EEPROM and security board

- **Done (datasheet):** the 93C46's dummy 0 bit before READ data. Supermodel
  leaves it out (its notes say shifting in an extra bit "causes problems");
  we show it on DO after the last address bit, and D15 comes with the next
  clock, as the datasheet has it. A write refused because writes are
  disabled starts no programming cycle.
- **EEPROM programming time** (improved): busy in real time on the CPU's
  clock, 1.75 ms a word, 1 ms an erase, 8 ms for all words (MAME's generic
  93Cxx times; Supermodel: 5 DO reads, microseconds). The board's part is a
  "93C46A" (MAME's board notes) of unknown make, and makers' datasheets
  give 2-10 ms, so the exact time is still a guess; milliseconds isn't.
  Writing a blank EEPROM's defaults now takes the game about 0.1 s more.
- **Kept between runs** (2026-10-21; as a cabinet, and Supermodel's NVRAM
  folder): the window saves each board's EEPROM and backup RAM and gives
  them back at the next start, so the operator's settings and the rankings
  stay (before, every run was a new board). Test runs (`--frames`) and
  `gamerun` still start blank, so they stay repeatable. The settings' layout
  and checksum (CRC-16-CCITT of words 6-34 in word 3) were found through the
  test menu (`tools/arcade_settings.hpp`); the window's Arcade settings page
  writes them as the test menu does (both copies, the checksum, then a
  restart; it leaves the count of saves, word 6, as it is), and the game
  keeps them, as checked by reading them back after it starts.
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
or a real board: 68000 at 10 MHz; IRQ 5 once per main-board frame (see
below; MAME's free-running 100 Hz stays as `irq5_per_frame = 0`); link
status registers read 5; comm RAM byte order Supermodel's
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

What the firmware's handlers settle (`hardware-notes.md` has the detail):

- **IRQ 2** is a command from the main board: at boot the handler reads
  status 0 and takes the command from it, so it follows the host's write
  to status 0 (`0x88`), as MAME and we have it.
- **IRQ 5** is a tick (flag, counter, LEDs; in link mode it also swaps the
  comm RAM bank and posts its counter at comm RAM offset 6). It has to be
  locked to the video frame: the game checks lock-step every frame
  (`0x7425C`: each cabinet's frame counter, offset 476 of its node, must
  be the game's own minus 1, or "Communication error !! (Sync_sys_broken)"),
  and with MAME's free-running 100 Hz a lone master shows that error in the
  attract mode within 30 s, while with IRQ 5 once per frame (or four times,
  Supermodel's number) it plays on (5,125 frames, into a stage). We raise
  it at the end of each main-board frame (`CommBoardOptions::irq5_per_frame`,
  1). What drives it on a real board (a video sync line?) isn't known.
- **Link status 5** has none of the bits the handlers treat as errors
  (receive `0xB8`, transmit `0x02`, interrupt status `0xA0`), and the
  firmware itself writes 5 to the transmit status and to `0x81`, so 5
  reads as "fine": consistent, though the other bits' meanings are still
  unknown.
- **Comm RAM byte order:** the game reads and writes comm RAM only with
  byte-reversed 32-bit accesses (`lwbrx`/`stwbrx`), and the firmware copies
  32-bit values the game gave it (`$500`, `$504`) into each frame header
  with `move.l`. For those to arrive as the same value the board must
  reverse all four bytes: Supermodel's `n ^ 3`, which we use. MAME's
  `n ^ 2` would swap their halves. **Confirmed running the link:** the
  game reads the firmware's tick counter with `lhz` from `0xC0000004`
  (`0x6FD3C`), a big-endian halfword load, and the firmware stores it
  byte-swapped (`rol #8`) at 68000 offset 6. With `n ^ 3` the game gets the
  counter (`0x022A`, `0x022C`, ...); with `n ^ 2` it would get it
  byte-swapped, and the firmware's `rol #8` would have no reason to be there.
- **Banks: settled by linked boards.** In link mode IRQ 2 and IRQ 5 swap
  the bank, and the frame header is written once in each bank. A ring of
  one runs the same either way, but with two boards (`gamerun --boards 2`)
  only one choice works: when the main board sees the bank the 68000
  selects (MAME), both cabinets leave NETWORK CHECKING and play together;
  when it sees the other one (`--net-other-bank`), the link runs (status 1
  `0x2041`/`0x2042`, frames every tick) but neither game ever leaves
  NETWORK CHECKING (5,188 frames).
- **A slave's tick (IRQ 5) comes from control register `0xE0`.** The
  slave's IRQ 5 handler (`0x2056`) clears D4 without saving it, and D4
  counts the polls since the slave armed its receiver: when the master's
  frame comes in, D4 must be under 2 (`0xAE0`), or the slave answers
  `$6502` instead of `$68xx` and the master never gets past its first
  command. So the tick has to come right with the master's frame, not from
  the slave's own (unsynchronized) video frame. The slave's IRQ 6 handler
  (`0x20C0`) writes `0xE0` exactly when it is in the phase that needs a tick
  (`$60E` = 2), and nothing else writes it; we raise IRQ 5 on that write
  (`CommBoardOptions::irq5_on_e0`; `gamerun --net-e0-irq5 off` turns it
  off). The master never writes `0xE0`; its tick stays once a main-board
  frame. MAME's TODO calls IRQ 5 the "data frame exchange cycle start
  signal on MASTER", source unknown.
- **Transmit complete waits for the frame being received.** In the data
  phase each side sends with its receiver armed, waits for transmit
  complete (IRQ 4), then looks for the other side's frame and gives up if
  its tick flag (`$608`, never cleared in the slave's data loop) is set
  (`0xC94`, `0xEF4`). The slave sends its 12,800-byte frame as soon as it
  has answered the master's small one, the master only after that answer
  reaches it, so with IRQ 4 after each frame's own time on the wire the
  slave always gives up (status 1 bit `0x40`) in the first exchange. With
  IRQ 4 held until the armed receiver has its frame too, as on a ring whose
  transmitters are clocked by what they receive, rings of 2, 3 and 4 run
  (`CommBoardOptions::send_waits_for_receive`; `--net-send-waits off`
  restores the other). The wire time itself is a guess, 4 MB/s
  (`link_cycles_per_byte`, `--link-byte-cycles`).
- **Linked play:** with board 0 set to master and the others to slave
  (setting 2), rings of 2, 3 and 4 come up (status 1 `0x2021 + 0x20 *
  (n - 1) + index`, index from 1), stay up through attract mode and
  play, and a 2-player game shows each cabinet the other's player.
  Registers seen but not understood: control `0xC0` (`0x7FFF` with each
  small frame, a per-node table value with the data frame: a timer?).
- **A lone cabinet** with the default settings uploads the firmware,
  checks the board and keeps it in reset; the link vectors are never
  installed. The link setting is the byte at `0x10019E` (`-32354(r13)`; the
  test menu steps it 0-2, 0 being stand-alone): with
  `gamerun --poke 10019E=1@0:2000` the game starts the board, shows
  "NETWORK CHECKING / THIS IS MASTER SITE", brings the ring of one up
  (status 1 `0x2021`) and plays as a linked master.
