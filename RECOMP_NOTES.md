# Recompilation notes

How *Soulcalibur III* (SLUS-21216) was brought up on PS2Recomp, what the game overrides do and why, the runtime
fixes this game led to, where it stands, and the tools used to debug it. Build steps are in the [README](README.md).

## Bring-up

The ELF is stripped. `recomp/sc3.toml` started as `ps2_analyzer` output (12,851 functions, 636 SCE SDK symbols
matched from its embedded database), then grew by hand:

- **Function map.** A headless Ghidra run with the Emotion Engine extension exported `recomp/ghidra_map.csv`
  (8,901 functions, 910 code labels). The recompiler does not read it (`ghidra_output` is empty); it is kept as
  a reference map of the ELF.
- **Hint lists** (`recomp/*_hints.txt`), folded into `entry_points`:
  - code reached only through data: `scripts/find_code_pointers.py` takes data words that point into `.text`
    right after a function return and are not yet a function start (the `*ptr_hints.txt` and `new_hints.txt`
    lists);
  - every J/JAL target that had no function of its own (`jump_hints.txt`): 63 helpers the analyzer had merged
    into a neighbour, for example 0x3E1820 inside the stub-bound `sceMpegInit`, where the main thread died during
    boot.
- **Stubs.** libcdvd, libdma, libgraph, libpad and libmpeg entry points are bound to the runtime's handlers by
  name (`stubs`), so CD reads, DMA and pad I/O go through the HLE layer.
- **The game's own `rand` (0x42D508) is recompiled, not stubbed.** The runtime's `rand` stub returned
  `std::rand() & 0x7FFF`, while the game's newlib code scales its results by RAND_MAX 0x7FFFFFFF, so every random
  pick came out 0. The recompiled `rand` follows the game's own `srand`.
- **`[mmio]` and `[jump_tables]`** are the analyzer's, kept as generated.

The recompiler emits one C++ file per function (about 12,900 with the entry stubs) and `register_functions.cpp`,
the address-to-function table. `ps2_vu_aot` then finds the VU1 and VU0 microprograms in the ELF itself and compiles
them into `generated/vu1/` (`scripts/recomp.sh` runs both); no run of the game is needed for it.

## Game overrides (`runtime/sc3_overrides.cpp`)

`PS2_REGISTER_GAME_OVERRIDE` matches the ELF by name, entry point (0x100008) and CRC32, then `applySoulcalibur3`
sets up the following.

### Memory

- **Reserved runtime memory at 0x80000-0x100000.** crt0 calls `SetupThread` with the main stack at 0x01FDFF00
  and `SetupHeap(end, -1)`: the heap runs up to the main stack and the game allocates nearly all of it. The
  runtime's HLE pools and invocation stacks move to kernel RAM instead (`configureReservedMemory`).

### Timing and rendering choices

- **Wall-clock catch-up Off.** SC3's audio follows emulated time. Fast-forwarding emulated time after a slow
  stretch ran the game and its music faster than real time, and skipping fields sent it down its late-frame
  paths, whose display lists break: the menu-transition and Chronicles freezes (the ordering-table walk at
  0x10E4B8 looping, the main thread stuck in the frame-end wait 0x12E4F0). With catch-up off, emulated time only
  ever slows down; a late vblank restarts the field timeline instead of leaving a debt (`PS2X_EE_CATCHUP` still
  overrides it).
- **GS SIGNAL stall off** (`setSignalStallEnabled(false)`). A frame chains about 30 SIGNAL handshakes between the
  GIF and VIF1 paths (the GS interrupt handler 0x10E658 restarts both chains when both have signalled). Waiting
  out the modelled GS interrupt latency at each one cost about 20% (Free Training 22 -> 18 fps at the time) and
  the game draws correctly without the stall.
