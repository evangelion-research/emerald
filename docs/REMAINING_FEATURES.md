# Remaining Features for Proof-Oriented Functional Programming and Autograd

Status reviewed: 2026-09-17, against the implementation and tests in this
repository.

## Executive summary

Emerald has a credible foundation: structural and generic types, tagged unions
and exhaustive matching, first-class functions and closures, purity/effect
tracking, a proof mode, structural termination checks, immutable `seq[T]`,
typed errors, statically shaped tensors, a dimension normalizer, native code
generation, a garbage-collected tensor runtime — and, since the autograd phase,
a working **reverse-mode autograd engine** (`value_and_grad`, a GC-integrated
tape, VJP rules for every differentiable primitive, and end-to-end training
tests).

It is therefore ready for **small typed functional programs, an experimental
tensor library, and first-generation training loops**. It is not yet a sound
general proof language: the known gradual, partial, and forgeable-invariant
escapes in proof mode are still open, and the functional fragment still lacks
recursive data types and constrained polymorphism.

The shortest responsible path is now:

1. close the soundness holes in `--proof` and define a small trusted kernel;
2. make the total, immutable functional fragment expressive enough to write
   real libraries;
3. after that, the ML layer that a correct autograd core makes useful.

“Fully feature complete” should not mean supporting every feature of a mature
language. For this project it should mean satisfying the concrete exit criteria
at the end of this document.

## What the repository already provides

| Area | Present evidence | Assessment |
|---|---|---|
| Functional core | Lambdas, closures, higher-order functions, composition, pipelines, `map`/`filter`/`reduce`, immutable `const`, exhaustive `match` | Good experimental base |
| Static types | Structural records, unions/intersections, literals, generics, narrowing, `never`, `Result`-shaped errors | Broad, but gradual and not a proof calculus |
| Proof checks | `--proof`, rejection of `any`/`partial`, exhaustiveness, purity, structural recursion, monotone-counter `while`, `Eq[a,b]`, `Fin[n]`, `--proof-report` | Useful verification mode, not yet sound enough to call a theorem prover |
| Effects | Function effect masks for IO, randomness, mutation, allocation, and nondeterminism; higher-order purity checking | Good base; surface exposes only `pure` |
| Tensors | `f32`/`f64`, static/dynamic shapes, broadcasting, views, reductions, matmul, elementwise nonlinearities | Complete forward surface |
| Autograd | `value_and_grad`, scoped reverse-mode tape, VJPs for the whole differentiable surface, broadcast-adjoint reduction, static adjoint shapes, purity requirement, finite-difference and end-to-end training tests ([`autograd.md`](autograd.md)) | Working MVP for `f32`/`f64` scalar losses |
| Shape reasoning | Nominal dimensions, polynomial normalization, `Eq`, `Fin`, diagnostics and `--shape-report`, static shapes enforced through autograd in proof mode | Valuable but intentionally much weaker than dependent arithmetic |
| Runtime/tooling | Native C backend, precise generational GC, modules, diagnostics, REPL, tree-sitter grammar, LSP, package manager, and broad golden/e2e tests | Suitable for experimentation |

The relevant implementation is concentrated in `src/check_*.c`, `src/dim.c`,
`src/runtime_tensor.c`, `src/runtime_autograd.c`, `include/builtins.def`, and
the proof/tensor/shape/autograd tests.

## P0: proof soundness work that must come first

### 1. Define the trusted computing base and proof guarantee

`docs/proofs.md` correctly calls the checker “a proof checker for a small
logic,” but the guarantee is informal and conditional on the compiler and
runtime. Create a written proof-mode contract specifying:

- which source constructs are in the logical fragment;
- which compiler/runtime functions are trusted axioms;
- whether a successful check proves type safety, termination, purity, absence
  of runtime failure, or only a subset of these;
- whether integer and floating-point operations use mathematical or machine
  semantics (`int` is documented as wrapping 64-bit; proofs do not model it);
- what tensor propositions mean after types and shapes are erased by codegen.

A small proof IR or independently checkable proof certificate is preferable in
the long term. At minimum, isolate proof checking from ordinary gradual typing
and test every trusted rule with positive and negative cases.

