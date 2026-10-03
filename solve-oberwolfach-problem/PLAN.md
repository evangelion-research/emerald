# A plan for attacking the Oberwolfach problem in Emerald

**Constraint.** Everything below is to be built with *only* this repository's
software: `bin/emeraldc`, the Emerald language, and `task`/`sh` as a driver. No
SAT solver, no ILP solver, no Sage/GAP/nauty, no C helper, no Python. That
constraint is the point of the exercise — it turns a combinatorics project into
a load test of Emerald's proof mode — and it is also the main cost, quantified
in §4 and §9.

**Status of this document.** This is a plan, not a result. Every feasibility
claim in §4 was measured against `bin/emeraldc` at `8fcddd5` and the probe is
quoted. Every mathematical claim in §2 is from the literature and is marked
with the confidence I hold it at; §11.1 is the task that pins it properly.

---

## 1. The problem

Let `F` be a 2-regular graph on `n` vertices — necessarily a disjoint union of
cycles, `F = C_{m_1} ∪ … ∪ C_{m_t}` with every `m_i ≥ 3` and `Σ m_i = n`. Write
the instance `OP(n; m_1,…,m_t)`.

| `n` | Decompose | Into | Each copy |
|---|---|---|---|
| odd | `K_n` | `(n−1)/2` 2-factors | `≅ F` |
| even | `K_n − I` (`I` a perfect matching) | `(n−2)/2` 2-factors | `≅ F` |

Edge count is the trivial necessary condition and it balances: `n(n−1)/2 =
(n−1)/2 · n` for odd `n`, and `n(n−2)/2 = (n−2)/2 · n` for even `n`.

Seating form: `n` mathematicians, round tables of sizes `m_1,…,m_t`, one meal
per round, everyone adjacent to everyone else exactly once.

The number of admissible `F` per order is the number of partitions of `n` into
parts `≥ 3`: 2 at `n=6`, 4 at `n=9`, 9 at `n=12`, 25 at `n=17`, 1775 at `n=40`
— **120 instances for `3 ≤ n ≤ 17`, 11202 for `18 ≤ n ≤ 40`**. This governs how
`M3` is scoped.

## 2. What is already known (so we know what "attempting" can mean)

High confidence:

- Posed by Ringel, 1967.
- **Four known non-solvable instances, believed to be the only ones:**
  `OP(6; 3,3)`, `OP(9; 4,5)`, `OP(11; 3,3,5)`, `OP(12; 3,3,3,3)`.
- **Resolved for all large `n`** — Glock, Joos, Kim, Kühn, Osthus,
  *Resolution of the Oberwolfach problem* (arXiv:1806.04644; JEMS 2021). The
  same paper resolves Hamilton–Waterloo for large `n`.
- `n ≤ 17` was settled before 2010; **`18 ≤ n ≤ 40` was settled
  computationally** by Deza, Franek, Hua, Meszka, Rosa, JCMCC 74 (2010) 95–102,
  using difference methods on a cluster.
- Uniform case `OP(n; m^k)` (Alspach–Schellenberg–Stinson–Wagner 1989 and
  successors) and the two-table case are solved.

Medium confidence, to be pinned in `M0`:

- `OP(n; 3^{n/3})` is exactly the Kirkman triple system question: `KTS(v)`
  exists for all `v ≡ 3 (mod 6)`; `NKTS(v)` exists for all `v ≡ 0 (mod 6)`
  **except `v = 6` and `v = 12`** — which is exactly the two all-triangle
  entries in the exception list. If that is right, our `n=6` and `n=12`
  non-existence runs are reproducing a known design-theoretic result and can be
  cross-checked against it.

**Consequence for this project — stated up front.** There is no realistic path
from this repository to a new theorem about OP. The honest targets are:

1. **An independently reproducible, machine-checked verification artifact**: a
   certificate checker whose *shape* is certified by `emeraldc --check --proof`,
   plus a library of verified solution certificates.
2. **A self-contained recomputation of the four exceptions**, as exhaustive
   searches whose completeness argument is made as local and as checkable as
   Emerald allows (§8).
3. **Concrete findings about Emerald's proof mode** from doing (1) and (2).
   Two already exist, before any Oberwolfach code is written (§10).
4. Stretch, probably out of reach: extend the certificate-verified frontier past
   `n = 40` for a selected cycle-type family (§7, `T4`).

Anything described as "solving the Oberwolfach problem" means (1)+(2)+(4).

