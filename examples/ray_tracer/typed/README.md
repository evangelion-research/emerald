# Typed ray tracer

A modular reimplementation of [Ray Tracing in One
Weekend](https://raytracing.github.io/books/RayTracingInOneWeekend.html) that
uses Emerald records, unions, closures, modules, narrowing, and explicit RNG
state. It is a demonstration program, not part of the compiler's bootstrap
path.

Build and run from the repository root:

```sh
./bin/emeraldc -o /tmp/emerald-raytracer examples/ray_tracer/typed/main.rald
/tmp/emerald-raytracer
```

The default render writes `out.ppm` (120×67, 4 samples per pixel, depth 16,
seed 42). Two runs with the same seed are byte-identical.

## What the example demonstrates

- Tagged unions and `never` exhaustiveness for primitive dispatch.
- `Hit | None` narrowing for intersection results.
- Structural records and intersections for point/direction/material data.
- Closures as material values.
- Module privacy and dependency ordering.
- Explicit RNG threading for deterministic rendering.
- `partial`/runtime-boundary limitations that remain outside the proof
  fragment.

The untyped [`one_weekend.rald`](../one_weekend.rald) remains the smaller
baseline and GC stress example. The typed version is intentionally more
verbose: its value is showing which invariants the current type system can and
cannot express. See the maintained language references in
[`docs/README.md`](../../../docs/README.md).