- **Native GS renderer** (`PS2Runtime::requestNativeGsRenderer`). The GS stream becomes OpenGL draws (docs/native.md
  in PS2Recomp). Free Training runs at 58 fps; the vertical seams every 64 pixels, the soft picture and the darkened
  frame were GS back end bugs, fixed on PS2Recomp sc3/native-gs-seams (pixel centres, HIGHLIGHT alpha, Ad blending,
  the Z buffer a draw's ZBUF names). `PS2X_GS_RENDERER=cpu` draws with the CPU reference instead.
- **SPU2 on the wall clock by default** (`PS2X_SPU2_WALLCLOCK=1` in scripts/run.sh, regress.sh and sc3run.sh;
  `=0` keeps the stretched IOP-clock output). The IOP mixes into a queue the audio callback takes from, so SC3's ADX
  stream (CRI ADXI, AutoDMA on core 1, one 256-sample half ahead) gets each half's interrupt before the next half
  plays: regress route 17367 AutoDMA transfers (17353 on the IOP clock), 23 empty halves (24869 before the queue),
  58.24 fps. The SPU2 stays within 512 samples of the IOP clock (PS2Recomp e749e57):
  regress route 17355 transfers, 24 empty halves, 58.24 fps, 6918 frames of silence (`gap=`, mostly while loading)
  in 189 s. SC3's movie soundtracks are host audio mixed into the output; the game never streams PCM on core 0.
- **IOP on the wall clock by default** (`PS2X_IOP_WALLCLOCK=1`, set by the override unless already set; `=0` puts the
  IOP back on the EE cycles the game accounts). Its timers, vblank interrupts and the sound drivers' ticks follow real
  time. It used to cost 4 to 7 fps: the runtime read the host clock at every `EeScheduler::accountCycles`, and
  `clock_gettime`, the duration conversion and `advanceIopEeCycles` took about a quarter of the EE thread in an
  uncapped fight (PS2X_PROFILE: 3150 of 11395 samples). It now reads it once per `kIopWakeEeCycles` (0.16 ms) of EE
  time (PS2Recomp sc3/native-hostclock-iop). Regress route, three runs a side, wall clock against EE clock: 59.79 fps
  against 59.31 capped, 58.62 against 57.81 with the fight uncapped, 6 and 13 frames over 25 ms against 10 and 9,
  0 to 69 empty ADX halves against 0 to 13 (before the fix: 55.87 against 59.79, 51.00 against 57.94, 18 and 57
  frames over 25 ms). Saves were made in Options (create, overwrite) and loaded back after a restart, the setting
  they hold restored at boot and by Load Data.
- **Frame loop on real time** (`SC3_NATIVE_FRAMES`, on unless `=0`; PS2Recomp docs/native.md, layer 1). The main loop
  (0x12DB20) ends each frame with an inline Timer 3 wait: 0x148098 (lines since the last frame) polled from 0x12EA08
  until it reaches the target at sp+0x30, then called once more from 0x12EA88 to record the frame's length. The
  override answers the polling call with the target at once, and at the recording call holds the frame to the
  FramePacer's cap, charges the EE the real time the frame did not account and presents the frame
  (`presentGuestFrame`). SC3 counts frames, so there is no time step to feed. EE time follows real time over the
  run: a frame that accounts more than it took leaves a lead the next frames use up (otherwise the IOP runs ahead and
  the SPU2 drops 44800 samples in the regress route), and a load's long frame is charged in full up to a second
  (the pacer's step stops at 0.1 s, and the ADX stream starved). While a one-shot movie plays (the intro, the
  opening, the Chronicles of the Sword story), the main loop takes another branch (0x12E638) and ends its frame in
  mwPly's vsync wait 0x3AE238: ADXM's 0x3F0F38 sleeps the main thread in 0x3F0F10 until ADXM's vsync thread, woken
  by the vblank interrupt, wakes it. The override answers that sleep (0x3F0F10 returning to 0x3F0F60 for 0x12E654)
  with the same native frame end, so movies are paced by the host as well and nothing runs on the vblank grid. Their
  pictures come from the movie clock (the soundtrack the audio callback took) at each window present: 29.97
  pictures and 60 presents a second, the movie clock 1.000 s per second, the intro 35.12 s against its 35.07 s,
  the opening 243.74 s against 243.68, the Chronicles movie 61.38 s against 61.23, a 330 s run's SPU2 output 330.3 s
  long; the main loop holds 60 frames per second through all of them and Start still skips. Without the override the
  main loop slept until emulated vblanks that no frame end kept on real time: EE time ran at 14% of real time and
  stood 1.8 s ahead when the movie ended (PS2Recomp's idle present, native/renderer ea929440, does not help: the
  movie pictures do not come from the GS). Two PS2Recomp fixes came with it: under native frames an idle wait lasts the EE cycles to its event, not the
  vblank timeline's lead (0.6 s stalls in the title-screen loads, sc3/native-frames-idle), and `PS2X_CAPTURE_DIR`
  saves the frame's own width (movie frames came out 1280x448, sc3/native-movie-scale). Regress route against the
  vblank grid: 59.56 fps against 58.22, game frames 59.65 per second (slowest 5 s 57.6) against 56.67 (46.3 in the
  character select and the stage load), 5 frames over 25 ms against 592, 21 empty ADX halves against 94 (8 to 94
  across runs). `PS2X_FRAME_CAP=0`: 90.3 GS frames and 112.5 game frames per second. The frame end sleeps on the EE
  thread (the game's own wait was a spin, so no other guest thread ran there either). `SC3_FRAME_SLEEP=1` sleeps
  the main thread instead (PS2Recomp `EeScheduler::waitHostDeadline`, the JC2 frame end) while the other guest
  threads run. Regress route, same build, back to back: 59.80 fps against 59.89, 6 frames over 25 ms against 10,
  9 empty ADX halves against 28, but EE time ran up to 2.8 s ahead of real time in the menus (18 ms without), since
  an idle EE skips from event to event while the main thread sleeps; uncapped 71.6 fps against 74.8 and 95 frames
  over 25 ms against 38 (that machine was busy: 90 fps uncapped when idle). So it stays off.
- **IRX modules recompiled** (`PS2X_IOP_NATIVE=1`, set by the override unless already set; `=0` interprets them).
  `scripts/iop_recomp.sh` boots with `PS2X_IOP_DUMP_MODULES` and runs PS2Recomp's `tools/iop_recomp.py` on each
  module into `generated/iop/`: CRI_ADXI 302 functions, MCMAN 202, PADMAN 136, LIBSD 59, SIO2MAN 49, NUSOUND 28,
  MCSERV 24, SDRDRV 13, none left to the interpreter. Regress route on par with the interpreter: 58.20 fps, 17362
  AutoDMA transfers, 14 empty halves (interpreted: 58.13 fps, 17364, 17).
- **VIF1 worker with the GS lag** (`setVif1Threaded(true)`, `setGsLag(true)`). VU1 took about 42% of the game
  thread in fights. VIF1 and the VU1 microcode now run on a worker that runs ahead of the EE on its own EE-cycle
  clock; the EE blocks only when it reaches a point past the busy worker's clock, and GS events raised by worker
  packets are scheduled at the worker's cycle, so GS drawing overlaps VU1. `PS2X_VIF1_THREAD=0` keeps both on the
  EE thread.
- **GS draw rates 8 pixels per GS cycle** (`setDrawRates({256, 32})`: 256 EE cycles per 1024 pixels, 32 per
  primitive). SIGNAL/FINISH latencies follow the estimated GS drawing since the previous event
  (`GsDrawEstimate`). At the default 4 px/cycle the modelled drawing of a fight frame took about 94% of a field
  and the game fell to ~45 fps in emulated time.

### Movies (CRI Sofdec on the host)

Sofdec decodes through libmpeg and the IPU, and the runtime's MPEG emulation is not fed by CRI's bitstream
callbacks: a started player blocks the main thread in `sceMpegGetPicture`. So players are never started:

- all ten `mwPlyStart*` entry points are replaced. `mwPlyStartAfs` / `mwPlyStartAfsLp` on AFS partition 1
  (`MOVIE.AFS`) start an FFmpeg decoder (`SfdPlayer`) on that SFD file for the handle; any other start reports
  its end straight away;
- **full-screen movies** (width >= 512: logos, THX, opening, intro, attract) replace the GS output through the
  runtime's presentation override, and their ADX soundtrack is mixed into the SPU2 output, whose length is the
  movie clock;
- **smaller movies** (menu previews, 128x96 and 256x192) are handed to the game by `mwPlyGetCurFrm` (0x3AB630) as
  IPU CSC output, RGBA32 16x16 macroblocks column by column, written into the player's unused work area (handle
  +0x20, the SFD handle at +0x3C marks its end). Alpha movies (handle +8 == 3) carry their alpha as the luma of
  the frame's bottom half. `mwPlyRelCurFrm` and `mwPlyIsNextFrmReady` are answered for host players;
- `mwPlyGetStat` (0x3AE9F8) and the movie task's poll (0x3AE988) report PLAYING until the decoder is drained,
  then PLAYEND (status 4 for handles without a host player, the task's normal exit); `mwPlyStop` (0x3AD870) stops
  the decoder, which is how Start skips a movie;
- **movie task end** (0x218600): a task that never started playing ended without clearing the movie job's
  "playing" word (scratchpad 0x70000030 -> +8), so the job waited forever; the override clears it.

`PS2X_SC3_NO_MOVIES=1` skips the one-shot movies only. Screens built on a looping movie wait for its frames: the
Tales of Souls loading screen before the last episodes stayed on a white NOW LOADING when those were skipped too.

### Native VU1 output (`runtime/sc3_native.cpp`, native layer 5)

On by default with the native GS renderer (`SC3_NATIVE_VU1=0` runs the programs on VU1). PS2Recomp's `Vu1StreamAdapter` reads every VIF1 stream at its DMA kick and decides, for each
MSCAL, whether VU1 runs the program or the adapter's own VU1 (the sandbox, the same AOT code) runs it and draws its
XGKICK packets through `GifToNative` into the GS targets, at the program's point of the GS stream.

- SC3 has two VU1 programs, A (1177 pairs) and B (1352 pairs), both loaded at VU address 0 and swapped 23 times a
  fight frame, so a start address alone does not say which one runs. The override hashes the resident code with the
  hash the VU1 AOT names its fragments by (A `95e81f9a833c1b45`, B `d2fbf7af837a8df4`), again only after an MPG
  (`Vu1StreamAdapter::codeVersion`). Nothing else ever runs on VU1 in the regress route.
- `SC3_NATIVE_VU1=observe`: the adapter sees every chain, every program stays on VU1; `[sc3:vu1]` every 5 s counts
  MSCALs by program and start address. Over the regress route the adapter's total is the state dump's `vu1 mscal`
  less the last unreported 5 s (6071933 against 6077558), character select and loads included, and per frame they
  agree in the fight (974.0 against 974.4). 58.37 fps, as without it.
- `SC3_NATIVE_VU1_HIDE=<A:0x10,B:*,...>` (or `@file`, read at each frame end): those programs skipped and not drawn.
  `REGRESS_PAUSE_HIDE` takes a pause still per entry. What each draws in Free Training:

  | Entry | MSCALs per frame | Draws |
  | --- | --- | --- |
  | A:0x10 | 178 | the stage's main geometry (floor, walls, balustrade); hiding it also loses the fighters |
  | A:0xd8 | 71 | distant stage props (pillars, lamps, background pieces) |
  | A:0x2b0 | 77 | stage props near the camera (balustrade top, shadowed parts) |
  | A:0xc18 | 11 | the sky and far background |
  | A:0x1558 | 8 | a floor layer (the glossy tiles) |
  | A:0x1730 | 13-17 | the floor's reflection (fighters and balustrade mirrored) |
  | A:0x1ab8 | 56 | weapons |
  | A:0x1c80, A:0x1f00 | 20, 37 | the weapons' and pillars' environment-mapped shine |
  | A:0xa48, A:0x1d28 | 7, rare | nothing visible in the paused scene |
  | B:0x1388 | 313 | the fighters' skinned bodies |
  | B:0x10 | 58 | the fighters (hiding it removes them entirely, 0x1388 included) |
  | B:0x15e8 | 60 | fighter parts (heads, sleeves, coat panels) |
  | B:0x350, B:0x5b0, B:0x1990, B:0x1ca8, B:0x1ef0 | 10, 5, 8, 27, 15 | small fighter parts, not separable at this size |

  Hiding a program also skips what it leaves in VU memory for the next ones, so a hidden setup program takes later
  ones with it.
- `SC3_NATIVE_VU1=1` (the default): every program runs off VU1 (in C++ since the ports below; on the sandbox with
  `SC3_NATIVE_VU1_PORT=0`), wherever VU1 would run it (menus, character select, attract
  fights, Character Creation, fights), the draw-only DIRECTHL packets too (none qualify: every DIRECTHL of a fight
  carries register setup). `SC3_NATIVE_VU1_SWITCH=<file>`
  (1 sandbox, 0 VU1, read at each frame end) and `REGRESS_NATIVE_VU1_AB=1` compare the two on the frozen pause:
  `pause_native_psnr` against `pause_floor_psnr` (two VU1 stills of the same pause). `SC3_NATIVE_VU1_ONLY` keeps the
  programs it does not list on VU1.

With the native GS renderer the sandbox's packets go to the GS back end itself (`Config::gsWrites`, TTA's
`tlotr/native-vu1-gs`): each batch carries the register writes its packet made (TEX0 with its CLUT load, CLAMP, TEX1,
ALPHA, TEST), replayed at the group's marker, so textures, CLUTs, regions, mipmaps and blending are the back end's,
in GS stream order. With S, T and Q kept as the packet had them (`sc3/native-gif-q`: U / V had been interpolated in
screen space), the pause still on the sandbox matches VU1: 34.6 dB against a floor of 34.4 dB, the floor strip under
the fighters drawn. Weapon trails, hit sparks and guard flashes show in captures every second over the fight.
`SC3_NATIVE_VU1_GSWRITES=0` draws through the native renderer's own pass instead (20 dB, after the
`sc3/native-gif-blend` fixes: Ad blends, A == B opaque, As stored, TFX / TCC, REGION_REPEAT / REGION_CLAMP windows).

