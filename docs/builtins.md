# Builtins

Emerald has **seventy-eight builtins**, compiled directly into calls on the
runtime (`src/runtime.c`) rather than resolved through a module. They are
always in scope in every module: the forty core builtins (core,
GC-observability, files-and-process, and the stdlib foundation), the eleven
[green-thread](#green-threads) builtins, and the twenty-five tensor primitives
of Phase 2 (see the [Tensors](#tensors) section).

They are not the standard library — that lives in [`stdlib/`](../stdlib/) and is
ordinary Emerald. The mutable `dict` and `set` runtime values are the one
collection exception: their storage and mutation are builtin, while their
constructors and operators are exposed directly as `dict()` and `set()`. A
builtin exists only when it *cannot* be written in Emerald:
allocation-level (`len`, `range`, `append`, `slice`), formatting (`str`,
`print`), foreign (`read_file`, `run`, `argv`), or privileged (`gc_stats`). See
[`stdlib/SPEC.md`](../stdlib/SPEC.md) §1.1 for why each of the ten added for the
library had to be one.

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

### `eprint(...) -> None`

`print` to stderr. Same formatting, same variadic rule. Diagnostics and
progress belong here so a pipeline's data stays clean on stdout.

### `write_out(x) -> None` / `write_err(x) -> None`

One value, formatted as `print` formats it, with **no trailing newline and no
separator** — the spelling a prompt or a progress dot needs. Both flush what
they wrote, because stdout is line-buffered on a terminal and a prompt with no
newline would otherwise sit in the buffer until after the read it precedes.

```
write_out("name? ")
name = read_line()
```

### `flush() -> None`

Flushes stdout and stderr. `write_out` already flushes, so this is for the case
where `print` output must be on screen before a long computation that has not
printed its own newline yet.

## Dictionaries and sets

`dict()` creates an empty string-keyed dictionary. `dict(pairs)` copies an
iterable of two-item lists or tuples, and `dict(other_dict)` copies a dictionary.
Use `d[key]` to read and `d[key] = value` to insert or replace; missing keys are
runtime errors, like Python's subscription operation. `len(d)` returns the
number of entries, iteration yields keys, and `key in d` tests membership.
Non-string keys are rejected by the checker for literals and by the runtime for
computed keys.

`set()` creates an empty set. `set(iterable)` collects values from any iterable;
iteration and `len()` work as for dictionaries. Set union, intersection,
difference, and symmetric difference use `|`, `&`, `-`, and `^` respectively.
These operations return fresh sets and do not mutate their operands.

## Sequences and conversion

`seq[T]` is the immutable, covariant view of a list. `freeze(xs)` returns a
`seq` without copying; `thaw(s)` returns a mutable copy. Indexing, iteration,
`len`, and `map`/`filter`/`reduce` accept sequences, while `append` and indexed
assignment reject them. See [`type-system.md`](type-system.md) for the
proof-mode invariance rule for mutable lists.

### `len(x) -> int`

Length of a `str` (in bytes), `list`, tuple, dictionary, set, or record. Other
values are rejected at compile time with `E_TYPE_NO_LEN`.

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

A uniform float in `[0, 1)` from an xorshift64\* generator, seeded from the
clock at startup. `seed_rand(n)` makes a run reproducible; see below.

Reproducibility does not have to be ambient. When randomness is part of the
result — as in the typed ray tracer — thread an explicit generator instead;
`examples/ray_tracer/typed/rng.rald` implements a seeded Park–Miller LCG in
~30 lines of Emerald and returns a `Draw[T] = { value: T, rng: Rng }` from
every draw, making randomness a visible dataflow rather than a hidden one.
`seed_rand` is the cheap answer for a test that just needs the same numbers
twice; the threaded generator is the honest one for a computation whose
output is defined by its seed.

The ambient effect itself is tracked. Emerald has a `pure` declaration
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

### `gc_stats() -> { collections: int, live: int, young: int, old: int, threshold: int, bytes_young: int, bytes_old: int }`

Returns a record — a real, typed record, so field access is checked and
`gc_stats().collectons` is a compile error:

| Field         | Meaning                                              |
|---------------|------------------------------------------------------|
| `collections` | number of collections since the program started       |
| `live`        | objects surviving the most recent collection          |
| `young`       | objects currently in the young generation             |
| `old`         | objects promoted to the old generation                |
| `threshold`   | young-generation size that triggers the next minor GC |
| `bytes_young` | bytes live in the young generation                    |
| `bytes_old`   | bytes live in the old generation                      |

`bytes_young`/`bytes_old` exist because the collector is **byte-aware**
(tensors.md): a single large tensor must trigger a collection even though it is
one object.

### `gc_collect() -> None`

Forces a major collection immediately. Deterministic — it is how
`tests/e2e/gc_bytes.rald` asserts that a dead allocation's bytes are actually
reclaimed rather than waiting for the threshold.

These are how [`examples/gc_stress.rald`](../examples/gc_stress.rald) and
`tests/e2e/gc_generational.rald` assert collector behaviour from inside the
language rather than by reading `/usr/bin/time`. See [`gc.md`](gc.md) for what
the numbers mean.


---

## The standard library's foundation

Ten builtins exist because [`stdlib/`](../stdlib/) cannot be written without
them. Nine are ordinary; `append` is the one that changes what the language can
express at all.

### `append(xs: list[T], v: T) -> None`

Amortized in-place growth: the capacity doubles, so building a list of `n`
elements costs O(n) rather than the O(n²) of `xs = xs + [v]`. The element type
is checked against the list's, so `append([1], "s")` is a compile error.

**This is the operation no Emerald code can express.** Before it, every
list-building function in the library was quadratic; `xs[len(xs)] = v` is a
runtime index error, because `em_setindex` writes into existing storage.

`append` mutates, so it is **not pure** — but with §1.2's local-mutation
purity, a `pure` function may `append` to a list it allocated itself and has
not let escape, so a function that builds its own `out: list[T] = []` is pure
while one that mutates a parameter is not. See
[`stdlib/SPEC.md`](../stdlib/SPEC.md) §1.2.

### `slice(seq, lo: int, hi: int)`

Substring, or sublist — the type is preserved (`str -> str`, `list[T] ->
list[T]`), and slicing anything else is a compile error.

Clamped and total: bounds outside the sequence are pulled to its edges, an
inverted range is empty, and negative bounds count from the end. `slice("hello",
-3, 5)` is `"llo"`; `slice("abc", 2, 1)` is `""`. It never fails at runtime.

There is no `s[a:b]` syntax; this is the whole slicing story.

### `ord(c: str) -> int` / `chr(n: int) -> str`

The first byte of a string as 0..255, and back. `ord("")` is a runtime error;
`chr` outside 0..255 is too. Bytes, not codepoints — see D1 in the stdlib spec.

Without these there is no arithmetic on characters, so no hashing, no character
classes, no escape decoding, and no number parsing written in Emerald.

### `float(x) -> float`

The counterpart of `int()`. `bool`/`int`/`float` convert; a `str` is parsed, and
a malformed one is a runtime error (`invalid literal for float(): '...'`), not a
`None`. `strings.parse_float` is the checked version.

### `eprint(...) -> None`

`print`, to stderr. Diagnostics must not go to stdout, or a compiler's
`--emit-c` output is corrupted by its own warnings.

### `argv() -> list[str]`

The process's argument vector, including `argv[0]`. `sys.args()` drops the
program name.

### `exit(code: int) -> never`

Terminates with a status. Its type is `never`, which has two consequences worth
knowing: `exit(1)` satisfies any declared return type, and a function whose last
statement is an `exit()` has not "fallen off the end". So this typechecks:

```
def checked(n: int) -> int {
    if n < 0 { exit(3) }
    return n
}
```

Together with `while True`, this is the second way to inhabit `never` without
recursion.

### `seed_rand(n: int) -> None`

Reseeds `rand()`, so the same seed replays the same stream. `0` is not a usable
xorshift state and maps to the default seed rather than wedging the generator.

Spelled `seed_rand` rather than `seed` because the builtin namespace is flat and
shared: a builtin named `seed` would make the global `seed = 7` that any
hand-rolled PRNG writes an `E_TYPE_ASSIGN`. Same reasoning as `io.append_to`
(see [`stdlib/SPEC.md`](../stdlib/SPEC.md) §8). Tier 1's `random.seed` will wrap
it under the Python name.

### `now() -> float`

Monotonic seconds. **The epoch is unspecified** — only differences are
meaningful, which is all a stopwatch or a benchmark harness needs. Monotonic
means it does not go backwards when the wall clock is adjusted, so it is not a
calendar and cannot be turned into one.

### `read_line() -> str | None`

One line from stdin **without** its newline, or `None` at EOF — so the read loop
is `while (line = read_line()) != None`, with no separate EOF test. A trailing
`\r` is dropped, so a CRLF file reads the same as an LF one, matching
`strings.split_lines`.

### `input(prompt: str) -> str | None`

`write_out(prompt)` then `read_line()`: the prompt is on screen before the read
blocks, and the result is the line without its newline, or `None` at EOF. The
`None` is the point — a script run with its stdin closed gets a value it can
branch on rather than a hang or an abort. `io.ask_or` folds it into a default.

```
answer = input("continue? [y/N] ")
if answer == None or answer == "" { exit(0) }
```

### `read_all() -> str`

Every remaining byte of stdin as one string; `""` at EOF, so a second call is
empty rather than an error. This is the filter idiom — read it all, transform,
print — where `read_line` is the streaming one. `io.stdin_lines` splits it.

### `read_file_opt(path: str) -> str | None`

`read_file` aborts on a missing file, which is right for a script and wrong for
a library. Same reader, `None` instead of a fatal error. `io.read` wraps this
into a `Result`.

### `file_exists(path: str) -> bool`

Is the path openable for reading? Used by `io.exists`. It answers about *now*,
so a file can vanish between the check and the open — read and handle the
failure instead when that matters.

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

The full pure set is `len`, `range`, `str`, `int`, `float`, `sqrt`, `tan`,
`slice`, `ord`, `chr`, `gc_stats`, `map`, `filter`, `reduce`. Everything else —
`print`, `eprint`, `rand`, `append`, `exit`, `argv`, and the file and process
builtins — is impure, and calling one from a `pure` function is
`E_TYPE_PURE_CALL`.

---

## Tensors

Phase 2 (see [`tensors.md`](tensors.md) and [`shapes.md`](shapes.md)) adds
**twenty-five** tensor primitives. They are builtins — not a stdlib module —
because their types are *shape obligations*, not ordinary signatures; the
checker special-cases them to verify those obligations statically.

**Constructors** — `zeros`, `ones`, `full`, `arange`, `tensor`, `randn`. All
return a tensor; `randn(shape, seed)` is seeded, so it is deterministic by
construction (and impure, since randomness is an effect even when seeded).

**Elementwise** — `+ - * /` on tensors (broadcasting) and unary `exp`, `log`,
`tanh`, `relu`.

**Shape-carrying** — `matmul`, `reshape`, `transpose`, `permute`, `expand`,
`sum`, `mean`, `max`, `argmax`, `tslice`. These are the operations whose types
are obligations; the checker emits `E_SHAPE_*` diagnostics when one cannot be
discharged statically.

**Introspection** — `shape` (`list[int]`), `ndim` (`int`), `dtype` (`str`),
`astype`, and `item` (`float`).

All of them except `randn` are **pure** and may be called from a `pure`
function.

## Green threads

Eleven builtins for concurrency, described in full in
[`concurrency.md`](concurrency.md). Tasks are cooperative: many tasks, one
running at a time, switching only at the points below.

| Builtin | Type | Meaning |
|---|---|---|
| `spawn(f)` | `(() -> T) -> Task[T]` | start `f` as a task; the spawner keeps running |
| `join(t)` | `Task[T] -> T` | wait for a task and take its result |
| `task_done(t)` | `Task[T] -> bool` | has it finished? never blocks |
| `task_yield()` | `() -> None` | let another runnable task have a turn |
| `sleep(secs)` | `float -> None` | suspend this task; others keep running |
| `chan(cap)` | `int -> Chan[any]` | a channel; `cap` 0 is unbuffered |
| `send(c, v)` | `(Chan[T], T) -> None` | block until a receiver or buffer space |
| `recv(c)` | `Chan[T] -> T \| None` | block for a value; `None` once drained and closed |
| `chan_close(c)` | `Chan[T] -> None` | no more sends; receivers drain, then get `None` |
| `chan_len(c)` | `Chan[T] -> int` | buffered items waiting |
| `task_stats()` | `() -> {spawned, alive, switches}` | counters, like `gc_stats()` |

`Chan[T]` and `Task[T]` are types you can write in annotations. `chan()` cannot
know its own element type, so it produces `Chan[any]` and the annotation is
what pins it down:

```rald
jobs: Chan[int] = chan(8)
send(jobs, "nope")     # error: send() on Chan[int] cannot carry "nope"
```

Because `recv` returns `T | None`, every receive loop has to consider the
closed channel — the same shape as `read_line()`, and the reason a worker loop
needs no separate "are we done" flag.

None of these are pure: a task is an effect.

---

## What is deliberately still missing

No string methods, no `sorted`, no `min`/`max`, no `abs`, no math beyond
`sqrt`/`tan`.

Those are [`stdlib/`](../stdlib/) modules: `strings`, `sort`, `lists`, and
`math`. Dictionaries and sets are deliberately not modules: their runtime
storage, mutation, and collection operators are builtin.

Still genuinely absent, with no library answer:

| Missing | Why it matters |
|---|---|
| `select` over channels | a task can only wait on one channel at a time, so fan-in needs a dedicated collector task rather than one loop over several channels |
| hashable-key constraints | dictionaries are string-keyed because the type system cannot express a general hashability bound |
| integer division | `/` is float division, so `math.floor_div` rounds through a double and is exact only under 2^53 |
