# What to run before you push

The whole suite runs in parallel. `ctest -j6` on one build tree takes about
two minutes Release, where it used to take eleven one test at a time. The
screen suite is eight shards packed by measured case time, and every test
but `test_view3d` runs with the dummy video driver, so no test opens a
desktop window except that one.

One build tree covers both data layouts. Tests read the data folder from
`TAK_TEST_DATA_DIR` when it is set, so the archives only layout is the
same binaries pointed at an empty folder. There is no second build.

```
bash scripts/run-suite.sh <build tree> [Release] [6]
```

That is the gate before a merge: both layouts, on the final rebased head,
under the test lock.

No test touches your own options. Every test binary is a test build that
never asks the platform where `options.cfg` lives, and ctest gives every test
a `TAK_CONFIG_DIR` of its own under `<build tree>/src/testrun/<test>/prefs`.
A test binary run by hand with no `TAK_CONFIG_DIR` keeps its options in
`test_prefs` under the folder it runs in. The okengine library keeps its
options in the folder its host names with `okx_set_user_dir`, and with none in
a per-user folder of its own, never in the game's. `test_prefs_guard` fails if
a test is registered without its own folder, if a program other than the game
compiles the paths module without being a test build, if a library compiles it
without a per-user folder of its own, or if any source but `src/core/paths.c`
reaches for the platform's per-user folders.

## Pick tests by what they run

`scripts/test-coverage.json` records which source files every ctest test
and every screen case executed. `scripts/coverage-map.py` builds it on
Linux (WSL is fine) from a build with `TAK_TEST_TRACE=ON`, which records
every function each case enters.

```
python scripts/test-pick.py --build-dir <build tree> --plan pick.sh
with_test_lock bash pick.sh
```

A changed source file selects the tests and cases that ran it, and a
changed header selects everything that ran a source including it. A file
the map has never seen, a build file or a script selects the full suite.
The picked run covers both layouts and spreads the screen cases over
parallel processes. Regenerate the map when the picker says it is stale.

The tier rules below still work and `test-tier.py` still checks them. They
stay until the coverage map has proven itself over a few merges, then the
picker replaces them.

## Ask the chooser, do not decide

```
python scripts/test-tier.py $(git diff --name-only origin/main...HEAD)
```

With no arguments it works the file list out itself. It prints the tier and
the literal commands. Add `--build-dir <your build tree>` to get
those commands with your own paths in them, `--explain` to see which rule caught
each file, and `--plan` to write a runnable script into the build tree that
takes the lock once, refuses to run in a stale tree, and fails when a filter
matches a different number of cases than the rules expect.

The tier comes from the paths and nothing else. There is no judgement call in
the moment, because the moment is exactly when the judgement is worst. The
rules are data in `scripts/test-tiers.toml`, so changing what a branch owes is
a commit and a review like any other change.

How the tier combines:

- The tier is the highest tier over every file in the change.
- Documentation plus one subsystem is tier 1. That subsystem plus unit
  simulation is tier 2.
- Two tier 1 subsystems stay tier 1 and run both command sets.
- Four or more distinct tier 1 entries escalate to tier 2. The union is
  approaching the whole binary by then, and interaction across subsystems is
  exactly what a list of named cases cannot see.
- A path that matches no rule is tier 2. A new file is therefore never
  accidentally cheap.

## Tier 0, no tests, no lock

Documentation, the readme and the other top level text files, issue templates,
the dependabot config, the ignore files, and the images the web page serves.
Nothing here is compiled and nothing here runs. Commit and push. The
pre-commit hook and the licence scan in CI are the only checks and both take
seconds.

Workflow files, the git hooks, anything under `scripts/` and the browser shell
are not tier 0. They are programs, they change what is built or how it is run,
and they fall through to tier 2.

## Tier 1, targeted, one lock take, one build tree

Each entry in the rules names its ctest targets and its `test_ui_screens`
cases, with the number of cases every filter is expected to match. The filter
is a plain substring match against the case name, so a run that names nothing
real still exits 0 with nothing run. That is why every invocation carries an
expected count, and why the generated plan fails when a count does not match.

The entries today cover fog, economy, movement, sound, the heads up display,
the front end screens, the dialog loader, the image formats, missions, the
command list, the asset tools, and any test source on its own.

The screen suite is registered with ctest as eight shards,
`test_ui_screens_1` through `_8`, packed by measured case time. A tier 1 run
does not use them. It invokes the
binary itself with a case filter, and the binary runs every shard unless it is
asked for one, so which shard a case landed in never changes what a targeted
run executes. That is also why the rules do not name the shards in their ctest
regexes: the screen suite is covered by the named case list on each rule, and
the chooser leaves out every ctest test that runs that binary rather than a
list of shard names that would go stale the next time the suite is repacked.

