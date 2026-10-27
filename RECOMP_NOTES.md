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
- **paraLLEl-GS preferred** (`GS::requestParallelRenderer`). The GPU renderer keeps fights at speed where the CPU
  rasterizer fell to ~20 fps under host load. `PS2X_GS_RENDERER=cpu` (or any other value) keeps the CPU path, and
  the runtime falls back to it when no Vulkan device is found.
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

### Test harness and tracers

- `PS2X_SC3_AUTOWIN=1`: on each GS interrupt during a fight, the player's health (float at 0x4D36A0) is set to
  full (240) and the opponent's (0x4D36A4) to 1, so the next scripted hit wins. Used to play Tales of Souls to
  the credits with scripted input.
- `PS2X_SC3_TRACE_ENTRY=<addr,...>`: registers, a few stack slots, Timer0 and the EE cycle at each entry.
- `PS2X_SC3_TRACE_OBJ=<fn>:<off,...>`: the words at those offsets of the object in `$a0` whenever they change
  (state machines such as the Art Gallery task, state at +0x130).
- `PS2X_SC3_RATE=<addr,...>`: calls per 60 vsyncs of each function.
- `PS2X_SC3_TRACE_PACING` (`_ALL`, `_SPAN`, `_FULL`): the frame limiter's Timer3 waits (0x148098).
- `PS2X_SC3_TRACE_GSIRQ=<n>` (`_VSYNC`, `_SPAN`): CSR, SIGLBLID and the VIF1/GIF chain addresses at the GS
  interrupt handler 0x10E658.
- `PS2X_SC3_TRACE_ADXT`, `PS2X_SC3_TRACE_ADXF`: ADX player status changes, AFS partition loading.
- `PS2X_SC3_TRACE_TASKS`, `PS2X_SC3_TRACE_MOVIE`, `PS2X_SC3_TRACE_FADE`: task creation with names (0x128B30),
  movie task and job states, the fade driver (0x1A2960).
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

Every mode has been played through with scripted input on the GPU renderer: Tales of Souls to the credits and the
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
- Reported while watching recorded runs (not yet from a human playthrough): audio "frying" and brief slowdowns.
  Those runs dumped frames every 0.25 to 1 s, which may contribute. Under investigation.
- Heavy random stages of the attract demo run with the EE at ~89% of real time, bound by the VU1 worker.
- The Single Player menu preview panel shows noise (seen with `PS2X_SC3_NO_MOVIES`; previews are fine with
  movies on).
- The root cause of the ordering-table double link behind the old menu-transition freeze was never found: it no
  longer reproduces since catch-up is off and the GS/CD timing fixes, but other timings may bring it back.
- Not reached yet: widescreen and higher internal resolution.

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
