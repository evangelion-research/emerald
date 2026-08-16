# Emerald's Type System

TypeScript-flavored **structural, gradual** typing over a Python-flavored
dynamic core. There are no classes: data is records, "inheritance" is
subset-of-fields.

The type layer is compile-time only. `emeraldc` erases it before codegen, so
types cost nothing at runtime — and unannotated code still runs like Python.

## The types

| Syntax               | Meaning                                                     |
|----------------------|-------------------------------------------------------------|
| `int` `float` `str` `bool` `None` | primitives                                     |
| `any`                | the gradual escape hatch; compatible with everything        |
| `never`              | the empty type; no value inhabits it                        |
| `3` `-1` `"red"` `True` | **literal types**: a single value as a type              |
| `list[T]`            | list with element type `T`                                  |
| `{ x: int, y: int }` | record (structural, anonymous)                              |
| `A \| B`             | union                                                       |
| `A & B`              | intersection of two record types (fields merge, right wins) |
| `type Name = ...`    | alias (must be declared before use, top level only)         |
| `type Name[A, B] = ...` | **generic** alias, applied as `Name[int, str]`            |
| `def f[T](...)`      | **generic** function, instantiated per call site            |

## Structural subtyping = "inheritance" without classes

```
type Point  = { x: int, y: int }
type Point3 = Point & { z: int }     # like `interface Point3 extends Point`

def mag2(p: Point) -> int { return p.x * p.x + p.y * p.y }

p: Point3 = { x: 3, y: 4, z: 5 }
mag2(p)                              # OK: Point3 has everything Point needs
```

Assignability rules (`dst <- src`):

- `any` accepts and flows into everything (gradual typing).
- `never` is assignable **to** everything and accepts **nothing** — it is the
  bottom type, which is what makes exhaustiveness proofs work.
- `bool -> int -> float` widen implicitly (Python numeric tower).
- A literal type accepts only the identical literal. A literal is assignable
  to its base type: `3` fits `int`, `"red"` fits `str`.
- A record fits a record type if it has **at least** the target's fields,
  with assignable types (width subtyping). Extra fields are fine.
- `src` fits `A | B` if it fits either; `A | B` fits `dst` only if **every**
  alternative fits.
- `list[S]` fits `list[T]` if `S` fits `T` — covariant, i.e. conveniently
  unsound in exactly the way TypeScript arrays are. A `list[int]` passed as
  `list[int | None]` can be mutated to smuggle a `None` back; the runtime's
  tagged values keep this from ever being memory-unsafe.
- A function type `(A, B) -> C` fits `(A', B') -> C'` when its parameters are
  invariant (`A == A'`, `B == B'`) and its return is covariant (`C` fits
  `C'`). A top-level `def` name reads as a function value of its declared
  type; a nested `def` likewise, and it captures enclosing locals.

## Literal types

A literal is a type inhabited by exactly one value, so a union of literals is
a finite enumeration — Emerald's stand-in for enums and for the refinement
types you would use to constrain a domain.

```
type Color = "red" | "green" | "blue"
type Dice  = 1 | 2 | 3 | 4 | 5 | 6

c: Color = "red"
c = "purple"        # error: cannot assign "purple" to 'c' declared as
                    #        "red" | "green" | "blue"
```

Under operators a literal behaves as its base type, so arithmetic on a
`Dice` gives `int` rather than a combinatorial union:

```
d: Dice = 4
n = d + 1           # n: int
```

### Widening, and when literals stay literal

An unannotated `x = 3` infers `int`, not `3` — otherwise nothing could ever be
reassigned. This is TypeScript's `let` widening: literals inferred from
literal *expressions* are "fresh" and widen when they land in an unannotated
binding.

A literal written in an **annotation** is not fresh and never widens. That is
what keeps discriminant fields sharp through a list:

```
type Shape = { kind: "circle", r: int } | { kind: "square", side: int }
shapes: list[Shape] = [{ kind: "circle", r: 2 }, { kind: "square", side: 3 }]
for s in shapes { print(area(s)) }   # s keeps the literal `kind`, so it
                                     # is still narrowable inside the loop
```

## Flow narrowing

The checker tracks a *current* type per variable alongside its declared type.
A condition refines the current type inside the branch it guards, and the
refinement is discarded when control leaves.

```
def or_default(x: int | None, d: int) -> int {
    if x == None { return d }
    return x + 0          # x: int here — the None alternative is gone
}
```

What narrows:

| Form                        | Effect                                                |
|-----------------------------|-------------------------------------------------------|
| `x == lit` / `x != lit`     | keep / drop the matching alternative                  |
| `x.field == lit`            | discriminated union: keep alternatives whose `field` can hold `lit` |
| `if x`                      | truthiness: drops `None`, `0`, `""`, `False`          |
| `not c`                     | inverts the narrowing of `c`                          |
| `a and b`                   | both facts hold in the then-branch                    |
| `a or b`                    | both facts fail in the else-branch                    |

Narrowings compose across the shape of the statement, not just inside one
branch:

- **The else-branch and later `elif` arms** see the earlier conditions as
  false, so a union is whittled down arm by arm.
