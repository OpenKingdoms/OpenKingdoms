#!/usr/bin/env bash
# The full suite on both data layouts from one build tree, in parallel.
#
#   bash scripts/run-suite.sh <build tree> [config] [jobs]
#
# The first pass reads the data folder the tree was configured with. The
# second points TAK_TEST_DATA_DIR at an empty folder, so every lookup
# falls through to the original archives the way the browser reads them.
# Run it under the test lock; it is the gate before a merge.
set -uo pipefail
BUILD="${1:?usage: run-suite.sh <build tree> [config] [jobs]}"
CONFIG="${2:-Release}"
JOBS="${3:-6}"
mkdir -p "$BUILD/empty-data"
fail=0
start=$(date +%s)
echo "== layout: extracted data"
ctest --test-dir "$BUILD" -C "$CONFIG" -j "$JOBS" --output-on-failure || fail=1
echo "== layout: archives only"
TAK_TEST_DATA_DIR="$BUILD/empty-data" \
    ctest --test-dir "$BUILD" -C "$CONFIG" -j "$JOBS" --output-on-failure || fail=1
echo "both layouts in $(( $(date +%s) - start )) s"
if [ $fail -ne 0 ]; then echo "SUITE FAILED"; exit 1; fi
echo "SUITE CLEAN on both layouts"
