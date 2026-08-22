# Emerald

A Python-flavored language with **braces instead of indentation**,
**TypeScript-style structural typing** instead of classes, a **two-generation
mark-and-sweep GC**, and a compiler written in modern C11 that emits native
binaries via your system `cc`.

```
type Point  = { x: int, y: int }
type Point3 = Point & { z: int }          # structural "inheritance"

def mag2(p: Point) -> int {
    return p.x * p.x + p.y * p.y
}

p: Point3 = { x: 3, y: 4, z: 5 }
print(mag2(p))                            # Point3 is-a Point by shape
for i in range(5) { print(i * i) }
```

Emerald is built to explore how much of a real program's meaning a practical
type system can hold, while providing a substrate for writing and studying
neural networks through machine-checked interpretation.

---

## The type system

Literal types, unions, generics, flow narrowing, and `never` — enough to make
the checker verify exhaustive case analysis, so a change to a data type
surfaces every site that must be updated:

```
type Circle = { kind: "circle", r: int }
type Square = { kind: "square", side: int }
type Shape  = Circle | Square

def area(s: Shape) -> int {
    if s.kind == "circle" { return s.r * s.r * 3 }
    if s.kind == "square" { return s.side * s.side }
    impossible: never = s      # typechecks only if the cases are exhaustive
    return 0
}

def head[T](xs: list[T]) -> T { return xs[0] }   # generics
type Pair[A, B] = { first: A, second: B }
```

Types are erased at runtime — there is no class, vtable, or nominal tag — so
structural subtyping costs nothing.

## Expected errors

There are no exceptions. A function that can fail says so in its return type,
and the checker holds both sides to it — the callee's declared errors are the
complete list, and every caller either handles them or declares that it passes
them on:

```
error NotFound { key: str }
error Malformed { key: str, saw: str }

def field(key: str) -> Result[str, NotFound | Malformed] { ... }

def port() -> Result[int, NotFound | Malformed] {
    const raw = try field("port")    # or return the failure to my caller
    return ok(int(raw))
}

const n = catch port() {             # an expression: the value, or an arm's
    NotFound e -> 80
    Malformed e -> 0 - 1             # a missing arm is a compile error
}
```

`error N { ... }` is sugar for a record with a literal `_tag`, so errors are
ordinary discriminated unions and `catch` proves exhaustive with the same
machinery as `match`. `try` compiles to a comparison and a `return`: no
unwinding, no handler search, no hidden control flow. Rust's obligations,
Effect's vocabulary — `map_error`, `catch_all`, `either`, `retry` and the rest
live in [`stdlib/result.rald`](stdlib/result.rald). See
[`docs/errors.md`](docs/errors.md) and
[`examples/errors.rald`](examples/errors.rald).

## Totality, purity, proof mode

Three properties separate "well-typed" from "a claim you can defend", and each
has a switch:

- **Functions are total by default.** Every recursive call must descend
  structurally — the argument is a projection chain out of a parameter of
  recursive-alias type (`n.succ`, `xs.tail`), or an element of a `seq` field
  (`t.kids[0]`). Mutual recursion is rejected as a cycle without structural
  descent, and under `--proof` a `while` loop must have an evident monotone
  integer counter (`for i in range(n)` is the supported total loop). A function
  whose recursion can't be shown to descend must say `partial`, which marks it
  as *not* a proof.
- **`pure`** — `def forward(x: T) -> U pure` promises no `print`, no `rand`,
  no file or process IO, and no impure callee (a nested `def` inside a pure
  function must itself be pure). Purity lives on function *types*, so a pure
  function cannot smuggle an impure callee through `map`/`filter`/`reduce` or
  an indirect call (`docs/effects.md`).
- **`seq[T]`** — the immutable, covariant, sound sequence. `list[T]` stays
  covariant (and mutable) in ordinary code — the unsound upcast now warns
  `W_UNSOUND_COVARIANCE` — while `seq[T]` is sound because the checker refuses
  to mutate it. `freeze(xs)`/`thaw(s)` convert, and under `--proof` `list[T]`
  is invariant while `seq[T]` is not.
