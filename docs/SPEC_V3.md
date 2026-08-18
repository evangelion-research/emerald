# Emerald Phase 3 — Make It Honest

**Status:** planned. Phase 1 (scripting language, functional core, modules) and
Phase 2 (tensors, shapes, `Fin[n]`, the MLP demo) are implemented — 123 golden
tests across `tests/{lexer,parser,check,e2e,imports,proof,shape,stdlib}`. This
document plans the phase that makes the proof fragment mean what it says.

**Thesis obligation this phase serves:** every claim in the research program —
commuting squares (Track C), approximate judgments (Track D), exported
certificates (Track J) — is a claim about a *function*. Today Emerald cannot
state that a model is a function: purity is a promise about direct calls only,
divergence still inhabits `never` outside the structural fragment, `any` hides
inside type constructors, and `list[T]` is covariantly unsound. A certificate
emitted over that substrate would be worthless, and Phase 5's whole output is
certificates. This phase buys the trustworthy base and nothing else.

**Exit criterion** (from [`research-directions.md`](research-directions.md)
§13): *"`emeraldc --check --proof` passed" is a statement you'd defend* —
concretely: proof mode admits no `any` (including `any` reachable through a
type constructor), no non-terminating function (including `while` loops and
mutual recursion), no unsound covariance, and reports what it checked as a
number, not a claim.

---

## 1. What the substrate already gives us

Facts established by reading the current tree, because the plan below leans on
each of them:

| Fact | Where | Consequence for Phase 3 |
|---|---|---|
| `pure` is enforced at *call sites* only: impure builtin, impure named callee, impure nested `def` | `src/check.c:1738`, `2069`, `3358` | The enforcement points for an effect system already exist. Effects extend these three checks rather than adding a fourth analysis. |
| Purity is **not** on function types — a `pure` function may call a function *value* freely | `Type.fun` has `params/ret/count` only (`src/check.c:56`) | This is the one real soundness hole in `pure` today, and it is exactly the hole higher-order model code (`map`, hooks, `>>`) falls through. Blocking for W3. |
| `builtin_pure` is a flat name list: 38 of 52 builtins are pure | `src/check.c:801`, `is_builtin` `src/check.c:765` | Effect *labels* can be one more column on that table. No per-builtin signature machinery needed. |
| Termination is structural descent through recursive-alias fields, with `partial` as the opt-out | `src/check.c:2836–2857`, comment at `2643` | `while True`, mutual recursion, and descent through `list` elements are the three named gaps. They are the whole of W4. |
| `--proof` bans `any` and `partial` (`E_PROOF_ANY`, `E_PROOF_PARTIAL`) | `src/check.c:3364`, `src/main.c:278` | The mode exists; it is under-strict, not missing. Additive work. |
| `list[T]` is **covariant**: `assignable_rec(dst->elem, src->elem)` | `src/check.c:373` | Deliberately inherited from TypeScript, documented as unsound in `docs/proofs.md`. A sound fragment needs a second sequence type, not a rewrite. |
| `DiagSeverity` has `SEV_WARNING`/`SEV_NOTE`, both rendered in text and JSON, but **every** diagnostic is created as `SEV_ERROR` and no `W_` code exists anywhere in `src/` | `include/diag.h:24–28`, `src/diag.c:72`, `168`, `241` | Warnings are a ~20-line unlock (`ck_warn`), not a subsystem. Do it first; `W_VACUOUS_PROOF` is meaningless without it. |
| `Type` is a fat struct with a `TyKind` tag and per-kind fields | `src/check.c:29–31`, `44–70` | `TY_SEQ` and a taint bit are additive, the same way `TY_TENSOR`/`TY_FIN` were in Phase 2. |
| The dim normalizer decides `==` and `≤` over sum-of-products, and **logs what it cannot decide** | `src/dim.c:359–394`, `--shape-report` | `Eq[a, b]` evidence reuses it directly. The escalation log is also the precedent for W8's measurement-first design. |
| Codegen never mentions a `Type`; runtime bounds checks are live (`rt_fatal` on index) | `src/codegen.c`, `src/runtime.c:699` | Bounds-check elimination stays out of scope, again. Phase 3 changes what is *rejected*, not what is *emitted*. |
| Golden tests only; one unit harness exists (`tests/shape/dim_unit.c`) | `tests/shape/` | Effect inference and the SCC termination analysis are the second and third things that want unit tests. The precedent is set. |

