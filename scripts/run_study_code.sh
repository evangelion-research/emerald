#!/bin/sh
# Compile and run every study-guide example, comparing against its golden output.
# Usage: scripts/run_study_code.sh [--bless]
set -u
cd "$(dirname "$0")/.." || exit 1
BLESS=0
[ "${1:-}" = "--bless" ] && BLESS=1
pass=0; fail=0
for f in study_guide/code/*.rald; do
    name=$(basename "$f" .rald)
    case "$name" in _*) continue ;; esac      # _*.rald are imported helpers
    exp="study_guide/code/expected/$name.out"
    got=$(./bin/emeraldc -o "/tmp/study_$name" "$f" 2>&1 && "/tmp/study_$name" 2>&1)
    rm -f "/tmp/study_$name"
    if [ "$BLESS" = "1" ]; then
        printf '%s\n' "$got" > "$exp"
        echo "  BLESS $name"
        continue
    fi
    if [ -f "$exp" ] && [ "$got" = "$(cat "$exp")" ]; then
        echo "  PASS  $name"; pass=$((pass + 1))
    else
        echo "  FAIL  $name"
        printf '%s\n' "$got" | diff -u "$exp" - 2>/dev/null | head -20
        fail=$((fail + 1))
    fi
done
[ "$BLESS" = "1" ] && exit 0
echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
