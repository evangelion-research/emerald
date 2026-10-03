#!/usr/bin/env bash
# check.sh — M0 checking driver for the Oberwolfach work.
#
# Runs, in order:
#   1. positive: the trusted checker and every certificate type-check under
#      `--check --proof`;
#   2. the `--proof-report` line for the checker and every certificate;
#   3. every certificate built and run, which is the runtime referee;
#   4. negative mutations — a verifier that has never rejected anything has
#      verified nothing (PLAN.md §7).
#
# Every certificate is a self-contained Emerald source file; generated ones
# live in certs/ and were produced by op_construct.rald (T0, Walecki).
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
dc="$root/bin/emeraldc"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if [ ! -x "$dc" ]; then
    echo "== building the compiler =="
    (cd "$root" && task)
fi

# All certificates import op_verify from the project directory.
inc="-I $here"

pass=0
fail=0

expect_ok() {  # desc cmd...
    desc="$1"; shift
    if out=$("$@" 2>&1); then
        echo "PASS  $desc"
        pass=$((pass + 1))
    else
        echo "FAIL  $desc — expected success"
        echo "$out" | head -5 | sed 's/^/        /'
        fail=$((fail + 1))
    fi
}

expect_err() {  # desc cmd...
    desc="$1"; shift
    if out=$("$@" 2>&1); then
        echo "FAIL  $desc — expected rejection, but it typechecked"
        fail=$((fail + 1))
    else
        echo "PASS  $desc"
        echo "$out" | head -2 | sed 's/^/        /'
        pass=$((pass + 1))
    fi
}

# Compile the mutant (must succeed), run it, and require it to report FAIL.
expect_refutes() {  # desc mutant.rald
    desc="$1"; src="$2"
    bin="$tmp/$(basename "$src" .rald)"
    if ! "$dc" $inc "$src" -o "$bin" >"$tmp/build.log" 2>&1; then
        echo "FAIL  $desc — mutant did not even compile"
        head -3 "$tmp/build.log" | sed 's/^/        /'
        fail=$((fail + 1))
        return
    fi
    out=$("$bin")
    if printf '%s' "$out" | grep -q '^FAIL'; then
        echo "PASS  $desc"
        echo "        $out"
        pass=$((pass + 1))
    else
        echo "FAIL  $desc — verifier accepted a broken certificate"
        echo "        $out"
        fail=$((fail + 1))
    fi
}

echo "===== 1. proof mode: trusted checker and certificates ====="
expect_ok "op_verify.rald checks under --proof" \
    "$dc" --check --proof "$here/op_verify.rald"

for f in "$here"/certs/*.rald; do
    expect_ok "$(basename "$f") checks under --proof" \
        "$dc" --check --proof $inc "$f"
done

echo
echo "===== 2. proof report for the checker and every certificate ====="
echo "-- op_verify.rald"
"$dc" --check --proof --proof-report "$here/op_verify.rald" | sed 's/^/   /'
for f in "$here"/certs/*.rald; do
    echo "-- $(basename "$f")"
    "$dc" --check --proof --proof-report $inc "$f" | sed 's/^/   /'
done

echo
echo "===== 3. run every certificate (the runtime referee) ====="
for f in "$here"/certs/*.rald; do
    b="$(basename "$f" .rald)"
    out="$tmp/$b"
    if "$dc" $inc "$f" -o "$out" >"$tmp/build.log" 2>&1 && "$out"; then
        pass=$((pass + 1))
    else
        echo "FAIL  $b did not build and run"
        head -3 "$tmp/build.log" | sed 's/^/        /'
        fail=$((fail + 1))
    fi
done

echo
echo "===== 4. negative checks: the certificates must reopen ====="

# --- compile-time rejections (proof obligations) ---

# drop a vertex case from succ_1: the `never` obligation reopens
grep -v 'if v == 4 { return 8 }' "$here/certs/op_9_333.rald" > "$tmp/n1_drop_vertex.rald"
expect_err "drop a vertex case -> exhaustiveness obligation reopens" \
    "$dc" --check --proof $inc "$tmp/n1_drop_vertex.rald"

# drop a factor case from factor_at: the factor-index `never` obligation reopens
grep -v 'if j == 3 { return succ_3 }' "$here/certs/op_9_333.rald" > "$tmp/n2_drop_factor.rald"
expect_err "drop a factor index case -> exhaustiveness obligation reopens" \
    "$dc" --check --proof $inc "$tmp/n2_drop_factor.rald"

# a successor may not return a value outside the vertex type
sed 's/if v == 0 { return 3 }/if v == 0 { return 9 }/' \
    "$here/certs/op_9_333.rald" > "$tmp/n3_bad_vertex.rald"
expect_err "return a literal outside V -> codomain rejected at compile time" \
    "$dc" --check --proof $inc "$tmp/n3_bad_vertex.rald"

# --- runtime rejections (the referee fires) ---

# split a triangle into a 2-cycle and a fixed point
sed -e 's/if v == 3 { return 6 }/if v == 3 { return 0 }/' \
    -e 's/if v == 6 { return 0 }/if v == 6 { return 6 }/' \
    "$here/certs/op_9_333.rald" > "$tmp/n4_two_cycle.rald"
expect_refutes "introduce a 2-cycle and a fixed point -> rejected" \
    "$tmp/n4_two_cycle.rald"

# merge two triangles into a 6-cycle: still a permutation, wrong cycle type
sed -e 's/if v == 6 { return 0 }/if v == 6 { return 1 }/' \
    -e 's/if v == 7 { return 1 }/if v == 7 { return 0 }/' \
    "$here/certs/op_9_333.rald" > "$tmp/n5_wrong_type.rald"
expect_refutes "wrong cycle type (6-cycle instead of triangles) -> rejected" \
    "$tmp/n5_wrong_type.rald"

# duplicate a factor: every edge of one triangle is then covered twice
sed -e 's/if v == 0 { return 4 }/if v == 0 { return 3 }/' \
    -e 's/if v == 1 { return 5 }/if v == 1 { return 4 }/' \
    -e 's/if v == 2 { return 3 }/if v == 2 { return 5 }/' \
    -e 's/if v == 3 { return 7 }/if v == 3 { return 6 }/' \
    -e 's/if v == 4 { return 8 }/if v == 4 { return 7 }/' \
    -e 's/if v == 5 { return 6 }/if v == 5 { return 8 }/' \
    -e 's/if v == 6 { return 1 }/if v == 6 { return 0 }/' \
    -e 's/if v == 7 { return 2 }/if v == 7 { return 1 }/' \
    -e 's/if v == 8 { return 0 }/if v == 8 { return 2 }/' \
    "$here/certs/op_9_333.rald" > "$tmp/n6_duplicate.rald"
expect_refutes "duplicate a factor -> edge multiplicity > 1 rejected" \
    "$tmp/n6_duplicate.rald"

# break the even-order removed matching: 0's partner is no longer reciprocal
sed 's/mate_idx: seq\[int\] = \[8, 7, 6, 5, 9, 3, 2, 1, 0, 4\]/mate_idx: seq[int] = [9, 7, 6, 5, 9, 3, 2, 1, 0, 4]/' \
    "$here/certs/op_10_Walecki.rald" > "$tmp/n7_bad_matching.rald"
expect_refutes "broken perfect matching (n = 10) -> rejected" \
    "$tmp/n7_bad_matching.rald"

echo
echo "===== summary: $pass passed, $fail failed ====="
[ "$fail" -eq 0 ]