## 3. What `--proof` certifies

Unchanged from `verify-tests-examples/README.md` §0, and it is the frame for
everything below:

| Certified by `--check --proof` | How |
|---|---|
| Exhaustiveness / coverage | `impossible: never = x` typechecks only once every alternative of a finite domain is eliminated |
| Termination | structural descent through a recursive alias, or bounded `for i in range(n)`, or a monotone-counter `while` |
| Purity | a `pure` function does no I/O, no randomness, calls nothing impure — see the caveat in §10.1 |
| No `any` | `any` is a compile error wherever it surfaces |
| Codomain | a function's declared return type constrains which literals its body may return |

Not certified: that a *computed* value equals a claimed one (a computed `int`
cannot be narrowed into a literal union), any equational fact, and the *values*
in a hand-written table. So every claim this project makes has two halves — a
machine-checked shape and a runtime-checked content — and §9 keeps them apart.

## 4. Measured constraints (probes run at `8fcddd5`)

These five findings determine the architecture. Each was run; the probe is in
the right-hand column.

| # | Finding | Probe |
|---|---|---|
| 4.1 | **Bounded backtracking is expressible as a *total* function.** Recursion on a Peano budget `type Nat = None \| { pred: Nat }`, descending on `budget.pred`, passes `--proof` and reports `pure`. This is the load-bearing feasibility fact: the search engine itself can live inside proof mode. | `count_words(nat_of(4), 3) == 81`; report `2 total, 0 partial, 2 pure` |
| 4.2 | **Mutable scratch state can be threaded through that recursion** and still typechecks as `pure` — a caller-allocated `list[int]` mutated by the callee (`used[v] = 1 … used[v] = 0`). Permutation enumeration works verbatim. | `perms(nat_of(10), scratch, 10) == 3628800` under `--proof` |
| 4.3 | **No stdlib module can be imported into a proof-mode module.** `import io` / `strings` / `lists` / `sort` / `math` all fail with `E_TYPE_TERMINATION` (a `while` the checker cannot bound); `fmt` fails with `E_PROOF_ANY`. The trusted checker must therefore be **zero-import**, builtins only. | a file containing only `import lists` + `print("x")`, under `--check --proof` → rejected |
| 4.4 | **There is no `split` builtin** (it lives in `strings.rald`, which 4.3 bans), and `json_parse` returns `any`, which `--proof` bans. Parsing a text certificate inside a proof-mode module means a hand-rolled character parser. | `E_TYPE_UNDEFINED: call to undefined function 'split'` |
| 4.5 | **Throughput**: a tight integer loop runs at `1.6 × 10⁸` ops/s (10⁸ iterations of `acc = acc ^ (i & j)` in 0.64 s — essentially C). A recursive backtracking walk runs at `≈ 4.2 × 10⁶` nodes/s (the full `11!` permutation tree, `≈1.1 × 10⁸` nodes, in 25.6 s). **Recursion costs ~240 ns/node**; loops cost ~6 ns/op. | `time ./p_tight`, `time ./p_rec` |

Budget implied by 4.5, at one core:

| Search size | Wall clock |
|---|---|
| 10⁷ nodes | 2.4 s |
| 10⁹ nodes | 4 min |
| 10¹⁰ nodes | 40 min |
| 10¹² nodes | 66 h — out of scope |

So: **anything needing more than ~10¹⁰ search nodes must be killed by symmetry
or by a better encoding, not by patience.** Where a hot inner step can be
rewritten as a bounded `for` over bitmasks instead of a recursive call, it gets
~40× faster; that is the main optimisation lever.

Additional known sharp edges, from `verify-tests-examples/README.md` §4 and the
docs: `and`/`or`/`==` yield base `bool`, not `True | False`; mutable locals
widen to their base type; an empty `seq` needs an annotated binding; `--proof`
narrowing of a record union needs a *single* literal discriminant; `dict()` keys
are strings only (so index tables must be flat `list[int]`, or use a canonical
*string* code as the key — which is how isomorph rejection will memoise);
`int` is wrapping 64-bit, which is fine for a ≤64-vertex bitmask and is noted
as an assumption in §9.

## 5. Architecture: a two-tier trust split

4.3 and 4.4 force the shape, and it is the right shape anyway:

