# Writing Proofs in Emerald

Emerald's checker is a proof checker for a small logic. This document explains
which propositions you can state, how to prove them, and — just as important —
where the system stops.

Run [`examples/proofs.rald`](../examples/proofs.rald) for a working tour.

## The correspondence

Under Curry–Howard, types are propositions and programs are proofs. Emerald's
type layer is structural rather than dependent, so the dictionary is:

| Logic                  | Emerald                                          |
|------------------------|--------------------------------------------------|
| proposition            | a type                                           |
| proof of `P`           | a value of type `P`                              |
| `P ∧ Q`                | `P & Q` (records), or `{ p: P, q: Q }`           |
| `P ∨ Q`                | `P \| Q`                                         |
| `P ⇒ Q`                | `def f(p: P) -> Q` — a total function            |
| `False` (⊥)            | `never`                                          |
| `¬P`                   | `def f(p: P) -> never`                           |
| `∀T. P(T)`             | `def f[T](...)` — a generic signature            |
| the proof checker      | `emeraldc --check`                               |

A function that typechecks is a proof of its signature: from *any* values of
the parameter types it always produces a value of the return type.

The correspondence is not exact. Emerald has no termination checker, so a
deliberately non-terminating function inhabits any return type — including
`never`:

```
def loop() -> never { while True { pass } }   # accepted: it never finishes
```

Every *other* path must return, though. Because falling off the end of a
function returns `None`, a function whose declared return type rejects `None`
must return on every path, and the checker enforces it:

```
  cheat() can finish without returning a value, but is declared to return never
```

So the only way to inhabit `never` is to diverge. Treat proofs as claims about
*well-formed data*, checked structurally, not as constructive proofs in a
consistent logic.

## Proof by exhaustive case analysis

This is the workhorse. `never` accepts no value, so a binding of type `never`
typechecks only if the checker has already eliminated every alternative.

```
type Lit  = { kind: "lit",  value: int }
type Add  = { kind: "add",  lhs: int, rhs: int }
type Neg  = { kind: "neg",  operand: int }
type Expr = Lit | Add | Neg

def eval(e: Expr) -> int {
    if e.kind == "lit" { return e.value }
    if e.kind == "add" { return e.lhs + e.rhs }
    if e.kind == "neg" { return -e.operand }
    unreachable: never = e      # proof obligation: the cases are exhaustive
    return 0
}
```

Each `if` narrows `e` by its discriminant. By the last line `e` has type
`never` and the binding succeeds. Extend `Expr` with a fourth case and the
line fails, naming exactly what you did not handle:

```
type Expr = Lit | Add | Neg | Mul

  proofs.rald:34: type error: cannot assign {kind: "mul", a: int, b: int}
                              to 'unreachable' declared as never
```

That error is the proof obligation reopening — the reason to write the
`never` binding even when the function already returns on every path.

## Proof by enumeration

A union of literal types is a finite domain. Membership claims about it are
checked by set inclusion.

```
type Even = 0 | 2 | 4 | 6 | 8
type Odd  = 1 | 3 | 5 | 7 | 9
type Digit = Even | Odd

# Theorem: every digit is even or odd.
def digit_is_even_or_odd(d: Digit) -> Even | Odd { return d }
```

The body is the identity function; the content is in the signature. It
typechecks because `Digit` unfolds to the same alternatives as `Even | Odd`.
Drop `8` from `Even` and the proof fails.

This generalizes to any decidable statement over a small enumerated domain:
state the subset as a type, and let assignability check the inclusion.

## Proof by parametricity

A generic signature is a universally quantified claim. Because a type
variable is opaque inside the body — `T` is assignable only to `T` — the body
cannot inspect or fabricate a `T`, so it must genuinely construct its result
from what it was given.

```
type Pair[A, B] = { first: A, second: B }

# Commutativity of conjunction: A ∧ B ⇒ B ∧ A
def swap[A, B](p: Pair[A, B]) -> Pair[B, A] {
    return { first: p.second, second: p.first }
}
```

`return { first: 5, second: 5 }` would be rejected: `5` is not an `A`. The
one escape is `any`, which is assignable in both directions and silently
discharges any obligation. **A proof that mentions `any` proves nothing** —
keep annotations complete in code you intend as a proof.

## Proof of impossibility

`¬P` is a total function `P -> never`. Since nothing inhabits `never`, such a
function can only be written when `P` itself is uninhabited — so the proof is
again exhaustive case analysis:

```
type Bit = 0 | 1

def not_two(b: Bit) -> int {
    if b == 0 { return 1 }
    if b == 1 { return 0 }
    contradiction: never = b   # there is no third bit
    return 0
}
```

## Eliminating partiality

`int | None` is the type of a value that may be missing. Narrowing turns the
runtime obligation into a compile-time one: you cannot use the value until
you have ruled `None` out.

```
def safe_mod(a: int, b: int) -> int | None {
    if b == 0 { return None }
    return a % b
}

def mod_or(a: int, b: int, fallback: int) -> int {
    q: int | None = safe_mod(a, b)
    if q == None { return fallback }
    return q          # q: int — the None case is provably gone
}
```

Removing the guard is an error, not a crash at runtime:

```
  unsupported operand types for +: int | None and int
```

## Style: making obligations explicit

- **Name your obligations.** `unreachable: never = e` reads as a claim, and
  the error message points at the claim rather than at a distant return.
- **Annotate everything in a proof.** Any unannotated parameter is `any`, and
  `any` satisfies every obligation vacuously.
- **Put the theorem in the signature, not the body.** The body is only the
  witness; the signature is what the checker verifies and what a reader
  should read.
- **Prefer literal unions to `int`** when the domain is finite — that is what
  makes case analysis checkable at all.
- **Re-run `emeraldc --check` after changing a type.** Widening a union
  reopens every exhaustiveness obligation that depended on it, which is the
  feature.

## What Emerald cannot prove

These are real limits, not omissions to work around:

- **No induction.** Types cannot be recursive, so there are no inductively
  defined naturals or lists-as-cons-cells, and no proofs by induction over
  them. Claims about all lists are limited to what parametricity gives.
- **No dependent types.** A type cannot mention a value, so "this list has
  length `n`", "this index is in bounds", or "this integer is positive" are
  not expressible. Finite enumerations via literal types are the substitute,
  and they do not scale past a handful of values.
- **No higher-order proofs.** Functions are not values, so you cannot state a
  proposition about a function or pass a lemma as an argument.
- **No termination checking.** An intentionally diverging function inhabits
  `never`, so the logic is not consistent the way a proof assistant's is.
  Every terminating path is checked; the infinite loop is the one escape.
- **Unsound covariant lists.** `list[int]` is assignable to
  `list[int | None]`, and mutating through the alias defeats the element-type
  claim. Do not build a proof on the element type of a shared list.

For proofs that need induction or dependent types, Emerald is the wrong tool —
reach for a real proof assistant. What Emerald does well is the mechanized,
always-on part: exhaustive case analysis, finite domains, partiality, and
parametric claims, checked on every build.