1. Build your own tree, outside the lock, with `-m:2`.
2. Confirm the tree is not stale. `python scripts/test-tier.py --check-tree
   <your build tree>` compares `ctest -N` against the number of tests
   `src/CMakeLists.txt` declares. A tree that knows fewer tests can skip the
   one target your tier depends on and still exit 0.
3. Take the lock once for the whole chain, not once per command.
4. Paste every `Results:` line with the number the rules expect beside it.
5. Tier 1 clean is not merge ready. It means the subsystem you touched still
   works.

## Tier 2, the full suite, one lock take

The two data layouts are the game files extracted on disk and the original
archives read in place. Both are run, because the file lookup paths through
them differ and a change can pass on one and fail on the other.

Every header, because the include graph fans out further than a filename
suggests. Everything under `src/core`, because memory, the string and file
helpers and the archive reader are compiled into most of the registered tests.
The tick loop, the world, the artificial intelligence and unit simulation,
because nearly every screen case boots a map and ticks it. Build files and
continuous integration files, because they change what is compiled. Anything
unrecognised, because the rules cannot vouch for it.

1. Build your tree, outside the lock, `-m:2`.
2. Confirm the tree knows every declared test.
3. Take the lock once and run `bash scripts/run-suite.sh <tree>`. It runs
   both layouts at `-j6`, a few minutes in all.
4. Paste the `SUITE CLEAN on both layouts` line.

One carve out, still decided by path alone. A change confined to
`include/test_framework.h`, `include/test_hpi_builder.h` or
`src/ui/test_ui_screens.c` cannot alter shipped behaviour, only the harness
that measures it. That runs the full suite on one data layout and takes the
lock once, so adding coverage does not cost double. Confined means those files
and nothing else. Mixed with any product file it is an ordinary tier 2.

## What still gates a merge

The full suite on both data layouts, once, on the final rebased head, run by
whoever merges. Nothing lands on the main branch that has not had one. The
protection on the main branch is exactly what it was. What goes away is the
repeated full runs on intermediate states of a branch that nobody will ever
ship.

CI cannot take this gate. It runs `ctest --label-exclude needs-data`, which is
the tests that need no game files, in about a second and a half. It has no
copy of the game and cannot legally be given one, so it runs on neither data
layout and it never builds a screen test at all. The two local runs are not
paying twice for something CI already does. They are the only place that tier
has ever run.

What CI is good at, and keeps doing. Portability across four compilers and
the browser build, which one local run cannot see. Licence hygiene, which is a
takedown risk and takes seconds. The data free logic tests. And, since the
tiers went in, the check that the tier rules still agree with the code.

## Keeping the rules honest

`python scripts/test_test_tier.py` runs in about a second, needs no game data,
and runs in CI on every pull request. It feeds the chooser file lists from real
commits on this repo and asserts the tier, including the changes that must
never go cheap. Then it checks the rules against the code:

- every filter matches the number of cases the rules claim, so a case that is
  added, renamed or removed breaks the rules file instead of quietly dropping
  out of coverage,
- every ui case that calls into a subsystem is inside that subsystem's rule,
- every ctest target a rule names is registered and really does compile that
  rule's sources,
- every path a rule names exists,
- nothing that runs or gets compiled is in tier 0.

That check is the thing that stops the policy rotting. When it fails, fix the
rules file in the same commit as the change that broke it. Most of the time
that is one command:

```
python scripts/test-tier.py --regenerate
```

It rewrites the counts, the targets and the case lists from the tree as it is
and tells you which rules it touched. It proposes, it does not decide. What
lands is the diff, read and committed the way any rule change is. A rule that
grew a case or a target is the tree saying what it now reaches, and a rule
that grew a great deal is worth a second look before it goes in.

## What the tiers do not claim

They do not help the work that most needs help. Unit simulation, the tick
loop, the artificial intelligence and everything under `src/core` are always
tier 2, and that is most gameplay work. The savings are in screens, sound, the
heads up display, the command list, the tools and documentation.

They cannot tell thin coverage from safe code. The command list is the
cheapest entry in the tree at about four seconds, and that is a statement
about how few tests reach it.

They say nothing about the browser. No automated run covers it at any tier,
and a change can be clean at tier 2 and still break the browser build in a way
only a manual smoke run finds.

They trade certainty during a branch for speed. A change that breaks something
across a subsystem boundary passes its tier 1 run and fails at the merge run,
and the cost is a late rework loop rather than a broken main branch.
