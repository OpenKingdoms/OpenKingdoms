#!/usr/bin/env bash
# The gate before a merge, from one build tree, in parallel.
#
#   bash scripts/run-suite.sh <build tree> [config] [jobs]
#
# The first pass points TAK_TEST_DATA_DIR at an empty folder, so every
# lookup falls through to the original archives the way the game and the
# browser read them, and runs every test but the on-demand ones. The
# second pass reads the data folder the tree was configured with and runs
# only the tests labelled layout, whose file lookups differ between the
# two. Run it under the test lock.
set -uo pipefail
BUILD="${1:?usage: run-suite.sh <build tree> [config] [jobs]}"
CONFIG="${2:-Release}"
JOBS="${3:-6}"
mkdir -p "$BUILD/empty-data"
fail=0
start=$(date +%s)
echo "== layout: archives only, every test"
TAK_TEST_DATA_DIR="$BUILD/empty-data" \
    ctest --test-dir "$BUILD" -C "$CONFIG" -j "$JOBS" --output-on-failure \
        -LE "on-demand|layout-only" || fail=1
echo "== layout: extracted data, the layout tests"
ctest --test-dir "$BUILD" -C "$CONFIG" -j "$JOBS" --output-on-failure \
    -L layout || fail=1
echo "both layouts in $(( $(date +%s) - start )) s"
if [ $fail -ne 0 ]; then echo "SUITE FAILED"; exit 1; fi
echo "SUITE CLEAN on both layouts"
