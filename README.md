# Soulcalibur III (USA) static recompilation

Work in progress: running Soulcalibur III (NTSC-U, SLUS-21216) natively on PC by statically recompiling the PS2 game
with [PS2Recomp](https://github.com/ran-j/PS2Recomp). The game boots, plays the opening movies, goes through the
menus and character select, and runs real fights with music and voices, at about a third of full speed for now.

This repository holds the project around the recompiler, not the game:
- `recomp/` - recompiler config (`sc3.toml`), the Ghidra function-map export and the entry-point hint lists
- `runtime/sc3_overrides.cpp` - game-specific runtime bindings: reserved memory, a host Sofdec movie player, tracers
- `scripts/` - run, disassembly, IRX and profiling helpers used during bring-up
- `patches/` - experiments kept for later (asynchronous VIF1/VU1 worker)
- `RECOMP_NOTES.md` - how to rebuild, what was fixed and why, current status

It contains no game data: no ISO, no extracted files, no recompiled game code. You need your own copy of the game.

The runtime changes made for this game are on the PS2Recomp branch `sc3/bringup`.
