# Emerald v1 — current release surface

Emerald v1 is the compiler, runtime, standard library, examples, and tests in
this repository. The implementation is written in C11 and emits native
binaries through the system `cc`.

## Implemented surface

| Area | Status |
|---|---|
| Compiler | Lexer, parser, module loader, checker, C code generator, and CLI |
| Type system | Structural records, unions/intersections, literals, narrowing, generics, `list`/`seq`, `never`, exhaustiveness, purity, and proof mode |
| Runtime | Tagged values, closures, tensors, cooperative tasks/channels, and a two-generation precise GC |
| Collections | Dynamic string-keyed `dict()` and `set()` values; indexing, iteration, membership, comprehensions, and set operators |
| Standard library | 11 Emerald modules covered by `tests/stdlib/` |
| Tooling | Stage emission, human/JSON diagnostics, warnings, proof reports, REPL, `--help`, `--version`, installation, and archive tasks |
| Tests | Golden tests for lexer/parser/check/json/proof/e2e/imports/stdlib/REPL/shape/warnings/reports/CLI |

## Resolved behavior

- `seq[T]` is immutable and covariant. `freeze(xs)` creates the read-only
  sequence boundary and `thaw(s)` copies back to a mutable list.
- Mutable `list[T]` compatibility is covariant in ordinary code and emits
  `W_UNSOUND_COVARIANCE`; proof mode makes named mutable lists invariant.
- Tuples, comprehensions, dictionary and set literals, slicing, default and
  keyword arguments, f-strings, floor division, exponentiation, compound
  assignment, and integer bitwise operators are supported.
- Dictionaries are string-keyed. Tensor operations support `f32` and `f64` and
  use the dimension and shape checker described in [`shapes.md`](shapes.md).

## Verification

From the repository root:

```sh
task build
task test
task runtime-check
task examples
```

The test runner covers compiler stages, runtime behavior, modules, the standard
library, the REPL, shape checking, benchmark regressions, warnings, proof
reports, and CLI behavior. `task bench` reports end-to-end timings without
performance thresholds.