The adapter runs on the VIF1 worker (`Config::onVif1Thread`, PS2Recomp `sc3/native-vu1-worker`;
`SC3_NATIVE_VU1_THREAD=0` keeps it on the game thread): VIF1 hands it the stream up to each MSCAL as it gets there, so
the sandbox runs each program at its place among VIF1's commands, on VU1's own memory (no second unpack), and moves
the worker's clock on by the program's cycles as VU1 did; the frame end's switch, hide file, report and publish are
queued behind the frame's transfers. Free Training, 30 s fights:

| Build | Capped | Uncapped fight | EE waits for VIF1 (per 5 s) |
| --- | --- | --- | --- |
| VU1 | 59.6-59.8 | 63.0 (53.7-68.4) | 1.4-1.6 s |
| sandbox on the game thread (before) | 40.9-42.3 | 43.1-43.4 | 1.2 s |
| same, `PS2X_VIF1_SLACK=1e8` | 43.8 | 45.7 | |
| on the VIF1 worker, clock moved at the gates | 37.7 | 40.7 | 3.0 s |
| on the worker, clock moved as each program runs | 53.2 | 54.2 | 2.7 s |
| on the worker, stream handed up to each MSCAL | 58.5 | 55.9 | 2.0 s |
| same, on VU1's own memory | 59.7-59.8 | 54.3-55.1 | 1.9 s |

Uncapped, the sandbox stays about 8 fps under VU1: its GifToNative work (about 1 ms a frame) now runs on the VIF1
worker, where VU1's packets went to the GS thread. The pause still matches VU1 (33.9 dB against a floor of 33.1).
Every screen of the regress route, the title's attract fights and Character Creation look the same on the sandbox
as on VU1 (stills on each side of a `SC3_NATIVE_VU1_SWITCH` flip; menus animate, so compared by eye).

