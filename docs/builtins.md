# Builtins

Emerald has no standard library yet. It has **thirteen builtins**, compiled
directly into calls on the runtime (`src/runtime.c`) rather than resolved
through a module. They are always in scope in every module.

Two rules apply to all of them:

- **Builtins are not values.** `f = print` is an error
  (`E_TYPE_BUILTIN_VALUE`) — there is no closure to hand out. Wrap it:
  `def show(x: any) -> None { print(x) }`.
- **Builtins cannot be redefined.** `def len(...)` is an error
  (`E_TYPE_REDEFINE`), at any scope, in any module.

The checker knows each builtin's arity and argument types, so misuse is a
compile error, not a runtime one. Where a check cannot be static (`int("x")`),
the runtime aborts with a located message — see
[`diagnostics.md`](diagnostics.md).

---

## Output

### `print(...) -> None`

Variadic. Writes each argument separated by a single space, then a newline.
Values are formatted the way `str()` formats them, except that strings print
bare (no quotes) at the top level and quoted inside containers:

```
print("hi", 1, [1, "a"], { x: 1 })
# hi 1 [1, 'a'] {x: 1}
```

`print()` with no arguments prints an empty line.

## Sequences and conversion

### `len(x) -> int`

Length of a `str` (in bytes), `list` (elements), or record (fields). Anything
else is rejected at compile time with `E_TYPE_NO_LEN`.

### `range(n) -> list[int]` / `range(lo, hi) -> list[int]`

Builds an **actual list**, eagerly — not a lazy iterator. `range(5)` is
`[0,1,2,3,4]`; `range(2, 5)` is `[2,3,4]`; an empty or inverted range is `[]`.
Arguments must be `int`. Because it materializes, `for i in range(10000000)`
allocates ten million values — the collector handles it (see [`gc.md`](gc.md)),
but it is not free.

### `str(x) -> str`

The canonical rendering of any value. Nested strings are quoted:
`str([1, "a"])` is `[1, 'a']`, and `str("a")` is `a` — the quoting starts one
level in, so `print(x)` and `print(str(x))` agree.

### `int(x) -> int`

`bool` → 0/1, `int` → itself, `float` → truncated **toward zero**, `str` →
parsed as a decimal integer. A malformed string is a runtime error
(`invalid literal for int(): '...'`), not `None`.

## Math

### `sqrt(x: float) -> float`

Accepts any number (`int` widens). A negative argument is a runtime error
rather than a NaN — the failure is loud on purpose.

### `tan(x: float) -> float`

Radians. Present because the camera in
[`examples/ray_tracer/`](../examples/ray_tracer/) needs a field-of-view.

### `rand() -> float`

A uniform float in `[0, 1)` from an xorshift64\* generator. **There is no seed
builtin**, so a program using `rand()` is not reproducible across runs. When
you need determinism — as the typed ray tracer does — thread an explicit
generator instead; `examples/ray_tracer/typed/rng.rald` implements a seeded
Park–Miller LCG in ~30 lines of Emerald and returns a `Draw[T] = { value: T,
rng: Rng }` from every draw, making randomness a visible dataflow rather than
an ambient effect.

That is still a gap worth noting: there is no *seeded* RNG builtin. But the
ambient effect itself is now tracked. Emerald has a `pure` declaration
(`def f(...) -> T pure`, see [`type-system.md`](type-system.md)); a pure
function may call only the **pure builtins** — `len`, `range`, `str`, `int`,
`sqrt`, `tan`, `gc_stats` — and calling `print`, `rand`, `read_file`,
`write_file`, `append_file`, or `run` from one is a compile error
(`E_TYPE_PURE_CALL`). So "this function is a pure function of its inputs"
*is* statable now, which is the precondition for every proof obligation about
a model (see [`research-directions.md`](research-directions.md) §3, Track B).

## Files and processes

### `read_file(path: str) -> str`

Reads the whole file into a string. A missing or unreadable file is a runtime
error, not an empty string or `None`.

### `write_file(path: str, content: str) -> None`

Truncates and writes. Creates the file if absent.

### `append_file(path: str, content: str) -> None`

Same, opening for append.

### `run(cmd: str) -> int`

Runs `cmd` through the system shell and returns its exit status. This is the
whole process API: no argv list, no captured stdout, no environment control.
Combined with `write_file`/`read_file` it is enough to shell out and read the
result back from a temp file, which is how the compiler's own examples drive
external tools.

## Introspection

### `gc_stats() -> { collections: int, live: int, young: int, old: int, threshold: int }`

Returns a record — a real, typed record, so field access is checked and
`gc_stats().collectons` is a compile error:

| Field         | Meaning                                              |
|---------------|------------------------------------------------------|
| `collections` | number of collections since the program started       |
| `live`        | objects surviving the most recent collection          |
| `young`       | objects currently in the young generation             |
| `old`         | objects promoted to the old generation                |
| `threshold`   | young-generation size that triggers the next minor GC |

This is how [`examples/gc_stress.rald`](../examples/gc_stress.rald) and
`tests/e2e/gc_generational.rald` assert collector behaviour from inside the
language rather than by reading `/usr/bin/time`. See [`gc.md`](gc.md) for what
the numbers mean.

---

## Higher-order list functions

These are the one place where a builtin is *generic*: each call site
instantiates fresh type variables, so they compose with each other and with
lambdas. They take a function *value* — a lambda, a named `def`, or a
pipeline — so they are the heart of the functional core (see
type-system.md).

### `map(f: (T) -> U, xs: list[T]) -> list[U]`

Applies `f` to every element, in order, building a new list.

```
print(map((x: int) => x * 2, [1, 2, 3]))   # [2, 4, 6]
```

### `filter(f: (T) -> bool, xs: list[T]) -> list[T]`

Keeps the elements for which `f` is truthy, in order.

```
print(filter((x: int) => x % 2 == 0, [1, 2, 3, 4]))   # [2, 4]
```

### `reduce(f: (U, T) -> U, acc: U, xs: list[T]) -> U`

Left fold: `f(f(...f(acc, x0), x1), ...)`. `acc` and the fold result share
one type variable `U`.

```
print(reduce((a: int, b: int) => a + b, 0, [1, 2, 3]))   # 6
```

`map`, `filter`, and `reduce` are pure builtins, so they may be called from
`pure` functions (when their function argument is pure too).

---

## What is deliberately missing

No dict type, no string methods (`split`, `join`, `upper`), no `sorted`, no
`min`/`max`, no `abs`, no math beyond `sqrt`/`tan`, no argv, no stdin. Programs
that need these write them: `examples/ray_tracer/typed/math.rald` defines
`min`/`max`/`abs`/`clamp`/`pow`/`radians` as ordinary Emerald functions, and
they typecheck and inline about as well as builtins would.

The reason is the [module system](modules.md): a standard library should be
Emerald modules resolved from a `src/` root, not a growing `if
strcmp(name, ...)` ladder in `check.c`. The builtins that exist are the ones
that *cannot* be written in Emerald — allocation-level (`len`, `range`),
formatting (`str`, `print`), foreign (`read_file`, `run`), or privileged
(`gc_stats`).
