# Remaining Features for Proof-Oriented Functional Programming and Autograd

Status reviewed: 2026-08-22, against the implementation and tests in this
repository.

## Executive summary

Emerald already has a credible foundation: structural and generic types,
tagged unions and exhaustive matching, first-class functions and closures,
purity/effect tracking, a proof mode, structural termination checks, immutable
`seq[T]`, typed errors, statically shaped tensors, a dimension normalizer,
native code generation, and a garbage-collected tensor runtime.

It is therefore ready for **small typed functional programs and an experimental
tensor library**, but it is not yet a sound general proof language and it does
not yet have the semantics or runtime machinery needed for autograd.

The shortest responsible path is:

1. close the soundness holes in `--proof` and define a small trusted kernel;
2. make the total, immutable functional fragment expressive enough to write
   real libraries;
3. specify differentiability, tensor mutation/aliasing, and gradient shapes;
4. implement reverse-mode autograd for the existing tensor primitives;
5. add numerical gradient checks, optimizers, and an end-to-end training test.

“Fully feature complete” should not mean supporting every feature of a mature
language. For this project it should mean satisfying the concrete exit criteria
at the end of this document.

## What the repository already provides

| Area | Present evidence | Assessment |
|---|---|---|
| Functional core | Lambdas, closures, higher-order functions, composition, pipelines, `map`/`filter`/`reduce`, immutable `const`, pattern matching | Good experimental base |
| Static types | Structural records, unions/intersections, literals, generics, narrowing, `never`, `Result`-shaped errors | Broad, but gradual and not a proof calculus |
| Proof checks | `--proof`, rejection of `any`/`partial`, exhaustiveness, purity, structural recursion, limited loop termination, `Eq[a,b]`, `Fin[n]` | Useful verification mode, not yet sound enough to call a theorem prover |
| Effects | Function effect masks for IO, randomness, mutation, allocation, and nondeterminism; higher-order purity checking | Good base; surface and semantics need refinement |
| Tensors | `f32`/`f64`, static/dynamic shapes, broadcasting, views, reductions, matmul, elementwise nonlinearities | Enough forward operations for a first autograd engine |
| Shape reasoning | Nominal dimensions, polynomial normalization, `Eq`, `Fin`, diagnostics and crossing reports | Valuable but intentionally much weaker than dependent arithmetic |
| Runtime/tooling | Native C backend, precise generational GC, modules, diagnostics, REPL and broad golden/e2e tests | Suitable for experimentation |

The relevant implementation is concentrated in `src/check_*.c`, `src/dim.c`,
`src/runtime_tensor.c`, `include/builtins.def`, and the proof/tensor/shape tests.
The current tensor builtin list contains forward operators only; there is no
gradient tape, differentiable tensor metadata, backward kernel, optimizer, or
gradient test suite.

## P0: proof soundness work that must come first

### 1. Define the trusted computing base and proof guarantee

Today `docs/proofs.md` correctly calls the checker “a proof checker for a small
logic,” but the guarantee is informal and conditional on the compiler and
runtime. Create a written proof-mode contract specifying:

- which source constructs are in the logical fragment;
- which compiler/runtime functions are trusted axioms;
- whether a successful check proves type safety, termination, purity, absence
  of runtime failure, or only a subset of these;
- whether integer and floating-point operations use mathematical or machine
  semantics;
- what tensor propositions mean after types and shapes are erased by codegen.

A small proof IR or independently checkable proof certificate is preferable in
the long term. At minimum, isolate proof checking from ordinary gradual typing
and test every trusted rule with positive and negative cases.

### 2. Make totality include absence of runtime aborts

The current totality checker proves return-path coverage and a limited class of
termination. That is not totality if an expression can still abort. Proof mode
must reject or require evidence for at least:

- division, modulo, and floor division by zero;
- integer overflow if integers retain fixed-width C semantics;
- invalid indexing and slicing, including lists and dynamic tensors;
- `item` on a non-scalar tensor;
- invalid reshape, permutation, axis, dtype, and ragged tensor construction;
- domain errors such as `sqrt(x)` for negative `x` and `log(x)` for nonpositive
  `x`;
