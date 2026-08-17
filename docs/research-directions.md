# Emerald as a Research Language

**Goal:** a Python-shaped, strongly-typed, C-compiled language in which you can
*write* neural networks and *study* them through type theory and machine-checked
proof — mechanistic interpretability where the interpretation is a typed object
the compiler validates, not a notebook claim.

This document is a design agenda: what Emerald is missing, what to build, in
what order, and which of it is publishable research rather than engineering.

## 0. The three papers, and what each demands of the language

| Paper | Core claim | What Emerald must grow to serve it |
|---|---|---|
| **From Mechanistic to Compositional Interpretability** (Gauderis, Dooms, Homer, Ayonrinde, Wiggins — arXiv:2605.08934) | An interpretation is a pair of syntactic + semantic mappings that must **commute**; quality = faithfulness vs. complexity (MDL); "compressive refinement" restructures a model into simpler parts without changing its function. | A **categorical core IR** (symmetric monoidal / string diagrams) so models *are* composable morphisms; a first-class `interpretation` construct carrying a commuting-square obligation; a **description-length metric** the compiler can compute over a decomposition; a checked **rewrite/refinement** relation. |
| **Validating Mechanistic Interpretations: An Axiomatic Approach** (Palumbo, Mangal, Wang, Vijayakumar, Pasareanu, Jha — arXiv:2407.13594, ICML 2025) | Borrow program analysis: axioms under which an interpretation *approximately* captures a network's semantics, **compositionally**. | **Approximate / probabilistic judgments** — obligations discharged to a bound `(ε, δ)` rather than exactly; abstraction functions `α` as typed, checkable objects; certificates as build artifacts. This is Emerald's gradual-typing philosophy applied to *proofs*. |
| **Typed Chain-of-Thought: A Curry–Howard Framework for Verifying LLM Reasoning** (Perrier — arXiv:2510.01069) | A faithful CoT trace ≅ a well-typed program; converting a trace into a well-typed proof is a certificate of faithfulness. | Emerald as the **target language for machine-generated proofs**: typed holes, incremental checking, repair-oriented JSON diagnostics (already started), an exportable proof term, and a total/consistent proof fragment so a certificate means something. |

Read together they define one system: **models as morphisms, interpretations as
typed refinements between them, obligations discharged exactly where possible
and statistically where not, with a machine-checkable certificate at the end.**
No existing language does this. That is the thesis.

## 1. Honest gap analysis

What Emerald has that matters here: structural + literal types, unions, flow
narrowing, `never`-based exhaustiveness, parametric generics with opaque type
variables, closures, recursive aliases, structured JSON diagnostics, modules, a
real GC, native compilation. That is a genuinely good substrate.

What blocks the research program today:

1. **No tensors.** You cannot write a neural network. `list[list[float]]` is not
   a tensor and its type says nothing about shape.
2. **No dependent/indexed types.** `docs/proofs.md` says it plainly: a type
   cannot mention a value, so no shape, no length, no bound, no index safety —
   which is exactly the content of every interesting statement about a network.
3. **No induction.** Recursive aliases exist, but the checker has no elimination
   principle, so nothing can be proved *about* an inductive structure.
4. **No termination checking** *(now partially addressed — structural descent
   plus a `partial` opt-out; `while True` and mutual recursion still escape;
   see the Status section)*, so `never` is inhabited by divergence and the
   logic is inconsistent. Any "proof" Emerald emits is currently a proof modulo
   "and this function halts".
5. **`any` is a universal solvent.** `docs/proofs.md` already warns that a proof
   mentioning `any` proves nothing — but nothing *enforces* that. There is no
   proof-mode *(now added: `--proof` bans `any` and `partial`, with taint
   tracking and vacuity warnings still future work)*, no taint tracking, no
   vacuity check.
6. **Unsound covariant lists**, inherited deliberately from TypeScript. Fine for
   scripting, fatal for a proof artifact.
