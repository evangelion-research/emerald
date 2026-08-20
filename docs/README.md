# Emerald documentation

This directory contains maintained documentation for the compiler that is in
this repository. Start with the document matching the job you have.

## Use Emerald

| Document | Contents |
|---|---|
| [`grammar.md`](grammar.md) | Lexical rules, syntax, precedence, records versus dictionaries/sets, comprehensions, slicing, calls, and f-strings. |
| [`type-system.md`](type-system.md) | Structural records, unions/intersections, generics, narrowing, `list`/`seq`, totality, purity, and proof mode. |
| [`errors.md`](errors.md) | `error` declarations, `Result`, `try`, `catch`, and checked error propagation. |
| [`builtins.md`](builtins.md) | The builtin API, runtime behavior, collections, sequences, I/O, concurrency, and tensors. |
| [`modules.md`](modules.md) | Import syntax, search order, privacy, linking, name mangling, and module diagnostics. |
| [`diagnostics.md`](diagnostics.md) | Human/JSON diagnostic formats, error codes, warnings, and runtime errors. |
| [`concurrency.md`](concurrency.md) | Cooperative tasks, channels, scheduling, blocking, closing, and deadlocks. |
| [`repl.md`](repl.md) | REPL behavior, commands, effects, and scratch-session details. |

## Understand the implementation

| Document | Contents |
|---|---|
| [`architecture.md`](architecture.md) | Compiler pipeline, generated C boundary, runtime values, driver flags, and tests. |
| [`gc.md`](gc.md) | Precise two-generation GC, shadow-stack roots, write barriers, and task roots. |
| [`effects.md`](effects.md) | The implemented purity/effect model and its deliberate limits. |
| [`proofs.md`](proofs.md) | What proof mode can establish and where the checker remains gradual or incomplete. |
| [`tensors.md`](tensors.md) | Tensor runtime representation, operations, views, dtypes, and GC interaction. |
| [`shapes.md`](shapes.md) | Dimension expressions, `Fin`, static shape obligations, and dynamic boundaries. |

## Release and maintenance

| Document | Contents |
|---|---|
| [`RELEASE_V1.md`](RELEASE_V1.md) | What is complete for the v1 candidate and the intentional v1 omissions. |
| [`REMAINING_V1.md`](REMAINING_V1.md) | Adversarial release audit: packaging, security, correctness, testing, and follow-up risks. |
| [`../stdlib/SPEC.md`](../stdlib/SPEC.md) | Maintained standard-library inventory, conventions, and known limits. |

## Setup and verification

From the repository root:

```sh
task                 # build bin/emeraldc
task test            # compiler, runtime, stdlib, REPL, shape, warning, report, and CLI tests
task examples        # compile and run the smoke examples
task runtime-check   # compile src/runtime_*.c standalone
```

For installation and archive behavior, use the `install` and `dist` tasks only
after reading the release audit. In particular, the current archive is a
packaging task, not yet a clean-machine release guarantee; follow the acceptance
steps in `REMAINING_V1.md` before calling it relocatable.

The maintained source of truth is the compiler and its tests. Older planning
memos and research bibliographies were intentionally removed from the v1 tree;
release decisions belong in `RELEASE_V1.md` and concrete remaining risks belong
in `REMAINING_V1.md`.
