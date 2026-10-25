# Recompilation notes

## Layout used during bring-up

- Game files extracted from your own disc to `~/Games/PS2/SoulcaliburIII/disc` (SLUS_212.16, SYSTEM.CNF, `*.AFS`,
  ROOT.OLK, IRX/, ...). The ELF is also copied next to `recomp/sc3.toml`.
- PS2Recomp checked out on branch `sc3/bringup` (a worktree at `~/repos/PS2Recomp.worktrees/sc3/bringup`).

## Rebuilding

1. Recompile (from `recomp/`, with `SLUS_212.16` copied there):
   `ps2_recomp sc3.toml` - writes `./output/`.
2. Copy `output/*.cpp` into `ps2xRuntime/src/runner/` and the generated headers into `ps2xRuntime/include/`
   (do not commit them; `register_functions.cpp` is tracked upstream as a stub).
3. Copy `runtime/sc3_overrides.cpp` into `ps2xRuntime/src/runner/`.
4. VU1 microprograms: run once with `PS2X_VU1_DUMP_CODE=<dir>` (writes each VU1 code image the runtime has
   no compiled code for), then `scripts/compile_vu1.sh <dir>` generates `src/runner/vu1_sc3.cpp`
   (`ps2_vu1_compile`, do not commit it). Without it the interpreter runs them, about twice slower.
5. Build (Linux, clang, no system cmake/ninja here so through mise):
   `cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-msse4.2`
   `ninja -C out/build ps2EntryRunner`
6. Run: `ps2EntryRunner <disc>/SLUS_212.16`, or `scripts/sc3run.sh <name> <seconds> [ENV=value...]` which logs,
   screenshots (Hyprland + grim) and stops the game after a while.

## sc3.toml

Analyzer output plus:
- libcdvd stub bindings (`sceCdSearchFile`, `sceCdDiskReady`, `sceCdGetDiskType`, ...);
- entry points found from data pointers (`scripts/find_code_pointers.py`) and from every J/JAL target that had no
  function of its own (63 helpers the analyzer had merged into neighbours, e.g. 0x3E1820 inside the stub-bound
  `sceMpegInit`). The hint lists in `recomp/` are those scans.

## Game overrides (runtime/sc3_overrides.cpp)

- Reserved runtime memory moved to 0x80000-0x100000: crt0 hands the whole heap up to the main stack (0x01FDFF00)
  to the game, so the runtime's HLE pools and invocation stacks live in kernel RAM.
- Sofdec movies: CRI Sofdec decodes through the IPU, which is not emulated. Players are never started; instead
  `mwPlyStartAfs` / `mwPlyStartAfsLp` on partition 1 (MOVIE.AFS) start a host decoder (FFmpeg) for that handle:
  - full-screen movies (opening, intro, attract) replace the GS output through the runtime presentation override,
    and their ADX soundtrack is mixed into the SPU2 output, which is the movie clock;
  - smaller movies (menu textures) are handed to the game by `mwPlyGetCurFrm` as IPU CSC output (RGBA32 16x16
    macroblocks, column-major) written in the player's unused work area;
  - `mwPlyGetStat` / the movie-task poll report PLAYING until the end, then PLAYEND; `mwPlyStop` stops the decoder.
  Movies that are not in MOVIE.AFS still report their end straight away. `PS2X_SC3_NO_MOVIES=1` skips them all.
- Movie task end (0x218600) releases the movie job's "playing" word when a movie never started.
- Tracers, env-gated: `PS2X_SC3_TRACE_ENTRY=<addr,...>`, `PS2X_SC3_TRACE_OBJ`, `PS2X_SC3_TRACE_MOVIE`,
  `PS2X_SC3_TRACE_FADE`, `PS2X_SC3_TRACE_ADXT`, `PS2X_SC3_TRACE_ADXF`, `PS2X_SC3_TRACE_TASKS`.

## Runtime fixes found with this game (branch sc3/bringup)

- EE: Timer 3 counts H-BLANKs from reset; thread priority 0 accepted; ChangeThreadPriority returns the old
  priority; `sceDmaRecvN`.
- Recompiler: MMI variable word shifts (PSLLVW/PSRLVW/PSRAVW) operand order; a branch delay slot at a function
  boundary was dropped when another function started on it (0x1DFF00 lost `addiu sp,0x30`, crash with ra=1).
- GS CPU backend: sprite coverage with fractional edges.
- IOP: modules started with loadcore's `start(argc, argv)` and sysclib `look_ctype_table` implemented. Without them
  CRI_ADXI never saw `spucore=2` and the game stopped core-0 output every frame (no ADX music).
- VU1 interpreter: about 35% faster (cheaper FMAC flags, hazard checks, decode lookups, ACC/Q/I only when read).
- Runtime hooks: presentation override and SPU2 mixer for host-decoded media.
- CD drive timing: 30/100 ms seeks by distance and a CAV transfer rate (40% at the hub). The Art Gallery fades
  in once its pictures are loaded while the Museum's 30-frame fade-out still runs; loads that ended too early
  let the fade-out finish last and leave the gallery black. Brightness is the float at 0x4690D4.
- Recompiler: VU0 macro VCLIP had the clip flag bits in the wrong order and judged against w instead of |w|.
  Stage 3 (outdoor courtyard of the first Chronicles battle, also the field map terrain) culls its zones with
  VCLIP masks (0x1087B0, mask 0xAA8) and drew nothing but the clear colours.
- Recompiler: no function starts in a branch delay slot. A full regeneration with this rule merges 14 former
  split functions (0x200000, 0x1E00F0, 0x2D0060, 0x400030, ...) into their callers; delete their old .cpp files
  from the runner when updating it.