```text
  tier 1 — UNTRUSTED SOLVER (plain Emerald: imports, while, partial allowed)
      op_search.rald, op_construct.rald, op_rotational.rald
                    |
                    |  emits a certificate AS EMERALD SOURCE
                    v
  certs/op_12_3333.rald      type V = 0 | 1 | ... | 11
                             def succ_0(v: V) -> V pure { ... }   # one per factor
                    |
                    |  compiled together with the checker
                    v
  tier 2 — TRUSTED CHECKER (zero imports, --proof clean, pure + total)
      op_verify.rald  ->  emeraldc --check --proof --proof-report
                      ->  ./op_verify   (runtime referee over the finite domain)
```

**Why certificates are generated Emerald source rather than data files.** 4.4
makes text parsing inside proof mode expensive; but more importantly, a
certificate written as source *goes through the type checker*. At a fixed `n`
the vertex set is a literal union `type V = 0 | … | n−1`, so each factor's
successor function carries a `never` obligation — the checker then
machine-verifies that **every vertex is covered by every factor**, which is
precisely the part of certificate validity that a runtime loop would only
sample. A data-file certificate gets termination and typing; a source
certificate also gets coverage. The solver therefore emits `.rald`, and
`certs/` is checked in.

Provenance: the solver writes `sha256` of the certificate text and its own
parameters into a header comment, so a certificate is traceable to the run that
produced it without any external tooling.

## 6. Encodings

**A 2-factor is a permutation.** Represent factor `j` by its successor map
`succ_j : V -> V`. A permutation of `V` whose every cycle has length `≥ 3` is
exactly a 2-factor, up to the choice of orientation. So the certificate for
`OP(n; m_1,…,m_t)` is `(n−1)/2` (or `(n−2)/2`) successor functions, plus the
removed matching `I` when `n` is even.

**Edges as bitmasks.** `int` is 64-bit, so for `n ≤ 64` a vertex set is one
`int` and the whole edge set of `K_n` is `n` masks (`nbr[v]`). Edge-disjointness
is `(used[v] & bit(w)) == 0`; covering is `used[v] == full ^ bit(v)`. This keeps
the solver's inner loop in the 6 ns/op regime of 4.5 rather than the 240 ns/node
regime. `n ≤ 40` fits comfortably; there is no `popcount` builtin, so a 16-bit
nibble/byte table in a `seq[int]` is needed.

**Cycle type.** A canonical sorted multiset, as a `seq[int]`. The *claimed* type
is additionally expressed as a codomain where possible — `def cycle_len(v: V) ->
3 | 4 | 5` for `OP(12; 3,4,5)` — so a factor whose cycle lengths are wrong
cannot even be written down (the `degree(v) -> 0 | 2` idiom from
`graph_certificates.rald`, applied to cycle length).

**Depth as a Peano budget.** Per 4.1, search depth is a `Nat`. For an exact
cover over `E = n(n−1)/2` edges, the budget is the number of branch decisions,
bounded by the number of factors times the number of cycles — small, so the
`Nat` chain costs nothing.

## 7. The verifier (`op_verify.rald`) — obligation by obligation

This is `M0` and it is the whole foundation: it is the oracle that makes the
solver allowed to be heuristic, wrong, and untrusted.

| Obligation | How it is discharged |
|---|---|
| each `succ_j` is a permutation | runtime: every image hit exactly once, over all `v` |
| every vertex is handled by `succ_j` | **machine-checked**: `impossible: never = v` in the generated certificate |
| no fixed point, no 2-cycle (cycles `≥ 3`) | runtime: `succ(v) ≠ v`, `succ(succ(v)) ≠ v` |
| cycle type of `succ_j` equals the prescribed multiset | runtime, via a bounded `for round in range(n)` orbit walk (total by construction, per 4.1) |
| the factors are pairwise edge-disjoint and together cover every edge | runtime: one `int` per vertex, accumulate `{v, succ_j(v)}`, assert every off-diagonal pair has multiplicity exactly 1 (`n` odd) or exactly 1 off `I` and 0 on `I` (`n` even) |
| `I` is a perfect matching (`n` even) | runtime: involution, no fixed point |
| the factor count is right | machine-checked by the codomain of the factor index: `def factor(j: 0\|1\|2\|3\|4) -> …` with a `never` obligation |
| the whole checker terminates and is pure | **machine-checked** by `--proof` |

Every check is a bounded loop over a finite domain, so `--proof` accepts the
lot, and the `--proof-report` line (`k total, 0 partial, k pure`) goes in the
README next to each result, exactly as `verify-tests-examples` does.

