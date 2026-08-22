# Emerald

<p align="center">
  <img src="assets/emerlad_v1.jpeg" alt="Emerald programming language logo" width="704">
</p>

Emerald is a statically typed programming language with Python-inspired syntax,
brace-delimited blocks, and TypeScript-style structural typing. Its C11 compiler
generates native executables through the system C compiler.

Key capabilities include:

- Structural records, unions, intersections, literal types, and generics
- Flow-sensitive narrowing and exhaustive case analysis
- Explicit, typed error handling without exceptions
- Totality and purity checking, with a stricter proof mode
- First-class functions, closures, pipelines, and tail-call optimization
- Cooperative tasks, typed channels, and statically checked tensor dimensions
- A precise, two-generation mark-and-sweep garbage collector
- Human-readable and machine-readable diagnostics

```rald
type Point  = { x: int, y: int }
type Point3 = Point & { z: int }

def magnitude_squared(p: Point) -> int {
    return p.x * p.x + p.y * p.y
}

p: Point3 = { x: 3, y: 4, z: 5 }
print(magnitude_squared(p))
```

`Point3` is assignable to `Point` because it contains the required fields.
Types are erased during compilation, so structural subtyping introduces no
runtime dispatch overhead.

## Project status

Emerald is an experimental language and compiler for programming-language
research, particularly type-driven verification and machine-checked numerical
software. This repository contains the compiler, runtime, standard library,
documentation, examples, and regression tests.

The current release version is `1.0.0`. See
[`docs/RELEASE_V1.md`](docs/RELEASE_V1.md) for the implemented language and
tooling surface.

## Requirements