- **A branch that always leaves** (`return`, `break`, `continue`) contributes
  nothing to the code after the `if`. That makes guard style work: after
  `if x == None { return 0 }`, the rest of the function sees `x: int`.
- **Otherwise the paths are joined.** After `if x == None { x = 7 }` the
  variable is `int` on both paths, because the assignment replaced the
  narrowed type on the branch that ran.
- **Assignment invalidates a stale narrowing** rather than restoring it, so a
  variable reassigned inside a branch keeps the type it was actually given.

Narrowing applies to plain variables (locals, parameters, globals). Field and
index expressions are narrowed *through* their base variable, so
`if s.kind == "circle"` refines `s`, not `s.kind` alone. A variable captured
by a nested function reads its stable declared type inside that function
(narrowing does not cross a closure boundary).

## Exhaustiveness proofs with `never`

`never` accepts no value. So a binding of type `never` typechecks only when
the checker can prove the value that reaches it cannot exist — which is
exactly a proof that the preceding cases were exhaustive.

```
type Circle = { kind: "circle", r: int }
type Square = { kind: "square", side: int }
type Shape  = Circle | Square

def area(s: Shape) -> int {
    if s.kind == "circle" { return s.r * s.r * 3 }
    if s.kind == "square" { return s.side * s.side }
    impossible: never = s      # OK: every alternative was handled
    return 0
}
```

Add a third alternative to `Shape` and the proof fails, naming the case you
forgot:

```
type Shape = Circle | Square | Tri

  area.rald:12: type error: cannot assign {kind: "tri", base: int, h: int}
                            to 'impossible' declared as never
```

This is the main tool for making a change to a data type surface every site
that must be updated. See [proofs.md](proofs.md) for using it to state and
check mathematical arguments.

## Generics

Generic functions take type parameters in brackets before the value
parameters. At each call site the checker unifies the argument types against
the parameter types, binds the type variables, and substitutes them into the
return type.

```
def head[T](xs: list[T]) -> T { return xs[0] }

h: int = head([1, 2, 3])       # T = int
s: str = head(["a", "b"])      # T = str
w: str = head([1, 2, 3])       # error: cannot assign int to 'w' declared as str
```

Generic aliases work the same way and are expanded at each use:

```
type Pair[A, B] = { first: A, second: B }

def swap[A, B](p: Pair[A, B]) -> Pair[B, A] {
    return { first: p.second, second: p.first }
}

p: Pair[int, str] = { first: 1, second: "x" }
q: Pair[str, int] = swap(p)
```

Inside a generic body a type variable is **opaque**: `T` is assignable only
to `T`, so `return 5` from a `-> T` function is an error. That is what makes
a generic signature a universally quantified statement rather than a hint.

Inference notes:

- Type arguments are inferred from the call, never written explicitly.
- A variable bound from several arguments takes the join (union) of them.
- Inferred arguments are widened, so `head([1, 2])` gives `int`, not `1 | 2`.
- An unconstrained variable falls back to `any` rather than erroring.
- Applying the wrong number of arguments is an error, as is applying `[...]`
  to a non-generic type.
- Generic aliases may not be recursive (expansion is depth-limited).

## Gradualness

- Unannotated parameters and returns are `any`; unannotated code checks
  exactly like Python would run.
- An **annotated** variable (`n: int = 5`) is enforced forever: assigning a
  `str` to it later is a compile error.
- An **inferred** variable takes the type of its first assignment; a
  conflicting later assignment quietly *widens* the variable
  (`x = 1` then `x = "s"` makes `x: int | str`) rather than erroring —
  Python code should stay valid.
- Field access is checked on typed records (`p.z` on a `Point` is a compile
  error, as is assigning a new field); on `any` everything is allowed and
  checked at runtime instead.
- On a union, a field access requires the field to exist on **every**
  alternative. Narrow first if it doesn't.

## What the checker also catches

- Unknown names, unknown types, calls to undefined functions.
- Wrong arity and wrong argument types at every call site, including
  instantiated generic signatures.
- Return-type mismatches; `return` outside a function.
- Paths that fall off the end of a function whose return type rejects `None`
  (falling off returns `None`). A `while True` with no `break` counts as
  never finishing, which is the only way to inhabit `never`.
- `break`/`continue` outside a loop; nested `def` (unsupported).
- Redefining or shadowing builtins; using a function name as a value.
- Non-iterables in `for`, non-indexables under `[]`, `str` item assignment.

Errors carry `file:line:` and don't stop the checker — you get the full
list. (Top-level errors print before function-body errors, because bodies
are checked in a later pass once global types are known.)

## Deliberate omissions

- **Recursive aliases**: a non-generic alias may reference itself (`type Tree =
  { v: int, kids: list[Tree] }`), enabling lists-as-cons-cells and inductive
  data. Recursive *generic* aliases are still rejected.
- **No bounds on type parameters** (`T extends Comparable`), no variance
  annotations, no conditional or mapped types.
- **No dependent types**: `list[T]` cannot carry a length, so statements
  about sizes are not expressible.
- **Covariant lists are unsound** by design, matching TypeScript.
- Narrowing is per-variable and does not survive into a called function.
