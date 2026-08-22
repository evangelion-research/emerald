#!/usr/bin/env bash
# Benchmark the implemented Emerald workloads. Timings are informational; the
# deterministic output checks are the regression tests.
set -u
cd "$(dirname "$0")/.."

EMERALDC=bin/emeraldc
TMP_ROOT="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMP_ROOT/emerald-bench.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

check_case() {
    local source="$1"
    local name="${source##*/}"
    name="${name%.rald}"
    local binary="$WORKDIR/$name"
    local actual

    if ! "$EMERALDC" -o "$binary" "$source" >/dev/null 2>"$WORKDIR/$name.compile.err"; then
        cat "$WORKDIR/$name.compile.err" >&2
        echo "FAIL $source (compilation)" >&2
        return 1
    fi

    actual="$("$binary")"
    if ! diff -u "${source%.rald}.expected" - <<<"$actual"; then
        echo "FAIL $source (output regression)" >&2
        return 1
    fi
    echo "PASS $source"
}

run_timed_case() {
    local source="$1"
    local name="${source##*/}"
    name="${name%.rald}"
    local binary="$WORKDIR/$name"

    TIMEFORMAT="  compile: %3R s"
    time "$EMERALDC" -o "$binary" "$source" >/dev/null
    TIMEFORMAT="  run:     %3R s"
    time "$binary" >/dev/null
}

for source in tests/bench/*.rald; do
    check_case "$source" || exit 1
done

if [ "${1:-}" != "--check-only" ]; then
    echo
    echo "Benchmark timings (informational):"
    for source in tests/bench/*.rald; do
        echo "== $source"
        run_timed_case "$source"
    done
fi
