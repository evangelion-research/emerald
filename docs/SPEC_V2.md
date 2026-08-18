# Emerald Phase 2 — Make It a Numeric Language

**Status:** implemented. Phase 1 (the scripting language, the proof fragment's
first slice, the functional core, modules) plus Phase 2 (tensors, shapes, the
MLP demo) are green — 123 tests. The exit criterion is met: a shape bug is a
compile error with both shapes printed (`tests/check/bad_shape_matmul.rald`,
`examples/mlp/shape_bug.rald`).

**Thesis obligation this phase serves:** you cannot write a neural network in
Emerald today. Every later track —
interpretations as typed objects, approximate judgments, autodiff, typed hooks —
assumes tensors exist and that their shapes are in the type system. This phase
buys that and nothing else.

**Exit criterion** (from
[`research-directions.md`](research-directions.md) §13): *a shape bug is a
compile error with both shapes printed*, and a hand-written MLP trains on a toy
task end-to-end in Emerald.

---

## 1. What the substrate already gives us

Facts established by reading the current tree, because the plan below leans on
each of them:

| Fact | Where | Consequence for Phase 2 |
|---|---|---|
| Codegen is completely untyped — `src/codegen.c` never mentions a `Type` | `src/codegen.c` | Tensors must be *runtime* objects reached through `Value`. No monomorphization needed, and none should be added. |
| Generics are checked by unification at each call site with a `Subst` | `src/check.c:818–950` | Shape variables can ride the *existing* unification machinery instead of a new one. This is the single biggest cost saving available. |
| `Type` is a fat struct with a `TyKind` tag and per-kind fields | `src/check.c:31–47` | Adding `TY_TENSOR` is additive; no representation rewrite. |
| `TypeExpr` already supports generic application `Name[T, ...]` | `include/ast.h:27–51` | `Tensor[f32, [B, S, D]]` parses into existing shape *if* a dim-expression node is added. |
| Objects are GC'd by **count**, not bytes (`gc_young_threshold = 256` objects) | `src/runtime.c:35–39, 208` | A 400 MB tensor counts as one object. The collector will not fire. **This must be fixed before tensors ship.** |
| Every stage has a driver flag and a golden suite; no stage is observable only through the next | `docs/architecture.md`, `tests/` | Shapes need their own observable surface (`--emit-shapes`) and their own suite, or they violate the project's own architecture rule. |
| `float` exists end to end (lexer → `TY_FLOAT` → `V_FLOAT`) | `src/lexer.c:61–80`, `src/runtime.c` | dtype work starts from a real scalar float, not from scratch. |
| Modules, with `-I` resolution and name mangling, work | `docs/modules.md` | Tensor operations have a plausible home other than "more builtins". |

## 2. Four decisions to take before writing code

These are the choices that are expensive to reverse. Each is stated with the
recommendation and the reason.

### D1. Tensor ops are whole-array runtime calls, not per-element generated code

**Decide: whole-array.** `matmul(a, b)` lowers to one `em_matmul(Value, Value)`
call; the loop nest lives in `src/runtime.c` over an unboxed `float *`. The
`Value` boxing cost is then paid once per *operation*, not once per *element*,
which is what makes an untyped codegen survivable for numerics. A per-element
path through `em_index`/`em_mul` would be roughly two orders of magnitude
slower and would force typed codegen — i.e. it would force the Phase 4 work
into Phase 2.

Consequence: the set of tensor primitives is closed and small, defined once in
`runtime.h`. That closed vocabulary is also exactly what a Phase 4
source-to-source autodiff transform needs, so this decision pays twice.

### D2. `Tensor` is a built-in type constructor, not a generic alias

Shape arithmetic (`D == H * Dh`), broadcasting, and the reshape product
obligation are typing *rules*, not instantiations. Add `TY_TENSOR` to the
`TyKind` enum with `{ DType dt; Shape *shape; }`. A user-level alias
`type Vec[N] = Tensor[f32, [N]]` still works on top.