7. **No effects/purity distinction** *(now partially addressed: a `pure`
   declaration with call-level enforcement; no effect rows or `!{...}` yet)*,
   so "this model is a pure function of its inputs" — the precondition for
   every commuting-square argument — is not
   statable.
8. **No path to real models.** Without weight loading and a graph importer you
   can only study toy networks you trained inside Emerald.

Sections 2–9 are ordered roughly by how much of that they unblock.

---

## 2. Track A — Shape types (the single highest-value addition)

Shapes are the *practical* dependent type: enough type-level arithmetic to talk
about sizes, without a full dependent theory. Prior art to mine: Dex, Futhark,
`jaxtyping`, Hasktorch, Diesel/Idris-style singletons, and Christiansen's
"Dependent Types in Practice".

### Surface

```
dim Batch, Seq, DModel, NHeads          # named axes, nominally distinct

type Tensor[dt: DType, shape: Shape]

def attention[B: dim, S: dim, D: dim](
    q: Tensor[f32, [B, S, D]],
    k: Tensor[f32, [B, S, D]],
    v: Tensor[f32, [B, S, D]],
) -> Tensor[f32, [B, S, D]] { ... }

def matmul[M: dim, K: dim, N: dim](
    a: Tensor[f32, [M, K]], b: Tensor[f32, [K, N]],
) -> Tensor[f32, [M, N]]
```

The `K` appearing twice is the whole point: a mismatched contraction is a type
error at the call site with both shapes printed, not a runtime exception 40
minutes into training.

### What to implement

- **Type-level naturals with a tiny solver.** You need `+`, `*`, and literals,
  plus equality and `≤`. Do *not* reach for a general SMT dependency first:
  normalize size expressions to a sum-of-products canonical form and compare
  syntactically; that discharges 90% of real obligations (concat, reshape,
  broadcast, split-heads `D = H * Dh`). Escalate to a bundled Presburger /
  difference-logic decision procedure only when the canonical form fails.
- **Named axes as nominal dims.** `Batch` and `Seq` both being `int` must not
  make them interchangeable. Nominal dimension declarations catch transposition
  bugs, which are the most common and most silent bug in model code.
- **Broadcasting as a typing rule**, with the unification rule spelled out in
  the grammar — broadcast is where shape systems usually get unsound or
  unusably strict; pick "broadcast only when statically decidable, otherwise
  require an explicit `expand`."
- **Reshape needs a product-equality obligation** (`prod(from) == prod(to)`),
  which is exactly where the solver earns its keep.
- **Escape hatch:** `Tensor[f32, ?]` (dynamic shape) checked at runtime, in the
  same spirit as `any`. Gradual shapes — but see §6 on tainting.
- **Index safety:** `Fin[n]` (an index below `n`) turns `xs[i]` into a checked
  proposition and gives you bounds-check elimination for free in codegen.

### Why this is also a paper

*Gradual shape typing with statistical fallback* — shape obligations that the
solver cannot discharge become runtime assertions, and the compiler reports the
static/dynamic boundary as a measured quantity. Nobody has published the
gradual-typing treatment of shapes with a soundness result and a blame
calculus. Emerald's existing gradual core makes it the natural home.

---

## 3. Track B — Effects, purity, and determinism

A commuting-square interpretation is meaningless if the model is not a function.
Add a small effect row system:

```
def forward(x: Tensor[f32, [B, S, D]]) -> Tensor[f32, [B, S, D]] pure { ... }

def sample(logits: Tensor[f32, [V]]) -> int !{Rand} { ... }
def train_step(...) -> f32 !{Rand, Mut, IO} { ... }
```

- Effects: `IO`, `Rand`, `Mut` (mutable state), `Alloc`, `NonDet` (float
  reassociation / nondeterministic reductions).
- **`pure` is the precondition for every proof obligation about a model.** Make
  the interpretation constructs of §4 *require* purity of both sides.
- Effect polymorphism for higher-order code (`map[e]`) — keep it row-based and
  inferable; do not require annotations on ordinary code.