**Negative tests are part of `M0`, not an afterthought.** `check.sh` must show
the verifier *rejecting*: a dropped edge, a swapped pair (breaking
disjointness), a 2-cycle, a wrong cycle type, a dropped vertex case (which must
fail at *compile* time, via the `never` obligation), and a duplicated factor. A
verifier that has never rejected anything has verified nothing.

## 8. The solver, in tiers

| Tier | Method | Target | Expected cost |
|---|---|---|---|
| `T0` | **Walecki** zig-zag construction: `K_n` (`n` odd) → `(n−1)/2` Hamiltonian cycles; `K_n − I` (`n` even) likewise | `OP(n; C_n)` for every `n` — the first test vectors for the verifier | closed form, microseconds |
| `T1` | **Difference classes on `Z_n`**: `D_d = {{x, x+d}}` is a 2-factor of type `(n/g)^g`, `g = gcd(d,n)`. All `(n−1)/2` classes are isomorphic only when `n` is prime, so this alone solves `OP(p; C_p)` — useful as a second, independent generator for cross-checking `T0` | cross-check | closed form |
| `T2` | **Group-invariant starter search** — the workhorse for `n ≤ 40`. Fix a group `G` acting on the vertices (`Z_n`; `Z_{n−1}` plus a fixed point; `Z_m × Z_2`), search for orbit representatives whose translates tile the edge set exactly. The search is over one or two factors' worth of structure, so the space collapses from astronomical to small. Pruned by the edge-difference multiset. **Derive the orbit arithmetic from Deza et al. rather than guessing it** — a wrong guess costs search success, never correctness, because the verifier is the oracle | `OP` for a chosen band of `n ≤ 40` | minutes per instance, if the device is right |
| `T3` | **Full exact-cover backtracking** with "branch on the lexicographically-least uncovered edge" plus isomorph rejection at the root | `n ≤ 13`; the four exceptions | §9 |
| `T4` | Stretch: push `T2` past `n = 40` for one cycle-type family, or attack a Hamilton–Waterloo instance | a possible contribution | unknown; kill after a fixed time box |

`T3`'s branching rule matters more than anything else: always extending the
*lowest uncovered edge* makes every node's child set a small, locally
enumerable, provably complete list — which is what §9 needs.

## 9. The four exceptions: making exhaustiveness as checkable as possible

This is the part worth thinking hardest about, because **Emerald cannot certify
that a search was exhaustive.** `--proof` gives termination, purity, and
coverage of *declared finite domains*; "these are all the cases" about a search
tree is not a proposition it can state.

What *can* be done is to make completeness **local**. With the
lowest-uncovered-edge branching rule, at every node the set of legal children is
"all factors-in-progress that cover edge `e`", and that set is generated by a
bounded `for` loop over the whole candidate domain, with the only pruning being
*"this edge is already used"* — which is sound by definition, not by argument.
Then:

- the search is a `pure`, total, proof-mode function `solutions() -> int`;
- the claim is `solutions() == 0`;
- completeness reduces to two local properties: **the candidate generator is
  complete**, and **every prune is a tautology**.

The first is cross-checked by counting the candidate domain two ways — by
enumeration and by closed formula — and asserting equality at runtime. That is a
genuine, cheap, strong self-check, and it belongs in `check.sh`.

The one step that stays a hand-written lemma is **isomorph rejection at the
root** (fixing the first factor, and `I` when `n` is even, up to relabelling).
It must be stated explicitly in the README as a trusted assumption, with the
argument written out, and guarded by running at least one case *without* it to
confirm the same answer.

Sizing, using 4.5:

| Instance | Space | Estimate |
|---|---|---|
| `OP(6; 3,3)` | 15 matchings × 10 candidate factors, depth 2 | ≤ 1500 nodes — instant, do it without isomorph rejection |
| `OP(9; 4,5)` | 4536 `(4,5)`-factors, depth 4, first factor fixed | ~10⁵–10⁷ nodes |
| `OP(11; 3,3,5)` | 55440 `(3,3,5)`-factors, depth 5, first factor fixed | ~10⁷–10⁹ nodes; the one to watch |
| `OP(12; 3,3,3,3)` | `I` fixed; 160 triangles avoiding `I`; partition 60 edges into 5 parallel classes of 4 triangles — i.e. `NKTS(12)` | ~10⁷–10⁸ nodes with edge-first branching |

All four are inside the budget. `n=12` is the historically computer-dependent
one and is the headline result of `M2`.