- **`Eq[a, b]`** — propositional equality of dimension expressions. `refl`
  inhabits `Eq[a, a]`; a value `e: Eq[a, b]` in scope lets a `Tensor[f32, [a]]`
  be used as `Tensor[f32, [b]]` across a function boundary.
- **`--proof`** — `emeraldc --check --proof f.rald` bans `any` (including `any`
  hidden inside a type constructor — the empty `[]` hole is closed), `partial`,
  and non-termination. `--proof-report` (text or `--json`) prints what was
  checked: function totals, vacuous obligations, taint sites, and covariance
  warnings.

## Functions are values, closures included

Function types are written `(A, B) -> C`. Pass functions around, call them
indirectly, and nest `def`s — a nested function captures enclosing locals by
shared, mutable cell:

```
def make_adder(n: int) -> (int) -> int {
    def add(x: int) -> int { return x + n }
    return add
}

add5  = make_adder(5)
add10 = make_adder(10)
print(add5(1), add10(1))            # 6 11
```

The functional core sits on top of this: `const` immutable bindings, lambdas
`(x: int) => x * 2` (unannotated parameters are inferred contextually), the
higher-order builtins `map` / `filter` / `reduce`, pipelines with `|>` and
composition with `>>`, exhaustive `match` on tagged records, thunks
`() => expr` for lazy evaluation, and **tail-call optimization** — a direct
`return f(...)` compiles to a jump, so tail recursion runs in constant stack
(10M-deep recursion is fine). See [`examples/functional/`](examples/functional/)
for a seven-part tour of each feature.

## The REPL

```
$ emeraldc --repl          # or just: emeraldc
emerald> xs = [1, 2]
emerald> append(xs, 3)
emerald> xs
[1, 2, 3]
```

There is no interpreter — the session *is* its source text. Each entry is
appended, the whole program is recompiled and re-run, and a marker line printed
between the old text and the new entry tells the REPL which output to show. So
the checker is the same checker, state needs no runtime support, and a typo
leaves the session untouched; the price is that effects repeat, once per entry.
`:list`, `:undo`, `:save FILE` and friends bridge back to ordinary files.
See [`docs/repl.md`](docs/repl.md).

## Builtins

Seventy-seven builtins compile straight into runtime calls and are in scope in
every module: the forty-one core builtins (`print`, `len`, `range`, `str`, `int`,
`float`, `append`, `slice`, `freeze`, `thaw`, `map`, `filter`, `reduce`,
`read_file`, `argv`, `run`, `gc_stats`, and friends), the eleven green-thread
builtins (`spawn`, `join`, `chan`, `send`, `recv`, `sleep`, `task_yield`, …),
and the twenty-five tensor primitives (`zeros`, `ones`, `matmul`,
`reshape`, `transpose`, `astype`, …), plus the Python-style `dict()` and
`set()` collection constructors. They are not values (`f = print` is an
error — there is no closure to hand out) and cannot be redefined.

A builtin exists only when it cannot be written in Emerald: allocation-level
(`append`, `slice`, `len`, `range`), formatting (`str`, `print`), foreign
(`read_file`, `argv`, `run`), privileged (`gc_stats`), or a tensor shape
obligation (`matmul`, `reshape`). `append` is the load-bearing core one —
without amortized in-place growth, every list built in a loop is quadratic.
The authoritative list is [`include/builtins.def`](include/builtins.def), the
one table the checker and codegen share. See [`docs/builtins.md`](docs/builtins.md),
[`docs/tensors.md`](docs/tensors.md), and [`docs/shapes.md`](docs/shapes.md).

## Green threads

`spawn` starts a cooperative task, `chan` connects tasks, `join` waits for one.
One task runs at a time and control changes hands only at a channel operation,
a `sleep`, a `join`, or an explicit `task_yield()` — so nothing is interrupted
mid-statement and the language needs no locks.

```rald
jobs: Chan[int] = chan(8)
w = spawn(() => worker("w0"))
for n in range(2, 40) { send(jobs, n) }
chan_close(jobs)              # receivers drain, then recv() gives None
print("handled", join(w))
```