### 2. Make totality include absence of runtime aborts

The totality checker proves return-path coverage and a limited class of
termination. That is not totality if an expression can still abort. Proof mode
must reject or require evidence for at least:

- division, modulo, and floor division by zero;
- integer overflow (fixed-width C semantics are kept);
- invalid indexing and slicing, including lists and dynamic tensors;
- `item` on a non-scalar tensor;
- invalid reshape, permutation, axis, dtype, and ragged tensor construction;
- domain errors such as `sqrt(x)` for negative `x` and `log(x)` for
  nonpositive `x` (`sqrt`/`log` are pure builtins, so this matters inside
  differentiable and proof code alike);
- allocation failure and runtime `rt_fatal` paths;
- explicitly aborting process/file builtins.

Use checked `Result` returns, refined input types, or explicit trusted
preconditions. `Fin[n]` is a start, but it currently covers only a narrow
indexing case and retains runtime checks.

### 3. Replace termination heuristics with a principled total fragment

Structural descent accepts selected projection chains; mutual recursive cycles
are rejected outright; loop checking recognizes the monotone-counter pattern.
This is useful but both incomplete and hard to extend safely. Needed:

- recursive algebraic data types with a positivity/strict-positivity check;
- structural recursion over all immutable inductive values;
- lexicographic and well-founded measures for common numeric algorithms;
- sound handling of higher-order recursion and recursion through closures;
- a clear policy for mutual recursion rather than blanket SCC rejection;
- termination checking for library combinators so users can avoid imperative
  loops in proof code;
- diagnostics that display the failed decrease obligation.

Until this exists, proof mode should conservatively reject constructs it
cannot prove total. (It already does the conservative thing for mutual
recursion and non-counter `while` loops — the standard library's non-trivial
loops are exactly what keeps it out of proof mode.)

### 4. Eliminate every gradual escape from proof mode

Proof mode already rejects `any` at annotations, signatures, and inferred
types, and rejects the unannotated empty-list literal that would infer it
(`bad_any_*`, `bad_empty_list`). Complete the audit for all remaining ways
dynamic knowledge can enter:

- unconstrained generic inference still falls back to `any`;
- dynamic dictionaries/sets and dynamic tensor shapes can erase obligations;
- foreign/runtime builtins expose values whose invariants are unchecked;
- casts or annotation boundaries must not merely “trust” a dynamic value in
  proof mode.

Proof mode should use an inference error, existential package, or explicit
checked conversion instead of an `any` fallback. `Tensor[dtype, ?]` is already
forbidden through `value_and_grad` in proof mode (`E_PROOF_SHAPE`); the same
standard should hold wherever a dynamic value could discharge an obligation.

### 5. Strengthen equality and propositions

`Eq[a, b]` concerns dimension expressions and is introduced by `refl` with
dimension-level elimination across function boundaries; it is not general
propositional equality. Add, in stages:

- equality for ordinary values/types with substitution/elimination;
- decidable refinement predicates for integers and tensor axes;
- existential types for values discovered at runtime, especially shapes;
- opaque/abstract types so invariants cannot be forged with structural record
  literals;
- proof-carrying constructors (smart constructors) for refined values;
- optional theorem/lemma syntax and erased proof arguments for clarity.

Opaque types are urgent: the typed ray tracer already documents that its
“unit-vector” brand is forgeable (see
[`examples/ray_tracer/typed/README.md`](../examples/ray_tracer/typed/README.md)
and the root README). The same issue would let users forge model, shape, or
differentiability invariants represented only by structural fields.

### 6. Separate computational effects from proof admissibility

The internal effect mask (`IO`, `Rand`, `Mut`, `Alloc`, `NonDet`) is richer
than the single `pure` surface modifier, as `docs/effects.md` records. Expose
or otherwise formalize effect-polymorphic function types. In particular:

- distinguish local allocation from observable mutation;
- distinguish mutation of a fresh local builder from mutation visible to a
  caller;
- track partiality/runtime failure separately from IO;
- make randomness explicit through a seed/state value (tensor `randn` is
  already seeded; general `rand` is not);