- **Determinism as a tracked effect** is unusually valuable for interp research:
  "this ablation experiment is bit-reproducible" becomes a type.
- Pairs naturally with **linear/affine types** for tensors: in-place ops
  (`x.add_(y)`) without aliasing bugs, guaranteed-no-copy fusion, and
  deterministic memory. Affine (use at most once) is the sweet spot; full
  linearity fights ergonomics.

---

## 4. Track C — Interpretations as first-class typed objects

This is the part that makes Emerald a *research* language rather than a nicer
PyTorch. Following Gauderis et al.: an interpretation is a pair of mappings
that must commute.

### 4.1 A categorical core IR

Make the compiler's intermediate representation a symmetric monoidal category:
morphisms compose sequentially (`>>`) and in parallel (`⊗`), with explicit
copy/discard. Concretely, models become values of a `Circuit[A, B]` type, and
`forward` is one of several possible *interpretations* of that circuit.

```
circuit Attention[B, S, D] : Tensor[f32,[B,S,D]] -> Tensor[f32,[B,S,D]] {
    qkv >> split_heads >> scores >> softmax >> weighted_sum >> merge_heads
}
```

Benefits beyond theory: a monoidal IR is the right form for graph rewriting,
fusion, and `--emit-diagram` (string diagrams as SVG, free documentation).

### 4.2 The `interpretation` declaration

```
interpretation induction_head of Model {
    syntax:   Model.blocks[1].attn.heads[4]  ->  Circuit.Copy[Prev]
    semantic: alpha(activations)             ->  abstract_state
    faithful_to: 0.02                        # allowed error
    on: distribution PileSample(n = 10000)
}
```

Obligations the compiler generates:

1. **Type-level:** the syntactic map is a well-typed circuit homomorphism —
   ports line up, shapes line up, purity holds on both sides.
2. **Commuting square:** `α ∘ f_concrete ≈ f_abstract ∘ α` up to `faithful_to`,
   over the named distribution.
3. **Complexity:** the compiler computes a description length of the abstract
   circuit (nodes, parameters, wire count under a fixed encoding) so that
   competing interpretations of the same model are *comparable numbers*, and a
   refinement can be checked to be genuinely compressive.

Obligation 1 is static. Obligations 2 and 3 are §5.

### 4.3 Compressive refinement as a checked rewrite

Give the language a rewrite-rule form whose left and right sides must be
provably (or approximately) equal as morphisms:

```
refine unfold_mlp : Model.mlp  ~>  (up >> gelu >> down)  by equality
refine prune_head : Model.blocks[3].heads[7]  ~>  zero   by approx(0.01)
```

The compiler maintains, per model, a **refinement tree** with faithfulness and
description-length at each node — the constrained optimisation the paper
describes, made into a build artifact you can diff across commits. "Did my new
interpretation actually get simpler without losing faithfulness?" becomes
`task interp:report`.

---

## 5. Track D — Approximate and statistical judgments

The axiomatic-validation paper's real lesson: for neural networks the
interesting properties are *approximate*, so bolt approximation into the
judgment form instead of pretending it away.

- **Probabilistic obligations.** `⊨(ε, δ)` — "the commuting square holds with
  error ≤ ε with confidence 1−δ over distribution D". Discharged by sampling at
  build time, with the sample count derived from a Hoeffding/Bernstein bound the
  compiler computes for you. The sample count is *not* a magic number the
  researcher picked; it is dictated by the requested bound.
- **Certificates as artifacts.** Every discharged obligation emits a JSON
  record: obligation, method (static | statistical | assumed), distribution,
  n, measured error, bound, seed, source location, and a hash of the model
  weights. `emeraldc --certificate out.json`. This is the reproducibility story
  and it's a natural extension of the existing `--json` diagnostics.
- **`assume`** for an obligation you consciously take on faith — recorded in the
  certificate as *assumed*, with the source location, so an assumption can never
  hide.
