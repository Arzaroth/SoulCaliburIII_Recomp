<div align="center">

# SoulCaliburIII_Recomp

**A native Linux port of *Soulcalibur III* (NTSC-U, `SLUS-21216`), made by statically recompiling the game's PlayStation 2 code to C++.**<br>
Not an emulator: the game's own MIPS R5900 code, rebuilt as a native program on a reimplementation of the PS2 hardware.

![Platform](https://img.shields.io/badge/platform-Linux-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-Clang-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-PS2Recomp-17a35b)
![Status](https://img.shields.io/badge/status-playable%2C%20in%20testing-e8a33c)

<img src="docs/screenshots/fight.jpg" width="49%"> <img src="docs/screenshots/title.jpg" width="49%">
<img src="docs/screenshots/character_select.jpg" width="49%"> <img src="docs/screenshots/versus.jpg" width="49%">
<img src="docs/screenshots/chronicles_field.jpg" width="49%"> <img src="docs/screenshots/stage_select.jpg" width="49%">

<sub>Captured from the running port (640x448 internal resolution, GPU renderer).</sub>

</div>

> [!IMPORTANT]
> No game files are included: no ELF, IRX modules, AFS archives, or code generated from them.
> You need **your own copy** of the North American release, PS2 ID **`SLUS-21216`**. The port is built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SLUS_212.16` | `9bbb67a70137052931bc245855eedeee6bbe5b49` |

## Status

**Playable from start to finish in automated tests, now in human testing.** Fights run at 60 frames per second in
real time on a Ryzen 7 7800X3D with the GPU renderer, with music and voices. Every mode has been played through with
scripted input: Tales of Souls to the credits, Chronicles of the Sword battles on the field map, World Competition,
Soul Arena, Museum, Character Creation, Shop and Options, with saves loaded back after a restart.

| Milestone | |
|---|:---:|
| Map and recompile the main ELF (12,900 functions) | ✅ |
| Boot: EE kernel, the game's own IOP modules, CD reads from the disc files | ✅ |
| Logos and movies (CRI Sofdec, decoded on the host) | ✅ |
| Title screen, menus, character and stage select | ✅ |
| Music and voices (CRI ADX on the game's own `CRI_ADXI.IRX`, SPU2) | ✅ |
| Fights at 60 fps (VU1 on its own thread, GPU renderer) | ✅ |
| Tales of Souls, World Competition, Soul Arena, Museum, Shop | ✅ |
| Chronicles of the Sword: story, field map, battles | ✅ |
| Memory card: save and load | ✅ |
| Long play sessions by a human | 🔧 |
| Widescreen, higher resolution | ⏳ |

✅ works · 🔧 in progress · ⏳ not reached yet

[RECOMP_NOTES.md](RECOMP_NOTES.md) records how the port was brought up, every fix and how it was found.

## Goal

A fully playable *Soulcalibur III* as a native executable, every mode from the opening movie to the endings, at the
original 60 frames per second. Accuracy first; once it is solid, widescreen and higher internal resolution.

## How it works

```
disc files ──► Ghidra (headless, EE) + ps2_analyzer + recomp/*_hints.txt ──► recomp/sc3.toml
                                           │
                                ps2_recomp ──► C++ (one file per function)
                                           │
             VU1 code images (dumped at run time) ──► ps2_vu1_compile ──► vu1_sc3.cpp
                                           │
   PS2Recomp runtime: EE kernel HLE · DMAC · GIF/GS · VIF/VU0/VU1 ─┼─► ps2EntryRunner
   PS2Recomp IOP:     R3000A interpreter running the game's own IRX · SPU2 · SIO2
```

- The ELF has no symbols. Ghidra maps the functions; `scripts/find_code_pointers.py` adds those reached only through
  pointers, jump tables or tail calls (the hint lists in `recomp/`).
- PS2Recomp turns every function into C++. VU1 microprograms are compiled ahead of time from the code images the game
  uploads, and run on their own thread ahead of the game, syncing only when the game reads VU1 state.
- The GS is drawn on the GPU by [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (Vulkan compute), with
  the CPU rasterizer as fallback.
- `runtime/sc3_overrides.cpp` holds the game-specific bindings: reserved memory, a host Sofdec movie player and
  mixer, the renderer and timing choices, and diagnostic tracers.

The runtime lives in the PS2Recomp fork [Arzaroth/PS2Recomp](https://github.com/Arzaroth/PS2Recomp), branch
`sc3/bringup`, built on [boubou666/PS2Recomp](https://github.com/boubou666/PS2Recomp). Most fixes made for this port
are generic and many were taken upstream, for example the IOP interrupt suspension that kept CRI ADX streams alive, the
VU0 VCLIP flags and VU1 run-ahead on its own clock.

## Requirements

- Linux (developed on Arch)
- CMake 3.21+, Ninja, Clang with C++20
- FFmpeg development libraries (`libavcodec`, `libavformat`, `libavutil`, `libswresample`, `libswscale`)
- X11/Wayland and OpenGL development headers (raylib is fetched and built by the runtime)
- A Vulkan driver for the GPU renderer (any recent NVIDIA, AMD or Intel driver)
- Ghidra with the Emotion Engine extension, only to regenerate the function map
- Your own copy of `SLUS-21216` (check the hash above), with its files extracted to a folder

## Build and run

The build is manual for now: the generated code is copied into the runtime's runner and built with it.

```bash
git clone https://github.com/Arzaroth/PS2Recomp.git -b sc3/bringup
git clone https://github.com/Arzaroth/SoulCaliburIII_Recomp.git

# 1. recompile: from recomp/, with SLUS_212.16 copied next to sc3.toml
ps2_recomp sc3.toml                                   # -> recomp/output/

# 2. copy the generated code and the game overrides into the runtime
cp recomp/output/*.cpp                ../PS2Recomp/ps2xRuntime/src/runner/
cp recomp/output/ps2_recompiled_*.h   ../PS2Recomp/ps2xRuntime/include/
cp runtime/sc3_overrides.cpp          ../PS2Recomp/ps2xRuntime/src/runner/

# 3. build
cd ../PS2Recomp
cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ -DPS2X_BUILD_STUDIO=OFF
ninja -C out/build ps2EntryRunner

# 4. run, with the extracted disc folder as cdrom0:
out/build/ps2xRuntime/ps2EntryRunner /path/to/disc/SLUS_212.16
```

VU1 microprograms run in an interpreter until they are compiled: run the game once with
`PS2X_VU1_DUMP_CODE=<dir>`, then `scripts/compile_vu1.sh <dir>` writes `vu1_sc3.cpp` into the runner; rebuild. See
[RECOMP_NOTES.md](RECOMP_NOTES.md) for the details. The generated code is about 13,000 C++ files and takes a while
to build the first time.

A controller is used when one is connected; otherwise the keyboard (US key positions):

| PS2 button | Keyboard |
|---|---|
| D-pad | Arrow keys or W A S D |
| Cross / Circle / Square / Triangle | X or Space / C or Esc / Z or Numpad 0 / V or Numpad 1 |
| L1 / R1 / L2 / R2 | Q / E / Left Shift / Right Shift |
| Start / Select | Enter / Tab |

| Key | What it does |
|---|---|
| F1 | Runtime debugger |
| F3 | Frame rate and emulated EE speed overlay (`PS2X_SHOW_FPS=1` starts with it on) |

<details>
<summary><b>Runtime and diagnostic options</b></summary>

| Variable | Effect |
|---|---|
| `PS2X_GS_RENDERER=cpu` | Draw with the CPU rasterizer. The default is the GPU (paraLLEl-GS, Vulkan), which falls back to the CPU when no Vulkan device is found |
| `PS2X_VIF1_THREAD=0` | Run VIF1 and the VU1 microcode on the game thread (default: their own thread, about a third faster in fights) |
| `PS2X_EE_CATCHUP=0\|field\|1` | Wall-clock catch-up: SC3 defaults to off, so emulated time never runs faster than real time |
| `PS2X_SC3_NO_MOVIES=1` | Skip the one-shot movies (for tests) |
| `PS2X_SC3_AUTOWIN=1` | Keep the player at full health and the opponent at 1 HP, so scripted input wins each fight |
| `PS2X_PAD_SCRIPT=t:BUTTON[/ms],…` | Scripted button presses (seconds from start); `PS2X_PAD_INPUT=<file>` reads presses appended to a file while the game runs |
| `PS2X_STATE_DUMP=<s>` | Every few seconds: EE and IOP threads, GS frames, EE speed, DMA and VIF counters, SPU2 state |
| `PS2X_FRAME_DUMP=<dir>`, `PS2X_FRAME_DUMP_EVERY=<s>` | Save the presented frame as raw 640x448 RGBA |
| `PS2X_PROFILE=<s>` | Sampling profiler: where every host thread spends its time |
| `PS2X_EE_PROFILE=1` | EE cycles by guest loop, in the state dump |
| `PS2X_GS_DUMP_WHEN=<file>` | Log the next GS primitives once that file appears (`PS2X_GS_DUMP_OUT`, `PS2X_GS_DUMP_COUNT`) |
| `PS2X_VU1_DUMP_CODE=<dir>` | Write each VU1 code image that has no compiled code, for `scripts/compile_vu1.sh` |
| `PS2X_SC3_TRACE_ADXT=1`, `_TASKS`, `_ENTRY=<addr,…>`, `_OBJ`, `_FADE`, `_MOVIE` | Game tracers: ADX player states, task creation, guest function entries, object fields, fades, movies |

</details>

## Disc facts

| | |
|---|---|
| Boot ELF | `SLUS_212.16`, entry `0x100008`, stripped |
| Video mode | NTSC (`SYSTEM.CNF` `VER = 1.01`) |
| IOP modules | `IRX/*.IRX`: CRI_ADXI, LIBSD, MCMAN, MCSERV, NUSOUND, PADMAN, SDRDRV, SIO2MAN |
| IOP image | `IOPRP300.IMG` |
| Data | `ROOT.OLK`, `BGM.AFS`, `SE.AFS`, `VOICE.AFS`, `MOVIE.AFS` (Sofdec) |

## Repository layout

| Path | Contents |
|---|---|
| `recomp/` | Recompiler config (`sc3.toml`), the Ghidra function map and the entry-point hint lists |
| `runtime/` | Game override module (`sc3_overrides.cpp`) |
| `scripts/` | VU1 compile, run, disassembly, IRX and profiling helpers |
| `patches/` | Experiments kept for reference |
| `RECOMP_NOTES.md` | Bring-up notes |

Everything derived from the game (disc files, generated code, VU1 images, builds, logs) stays outside this repository.
Never commit or redistribute it.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPL), the recompiler and runtime this port is built on.
- [boubou666/PS2Recomp](https://github.com/boubou666/PS2Recomp) and the *Twisted Metal: Black*, *Valkyrie Profile 2: Silmeria* and *Drakengard* recompilations developed on it; runtime fixes are shared both ways.
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) by Hans-Kristian Arntzen (LGPL), the GPU renderer.
- *Soulcalibur III* © Bandai Namco Entertainment, developed by Project Soul. This is an unofficial fan project, not affiliated with or endorsed by them.