- IOP: no interrupt delivery or preemption while a thread has interrupts suspended (CpuSuspendIntr). CRI_ADXI's
  DTX and PSM threads share chunk lists; a rare lost chunk left the BGM player in PREP after its next stop, which
  froze Chronicles on the DECISIVE BATTLE banner.
- GS: the DMA-level SIGNAL stall is turned off by the overrides (each handshake waits the modelled IRQ latency).
- vsprintf HLE: one 64-bit va_list slot per argument (button help showed "Confirm (null)").
- Diagnostics: `PS2X_STATE_DUMP`, `PS2X_CALL_TRACE`, `PS2X_WATCH=addr[:n]`, `PS2X_PAD_SCRIPT="t:button,..."`,
  `PS2X_FRAME_DUMP=<dir>`, `PS2X_DEBUG_UI=0`, `PS2X_IOP_TRACE_PC`, `PS2X_IOP_COUNT_RANGE`, `PS2X_IOP_WATCH`,
  branch history in missing-target reports, VU1 record/replay (`PS2X_VU1_RECORD` + `ps2_vu1_replay`).

## Status

Works: boot, opening and intro movies (skippable with Start), title, menus, character select, Tales of Souls story
text, fights (attacks, KO, results) with music and voices.
Full run: Tales of Souls played to the end with movies on (Siegfried, every episode, final boss Abyss, ending,
results, then the save prompt) with PS2X_SC3_AUTOWIN=1 so scripted input wins each fight; no crash, no missing
target, 30 fps outside fights. PS2X_SC3_NO_MOVIES=1 skips one-shot movies only: the loading screen before the last episodes waits for the frames
of its looping movie (it stalled on a white NOW LOADING screen when those were skipped too). With it, Zasalamel's
run also reaches Abyss, the ending, the unlocks and the save prompt.
Modes reached with scripted input: VS (standard fight), Free Training, Shop, Options, World Competition
(tournament round 1 fight), Soul Arena (stage 1 fight), Museum (Art Gallery menu, pictures and Hi-Res pages),
and Chronicles of the Sword: character editor, story text, dialogue, briefing, sortie and the real-time field
map with the command tutorial. In the editor's "Finish editing?" dialog, Left selects Yes.
Memory card (mc0/ next to the ELF): Options > SAVE & LOAD > SAVE DATA creates BASLUS-21216SC3/ (icon.sys,
normal.ico, 273408-byte save) and LOAD DATA reads it back ("Load complete!"). On an empty card the "No data
found" dialog creates data with Square (the pink icon; the dialog's button table maps only Square and Start).

Speed: the game paces itself at 30 fps (main loop waits two fields) and now holds it in the attract loop,
including Zasalamel's Chamber of Time (3-loop benchmark: 29.9 fps there, 30.2 in the second demo).
  - GS SIGNAL/FINISH/LABEL latency follows the estimated GS drawing time since the previous event (scissored
    primitive area + per-primitive cost, `GsDrawEstimate`). The old fixed 200000-cycle latency held Chamber of
    Time to 20 fps: its frame chains ~16 SIGNAL handshakes, each paced in host time too. Title/loading
    segments draw few large primitives and keep long latencies, which their loader pacing needs (short fixed
    latencies left the "NOW LOADING" overlay over the 2nd demo and froze the 3rd). Tunables:
    `PS2X_GS_EVENT_LATENCY` (base), `PS2X_GS_PIXEL_CYCLES`, `PS2X_GS_PRIM_CYCLES`.
  - VU1 microprograms compiled ahead of time (`scripts/compile_vu1.sh`, three images): one function per
    straight-line segment with stall checks proven unnecessary dropped, ~7.6 ns per VU cycle (interpreter
    ~23). `PS2X_VU1_EXACT_FLAGS=1` keeps every flag (and disables the compiled code).
  - `patches/async_vif1_wip.patch` / `patches/async_vif1_path3_wip.patch`: VIF1/VU1 on a worker thread,
    no longer needed for speed; kept for reference.

Open:
- Fights are bound by the host software GS. On an 8-core 7800X3D with the default 6 raster workers (cores - 2),
  the attract demo runs 29-30.5 fps, Free Training 29.2-29.6, fights with hit effects 27-31. The rest is the
  generic per-pixel write path. The workers' rare divergence in the shadow pass was a texture-row range capped at
  the texture height under REGION_CLAMP (fixed in PS2Recomp 26574ece); recorded traces now replay identically.
- Entering a fight (Free Training, Tales of Souls) hung on a black screen with a packet linked into two
  ordering-table buckets (the walk at 0x10E4B8 loops). It no longer reproduces (9 of 9 runs) since CSR.FIFO
  reports the GS busy until its estimated drawing is done and CD reads take drive time, but the exact
  cause of the double link was not found, so it may come back with other timings.
- Ghosting: the game blends each frame 70/30 with the previous one, which shows at low frame rates.

Notes:
- Menu preview movies work: Mode Select creates and immediately stops its preview tasks (state 4, by
  design); the Single Player / VS submenus start them as the cursor moves (MOVIE.AFS files 10, 11, ...,
  128x96 and 256x192 alpha movies through mwPlyGetCurFrm).

Regenerate with a recompiler that has the SQRT.S fix (PS2Recomp aeaaa48, from boubou666's 58b5c8b): the EE takes
SQRT.S's operand from ft, and the old code took sqrt($f0) instead, which broke 56 sites in 41 functions, among them the
game's vec3 length (0x103000), 2D length (0x103040) and point distance (0x103058).

Pitfall: recompiled functions return early at scheduler checkpoints (ctx->pc mid-body), so trace wrappers around
guest functions must only read state on entry, never v0 after the call.
