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
  Spikeout relies on this: a routine at `0x6FAC8` probes the security
  device and shows "ILLEGAL ROM" if it gets 0.
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

## Tilegen text

Name tables at `0xF10F8000`, one per layer, `0x2000` bytes apart: 64 × 64
entries of 16 bits, little-endian on the tile generator side, so each CPU
word holds two entries in swapped order. Spikeout's font puts ASCII at
tile numbers `0x20`–`0x7E`, which makes its messages readable straight
from the name table (`gamerun --text`).

## Sound board link (`0xF0080000`)

`+0` MIDI data (write: send byte to the sound board, clears IRQ `0x40`),
`+4` MIDI control (bit `0x20` enables periodic MIDI IRQs; clearing it
clears IRQ `0x40`).

## RTC (`0xF0140000`)

Register *n* at `+4n`. Byte 1 of each word reads `0x03` (battery voltage
OK); a self-test checks it.

## Inputs (`0xF0040000`, 315-5649)

| Reg | Contents (active low) |
|---|---|
| `0x00` | input bank select (R/W) |
| `0x04` | bank 0: coin 1/2 (bits 0–1), test A (2), service A (3), start 1/2 (4–5). Bank 1: service B (6), test B (7), EEPROM data out (5) |
| `0x08` | **Spikeout**: beat (0), charge (1), shift (2), jump (3); joystick down (4), up (5), right (6), left (7) |

**One player per cabinet.** Spikeout's multiplayer is up to four linked
cabinets, one player each — not several players on one board.

## Security board (`0xF0180000`, 315-5881)

- `0xF0180000`–`0xF019FFFF`: RAM the decrypter reads.
- `0xF01A0000` registers: `+0x00` status (reads 0); `+0x10`/`+0x14` reset
  the decrypt address; `+0x18` sets the sub-key (byte-swapped high half);
  `+0x1C` data — first read after a reset returns `0xFFFF0000`, later reads
  return decrypted words in the high half.
- Key for Spikeout Final Edition: `0x29236FC8` (`Crypto.cpp` implements it).

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
