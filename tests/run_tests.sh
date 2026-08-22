#!/usr/bin/env bash
# Emerald test runner: golden tests per compiler stage.
#
#   tests/run_tests.sh [lexer|parser|check|json|e2e|proof|imports|stdlib|repl|shape|bench|warn|report|cli]...
#                                                                         (default: all stages)
#
# Each stage compares tool output against a .expected file:
#   lexer/   emeraldc --emit-tokens X.rald  vs X.expected
#   parser/  emeraldc --emit-ast X.rald     vs X.expected
#   check/   emeraldc --check X.rald        vs X.expected (stdout+stderr)
#   json/    emeraldc --check --json X.rald vs X.json.expected (machine-readable)
#   proof/   emeraldc --check --proof X.rald vs X.expected
#   e2e/     compile X.rald, run the binary vs X.expected (stdout)
#   imports/ one directory per case; each has a main.rald entry, optional
#            `flags` (extra emeraldc arguments, e.g. -I roots), and `expected`.
#            A case named bad_* is checked with --check and must fail; every
#            other case is compiled and run.
#   repl/    one .in per session, fed to `emeraldc --repl` on stdin; the
#            session's scratch path is rewritten to <repl> so the golden
#            output does not depend on the pid.
#   cli/     stable command-line help and version smoke tests.
#   stdlib/  one .rald per module, compiled and run against .expected. These
#            import from stdlib/ with no -I flag, so they also test that the
#            stdlib root resolves by default.
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

run_proof() {
    echo "== proof"
    for f in tests/proof/*.rald; do
        report "$f" "${f%.rald}.expected" "$("$EMERALDC" --check --proof "$f" 2>&1)"
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

run_imports() {
    echo "== imports"
    for dir in tests/imports/*/; do
        name="${dir%/}"
        entry="$name/main.rald"
        [ -f "$entry" ] || continue
        flags=""
        [ -f "$name/flags" ] && flags="$(cat "$name/flags")"

        case "$(basename "$name")" in
        bad_*)
            # a rejected program: the diagnostics themselves are the golden output
            report "$name" "$name/expected" \
                "$($EMERALDC --check $flags "$entry" 2>&1)"
            ;;
        *)
            bin="$name/main.bin"
            if ! $EMERALDC $flags -o "$bin" "$entry" 2>/tmp/emerald_imp.$$; then
                FAIL=$((FAIL + 1))
                echo "  FAIL $name (compilation)"
                sed 's/^/    /' /tmp/emerald_imp.$$
                rm -f /tmp/emerald_imp.$$
                continue
            fi
            rm -f /tmp/emerald_imp.$$
            report "$name" "$name/expected" "$("$bin" 2>&1)"
            rm -f "$bin"
            ;;
        esac
    done
}