- **Distributions as typed values** (`distribution D over Tensor[...]`), since
  every approximate claim is relative to one and papers routinely leave it
  implicit.
- **Abstraction functions `α` as language objects**, with an obligation that
  they are consistent (deterministic, total on the support of D).

Research framing: *gradual verification for neural systems* — a single language
where a property can be proved statically, checked dynamically, estimated
statistically, or assumed, with the boundary explicit and machine-readable. That
is a strong, defensible contribution, and it is the same idea as Emerald's
gradual types applied one level up.

---

## 6. Track E — Making the proof fragment actually mean something

Today `--check` passing is not a proof: divergence inhabits `never`, `any`
discharges everything, and lists are unsound. Fix all three, but *without*
losing the Python-like scripting experience — by making rigor a mode.

- **`--proof` / `#[proof]` mode**, in which: `any` is banned, dynamic shapes are
  banned, covariance is restricted to immutable sequences, and every function
  must pass termination checking.
- **`any`-taint propagation.** Even outside proof mode, track whether a value's
  type was ever routed through `any` or `?` and report obligations discharged
  vacuously as warnings (`W_VACUOUS_PROOF`). Cheap to implement, and it directly
  operationalises the warning already written in `proofs.md`.
- **Termination checking** by structural descent on recursive aliases, with an
  explicit `partial` keyword for functions that opt out (and which then may not
  appear in a proof). This turns `never` into an honest bottom type.
- **Induction principles.** Recursive aliases already exist; derive the
  eliminator automatically so `Nat`/`List`/`Tree` claims are provable rather
  than merely statable. This is the single change that most increases what
  Emerald can prove per line of implementation.
- **Immutable sequences** (`seq[T]`, covariant and sound) distinct from `list[T]`
  (invariant, mutable). Keeps the TypeScript ergonomics *and* gets a sound
  fragment.
- **Propositional equality** (`Eq[a, b]`) with `refl`, even in a restricted
  form, so shape equalities can be passed around as evidence rather than being
  re-derived by the solver at each site.

---

## 7. Track F — Type-theoretic infrastructure worth adding regardless

- **Row polymorphism for records.** Intersections are a weak substitute:
  `{ ...r, lr: float }` with a row variable expresses model configs, optimizer
  states, and hook contexts far better, and it is a modest extension to a
  structural checker you already have.
- **Bounded quantification** (`T: Numeric`) plus **type classes / traits**. You
  will want `Numeric`, `Differentiable`, `Serializable` as constraints; without
  them every numeric routine is written per-dtype.
- **GADTs / indexed data types.** The single most useful feature for
  *representing* the computation graphs you want to analyze: a typed AST where
  `Expr[T]` guarantees a well-typed circuit by construction. Interp tooling
  written in Emerald over Emerald's own IR becomes type-safe.
- **Higher-kinded types**, if you want functorial constructions (tangent
  functors, `Circuit[-, -]` as a profunctor) stated rather than duplicated.
- **Refinement types on scalars** (`{ x: float | 0.0 <= x <= 1.0 }`) for
  probabilities, learning rates, temperatures. Reuse the §2 solver.
- **Module-level abstraction:** signatures/interfaces over the existing module
  system so a `Model` can be quantified over abstractly — needed before any
  claim of the form "for all models of this shape".

---

## 8. Track G — Numerics: what makes it a real NN language

None of the theory matters if you cannot run a model.

- **N-d array in the runtime:** strided views, dtype tag (`f32/f16/bf16/i8`),
  slicing without copy, `Obj` variant `O_TENSOR` alongside the existing
  `O_STR/O_LIST/O_REC`. Keep it GC-managed but with a manual free path for large
  buffers (or arena/affine ownership per §3).
- **Kernels:** naive C first, then Accelerate/BLAS on macOS, then a small
  autotuned matmul. Don't write a GPU backend by hand — emit MLIR/StableHLO or
  target an existing runtime when you get there.