- define whether tensor views and the autograd tape are pure — the tape
  already behaves purely (state is scoped inside `value_and_grad`), but the
  type story is still "the builtin is `pure`";
- support effect variables for reusable higher-order functions.

## P1: functional-language gaps

### 7. Add sound, ergonomic immutable data structures

`seq[T]` supplies the sound immutable boundary, while `list[T]` is mutable and
ordinary mode permits covariance with a warning. A proof-oriented functional
library needs persistent sequences/maps/sets, or linear/uniqueness types that
make mutation safe. The stdlib's pure `lists` helpers (`concat`, `copy`,
`take`, `drop`, `reverse`, the searching set) are a start; required beyond
them are folds, scans, indexed maps, and efficient builders whose mutation
remains encapsulated.

Proof-mode standard library code should itself pass `--proof`. Today only the
leaf module `chars` does (there is a regression test pinning that), and modules
such as `fmt` deliberately use `list[any]`.

### 8. Complete algebraic data types and pattern matching

Tagged structural unions and exhaustive `match` work, but the language needs
first-class recursive sum/product declarations with:

- safe constructors and opaque representation;
- recursive generic types (currently rejected with `E_TYPE_RECURSIVE_GENERIC`);
- nested tuple/list/constructor patterns;
- guards and precise exhaustiveness/redundancy checking;
- destructuring in bindings and function parameters.

This will simplify proof terms, optimizer state, and neural-network module
definitions.

### 9. Improve polymorphism and abstraction

Current generics are call-site inferred and unconstrained variables fall back
to `any`. Mature functional code needs:

- explicit type application when inference is ambiguous;
- constraints/traits for numeric and differentiable operations;
- higher-kinded abstraction or a deliberately smaller alternative for
  functor/fold/traverse-style libraries;
- generic recursive types;
- principled generalization/value restriction in the presence of mutation;
- module signatures, opaque exports, and separate interface checking.

For autograd, a `Scalar`/`Differentiable`-style constraint would let generic
numerical code reject strings and nondifferentiable operands statically; today
the purity requirement on `value_and_grad` plus dtype checking is the only
guard.

### 10. Clarify function-value semantics

Functions and closures exist, including indirect calls, capturing by shared
mutable cell, and documented assignability rules. Support and test arbitrary
callable expressions, generic function values, partial application,
effect-polymorphic callbacks, and recursive closures. Define equality,
printing, serialization, and capture/mutation restrictions for function values
rather than letting runtime behavior become the specification.

## P1: next steps for autograd

The MVP is done (see [`autograd.md`](autograd.md)): functional
`value_and_grad` API, scoped reverse-mode tape with GC integration, VJPs for
elementwise arithmetic, unary nonlinearities, reductions, matmul (all four
layouts), reshape, transpose, permute, slice, expand, and cast, static adjoint
shapes, and finite-difference checks for every rule. What remains:

### 11. Widen the differentiable surface

- batched matmul and a faster matrix kernel/BLAS option;
- numerically stable primitives (`sigmoid`, `softmax`, `logsumexp`,
  cross entropy, normalization) — these are where composition of the existing
  rules is measurably less stable than a fused rule;
- keep-dimension reductions or a typed reshape/expand sequence for them.

### 12. Engineering hardening of the tape

- detach/no-grad scopes for evaluation and optimizer updates;
- explicit zeroing/accumulation control for parameter trees;
- a documented version-check policy if in-place mutation ever meets the tape
  (saved views already reference shared buffers);
- tape memory accounting surfaced in `gc_stats` and a GC stress test over a
  large recorded graph.

### 13. Carry differentiability through the type system more precisely

The checker already requires purity, matching dtypes, matching static shapes,
and a scalar loss. Still missing:

- a way to state that a function is differentiable *with respect to selected
  inputs* (with several parameters, packing into one leaf is the workaround —
  the training test does exactly that);
- rejecting differentiation *through* nondeterminism or visible mutation of
  captured cells, beyond the purity requirement;
- static rejection of unsupported primitives inside a differentiable function
  (today they silently cut the gradient per the documented policy).

## P2: useful ML layer after the core

Once the surface above is wider, add:

- parameter trees and pure tree-map/tree-fold utilities;
- SGD first, then momentum and Adam, with typed optimizer state;
- deterministic parameter initialization with explicit RNG state;
- model serialization and dtype/shape validation on load;
- a small `nn` library (`linear`, activations, losses), without requiring
  classes;
- profiling for forward time, backward time, allocations, and tape memory.

GPU support, distributed execution, JIT fusion, higher-order gradients, sparse
tensors, convolution, and mixed precision are not prerequisites for a credible
autograd release.

## Testing and validation

The existing golden and end-to-end suites already cover the two most important
classes: **finite-difference checks for every VJP rule** (in `f64`, including
broadcasting, views, repeated parents, and reduction axes) and **deterministic
end-to-end training** (linear regression and a two-layer MLP). Add:

1. **Proof soundness adversarial tests:** attempts to inhabit `never`, smuggle
   `any`, forge abstract invariants, abort inside total code, mutate through an
   alias, or evade termination through a closure/higher-order call.
2. **Property tests for the type/shape solver:** normalization idempotence,
   symmetry/transitivity, substitution, and randomly generated equivalent and
   inequivalent dimension expressions (the solver has a unit harness in
   `tests/shape/dim_unit.c` to build on).
3. **Graph tests:** GC stress over a large recorded tape, tape collection
   timing, and gradient shapes crossing dynamic boundaries.
4. **Nondifferentiability tests:** zero for ReLU, max ties, and discrete ops
   must match the written policy.
5. **Cross-check tests:** compare selected forward values and gradients
   against a trusted numerical reference, keeping reference code outside the
   language's trusted proof story.

## Recommended implementation sequence

### Milestone A — honest proof mode

- publish the trusted-core and machine-number semantics;
- forbid unchecked dynamic/`any` boundaries;
- model runtime failure and partial builtins;
- add opaque types and proof-safe constructors;
- strengthen totality and grow the proof-safe standard-library subset beyond
  `chars`.

Exit: a successful proof check has a precise documented meaning and cannot be
defeated by known abort, dynamic typing, mutation, or invariant-forging paths.

### Milestone B — functional proof library

- recursive algebraic data types and full structural recursion;
- persistent collections and proof-clean folds/traversals;
- constrained generics and explicit type application;
- module interfaces and abstract exports.

Exit: representative proofs and numerical algorithms can be written without
`partial`, unchecked mutation, or builtin-only control flow.

### Milestone C — autograd breadth

- stable fused losses and batched matmul;
- detach/no-grad, tape memory accounting, and GC stress tests;
- differentiability-with-respect-to-selected-inputs in the type system.

Exit: the ML layer below can be built without extending the runtime tape for
every new primitive.

### Milestone D — train a model comfortably

- stable losses, explicit RNG, parameter trees, SGD/Adam, serialization;
- performance work sufficient for small models;
- deterministic linear-regression and MLP examples that no longer need
  packed-parameter workarounds.

Exit: Emerald can define, differentiate, and train a small model end to end
with the ergonomics of a real ML library, without leaving Emerald source code.

## Definition of feature complete for this goal

Emerald is feature complete for a **proof-oriented functional language with a
usable autograd foundation** when all of the following are true:

- `--proof` has a written soundness boundary and rejects every known gradual,
  partial, runtime-aborting, and forgeable-invariant escape within that boundary;
- total functions terminate and cannot fail for admitted inputs, modulo an
  explicitly documented trusted runtime model;
- recursive immutable data, pattern matching, abstraction, and constrained
  polymorphism are sufficient for a proof-clean core library;
- pure higher-order numerical functions can be differentiated only when every
  executed primitive has a valid derivative rule;
- reverse-mode gradients have statically checked dtypes and shapes;
- broadcasting, reductions, views, shared graph nodes, and GC interaction are
  tested, not merely demonstrated;
- finite-difference tests validate each primitive and deterministic end-to-end
  examples train successfully;
- unsupported operations fail at compile time with a diagnostic or cross an
  explicit checked dynamic boundary—never silently lose a proof obligation.

At that point Emerald would be a credible research language for verified
functional numerical programs with first-generation autograd. General theorem
proving, production-scale deep learning, GPUs, distributed training, and
higher-order differentiation should remain separate later milestones.