### D3. The shape solver is a canonical-form normalizer, not SMT

Per §2 of the research directions: normalize each dim expression to a
sum-of-products over dim variables and integer literals, then compare
syntactically. This discharges concat, split-heads, reshape, and broadcast —
the obligations that actually occur. **Do not take an SMT dependency in this
phase.** Escalation is a Phase 3+ decision, and the normalizer's failures
should be *logged* so that decision is made from data.

### D4. Dynamic shapes are a first-class escape hatch, and the boundary is measured

`Tensor[f32, ?]` must exist from day one, or no real program can be ported.
Every static→dynamic crossing inserts a runtime shape assertion and is
**counted**, with the count reportable (`--shape-report`). That counter is not
bookkeeping: "how many obligations were discharged statically vs dynamically in
ported model code" is the empirical result of the gradual-shape-typing paper
(§15.1). Building the counter later means re-running every experiment.

## 3. Workstreams

Ordered by dependency. W1–W4 are the critical path to the exit criterion.

### W1 — Tensors in the runtime (untyped first)

Make tensors *exist and run* before making them *checked*. This keeps every
later workstream testable.

- `O_TENSOR` alongside `O_STR/O_LIST/O_REC/O_FUNC/O_CELL` in `include/runtime.h`:
  `{ DType dt; uint8_t ndim; int64_t *dims; int64_t *strides; void *data; Obj *base; }`.
  `base` is a non-NULL owner for a view, which makes slicing and transpose
  zero-copy and gives the GC a single edge to trace.
- **GC byte accounting** (blocking, per §1): track `gc_bytes_young/old`
  alongside the object counts and trigger on either. Extend `gc_stats()` with
  `bytes_young` / `bytes_old`, which also makes the fix observable from a test.
- dtypes: `f32`, `f64` in this phase. Reserve the tag width for `f16`, `bf16`,
  `i8`, `i32` but do not implement them — they are Phase 4 (quantized real
  models), and stubbing them now means writing kernels nobody calls.
- Primitives: construction (`zeros`, `ones`, `full`, `arange`, `tensor` from a
  nested list, `randn` with a seeded generator), elementwise binary with
  broadcasting, elementwise unary (`exp`, `log`, `tanh`, `relu`), `matmul`,
  `reshape`, `transpose`/`permute`, `sum`/`mean`/`max`/`argmax` over an axis,
  slicing, and `item`.
- Printing: a tensor's `str()` must show dtype and shape. This is the debugging
  surface for the entire phase.

*Done when:* an untyped Emerald program builds an MLP forward pass and prints
the output, and `gc_stress`-style allocation of large tensors stays flat in
RSS.

*Tests:* `tests/e2e/tensor_*.rald` (values and shapes at runtime),
plus a GC test asserting `gc_stats().bytes_*` falls after a collection.

### W2 — Type-level naturals and the canonical-form solver

Build this **standalone and unit-tested before wiring it into the checker**. It
is the one component whose bugs would be silent and would invalidate every
proof claim downstream.

- `DimExpr`: variable, literal, `+`, `*` (and `-` only if a use case appears —
  it complicates normalization and is not needed by the target ops).
- `dim_normalize` → sum-of-products canonical form with sorted terms; `dim_eq`
  is then `memcmp`-shaped structural equality.
- `dim_le` for `Fin[n]` (W5). Start with the decidable fragment: equal
  normal forms, literal comparison, and `a ≤ a + k` for literal `k ≥ 0`.
- **Failure logging**: when `dim_eq` cannot decide, record the pair. This is
  the dataset that decides D3's escalation question.

*Tests:* a dedicated `tests/shape/` suite driven by a new `--emit-shapes`
flag, plus a direct unit harness for the normalizer (the first non-golden test
in the project; that is fine and should be stated in `architecture.md`).

### W3 — Surface syntax: `dim`, shape annotations, kinded type parameters