- **Autodiff as a typed source-to-source transformation.** This is the most
  elegant place to do type-theory research in a practical setting: define a
  tangent-type functor `T` on your types and give
  `grad : (A -> R) pure -> (A -> T A) pure` a real type, with the transformation
  proved (or at least checked) type-preserving. Reverse mode over a monoidal IR
  is *transposition of morphisms* — which lands you directly in the same
  categorical framework as §4. Cite: Elliott's "The Simple Essence of Automatic
  Differentiation"; Dex's index-set approach.
- **Seeded, effect-tracked RNG** so experiments are reproducible by construction.
- **Weight I/O:** safetensors first (simple, typed, widely used), then GGUF.
  Loading must *reconstruct shapes into the type system* — a loader that returns
  `Tensor[f32, ?]` everywhere wastes the entire type system, so provide a
  declared-schema loader that checks the file against a written model type and
  fails loudly.
- **Model importer:** ONNX or StableHLO → Emerald typed IR. This is the bridge
  from toy models to real ones and it is the difference between a language demo
  and a research tool. Prioritize it earlier than feels comfortable.

---

## 9. Track H — The mech-interp API, typed

The reason to build a language rather than a library: interpretability
operations become type-checked instead of string-keyed.

- **Typed hooks.** In TransformerLens you write `"blocks.4.attn.hook_z"` — a
  string, wrong at runtime if the model changes. In Emerald a hook site is a
  path with a type: `hook(model.blocks[4].attn.z) : Tensor[f32, [B,S,H,Dh]]`.
  Renaming a layer breaks the analysis at compile time. That alone is worth the
  project.
- **Ablation / patching as scoped effects:** `with patch(site, value) { ... }`,
  where the patch's type must match the site's type and the block's purity is
  tracked.
- **Activation caches as row-typed records** — the cache's type is derived from
  the model type, so `cache.blocks[4].attn.z` is checked and autocompleted.
- **Circuits as values** you can compose, diff, and hand to §4's interpretation
  machinery. `subcircuit`, `restrict`, `compose` with port typing.
- **SAE / feature-dictionary types:** `Dict[NFeatures, DModel]` with the
  reconstruction obligation stated as an approximate judgment per §5.
- **Causal-scrubbing style hypotheses** expressed as a checked correspondence
  between a hypothesis circuit and the model — which is precisely a refinement
  obligation, so it falls out of §4.3 rather than being a bespoke feature.

---

## 10. Track I — Metatheory and assurance

If the compiler's own type checker is buggy, every certificate it emits is
worthless. Proportionate rigor:

- **Write the core calculus down.** A small paper-style formal spec of
  Emerald-core (types, subtyping, narrowing, generics) in `docs/core-calculus.md`
  — even unmechanized, this catches design bugs and is required for any paper.
- **Mechanize progressively** in Lean or Rocq: start with subtyping being
  reflexive/transitive and narrowing being sound; grow toward preservation.
- **Property-based / fuzz testing of the checker:** generate random well-typed
  programs, assert they check; generate random ill-typed mutations, assert they
  fail. Also random programs run under both a reference interpreter and the
  compiled binary (differential testing) — the cheapest way to find codegen bugs.
- **Solver certificates:** when the shape solver discharges an obligation, it
  should be able to emit a checkable justification, so the trusted core stays
  small.

---

## 11. Track J — Making Emerald a good LLM target (paper 3)

Perrier's framing needs a language a model can *aim at* and be corrected by.
Emerald's structured JSON diagnostics are already a strong start; finish the job:

- **Typed holes.** `_` as an expression reports the expected type and the local
  context. This is the single most useful affordance for machine-generated code:
  the model writes a skeleton, the compiler tells it exactly what goes in each
  gap, iterate. Same mechanism serves human editing via LSP.
- **Repair-oriented diagnostics:** every error carries a machine-usable
  suggestion field and a stable code (you have codes already), plus a
  `--explain E_TYPE_RETURN` mode.
- **Incremental / server mode** so a check is milliseconds, not a process spawn
  — the loop is the product.
