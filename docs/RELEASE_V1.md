# Emerald v1 — release readiness

This document describes the current release surface and the checks required
before tagging `v1.0.0`. It is intentionally short; adversarial implementation
risks and acceptance tests live in [`REMAINING_V1.md`](REMAINING_V1.md).

## Current surface

| Area | Status |
|---|---|
| Compiler | C11 lexer, parser, module loader, checker, C code generator, and CLI |
| Type system | Structural records, unions/intersections, literals, narrowing, generics, `list`/`seq`, `never`, exhaustiveness, purity, and proof mode |
| Runtime | Tagged values, closures, tensors, cooperative tasks/channels, and a two-generation precise GC |
| Collections | Dynamic string-keyed `dict()` and `set()` runtime values; indexing, iteration, membership, comprehensions, and set operators |
| Standard library | 11 Emerald modules, tested through `tests/stdlib/` |
| Tooling | Stage emission, human/JSON diagnostics, warnings, proof reports, REPL, `--help`, `--version`, installation, and archive tasks |
| Tests | Golden tests for lexer/parser/check/json/proof/e2e/imports/stdlib/REPL/shape/warnings/reports/CLI; the last recorded run passed 185 cases |

## Resolved design decisions

### B1/B2: sequence variance

- `seq[T]` is immutable and covariant in every mode.
- `freeze(xs)` creates the read-only sequence boundary; `thaw(s)` copies back to
  a mutable list.
- Ordinary mutable `list[T]` compatibility remains covariant but warns with
  `W_UNSOUND_COVARIANCE`.
- `--proof` makes named mutable lists invariant while preserving fresh-literal
  widening and the explicit `any` escape hatch.

The behavior is covered by `tests/e2e/seq.rald`, checker/proof cases, warning
cases, and their blessed outputs.

### Python-shaped expression surface

The v1 parser and runtime support tuples, list/set/dict comprehensions,
dictionary and set literals, slicing, default and keyword arguments, f-strings,
floor division, exponentiation, compound assignment, and integer bitwise
operators. Dictionaries remain string-keyed because the type system has no
bounded `Hashable` constraint.

## Intentional v1 omissions

These are documented boundaries, not unfinished syntax:

- methods and chained comparisons;
- channel `select`;
- Unicode-aware character operations (strings are byte-oriented);
- general hashable dictionary keys or static `dict`/`set` type constructors;
- formatter, syntax-highlighting, and editor integrations;
- separate compilation and a self-hosted compiler.

Useful follow-up work must first satisfy the release audit in
[`REMAINING_V1.md`](REMAINING_V1.md), rather than expanding this list
speculatively.

## Verification

From a clean checkout:

```sh
task --force build
task --force test
task runtime-check
task examples
git diff --check
```

Before calling an archive or installation relocatable, perform the clean-machine
smoke test described in `REMAINING_V1.md`: extract/install it outside the source
checkout, invoke the compiler through `PATH`, compile a program importing the
stdlib, and run it with `EMERALD_SRC` and `EMERALD_STDLIB` unset.

The release is ready for the manual tag only after the P0 items in the audit are
closed or explicitly accepted: packaging, CI policy, stage exit statuses, shell
and temporary-path safety, and sanitizer coverage for runtime boundaries.