run_shape() {
    echo "== shape"
    bin="${TMPDIR:-/tmp}/emerald_dim_unit.$$"
    if ! cc -std=c11 -Wall -Wextra -O2 -g -Iinclude -o "$bin" \
           tests/shape/dim_unit.c src/dim.c 2>/tmp/emerald_dim.$$; then
        FAIL=$((FAIL + 1))
        echo "  FAIL tests/shape/dim_unit.c (compilation)"
        sed 's/^/    /' /tmp/emerald_dim.$$
        rm -f /tmp/emerald_dim.$$
        return
    fi
    rm -f /tmp/emerald_dim.$$
    report "tests/shape/dim_unit.c" "tests/shape/dim_unit.expected" \
           "$("$bin" 2>&1)"
    rm -f "$bin"
    for f in tests/shape/*.rald; do
        report "$f" "${f%.rald}.expected" \
            "$("$EMERALDC" --emit-shapes "$f" 2>&1)"
    done
}

run_bench() {
    echo "== benchmark regressions"
    for f in tests/bench/*.rald; do
        name="${f%.rald}"
        bin="${name}.bin"
        if ! "$EMERALDC" -o "$bin" "$f" 2>/tmp/emerald_bench.$$; then
            FAIL=$((FAIL + 1))
            echo "  FAIL $f (compilation)"
            sed 's/^/    /' /tmp/emerald_bench.$$
            rm -f /tmp/emerald_bench.$$ "$bin"
            continue
        fi
        rm -f /tmp/emerald_bench.$$
        report "$f" "${f%.rald}.expected" "$("$bin" 2>&1)"
        rm -f "$bin"
    done
}

run_repl() {
    echo "== repl"
    for f in tests/repl/*.in; do
        out="$("$EMERALDC" --repl <"$f" 2>&1 |
               sed 's#/tmp/emerald-repl-[0-9]*#<repl>#g')"
        report "$f" "${f%.in}.expected" "$out"
    done
}

run_report() {
    echo "== proof report"
    report "tests/proof_report.rald (text)" "tests/proof_report.text.expected" \
        "$("$EMERALDC" --check --proof-report tests/proof_report.rald 2>&1)"
    report "tests/proof_report.rald (json)" "tests/proof_report.json.expected" \
        "$("$EMERALDC" --check --proof-report --json tests/proof_report.rald 2>&1)"
}

run_warn() {
    echo "== warnings"
    # W1: a warning still compiles (exit 0), --werror promotes it (nonzero),
    # and -Wno-<code> silences it (so --werror then passes again).
    if ./bin/emeraldc --check tests/check/warn_covariance.rald >/dev/null 2>&1; then
        PASS=$((PASS + 1)); echo "  PASS warn-compiles"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL warn-compiles (warning should not fail the build)"
    fi
    if ! ./bin/emeraldc --check --werror tests/check/warn_covariance.rald >/dev/null 2>&1; then
        PASS=$((PASS + 1)); echo "  PASS warn-werror"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL warn-werror (--werror should promote the warning)"
    fi
    if ./bin/emeraldc --check --werror -Wno-W_UNSOUND_COVARIANCE tests/check/warn_covariance.rald >/dev/null 2>&1; then
        PASS=$((PASS + 1)); echo "  PASS warn-suppress"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL warn-suppress (-Wno-W_UNSOUND_COVARIANCE should silence it)"
    fi
}

run_cli() {
    echo "== cli"
    report "emeraldc --help" tests/cli_help.expected \
        "$("$EMERALDC" --help 2>&1)"
    report "emeraldc --version" tests/cli_version.expected \
        "$("$EMERALDC" --version 2>&1)"
    if "$EMERALDC" --definitely-not-an-option >/dev/null 2>&1; then
        FAIL=$((FAIL + 1)); echo "  FAIL invalid option (should exit nonzero)"
    else
        PASS=$((PASS + 1)); echo "  PASS invalid option"
    fi
}

run_stdlib() {
    echo "== stdlib"
    for f in tests/stdlib/*.rald; do
        bin="${f%.rald}.bin"
        if ! "$EMERALDC" -o "$bin" "$f" 2>/tmp/emerald_std.$$; then
            FAIL=$((FAIL + 1))
            echo "  FAIL $f (compilation)"
            sed 's/^/    /' /tmp/emerald_std.$$
            rm -f /tmp/emerald_std.$$
            continue
        fi
        rm -f /tmp/emerald_std.$$
        # stdin comes from an optional X.stdin, and /dev/null otherwise, so a
        # test calling read_line() sees EOF rather than the terminal.
        stdin="${f%.rald}.stdin"
        [ -f "$stdin" ] || stdin=/dev/null
        report "$f" "${f%.rald}.expected" "$("$bin" <"$stdin" 2>&1)"
        rm -f "$bin"
    done
}

stages="${*:-lexer parser check json proof e2e imports stdlib repl shape bench warn report cli}"
for s in $stages; do
    case "$s" in
        lexer) run_lexer ;;
        parser) run_parser ;;
        check) run_check ;;
        json) run_json ;;
        proof) run_proof ;;
        e2e) run_e2e ;;
        imports) run_imports ;;
        stdlib) run_stdlib ;;
        repl) run_repl ;;
        bench) run_bench ;;
        cli) run_cli ;;
        shape) run_shape ;;
        warn) run_warn ;;
        report) run_report ;;
        *) echo "unknown stage: $s" >&2; exit 2 ;;
    esac
done

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
