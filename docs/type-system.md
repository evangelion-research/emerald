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
| `list[T]`            | mutable list with element type `T`                          |
| `seq[T]`             | immutable, covariant sequence with element type `T`          |
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
- In ordinary mode, `list[S]` fits `list[T]` if `S` fits `T` — covariant, as
  in TypeScript arrays. When this upcast would rely on mutable aliasing, the
  checker emits `W_UNSOUND_COVARIANCE`; it remains memory-safe because runtime
  values are tagged. Under `--proof`, mutable lists are invariant (apart from
  fresh literal widening and the gradual `any` escape hatch), so the same
  upcast is rejected.
- `seq[S]` fits `seq[T]` covariantly in every mode. A `seq` is backed by the
  same runtime list representation, but the checker rejects `append` and item
  assignment, making the covariance sound. `freeze(list[T]) -> seq[T]` and
  `thaw(seq[T]) -> list[T]` make the boundary explicit.
- A function type `(A, B) -> C` fits `(A', B') -> C'` when its parameters are
  invariant (`A == A'`, `B == B'`) and its return is covariant (`C` fits
  `C'`). A top-level `def` name reads as a function value of its declared
  type; a nested `def` likewise, and it captures enclosing locals.

## Mutable lists, immutable sequences, and proof mode

`list[T]` is the ergonomic mutable collection. Its ordinary-mode covariance is
kept for compatibility with the scripting language, but `W_UNSOUND_COVARIANCE`
marks assignments and calls that could write a wider element type through a
narrower list reference. Treat that warning as a request to use `seq[T]` or
`freeze()` when the value is read-only.

`seq[T]` is the sound collection boundary: it supports indexing, length,
iteration, and the read-only `map`/`filter`/`reduce` paths, but rejects
`append()` and `xs[i] = value`. A list literal receives its `seq` element type
from a contextual annotation, so `xs: seq[int] = [1, 2, 3]` is checked without
introducing a second literal syntax. `thaw()` copies before returning a mutable
list, so mutations cannot affect the source sequence.

Proof mode uses the same rule rather than requiring every program to migrate:
mutable `list[T]` becomes invariant, while immutable `seq[T]` remains covariant.
Fresh literals may still widen at a call boundary because they have not escaped
as aliases; this is why ordinary list-building proofs such as `map((x) => ...,
[1, 2])` remain usable.

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
- A function's type parameters are in scope in its **body** as well as its
  signature, so a generic function can declare a local of its own type
  parameter: `out: list[T] = []` inside `def f[T](...)`.
- Unification looks through unions on both sides, so `def f[T, E](r: Result[T, E])`
  binds `T = int` from a `Result[int, str]` argument even though `Result`
  expands to a two-alternative union.
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
  never finishing, which (together with a `partial` function) is the only
  way to inhabit `never` without recursion.
- `break`/`continue` outside a loop; nested `def` (unsupported).
- Redefining or shadowing builtins; using a function name as a value.
- Non-iterables in `for`, non-indexables under `[]`, `str` item assignment.

Errors carry `file:line:` and don't stop the checker — you get the full
list. (Top-level errors print before function-body errors, because bodies
are checked in a later pass once global types are known.)

## Expected errors

Failure is a value, and the type that carries it is an ordinary tagged union:

```
type Result[T, E] = { ok: True, val: T } | { ok: False, err: E }
```

Nothing about `Result` is built in. What the language adds is *checking* on
values of that shape — `try` and `catch` recognise any type discriminated by a
boolean-literal `ok` field carrying `val` on the True side and `err` on the
False side, so a program that grows its own result type keeps the support.

`error Name { ... }` declares one expected failure. It desugars to a record
with a literal discriminant:

```
error NotFound { key: str }     ==     type NotFound = { _tag: "NotFound", key: str }
```

The literal `_tag` is what makes a union of errors *discriminated*: the same
machinery that proves a `match` exhaustive tells `NotFound` from `Denied`. A
record literal `NotFound { key: "port" }` fills the `_tag` in, and — unlike an
ordinary string literal field — that one does not widen to `str` when it flows
through a generic, so an error still matches its declaration after a round trip
through `err(...)`.

### `try`: the error channel

`try e` is the value of `e` when it succeeded, and an early `return` of the
failure when it did not. The obligation is on the enclosing function, and it is
exactly Rust's rule for `?`:

```
def port() -> Result[int, NotFound | Malformed] {
    const raw = try field("port")     # field can fail with NotFound
    const n = try digits_of(raw)      # digits_of can fail with Malformed
    return ok(n)
}
```

Drop `Malformed` from that signature and the checker names the escaping error
(`E_TYPE_ERRCHAN`) — a function's declared errors are the complete list of what
it can fail with, and the compiler holds it to that. A function whose return
type is unannotated (`any`) is unchecked here, like everywhere else in the
gradual type system. A lambda has no declared return type at all, so `try`
inside one is rejected outright (`E_TYPE_TRY`) rather than silently propagating
out of a function whose type says it cannot fail.

### `catch`: handling, exhaustively

`catch` is an expression whose value is the subject's success value, or the
value of the arm that matched:

```
const n = catch port() {
    NotFound e -> 80
    Malformed e -> 0 - 1
}
```

