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

There are two kinds.

- Data-free tests cover parsers against synthetic fixtures, maths, the COB
  virtual machine, pathfinding and unit movement on generated maps, wire
  format and memory handling. These run in CI on every PR, on all three platforms.
- Data-dependent tests are anything that loads real game files. They carry
  the CTest label `needs-data` and CI excludes them
  (`ctest --label-exclude needs-data`), because GitHub's runners have no
  game data and can't be given any. Maintainers run the full suite locally
  before merging.

If you're adding a test, prefer the data-free tier. If your test really does
need real data, add it to the `needs-data` list in `src/CMakeLists.txt`
so CI doesn't try to run it.

### Run the suite in Release

```bash
cmake --build build --config Release
bash scripts/run-suite.sh build Release 6
```

That is the gate before a merge. It runs every test on the original
archives, the way the game and the browser read them, then the tests
labelled `layout` again on the extracted data folder. About a minute and a
half on Linux. Tests labelled `on-demand` are measurements and soaks, and
neither the gate nor CI runs them. Run them with `ctest -L on-demand` when
your change is about what they measure.

Nothing in the tree relies on a Debug build to catch anything. There is
no `assert()` in product code, no code behind `NDEBUG`, and the assertion
macros in `include/test_framework.h` are ordinary `if` statements that
print and count, so the optimiser cannot drop one. Keep a Debug build for
the times you need to step through a failure. If a case ever passes in
one configuration and fails in the other, that is a finding worth
reporting rather than something to work around.

### The screen suite's shards

`test_ui_screens` is registered as eight shards, packed by measured case
time from `src/ui/test_ui_case_times.h` so they finish together.
`test_ui_screens --verify-shards` walks the registration, runs nothing and
fails if the split is broken. It reads no game data, so CI runs it. When
the shards stop finishing together, regenerate the times with
`python scripts/ui-case-times.py <build>/Testing/Temporary/LastTest.log`
after a run of the shards.

### A skip is not a pass

A case that cannot meet its precondition calls `SKIP` or `SKIP_MARK`. It
is counted as a skip, never as a pass, and it makes the binary exit
nonzero, because a case that tested nothing must not read as green. A
suite that has a configuration where skipping is the correct answer,
such as the CI run with no game data, declares it with
`TEST_ALLOW_SKIPS` and the reason appears in the report.

### What gets a new test

Do not add an end to end case for every feature or fix. Prove a fix with an
existing test that now catches it, a cheap unit test beside the code, or a
repro written into the pull request. The screen suite, which boots real
battles, gains a case only for simulation determinism and the state hash,
lockstep and the net protocol, saves and loads, golden frames, a crash that
is cheap to repeat, and parity with the original where it already went
wrong once. A check of one function belongs in a unit test with a synthetic
world, as `test_movement` and `test_line_of_fire` do.

### Which tests to run before you push

The gate is short, but it needs the game files and it is still not
something to run on every intermediate state of a branch. What a change owes is decided by the files it
touches. Ask the chooser:

```bash
python scripts/test-tier.py $(git diff --name-only origin/main...HEAD)
```

It prints the tier and the exact commands. With no arguments it works the file
list out itself. Add `--build-dir <your build tree>` to get the commands with
your own paths in them, and `--explain` to see which rule caught each file.

- Tier 0 is documentation and other text that nothing compiles or runs. No
  tests.
- Tier 1 is one subsystem, so it runs that subsystem's targets and its named
  screen cases. One to five minutes.
- Tier 2 is `scripts/run-suite.sh`. Headers, the tick loop, unit
  simulation, anything under `src/core`, build and workflow files, and any
  path no rule recognises.

The rules are data in `scripts/test-tiers.toml` and the reasoning is in
[docs/testing-tiers.md](docs/testing-tiers.md). Changing what a branch owes is
a commit and a review, not a decision made in a hurry. If you add or rename a
screen case, run `python scripts/test_test_tier.py`, which takes a second and
tells you which rule needs updating. `python scripts/test-tier.py --regenerate`
does the updating for you and leaves a diff to review. CI runs the check on
every pull request.

The full suite on both data layouts still gates every merge. It runs once, on
the final rebased head, before the change lands.

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

## Licence of contributions

Contributions are made under the GPL version 3, and you also agree that they may be used under the additional permission in [LICENSE.unity-exception](LICENSE.unity-exception), which lets OpenKingdoms run inside a Unity front end.