The adapter sees about 300 k fewer MSCALs than VIF1 runs over the route, all in the load after stage select (the
same on the game thread): those programs stay on VU1 and draw as before.

The title screen was the one difference left: on the sandbox its background stayed dark navy, without the blue
glow and the flash of the logo. Programs moved to the sandbox one at a time (`SC3_NATIVE_VU1_ONLY`) put it on A:0x10;
a dump of that program's XGKICK packets showed draws under TEST 0x53001, an alpha test that never passes (ATST
NEVER) with AFAIL RGB_ONLY: the GS writes their colour and keeps alpha and Z. `GifToNative` dropped them as it drops
KEEP. Fixed on PS2Recomp `sc3/native-vu1-title` (ZB_ONLY now goes to the GS too, which masks the colour): over
seconds 21-24 of the title the frame's mean luminance is 168/172/180/194 on both sides (150-153 before) and the
captures' PSNR against a VU1 run equals that of two VU1 runs (26.4/22.0/18.9 dB). With only A:0x1f00 on the sandbox
the screen had turned white; the same fix ended that.

Checked against VU1 with the sandbox on, by stills on each side of a switch and by one capture a second over whole
runs on each side: the Chronicles of the Sword story, briefing, field map, march, the battle (intro, fight, K.O.,
BATTLE RESULT) and the field map after it; three Tales of Souls episodes (Valentine Mansion, Lakeside Coliseum,
Clock Tower: story pages, map, intros, fights, YOU WIN); the boot movies played through (Produced by NAMCO and the
title movie, twice each). Nothing else differs.

Full regress, Free Training, 60 s fights, three runs a side (fight frames over 25 ms counted over the whole run):

| | Capped fps | Uncapped fight fps | Frames over 25 ms | Empty ADX halves | Error lines |
| --- | --- | --- | --- | --- | --- |
| sandbox | 59.45-59.50 | 55.4-56.2 | 4-5 (uncapped 10-33) | 12-25 | 6 (the usual Sofdec / ADX ones) |
| VU1 | 58.65-59.72 | 61.7-63.1 | 5-12 (uncapped 8-17) | 0-18 | 6 |

With the title fix and the sandbox as the default: 59.42 capped, 54.9 uncapped (VU1: 59.45, 61.8).

#### B:0x1388 in C++ (`runtime/sc3_vu1_port.cpp`)

The fighters' skinned bodies, about 313 runs a fight frame of 3-58 vertices (1.9 bones each on average), go
through `Choice::Port` (`SC3_NATIVE_VU1_PORT`, on by default; `=0` the sandbox, `=check` both, compared). What the
program does, from its microcode (`vu_disasm.py` on the code image) and the AOT blocks:

- TOP holds the GIF tag (NLOOP | EOP, PRE with PRIM 0x5c: a textured, Gouraud, alpha-blended strip; NREG 3: ST,
  RGBAQ, XYZF2), then per vertex 1-5 influences of two qwords (position times weight, w the weight; normal times
  weight, w the bone's palette address, bit 15 on the last), the ST qword (w: the vertex's flags, bit 15 ADC) and
  the colour as floats.
- Skinning, in place from TOP + 1: position = sum of bone (four columns, the palette at VU 0) times (x, y, z, w),
  then through vf28-vf31 to GS screen space times w; normal = sum of the bones' 3x3 times n. Four qwords out per
  vertex: ST and flags, colour, normal, position.
- Lighting and projection, one vertex behind its loads (software pipelined; the first iteration's output lands at
  TOP + 0xec and the tag overwrites it): Q = 1 / w, STQ = (ST times vf15, 1) times Q, XYZ = FTOI4 of the position
  times Q; light = vf20 + vf21-23 times max(vf25-27 . n, 0); RGBA = FTOI0 of min(colour times light + vf10 times
  128, vf17.x), vf10 a clamped (vf16 + n'.w) term through vf24; F = 0xff (vi3 = 0xff0). ADC when the vertex's flag
  says so or the guard-band test ((vf18 + xyz / w) times vf19 against +-1, CLIP) fails for any of the last three
  vertices (FCAND 0x3ffff).
- The packet goes at TOP + 0xee, XGKICKed; a packet another program left at TOP + vi6 is kicked first (never in the
  fights recorded).

The C++ keeps the interpreter's float semantics (operands normalized, the VU's operand alignment in additions,
round toward zero, out-of-range results through the same double classification), its Q and clip flag timing
(the DIV lands at the MULq seven pairs on, FCAND sees the CLIP four pairs before it), and leaves the registers,
clip, MAC, status (ZS and SS must already be set, else the sandbox runs it) and cycles (33 + 43 per vertex + 10
per bone, plus 2 per qword of a pending kick) as the program does. Each vertex runs without range checks and again
on the exact path when a value leaves 2^-60..2^120.

Checked bit for bit (packets, VU memory, every register, clip, MAC, status, cycles): 1500 runs recorded from a
fight with `PS2X_VU1_RECORD_PC=0x1388` replayed by `sc3_tests` (`SC3_BUILD_TESTS=ON`, `SC3_VU1_PORT_TRACE`, both
paths), a synthetic batch against the interpreter's digests, and `SC3_NATIVE_VU1_PORT=check` over the whole
regress route: 1,960,411 runs, no difference. The pause still on the port matches the sandbox's in the 3D (the
`REGRESS_NATIVE_VU1_PORT_AB` diff is the blinking pause menu only).

A Soul Arena mission later found a difference the regress route never shows: lights around 1e37 saturate the
colour product to FLT_MAX, and the last MADD adds vf10 times 128 with vf10 at 0. The interpreter decides O on the
long double sum, exactly FLT_MAX, so no overflow; the port set O (MAC 0xe000, status bit 3) for any result that
landed on 0x7f7fffff. It now takes the long double sum too (`vu1-maxflag`). The 4,340 runs recorded in that mission
(`SC3_NATIVE_VU1_RECORD`, check mode records only the mismatches) replay without a difference, and `sc3_tests`
has a synthetic batch with the lights at 1e37 against the interpreter's digest.

Cost: replaying the 1500 runs, 8.1 us a run against the interpreter's 19.5 (10.8 on the exact path). Profile of
the VIF1 thread over 15 s of an uncapped fight (same binary): B:0x1388's AOT blocks 2174 samples on the sandbox,
754 for the C++; the thread from 7807 to 6977 samples while drawing more frames, the EE's waits for it from 2079
to 1851. Regress, Free Training, three or six runs a side:

| | Capped | Uncapped fight |
| --- | --- | --- |
| sandbox | 58.43-58.70 | 52.2-55.3 (mean 53.9) |
| port | 58.97-59.30 | 51.7-56.5 (mean 54.4) |
| VU1 | 59.28 | 58.8 |

Across runs the stage changes and the spread hides the gain; inside one uncapped 90 s fight, switching between the
two every 10 s (`SC3_NATIVE_VU1_PORT=@file`, the first 5 s after each switch left out), three fights: sandbox 53.4 /
54.5 / 54.2 fps, port 55.5 / 56.0 / 55.9, about 1.8 fps more.

What is left between the sandbox and VU1 is not the microprograms: on VU1 the same AOT code runs on the same VIF1
thread, and its packets go to the GS thread. On the sandbox the VIF1 thread also turns them into native batches
(`GifToNative`, about 730 samples) and keeps the texture cache (`TextureCache::uploadLocked` and `WriteCT32`, about
1050). The next step for B:0x1388 is batches straight from the port, without building and reading back its GIF
packet.

#### Programs A and B in C++ (`scripts/vu1_port_gen.py`, `generated/vu1port/`)

Every MSCAL of A and B runs as C++ by default: A's 20 start pcs (the setups at 0x10, 0x1c80 and 0x2098, which load
the constants and JR to a routine whose address the stream left in VU memory, and the 17 routines) and B's 18 (setups
at 0x10, 0x1990, 0x21e0 and 15 routines). B:0x1388 called directly keeps its hand-written port above while the
status holds sticky Z and S. `SC3_NATIVE_VU1_LOG` names entries with their JR target (`A:0x10>0x2b0`) and prints a
line of MSCALs left on the sandbox whenever there are any: over everything checked below, none.

`scripts/vu1_port_gen.py` writes the C++ from the microcode `ps2_vu_aot` compiled; `scripts/recomp.sh` runs it, so,
like `generated/`, nothing derived from the game is in the repository.

- Each instruction becomes the operation it does on the VU registers, held in locals, with the interpreter's float
  rules (operands normalized, the PS2 addition, round toward zero, MAX/MINI on raw bits, FTOI, CLIP). A result or an
  FMAC product outside the normal range goes through the interpreter's own classification (`Vu1Lifted::fmacFlags`,
  PS2Recomp `sc3/native-vu1-all`).
- What VU1Interpreter's pipeline makes depend on timing is worked out by the generator, for every path from every
  start pc, with the pipeline state as part of where a path is: each pair's stall (VF/VI/ACC latencies, the DIV
  unit), when a DIV's Q becomes visible to MULq, which CLIP an FCAND sees and which FMAC an FMAND sees, the old VI a
  branch reads right after an IALU write, the cycles. A loop's first iteration and its steady state become separate
  code where their timing differs: 1675 timed pairs for A's 1177, 1861 for B's 1352.
- XGKICK: a PATH1 transfer of two cycles a qword; the next XGKICK and the program's end wait for it (the only timing
  the data decides). A store while a transfer may still read VU memory (A at 0x1c60 writes the next packet's tag)
  lets it catch up first, as the interpreter does.
- MAC flags are computed only where an FMAND or the end state reads them, sticky Z and S only while the status does
  not hold them yet; U and O only arise on the exact path.

Checked bit for bit (packets, VU memory, registers, Q and I, clip, MAC, status, pc, cycles):

- 15,059 runs recorded from the sandbox over the regress route (`SC3_NATIVE_VU1_RECORD`, replayed by `sc3_tests`
  with `SC3_VU1_TRACE`): no difference.
- `SC3_NATIVE_VU1_PORT=check`, the port and the sandbox on every MSCAL, compared, with no MSCAL left on the sandbox:
  the regress route (5.35 M MSCALs), the attract demo idling on the title (5.80 M), Tales of Souls through three
  episodes (5.95 M), Chronicles of the Sword (6.85 M), VS Modes (4.42 M), the Shop (4.50 M) and Options (0.56 M):
  33.4 M MSCALs, no difference. Entries seen: every start pc of A but 0xda8, 0xf68, 0x2098/0x2140 and 0x2330, of B
  but 0xd8, 0xb78, 0xdd0, 0x10f0, 0x21e0/0x2290, 0x24f8 and 0x2740 (ported all the same, never run in these modes).
- Check mode with both logs over the other modes, driven live through `PS2X_PAD_INPUT` with the Tales of Souls save
  on the memory card: Character Creation (a new character through every tab and most equipment slots), Museum
  (Character Profiles, the character viewer with its stage and camera), Soul Arena (Quick Play through eight
  stages, five missions with two fighters), World Competition (tournament rounds), Tales of Souls to episode 6,
  every character's three costumes and weapons on the character select, and Free Training on every stage of the select grid with a new
  opponent each: 268.8 M MSCALs and 1.79 G VU0 calls. The only difference was B:0x1388's O flag above (4,340 runs,
  one mission); with the fix, 159.4 M MSCALs of those modes again without one. The entries left:

  | Entry | Calls | Reached in |
  | --- | --- | --- |
  | A:0x450 (A:0x10>0x450) | 13,396 | Character Creation's model and job previews, Soul Arena |
  | A:0xf68 (direct and A:0x10>0xf68) | 215,064 | most stages' fights: Soul Arena, World Competition, Free Training |
  | B:0xd8 (direct and B:0x10>0xd8) | 463,170 | Lizardman and others in World Competition, Soul Arena missions, Free Training |
  | B:0xb78 (direct and B:0x10>0xb78) | 239,847 | Nightmare and others, the character select, fights |
  | A:0xda8, A:0x2098/0x2140, A:0x2330 | 0 | never |
  | B:0xdd0, B:0x10f0, B:0x21e0/0x2290, B:0x24f8, B:0x2740 | 0 | never |
  | VU0 0x18, 0x438 | 0 | never |

  B:0x21e0's routines are the 0x1990 family's (fighter parts) with ST from the normal instead of lit colours, an
  environment map; A:0x2098 is a copy of A:0x1c80's setup (the weapons' shine) for 0x2140 and 0x2330. VU0 0x18 is
  the 16-bit angle sine at 0x100a18 (reached from 0x100aa8, cosine), 0x438 a test from the VU0 macro-mode copy of
  the VU1 pipeline at 0x4437c8-0x444450; neither EE path ran. Not tried: Chronicles past its first battles, Tales
  of Souls' last episodes and credits (`PS2X_SC3_AUTOWIN` lost episode 6 to a ring out), unlockable characters and
  Museum items this save does not have, Critical Finish.