- allocation failure and runtime `rt_fatal` paths;
- explicitly aborting process/file builtins.

Use checked `Result` returns, refined input types, or explicit trusted
preconditions. `Fin[n]` is a start, but it currently covers only a narrow
indexing case and retains runtime checks.

### 3. Replace termination heuristics with a principled total fragment

Current structural descent accepts selected projection chains; mutual recursive
cycles are rejected; loop checking recognizes a few counter patterns. This is
useful but both incomplete and hard to extend safely.

Needed:

- recursive algebraic data types with a positivity/strict-positivity check;
- structural recursion over all immutable inductive values;
- lexicographic and well-founded measures for common numeric algorithms;
- sound handling of higher-order recursion and recursion through closures;
- a clear policy for mutual recursion rather than blanket SCC rejection;
- termination checking for library combinators so users can avoid imperative
  loops in proof code;
- diagnostics that display the failed decrease obligation.

Until this exists, proof mode should conservatively reject constructs it cannot
prove total.

### 4. Eliminate every gradual escape from proof mode

The implementation now detects nested `any` taint and the tests reject empty
list inference in proof mode, which improves on older documentation. Complete
the audit for all ways `any` or dynamic knowledge can enter:

- unconstrained generic inference currently falls back to `any`;
- unannotated lambdas and function boundaries can infer `any`;
- dynamic dictionaries/sets and dynamic tensor shapes can erase obligations;
- foreign/runtime builtins may expose values whose invariants are unchecked;
- casts or annotation boundaries must not merely “trust” a dynamic value in
  proof mode.

Proof mode should use an inference error, existential package, or explicit
checked conversion instead of an `any` fallback. `Tensor[dtype, ?]` should be
forbidden in proofs unless it is refined by a runtime check that returns a
typed witness.

### 5. Strengthen equality and propositions

`Eq[a,b]` currently concerns dimension expressions and is introduced by
`refl`; it is not general propositional equality. Add, in stages:

- equality for ordinary values/types with substitution/elimination;
- decidable refinement predicates for integers and tensor axes;
- existential types for values discovered at runtime, especially shapes;
- opaque/abstract types so invariants cannot be forged with structural record
  literals;
- proof-carrying constructors (smart constructors) for refined values;
- optional theorem/lemma syntax and erased proof arguments for clarity.

Opaque types are urgent: the typed ray tracer already documents that its
“unit-vector” brand is forgeable. The same issue would let users forge model,
shape, or differentiability invariants represented only by structural fields.

### 6. Separate computational effects from proof admissibility

The internal effect mask is richer than the single `pure` surface modifier.
Expose or otherwise formalize effect-polymorphic function types. In particular:

- distinguish local allocation from observable mutation;
- distinguish mutation of a fresh local builder from mutation visible to a
  caller;
- track partiality/runtime failure separately from IO;
- make randomness explicit through a seed/state value;
- define whether tensor views and a future gradient tape are pure;
- support effect variables for reusable higher-order functions.

This is important for autograd: a hidden mutable tape is operationally useful,
but a pure `grad(f)` API needs a semantics showing that tape mutation is local
and cannot escape.

## P1: functional-language gaps

### 7. Add sound, ergonomic immutable data structures

`seq[T]` supplies the sound immutable boundary, while `list[T]` is mutable and
ordinary mode permits covariance with a warning. A proof-oriented functional
library needs persistent sequences/maps/sets, or linear/uniqueness types that
make mutation safe. Required operations include constructors, folds, zips,
scans, indexed maps, traversal, and efficient builders whose mutation remains
encapsulated.

Proof-mode standard library code should itself pass `--proof`. The repository
notes that the whole standard library currently does not, and modules such as
`fmt` deliberately use `list[any]`.

### 8. Complete algebraic data types and pattern matching

Tagged structural unions work, but the language needs first-class recursive
sum/product declarations with:

- safe constructors and opaque representation;
- recursive generic types (currently explicitly unsupported);
- nested tuple/list/constructor patterns;
- guards and precise exhaustiveness/redundancy checking;
- destructuring in bindings and function parameters.

This will simplify proof terms, symbolic differentiation trees, optimizer
state, and neural-network module definitions.

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

