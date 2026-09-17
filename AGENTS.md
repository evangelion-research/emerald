# Project knowledge

This file gives Freebuff context about your project: goals, commands, conventions, and gotchas.

## Quickstart
- Setup: Requires a C11 compiler (`cc`) and [Task](https://taskfile.dev/) (`brew install go-task` on macOS)
- Dev: `task` (builds `bin/emeraldc`), `task examples` (compile & run examples)
- Test: `task test` (full suite) or `task test:lexer|parser|check|proof|e2e|imports|stdlib|shape|bench|repl|cli`

## Architecture
- Key directories: `src/` (compiler + runtime in C), `stdlib/` (Emerald standard library), `tests/` (golden tests by stage), `include/` (headers + builtins.def), `docs/` (language reference), `examples/` (runnable programs)
- Data flow: `source → lexer (src/lexer.c) → parser (src/parser_*.c) → modules (src/module_*.c) → type checker (src/check_*.c) → codegen (src/codegen_*.c) → cc → executable`
- Runtime (`src/runtime_*.c`) is compiled into each generated program, not into `emeraldc`. Must stay independent of compiler headers.

## Skills
- Always load the `ponytail` skill (`.agents/skills/ponytail/SKILL.md`) and follow it on **every** coding task: writing, modifying, refactoring, fixing, or reviewing code. It enforces the laziest solution that actually works — minimal code, stdlib-first, no speculative abstractions (YAGNI).
- Companion skills, loaded only when asked: `ponytail-review` (diff review), `ponytail-audit` (whole-repo audit), `ponytail-debt` (deferred-work ledger), `ponytail-gain` (impact scoreboard), `ponytail-help` (quick reference).

## Conventions
- Formatting/linting: C11 with `-Wall -Wextra -O2 -g -Iinclude`. No external dependencies beyond libc.
- Patterns to follow: Each compiler stage has a CLI flag and its own golden test suite. Golden files (`.expected`, `.json.expected`) pin exact output.
- Things to avoid: Never let `src/runtime_*.c` depend on compiler-internal headers. Always run `task bless` and review the diff after compiler changes. Don't commit without verifying `task test` passes.