## 2. Five decisions to take before writing code

### D1. Effects are a closed label set with inferred rows, not user-declared effects

**Decide: closed.** `IO`, `Rand`, `Mut`, `Alloc`, `NonDet` — the set named in
research-directions §3 — fixed in the compiler, with no surface syntax for
declaring a new effect. A function's effect set is **inferred** from its body
and only *checked* against an annotation when one is written.

Reason: user-declared effects need a handler story to be worth anything, and a
handler story is an evaluation-order change in a codegen that is currently a
direct C translation. A closed set is a lattice join over a 5-bit mask — the
join is `|=`, the subtyping test is `&`, and both fit inside the existing
`assignable`. `pure` becomes the empty mask and stays a keyword (see D-open).

Consequence: `!{Rand, Mut}` annotations are checked, never inferred *away*; a
function annotated `pure` whose body allocates is a *diagnosable* error rather
than a silently-widened signature.

### D2. Effects live on the function *type*, and closing that hole is blocking

`Type.fun` gains an effect mask. `(int) -> int` means "at most the effects on
this arrow", so `(int) -> int !{}` (pure) is assignable to `(int) -> int !{IO}`
and not the reverse — ordinary contravariance, already implemented for
parameters.

This is not optional polish. Without it `map(xs, print_and_return)` typechecks
inside a `pure` function today, which means *every* purity claim in the tree is
conditional on nobody using a higher-order builtin. `map`/`filter`/`reduce` get
effect-polymorphic signatures (one effect variable each, resolved at the call
site by the same `Subst` that resolves type variables at `src/check.c:818–950`).

### D3. `seq[T]` is a new type constructor; `list[T]` keeps its covariance outside proof mode

**Decide: add, don't break.** `TY_SEQ` — immutable, covariant, sound. `list[T]`
stays covariant and mutable in ordinary code (that is the README's promise and
the ergonomics the scripting language is for), but:

- under `--proof`, `list[T]` assignability becomes **invariant**, and
- outside proof mode, a covariant list assignment that would be unsound emits
  `W_UNSOUND_COVARIANCE` (the first real use of W1's warning channel).

Reason: making lists invariant globally is a breaking change to a language with
a 13-module stdlib and 123 tests, taken *before* there is data on how much real
code depends on the covariance. The warning produces that data over one phase;
the invariance decision is then made from a count, not a preference. This is the
same measure-then-escalate pattern as the Phase 2 dim log (D3 of `SPEC_V2.md`).

`seq[T]` needs: literal syntax (reuse `[...]` with contextual typing — no new
token), `freeze(xs: list[T]) -> seq[T]` and `thaw`, indexing, `len`, iteration,
and `map`/`filter`/`reduce` overloads. No new runtime object: a `seq` is an
`O_LIST` the checker refuses to mutate, so codegen is unchanged.

### D4. Induction is *derived eliminators*, not a tactic language

For each non-generic recursive alias, the checker derives a fold whose
termination is true by construction, so a proof about all values of that type
is a total function rather than a `partial` one:

```
type Nat = { zero: bool } | { succ: Nat }

# derived, not written:
#   elim_Nat[R](n: Nat, on_zero: () -> R, on_succ: (Nat, R) -> R) -> R pure
```

Reason: this is the largest increase in provable-claims-per-line available
(research-directions §6), and it costs one derivation pass plus one codegen
template. A tactic language, metavariables, or an elaborator is the line §14
explicitly says not to cross.

Scope cut: derive for *non-generic* recursive aliases only in this phase.
Generic recursive aliases need the eliminator to be generic in the alias
parameters too, and that interacts with the fresh-variable-per-call-site logic
in a way worth deferring until the non-generic case has users.

### D5. `any`-taint is a bit on `Type`, not a separate analysis pass

Add `bool tainted` to `Type` alongside `fresh` (`src/check.c:46`). It is set
when a type is constructed from `TY_ANY` or from a dynamic shape (`Tensor[f32,
?]`), and it is **propagated by construction**: `ty_list(t)` inherits `t`'s bit,
record and union constructors join their members', a call's result inherits the
join of the arguments' bits when the callee is generic.

Reason: a separate dataflow pass over an already-built type graph would have to
re-derive provenance the checker had in hand and threw away. The bit costs one
`|=` per constructor. Then:

- `--proof` rejects a *tainted* type wherever it rejects `any` today, which is
  what closes the `[]`-is-`list[any]` hole named in the research-directions
  Status section;
- outside proof mode, an obligation discharged by a tainted type
  (`x: never = e` where `e` is tainted, an exhaustiveness check over a tainted
  union) emits `W_VACUOUS_PROOF`, which is the warning `docs/proofs.md` has been
  promising in prose since Phase 1.

## 3. Workstreams

Ordered by dependency. W1–W5 are the critical path to the exit criterion.

### W1 — The warning channel (blocking, small)

Nothing in the compiler has ever emitted a non-error diagnostic. Two of this
phase's deliverables are warnings.

- `ck_warn(...)` / `ck_warn_t(...)` mirroring the existing `ck_error` pair,
  setting `SEV_WARNING` (`src/diag.c:72` currently hardcodes `SEV_ERROR`).
- `diag_error_count` already filters on severity (`src/diag.c:116`), so a
  warning must not fail the build — verify that and test it.
- `--werror` to promote warnings, and `-Wno-<code>` to silence one. Proof mode
  implies `--werror` for `W_VACUOUS_PROOF`.
- The `W_` code namespace enters `docs/diagnostics.md` next to `E_`.

*Done when:* a golden test asserts a program that emits a warning still compiles
and still runs, and that `--werror` makes it fail.

### W2 — `any`-taint and vacuity

D5, implemented. Then the two consumers:

- `W_VACUOUS_PROOF` at every obligation site that a tainted type satisfies:
  `never` bindings, `match` exhaustiveness, `Eq` evidence (W7), shape equalities
  discharged through `Tensor[f32, ?]`.
- Proof mode rejects tainted types: the empty-list literal `[]` with no
  contextual type becomes an error under `--proof` rather than silently
  `list[any]`. Check `stdlib/` and `tests/proof/good_stdlib.rald` for how much
  of the stdlib survives that; the count is a result worth writing down.

*Tests:* extend `tests/proof/` — currently 5 cases (10 files), which is thin for the
feature the phase is named after.

### W3 — Effects

- `EffMask` (5 bits) on `FuncSig` and on `Type.fun`; `pure` desugars to the
  empty mask at parse time so there is one representation, not two.
- Surface syntax `-> T !{Rand, Mut}`, parsed where `pure` is parsed today.
- Inference: a function's mask is the join of its callees' masks, its builtins'
  masks (one column added to `builtin_pure`, `src/check.c:801`), and the
  intrinsic effects of its statements (assignment to a captured cell → `Mut`,
  tensor allocation → `Alloc`).
- Recursion: fixpoint over the call graph — start every function at the empty
  mask and iterate the SCC to stability. This is the same SCC computation W4
  needs; build it once, in one place, and use it twice.
- Effect polymorphism for `map`/`filter`/`reduce` and for user functions taking
  function parameters: one effect variable per arrow, unified at the call site.
- `Alloc` and `NonDet` are *tracked and reported* but not banned by anything in
  this phase. They exist so Phase 4's autodiff and Phase 5's determinism claims
  have a place to attach; banning them now would only produce annotation churn.

*Done when:* `pure` is unsmugglable — the higher-order hole in D2 has a test
that fails on the current tree and passes after.

### W4 — Termination, completed

The three gaps named in `src/check.c:2643`:

- **Mutual recursion.** Build the call-graph SCCs (shared with W3). Within an
  SCC, require a lexicographic descent measure over the parameters: some
  parameter descends structurally on every edge of the cycle and none ascends.
  Reject with `E_TYPE_TERMINATION` naming the cycle, not just the function.
- **Descent through sequence elements.** `f(t.kids[0])` where `kids: seq[Tree]`
  is a descent — element-of is structurally smaller. Note this is sound for
  `seq[T]` and *not* for `list[T]`, which can be mutated between the call and
  the recursion. That asymmetry is an argument for D3 that shows up for free.
- **`while` loops.** Recommend the honest cut: in proof mode, a `while` whose
  condition is not statically decreasing is an error (`E_TYPE_TERMINATION`),
  with `partial` — already banned in proof mode — as the only escape. Do *not*
  attempt a general ranking-function synthesis; `for i in range(n)` is the
  supported total loop and the stdlib should be measured against that.

*Tests:* the golden suite plus a unit harness for the SCC/descent analysis, in
the shape of `tests/shape/dim_unit.c`.

### W5 — `seq[T]`

D3, implemented: `TY_SEQ`, covariant in `assignable`, invariant `list` under
proof mode, `W_UNSOUND_COVARIANCE` outside it, `freeze`/`thaw`, and the checker
refusing `xs[i] = v` and `append` on a `seq`. Runtime unchanged (a `seq` is an
`O_LIST`); codegen unchanged.

Then port the parts of `stdlib/` that never mutate (`lists`, `strings`, `sort`
on a copy, `chars`) to take `seq[T]` and report how many signatures changed.
That number is the ergonomics evidence D3 asked for.

### W6 — Induction principles

D4, implemented: derive `elim_<Alias>` for each non-generic recursive alias,
type it `pure`, mark it total by construction (it bypasses the descent check
because its own lowering is a bounded fold), and lower it in codegen as the
recursion the user would have written.

*Done when:* a proof about all `Nat` (e.g. `add_zero`) is written in
`tests/proof/` as a total, pure, proof-mode-clean function.

### W7 — Propositional equality `Eq[a, b]`

The Phase 2 shape solver re-derives every dim equality at every site. `Eq[a, b]`
with `refl` makes a discharged equality a *value* that can be passed, stored in
a record, and returned:

- `Eq[a, b]` as a type (a `TY_` kind or a built-in generic alias — prefer the
  latter if it can carry two `DimExpr`s without a new kind).
- `refl : Eq[a, a]`, checked by `dim_eq` (`src/dim.c`).
- Elimination: given `e: Eq[a, b]`, a `Tensor[f32, [a]]` is usable as
  `Tensor[f32, [b]]` within the scope where `e` is in hand.

Scope cut: dim-level equality only. Equality between arbitrary types needs the
elaborator D4 refuses.

*Why now:* it is the piece that makes the shape system compositional across
function boundaries, and it is the piece Phase 5's interpretation obligations
will be phrased in.

### W8 — `--proof-report`, the measurement

`--shape-report` (Phase 2, D4) exists because "how many obligations were
discharged statically" is the empirical result, not bookkeeping. The same
argument applies one level up. `--proof-report` prints, for a program:

| Counted | Why it matters |
|---|---|
| functions total / `partial`, and which | the termination story's actual coverage |
| functions pure / by effect mask | how much of a model is a function |
| obligations discharged: exact / vacuous (tainted) / assumed | the honest denominator of every proof claim |
| taint sites, by source location | where `any` actually enters real code |
| covariance warnings | the D3 escalation dataset |

Report as text and under `--json`, for the same reason the diagnostics are:
Phase 5's certificates and Track J's repair loop both consume it.

### W9 — Docs, metatheory, and the re-audit

- `docs/proofs.md` — a re-audit against the stricter checker. It currently
  claims "no dependent types … 'this index is in bounds' is not expressible",
  which Phase 2's `Fin[n]` already falsified; and it promises vacuity warnings
  that did not exist. Both get corrected, and the "What Emerald cannot prove"
  section rewritten against what W1–W7 actually land.
- `docs/effects.md` — new: the label set, inference, the arrow syntax, what
  `pure` does and does not promise.
- `docs/core-calculus.md` — the effect and `seq` rules, written in the same
  style as the existing subtyping rules. Required for the Track A/B paper.
- `docs/diagnostics.md` — the `W_` namespace, `--werror`, `--explain`.
- `docs/grammar.md` — `!{...}`, `seq[T]`, `Eq`, `refl`.
- `README.md` — the totality/purity/proof section is now describable without
  the caveats it currently carries.
- Property-based checking of the checker (research-directions §10): generate
  well-typed programs and assert they check; mutate them and assert they fail.
  Start it here because effect inference and the SCC analysis are the first
  components whose bugs are *silent* rather than loud.

## 4. Milestones

| # | Milestone | Contents | Exit |
|---|---|---|---|
| M0 | Warnings exist | W1 | a warning compiles, `--werror` fails |
| M1 | `any` cannot hide | W2 | `[]` under `--proof` is an error; `W_VACUOUS_PROOF` fires on the `proofs.md` example |
| M2 | `pure` is unsmugglable | W3 | the higher-order purity test fails before, passes after |
| M3 | Nothing diverges in the checked fragment | W4 | mutual recursion and unbounded `while` are rejected under `--proof` — **the exit criterion**, with M1 and M2 |
| M4 | There is a sound fragment | W5, W6, W7 | a `Nat` induction proof and a cross-function shape equality both check, proof-mode clean |
| M5 | It is measured and written down | W8, W9 | `--proof-report` over `stdlib/` and `examples/`, docs re-audited |

M3 is the milestone the phase is judged on: M1+M2+M3 together are exactly the
sentence "`--check --proof` passed" has to be able to bear. M4 makes proof mode
*useful* rather than merely honest; M5 turns it into the Track A/B paper's
evaluation section. If the phase must be cut short, cut after M3 and write up
what the report says about the stdlib.

## 5. Explicitly not in this phase

- **No autodiff, no weight loading, no ONNX.** Phase 4, unchanged.
- **No `circuit` / `interpretation` declarations, no approximate judgments, no
  certificates.** Phase 5. They are the *consumers* of this phase; building a
  certificate format over an untrustworthy checker is the failure mode this
  phase exists to prevent.
- **No user-declared effects and no handlers** (D1).
- **No row polymorphism for records** (Track F). Effect rows and record rows
  are different features that share a word.
- **No linear/affine types.** In-place tensor ops are Phase 4's problem, and
  they want `Mut` to exist first — which is what W3 delivers.
- **No generic recursive-alias eliminators** (D4 scope cut).
- **No bounds-check elimination.** Still blocked on typed codegen; still a
  Phase 4 item bundled with autodiff's lowering.
- **No SMT.** The Phase 2 dim log decides that, and it decides it with data.
- **No self-hosting.**

## 6. Risks

| Risk | Mitigation |
|---|---|
| Effect annotations become noise — every function grows a `!{...}` and the language stops looking like Python | Inference is the default and annotations are optional everywhere except where a promise is being made. Measure with `--proof-report`: if the stdlib needs annotations on more than a small fraction of its functions, the inference is wrong, not the users. |
| Making `list` invariant breaks the stdlib and the examples | D3: don't. Warn, count, and decide next phase. |
| Termination checking rejects reasonable code (`while` in the stdlib's IO loops) and proof mode becomes unusable in practice | Proof mode is a *mode*. Ordinary code keeps `while`. If the stdlib cannot pass proof mode at all, that is the finding, and the honest response is to report which modules can. |
| The effect fixpoint and the termination SCC are two implementations of the same graph, and they drift | Build the call graph once, in one file, with the unit harness. Named explicitly in W3 and W4 for that reason. |
| Taint over-fires: every type touched by a stdlib `any` is tainted and every proof is "vacuous" | Taint is set at construction from `any`/`?` only, not from unification with a taint-free variable. If it still over-fires, the warning is the diagnostic *for the stdlib*, and W2's stdlib survey is scheduled precisely to find out before the warning ships as an error. |
| `Eq[a, b]` pulls the elaborator in through the back door | W7's scope cut is dim-level only. If type-level equality starts wanting metavariables, stop — that is research-directions §14's line. |

## 7. One decision left open

**D-open: does `pure` survive as a keyword once `!{}` exists?**

Two representations for one property is exactly the drift the `builtins.md`
"thirteen builtins" bug came from. Three options:

1. **Keep both**, `pure` as sugar for `!{}`. Familiar, and 38 builtins plus
   half the stdlib are already annotated with it. But two spellings of the same
   thing in the grammar forever.
2. **Deprecate `pure`** in favour of `!{}`. One spelling, but it churns every
   existing annotation and makes `def f() -> int !{}` the way to say the most
   common thing, which is backwards — the common case should be short.
3. **Keep `pure`, and do not add empty-`!{}` at all** — `!{...}` is only ever
   written with at least one effect, and purity has exactly one spelling.

Recommendation: **option 3.** It keeps the ergonomics (`pure` reads as English,
and it is the annotation people write most), it keeps one spelling per concept,
and it means the parser never has to decide what `!{}` means next to a `pure`
keyword on the same signature. Internally there is still one representation —
the empty mask — which is all D1 actually needs.

Decide before W3 lands, not after.
