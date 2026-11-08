// gamerun: run the recompiled boot ROM into the recompiled game and report
// how far it gets.
//
//   gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE] [--dump-textures FILE.png]
//           [--dump-backup FILE] [--eeprom FILE]
//           [--netboard sim|real] [--net-trace N] [--net-disasm FILE]
//           [--screenshot FILE.png] [--shot-every N] [--press [B:]NAME@FRAME[:N]]...
//           [--wav FILE.wav] [--probe X,Y@FRAME] [--quads]
//           [--boards N] [--slice LINES] [--link-latency CYCLES] [--input-delay N] [--renderer software|gpu|compare]
//           [--gpu-scale N] [--gpu-time N] [--wide E] [--screen-time WxH] [--match-models] [--parallel] [--no-2d] [--no-sound]
//
// Built only when CMake's SPIKEOUT_ROM_DIR is set. A run ends at the first
// thing the runtime can't do yet (code that wasn't found, an exception, a
// device the game waits on), or after N basic blocks (default 200 million).
// That stop, the blocks before it and the devices the game touched are
// what to work on next. --text prints the text on the tile layers (what
// screen the game is on); --dump-tile writes the tile generator's RAM at the
// end (characters, name tables, palette; derived from the ROMs).
// --replay FILE plays a recording (spikeout --record; tools::Recording) from
// power-on: its clock, EEPROM and backup RAM, its inputs each frame, main RAM
// checked against its hashes (the report says if the run parted from it, and
// where), to its end unless --blocks says otherwise. --inputs FILE plays a
// run spikebot recorded (--record-inputs; tools::InputLog) from the state it
// began from (--load-state): board 0's inputs each frame, the bot's RAM
// writes, main RAM checked against its hashes, to its end. Linked boards'
// (spikebot --boards N; tools::LinkedInputLog) need --boards N --parallel:
// every board's inputs and writes where the bot made them (frame ends, the
// slices' meetings, points after a frame's end), each board's RAM checked.
// --save-state FILE@FRAME saves the whole board at the end of FRAME (in the
// first block after it outside an exception; tools::board_state), and
// --load-state FILE runs on from such a state (or the window's, F9) instead
// of from power-on; with --replay, the recording's inputs from there.
// --blocks then counts from the state. Loaded, a run is the same as the one
// that saved it, to the byte. With --boards N, one file holds every board
// and the packets on their way between them (m3::LinkRing::cut_state),
// loaded with the same --boards N.
// --watch-write LO-HI@FRAME[:N] (hex addresses) prints every write to
// [LO, HI) in N frames from FRAME (default 1): the block writing, LR and
// registers (finding the code that draws something; work RAM too).
// --dump-backup writes the backup RAM (128 KB) at the end. --dump-layers
// writes each tile layer's whole 512x512 name table with every screenshot
// (FILE-<frame>-L<n>.png). --map-trace FROM:N prints each tile map the game
// draws in N frames from FROM (m3::TileSides: its number, layer and rows).
// --eeprom [B:]FILE starts with that settings EEPROM (128 bytes, as the window's
// nvram/board<N>.eeprom; else a blank one, which the game sets up); with B:,
// board B's only (linked cabinets set up differently).
// --dump-textures FILE.png writes the Real3D's texture RAM at the end as a
// 2048x2048 picture, each texel read as T1RGB5 (the other formats show as
// noise; derived from the ROMs).
//
// The network board is its own 68000 running the firmware the game uploads
// (m3::CommBoard); --netboard sim uses the simulation instead. --net-trace N
// prints its first N accesses to the board's registers, --net-disasm writes
// a disassembly of its RAM (the firmware; derived from the ROMs).
//
// --boards N runs N complete boards (cabinets) in one process, their network
// boards linked in a ring (m3::LinkRing): board i sends to board i + 1. Each
// runs on its own thread, but only one at a time: they take turns every
// --slice display lines (default 8), in board order, so a run is the same
// every time. A packet arrives once the receiver's clock reaches the time it
// was sent (plus --link-latency main board cycles, default 0). --parallel
// runs the boards at once instead, meeting every slice, where the link's
// packets go on (still the same every run; the latency at least a slice).
// Network play (Mode B, tools::NetPlay, docs/netplay.md): --net-host NAME
// --seats N offers a game on the networks this machine is on and waits for
// N - 1 players; --net-join NAME finds it there (LAN discovery) and joins.
// Every process then runs every board --parallel, its own board's inputs
// (--press for it) sent to the others, theirs received; the same run, to
// the byte, as one gamerun --boards N --parallel with all the presses.
// --clock SECONDS sets the boards' clocks at power-on (else the host's
// time; the host's goes to all). --lobby URL goes through an internet lobby
// instead (a test's own, `wrangler dev` in lobby/): --net-host offers it
// there (--private: by its code only), --net-join CODE joins by the code.
// --load-state FILE (a linked state, the same file on every machine; part
// of the build id) starts network play from it instead of power-on.
// --net-one-board FRAME (every machine): one board a PC. Every board runs
// here up to the end of FRAME (Mode B), then only this machine's: the
// others are handed over to its tools::remote::Cabinets (tasks mode), made
// from their players' inputs as the input delay brings them (--one-board
// repeated, in one process, hands several boards over the same way). With
// --one-board-auto (two seats) the rules run the other board whole here as
// a shadow where side runs can't follow it, seeing its coin and start only
// as far ahead as the input delay (--one-board-lookahead N limits it in one
// process too; 60 by default). --one-board-held (with the rules): at the
// hand-over each other board runs on whole, as its shadow, until the rules
// find the game calm (the window's way: from power-on, through attract).
// --net-leave-at FRAME (with --net-one-board): this player leaves 30 frames
// after FRAME, its board's state sent to every other machine first, which
// runs its cabinet on whole from it (so a player can still join in progress).
// --net-vanish-at FRAME: this machine gone at the end of FRAME, no goodbye (as a
// crash); the others run its board on from its last backup (tools::backup).
// --net-rollback N (every machine): rollback, the boards running on up to N frames
// ahead with guesses for inputs not in yet, going back together when one was
// wrong (tools::rollback; frames run again draw and play nothing).
// --net-hold-inputs N: tests: this machine's inputs sent N frames' time late.
// --one-board-late (--parallel): nothing of it until FRAME, then set up at
// a meeting there, every board stopped (the window's Auto, when this PC
// finds it can't run every board at full speed).
// --no-frame-hashes leaves out the comparisons' RAM hash of every frame; --frame-hashes
// prints it in any run. An argument @FILE stands for the words in FILE (a long
// --press list: Windows' command line holds 32 KB).
// --no-2d and --no-sound leave out the tile generator's drawing and the
// sound and music boards: the game runs the same without them (nothing
// reads them back), as a remote player's cabinet would.
// Each board's
// link setting is its own: set it with --poke (board 0 master, board 1
// slave: `--poke 0:10019E=1@0:2000 --poke 1:10019E=2@0:2000`). --blocks
// counts for each board. Files from boards after the first get "-bN" before
// their extension; --net-disasm writes board 0's firmware. --link-trace N
// prints the first N packets; --link-dump FILE writes, for every data frame
// sent, the board, the frame and the sender's own node (its first 0x900
// bytes: comm RAM 0x100-0x9FF, what its main board wrote); --link-record
// B:FILE logs board B's main board's calls to its network board, and
// --link-standin B:FILE runs board B's network board from such a log, its
// main board not run (--link-standin-at CYCLES: each frame's writes made
// that long after its start); --camera-record B:FILE records board B's
// camera as each update starts, and --camera-test B:FILE runs the camera
// update on board B once more on the side for the other board's camera
// (docs/netplay.md, "What the cabinets send each other"); --net-watch prints each board's network
// status registers when they change. The link's open questions
// (docs/fidelity.md) have switches: --net-e0-irq5 off, --net-send-waits off,
// --net-other-bank, --link-byte-cycles C (68000 cycles a byte on the wire),
// --net-irq5-per-frame N, --net-slave-irq5-per-frame N, --net-irq5-phase F,
// and the ring model's parts: --net-ring-forwarding off, --net-double-buffer
// off; --net-old-link puts the link back as it was before the ring model
// (linked games at 2/(N+1) speed). --net-host-trace FRAME:N adds the main
// board's accesses to the network board, by time, to the report.
// --rollback-probe FRAME:N[:speed] measures what rollback would cost over N
// frames from FRAME (docs/netplay.md): each board's state saved every frame
// (timed), how much of it changed since the frame before (4 KB pages), a
// copy of it; :speed only times the frames (run with --no-2d --no-sound).
// --rollback-test FRAME:N[:DEPTH[:EVERY[:compare][:quiet][:timed]]] proves the
// rollback snapshots (m3::Snapshots): a snapshot every frame, back DEPTH
// frames every EVERY and on again, each frame's whole state hashed and
// compared when it runs again (:compare: pages compared, not listed).
// Linked boards (--boards N --parallel) go back together, with the link
// ring's packets on their way (its part hashed and compared too). :quiet:
// the frames run again draw and play nothing (rollback's re-runs), the
// sound board left out of the snapshots (it hears each frame once): the
// pictures (--renderer gpu's hash) and the sound (--wav) as without the test.
// :timed: nothing hashed or checked, to time rollback's own costs (the
// snapshots, going back, the frames run again).
//
// The tile generator draws a line at a time as the frame runs; the 3D is
// drawn at the end of a frame, only when screenshots are being written.
// --screenshot writes the last finished frame as a PNG; with --shot-every N
// also FILE-<frame>.png every N frames.
//
// --press [B:]NAME@FRAME[:N] holds an input for N frames (default 6) from
// FRAME: coin, start, test, service, beat, charge, shift, jump, up, down,
// left, right. Repeat it for more presses (--presses FILE: a list of them; --bot SEED[@FROM]: a player pressing
// all the time, random, on every board). --poke [B:]ADDR=VALUE@FRAME[:N]
// writes a byte of RAM; --peek [B:]ADDR:LENGTH@FRAME[:N] prints LENGTH bytes
// of RAM at the end of those frames; --ram-dump B:LO-HI@FRAME:N FILE writes
// board B's main RAM [LO, HI) at the end of each of N frames (after the
// frame and the logic counter at 0x1000A8, 32 bits each). Without B: they apply to every board. Every board
// reads its inputs from a table by frame (tools::InputTable), as network
// play will fill it; --input-delay N sets each one there N frames on, as
// delay-based network play does (the frames on their way are not in a
// save state).
//
// The sound board (m3::SoundBoard) runs with the game; --wav writes what
// it played (44.1 kHz stereo).
//
// --probe X,Y@FRAME draws the 3D at FRAME and lists every triangle that
// covered pixel (X, Y), with its polygon header and what became of it there
// (drawn, behind, cut out, blended): for finding holes and wrong pixels.
// --quads draws quads whole, as Supermodel's "quad rendering" option (slower).
// --fog-over-nothing draws scroll fog in frames with no polygons too, as
// Supermodel (the ranking after the staff credits: orange; m3::Real3dScene::background_fog).
//
// --renderer gpu draws the 3D on the GPU instead (gpu::Real3dGpu, read back
// for the screenshots); --renderer compare draws it both ways and reports
// how far apart the pictures are, with each screenshot's GPU version beside
// it (FILE-<frame>-gpu.png). --gpu-time SCALE (with --screenshot) also draws
// each frame on the GPU at that render scale, not read back, and reports how
// long the GPU took a frame (drawn 8 times running, so its clock stays up;
// the screenshots stay the software renderer's).
// --gpu-scale N draws the GPU's 3D at render scale N (the screenshots take
// every Nth pixel); the report gives a hash of every GPU picture, full size,
// for checking that a change to the GPU renderer draws exactly the same.
// --wide E (with --renderer gpu, scale 1) draws widescreen, E pixels wider
// each side (the window's 16:9 is 93), and writes each screenshot's whole
// picture as the window shows it too (FILE-<frame>-wide.png). The report
// gives the 2D layers' drawing time a frame, and with --wide the time
// making their sides (on average and the worst frame), as the window's game
// thread spends them. --screen-time WxH (with --wide and --screenshot)
// times the window's screen pass at that size, drawn off screen, widescreen
// and 4:3. --match-models builds each frame's scene and reports how many of
// its models are found in the frame before's (smooth motion's matching: by
// viewport, address and culling-node path).
#include "games/games.hpp"
#include "ppc/disasm.hpp"
#include "ppc/instruction.hpp"
#include "recomp/context.hpp"
#include "recomp/frame_timing.hpp"
#include "runtime/bus.hpp"
#include "runtime/commboard.hpp"
#include "runtime/cpu_time.hpp"
#include "runtime/rom.hpp"
#include "runtime/snapshots.hpp"
#include "runtime/watched_memory.hpp"
#include "runtime/real3d_interp.hpp"
#include "runtime/real3d_render.hpp"
#include "runtime/tilegen.hpp"
#include "runtime/widescreen.hpp"
#include "game_setup.hpp"
#include "input_table.hpp"
#include "netplay.hpp"
#include "net/compress.hpp"
#include "input_log.hpp"
#include "recording.hpp"
#include "one_board.hpp"
#include "rollback.hpp"
#include "delay_tuner.hpp"
#include "remote_cabinet.hpp"
#include "png_write.hpp"
#include "wav_write.hpp"
#ifdef SPIKEOUT_JIT
#include "jit/jit.hpp" // (gamerun-jit: the game translated from the ROM at start, LLVM)
#endif
#ifdef SPIKEOUT_INTERP
#include "interp/interp.hpp" // (gamerun-interp: the game run from the ROM by the interpreter)
#endif
#ifdef SPIKEOUT_GPU
#include "gpu/present.hpp"
#include "gpu/real3d_gpu.hpp"

#include <SDL3/SDL_gpu.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <random>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kAllBoards = -1;
constexpr std::size_t kNodeBytes = 0x900; // --link-dump: a board's own part of each data frame
constexpr int kGpuTimeRepeats = 8; // --gpu-time draws each frame this many times

struct Press {
    std::string name;
    uint64_t frame = 0, frames = 6;
    int bank = 0; // 0: input bank 0 (coin, start...), 1: the controls byte
    int bit = 0;
    int board = kAllBoards;
};

// --poke ADDR=VALUE@FRAME[:N]: write a byte of RAM at the end of N frames
// (default 1) from FRAME, for experiments (a setting the game reads, say).
struct Peek { // --peek: main RAM printed at the end of frames
    uint32_t addr = 0, length = 4;
    uint64_t frame = 0, frames = 1;
    int board = kAllBoards;
};

struct Poke {
    uint32_t addr = 0;
    uint8_t value = 0;
    uint64_t frame = 0, frames = 1;
    int board = kAllBoards;
};

struct Options {
    const char* dir = nullptr;
    const char* dump_tile = nullptr;
    const char* dump_textures = nullptr;
    const char* dump_backup = nullptr;
    const tools::Recording* replay = nullptr; // --replay FILE
    const tools::InputLog* input_log = nullptr; // --inputs FILE (spikebot --record-inputs)
    const std::map<tools::LinkedInputLog::Where, tools::LinkedInputLog::Change>* linked_inputs = nullptr; // (linked boards')
    uint64_t linked_inputs_end = 0;           // (the last frame there)
    std::string save_state;                   // --save-state FILE@FRAME
    uint64_t save_frame = 0;
    const char* load_state = nullptr;         // --load-state FILE
    uint32_t watch_lo = 0, watch_hi = 0;      // --watch-write LO-HI@FRAME:N
    uint64_t watch_frame = 0, watch_frames = 1;
    uint64_t rb_from = 0, rb_frames = 0, rb_depth = 4, rb_every = 10; // --rollback-test FRAME:N[:DEPTH[:EVERY[:compare][:quiet][:timed]]]
    bool rb_compare = false, rb_quiet = false, rb_timed = false;
    uint64_t host_trace_frame = 0, host_trace_frames = 0; // --net-host-trace FRAME:N
    uint64_t rollback_from = 0, rollback_frames = 0;      // --rollback-probe FRAME:N[:speed]
    bool rollback_speed_only = false;                     // (:speed: only the wall clock, no states)
    bool dump_layers = false;
    // --eeprom [B:]FILE: the settings EEPROM to start with (128 bytes, high byte first), for board B or all
    std::vector<std::pair<int, std::string>> eeprom_files;
    std::vector<std::pair<int, std::string>> backup_files; // --backup [B:]FILE: backup RAM to start with (as nvram/board<N>.backup)
    std::vector<uint8_t> eeprom_bytes; // (its bytes, or the network host's)
    std::array<std::vector<uint8_t>, 4> board_eeprom; // (one board's own, over those)
    std::array<std::vector<uint8_t>, 4> board_backup; // (network play: each board's backup RAM, its player's cabinet's)
    std::optional<int64_t> clock;      // --clock SECONDS: the boards' clocks at power-on (else the host's time)
    // --net-host NAME --seats N / --net-join NAME: network play (tools::NetPlay)
    std::string net_host, net_join;
    bool net_spectate = false; // --net-spectate (with --net-join): a spectator: every board from the host's inputs, no seat
    uint8_t net_spectate_seat = 0; // --net-spectate-seat N: the seat (0-3) a spectator on one board watches
    bool pace = false; // --pace: at the arcade's speed (this process's own board held to the refresh)
    uint8_t net_seats = 2;
    // --lobby URL: through an internet lobby (a test's own, `wrangler dev`):
    // --net-host offers it there (--private: by its code only), --net-join CODE
    std::string lobby_url;
    bool net_private = false;
    bool net_catch_up = false;    // --net-catch-up: joined in progress, the cabinet taken once caught up (as the window)
    bool net_start_alone = false; // --net-start-alone: the host starts at once, the other cabinets empty until players join
    // --net-one-board FRAME: from the end of FRAME, this machine runs only its own board, the others made by its
    // tools::remote::Cabinets from their players' inputs (Mode B, every board run here, before)
    uint64_t net_one_board = 0;
    uint64_t net_leave_at = 0; // --net-leave-at FRAME: this player leaves a little after FRAME, its board handed to the others first
    uint64_t net_vanish_at = 0; // --net-vanish-at FRAME: this machine gone at the end of FRAME without a word (a crash)
    uint64_t net_rollback = 0;   // --net-rollback N: rollback, the boards running up to N frames ahead on guessed inputs (tools::rollback)
    std::optional<uint64_t> net_rollback_history = tools::rollback::kWindow; // --net-rollback-history N (0: N = --net-rollback's)
    uint64_t net_rollback_depth = 0; // (the history kept: the window's as memory allows)
    uint64_t net_hold_inputs = 0; // --net-hold-inputs N: tests: this machine's inputs sent N frames' time late
    bool no_frame_hashes = false; // --no-frame-hashes: none of the comparisons' RAM hashes each frame (timing runs)
    bool frame_hashes = false;    // --frame-hashes: those hashes in any run (a network game joined in progress, compared)
    uint64_t max_frames = 0;      // --frames N: the run ends at the end of frame N
    std::size_t local_seat = 0; // (network play: the board this machine's player has)
    tools::NetPlay* netplay = nullptr;
    const char* net_disasm = nullptr;
    const char* screenshot = nullptr;
    const char* wav = nullptr;
    uint64_t shot_every = 0, tilegen_trace = 0;
    int probe_x = -1, probe_y = -1;
    int sound_volume = 100, music_volume = 100;
    uint64_t probe_frame = 0;
    std::vector<Press> presses;
    std::vector<Poke> pokes;
    std::vector<Peek> peeks;
    const char* ram_dump = nullptr; // --ram-dump B:LO-HI@FRAME:N FILE
    const char* ram_hashes = nullptr; // --ram-hashes B@FRAME FILE
    int ram_hashes_board = 0;
    uint64_t ram_hashes_from = 0;
    int ram_dump_board = 0;
    uint32_t ram_dump_lo = 0, ram_dump_hi = 0;
    uint64_t ram_dump_from = 0, ram_dump_frames = 0;
    bool show_text = false, real_netboard = true, force_master = false, quads = false, midi_burst = false,
         dma_instant = false, fog_over_nothing = false;
    std::size_t net_trace = 0;
    uint64_t music_hash = 0; // --music-hash N: each board's music board state hashed every N frames
    int64_t blocks = 200'000'000;
    std::size_t boards = 1;
    uint32_t slice = 8;
    uint64_t link_latency = 0;
    uint64_t input_delay = 0; // --input-delay N: inputs read N frames after they were set
    bool parallel = false; // --parallel: the boards run at once (tools::Turns, m3::LinkRing deferred)
    // --no-2d, --no-sound: what a remote cabinet needn't run (the game the same without them)
    bool no_2d = false, no_sound = false;
    uint64_t link_trace = 0;
    const char* link_dump = nullptr; // --link-dump FILE
    const char* link_record = nullptr; // --link-record B:FILE
    const char* link_standin = nullptr; // --link-standin B:FILE
    int link_record_board = -1, link_standin_board = -1;
    uint64_t link_standin_at = 0; // --link-standin-at CYCLES
    std::string link_standin_patch; // --link-standin-patch enemy-flags
    const char* camera_record = nullptr; // --camera-record B:FILE
    const char* camera_test = nullptr;   // --camera-test B:FILE
    int camera_record_board = -1, camera_test_board = -1;
    uint64_t camera_trace = 0; // --camera-trace N: board 0's camera updates and view tests, where from
    uint64_t camera_test_from = 0; // --camera-test-from FRAME
    uint64_t map_trace_from = 0, map_trace_frames = 0; // --map-trace FROM:N
    bool camera_test_flags = false; // --camera-test-flags
    bool camera_test_mirror = false; // --camera-test-mirror
    bool camera_test_free = false;   // --camera-test-free
    bool link_standin_no_reads = false; // --link-standin-no-reads
    bool link_standin_coarse = false;   // --link-standin-coarse
    bool link_standin_blocks = false;   // --link-standin-blocks
    int drop_board = -1;                // --drop B@FRAME
    uint64_t drop_frame = 0;
    const char* drop_buttons = nullptr; // --drop-buttons DUMP
    int one_board = -1;                 // --one-board B@FRAME (and --one-board-camera FILE)
    int one_board_stays = -1;           // --one-board-stays B (tests: the board that stays, not the first not going)
    std::vector<std::size_t> one_board_also; // (--one-board repeated: more boards made by the one that stays, from the same FRAME)
    uint64_t one_board_frame = 0;
    const char* one_board_camera = nullptr;
    std::vector<uint64_t> one_board_missed; // --one-board-miss FRAME (its main board's frames lost, given)
    uint64_t one_board_shadow = 0;          // --one-board-shadow FRAME (its shadow from there)
    bool one_board_tasks = false;           // --one-board-tasks (tools::remote::Cabinets::use_tasks)
    bool one_board_auto = false;            // --one-board-auto (a shadow started and stopped by the rules)
    uint64_t one_board_lookahead = 60;      // --one-board-lookahead N (the rules' look at the other cabinet's coin and start)
    uint64_t one_board_hand_back = 0;       // --one-board-hand-back FRAME (its shadow back to side runs, asked then)
    bool one_board_held = false;            // --one-board-held (at the hand-over its board runs on whole, until the rules hand it back)
    bool one_board_late = false;            // --one-board-late (set up at a meeting in FRAME, not before the boards start)
    uint64_t one_board_whole = 0;           // --one-board-whole FRAME (the other run whole from then on: Game::keep_whole)
    bool one_board_local_shadows = false;   // --one-board-local-shadows (network play: shadows from this board's state, not the other's)
    int wait_board = -1;               // --wait-seat B@FROM:UNTIL
    uint64_t wait_from = 0, wait_until = 0;
    int clone_board = -1;               // --clone-seat B@FRAME
    uint64_t clone_frame = 0;
    bool camera_events = false; // --camera-events: board 0's camera written by other code, counted
    bool script_actions = false; // --script-actions: every stage script action run, by its record
    bool net_watch = false;
    // --renderer: the 3D in software, on the GPU, or both, compared.
    enum class Renderer { Software, Gpu, Compare } renderer = Renderer::Software;
    int gpu_time_scale = 0; // --gpu-time: the GPU's time a frame at this render scale
    int gpu_scale = 1;      // --gpu-scale: the GPU renderer's render scale (gpu, compare)
    int wide = 0;           // --wide: widescreen, the picture this many pixels wider each side (gpu)
    int screen_w = 0, screen_h = 0; // --screen-time WxH: the window's screen pass timed at that size
    bool match_models = false;      // --match-models: smooth motion's matching of models between frames, reported
    double gpu_profile_over = 0;    // --gpu-slow MS (with --gpu-time): the frames the GPU took longer than that, listed
    bool gpu_hash_frames = false;   // --gpu-hash-frames: each GPU picture's hash, printed
    uint64_t gpu_full_frame = 0;    // --gpu-full FRAME: that frame's GPU picture written whole (FILE-<frame>-full.png)
    bool interp = false;            // --interp: smooth motion's pictures between frames, reported and drawn
    bool interp_trace = false;      // --interp-trace FROM:TO: and each frame's, printed
    uint64_t interp_trace_from = 0, interp_trace_to = 0;
    bool irq5_on_e0 = true, send_waits_for_receive = m3::CommBoardOptions().send_waits_for_receive,
         host_sees_other_bank = false;
    double link_cycles_per_byte = m3::CommBoardOptions().link_cycles_per_byte;
    uint32_t irq5_per_frame = m3::CommBoardOptions().irq5_per_frame; // --net-irq5-per-frame N (a link question: docs/fidelity.md)
    uint32_t slave_irq5_per_frame = m3::CommBoardOptions().slave_irq5_per_frame; // --net-slave-irq5-per-frame N
    double irq5_phase = 0; // --net-irq5-phase F (a part of a frame)
    bool ring_forwarding = m3::CommBoardOptions().ring_forwarding; // --net-ring-forwarding on/off
    bool double_buffer = m3::CommBoardOptions().double_buffer; // --net-double-buffer on/off
    bool old_link = false; // --net-old-link: the link before the ring model (CommBoardOptions::use_old_link)
};

// Where each input is, active low (hardware-notes.md, Inputs).
bool input_bit(const std::string& name, int& bank, int& bit) {
    static const struct {
        const char* name;
        int bank, bit;
    } kInputs[] = {{"coin", 0, 0},  {"test", 0, 2},  {"service", 0, 3}, {"start", 0, 4},
                   {"beat", 1, 0},  {"charge", 1, 1}, {"shift", 1, 2},  {"jump", 1, 3},
                   {"down", 1, 4},  {"up", 1, 5},     {"right", 1, 6},  {"left", 1, 7}};
    for (const auto& i : kInputs)
        if (name == i.name) {
            bank = i.bank;
            bit = i.bit;
            return true;
        }
    return false;
}

// "B:rest" -> board B and "rest"; anything else -> every board.
std::string board_prefix(const std::string& spec, int& board) {
    const size_t colon = spec.find(':');
    const size_t end = spec.find_first_of("=@");
    if (colon != std::string::npos && colon > 0 && colon < end &&
        spec.find_first_not_of("0123456789") == colon) {
        board = std::atoi(spec.c_str());
        return spec.substr(colon + 1);
    }
    board = kAllBoards;
    return spec;
}

// FILE.ext -> FILE-bN.ext for boards after the first.
std::string board_path(const std::string& path, std::size_t board) {
    if (board == 0) return path;
    const std::string tag = "-b" + std::to_string(board);
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + tag;
    return path.substr(0, dot) + tag + path.substr(dot);
}

// --probe: every triangle that covered the pixel, and what became of it.
void print_probe(const m3::Real3dRenderer& r, int x, int y, uint64_t frame) {
    std::printf("Probe %d,%d at frame %llu: %zu triangles cover it\n", x, y, static_cast<unsigned long long>(frame),
                r.probe().size());
    for (const auto& h : r.probe())
        std::printf("  polygon %06x [%08x %08x %08x %08x %08x %08x %08x] priority %d, %s format %d%s, alpha %.2f, "
                    "texel alpha %.2f, 1/w %.4g, texture %ux%u level %.2f, %s: %s\n",
                    h.polygon, h.header[0], h.header[1], h.header[2], h.header[3], h.header[4], h.header[5],
                    h.header[6], h.priority, h.textured ? "textured" : "flat", h.format, h.contour ? " contour" : "",
                    static_cast<double>(h.alpha), static_cast<double>(h.texel_alpha), static_cast<double>(h.w), h.tw, h.th,
                    static_cast<double>(h.lod), h.pass,
                    h.outcome);
}

using tools::LinkedState; // (tools/game_setup.hpp)

// --link-record / --link-standin: what a main board does to its network
// board, every call in order with the main board's time (`t`, its cycles):
// sync, the reads (with the value read) and writes, a frame's end, and
// gamerun's own meeting with the other boards (the link's packets go on
// there).
struct HostCall {
    enum Op : uint32_t { Sync, Read8, Read16, Read32, Write8, Write16, Write32, EndFrame, Pass };
    uint32_t op = Sync, addr = 0, value = 0, spare = 0;
    uint64_t t = 0;
};

// One Model 3 board and what the run keeps of it.

struct Machine {
    Machine(std::size_t i, const m3::RomSet& roms, std::span<const recomp::FunctionEntry> functions)
        : index(i), bus(roms), c(bus, functions), sound(roms),
          screen(m3::TilegenRenderer::kWidth * m3::TilegenRenderer::kHeight) {}

    std::size_t index;
    m3::Bus bus;
    m3::CommBoard* comm = nullptr;
    // --rollback-probe: what rollback would cost, a frame at a time (its state saved,
    // how much of it changed since the frame before, a copy of it), and board 0's wall time.
    struct Probe {
        std::vector<uint8_t> before;
        uint64_t frames = 0;
        double save_ms = 0, save_ms_worst = 0, copy_ms = 0;
        std::size_t size = 0;
        double changed = 0, changed_worst = 0; // bytes in changed 4 KB pages
        std::chrono::steady_clock::time_point started{};
        double wall_s = 0;
    } probe;
    // --rollback-test: snapshots every frame (m3::Snapshots), going back now
    // and then and running again; each frame's whole state hashed the first
    // time and compared the times after.
    struct RollbackTest {
        std::unique_ptr<m3::Snapshots> snaps;
        std::map<uint64_t, uint64_t> hashes, buffers; // the whole state's, the big buffers'
        std::set<uint64_t> rolled; // frames gone back from
        uint64_t checked = 0, differed = 0, restores = 0, restored_exact = 0, saves = 0, first_differ = 0;
        double save_ms = 0, save_worst = 0, restore_ms = 0, restore_worst = 0;
        std::size_t pages = 0, pages_worst = 0;
        uint64_t saved_frame = 0;  // (linked boards: the last frame saved)
        bool rewind = false;       // (linked boards: put back at the slices' meeting; on from there)
        uint64_t quiet_through = 0; // :quiet: frames up to this one are being run again
        std::chrono::steady_clock::time_point back_at{}; // the last going back, to time the frames run again
        double rerun_s = 0;
        uint64_t rerun_frames = 0;
    } rb;
    bool loaded = false; // --load-state: the run resumes where the state was saved
    // --camera-record / --camera-test (network play: can a remote player's
    // camera be run on another board?)
    struct CameraTest {
        std::FILE* record = nullptr; // --camera-record: this board's camera as each update starts
        std::map<uint32_t, std::vector<uint8_t>> recorded; // --camera-test: the other's, by logic counter
        std::vector<uint8_t> mine; // the second camera, run here from the first recorded
        bool testing = false, inside = false;
        uint64_t trace_left = 0; // --camera-trace N
        std::FILE* view_out = nullptr; // --camera-test: FILE.view, the other camera's view tests
        std::FILE* mine_out = nullptr; // and FILE.mine, the second camera after each update
        uint64_t view_tests = 0;
        double side_seconds = 0; // the side runs' game code alone
        uint64_t from = 0; // --camera-test-from FRAME: the second camera started there
        bool copy_flags = false; // --camera-test-flags: its flags word from this board's camera each frame
        // --camera-test-free: the second camera only run on (from the recorded one at --camera-test-from),
        // not compared: its heading after each update and its view tests kept, by logic count, for
        // --drop's made blocks (the other cabinet's camera, run here)
        bool free = false;
        std::map<uint32_t, uint16_t> heading_at;                      // (logic: its heading, camera +0x2A)
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> view_at;    // ((logic, character): its flags word)
        // (and players 1-4 as this board writes its own record, 0x6DA90, by logic count: each one's
        // first word, +0x44, +0x2A, +0x74, +0x76, what a made record takes from its player)
        std::map<uint32_t, std::array<std::array<uint32_t, 5>, 4>> players_at;
        // (and that cabinet's own flow run on the side, with its own variables, kCabinetOwn: their
        // values, and its header after its flow, by logic count)
        std::vector<uint8_t> own;
        std::map<uint32_t, std::array<uint8_t, 0x100>> header_at;
        std::map<uint32_t, std::vector<uint8_t>> own_at;   // (after each logic count: checked against that board's own)
        std::map<uint32_t, std::vector<uint8_t>> own_seed; // (that board's own after each logic count, while it runs)
        uint64_t toggled_at = 0;                           // (the frame its toggles were last flipped for)
        bool mirror = false; // --camera-test-mirror: other code's changes to this board's camera made to the second too
        bool in_view = false, in_update = false; // (this board's view routine, or its update, running)
        bool events = false; // --camera-events: code other than the camera's own writing into it, counted
        struct Writer {
            uint64_t writes = 0, first = 0, last = 0;
            std::set<uint32_t> offsets;
            std::vector<std::array<uint32_t, 3>> seen; // (logic counter, offset, value)
        };
        std::map<uint64_t, Writer> event_writers; // by the writing block (high half) and its caller (LR, low)
        bool actions = false; // --script-actions: every stage script action run (the dispatch's hook)
        struct ActionRun {
            uint8_t action = 0;
            uint64_t runs = 0, first = 0, last = 0;
        };
        std::map<uint32_t, ActionRun> actions_run; // by the action's record

        uint64_t mirrored = 0;
        uint64_t calls = 0, runs = 0, compared = 0, heading_free = 0, object_free = 0, heading_step = 0, object_step = 0;
        uint32_t first_logic = 0, first_free_miss = 0;
        std::map<uint32_t, uint64_t> written; // RAM the side runs wrote outside the camera, by 16 bytes
        std::string failed;
    } camera;
    std::vector<HostCall> host_log; // --link-record: this board's main board's calls to its network board
    bool recording_host = false;
    LinkedState* linked = nullptr; // linked boards' --save-state / --load-state
    tools::InputTable* inputs = nullptr; // every board's inputs by frame
    bool inputs_started = false;
    bool took_seat = false; // (network play: a game joined in progress, our cabinet asked for)
    // (--net-one-board: every board whole here for a state asked for, then saved; whole until this frame after)
    bool state_pending = false;
    uint64_t whole_until = 0;
    uint64_t parting_frame = 0; // (--net-leave-at: this player leaving after this frame; its state sent, the goodbye next)
    bool leave_next = false;
    tools::backup::Maker backups;   // (--net-one-board: this board's backups on the other machines, tools::backup)
    std::map<uint64_t, uint64_t> hash_pending; // (--net-rollback: RAM hashes for the others, once their frames are settled)
    bool one_board_handed = false; // (--one-board: handed over already)
    double input_wait_max = 0; // (network play: the longest a frame waited for inputs, ms)
    uint64_t sync_waits = 0;   // (--net-rollback: frames held back by the time sync)
    uint64_t input_waits_long = 0; // (network play: frames that waited over a frame's time for inputs)
    std::set<uint8_t> backup_bases; // (the seats sent one: changes from it next)
    uint64_t watch_from = 0, watch_to = 0; // --watch-write: the frames whose writes are printed
    // --replay: the next recorded inputs and hash, hashes matched, the first frame not
    size_t replay_input = 0, replay_hash = 0;
    uint64_t replay_matched = 0, replay_parted = 0;
    uint32_t line = 0; // (linked boards: at the last meeting)
    uint8_t linked_bank0 = 0xFF, linked_controls = 0xFF; // (--inputs, linked boards: as last set)
    recomp::Context c;
    recomp::FrameTiming timing;
    tools::one_board::Board ob{c, bus, timing, index}; // --one-board (tools/one_board.hpp)
    m3::TilegenRenderer tilegen;
    m3::Real3dRenderer real3d;
    tools::FrameSound sound;
    std::vector<uint32_t> screen;
    std::vector<int16_t> audio;