**Also search the complement.** For each of the four, run the identical engine
on a nearby *solvable* instance (`OP(9; 3,3,3)`, `OP(12; 3,4,5)`, `OP(13; 3,4,6)`)
and require it to *find* solutions. A search that returns 0 everywhere is a bug,
not a theorem; this is the single most important sanity control in the project.

## 10. Trust ledger (the table every result must carry)

| Layer | Status |
|---|---|
| certificate covers every vertex / every factor index | **machine-checked** (`never` obligations in generated source) |
| checker terminates, is pure, mentions no `any`, no `partial` | **machine-checked** (`--proof --proof-report`) |
| cycle types, disjointness, completeness, matching validity | **runtime-checked over the entire finite domain** — not sampled |
| search exhaustiveness | **argued**, via local completeness + two-way candidate counts + positive controls (§9) |
| isomorph rejection at the root | **trusted hand lemma**, with one unreduced confirmation run |
| `int` does not overflow (bitmasks, counters) | **trusted**: `int` is wrapping 64-bit and `--proof` does not model it (`docs/REMAINING_FEATURES.md` §2) |
| compiler and runtime are correct | **trusted**: no proof IR, no independently checkable certificate; `emeraldc` is the TCB |
| `pure` implies no observable mutation | **false as implemented** — see 10.1 |

### 10.1 Two findings about Emerald, already

Both came out of the §4 probes and both should be filed against the compiler
regardless of what happens to this project.

1. **`pure` admits observable mutation.** `docs/effects.md` says `pure` is the
   empty effect mask and "promises the function has no observable effect", and
   lists `Mut` among the labels. But both of these typecheck under
   `--check --proof`:

   ```rald
   g: list[int] = [0, 0, 0]
   def bump() -> int pure { g[0] = g[0] + 1  return g[0] }      # mutates a global

   def bump2(xs: list[int]) -> int pure { xs[0] = xs[0] + 1  return xs[0] }
   ```

   So `Mut` is either not inferred for indexed assignment or not required empty
   by `pure`. This project *relies* on the behaviour (4.2 is how the search
   engine threads scratch state), which makes the honest consequence sharp:
   **in this repository the `pure` label on a search function means "no I/O and
   no randomness", not "a function of its inputs"** — so it does not license
   any commuting-square or referential-transparency argument. The trust ledger
   says so, and the plan does not use purity for anything stronger.

2. **Proof mode and the standard library are disjoint** (4.3). Not a bug — it is
   documented that stdlib does not pass `--proof` — but the consequence is worth
   recording: *any* zero-dependency verified artifact in Emerald must re-implement
   its own parsing, sorting, and collection helpers from builtins. A small
   `proofkit.rald` (proof-clean `sorted_ints`, `split`, `contains`, `popcount`)
   is a reusable by-product of this project and arguably the most generally
   useful thing it will produce.

## 11. Milestones and exit criteria

### M0 — foundation (the only milestone that is certainly achievable)

1. **11.1 Pin the literature.** Read arXiv:1806.04644 and the Deza–Franek–Hua–Meszka–Rosa
   paper; write `NOTES.md` with the exception list, the `KTS`/`NKTS` correspondence,
   and the difference-method device, each with a citation. Correct §2 of this file
   against it. *Do this first: it is the cheapest way to avoid building the wrong search.*
2. `op_types.rald`, `op_verify.rald` (zero-import, `--proof` clean), `proofkit.rald`.
3. `T0` Walecki + `T1` difference classes as certificate generators.
4. `check.sh` in the style of `verify-tests-examples/check.sh`: positive checks,
   `--proof-report` for every module, run every witness, and **≥ 6 negative
   mutations** (§7).

*Exit:* `check.sh` exits 0; the verifier accepts Walecki certificates for every
odd `n ≤ 21` and every even `n ≤ 20`, and rejects all six mutations, two of them
at compile time.

### M1 — small orders by exhaustive search

`T3` engine; all 120 admissible instances with `n ≤ 17`, each either a verified
certificate or an exhaustive-search non-existence claim. Positive controls per
§9.

*Exit:* exactly three non-existence results in range — `OP(6; 3,3)`,
`OP(9; 4,5)`, `OP(11; 3,3,5)` — and 117 verified certificates. **If a fourth
appears, the search is wrong; stop and debug.** This is a hard, pre-registered
falsification test of the engine.

### M2 — `OP(12; 3,3,3,3)`

The `NKTS(12)` run, with node count, candidate-count cross-check, and the
isomorph-rejection lemma written out; plus the unreduced confirmation run.

