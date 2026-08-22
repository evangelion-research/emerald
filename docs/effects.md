# Effects

Emerald's purity story is a *closed* effect system: effects
live on function **types**, not just on call sites, so a `pure` function cannot
smuggle an impure callee through a higher-order boundary.

## The model

Internally every function type carries an **effect mask** (`EffMask`), a join
of a fixed label set. The labels defined in the compiler are `IO`, `Rand`,
`Mut`, `Alloc`, and `NonDet`; `pure` is the **empty mask**.

- A function's mask is the join of the masks of everything it calls: named
  functions, builtins, and (crucially) function *values* it invokes or passes
  to `map`/`filter`/`reduce`.
- A lambda's effect is the effect of its **body** — creating a closure is pure,
  so `map(xs, (x) => print_and_return(x))` inside a `pure` function is caught
  at the call site.

## What `pure` promises

`def f(x: T) -> U pure` promises the function has no observable effect: no
`print`, no `rand`, no file/process IO, no impure helper (a nested `def` inside
a pure function must itself be pure). This is enforced:

- at **call sites** — a pure function may not call an impure named function or
  builtin, and
- on the **function type** — `(T) -> U` with an empty mask is not assignable
  where an effectful arrow is allowed to *flow back into* a pure context, so a
  function *value* cannot carry impurity past the check.

That second enforcement is the point: without it, `map(xs, print)` typechecks
inside a pure function and every purity claim in the tree is conditional on
nobody using a higher-order builtin.

## What `pure` does **not** promise

- It does not promise the function terminates (that is the totality system,
  `docs/proofs.md`), or that it is deterministic beyond the effect labels.
- `Alloc` and `NonDet` are tracked labels. The current surface exposes only
  the `pure` declaration, which requires the empty effect mask.

## Scope

The surface syntax is `pure`. Pure functions have the empty effect mask;
other functions are effectful. Higher-order purity is checked and covered by
`tests/check/{bad,good}_pure_higher_order.rald`.
