# Model 3 Step 2.1 hardware notes

Behavior the runtime has to reproduce, collected from Supermodel
(`Src/Model3/Model3.cpp` unless noted). See [REFERENCES.md](REFERENCES.md).

## CPU

- PowerPC 603e "Goldeneye" (603R/603ev), PVR `0x00071202`.
- 166 MHz core: 66 MHz bus × 2.5. The time base / decrementer tick at a
  fraction of the bus clock.
- Starts at the system reset vector `0xFFF00100` (MSR[IP] = 1).

## Bus behavior

- **Unmapped reads return all ones** (`0xFF`, `0xFFFF`, `0xFFFFFFFF`).
  Supermodel says Spikeout shows "ILLEGAL ROM" if unhandled reads give 0,
  naming a routine at `0x6FAC8`; in Final Edition that address is network
  code, and the security check is the one described under
  [Security board](#security-board-0xf0180000-315-5881).
- 64-bit accesses are split into two 32-bit accesses, high word first.
- The `0xF0xxxxxx` system devices are also reached at `0xFExxxxxx`.
- Tilegen RAM (`0xF1000000`–`0xF111FFFF`, including palette) is stored
  little-endian on the tile generator side; the CPU sees it byte-swapped.
  A write to `0xF118000C` marks the end of a 2D frame.
- MPC106 PCI bridge: config address at `0xFEC00000`, data at `0xFEE00000`.
  Real3D's PCI ID: games that ask through the DMA device expect
  `0x16C311DB` even on Step 2.x (`Real3D.cpp`).

## Program ROM

After chip interleaving with the byte swap from `Games.xml`, Supermodel's
buffer is in the CPU's big-endian byte order (it only flips it afterwards
for fast host access). So our loader should detect word order `as-is` on a
good dump; anything else points to a bad or differently-ordered dump.

## System registers (`0xF0100000`)

| Offset | Read | Write |
|---|---|---|
| `0x08` | CROM bank register | CROM bank select: bank = `~value & 0xF`. Reset writes `0xFF` (bank 0) |
| `0x0C` | – | JTAG TAP: TCK bit 6, TDI bit 5, TMS bit 2, TRST bit 7 |
| `0x10` | JTAG TDO in bit 5 | – |
| `0x14` | IRQ enable | IRQ enable |
| `0x18` | IRQ pending | IRQ acknowledge (clears the written bits) |
| `0x1C` | `0xFF` | LEDs |

The Real3D chips' mode words are configured over JTAG (`Real3D.cpp`).

## Interrupts and frame timing

Refresh is **57.524160 Hz**, 424 lines per frame: 384 active, 40 blanking.
Per frame, Supermodel does:

1. VBlank: run the CPU until IRQ `0x02` is acknowledged; while MIDI control
   bit `0x20` is set and IRQ `0x40` is enabled, raise `0x40` (sound board
   MIDI) repeatedly, ~1000 cycles apart; run the rest of VBlank; raise
   IRQ `0x0D`.
2. Active display: 384 lines. At the line in Tilegen register `0x08`
   (usually 238/239) the Real3D ping-pong status bit flips — games use it to
   pace rendering. At line 383 raise IRQ `0x02`.

Known IRQ bits: `0x01`, `0x04`, `0x08` (end of VBlank group), `0x02`
(display/Tilegen), `0x40` (sound board MIDI).

IRQs are acknowledged by writing the bit to Tilegen register `0x10`
(`0xF1180010`, 32-bit, byte-reversed: Spikeout uses `stwbrx`), not through
`0xF0100018`; Spikeout's handlers then poll `0xFE100018` until the bit
clears. Only 32-bit accesses reach the Tilegen registers (Supermodel's
`Model3.cpp` byte-swaps them; 8/16-bit accesses are dropped).

## Real3D on the PCI bus, DMA device, ping-pong

- PCI config, bus 0, device 13: register 0 = `0x178611DB` (the Step 2.x DMA
  device). Spikeout's DMA init (`0x4F34`) checks vendor `0x11DB` and device
  `0x1786` before marking DMA ready; without it, `dma_memcpy` asserts.
- DMA device `0xC2000000`: `+0x0C` status (bit 7 busy, bit 0 IRQ),
  `+0x0D` IRQ acknowledge, `+0x0E` configuration (bit 0: IRQ on completion,
  bit 7: byte-reverse), `+0x00/04/08` source/destination/length (writing
  the length copies), `+0x10` command (`0x20000000`: ID, answers Mercury's
  `0x16C311DB`; `0x80000000 | reg`: read a status register), `+0x14`
  result. 32-bit registers are byte-swapped on the bus. Completion IRQ is
  `0x100` (non-maskable).