Cost, replaying the route's runs on stallman: 2.5-8 us a run for A's entries against the sandbox's 5.3-13.5, 7-16 us
for B's against 13-30. Regress, Free Training, two runs a side, on the same runtime (PS2Recomp 4ebc8e3a), every
program in C++ (VU1 and VU0) against every program on the sandbox and VU0 on its interpreter
(`SC3_NATIVE_VU1_PORT=0 SC3_NATIVE_VU0=0`):

| | Capped | Uncapped fight |
| --- | --- | --- |
| sandbox and VU0 interpreter | 59.02, 59.78 | 55.72, 55.87 |
| C++ | 60.00, 60.02 | 61.43, 62.57 |

On `sc3/native-vu1` e33f7853 (the host-clock IOP and native/renderer 71524c5 merged): 60.00 and 60.02 capped,
60.98 and 60.88 uncapped.

#### VU0 microprograms in C++ (`runtime/sc3_vu0.cpp`)

VU0 runs SC3's math library as microcode (`VCALLMS` from about 15 EE functions): one program, the 172 pairs
`ps2_vu_aot` found at ELF offset 0x367a90 (VU0 code memory hash 5e40f831...), called from 0x0, 0x38, 0x60, 0xf0,
0x360, 0x3c8 and 0x3e8, about 370,000 times a second in a fight. `PS2Runtime::setVu0MicroHook` (PS2Recomp
`sc3/native-vu1-all`) hands each call to `runtime/sc3_vu0.cpp`, which runs the program as C++ written by
`vu1_port_gen.py` the same way as VU1's (VU0's 4 KB memories and branch range, plus ITOF, MR32, SQRT, RSQRT, WAITQ,
FCGET), from the state VU0 starts at (reset, the registers and flags copied from the EE context).
`SC3_NATIVE_VU0=0` gives every call back to the interpreter, `=check` runs both and compares; `SC3_NATIVE_VU0_LOG`
and `SC3_NATIVE_VU0_RECORD` log and record as for VU1.

Checked: the route's 7,732 recorded runs replayed by `sc3_tests` (`SC3_VU0_TRACE`), 0.15-0.36 us a run, and
`SC3_NATIVE_VU0=check` over the same seven runs as VU1 above: 197.4 M calls, no difference, none left to the
interpreter. 0x18 and 0x438 never ran, in those modes or in the ones above.

### Test harness and tracers

- `PS2X_SC3_AUTOWIN=1`: on each GS interrupt during a fight, the player's health (float at 0x4D36A0) is set to
  full (240) and the opponent's (0x4D36A4) to 1, so the next scripted hit wins. Used to play Tales of Souls to
  the credits with scripted input.
- `PS2X_SC3_TRACE_ENTRY=<addr,...>`: registers, a few stack slots, Timer0 and the EE cycle at each entry.
- `PS2X_SC3_TRACE_OBJ=<fn>:<off,...>`: the words at those offsets of the object in `$a0` whenever they change
  (state machines such as the Art Gallery task, state at +0x130).
- `PS2X_SC3_RATE=<addr,...>`: calls per 60 vsyncs of each function.
- `PS2X_SC3_TRACE_PACING` (`_ALL`, `_SPAN`, `_FULL`): the frame limiter's Timer3 waits (0x148098).
- `scripts/regress.sh` ends in the Free Training pause menu and keeps that frozen frame as `pause.png`;
  `scripts/imgdiff.py` compares two frames (PSNR, a diff image) with Python's standard library only. Runs differ in
  stage and poses, so compare stills from one run (`REGRESS_NATIVE_VU1_AB`, `REGRESS_PAUSE_HIDE`), not across runs
  (two VU1 runs' pause stills: 13 dB). `REGRESS_SHOT_SECS=1 REGRESS_KEEP_SHOTS=1` keeps a capture a second.
  `REGRESS_FIGHT_FRAME_CAP=0` uncaps the fight only (`SC3_FRAME_CAP_FILE`): with `PS2X_FRAME_CAP=0` the menus run
  ahead of the route's presses and it ends in Character Creation.
- `SC3_TRACE_FRAMES=1`: every 5 s, the main loop's frames per second, the frames over 25 ms, the longest, EE time's
  lead over real time and the vsync count; each frame over 50 ms on its own.
- `SC3_FRAME_SLEEP=1`: native frames end on `EeScheduler::waitHostDeadline` (other guest threads run while the
  main thread waits) instead of holding the EE thread in the FramePacer.
- `PS2X_SC3_TRACE_GSIRQ=<n>` (`_VSYNC`, `_SPAN`): CSR, SIGLBLID and the VIF1/GIF chain addresses at the GS
  interrupt handler 0x10E658.
- `PS2X_SC3_TRACE_ADXT`, `PS2X_SC3_TRACE_ADXF`: ADX player status changes, AFS partition loading.
- `PS2X_SC3_TRACE_TASKS`, `PS2X_SC3_TRACE_MOVIE`, `PS2X_SC3_TRACE_FADE`: task creation with names (0x128B30),
  movie task and job states and, every 5 s of a full-screen movie, its pictures and the window's presents per second
  against the movie clock, the fade driver (0x1A2960).
- Experiments kept behind variables: `PS2X_SC3_FORCE_TEXT` (start texture-font fades that never started),
  `PS2X_SC3_ADX_PLAYEND` (report PLAYEND for ADX players stuck in DECINFO). Neither is needed today.

Recompiled functions return early at scheduler checkpoints with `ctx->pc` inside their body, so a wrapper around
a guest function may only read state on entry, never `$v0` after the call. Overrides registered with
`registerFunction` only catch dispatched calls, not direct calls between recompiled functions.

## Runtime fixes found with this game

All of them are generic and on boubou666's `main`, which the submodule follows; there is no SC3 branch any more.

### EE, kernel and libraries

- **Frame budget calibration (30 fps instead of 60).** SC3 measures at boot how many Timer0 counts pass in 256
  Timer3 (HBLANK) changes, reading Timer3 twice per iteration, and sets its frame budget from that. Reading an
  unchanged timer charged one tick, applied only at the next scheduler checkpoint, so each further poll before it
  added another: the game measured Timer0 at twice its rate, set a two-field budget and ran fights at 30 fps.
  Busy-wait polls now owe a single tick until it is applied (f7a61cdf).
- Timer 3 counts HBLANKs from reset; thread priority 0 accepted; `ChangeThreadPriority` returns the old priority;
  `sceDmaRecvN`.
- **vsprintf**: one 64-bit va_list slot per argument (button help showed "Confirm (null)"); a null format pointer
  formats as the string at address 0 (the Chronicles field map draws a label whose pointer is 0).
- **CD drive timing**: 30/100 ms seeks by distance and a CAV transfer rate; `sceCdSync` reports a read busy for as
  long as the drive takes and a blocking `sceCdSync(0)` waits across vblanks. The Museum's Art Gallery stayed black
  because its loads finished before the Museum's 30-frame fade-out, which then finished last (screen brightness is
  the float at 0x4690D4).

