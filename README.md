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

## Layout

| Path            | What                                                    |
|-----------------|---------------------------------------------------------|
| `include/`      | compiler headers (`.h` files)                           |
| `src/`          | compiler implementation (`.c` files: `lexer` → `parser` → `check` → `codegen` → `main`) and `runtime.c` (Value model + GC, compiled into every program) |
| `docs/`         | grammar, plan, `type-system.md`, `proofs.md`, `gc.md`   |
| `tests/`        | golden tests per stage + `run_tests.sh`                 |
| `examples/`     | runnable programs (`shapes.rald` shows structural typing, `proofs.rald` the proof features, `gc_stress.rald` the collector) |
| `Taskfile.yml`  | build/test/examples/bless/clean                         |
