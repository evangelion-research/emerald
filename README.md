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

## Modules

A program can span several files. A `.rald` file is a module, and `import`
names code in another one:

```
import strings                    # module object: strings.split(...)
import text.strings as ts         # dotted paths map to directories
from strings import split, join   # names lifted into this module
```

Module paths resolve against the importing file's directory, then the project's
`src/` root, then each `-I <dir>` in the order given — first hit wins. A leading
underscore makes a top-level name private; everything else is exported. The
compiler loads the whole import graph and links it into one program, mangling
each imported module's top-level names to `<module>__<name>`, so two packages
can both define `parse`.

```
emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald
```

That command line is the whole contract between `emeraldc` and any package
manager driving it. See [`docs/modules.md`](docs/modules.md).

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
task test            # 87 golden tests across 6 stage suites (incl. proof mode)
task examples        # compile & run every example

bin/emeraldc examples/fib.rald && ./examples/fib
```

## Pipeline

```
foo.rald ─► lexer ─► parser ─► modules ─► type checker ─► C codegen ─► cc ─► ./foo
```

Every stage has a driver flag (`--emit-tokens`, `--emit-ast`, `--check`,
`--emit-c`, `--keep-c`, `--json`) and its own golden test directory under
`tests/`. No stage is observable only through the stage after it. See
[`docs/architecture.md`](docs/architecture.md).

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

The blockers are stated plainly there: no tensors, no indexed types, no
induction, no termination checking, `any` as a universal solvent, unsound
covariant lists, no effects. The tracks that follow — shape types, effects and
purity, interpretations as first-class typed objects, approximate `(ε, δ)`
judgments, a proof fragment that actually means something — are ordered by how
much of that they unblock.

---

## Layout

| Path | What |
|---|---|
| `include/` | compiler headers |
| `src/` | compiler (`lexer` → `parser` → `module` → `check` → `codegen` → `main`, plus `diag`) and `runtime.c` (Value model + GC, compiled into every program) |
| `docs/` | **start at [`docs/README.md`](docs/README.md)** — grammar, type system, builtins, modules, diagnostics, architecture, GC, proofs, research directions |
| `tests/` | golden tests per stage (`lexer`, `parser`, `check`, `e2e`, `imports`) + `run_tests.sh` |
| `examples/` | runnable programs — `shapes.rald` (structural typing), `proofs.rald` (proof features), `gc_stress.rald` (the collector), `modules/` (a multi-file program), `ray_tracer/` (the experiment) |
| `Taskfile.yml` | build / test / examples / bless / clean |
