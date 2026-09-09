# Contributing to OpenTAK

Thanks for wanting to help. This project has an unusual goal — it isn't
"build a good RTS", it's "rebuild *this specific* 1999 RTS so that it
behaves the way people remember" — and that shapes how contributions work.

Read the [parity rules](#parity-the-thing-that-makes-this-project-different)
before writing gameplay code. Everything else is ordinary.

---

## Ground rules on game content

**Never commit game data.** No `.hpi`, `.gp3`, `.ufo`, no extracted assets,
no sprites, no sounds, no maps ripped from the original, not even a small
one for a test fixture. Fixtures are either generated synthetically at test
time or hand-authored in the original file formats — see
`src/core/parser/test.tdf` for the pattern.

**Never commit original game code**, decompiled or disassembled, in any
form: not as source, not in a comment, not quoted in a commit message, not
pasted into a PR discussion. If you have been reading a disassembly, don't
paste from it — describe the behaviour in your own words.

These aren't formalities. A single asset or a pasted decompiled function in
a public repository is a takedown risk for everyone's work.

---

## Getting set up

```bash
git clone https://github.com/zbennett10/open-tak.git
cd open-tak
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Platform prerequisites are in the [README](README.md). To run the game
itself you need your own copy of the original — see
[docs/ASSETS.md](docs/ASSETS.md).

### Build the WebAssembly target too

This is not optional and it catches real bugs. Emscripten's clang rejects
things MSVC quietly accepts — an implicit function declaration is a warning
on Windows and a hard error in the browser build. A PR that's green on
native can still break the web build entirely.

```bash
# with emsdk active
cmake -B build-wasm -DCMAKE_TOOLCHAIN_FILE=$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
cmake --build build-wasm
```

CI enforces this, but finding it locally is faster than finding it in
review.

---

## Tests

Tests live next to the code they cover, as `test_*.c` alongside the
subsystem (`src/game/test_pathing.c`, `src/render/test_cob_vm.c`,
`src/core/hpi/test_hpi.c`, and so on), registered with CTest in
`src/CMakeLists.txt`. `include/test_framework.h` has the assertion macros.

Two tiers:

- **Data-free tests** — parsers against synthetic fixtures, maths, the COB
  virtual machine, pathfinding on generated maps, wire format, memory
  handling. These run in CI on every PR, on all three platforms.
- **Data-dependent tests** — anything that loads real game files. These
  carry the CTest label `needs-data` and CI excludes them
  (`ctest --label-exclude needs-data`), because GitHub's runners have no
  game data and can't be given any. **Maintainers run the full suite
  locally before merging.**

If you're adding a test, prefer the data-free tier. If your test genuinely
needs real data, add it to the `needs-data` list in `src/CMakeLists.txt`
so CI doesn't try to run it.

Behaviour changes want a test that would have failed before your change.
Bug fixes especially: the regression test is the part that stops the bug
coming back six months later.

---

## Parity: the thing that makes this project different

The original game is the specification. When OpenTAK and the 1999 game
disagree, OpenTAK is wrong — even when OpenTAK's behaviour is arguably
better.

That means **behaviour changes need evidence.** In your PR, say what the
original does and how you know. Acceptable evidence, roughly in order of
strength:

1. A citation to our behaviour notes in [`docs/notes/`](docs/notes/), which
   is where reconstructed behaviour is written down.
2. The game manual, for documented rules.
3. A reproducible observation from the original game — what you did, what
   happened, ideally a screenshot or video.
4. A data-file fact (an `.fbi`/`.tdf` field and its effect).

"This felt better" is not evidence, and a PR that improves on the original
will be asked to justify itself as a deliberate, documented deviation.
Deviations are sometimes right — we run the simulation at 60 Hz where the
original ran at 30, for instance — but they get recorded rather than
slipped in.

If you find a place where we deviate and shouldn't, **file it** — use the
*parity deviation* issue template. Those reports are genuinely some of the
most useful contributions to this project, and you don't need to write any
code to make one.

---

## Code style

Match the surrounding code. Concretely:

- C11. No compiler extensions beyond what the existing code already uses.
- Four-space indent, no tabs.
- `snake_case` for functions and locals, `PascalCase` for types,
  `SCREAMING_CASE` for macros. Subsystem-prefixed public functions
  (`Units_`, `Terrain_`, `Cob_`).
- **Keep comments brief.** One or two lines. A comment should say something
  the code cannot — a constraint, a reference to a note, a non-obvious
  reason. Not a paragraph, and not a restatement of the next line.
- Declare before use. The WebAssembly build will fail on implicit
  declarations even when your native build doesn't.

No automatic formatter is enforced; don't reformat code you aren't
otherwise changing, as it buries the real diff.

---

## Pull requests

- Branch from `main`, one logical change per PR.
- Fill in the PR template — particularly the parity evidence section if you
  touched game behaviour.
- CI must be green: native builds on Windows/macOS/Linux, the WebAssembly
  build, and the data-free test suite.
- **CodeRabbit** reviews PRs automatically. Treat it as a useful first pass,
  not as an authority — it doesn't know the original game, and it will
  occasionally object to something that is deliberately faithful. Say so and
  move on. A human maintainer approves every merge.
- Maintainers run the full data-dependent suite before merging. If that
  turns up a failure, you'll hear about it with the output.

Commits: present tense, explain *why* rather than *what*. The diff shows
what.

---

## Filing issues

- **Bug** — what happened, what you expected, platform, and how to
  reproduce.
- **Parity deviation** — legacy behaviour / our behaviour / evidence. The
  template asks for exactly that.
- **Feature** — bear in mind that "feature" in this project usually means
  "something the original had that we haven't rebuilt yet". New ideas that
  go beyond the original are a harder sell and belong in a discussion first.

Please don't file issues asking for game files or where to download the
game. Those get closed.

---

## Code of conduct

The [Contributor Covenant](CODE_OF_CONDUCT.md) applies. Be decent to people.