    std::string why;
    double seconds = 0;
    double cpu_seconds = 0; // (its thread's CPU time, user and kernel: its own work, not its waits)
    int peak = 0;
    double energy = 0, sound_seconds = 0, render_seconds = 0, setup_ms = 0, raster_ms = 0;
    // --match-models: each frame's scene, and its models found in the frame before's
    m3::Real3dSceneBuilder match_builder;
    m3::Real3dScene match_scene[2];
    uint64_t match_models = 0, match_one = 0, match_several = 0, match_unmatched = 0, match_frames = 0;
    uint64_t match_unmatched_ram = 0, match_unmatched_path = 0;
    // --interp: the scene halfway from the frame before (smooth motion), and how it went
    m3::Real3dScene interp_scene;
    uint64_t interp_frames = 0, interp_cuts = 0, interp_models = 0, interp_blended = 0, interp_moved = 0;
    uint64_t interp_nearest = 0, interp_ambiguous = 0, interp_worst_frame = 0, interp_followed = 0;
    std::vector<uint32_t> sky_before; // the frame before's layers below the 3D, and its sky's scroll
    uint32_t sky_scroll_before = 0;
    uint64_t sky_frames = 0, sky_found = 0;
    m3::InterpStats interp_worst_stats;
    double interp_seconds = 0, interp_worst = 0;
    uint64_t textures_seen = 0, texture_frames = 0; // frames in which the game uploaded textures
    // the 2D layers' drawing, and (--wide) their widescreen sides' making, every frame
    double tilegen_seconds = 0, wide_seconds = 0, wide_worst = 0;
    uint64_t tilegen_frames = 0, wide_frames = 0, wide_worst_frame = 0;
    uint64_t frames_rendered = 0, frames_spotlit = 0, frames_scroll_fog = 0, frames_ambient_fog = 0,
             frames_block_culled = 0;
    uint64_t micro_tris = 0, translator_tris = 0, inverted_tris = 0;
    uint16_t net_status[2] = {0, 0}; // --net-watch
    // --renderer gpu / compare: the GPU's picture, and how it compared with
    // the software renderer's (pixels with a channel more than 2 apart).
#ifdef SPIKEOUT_GPU
    std::unique_ptr<gpu::Real3dGpu> gpu;
    SDL_GPUDevice* gpu_device = nullptr;
#endif
    m3::Real3dSceneBuilder builder;
    m3::Real3dScene scene;
    std::vector<uint32_t> gpu_image;
    // --wide: the GPU's whole widescreen picture, and the 2D layers made as wide
    std::vector<uint32_t> gpu_wide, bottom_wide, top_wide;
    m3::WideLayer wide_bottom, wide_top;
#ifdef SPIKEOUT_GPU
    // --screen-time: the window's screen pass, drawn off screen and timed (ms, summed over the screenshots)
    std::unique_ptr<gpu::Presenter> presenter;
    SDL_GPUTexture* screen_target = nullptr;
    double screen_wide_ms = 0, screen_43_ms = 0;
    uint64_t screen_samples = 0;
#endif
    double gpu_seconds = 0, gpu_worst = 0;
    // --gpu-time: a frame drawn alone, its costs (ms, summed): the CPU's parts, the GPU's after
    double profile_pack = 0, profile_upload = 0, profile_record = 0, profile_cpu = 0, profile_gpu = 0, profile_gpu_worst = 0;
    uint64_t profile_frames = 0, profile_passes = 0, profile_draws = 0;
    uint64_t gpu_worst_frame = 0, gpu_slow = 0; // (slow: over a frame, 17.4 ms)
    uint64_t gpu_hash = 0xCBF29CE484222325ull; // of every picture read back, full size (--gpu-scale)
    uint64_t gpu_frames = 0, frames_compared = 0, frames_differing = 0, pixels_differing = 0;
    uint64_t worst_frame = 0, worst_pixels = 0;
    int max_difference = 0;
};

// How far apart two pictures are: pixels with a channel more than 2 apart,
// and the largest difference.
std::pair<uint64_t, int> difference(std::span<const uint32_t> a, std::span<const uint32_t> b) {
    uint64_t pixels = 0;
    int largest = 0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i] == b[i]) continue;
        int most = 0;
        for (int s = 0; s < 32; s += 8)
            most = std::max(most, std::abs(static_cast<int>((a[i] >> s) & 0xFF) - static_cast<int>((b[i] >> s) & 0xFF)));
        if (most > 2) ++pixels;
        largest = std::max(largest, most);
    }
    return {pixels, largest};
}

bool for_board(int board, std::size_t index) { return board == kAllBoards || static_cast<std::size_t>(board) == index; }

// --replay: the end of `frame`: the recorded inputs for the next, main RAM
// checked against the recorded hash, and the run ended after the last.
void replay_frame(Machine& m, const tools::Recording& rec, uint64_t frame) {
    while (m.replay_input < rec.inputs.size() && rec.inputs[m.replay_input].first < frame) ++m.replay_input;
    if (m.replay_input < rec.inputs.size() && rec.inputs[m.replay_input].first == frame) {
        const auto& in = rec.inputs[m.replay_input].second;
        m.bus.inputs.bank0 = in.bank0;
        m.bus.inputs.bank1 = in.bank1;
        m.bus.inputs.controls = in.controls;
        m.bus.inputs.controls2 = in.controls2;
    }
    while (m.replay_hash < rec.hashes.size() && rec.hashes[m.replay_hash].first < frame) ++m.replay_hash;
    if (m.replay_hash < rec.hashes.size() && rec.hashes[m.replay_hash].first == frame) {
        if (tools::Recording::ram_hash(m.bus.ram()) == rec.hashes[m.replay_hash].second) {
            ++m.replay_matched;
        } else if (!m.replay_parted) {
            m.replay_parted = frame;
            std::printf("replay: main RAM differs from the recording's at frame %llu\n", static_cast<unsigned long long>(frame));
        }
    }
    if (!rec.inputs.empty() && frame >= rec.inputs.back().first) m.c.limit_blocks(0); // the recording's end
}

// Save states: the sound board's part (none with --no-sound).
std::function<void(m3::State&)> sound_state(Machine& m, const Options& o) {
    if (o.no_sound) return {};
    return [&m](m3::State& s) { m.sound.state(s); };
}

// --inputs, linked boards: what the bot changed on board m where it stood (a
// frame end, a meeting, a point after a frame's end), as it did. Returns the
// change found there (or null).
const tools::LinkedInputLog::Change* linked_change(Machine& m, const Options& o, tools::LinkedInputLog::At at, uint64_t frame,
                                                   uint32_t line = 0) {
    const auto it = o.linked_inputs->find({at, static_cast<uint8_t>(m.index), frame, line});
    if (it == o.linked_inputs->end()) return nullptr;
    const tools::LinkedInputLog::Change& c = it->second;
    if (c.inputs) {
        m.linked_bank0 = m.bus.inputs.bank0 = c.bank0;
        m.linked_controls = m.bus.inputs.controls = c.controls;
    }
    for (const auto& [addr, value] : c.writes) {
        m.bus.ram()[addr] = static_cast<uint8_t>(value >> 8);
        m.bus.ram()[addr + 1] = static_cast<uint8_t>(value);
    }
    return &c;
}

// --rollback-test: thrown from a snapshot point once the board is back at an
// earlier frame; run() goes on from there (as a loaded state does).
struct Rewound {};
// --drop B@FRAME: thrown at the end of board B's frame FRAME; run() goes on making its blocks.
struct Dropped {
    uint64_t write_at;
    uint32_t slice;
    bool module = false; // (--one-board: its blocks made by the other board's tools::remote::Cabinets)
    bool shadow = false; // (--one-board-shadow: it waits to be that cabinet's shadow)
};
std::vector<std::unique_ptr<struct Machine>>* g_machines = nullptr; // (--drop: board 0, whose block the made ones start from)
m3::LinkRing* g_ring = nullptr; // (linked boards' ring: one board a PC's shadows from another machine's board)
std::atomic<uint64_t> g_parted_after{0}; // (--net-leave-at: this player gone after this frame; every board here stops there)
std::atomic<bool> g_catching_up{false}; // (--net-catch-up: joined in progress, not yet with the others)
std::atomic<int> g_catch_percent{0};    // (and how far caught up, percent)
// (--input-delay N, or auto: tools::DelayTuner once a second in network play, as the window's Auto; read each frame)
std::atomic<uint64_t> g_input_delay{0};
bool g_input_delay_auto = false;
std::atomic<bool> g_capture_armed{false}; // (every board's state wanted, at a frame whose inputs are all in already)
std::chrono::steady_clock::time_point g_capture_armed_at{};
struct Pace { // --pace: how well it kept to the arcade's speed (the end says)
    uint64_t frames = 0, late = 0;
    double seconds = 0, worst = 0;
} g_pace;
const m3::RomSet* g_roms = nullptr; // (one board a PC: a board whose player went run on from its backup)
std::unique_ptr<tools::rollback::Linked> g_net_rb; // (--net-rollback: every board going back together)
// --clone-seat B@FRAME: board 0's state at its first block after FRAME's end (no sound), which
// board B takes at its own, as its own cabinet (thrown after: run() goes on from it).
std::vector<uint8_t> g_clone_state;
std::mutex g_clone_lock;
const char* g_drop_buttons = nullptr; // --drop-buttons DUMP (for made_blocks)
const std::vector<Press>* g_presses = nullptr; // --press (for made_blocks: a dropped seat's own buttons)
int g_drop_board = -1; // --drop B (the side camera's buttons are that cabinet's)
struct Cloned {};
// --wait-seat B@FROM:UNTIL: thrown at board B's first block after FROM's end; run() holds its blocks
// to UNTIL, then brings the board back there.
struct Frozen {
    uint64_t until, write_at;
    uint32_t slice;
};

// --link-record: a network board that logs its main board's calls.
class RecordingNetBoard : public m3::NetBoardDevice {
public:
    RecordingNetBoard(std::unique_ptr<m3::NetBoardDevice> inner, std::vector<HostCall>& log) : inner_(std::move(inner)), log_(log) {}
    uint8_t read8(uint32_t a) override { return static_cast<uint8_t>(note(HostCall::Read8, a, inner_->read8(a))); }
    uint16_t read16(uint32_t a) override { return static_cast<uint16_t>(note(HostCall::Read16, a, inner_->read16(a))); }
    uint32_t read32(uint32_t a) override { return note(HostCall::Read32, a, inner_->read32(a)); }
    void write8(uint32_t a, uint8_t v) override { note(HostCall::Write8, a, v), inner_->write8(a, v); }
    void write16(uint32_t a, uint16_t v) override { note(HostCall::Write16, a, v), inner_->write16(a, v); }
    void write32(uint32_t a, uint32_t v) override { note(HostCall::Write32, a, v), inner_->write32(a, v); }
    void sync(uint64_t t) override {
        now_ = t;
        note(HostCall::Sync, 0, 0);
        inner_->sync(t);
    }
    void end_frame() override { note(HostCall::EndFrame, 0, 0), inner_->end_frame(); }
    std::string describe() const override { return inner_->describe(); }
    void state(m3::State& s) override { inner_->state(s); }

private:
    uint32_t note(HostCall::Op op, uint32_t addr, uint32_t value) {
        log_.push_back({op, addr, value, 0, now_});
        return value;
    }
    std::unique_ptr<m3::NetBoardDevice> inner_;
    std::vector<HostCall>& log_;
    uint64_t now_ = 0;
};

// --camera-record / --camera-test: the camera's update (r12 the camera at
// 0x11D480; +0x2A its heading, as the link sends it), and the logic counter.
constexpr uint32_t kCameraUpdate = 0x2C4DC, kCamera = 0x11D480, kCameraBytes = 0xC00, kCameraHeading = 0x2A;
constexpr uint32_t kViewTest = 0x33A6C; // a character (r14) on this camera's screen (flags bit 0x08) or near it (0x10)
constexpr uint32_t kCameraView = 0x32578; // after the update: the camera's zone, ..., its view matrix at +0x640 (f20-f31)
constexpr uint32_t kLogicCounter = 0x1000A8;
// --camera-test-mirror: what code other than the camera's own writes into
// the camera (the game's events: 0x5D21C at a gate sets its mode, +0x48,
// and more), written into the second camera too (a write watch). The
// camera's own: the update (from its hook until 0xC670 is called from
// 0x32514, just after it returns), the view routine (from its hook until
// the first view test), the flags and counter at +0x00-0x07 (each
// cabinet's), 0x32470's positions from player 1 (+0x4A8-0x4BF).
constexpr uint32_t kAfterUpdate = 0xC670, kAfterUpdateReturn = 0x32514;
constexpr uint32_t kActionDispatch = 0x5AB18; // a stage script's action about to run (r31 its record, byte 1 the action)
constexpr uint32_t kActionDispatchAtOnce = 0x5A874; // the same, from the interpreter that runs a script through (0x5A824)
constexpr uint32_t kRecordWrite = 0x6DA90; // this cabinet's record written (+0x04 its buttons, +0x10 its player's first word)
// A camera shake (0x345E4, r3 the kind: the table at 0x90114, 8 bytes a kind, its byte into the
// camera's +0xD2 and +0xD3, its float into +0xD4), decided by the cabinet's own player: 0x21C88 (a
// character's animation command, r14 the character, r28+4 the kind: every cabinet's in mode 3 with
// a flag, else the local player's, -16348(r13), or one holding it, +0x44 bit 0x1000 and +272) and
// 0x22750 (kind 7 for the player the camera follows, camera +0x4A, against the character's +114).
// Board 0's own decisions written into its camera (the block 0x345EC) aren't the other's.
constexpr uint32_t kShakeByAnimation = 0x21C88, kShakeFollowed = 0x22750, kShakeWrite = 0x345EC;
// A cabinet's own flow: the mode dispatcher, called once a frame from the main loop (0x11A80, after
// the link's 0x6D758), its mode -16374(r13) (7 in the game, 15 after a game over), the in-game mode's
// routine 0x4B460 (the time over, the continue countdown -7523(r13), the flags -8188(r13), the header's
// status 0x140106). Run on the side for another cabinet with its own variables: the bytes of r13's
// areas that differ between two linked boards (board 0 at a frame against board 1 the frame after,
// through a time over, a continue and a game over), less the main loop's timings (-16252..-16242).
constexpr uint32_t kModeDispatch = 0x492AC;
constexpr std::pair<uint32_t, uint32_t> kCabinetOwn[] = {
    {0x104000, 0x104014}, {0x10401A, 0x10401C}, {0x104024, 0x10402C}, {0x104051, 0x104052}, {0x10407B, 0x10407C},
    {0x104083, 0x104084}, {0x104090, 0x104094}, {0x1040B0, 0x1040BC}, {0x1040C7, 0x1040C8}, {0x1040F0, 0x1040FC},
    {0x104103, 0x104104}, {0x10065B, 0x10065C}, {0x106004, 0x10600C}, {0x106180, 0x106181}, {0x1061B7, 0x1061BB},
    {0x1061CD, 0x1061E6}, {0x106242, 0x10624D}, {0x106268, 0x10626C}, {0x106295, 0x1062A4}, {0x1062AD, 0x1062AE},
    {0x1062C7, 0x1062C8}, {0x1062DC, 0x1062DD}, {0x100100, 0x100120}, // (its buttons' words, 0x5094's, and copies)
    {0x140104, 0x140108}, // (and its block's header status, +0x04-0x07: its flow ORs bits into +0x06)
    // (and each character's view of it, +0x34A0-0x34BF: its camera's view tests write them, its camera's update
    // reads them; players 1-4, then enemies 1-8)
    {0x1A34A0, 0x1A34C0}, {0x1A74A0, 0x1A74C0}, {0x1AB4A0, 0x1AB4C0}, {0x1AF4A0, 0x1AF4C0}, {0x1B34A0, 0x1B34C0},
    {0x1B74A0, 0x1B74C0}, {0x1BB4A0, 0x1BB4C0}, {0x1BF4A0, 0x1BF4C0}, {0x1C34A0, 0x1C34C0}, {0x1C74A0, 0x1C74C0},
    {0x1CB4A0, 0x1CB4C0}, {0x1CF4A0, 0x1CF4C0},
    // (and their view bits, the first word's low byte)
    {0x1A0003, 0x1A0004}, {0x1A4003, 0x1A4004}, {0x1A8003, 0x1A8004}, {0x1AC003, 0x1AC004}, {0x1B0003, 0x1B0004},
    {0x1B4003, 0x1B4004}, {0x1B8003, 0x1B8004}, {0x1BC003, 0x1BC004}, {0x1C0003, 0x1C0004}, {0x1C4003, 0x1C4004},
    {0x1C8003, 0x1C8004}, {0x1CC003, 0x1CC004}};
std::size_t cabinet_own_size() {
    std::size_t n = 0;
    for (const auto& [lo, hi] : kCabinetOwn) n += hi - lo;
    return n;
}
uint32_t cabinet_own_byte(std::size_t k) { // (the k-th own byte's address)
    for (const auto& [lo, hi] : kCabinetOwn) {
        if (k < hi - lo) return lo + static_cast<uint32_t>(k);
        k -= hi - lo;
    }
    return 0;
}
void camera_written(void* user, uint32_t addr, uint32_t value, uint32_t size) {
    Machine& m = *static_cast<Machine*>(user);
    Machine::CameraTest& t = m.camera;
    const uint32_t pc = m.c.cpu.pc;
    if (t.events && !t.inside && !t.in_view && !t.in_update && addr - kCamera >= 8) { // --camera-events: who, how often
        // (by the writing block and its caller, LR: 0x37258 and 0x372D4 are called by a script's
        // action and by other code, so the block alone doesn't say whose the write is)
        auto& e = t.event_writers[static_cast<uint64_t>(pc) << 32 | m.c.cpu.lr];
        if (e.writes++ == 0) e.first = m.timing.frames();
        e.last = m.timing.frames();
        e.offsets.insert(addr - kCamera);
        e.seen.push_back({m.bus.read32(kLogicCounter), addr - kCamera, value}); // (by game frame: compared between boards)
        return;
    }
    if (t.inside || t.in_view || t.in_update || t.mine.empty()) return;
    if (t.free && pc == kShakeWrite) return; // (decided again for the other, camera_hook)
    // (this cabinet's own continue and name entry keep their working values in its camera, +0xB04-0xB1E: its flow's,
    // run for the other by the side flow)
    if (t.free && (pc == 0x7682C || pc == 0x76F78 || pc == 0x76868 || pc == 0x76944)) return;
    for (uint32_t k = 0; k < size; ++k) {
        const uint32_t at = addr + k - kCamera;
        if (at < 8 || at >= kCameraBytes || (at >= 0x4A8 && at < 0x4C0)) continue;
        const auto byte = static_cast<uint8_t>(value >> (8 * (size - 1 - k)));
        if (t.mirrored++ < 4000)
            std::printf("camera test: logic %u, the camera's +%03x set to %02x by %08x (the second camera's %02x)\n",
                        m.bus.read32(kLogicCounter), at, byte, pc, t.mine[at]);
        t.mine[at] = byte;
    }
}
constexpr uint32_t kViewMatrix = 0x640;
double ops_f(uint64_t bits) { // an FPR's double
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

// The hook at each camera update. --camera-record: the camera as the update
// starts, with the logic counter. --camera-test: the update run once more on
// the side, with the second camera in the camera's place (no scheduler, no
// interrupts; RAM, the CPU and the clock put back after), the result
// compared with the other board's camera as its next update starts, both
// as the second camera runs on by itself (free) and from the recorded one
// each time (one step).
uint64_t state_hash(const std::function<void(m3::State&)>& walk);

// --one-board B@FRAME: board B made by the other (two boards) from the end of FRAME (tools/one_board.hpp). With
// --one-board-camera (a --camera-record of it, by logic count): its side runs started ten frames before, its own
// variables from the leaving board's own as a flow of its started, the flows since run on. Without: board B handed
// over as a shadow is (Cabinets::running, ask_hand_back): it stops where the module asks, which takes its own
// variables, camera and own areas there, a slave's next count caught up (as network play will have it).
// More than one board may go at once (--one-board repeated; network play: every seat but this machine's), all made
// by the one that stays (network play's own; else the master unless it goes, else the first that stays); any
// others linked as usual. The camera recording, given missed frames, shadows and the rules: one board going only.
tools::one_board::Game g_one_board;
bool one_board_goes(const Options& o, std::size_t board) {
    return o.one_board >= 0 && (static_cast<std::size_t>(o.one_board) == board ||
                                std::find(o.one_board_also.begin(), o.one_board_also.end(), board) != o.one_board_also.end());
}
m3::CommBoardOptions link_options(const Options& o);
void set_up_one_board(std::vector<std::unique_ptr<Machine>>& machines, const Options& o) {
    tools::one_board::Game::Settings s;
    s.stays = o.netplay ? o.local_seat : o.one_board_stays >= 0 ? static_cast<std::size_t>(o.one_board_stays) : 0;
    while (one_board_goes(o, s.stays)) ++s.stays;
    s.going.push_back(static_cast<std::size_t>(o.one_board));
    for (std::size_t b : o.one_board_also) s.going.push_back(b);
    s.frame = o.one_board_frame;
    s.tasks = o.one_board_tasks, s.rules = o.one_board_auto, s.lookahead = o.one_board_lookahead;
    s.shadow_from = o.one_board_shadow, s.hand_back_at = o.one_board_hand_back;
    s.keep_ram = o.one_board_shadow != 0 || o.one_board_auto || !o.one_board_camera;
    s.slice = o.slice;
    s.net = o.netplay != nullptr; // (each board's table: given as it is made)
    if (o.netplay && !o.one_board_local_shadows) { // (shadows from the other machine's real board: --one-board-local-shadows, this one's)
        tools::NetPlay* net = o.netplay;
        s.ask_board = [net](std::size_t seat, uint64_t frame) { net->session().want_board(static_cast<uint8_t>(seat), frame); };
        s.wait_board = [net](std::size_t seat, uint64_t frame) -> std::optional<tools::one_board::RemoteBoard> {
            auto packed = net->wait_board(static_cast<uint8_t>(seat), frame, std::chrono::seconds(30));
            if (!packed) return std::nullopt;
            auto board = tools::one_board::unpack_remote(*packed, [](std::span<const uint8_t> d) { return net::decompress(d); });
            if (board && board->state.empty()) { // ("my backup of that frame": from the copy kept here)
                auto copy = net->backup_at(static_cast<uint8_t>(seat), frame);
                if (!copy) {
                    std::printf("board %zu: its backup of frame %llu not here\n", seat, (unsigned long long)frame);
                    return std::nullopt;
                }
                board->regions = std::move(copy->regions), board->small = std::move(copy->small);
                board->taken = copy->taken, board->sent = copy->sent;
            }
            return board;
        };
        s.rewind_link = [](std::size_t seat, const tools::one_board::RemoteBoard& b) {
            // (taken at the same point of the same frame, the counts are the same here and there; put right if not)
            if (!g_ring || (g_ring->taken(seat) == b.taken && g_ring->sent(seat) == b.sent)) return;
            std::printf("one board: board %zu's port there: taken %llu, sent %llu; here: taken %llu, sent %llu\n", seat,
                        (unsigned long long)b.taken, (unsigned long long)b.sent, (unsigned long long)g_ring->taken(seat),
                        (unsigned long long)g_ring->sent(seat));
            g_ring->rewind(seat, b.taken);
            g_ring->unsend(seat, b.sent);
        };
        // (a board whose player went without a parting: its last backup run on here; the packets to each board and
        // its player's inputs kept since its backup, for that)
        s.backup_frame = [net](std::size_t seat) { return net->keeper().frame(seat); };
        s.replay = [net, &o](std::size_t seat, uint64_t until, std::function<bool(uint64_t)> live) -> std::optional<tools::one_board::RemoteBoard> {
            // (a spectator: the newest of the backups kept at or before the frame, its players' taken ahead of it)
            const tools::backup::Copy copy = o.net_spectate ? net->keeper().copy_for(seat, until) : net->keeper().copy(seat);
            if (!copy.valid() || copy.frame > until) {
                const auto newest = net->keeper().frame(seat);
                std::printf("board %zu: no backup of it at or before frame %llu here (the newest: %s)\n", seat, (unsigned long long)until,
                            newest ? std::to_string(*newest).c_str() : "none");
                return std::nullopt;
            }
            tools::backup::Replay r{*g_roms, m3::games::kSpikeoutFinalEdition, link_options(o),
                                    [&o](m3::Bus& bus) { bus.midi_burst = o.midi_burst, bus.dma_timed = !o.dma_instant; },
                                    [](recomp::Context& c) { m3::games::install_spikeout_hooks(c); }, o.slice, copy,
                                    net->keeper().log(seat),
                                    [net, seat](uint64_t frame) { return net->keeper().inputs(seat, frame); }, std::move(live),
                                    [seat, &o](uint64_t frame, m3::Bus& bus) { // (its RAM each frame, compared with the board's own)
                                        if (!o.no_frame_hashes)
                                            std::printf("replay board %zu frame %llu: RAM %016llx\n", seat, (unsigned long long)frame,
                                                        (unsigned long long)tools::Recording::ram_hash(bus.ram()));
                                    }};
            try {
                auto result = r.run(until);
                if (!result) return std::nullopt;
                std::printf("board %zu: run on from its backup at frame %llu to %llu in %.1f s\n", seat, (unsigned long long)copy.frame,
                            (unsigned long long)until, result->seconds);
                return tools::one_board::RemoteBoard{std::move(result->state), result->taken, result->sent, {}, {}};
            } catch (const std::exception& e) {
                std::printf("board %zu: its backup can't be run on: %s\n", seat, e.what());
                return std::nullopt;
            }
        };
        // (a spectator on one board: no machine to ask; a cabinet's shadow is its player's last backup run on here to
        // the frame, as a board whose player went: the backups come from each player's machine, Session::set_watch_one_board)
        if (o.net_spectate) {
            s.ask_board = [](std::size_t, uint64_t) {};
            s.wait_board = [replay = s.replay](std::size_t seat, uint64_t frame) -> std::optional<tools::one_board::RemoteBoard> {
                return replay(seat, frame, [](uint64_t) { return true; }); // (its packets in the log: this board met it there)
            };
        }
        if (o.net_rollback) { // (--net-rollback: the switches only on frames that won't be run again; see Game::may_guess)
            s.guess_ahead = o.net_rollback;
            const std::size_t boards = o.boards;
            s.settled = [boards](uint64_t frame) {
                tools::InputTable* t = (*g_machines)[0]->inputs;
                return t->all_set(frame, boards) && t->wrong_from() > frame;
            };
            s.wait_settled = [boards, stays = o.local_seat](uint64_t frame) { // (this machine's own never guessed: set as its board reads it)
                tools::InputTable* t = (*g_machines)[0]->inputs;
                return t->wait_all(frame, boards, stays) && t->wrong_from() > frame;
            };
            s.part_whole = [](std::size_t board, bool whole, uint64_t) {
                if (g_net_rb) g_net_rb->set_active(board, whole);
            };
            s.rewound = [](std::size_t board) { return g_net_rb ? g_net_rb->rewound_to(board) : 0; };
            s.pin = [](uint64_t frame) {
                if (g_net_rb) g_net_rb->pin(frame);
            };
            if (!o.one_board_local_shadows) {
                s.has_board = [net](std::size_t seat, uint64_t frame, uint32_t ms) {
                    return net->has_board(static_cast<uint8_t>(seat), frame, std::chrono::milliseconds(ms));
                };
                s.run_again_from = [](uint64_t frame) { (*g_machines)[0]->inputs->run_again_from(frame); };
            }
        }
        if (g_ring)
            g_ring->on_departure([net](std::size_t to, uint64_t seq, uint64_t at, const std::vector<uint8_t>& data) {
                net->keeper().packet(to, seq, at, data);
                net->node_check().departed(to, seq, at, data);
            });
        s.record_fix = [net](std::size_t seat, tools::remote::Block& next, uint32_t rec) { net->node_check().fix_record(seat, next, rec); };
        s.header_fix = [net](std::size_t seat, const std::function<void(std::size_t, uint8_t)>& put) { net->node_check().fix_header(seat, put); };
        // (with rollback: a made packet sent as its real one once that has come; one come after, and different, gone
        // back for (tools::NodeCheck, step 3); NODE_CORRECT=0: only told)
        static const bool correct = !(std::getenv("NODE_CORRECT") && std::getenv("NODE_CORRECT")[0] == '0');
        if (o.net_rollback && g_ring && correct) {
            net->node_check().set_correcting([](uint64_t frame) { (*g_machines)[0]->inputs->run_again_from(frame); },
                                             [stays = o.local_seat] { return (*g_machines)[stays]->timing.frames(); });
            g_ring->on_patch([net](std::size_t from, uint64_t seq, std::vector<uint8_t>& data) { net->node_check().patch(from, seq, data); });
        }
    }
    if (!s.net && g_presses) // (its --press list's: bank 0 and the controls)
        s.buttons = [](std::size_t seat, uint64_t frame) -> std::optional<tools::InputTable::Inputs> {
            uint8_t in[2] = {0xFF, 0xFF};
            for (const Press& p : *g_presses)
                if (for_board(p.board, seat) && frame >= p.frame && frame < p.frame + p.frames)
                    in[p.bank] = static_cast<uint8_t>(in[p.bank] & ~(1u << p.bit));
            return tools::InputTable::Inputs{in[0], 0xFF, in[1], 0xFF};
        };
    s.cameras = std::make_shared<std::map<uint32_t, std::vector<uint8_t>>>();
    if (o.one_board_camera) {
        const std::vector<uint8_t> bytes = tools::read_file(o.one_board_camera);
        for (std::size_t at = 0; at + 4 + kCameraBytes <= bytes.size(); at += 4 + kCameraBytes) {
            uint32_t logic = 0;
            std::memcpy(&logic, bytes.data() + at, 4);
            (*s.cameras)[logic].assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 4), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4 + kCameraBytes));
        }
    }
    const auto cameras = s.cameras;
    std::vector<tools::one_board::Board*> boards;
    for (auto& machine : machines) boards.push_back(&machine->ob);
    Machine& m = *machines[s.stays];
    const std::size_t gone = s.going.front();
    g_one_board.set_up(std::move(boards), std::move(s));
    for (uint64_t f : o.one_board_missed) m.ob.cabinets->miss(gone, f);
    // (its camera on the side against the recording's, as its update starts: the first difference shown)
    m.ob.cabinets->on_camera = [cameras, shown = false](std::size_t seat, uint32_t logic, const std::vector<uint8_t>& camera) mutable {
        const auto next = cameras->find(logic);
        if (shown || next == cameras->end()) return;
        std::string at;
        for (uint32_t i = 8; i < kCameraBytes && at.size() < 400; ++i)
            if (camera[i] != next->second[i] && (i < 0x4A8 || i >= 0x4C0)) {
                char one[24];
                std::snprintf(one, sizeof one, " +%03x %02x/%02x", i, camera[i], next->second[i]);
                at += one;
            }
        if (at.empty()) return;
        shown = true;
        std::printf("one board: seat %zu's camera first differs as its update of count %u starts, side/recorded:%s\n", seat + 1, logic, at.c_str());
    };
}
void camera_hook(recomp::Context& c, uint32_t pc, void* user) {
    Machine& m = *static_cast<Machine*>(user);
    Machine::CameraTest& t = m.camera;
    if (t.trace_left && (pc == kCameraUpdate || pc == kViewTest)) { // --camera-trace N
        --t.trace_left;
        std::printf("%strace frame %llu logic %u: %s, LR %08x, r12 %08x, r14 %08x, f12-f15 %g %g %g %g, f19 %g\n",
                    t.inside ? "  (side) " : "", static_cast<unsigned long long>(m.timing.frames()), m.bus.read32(kLogicCounter),
                    pc == kCameraUpdate ? "camera update" : "view test", c.cpu.lr, c.cpu.gpr[12], c.cpu.gpr[14],
                    ops_f(c.cpu.fpr[12]), ops_f(c.cpu.fpr[13]), ops_f(c.cpu.fpr[14]), ops_f(c.cpu.fpr[15]), ops_f(c.cpu.fpr[19]));
        if (pc == kViewTest) { // the view matrix it uses (f20-f31), and where in the camera those values are as floats
            std::printf("  f20-f31:");
            for (int k = 20; k < 32; ++k) {
                const float v = static_cast<float>(ops_f(c.cpu.fpr[k]));
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                std::printf(" %g", v);
                for (uint32_t a = kCamera; a < kCamera + kCameraBytes; a += 4)
                    if (bits != 0 && m.bus.read32(a) == bits) std::printf("@+%x", a - kCamera);
            }
            std::printf("\n");
            const float first = static_cast<float>(ops_f(c.cpu.fpr[20]));
            uint32_t want;
            std::memcpy(&want, &first, 4);
            std::printf("  f20 found in RAM at:");
            for (uint32_t a = 0; a + 4 <= m.bus.ram().size(); a += 4)
                if (m.bus.read32(a) == want) std::printf(" %06x", a);
            std::printf("\n");
        }
    }
    if (t.inside) return; // (the side run: no other hooks either)
    if (pc == kRecordWrite) {
        if (!t.free) return;
        auto& players = t.players_at[m.bus.read32(kLogicCounter)];
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t p = 0x1A0000 + 0x4000 * k;
            players[k] = {m.bus.read32(p), m.bus.read32(p + 0x44), m.bus.read16(p + 0x2A), m.bus.read16(p + 0x74), m.bus.read16(p + 0x76)};
        }
        return;
    }
    if (pc == kShakeByAnimation || pc == kShakeFollowed) { // (the other cabinet's shake, into its camera run here)
        if (!t.free || t.mine.empty() || g_drop_board < 0) return;
        const uint32_t r13 = c.cpu.gpr[13], character = c.cpu.gpr[14];
        uint32_t kind = 0;
        if (pc == kShakeFollowed) {
            if (t.mine[0x4A] == m.bus.read8(character + 114)) kind = 7;
        } else {
            const uint32_t local = 0x1A0000 + 0x4000 * static_cast<uint32_t>(g_drop_board);
            bool shake;
            if (m.bus.read8(r13 - 16376) == 3) shake = (m.bus.read32(r13 - 16380) & 0x01080000u) != 0;
            else if (m.bus.read32(character) & 0x04000000u) shake = false;
            else shake = local == character || ((m.bus.read32(character + 68) & 0x1000u) && m.bus.read32(character + 272) == local);
            if (shake) kind = m.bus.read8(c.cpu.gpr[28] + 4);
        }
        if (kind) {
            const uint32_t entry = 0x90114 + 8 * (kind - 1);
            t.mine[0xD2] = t.mine[0xD3] = m.bus.read8(entry);
            for (uint32_t k = 0; k < 4; ++k) t.mine[0xD4 + k] = m.bus.read8(entry + 4 + k);
        }
        return;
    }
    if (pc == kActionDispatch || pc == kActionDispatchAtOnce) { // --script-actions: the record (r31) about to run, its action (byte 1)
        if (!t.actions) return;
        const uint32_t record = c.cpu.gpr[31];
        Machine::CameraTest::ActionRun& a = t.actions_run[record];
        if (a.runs++ == 0) a.action = m.bus.read8(record + 1), a.first = m.timing.frames();
        a.last = m.timing.frames();
        return;
    }
    if (pc == kCameraView) { // (--camera-test-mirror: its writes are the camera's own, until the next view test)
        t.in_view = true;
        return;
    }
    if (pc == kAfterUpdate) { // (and the update's, until it returns)
        if (c.cpu.lr == kAfterUpdateReturn) t.in_update = false;
        return;
    }
    if (pc == kCameraUpdate) t.in_update = true;
    if (pc == kCameraUpdate || pc == kViewTest) t.in_view = false;
    if (pc != kCameraUpdate && pc != kViewTest && pc != kModeDispatch)
        return m3::games::spikeout_hook(c, pc, nullptr);
    auto ram = m.bus.ram();
    const uint32_t logic = m.bus.read32(kLogicCounter);
    // The function at `pc` run on the side with `camera` in the camera's
    // place: the camera after it, and the character's (r14) flags word.
    auto side_run = [&](const std::vector<uint8_t>& camera, uint32_t* flags = nullptr) {
        const recomp::Context::Aside aside = c.set_aside();
        const std::vector<uint8_t> before(ram.begin(), ram.end());
        std::copy(camera.begin(), camera.end(), ram.begin() + kCamera);
        // (the other cabinet's own variables, its flow's: its camera reads them too, its mode and its time over)
        for (std::size_t k = 0; k < t.own.size(); ++k) ram[cabinet_own_byte(k)] = t.own[k];
        if (g_presses && g_drop_board >= 0) { // (the other cabinet's buttons, 0x5094's words: its camera reads them (shift);
                                              // its frame for this logic count one after this board's, a slave behind)
            auto held = [&](uint64_t frame) {
                uint8_t in[2] = {0xFF, 0xFF};
                for (const Press& p : *g_presses)
                    if (for_board(p.board, static_cast<std::size_t>(g_drop_board)) && frame >= p.frame && frame < p.frame + p.frames)
                        in[p.bank] = static_cast<uint8_t>(in[p.bank] & ~(1u << p.bit));
                return ~(uint32_t(in[0]) | uint32_t(in[1]) << 8 | 0xFFu << 16 | 0xFFu << 24);
            };
            // (the slave, board 1 up, a frame behind this one; the master, board 0, a frame ahead)
            const uint64_t frame = g_drop_board == 0 ? m.timing.frames() - 1 : m.timing.frames() + 1;
            const uint32_t now = held(frame), was = held(frame - 1);
            m.bus.write32(0x100100, now), m.bus.write32(0x100104, now & ~was), m.bus.write32(0x100108, was & ~now);
            if (!t.own.empty() && t.toggled_at != frame) // (its toggles, 0x10010C, the pressed ones flipped once a frame: kept in its own)
                m.bus.write32(0x10010C, m.bus.read32(0x10010C) ^ (now & ~was)), t.toggled_at = frame;
            // (and that cabinet's local player, as --clone-seat sets it)
            const auto b = static_cast<uint32_t>(g_drop_board);
            m.bus.write32(0x104024, 0x1A0000 + 0x4000 * b);
            m.bus.write8(0x10065B, static_cast<uint8_t>(b + 1));
            m.bus.write32(0x106008, 0x1062AE + 2 * (b + 1));
        }
        c.set_scheduler(nullptr);
        c.cpu.msr &= ~recomp::Context::kMsrEE;
        c.limit_aside(30'000'000); // (not the board's turn's budget; ten frames at most)
        t.inside = true;
        m.bus.hold_device_writes(true); // (its sounds, a hit's, not this board's: only RAM is put back after)
        const auto started = std::chrono::steady_clock::now();
        try {
            if (pc == kViewTest) { // with this camera's view matrix, as the game has it in f20-f31 then
                for (int k = 0; k < 12; ++k) {
                    uint32_t bits = m.bus.read32(kCamera + kViewMatrix + 4 * static_cast<uint32_t>(k));
                    float f;
                    std::memcpy(&f, &bits, 4);
                    const double d = f;
                    std::memcpy(&c.cpu.fpr[static_cast<std::size_t>(20 + k)], &d, 8);
                }
                c.call(pc);
            } else if (pc == kModeDispatch) { // the cabinet's flow (its mode's routine)
                c.call(pc);
            } else { // the update, then what makes its view
                c.call(pc);
                c.cpu = aside.cpu;
                c.cpu.msr &= ~recomp::Context::kMsrEE;
                c.cpu.gpr[12] = kCamera;
                c.call(kCameraView);
            }
        } catch (const std::exception& e) {
            if (t.failed.empty()) t.failed = e.what();
        }
        t.side_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); // (the game code only)
        t.inside = false;
        m.bus.hold_device_writes(false);
        std::vector<uint8_t> out(ram.begin() + kCamera, ram.begin() + kCamera + kCameraBytes);
        if (flags) *flags = m.bus.read32(aside.cpu.gpr[14]);
        for (std::size_t k = 0; k < t.own.size(); ++k) t.own[k] = ram[cabinet_own_byte(k)]; // (its own after: its flow's, its camera's)
        if (pc == kModeDispatch) { // (and its header after its flow, 0x140100: its status at +0x06)
            std::copy(ram.begin() + 0x140100, ram.begin() + 0x140200, t.header_at[logic].begin());
        }
        if (pc == kCameraUpdate)
            for (std::size_t i = 0; i < ram.size(); ++i)
                if (ram[i] != before[i] && (i < kCamera || i >= kCamera + kCameraBytes)) ++t.written[static_cast<uint32_t>(i & ~15u)];
        std::copy(before.begin(), before.end(), ram.begin());
        c.put_back(aside);
        ++t.runs;
        return out;
    };
    if (pc == kModeDispatch) {
        if (static_cast<int>(m.index) == g_drop_board) { // (the real cabinet, before its PC goes: its own bytes as its flow of
                                                         // this logic count starts, kept to start the side flow from, and checked)
            std::vector<uint8_t> mine(cabinet_own_size());
            for (std::size_t k = 0; k < mine.size(); ++k) mine[k] = ram[cabinet_own_byte(k)];
            Machine& b0 = *(*g_machines)[g_drop_board == 0 ? 1 : 0]; // (the board running its side flow)
            std::lock_guard guard(g_clone_lock);
            if (const auto side = b0.camera.own_at.find(logic - 1); side != b0.camera.own_at.end() && side->second != mine) {
                static int shown = 0;
                if (shown++ < 12) {
                    std::printf("side flow: after logic %u (frame %llu) differs from board %d's own:", logic - 1,
                                (unsigned long long)m.timing.frames(), g_drop_board);
                    for (std::size_t k = 0, n = 0; k < mine.size() && n < 16; ++k)
                        if (side->second[k] != mine[k]) std::printf(" %06x %02x/%02x", cabinet_own_byte(k), side->second[k], mine[k]), ++n;
                    std::printf("\n");
                }
            }
            b0.camera.own_seed[logic - 1] = std::move(mine);
            return;
        }
        if (!t.free || t.mine.empty() || g_drop_board < 0) return;
        uint32_t runs = 1;
        if (t.own.empty()) { // (started from that cabinet's own bytes after the newest logic count it has run, its flows
                             // since then run here first, with this board's shared world: approximate)
            std::lock_guard guard(g_clone_lock);
            if (t.own_seed.empty() || t.own_seed.rbegin()->first >= logic) return;
            t.own = t.own_seed.rbegin()->second;
            runs = logic - t.own_seed.rbegin()->first;
        }
        for (uint32_t k = 0; k < runs; ++k) t.mine = side_run(t.mine);
        std::lock_guard guard(g_clone_lock);
        t.own_at[logic] = t.own;
        return;
    }
    if (pc == kViewTest && t.free) { // (--camera-test-free: kept for the made blocks)
        if (t.mine.empty()) return;
        uint32_t flags = 0;
        side_run(t.mine, &flags);
        t.view_at[{logic, c.cpu.gpr[14]}] = flags;
        return;
    }
    if (pc == kCameraUpdate && t.free) {
        if (t.mine.empty()) { // (started from the other's recorded camera, as its update for this logic count starts)
            if (m.timing.frames() < t.from) return;
            const auto in = t.recorded.find(logic);
            if (in == t.recorded.end()) return;
            t.mine = in->second, t.first_logic = logic;
        }
        t.mine = side_run(t.mine);
        if (!t.first_free_miss) // (against the other's recorded camera, as its next update starts: the first difference shown)
            if (const auto next = t.recorded.find(logic + 1); next != t.recorded.end()) {
                std::string at;
                for (uint32_t i = 8; i < kCameraBytes; ++i)
                    if (t.mine[i] != next->second[i] && (i < 0x4A8 || i >= 0x4C0) && at.size() < 600) {
                        char one[32];
                        std::snprintf(one, sizeof one, " +%03x %02x/%02x", i, t.mine[i], next->second[i]);
                        at += one;
                    }
                if (!at.empty()) {
                    t.first_free_miss = logic;
                    std::printf("side camera first differs after update of logic %u (frame %llu), side/real:%s\n", logic,
                                (unsigned long long)m.timing.frames(), at.c_str());
                }
            }
        t.heading_at[logic] = static_cast<uint16_t>(t.mine[kCameraHeading] << 8 | t.mine[kCameraHeading + 1]);
        return;
    }
    if (pc == kViewTest) { // the other camera's view of this character, written down (logic, character, flags word)
        if (!t.testing || t.mine.empty() || !t.view_out) return;
        uint32_t flags = 0;
        side_run(t.mine, &flags);
        const uint32_t row[3] = {logic, c.cpu.gpr[14], flags};
        std::fwrite(row, sizeof row, 1, t.view_out);
        ++t.view_tests;
        return;
    }
    ++t.calls;
    if (t.record) {
        std::fwrite(&logic, sizeof logic, 1, t.record);
        std::fwrite(ram.data() + kCamera, 1, kCameraBytes, t.record);
        return;
    }
    if (!t.testing) return;
    const auto in = t.recorded.find(logic), next = t.recorded.find(logic + 1);
    if (in == t.recorded.end()) return;
    if (t.mine.empty() && m.timing.frames() < t.from) return; // (--camera-test-from FRAME)
    if (t.mine.empty()) t.mine = in->second, t.first_logic = logic;
    auto heading = [](const std::vector<uint8_t>& cam) { return cam[kCameraHeading] << 8 | cam[kCameraHeading + 1]; };
    if (t.copy_flags) { // the camera's flags word as this board's code made it this frame, but its own 0x00200000 bit
        const uint32_t own = (static_cast<uint32_t>(t.mine[1]) << 16) & 0x00200000u;
        const uint32_t here = m.bus.read32(kCamera);
        const uint32_t flags = (here & ~0x00200000u) | own;
        for (int k = 0; k < 4; ++k) t.mine[static_cast<std::size_t>(k)] = static_cast<uint8_t>(flags >> (24 - 8 * k));
    }
    t.mine = side_run(t.mine);
    if (t.mine_out) { // FILE.mine: the second camera after each update (the logic counter, its bytes)
        std::fwrite(&logic, sizeof logic, 1, t.mine_out);
        std::fwrite(t.mine.data(), 1, t.mine.size(), t.mine_out);
    }
    if (t.view_out) { // (and the second camera's heading: character 0xFFFFFFFF)
        const uint32_t row[3] = {logic, 0xFFFFFFFFu, static_cast<uint32_t>(heading(t.mine))};
        std::fwrite(row, sizeof row, 1, t.view_out);
    }
    const std::vector<uint8_t> step = side_run(in->second);
    if (next == t.recorded.end()) return;
    ++t.compared;
    const bool free_same = heading(t.mine) == heading(next->second);
    t.heading_free += free_same, t.object_free += t.mine == next->second;
    if (!free_same && !t.first_free_miss) t.first_free_miss = logic;
    t.heading_step += heading(step) == heading(next->second), t.object_step += step == next->second;
}

