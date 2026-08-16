#!/usr/bin/env bash
# Emerald test runner: golden tests per compiler stage.
#
#   tests/run_tests.sh [lexer|parser|check|json|e2e]...   (default: all stages)
#
# Each stage compares tool output against a .expected file:
#   lexer/   emeraldc --emit-tokens X.rald  vs X.expected
#   parser/  emeraldc --emit-ast X.rald     vs X.expected
#   check/   emeraldc --check X.rald        vs X.expected (stdout+stderr)
#   json/    emeraldc --check --json X.rald vs X.json.expected (machine-readable)
#   e2e/     compile X.rald, run the binary vs X.expected (stdout)
set -u
cd "$(dirname "$0")/.."   # repo root, so diagnostics have stable paths

EMERALDC=bin/emeraldc
PASS=0 FAIL=0

report() { # name, expected_file, actual_output
    if diff -u "$2" - <<<"$3" >/tmp/emerald_diff.$$ 2>&1; then
        PASS=$((PASS + 1))
        echo "  PASS $1"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL $1"
        sed 's/^/    /' /tmp/emerald_diff.$$
    fi
    rm -f /tmp/emerald_diff.$$
}

run_lexer() {
    echo "== lexer"
    for f in tests/lexer/*.rald; do
        report "$f" "${f%.rald}.expected" "$("$EMERALDC" --emit-tokens "$f" 2>&1)"
    done
}

run_parser() {
    echo "== parser"
    for f in tests/parser/*.rald; do
        report "$f" "${f%.rald}.expected" "$("$EMERALDC" --emit-ast "$f" 2>&1)"
    done
}

run_check() {
    echo "== check"
    for f in tests/check/*.rald; do
        report "$f" "${f%.rald}.expected" "$("$EMERALDC" --check "$f" 2>&1)"
    done
}

run_json() {
    echo "== json diagnostics"
    for f in tests/check/*.rald; do
        report "$f" "${f%.rald}.json.expected" "$("$EMERALDC" --check --json "$f" 2>&1)"
    done
}

run_e2e() {
    echo "== e2e"
    for f in tests/e2e/*.rald; do
        bin="${f%.rald}"
        if ! "$EMERALDC" -o "$bin" "$f" 2>/tmp/emerald_cc.$$; then
            FAIL=$((FAIL + 1))
            echo "  FAIL $f (compilation)"
            sed 's/^/    /' /tmp/emerald_cc.$$
            rm -f /tmp/emerald_cc.$$
            continue
        fi
        rm -f /tmp/emerald_cc.$$
        report "$f" "${f%.rald}.expected" "$("$bin" 2>&1)"
        rm -f "$bin"
    done
}

stages="${*:-lexer parser check json e2e}"
for s in $stages; do
    case "$s" in
        lexer) run_lexer ;;
        parser) run_parser ;;
        check) run_check ;;
        json) run_json ;;
        e2e) run_e2e ;;
        *) echo "unknown stage: $s" >&2; exit 2 ;;
    esac
done

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
