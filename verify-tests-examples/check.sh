#!/usr/bin/env bash
# Checking steps for the machine-checked mathematics and graph encodings.
# Runs every positive check, then every negative (mutation) check, and reports.
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

echo "===== proof mode: positive checks ====="
for f in math_proofs graph_trees graph_certificates graph_algorithms; do
    expect_ok "$f.rald checks under --proof" "$dc" --check --proof "$here/$f.rald"
done

echo
echo "===== proof report for every module ====="
for f in math_proofs graph_trees graph_certificates graph_algorithms; do
    echo "-- $f.rald"
    "$dc" --check --proof --proof-report "$here/$f.rald" | sed 's/^/   /'
done

echo
echo "===== run the witnesses ====="
for f in math_proofs graph_trees graph_certificates graph_algorithms; do
    out="$tmp/$f"
    if "$dc" "$here/$f.rald" -o "$out" 2>/dev/null && "$out"; then
        pass=$((pass + 1))
    else
        echo "FAIL  $f did not build and run"
        fail=$((fail + 1))
    fi
    echo
done

echo "===== negative checks: the proofs must reopen ====="

# arithmetic: widen the residue domain by one element
sed 's/^type Z4 = 0 | 1 | 2 | 3$/type Z4 = 0 | 1 | 2 | 3 | 4/' \
    "$here/math_proofs.rald" > "$tmp/z4_mutant.rald"
expect_err "widen Z4 -> exhaustiveness obligation reopens" \
    "$dc" --check --proof "$tmp/z4_mutant.rald"

# trees: full traversal via a for loop over a seq field is not a structural descent
cat > "$tmp/tree_mutant.rald" <<'EOF'
type NTree = None | { v: int, kids: seq[NTree] }
def total_weight(t: NTree) -> int pure {
    if t == None { return 0 }
    total = t.v
    for k in t.kids { total = total + total_weight(k) }
    return total
}
EOF
expect_err "traverse all children via a for loop -> termination rejected" \
    "$dc" --check --proof "$tmp/tree_mutant.rald"

# certificates: a degree table may not return an odd (odd-degree) literal
sed 's/^    if v == 0 { return 2 }$/    if v == 0 { return 3 }/' \
    "$here/graph_certificates.rald" > "$tmp/deg_mutant.rald"
expect_err "degree returns 3 against codomain 0 | 2 -> rejected" \
    "$dc" --check --proof "$tmp/deg_mutant.rald"

# algorithms: dropping a table case reopens the exhaustiveness obligation
grep -v 'if t == 3 { return 2 }' "$here/graph_algorithms.rald" > "$tmp/tbl_mutant.rald"
expect_err "drop a vertex case from the certified table -> rejected" \
    "$dc" --check --proof "$tmp/tbl_mutant.rald"

# algorithms: a computed int cannot be narrowed into a literal union
cat > "$tmp/derived_mutant.rald" <<'EOF'
type V = 0 | 1 | 2 | 3 | 4
def bfs_from_zero() -> list[int] pure {
    d: list[int] = [99, 99, 99, 99, 99]
    d[0] = 0
    return d
}
def bfs_computed_bound(t: V) -> 0 | 1 | 2 pure {
    d = bfs_from_zero()
    return d[t]
}
EOF
expect_err "derive a bound from the computation -> rejected (int vs 0 | 1 | 2)" \
    "$dc" --check --proof "$tmp/derived_mutant.rald"

echo
echo "===== summary: $pass passed, $fail failed ====="
[ "$fail" -eq 0 ]