*Exit:* `solutions() == 0`, reproduced by two independently written candidate
generators (triangle-list and bitmask), with both node counts recorded.

### M3 — `18 ≤ n ≤ 40` by difference methods

`T2`. 11202 instances is not a weekend; scope by family — all uniform `m^k`
first, then two-table, then the rest as far as it goes. Every success is a
checked-in certificate; every failure is logged as *"not found by this device"*,
never as non-existence.

*Exit:* a table of `n` × cycle-type with verified/not-found per cell, and all
verified cells re-verified from a clean build.

### M4 — stretch

Time-boxed. `n > 40` for one family, or a Hamilton–Waterloo instance. Kill
criterion stated in advance: if `T2` has not produced a verified certificate
beyond `n = 40` within the box, write up the negative engineering result (§4.5
throughput vs. required search) and stop.

## 12. Layout and commands

```text
solve-oberwolfach-problem/
  PLAN.md          this file
  NOTES.md         literature, pinned (M0.1)
  README.md        the claim ledger: per result, machine-checked vs runtime-checked vs trusted
  proofkit.rald    proof-clean replacements for the stdlib bits we need
  op_types.rald    V, Nat, cycle-type encodings
  op_verify.rald   the trusted checker — zero imports, --proof clean
  op_construct.rald  T0 Walecki, T1 difference classes
  op_rotational.rald T2 group-invariant starter search
  op_search.rald     T3 exact-cover backtracking
  op_exceptions.rald the four non-existence runs + positive controls
  certs/op_<n>_<type>.rald   generated certificates (checked in)
  check.sh         positive + report + run + negative mutations
```

```sh
cd /Users/sagnikc/Fun/emerald
bin/emeraldc --check --proof --proof-report solve-oberwolfach-problem/op_verify.rald
bin/emeraldc solve-oberwolfach-problem/op_verify.rald -o /tmp/opv && /tmp/opv
solve-oberwolfach-problem/check.sh
```

## 13. Risks and decision points

| Risk | Signal | Response |
|---|---|---|
| `T3` recursion too slow at `n = 11` (4.5: 240 ns/node) | node rate below ~3M/s | move the inner extension loop to bitmask `for` loops (40× lever); only then consider an explicit stack with a monotone-counter `while` |
| `T2`'s group device guessed wrong | `T2` finds nothing at orders where solutions are known to exist | this is why `M0.1` is first; fall back to `T3` for small `n` and accept a smaller `M3` |
| proof mode blocks the search engine | `E_TYPE_TERMINATION` that 4.1's Peano budget cannot absorb | the two-tier split (§5) already allows it: demote the solver to plain Emerald, keep the checker `--proof` clean. **Never demote the checker.** |
| GC pressure in deep search | time per node growing with depth | pre-allocate all scratch at the root and thread it (4.2), allocate nothing per node |
| scope creep in `M3` | more than a week on 11202 instances | `M3` is explicitly family-scoped and partial completion is a valid outcome |
| the project proves nothing new | certain | that is the stated premise in §2; the deliverables are the artifact, the recomputation, and the compiler findings |

## 14. First three actions

1. `M0.1` — read the two papers, write `NOTES.md`, correct §2 here.
2. `op_verify.rald` + a hand-written `OP(9; 3,3,3)` certificate (the affine
   plane of order 3 — easy to write by hand and known to exist), and make
   `check.sh` reject six mutations of it.
3. `T0` Walecki, to feed the verifier certificates at every order up to 21
   before any search code is written.

## 15. References

- Glock, Joos, Kim, Kühn, Osthus. *Resolution of the Oberwolfach problem.*
  arXiv:1806.04644; J. Eur. Math. Soc. 23 (2021).
- Deza, Franek, Hua, Meszka, Rosa. *Solutions to the Oberwolfach problem for
  orders 18 to 40.* JCMCC 74 (2010) 95–102.
- Alspach, Schellenberg, Stinson, Wagner. *The Oberwolfach problem and factors
  of uniform odd length cycles.* JCTA 52 (1989).
- Bryant, Danziger. *On bipartite 2-factorisations and the Oberwolfach problem.*
- Ray-Chaudhuri, Wilson. *Solution of Kirkman's schoolgirl problem.* (1971)
- This repository: `docs/proofs.md`, `docs/effects.md`,
  `docs/REMAINING_FEATURES.md`, `verify-tests-examples/README.md`.
