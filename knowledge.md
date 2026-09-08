# Project Knowledge: Emerald Compiler

## What This Is

Emerald is a statically typed programming language with Python-inspired syntax and TypeScript-style structural typing. This repo contains the **C11 compiler** (`emeraldc`), runtime, standard library, documentation, and regression tests. The compiler generates native executables by shelling out to the system C compiler (`cc`).

The language is for PL research: type-driven verification, machine-checked numerical software, structural typing, proof mode, tensor shape checking.

## Key Code Locations

| Path | What It Is |
|---|---|
| `src/lexer.c` | Lexer — tokens, keywords, numbers, strings, comments |
| `src/parser_*.c` | Parser — recursive descent → AST |
| `src/module_*.c` | Module loader — import resolution, cycle detection, linking |
| `src/check_*.c` | Type checker — structural types, flow narrowing, shapes (~4,500 lines across 8 files, shares `src/check_internal.h`) |
| `src/codegen_*.c` | Codegen — AST → C with GC-rooted slot frames |
| `src/runtime_*.c` | Runtime — tagged Value model, operators, builtins, GC (compiled into each generated program, NOT into emeraldc) |
| `src/dim.c` | Dimension solver for tensor shapes |
| `src/diag.c` | Structured diagnostics (human-readable + JSON) |
| `src/main.c` | CLI driver |
| `src/repl.c` | REPL implementation |
| `include/builtins.def` | Authoritative built-in function definitions |
| `stdlib/*.rald` | Standard library in Emerald |
| `docs/` | Language reference and implementation docs |
| `examples/` | Runnable examples (language, concurrency, proof, tensor, ray tracer) |
| `tests/` | Golden tests by stage (lexer, parser, check, proof, e2e, imports, stdlib, shape, bench, repl) |

## Build & Test Commands

Requires: C11 compiler available as `cc`, [Task](https://taskfile.dev/) (`brew install go-task`).

```sh
task                    # build bin/emeraldc (default)
task test               # full test suite (builds first, includes runtime-check)
task test:lexer         # golden tests for --emit-tokens
task test:parser        # golden tests for --emit-ast
task test:check         # golden tests for --check
task test:proof         # golden tests for --check --proof
task test:e2e           # compile-and-run golden tests
task test:imports       # module resolution and linking
task test:stdlib        # standard library tests
task test:shape         # unit tests for dim solver
task test:bench         # benchmark regression checks
task test:repl          # REPL golden tests
task test:cli           # CLI smoke tests
task examples           # compile & run all examples
task bench              # run benchmarks with timings
task bless              # regenerate all .expected golden files (review diff!)
task clean              # remove build outputs
task runtime-check      # prove runtime compiles standalone under strict flags
task install PREFIX=/usr/local   # install emeraldc + stdlib
task dist               # build release tarball
```

## Compiler Pipeline

```
source → lexer → parser → modules → type checker → C codegen → cc → executable
```

Each stage has a CLI flag (`--emit-tokens`, `--emit-ast`, `--check`, `--emit-c`, etc.) and its own golden test suite. The compiler flag `--json` emits one JSON object per diagnostic.

## Conventions

- **C11**, no external dependencies beyond libc and a C compiler to shell out to.
- `CC` env var overrides the C compiler; `$EMERALD_STDLIB` overrides stdlib location.
- Build flags: `-std=c11 -Wall -Wextra -O2 -g -Iinclude`
- Runtime files (`src/runtime_*.c`) are compiled into each generated program, not into `emeraldc`. They must stay independent of compiler headers.
- Golden files (`.expected`, `.json.expected`) pin exact output. After changes, run `task bless` and **review the diff** before committing.
- `tests/imports/` cases are directories (multi-file programs). `bad_*` dirs assert errors; others assert program output.
- `tests/check/` holds two golden files per case: `.expected` (human) and `.json.expected` (JSON diagnostic schema).
- `tests/shape/dim_unit.c` is a compiled C unit test, not a golden file.
- Emerald source files use `.rald` extension.
- Top-level names beginning with `_` are private; all others are exported.

## Gotchas

- **Runtime independence**: `src/runtime_*.c` must not depend on compiler-internal headers. `task runtime-check` verifies this with `-fsyntax-only`.
- **Bless before committing**: After any compiler change, `task bless` to regenerate golden files, then `git diff` the `.expected` files to verify nothing changed silently.
- **Structural typing is erased**: Types are compile-time only. Generated C uses raw `Value` — no vtable, no class, no nominal tag.
- **Closures capture by shared mutable cell**: Nested functions sharing a variable observe each other's writes.
- **Proof mode** (`--proof`): bans `any`, `partial`, unsupported recursion, and loops without provable termination.
- **`$CC` and `$EMERALD_SRC`**: `CC` controls the backend C compiler. `EMERALD_SRC` overrides the runtime source location baked in at build time.

## Architecture Summary

The compilation unit is the **import graph** (not the file). The module loader resolves imports, detects cycles, topologically orders modules, mangles names to `__<module>__<name>`, and hands the checker one linked program. Checker and codegen never see modules.

Values are 16-byte structs passed by value. Only strings, lists, records, and closures reach the heap. Short strings use SSO. Generated C roots every live local in a shadow-stack frame for precise GC.
