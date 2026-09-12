#!/usr/bin/env python3
"""Print which tests a change has to run, and the exact commands to run them.

    python scripts/test-tier.py $(git diff --name-only origin/main...HEAD)
    python scripts/test-tier.py                 # works the file list out itself

The tier comes from the paths, mechanically. The rules are data in
scripts/test-tiers.toml and the reasoning is in docs/testing-tiers.md. This
file decides nothing on its own, so a rule change is a commit and a review.

Useful options:
    --build-dir DIR   name the build tree, so the printed commands are literal
    --config CFG      Release (default) or Debug
    --plan            also write a runnable script that checks its own counts
    --tier-only       print 0, 1 or 2 and nothing else, for a script or a hook
    --json            the whole decision as JSON
    --check-tree DIR  compare ctest -N in DIR against the declared test count
    --explain         say which rule matched every path
"""

import argparse
import json
import os
import posixpath
import re
import subprocess
import sys

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python 3.10 and older
    sys.stderr.write("test-tier.py needs Python 3.11 or newer for tomllib\n")
    raise SystemExit(64)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
RULES = os.path.join(HERE, "test-tiers.toml")
LOCK = "source <scratchpad>/testlock.sh   # then: with_test_lock <command>"


# --------------------------------------------------------------------------
# path matching
# --------------------------------------------------------------------------

def glob_to_re(pattern):
    """Translate a rule glob. ** spans directories, * does not."""
    out = []
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(c))
            i += 1
    return re.compile("^" + "".join(out) + "$")


def normalise(path):
    p = path.strip().replace("\\", "/").strip('"')
    while p.startswith("./"):
        p = p[2:]
    return p


class Rules:
    def __init__(self, path=RULES):
        with open(path, "rb") as fh:
            self.raw = tomllib.load(fh)
        self.path = path
        self.policy = self.raw.get("policy", {})
        self.tier0 = [glob_to_re(p) for p in self.raw["tier0"]["paths"]]
        self.tier0_src = self.raw["tier0"]["paths"]
        self.tier2 = [glob_to_re(p) for p in self.raw["tier2"]["paths"]]
        self.tier2_src = self.raw["tier2"]["paths"]
        self.harness = set(self.raw["tier2"]["harness"]["paths"])
        self.tier1 = []
        for entry in self.raw["tier1"]:
            entry = dict(entry)
            entry["_re"] = [glob_to_re(p) for p in entry["paths"]]
            self.tier1.append(entry)

    def match(self, path):
        """Return (tier, rule name). Order is documented in the rules file."""
        if path in self.harness:
            return 2, "harness"
        for entry in self.tier1:
            if any(rx.match(path) for rx in entry["_re"]):
                return 1, entry["name"]
        for rx, src in zip(self.tier0, self.tier0_src):
            if rx.match(path):
                return 0, src
        for rx, src in zip(self.tier2, self.tier2_src):
            if rx.match(path):
                return 2, src
        return 2, "no rule matched"


# --------------------------------------------------------------------------
# what the build knows about itself
# --------------------------------------------------------------------------

