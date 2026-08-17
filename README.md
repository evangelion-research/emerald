# Emerald

A Python-flavored language with **braces instead of indentation**,
**TypeScript-style structural typing** instead of classes, a
**two-generation mark-and-sweep GC**, and a compiler written in modern C11
that emits native binaries via your system `cc`.

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

counter_state = [0]
def make_counter() -> (int) -> int {
    def inc(d: int) -> int { counter_state[0] = counter_state[0] + d return counter_state[0] }
    return inc
}
```

Read [`docs/type-system.md`](docs/type-system.md) for the full reference and
[`docs/proofs.md`](docs/proofs.md) for using the checker to state and verify
mathematical arguments — with an honest account of where it stops.
[`examples/proofs.rald`](examples/proofs.rald) is a runnable tour.

## Quick start

Requires a C compiler and [go-task](https://taskfile.dev) (`brew install go-task`).

```sh
task                 # build bin/emeraldc
task test            # run all 4 stage test suites
task examples        # compile & run examples/*.rald

bin/emeraldc examples/fib.rald && ./examples/fib
```

## Pipeline

```
foo.rald ─► lexer ─► parser ─► type checker ─► C codegen ─► cc ─► ./foo
```

Every stage has a driver flag (`--emit-tokens`, `--emit-ast`, `--check`,
`--emit-c`, `--keep-c`) and its own golden test directory under `tests/`.

## Diagnostics

Errors are reported as **structured diagnostics** with a stable machine-readable
code, a precise `file:line:column`, the offending source line with a caret, and
— for type mismatches — the expected and actual types as separate fields.

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
error, including `kind`, `code`, `line`, `column`, `message`, `expected`,
`actual`, and the `source_line`). This is designed to be fed back into an LLM
(or another tool) to fix the program and re-run. Runtime errors in compiled
programs also report their source location:

```
emerald: runtime error: division by zero (at foo.rald:7)
```

See [`docs/diagnostics.md`](docs/diagnostics.md) for the error-code reference
and the JSON schema.

## Modules

A program can span several files. A `.rald` file is a module, and `import` names
code in another one:

```
import strings                    # module object: strings.split(...)
import text.strings as ts         # dotted paths map to directories
from strings import split, join   # names lifted into this module
```

Module paths resolve against the importing file's directory, then the project's
`src/` root, then each `-I <dir>` in the order given — first hit wins. A leading
underscore makes a top-level name private; everything else is exported. The
compiler loads the whole import graph and links it into one program, mangling
each imported module's top-level names to `<module>__<name>`, so two packages can
both define `parse`.

```
emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald
```

That command line is the whole contract between `emeraldc` and any package
manager driving it. See [`docs/modules.md`](docs/modules.md).

## Layout

| Path            | What                                                    |
|-----------------|---------------------------------------------------------|
| `include/`      | compiler headers (`.h` files)                           |
| `src/`          | compiler implementation (`.c` files: `lexer` → `parser` → `module` → `check` → `codegen` → `main`) and `runtime.c` (Value model + GC, compiled into every program) |
| `docs/`         | grammar, plan, `type-system.md`, `proofs.md`, `gc.md`, `diagnostics.md`, `modules.md`, `research-directions.md` (where the language is headed) |
| `tests/`        | golden tests per stage + `run_tests.sh`                 |
| `examples/`     | runnable programs (`shapes.rald` shows structural typing, `proofs.rald` the proof features, `gc_stress.rald` the collector) |
| `Taskfile.yml`  | build/test/examples/bless/clean                         |