- Status register 0 (`0x84000000`, or the DMA command) = `0xFDFFFFFF`, plus
  `0x02000000` when the ping-pong bit is set. It flips at the start of the
  display line in Tilegen register `0x08`; Spikeout waits for it every
  frame.

## Real3D memory and scene

The CPU writes 32-bit words, which the Real3D sees byte-swapped (a
little-endian read of the CPU's bytes, as for Tilegen RAM). VROM, as the
ROM set is laid out, is little-endian words too (checked by decoding
Spikeout's models: the other order gives a fraction of the polygons).

| Address | |
|---|---|
| `0x88000000` | command port: a write ends the frame's uploads (flushes the texture FIFO) |
| `0x8C000000` | low culling RAM, 4 MB (matrices, nodes, LOD tables) |
| `0x8E000000` | high culling RAM, 1 MB (the viewport list starts at word `0x800000`) |
| `0x94000000` | texture FIFO: length word, header, 16-bit texels in 8x8 tiles |
| `0x98000000` | polygon RAM, 4 MB (models built at run time, colour tables) |
| `0x9C000000` | configuration registers (ping-pong buffer sizes) |

Word addresses in the scene: below `0x100000` low culling RAM or polygon
RAM, `0x800000`-`0x83FFFF` high culling RAM, models from `0x100000` up in
VROM. Texture RAM is two 2048x1024 pages of 16-bit texels; mipmap level
*i* of a texture at (x, y) is at (2048 - 2048 >> i + x >> i, 1024 - 1024 >>
i + y >> i). Formats of the viewport, culling node, polygon and texel are
summarized in `src/runtime/real3d_render.hpp` and `real3d.hpp`.

In Spikeout's attract mode a frame has about 10 viewports, 1,000 nodes, 600
models and 11,000 polygons; it uploaded 700 textures in the first 5,000
frames.

## JTAG (Real3D chips)

Step 2.1 chain, first to last: Jupiter, Mercury, Venus, Earth, 3D-RAM,
Mars, Mars, 3D-RAM ×3, Earth, 3D-RAM, Mars, Mars, 3D-RAM ×3. ASIC
instructions: 3 IDCODE (32 bits), 8 mode word (32 bits), 9 memory
configuration (Mercury, 57 bits), anything else BYPASS; 5-bit instruction
register (captures `0b00001`); 3D-RAMs: 4-bit instruction register
(captures `0b1001`), always BYPASS. ID codes: Mercury `0x416c3057`, Venus
`0x316c4057`, Earth `0x516c5057`, Mars `0x316c6057`, Jupiter `0x516c7057`.
Spikeout reports "INVALID CONFIG DETECTED" without them.

## Tile generator (2D layers)

The game writes Tilegen RAM little-endian (`stwbrx`): the tile generator's
32-bit word *i* is a little-endian read of the bytes at `0xF1000000 + 4i`
(palette entry 2 is white `0x7FFF`, a blank text tile is `0x0020`). Layout
and registers as in Supermodel's `Render2D.cpp` notes; summary in
`src/runtime/tilegen.hpp`. Four 64 × 64 layers of 8 × 8 tiles, 4- or
8-bit, in two pairs (0/1 and 2/3); a line mask picks one layer of each pair
per 32 pixels; each layer is above or below the 3D (register `0x20`);
`0x40`/`0x44` add a colour offset to each pair (signed 8-bit per channel,
doubled; MAME ignores these, Supermodel applies them). Name tables at
`0xF10F8000`, one per layer, `0x2000` bytes apart, two entries per word
(the left one in the high half). Spikeout's font puts ASCII at tile
numbers `0x20`–`0x7E`, so its messages read straight from the name table
(`gamerun --text`).

Spikeout uses layers 0 and 2 (primary everywhere), 8-bit tiles: text and
logos on 0, full-screen backgrounds and fades on 2. Its logo fades in by a
white layer 2 with a colour offset moving from `0x00808000` to 0.

## Sound board link (`0xF0080000`)

`+0` MIDI data (write: send byte to the sound board, clears IRQ `0x40`),
`+4` MIDI control (bit `0x20` enables periodic MIDI IRQs; clearing it
clears IRQ `0x40`). Reads: `0x00` and `0x83`. Spikeout queues its sound
commands (`0x5830`, a 32-entry ring buffer) and sends them a byte per MIDI
IRQ; without the IRQs the queue fills and the game waits. Supermodel
raises IRQ `0x40` at the start of VBlank every 1000 cycles while bit
`0x20` and the IRQ enable are set, at most 129 times; so do we.

## RTC (`0xF0140000`, Epson RTC-72421)

Register *n* at `+4n`, in the top byte of each word: 0–12 the time as BCD
digits (seconds, minutes, hours, day, month, two-digit year, day of the
week), 13–15 control (HOLD, 30-second adjust, interrupt, RESET, STOP,
24/12 hours). Byte 1 of each word reads `0x03` (battery voltage OK); a
self-test checks it. Spikeout reads it ~12,000 times while starting.

## Inputs (`0xF0040000`, 315-5649)

| Reg | Contents (active low) |
|---|---|
| `0x00` | input bank select (R/W) |
| `0x04` | bank 0: coin 1/2 (bits 0–1), test A (2), service A (3), start 1/2 (4–5). Bank 1: service B (6), test B (7), EEPROM data out (5) |
| `0x08` | **Spikeout**: beat (0), charge (1), shift (2), jump (3); joystick down (4), up (5), right (6), left (7) |

**One player per cabinet.** Spikeout's multiplayer is up to four linked
cabinets, one player each — not several players on one board.

## Settings EEPROM (93C46 on the I/O board)

64 words of 16 bits, bit-banged through register `0x00` (CS bit 6, CLK
bit 7, DI bit 5) and read in bit 5 of register `0x04`, bank 1
(`m3::Eeprom93C46`). On a blank chip Spikeout writes its defaults: words
0–2 are the signature "M3SEGA", then the settings, with a second copy of
them from word `0x23`. With these settings the cabinet is a link master
and, alone, runs without starting the network board's 68000.

## Security board (`0xF0180000`, 315-5881)

- `0xF0180000`–`0xF019FFFF`: RAM the decrypter reads; encrypted word *i*
  is the top half of 32-bit word *i*.
- `0xF01A0000` registers: `+0x00` status (reads 0); `+0x10`/`+0x14` reset
  the decrypt address; `+0x18` sets the sub-key (byte-swapped high half);
  `+0x1C` data — first read after a reset returns `0xFFFF0000`, later reads
  return decrypted words in the high half.
- Key for Spikeout Final Edition: `0x29236FC8`. The cipher (MAME's, also in
  Supermodel's `Crypto.cpp`) is `m3::Crypt315_5881`.
- The game's check (Final Edition): `0x123B4` calls `0x7650C` with sub-key
  `0x9EF6`, the source data at `0x8D943` and the expected plaintext at
  `0x8D8E8`; `0x765F4` writes the source words with `stwbrx` to
  `0xFE180000 + 4i`, restarts the decrypter, sets the sub-key, discards the
  first data read, polls status bit 0 (busy) and reads the words back with
  `lwbrx`. A mismatch prints "ILLEGAL ROM" and stops. With the
  decrypter emulated the check passes.

## Network board firmware (uploaded by the game)

The game writes the board's firmware into net RAM (`0xC0020000`, the
68000's RAM at 0) and starts the 68000 through I/O register `0xC0`
(`0xC0010180`). Facts from Spikeout's firmware:

- Vectors: SSP `0xFF00`, PC `0x800`; IRQ 2 `0x22AA` (command from the main
  board), IRQ 4 `0x2260` (transmit complete), IRQ 5 `0x2056` (periodic
  tick: acknowledges and toggles the LEDs), IRQ 6 `0x20C0` (receive
  complete; sets a "frame received" flag at `$611`).
- Main loop `0x834`: waits for a command (set by the IRQ 2 handler from
  status 0), runs it from a table, writes the result to status 0 with bit
  15 cleared. Command 1 is the comm RAM test the game also does.
- Link role: 68000 word `0x400` (written by the game at `0xC0020800`):
  0 master, anything else slave. Spikeout derives it from a setting in the
  EEPROM; before the EEPROM was emulated it got `0x0020`, a slave.
- Status 1 as the game reads it: bits 13-15 link state (non-zero: still
  checking; `0xE000` at start), bit 11 "no carrier"; `0x2021 + 0x20 * n +
  index` when the ring is up. A slave arms its receiver and waits for a
  master's frame without sending; with none it reports no carrier.

## Network board (`0xC0000000`)

Present on Spikeout (`<netboard>true</netboard>`). Layout as seen by the
CPU (16-bit board on a 32-bit bus):

| Range | Contents |
|---|---|
| `0xC0000000`–`0xC000FFFF` | comm RAM, shared with the board's own CPU |
| `0xC0010000`–`0xC00101FF` | I/O registers |
| `0xC0020000`–`0xC003FFFF` | net RAM |

Spikeout reads comm RAM even in single-cabinet play. Supermodel has two
implementations in `Src/Network/`: `NetBoard.cpp` (emulates the board's
68000) and `SimNetBoard.cpp` (high-level; links cabinets in a ring over TCP,
`PortIn`/`PortOut`/`AddressOut`). Both contain Spikeout-specific
workarounds, so link play is not fully understood yet.
