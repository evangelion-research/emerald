# Emerald

A Python-flavored language with **braces instead of indentation**,
**TypeScript-style structural typing** instead of classes, a **mark-and-sweep
GC**, and a compiler written in modern C11 that emits native binaries via
your system `cc`.

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
| `src/`          | compiler (`lexer` → `parser` → `check` → `codegen` → `main`) and `runtime.c` (Value model + GC, compiled into every program) |
| `docs/`         | grammar, plan, `type-system.md`, `gc.md`                |
| `tests/`        | golden tests per stage + `run_tests.sh`                 |
| `examples/`     | runnable programs (`shapes.rald` shows structural typing, `gc_stress.rald` the collector) |
| `Taskfile.yml`  | build/test/examples/bless/clean                         |