- A C11-compatible compiler available as `cc`
- [Task](https://taskfile.dev/) (`brew install go-task` on macOS)

## Build and test

```sh
task                 # build bin/emeraldc
task test            # run the complete test suite
task examples        # compile and run the examples
task bench           # run benchmark workloads
task runtime-check   # validate the runtime with strict compiler flags
```

Compile and run a program:

```sh
bin/emeraldc examples/fib.rald
./examples/fib
```

Install a release build (`PREFIX` defaults to `/usr/local`):

```sh
task install PREFIX=/usr/local
task dist
```

The compiler locates the standard library automatically. Set
`EMERALD_STDLIB` to override its location.

## Type system

Emerald supports structural records, union and intersection types, literal
types, generic functions and aliases, and flow-sensitive narrowing.

```rald
type Circle = { kind: "circle", radius: int }
type Square = { kind: "square", side: int }
type Shape  = Circle | Square

def area(shape: Shape) -> int {
    if shape.kind == "circle" { return shape.radius * shape.radius * 3 }
    if shape.kind == "square" { return shape.side * shape.side }
    unreachable: never = shape
    return 0
}

def head[T](items: list[T]) -> T { return items[0] }
```

Assigning `shape` to `never` succeeds only when the preceding branches cover
every member of the union. Adding another shape therefore produces a compile
error at each non-exhaustive use site. See
[`docs/type-system.md`](docs/type-system.md).

## Typed error handling

Fallible functions declare their errors in a `Result` return type. Callers must
handle or propagate every declared failure.

```rald
error NotFound { key: str }
error Malformed { key: str, value: str }

def field(key: str) -> Result[str, NotFound | Malformed] { ... }

def port() -> Result[int, NotFound | Malformed] {
    const raw = try field("port")
    return ok(int(raw))
}

const value = catch port() {
    NotFound error -> 80
    Malformed error -> 0 - 1
}
```

`try` propagates an error to the caller. `catch` is an expression whose arms
must exhaust the error union. Errors are ordinary tagged records; handling does
not require stack unwinding or hidden control flow. See
[`docs/errors.md`](docs/errors.md) and [`stdlib/result.rald`](stdlib/result.rald).

## Totality, purity, and proof mode

Emerald can verify additional program properties:

- Functions are total by default. Recursive calls must make a structurally
  smaller argument; other functions must be declared `partial`.
- A `pure` function cannot perform I/O, use randomness, or call an impure
  function, including indirectly through a higher-order function.
- `seq[T]` is immutable and covariant. `list[T]` remains mutable and becomes
  invariant in proof mode.
- `Eq[a, b]` represents equality between dimension expressions and can justify
  tensor shape equivalence across function boundaries.
- `--proof` rejects `any`, `partial`, unsupported recursion, and loops whose
  termination cannot be established.

```sh
bin/emeraldc --check --proof program.rald
bin/emeraldc --check --proof --proof-report program.rald
bin/emeraldc --check --proof --proof-report --json program.rald
```

See [`docs/proofs.md`](docs/proofs.md) and
[`docs/effects.md`](docs/effects.md).

## Functions and closures

Functions are first-class values. Function types use `(A, B) -> C` notation,
and nested functions capture variables from their enclosing scope.

```rald
def make_adder(value: int) -> (int) -> int {
    def add(input: int) -> int { return input + value }
    return add
}

add5 = make_adder(5)
print(add5(1))
```

The language also provides immutable `const` bindings, lambdas, `map`,
`filter`, `reduce`, pipelines (`|>`), function composition (`>>`), exhaustive
`match` expressions, and thunks. Direct tail calls execute in constant stack
space. See [`examples/functional/`](examples/functional/).

## Modules and standard library

Each `.rald` file defines a module. Imports support module objects, dotted
paths, aliases, and selective name imports.

```rald
import strings
import text.strings as text_strings
from result import Result, unwrap_or
```

Modules are resolved relative to the importing file, followed by the project
source root, each `-I` directory, and the standard library. Top-level names
beginning with `_` are private; all others are exported.

The standard library provides `result`, `chars`, `strings`, `builder`, `lists`,
`sort`, `math`, `io`, `sys`, `path`, and `fmt`. See
[`docs/modules.md`](docs/modules.md) and [`stdlib/SPEC.md`](stdlib/SPEC.md).

## Built-in operations

Built-ins provide functionality requiring runtime, allocation, foreign-system,
or type-checker support. They include core collection and conversion operations,
file and process I/O, cooperative tasks, channels, tensors, and the `dict()` and
`set()` constructors.

Built-ins are not first-class function values and cannot be redefined. Their
authoritative definition is [`include/builtins.def`](include/builtins.def). See
[`docs/builtins.md`](docs/builtins.md), [`docs/tensors.md`](docs/tensors.md),
and [`docs/shapes.md`](docs/shapes.md).

## Concurrency

Emerald provides cooperative tasks and typed channels. One task executes at a
time; scheduling occurs at channel operations, `sleep`, `join`, or an explicit
`task_yield()` call.

```rald
jobs: Chan[int] = chan(8)
worker_task = spawn(() => worker("worker-0"))
for value in range(2, 40) { send(jobs, value) }
chan_close(jobs)
print("handled", join(worker_task))
```

`Chan[T]` validates transmitted values, `recv` returns `T | None` for a closed
channel, and `Task[T]` preserves the task's return type. The runtime reports
deadlocks instead of waiting indefinitely. See
[`docs/concurrency.md`](docs/concurrency.md).

## REPL

Start an interactive session with `bin/emeraldc --repl`. The REPL records each
successful entry as source, recompiles the complete session, and displays the
latest entry's output. It therefore uses the same parser, checker, and compiler
as file-based programs. `:list`, `:undo`, and `:save FILE` manage the session.
See [`docs/repl.md`](docs/repl.md).

## Diagnostics

Compiler diagnostics contain a stable code, source location, excerpt, and
structured expected and actual values when applicable.

```text
error[E_TYPE_RETURN]: returning str from a function declared to return int
  --> example.rald:3:5
    |
 3 |     return value
   |     ^
   = expected: int
   = actual:   str
```

Pass `--json` to emit one JSON object per diagnostic for editor, CI, and other
tooling integrations. Runtime errors also include their Emerald source
location. See [`docs/diagnostics.md`](docs/diagnostics.md).

## Compiler pipeline

```text
source -> lexer -> parser -> modules -> type checker -> C codegen -> cc -> executable
```

Each stage has a corresponding command-line mode and golden test suite:

```text
emeraldc [--emit-tokens|--emit-ast|--emit-shapes|--check|--emit-c]
         [--json] [--proof] [--shape-report] [--keep-c]
         [-I DIR]... [-o OUT] file.rald
```

See [`docs/architecture.md`](docs/architecture.md).

## Language semantics

The expression syntax includes tuples, lists, dictionaries, sets,
comprehensions, slicing, default and keyword arguments, f-strings, and integer
bitwise operators. Important runtime semantics include:

- `int` is a signed 64-bit two's-complement value; arithmetic overflow wraps.
- `float` uses IEEE-754 double precision.
- Dictionaries use string keys.
- Set operations use `|`, `&`, `-`, and `^`.
- `>>` composes functions or shifts integers, depending on its operands.

See [`docs/grammar.md`](docs/grammar.md).

## Research example: proof-carrying ray tracer

[`examples/ray_tracer/`](examples/ray_tracer/) implements *Ray Tracing in One
Weekend* to evaluate which domain invariants Emerald can verify. It contains a
direct implementation and a typed, 13-module version encoding exhaustive
primitive dispatch, valid color channels, hit/miss separation, scatter-result
validity, and distinct point and direction types. Unit-vector validity uses a
forgeable brand and is therefore not a complete proof.

See [`examples/ray_tracer/typed/README.md`](examples/ray_tracer/typed/README.md)
for the analysis and [`examples/proofs.rald`](examples/proofs.rald) for runnable
proof examples.

## Repository structure

| Path | Description |
|---|---|
| [`include/`](include/) | Compiler and runtime headers and the built-in definition table |
| [`src/`](src/) | Compiler pipeline, runtime, diagnostics, and command-line interface |
| [`stdlib/`](stdlib/) | Standard library implemented in Emerald |
| [`docs/`](docs/) | Language reference and implementation documentation |
| [`examples/`](examples/) | Runnable language, concurrency, proof, tensor, and research examples |
| [`tests/`](tests/) | Golden, integration, runtime, proof, shape, and benchmark tests |
| [`Taskfile.yml`](Taskfile.yml) | Build, test, benchmark, installation, and distribution tasks |

Start with the [documentation index](docs/README.md) for detailed references.

## License

See [`LICENSE`](LICENSE).
