#!/usr/bin/env python3
"""Pick the tests a change needs from what the tests really run.

    python scripts/test-pick.py [changed files] [--base origin/main]
                                [--build-dir DIR] [--config Release] [-j 6]
                                [--plan FILE]

scripts/test-coverage.json records, for every ctest test and every
screen suite case, the source files it executed (scripts/coverage-map.py
builds it). A changed source file selects the tests and cases that ran
it. A changed header selects everything that ran a source including it.
Documentation and other files the tier rules call tier 0 select nothing.
Anything else, a build file, a script, a new source the map has never
seen, selects the full suite, on both data layouts.

--plan writes a script that runs the selection on both data layouts,
the screen cases spread over parallel processes. Run it under the test
lock. The full suite on both layouts still gates the merge:
scripts/run-suite.sh.
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MAP = os.path.join(HERE, "test-coverage.json")
TIMES = os.path.join(REPO, "src", "ui", "test_ui_case_times.h")
STALE_AFTER = 60   # commits since the map was recorded


def load_tier_module():
    spec = importlib.util.spec_from_file_location(
        "test_tier", os.path.join(HERE, "test-tier.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def case_times():
    import re
    try:
        with open(TIMES, encoding="utf-8") as f:
            return {n: int(ms) for n, ms in
                    re.findall(r'\{ "(\w+)", (\d+) \}', f.read())}
    except OSError:
        return {}


class Pick:
    def __init__(self):
        self.full = False
        self.why = []
        self.tests = set()
        self.cases = set()
        self.ignored = []


def pick(paths, cov, rules):
    p = Pick()
    files = cov["files"]
    index = {f: i for i, f in enumerate(files)}
    tests_by_file, cases_by_file = {}, {}
    for t, ids in cov["tests"].items():
        for i in ids:
            tests_by_file.setdefault(i, set()).add(t)
    for c, ids in cov["ui_cases"].items():
        for i in ids:
            cases_by_file.setdefault(i, set()).add(c)

    def select_source(i):
        p.tests |= tests_by_file.get(i, set())
        p.cases |= cases_by_file.get(i, set())

    for path in paths:
        tier, why = rules.match(path)
        if tier == 0:
            p.ignored.append(path)
            continue
        if path.endswith(".h") and path in cov["headers"]:
            for i in cov["headers"][path]:
                select_source(i)
            if path in index:
                select_source(index[path])
            continue
        if path.endswith(".c") and path in index:
            select_source(index[path])
            continue
        p.full = True
        if path.endswith((".c", ".h")):
            p.why.append("%s is not in the coverage map (new, or no test "
                         "runs it). Regenerate the map." % path)
        else:
            p.why.append("%s is not code the map can trace" % path)
    # The screen shards are one binary: pick its cases, not its ctest names.
    shard_tests = {t for t in p.tests if t.startswith("test_ui_screens_")}
    p.tests -= shard_tests
    if p.cases:
        p.tests.add("test_ui_screens_shards")
    return p


def map_age(cov):
    try:
        out = subprocess.run(["git", "-C", REPO, "rev-list", "--count",
                              "%s..HEAD" % cov["commit"]],
                             capture_output=True, text=True)
        return int(out.stdout.strip())
    except (ValueError, KeyError):
        return None


def plan_script(p, build, config, jobs, cov):
    b = build or "${BUILD:?set BUILD or pass --build-dir}"
    lines = [
        "#!/usr/bin/env bash",
        "# Written by scripts/test-pick.py. Run it once under the test lock.",
        "set -uo pipefail",
        'BUILD="%s"' % b,
        'CONFIG="%s"' % config,
        'JOBS=%d' % jobs,
        "fail=0",
    ]
    if p.full:
        lines += ['bash "%s/scripts/run-suite.sh" "$BUILD" "$CONFIG" "$JOBS" || fail=1'
                  % REPO.replace("\\", "/")]
    else:
        ui = 'UI="$BUILD/src/$CONFIG/test_ui_screens.exe"; [ -x "$UI" ] || UI="$BUILD/src/test_ui_screens"'
        lines.append(ui)
        lines.append('mkdir -p "$BUILD/empty-data"')
        if p.cases:
            lines.append("cat > \"$BUILD/pick-cases.txt\" <<'CASES'")
            lines += sorted(p.cases)
            lines.append("CASES")
        lines += [
            "run_layout() {",
            '  echo "== layout: $1"',
        ]
        if p.tests:
            regex = "^(%s)$" % "|".join(sorted(p.tests))
            lines.append('  ctest --test-dir "$BUILD" -C "$CONFIG" -j "$JOBS" '
                         '-R "%s" --output-on-failure || fail=1' % regex)
        if p.cases:
            lines += [
                '  local n=$(( JOBS < %d ? JOBS : %d ))' % (len(p.cases), len(p.cases)),
                '  local pids=() k',
                '  for k in $(seq 1 $n); do',
                '    SDL_VIDEODRIVER=dummy "$UI" --case-list="$BUILD/pick-cases.txt" '
                '--shard=$k/$n > "$BUILD/pick-ui-$k.log" 2>&1 & pids+=($!)',
                "  done",
                '  for k in $(seq 1 $n); do',
                '    if ! wait "${pids[$((k-1))]}"; then fail=1; '
                'grep -E "FAIL|BROKEN" "$BUILD/pick-ui-$k.log"; fi',
                '    grep "^Results:" "$BUILD/pick-ui-$k.log"',
                "  done",
            ]
        lines += [
            "}",
            "run_layout extracted",
            'TAK_TEST_DATA_DIR="$BUILD/empty-data" run_layout archives',
        ]
    lines += [
        'if [ $fail -ne 0 ]; then echo "PICKED RUN FAILED"; exit 1; fi',
        'echo "PICKED RUN CLEAN. The full suite still gates the merge."',
    ]
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--build-dir")
    ap.add_argument("--config", default="Release")
    ap.add_argument("-j", type=int, default=6)
    ap.add_argument("--plan")
    a = ap.parse_args(argv)

    tier = load_tier_module()
    rules = tier.Rules()
    paths = [tier.normalise(x) for x in a.paths] or tier.changed_from_git(a.base)

    try:
        with open(MAP, encoding="utf-8") as f:
            cov = json.load(f)
    except OSError:
        cov = None
    if cov is None:
        p = Pick()
        p.full = True
        p.why.append("no scripts/test-coverage.json; build one with "
                     "scripts/coverage-map.py")
    else:
        p = pick(paths, cov, rules)
        age = map_age(cov)
        if age is not None and age > STALE_AFTER:
            print("NOTE: the coverage map is %d commits old. Regenerate it "
                  "with scripts/coverage-map.py." % age)

    times = case_times()
    if p.full:
        print("FULL SUITE, both data layouts:")
        for w in p.why:
            print("  " + w)
    elif not p.tests and not p.cases:
        print("NOTHING TO RUN: %s" % (", ".join(p.ignored) or "no changes"))
    else:
        ms = sum(times.get(c, 1000) for c in p.cases)
        print("%d ctest test(s), %d screen case(s), about %.0f s of cases"
              % (len(p.tests), len(p.cases), ms / 1000.0))
        for t in sorted(p.tests):
            print("  ctest  " + t)
        for c in sorted(p.cases):
            print("  case   " + c)
    if a.plan:
        with open(a.plan, "w", newline="\n") as f:
            f.write(plan_script(p, a.build_dir, a.config, a.j, cov))
        print("plan written to %s; run: with_test_lock bash %s" % (a.plan, a.plan))
    return 0


if __name__ == "__main__":
    sys.exit(main())