### Recompiler

- MMI variable word shifts (PSLLVW/PSRLVW/PSRAVW) had their operands swapped (broken menus).
- A trailing branch delay slot at a function boundary was dropped when another function started on it (0x1DFF00
  lost `addiu sp,0x30`, crash with ra=1); no function starts in a delay slot any more, which merged 14 former
  split functions (0x200000, 0x1E00F0, ...) into their callers.
- SQRT.S took sqrt($f0) instead of ft (56 sites, among them the game's vector length 0x103000 and distance
  0x103058).
- **VU0 macro VCLIP** put -x/-y/-z in the even clip bits and judged against w instead of |w|. Stage zone culling
  (0x1072B0 -> 0x1087B0, CLIP then `CFC2 vi18 & 0xAA8`) hid every stage part in front of the camera: the first
  Chronicles battle (stage 3, the snow courtyard) and the field map terrain drew only the clear colours.

### IOP and audio

- **Module start**: IRX modules start with loadcore's `start(argc, argv)` and sysclib's `look_ctype_table` is
  implemented. Without them CRI_ADXI never saw `spucore=2` and the game stopped core-0 output every frame: no ADX
  music.
- **CpuSuspendIntr ignored (Chronicles DECISIVE BATTLE stall).** The IOP scheduler delivered interrupts and let
  higher-priority threads run while a thread had interrupts suspended. CRI_ADXI's DTX thread (priority 48) and
  PSM thread (priority 39, woken by the SPU2 ADMA interrupt) share stream-joint chunk lists, and about one
  exchange in 17,000 lost a chunk. After a BGM stop the EE side's restart waits for the whole 0x4000 ring, so the
  player stayed in PREP forever and Chronicles froze on the DECISIVE BATTLE banner. No interrupt dispatch while
  interrupts are suspended, and the masked thread keeps the CPU across slices (64181c70).
- **Audio "frying".** Three causes. The IOP scheduler let a lower-id thread of equal priority preempt the
  running one at every slice end, so CRI thread 7 cut into the ADX mixer thread 10 (both priority 39) and the
  right channel sent stale samples; equal priorities now keep the CPU until they block (389bf5a). Calls the EE ran
  inline on the IOP were never charged against the cycles the EE paid for, so the SPU2 produced 0.9-2.5% more
  audio than real time (fffb0c0). The host time stretcher spliced on every burst of that jitter; it now plays
  contiguous grains while the stream is at pace (0177887). Over the regression route the SPU2 now produces
  48,084 frames per second for 48,112 taken, at a stretch rate of 1.00 throughout.

### GS, DMA and VIF

- **GS interrupt handshake.** SC3 paces its frames on SIGNAL/FINISH interrupts: CSR.FIFO reads empty only once
  the GS has drawn what it was sent, INTC/DMAC causes are latched, the GS interrupt is level-triggered, and
  SIGNAL/FINISH/LABEL are ordered. Their latency follows the estimated drawing time since the previous event
  (`GsDrawEstimate`, scissored primitive area plus a per-primitive cost): a fixed 200,000-cycle latency held the
  heavy attract stage (Chamber of Time) to 20 fps, while shorter fixed ones starved the loader thread and left the
  NOW LOADING overlay over the attract demo. Loading screens draw few huge primitives and keep long latencies.
- **GS event deadlines on the vblank timeline.** GS events were paced by emulated cycles and by a host deadline of
  now plus latency, so the EE slept about 0.9 s in every 5 s of a fight. Their host deadlines now sit on the vblank
  timeline, and GS interrupt checks use an absolute cycle.
- **Wall-clock catch-up** can be bounded to once per field or turned off (`WallClockCatchUp`); with it off a late
  vblank restarts the field timeline (8f08ca89), see the override above.
- **VU1 run-ahead**: the VIF1 worker on its own EE-cycle clock with the GS lag, GS events of worker packets at
  the worker's cycle, a GIF DMA queued under the GS lag completing its channel when PATH3 goes to the worker.
- **REGION_REPEAT drains.** SC3 draws its fighters with 1024x1024 region-repeat textures masked to 256 texels.
  Counted whole, their texel span wrapped through VRAM into the display buffer, so every fighter strip looked like
  a primitive sampling its own target and was drawn serially after a full raster drain (about 17,000 drains per
  5 s in Free Training). A region repeat now samples exactly the texels between FIX and MSK | FIX.
- GIF tag continuation across PATH3 DMA transfers (the frame copy is one REGLIST tag of 280 sprites in 10
  transfers; without it a stale 64-pixel strip showed on the left), CLUT loads deferred to the raster workers.

### VU

- VU1 interpreter split into inline upper/lower/step headers, dead MAC and status flags skipped, FMAC arithmetic
  specialized per instruction; the EFU opcodes were off by one (0x79 is ERSQRT, 0x7C ESIN, 0x7D EATAN, 0x7E EEXP).
- **Compiled microcode: `ps2_vu_aot`.** The port first compiled its microprograms with its own `ps2_vu1_compile`,
  from code images dumped while playing, into single-entry segments with static stall elision (about 5.8 ns per VU
  cycle against ~20 for the interpreter). It now uses main's `ps2_vu_aot`, shared with the other ports, which reads
  the microcode straight from the ELF and compiles blocks with the stalls scheduled at compile time and the VU
  registers in locals. VU0 microprograms are compiled too (their flags stay exact: the EE reads them with CFC2).
- **Sticky-flag elision (3c1da2df, fc88d9b8, e27fba56).** `ps2_vu_aot` skipped the MAC and status writes no reader
  sees only when no microprogram of the ELF can read the sticky status bits. SC3 has FSEQ readers, so all 3,282 of its compiled VU1
  FMACs computed and queued exact flags with a clock advance before each, and they took most of the VU1 thread,
  which is why the port kept its own compiler. Such an FMAC now keeps only what it adds to the sticky bits, folded
  in before any flag write a reader sees, which brought `ps2_vu_aot` to the speed the port needs and let it move
  to plain `main` (with the earlier block-exit, rounding and operand normalization work, e73aba08).