`Chan[T]` and `Task[T]` are checked at both ends: `send` must carry a `T`,
`recv` yields `T | None` (the closed channel is in the type), and `join` hands
back what the task returned. Blocking forever is reported as a deadlock rather
than hanging. See [`docs/concurrency.md`](docs/concurrency.md) and
[`examples/tasks.rald`](examples/tasks.rald).

## Standard library

Everything else is ordinary Emerald in [`stdlib/`](stdlib/), resolved with no
flags:

```
import strings
from result import Result, unwrap_or

def parse_flag(arg: str) -> Result[{ name: str, val: str }, str] {
    p = strings.partition(arg, "=")
    if p.found == False { return { ok: False, err: "expected name=value" } }
    return { ok: True, val: { name: p.before, val: p.after } }
}
```

Eleven modules: `result` (errors as values — there are no exceptions), `chars`,
`strings`, `builder`, `lists`, `sort`, `math` (including `exp`/`log`/`sin`
written in Emerald), `io`, `sys`, `path`, and `fmt`. Dictionaries and sets are
builtin runtime values constructed with `dict()` and `set()`.

Its shape is Python's; its signatures are not, because there are no methods and
no exceptions. It is scoped to the compiler, runtime, examples, and tests. See
[`stdlib/SPEC.md`](stdlib/SPEC.md) for the maintained module inventory and
conventions.

## Modules

A program can span several files. A `.rald` file is a module, and `import`
names code in another one:

```
import strings                    # module object: strings.split(...)
import text.strings as ts         # dotted paths map to directories
from strings import split, join   # names lifted into this module
```

Module paths resolve against the importing file's directory, then the project's
`src/` root, then each `-I <dir>` in the order given, then the standard library
— first hit wins, so a project can shadow a stdlib module with its own. A leading
underscore makes a top-level name private; everything else is exported. The
compiler loads the whole import graph and links it into one program, mangling
each imported module's top-level names to `<module>__<name>`, so two packages
can both define `parse`.