// --rollback-test: what a snapshot walks (:quiet: not the sound board, which
// hears each frame once).
std::function<void(m3::State&)> rollback_walk(Machine& m, const Options& o) {
    return [&m, &o, sound = o.rb_quiet ? std::function<void(m3::State&)>() : sound_state(m, o)](m3::State& s) {
        tools::board_state(s, m.c, m.timing, sound);
    };
}

uint64_t hash_bytes(uint64_t h, const uint8_t* d, std::size_t n) {
    h ^= n;
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, d + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ d[i]) * 0x100000001B3ull;
    return h;
}

// A board's whole state (as a save state has it), hashed.
uint64_t state_hash(const std::function<void(m3::State&)>& walk) {
    m3::State s;
    walk(s);
    return hash_bytes(0x9E3779B97F4A7C15ull, s.data().data(), s.data().size());
}

// Only its big buffers (State::fixed's: what snapshots keep by page), hashed.
uint64_t buffers_hash(const std::function<void(m3::State&)>& walk) {
    struct Hash : m3::State::Regions {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        void region(void* data, std::size_t bytes) override { h = hash_bytes(h, static_cast<const uint8_t*>(data), bytes); }
    } hash;
    m3::State s;
    s.set_regions(&hash);
    walk(s);
    return hash.h;
}

// --rollback-test on linked boards (--parallel): each board keeps its own
// snapshots (Machine::RollbackTest, at its own point after each frame); the
// link ring's part of each frame (m3::LinkRing::cut_state: the packets on
// their way between those points) is kept here, cut at the slices' meeting
// once every board has saved that frame. Going back is done there too, with
// every board stopped: each board and the ring as they were at the same
// frame, as a linked save state loads; each board then goes on from its
// own point (Rewound, thrown as it leaves the meeting).
struct LinkedRollback {
    m3::LinkRing* ring = nullptr;
    std::vector<Machine*> boards;
    std::map<uint64_t, std::vector<uint8_t>> cuts; // the ring's part, by frame
    std::map<uint64_t, uint64_t> hashes;           // its hash the first time
    std::set<uint64_t> rolled;
    uint64_t checked = 0, differed = 0, first_differ = 0, restores = 0, unmatched = 0;
    double restore_ms = 0, restore_worst = 0;

    void between(const Options& o) { // (at the meeting, every board stopped)
        if (!ring->cut_complete()) return;
        const uint64_t frame = boards[0]->rb.saved_frame;
        for (Machine* m : boards)
            if (m->rb.saved_frame != frame && unmatched++ == 0)
                std::fprintf(stderr, "gamerun: --rollback-test: the boards saved different frames (%llu, %llu)\n",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(m->rb.saved_frame));
        m3::State s;
        ring->cut_state(s);
        const uint64_t h = hash_bytes(0x9E3779B97F4A7C15ull, s.data().data(), s.data().size());
        if (auto it = hashes.find(frame); it == hashes.end()) {
            hashes[frame] = h;
        } else {
            ++checked;
            if (it->second != h && differed++ == 0) first_differ = frame;
        }
        cuts[frame] = std::move(s.data());
        if (frame > o.rb_depth) cuts.erase(cuts.begin(), cuts.lower_bound(frame - o.rb_depth));
        if ((frame - o.rb_from) % o.rb_every != o.rb_every - 1 || frame < o.rb_from + o.rb_depth || !rolled.insert(frame).second)
            return;
        const uint64_t back = frame - o.rb_depth;
        const auto start = std::chrono::steady_clock::now();
        for (Machine* m : boards) { // (each board's own walk marks the ring's port saved: the ring's load after clears it)
            Machine::RollbackTest& t = m->rb;
            const auto walk = rollback_walk(*m, o);
            const auto t1 = std::chrono::steady_clock::now();
            t.snaps->restore(back, walk);
            const double restore = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            t.restore_ms += restore, t.restore_worst = std::max(t.restore_worst, restore);
            ++t.restores;
            if (!o.rb_timed && buffers_hash(walk) == t.buffers[back]) ++t.restored_exact;
            t.rewind = true;
            t.quiet_through = frame;
            t.back_at = start;
        }
        m3::State load(std::vector<uint8_t>(cuts.at(back)));
        ring->cut_state(load);
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        restore_ms += took, restore_worst = std::max(restore_worst, took);
        ++restores;
    }
};

// The network board's options from the switches (also in the build's id
// for network play: machines with others can't play together).
m3::CommBoardOptions link_options(const Options& o) {
    m3::CommBoardOptions options;
    options.irq5_on_e0 = o.irq5_on_e0;
    options.irq5_per_frame = o.irq5_per_frame;
    options.slave_irq5_per_frame = o.slave_irq5_per_frame;
    options.irq5_phase = o.irq5_phase;
    options.ring_forwarding = o.ring_forwarding;
    options.double_buffer = o.double_buffer;
    if (o.old_link) options.use_old_link();
    options.link_cycles_per_byte = o.link_cycles_per_byte;
    options.send_waits_for_receive = o.send_waits_for_receive;
    options.host_sees_other_bank = o.host_sees_other_bank;
    return options;
}

// GAMERUN_ONEPC_CHECK: board 0's copy of player 2's first word, by frame, as board 0 wrote its own record's.
std::map<uint64_t, uint32_t>& onepc_p2_word() {
    static std::map<uint64_t, uint32_t> seen;
    return seen;
}

void set_up(Machine& m, const Options& o, const m3::GameSpec& game, m3::LinkRing* ring, tools::Turns& turns) {
    m3::Bus& bus = m.bus;
    bus.midi_burst = o.midi_burst;
    bus.dma_timed = !o.dma_instant;
    bus.set_security_key(game.security_key);
    // RAM from power-on as the window has it (zero). It was 0xA5 here once:
    // the game's pictures and sound are the same either way (it writes
    // before it reads), but network play compares all of RAM, so gamerun
    // and the window must agree (docs/netplay.md).
    if (o.replay) { // as the window started the recorded run: its clock, EEPROM and backup RAM
        bus.rtc().set_time(o.replay->clock, 0);
        std::copy(o.replay->eeprom.begin(), o.replay->eeprom.end(), bus.eeprom().words().begin());
        if (o.replay->backup.size() == m3::Bus::kBackupRamSize)
            std::copy(o.replay->backup.begin(), o.replay->backup.end(), bus.backup_ram().begin());
    } else {
        if (o.clock) bus.rtc().set_time(*o.clock, 0);
        else tools::set_rtc_to_host(bus);
    }
    const std::vector<uint8_t>& eeprom =
        m.index < o.board_eeprom.size() && o.board_eeprom[m.index].size() == 128 ? o.board_eeprom[m.index] : o.eeprom_bytes;
    if (eeprom.size() == 128)
        for (std::size_t i = 0; i < 64; ++i) bus.eeprom().words()[i] = static_cast<uint16_t>(eeprom[2 * i] << 8 | eeprom[2 * i + 1]);
    if (m.index < o.board_backup.size() && o.board_backup[m.index].size() == m3::Bus::kBackupRamSize)
        std::copy(o.board_backup[m.index].begin(), o.board_backup[m.index].end(), bus.backup_ram().begin());
    if (o.real_netboard) {
        auto board = std::make_unique<m3::CommBoard>(ring ? ring->port(m.index) : nullptr, link_options(o));
        m.comm = board.get();
        m.comm->trace_limit = o.net_trace;
        m.comm->force_master = o.force_master;
        m.recording_host = o.link_record && static_cast<int>(m.index) == o.link_record_board;
        if (m.recording_host) bus.use_netboard(std::make_unique<RecordingNetBoard>(std::move(board), m.host_log));
        else bus.use_netboard(std::move(board));
    } else if (ring) {
        bus.use_netboard(std::make_unique<m3::SimNetBoard>(ring->port(m.index)));
    }
    m.c.set_scheduler(&m.timing);
    if (o.load_state || (m.linked && !m.linked->loaded.empty())) { // everything above that the game sees, as it was saved
        try {
            m3::State s(m.linked ? std::move(m.linked->loaded[m.index]) : tools::read_state_file(tools::read_file(o.load_state)).state);
            tools::board_state(s, m.c, m.timing, sound_state(m, o));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "gamerun: %s: %s\n", o.load_state ? o.load_state : "the host's state", e.what());
            std::exit(1);
        }
        m.loaded = true;
        std::printf("Loaded %s: frame %llu, block %08x\n", o.load_state ? o.load_state : "the host's state",
                    static_cast<unsigned long long>(m.timing.frames()), m.c.cpu.pc);
        // --inputs: the bot's writes after the state's frame and the first
        // frame's inputs, here where it resumed (as it did after its load)
        if (o.input_log && m.index == 0) {
            if (auto it = o.input_log->frames.find(m.timing.frames()); it != o.input_log->frames.end())
                for (const auto& [addr, value] : it->second.writes) {
                    m.bus.ram()[addr] = static_cast<uint8_t>(value >> 8);
                    m.bus.ram()[addr + 1] = static_cast<uint8_t>(value);
                }
            if (auto it = o.input_log->frames.find(m.timing.frames() + 1); it != o.input_log->frames.end()) {
                m.bus.inputs.bank0 = it->second.bank0;
                m.bus.inputs.controls = it->second.controls;
            }
        }
        if (o.linked_inputs) linked_change(m, o, tools::LinkedInputLog::At::Resumed, m.timing.frames());
    }
    m.c.limit_blocks(static_cast<uint64_t>(o.blocks));
    if (o.map_trace_frames) // --map-trace FROM:N: each tile map the game draws, its layer and rows
        bus.tile_sides().on_map = [&m, from = o.map_trace_from, to = o.map_trace_from + o.map_trace_frames](int layer, uint16_t map,
                                                                                                             int first, int end) {
            const uint64_t frame = m.timing.frames();
            if (frame >= from && frame < to)
                std::printf("%sframe %llu: map %02x on layer %d, rows %d-%d\n",
                            m.index ? ("board " + std::to_string(m.index) + " ").c_str() : "", static_cast<unsigned long long>(frame),
                            map, layer, first, end - 1);
        };
    if (o.watch_hi > o.watch_lo) { // --watch-write: each write there, where the code was, and its registers
        m.watch_from = o.watch_frame;
        m.watch_to = o.watch_frame + o.watch_frames;
        bus.watch_writes(o.watch_lo, o.watch_hi, [](void* user, uint32_t addr, uint32_t value, uint32_t size) {
            Machine& w = *static_cast<Machine*>(user);
            const uint64_t frame = w.timing.frames();
            if (frame < w.watch_from || frame >= w.watch_to) return;
            const auto& r = w.c.cpu.gpr;
            std::printf("%sframe %llu: write%u %08x = %08x at block %08x, LR %08x; r3-r12 %08x %08x %08x %08x %08x %08x %08x %08x "
                        "%08x %08x, r14 %08x, r24-r31 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                        w.index ? ("board " + std::to_string(w.index) + " ").c_str() : "",
                        static_cast<unsigned long long>(frame), 8 * size, addr, value, w.c.cpu.pc, w.c.cpu.lr, r[3], r[4], r[5],
                        r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[14], r[24], r[25], r[26], r[27], r[28], r[29], r[30], r[31]);
        }, &m);
    }
    m.real3d.set_probe(o.probe_x, o.probe_y);
    m.real3d.set_quads(o.quads);
    m.real3d.set_fog_over_nothing(o.fog_over_nothing);
    m.builder.set_fog_over_nothing(o.fog_over_nothing);
    m.match_builder.set_fog_over_nothing(o.fog_over_nothing);
    m.sound.board.set_volumes(o.sound_volume, o.music_volume);

    // The 2D layers, a line at a time; each finished frame is kept for --screenshot.
    // With several boards, the turn passes on every o.slice lines, the
    // network board caught up first so what it sent is on the link.
    m.timing.on_display_line = [&m, &turns, slice = o.slice, no_2d = o.no_2d, quiet = o.rb_quiet](uint32_t line) {
        if (!no_2d && !(quiet && m.timing.frames() < m.rb.quiet_through) && !(g_net_rb && g_net_rb->quiet(m.index, m.timing.frames() + 1))) { // (:quiet: not in a frame run again)
            const auto start = std::chrono::steady_clock::now();
            if (line == 0) {
                m.tilegen.begin_frame();
                ++m.tilegen_frames;
            }
            m.tilegen.draw_line(m.bus, static_cast<int>(line));
            m.tilegen_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        if (turns.boards() > 1 && line % slice == slice - 1) {
            m.bus.netboard().sync(m.c.cycles());
            // (tests: GAMERUN_MEET_FRAMES=B:FROM:TO, board B's network board hashed at each meeting of those frames)
            static long long meet_board = -1;
            static unsigned long long meet_from = 0, meet_to = 0;
            if (static const char* spec = std::getenv("GAMERUN_MEET_FRAMES"); spec && meet_board == -1)
                std::sscanf(spec, "%lld:%llu:%llu", &meet_board, &meet_from, &meet_to);
            if (static_cast<long long>(m.index) == meet_board && m.timing.frames() >= meet_from && m.timing.frames() <= meet_to) {
                m3::State nb;
                m.bus.netboard().state(nb);
                if (g_ring) g_ring->abandon_cut(m.index);
                const uint64_t f = m.timing.frames();
                std::printf("meet board %zu frame %llu line %u: netboard %016llx, taken %llu, cycles %llu (line start %llu)\n", m.index,
                            (unsigned long long)f, line, (unsigned long long)state_hash([&](m3::State& s) {
                                std::vector<uint8_t> b = nb.data();
                                s.io(b);
                            }),
                            (unsigned long long)(g_ring ? g_ring->taken(m.index) : 0), (unsigned long long)m.c.cycles(),
                            (unsigned long long)(m.timing.frame_start(f) + m.timing.line_cycles() * (recomp::FrameTiming::kVBlankLines + line)));
            }
            if (m.recording_host) m.host_log.push_back({HostCall::Pass, 0, 0, 0, m.c.cycles()});
            m.line = line;
            tools::one_board::Game::meet(m.ob, line); // (--one-board: the other cabinets' blocks due)
            turns.pass(m.index);
            if (g_net_rb && g_net_rb->rewind(m.index)) throw tools::rollback::Rewound{}; // (--net-rollback: gone back, with the others)
            if (m.rb.rewind) { // (--rollback-test: put back at the meeting, with the others)
                m.rb.rewind = false;
                throw Rewound{};
            }
        }
    };
    // GAMERUN_ONEPC_CHECK: board 0's copy of player 2's first word as board 0 writes its own record's
    // +0x10 (0x6DA90, the same point of its frame as board 1 writes its)
    if (std::getenv("GAMERUN_ONEPC_CHECK") && m.index == 0)
        bus.watch_writes(0x140210, 0x140214, [](void* user, uint32_t addr, uint32_t, uint32_t) {
            Machine& b = *static_cast<Machine*>(user);
            if (addr == 0x140210) onepc_p2_word()[b.timing.frames()] = b.bus.read32(0x1A4000);
        }, &m);
    m.timing.on_frame_end = [&m, &o] {
        const uint64_t frame = m.timing.frames();
        tools::one_board::Game::end_frame(m.ob); // (--one-board: the other cabinets' frames end with this board's)
        if (o.one_board_whole && frame == o.one_board_whole && m.ob.cabinets) { // --one-board-whole FRAME
            tools::one_board::Game::keep_whole(m.ob);
            std::printf("board %zu: the other run whole from frame %llu\n", m.index, (unsigned long long)frame);
        }
        // (at the first frame's end from FRAME on: a network game joined after --net-one-board's frame starts later, and
        // its boards, never at that frame, stayed whole for good, the side runs of their seeds failing every frame beside
        // them (a joiner at 16-28 frames a second, 2026-11-06))
        if (one_board_goes(o, m.index) && frame >= o.one_board_frame && !m.one_board_handed && !o.one_board_late) { // --one-board B@FRAME
            m.one_board_handed = true;
            if (o.one_board_camera) throw Dropped{2400000, o.slice, true, o.one_board_shadow != 0 || o.one_board_auto};
            g_one_board.hand_over(m.ob, o.one_board_held); // (--one-board-held: handed back when the rules find it calm)
        }
        if (o.drop_frame && static_cast<int>(m.index) == o.drop_board && frame == o.drop_frame) // --drop B@FRAME: its PC gone here
            throw Dropped{o.link_standin_at ? o.link_standin_at : 2400000, o.slice};
        if (o.clone_frame && frame + 1 == o.clone_frame && m.index == 0) // --clone-seat: board 0's state, for board B (a frame
                                                                          // before: a slave's world is the master's a frame back)
            m.c.request_snapshot([&m](uint32_t) {
                m3::State s;
                tools::board_state(s, m.c, m.timing, {});
                std::lock_guard guard(g_clone_lock);
                g_clone_state = std::move(s.data());
            });
        if (o.clone_frame && frame == o.clone_frame && static_cast<int>(m.index) == o.clone_board)
            m.c.request_snapshot([&m](uint32_t) {
                // (this cabinet's own: its network board and EEPROM, its link buffers, its counters)
                m3::State nb, ee;
                m.bus.netboard().state(nb);
                m.bus.eeprom().state(ee);
                const std::vector<uint8_t> link(m.bus.ram().begin() + 0x140000, m.bus.ram().begin() + 0x143000);
                const uint32_t counters[3] = {m.bus.read32(0x1000A4), m.bus.read32(0x1000A8), m.bus.read32(0x140048)};
                std::vector<uint8_t> from;
                {
                    std::lock_guard guard(g_clone_lock);
                    from = g_clone_state;
                }
                if (from.empty()) throw std::runtime_error("--clone-seat: no state of board 0's at this frame");
                const uint64_t frames_here = m.timing.frames();
                m3::State s(std::move(from));
                tools::board_state(s, m.c, m.timing, {});
                if (const uint64_t behind = frames_here - m.timing.frames()) { // (its time, as its network board has it)
                    const uint64_t shift = behind * m.timing.frame_cycles();
                    m.timing.move_on(behind);
                    recomp::Context::Aside a = m.c.set_aside();
                    a.cycles += shift, a.next_check += shift, a.scheduler_due += shift;
                    a.tb_offset -= shift * 33 / 332;
                    m.c.put_back(a);
                }
                m3::State nb_in(std::move(nb.data())), ee_in(std::move(ee.data()));
                m.bus.netboard().state(nb_in);
                m.bus.eeprom().state(ee_in);
                std::copy(link.begin(), link.end(), m.bus.ram().begin() + 0x140000);
                m.bus.write32(0x1000A4, counters[0]), m.bus.write32(0x1000A8, counters[1]), m.bus.write32(0x140048, counters[2]);
                const uint32_t player = 0x1A0000 + 0x4000 * static_cast<uint32_t>(m.index);
                m.bus.write32(0x104024, player);                          // the local player's object
                m.bus.write8(0x10065B, static_cast<uint8_t>(m.index + 1)); // its number
                m.bus.write32(0x106008, 0x1062AE + 2 * static_cast<uint32_t>(m.index + 1)); // its time's entry (as 0x11C3C sets it)
                m.bus.write8(0x10019E, 2);                                // the link: a slave
                m.bus.write8(kCamera + 0x4A, static_cast<uint8_t>(m.index)); // the player its camera follows
                std::printf("board %zu: board 0's state taken as cabinet %zu at frame %llu\n", m.index, m.index + 1,
                            static_cast<unsigned long long>(m.timing.frames()));
                throw Cloned{};
            });
        if (o.wait_from && static_cast<int>(m.index) == o.wait_board && frame == o.wait_from) // --wait-seat: stopped at a clean point
            m.c.request_snapshot([&o](uint32_t) { throw Frozen{o.wait_until, o.link_standin_at ? o.link_standin_at : 2400000, o.slice}; });
        // GAMERUN_ONEPC_CHECK (two boards): how much of board 1's record (0x140200) board 0 has: its
        // copy of player 2 (0x1A4000) at the same frame's end and the one before, against the
        // record's +0x08 (the player's +0x2A), +0x0A (its +0x74 or +0x76) and +0x10 (its first word).
        static const bool onepc_check = std::getenv("GAMERUN_ONEPC_CHECK") != nullptr;
        if (onepc_check) {
            struct Seen { uint32_t word, h2a, h74, h76; };
            static std::mutex lock;
            static std::map<uint64_t, Seen> board0;
            static uint64_t checked = 0, same[6] = {};
            std::lock_guard guard(lock);
            if (m.index == 0) {
                board0[frame] = {m.bus.read32(0x1A4000), m.bus.read16(0x1A402A), m.bus.read16(0x1A4074), m.bus.read16(0x1A4076)};
            } else if (m.index == 1) {
                const auto now = board0.find(frame), before = board0.find(frame - 1);
                if (now != board0.end() && before != board0.end()) {
                    const uint32_t r10 = m.bus.read32(0x140210);
                    const uint16_t r08 = m.bus.read16(0x140208), r0a = m.bus.read16(0x14020A);
                    ++checked;
                    same[0] += r08 == now->second.h2a, same[1] += r08 == before->second.h2a;
                    same[2] += r0a == now->second.h74 || r0a == now->second.h76, same[3] += r0a == before->second.h74 || r0a == before->second.h76;
                    static uint64_t as74 = 0, as76 = 0, both = 0; // (+0x0A: which of the two it is)
                    as74 += r0a == before->second.h74, as76 += r0a == before->second.h76, both += before->second.h74 == before->second.h76;
                    if (checked % 3500 == 0)
                        std::printf("onepc check: +0A as +74 %llu, as +76 %llu, the two the same %llu\n", (unsigned long long)as74,
                                    (unsigned long long)as76, (unsigned long long)both);
                    same[4] += r10 == now->second.word, same[5] += r10 == before->second.word;
                    static std::map<uint32_t, uint64_t> xors; // (+0x10's differences from the copy as board 0 wrote its own)
                    static uint64_t at_write = 0;
                    if (auto w = onepc_p2_word().find(frame - 1); w != onepc_p2_word().end()) { // (written during the frame now ended)
                        at_write += r10 == w->second;
                        if (r10 != w->second && ++xors[r10 ^ w->second] <= 3)
                            std::printf("onepc check: frame %llu +10 %08x, board 0's copy %08x\n", (unsigned long long)frame, r10, w->second);
                    }
                    static uint64_t earlier[3] = {}; // (board 0's copy 1, 2 and 3 frames before)
                    for (int k = 0; k < 3; ++k)
                        if (auto w = onepc_p2_word().find(frame - 2 - k); w != onepc_p2_word().end()) earlier[k] += r10 == w->second;
                    if (checked % 500 == 0)
                        std::printf("onepc check: +10 as board 0 wrote its own %llu; a frame before %llu, two %llu, three %llu\n",
                                    (unsigned long long)at_write, (unsigned long long)earlier[0], (unsigned long long)earlier[1],
                                    (unsigned long long)earlier[2]);
                    if (checked % 3500 == 0)
                        for (const auto& [x, n] : xors) std::printf("onepc check: +10 differs by %08x %llu times\n", x, (unsigned long long)n);
                    if (checked % 500 == 0)
                        std::printf("onepc check, %llu frames: +08 %llu/%llu (this frame/the one before), +0A %llu/%llu, +10 %llu/%llu\n",
                                    (unsigned long long)checked, (unsigned long long)same[0], (unsigned long long)same[1],
                                    (unsigned long long)same[2], (unsigned long long)same[3], (unsigned long long)same[4],
                                    (unsigned long long)same[5]);
                }
            }
        }
        // --rollback-test: a frame run again after going back (:quiet: drawn
        // and heard the first time only); the re-runs timed.
        const bool quiet = (o.rb_quiet && frame <= m.rb.quiet_through) || (g_net_rb && g_net_rb->quiet(m.index, frame));
        if (frame == m.rb.quiet_through && m.rb.back_at != std::chrono::steady_clock::time_point{}) {
            m.rb.rerun_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - m.rb.back_at).count();
            m.rb.rerun_frames += o.rb_depth;
            m.rb.back_at = {};
        }
        if (const uint64_t up = m.bus.real3d().textures_uploaded(); up != m.textures_seen) { // a frame that changed textures
            m.textures_seen = up;
            ++m.texture_frames;
        }
        if (o.match_models) { // each frame's models found in the frame before's (smooth motion)
            std::swap(m.match_scene[0], m.match_scene[1]);
            m.match_builder.build(m.bus.real3d(), m.match_scene[1]);
            const auto& before = m.match_scene[0].models;
            const auto& now = m.match_scene[1].models;
            std::unordered_map<uint64_t, int> keys; // (viewport, address, path): how many models in the frame before
            auto key = [](const m3::Real3dScene::Model& model) {
                return static_cast<uint64_t>(model.viewport) << 48 ^ static_cast<uint64_t>(model.address) << 24 ^ model.path;
            };
            for (const auto& model : before) ++keys[key(model)];
            for (const auto& model : now) {
                const auto it = keys.find(key(model));
                if (it == keys.end()) {
                    ++m.match_unmatched;
                    if (model.address < 0x100000) ++m.match_unmatched_ram; // a model the game writes (polygon RAM)
                    bool moved = false; // the same model and viewport, under other nodes (a level of detail, a new path)
                    for (const auto& b : before)
                        if (b.address == model.address && b.viewport == model.viewport) {
                            moved = true;
                            break;
                        }
                    if (moved) ++m.match_unmatched_path;
                }
                else if (it->second == 1) ++m.match_one;
                else ++m.match_several;
            }
            m.match_models += now.size();
            if (!before.empty() && !now.empty()) ++m.match_frames;
            if (o.interp && !before.empty() && !now.empty()) {
                m3::InterpStats stats;
                const auto start = std::chrono::steady_clock::now();
                m3::interpolate(m.match_scene[0], m.match_scene[1], 0.5f, m.interp_scene, &stats);
                const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                m.interp_seconds += took;
                if (took > m.interp_worst) {
                    m.interp_worst = took;
                    m.interp_worst_frame = frame;
                    m.interp_worst_stats = stats;
                }
                ++m.interp_frames;
                if (stats.cut) ++m.interp_cuts;
                m.interp_models += stats.models;
                m.interp_blended += stats.blended;
                m.interp_moved += stats.moved;
                m.interp_followed += stats.followed;
                m.interp_nearest += stats.nearest;
                m.interp_ambiguous += stats.ambiguous;
                // The sky's move (layer 3 below the 3D, its register's scroll), as the window finds it.
                const uint32_t sky = m.bus.tile_reg(0x6C);
                int sky_x = 0, sky_y = 0;
                bool sky_moved = false;
                if ((sky & 0x80000000u) && !(sky & 0x8000) && !(m.bus.tile_reg(0x20) & 0x800) && !m.sky_before.empty()) {
                    sky_moved = m3::TilegenRenderer::find_move(m.sky_before, m.tilegen.bottom(), m.sky_scroll_before, sky,
                                                               sky_x, sky_y);
                    ++m.sky_frames;
                    if (sky_moved) ++m.sky_found;
                }
                m.sky_before.assign(m.tilegen.bottom().begin(), m.tilegen.bottom().end());
                m.sky_scroll_before = sky;
                if (o.interp_trace && frame >= o.interp_trace_from && frame < o.interp_trace_to)
                    std::printf("frame %llu: %zu models, %zu blended, %zu not found (%zu followed), %zu moved, %zu spun; %zu nearest (%zu "
                                "ambiguous, %zu compared)%s; %.3f ms; sky %s %d, %d\n",
                                static_cast<unsigned long long>(frame), stats.models, stats.blended, stats.unmatched, stats.followed,
                                stats.moved, stats.spun, stats.nearest, stats.ambiguous, stats.compared,
                                stats.cut ? ", a cut" : "", 1000.0 * took, sky_moved ? "moved" : "(none)", sky_x, sky_y);
            }
        }
        // The 3D is drawn only when it will be seen.
        using R = Options::Renderer;
        const bool drawn = !quiet && (o.screenshot || (o.probe_frame && frame == o.probe_frame));
        if (drawn && o.renderer != R::Gpu) { // in software
            const auto render_start = std::chrono::steady_clock::now();
            m.real3d.render(m.bus.real3d());
            const auto& s = m.real3d.stats();
            m.setup_ms += s.setup_ms;
            m.raster_ms += s.raster_ms;
            if (s.spotlights) ++m.frames_spotlit;
            if (s.block_culled) ++m.frames_block_culled;
            m.frames_scroll_fog += s.scroll_fog;
            m.frames_ambient_fog += s.ambient_fog;
            m.micro_tris += s.micro_polys;
            m.translator_tris += s.translator_polys;
            m.inverted_tris += s.inverted_polys;
            m.render_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - render_start).count();
            ++m.frames_rendered;
        }
