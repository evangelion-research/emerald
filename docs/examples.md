# Examples

The examples are small runnable programs, not a separate standard library or
benchmark suite. Build the compiler first, then run every smoke example with:

```sh
task examples
```

That task compiles each top-level `examples/*.rald` file and each nested
`examples/*/main.rald` entry, then runs the resulting binary with stdin closed.
For an individual program:

```sh
bin/emeraldc -o /tmp/emerald-hello examples/hello.rald
/tmp/emerald-hello
```

## Top-level examples

| Example | Demonstrates |
|---|---|
| [`hello.rald`](../examples/hello.rald) | Minimal program and output |
| [`fib.rald`](../examples/fib.rald) | Functions, recursion, and arithmetic |
| [`fizzbuzz.rald`](../examples/fizzbuzz.rald) | Loops, ranges, and conditionals |
| [`lists.rald`](../examples/lists.rald) | Lists and collection operations |
| [`strings_tour.rald`](../examples/strings_tour.rald) | String and standard-library helpers |
| [`file_io.rald`](../examples/file_io.rald) | File operations |
| [`console.rald`](../examples/console.rald) | EOF-safe console input |
| [`errors.rald`](../examples/errors.rald) | `error`, `Result`, `try`, and `catch` |
| [`pattern_match.rald`](../examples/pattern_match.rald) | Exhaustive pattern matching |
| [`proofs.rald`](../examples/proofs.rald) | `never`, literal unions, and proof-mode ideas |
| [`shapes.rald`](../examples/shapes.rald) | Structural records and intersections |
| [`tasks.rald`](../examples/tasks.rald) | Cooperative tasks and typed channels |
| [`testing.rald`](../examples/testing.rald) | Assertion-style checks in Emerald |
| [`gc_stress.rald`](../examples/gc_stress.rald) | Garbage-collector allocation pressure |

## Grouped examples

- [`functional/`](../examples/functional/) contains focused programs for
  lambdas, closures, callbacks, composition, higher-order functions, and lazy
  thunks. Its entry point is `functional/main.rald`.
- [`ray_tracer/one_weekend.rald`](../examples/ray_tracer/one_weekend.rald) is
  the compact renderer.
- [`ray_tracer/typed/`](../examples/ray_tracer/typed/) is the modular typed
  renderer. Its entry point is `main.rald`; it writes `out.ppm` and uses an
  explicit RNG state so the same seed produces the same output.

The ray tracer is intentionally a research example rather than a supported
application. Its typed version uses `partial` at runtime-boundary points that
are outside the current proof fragment; see its
[`README.md`](../examples/ray_tracer/typed/README.md).

## Related checks

The executable contracts for language features live under [`tests/`](../tests/),
not under `examples/`. Use the focused tasks when investigating a feature:

```sh
task test:e2e
task test:stdlib
task test:proof
task test:shape
```