For autograd, at least a `Scalar`/`Differentiable`-style constraint is needed so
generic numerical code cannot accidentally accept strings, integers with an
undefined derivative policy, or nondifferentiable functions.

### 10. Clarify function-value semantics

Functions and closures exist, including indirect calls, but the callable
surface and inference remain narrower than a mature functional language.
Support and test arbitrary callable expressions, generic function values,
partial application, effect-polymorphic callbacks, and recursive closures.
Define equality, printing, serialization, and capture/mutation restrictions for
function values rather than letting runtime behavior become the specification.

## P1: minimum viable reverse-mode autograd

### 11. Choose and document the differentiation model

Start with reverse-mode automatic differentiation over floating tensors. A
reasonable public API is:

```rald
def value_and_grad[B: dim, D: dim](
    f: (Tensor[f32, [B, D]]) -> Tensor[f32, []] pure,
    x: Tensor[f32, [B, D]]
) -> { value: Tensor[f32, []], grad: Tensor[f32, [B, D]] }
```

Decide explicitly:

- eager tape versus source transformation;
- `grad(f)`/`value_and_grad(f)` versus mutable `requires_grad` tensors;
- treatment of control flow and closures;
- gradient dtype and mixed-precision policy;
- whether higher-order gradients are required initially (recommended: no);
- behavior at nondifferentiable points (`relu(0)`, `max` ties);
- whether integer, boolean, indexing, random, IO, and mutation operations stop
  gradients or are compile errors.

For Emerald’s proof/purity goals, a functional `value_and_grad` API backed by a
locally scoped tape is a cleaner first surface than globally mutable `.grad`
fields.

### 12. Add graph/tape runtime objects and GC tracing

The runtime needs nodes containing the forward value, parent edges, backward
rule, saved tensors/metadata, and accumulated adjoint. These objects and all
edges must participate in the precise generational GC and write barrier.

Required engineering includes:

- tape lifetime and deterministic release after backward;
- topological traversal and cycle defense;
- gradient accumulation for shared subexpressions;
- zeroing/detaching gradients;
- safe saved views and version checks if in-place mutation remains possible;
- no-grad/detach scopes for evaluation and optimizer updates;
- memory accounting in `gc_stats` and stress tests.

### 13. Implement and verify VJP rules for the existing tensor surface

Implement vector-Jacobian products first; do not construct full Jacobians.
Minimum rules:

| Operation | Backward requirement |
|---|---|
| `+`, `-`, `*`, `/` | local derivatives plus reduction over broadcast axes |
| `exp`, `log`, `tanh`, `relu` | saved input/output and documented boundary behavior |
| `matmul` | transposed matmuls and batch/broadcast policy |
| `reshape` | reshape adjoint to input shape |
| `transpose`, `permute` | apply inverse permutation |
| `sum`, `mean` | restore reduced axis and expand; divide for mean |
| `max` | tie/subgradient policy and mask |
| `tslice` | scatter-add into an input-shaped zero tensor |
| `expand` | sum over expanded axes |
| `astype` | cast gradient under an explicit precision policy |
| `tensor`, `zeros`, `ones`, `full`, `arange`, `randn` | leaf/constant policy |
| `argmax`, `shape`, `ndim`, `dtype`, `item` | nondifferentiable or boundary semantics |

Broadcast-gradient reduction is a particularly important missing primitive.
The runtime also needs internal zeros-like, ones-like, add-in-place or
functional accumulation, inverse permutation, and scatter-add kernels.

### 14. Carry differentiability through the type/effect system

Shape preservation alone is insufficient. Add a way to express that a
function is differentiable with respect to selected floating inputs and
returns a scalar (for `grad`) or accepts a cotangent (for general VJP).

The checker should reject differentiation through:

- IO, nondeterminism, task/channel effects, and visible mutation;
- unsupported builtins or a missing derivative rule;
- integer/discrete results when no relaxation is declared;
- dynamic-shape changes that invalidate the adjoint type;
- tensors with unsupported dtype.

Avoid claiming mathematical differentiability solely from an effect bit. A
practical first contract is “all executed primitive operations have registered
VJP rules and the function is pure,” with nondifferentiable points documented.