#ifdef SPIKEOUT_GPU
        if (drawn && m.gpu) { // on the GPU, from the same scene
            const auto start = std::chrono::steady_clock::now();
            const m3::Real3d& mem = m.bus.real3d();
            if (o.renderer == R::Gpu) m.builder.build(mem, m.scene);
            if (o.gpu_time_scale) { // drawn and waited for, not read back; several times, to keep the GPU busy (its clock up)
                if (m.builder.extend() > 0) m.builder.build(mem, m.scene); // (--wide: its scene opened out)
                const m3::Real3dScene& timed = m.builder.extend() > 0 ? m.scene : m.real3d.scene();
                for (int i = 0; i < kGpuTimeRepeats; ++i) m.gpu->render(mem, timed);
                SDL_WaitForGPUIdle(m.gpu_device);
                // And once alone: its CPU parts, and the GPU's time after (from idle).
                const auto submitting = std::chrono::steady_clock::now();
                m.gpu->render(mem, timed);
                const auto submitted = std::chrono::steady_clock::now();
                SDL_WaitForGPUIdle(m.gpu_device);
                const double gpu_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitted).count();
                const auto& cost = m.gpu->cost();
                m.profile_pack += cost.pack_ms;
                m.profile_upload += cost.upload_ms;
                m.profile_record += cost.record_ms;
                m.profile_cpu += std::chrono::duration<double, std::milli>(submitted - submitting).count();
                m.profile_gpu += gpu_ms;
                m.profile_gpu_worst = std::max(m.profile_gpu_worst, gpu_ms);
                if (o.gpu_profile_over > 0 && gpu_ms > o.gpu_profile_over)
                    std::printf("frame %llu: the GPU %.2f ms; %zu polygons, %zu viewports, %d passes, %d draws\n",
                                static_cast<unsigned long long>(frame), gpu_ms, timed.polygons.size(), timed.viewports.size(),
                                cost.passes, cost.draws);
                m.profile_passes += cost.passes;
                m.profile_draws += cost.draws;
                ++m.profile_frames;
            } else {
                m.gpu->render(mem, o.renderer == R::Gpu ? m.scene : m.real3d.scene(), &m.gpu_image);
                for (uint32_t p : m.gpu_image) m.gpu_hash = (m.gpu_hash ^ p) * 0x100000001B3ull; // FNV-1a, a word at a time
                if (o.gpu_full_frame && frame == o.gpu_full_frame && o.screenshot) { // --gpu-full: the picture whole
                    const std::string path = o.screenshot;
                    const size_t dot = path.find_last_of('.');
                    std::vector<uint32_t> rgb(m.gpu_image.size());
                    for (std::size_t i = 0; i < rgb.size(); ++i) rgb[i] = 0xFF000000u | m.gpu_image[i];
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-full" + path.substr(dot), rgb,
                                     m.gpu->width(), m.gpu->height());
                }
                if (o.gpu_hash_frames) { // each picture's own hash, to find where two runs part
                    uint64_t h = 0xCBF29CE484222325ull;
                    for (uint32_t p : m.gpu_image) h = (h ^ p) * 0x100000001B3ull;
                    std::printf("%sframe %llu: GPU picture %016llx\n", m.index ? ("board " + std::to_string(m.index) + " ").c_str() : "",
                                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(h));
                }
                if (const int e = m.builder.extend(); e > 0) { // widescreen: kept whole, its middle for the rest
                    const int w = m3::TilegenRenderer::kWidth, h = m3::TilegenRenderer::kHeight;
                    m.gpu_wide = m.gpu_image;
                    m.gpu_image.resize(static_cast<size_t>(w) * h);
                    for (int y = 0; y < h; ++y)
                        std::copy_n(m.gpu_wide.begin() + static_cast<std::ptrdiff_t>(y * (w + 2 * e) + e), w,
                                    m.gpu_image.begin() + static_cast<std::ptrdiff_t>(y * w));
                }
                if (o.gpu_scale > 1) { // every scale-th pixel, for the screenshots
                    const int s = o.gpu_scale, w = m3::TilegenRenderer::kWidth, h = m3::TilegenRenderer::kHeight;
                    std::vector<uint32_t> small(static_cast<size_t>(w) * h);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            small[static_cast<size_t>(y) * w + x] = m.gpu_image[static_cast<size_t>(y * s) * (w * s) + x * s];
                    m.gpu_image.swap(small);
                }
            }
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() /
                                (o.gpu_time_scale ? kGpuTimeRepeats : 1);
            m.gpu_seconds += took;
            if (took > m.gpu_worst) {
                m.gpu_worst = took;
                m.gpu_worst_frame = frame;
            }
            if (took > 1.0 / recomp::FrameTiming::kRefreshHz) ++m.gpu_slow;
            ++m.gpu_frames;
            if (o.renderer == R::Compare) {
                const auto [pixels, largest] = difference(m.real3d.image(), m.gpu_image);
                ++m.frames_compared;
                if (pixels) ++m.frames_differing;
                m.pixels_differing += pixels;
                m.max_difference = std::max(m.max_difference, largest);
                if (pixels > m.worst_pixels) {
                    m.worst_pixels = pixels;
                    m.worst_frame = frame;
                }
            }
        }
#endif
        if (o.probe_frame && frame == o.probe_frame && m.index == 0) print_probe(m.real3d, o.probe_x, o.probe_y, frame);
        if (!o.no_2d && !quiet) m.tilegen.composite(m.screen, o.renderer == R::Gpu ? std::span<const uint32_t>(m.gpu_image) : m.real3d.image());
        // (tests: GAMERUN_2D_HASH=1, each frame's 2D layers hashed, their sides and marks too: the tile generator's work)
        if (static const bool hash_2d = std::getenv("GAMERUN_2D_HASH") != nullptr; hash_2d && !o.no_2d && !quiet) {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const auto layer : {m.tilegen.bottom(), m.tilegen.top(), m.tilegen.bottom_side(), m.tilegen.top_side(),
                                     m.tilegen.bottom_own(), m.tilegen.top_own()})
                for (uint32_t p : layer) h = (h ^ p) * 0x100000001B3ull;
            for (const auto marks : {m.tilegen.bottom_real(), m.tilegen.top_real()})
                for (uint8_t b : marks) h = (h ^ b) * 0x100000001B3ull;
            std::printf("board %zu frame %llu: 2D %016llx\n", m.index, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(h));
        }
        if (const int e = m.tilegen.extend(); e > 0 && !o.no_2d && !quiet) { // --wide: the sides made every frame, as the window does
            const auto start = std::chrono::steady_clock::now();
            m.wide_bottom.build(m.tilegen.bottom(), m.tilegen.bottom_side(), e, m.bottom_wide, false, m.tilegen.bottom_own(),
                                m.tilegen.bottom_real());
            m.wide_top.build(m.tilegen.top(), m.tilegen.top_side(), e, m.top_wide, true, m.tilegen.top_own(),
                             m.tilegen.top_real());
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            m.wide_seconds += took;
            if (took > m.wide_worst) {
                m.wide_worst = took;
                m.wide_worst_frame = frame;
            }
            ++m.wide_frames;
        }
        const auto sound_start = std::chrono::steady_clock::now();
        if (o.no_sound || quiet) m.bus.midi_out().clear(); // (:quiet: heard the first time)
        const std::span<const int16_t> pcm = o.no_sound || quiet ? std::span<const int16_t>() :
            m.sound.frame(m.bus.midi_out(), m.timing.frame_start(frame - 1), m.timing.frame_cycles());
        m.sound_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - sound_start).count();
        if (o.wav) m.audio.insert(m.audio.end(), pcm.begin(), pcm.end());
        for (int16_t s : pcm) {
            m.peak = std::max(m.peak, std::abs(static_cast<int>(s)));
            m.energy += static_cast<double>(s) * s;
        }
        // (network play: each cabinet's link role written over its first frames as the window writes it (board 0 the
        // master, the others slaves), so gamerun and the window agree without --poke: a test's own pokes with other
        // values made a window joining before frame 3000 go out of step)
        if (o.netplay && frame < tools::kLinkSettingFrames)
            m.bus.write8(tools::kLinkSettingAddr, m.index == 0 ? 1 : 2);
        for (const Poke& k : o.pokes)
            if (for_board(k.board, m.index) && frame >= k.frame && frame < k.frame + k.frames)
                m.bus.write8(k.addr, k.value);
        if (o.ram_dump && m.index == static_cast<std::size_t>(o.ram_dump_board) && frame >= o.ram_dump_from &&
            frame < o.ram_dump_from + o.ram_dump_frames) { // --ram-dump: the frame, the logic counter (0x1000A8), the range
            static std::FILE* file = std::fopen(o.ram_dump, "wb");
            const uint32_t head[2] = {static_cast<uint32_t>(frame), m.bus.read32(0x1000A8)};
            std::fwrite(head, sizeof head, 1, file);
            std::fwrite(m.bus.ram().data() + o.ram_dump_lo, 1, o.ram_dump_hi - o.ram_dump_lo, file);
            if (frame + 1 == o.ram_dump_from + o.ram_dump_frames) std::fflush(file);
        }
        if (o.ram_hashes && m.index == static_cast<std::size_t>(o.ram_hashes_board) && frame >= o.ram_hashes_from) {
            // --ram-hashes: the frame, the logic counter, then a hash of each 4 KB page of main RAM (dumped at the
            // same point as --ram-dump, so two runs compare page for page: tools/onepc-diff.ps1)
            static std::FILE* file = std::fopen(o.ram_hashes, "wb");
            // (RAM_HASH_IGNORE=LO-HI,...: hex ranges left out, as zeros (bytes known to differ, to look past them))
            static const std::vector<std::pair<uint32_t, uint32_t>> ignore = [] {
                std::vector<std::pair<uint32_t, uint32_t>> v;
                if (const char* s = std::getenv("RAM_HASH_IGNORE"))
                    for (const char* p = s; *p;) {
                        unsigned lo = 0, hi = 0;
                        int used = 0;
                        if (std::sscanf(p, "%x-%x%n", &lo, &hi, &used) != 2) break;
                        v.emplace_back(lo, hi);
                        p += used;
                        if (*p == ',') ++p;
                    }
                return v;
            }();
            static std::vector<uint8_t> copy;
            const uint8_t* ram = m.bus.ram().data();
            if (!ignore.empty()) {
                copy.assign(m.bus.ram().begin(), m.bus.ram().end());
                for (const auto& [lo, hi] : ignore)
                    if (hi <= copy.size() && lo < hi) std::fill(copy.begin() + lo, copy.begin() + hi, uint8_t{0});
                ram = copy.data();
            }
            const std::size_t pages = m.bus.ram().size() / 4096;
            std::vector<uint32_t> row(2 + pages);
            row[0] = static_cast<uint32_t>(frame), row[1] = m.bus.read32(0x1000A8);
            for (std::size_t p = 0; p < pages; ++p) {
                uint64_t h = 0xcbf29ce484222325ull;
                const uint64_t* w = reinterpret_cast<const uint64_t*>(ram + p * 4096);
                for (std::size_t i = 0; i < 512; ++i) h = (h ^ w[i]) * 0x100000001b3ull, h ^= h >> 29;
                row[2 + p] = static_cast<uint32_t>(h ^ (h >> 32));
            }
            std::fwrite(row.data(), sizeof(uint32_t), row.size(), file);
            if (frame % 64 == 0) std::fflush(file);
        }
        for (const Peek& k : o.peeks)
            if (for_board(k.board, m.index) && frame >= k.frame && frame < k.frame + k.frames) {
                std::printf("%sframe %llu: %06x:", m.index ? ("board " + std::to_string(m.index) + " ").c_str() : "",
                            static_cast<unsigned long long>(frame), k.addr);
                for (uint32_t i = 0; i < k.length && k.addr + i < m.bus.ram().size(); ++i)
                    std::printf("%s%02x", i % 4 ? "" : " ", m.bus.ram()[k.addr + i]);
                std::printf("\n");
            }
        // --press: inputs held for the coming frame (active low).
        uint8_t in[2] = {0xFF, 0xFF};
        for (const Press& p : o.presses)
            if (for_board(p.board, m.index) && frame >= p.frame && frame < p.frame + p.frames)
                in[p.bank] = static_cast<uint8_t>(in[p.bank] & ~(1u << p.bit));
        m.bus.inputs.bank0 = in[0];
        m.bus.inputs.controls = in[1];
        if (o.replay && m.index == 0) replay_frame(m, *o.replay, frame);
        if (o.input_log && m.index == 0) { // --inputs: main RAM checked, the run's writes made, the next frame's inputs set
            const auto& frames = o.input_log->frames;
            if (auto it = frames.find(frame); it != frames.end()) {
                if (it->second.hash) {
                    if (tools::Recording::ram_hash(m.bus.ram()) == it->second.hash) {
                        ++m.replay_matched;
                    } else if (!m.replay_parted) {
                        m.replay_parted = frame;
                        std::printf("inputs: main RAM differs from the recorded run's at frame %llu\n", static_cast<unsigned long long>(frame));
                    }
                }
                for (const auto& [addr, value] : it->second.writes) {
                    m.bus.ram()[addr] = static_cast<uint8_t>(value >> 8);
                    m.bus.ram()[addr + 1] = static_cast<uint8_t>(value);
                }
            }
            if (auto next = frames.find(frame + 1); next != frames.end()) {
                m.bus.inputs.bank0 = next->second.bank0;
                m.bus.inputs.controls = next->second.controls;
            } else if (!frames.empty() && frame >= frames.rbegin()->first) {
                m.c.limit_blocks(0); // (the recorded run's end)
            }
        }
        if (o.linked_inputs) { // --inputs, linked boards: main RAM checked, the inputs as the bot left them, its changes
            m.bus.inputs.bank0 = m.linked_bank0;
            m.bus.inputs.controls = m.linked_controls;
            if (const auto* c = linked_change(m, o, tools::LinkedInputLog::At::FrameEnd, frame); c && c->hash) {
                if (tools::Recording::ram_hash(m.bus.ram()) == c->hash) {
                    ++m.replay_matched;
                } else if (!m.replay_parted) {
                    m.replay_parted = frame;
                    std::printf("inputs: board %zu's main RAM differs from the recorded run's at frame %llu\n", m.index,
                                static_cast<unsigned long long>(frame));
                }
            }
            if (o.linked_inputs->count({tools::LinkedInputLog::At::Resumed, static_cast<uint8_t>(m.index), frame, 0}))
                m.c.request_snapshot([&m, &o, frame](uint32_t) { linked_change(m, o, tools::LinkedInputLog::At::Resumed, frame); });
            if (frame >= o.linked_inputs_end) m.c.limit_blocks(0); // (the recorded run's end)
        }
        // Into the board's input table (--input-delay frames on), and the
        // next frame's out of it.
        // Network play: only this machine's player's board has a source
        // here (into the session too); the others' inputs come from theirs.
        if (!o.netplay || m.index == o.local_seat) {
            const tools::InputTable::Inputs source{m.bus.inputs.bank0, m.bus.inputs.bank1, m.bus.inputs.controls,
                                                   m.bus.inputs.controls2};
            auto put = [&](uint64_t at, const tools::InputTable::Inputs& value) {
                if (o.netplay) o.netplay->local(m.index, at, value);
                else m.inputs->set(m.index, at, value);
            };
            if (g_input_delay && !m.inputs_started) put(frame, {}); // (until the first comes: none held)
            m.inputs_started = true;
            put(frame + g_input_delay, source);
        }
        // (--net-one-board: from the hand-over, only this machine's own board: a shadow of another is exact in what it
        // sends, not in all of its RAM)
        // (--net-rollback: once every board's inputs up to that frame are in and no guess up to it was wrong: a frame
        // that won't be run again)
        if (o.netplay && frame % 120 == 0 && !(one_board_goes(o, m.index) && frame > o.one_board_frame)) {
            if (g_net_rb) m.hash_pending[frame] = tools::Recording::ram_hash(m.bus.ram());
            else o.netplay->hash(frame, m.index, tools::Recording::ram_hash(m.bus.ram()));
            tools::ram_dump(m.index, frame, m.bus.ram().data(), m.bus.ram().size());
        }
        while (g_net_rb && !m.hash_pending.empty() && m.inputs->all_set(m.hash_pending.begin()->first, o.boards) &&
               m.inputs->wrong_from() > m.hash_pending.begin()->first) {
            o.netplay->hash(m.hash_pending.begin()->first, m.index, m.hash_pending.begin()->second);
            m.hash_pending.erase(m.hash_pending.begin());
        }
        if (o.music_hash && frame % o.music_hash == 0 && m.sound.board.music()) { // --music-hash: the music board's state, and what it plays
            const m3::Dsb2::NowPlaying p = m.sound.board.music()->now_playing();
            std::printf("board %zu frame %llu: music %016llx, %s %06x-%06x at %06x, volume %02x %02x\n", m.index,
                        static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(state_hash([&m](m3::State& s) { m.sound.board.music()->state(s); })),
                        p.playing ? "playing" : "stopped", p.start, p.end, p.pos, p.volume_left, p.volume_right);
        }
        if ((o.link_record || o.link_standin || o.camera_test || o.drop_frame || o.clone_frame || o.one_board >= 0 || o.frame_hashes) &&
            !o.no_frame_hashes) { // (each frame's main RAM, and every 100th the whole board's, to compare the two runs)
            // (tests: GAMERUN_PARTS_FRAMES=FROM:TO, every frame there: its state's parts hashed, the CPU, the frame timing,
            // the network board, the bus with its devices, RAM)
            static unsigned long long parts_from = ~0ull, parts_to = 0;
            if (static const char* parts = std::getenv("GAMERUN_PARTS_FRAMES"); parts && parts_from == ~0ull)
                std::sscanf(parts, "%llu:%llu", &parts_from, &parts_to);
            if (frame >= parts_from && frame <= parts_to)
                m.c.request_snapshot([&m, frame](uint32_t at) {
                    const auto h = [](const std::function<void(m3::State&)>& walk) { return static_cast<unsigned long long>(state_hash(walk)); };
                    std::printf("board %zu frame %llu parts (pc %08x): cpu %016llx timing %016llx netboard %016llx bus %016llx ram %016llx\n",
                                m.index, static_cast<unsigned long long>(frame), at, h([&](m3::State& s) { m.c.state(s); }),
                                h([&](m3::State& s) { m.timing.state(s); }), h([&](m3::State& s) { m.bus.netboard().state(s); }),
                                h([&](m3::State& s) { m.bus.state(s); }),
                                static_cast<unsigned long long>(tools::Recording::ram_hash(m.bus.ram())));
                    if (g_ring) g_ring->abandon_cut(m.index); // (the network board's part saved alone: no cut under way)
                });
            std::printf("board %zu frame %llu: RAM %016llx\n", m.index, static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(tools::Recording::ram_hash(m.bus.ram())));
            if (frame % 100 == 0)
                m.c.request_snapshot([&m, &o, frame](uint32_t) {
                    std::printf("board %zu frame %llu: whole state %016llx\n", m.index, static_cast<unsigned long long>(frame),
                                static_cast<unsigned long long>(state_hash(
                                    [&](m3::State& s) { tools::board_state(s, m.c, m.timing, sound_state(m, o)); })));
                    if (std::getenv("GAMERUN_STATE_PARTS")) // (which part differs: the CPU, the frame timing, the network board)
                        std::printf("board %zu frame %llu: parts cpu %016llx timing %016llx netboard %016llx\n", m.index,
                                    static_cast<unsigned long long>(frame),
                                    static_cast<unsigned long long>(state_hash([&](m3::State& s) { m.c.state(s); })),
                                    static_cast<unsigned long long>(state_hash([&](m3::State& s) { m.timing.state(s); })),
                                    static_cast<unsigned long long>(state_hash([&](m3::State& s) { m.bus.netboard().state(s); })));
                    // (tests: GAMERUN_NETBOARD_DUMP=DIR, the network board's state at each of these frames written to
                    // DIR/netboard-<process>-<board>-<frame>.bin, to see which of its bytes two runs differ in)
                    if (const char* dir = std::getenv("GAMERUN_NETBOARD_DUMP")) {
                        static const unsigned long run_id = static_cast<unsigned long>( // (this process's, its start's clock)
                            std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007);
                        m3::State s;
                        m.bus.netboard().state(s);
                        char path[600];
                        std::snprintf(path, sizeof path, "%s/netboard-%lu-%zu-%llu.bin", dir, run_id,
                                      m.index, static_cast<unsigned long long>(frame));
                        tools::write_file(path, s.data());
                    }
                });
        }
        {
            bool guessed = false; // (--net-rollback: a guess, a few frames ahead at most)
            const auto waited = std::chrono::steady_clock::now();
            // (--net-rollback, time sync: more than a few frames ahead of another machine (its inputs show it), waited
            // here until it is close (gamerun runs as fast as it can: the window slows a little instead))
            static const bool no_sync = std::getenv("ROLLBACK_NO_SYNC") != nullptr; // (tests: rollback far back)
            // (not while catching up (--net-catch-up), as the window: this seat's inputs, the host's until the cabinet is
            // taken, would make it look ahead of a far player while it is well behind them all)
            // (--pace: the window's way instead, a little slower or sooner each frame (below), as players have it)
            if (g_net_rb && m.index == o.local_seat && !no_sync && !g_catching_up && !o.pace)
                for (int k = 0; k < 3000 && o.netplay->lead(m.index).ahead > 4; ++k) {
                    if (k == 0) ++m.sync_waits;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            const auto got = g_net_rb && g_one_board.may_guess(frame) ? m.inputs->guess(m.index, frame, o.net_rollback, guessed, tools::rollback::quiet_ahead(o.net_rollback_depth))
                                                                      : m.inputs->wait(m.index, frame);
            if (o.netplay) { // (network play: the longest a frame waited for inputs, and how often over a frame, at the end)
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waited).count();
                m.input_wait_max = std::max(m.input_wait_max, ms);
                if (ms > 1000.0 / recomp::FrameTiming::kRefreshHz) ++m.input_waits_long;
                if (ms > 500) // (a long one: when, and whether guesses were allowed then (a hand-back's switch stops them))
                    std::printf("net: board %zu frame %llu waited %.0f ms for its inputs (%s)\n", m.index,
                                static_cast<unsigned long long>(frame), ms,
                                !g_net_rb ? "no rollback" : g_one_board.may_guess(frame) ? "guesses allowed" : "no guesses: a switch");
            }
            if (!got) throw recomp::OutOfBudget(m.c.cpu.pc); // (the session ended)
            const tools::InputTable::Inputs in_table = *got;
            // (--rollback-test, --net-rollback: the frames it may run again keep theirs)
            // (one board here: a cabinet handed over to the module, which reads its frames around this one's)
            const uint64_t keep = o.rb_frames ? o.rb_depth + 1 : g_net_rb ? std::max(o.net_rollback, o.net_rollback_depth) + 2 : o.one_board >= 0 ? 64 : 0;
            if (frame > keep) m.inputs->forget_before(m.index, frame - keep);
            m.bus.inputs.bank0 = in_table.bank0;
            m.bus.inputs.bank1 = in_table.bank1;
            m.bus.inputs.controls = in_table.controls;
            m.bus.inputs.controls2 = in_table.controls2;
        }
        // Network play, the host: every board's state asked for (by a player
        // joining the game in progress, or a machine out of step), at the
        // end of a frame every board is yet to finish.
        // (--net-one-board: once every other board runs whole here (each a shadow from its machine's real board, asked of
        // it for this: Game::want_whole), saved as usual; a board whose player went without a parting can't be)
        const bool one_board_here = o.netplay && m.index == o.local_seat && m.ob.cabinets;
        // (every board's state: with rollback at a frame whose inputs are all in already, the game a little slower
        // until then (--pace), as the window: no wait for a far player's at the frame; after 2 s at the next frame)
        auto capture_at = [&](uint64_t at) {
            if (!g_net_rb) {
                m.linked->capture_frame = at;
            } else if (!g_capture_armed) {
                g_capture_armed = true;
                g_capture_armed_at = std::chrono::steady_clock::now();
            }
        };
        if (o.netplay && m.linked && m.index == o.local_seat && g_capture_armed && m.linked->capture_frame == 0 &&
            (tools::players_inputs_past(o.netplay->session(), m.index, frame + 2) ||
             std::chrono::steady_clock::now() - g_capture_armed_at > std::chrono::seconds(2))) {
            m.linked->capture_frame = frame + 2;
            g_capture_armed = false;
        }
        if (o.netplay && m.linked && m.index == o.local_seat && m.linked->capture_frame == 0 && !g_capture_armed &&
            o.netplay->take_state_request()) {
            if (!one_board_here || g_one_board.all_whole()) {
                capture_at(frame + 2);
            } else if (g_one_board.can_be_whole()) {
                std::printf("net: every board's state asked for at frame %llu: every board whole here first\n", (unsigned long long)frame);
                g_one_board.want_whole(true);
                m.state_pending = true;
            } else {
                std::printf("net: every board's state asked for at frame %llu: a board here whose player went is had nowhere whole, "
                            "none sent\n",
                            static_cast<unsigned long long>(frame));
            }
        }
        if (one_board_here && m.state_pending && m.linked->capture_frame == 0 && g_one_board.all_whole()) {
            std::printf("net: every board whole here at frame %llu: their state saved\n", (unsigned long long)frame);
            capture_at(frame + 2);
            m.state_pending = false, m.whole_until = frame + 10;
        }
        if (one_board_here && m.whole_until && frame >= m.whole_until) g_one_board.want_whole(false), m.whole_until = 0;
        // (--net-one-board: players leaving after a frame, their boards' states to come; players gone)
        if (one_board_here) {
            // (tests: NET_PARTINGS_LATE=1, each taken only once its frame has passed: as one held up on the way)
            static const bool late = std::getenv("NET_PARTINGS_LATE") != nullptr;
            static std::vector<std::pair<uint8_t, uint64_t>> held;
            for (const auto& p : o.netplay->take_partings()) held.push_back(p);
            for (auto it = held.begin(); it != held.end();)
                if (!late || frame > it->second) {
                    g_one_board.parting(it->first, it->second);
                    it = held.erase(it);
                } else {
                    ++it;
                }
            for (const auto& [seat, from] : o.netplay->take_gone()) g_one_board.seat_gone(seat, from);
            for (const uint8_t seat : o.netplay->take_taken()) g_one_board.seat_taken(seat);
        }
        // --net-leave-at FRAME: this player leaving after a frame a little ahead, the others told; its board's state then
        // sent to all of them; the goodbye once they have it
        if (o.netplay && m.index == o.local_seat && o.net_leave_at && frame == o.net_leave_at) {
            // (tests: NET_PARTING_AHEAD=N, told only N frames ahead: too late for the others, as a slow network might)
            static const uint64_t ahead = std::getenv("NET_PARTING_AHEAD") ? std::strtoull(std::getenv("NET_PARTING_AHEAD"), nullptr, 0)
                                                                           : tools::one_board::Game::kAskAhead;
            m.parting_frame = frame + ahead;
            o.netplay->session().announce_parting(m.parting_frame);
            std::printf("net: leaving after frame %llu\n", (unsigned long long)m.parting_frame);
        }
        if (o.netplay && m.index == o.local_seat && o.net_vanish_at && frame == o.net_vanish_at) { // --net-vanish-at FRAME
            std::printf("net: gone at frame %llu, as a crash\n", (unsigned long long)frame);
            std::fflush(stdout);
            o.netplay->session().vanish();
            std::_Exit(0);
        }
        if (m.leave_next) { // (its state sent: the goodbye once every other machine has it)
            g_parted_after = m.parting_frame; // (every board here stops there: one board or every board)
            for (int k = 0; k < 3000 && o.netplay->session().sending(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::printf("net: left after frame %llu\n", (unsigned long long)m.parting_frame);
            throw recomp::OutOfBudget(m.c.cpu.pc);
        }
        // (network play: this board's backup on every other machine, now and then, for one board a PC's (any of them may
        // be running this board on side runs): the pages changed since the last, or whole to a machine that has none of
        // it; packed and sent off this thread)
        // (at the snapshot point; a shadow's request too: the backups first)
        const auto backup_now = [&m, &o](uint64_t at) {
            if (m.backups.frame() == at) return; // (taken already: the backups' turn this frame)
            for (const uint8_t s : o.netplay->take_whole_wanted()) m.backup_bases.erase(s);
            {
                const uint64_t taken = g_ring ? g_ring->taken(m.index) : 0, sent = g_ring ? g_ring->sent(m.index) : 0;
                m.backups.take(at, [&m](m3::State& s) { tools::board_state(s, m.c, m.timing, {}); }, taken, sent);
                if (g_ring) g_ring->abandon_cut(m.index); // (one board's state alone: no cut under way)
                std::vector<uint8_t> to_changes, to_whole;
                std::vector<uint8_t> to = o.netplay->session().backup_watchers(); // (spectators on one board too)
                for (uint8_t s = 0; s < o.boards; ++s) // (a player still joining: none yet, Session::joining)
                    if (s == m.index) continue;
                    else if (o.netplay->session().joining(s)) m.backup_bases.erase(s); // (whole once seated)
                    else to.push_back(s);
                for (const uint8_t s : to) {
                    (m.backups.changes_possible() && m.backup_bases.count(s) ? to_changes : to_whole).push_back(s);
                    m.backup_bases.insert(s);
                }
                std::size_t changes = 0, whole = 0;
                if (!to_changes.empty()) {
                    std::vector<uint8_t> raw = m.backups.changes();
                    changes = raw.size();
                    o.netplay->send_backup(at, std::move(raw), std::move(to_changes));
                }
                if (!to_whole.empty()) {
                    std::vector<uint8_t> raw = m.backups.whole();
                    whole = raw.size();
                    o.netplay->send_backup(at, std::move(raw), std::move(to_whole));
                }
                static const bool say = std::getenv("ONEBOARD_BACKUPS") != nullptr; // (tests: their sizes, before packing)
                if (say) std::printf("board %zu frame %llu: backup, %zu KB of changes, %zu KB whole (unpacked)\n", m.index,
                                     (unsigned long long)at, changes / 1024, whole / 1024);
            }
        };
        if (o.netplay && m.index == o.local_seat && !o.one_board_local_shadows && frame >= m.backups.frame() + tools::backup::kEvery &&
            o.netplay->session().seated() && // (a machine still joining sends none: its cabinet asked for first)
            (!g_net_rb || (m.inputs->all_set(frame, o.boards) && m.inputs->wrong_from() > frame))) // (--net-rollback: a settled frame's)
            m.c.request_snapshot([backup_now, frame](uint32_t) { backup_now(frame); });
        // One board a PC: another machine's shadow of this board wants its state at the end of this frame (taken where
        // a save state is, without the sound board; sent compressed); or every other machine, this player leaving.
        if (o.netplay && m.index == o.local_seat) {
            std::vector<uint8_t> seats = o.netplay->take_board_requests(frame);
            // (--net-rollback: only a frame that won't be run again: the inputs it ran on waited for (those up to the
            // frame before: a frame's are set at its end, for the next; the machine that asked sets its own of this
            // frame only once its shadow is loaded); one guessed wrong, the frame is run again (from the next meeting)
            // and the state taken then)
            // (and none while a shadow here waits for a state of a frame before it, come late: every board goes back
            // there, this frame run again with that cabinet whole)
            if (g_net_rb && !seats.empty() && frame &&
                (!m.inputs->wait_all(frame - 1, o.boards) || m.inputs->wrong_from() < frame || g_one_board.late_before(frame))) {
                for (const uint8_t seat : seats) o.netplay->board_request_again(seat, frame);
                std::printf("board %zu: frame %llu guessed wrong or to be run again: its state taken when it is\n", m.index,
                            static_cast<unsigned long long>(frame));
                seats.clear();
            }
            if (m.parting_frame && frame == m.parting_frame) {
                for (uint8_t s = 0; s < o.boards; ++s)
                    if (s != m.index && std::find(seats.begin(), seats.end(), s) == seats.end()) seats.push_back(s);
                m.leave_next = true;
            }
            // (as its backup of this frame, the pages written since the last one, sent to every machine off this thread,
            // then "my backup" to those who asked; leaving, the whole state, sent at once. Tests and the old way:
            // ONEBOARD_WHOLE_STATES=1)
            static const bool whole_states = std::getenv("ONEBOARD_WHOLE_STATES") != nullptr;
            const bool by_backup = !seats.empty() && !m.leave_next && !o.one_board_local_shadows && !whole_states;
            if (by_backup)
                m.c.request_snapshot([&m, &o, backup_now, seats, frame](uint32_t) {
                    backup_now(frame);
                    std::printf("net: this board's state at frame %llu to seat(s) %u..: its backup\n", (unsigned long long)frame,
                                seats.front() + 1u);
                    o.netplay->send_board_after_backups(frame, tools::one_board::pack_remote(0, 0, {}), seats);
                });
            else if (!seats.empty()) // (one state for all who asked)
                m.c.request_snapshot([&m, &o, seats, frame](uint32_t) {
                    m3::State s;
                    tools::board_state(s, m.c, m.timing, {});
                    const uint64_t taken = g_ring ? g_ring->taken(m.index) : 0, sent = g_ring ? g_ring->sent(m.index) : 0;
                    if (g_ring) g_ring->abandon_cut(m.index); // (one board's state alone: no cut under way)
                    const std::vector<uint8_t> packed = tools::one_board::pack_remote(taken, sent, net::compress(s.data()));
                    for (const uint8_t seat : seats) {
                        std::printf("net: this board's state at frame %llu to seat %u (%zu KB packed)\n", (unsigned long long)frame,
                                    seat + 1u, packed.size() / 1024);
                        o.netplay->session().send_board(seat, frame, packed);
                    }
                });
        }
        // A player who joined in progress: its cabinet, at once (gamerun runs
        // as fast as it can, as the host does: it wouldn't catch up first,
        // as the window does; the host then waits for its inputs). With
        // --net-catch-up as the window: unpaced until within 4 frames of the
        // furthest of the others' inputs (or not gaining after 5 s), then the
        // cabinet taken (the host's game not held up by the join).
        if (o.netplay && m.index == o.local_seat && !m.took_seat) {
            bool take = true;
            if (g_catching_up) {
                uint64_t front = 0;
                for (uint8_t s = 0; s < o.boards; ++s)
                    if (s != o.local_seat) front = std::max(front, o.netplay->session().inputs_known(s));
                static const auto began = std::chrono::steady_clock::now();
                static const uint64_t behind_first = front > frame ? front - frame : 0;
                static const uint64_t frame_first = frame;
                const double so_far = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                const bool given_up = so_far > 5 && front > frame + 4 && front - frame + 30 >= behind_first;
                take = frame + 4 >= front || given_up;
                const uint64_t behind = front > frame ? front - frame : 0; // (how far caught up, for the status: by fives)
                g_catch_percent = behind_first && behind <= behind_first
                                      ? static_cast<int>(100 * (behind_first - behind) / behind_first) / 5 * 5
                                      : 0;
                if (take) {
                    g_catching_up = false;
                    std::printf("net: caught up at frame %llu: %llu frames in %.1f s%s\n", static_cast<unsigned long long>(frame),
                                static_cast<unsigned long long>(frame - frame_first), so_far,
                                given_up ? " (not gaining: taken anyway)" : "");
                }
            }
            if (take) {
                m.took_seat = true;
                o.netplay->session().take_seat();
            }
        }
        // (how this machine plays, for the others' seat lists, as the window says it: sent as it changes, repeated)
        if (o.netplay && m.index == o.local_seat && !o.net_spectate) {
            using Status = net::Session::Status;
            Status me;
#if defined(_WIN32)
            me.platform = Status::Platform::windows;
#elif defined(__APPLE__)
            me.platform = Status::Platform::macos;
#elif defined(__linux__)
            me.platform = Status::Platform::linux_os;
#endif
            me.boards = o.net_one_board && frame > o.net_one_board ? Status::Boards::one : Status::Boards::every;
            me.net = g_net_rb ? Status::Net::rollback : Status::Net::delay;
            if (g_input_delay_auto) { // (--input-delay auto: once a second, the window's rule: tools::DelayTuner, a frame at a time
                                      // once it has asked for the same for 3 seconds)
                static tools::DelayTuner tuner;
                static auto checked = std::chrono::steady_clock::now();
                static int asked = 0;
                if (const auto now = std::chrono::steady_clock::now(); now - checked >= std::chrono::seconds(1) && !g_catching_up) {
                    checked = now;
                    int one_way = -1;
                    const auto trips = o.netplay->session().round_trips();
                    for (std::size_t s = 0; s < o.boards && s < trips.size(); ++s)
                        if (s != o.local_seat && trips[s] >= 0) one_way = std::max(one_way, trips[s] / 2);
                    const auto [catches, late] = g_net_rb ? g_net_rb->take_catches() : std::pair<uint64_t, uint64_t>{0, 0};
                    const uint64_t want = tuner.tick(one_way, g_net_rb != nullptr, catches, late), have = g_input_delay;
                    asked = want == have ? 0 : asked + 1;
                    if (asked >= 3 && one_way >= 0) {
                        asked = 0;
                        g_input_delay = want > have ? have + 1 : have - 1;
                        std::printf("net: input delay %llu at frame %llu (auto; the furthest player %d ms away one way, %llu of %llu "
                                    "catch-ups late)\n",
                                    (unsigned long long)g_input_delay.load(), (unsigned long long)frame, one_way,
                                    (unsigned long long)late, (unsigned long long)catches);
                    }
                }
            }
            me.delay = static_cast<int16_t>(g_input_delay.load());
            me.auto_delay = g_input_delay_auto;
            if (g_catching_up) me.catch_up = static_cast<int8_t>(g_catch_percent.load());
            o.netplay->session().set_status(me);
        }
        if (o.max_frames && frame >= o.max_frames) throw recomp::OutOfBudget(m.c.cpu.pc); // --frames
        // --pace: at the arcade's speed (this process's own board, or board 0, held to the refresh: the others meet it),
        // as the window: network tests whose timing matters (a spectator on one board, behind players that run in time)
        if (o.pace && m.index == (o.netplay ? o.local_seat : 0)) {
            using clock = std::chrono::steady_clock;
            static auto started = clock::now(), last = started;
            static uint64_t first = frame;
            if (g_catching_up) { // (--net-catch-up: flat out until caught up, paced from there)
                started = last = clock::now(), first = frame;
            } else {
                // (rollback's time sync as the window has it (tools/spikeout.cpp, pace): ahead of another player by
                // over 4 frames, this frame a little later (a twentieth of a frame a frame, up to half); behind the one
                // furthest on by over 8, a little sooner (up to a tenth); over 60 behind, unpaced)
                const auto frame_time = [](double share) {
                    return std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(share / recomp::FrameTiming::kRefreshHz));
                };
                if (g_net_rb && o.netplay) {
                    if (g_capture_armed) started += frame_time(0.3); // (the far players' inputs coming up, as the window)
                    static tools::TimeSync sync; // (the window's rule: tools/netplay.hpp)
                    const auto lead = o.netplay->lead(m.index);
                    const auto step = sync.step(lead.ahead, lead.behind);
                    if (step.free) started = clock::now(), first = frame;
                    else {
                        if (step.later > 0) started += frame_time(step.later);
                        else if (step.later < 0) started -= frame_time(-step.later);
                        if (step.slowed) ++m.sync_waits;
                    }
                }
                auto due = started + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(
                                         static_cast<double>(frame - first) / recomp::FrameTiming::kRefreshHz));
                if (clock::now() - due > std::chrono::milliseconds(250)) // (too far behind: not caught up, as the window)
                    started = clock::now(), first = frame, due = started;
                // (waited out busy, as the window: Windows' sleeps end up to 15 ms late; elsewhere asleep to 2 ms before)
#ifndef _WIN32
                std::this_thread::sleep_until(due - std::chrono::milliseconds(2));
#endif
                while (clock::now() < due) std::this_thread::yield();
                // (how smoothly: the time from the last frame, a stutter when over 1.5 frames (as a player would see
                // it); a stall puts one frame late, the frames after it run again at the arcade's speed)
                const auto now = clock::now();
                const double gap = std::chrono::duration<double, std::milli>(now - last).count();
                last = now;
                static const auto paced_at = now; // (the totals: from the first paced frame, through stalls)
                static const uint64_t paced_from = frame;
                g_pace.frames = frame - paced_from;
                g_pace.seconds = std::chrono::duration<double>(now - paced_at).count();
                if (gap > 1.5 * 1000.0 / recomp::FrameTiming::kRefreshHz) ++g_pace.late;
                g_pace.worst = std::max(g_pace.worst, gap);
                // (SPIKEOUT_NET_DEBUG: each 300 frames' share, to see when: a join, a stall)
                static const bool debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
                static uint64_t late_before = 0;
                static double worst_here = 0;
                worst_here = std::max(worst_here, gap);
                if (debug && frame % 300 == 0) {
                    std::printf("pace: frames to %llu: %llu stutters, the longest gap %.0f ms\n",
                                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g_pace.late - late_before),
                                worst_here);
                    std::fflush(stdout);
                    late_before = g_pace.late, worst_here = 0;
                }
            }
        }
        // (--net-leave-at, every board here: the others stop with this player's, at its parting frame; else they'd wait
        // for its inputs for ever, the session never saying goodbye, the other machines waiting on it in turn)
        if (o.netplay && g_parted_after && frame >= g_parted_after) throw recomp::OutOfBudget(m.c.cpu.pc);
        if (o.netplay && o.netplay->resync_pending()) throw recomp::OutOfBudget(m.c.cpu.pc); // (again from the host's state)
        bool capture = m.linked && m.linked->capture_frame != 0 && frame == m.linked->capture_frame;
        // (--net-rollback: only a frame that won't be run again: every input of it in, none guessed wrong; else when it is)
        static const bool capture_debug = std::getenv("SPIKEOUT_NET_DEBUG") != nullptr;
        const auto capture_waited = std::chrono::steady_clock::now();
        if (capture && g_net_rb && (!m.inputs->wait_all(frame, o.boards) || m.inputs->wrong_from() <= frame)) capture = false;
        if (capture && capture_debug)
            std::printf("net: board %zu: every board's state at frame %llu: its inputs waited for %.0f ms\n", m.index,
                        static_cast<unsigned long long>(frame),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - capture_waited).count());
        if (((!o.save_state.empty() && frame == o.save_frame) || capture) && m.linked) // linked boards: each saves, the last writes
            m.c.request_snapshot([&m, &o](uint32_t) {
                const auto save_started = std::chrono::steady_clock::now();
                struct Said { // (SPIKEOUT_NET_DEBUG: how long the save took here, at the end)
                    std::chrono::steady_clock::time_point at;
                    std::size_t board;
                    ~Said() {
                        if (std::getenv("SPIKEOUT_NET_DEBUG"))
                            std::printf("net: board %zu saved in %.0f ms\n", board,
                                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count());
                    }
                } said{save_started, m.index};
                m3::State s(tools::kBoardStateBytes); // (its room at once: grown, it was copied over and over)
                tools::board_state(s, m.c, m.timing, sound_state(m, o));
                {
                    std::lock_guard guard(m.linked->lock);
                    m.linked->boards[m.index] = std::move(s.data());
                    ++m.linked->saved;
                }
                if (!o.parallel) m.linked->finish(); // (taking turns: the others are waiting)
            });
        else if (!o.save_state.empty() && frame == o.save_frame && m.index == 0) // --save-state: at the next block it can resume at
            m.c.request_snapshot([&m, &o](uint32_t pc) {
                m3::State s;
                tools::board_state(s, m.c, m.timing, sound_state(m, o));
                if (tools::write_file(o.save_state, s.data()))
                    std::printf("Saved %s: frame %llu, block %08x, %zu bytes\n", o.save_state.c_str(),
                                static_cast<unsigned long long>(m.timing.frames()), pc, s.data().size());
                else
                    std::fprintf(stderr, "gamerun: cannot write %s\n", o.save_state.c_str());
            });
        // --net-watch: the network board's status registers, when they change.
        if (o.net_watch && m.comm) {
            const uint16_t s0 = m.comm->io(0x88), s1 = m.comm->io(0x8A);
            if (s0 != m.net_status[0] || s1 != m.net_status[1])
                std::printf("board %zu frame %llu: network status0 %04x status1 %04x, 68000 PC %06x\n", m.index,
                            static_cast<unsigned long long>(frame), s0, s1, m.comm->pc());
            m.net_status[0] = s0;
            m.net_status[1] = s1;
        }
        // --rollback-probe: each frame's state saved (as rollback would, every
        // frame), timed; compared with the frame before's by 4 KB pages (what
        // a snapshot of the changes only would copy); copied (a restore's
        // least). Board 0 also times the frames' wall clock.
        if (o.rollback_frames && frame >= o.rollback_from && frame <= o.rollback_from + o.rollback_frames) {
            if (m.index == 0 && frame == o.rollback_from) m.probe.started = std::chrono::steady_clock::now();
            if (m.index == 0 && frame == o.rollback_from + o.rollback_frames)
                m.probe.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.probe.started).count();
            if (frame < o.rollback_from + o.rollback_frames && !o.rollback_speed_only)
                m.c.request_snapshot([&m, &o](uint32_t) {
                    using clock = std::chrono::steady_clock;
                    const auto t0 = clock::now();
                    m3::State s;
                    tools::board_state(s, m.c, m.timing, sound_state(m, o));
                    const auto t1 = clock::now();
                    std::vector<uint8_t> copy(s.data()); // (a restore copies as much back)
                    const auto t2 = clock::now();
                    Machine::Probe& p = m.probe;
                    const double save = std::chrono::duration<double, std::milli>(t1 - t0).count();
                    p.save_ms += save, p.save_ms_worst = std::max(p.save_ms_worst, save);
                    p.copy_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
                    p.size = copy.size();
                    if (p.before.size() == copy.size()) {
                        std::size_t changed = 0;
                        for (std::size_t at = 0; at < copy.size(); at += 4096) {
                            const std::size_t n = std::min<std::size_t>(4096, copy.size() - at);
                            if (std::memcmp(copy.data() + at, p.before.data() + at, n) != 0) changed += n;
                        }
                        p.changed += static_cast<double>(changed);
                        p.changed_worst = std::max(p.changed_worst, static_cast<double>(changed));
                        ++p.frames;
                    }
                    p.before = std::move(copy);
                });
        }
        // --rollback-test: at each frame (the snapshot point after its end), the
        // whole state hashed (the first time kept, then compared) and a
        // snapshot taken; every rb_every frames, back rb_depth frames, the
        // state there checked against its hash, and the run goes on from it.
        if (o.rb_frames && frame >= o.rb_from && frame < o.rb_from + o.rb_frames)
            m.c.request_snapshot([&m, &o, frame](uint32_t) {
                using clock = std::chrono::steady_clock;
                Machine::RollbackTest& t = m.rb;
                const auto walk = rollback_walk(m, o);
                if (!t.snaps) {
                    t.snaps = std::make_unique<m3::Snapshots>(o.rb_depth);
                    t.snaps->compare_only(o.rb_compare);
                }
                const uint64_t h = o.rb_timed ? 0 : state_hash(walk);
                if (o.rb_timed) { // (:timed: nothing hashed, rollback's own costs alone)
                } else if (auto it = t.hashes.find(frame); it == t.hashes.end()) {
                    t.hashes[frame] = h;
                    t.buffers[frame] = buffers_hash(walk);
                } else {
                    ++t.checked;
                    if (it->second != h && t.differed++ == 0) t.first_differ = frame;
                }
                const auto t0 = clock::now();
                t.snaps->save(frame, walk);
                const double save = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
                if (t.saves++ > 0) { // (the first copies everything)
                    t.save_ms += save, t.save_worst = std::max(t.save_worst, save);
                    t.pages += t.snaps->pages_saved(), t.pages_worst = std::max(t.pages_worst, t.snaps->pages_saved());
                }
                t.saved_frame = frame;
                if (o.boards > 1) return; // (linked boards go back together: LinkedRollback, at the slices' meeting)
                if ((frame - o.rb_from) % o.rb_every == o.rb_every - 1 && frame >= o.rb_from + o.rb_depth &&
                    t.rolled.insert(frame).second) {
                    const uint64_t back = frame - o.rb_depth;
                    const auto t1 = clock::now();
                    t.snaps->restore(back, walk);
                    const double restore = std::chrono::duration<double, std::milli>(clock::now() - t1).count();
                    t.restore_ms += restore, t.restore_worst = std::max(t.restore_worst, restore);
                    ++t.restores;
                    // (the big buffers: as they were then; the rest is checked by
                    // the frames run again, a just-loaded state saving its block
                    // count one less, ready to count it again)
                    if (!o.rb_timed && buffers_hash(walk) == t.buffers[back]) ++t.restored_exact;
                    t.quiet_through = frame;
                    t.back_at = t1;
                    throw Rewound{};
                }
            });
        if (m.comm && o.host_trace_frames) // --net-host-trace: the frames after this one
            m.comm->host_tracing = frame >= o.host_trace_frame && frame < o.host_trace_frame + o.host_trace_frames;
        if (o.tilegen_trace && frame % o.tilegen_trace == 0) {
            std::printf("%sframe %6llu tilegen", m.index ? ("board " + std::to_string(m.index) + " ").c_str() : "",
                        static_cast<unsigned long long>(frame));
            for (uint32_t reg : {0x20u, 0x40u, 0x44u, 0x60u, 0x64u, 0x68u, 0x6Cu}) std::printf(" %02x=%08x", reg, m.bus.tile_reg(reg));
            std::printf("\n");
        }
        if (o.screenshot && o.shot_every && frame % o.shot_every == 0 && !quiet) {
            const std::string path = board_path(o.screenshot, m.index);
            const size_t dot = path.find_last_of('.');
#ifdef SPIKEOUT_GPU
            if (o.interp && m.gpu && m.builder.extend() == 0 && !m.match_scene[0].models.empty()) {
                // --interp: the frame before and the picture halfway from it (the 2D layers this frame's)
                auto write = [&](const m3::Real3dScene& scene, const char* tag) {
                    std::vector<uint32_t> three_d, screen(m.screen.size());
                    m.gpu->render(m.bus.real3d(), scene, &three_d);
                    m.tilegen.composite(screen, three_d);
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + tag + path.substr(dot), screen,
                                     m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                };
                write(m.match_scene[0], "-before");
                write(m.interp_scene, "-mid");
            }
#endif
            if (const int e = m.builder.extend(); e > 0 && !m.gpu_wide.empty() && m.tilegen.extend() == e) {
                // --wide: the widescreen picture as the window shows it
                m.wide_bottom.build(m.tilegen.bottom(), m.tilegen.bottom_side(), e, m.bottom_wide, false,
                                    m.tilegen.bottom_own(), m.tilegen.bottom_real());
                m.wide_top.build(m.tilegen.top(), m.tilegen.top_side(), e, m.top_wide, true, m.tilegen.top_own(),
                                 m.tilegen.top_real());
                std::vector<uint32_t> wide(m.bottom_wide.size());
                m3::WideLayer::composite(wide, m.bottom_wide, m.top_wide, m.gpu_wide, e);
#ifdef SPIKEOUT_GPU
                if (m.presenter) { // --screen-time: the screen pass (layers uploaded, shaded), widescreen and 4:3
                    constexpr int kRepeats = 50;
                    auto time_draws = [&](std::span<const uint32_t> bottom, std::span<const uint32_t> top, int extend) {
                        const int w = m3::TilegenRenderer::kWidth, h = m3::TilegenRenderer::kHeight;
                        m.presenter->present_layers(bottom, top, m.gpu->display_image(), w, h, extend); // (its textures made)
                        SDL_WaitForGPUIdle(m.gpu_device);
                        const auto start = std::chrono::steady_clock::now();
                        for (int i = 0; i < kRepeats; ++i)
                            m.presenter->present_layers(bottom, top, m.gpu->display_image(), w, h, extend);
                        SDL_WaitForGPUIdle(m.gpu_device);
                        return 1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / kRepeats;
                    };
                    m.screen_wide_ms += time_draws(m.bottom_wide, m.top_wide, e);
                    m.screen_43_ms += time_draws(m.tilegen.bottom(), m.tilegen.top(), 0);
                    ++m.screen_samples;
                }
#endif
                tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-wide" + path.substr(dot), wide,
                                 m3::TilegenRenderer::kWidth + 2 * e, m3::TilegenRenderer::kHeight);
                if (o.dump_layers) { // and the layers below the 3D alone, and the 3D's alpha (grey)
                    std::vector<uint32_t> below(m.bottom_wide.size());
                    for (std::size_t i = 0; i < below.size(); ++i) below[i] = 0xFF000000u | m.bottom_wide[i];
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-wide2d" + path.substr(dot), below,
                                     m3::TilegenRenderer::kWidth + 2 * e, m3::TilegenRenderer::kHeight);
                    // and those above it (nothing: magenta)
                    std::vector<uint32_t> above(m.top_wide.size());
                    for (std::size_t i = 0; i < above.size(); ++i) above[i] = m.top_wide[i] >> 24 ? m.top_wide[i] : 0xFFFF00FFu;
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-wideabove" + path.substr(dot), above,
                                     m3::TilegenRenderer::kWidth + 2 * e, m3::TilegenRenderer::kHeight);
                    std::vector<uint32_t> alpha(m.gpu_wide.size());
                    for (std::size_t i = 0; i < alpha.size(); ++i) {
                        const uint32_t a = m.gpu_wide[i] >> 24;
                        alpha[i] = 0xFF000000u | a << 16 | a << 8 | a;
                    }
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-wide3da" + path.substr(dot), alpha,
                                     m3::TilegenRenderer::kWidth + 2 * e, m3::TilegenRenderer::kHeight);
                    // and the sides the tile generator drew itself, below and above the 3D (left, then right)
                    // (and from the game's own columns, m3::TileSides: those rows marked red at the edge)
                    for (const auto& [tag, side, real] :
                         {std::tuple{"-sidebelow", m.tilegen.bottom_side(), std::span<const uint8_t>()},
                          std::tuple{"-sideabove", m.tilegen.top_side(), std::span<const uint8_t>()},
                          std::tuple{"-ownbelow", m.tilegen.bottom_own(), m.tilegen.bottom_real()},
                          std::tuple{"-ownabove", m.tilegen.top_own(), m.tilegen.top_real()}}) {
                        if (side.size() != static_cast<std::size_t>(2 * e) * m3::TilegenRenderer::kHeight) continue;
                        std::vector<uint32_t> img(side.begin(), side.end());
                        for (uint32_t& p : img) p = (p >> 24) ? p : 0xFF404040u;
                        for (std::size_t y = 0; y < real.size(); ++y)
                            for (int s = 0; s < 2; ++s)
                                if (real[y] & (1u << s)) img[y * static_cast<std::size_t>(2 * e) + (s ? 2 * e - 1 : 0)] = 0xFFFF0000u;
                        tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + tag + path.substr(dot), img, 2 * e,
                                         m3::TilegenRenderer::kHeight);
                    }
                }
            }
            if (o.dump_layers) // each layer's whole name table, for looking at
                for (int n = 0; n < 4; ++n) {
                    std::vector<uint32_t> map(512 * 512);
                    m3::TilegenRenderer::draw_map(m.bus.tile_ram(), m.bus.tile_regs(), n, map);
                    tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-L" + std::to_string(n) +
                                         path.substr(dot), map, 512, 512);
                    // and its name table's entries, a line per tile row, then the line scroll values
                    const std::string text = path.substr(0, dot) + "-" + std::to_string(frame) + "-L" + std::to_string(n) + ".txt";
                    if (std::FILE* f = std::fopen(text.c_str(), "w")) {
                        const auto ram = m.bus.tile_ram();
                        auto word = [&](uint32_t at) {
                            return uint32_t{ram[at]} | uint32_t{ram[at + 1]} << 8 | uint32_t{ram[at + 2]} << 16 |
                                   uint32_t{ram[at + 3]} << 24;
                        };
                        std::fprintf(f, "reg %08x\n", m.bus.tile_regs()[0x60 / 4 + static_cast<std::size_t>(n)]);
                        for (uint32_t row = 0; row < 64; ++row) {
                            for (uint32_t col = 0; col < 32; ++col) {
                                const uint32_t w = word(0xF8000 + static_cast<uint32_t>(n) * 0x2000 + (row * 32 + col) * 4);
                                std::fprintf(f, "%04x %04x ", w >> 16, w & 0xFFFF);
                            }
                            std::fprintf(f, "\n");
                        }
                        for (uint32_t line = 0; line < 384; line += 2) {
                            const uint32_t w = word(0xF6000 + static_cast<uint32_t>(n) * 0x400 + line * 2);
                            std::fprintf(f, "%s%03x %03x", line % 32 ? " " : "\nscroll ", w >> 16, w & 0xFFFF);
                        }
                        std::fprintf(f, "\n");
                        std::fclose(f);
                    }
                }
            tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + path.substr(dot), m.screen,
                             m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
            if (o.renderer == R::Compare && !m.gpu_image.empty()) { // and the GPU's, beside it
                std::vector<uint32_t> gpu_screen(m.screen.size());
                m.tilegen.composite(gpu_screen, m.gpu_image);
                tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-gpu" + path.substr(dot), gpu_screen,
                                 m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                // Where the 3D differs: red, over the software picture dimmed.
                std::vector<uint32_t> diff(m.screen.size());
                const auto soft = m.real3d.image();
                for (std::size_t i = 0; i < diff.size() && i < soft.size() && i < m.gpu_image.size(); ++i) {
                    const auto [pixels, largest] = difference(soft.subspan(i, 1), std::span<const uint32_t>(m.gpu_image).subspan(i, 1));
                    (void)largest;
                    const uint32_t p = m.screen[i];
                    diff[i] = pixels ? 0xFFFF0000u : 0xFF000000u | ((p >> 2) & 0x003F3F3Fu);
                }
                tools::write_png(path.substr(0, dot) + "-" + std::to_string(frame) + "-diff" + path.substr(dot), diff,
                                 m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                // The two 3D pictures alone, colour and alpha (grey).
                auto write_3d = [&](std::span<const uint32_t> img, const std::string& tag) {
                    std::vector<uint32_t> rgb(img.size()), alpha(img.size());
                    for (std::size_t i = 0; i < img.size(); ++i) {
                        rgb[i] = 0xFF000000u | img[i];
                        const uint32_t a = img[i] >> 24;
                        alpha[i] = 0xFF000000u | a << 16 | a << 8 | a;
                    }
                    const std::string at = path.substr(0, dot) + "-" + std::to_string(frame);
                    tools::write_png(at + "-3d" + tag + path.substr(dot), rgb, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                    tools::write_png(at + "-3da" + tag + path.substr(dot), alpha, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight);
                };
                write_3d(soft, "");
                write_3d(m.gpu_image, "-gpu");
            }
        }
        // (--net-rollback: this board's snapshot after the frame, the last asked: another state taken here leaves no cut)
        // (lazy, as the window: only the frames a going back may need, Linked::wants, the same for every board; the
        // boards filled here told by this machine's own)
        if (g_net_rb && o.netplay && m.index == o.local_seat) {
            std::vector<bool> local(o.boards);
            for (std::size_t s = 0; s < o.boards; ++s)
                local[s] = o.netplay->session().fills_inputs(static_cast<uint8_t>(s), frame + 1);
            g_net_rb->set_local(std::move(local));
        }
        if (g_net_rb && g_net_rb->wants(frame)) m.c.request_snapshot([&m, frame](uint32_t) { g_net_rb->save(m.index, frame); });
    };
}