The `-I` / `-o` command line (see [Pipeline](#pipeline)) is the whole contract
between `emeraldc` and any package manager driving it. See
[`docs/modules.md`](docs/modules.md).

## Diagnostics

Errors are **structured diagnostics** with a stable machine-readable code, a
precise `file:line:column`, the offending source line with a caret, and — for
type mismatches — the expected and actual types as separate fields.

```
error[E_TYPE_RETURN]: returning str from a function declared to return int
  --> foo.rald:3:5
    |
 3 |     return y
   |     ^
   = expected: int
   = actual:   str
```

Pass `--json` to any mode to emit the same diagnostics as JSON (one object per
error, with `kind`, `code`, `line`, `column`, `message`, `expected`, `actual`,
and `source_line`). This is designed to be fed back to a tool — or an LLM —
that fixes the program and re-runs. Runtime errors in compiled programs also
report their source location:

```
emerald: runtime error: division by zero (at foo.rald:7)
```

See [`docs/diagnostics.md`](docs/diagnostics.md) for the code reference and the
JSON schema.

---

## Quick start

Requires a C compiler and [go-task](https://taskfile.dev) (`brew install go-task`).

```sh
task                 # build bin/emeraldc
task test            # golden suite: lexer/parser/check/json/proof/e2e/imports/...
task bench           # run benchmark workloads with informational timings
task examples        # compile & run the smoke examples

bin/emeraldc examples/fib.rald && ./examples/fib
```

Install a release build (`PREFIX` defaults to `/usr/local`):

```sh
task install PREFIX=/usr/local   # emeraldc -> $PREFIX/bin, stdlib -> $PREFIX/lib/emerald/stdlib
task dist                        # build emerald-1.0.0.tar.gz (smoke-test before release)
```

The compiler searches for its stdlib with no `-I` flag: `$EMERALD_STDLIB`
overrides when set, otherwise it looks next to the executable
(`../stdlib`, `../lib/emerald/stdlib`, …) and finally the compile-time default.
`emeraldc --version` prints the version.

## Pipeline

```
foo.rald ─► lexer ─► parser ─► modules ─► type checker ─► C codegen ─► cc ─► ./foo
```

Every stage has a driver flag and its own golden test directory under `tests/`.
No stage is observable only through the stage after it.

```
emeraldc [--emit-tokens|--emit-ast|--emit-shapes|--check|--emit-c]
         [--json] [--proof] [--shape-report] [--keep-c] [-I DIR]... [-o OUT] file.rald
```

See [`docs/architecture.md`](docs/architecture.md).

## Language status

The Python-shaped expression layer now includes tuples, list/set/dict
comprehensions, dynamic dict and set literals, slicing (including omitted
bounds and steps), default and keyword arguments, f-strings, and integer
bitwise operators. Records remain available with their existing structural
syntax; `{name: value}` is a record field when the key is an identifier, while
quoted or computed keys form dictionaries. `>>` remains function composition
for function values and is also numeric right shift for integer operands.

Two semantic choices worth knowing before you rely on them:

- **Integer arithmetic wraps.** `9223372036854775807 + 1` is
  `-9223372036854775808`, with no diagnostic: `int` is fixed-width
  two's-complement (64-bit), `float` is IEEE-754 double.
- **`dict`/`set` are builtin runtime values.** Use Python-style `dict()` and
  `set()` constructors, indexing/assignment for dictionaries, `in` for
  membership, and `|`, `&`, `-`, or `^` for set operations. Dictionaries remain
  string-keyed because there is no general "hashable key" constraint to hang
  a `dict[K, V]` on.

---

## The experiment: a proof-carrying ray tracer

The largest Emerald program is a re-implementation of *Ray Tracing in One
Weekend* — [`examples/ray_tracer/`](examples/ray_tracer/) — and it exists to
answer a question, not to render an image.

It comes in two versions. `one_weekend.rald` is a 280-line transliteration that
uses types as documentation; it renders the book's final image and stresses the
GC. `typed/` is the same program rewritten across 13 modules, where every
implicit invariant in the book was first **written down as a proposition**, then
encoded if the type system could hold it.

The scorecard records the properties checked by the typed program:

| | |
|---|---|
| **6 provable** | primitive dispatch is exhaustive (`never`); colour channels are exactly `r\|g\|b` (literal unions); a miss can never be read as a hit (`Hit \| None` + narrowing); a failed scatter carries no fake data; points and directions cannot be confused (`padd(p: Pt, d: Dir)` — `padd(p, p)` is a compile error) |
| **1 partial** | `reflect`/`refract` get unit-length input — the `Unit` brand is checked but forgeable by hand |

The type system also catches ordinary mistakes, including a rejection sampler
that failed to rebind its generator and a defocus disk that was 20× too large.

Read [`typed/README.md`](examples/ray_tracer/typed/README.md) for the full table,
the deviations, and the performance cost of the brands (~10%).

[`docs/proofs.md`](docs/proofs.md) is the general account — proof by exhaustive
case analysis, by enumeration, by parametricity, by impossibility — with the
and
[`examples/proofs.rald`](examples/proofs.rald) is a runnable tour.

---

## Layout

| Path | What |
|---|---|
| `include/` | compiler headers |
| `src/` | compiler (`lexer` → `parser` → `module` → `check` → `codegen` → `main`, plus `diag`) and the runtime implementation (Value model + GC, compiled into every program) |
| `docs/` | **start at [`docs/README.md`](docs/README.md)** — language reference and implementation notes |
| `stdlib/` | the standard library, in Emerald — **start at [`stdlib/SPEC.md`](stdlib/SPEC.md)** |
| `tests/` | golden tests per stage (`lexer`, `parser`, `check`, `json`, `proof`, `e2e`, `imports`, `stdlib`, `repl`, `shape`, benchmark regressions, warnings, proof report) + `run_tests.sh` |
| `examples/` | runnable programs — `shapes.rald` (structural typing), `proofs.rald` (proof features), `gc_stress.rald` (the collector), `functional/` (a seven-part tour of the functional core), `modules/` (a multi-file program), `ray_tracer/` (the experiment), `mlp/` (a hand-written MLP trained on XOR, and its `shape_bug` compile error) |
| `Taskfile.yml` | build / test / examples / install / dist / bless / clean |
