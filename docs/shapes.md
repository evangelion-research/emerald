# Shapes

Phase 2's thesis obligation is a shape bug becoming a **compile error with both
shapes printed**. This page documents the type-level half: dimension names, the
shape solver, the per-operation typing rules, and the boundary where checking
goes dynamic.

The runtime half is in [`tensors.md`](tensors.md).

## Surface syntax

### Nominal dimensions

```emerald
dim Batch, Seq, DModel
```

`dim` introduces a **nominally distinct** dimension name. `Batch` and `Seq`
both being sizes does not make them interchangeable — transposition bugs are
the most common silent bug in model code, and nominal dims are what catch them.

### Tensor types

```emerald
x: Tensor[f32, [Batch, DModel]]
y: Tensor[f64, ?]                # the dynamic escape hatch
```

The shape is a list of **dimension expressions** over dim names and integer
literals, built from `+` and `*` (`B * S`, `D + 1`). `?` is a dynamic shape:
the value exists but its shape is not statically known.

### Kinded type parameters

Both generic functions and generic aliases route through one parameter parser,
so a parameter may be kinded `name: dim`:

```emerald
def split_heads[B: dim, S: dim, D: dim](x: Tensor[f32, [B, S, D]]) -> Tensor[f32, [B, S * D, D + 1]] { ... }
type Vec[N: dim] = Tensor[f32, [N]]
```

`Tensor` is a built-in type constructor, not a generic alias (D2): shape
arithmetic is a *typing rule*, not an instantiation. User aliases like `Vec[N]`
still work on top.

## The solver: a canonical-form normalizer, not SMT

Per D3, each dimension expression is normalized to a **sum-of-products** over
dim variables and integer literals, then compared syntactically. There is
deliberately no SMT dependency in this phase.

- `dim_eq(a, b)` — normalize both, compare term-for-term. Always decidable.
- `dim_le(a, b)` — the decidable fragment needed by `Fin[n]` (W5): equal normal
  forms, literal comparison, and `a <= a + k` for literal `k >= 0`.

Anything `dim_le` cannot decide is **logged** (the escalation log, dumped by
`dim_log_dump`) rather than guessed. That dataset is what decides the D3
escalation question in Phase 3, from data rather than intuition. The solver is a
standalone component (`src/dim.c`, `include/dim.h`) unit-tested directly by
`tests/shape/dim_unit.c` — the first non-golden test in the project.

## Typing rules

Each shape-carrying operation generates an obligation, verified statically:

| Op | Obligation | Result |
|---|---|---|
| `matmul(a: [M,K], b: [K2,N])` | `K ≡ K2` | `[M,N]` |
| elementwise `a ⊕ b` | broadcastable (equal rank; each axis equal or 1) | broadcast shape |
| `reshape(t, s)` | `prod(shape(t)) ≡ prod(s)` | `s` |
| `transpose` / `permute` | permutation is a bijection on axes | reordered shape |
| reductions (`sum`, `mean`, `max`, `argmax`) | axis literal in range | shape minus that axis |
| `tslice(t, axis, lo, hi)` | axis literal in range | axis replaced by `hi - lo` |

Broadcasting takes the strict cut from the risk table: only broadcast when it
is statically decidable, otherwise require an explicit `expand`. The result is
unsurprising and the escape hatch (`expand`) is always available.

### The exit-criterion diagnostic

```emerald
dim Batch, DModel, DHidden, DOut

def layer(x: Tensor[f32, [Batch, DModel]], w2: Tensor[f32, [DHidden, DOut]]) -> Tensor[f32, [Batch, DOut]] {
    h = matmul(x, w2)     # the weight's axes are transposed
    return h
}
```

```
error[E_SHAPE_MATMUL]: contracted dimensions do not match
  --> shape_bug.rald:4:15
    | ...
   = left:     Tensor[f32, [Batch, DModel]]
   = right:    Tensor[f32, [DHidden, DOut]]
   = mismatch: DModel != DHidden  (contraction axis)
```

Both shapes printed, the mismatching axis named. Under `--json`, `left`,
`right`, and `mismatch` are separate structured fields, so the LLM-repair loop
gets structure rather than prose. See [`examples/mlp/shape_bug.rald`](../examples/mlp/shape_bug.rald)
and `tests/check/bad_shape_matmul.rald`.

## The gradual boundary, measured

`Tensor[f32, ?]` exists from day one, because no real program can be ported
without it. A dynamic tensor bound to a statically-shaped annotation keeps the
annotation's shape on later reads: the compiler trusts the assertion and checks
the code *after* the boundary statically.

Every static↔dynamic crossing is **counted**, and the count is reportable:

```sh
emeraldc --shape-report --check model.rald
# shape-crossings: 12
```

That counter is not bookkeeping: "how many obligations were discharged
statically vs dynamically in ported model code" is the empirical result of the
gradual-shape-typing paper (§15.1 of
[`research-directions.md`](research-directions.md)).

## `Fin[n]` index safety

`Fin[n]` is an index provably below `n`. `xs[i]` with `i: Fin[len(xs)]` is
statically safe; an index *provably out of range* is a compile error:

```emerald
dim N
def bad(xs: Tensor[f32, [N]], i: Fin[N + 1]) -> float {
    return item(xs[i])     # error[E_SHAPE_INDEX]: Fin[1 + N] vs size N
}
```

`Fin[a]` is a subtype of `Fin[b]` exactly when `a <= b` in the decidable
fragment, and of `int`. Honest scoping: this phase **rejects provably-bad
indexing**; the runtime bounds check remains, because bounds-check *elimination*
needs typed codegen (Phase 4).