- **Exportable proof terms**: `--emit-proof` producing a compact certificate of
  *why* a program checked, so a verified CoT trace is a portable artifact rather
  than "trust my compiler".
- **A trace-to-program schema:** define the JSON shape of a CoT step (claim,
  justification, dependencies) and a translator into Emerald declarations, so
  the paper's informal→formal mapping has a concrete implementation.
- **Benchmark it.** Take a CoT dataset, translate, measure what fraction becomes
  well-typed, and correlate typedness with answer correctness. That is a
  complete paper and it needs almost nothing beyond holes + fast checking.

---

## 12. Tooling that stops being optional

LSP server (holes, hovers showing inferred shapes, go-to-def) · REPL ·
Jupyter kernel (research is interactive; a compile-run-only workflow will lose
to Python regardless of type-system quality) · package manager over the existing
`-I` contract · `--emit-diagram` for string diagrams · a profiler ·
`emeraldc fmt` · a debugger story better than `print`.

Of these, the **notebook kernel and LSP** are the ones that decide whether you
personally keep using the language six months from now.

---

## 13. Suggested ordering

**Phase 2 — make it a numeric language.**
Tensors in the runtime, shape types with the canonical-form solver, named dims,
`Fin[n]` indexing, dtypes, BLAS matmul, seeded RNG. Ship a hand-written MLP
trained on a toy task, end-to-end, in Emerald.
*Exit criterion: a shape bug is a compile error with both shapes printed.*

**Phase 3 — make it honest.**
Effects/purity, termination checking, `--proof` mode with `any`-taint, sound
`seq[T]`, induction principles. Re-audit `docs/proofs.md` claims against the
stricter checker.
*Exit criterion: "`--check --proof` passed" is a statement you'd defend.*

**Phase 4 — make it differentiable and connected.**
Autodiff as a typed transformation over the monoidal IR, safetensors loading
with schema checking, ONNX/StableHLO importer. Reproduce a real small model
(GPT-2 small) and match reference logits.
*Exit criterion: you can run a real model you didn't train.*

**Phase 5 — make it an interpretability language.**
Typed hooks, patching/ablation, activation caches, `circuit`/`interpretation`
declarations, approximate judgments, description-length metric, certificates.
*Exit criterion: an induction-head interpretation stated as an Emerald
declaration, checked, with a certificate and an MDL number.*

**Phase 6 — the LLM loop.**
Holes, incremental server, proof export, CoT translation + benchmark.

Metatheory (§10) and tooling (§12) run continuously alongside, not as a phase.

## 14. What to deliberately *not* build

- A GPU backend by hand. Emit an existing IR or stay CPU-only until the language
  ideas are proven.
- Full dependent types. Shapes + refinements + induction covers the research
  program at a fraction of the cost and keeps error messages readable. If you
  find yourself writing an elaborator with metavariables and universe levels,
  the project has changed into a proof assistant.
- Exceptions with tracebacks, a large stdlib, async, an object system. They are
  real gaps for a general-purpose language and irrelevant to this thesis.
- Self-hosting (currently listed as a Phase-2 candidate). It is a fun milestone
  that buys the research nothing and costs months; defer indefinitely.
- Competing with PyTorch on training throughput. The claim is *checkability*,
  not speed.

## 15. Three papers Emerald could produce

1. **Gradual shape and effect typing for neural programs**, with a blame
   calculus and a soundness result, evaluated by how many real shape bugs are
   caught statically in ported model code.
2. **Compositional interpretations as checked refinements** — an implementation
   of the Gauderis et al. framework where faithfulness and description length
   are compiler-computed, plus a case study (induction heads, or the 2-SAT
   transformer from Palumbo et al., which gives you a direct comparison point).
3. **Typed chain-of-thought at scale** — translate CoT traces into Emerald,
   measure typedness vs. correctness, and show typed holes + repair diagnostics
   raise the conversion rate.

Each is reachable from a phase above, and each strengthens the language for the
next.

## Status — what Phase 3's first slice has already landed

