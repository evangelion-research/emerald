# solve-oberwolfach-problem — the artifact ledger

Implementation of `PLAN.md`. This is the **M0 foundation** as it stands: the
trusted checker, the `T0` Walecki generator, a hand-written exemplar, and a
`check.sh` that shows the checker rejecting broken certificates. It is not a
new theorem and does not claim to be (`PLAN.md` §2).

## Run it

```sh
cd /Users/sagnikc/Fun/emerald
bin/emeraldc --check --proof --proof-report solve-oberwolfach-problem/op_verify.rald
bin/emeraldc --check --proof --proof-report -I solve-oberwolfach-problem solve-oberwolfach-problem/certs/op_21_Walecki.rald
bin/emeraldc -I solve-oberwolfach-problem solve-oberwolfach-problem/certs/op_21_Walecki.rald -o /tmp/o21 && /tmp/o21
solve-oberwolfach-problem/check.sh
```

`check.sh` exits `0` on success. As of this writing it reports
`48 passed, 0 failed`: 21 positive `--proof` checks (the checker plus 20
certificates), 20 built-and-run witnesses, 3 compile-time rejections, and 4
runtime rejections. The `--proof-report` lines for the same 21 modules are
printed separately and are not part of the pass count.

## Layout

```text
PLAN.md            the plan (unchanged spec)
NOTES.md           M0.1: literature, with confidence tags and open items
op_verify.rald     the trusted checker — zero imports, --proof clean
op_construct.rald  T0 Walecki generator (untrusted, plain Emerald)
certs/op_9_333.rald        hand-written OP(9;3,3,3) exemplar (reads, 13 total/0 partial/13 pure)
certs/op_<n>_Walecki.rald  generated OP(n; C_n), odd 3..21 and even 4..20
check.sh           positives + reports + runs + negative mutations
```

## The two-tier trust split (PLAN §5), as built

```text
op_construct.rald  (plain Emerald: imports, while, write_file)
      |  emits Emerald source
      v
certs/*.rald  ──import──>  op_verify.rald  (zero imports, pure, total)
      |                          |
      |                          +-- emeraldc --check --proof: shape
      v
  emeraldc ... -o bin && ./bin   -- runtime referee over the whole finite domain
```

Certificates are **Emerald source**, not data, so they get more than typing and
termination: because a certificate fixes the vertex type as a literal union
`type V = 0 | … | n−1`, each successor function carries an
`impossible: never = v` obligation, and the checker machine-verifies that
**every vertex is covered by every factor**. That is the part of certificate
validity a runtime loop could only sample.

## Claim ledger

Every result below is decomposed the way `PLAN.md` §10 requires. "Machine" means
`emeraldc --check --proof` discharged an obligation; "runtime" means a bounded
loop checked the values over the entire finite domain, not by sampling.

| Claim | Machine-checked | Runtime-checked | Trusted |
|---|---|---|---|
| `op_verify.rald` terminates, is `pure`, mentions no `any`/`partial` | `9 total, 0 partial, 9 pure` | — | compiler/runtime correct |
| each certificate: all `n` vertices handled by each factor | `never = v` discharges | — | — |
| each certificate: factor index exhaustive | `never = j` discharges | — | — |
| `OP(9;3,3,3)`: 4 triangle-factors cover `K₉` once | coverage, codomain, purity | permutation, cycles ≥ 3, cycle type `3³`, edge cover | hand-transcribed tables (`certs/op_9_333.rald`); the checker is the oracle |
| `OP(n; C_n)` for odd `n ≤ 21`, even `n ≤ 20`: Walecki decomposition | coverage, codomain, purity | permutation, cycles ≥ 3, cycle type `n¹`, edge cover, removed matching (even) | the Walecki formula in `op_construct.rald`; checked by the verifier before being trusted |
| `int` does not overflow (indices, counters) | — | — | `int` is wrapping 64-bit; values here are `≤ 21` |
| the compiler and runtime are correct | — | — | `emeraldc` is the TCB |
| `pure` implies "a function of its inputs" | — | — | **false as implemented** (`PLAN.md` §10.1); used only as "no I/O, no randomness" |

The `T0` family is closed-form and is *not* a search, so there is no
exhaustiveness claim to make or defend.

## Negative checks: the proof must reopen

`check.sh` requires every one of these to be rejected (3 at compile time, 4 at
runtime). The two `never`-obligation rejections are the load-bearing ones.

| Mutation | Rejected by |
|---|---|
| drop a vertex case from `succ_1` | compile: `E_TYPE_ASSIGN: cannot assign 4 to 'impossible' declared as never` |
| drop a factor case from `factor_at` | compile: `E_TYPE_ASSIGN: cannot assign 3 to 'impossible' declared as never` |
| return `9` (outside `V`) from a successor | compile: `E_TYPE_RETURN` |
| split a triangle into a 2-cycle + fixed point | runtime: `no_short_cycles` fails |
| merge two triangles into a 6-cycle (still a permutation) | runtime: `cycle_type_ok` fails |
| duplicate a factor (each edge covered twice) | runtime: `edges_exact` fails |
| break the even-order removed matching (`n = 10`) | runtime: `matching_ok` fails |

## Findings about Emerald (this project's by-products)

### F1 — a bare function-typed generic parameter does not instantiate

The checker was originally written to take the removed 1-factor as a function
value, `def edges_exact[V](…, removed: (V) -> V, …)` and call `removed(a)`
inside a `pure` generic body. It does not compile:

```text
error[E_TYPE_ARG]: argument 1 of removed(): expected 0 | 1 | … | 8, got V
```

At the call site `V` is inferred as the certificate's concrete union, but inside
the generic body the parameter's type is fixed to that concrete union while the
local `a` is still the generic `V`. A `seq[(V) -> V]` parameter does *not* have
this problem — iterating a sequence of function values and calling its elements
from a `pure` generic function works (that is how the factors are passed). The
workaround is to pass index data (`mate_idx: seq[int]`) instead. This is a
compiler issue worth filing, not a design choice.

### Confirmations of PLAN §4 probes

- **4.1** a Peano-budget total recursive function passes `--proof` and reports
  pure: reproduced (`count_words(nat_of(4), 3) == 81`).
- **4.2** mutable scratch threaded through a proof-mode recursion stays `pure`:
  reproduced (`perms` over `10! = 3628800`).
- **4.3** a stdlib `import` inside a proof-mode module is rejected: relied on —
  `op_verify.rald` is zero-import, builtins only.
- **4.5** throughput was not re-measured here; nothing in `M0` needed it.

## Status and deviations from `PLAN.md` §12

- Implemented: `op_verify.rald`, `op_construct.rald`, `certs/`, `check.sh`,
  `NOTES.md`, this file. `op_verify.rald` is a zero-import **library** that
  certificates import; the checked-and-runnable artifacts are the certificates
  in `certs/`, rather than `op_verify.rald` itself having a `main`. This keeps
  imports free of top-level side effects.
- Moved to the next milestone: `proofkit.rald`, `op_types.rald`,
  `op_search.rald`, `op_rotational.rald`, `op_exceptions.rald`. The checker did
  not need a Peano budget (every loop is bounded) or any stdlib helper, so
  creating those files now would be stubs.
- Open in `M0.1`: read the Deza et al. PDF and the `NKTS` source to remove the
  `[unpinned]` tags in `NOTES.md` §3–§5.
