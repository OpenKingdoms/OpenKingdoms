#!/usr/bin/env python3
"""Record which source files every test, and every screen suite case, runs.

    python3 scripts/coverage-map.py --data-dir DIR --game-dir DIR [--build DIR] [-j N]

Linux only (WSL is fine). Builds the tree with TAK_TEST_TRACE=ON, runs
the whole suite with each case's entered functions written beside the
test, maps the addresses to source files with nm, and writes
scripts/test-coverage.json. scripts/test-tier.py reads that file to pick
the tests a change needs.

Regenerate it when test-tier.py says the map is stale: a source file it
does not know, or a map recorded too many commits ago.
"""
import argparse
import bisect
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "scripts", "test-coverage.json")


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def rel(path):
    path = os.path.normpath(path)
    r = os.path.relpath(path, ROOT)
    return None if r.startswith("..") else r.replace(os.sep, "/")


def symbol_files(binary, cache):
    """Sorted function start addresses and the repo file each is in."""
    if binary in cache:
        return cache[binary]
    out = subprocess.run(["nm", "-l", "--defined-only", binary],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]+) [tTwW] \S+\t(.+):\d+$", line)
        if m:
            f = rel(m.group(2))
            if f:
                rows.append((int(m.group(1), 16), f))
    rows.sort()
    cache[binary] = ([a for a, _ in rows], [f for _, f in rows])
    return cache[binary]


def files_for(addresses, table):
    addrs, files = table
    got = set()
    for a in addresses:
        i = bisect.bisect_right(addrs, a) - 1
        if i >= 0 and addrs[i] == a:
            got.add(files[i])
    return got


def header_users(build):
    """repo header -> repo sources whose objects depend on it."""
    out = subprocess.run(["ninja", "-C", build, "-t", "deps"],
                         capture_output=True, text=True, check=True).stdout
    users, source = {}, None
    for line in out.splitlines():
        if line and not line.startswith(" "):
            source = None
            continue
        dep = line.strip()
        if not dep:
            continue
        r = rel(dep if os.path.isabs(dep) else os.path.join(build, dep))
        if not r:
            continue
        if r.endswith(".c") and source is None:
            source = r
        elif r.endswith(".h") and source:
            users.setdefault(r, set()).add(source)
    return users


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--build", default=os.path.expanduser("~/okbuild/trace"))
    ap.add_argument("-j", type=int, default=4)
    a = ap.parse_args()

    run(["cmake", "-S", ROOT, "-B", a.build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DTAK_TEST_TRACE=ON",
         "-DTAK_DATA_DIR=" + a.data_dir, "-DTAK_GAME_DIR=" + a.game_dir])
    run(["cmake", "--build", a.build, "-j", str(a.j)])

    listing = json.loads(subprocess.run(
        ["ctest", "--test-dir", a.build, "--show-only=json-v1"],
        capture_output=True, text=True, check=True).stdout)
    tests = {}
    for t in listing["tests"]:
        props = {p["name"]: p["value"] for p in t.get("properties", [])}
        wd = props.get("WORKING_DIRECTORY")
        cmd = t.get("command") or []
        if wd and cmd and os.path.isfile(cmd[0]):
            tests[t["name"]] = (cmd[0], wd)
            trace = os.path.join(wd, "trace.txt")
            if os.path.exists(trace):
                os.remove(trace)

    env = dict(os.environ, TAK_TRACE_OUT="trace.txt", SDL_VIDEODRIVER="dummy")
    subprocess.run(["ctest", "--test-dir", a.build, "-j", str(a.j),
                    "--timeout", "3600"], env=env)

    cache, by_test, by_case = {}, {}, {}
    for name, (binary, wd) in sorted(tests.items()):
        trace = os.path.join(wd, "trace.txt")
        if not os.path.exists(trace):
            print("no trace for", name)
            continue
        table = symbol_files(binary, cache)
        union = set()
        with open(trace) as f:
            for line in f:
                parts = line.split()
                if not parts:
                    continue
                got = files_for((int(x, 16) for x in parts[1:]), table)
                union |= got
                if os.path.basename(binary).startswith("test_ui_screens"):
                    by_case.setdefault(parts[0], set()).update(got)
        by_test[name] = union

    users = header_users(a.build)
    files = sorted(set().union(*by_test.values(), *by_case.values(),
                               *users.values(), users.keys()))
    index = {f: i for i, f in enumerate(files)}

    def ids(s):
        return sorted(index[f] for f in s)

    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    doc = {
        "commit": commit,
        "files": files,
        "tests": {t: ids(s) for t, s in sorted(by_test.items())},
        "ui_cases": {c: ids(s) for c, s in sorted(by_case.items())},
        "headers": {h: ids(s) for h, s in sorted(users.items())},
    }
    with open(OUT, "w", newline="\n") as f:
        f.write("{\n")
        f.write('"commit": %s,\n' % json.dumps(commit))
        f.write('"files": %s,\n' % json.dumps(files, indent=0))
        for key in ("tests", "ui_cases", "headers"):
            f.write('"%s": {\n' % key)
            items = list(doc[key].items())
            for i, (k, v) in enumerate(items):
                f.write("%s: %s%s\n" % (json.dumps(k), json.dumps(v),
                                        "," if i + 1 < len(items) else ""))
            f.write("}%s\n" % ("," if key != "headers" else ""))
        f.write("}\n")
    print("%d files, %d tests, %d screen cases -> %s"
          % (len(files), len(by_test), len(by_case), OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
