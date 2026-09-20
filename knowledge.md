# Project knowledge

## What this is

Emerald is an experimental statically typed programming language with Python-inspired syntax, structural typing, checked errors, proof mode, tensors/autograd, cooperative tasks, and a C11 compiler that generates native executables.

The pipeline is:

```text
source -> lexer -> parser -> modules -> type checker -> C codegen -> system cc -> executable
```

## Key locations

- `src/`: compiler pipeline, diagnostics, CLI, and runtime implementation
- `include/`: compiler/runtime headers and authoritative builtin table (`builtins.def`)
- `stdlib/`: Emerald standard library
- `tests/`: golden, integration, runtime, proof, shape, benchmark, REPL, and CLI tests
- `examples/`: runnable language and subsystem examples
- `docs/`: language reference and implementation documentation
- `Taskfile.yml`: build, test, example, benchmark, install, and distribution tasks
- `VERSION`: current compiler version

## Requirements and commands

Requirements: a C11-compatible `cc` compiler and [Task](https://taskfile.dev/). No external runtime/compiler dependencies beyond libc and POSIX facilities.

```sh
task                 # build bin/emeraldc and bin/libemerald.a
task test            # full suite plus runtime checks and examples
task examples        # compile and run examples
task runtime-check   # strict standalone runtime syntax check
task bench           # run benchmark workloads
task clean           # remove build outputs/generated artifacts
```

Focused test tasks include `task test:lexer`, `test:parser`, `test:check`, `test:proof`, `test:e2e`, `test:imports`, `test:repl`, `test:cli`, `test:stdlib`, `test:shape`, and `test:bench`.

Compile a program with `bin/emeraldc file.rald`; generated programs link against the runtime automatically. Useful compiler modes include `--emit-tokens`, `--emit-ast`, `--check`, `--proof`, `--json`, and `--repl`.

`task install PREFIX=/usr/local` installs a release layout; `task dist` creates a tarball; `task dist:verify` smoke-tests it.

## Conventions and gotchas

- Follow C11 style and compile with `-std=c11 -Wall -Wextra -O2 -g -Iinclude`.
- Keep `src/runtime_*.c` independent of compiler-internal headers; runtime code is compiled into generated programs.
- Golden `.expected` files pin exact compiler/test output. After intentional compiler-output changes, run `task bless` and review the complete diff.
- Run `task test` before considering compiler changes complete.
- The standard library and compiler tests are the source of truth when docs and implementation differ.
- Compiler search paths can be overridden with `EMERALD_STDLIB` and `EMERALD_LIB`; `-I DIR` adds module search paths.
- The platform target is POSIX C11 (macOS and Linux); Windows is unsupported.
- Avoid external dependencies and speculative abstractions; prefer the existing stage patterns and libc.
