# OpenKingdoms: notes for AI coding assistants

This file is read automatically by Claude Code and similar tools. It says
how this project likes to be worked on. Humans: CONTRIBUTING.md covers the
same ground in more words.

## What this project is

A from-scratch C11 engine for Total Annihilation: Kingdoms (1999). The
original game is the specification. When the engine and the original
disagree by accident, the engine is wrong. Deliberate improvements are
welcome and are recorded in docs/MANUAL_DEVIATIONS.md.

## Ground rules

- Never add game content: no .hpi, .gaf, .3do, .cob, .tnt, sounds, maps,
  not even as a test fixture. Fixtures are synthetic or hand-authored.
- Never add original game code in any form, and never describe it as
  having been read from a disassembly or similar. Describe behaviour in
  your own words. Citations of the form `legacy:NNNN` are reference anchors
  maintainers use to re-check a claim; keep them on behaviour changes, do
  not invent them.
- Behaviour changes need evidence: a note in docs/notes/, the manual, a
  reproducible observation, or a data-file fact. Say which in the PR.

## Building and testing

- Native: `cmake -B build -DTAK_GAME_DIR="<your install>"`, then
  `cmake --build build`, then `ctest --test-dir build --output-on-failure`.
  Tests that need game data carry the CTest label `needs-data`; CI runs
  the rest with `--label-exclude needs-data`.
- Browser: `scripts/build-wasm.sh --public` (or `build-wasm.ps1 -Public`).
  Build the WebAssembly target before opening a PR: Emscripten's clang
  rejects implicit declarations that MSVC accepts. Declare before use.
- `scripts/web-smoke.js` drives a browser through the whole
  bring-your-own-files flow, including a skirmish, against a real install.
- Every bug fix gets a test that would have failed before it. Tests live
  next to the code as `test_*.c`, registered in src/CMakeLists.txt.

## Conventions

- Simulation runs at 60 Hz (the original ran 30). Rates from the data
  files convert accordingly. 1 world unit = 1 px, 16 px per cell, 32 px per
  tile, positions in 16.16 fixed point, angles 65536 per turn.
- Simulation code must be deterministic: no floats in simulation state, one
  seeded RNG, no wall-clock reads, no iteration-order dependence. Lockstep
  multiplayer desyncs on any of these.
- Comments are brief: one or two lines stating a constraint or citing a
  reference anchor. No paragraphs, no restating the next line.
- Prose in docs: plain sentences, no em dashes, no semicolons, no filler.
- Commit messages: present tense, say why. No tool attribution trailers.
- Keep MSVC, gcc, clang and Emscripten all clean; CI builds all four.

## Where things are

- src/game/: simulation (economy, AI, fog, pathing, terrain data).
- src/render/: models, sprites, the COB animation VM, unit logic, GPU.
- src/ui/: menus, skirmish setup, HUD, in-game input, loading.
- src/core/hpi/: archive reading and the virtual filesystem.
- web/shell.html: the browser page (file picker, OPFS cache).
- docs/notes/: behaviour notes, the reference contributors work from.