- `dim Batch, Seq, DModel` — a declaration introducing **nominally distinct**
  dimension names. `Batch` and `Seq` both being sizes must not make them
  interchangeable; transposition bugs are the most common silent bug in model
  code and nominal dims are what catch them.
- `Tensor[f32, [B, S, D]]` — needs a new `TypeExpr` kind for a shape list and
  for dim arithmetic. `TE_LIST` is `list[T]` and must not be overloaded.
- Kinded type parameters: `parse_type_params` (`src/parser.c:285`) currently
  reads bare names; extend to `name` *or* `name: dim`. Both `def f[B: dim]` and
  `type Vec[N: dim]` route through that one function, so this is a single edit
  serving both.
- Keyword strategy: `dim` becomes a keyword (`docs/grammar.md:24` lists the
  existing set). Check `examples/` and `tests/` for uses as an identifier
  before committing.

### W4 — Typing rules, and the error message that is the exit criterion

Per operation, with the obligation each generates:

| Op | Obligation |
|---|---|
| `matmul(a: [M,K], b: [K2,N])` | `K ≡ K2` → `[M,N]` |
| elementwise `a ⊕ b` | broadcastable, statically decidable, else require explicit `expand` |
| `reshape(t, s)` | `prod(shape(t)) ≡ prod(s)` |
| `transpose`/`permute` | permutation is a bijection on axes |
| reductions | axis literal in range; result shape drops/keeps the axis |
| `concat(ts, axis)` | all non-`axis` dims equal; `axis` dims sum |

The diagnostic is the deliverable, so specify it now, in the existing
`E_`-code style (`docs/diagnostics.md`):

```
error[E_SHAPE_MATMUL]: contracted dimensions do not match
  --> model.rald:42:12
     |
  42 |     h = matmul(x, w2)
     |         ^
   = left:     Tensor[f32, [Batch, DModel]]
   = right:    Tensor[f32, [DHidden, DOut]]
   = mismatch: DModel != DHidden  (contraction axis)
```

Both shapes printed, the mismatching axis named. `--json` carries `left`,
`right`, and `mismatch` as separate fields, so the LLM-repair loop of Track J
gets structure rather than prose.

Also in W4: `Tensor[f32, ?]` with an inserted runtime assertion at each
static→dynamic boundary, and the D4 counter.

### W5 — `Fin[n]` and index safety

`Fin[n]` is an index provably below `n`. `xs[i]` with `i: Fin[len(xs)]` is
statically safe.

Honest scoping note: **bounds-check elimination requires codegen to know
types**, and D1 keeps codegen untyped. So in Phase 2 `Fin[n]` buys *static
rejection of provably-bad indexing* while the runtime check remains. That is
still worth having, but the `docs/` text must not claim the checks are
eliminated. Elimination is a Phase 4 item, bundled with whatever typed lowering
autodiff needs.

### W6 — Numerics that make it usable

- **Seeded RNG.** One explicit generator, threaded, not ambient. `rand()` today
  is ambient and impure; the tensor `randn` must take a generator so
  experiments are reproducible *by construction*. This also pre-positions the
  `Rand` effect of Track B.
- **BLAS matmul.** Naive C first (correctness baseline, kept as a reference
  kernel for differential tests), then Accelerate on macOS behind a build flag.
  Keep the naive kernel: it is the oracle.
- A minimal autograd is **out of scope** — Phase 4. The MLP demo trains with
  hand-written gradients, and doing so deliberately produces the requirements
  list for autodiff, the same way the ray tracer produced the proof scorecard.

### W7 — The demo, which is also the evidence

`examples/mlp/` — an MLP trained on a toy task (XOR or a small spiral),
hand-written backward pass, seeded, printing loss curve and final accuracy.

Alongside it `examples/mlp/shape_bug.rald`: the same program with two axes
transposed, whose *expected output is a compile error*. That file is the exit
criterion made executable and belongs in the test suite, not just in `examples/`.

### W8 — Docs and continuous obligations

