#!/usr/bin/env python3
"""Tests for the tier chooser, and for the rules it reads.

    python scripts/test_test_tier.py

Two halves.

The first half feeds the chooser file lists taken from real commits on this
repo and asserts the tier, including the cases that must never go cheap. It
needs nothing but the rules file, so it runs anywhere, including CI.

The second half checks the rules against the code they claim to cover. Every
test_ui_screens filter has to match the number of cases the rules say it
matches, every ctest target a rule names has to be a registered test that
really compiles that rule's sources, and every ui case that calls into a
subsystem has to be inside that subsystem's rule. This is the half that stops
the policy rotting. A case added, renamed or retargeted breaks it here, in a
second, rather than silently dropping out of coverage until the merge run.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import importlib.util

_spec = importlib.util.spec_from_file_location(
    "test_tier", os.path.join(HERE, "test-tier.py"))
tier = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tier)

FAILURES = []
CHECKS = [0]


def check(ok, what, detail=""):
    CHECKS[0] += 1
    if not ok:
        FAILURES.append("%s%s" % (what, ("\n      " + detail) if detail else ""))


def tier_of(paths):
    rules = RULES
    d = tier.decide([tier.normalise(p) for p in paths], rules, INDEX)
    return d.tier, d


RULES = tier.Rules()
INDEX = tier.CMakeIndex()


# ---------------------------------------------------------------------------
# Half one. Real file lists from this repo, and the cases that must never be
# cheap. Each list is what `git show --name-only` printed for that commit.
# ---------------------------------------------------------------------------

REAL_COMMITS = [
    # (short hash, subject, expected tier, files)
    ("e4eb55f", "Use the painted banner", 0, [
        "README.md",
        "docs/img/banner.png",
    ]),
    ("1be1b35", "improving readme", 0, [
        "README.md",
    ]),
    ("169efa5", "Options dialog draws over the main screen", 2, [
        # One screen file, which is tier 1 on its own, plus the ui harness.
        # The harness discount is only for a change confined to the harness,
        # so this is an ordinary tier 2.
        "src/ui/options.c",
        "src/ui/test_ui_screens.c",
    ]),
    ("b9d46e3", "Minimap draws a dot per visible unit", 2, [
        "include/tak_minimap.h",
        "include/tak_unit.h",
        "src/render/minimap.c",
        "src/render/units.c",
        "src/ui/test_ui_screens.c",
    ]),
    ("51077e4", "Playtest fixes: kill counts, sight-gated targeting", 2, [
        "docs/notes/2026-09-04-p2-gates.md",
        "include/tak_fog.h",
        "include/tak_unit.h",
        "src/game/fog.c",
        "src/game/test_fog.c",
        "src/render/units.c",
        "src/ui/hud.c",
        "src/ui/ingame.c",
        "src/ui/loading.c",
        "src/ui/test_ui_screens.c",
    ]),
    ("0265cfb", "One glob matcher for every platform", 2, [
        "include/tak_util.h",
        "src/CMakeLists.txt",
        "src/core/test_util.c",
        "src/core/util.c",
    ]),
    ("e07c10c", "Bump actions/cache from 4 to 6", 2, [
        ".github/workflows/ci.yml",
    ]),
    ("852a28f", "The page has an icon and a link preview", 2, [
        ".github/workflows/web.yml",
        "web/favicon.png",
        "web/og.jpg",
        "web/shell.html",
    ]),
    ("03463ae", "The browser smoke checks that a music track starts", 2, [
        "scripts/web-smoke.js",
        "src/sound/music.c",
    ]),
    ("7692cac", "One hash over the whole simulation", 2, [
        "include/tak_sim_hash.h",
        "include/tak_sim_rand.h",
        "src/CMakeLists.txt",
        "src/game/ai.c",
        "src/game/sim_hash.c",
        "src/game/sim_rand.c",
        "src/game/test_ai.c",
        "src/game/test_sim_hash.c",
    ]),
    ("a9eba7e", "A versioned, checksummed save container", 2, [
        "cmake/tak_build_stamp.h.in",
        "docs/notes/2026-09-11-save-container.md",
        "include/tak_bytes.h",
        "include/tak_savefile.h",
        "src/CMakeLists.txt",
        "src/core/savefile.c",
        "src/core/test_savefile.c",
        "src/net/commands.c",
    ]),
    ("38c5265", "Settings persist in the browser", 2, [
        "docs/notes/2026-09-11-where-files-are-written.md",
        "include/tak_paths.h",
        "scripts/web-smoke.js",
        "src/CMakeLists.txt",
        "src/core/paths.c",
        "src/core/settings.c",
        "src/core/test_paths.c",
        "web/shell.html",
    ]),
    ("998ede3", "Sound effects start the way the original starts them", 2, [
        "docs/notes/sound-triggers.md",
        "include/tak_sound.h",
        "src/game/world.c",
        "src/render/units.c",
        "src/sound/ambient.c",
        "src/sound/game_sound.c",
        "src/sound/sound.c",
        "src/sound/soundclass.c",
        "src/sound/test_sound.c",
        "src/ui/hud.c",
        "src/ui/ingame.c",
        "src/ui/test_ui_screens.c",
    ]),
    ("c70763e", "The multiplayer room reads right", 2, [
        "include/tak_gui.h",
        "src/ui/battle_setup.c",
        "src/ui/gui_loader.c",
        "src/ui/gui_render.c",
        "src/ui/multiplayer.c",
        "src/ui/translate.c",
    ]),
    ("f96e6dc", "Statics fit their art to their cell again", 2, [
        "include/tak_hud.h",
        "src/ui/gui_render.c",
        "src/ui/hud.c",
        "src/ui/test_ui_screens.c",
    ]),
]

# The same commits, cut down to the files an agent actually pushes partway
# through a branch. This is where the tiering is meant to pay, because a
# merged commit is the whole feature and nearly always touches a header.
REAL_SLICES = [
    ("51077e4", "the fog change on its own", 1, ["src/game/fog.c"]),
    ("51077e4", "fog plus its own test", 1,
     ["src/game/fog.c", "src/game/test_fog.c"]),
    ("51077e4", "fog plus a note", 1,
     ["src/game/fog.c", "docs/notes/2026-09-04-p2-gates.md"]),
    ("998ede3", "the sound files on their own", 1,
     ["src/sound/sound.c", "src/sound/game_sound.c", "src/sound/soundclass.c",
      "src/sound/ambient.c"]),
    ("c70763e", "the dialog loader on its own", 1,
     ["src/ui/gui_loader.c", "src/ui/gui_render.c"]),
    ("169efa5", "the options screen on its own", 1, ["src/ui/options.c"]),
    ("b9d46e3", "the minimap on its own", 1, ["src/render/minimap.c"]),
    ("0265cfb", "a test source on its own", 1, ["src/core/test_util.c"]),
    ("a9eba7e", "the command list on its own", 1, ["src/net/commands.c"]),
    ("38258c9", "the map tool on its own", 1, ["tools/map_inspect.c"]),
    ("998ede3", "one note", 0, ["docs/notes/sound-triggers.md"]),
]

NEVER_CHEAP = [
    ("a shared header", ["include/tak_unit.h"]),
    ("a leaf header", ["include/tak_fog.h"]),
    ("the harness header, mixed with product code",
     ["include/test_framework.h", "src/game/fog.c"]),
    ("the tick loop", ["src/game/world.c"]),
    ("unit simulation", ["src/render/units.c"]),
    ("the in game screen", ["src/ui/ingame.c"]),
    ("memory", ["src/core/memory.c"]),
    ("the archive reader", ["src/core/hpi/hpi.c"]),
    ("the root build file", ["CMakeLists.txt"]),
    ("the test build file", ["src/CMakeLists.txt"]),
    ("the preset file", ["CMakePresets.json"]),
    ("the package manifest", ["vcpkg.json"]),
    ("a continuous integration workflow", [".github/workflows/ci.yml"]),
    ("the pre-commit hook", [".githooks/pre-commit"]),
    ("the chooser itself", ["scripts/test-tier.py"]),
    ("the rules themselves", ["scripts/test-tiers.toml"]),
    ("the browser shell", ["web/shell.html"]),
    ("a file no rule has ever seen", ["src/game/brand_new_subsystem.c"]),
    ("a new directory no rule has ever seen", ["src/physics/rigid.c"]),
    ("a platform file", ["src/platform/sdl2_platform.c"]),
    ("a vendored library", ["third_party/stb_image.h"]),
    ("the entry point", ["src/main.c"]),
]


def half_one():
    for short, subject, want, files in REAL_COMMITS:
        got, _ = tier_of(files)
        check(got == want, "commit %s (%s) should be tier %d, chooser said %d"
              % (short, subject, want, got))
    for short, what, want, files in REAL_SLICES:
        got, _ = tier_of(files)
        check(got == want, "%s, %s, should be tier %d, chooser said %d"
              % (short, what, want, got))
    for what, files in NEVER_CHEAP:
        got, _ = tier_of(files)
        check(got == 2, "%s must be tier 2, chooser said %d" % (what, got))

    # The rules of combination.
    got, _ = tier_of(["docs/PARITY.md", "src/game/fog.c"])
    check(got == 1, "docs plus one subsystem is tier 1, got %d" % got)
    got, _ = tier_of(["src/game/fog.c", "src/render/units.c"])
    check(got == 2, "a tier 1 file plus a tier 2 file is tier 2, got %d" % got)
    got, d = tier_of(["src/game/fog.c", "src/game/economy.c"])
    check(got == 1 and len(d.entries) == 2,
          "two tier 1 entries stay tier 1 with both command sets, got %d" % got)
    got, d = tier_of(["src/game/fog.c", "src/game/economy.c",
                      "src/sound/sound.c"])
    check(got == 1, "three tier 1 entries are still tier 1, got %d" % got)
    got, d = tier_of(["src/game/fog.c", "src/game/economy.c",
                      "src/sound/sound.c", "src/ui/hud.c"])
    check(got == 2 and d.escalated,
          "four tier 1 entries escalate to tier 2, got %d escalated=%s"
          % (got, d.escalated))

    # The harness carve out, and its edge.
    got, d = tier_of(["src/ui/test_ui_screens.c"])
    check(got == 2 and d.trees == 1 and d.harness_only,
          "the ui harness alone is the full suite on one tree, got tier %d "
          "on %d tree(s)" % (got, d.trees))
    got, d = tier_of(["include/test_framework.h", "include/test_hpi_builder.h",
                      "src/ui/test_ui_screens.c"])
    check(got == 2 and d.trees == 1,
          "the harness files together are one tree, got %d tree(s)" % d.trees)
    got, d = tier_of(["src/ui/test_ui_screens.c", "src/render/units.c"])
    check(got == 2 and d.trees == 2,
          "the harness mixed with product code is both trees, got %d tree(s)"
          % d.trees)

    # Windows path separators and a leading ./ must not change the answer.
    got, _ = tier_of(["src\\game\\fog.c"])
    check(got == 1, "a backslash path should still be tier 1, got %d" % got)
    got, _ = tier_of(["./src/render/units.c"])
    check(got == 2, "a ./ prefixed path should still be tier 2, got %d" % got)

    # A test source that builds no registered target cannot be tier 1, and
    # every way of asking has to agree about that.
    got, d = tier_of(["src/game/test_not_registered_yet.c"])
    check(got == 2 and d.problem,
          "an unregistered test source must fall to tier 2, got %d" % got)

    # A tier 1 run has to name real work.
    _, d = tier_of(["src/game/fog.c"])
    targets, ui, problem = tier.plan_for(d, INDEX)
    names_run = [f for f, _ in ui]
    check(problem is None and any("test_fog" in t for t in targets)
          and len(ui) >= 9
          and "trebuchet_waits_for_a_spotter" in names_run,
          "the fog rule should run test_fog and the sight gated cases, got "
          "%s and %d invocations" % (targets, len(ui)))
    _, d = tier_of(["src/core/test_util.c"])
    targets, ui, problem = tier.plan_for(d, INDEX)
    check(targets == ["^(test_util)$"],
          "a test source runs its own target, got %s" % targets)
    _, d = tier_of(["src/render/test_cob_vm.c"])
    targets, ui, problem = tier.plan_for(d, INDEX)
    check(targets and "test_cob_vm_corpus" in targets[0],
          "a test source that builds three targets runs all three, got %s"
          % targets)

    # The plan the chooser writes has to be a script that actually parses.
    # Its ui helper is quoted shell inside quoted Python, which is exactly
    # the sort of thing that breaks silently.
    import shutil
    import subprocess
    if shutil.which("bash"):
        for files in (["src/game/fog.c"], ["src/ui/hud.c", "src/net/commands.c"],
                      ["src/sound/sound.c", "src/game/economy.c"]):
            _, d = tier_of(files)
            targets, ui, _problem = tier.plan_for(d, INDEX)
            text = tier.render_plan_script(d, targets, ui, None, "Release",
                                           INDEX.declared_test_count())
            # Read on standard input, so no path has to survive the shell,
            # and as bytes, so Windows does not turn the line endings into
            # carriage returns the shell then chokes on.
            done = subprocess.run(["bash", "-n"], input=text.encode("utf-8"),
                                  capture_output=True)
            check(done.returncode == 0,
                  "the plan written for %s is not a valid script" % files,
                  done.stderr.decode("utf-8", "replace").strip())


# ---------------------------------------------------------------------------
# Half two. The rules against the code.
# ---------------------------------------------------------------------------

UI_SOURCE = os.path.join(REPO, "src", "ui", "test_ui_screens.c")
EXPORTED = re.compile(
    r"^(?!static\b|typedef\b|#|\s)(?:[A-Za-z_][\w\*\s]*?)\b([A-Za-z_]\w*)\s*\([^;]*$",
    re.M)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def ui_cases():
    text = read(UI_SOURCE)
    names = re.findall(tier.RUN_UI_TEST_RE, text)
    parts = re.split(r"\nTEST\((\w+)\)", text)
    bodies = {parts[i]: parts[i + 1] for i in range(1, len(parts), 2)}
    return names, bodies


def half_two():
    names, bodies = ui_cases()
    check(len(names) > 100, "the ui harness should hold the whole screen "
          "suite, found %d cases" % len(names))
    check(INDEX.ok and INDEX.declared_test_count() > 0,
          "src/CMakeLists.txt should parse into registered tests")

    # A test name the CMake reader could not resolve, because the test
    # is registered through something it does not follow. Left quiet
    # it would compare rules against a name that exists nowhere, which
    # is how a rule passes while covering nothing.
    for name in sorted(INDEX.tests):
        check("${" not in name,
              "test name %s came out of src/CMakeLists.txt unresolved,"
              " so the rules cannot be checked against it" % name)

    registered = set(INDEX.tests)

    for entry in RULES.tier1:
        name = entry["name"]

        # Every path a rule names has to exist. A rename that misses the
        # rules file would otherwise leave the rule matching nothing and the
        # renamed file falling to tier 2 silently.
        for p in entry["paths"]:
            if "*" in p:
                continue
            check(os.path.exists(os.path.join(REPO, p)),
                  "rule %s names %s, which is not in the tree" % (name, p))

        # Every filter has to match the number of cases the rule claims.
        for f, want in entry.get("ui", []):
            got = sum(1 for n in names if f in n)
            check(got == want,
                  'rule %s: filter "%s" matches %d cases, the rules say %d'
                  % (name, f, got, want))
            check(got > 0,
                  'rule %s: filter "%s" matches nothing, so that invocation '
                  "would pass without running anything" % (name, f))

        # Every ctest target a rule names has to be registered, and has to be
        # reachable from the sources the rule covers.
        regex = entry.get("ctest", "")
        named = set()
        if regex:
            rx = re.compile(regex)
            named = {t for t in registered if rx.search(t)}
            check(named, "rule %s names ctest %s, which matches no registered "
                  "test" % (name, regex))
        if entry.get("kind") == "own-target":
            continue
        for p in entry["paths"]:
            if "*" in p:
                continue
            reached = set(INDEX.targets_for(p))
            missing = reached - named
            check(not missing,
                  "rule %s: %s is compiled into %s, which the rule does not "
                  "run" % (name, p, ", ".join(sorted(missing))))

        # Every ui case that calls into this subsystem has to be covered by
        # one of the rule filters. This is the check that catches a new case
        # for an old subsystem.
        symbols = set()
        for p in entry["paths"]:
            full = os.path.join(REPO, p)
            if "*" in p or not os.path.exists(full):
                continue
            symbols |= {s for s in EXPORTED.findall(read(full)) if len(s) > 4}
        if not symbols:
            continue
        calls = re.compile(r"\b(?:%s)\s*\(" % "|".join(sorted(symbols)))
        filters = [f for f, _ in entry.get("ui", [])]
        for case in names:
            body = bodies.get(case)
            if not body or not calls.search(body):
                continue
            check(any(f in case for f in filters),
                  "rule %s does not run %s, which calls into it" % (name, case))

    # The harness set is three files and they all exist.
    for p in RULES.harness:
        check(os.path.exists(os.path.join(REPO, p)),
              "the harness set names %s, which is not in the tree" % p)

    # No file may be tier 0 and something else at once, and the only place a
    # tier 1 rule is allowed to win over a tier 2 rule is a test source.
    tier0_rx = [tier.glob_to_re(p) for p in RULES.tier0_src]
    tier1_rx = [(e["name"], rx) for e in RULES.tier1 for rx in e["_re"]]
    tier2_rx = [tier.glob_to_re(p) for p in RULES.tier2_src]
    for path in tracked_files():
        zero = any(rx.match(path) for rx in tier0_rx)
        one = [n for n, rx in tier1_rx if rx.match(path)]
        two = any(rx.match(path) for rx in tier2_rx)
        check(not (zero and (one or two)),
              "%s matches tier 0 and also %s" % (path, one or "tier 2"))
        if one and two:
            check(re.match(r"^src/(.*/)?test_[^/]*\.c$", path),
                  "%s matches tier 1 rule %s and a tier 2 rule, and it is not "
                  "a test source" % (path, one))
        check(len(set(one)) < 2,
              "%s matches more than one tier 1 rule: %s" % (path, one))

    # Nothing executable or compiled is allowed into tier 0.
    for path in tracked_files():
        if any(rx.match(path) for rx in tier0_rx):
            # Issue templates and dependabot are declarative config that
            # nothing runs. A workflow is a program, and so is everything
            # that gets compiled or interpreted.
            check(not path.endswith((".c", ".h", ".py", ".sh", ".ps1", ".js",
                                     ".cmake", ".in")),
                  "tier 0 must not hold anything executable or compiled, but "
                  "it matches %s" % path)
            check(not path.startswith((".github/workflows/", ".githooks/",
                                       "scripts/")),
                  "tier 0 must not hold anything that runs, but it matches %s"
                  % path)


def tracked_files():
    if not hasattr(tracked_files, "cache"):
        import subprocess
        try:
            out = subprocess.run(["git", "ls-files"], cwd=REPO, text=True,
                                 capture_output=True, check=True).stdout
            tracked_files.cache = [l.strip() for l in out.split("\n") if l.strip()]
        except Exception:
            tracked_files.cache = []
            for root, _, files in os.walk(REPO):
                if ".git" in root:
                    continue
                for f in files:
                    rel = os.path.relpath(os.path.join(root, f), REPO)
                    tracked_files.cache.append(rel.replace("\\", "/"))
    return tracked_files.cache


def main():
    half_one()
    half_two()
    print("ran %d checks" % CHECKS[0])
    if FAILURES:
        print("")
        for f in FAILURES:
            print("FAIL  %s" % f)
        print("")
        print("%d of %d checks failed" % (len(FAILURES), CHECKS[0]))
        print("If a ui case moved, add it to the rule in "
              "scripts/test-tiers.toml and fix the expected count there.")
        return 1
    print("the tier rules agree with the tree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