Every error the subject can produce must be named by an arm or by a single
catch-all `_` (which binds whatever the named arms did not take). A missing arm
is `E_TYPE_CATCH`, an arm naming an error that cannot occur is also
`E_TYPE_CATCH`, and so is a second arm for an error already handled. The type
of the whole expression is the join of the success type and the arms' types —
so a `catch` whose arms all produce the success type produces exactly that.

This is not exception handling. There is no unwinding, no handler search, and
no invisible control flow: `try` compiles to a comparison and a `return`, and
`catch` to a chain over `_tag`. The cost is what it looks like.

[`errors.md`](errors.md) is the full reference: the combinator table against
Effect's API, the module-boundary rule, and what each form lowers to.

## Purity, totality, and proof mode

Three declarations on a `def` turn the checker from a type checker into a
*claim* checker, in the sense that matters for [`proofs.md`](proofs.md):

- **`pure`** — `def f(x: int) -> int pure { ... }`. A pure function may only
  call other pure functions and the pure builtins (`len`, `range`, `str`,
  `int`, `sqrt`, `tan`, `gc_stats`). Calling `print`, `rand`, or the
  file/process builtins is a compile error (`E_TYPE_PURE_CALL`), and a
  nested `def` inside a pure function must itself be pure
  (`E_TYPE_PURE_NESTED`). "This function is a pure function of its inputs"
  is now statable — the precondition for any proof obligation about a model.
  Purity is deliberately shallow: it is enforced on *calls*, not on global
  state or function values, so `pure` is a promise about what the function
  invokes, not a full effect system.

- **`partial`** — `def f(n: int) -> int partial { ... }`. Functions are
  **total by default**: a recursive call must descend structurally, i.e. at
  least one argument must be a projection chain from a parameter whose
  declared type is a recursive alias (`n.succ`, `xs.tail`), with every step
  landing back on that same alias. A recursive call that does not descend is
  an error (`E_TYPE_TERMINATION`) unless the function is declared `partial`.
  Mutual recursion and descent through list elements (`t.kids[0]`) are not
  recognized yet, so those also need `partial`.

- **`--proof`** — `emeraldc --check --proof f.rald`. Proof mode bans `any`
  (including `any` reachable inside a type constructor), bans `partial`, and
  makes mutable `list[T]` invariant. A clean `--check --proof` therefore means
  every value has a static type, mutable aliases cannot cross an unsound list
  upcast, and every function terminates structurally — the minimal meaning of
  "this is a proof" (see `proofs.md`). Use `seq[T]` for sound covariant
  read-only data.

Both `pure` and `partial` may appear on the same function (`pure partial`),
and neither changes code generation — they are checker-only.

## The functional core

Emerald's functional core is a small, typed layer on top of the same value
model: functions are values, lists are the main data structure, and records
serve as tagged sum types for pattern matching.

- **`const`** — `const x = v` / `const x: T = v` binds `x` immutably.
  Reassigning a `const` is `E_TYPE_CONST`. Prefer `const` for bindings that
  never change; it is the honest spelling of "this is a value".

- **Lambdas** — `(a: int, b) => body` is an anonymous function value with
  type `(int, any) -> ...`. Unannotated parameters are typed contextually:
  inside `map(f, xs)`, `filter(f, xs)` or `reduce(f, acc, xs)` the lambda's
  parameters take the instantiated element types, so `map((x) => x * 2,
  [1, 2, 3])` checks even though `x` has no annotation. A lambda captures
  enclosing locals by reference (shared cell), so closures can carry state:
  `c = 0; def bump() -> int { c = c + 1; return c }; return bump`.

- **Higher-order builtins** — typed with fresh type variables at each call
  site (so nested use composes); `map`, `filter`, and `reduce` preserve whether
  their sequence argument is a mutable `list` or immutable `seq`:

  ```
  map(f: (T) -> U, xs: list[T]) -> list[U]
  filter(f: (T) -> bool, xs: list[T]) -> list[T]
  reduce(f: (U, T) -> U, acc: U, xs: list[T]) -> U
  ```

- **Pipe and compose** — `x |> f` is `f(x)` (right side must be a unary
  function), and `f >> g` is `x -> g(f(x))`; `>>` binds tighter than `|>`.
  `>>` builds a closure at runtime (`em_compose`), so composed pipelines are
  ordinary function values.

- **`match`** — `match e { pat -> { ... } }` with patterns `_`, a binding
  name, a literal, or a record shape `{ kind: "circle", r }` (which binds
  `r`). The checker proves exhaustiveness (`E_TYPE_MATCH`), so a `match`
  over a tagged union replaces the manual `if e.kind == ...` chains:

  ```
  type Shape = { kind: "circle", r: float } | { kind: "square", side: float }
  def area(s: Shape) -> float {
      match s {
          { kind: "circle", r } -> { return 3.14159 * r * r }
          { kind: "square", side } -> { return side * side }
      }
  }
  ```

- **Tail calls** — a `return f(...)` that calls the enclosing function
  directly is compiled to a jump (reassign parameters, loop) instead of a
  recursive call, so tail recursion runs in constant stack: `sum_to(n - 1,
  acc + n)` for `n = 10_000_000` is fine. Tail calls inside `if`/`match`
  arms are recognized; mutual recursion is not (it needs a trampoline), and
  a self-call inside a nested `def` belongs to that nested function.
