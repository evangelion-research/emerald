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

Emerald is being built for a reason beyond having another scripting language:
**to find out how much of a real program's meaning a practical type system can
be made to hold** — and to grow, from that answer, a language in which neural
networks can be written *and* studied through machine-checked interpretation.
The language below is the substrate; [where it's going](#where-this-is-going)
is the point.

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
  recursive-alias type (`n.succ`, `xs.tail`). A function whose recursion can't
  be shown to descend must say `partial`, which marks it as *not* a proof.
- **`pure`** — `def forward(x: T) -> U pure` promises no `print`, no `rand`,
  no file or process IO, and no impure callee (a nested `def` inside a pure
  function must itself be pure). Thirty-eight of the fifty-two builtins are
  pure and may be called from pure code, as is roughly half the standard
  library — everything that reads rather than builds.
- **`--proof`** — `emeraldc --check --proof f.rald` bans `any` and `partial`.
  Since `any` is assignable in both directions, a proof that mentions it proves
  nothing, so proof mode rejects every `any` that surfaces: unannotated
  parameters and returns, explicit annotations, inferred `any`. It is a strict
  first cut, not a complete soundness guarantee — the element type of an empty
  `[]` is still `any` underneath.

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

## Builtins

Fifty-two builtins compile straight into runtime calls and are in scope in
every module. Twenty-seven are the core set: `print`, `eprint`, `len`, `range`,
`str`, `int`, `float`, `sqrt`, `tan`, `rand`, `append`, `slice`, `ord`, `chr`,
`argv`, `exit`, `map`, `filter`, `reduce`, `read_file`, `read_file_opt`,
`file_exists`, `write_file`, `append_file`, `run`, `gc_stats`, `gc_collect`.
The other twenty-five are the Phase 2 tensor primitives: `zeros`, `ones`,
`full`, `arange`, `tensor`, `randn`, `exp`, `log`, `tanh`, `relu`, `matmul`,
`reshape`, `transpose`, `permute`, `expand`, `sum`, `mean`, `max`, `argmax`,
`tslice`, `item`, `shape`, `ndim`, `dtype`, `astype`. They are not values
(`f = print` is an error — there is no closure to hand out) and cannot be
redefined.

A builtin exists only when it cannot be written in Emerald: allocation-level
(`append`, `slice`, `len`, `range`), formatting (`str`, `print`), foreign
(`read_file`, `argv`, `run`), privileged (`gc_stats`), or a tensor shape
obligation (`matmul`, `reshape`). `append` is the load-bearing core one —
without amortized in-place growth, every list built in a loop is quadratic.
See [`docs/builtins.md`](docs/builtins.md), [`docs/tensors.md`](docs/tensors.md),
and [`docs/shapes.md`](docs/shapes.md).

## Standard library

Everything else is ordinary Emerald in [`stdlib/`](stdlib/), resolved with no
flags:

```
import strings
import dict
from result import Result, unwrap_or

def parse_flag(arg: str) -> Result[{ name: str, val: str }, str] {
    p = strings.partition(arg, "=")
    if p.found == False { return { ok: False, err: "expected name=value" } }
    return { ok: True, val: { name: p.before, val: p.after } }
}
```

Twelve modules: `result` (errors as values — there are no exceptions), `chars`,
`strings`, `builder`, `lists`, `sort`, `dict` (a real hash table, because
records cannot take dynamic keys), `set`, `math` (including `exp`/`log`/`sin`
written in Emerald), `io`, `sys`, `path`.

Its shape is Python's; its signatures are not, because there are no methods and
no exceptions. It was scoped to what a self-hosted compiler needs and nothing
else. See [`stdlib/SPEC.md`](stdlib/SPEC.md) — including §8, the three compiler
bugs and limitations writing it exposed.

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
that fixes the program and re-runs; see
[`docs/research-directions.md`](docs/research-directions.md) §11 for why that
matters. Runtime errors in compiled programs also report their source location:

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
task test            # 123 tests across 7 stage suites (incl. proof mode)
task examples        # compile & run every example

bin/emeraldc examples/fib.rald && ./examples/fib
```

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

The scorecard is the deliverable. Of 16 propositions:

| | |
|---|---|
| **6 provable** | primitive dispatch is exhaustive (`never`); colour channels are exactly `r\|g\|b` (literal unions); a miss can never be read as a hit (`Hit \| None` + narrowing); a failed scatter carries no fake data; points and directions cannot be confused (`padd(p: Pt, d: Dir)` — `padd(p, p)` is a compile error) |
| **1 partial** | `reflect`/`refract` get unit-length input — the `Unit` brand is checked but forgeable by hand |
| **7 out of reach** | ‖unit(v)‖ = 1; `lo ≤ hi` on intervals; colour ∈ [0,1]; `ray_color` terminates; the render is a pure function of the seed; image indices in bounds; the scene list is not aliased |

Those seven are the honest result, and they are not a wishlist — they were
produced by a real program that wanted them. They map one-to-one onto the
missing features: **opaque types, scalar refinements, effects, termination
checking, and shape types.** The type system also earned its keep in the
ordinary way, catching a rejection sampler that never rebound its generator and
a defocus disk that was 20× too large.

Read [`PLAN.md`](examples/ray_tracer/PLAN.md) for the design and
[`typed/README.md`](examples/ray_tracer/typed/README.md) for the full table,
the deviations, and the performance cost of the brands (~10%).

[`docs/proofs.md`](docs/proofs.md) is the general account — proof by exhaustive
case analysis, by enumeration, by parametricity, by impossibility — with the
same honesty about where it stops, and
[`examples/proofs.rald`](examples/proofs.rald) is a runnable tour.

## Where this is going

[`docs/research-directions.md`](docs/research-directions.md) is the agenda: a
gap analysis and ten research tracks aimed at one thesis —

> **models as morphisms, interpretations as typed refinements between them,
> obligations discharged exactly where possible and statistically where not,
> with a machine-checkable certificate at the end.**

The blockers are stated plainly there: no induction, no termination checking,
`any` as a universal solvent, unsound covariant lists, no effects. The tracks
that follow — shape types, effects and purity, interpretations as first-class
typed objects, approximate `(ε, δ)` judgments, a proof fragment that actually
means something — are ordered by how much of that they unblock.

Phase 2 has already landed the first of those tracks: **tensors and shape
types** ([`docs/tensors.md`](docs/tensors.md), [`docs/shapes.md`](docs/shapes.md)).
A shape bug is now a compile error with both shapes printed, an MLP trains on
XOR end-to-end ([`examples/mlp/`](examples/mlp/)), and `Fin[n]` rejects a
provably out-of-range index at compile time.

---

## Layout

| Path | What |
|---|---|
| `include/` | compiler headers |
| `src/` | compiler (`lexer` → `parser` → `module` → `check` → `codegen` → `main`, plus `diag`) and `runtime.c` (Value model + GC, compiled into every program) |
| `docs/` | **start at [`docs/README.md`](docs/README.md)** — grammar, type system, builtins, modules, diagnostics, architecture, GC, proofs, research directions, plus the Phase 2 [`tensors.md`](docs/tensors.md) and [`shapes.md`](docs/shapes.md) |
| `stdlib/` | the standard library, in Emerald — **start at [`stdlib/SPEC.md`](stdlib/SPEC.md)** |
| `tests/` | golden tests per stage (`lexer`, `parser`, `check`, `e2e`, `imports`, `stdlib`, `shape`) + `run_tests.sh` |
| `examples/` | runnable programs — `shapes.rald` (structural typing), `proofs.rald` (proof features), `gc_stress.rald` (the collector), `functional/` (a seven-part tour of the functional core), `modules/` (a multi-file program), `ray_tracer/` (the experiment), `mlp/` (a hand-written MLP trained on XOR, and its `shape_bug` compile error) |
| `Taskfile.yml` | build / test / examples / bless / clean |