### 15. Make shape proofs work in both forward and backward passes

Every VJP must have a statically checkable adjoint shape. Extend the shape
system for:

- broadcast-axis inference and “unbroadcast” reductions;
- batch matmul if supported;
- keep-dimension reductions or an equivalent typed reshape/expand sequence;
- scatter shapes for slices;
- existential/runtime shapes crossing into autograd;
- equality evidence transport through transpose, reshape, and VJP rules.

The current polynomial normalizer is enough for many reshape products, but not
for data-dependent dimensions, inequalities, modulo/divisibility, or general
broadcast disjunctions. Keep a conservative static fragment and require
checked existential witnesses at the dynamic boundary rather than silently
trusting annotations.

## P2: useful ML layer after core autograd

Once reverse mode is correct, add:

- numerically stable `sigmoid`, `softmax`, `logsumexp`, cross entropy, variance,
  and normalization primitives;
- batched matmul and a faster matrix kernel/BLAS option;
- parameter trees and pure tree-map/tree-fold utilities;
- SGD first, then momentum and Adam, with typed optimizer state;
- deterministic parameter initialization with explicit RNG state;
- model serialization and dtype/shape validation on load;
- a small `nn` library (`linear`, activations, losses), without requiring
  classes;
- profiling for forward time, backward time, allocations, and tape memory.

GPU support, distributed execution, JIT fusion, higher-order gradients, sparse
tensors, convolution, and mixed precision are not prerequisites for the first
credible autograd release.

## Testing and validation still required

The existing golden and end-to-end suites are a strong base. Add the following
test classes:

1. **Proof soundness adversarial tests:** attempts to inhabit `never`, smuggle
   `any`, forge abstract invariants, abort inside total code, mutate through an
   alias, or evade termination through a closure/higher-order call.
2. **Property tests for the type/shape solver:** normalization idempotence,
   symmetry/transitivity, substitution, and randomly generated equivalent and
   inequivalent dimension expressions.
3. **Primitive gradient checks:** compare every VJP with central finite
   differences in `f64`, including broadcasting, views, repeated parents, and
   reduction axes.
4. **Graph tests:** diamond graphs, unused branches, repeated backward policy,
   detach/no-grad, saved-value mutation, and tape collection under GC stress.
5. **Nondifferentiability tests:** zero for ReLU, max ties, discrete ops, and
   domain errors must match the written policy.
6. **End-to-end training:** fit a tiny linear regression and a two-layer MLP;
   assert deterministic loss decrease and learned values/shapes.
7. **Cross-check tests:** compare selected forward values and gradients against
   a trusted numerical reference, while keeping reference code outside the
   language’s trusted proof story.

## Recommended implementation sequence

### Milestone A — honest proof mode

- publish the trusted-core and machine-number semantics;
- forbid unchecked dynamic/`any` boundaries;
- model runtime failure and partial builtins;
- add opaque types and proof-safe constructors;
- strengthen totality and make a proof-safe standard-library subset pass.

Exit: a successful proof check has a precise documented meaning and cannot be
defeated by known abort, dynamic typing, mutation, or invariant-forging paths.

### Milestone B — functional proof library

- recursive algebraic data types and full structural recursion;
- persistent collections and proof-clean folds/traversals;
- constrained generics and explicit type application;
- module interfaces and abstract exports.

Exit: representative proofs and numerical algorithms can be written without
`partial`, unchecked mutation, or builtin-only control flow.

### Milestone C — autograd MVP

- functional `value_and_grad` API and scoped reverse-mode tape;
- VJPs for elementwise arithmetic, unary nonlinearities, reductions, reshape,
  transpose, expand, slice, and matmul;
- typed adjoint shapes, detach/no-grad, and GC integration;
- finite-difference checks for every primitive.

Exit: a pure scalar-loss function over `f32`/`f64` tensors returns gradients of
the same static shapes, and all primitive gradient checks pass.

### Milestone D — train a model

- stable losses, explicit RNG, parameter trees, SGD/Adam, serialization;
- performance work sufficient for small models;
- deterministic linear-regression and MLP examples.

Exit: Emerald can define, differentiate, and train a small model end to end
without leaving Emerald source code.

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