- The UNPACK fast path, XGKICK copying whole elapsed qwords, PATH1 catching up only from where it last did.

### Diagnostics added for this port

`PS2X_STATE_DUMP`, `PS2X_CALL_TRACE`, `PS2X_WATCH`, `PS2X_PAD_SCRIPT` and `PS2X_PAD_INPUT`, `PS2X_FRAME_DUMP`,
`PS2X_EE_PROFILE`, the Linux `PS2X_PROFILE` sampler, `PS2X_GS_DUMP_AT` / `PS2X_GS_DUMP_WHEN`, the IOP tracers
(`PS2X_IOP_TRACE_PC`, `PS2X_IOP_COUNT_RANGE`, `PS2X_IOP_WATCH`), VU1 record and replay, and the EE speed in the
F3 overlay.

## Status

Every mode has been played through with scripted input: Tales of Souls to the credits and the
first save, Chronicles of the Sword (story, editor, briefing, field map, battles, results), World Competition, Soul
Arena, Museum, Character Creation, Shop and Options, with saves loaded back after a restart. Movies play with
sound and skip with Start.

Speed (Ryzen 7 7800X3D, 8 cores): Free Training on a fixed stage runs 298-300 GS frames per 5 s (60 fps) with the
EE at 100% of real time (~1474M cycles per 5 s) and no audio underruns.

Things that look wrong but are the game:

- the Tales of Souls credits run at ~15 fps: the EE runs at full speed with no GS waits, the game paces them;
- Soul Arena fighters tinted in cycling colours, or gold: the Soul Charge and Guard Impact effects;
- the game blends each frame 70/30 with the previous one, which shows as ghosting at low frame rates.

Open:

- The Chronicles field map and its battles dip to ~53-55 fps, with choppy sound when units move on the map.
- Heavy random stages of the attract demo run with the EE at ~89% of real time, bound by the VU1 worker.
- The Single Player menu preview panel shows noise (seen with `PS2X_SC3_NO_MOVIES`; previews are fine with
  movies on).
- The root cause of the ordering-table double link behind the old menu-transition freeze was never found: it no
  longer reproduces since catch-up is off and the GS/CD timing fixes, but other timings may bring it back.
- Not reached yet: widescreen.

The port runs on the native target the other ports use, through PS2Recomp's `sc3/native` integration branch: the
native GS renderer (`GSNativeBackend`, OpenGL) with the SC3 fixes for pixel centres and the 64-pixel seams, HIGHLIGHT
alpha, Ad blending, the Z buffer a draw's ZBUF names, and the character shadows (COLCLAMP off wraps blends, a colour
buffer read as Z keeps the Z block order, untextured draws at their own colour); the SPU2 on the wall clock, kept within 512
samples of the IOP clock; the eight IRX modules recompiled (813 functions, `scripts/iop_recomp.sh`). The regression
route runs at 58.2 fps, and at 58.1 fps with `PS2X_GS_SCALE=2` (1280x896).

## Debugging recipes

Paths below are relative to the repository; `scripts/run.sh` passes extra environment through.

- **State dump**: `PS2X_STATE_DUMP=5` prints every 5 s the EE and IOP threads, GS frames, EE cycles, DMA and VIF
  counters, VU1 cycles, host idle and worker waits, SPU2 state. Benchmark lines: GS frames per 5 s from
  `[state] gs frames`, EE speed from `grep -o '\[state\] ee cycle=[0-9]*'` (differences of ~1474M per 5 s = 100%).
  `[MC]` log lines lack a newline, so anchored `^\[state\]` greps drop some state lines.
- **Live input**: `PS2X_PAD_INPUT=<file>` presses the buttons of each line appended to the file while the game
  runs, one word every 0.35 s (`echo "down down cross" >> file`, `cross+up` presses both); `PS2X_PAD_SCRIPT=t:BUTTON[/ms],...` schedules presses in seconds from start. `PS2X_PAD2_SCRIPT` takes the same format for player 2 (second controller, SIO2 port 1). In zsh,
  write `"${t}:cross"`, not `"$t:cross"` (`:c` is a history modifier). Route to Free Training: Start x4 on the
  title, Cross (Single Player), Down x4 + Cross (Practice), Down + Cross (Free Training), Cross to pick the
  characters and stage (stage select defaults to Random; Right picks a fixed one).
- **Frame dumps**: `PS2X_FRAME_DUMP=<dir> PS2X_FRAME_DUMP_EVERY=<s>` writes the presented frame as raw 640x448
  RGBA (`magick -size 640x448 -depth 8 rgba:<file> out.png`). They fill a disk quickly and cost time themselves.
- **GS dumps**: `PS2X_GS_DUMP_WHEN=<file>` logs the next `PS2X_GS_DUMP_COUNT` primitives (with the VU1 program
  that kicked them) to `PS2X_GS_DUMP_OUT` once that file appears, `PS2X_GS_DUMP_AT=<s>` after a delay; on the CPU
  renderer `PS2X_GS_DUMP=<dir>` also writes decoded textures and VRAM. Comparing a dump of a broken scene with a
  good one found the missing Chronicles stage (VU1 programs and big textured triangles only in the good one).
- **Profiler**: `PS2X_PROFILE=<s>` (`PS2X_PROFILE_DELAY`, `PS2X_PROFILE_OUT`) samples every host thread on its CPU
  clock; `PS2X_EE_PROFILE=1` adds EE cycles by guest resume pc to the state dump (odd keys are timer-poll spins).
- **Watches and traces**: `PS2X_WATCH=addr[:n],...` and `PS2X_IOP_WATCH` print guest words in the state dump;
  `PS2X_CALL_TRACE=1` traces calls including indirect ones; the `PS2X_SC3_TRACE_*` tracers above.
- **VU1**: `PS2X_VU1_RECORD=<file>` records microprogram runs for `ps2_vu1_replay` (checks and times the
  interpreter offline); `PS2X_VU1_EXACT_FLAGS=1` keeps every flag and disables the compiled code.
- **Host backtraces**: `ptrace_scope=1` only allows attaching to children, so start the runner under gdb and
  interrupt it to sample all threads.
- `scripts/sc3run.sh <name> <seconds> [ENV=value...]` runs the game for that long with a state dump, logs to
  `build/runs/<name>.log` and screenshots the window (Hyprland + grim); `scripts/threadcpu.sh`, `gprof_*.py`,
  `dis.sh`, `irxdis.sh`, `irx*.py`, `string_refs.py` are disassembly and profiling helpers.
- `patches/async_vif1*.patch`: the first VIF1-worker experiments, superseded by the runtime's VIF1 worker; kept
  for reference.