- New `docs/tensors.md` (runtime model, dtypes, views, GC interaction) and
  `docs/shapes.md` (dim algebra, the solver's decidable fragment, the typing
  rules table, the gradual boundary).
- `docs/diagnostics.md`: the new `E_SHAPE_*` codes.
- `docs/builtins.md` intro currently says "thirteen builtins" while
  `is_builtin` (`src/check.c:582`) lists sixteen. Fix that *now*, before the
  tensor primitives make the drift worse — and decide D-open below.
- `README.md`: the builtins list and the phase status.
- `docs/architecture.md`: the `--emit-shapes` stage flag and the first unit-test
  harness.

## 4. Milestones

| # | Milestone | Contents | Exit |
|---|---|---|---|
| M0 | GC is byte-aware | W1 GC accounting only | large-allocation test stays flat in RSS |
| M1 | Tensors run | W1 | untyped MLP forward pass prints correct numbers |
| M2 | Solver is trustworthy | W2 | normalizer unit suite green, failure log wired |
| M3 | Shapes parse and check | W3, W4 | `shape_bug.rald` fails to compile with both shapes printed — **the exit criterion** |
| M4 | It is usable | W5, W6 | seeded, BLAS-backed, `Fin[n]` in the checker |
| M5 | It is demonstrated | W7, W8 | `examples/mlp/` trains; docs land |

M3 is the milestone the phase is judged on. M4 and M5 make it real, but if the
phase must be cut short, cut after M3 and write up what M3 proved.

## 5. Explicitly not in this phase

Restating §14 of the research directions plus this phase's own boundaries, so
scope creep has to argue against a written line:

- **No autodiff.** Phase 4. Hand-written gradients in the demo, on purpose.
- **No GPU backend.** CPU only until the language ideas are proven.
- **No full dependent types.** If an elaborator with metavariables and universe
  levels starts appearing, the project has turned into a proof assistant.
- **No effect rows** (`!{Rand, Mut, IO}`). `pure` already exists; rows are
  Phase 3.
- **No weight loading / ONNX import.** Phase 4. Tempting because it makes the
  language look real, but it is worthless before shapes are checkable —
  a loader that returns `Tensor[f32, ?]` everywhere wastes the entire phase.
- **No SMT dependency** (D3).
- **No bounds-check elimination** (W5).
- **No self-hosting.**

## 6. Risks

| Risk | Mitigation |
|---|---|
| Broadcasting makes the checker unsound or unusably strict — this is where shape systems usually fail | Only broadcast when statically decidable; otherwise require explicit `expand`. Take the strict cut; loosen from real programs. |
| Untyped codegen makes tensor code too slow to train anything | D1 confines the cost to per-op boxing. Measure at M1 with a real forward pass; if it is fatal, that is far cheaper to learn at M1 than at M5. |
| Builtin explosion — sixteen becomes forty and the "no stdlib" story collapses | See D-open. Decide before W1 lands, not after. |
| The nominal-`dim` rule proves too rigid in practice (every helper needs its own dim vars) | Dim variables are already generic parameters; lean on inference at call sites, and collect the friction cases rather than weakening the rule early. |
| Shape work stalls in the type checker and no program ever runs | W1 first, deliberately: untyped tensors run before shapes are checked. |

## 7. One decision left open

**D-open: are tensor primitives builtins, or the first real stdlib module?**

Builtins are the fast path (the checker already special-cases arity and types
for the existing sixteen) but there are ~25 tensor primitives, and
`docs/builtins.md`'s "no standard library yet, thirteen builtins" framing does
not survive that. The module system already works, with `-I` resolution and
mangling, so `import tensor` is available.

Recommendation: **builtins for the primitives that need special typing rules**
(`matmul`, `reshape`, elementwise, the constructors — the ones whose types are
shape obligations, not signatures) **and a `tensor` module for the derived
ones** (`relu`, `softmax`, `mean`, initializers) written in Emerald itself.
That also dogfoods the module system against a real library, which nothing
currently does.