class CMakeIndex:
    """Which registered ctest targets compile a given source file.

    Parsed from src/CMakeLists.txt so the answer cannot drift from the build.
    """

    def __init__(self, repo=REPO):
        self.ok = False
        self.tests = {}
        self.sources = {}
        try:
            with open(os.path.join(repo, "src", "CMakeLists.txt"),
                      encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            return
        text = re.sub(r"#[^\n]*", "", text)
        variables = {}

        def expand(s):
            # An unknown name is left as it was, so ${CMAKE_SOURCE_DIR}/tools
            # survives to be resolved below instead of collapsing to /tools.
            for _ in range(6):
                s = re.sub(r"\$\{(\w+)\}",
                           lambda m: " ".join(variables[m.group(1)])
                           if m.group(1) in variables else m.group(0), s)
            return s

        for m in re.finditer(r"\bset\(\s*(\w+)([^()]*)\)", text):
            variables[m.group(1)] = expand(m.group(2)).split()
        exe_sources = {}
        for m in re.finditer(r"\badd_executable\(\s*([\w-]+)([^()]*)\)", text):
            items = expand(m.group(2)).split()
            files = []
            for item in items:
                if not item.endswith(".c"):
                    continue
                item = item.replace("${CMAKE_SOURCE_DIR}/", "")
                if not item.startswith(("src/", "tools/", "third_party/")):
                    item = "src/" + item
                files.append(posixpath.normpath(item))
            exe_sources[m.group(1)] = files
        for m in re.finditer(r"add_test\(NAME\s+(\S+)\s+COMMAND\s+([\w-]+)", text):
            self.tests[m.group(1)] = m.group(2)
        self.exe_sources = exe_sources
        for test, exe in self.tests.items():
            for src in exe_sources.get(exe, []):
                self.sources.setdefault(src, set()).add(test)
        self.ok = bool(self.tests)

    def declared_test_count(self):
        return len(self.tests)

    def targets_for(self, source, exclude=("test_ui_screens",)):
        return sorted(t for t in self.sources.get(source, set())
                      if t not in exclude)


# --------------------------------------------------------------------------
# the decision
# --------------------------------------------------------------------------

class Decision:
    def __init__(self):
        self.tier = 0
        self.per_path = []
        self.entries = []
        self.reasons = []
        self.trees = 0
        self.escalated = False
        self.harness_only = False


def decide(paths, rules, index):
    d = Decision()
    names = []
    for path in paths:
        tier, why = rules.match(path)
        d.per_path.append((path, tier, why))
        d.tier = max(d.tier, tier)
        if tier == 1 and why not in names:
            names.append(why)
        if tier == 2:
            d.reasons.append(path if why == path
                             else "%s  (rule: %s)" % (path, why))
    d.entries = [e for e in rules.tier1 if e["name"] in names]

    limit = rules.policy.get("escalate_at_distinct_entries", 4)
    if d.tier == 1 and len(names) >= limit:
        d.tier = 2
        d.escalated = True
        d.reasons.append(
            "%d distinct tier 1 entries (%s). The union is approaching the "
            "whole binary and a named case list cannot see across subsystems."
            % (len(names), ", ".join(names)))

    if paths and all(p in rules.harness for p in paths):
        d.harness_only = True

    if d.tier == 2:
        d.trees = 1 if d.harness_only else 2
    elif d.tier == 1:
        d.trees = 1
    return d


def plan_for(decision, index):
    """The ctest regexes and ui invocations a tier 1 run has to cover."""
    targets = []
    ui = []
    seen = set()
    for entry in decision.entries:
        if entry.get("kind") == "own-target":
            continue
        if entry.get("ctest"):
            targets.append(entry["ctest"])
        for f, n in entry.get("ui", []):
            if f not in seen:
                seen.add(f)
                ui.append((f, n))
    # the own-target entry resolves its targets from the build itself
    own = []
    for path, tier, why in decision.per_path:
        if tier == 1 and why == "test source":
            found = index.targets_for(path) if index.ok else []
            if not found:
                own.append(None)
            else:
                own.extend(found)
    if None in own:
        return None, None, "a test source builds no registered target"
    if own:
        # Anything an entry already runs does not need naming twice.
        already = set()
        for regex in targets:
            rx = re.compile(regex)
            already |= {t for t in index.tests if rx.search(t)}
        rest = sorted(set(own) - already)
        if rest:
            targets.append("^(%s)$" % "|".join(rest))
    return targets, ui, None


# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------

def ui_exe(build, config):
    if build:
        for candidate in ("%s/src/%s/test_ui_screens.exe" % (build, config),
                          "%s/src/test_ui_screens" % build,
                          "%s/src/test_ui_screens.exe" % build):
            if os.path.exists(candidate):
                return candidate
    return "$BUILD/src/$CONFIG/test_ui_screens.exe"


def render_plan_script(decision, targets, ui, build, config, count):
    b = build or "D:/OKBuild/<your tree>"
    lines = [
        "#!/usr/bin/env bash",
        "# Written by scripts/test-tier.py. One lock take covers this whole",
        "# script, so run it as: with_test_lock bash <this file>",
        "set -uo pipefail",
        'BUILD="%s"' % b,
        'CONFIG="%s"' % config,
        'UI="%s"' % ui_exe(build, config).replace(b, "$BUILD"),
        "fail=0",
        "",
        "# A tree that does not know about every test can skip the one target",
        "# this run depends on and still exit 0.",
        'have=$(ctest --test-dir "$BUILD" -C "$CONFIG" -N | sed -n "s/^Total Tests: //p")',
        'if [ "$have" != "%d" ]; then' % count,
        '  echo "STALE TREE: ctest -N reports ${have:-none}, src/CMakeLists.txt declares %d. Reconfigure."' % count,
        "  exit 1",
        "fi",
        'echo "tree knows $have tests"',
        "",
        "# A filter that matches nothing exits 0 with 0 total, so every ui",
        "# invocation states the number of cases it expects.",
        "ui_run() {",
        '  local filter="$1" want="$2" out line',
        '  out=$("$UI" "$filter" 2>&1); rc=$?',
        '  line=$(printf "%s\\n" "$out" | grep -E "^Results:" | tail -1)',
        '  if [ $rc -ne 0 ] || [ "$line" != "Results: $want passed, 0 failed, $want total" ]; then',
        '    printf "%s\\n" "$out"',
        '    echo "FAILED  ui \\"$filter\\"  got: ${line:-no Results line}  want: Results: $want passed, 0 failed, $want total"',
        "    fail=1",
        "  else",
        '    echo "ok      ui \\"$filter\\"  $line"',
        "  fi",
        "}",
        "",
    ]
    for regex in targets:
        lines += [
            'echo "== ctest -R %s"' % regex,
            'ctest --test-dir "$BUILD" -C "$CONFIG" -R "%s" --output-on-failure || fail=1' % regex,
            "",
        ]
    if ui:
        lines.append('echo "== test_ui_screens, %d invocations"'
                     % len(ui))
        for f, n in ui:
            lines.append('ui_run "%s" %d' % (f, n))
        lines.append("")
    lines += [
        'if [ $fail -ne 0 ]; then echo "TIER 1 RUN FAILED"; exit 1; fi',
        'echo "TIER 1 RUN CLEAN. This branch is tier 1 clean, not merge ready."',
        'echo "The full suite on both data layouts still gates the merge."',
    ]
    return "\n".join(lines) + "\n"


def print_report(decision, rules, index, args):
    config = args.config
    build = args.build_dir
    b = build or "D:/OKBuild/<your tree>"
    count = index.declared_test_count() if index.ok else 0
    out = print

    if decision.tier == 0:
        out("TIER 0   no tests, no lock take")
        out("")
        out("  Nothing here is compiled or executed. Commit and push. The")
        out("  pre-commit hook and the licence scan in CI are the only")
        out("  checks and both take seconds.")
        return 0

    if decision.tier == 1:
        targets, ui, problem = plan_for(decision, index)
        if problem:
            out("TIER 2   %s, so the rules cannot vouch for this change" % problem)
            decision.tier = 2
        else:
            cases = sum(n for _, n in ui)
            out("TIER 1   targeted, one lock take, one build tree")
            out("  matched: %s" % ", ".join(
                sorted({w for _, t, w in decision.per_path if t == 1})))
            out("  %d ctest invocation(s), %d test_ui_screens invocation(s), %d cases"
                % (len(targets), len(ui), cases))
            out("")
            out("  1. Build outside the lock.")
            out("       cmake --build %s --config %s -- -m:2" % (b, config))
            out("  2. Run it. One lock take for the whole chain.")
            out("       %s" % LOCK)
            if args.plan:
                path = args.plan_file or os.path.join(
                    build or ".", "test-tier-plan.sh")
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(render_plan_script(decision, targets, ui, build,
                                                config, count))
                out("       with_test_lock bash %s" % path.replace("\\", "/"))
                out("")
                out("  That script checks the tree is not stale, runs every")
                out("  target and every filter, and fails if any filter")
                out("  matches a different number of cases than the rules")
                out("  expect. Paste its output.")
            else:
                out("       with_test_lock bash <(python scripts/test-tier.py --plan-stdout %s)"
                    % " ".join(p for p, _, _ in decision.per_path))
                out("")
                out("     or write the script out and read it first:")
                out("       python scripts/test-tier.py --build-dir %s --plan %s"
                    % (b, " ".join(p for p, _, _ in decision.per_path)))
            out("")
            out("  What that runs:")
            for regex in targets:
                out('     ctest --test-dir %s -C %s -R "%s" --output-on-failure'
                    % (b, config, regex))
            for f, n in ui:
                out('     %s "%s"   expect Results: %d passed, 0 failed, %d total'
                    % (ui_exe(build, config), f, n, n))
            out("")
            out("  3. Paste every Results line with the number the rules")
            out("     expect beside it. A filter that matches nothing exits 0")
            out("     with 0 total, so an unchecked ui run proves nothing.")
            out("  4. Tier 1 clean is not merge ready. The full suite on both")
            out("     data layouts still gates the merge.")
            return 0

    trees = decision.trees
    out("TIER 2   the full suite%s" % ("" if trees == 2 else ", one data layout"))
    if decision.harness_only:
        out("  This change is confined to the test harness, so it cannot")
        out("  alter shipped behaviour. One data layout, one lock take.")
    for reason in decision.reasons[:6]:
        out("  %s" % reason)
    if len(decision.reasons) > 6:
        out("  and %d more" % (len(decision.reasons) - 6))
    out("")
    out("  1. Build the tree%s outside the lock, -m:2." % ("" if trees == 1 else "s"))
    out("  2. ctest -N must report %d tests in %s. Fewer means the tree"
        % (count, "each tree" if trees == 2 else "the tree"))
    out("     is stale and a run there is a false green. Reconfigure first.")
    out("  3. %s" % LOCK)
    out('       with_test_lock ctest --test-dir %s -C %s --output-on-failure' % (b, config))
    if trees == 2:
        out("     Release the lock, then take it again for the second tree.")
        out('       with_test_lock ctest --test-dir %s-ip -C %s --output-on-failure' % (b, config))
        out("     Two takes, never one. The queue has to drain between them")
        out("     and no single take should run over 20 minutes.")
    out("  4. Paste the \"100%% tests passed, 0 tests failed out of %d\" line"
        % count)
    out("     from %s." % ("both trees" if trees == 2 else "the tree"))
    return 0


# --------------------------------------------------------------------------

def changed_from_git(base):
    def run(*args):
        try:
            return subprocess.run(args, cwd=REPO, capture_output=True,
                                  text=True, check=True).stdout.split("\n")
        except (OSError, subprocess.CalledProcessError):
            return []
    files = set()
    for line in run("git", "diff", "--name-only", "%s...HEAD" % base):
        if line.strip():
            files.add(normalise(line))
    for line in run("git", "status", "--porcelain"):
        if len(line) > 3:
            files.add(normalise(line[3:].split(" -> ")[-1]))
    return sorted(files)


def check_tree(build, config, count):
    # A build tree can belong to another worktree, so count what its own
    # source tree declares rather than what this one does.
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.exists(cache):
        with open(cache, encoding="utf-8", errors="replace") as fh:
            found = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$",
                              fh.read(), re.M)
        if found:
            home = found.group(1).strip()
            other = CMakeIndex(home)
            if other.ok and home != REPO:
                print("the tree was configured from %s" % home)
                count = other.declared_test_count()
    try:
        proc = subprocess.run(["ctest", "--test-dir", build, "-C", config, "-N"],
                              capture_output=True, text=True)
    except OSError as exc:
        print("could not run ctest: %s" % exc)
        return 1
    found = re.search(r"Total Tests: (\d+)", proc.stdout)
    have = int(found.group(1)) if found else -1
    if have == count:
        print("OK   %s knows all %d tests" % (build, count))
        return 0
    print("STALE  %s reports %s, src/CMakeLists.txt declares %d."
          % (build, have if have >= 0 else "no total", count))
    print("       A targeted run in this tree can skip the target it depends")
    print("       on and still exit 0. Reconfigure before running anything.")
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="changed files")
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--build-dir", default=None)
    ap.add_argument("--config", default=None)
    ap.add_argument("--rules", default=RULES)
    ap.add_argument("--tier-only", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--explain", action="store_true")
    ap.add_argument("--plan", action="store_true",
                    help="write a runnable script that checks its own counts")
    ap.add_argument("--plan-file", default=None)
    ap.add_argument("--plan-stdout", action="store_true")
    ap.add_argument("--check-tree", default=None)
    args = ap.parse_args(argv)

    rules = Rules(args.rules)
    index = CMakeIndex()
    if args.config is None:
        args.config = rules.policy.get("default_config", "Release")

    if args.check_tree:
        return check_tree(args.check_tree, args.config,
                          index.declared_test_count())

    paths = [normalise(p) for p in args.paths if p.strip()]
    from_git = False
    if not paths:
        paths = changed_from_git(args.base)
        from_git = True
    if not paths:
        if args.tier_only:
            print(0)
        else:
            print("TIER 0   nothing changed against %s" % args.base)
        return 0

    decision = decide(paths, rules, index)

    if args.tier_only:
        print(decision.tier)
        return 0
    if args.json:
        targets, ui, problem = plan_for(decision, index)
        print(json.dumps({
            "tier": decision.tier,
            "trees": decision.trees,
            "entries": [e["name"] for e in decision.entries],
            "escalated": decision.escalated,
            "harness_only": decision.harness_only,
            "ctest": targets or [],
            "ui": [{"filter": f, "expect": n} for f, n in (ui or [])],
            "paths": [{"path": p, "tier": t, "rule": w}
                      for p, t, w in decision.per_path],
            "declared_tests": index.declared_test_count(),
        }, indent=2))
        return 0
    if args.plan_stdout:
        targets, ui, problem = plan_for(decision, index)
        if decision.tier != 1 or problem:
            sys.stderr.write("--plan-stdout only makes sense at tier 1\n")
            return 64
        sys.stdout.write(render_plan_script(decision, targets, ui,
                                            args.build_dir, args.config,
                                            index.declared_test_count()))
        return 0

    if from_git:
        print("%d changed file(s) against %s" % (len(paths), args.base))
    if not index.ok:
        print("WARNING  src/CMakeLists.txt could not be read, so the test")
        print("         count and the test source rule cannot be resolved.")
    rc = print_report(decision, rules, index, args)
    if args.explain:
        print("")
        print("  how each path was decided")
        for path, tier, why in decision.per_path:
            print("    tier %d  %-44s %s" % (tier, path, why))
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