void made_blocks(Machine& m, tools::Turns& turns, uint64_t write_at, uint32_t slice);

// --wait-seat: a waiting cabinet nobody runs (an empty seat, its board stopped at a clean point):
// its network board fed its last block held, the header's three counters (+0x00, +0x1C, +0x58)
// one up a frame, at the stand-in's times, until frame `until` starts; then the board brought to
// the present: its frame count and clock moved on (the time base held: no time passes for it), and
// the RAM its counters come from (0x1000A4, 0x1000A8, 0x140048) set to go on from the held
// blocks' last, each as far from it as the board had them when it stopped. Then it runs on.
void wait_seat(Machine& m, tools::Turns& turns, const Frozen& z) {
    m3::NetBoardDevice& nb = m.bus.netboard();
    std::array<uint32_t, 0x240> block{};
    for (uint32_t i = 0; i < 0x240; ++i) block[i] = m.bus.read32(0x141000 + 4 * i);
    const uint32_t sources[3] = {0x1000A4, 0x1000A8, 0x140048}, words[3] = {0x00, 0x1C, 0x58};
    int64_t ahead[3];
    for (int k = 0; k < 3; ++k) ahead[k] = int64_t(m.bus.read32(sources[k])) - int64_t(block[words[k] / 4]);
    const uint64_t frame_cycles = m.timing.frame_cycles(), line_cycles = m.timing.line_cycles();
    const uint64_t vblank = line_cycles * recomp::FrameTiming::kVBlankLines;
    const uint64_t first = m.timing.frames();
    for (uint64_t f = first; f < z.until; ++f) {
        const uint64_t frame_start = m.timing.frame_start(f), at = frame_start + z.write_at;
        bool written = false;
        auto write = [&](uint64_t until) {
            if (written || at > until) return;
            nb.sync(at);
            for (uint32_t w : words) ++block[w / 4];
            for (uint32_t i = 0; i < 0x240; ++i) nb.write32(0xC0000100 + 4 * i, block[i]);
            written = true;
        };
        for (uint32_t line = z.slice - 1; line < recomp::FrameTiming::kDisplayLines; line += z.slice) {
            const uint64_t t = frame_start + vblank + line * line_cycles;
            write(t);
            nb.sync(t);
            turns.pass(m.index);
        }
        const uint64_t end = frame_start + frame_cycles;
        write(end);
        nb.sync(end);
        nb.end_frame();
    }
    const uint64_t frames = z.until - first, shift = frames * frame_cycles;
    m.timing.move_on(frames);
    recomp::Context::Aside a = m.c.set_aside();
    a.cycles += shift, a.next_check += shift, a.scheduler_due += shift;
    a.tb_offset -= shift * 33 / 332; // (the time base where it was)
    m.c.put_back(a);
    for (int k = 0; k < 3; ++k) m.bus.write32(sources[k], static_cast<uint32_t>(int64_t(block[words[k] / 4]) + ahead[k]));
    std::printf("board %zu: held %llu frames (%llu-%llu), back at frame %llu\n", m.index, (unsigned long long)frames,
                (unsigned long long)first, (unsigned long long)z.until, (unsigned long long)m.timing.frames());
}

void run(Machine& m, tools::Turns& turns) {
    turns.wait(m.index);
    const auto start = std::chrono::steady_clock::now();
    for (bool resume = m.loaded;;) {
        try {
            if (resume) m.c.resume_block(m.c.cpu.pc);
            m.c.call(m.c.cpu.pc);
            m.why = "the reset code returned";
        } catch (const Rewound&) { // (--rollback-test: back at an earlier frame, on from there)
            resume = true;
            continue;
        } catch (const tools::rollback::Rewound&) { // (--net-rollback: the same)
            resume = true;
            continue;
        } catch (const Dropped& d) { // (--drop: this board's PC gone; its blocks made from board 0 from here on)
            m.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (d.module) { // (--one-board: made by the other board's cabinets from the next frame's block, at its meetings)
                tools::one_board::Board& other = (*g_machines)[m.ob.peer]->ob;
                if (d.shadow) tools::one_board::Game::shadow_own_bytes(m.ob, other);
                {
                    std::lock_guard guard(other.cabinets_lock);
                    other.cabinets->start_making(m.index, m.bus.netboard(), m.bus.ram(), m.timing.frames() - 1);
                }
                m.why = "one board: its blocks made by board " + std::to_string(other.index) + " from frame " + std::to_string(m.timing.frames());
                if (d.shadow && g_one_board.shadow_wait(m.ob, turns, d.slice)) { // (on as its shadow)
                    m.why.clear();
                    resume = true;
                    continue;
                }
                turns.finish(m.index);
                return;
            }
            made_blocks(m, turns, d.write_at, d.slice);
            return;
        } catch (const tools::one_board::HandedBack& h) { // (--one-board: its shadow stopped, the module on from where it finished h.count)
            if (g_one_board.handed_back(m.ob, turns, h.count, m.why)) {
                resume = true;
                continue;
            }
        } catch (const Cloned&) { // (--clone-seat: on from board 0's state, as this cabinet)
            resume = true;
            continue;
        } catch (const Frozen& z) { // (--wait-seat: held, then brought back to the present)
            wait_seat(m, turns, z);
            resume = true;
            continue;
        } catch (const recomp::OutOfBudget&) {
            m.why = "block limit reached";
        } catch (const recomp::MissingCode& e) {
            m.why = e.what();
        } catch (const recomp::GuestError& e) {
            m.why = e.what();
        } catch (const std::exception& e) {
            m.why = std::string("error: ") + e.what();
        }
        break;
    }
    tools::one_board::Game::stopped(m.ob); // (--net-one-board: this machine's own board's shadows stop with it)
    m.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    turns.finish(m.index);
}

// --link-standin: a board whose main board isn't run, its network board
// (loaded from the same state) driven by the calls its main board made in
// a run recorded with --link-record; each read checked against the value
// it gave then.
// `write_at` (--link-standin-at CYCLES): each frame's writes held, and
// made together that many main cycles after the frame's end before
// (rather than when the main board made them).
void stand_in(Machine& m, tools::Turns& turns, const std::vector<HostCall>& log, uint64_t write_at, bool patch_enemy_flags,
              bool no_reads, bool coarse) {
    turns.wait(m.index);
    m3::NetBoardDevice& nb = m.bus.netboard();
    uint64_t reads = 0, differed = 0, first = 0, frame_end = 0;
    std::vector<HostCall> held;
    const auto start = std::chrono::steady_clock::now();
    for (const HostCall& e : log) {
        if (write_at && !held.empty() && frame_end && e.t >= frame_end + write_at) {
            nb.sync(frame_end + write_at);
            for (const HostCall& w : held) nb.write32(w.addr, w.value);
            held.clear();
        }
        if (e.op == HostCall::EndFrame) frame_end = e.t;
        if (write_at && e.op == HostCall::Write32 && frame_end) {
            held.push_back(e);
            continue;
        }
        // (--link-standin-no-reads: the main board's reads left out; they change nothing on the
        // network board, io_read being const: only its writes and the time matter)
        if (no_reads && e.op >= HostCall::Read8 && e.op <= HostCall::Read32) continue;
        // (--link-standin-coarse: the main board's own syncs left out too; the network board is
        // brought to the time only at the slices' meetings and the frames' ends, as a stand-in
        // run beside another board would be)
        if (coarse) {
            if (e.op == HostCall::Sync) continue;
            if (e.op == HostCall::Pass || e.op == HostCall::EndFrame) nb.sync(e.t);
        }
        uint32_t got = e.value;
        if (patch_enemy_flags && e.op == HostCall::Write32 && e.addr >= 0xC0000200 && e.addr < 0xC0000A00) {
            // (--link-standin-patch enemy-flags: each record's enemies' flags low byte as 0x03)
            const uint32_t at = (e.addr - 0xC0000200) % 0x200;
            if (at >= 0x44 && at <= 0x98 && (at - 0x44) % 12 == 0) {
                nb.write32(e.addr, (e.value & ~0xFFu) | 0x03);
                continue;
            }
        }
        switch (e.op) {
        case HostCall::Sync: nb.sync(e.t); break;
        case HostCall::Read8: got = nb.read8(e.addr); break;
        case HostCall::Read16: got = nb.read16(e.addr); break;
        case HostCall::Read32: got = nb.read32(e.addr); break;
        case HostCall::Write8: nb.write8(e.addr, static_cast<uint8_t>(e.value)); break;
        case HostCall::Write16: nb.write16(e.addr, static_cast<uint16_t>(e.value)); break;
        case HostCall::Write32: nb.write32(e.addr, e.value); break;
        case HostCall::EndFrame: nb.end_frame(); break;
        case HostCall::Pass: turns.pass(m.index); break;
        }
        if (e.op >= HostCall::Read8 && e.op <= HostCall::Read32) {
            ++reads;
            if (got != e.value && differed++ == 0) first = e.t;
        }
    }
    m.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    m.why = "stood in: " + std::to_string(log.size()) + " calls replayed, " + std::to_string(reads) + " reads, " +
            std::to_string(differed) + " read differently" +
            (differed ? " (the first at main cycle " + std::to_string(first) + ")" : std::string());
    turns.finish(m.index);
}

