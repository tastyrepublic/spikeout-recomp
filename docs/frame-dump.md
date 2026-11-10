# Comparing our 3D with Supermodel's (2026-11-10)

Supermodel is this project's first hardware reference, and its 3D changes often. To follow it, both programs can draw
the very same frame: a **3D frame dump** holds everything the Real3D draws a frame from, and each side draws a dump
from the other. When they differ, the dump says on which side the difference is: the game's scene (what the emulated
game put in the Real3D's memory) or the drawing (what a renderer makes of the same memory).

Dumps hold the game's models and textures as it uploaded them (from the ROM set): **never commit them**, as save states.

## The dump (`.m3d`)

Every number little-endian:

| Field | |
|---|---|
| `M3FRDUMP` | 8 bytes |
| version | u32, 1 |
| game mark | u32: the game's 315-5881 key (our `GameSpec::security_key`, Supermodel's `Game::encryption_key`) |
| flags | u32: bit 0, block culling (the Mercury mode word's bit 2) |
| configuration registers | u32 x 4 (0x9C000000: the update buffers' sizes) |
| sizes | u32: words of low culling RAM (0x100000), high culling RAM (0x40000), polygon RAM (0x100000); texture width, height (2048, 2048) |
| the memories | in that order: 32-bit words as the Real3D reads them (after the bus's byte swap), then the texels, 16-bit, row by row |

About 17.8 MB. Our side: `m3::Real3d::frame_dump` / `load_frame_dump` (src/runtime/real3d.cpp). Supermodel's:
`CReal3D::WriteFrameDump` / `LoadFrameDump`, added by `tools/supermodel/frame-dump.patch`.

## The tools

- **`gamerun --dump-3d FILE@FRAME`**: writes the dump of FRAME (board 0, or this machine's in network play). Only when
  asked: nothing else changes.
- **`frame3d ROMS DUMP [--renderer gpu|software] [--scale N] [--png OUT] [--models OUT.txt]`**: draws a dump with our
  renderers (the 3D alone, over black) and lists the models drawn (address, viewport, polygons, eye-space position,
  path hash).
- **`tools/supermodel/build.ps1 [-Commit 77d28ee|master]`**: Supermodel's source at that commit, the patch applied,
  built with this PC's WinLibs GCC (SDL2, SDL2_net and zlib fetched once), with a copy of the owner's Supermodel
  settings, into `Z:\Emu\supermodel-build\run` (outside the repository). Run it again after each Supermodel update; if
  the patch no longer applies, it says so (their code moved: the patch is updated by hand).
- **`tools/supermodel/draw.ps1 -Dump D -Out BASE`**: Supermodel draws the dump (`SUPERMODEL_DRAW_3D`), saves
  `BASE.png` and its model list `BASE-models.txt` (`SUPERMODEL_MODELS_OUT`) and quits.
- **In the patched Supermodel**: with `SUPERMODEL_DUMP_3D=1` set, Alt+S (its screenshot) also writes the dump beside
  the screenshot, for scenes reached in Supermodel.
- **`tools/compare-supermodel.ps1 -Dumps ... | -Scenes STATE@FRAME[@INPUTS] ...`**: each scene drawn by both,
  ours | Supermodel's | the differences (x4) side by side, the share of the picture that differs, and the models only
  one side drew; a report in `build\compare\index.html`.

A dump drawn alone has no 2D layers: where no 3D is drawn (the sky behind it) the two programs fill differently
(ours its fog colour, Supermodel's dark), which the picture's share counts. The model lists are the sharper test.

## First finding: the guard rails (issue #6)

The roadside guard rail beside Spikers' camp (stage 1, the slope above the road) was missing in both our renderers.
Supermodel, drawing our own dump of that frame, drew it: the game's scene was right, our scene builder wasn't. Its
model list had 33 models ours didn't (0x85E6E8 on): the rail's segments, at the end of a long chain of sibling nodes
(the road's roadside objects). Our walk counted each sibling as a level deeper and stopped at 64; Supermodel counts only
the nesting (children), up to 1024 (Model.cpp, `StackLimit`). Fixed: siblings walk at the same depth, the limit
1024 (src/runtime/real3d_scene.cpp). Also taken from Supermodel then: a four-level detail table's entries are culling
nodes when the node's word 3 has bit 29, else models (it went by the child pointer's bit 24). After both, the model
lists matched Supermodel's one for one in every scene tried (four: the slope, the car park, two of the owner's states).

## Over the whole game, and Supermodel's newest (2026-11-10)

197 scenes (our save states and the bot's runs through every stage, one and two players; a state of linked boards is
run as 2-4 of them, board 0's dump) drawn by ours and by Supermodel 77d28ee, then again by Supermodel's master
(65ec91d, built with `-Commit master -Work Z:\Emu\supermodel-master`): the model lists the same in every scene, with
both. Master changed how a four-level detail table's entries are told apart (a8297d4: the child pointer's bit 24, where
77d28ee and ours read the node's word 3 bit 29); in these scenes it makes no difference, and master's pictures are
77d28ee's (0.001% of the picture on average). The share of the picture that differs (7% on average, 25% at most) is
the sky behind the 3D (no 2D in a dump) and thin edges.

## Next: our save states in Supermodel's format (planned)

So that Supermodel can be put at any of our save points without playing there. A converter, `tools/state2supermodel`,
would write a Supermodel save state (its `CBlockFile`: named blocks, one per device) from ours (`m3::State`):

1. **Copied straight**: the PowerPC's RAM, the tile generator's memory and registers, the Real3D's memories (the same
   words as the dump), the backup RAM and EEPROM.
2. **Mapped field by field**: the PowerPC's registers and timers (its `ppc_save_state` layout), the MPC10x bridge, the
   interrupt controller, the Real3D's registers and mode words, the 53C810's state.
3. **The hard part**: the sound boards (68000, SCSP, the music board) and the network board keep their own emulator's
   bookkeeping, which ours keeps differently; a first converter may leave them as Supermodel starts them (no sound for a
   moment, or a reset of the link) and say so. A converted state is checked by its first picture: Supermodel's, drawn
   from it, against ours at the same frame (the compare report).
4. Supermodel's state format is that of one version (77d28ee first); the converter names the version it writes for.

Until then: scenes reached in Supermodel by the owner (its save states, F5/F7) give dumps with Alt+S; ours come from
our states or the bot's recordings.
