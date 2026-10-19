# Model 3 Step 2.1 memory map (main CPU)

As used by Spikeout / Spikeout: Final Edition. Supermodel's
`Src/Model3/Model3.cpp` is authoritative (see [REFERENCES.md](REFERENCES.md));
MAME's `sega/model3.cpp` was used to cross-check. Regions marked (S) are only
mapped by Supermodel. The code version is `src/runtime/memory_map.cpp`.

Regions marked *mirrored* also respond with address bits 25–27 set
(`0xF2xxxxxx` … `0xFExxxxxx`) in MAME; Supermodel only implements the
`0xFExxxxxx` mirror, which is the one games are known to use.

| Start | End | Device | Notes |
|---|---|---|---|
| `0x00000000` | `0x007FFFFF` | Work RAM | 8 MB |
| `0x84000000` | `0x8400003F` | Real3D status | read |
| `0x88000000` | `0x88000007` | Real3D command port | write |
| `0x8C000000` | `0x8C3FFFFF` | Real3D low culling RAM | write (S) |
| `0x8E000000` | `0x8E0FFFFF` | Real3D high culling RAM (display list) | write |
| `0x90000000` | `0x900000FF` | Real3D VROM texture port | write (S) |
| `0x94000000` | `0x940FFFFF` | Real3D texture FIFO | write (S) |
| `0x98000000` | `0x983FFFFF` | Real3D polygon RAM | write; 4 MB per Supermodel, MAME maps 1 MB |
| `0x9C000000` | `0x9CFFFFFF` | Real3D configuration registers | write (S) |
| `0xC0000000` | `0xC003FFFF` | Network board | cabinet link |
| `0xC2000000` | `0xC20000FF` | Real3D DMA | Step 2.x |
| `0xF0040000` | `0xF004003F` | 315-5649 I/O (inputs, EEPROM) | mirrored |
| `0xF0080000` | `0xF008FFFF` | Sound board UART | mirrored |
| `0xF00C0000` | `0xF00DFFFF` | Backup SRAM | mirrored |
| `0xF0100000` | `0xF010003F` | System (IRQs, CROM bank) | mirrored |
| `0xF0140000` | `0xF014003F` | RTC | mirrored |
| `0xF0180000` | `0xF019FFFF` | Security board RAM | mirrored |
| `0xF01A0000` | `0xF01A003F` | 315-5881 decrypter | mirrored |
| `0xF1000000` | `0xF10F7FFF` | Tilegen character RAM | |
| `0xF10F8000` | `0xF10FFFFF` | Tilegen tilemaps | |
| `0xF1100000` | `0xF111FFFF` | Palette RAM | |
| `0xF1180000` | `0xF11800FF` | Tilegen registers | |
| `0xF8FFF000` | `0xF8FFF0FF` | MPC106 registers | PCI bridge |
| `0xFEC00000` | `0xFEDFFFFF` | MPC106 config address | |
| `0xFEE00000` | `0xFEFFFFFF` | MPC106 config data | |
| `0xFF000000` | `0xFF7FFFFF` | Banked CROM window | 8 MB of `banked_crom` |
| `0xFF800000` | `0xFFFFFFFF` | Fixed CROM | reset vector at `0xFFF00100` |

## Program ROM layout

- **Fixed CROM** (8 MB): four 2 MB chips (`epr-21653..21656`), 16 bits each
  on the 64-bit bus.
- **Banked CROM** (112 MB): four groups of four chips (CROM0–CROM3). The
  window at `0xFF000000` shows `banked_crom[bank * 8 MB ...]`. The bank is
  selected by writing the system register at `0xF0100008`: bank = inverted
  low nibble of the most significant byte. Bank 0 is selected at reset.
- **VROM** (64 MB): sixteen 4 MB chips on a 256-bit bus (Real3D textures).