// --link-standin-blocks: board m's network board driven by its main board's blocks alone, as a
// PC running only its own board would get them from the remote one: from the recorded calls,
// only each frame's writes are kept (the frames counted by their ends); the time is this side's
// own, the network board brought to each slice's meeting (the start of display lines 7, 15, ...:
// the nominal cycle, as recomp::FrameTiming has it) and each frame's end, and the block written
// `write_at` main cycles after the end of the frame before it.
void stand_in_blocks(Machine& m, tools::Turns& turns, const std::vector<HostCall>& log, uint64_t write_at, uint32_t slice) {
    turns.wait(m.index);
    m3::NetBoardDevice& nb = m.bus.netboard();
    std::vector<std::vector<HostCall>> blocks(1); // (by frame, from the state's frame)
    for (const HostCall& e : log) {
        if (e.op == HostCall::EndFrame) blocks.emplace_back();
        else if (e.op >= HostCall::Write8 && e.op <= HostCall::Write32) blocks.back().push_back(e);
    }
    const auto start = std::chrono::steady_clock::now();
    const uint64_t frame_cycles = m.timing.frame_cycles(), line_cycles = m.timing.line_cycles();
    const uint64_t vblank = line_cycles * recomp::FrameTiming::kVBlankLines;
    uint64_t written = 0;
    const uint64_t first = m.timing.frames();
    std::size_t due = 0; // (the next frame's block to write)
    auto write_block = [&](uint64_t until) { // the blocks due by `until`
        while (due < blocks.size() && m.timing.frame_start(first + due) + write_at <= until) {
            nb.sync(m.timing.frame_start(first + due) + write_at);
            for (const HostCall& w : blocks[due]) {
                if (w.op == HostCall::Write32) nb.write32(w.addr, w.value);
                else if (w.op == HostCall::Write16) nb.write16(w.addr, static_cast<uint16_t>(w.value));
                else nb.write8(w.addr, static_cast<uint8_t>(w.value));
            }
            written += blocks[due].size();
            ++due;
        }
    };
    for (std::size_t k = 0; k + 1 < blocks.size(); ++k) { // (each frame the log has an end of)
        const uint64_t frame_start = m.timing.frame_start(first + k);
        for (uint32_t line = slice - 1; line < recomp::FrameTiming::kDisplayLines; line += slice) {
            const uint64_t t = frame_start + vblank + line * line_cycles;
            write_block(t);
            nb.sync(t);
            turns.pass(m.index);
        }
        const uint64_t end = frame_start + frame_cycles;
        write_block(end);
        nb.sync(end);
        nb.end_frame();
    }
    // (and the meetings of the frame the log stops in, as many as it has)
    std::size_t passes_after = 0;
    for (auto it = log.rbegin(); it != log.rend() && it->op != HostCall::EndFrame; ++it) passes_after += it->op == HostCall::Pass;
    const uint64_t last_start = m.timing.frame_start(first + blocks.size() - 1);
    for (std::size_t p = 0; p < passes_after; ++p) {
        const uint64_t t = last_start + vblank + ((p + 1) * slice - 1) * line_cycles;
        write_block(t);
        nb.sync(t);
        turns.pass(m.index);
    }
    m.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    m.why = "stood in from blocks: " + std::to_string(blocks.size() - 1) + " frames, " + std::to_string(written) + " writes";
    turns.finish(m.index);
}

// --drop B@FRAME: board B's PC gone at the end of its frame FRAME. Its network board goes on (the
// stand-in's time: each slice's meeting and frame's end), fed blocks made each frame from board
// 0's own (the shared bytes) and what every PC can know alike (docs/netplay.md, "a dropped
// cabinet"): the header's three counters (+0x00, +0x1C, +0x58) one up a frame from its last, its
// other own bytes as they were; the next record (in its slots' turn) board 0's newest, with its
// own fields: no buttons (+0x04), the player's facing (+0x08, its +0x2A), +0x0A (its +0x76 when
// its +0x44's top bit is set, else +0x74) and first word (+0x10) from board 0's copy of that
// player a frame before, the camera's heading (+0x0C), the empty slots' leftovers (+0x34-0x37,
// +0x40-0x43) and the on-screen bits held as they were. RAM's order (the block at 0x141000:
// header 0x100, four records of 0x200; written to the network board at 0xC0000100 on).
void made_blocks(Machine& m, tools::Turns& turns, uint64_t write_at, uint32_t slice) {
    Machine& local = *(*g_machines)[m.index == 0 ? 1 : 0]; // (the board whose block it starts from: the other)
    m3::NetBoardDevice& nb = m.bus.netboard();
    std::array<uint32_t, 0x240> block{}; // (its last block, as words)
    for (uint32_t i = 0; i < 0x240; ++i) block[i] = m.bus.read32(0x141000 + 4 * i);
    auto newest = [](const std::array<uint32_t, 0x240>& b) {
        int best = 0;
        for (int s = 1; s < 4; ++s)
            if (b[(0x100 + s * 0x200 + 0xDC) / 4] > b[(0x100 + best * 0x200 + 0xDC) / 4]) best = s;
        return best;
    };
    const uint32_t player = 0x1A0000 + 0x4000 * static_cast<uint32_t>(m.index); // (its player's object)
    struct Copy { uint32_t first, flags44; uint16_t facing, h74, h76; } before{};
    auto copy_now = [&] {
        return Copy{local.bus.read32(player), local.bus.read32(player + 0x44), local.bus.read16(player + 0x2A),
                    local.bus.read16(player + 0x74), local.bus.read16(player + 0x76)};
    };
    before = copy_now();
    // --drop-buttons DUMP: this seat's buttons word (record +0x04) for each logic count, as a real run of
    // it sent them (a --link-dump: the network board's byte order, each word swapped)
    std::map<uint32_t, uint32_t> buttons;
    if (g_drop_buttons) {
        const std::vector<uint8_t> bytes = tools::read_file(g_drop_buttons);
        auto word = [&](std::size_t at) {
            uint32_t v;
            std::memcpy(&v, bytes.data() + at, 4);
            return v; // (the dump's words as the host reads them: little-endian, so RAM's order)
        };
        for (std::size_t at = 0; at + 8 + 0x900 <= bytes.size(); at += 8 + 0x900) {
            if (word(at) != m.index) continue;
            for (uint32_t s = 0; s < 4; ++s) {
                const std::size_t rec = at + 8 + 0x100 + 0x200 * s;
                if (const uint32_t logic = word(rec + 0xDC)) buttons[logic] = word(rec + 0x04);
            }
        }
    }
    const uint64_t frame_cycles = m.timing.frame_cycles(), line_cycles = m.timing.line_cycles();
    const uint64_t vblank = line_cycles * recomp::FrameTiming::kVBlankLines;
    uint64_t made = 0;
    auto put8 = [](std::array<uint32_t, 0x240>& b, uint32_t byte, uint8_t v) { // (RAM's byte order: big-endian words)
        uint32_t& w = b[byte / 4];
        const uint32_t shift = 8 * (3 - byte % 4);
        w = (w & ~(0xFFu << shift)) | (uint32_t(v) << shift);
    };
    auto get8 = [](const std::array<uint32_t, 0x240>& b, uint32_t byte) { return static_cast<uint8_t>(b[byte / 4] >> (8 * (3 - byte % 4))); };
    for (uint64_t f = m.timing.frames();; ++f) {
        const uint64_t frame_start = m.timing.frame_start(f), at = frame_start + write_at;
        bool written = false;
        auto write_made = [&](uint64_t until) {
            if (written || at > until) return;
            nb.sync(at);
            std::array<uint32_t, 0x240> mine{};
            for (uint32_t i = 0; i < 0x240; ++i) mine[i] = local.bus.read32(0x141000 + 4 * i);
            std::array<uint32_t, 0x240> next = block;
            for (uint32_t i = 0; i < 0x40; ++i) next[i] = mine[i]; // the header: board 0's, with its own bytes
            for (uint32_t byte : {0x02u, 0x03u, 0x16u, 0x17u, 0x1Eu, 0x1Fu, 0x23u, 0x25u, 0x26u, 0x41u, 0x44u, 0x45u, 0x46u, 0x47u, 0x4Eu,
                                  0x4Fu, 0x5Au, 0x5Bu})
                put8(next, byte, get8(block, byte));
            for (uint32_t w : {0x00u, 0x1Cu, 0x58u}) next[w / 4] = block[w / 4] + 1; // (the counters)
            // (its status, +0x06: from its own flow run on the side, after the logic count before)
            if (const auto h = local.camera.header_at.find(next[0x1C / 4] - 1); h != local.camera.header_at.end())
                put8(next, 0x06, h->second[6]), put8(next, 0x07, h->second[7]);
            // (the record board 0 wrote for the logic count this one is at: a slave runs a frame
            // behind the master, so board 0's from the frame before, still in its slots)
            // (a record of its, logic count `logic`, into slot `to`, its buttons those of its frame `bf`; `old` the slot
            // of the block before whose leftovers it keeps)
            auto make_record = [&](int to, uint32_t logic, uint64_t bf, uint32_t old, bool quiet) {
            int from = -1;
            for (int s = 0; s < 4; ++s)
                if (mine[(0x100 + s * 0x200 + 0xDC) / 4] == logic) from = s;
            const bool placeholder = from < 0;
            if (placeholder) { // (none: the other board's newest, noted)
                from = newest(mine);
                if (m.why.empty() && !quiet)
                    m.why = "no record of board " + std::to_string(local.index) + "'s for logic " + std::to_string(logic) + " (frame " + std::to_string(f) + ")";
            }
            const uint32_t rec = 0x100 + 0x200 * static_cast<uint32_t>(to), src = 0x100 + 0x200 * static_cast<uint32_t>(from);
            for (uint32_t i = 0; i < 0x80; ++i) next[rec / 4 + i] = mine[src / 4 + i];
            if (placeholder) next[(rec + 0xDC) / 4] = logic, put8(next, rec + 0xD9, static_cast<uint8_t>(logic)); // (its count's)
            // (its buttons word: what 0x5094 makes of its inputs each frame, held ones set, the system
            // board's 0x04 under select 0 in bits 0-7, 0x08 in 8-15, 0x0C in 16-23, 0x04 under select 1
            // in 24-31, its EEPROM's data line idle (high); from its --press list for this frame)
            uint8_t in[2] = {0xFF, 0xFF};
            if (g_presses)
                for (const Press& p : *g_presses)
                    if (for_board(p.board, m.index) && bf >= p.frame && bf < p.frame + p.frames)
                        in[p.bank] = static_cast<uint8_t>(in[p.bank] & ~(1u << p.bit));
            next[(rec + 0x04) / 4] = ~(uint32_t(in[0]) | uint32_t(in[1]) << 8 | 0xFFu << 16 | 0xFFu << 24);
            if (const auto b = buttons.find(logic); b != buttons.end()) next[(rec + 0x04) / 4] = b->second;
            // (its player's fields as board 0 had them as it wrote its own record of this logic count, the
            // same point of the frame's code: --camera-test-free's hook; else as board 0 has them now)
            if (const auto p = local.camera.players_at.find(logic); p != local.camera.players_at.end()) {
                const auto& v = p->second[m.index];
                before = Copy{v[0], v[1], uint16_t(v[2]), uint16_t(v[3]), uint16_t(v[4])};
            }
            next[(rec + 0x08) / 4] = uint32_t(before.facing) << 16 | ((before.flags44 & 0x80000000u) ? before.h76 : before.h74);
            // (its camera's heading: run here on the side for its logic count, --camera-test-free; else held)
            const auto heading = local.camera.heading_at.find(logic - 1); // (the record of logic L carries the camera after update L-1)
            const uint32_t held = block[(old + 0x0C) / 4] & 0xFFFF0000u;
            next[(rec + 0x0C) / 4] = (heading != local.camera.heading_at.end() ? uint32_t(heading->second) << 16 : held) |
                                     (next[(rec + 0x0C) / 4] & 0xFFFFu);
            next[(rec + 0x10) / 4] = before.first;
            if (!local.camera.view_at.empty()) // (its low byte its view bits: its own camera's test of it, run here, the logic count before)
                if (const auto v = local.camera.view_at.find({logic - 1, player}); v != local.camera.view_at.end())
                    next[(rec + 0x10) / 4] = (before.first & ~0xFFu) | (v->second & 0xFFu);
            next[(rec + 0x34) / 4] = block[(old + 0x34) / 4], next[(rec + 0x40) / 4] = block[(old + 0x40) / 4];
            for (uint32_t byte : {0x47u, 0x5Fu, 0x6Bu, 0x77u}) put8(next, rec + byte, get8(block, old + byte)); // (on-screen bits)
            // (with its camera run here: every character's flags low byte, its on-screen bits, from that
            // camera's view test of it two logic counts before (the entry is the character as it was at the
            // record of the count before, its test earlier in that frame), but none for one gone since (its
            // word cleared after the test: board 0's entry all zero); the list: players 1-4, then enemies 1-8)
            if (!local.camera.view_at.empty())
                for (uint32_t k = 0; k < 12; ++k) {
                    const uint32_t character = k < 4 ? 0x1A0000 + 0x4000 * k : 0x1B0000 + 0x4000 * (k - 4);
                    const uint32_t entry = rec + 0x14 + 12 * k;
                    const auto v = local.camera.view_at.find({logic - 2, character});
                    if (v != local.camera.view_at.end() && next[entry / 4] != 0) put8(next, entry + 3, static_cast<uint8_t>(v->second));
                }
            };
            const uint32_t logic = next[0x1C / 4];
            if (m.index == 0) { // (the master, made by a slave a frame behind it: every record again from the slave's own of
                // the same logic count, in slot logic & 3 as 0x6EE78 puts them; the newest, its next count's, the slave
                // hasn't yet: its newest's for now; the slave's game reads the one two back)
                for (uint32_t back = 4; back-- > 0;) {
                    const uint32_t x = logic - back;
                    make_record(static_cast<int>(x & 3), x, f - back, 0x100 + 0x200 * (x & 3), back == 0);
                }
            } else {
                make_record((newest(block) + 1) % 4, logic, f, 0x100 + 0x200 * static_cast<uint32_t>(newest(block)), false);
            }
            for (uint32_t i = 0; i < 0x240; ++i) nb.write32(0xC0000100 + 4 * i, next[i]);
            block = next;
            written = true;
            ++made;
        };
        for (uint32_t line = slice - 1; line < recomp::FrameTiming::kDisplayLines; line += slice) {
            const uint64_t t = frame_start + vblank + line * line_cycles;
            write_made(t);
            nb.sync(t);
            if (turns.alone(m.index)) {
                m.why = "dropped; " + std::to_string(made) + " blocks made" + (m.why.empty() ? std::string() : "; " + m.why);
                turns.finish(m.index);
                return;
            }
            turns.pass(m.index);
        }
        const uint64_t end = frame_start + frame_cycles;
        write_made(end);
        nb.sync(end);
        nb.end_frame();
    }
}

void report(Machine& m, const Options& o) {
    recomp::Context& c = m.c;
    m3::Bus& bus = m.bus;
    const long long ran = static_cast<long long>(c.blocks);

    std::printf("Stopped: %s\n", m.why.c_str());
    std::printf("Ran %lld blocks in %.2f s (%.0f million blocks/s), %.2f 603e cycles a block; its thread's CPU time %.2f s\n", ran,
                m.seconds, static_cast<double>(ran) / m.seconds / 1e6,
                ran > 0 ? static_cast<double>(c.cycles()) / static_cast<double>(ran) : 0.0, m.cpu_seconds);
    if (c.skipped_blocks) // (the vblank wait gone round at once: counted in the blocks, not run)
        std::printf("Idle: %llu of those blocks the vblank wait's, not run (--no-idle-skip runs them)\n",
                    (unsigned long long)c.skipped_blocks);
    if (m.ob.cabinets) { // --one-board: the other cabinets' side runs
        std::printf("Other cabinets: %llu side runs, %.2f s in them (%.2f s with their set-ups; %llu RAM bytes written and put back), "
                    "their network boards %.2f s, %llu blocks made%s%s\n",
                    (unsigned long long)m.ob.cabinets->side_runs(), m.ob.cabinets->side_seconds(), m.ob.cabinets->whole_seconds(),
                    (unsigned long long)m.ob.cabinets->journal_bytes(), m.ob.cabinets->netboard_seconds(), (unsigned long long)m.ob.cabinets->made(),
                    m.ob.cabinets->failed().empty() ? "" : "; first failure: ", m.ob.cabinets->failed().c_str());
        if (m.ob.cabinets->workers())
            std::printf("Other cabinets: their tasks' side runs on %zu other thread(s), %.2f s there with their set-ups; this "
                        "board waited %.2f s for them, %.2f s handing them over\n",
                        m.ob.cabinets->workers(), m.ob.cabinets->worker_seconds(), m.ob.cabinets->worker_wait(),
                        m.ob.cabinets->worker_hand());
        std::printf("Other cabinets' side runs by kind (their buttons waited for %llu times, %.2f s):\n%s",
                    (unsigned long long)g_one_board.held_waits(), g_one_board.held_wait_seconds(), m.ob.cabinets->profile().c_str());
        std::printf("Devices read in them (by m3::Device):");
        for (std::size_t d = 0; d < m.bus.held_reads().size(); ++d)
            if (m.bus.held_reads()[d]) std::printf(" %zu: %llu", d, (unsigned long long)m.bus.held_reads()[d]);
        std::printf("\n");
        if (m.ob.cabinets->heavy_frames())
            std::printf("Other cabinets: %llu heavy frames on side runs (first at %llu): a frame they may have lost, no shadow running\n",
                        (unsigned long long)m.ob.cabinets->heavy_frames(), (unsigned long long)m.ob.cabinets->first_heavy());
    }
    std::printf("PC %08x  LR %08x  CTR %08x  MSR %08x  CR %08x\n", c.cpu.pc, c.cpu.lr, c.cpu.ctr, c.cpu.msr, c.cpu.cr);
    // Address translation as the game set it up (the runtime treats every
    // address as physical; these show whether that holds).
    {
        using A = m3::Jtag::Asic;
        const m3::Jtag& j = bus.jtag();
        std::printf("Real3D mode words (JTAG): Mercury %08x  Venus %08x  Earth %08x  Mars %08x  Jupiter %08x\n",
                    j.modeword(A::Mercury), j.modeword(A::Venus), j.modeword(A::Earth), j.modeword(A::Mars),
                    j.modeword(A::Jupiter));
    }
    std::printf("MMU: SDR1 %08x  HID0 %08x\n", c.cpu.sdr1, c.cpu.hid0);
    for (int i = 0; i < 4; ++i)
        std::printf("  IBAT%d %08x %08x   DBAT%d %08x %08x\n", i, c.cpu.ibat[2 * i], c.cpu.ibat[2 * i + 1], i,
                    c.cpu.dbat[2 * i], c.cpu.dbat[2 * i + 1]);
    std::printf("  SR");
    for (uint32_t sr : c.cpu.sr) std::printf(" %08x", sr);
    std::printf("\n");
    // The page tables in RAM: the one SDR1 gives (HTABORG, HTABMASK), which
    // Spikeout's instruction TLB miss handler searches, and the one 128 KB
    // above it its data handlers search. Every valid PTE's page, from its
    // group and API, against the page it maps to.
    for (const uint32_t org : {c.cpu.sdr1 & 0xFFFF0000u, (c.cpu.sdr1 & 0xFFFF0000u) + 0x20000}) {
        const uint32_t groups = ((c.cpu.sdr1 & 0x1FF) + 1) * 1024; // 64-byte groups of 8 PTEs
        uint32_t valid = 0, identity = 0, shown = 0;
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t g = 0; g < groups && org + (g + 1) * 64 <= bus.ram().size(); ++g)
            for (uint32_t k = 0; k < 8; ++k) {
                const uint32_t at = org + g * 64 + k * 8;
                const uint32_t w0 = bus.read32(at), w1 = bus.read32(at + 4);
                if (!(w0 >> 31)) continue;
                ++valid;
                const uint32_t vsid = (w0 >> 7) & 0xFFFFFF, h = (w0 >> 6) & 1, api = w0 & 0x3F;
                const uint32_t hash = h ? ~g : g;
                const uint32_t page = (api << 10) | ((hash ^ vsid) & 0x3FF); // 16-bit page index
                int seg = -1;
                for (int s = 0; s < 16; ++s)
                    if ((c.cpu.sr[s] & 0xFFFFFF) == vsid) seg = s;
                const uint32_t pa = w1 & 0xFFFFF000u;
                const uint32_t ea = seg < 0 ? 0 : static_cast<uint32_t>(seg) << 28 | page << 12;
                if (seg >= 0 && ea == pa) {
                    ++identity;
                    lo = std::min(lo, ea);
                    hi = std::max(hi, ea + 0xFFF);
                } else if (shown++ < 8) {
                    std::printf("  PTE at %06x: VSID %06x (segment %d) page %04x -> %08x, WIMG %x PP %u\n", at, vsid, seg,
                                page, pa, (w1 >> 3) & 0xF, w1 & 3);
                }
            }
        std::printf("Page table at %08x: %u valid PTEs, %u map their page to itself (%08x-%08x)\n", org, valid, identity,
                    lo == UINT32_MAX ? 0 : lo, hi);
    }

    std::printf("Frames %llu (%.2f s of game time); interrupts taken: %llu external, %llu decrementer; "
                "IRQ enable %02x, pending %02x\n",
                static_cast<unsigned long long>(m.timing.frames()),
                static_cast<double>(m.timing.frames()) / recomp::FrameTiming::kRefreshHz,
                static_cast<unsigned long long>(c.external_interrupts),
                static_cast<unsigned long long>(c.decrementer_interrupts), bus.irq().enable(), bus.irq().state());

    std::printf("Network board: %s\n", bus.netboard().describe().c_str());
    if (o.replay && m.index == 0)
        std::printf("Replay: %llu of %zu hashes matched the recording's%s\n", static_cast<unsigned long long>(m.replay_matched),
                    o.replay->hashes.size(),
                    m.replay_parted ? (", parted at frame " + std::to_string(m.replay_parted)).c_str() : " (the same run)");
    if (o.linked_inputs)
        std::printf("Inputs: board %zu: %llu of the recorded run's hashes matched%s\n", m.index,
                    static_cast<unsigned long long>(m.replay_matched),
                    m.replay_parted ? (", parted at frame " + std::to_string(m.replay_parted)).c_str() : " (the same run)");
    if (o.input_log && m.index == 0)
        std::printf("Inputs: %llu of the recorded run's hashes matched%s\n", static_cast<unsigned long long>(m.replay_matched),
                    m.replay_parted ? (", parted at frame " + std::to_string(m.replay_parted)).c_str() : " (the same run)");
    const double played = static_cast<double>(m.sound.board.samples());
    std::printf("Sound board: %s; %d slots playing, %llu MIDI bytes dropped; output peak %d, RMS %.0f; %.2f s to run it\n",
                m.sound.board.describe().c_str(), m.sound.board.active_slots(),
                static_cast<unsigned long long>(m.sound.board.midi_dropped()), m.peak,
                played > 0 ? std::sqrt(m.energy / (2 * played)) : 0.0, m.sound_seconds);
    if (const m3::Dsb2* music = m.sound.board.music())
        std::printf("Music board: %s; its output peak %d, RMS %.0f\n", music->describe().c_str(), music->music_peak,
                    played > 0 ? std::sqrt(music->music_energy / (2 * played)) : 0.0);
    if (o.wav) {
        const std::string path = board_path(o.wav, m.index);
        if (tools::write_wav(path.c_str(), m.audio, m3::Scsp::kSampleRate)) std::printf("Wrote %s\n", path.c_str());
    }
    if (c.first_nan_pc) // SPIKEOUT_FP_TRAP builds
        std::printf("First FP NaN: at %08x (%016llx)\n", c.first_nan_pc, static_cast<unsigned long long>(c.first_nan_bits));
    if (m.match_models) {
        const double n = static_cast<double>(m.match_models);
        std::printf("Models between frames: %llu over %llu frames; found in the frame before: %.2f%% once, %.2f%% several "
                    "times (the same key), %.2f%% not\n",
                    static_cast<unsigned long long>(m.match_models), static_cast<unsigned long long>(m.match_frames),
                    100.0 * static_cast<double>(m.match_one) / n, 100.0 * static_cast<double>(m.match_several) / n,
                    100.0 * static_cast<double>(m.match_unmatched) / n);
        std::printf("  of those not found: %.2f%% in polygon RAM (written by the game), %.2f%% the same model under "
                    "other nodes\n",
                    100.0 * static_cast<double>(m.match_unmatched_ram) / static_cast<double>(std::max<uint64_t>(m.match_unmatched, 1)),
                    100.0 * static_cast<double>(m.match_unmatched_path) / static_cast<double>(std::max<uint64_t>(m.match_unmatched, 1)));
    }
    if (m.interp_frames) {
        const double n = static_cast<double>(std::max<uint64_t>(m.interp_models, 1));
        std::printf("Smooth motion: %llu frames, %llu cuts (drawn as the frame); models blended %.2f%%, moved with a relative "
                    "%.2f%%, moved too far %.2f%%; %.3f ms a picture on average, %.3f at most\n",
                    static_cast<unsigned long long>(m.interp_frames), static_cast<unsigned long long>(m.interp_cuts),
                    100.0 * static_cast<double>(m.interp_blended) / n, 100.0 * static_cast<double>(m.interp_followed) / n,
                    100.0 * static_cast<double>(m.interp_moved) / n, 1000.0 * m.interp_seconds / static_cast<double>(m.interp_frames),
                    1000.0 * m.interp_worst);
        const m3::InterpStats& w = m.interp_worst_stats;
        std::printf("  found as the nearest of the same model %.2f%% (ambiguous %.2f%%); the slowest frame %llu: %zu models, "
                    "%zu nearest, %zu compared\n",
                    100.0 * static_cast<double>(m.interp_nearest) / n, 100.0 * static_cast<double>(m.interp_ambiguous) / n,
                    static_cast<unsigned long long>(m.interp_worst_frame), w.models, w.nearest, w.compared);
        std::printf("  the sky's move found in %llu of %llu frames with a sky\n", static_cast<unsigned long long>(m.sky_found),
                    static_cast<unsigned long long>(m.sky_frames));
    }
    std::printf("Textures: uploaded in %llu of %llu frames\n", static_cast<unsigned long long>(m.texture_frames),
                static_cast<unsigned long long>(m.timing.frames()));
    if (m.tilegen_frames)
        std::printf("2D layers: %.3f ms a frame drawing them on average\n",
                    1000.0 * m.tilegen_seconds / static_cast<double>(m.tilegen_frames));
    if (m.wide_frames)
        std::printf("Widescreen sides: %.3f ms a frame making them on average, %.3f at most (frame %llu)\n",
                    1000.0 * m.wide_seconds / static_cast<double>(m.wide_frames), 1000.0 * m.wide_worst,
                    static_cast<unsigned long long>(m.wide_worst_frame));
#ifdef SPIKEOUT_GPU
    if (m.profile_frames) {
        const double n = static_cast<double>(m.profile_frames);
        std::printf("A frame drawn alone: the CPU %.2f ms (packing %.2f, uploading %.2f, recording %.2f), then the GPU %.2f "
                    "(%.2f at most); %.1f passes, %.0f draws\n",
                    m.profile_cpu / n, m.profile_pack / n, m.profile_upload / n, m.profile_record / n, m.profile_gpu / n,
                    m.profile_gpu_worst, static_cast<double>(m.profile_passes) / n, static_cast<double>(m.profile_draws) / n);
    }
    if (m.screen_samples)
        std::printf("Screen pass at %dx%d: %.3f ms widescreen, %.3f ms 4:3 (layers uploaded and shaded, on average over "
                    "%llu screenshots)\n",
                    o.screen_w, o.screen_h, m.screen_wide_ms / static_cast<double>(m.screen_samples),
                    m.screen_43_ms / static_cast<double>(m.screen_samples), static_cast<unsigned long long>(m.screen_samples));
#endif
    if (o.screenshot) {
        if (m.frames_rendered)
            std::printf("Real3D renderer: %.2f ms a frame on average (%.2f setting up, %.2f drawing; %llu frames)\n",
                        1000.0 * m.render_seconds / static_cast<double>(m.frames_rendered),
                        m.setup_ms / static_cast<double>(m.frames_rendered),
                        m.raster_ms / static_cast<double>(m.frames_rendered),
                        static_cast<unsigned long long>(m.frames_rendered));
        if (m.gpu_frames)
            std::printf("GPU renderer: %.2f ms a frame on average, %.2f at most (frame %llu), %llu over 17.4 ms, %s (%llu frames)\n",
                        1000.0 * m.gpu_seconds / static_cast<double>(m.gpu_frames), 1000.0 * m.gpu_worst,
                        static_cast<unsigned long long>(m.gpu_worst_frame), static_cast<unsigned long long>(m.gpu_slow),
                        o.gpu_time_scale ? ("at render scale " + std::to_string(o.gpu_time_scale) + ", waited for").c_str()
                                         : "read back",
                        static_cast<unsigned long long>(m.gpu_frames));
        if (m.gpu_frames && !o.gpu_time_scale)
            std::printf("GPU pictures: hash %016llx (render scale %d)\n", static_cast<unsigned long long>(m.gpu_hash),
                        o.gpu_scale);
        if (m.frames_compared)
            std::printf("GPU against software: %llu of %llu frames differ (a channel more than 2 apart), %llu pixels "
                        "in all, %.1f a frame; the most in frame %llu (%llu pixels); largest difference %d\n",
                        static_cast<unsigned long long>(m.frames_differing), static_cast<unsigned long long>(m.frames_compared),
                        static_cast<unsigned long long>(m.pixels_differing),
                        static_cast<double>(m.pixels_differing) / static_cast<double>(m.frames_compared),
                        static_cast<unsigned long long>(m.worst_frame), static_cast<unsigned long long>(m.worst_pixels),
                        m.max_difference);
        std::printf("  block culling (white, no scene) in %llu frames; spotlight on in %llu; scroll fog in %llu, "
                    "ambient fog in %llu; triangle setups with a microtexture %llu, translator map %llu, inverted "
                    "texture %llu\n",
                    static_cast<unsigned long long>(m.frames_block_culled),
                    static_cast<unsigned long long>(m.frames_spotlit),
                    static_cast<unsigned long long>(m.frames_scroll_fog),
                    static_cast<unsigned long long>(m.frames_ambient_fog), static_cast<unsigned long long>(m.micro_tris),
                    static_cast<unsigned long long>(m.translator_tris), static_cast<unsigned long long>(m.inverted_tris));
        const auto& st = m.real3d.stats();
        std::printf("Real3D (last frame): %u viewports, %u nodes, %u models, %u polygons, %u triangles drawn; "
                    "%llu textures uploaded\n",
                    st.viewports, st.nodes, st.models, st.polygons, st.triangles_drawn,
                    static_cast<unsigned long long>(bus.real3d().textures_uploaded()));
        std::printf("  %u models in polygon RAM, %u hidden nodes, %u nodes too far (LOD), %u outside the view, "
                    "%u models faded out\n",
                    st.models_in_polygon_ram, st.hidden_nodes, st.lod_rejected, st.frustum_culled, st.faded_out);
        std::printf("  configuration registers: %08x %08x %08x %08x\n", bus.real3d().config(0), bus.real3d().config(1),
                    bus.real3d().config(2), bus.real3d().config(3));
        for (const auto& v : m.real3d.viewport_info())
            std::printf("  viewport %06x: priority %d, %gx%g at (%g, %g), eye matrix %u, %u models%s; "
                        "spotlight words %08x %08x %08x %08x %08x\n",
                        v.addr, v.priority, static_cast<double>(v.width), static_cast<double>(v.height),
                        static_cast<double>(v.x), static_cast<double>(v.y), v.eye, v.models, v.disabled ? ", off" : "",
                        v.spot_words[0], v.spot_words[1], v.spot_words[2], v.spot_words[3], v.spot_words[4]);
    }
    std::printf("EEPROM:");
    for (uint32_t i = 0; i < 64; ++i) std::printf("%s%04x", i % 16 == 0 ? "\n  " : " ", bus.eeprom().words()[i]);
    std::printf("\n");
    m3::CommBoard* comm = m.comm;
    if (comm && !comm->trace.empty()) {
        std::printf("\nNetwork board 68000, register accesses:\n");
        for (const auto& a : comm->trace)
            std::printf("  %10llu  pc %06x  %s%u %06x  %04x\n", static_cast<unsigned long long>(a.cycle), a.pc,
                        a.write ? "W" : "R", a.size * 8u, a.addr, a.value);
    }
    if (const Machine::Probe& p = m.probe; p.frames || p.wall_s > 0) { // --rollback-probe
        std::printf("\nRollback probe:");
        if (p.wall_s > 0) std::printf(" frames run at %.1f a second (wall clock, board 0);", static_cast<double>(o.rollback_frames) / p.wall_s);
        if (p.frames)
            std::printf(" state %zu KB; saved in %.3f ms (worst %.3f), copied in %.3f ms; changed a frame %.0f KB (worst %.0f KB) "
                        "in 4 KB pages; over %llu frames",
                        p.size / 1024, p.save_ms / static_cast<double>(p.frames + 1), p.save_ms_worst,
                        p.copy_ms / static_cast<double>(p.frames + 1), p.changed / static_cast<double>(p.frames) / 1024,
                        p.changed_worst / 1024, static_cast<unsigned long long>(p.frames));
        std::printf("\n");
    }
    if (const Machine::RollbackTest& t = m.rb; t.snaps) { // --rollback-test
        const double saves = static_cast<double>(t.saves > 1 ? t.saves - 1 : 1);
        std::printf("\nRollback test: %llu frames run again and checked, %llu differed%s; %llu times back %llu frames, "
                    "%llu with the big buffers back exactly\n",
                    static_cast<unsigned long long>(t.checked), static_cast<unsigned long long>(t.differed),
                    t.differed ? (" (the first at frame " + std::to_string(t.first_differ) + ")").c_str() : "",
                    static_cast<unsigned long long>(t.restores), static_cast<unsigned long long>(o.rb_depth),
                    static_cast<unsigned long long>(t.restored_exact));
        std::printf("Rollback snapshots (%s): small state %zu KB, big buffers %zu KB; a frame %.3f ms (worst %.3f), "
                    "%.0f pages (worst %zu) of 4 KB; going back %.3f ms (worst %.3f); %zu pages written by loading\n",
                    t.snaps->watched() ? m3::watched::name(m3::watched::method()) : "compared", t.snaps->small_bytes() / 1024,
                    t.snaps->big_bytes() / 1024,
                    t.save_ms / saves, t.save_worst, static_cast<double>(t.pages) / saves, t.pages_worst,
                    t.restores ? t.restore_ms / static_cast<double>(t.restores) : 0.0, t.restore_worst, t.snaps->load_writes());
        if (t.rerun_frames)
            std::printf("Rollback re-runs%s: %llu frames run again at %.1f a second (wall clock, from going back)\n",
                        o.rb_quiet ? " (drawing and playing nothing)" : "", static_cast<unsigned long long>(t.rerun_frames),
                        static_cast<double>(t.rerun_frames) / t.rerun_s);
    }
    if (comm && !comm->host_trace.empty()) {
        static const char* const kRegions[] = {"comm", "io", "netram", "netram"};
        std::printf("\nNetwork board, the main board's accesses (68000 cycles):\n");
        for (const auto& r : comm->host_trace)
            std::printf("  %10llu  host %s %s %05x-%05x x%u bank %u, to %llu%s\n", static_cast<unsigned long long>(r.first),
                        r.write ? "W" : "R", kRegions[r.region & 3], r.lo, r.hi, r.count, r.bank,
                        static_cast<unsigned long long>(r.last), r.region == 1 && r.write ? (" = " + std::to_string(r.value)).c_str() : "");
    }
    if (comm && o.net_disasm && m.index == 0) {
        if (std::FILE* f = std::fopen((std::string(o.net_disasm) + ".bin").c_str(), "wb")) { // raw 68000 RAM
            std::fwrite(comm->ram().data(), 1, comm->ram().size(), f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen(o.net_disasm, "w")) {
            const auto ram = comm->ram();
            for (uint32_t addr = 0; addr < 0x10000;) {
                uint32_t len = 2;
                const std::string text = comm->disassemble(addr, &len);
                std::fprintf(f, "%06x  %02x%02x  %s\n", addr, ram[addr], ram[addr + 1], text.c_str());
                addr += std::max<uint32_t>(len, 2);
            }
            std::fclose(f);
        }
    }

    std::printf("\nLast blocks:\n");
    for (uint32_t pc : c.recent_blocks())
        if (pc) {
            const uint32_t raw = bus.read32(pc);
            std::printf("  %08x  %s\n", pc, ppc::disassemble(ppc::decode(raw), pc).c_str());
        }

    if (o.show_text) {
        // The four layers' name tables: 64 x 64 entries of 16 bits from
        // 0xF10F8000, 0x2000 bytes apart. The tile generator is
        // little-endian, so the two entries in each CPU word are swapped.
        // Spikeout's font puts ASCII at tile numbers 0x20-0x7E.
        const auto tile = bus.tile_ram();
        std::printf("\nTilegen registers (as the tile generator sees them):\n ");
        for (uint32_t reg : {0x00u, 0x08u, 0x0Cu, 0x20u, 0x40u, 0x44u, 0x60u, 0x64u, 0x68u, 0x6Cu})
            std::printf(" %02x=%08x", reg, bus.tile_reg(reg));
        std::printf("\n\nText on the tile layers:\n");
        for (uint32_t layer = 0; layer < 4; ++layer) {
            for (uint32_t row = 0; row < 64; ++row) {
                std::string line;
                for (uint32_t col = 0; col < 64; ++col) {
                    const uint32_t off = 0xF8000 + layer * 0x2000 + row * 128 + (col ^ 1) * 2;
                    const uint32_t entry = uint32_t{tile[off]} << 8 | tile[off + 1];
                    const uint32_t code = entry >> 8;
                    line += (entry & 0xFF) == 0 && code >= 0x20 && code < 0x7F ? static_cast<char>(code) : '.';
                }
                if (line.find_first_not_of(" .") != std::string::npos)
                    std::printf("  layer %u row %2u: %s\n", layer, row, line.c_str());
            }
        }
    }

    if (o.screenshot) {
        const std::string path = board_path(o.screenshot, m.index);
        if (tools::write_png(path, m.screen, m3::TilegenRenderer::kWidth, m3::TilegenRenderer::kHeight))
            std::printf("\nScreen (last finished frame) written to %s\n", path.c_str());
        else
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
    }

    if (o.dump_tile) {
        const std::string path = board_path(o.dump_tile, m.index);
        std::FILE* f = std::fopen(path.c_str(), "wb");
        const auto tile = bus.tile_ram();
        if (!f || std::fwrite(tile.data(), 1, tile.size(), f) != tile.size())
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
        if (f) std::fclose(f);
    }

    if (o.dump_backup) {
        const std::string path = board_path(o.dump_backup, m.index);
        std::FILE* f = std::fopen(path.c_str(), "wb");
        const auto ram = bus.backup_ram();
        if (!f || std::fwrite(ram.data(), 1, ram.size(), f) != ram.size())
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
        if (f) std::fclose(f);
    }

    if (o.dump_textures) {
        const std::string path = board_path(o.dump_textures, m.index);
        const auto ram = bus.real3d().texture_ram();
        std::vector<uint32_t> picture(ram.size());
        for (std::size_t i = 0; i < ram.size(); ++i) {
            const uint32_t t = ram[i];
            auto c5 = [](uint32_t v) { return (v << 3) | (v >> 2); };
            picture[i] = 0xFF000000u | c5((t >> 10) & 0x1F) << 16 | c5((t >> 5) & 0x1F) << 8 | c5(t & 0x1F);
        }
        if (!tools::write_png(path, picture, 2048, static_cast<int>(ram.size() / 2048)))
            std::fprintf(stderr, "gamerun: cannot write %s\n", path.c_str());
    }

    std::printf("\nDevices accessed that aren't emulated yet:\n");
    if (bus.unhandled().empty()) std::printf("  none\n");
    for (const auto& [region, u] : bus.unhandled())
        std::printf("  %-16.*s %llu reads, %llu writes (first at %08x)\n", int(region.size()), region.data(),
                    static_cast<unsigned long long>(u.reads), static_cast<unsigned long long>(u.writes),
                    u.first_addr);
}

} // namespace

