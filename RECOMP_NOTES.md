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
4. Build (Linux, clang, no system cmake/ninja here so through mise):
   `cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-msse4.2`
   `ninja -C out/build ps2EntryRunner`
5. Run: `ps2EntryRunner <disc>/SLUS_212.16`, or `scripts/sc3run.sh <name> <seconds> [ENV=value...]` which logs,
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
- Diagnostics: `PS2X_STATE_DUMP`, `PS2X_CALL_TRACE`, `PS2X_WATCH=addr[:n]`, `PS2X_PAD_SCRIPT="t:button,..."`,
  `PS2X_FRAME_DUMP=<dir>`, `PS2X_DEBUG_UI=0`, `PS2X_IOP_TRACE_PC`, `PS2X_IOP_COUNT_RANGE`, `PS2X_IOP_WATCH`,
  branch history in missing-target reports, VU1 record/replay (`PS2X_VU1_RECORD` + `ps2_vu1_replay`).

## Status

Works: boot, opening and intro movies (skippable with Start), title, menus, character select, Tales of Souls story
text, fights (attacks, KO, results) with music and voices.

Open:
- Speed: battles run at about 30% of full speed. Frame time is EE work (~21 ms) plus VU1 microprograms (~29 ms,
  interpreted, cycle-accurate), serialized because the game waits for VU1. An asynchronous VIF1 worker
  (`patches/async_vif1_wip.patch`) is stable but does not help for that reason. A VU1 block compiler is the next
  real gain. `PS2X_GS_ASYNC_RASTER=1` helps.
- Ghosting: the game blends each frame 70/30 with the previous one, which shows at low frame rates.
- Menu preview movies (alpha movies, MOVIE.AFS files 8 and 9) start on the host but the menu has not asked for
  their frames yet; the trigger is still unknown.

Pitfall: recompiled functions return early at scheduler checkpoints (ctx->pc mid-body), so trace wrappers around
guest functions must only read state on entry, never v0 after the call.
