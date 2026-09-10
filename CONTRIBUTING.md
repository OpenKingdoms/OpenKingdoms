# Contributing to OpenKingdoms

Thanks for wanting to help. The goal of this project is to rebuild one
specific 1999 RTS so that it behaves the way people remember it, which is a
narrower target than building a good RTS. That shapes how contributions work.

Read the [parity rules](#parity-the-thing-that-makes-this-project-different)
before writing gameplay code. Everything else is ordinary.

---

## Ground rules on game content

Never commit game data. That means no `.hpi`, `.gp3`, `.ufo`, no extracted
assets, no sprites, no sounds, no maps ripped from the original, not even a
small one for a test fixture. Fixtures are either generated synthetically at
test time or hand-authored in the original file formats. See
`src/core/parser/test.tdf` for the pattern.

Never commit original game code, decompiled or disassembled, in any form. Not
as source, not in a comment, not quoted in a commit message, not pasted into a
PR discussion. If you have been reading a disassembly, don't paste from it.
Describe the behaviour in your own words.

These aren't formalities. A single asset or a pasted decompiled function in
a public repository is a takedown risk for everyone's work.

---

## Getting set up

```bash
git clone https://github.com/OpenKingdoms/OpenKingdoms.git
cd OpenKingdoms
git config core.hooksPath .githooks
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The `hooksPath` line turns on a pre-commit check that refuses game
content, oversized files and a few other things CI would reject anyway.
It saves a round trip.

Platform prerequisites are in the [README](README.md). To run the game
itself you need your own copy of the original, so see
[docs/ASSETS.md](docs/ASSETS.md).

### Build the WebAssembly target too

This is not optional and it catches real bugs. Emscripten's clang rejects
things MSVC quietly accepts. An implicit function declaration is a warning on
Windows and a hard error in the browser build, so a PR that's green on native
can still break the web build entirely.

```bash
# Linux, macOS, Git Bash on Windows (needs emsdk and ninja)
scripts/build-wasm.sh --public

# PowerShell on Windows
scripts\build-wasm.ps1 -Public
```

`--public` builds the engine alone, the way CI does. Without it the script
packs your own game data into the page for quick local testing, which is
handy but must never be deployed.

CI enforces this, but finding it locally is faster than finding it in
review.

---

## Tests

Tests live next to the code they cover, as `test_*.c` alongside the
subsystem (`src/game/test_pathing.c`, `src/render/test_cob_vm.c`,
`src/core/hpi/test_hpi.c`, and so on), registered with CTest in
`src/CMakeLists.txt`. `include/test_framework.h` has the assertion macros.

There are two tiers.

- Data-free tests cover parsers against synthetic fixtures, maths, the COB
  virtual machine, pathfinding on generated maps, wire format and memory
  handling. These run in CI on every PR, on all three platforms.
- Data-dependent tests are anything that loads real game files. They carry
  the CTest label `needs-data` and CI excludes them
  (`ctest --label-exclude needs-data`), because GitHub's runners have no
  game data and can't be given any. Maintainers run the full suite locally
  before merging.

If you're adding a test, prefer the data-free tier. If your test really does
need real data, add it to the `needs-data` list in `src/CMakeLists.txt`
so CI doesn't try to run it.

Behaviour changes want a test that would have failed before your change.
That goes double for bug fixes, where the regression test is the part that
stops the bug coming back six months later.

---

## Parity: the thing that makes this project different

The original game is the specification. When OpenKingdoms and the 1999 game
disagree by accident, OpenKingdoms is wrong, even when OpenKingdoms's
behaviour is arguably better.

That means behaviour changes need evidence. In your PR, say what the
original does and how you know. Acceptable evidence, roughly in order of
strength:

1. A citation to our behaviour notes in [`docs/notes/`](docs/notes/), which
   is where reconstructed behaviour is written down.
2. The game manual, for documented rules.
3. A reproducible observation from the original game. Say what you did and
   what happened, ideally with a screenshot or video.
4. A data-file fact, meaning an `.fbi`/`.tdf` field and its effect.

"This felt better" is not evidence, and a PR that improves on the original
will be asked to justify itself as a documented deviation. Deviations are
sometimes right. We run the simulation at 60 Hz where the original ran at 30,
for instance. They get recorded rather than slipped in.

If you find a place where we deviate and shouldn't, file it with the parity
deviation issue template. Those reports are some of the most useful
contributions to this project, and you don't need to write any code to make
one.

---

## Code style

Match the surrounding code. Concretely:

- C11. No compiler extensions beyond what the existing code already uses.
- Four-space indent, no tabs.
- `snake_case` for functions and locals, `PascalCase` for types,
  `SCREAMING_CASE` for macros. Subsystem-prefixed public functions
  (`Units_`, `Terrain_`, `Cob_`).
- Keep comments brief, one or two lines. A comment should say something the
  code cannot, such as a constraint, a reference to a note, or a non-obvious
  reason. Don't write a paragraph and don't restate the next line.
- Declare before use. The WebAssembly build will fail on implicit
  declarations even when your native build doesn't.

No automatic formatter is enforced. Don't reformat code you aren't otherwise
changing, because it buries the real diff.

---

## Pull requests

- Branch from `main`, one logical change per PR.
- Fill in the PR template, particularly the parity evidence section if you
  touched game behaviour.
- CI must be green: native builds on Windows/macOS/Linux, the WebAssembly
  build, and the data-free test suite.
- CodeRabbit reviews PRs automatically. Treat it as a useful first pass and
  not as an authority. It doesn't know the original game, and it will
  occasionally object to something that is faithful on purpose. Say so and
  move on. A human maintainer approves every merge.
- Maintainers run the full data-dependent suite before merging. If that
  turns up a failure, you'll hear about it with the output.

Write commit messages in the present tense, and explain why rather than what.
The diff shows what.

---

## Filing issues

- A bug report needs what happened, what you expected, your platform, and
  how to reproduce it.
- A parity deviation needs the legacy behaviour, our behaviour, and your
  evidence. The template asks for exactly that.
- For features, bear in mind that "feature" in this project usually means
  something the original had that we haven't rebuilt yet. New ideas that go
  beyond the original are a harder sell and belong in a discussion first.

Please don't file issues asking for game files or where to download the
game. Those get closed.

---

## Code of conduct

The [Contributor Covenant](CODE_OF_CONDUCT.md) applies. Be decent to people.