int main(int argc_given, char** argv_given) {
    // (tests: GAMERUN_FLUSH=1 writes what it said out every 2 s, for a run that stops answering: what it said last;
    // a line at a time would be thousands of small writes a second)
    if (std::getenv("GAMERUN_FLUSH"))
        std::thread([] {
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(2)), std::fflush(stdout);
        }).detach();
    // (an argument @FILE: the file's words in its place: a long --press list, Windows' command line being 32 KB)
    std::deque<std::string> words; // (a deque: its strings stay where they are as it grows)
    std::vector<char*> args;
    for (int k = 0; k < argc_given; ++k) {
        if (argv_given[k][0] != '@' || k == 0) {
            args.push_back(argv_given[k]);
            continue;
        }
        std::ifstream in(argv_given[k] + 1);
        if (!in) {
            std::fprintf(stderr, "gamerun: can't read %s\n", argv_given[k] + 1);
            return 2;
        }
        for (std::string w; in >> w;) args.push_back(words.emplace_back(std::move(w)).data());
    }
    args.push_back(nullptr);
    const int argc = static_cast<int>(args.size()) - 1;
    char** argv = args.data();
    Options o;
    tools::Recording recording;
    tools::InputLog input_log; // (--inputs)
    std::map<tools::LinkedInputLog::Where, tools::LinkedInputLog::Change> linked_inputs; // (--inputs, linked boards')
    bool blocks_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--blocks" && i + 1 < argc) o.blocks = std::strtoll(argv[++i], nullptr, 0), blocks_given = true;
        else if (arg == "--inputs" && i + 1 < argc) { // FILE (spikebot --record-inputs; with --load-state, the state it began from)
            if (input_log.load(argv[++i])) {
                o.input_log = &input_log;
            } else if (tools::LinkedInputLog log; log.load(argv[i])) { // (linked boards')
                linked_inputs = log.by_where();
                for (const auto& [where, c] : linked_inputs) o.linked_inputs_end = std::max(o.linked_inputs_end, where.frame);
                o.linked_inputs = &linked_inputs;
            } else {
                std::fprintf(stderr, "gamerun: %s: not a recorded run (spikebot --record-inputs)\n", argv[i]);
                return 1;
            }
        }
        else if (arg == "--replay" && i + 1 < argc) {
            std::string error;
            if (!recording.load(argv[++i], error)) {
                std::fprintf(stderr, "gamerun: %s\n", error.c_str());
                return 1;
            }
            o.replay = &recording;
        }
        else if (arg == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const size_t at = spec.find_last_of('@');
            if (at == std::string::npos) {
                std::fprintf(stderr, "gamerun: --save-state FILE@FRAME\n");
                return 1;
            }
            o.save_state = spec.substr(0, at);
            o.save_frame = std::strtoull(spec.c_str() + at + 1, nullptr, 0);
        }
        else if (arg == "--load-state" && i + 1 < argc) o.load_state = argv[++i];
        else if (arg == "--watch-write" && i + 1 < argc) { // LO-HI@FRAME[:N], hex addresses
            const std::string spec = argv[++i];
            char* end = nullptr;
            o.watch_lo = static_cast<uint32_t>(std::strtoul(spec.c_str(), &end, 16));
            if (*end == '-') o.watch_hi = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
            if (*end == '@') o.watch_frame = std::strtoull(end + 1, &end, 0);
            if (*end == ':') o.watch_frames = std::strtoull(end + 1, &end, 0);
        }
        else if (arg == "--rollback-test" && i + 1 < argc) { // FRAME:N[:DEPTH[:EVERY]]
            char* end = nullptr;
            o.rb_from = std::strtoull(argv[++i], &end, 0);
            o.rb_frames = *end == ':' ? std::strtoull(end + 1, &end, 0) : 600;
            if (*end == ':') o.rb_depth = std::strtoull(end + 1, &end, 0);
            if (*end == ':') o.rb_every = std::strtoull(end + 1, &end, 0);
            while (*end == ':') {
                char* word = end + 1;
                end = std::strchr(word, ':');
                if (!end) end = word + std::strlen(word);
                const std::string w(word, end);
                if (w == "compare") o.rb_compare = true;
                else if (w == "quiet") o.rb_quiet = true;
                else if (w == "timed") o.rb_timed = true;
                else {
                    std::fprintf(stderr, "gamerun: --rollback-test: unknown %s\n", w.c_str());
                    return 2;
                }
            }
            if (o.rb_every == 0) o.rb_every = 1;
        }
        else if (arg == "--rollback-probe" && i + 1 < argc) { // FRAME:N
            char* end = nullptr;
            o.rollback_from = std::strtoull(argv[++i], &end, 0);
            o.rollback_frames = *end == ':' ? std::strtoull(end + 1, &end, 0) : 600;
            o.rollback_speed_only = std::string(end) == ":speed";
        }
        else if (arg == "--net-host-trace" && i + 1 < argc) { // FRAME:N
            char* end = nullptr;
            o.host_trace_frame = std::strtoull(argv[++i], &end, 0);
            o.host_trace_frames = *end == ':' ? std::strtoull(end + 1, nullptr, 0) : 1;
        }
        else if (arg == "--dump-tile" && i + 1 < argc) o.dump_tile = argv[++i];
        else if (arg == "--dump-textures" && i + 1 < argc) o.dump_textures = argv[++i];
        else if (arg == "--dump-backup" && i + 1 < argc) o.dump_backup = argv[++i];
        else if (arg == "--dump-layers") o.dump_layers = true;
        else if (arg == "--backup" && i + 1 < argc) {
            int board = kAllBoards;
            const std::string path = board_prefix(argv[++i], board);
            o.backup_files.push_back({board, path});
        }
        else if (arg == "--eeprom" && i + 1 < argc) {
            int board = kAllBoards;
            const std::string path = board_prefix(argv[++i], board);
            o.eeprom_files.push_back({board, path});
        }
        else if (arg == "--clock" && i + 1 < argc) o.clock = std::strtoll(argv[++i], nullptr, 0);
        else if (arg == "--net-host" && i + 1 < argc) o.net_host = argv[++i];
        else if (arg == "--net-join" && i + 1 < argc) o.net_join = argv[++i];
        else if (arg == "--net-spectate") o.net_spectate = true;
        else if (arg == "--pace") o.pace = true;
        else if (arg == "--net-spectate-seat" && i + 1 < argc) o.net_spectate_seat = static_cast<uint8_t>(std::atoi(argv[++i]));
        else if (arg == "--lobby" && i + 1 < argc) o.lobby_url = argv[++i];
        else if (arg == "--private") o.net_private = true;
        else if (arg == "--net-start-alone") o.net_start_alone = true;
        else if (arg == "--net-catch-up") o.net_catch_up = true;
        else if (arg == "--net-one-board" && i + 1 < argc) o.net_one_board = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--net-leave-at" && i + 1 < argc) o.net_leave_at = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--net-vanish-at" && i + 1 < argc) o.net_vanish_at = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--net-rollback" && i + 1 < argc) o.net_rollback = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--net-rollback-history" && i + 1 < argc) {
            const uint64_t n = std::strtoull(argv[++i], nullptr, 0);
            o.net_rollback_history = n ? std::optional<uint64_t>(n) : std::nullopt;
        }
        else if (arg == "--net-hold-inputs" && i + 1 < argc) o.net_hold_inputs = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--no-frame-hashes") o.no_frame_hashes = true;
        else if (arg == "--no-idle-skip") m3::games::set_idle_skip(false), m3::CommBoard::set_wait_skip(false); // (the vblank wait and the network board's waits spun, as before)
        else if (arg == "--no-net-idle-skip") m3::CommBoard::set_wait_skip(false); // (the network board's only)
        else if (arg == "--frame-hashes") o.frame_hashes = true;
        else if (arg == "--frames" && i + 1 < argc) o.max_frames = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--seats" && i + 1 < argc)
            o.net_seats = static_cast<uint8_t>(std::clamp<unsigned long>(std::strtoul(argv[++i], nullptr, 0), 2, net::kMaxSeats));
        else if (arg == "--text") o.show_text = true;
        else if (arg == "--netboard" && i + 1 < argc) o.real_netboard = std::string(argv[++i]) == "real";
        else if (arg == "--net-trace" && i + 1 < argc) o.net_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--music-hash" && i + 1 < argc) o.music_hash = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--net-disasm" && i + 1 < argc) o.net_disasm = argv[++i];
        else if (arg == "--net-force-master") o.force_master = true;
        else if (arg == "--net-send-waits" && i + 1 < argc) o.send_waits_for_receive = std::string(argv[++i]) != "off";
        else if (arg == "--net-watch") o.net_watch = true;
        else if (arg == "--net-other-bank") o.host_sees_other_bank = true;
        else if (arg == "--net-e0-irq5" && i + 1 < argc) o.irq5_on_e0 = std::string(argv[++i]) != "off";
        else if (arg == "--net-irq5-per-frame" && i + 1 < argc) o.irq5_per_frame = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--net-ring-forwarding" && i + 1 < argc) o.ring_forwarding = std::string(argv[++i]) != "off";
        else if (arg == "--net-double-buffer" && i + 1 < argc) o.double_buffer = std::string(argv[++i]) != "off";
        else if (arg == "--net-old-link") o.old_link = true;
        else if (arg == "--net-irq5-phase" && i + 1 < argc) o.irq5_phase = std::strtod(argv[++i], nullptr);
        else if (arg == "--net-slave-irq5-per-frame" && i + 1 < argc) o.slave_irq5_per_frame = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (arg == "--link-byte-cycles" && i + 1 < argc) o.link_cycles_per_byte = std::strtod(argv[++i], nullptr);
        else if (arg == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (arg == "--wav" && i + 1 < argc) o.wav = argv[++i];
        else if (arg == "--quads") o.quads = true;
        else if (arg == "--fog-over-nothing") o.fog_over_nothing = true;
        else if (arg == "--gpu-time" && i + 1 < argc) o.gpu_time_scale = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (arg == "--gpu-scale" && i + 1 < argc) o.gpu_scale = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (arg == "--wide" && i + 1 < argc) o.wide = std::clamp(std::atoi(argv[++i]), 0, 400);
        else if (arg == "--match-models") o.match_models = true;
        else if (arg == "--gpu-slow" && i + 1 < argc) o.gpu_profile_over = std::atof(argv[++i]);
        else if (arg == "--gpu-hash-frames") o.gpu_hash_frames = true;
        else if (arg == "--gpu-full" && i + 1 < argc) o.gpu_full_frame = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--interp") o.interp = o.match_models = true;
        else if (arg == "--interp-trace" && i + 1 < argc) {
            const std::string range = argv[++i];
            const size_t colon = range.find(':');
            o.interp = o.match_models = o.interp_trace = true;
            o.interp_trace_from = std::strtoull(range.c_str(), nullptr, 0);
            o.interp_trace_to = colon == std::string::npos ? o.interp_trace_from + 1
                                                           : std::strtoull(range.c_str() + colon + 1, nullptr, 0);
        }
        else if (arg == "--screen-time" && i + 1 < argc) {
            const std::string size = argv[++i];
            const size_t x = size.find('x');
            if (x != std::string::npos) {
                o.screen_w = std::clamp(std::atoi(size.substr(0, x).c_str()), 0, 8192);
                o.screen_h = std::clamp(std::atoi(size.substr(x + 1).c_str()), 0, 8192);
            }
        }
        else if (arg == "--renderer" && i + 1 < argc) {
            const std::string r = argv[++i];
            o.renderer = r == "gpu" ? Options::Renderer::Gpu : r == "compare" ? Options::Renderer::Compare
                                                                             : Options::Renderer::Software;
        }
        else if (arg == "--midi-burst") o.midi_burst = true;
        else if (arg == "--dma-instant") o.dma_instant = true;
        else if (arg == "--sound-volume" && i + 1 < argc) o.sound_volume = std::atoi(argv[++i]);
        else if (arg == "--music-volume" && i + 1 < argc) o.music_volume = std::atoi(argv[++i]);
        else if (arg == "--shot-every" && i + 1 < argc) o.shot_every = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--boards" && i + 1 < argc) o.boards = std::max<std::size_t>(1, std::strtoull(argv[++i], nullptr, 0));
        else if (arg == "--slice" && i + 1 < argc) o.slice = std::max<uint32_t>(1, static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
        else if (arg == "--link-latency" && i + 1 < argc) o.link_latency = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--input-delay" && i + 1 < argc) { // N, or auto (network play: tools::DelayTuner, as the window)
            const std::string v = argv[++i];
            g_input_delay_auto = v == "auto";
            o.input_delay = g_input_delay_auto ? 0 : std::strtoull(v.c_str(), nullptr, 0);
        }
        else if (arg == "--parallel") o.parallel = true;
        else if (arg == "--no-2d") o.no_2d = true;
        else if (arg == "--no-sound") o.no_sound = true;
        else if (arg == "--link-trace" && i + 1 < argc) o.link_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--link-dump" && i + 1 < argc) o.link_dump = argv[++i];
        else if (arg == "--link-standin-at" && i + 1 < argc) o.link_standin_at = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--link-standin-patch" && i + 1 < argc) o.link_standin_patch = argv[++i];
        else if (arg == "--camera-trace" && i + 1 < argc) o.camera_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--camera-test-from" && i + 1 < argc) o.camera_test_from = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--camera-test-flags") o.camera_test_flags = true;
        else if (arg == "--camera-test-mirror") o.camera_test_mirror = true;
        else if (arg == "--camera-test-free") o.camera_test_free = true;
        else if (arg == "--link-standin-no-reads") o.link_standin_no_reads = true;
        else if (arg == "--link-standin-coarse") o.link_standin_coarse = true;
        else if (arg == "--link-standin-blocks") o.link_standin_blocks = true;
        else if (arg == "--clone-seat" && i + 1 < argc) { // B@FRAME: board B made from board 0's state at FRAME
            char* end = nullptr;
            o.clone_board = static_cast<int>(std::strtol(argv[++i], &end, 0));
            if (*end == '@') o.clone_frame = std::strtoull(end + 1, &end, 0);
        }
        else if (arg == "--wait-seat" && i + 1 < argc) { // B@FROM:UNTIL: board B stopped at FROM, back at UNTIL
            char* end = nullptr;
            o.wait_board = static_cast<int>(std::strtol(argv[++i], &end, 0));
            if (*end == '@') o.wait_from = std::strtoull(end + 1, &end, 0);
            if (*end == ':') o.wait_until = std::strtoull(end + 1, &end, 0);
        }
        else if (arg == "--drop" && i + 1 < argc) { // B@FRAME: board B's PC gone at the end of FRAME
            char* end = nullptr;
            o.drop_board = static_cast<int>(std::strtol(argv[++i], &end, 0));
            g_drop_board = o.drop_board;
            if (*end == '@') o.drop_frame = std::strtoull(end + 1, &end, 0);
        }
        else if (arg == "--drop-buttons" && i + 1 < argc) o.drop_buttons = g_drop_buttons = argv[++i]; // DUMP (a --link-dump)
        else if (arg == "--one-board" && i + 1 < argc) { // B@FRAME: board B made by the other from the end of FRAME
            char* end = nullptr;
            const int board = static_cast<int>(std::strtol(argv[++i], &end, 0));
            if (o.one_board < 0) o.one_board = board;
            else o.one_board_also.push_back(static_cast<std::size_t>(board));
            if (*end == '@') o.one_board_frame = std::strtoull(end + 1, &end, 0);
        }
        else if (arg == "--one-board-camera" && i + 1 < argc) o.one_board_camera = argv[++i]; // FILE (a --camera-record of B)
        else if (arg == "--one-board-miss" && i + 1 < argc) o.one_board_missed.push_back(std::strtoull(argv[++i], nullptr, 0));
        else if (arg == "--one-board-shadow" && i + 1 < argc) o.one_board_shadow = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--one-board-tasks") o.one_board_tasks = true;
        else if (arg == "--one-board-auto") o.one_board_auto = o.one_board_tasks = true;
        else if (arg == "--one-board-lookahead" && i + 1 < argc) o.one_board_lookahead = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--one-board-hand-back" && i + 1 < argc) o.one_board_hand_back = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--one-board-held") o.one_board_held = true;
        else if (arg == "--one-board-stays" && i + 1 < argc) o.one_board_stays = std::atoi(argv[++i]);
        else if (arg == "--one-board-late") o.one_board_late = true;
        else if (arg == "--one-board-whole" && i + 1 < argc) o.one_board_whole = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--one-board-local-shadows") o.one_board_local_shadows = true;
        else if (arg == "--camera-events") o.camera_events = true;
        else if (arg == "--script-actions") o.script_actions = true;
        else if (arg == "--map-trace" && i + 1 < argc) { // FROM:N
            char* end = nullptr;
            o.map_trace_from = std::strtoull(argv[++i], &end, 0);
            o.map_trace_frames = *end == ':' ? std::strtoull(end + 1, nullptr, 0) : 1;
        }
        else if ((arg == "--link-record" || arg == "--link-standin" || arg == "--camera-record" || arg == "--camera-test") &&
                 i + 1 < argc) { // B:FILE
            char* end = nullptr;
            const int board = static_cast<int>(std::strtol(argv[++i], &end, 0));
            if (*end != ':') {
                std::fprintf(stderr, "gamerun: %s B:FILE\n", arg.c_str());
                return 2;
            }
            if (arg == "--link-record") o.link_record = end + 1, o.link_record_board = board;
            else if (arg == "--link-standin") o.link_standin = end + 1, o.link_standin_board = board;
            else if (arg == "--camera-record") o.camera_record = end + 1, o.camera_record_board = board;
            else o.camera_test = end + 1, o.camera_test_board = board;
        }
        else if (arg == "--probe" && i + 1 < argc) { // X,Y@FRAME
            unsigned long long frame = 0;
            if (std::sscanf(argv[++i], "%d,%d@%llu", &o.probe_x, &o.probe_y, &frame) == 3) o.probe_frame = frame;
        }
        else if (arg == "--tilegen-trace" && i + 1 < argc) o.tilegen_trace = std::strtoull(argv[++i], nullptr, 0);
        else if (arg == "--poke" && i + 1 < argc) {
            Poke k;
            const std::string spec = board_prefix(argv[++i], k.board);
            unsigned addr = 0, value = 0;
            unsigned long long frame = 0, frames = 1;
            const int n = std::sscanf(spec.c_str(), "%x=%x@%llu:%llu", &addr, &value, &frame, &frames);
            if (n < 3) {
                std::fprintf(stderr, "gamerun: --poke [B:]ADDR=VALUE@FRAME[:N] (hex address and value)\n");
                return 2;
            }
            k.addr = addr;
            k.value = static_cast<uint8_t>(value);
            k.frame = frame;
            k.frames = n == 4 ? frames : 1;
            o.pokes.push_back(k);
        }
        else if (arg == "--ram-dump" && i + 2 < argc) { // B:LO-HI@FRAME:N FILE
            unsigned board = 0, lo = 0, hi = 0;
            unsigned long long frame = 0, frames = 0;
            if (std::sscanf(argv[++i], "%u:%x-%x@%llu:%llu", &board, &lo, &hi, &frame, &frames) != 5 || hi <= lo ||
                hi > m3::kWorkRamSize) {
                std::fprintf(stderr, "gamerun: --ram-dump B:LO-HI@FRAME:N FILE (hex addresses in main RAM)\n");
                return 2;
            }
            o.ram_dump_board = static_cast<int>(board), o.ram_dump_lo = lo, o.ram_dump_hi = hi;
            o.ram_dump_from = frame, o.ram_dump_frames = frames, o.ram_dump = argv[++i];
        }
        else if (arg == "--ram-hashes" && i + 2 < argc) { // B@FRAME FILE
            unsigned board = 0;
            unsigned long long frame = 0;
            if (std::sscanf(argv[++i], "%u@%llu", &board, &frame) != 2) {
                std::fprintf(stderr, "gamerun: --ram-hashes B@FRAME FILE\n");
                return 2;
            }
            o.ram_hashes_board = static_cast<int>(board), o.ram_hashes_from = frame, o.ram_hashes = argv[++i];
        }
        else if (arg == "--peek" && i + 1 < argc) { // [B:]ADDR:LENGTH@FRAME[:N], hex address and length
            Peek k;
            const std::string spec = board_prefix(argv[++i], k.board);
            unsigned addr = 0, length = 4;
            unsigned long long frame = 0, frames = 1;
            const int n = std::sscanf(spec.c_str(), "%x:%x@%llu:%llu", &addr, &length, &frame, &frames);
            if (n < 3) {
                std::fprintf(stderr, "gamerun: --peek [B:]ADDR:LENGTH@FRAME[:N] (hex address and length)\n");
                return 2;
            }
            k.addr = addr, k.length = length, k.frame = frame, k.frames = n == 4 ? frames : 1;
            o.peeks.push_back(k);
        }
        else if (arg == "--press" && i + 1 < argc) {
            // [B:]NAME@FRAME[:N]: hold NAME for N frames (default 6) from FRAME.
            Press p;
            const std::string spec = board_prefix(argv[++i], p.board);
            const size_t at = spec.find('@'), colon = spec.find(':', at == std::string::npos ? 0 : at);
            p.name = spec.substr(0, at);
            if (at != std::string::npos) p.frame = std::strtoull(spec.c_str() + at + 1, nullptr, 0);
            if (colon != std::string::npos) p.frames = std::strtoull(spec.c_str() + colon + 1, nullptr, 0);
            if (!input_bit(p.name, p.bank, p.bit)) {
                std::fprintf(stderr, "gamerun: unknown input '%s'\n", p.name.c_str());
                return 2;
            }
            o.presses.push_back(p);
            g_presses = &o.presses;
        }
        else if (arg == "--bot" && i + 1 < argc) { // SEED[@FROM]: a player pressing all the time, as real players do
            // (random: moves held 10-44 frames, beats, jumps, charges, shifts; coin and start every 3000 frames, so it
            // plays on after a game over), on every board (network play: only this machine's own is sent), from FROM
            // (default 1200) for 150,000 frames (43 minutes). The same SEED, the same presses.
            const std::string spec = argv[++i];
            const std::size_t at = spec.find('@');
            const uint32_t seed = static_cast<uint32_t>(std::strtoul(spec.c_str(), nullptr, 0));
            const uint64_t from = at == std::string::npos ? 1200 : std::strtoull(spec.c_str() + at + 1, nullptr, 0);
            auto add = [&](int board, const char* name, uint64_t frame, uint64_t frames) {
                Press p;
                p.name = name, p.frame = frame, p.frames = frames, p.board = board;
                input_bit(p.name, p.bank, p.bit);
                o.presses.push_back(p);
            };
            for (int board = 0; board < 4; ++board) {
                std::mt19937 r(seed * 4u + static_cast<uint32_t>(board));
                auto n = [&](uint32_t k) { return static_cast<uint64_t>(r() % k); };
                static const char* const dirs[] = {"up", "down", "left", "right"};
                for (uint64_t f = from; f < from + 150000;) {
                    if (f % 3000 < 30) {
                        add(board, "coin", f, 6), add(board, "start", f + 60, 6);
                        f += 120;
                        continue;
                    }
                    switch (n(10)) {
                    case 0: case 1: case 2: case 3: add(board, dirs[n(4)], f, 10 + n(35)); f += 8 + n(20); break;
                    case 4: case 5: case 6: add(board, "beat", f, 2 + n(5)); f += 10 + n(15); break;
                    case 7: add(board, "jump", f, 3 + n(4)); f += 20 + n(20); break;
                    case 8: add(board, "charge", f, 20 + n(30)); f += 40 + n(20); break;
                    default: add(board, "shift", f, 4); f += 15 + n(15); break;
                    }
                }
            }
            g_presses = &o.presses;
        }
        else if (arg == "--presses" && i + 1 < argc) { // FILE: --press specs, one a line ("--press " before each or not):
                                                       // a player pressing all the time (bots), past a command line's length
            std::ifstream in(argv[++i]);
            if (!in) {
                std::fprintf(stderr, "gamerun: can't read %s\n", argv[i]);
                return 2;
            }
            for (std::string line; std::getline(in, line);) {
                if (const auto p = line.find("--press "); p != std::string::npos) line = line.substr(p + 8);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                Press p;
                const std::string spec = board_prefix(line.c_str(), p.board);
                const size_t at = spec.find('@'), colon = spec.find(':', at == std::string::npos ? 0 : at);
                p.name = spec.substr(0, at);
                if (at != std::string::npos) p.frame = std::strtoull(spec.c_str() + at + 1, nullptr, 0);
                if (colon != std::string::npos) p.frames = std::strtoull(spec.c_str() + colon + 1, nullptr, 0);
                if (!input_bit(p.name, p.bank, p.bit)) {
                    std::fprintf(stderr, "gamerun: unknown input '%s' in %s\n", p.name.c_str(), argv[i]);
                    return 2;
                }
                o.presses.push_back(p);
            }
            g_presses = &o.presses;
        }
        else if (!o.dir) o.dir = argv[i];
        else o.dir = nullptr, i = argc;
    }
    g_input_delay = o.input_delay;
    if (!o.dir) {
        std::fprintf(stderr, "usage: gamerun <rom_dir> [--blocks N] [--text] [--dump-tile FILE]\n"
                             "               [--netboard sim|real] [--net-trace N] [--net-disasm FILE]\n"
                             "               [--screenshot FILE.png] [--shot-every N] [--wav FILE.wav]\n"
                             "               [--probe X,Y@FRAME] [--quads] [--midi-burst] [--dma-instant]\n"
                             "               [--sound-volume N] [--music-volume N]\n"
                             "               [--boards N] [--slice LINES] [--link-latency CYCLES] [--input-delay N]\n"
                             "               [--press [B:]NAME@FRAME[:N]]... [--poke [B:]ADDR=VALUE@FRAME[:N]]...\n");
        return 2;
    }
    if (o.replay) {
        if (!blocks_given) o.blocks = INT64_MAX / 2; // (to the recording's end)
        std::printf("replay: %zu frames of inputs, %zu hashes\n", o.replay->inputs.size(), o.replay->hashes.size());
    }
    const m3::GameSpec& game = m3::games::kSpikeoutFinalEdition;
    const m3::RomSet roms = m3::load_rom_set(game, o.dir);
    g_roms = &roms;
    if (!roms.complete()) {
        std::fprintf(stderr, "gamerun: ROM set incomplete; run romcheck for details\n");
        return 1;
    }
#ifdef SPIKEOUT_JIT
    try {
        jit::prepare(game, roms);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "gamerun: %s\n", e.what());
        return 1;
    }
#endif
#ifdef SPIKEOUT_INTERP
    try {
        interp::prepare(game, roms);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "gamerun: %s\n", e.what());
        return 1;
    }
