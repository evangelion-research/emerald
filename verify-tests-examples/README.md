# Machine-checked finite mathematics and graph encodings

Self-contained examples showing what Emerald's proof mode (`--proof`) does and
does not certify, applied to finite arithmetic and to graph theory. Everything
here is re-checked by `./check.sh`.

## 0. What `--proof` actually certifies

This matters more than any individual result, and it is narrower than "theorem
proving". Emerald's checker is a proof checker for a *structural* logic. It
certifies:

| Certified | How |
|---|---|
| **Exhaustiveness / coverage** | `impossible: never = x` typechecks only when every alternative of the finite domain has been eliminated |
| **Termination** | recursive calls must descend through the same inductive structure (`t.l`, `t.kids[0]`); everything else needs `partial`, which proof mode rejects |
| **Purity** | a `pure` function does no I/O, randomness, or impure calls |
| **No `any`** | a proof that mentions `any` proves nothing, so `any` is a compile error where it surfaces |
| **Output type** | a function's codomain constrains which literals its body may return |

It does **not** certify:

- that a *computed* value equals a claimed value (see §3.4 — a computed `int`
  cannot be narrowed into a literal union at all);
- any equational fact (`|V| = |E| + 1`, `mirror(mirror t) = t`, De Morgan);
- the *values* in a hand-written certificate table. `def degree(v) -> 0 | 2`
  stops the body from returning `3`; it cannot tell whether `2` is the true
  degree. Values are cross-checked at **runtime** over the finite domain.

So a claim here has two parts: a machine-checked *shape* (coverage, termination,
purity, output type) and a runtime-checked *content* (the values). Both are
reported separately below.

## 1. Finite arithmetic — `math_proofs.rald`

- Z/4Z closed under addition mod 4 and additive inverse; squares mod 4 lie in
  `{0, 1}`; De Morgan over the two-element domain; `A ∧ B ⇒ B ∧ A`.
- Machine-checked: all four residues are covered, every function is total and
  pure, no `any`. Report: `10 total, 0 partial, 10 pure, 0 vacuous, 0 taint`.
- Runtime-checked: De Morgan's equivalence over all four input pairs.
- The residue statement is verified **over the four classes of Z/4Z only**; the
  step from "all integers" is the division algorithm, which is assumed.

## 2. Graph theory

### 2.1 Trees by structural induction — `graph_trees.rald`

`type Tree = None | { v: int, l: Tree, r: Tree }`, with `size`, `depth`,
`leaves`, `mirror`, `eq_tree`. Each recurses on `t.l` / `t.r`, so termination is
machine-checked. A value of type `Tree` cannot contain a cycle — the recursive
alias *is* the acyclicity proof.

Report: `6 total, 0 partial, 6 pure`. Runtime-checked: `mirror` is an
involution on the concrete witness tree.

**n-ary limitation (the interesting finding).** Only descent through a `seq`
*element* (`t.kids[0]`) is recognised. A fold over all children
(`for k in t.kids { ... f(k) }`) and descent via a slice (`t.kids[1:]`) are both
rejected with `E_TYPE_TERMINATION`. Full n-ary traversal therefore needs the
binary encoding, or an explicit `partial` function — which proof mode rejects.

### 2.2 Certificates on C₅ — `graph_certificates.rald`

`V = 0 | 1 | 2 | 3 | 4`; the 5-cycle.

- `adj` is a relation on `V × V` whose five rows are proved covered.
- `degree : V -> 0 | 2` — the codomain says "every degree is even" (the
  Eulerian condition). A body returning `3` is a hard error.
- The edge set is a finite literal union, and `edge_id`'s `never` obligation
  machine-checks that the five edges are **exactly** the edge set.
- A matching as a finite union, with `matching_size` covering both edges.

Report: `14 total, 0 partial, 14 pure`. Runtime-checked over the whole domain:
symmetry, irreflexivity, `degree` matches the adjacency count, the 3-colouring
is proper, the matching is disjoint — and **`proper 2-colourings of C₅: 0`**,
i.e. non-bipartiteness, established by exhausting all 2⁵ colourings. Note that
this last one is a runtime referee: the checker cannot state "no 2-colouring
exists".

### 2.3 Verified algorithms — `graph_algorithms.rald`

- BFS distances from vertex 0 by bounded relaxation (`|V|` rounds), and a
  reachability fixpoint. Both are pure and terminate by construction.
- `bfs_bound : V -> 0 | 1 | 2` certifies `diam(C₅) = 2` as a covered table.
- Runtime referee: the computed distances equal the certified table, and all
  five vertices are reachable from 0.

Report: `6 total, 0 partial, 6 pure`.

## 3. Checking steps

```sh
cd /path/to/emerald
verify-tests-examples/check.sh          # everything: 13 checks, exit 0
```

Individual steps:

```sh
bin/emeraldc --check --proof --proof-report verify-tests-examples/graph_trees.rald
bin/emeraldc --check --proof --proof-report verify-tests-examples/graph_certificates.rald
bin/emeraldc --check --proof --proof-report verify-tests-examples/graph_algorithms.rald
bin/emeraldc verify-tests-examples/graph_certificates.rald -o /tmp/gc && /tmp/gc
```

### 3.4 The negative checks (why the claims are not vacuous)

Each of these is expected to be **rejected**, and is:

| Mutation | Error |
|---|---|
| widen `Z4` by one residue | `E_TYPE_ASSIGN: cannot assign 4 to 'impossible' declared as never` |
| traverse all children via a `for` loop | `E_TYPE_TERMINATION: ... does not decrease a recursive-alias argument` |
| `degree` returns `3` against codomain `0 \| 2` | `E_TYPE_RETURN: returning 3 from a function declared to return 0 \| 2` |
| drop a vertex case from the certified table | `E_TYPE_ASSIGN: cannot assign 3 to 'impossible' declared as never` |
| derive a bound from the computation (`return d[t]`) | `E_TYPE_RETURN: returning int from a function declared to return 0 \| 1 \| 2` |

The last row is the load-bearing one: it is *why* certificates are hand-written
tables rather than computed values, and why §0 separates machine-checked shape
from runtime-checked content.

## 4. Other things that cost time

- `and` / `or` / `==` produce the base type `bool`, which is **not** assignable
  to `Bool = True | False`. A `Bool`-returning function must return the literals
  by case. (`bool` is nameable and is the right return type for a function whose
  result is accumulated in a mutable local, since mutable locals widen.)
- `--proof` narrowing of a record union needs a **single literal discriminant**
  (`e.id`). Narrowing on two independent fields with `and` does not eliminate
  alternatives.
- An empty `seq` needs a named annotated binding (`empty: seq[Tree] = []`);
  a bare `[]` or `freeze([])` infers `list[any]` and is banned.
- Mutable locals widen to their base type, so `ok = True; ok = False; return ok`
  yields `bool`, not `Bool`.

## 5. Repository

- `https://github.com/evangelion-research/emerald`, revision
  `1210bbb793352383e63cd7bd1c0761a7349e875f` (`main`).
- This directory is **untracked**: the revision above does not yet contain it.
  Committing `verify-tests-examples/` is the remaining step for a
  self-contained repository reference.
- There is no interaction-net artifact in this repository; these are
  verification-instrument results.
