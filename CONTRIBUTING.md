# Contributing to Emerald

Thanks for looking at the compiler. This document covers the mechanics of a
contribution; the language-design side is governed by `docs/stability.md`.

## Build and test

Prerequisites: a C11 compiler as `cc`, and
[Task](https://taskfile.dev/).

```sh
task                 # build bin/emeraldc + bin/libemerald.a
task test            # the full golden/runtime/CLI/examples suite
task examples        # compile & run every example
task runtime-check   # strict-flags syntax check of the runtime
```

If you changed compiler **output** (diagnostics, AST dump, tokens, codegen
shape), goldens will fail. After *verifying by eye that the new output is
correct and better*, regenerate them:

```sh
task bless
git diff          # review every golden change before committing
```

A blessed diff you have not read line by line is how a regression ships.

## Rules of thumb

- C11, `-Wall -Wextra` clean, no dependencies beyond libc.
- The runtime (`src/runtime_*.c`) must never include compiler-internal headers;
  `task runtime-check` enforces the spirit of this.
- Every compiler stage has a CLI flag and a golden suite — if you touch a
  stage, its goldens are part of the change.
- No `TODO`/`FIXME`/`XXX`/`HACK` markers land in `src/`, `include/`, or
  `stdlib/`. Either do it or don't.

## Go subprojects

`pme`, `emlsp`, and `emerald-ide` each build and test independently:

```sh
cd pme && go test ./...
```

## Pull requests

- One logical change per PR; include the test (golden, e2e, or Go) that pins it.
- CI runs the full matrix (macOS + Linux, compiler + Go) on every PR; it must
  be green.
- Update the relevant doc (`docs/*.md`, README) in the same PR as the behavior.
- For changes to anything in the stability boundary (`docs/stability.md`),
  say so explicitly in the PR description.

## Reporting bugs

Open a GitHub issue with the smallest `.rald` program that reproduces it, the
exact `emeraldc` invocation, and the observed vs. expected output. Security
issues follow `SECURITY.md` instead.