#endif

    const std::vector<recomp::FunctionEntry> functions = tools::game_functions();
    const std::vector<recomp::FunctionEntry> blocks = tools::game_blocks(); // (save states)
    for (const auto& [board, path] : o.backup_files) { // --backup [B:]FILE
        std::vector<uint8_t> bytes = tools::read_file(path);
        if (bytes.size() != m3::Bus::kBackupRamSize) {
            std::fprintf(stderr, "gamerun: %s isn't a backup RAM (%u bytes)\n", path.c_str(), m3::Bus::kBackupRamSize);
            return 1;
        }
        for (std::size_t b = 0; b < o.board_backup.size(); ++b)
            if (board == kAllBoards || static_cast<std::size_t>(board) == b) o.board_backup[b] = bytes;
    }
    for (const auto& [board, path] : o.eeprom_files) { // --eeprom [B:]FILE
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> bytes(128, 0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), 128)) {
            std::fprintf(stderr, "gamerun: cannot read %s (128 bytes)\n", path.c_str());
            return 1;
        }
        if (board == kAllBoards) o.eeprom_bytes = bytes;
        else if (board >= 0 && static_cast<std::size_t>(board) < o.board_eeprom.size()) o.board_eeprom[static_cast<std::size_t>(board)] = bytes;
    }

    // Network play: the game found or offered on the networks this machine
    // is on, every board run here, the boards as the host started them.
    if (!o.one_board_also.empty() &&
        (o.one_board_camera || !o.one_board_missed.empty() || o.one_board_shadow || o.one_board_auto || o.one_board_hand_back ||
         o.one_board_held)) {
        std::fprintf(stderr, "gamerun: with more than one board going, side runs only (no camera, missed frames, shadows or rules)\n");
        return 2;
    }
    const bool networked = !o.net_host.empty() || !o.net_join.empty();
    if (o.rb_frames && (networked || (o.boards > 1 && !o.parallel) || !o.save_state.empty() || o.load_state)) {
        std::fprintf(stderr, "gamerun: --rollback-test is for one board or linked boards run at once (--parallel), "
                             "not with network play or save states\n");
        return 2;
    }
    if (o.linked_inputs && (o.boards < 2 || !o.parallel || o.slice != 8 || !o.load_state)) {
        std::fprintf(stderr, "gamerun: these inputs are linked boards' (spikebot --boards N): with --boards N --parallel "
                             "--load-state FILE, the default slice\n");
        return 2;
    }
    tools::InputTable inputs(networked ? net::kMaxSeats : o.boards);
    std::unique_ptr<tools::NetPlay> netplay;
    std::jthread spectate_end; // (a spectator: its game ended, the input table closed: stopped before netplay goes)
    std::vector<uint8_t> join_state; // (a game joined in progress: every board's state, unpacked)
    const m3::CommBoardOptions link = link_options(o);
    net::LobbyClient lobby_client; // (the whole run: a room taken over is listed again, below)
    bool on_lobby = false;
    if (networked) {
        if (o.replay || !o.save_state.empty()) {
            std::fprintf(stderr, "gamerun: network play starts from power-on or a linked state (no --replay or --save-state)\n");
            return 2;
        }
        if (o.net_one_board && (o.one_board >= 0 || o.one_board_shadow || o.one_board_camera)) {
            std::fprintf(stderr, "gamerun: --net-one-board hands every other board over itself (of the --one-board options, --one-board-auto "
                                 "only)\n");
            return 2;
        }
        // (--load-state: every machine starts from the same linked state, its own copy; the state is part of the
        // build, so machines with different ones can't join each other)
        uint64_t build = tools::build_id(roms, functions.size(), o.real_netboard ? &link : nullptr);
        if (o.load_state) {
            const std::vector<uint8_t> bytes = tools::read_file(o.load_state);
            build = (build ^ tools::Recording::ram_hash(bytes)) * 0x100000001B3ull;
        }
        netplay = std::make_unique<tools::NetPlay>(inputs, build,
                                                   [](const std::string& s) { std::printf("net: %s\n", s.c_str()), std::fflush(stdout); });
        net::Session& session = netplay->session();
        session.set_cabinet(o.eeprom_bytes, {}); // (this process's cabinet: --eeprom FILE)
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        auto log = [](const std::string& s) { std::printf("net: %s\n", s.c_str()), std::fflush(stdout); };
        net::LobbyClient& lobby = lobby_client;
        std::optional<net::Json> welcome;
        if (!o.lobby_url.empty()) {
            if (!net::LobbyClient::available()) {
                std::fprintf(stderr, "gamerun: built without the internet lobby (libcurl)\n");
                return 1;
            }
            welcome = tools::lobby_hello(lobby, build, o.net_host.empty() ? "gamerun" : o.net_host, o.lobby_url);
            if (!welcome) {
                std::fprintf(stderr, "gamerun: no lobby at %s: %s\n", o.lobby_url.c_str(), lobby.error().c_str());
                return 1;
            }
            std::printf("net: the lobby says hello (a relay: %s, country \"%s\")\n", (*welcome)["turn"].as_bool() ? "yes" : "no",
                        (*welcome)["country"].as_string().c_str());
            session.set_country((*welcome)["country"].as_string());
        }
        if (!o.net_host.empty()) {
            if (!session.host(o.net_host, o.net_seats)) {
                std::fprintf(stderr, "gamerun: can't offer a game: %s\n", session.error().c_str());
                return 1;
            }
            std::printf("net: offering \"%s\" for %u players (port %u)\n", o.net_host.c_str(), o.net_seats, session.port());
            if (welcome) {
                tools::find_addresses(lobby, session, (*welcome)["turn"].as_bool(), log);
                const auto code = tools::lobby_offer(lobby, session, o.net_host, o.net_seats, !o.net_private);
                if (!code) {
                    std::fprintf(stderr, "gamerun: the lobby didn't take the game\n");
                    return 1;
                }
                std::printf("net: on the lobby as %s (%s)\n", code->c_str(), o.net_private ? "private" : "listed");
                std::fflush(stdout); // (a test reads the code as it comes)
            }
            while (!o.net_start_alone && !session.all_seats_taken() && std::chrono::steady_clock::now() < give_up) {
                if (welcome) tools::lobby_pump(lobby, session);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!o.net_start_alone && !session.all_seats_taken()) {
                std::fprintf(stderr, "gamerun: nobody joined\n");
                return 1;
            }
            net::StartInfo info;
            if (o.clock) info.clock = *o.clock;
            else {
                m3::Bus probe(roms);
                info.clock = tools::set_rtc_to_host(probe);
            }
            session.start(info);
            if (welcome) {
                net::Json m = net::Json::object();
                std::size_t taken = 0; // (the seats with players: started alone, the others free to join)
                for (const std::string& name : session.players()) taken += name.empty() ? 0 : 1;
                m.set("t", "update"), m.set("taken", static_cast<int>(taken)), m.set("started", true);
                lobby.send(m);
            }
        } else if (welcome) { // --net-join CODE through the lobby
            std::string error;
            if (!session.listen()) {
                std::fprintf(stderr, "gamerun: %s\n", session.error().c_str());
                return 1;
            }
            tools::find_addresses(lobby, session, (*welcome)["turn"].as_bool(), log);
            if (!tools::lobby_join(lobby, session, build, o.net_join, "gamerun", error, o.net_spectate)) {
                std::fprintf(stderr, "gamerun: can't join %s: %s\n", o.net_join.c_str(), error.c_str());
                return 1;
            }
            std::printf("net: joining %s through the lobby\n", o.net_join.c_str());
        } else {
            net::Discovery discovery;
            if (!discovery.open()) {
                std::fprintf(stderr, "gamerun: can't look for games: %s\n", discovery.error().c_str());
                return 1;
            }
            std::optional<net::GameInfo> found;
            while (!found && std::chrono::steady_clock::now() < give_up) {
                for (const net::GameInfo& g : discovery.poll())
                    if (g.name == o.net_join && (o.net_spectate || g.taken < g.seats)) found = g; // (a game in progress too, a cabinet empty)
                if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!found) {
                std::fprintf(stderr, "gamerun: no game \"%s\" found\n", o.net_join.c_str());
                return 1;
            }
            std::printf("net: found \"%s\" at %s\n", found->name.c_str(), found->host.str().c_str());
            if (!(o.net_spectate ? session.watch(*found, "gamerun") : session.join(*found, "gamerun"))) {
                std::fprintf(stderr, "gamerun: can't join: %s\n", session.error().c_str());
                return 1;
            }
        }
        on_lobby = welcome.has_value();
        std::optional<net::StartInfo> start;
        while (!start && std::chrono::steady_clock::now() < give_up + std::chrono::seconds(120)) {
            start = netplay->wait_start(std::chrono::milliseconds(100));
            if (!start && session.room_closed()) {
                std::fprintf(stderr, "gamerun: the host closed the room\n");
                return 1;
            }
        }
        if (!start) {
            std::fprintf(stderr, "gamerun: the game didn't start\n");
            return 1;
        }
        o.boards = start->seats;
        o.parallel = true;
        o.local_seat = start->my_seat;
        if (g_input_delay_auto) { // (--input-delay auto: from the furthest player now, as the window's Auto starts)
            int one_way = -1;
            const auto trips = netplay->session().round_trips();
            for (std::size_t s = 0; s < trips.size(); ++s)
                if (s != o.local_seat && trips[s] >= 0) one_way = std::max(one_way, trips[s] / 2);
            g_input_delay = tools::DelayTuner::base(one_way, o.net_rollback != 0);
            std::printf("net: input delay %llu (auto; the furthest player %d ms away one way)\n", (unsigned long long)g_input_delay.load(),
                        one_way);
        }
        // (a spectator: the board it watches stands for its own here, never played: Session::local_input takes nothing)
        if (o.net_spectate) o.local_seat = std::min<uint8_t>(o.net_spectate_seat, static_cast<uint8_t>(start->seats - 1));
        // (the made packets' check (tools::NodeCheck): every machine sends its own board's, and compares the others' as
        // it runs them (made by side runs, or whole: then always the same); a spectator sends none)
        netplay->node_check().set(start->seats, o.net_spectate ? ~std::size_t{0} : o.local_seat);
        netplay->node_check().set_making(true);
        if (o.net_rollback) // (our own nodes sent once their frames are settled: none from a run on guesses)
            netplay->node_check().set_settled(
                [boards = start->seats](uint64_t frame) {
                    if (!g_machines || g_machines->empty()) return false;
                    tools::InputTable* t = (*g_machines)[0]->inputs;
                    return t && t->all_set(frame, boards) && t->wrong_from() > frame;
                },
                recomp::FrameTiming().frame_cycles());
        o.clock = start->clock;
        for (std::size_t s = 0; s < start->seats && s < o.board_eeprom.size(); ++s) { // each board from its player's cabinet
            o.board_eeprom[s] = tools::seat_eeprom(*start, s);
            o.board_backup[s] = s < start->backup.size() ? start->backup[s] : std::vector<uint8_t>();
        }
        o.eeprom_bytes.clear(); // (not this process's own: every board as every machine has it)
        o.netplay = netplay.get();
        if (o.net_hold_inputs) netplay->session().hold_inputs(o.net_hold_inputs);
        // (rollback's history, as the window: the long window as memory allows, a quiet player guessed that far, a
        // heard one --net-rollback ahead; --net-rollback-history N another length, 0 the old way: as far as it)
        if (o.net_rollback) {
            const uint64_t wanted = o.net_rollback_history ? std::max(o.net_rollback, *o.net_rollback_history) : o.net_rollback;
            o.net_rollback_depth = tools::rollback::fit_window(wanted, start->seats, tools::rollback::free_for_window());
            if (o.net_rollback_depth < wanted)
                std::printf("Rollback: a history of %llu frames, not %llu: what this machine's memory can keep\n",
                            (unsigned long long)o.net_rollback_depth, (unsigned long long)wanted);
            o.net_rollback = std::min(o.net_rollback, o.net_rollback_depth);
        }
        if (o.net_rollback && o.net_one_board && (!o.one_board_held || o.one_board_local_shadows)) {
            std::fprintf(stderr, "gamerun: --net-rollback with --net-one-board: with --one-board-held and shadows from the real "
                                 "boards (the switches only on settled frames)\n");
            return 2;
        }
        if (o.net_one_board && o.one_board_auto && start->seats != 2 && o.one_board_local_shadows) {
            std::fprintf(stderr, "gamerun: --one-board-auto with this board's own shadows is for two seats (from the others' real "
                                 "boards: any number)\n");
            return 2;
        }
        if (o.net_one_board) { // (this machine's board stays; the others are handed over to its module, tasks mode)
            for (std::size_t b = 0; b < start->seats; ++b) {
                if (b == o.local_seat) continue;
                if (o.one_board < 0) o.one_board = static_cast<int>(b);
                else o.one_board_also.push_back(b);
            }
            o.one_board_frame = o.net_one_board;
            o.one_board_tasks = true;
            std::printf("net: one board here from frame %llu: the other %u made from their players' inputs\n",
                        static_cast<unsigned long long>(o.net_one_board), start->seats - 1);
        }
        if (o.net_spectate && o.net_rollback) {
            std::fprintf(stderr, "gamerun: a spectator runs on inputs all in (no --net-rollback)\n");
            return 2;
        }
        if (o.net_spectate) {
            // (the board it watches: run whole; with --net-one-board, the others made from their players' buttons and
            // backups, Session::set_watch_one_board)
            if (o.net_one_board) {
                netplay->keeper().set_keep(3); // (its frames behind the backups' (2 s and more): some kept)
                netplay->session().set_watch_one_board(true);
            }
            std::printf("net: watching %u cabinets, clock %lld%s\n", start->seats, static_cast<long long>(start->clock),
                        o.net_one_board ? (", on one board: seat " + std::to_string(o.local_seat + 1)).c_str() : "");
            // (its game ended: the host gone, no new one greeting it: the boards waiting for inputs let go)
            spectate_end = std::jthread([&inputs, &session = netplay->session()](std::stop_token stop) {
                while (!stop.stop_requested() && !session.room_closed()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!stop.stop_requested()) inputs.close();
            });
        }
        else
            std::printf("net: playing seat %u of %u (board %u), clock %lld\n", start->my_seat + 1, start->seats,
                        start->my_seat, static_cast<long long>(start->clock));
        if (start->state_frame) { // a game joined in progress: every board as the host had them
            auto state = net::decompress(start->state);
            if (!state) {
                std::fprintf(stderr, "gamerun: the host's state doesn't unpack\n");
                return 1;
            }
            join_state = std::move(*state);
            g_catching_up = o.net_catch_up && !o.net_spectate; // (--net-catch-up: the cabinet once caught up)
            std::printf("net: running on from the host's frame %llu (%zu KB, %zu KB packed)\n",
                        static_cast<unsigned long long>(*start->state_frame), join_state.size() / 1024, start->state.size() / 1024);
        }
    }

    // Joined through the lobby by its code: if the host leaves the game in
    // progress and this machine hosts the room then (host migration), the
    // room is listed again under that code, and players joining it let in.
    std::jthread relist;
    // Hosting through the lobby: players joining once the game has started (--net-start-alone, or a seat left
    // empty) let in as the window does, their addresses from the lobby (needed on the internet: without them a
    // router or the relay lets nothing of theirs through).
    if (netplay && on_lobby && !o.net_host.empty())
        relist = std::jthread([&lobby_client, &session = netplay->session()](std::stop_token stop) {
            while (!stop.stop_requested()) {
                tools::lobby_pump(lobby_client, session);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    if (netplay && on_lobby && !o.net_join.empty() && !o.net_spectate)
        relist = std::jthread([&lobby_client, &session = netplay->session(), code = o.net_join](std::stop_token stop) {
            bool listed = false;
            while (!stop.stop_requested()) {
                if (!listed && session.hosting()) {
                    listed = true;
                    const auto again = tools::lobby_offer(lobby_client, session, "gamerun", 2, true, code);
                    std::printf("net: on the lobby as %s (%s)\n", again ? again->c_str() : "nothing",
                                again && *again == code ? "taken over" : "a new code");
                    std::fflush(stdout);
                    net::Json m = net::Json::object();
                    m.set("t", "update"), m.set("taken", 1), m.set("started", true);
                    lobby_client.send(m);
                }
                if (listed) tools::lobby_pump(lobby_client, session);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });

    std::unique_ptr<m3::LinkRing> ring;
    if (o.boards > 1) {
        ring = std::make_unique<m3::LinkRing>(o.boards);
        g_ring = ring.get();
        // (tests: ONEBOARD_SENDS=B:FROM:TO prints board B's packets taken in and sent (as the ring's exchange passes them
        // on, numbered by it) from FROM to TO, with a hash of each)
        if (const char* spec = std::getenv("ONEBOARD_SENDS")) {
            unsigned long long b = 0, from = 0, to = 0;
            if (std::sscanf(spec, "%llu:%llu:%llu", &b, &from, &to) == 3) {
                auto counts = std::make_shared<std::vector<uint64_t>>(o.boards, 0);
                ring->on_receive([b, from, to](std::size_t board, uint64_t seq, uint64_t at, uint64_t now) {
                    if (board == b && seq >= from && seq <= to)
                        std::printf("recv board %zu #%llu: could arrive at %llu, taken at %llu\n", board, (unsigned long long)seq,
                                    (unsigned long long)at, (unsigned long long)now);
                });
                ring->on_send([counts, b, from, to](std::size_t board, uint64_t t, const std::vector<uint8_t>& data) {
                    const uint64_t n = (*counts)[board]++;
                    if (board != b || n < from || n > to) return;
                    uint64_t h = 0xCBF29CE484222325ull;
                    for (uint8_t v : data) h = (h ^ v) * 0x100000001B3ull;
                    std::printf("send board %zu #%llu at %llu: %zu bytes, hash %016llx\n", board, (unsigned long long)n,
                                (unsigned long long)t, data.size(), (unsigned long long)h);
                });
            }
        }
        ring->set_latency(o.link_latency);
        if (o.netplay) // (one board a PC's check: every packet; set_up_one_board's own adds the backups' keeper)
            ring->on_departure([net = o.netplay](std::size_t to, uint64_t seq, uint64_t at, const std::vector<uint8_t>& data) {
                net->node_check().departed(to, seq, at, data);
            });
        if (o.link_trace)
            ring->on_send([left = o.link_trace](std::size_t from, uint64_t at, const std::vector<uint8_t>& data) mutable {
                if (!left) return;
                --left;
                std::printf("link: board %zu sends at frame %.3f, %zu bytes:", from,
                            static_cast<double>(at) / static_cast<double>(recomp::FrameTiming().frame_cycles()),
                            data.size());
                for (std::size_t k = 0; k < std::min<std::size_t>(data.size(), 24); ++k) std::printf(" %02x", data[k]);
                std::printf("\n");
            });
        else if (o.link_dump) // each data frame's first part, the sender's own node (comm RAM 0x100-0x9FF)
            ring->on_send([file = std::shared_ptr<std::FILE>(std::fopen(o.link_dump, "wb"), std::fclose),
                           lock = std::make_shared<std::mutex>()](std::size_t from, uint64_t at, const std::vector<uint8_t>& data) {
                if (!file || data.size() < kNodeBytes) return; // (the small sync frames)
                const uint32_t head[2] = {static_cast<uint32_t>(from),
                                          static_cast<uint32_t>(at / recomp::FrameTiming().frame_cycles())};
                std::lock_guard guard(*lock);
                std::fwrite(head, sizeof head, 1, file.get());
                std::fwrite(data.data(), 1, kNodeBytes, file.get());
            });
    }
    // Parallel: the boards run at once, meeting every slice; the link's
    // packets go on there, so they take at least a slice to arrive.
    const bool parallel = o.parallel && o.boards > 1;
    if (parallel) {
        const uint64_t slice_cycles =
            (recomp::FrameTiming().frame_cycles() * o.slice + recomp::FrameTiming::kLines - 1) / recomp::FrameTiming::kLines;
        ring->set_deferred(true);
        ring->set_latency(std::max(o.link_latency, slice_cycles));
        std::printf("Parallel: the link's latency %llu main board cycles (a slice of %u lines)\n",
                    static_cast<unsigned long long>(std::max(o.link_latency, slice_cycles)), o.slice);
    }
    // Linked boards' save states: one file for all of them.
    LinkedState linked;
    if (ring && (!o.save_state.empty() || o.load_state || networked)) {
        linked.ring = ring.get();
        linked.path = o.save_state;
        linked.boards.resize(o.boards);
        if (o.load_state || !join_state.empty()) {
            try {
                m3::State s(o.load_state ? tools::read_state_file(tools::read_file(o.load_state)).state : std::move(join_state));
                linked.io(s);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "gamerun: %s: %s (a state of %zu linked boards?)\n", o.load_state ? o.load_state : "the host's state",
                             e.what(), o.boards);
                return 1;
            }
            linked.loaded = std::move(linked.boards);
            linked.boards.assign(o.boards, {});
        }
        // (asked for every board's state: a player joining in progress, or one out of step; put together, packed and
        // sent off the game thread, as the window)
        if (networked) {
            linked.sink = [&netplay](uint64_t frame, std::vector<uint8_t> state) {
                netplay->send_state_later(frame, [s = std::move(state)]() mutable { return std::move(s); });
            };
            linked.parts_sink = [&netplay](uint64_t frame, std::vector<uint8_t> cut, std::vector<std::vector<uint8_t>> parts) {
                netplay->send_state_later(frame, [c = std::move(cut), p = std::move(parts)]() mutable {
                    return tools::LinkedState::assemble(c, p);
                });
            };
        }
    }
    LinkedRollback rollback; // (--rollback-test, linked boards)
    if (ring && o.rb_frames) rollback.ring = ring.get();
    std::vector<std::unique_ptr<Machine>> machines;
    g_machines = &machines;
    // (cleared as the boards go, before the network session stops: its thread's node check reads them (NodeCheck's
    // settled test; a crash at the end of an internet test, 2026-11-07))
    struct MachinesGone {
        ~MachinesGone() { g_machines = nullptr; }
    } machines_gone;
    auto make_turns = [&] {
        return std::make_unique<tools::Turns>(o.boards, parallel, parallel ? std::function<void()>([&ring, &linked, &rollback, &o, &machines] {
            ring->exchange();
            if (o.linked_inputs) // (--inputs: what the bot changed where it stopped the boards, at a meeting)
                for (auto& m : machines)
                    linked_change(*m, o, tools::LinkedInputLog::At::Meeting, machines[0]->timing.frames(), machines[0]->line);
            // --one-board-late: set up and handed over at the first meeting in FRAME, every board stopped there (the
            // window's way when it finds this PC too slow for every board: nothing of it before)
            static bool late_done = false;
            if (o.one_board_late && !late_done && machines[0]->timing.frames() >= o.one_board_frame) {
                late_done = true;
                set_up_one_board(machines, o);
                for (auto& m : machines)
                    if (one_board_goes(o, m->index)) g_one_board.hand_over(m->ob, o.one_board_held);
                std::printf("one board: set up at frame %llu, line %u\n", (unsigned long long)machines[0]->timing.frames(), machines[0]->line);
            }
            if (g_net_rb) { // (--net-rollback: the ring's part of the frame every board saved; back, if a guess was wrong)
                const bool capturing = !linked.boards.empty() && linked.saved == linked.boards.size();
                if (const uint64_t back = g_net_rb->between(capturing ? &linked.cut : nullptr))
                    for (auto& m : machines) m->hash_pending.erase(m->hash_pending.upper_bound(back), m->hash_pending.end());
            }
            if (linked.ring && (!linked.path.empty() || linked.sink)) linked.finish();
            if (rollback.ring) rollback.between(o);
        }) : std::function<void()>());
    };
    std::unique_ptr<tools::Turns> turns_ptr = make_turns();
    g_one_board.set_turns(turns_ptr.get()); // (one board: a shadow's hand-back waited for at the meetings)
    // The GPU (--renderer gpu or compare), without a window; the boards take
    // turns, so they share it.
#ifdef SPIKEOUT_GPU
    gpu::Device gpu_device;
    if (o.renderer != Options::Renderer::Software || o.gpu_time_scale) {
        if (gpu_device.open_headless()) {
            std::printf("GPU: %s\n", gpu_device.driver());
        } else {
            std::fprintf(stderr, "gamerun: no GPU (%s)\n", gpu_device.error().c_str());
            return 1;
        }
    }
#else
    if (o.renderer != Options::Renderer::Software || o.gpu_time_scale) {
        std::fprintf(stderr, "gamerun: built without the GPU renderer (SPIKEOUT_GPU)\n");
        return 1;
    }
#endif
  for (;;) { // (network play: again from the host's state, a machine out of step)
    tools::Turns& turns = *turns_ptr;
    machines.clear();
    for (std::size_t i = 0; i < o.boards; ++i) {
        machines.push_back(std::make_unique<Machine>(i, roms, functions));
        machines.back()->c.set_blocks(blocks);
        m3::games::install_spikeout_hooks(machines.back()->c);
        if ((o.camera_trace && i == 0) || o.camera_events || o.script_actions) { // (--camera-events, --script-actions: every board)
            Machine& m = *machines.back();
            m.camera.trace_left = i == 0 ? o.camera_trace : 0;
            m.camera.actions = o.script_actions;
            m.c.set_hook(camera_hook, &m);
            if (o.camera_events) {
                m.camera.events = true;
                m.bus.watch_writes(kCamera, kCamera + kCameraBytes, camera_written, &m);
            }
        }
        if (o.one_board >= 0 && i + 1 == o.boards && !o.one_board_late) set_up_one_board(machines, o);
        if (o.drop_frame && static_cast<int>(i) == o.drop_board && o.camera_test_free) // (its own flow's bytes, for the side flow)
            machines.back()->c.set_hook(camera_hook, machines.back().get());
        if ((o.camera_record && static_cast<int>(i) == o.camera_record_board) ||
            (o.camera_test && static_cast<int>(i) == o.camera_test_board)) {
            Machine& m = *machines.back();
            if (o.camera_record) {
                m.camera.record = std::fopen(o.camera_record, "wb");
            } else {
                const std::vector<uint8_t> bytes = tools::read_file(o.camera_test);
                for (std::size_t at = 0; at + 4 + kCameraBytes <= bytes.size(); at += 4 + kCameraBytes) {
                    uint32_t logic = 0;
                    std::memcpy(&logic, bytes.data() + at, 4);
                    m.camera.recorded[logic].assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 4),
                                                    bytes.begin() + static_cast<std::ptrdiff_t>(at + 4 + kCameraBytes));
                }
                m.camera.testing = true;
                m.camera.from = o.camera_test_from;
                m.camera.copy_flags = o.camera_test_flags;
                m.camera.mirror = o.camera_test_mirror;
                m.camera.free = o.camera_test_free;
                if (o.camera_test_mirror) m.bus.watch_writes(kCamera, kCamera + kCameraBytes, camera_written, &m);
                m.camera.view_out = std::fopen((std::string(o.camera_test) + ".view").c_str(), "wb");
                m.camera.mine_out = std::fopen((std::string(o.camera_test) + ".mine").c_str(), "wb");
            }
            m.c.set_hook(camera_hook, &m);
        }
        if (linked.ring) machines.back()->linked = &linked;
        machines.back()->inputs = machines.back()->ob.inputs = &inputs;
        set_up(*machines.back(), o, game, ring.get(), turns);
#ifdef SPIKEOUT_GPU
        if (o.renderer != Options::Renderer::Software || o.gpu_time_scale) {
            auto& m = *machines.back();
            m.gpu = std::make_unique<gpu::Real3dGpu>();
            m.gpu->set_quads(o.quads);
            m.gpu_device = gpu_device.get();
            if (o.wide && ((o.renderer == Options::Renderer::Gpu && o.gpu_scale == 1) || o.gpu_time_scale)) { // (--gpu-time: timed so)
                m.builder.set_extend(o.wide);
                m.tilegen.set_extend(o.wide);
            }
            if (!m.gpu->open(gpu_device, o.gpu_time_scale ? o.gpu_time_scale : o.gpu_scale, m.builder.extend())) {
                std::fprintf(stderr, "gamerun: GPU renderer: %s\n", m.gpu->error().c_str());
                return 1;
            }
            if (o.screen_w > 0 && o.screen_h > 0 && i == 0) { // the window's screen pass, off screen
                SDL_GPUTextureCreateInfo tex{};
                tex.type = SDL_GPU_TEXTURETYPE_2D;
                tex.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
                tex.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
                tex.width = static_cast<Uint32>(o.screen_w);
                tex.height = static_cast<Uint32>(o.screen_h);
                tex.layer_count_or_depth = 1;
                tex.num_levels = 1;
                m.screen_target = SDL_CreateGPUTexture(gpu_device.get(), &tex);
                m.presenter = std::make_unique<gpu::Presenter>();
                if (!m.screen_target || !m.presenter->open(gpu_device)) {
                    std::fprintf(stderr, "gamerun: --screen-time: can't draw the screen off screen\n");
                    return 1;
                }
                m.presenter->set_target(m.screen_target, static_cast<uint32_t>(o.screen_w), static_cast<uint32_t>(o.screen_h));
            }
        }
#endif
    }

    std::printf("%zu recompiled functions; running up to %lld blocks from %08x", functions.size(),
                static_cast<long long>(o.blocks), machines[0]->c.cpu.pc);
    if (o.boards > 1)
        std::printf(" on each of %zu linked boards, %s every %u lines", o.boards, parallel ? "running at once, meeting" : "taking turns", o.slice);
    std::printf("\n\n");

    g_net_rb.reset();
    if (o.net_rollback && o.netplay && ring) { // (--net-rollback)
        g_net_rb = std::make_unique<tools::rollback::Linked>(o.boards, std::max(o.net_rollback, o.net_rollback_depth), ring.get(), inputs);
        // (one board here correcting its made packets (tools::NodeCheck): every frame kept, a late real packet's frame
        // there to go back to)
        if (o.one_board >= 0 && !(std::getenv("NODE_CORRECT") && std::getenv("NODE_CORRECT")[0] == '0')) g_net_rb->keep_every(true);
        for (auto& m : machines)
            g_net_rb->attach(m->index, [mp = m.get()](m3::State& s) {
                // (one board: the boards on side runs, made by this one's module, their network boards with it; first:
                // board_state ends with its check that nothing follows it)
                if (mp->ob.cabinets)
                    for (auto& g : *g_machines)
                        if (g.get() != mp && !g_net_rb->active(g->index)) g->bus.netboard().state(s);
                tools::board_state(s, mp->c, mp->timing, {});
            });
        for (auto& m : machines) // (one board: the module's own state, kept and put back with the staying board)
            if (m->ob.cabinets) {
                tools::remote::Cabinets* cab = m->ob.cabinets.get();
                g_net_rb->attach_extra(
                    m->index, [cab] { return std::shared_ptr<const void>(std::make_shared<tools::remote::Cabinets::Kept>(cab->keep())); },
                    [cab](const void* k) { cab->put_back(*static_cast<const tools::remote::Cabinets::Kept*>(k)); });
            }
        g_net_rb->start(machines[0]->timing.frames());
    }
    rollback.boards.clear();
    for (auto& m : machines) rollback.boards.push_back(m.get());
    if (o.boards == 1) {
        const double cpu_before = m3::thread_cpu_seconds();
        run(*machines[0], turns);
        machines[0]->cpu_seconds = m3::thread_cpu_seconds() - cpu_before;
    } else {
        std::vector<HostCall> standin_log; // (--link-standin: the calls to replay)
        if (o.link_standin) {
            const std::vector<uint8_t> bytes = tools::read_file(o.link_standin);
            standin_log.resize(bytes.size() / sizeof(HostCall));
            std::memcpy(standin_log.data(), bytes.data(), standin_log.size() * sizeof(HostCall));
            std::printf("Board %d stands in: %zu calls to its network board replayed, its main board not run\n",
                        o.link_standin_board, standin_log.size());
        }
        std::vector<std::thread> threads;
        for (auto& m : machines) {
            if (static_cast<int>(m->index) == o.link_standin_board)
                threads.emplace_back([&m, &turns, &standin_log, &o] {
                    if (o.link_standin_blocks) stand_in_blocks(*m, turns, standin_log, o.link_standin_at, o.slice);
                    else
                        stand_in(*m, turns, standin_log, o.link_standin_at, o.link_standin_patch == "enemy-flags",
                                 o.link_standin_no_reads, o.link_standin_coarse);
                });
            else
                threads.emplace_back([&m, &turns] {
                    run(*m, turns);
                    m->cpu_seconds = m3::thread_cpu_seconds();
                });
        }
        for (auto& t : threads) t.join();
        for (auto& m : machines)
            if (m->recording_host) {
                if (tools::write_file(o.link_record, std::span(reinterpret_cast<const uint8_t*>(m->host_log.data()),
                                                               m->host_log.size() * sizeof(HostCall))))
                    std::printf("Recorded board %zu's %zu calls to its network board in %s\n", m->index, m->host_log.size(),
                                o.link_record);
            }
    }
    // Network play: our boards out of step with the host's; every board
    // again from its state, the inputs since fed again.
    const auto resync = netplay ? netplay->take_resync() : std::nullopt;
    if (!resync) break;
    auto unpacked = net::decompress(resync->state);
    if (!unpacked) {
        std::fprintf(stderr, "gamerun: the host's state doesn't unpack\n");
        return 1;
    }
    try {
        m3::State s(std::move(*unpacked));
        linked.boards.assign(o.boards, {});
        linked.io(s);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "gamerun: the host's state: %s\n", e.what());
        return 1;
    }
    linked.loaded = std::move(linked.boards);
    linked.boards.assign(o.boards, {});
    inputs.reset();
    netplay->session().feed_inputs(*resync->state_frame);
    turns_ptr = make_turns();
    g_one_board.set_turns(turns_ptr.get());
    std::printf("net: running on from the host's frame %llu again\n", static_cast<unsigned long long>(*resync->state_frame));
  }

    for (auto& m : machines) {
        if (o.boards > 1) std::printf("%s==== Board %zu ====\n", m->index ? "\n" : "", m->index);
        report(*m, o);
    }
    if (ring) std::printf("\nLink: %llu packets delivered around the ring\n", static_cast<unsigned long long>(ring->delivered()));
    if (o.camera_events && machines.size() > 1) { // each event writer: the same writes at the same game frames on boards 0 and 1?
        const auto& a = machines[0]->camera.event_writers;
        const auto& b = machines[1]->camera.event_writers;
        std::set<uint64_t> writers;
        for (const auto& [pc, w] : a) writers.insert(pc);
        for (const auto& [pc, w] : b) writers.insert(pc);
        std::printf("Camera events, board 0 against board 1 (each write by game frame, offset and value):\n");
        for (uint64_t pc : writers) {
            std::multiset<std::array<uint32_t, 3>> sa, sb;
            if (auto it = a.find(pc); it != a.end()) sa.insert(it->second.seen.begin(), it->second.seen.end());
            if (auto it = b.find(pc); it != b.end()) sb.insert(it->second.seen.begin(), it->second.seen.end());
            std::size_t both = 0;
            for (const auto& w : sa) both += sb.count(w) != 0;
            const char* kind = sa == sb ? "shared (the same on both)" : both == 0 ? "each board's own (none the same)" : "partly the same";
            std::printf("  %08x (from %08x): board 0 %zu writes, board 1 %zu, %zu of board 0's on board 1 too: %s\n",
                        static_cast<uint32_t>(pc >> 32), static_cast<uint32_t>(pc), sa.size(), sb.size(), both, kind);
        }
    }
    for (auto& mp : machines) {
        Machine::CameraTest& t = mp->camera;
        if (t.record) {
            std::fclose(t.record);
            std::printf("Camera: board %zu's camera at %llu updates recorded in %s\n", mp->index,
                        static_cast<unsigned long long>(t.calls), o.camera_record);
        }
        if (t.events) { // --camera-events
            std::printf("Camera events, board %zu: %zu blocks wrote into the camera besides its own code\n", mp->index,
                        t.event_writers.size());
            for (const auto& [pc, w] : t.event_writers) {
                std::printf("  %08x (from %08x): %llu writes, frames %llu-%llu, at", static_cast<uint32_t>(pc >> 32),
                            static_cast<uint32_t>(pc), static_cast<unsigned long long>(w.writes),
                            static_cast<unsigned long long>(w.first), static_cast<unsigned long long>(w.last));
                std::size_t shown = 0;
                for (uint32_t off : w.offsets)
                    if (shown++ < 16) std::printf(" +%03x", off);
                std::printf("%s\n", w.offsets.size() > 16 ? " ..." : "");
            }
        }
        if (t.actions) { // --script-actions: by action, the records that ran it
            std::map<uint8_t, std::vector<std::pair<uint32_t, Machine::CameraTest::ActionRun>>> by_action;
            for (const auto& [record, a] : t.actions_run) by_action[a.action].emplace_back(record, a);
            std::printf("Script actions, board %zu: %zu kinds, %zu records run\n", mp->index, by_action.size(), t.actions_run.size());
            for (const auto& [action, records] : by_action) {
                uint64_t runs = 0, first = ~0ull;
                for (const auto& [record, a] : records) runs += a.runs, first = std::min(first, a.first);
                std::printf("  action %02x: %zu records, %llu runs, first at frame %llu:", action, records.size(),
                            static_cast<unsigned long long>(runs), static_cast<unsigned long long>(first));
                std::size_t shown = 0;
                for (const auto& [record, a] : records)
                    if (shown++ < 12) std::printf(" %06x", record);
                std::printf("%s\n", records.size() > 12 ? " ..." : "");
            }
        }
        if (!t.testing) continue;
        if (t.view_out) {
            std::fclose(t.view_out);
            if (t.mine_out) std::fclose(t.mine_out);
            std::printf("Camera test: %llu view tests run on the side, in %s.view\n", static_cast<unsigned long long>(t.view_tests),
                        o.camera_test);
        }
        std::printf("Camera test: the side runs' game code %.3f ms a frame (%.1f us a run; the test's own copies of RAM not counted)\n",
                    t.calls ? 1000.0 * t.side_seconds / static_cast<double>(t.calls) : 0.0,
                    t.runs ? 1e6 * t.side_seconds / static_cast<double>(t.runs) : 0.0);
        const double n = t.compared ? static_cast<double>(t.compared) : 1.0;
        std::printf("Camera test, board %zu: %llu updates, %llu run on the side from logic %u, %llu compared with the "
                    "other board's camera at its next update%s%s\n"
                    "  run on by itself: heading the same %.1f%%, the whole camera %.1f%% (first heading differing at logic %u)\n"
                    "  one step from the recorded camera: heading %.1f%%, the whole camera %.1f%%\n"
                    "  written outside the camera by the side runs (16-byte blocks):",
                    mp->index, static_cast<unsigned long long>(t.calls), static_cast<unsigned long long>(t.runs), t.first_logic,
                    static_cast<unsigned long long>(t.compared), t.failed.empty() ? "" : "; failed: ", t.failed.c_str(),
                    100.0 * static_cast<double>(t.heading_free) / n, 100.0 * static_cast<double>(t.object_free) / n,
                    t.first_free_miss, 100.0 * static_cast<double>(t.heading_step) / n,
                    100.0 * static_cast<double>(t.object_step) / n);
        std::size_t shown = 0;
        for (const auto& [at, count] : t.written)
            if (shown++ < 40) std::printf(" %06x(%llu)", at, static_cast<unsigned long long>(count));
        std::printf("%s\n", t.written.size() > 40 ? (" ... " + std::to_string(t.written.size()) + " blocks").c_str() : "");
    }
    if (rollback.ring)
        std::printf("Rollback test, the link ring: %llu frames' packets on their way checked, %llu differed%s; all the boards "
                    "%llu times back together, in %.3f ms (worst %.3f)%s\n",
                    static_cast<unsigned long long>(rollback.checked), static_cast<unsigned long long>(rollback.differed),
                    rollback.differed ? (" (the first at frame " + std::to_string(rollback.first_differ) + ")").c_str() : "",
                    static_cast<unsigned long long>(rollback.restores),
                    rollback.restores ? rollback.restore_ms / static_cast<double>(rollback.restores) : 0.0, rollback.restore_worst,
                    rollback.unmatched ? "; THE BOARDS SAVED DIFFERENT FRAMES" : "");
    if (netplay) {
        { // (each other player's link at the end, as the window's seat list has it: on this network, direct, the relay)
            std::string links;
            const auto seats = netplay->session().seats();
            const auto trips = netplay->session().round_trips();
            for (std::size_t s = 0; s < seats.size(); ++s) {
                using L = net::Session::SeatView::Link;
                const L l = seats[s].link;
                if (l == L::none) continue;
                links += " " + std::to_string(s + 1) + "P " + (l == L::lan ? "LAN" : l == L::direct ? "DIRECT" : "RELAY");
                if (s < trips.size() && trips[s] >= 0) links += " (" + std::to_string(trips[s]) + " ms)";
            }
            if (!links.empty()) std::printf("Network: links:%s\n", links.c_str());
        }
        netplay->session().leave();
        std::printf("Network: %llu hash checks differed\n", static_cast<unsigned long long>(netplay->desyncs()));
        std::printf("Node check: %s\n", netplay->node_check().report().c_str());
        if (g_net_rb) std::printf("Rollback: %s\n", g_net_rb->report().c_str());
        double wait_max = 0;
        uint64_t sync_waits = 0, long_waits = 0;
        for (const auto& m : machines)
            wait_max = std::max(wait_max, m->input_wait_max), sync_waits += m->sync_waits, long_waits += m->input_waits_long;
        if (g_net_rb) std::printf("Rollback: time sync held back %llu frames\n", (unsigned long long)sync_waits);
        std::printf("Network: the longest a frame waited for inputs: %.0f ms; over a frame's time %llu times\n", wait_max,
                    static_cast<unsigned long long>(long_waits));
    }
    if (g_pace.frames)
        std::printf("Pace: %llu frames in %.1f s (%.2f a second; the arcade's %.2f), stutters (over 1.5 frames from the "
                    "last) %llu, the longest gap %.0f ms\n",
                    static_cast<unsigned long long>(g_pace.frames), g_pace.seconds,
                    static_cast<double>(g_pace.frames) / g_pace.seconds, recomp::FrameTiming::kRefreshHz,
                    static_cast<unsigned long long>(g_pace.late), g_pace.worst);
    std::printf("CPU time: %.2f s (the whole process, every thread)\n", m3::process_cpu_seconds());
    return 0;
}