The honest gap list in §1 is being worked from the bottom of the proof
fragment up, because every later track assumes it. Implemented so far
(checker-only; no runtime changes; `task test` covers all of it):

- **`pure` functions** (§3, Track B, first step): `def f(...) -> T pure` may
  only call pure functions and the pure builtins; impure nested `def`s inside
  a pure function are rejected. Purity is enforced on calls, not on global
  state or function values — a promise about what a function invokes, not a
  full effect-row system.
- **Termination checking by structural descent + `partial`** (§6, Track E):
  functions are total by default; every recursive call must descend through a
  recursive alias's own fields (`n.succ`, `xs.tail`). `partial` opts out and
  is required for int-counter recursion, mutual recursion, and list-mediated
  descent. This makes `never` honest *within* the checked fragment.
- **`--proof` mode** (§6, Track E): bans `any` and `partial`. A clean
  `emeraldc --check --proof` run means every value has a static type and
  every function terminates structurally — the exit criterion of Phase 3 is
  now meaningful for the structural-recursion fragment.
- **The functional core** (Track F groundwork; the second slice): `const`
  immutable bindings, lambdas `(x: int) => body` with contextual typing,
  `map`/`filter`/`reduce` typed with fresh variables per call site, `|>`
  pipe and `>>` compose, exhaustive `match` with record/literal/bind
  patterns, and **tail-call optimization** for direct self-recursion (`return
  f(...)` compiles to a jump — 10M-deep recursion runs in constant stack).
  Codegen lowers lambdas through the existing closure machinery (captures
  share a cell, so closures can carry mutable state); match lowers to
  guarded if-chains. This is what makes the functional-programming examples
  in §7 writable.

What is deliberately still open, in each case a strict first cut:

- `any` hidden *inside* a type (`list[any]` from `[]`) is not tainted
  through yet — no `W_VACUOUS_PROOF` warnings, no `any`-taint propagation.
- `while True` divergence and mutual recursion still escape termination
  checking outside proof mode.
- Purity is not tracked on function types, so a pure function may call a
  function *value* without the call being checked.
- `seq[T]` (sound immutable sequences), induction principles, and the full
  effect-row system (`!{Rand, Mut, IO}`) remain future work.
- No auto-currying or partial application: `xs |> map(f)` is not (yet)
  syntax — write `xs |> (ys) => map(f, ys)` or `map(f, xs)`. Lambda bodies
  are single expressions (multi-statement bodies need a nested `def`).
- Mutual recursion is not tail-call-optimized (it needs a trampoline), and
  `match` cannot yet guard on list shapes (`[]` / `x :: xs`); list
  destruction is still `if len(xs) == 0` plus indexing.

See [`proofs.md`](proofs.md) ("Totality, purity, and proof mode") and
[`type-system.md`](type-system.md) for the language-level account.

## References

- Gauderis, Dooms, Homer, Ayonrinde, Wiggins. *From Mechanistic to Compositional
  Interpretability.* arXiv:2605.08934
- Palumbo, Mangal, Wang, Vijayakumar, Pasareanu, Jha. *Validating Mechanistic
  Interpretations: An Axiomatic Approach.* arXiv:2407.13594 (ICML 2025)
- Perrier. *Typed Chain-of-Thought: A Curry–Howard Framework for Verifying LLM
  Reasoning.* arXiv:2510.01069
- Paszke et al. *Getting to the Point: Index Sets and Parallelism-Preserving
  Autodiff for Pointful Array Programming* (Dex)
- Elliott. *The Simple Essence of Automatic Differentiation*
- Siek & Taha. *Gradual Typing for Functional Languages*; Wadler & Findler.
  *Well-Typed Programs Can't Be Blamed* (blame calculus)
- Coecke & Kissinger. *Picturing Quantum Processes* (string diagrams / SMCs)
- Nanda et al., *Progress Measures for Grokking*; Olsson et al., *In-context
  Learning and Induction Heads* (case-study targets)
