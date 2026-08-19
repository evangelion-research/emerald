# Changelog

All notable changes to Emerald are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-08-19

First release: a compiler written in C11 that emits native binaries through the
system `cc`, and a language that uses a structural type system to hold as much
of a program's meaning as is practical.

### Added

- **Pipeline** — lexer → parser → module loader → type checker → C codegen, with
  a per-stage driver flag (`--emit-tokens`, `--emit-ast`, `--emit-shapes`,
  `--check`, `--emit-c`) and golden tests for each stage.
- **Type system** — structural records, intersection (`&`) and union (`|`),
  literal types, flow narrowing, `never`, generics and bounded generics,
  exhaustiveness checking, and `any`.
- **Expected errors** — `error` declarations, `Result[T, E]`, `try` propagation,
  and exhaustive `catch`; no exceptions.
- **Runtime** — a two-generation mark-and-sweep garbage collector with shadow
  stacks, and 76 builtins compiled straight into runtime calls.
- **Concurrency** — cooperative green threads: `spawn`/`join`/`chan`/`send`/
  `recv`/`sleep`/`task_yield`, with deadlock reporting.
- **Standard library** — 13 modules in Emerald: `math`, `lists`, `strings`,
  `sort`, `dict`, `set`, `io`, `sys`, `path`, `chars`, `fmt`, `result`,
  `builder`.
- **Tensors and shapes** — a `dim` solver, `Tensor[dtype, shape]`, `Fin[n]`,
  and `--shape-report`.
- **Proof mode** — `--proof` with warning-based taint rejection
  (`W_VACUOUS_PROOF`), `seq[T]` with `freeze`/`thaw`, covariant-`seq` and
  invariant-`list` soundness (`W_UNSOUND_COVARIANCE`), monotone-counter `while`
  termination, mutual-recursion rejection, and `--proof-report` (text and
  JSON).
- **Propositions** — `Eq[a, b]` with `refl` and dimension-level elimination
  across function boundaries.
- **Operators** — floor division `//`, exponentiation `**`, and compound
  assignment `+= -= *= /=`.
- **Tooling** — `--json` diagnostics, `--proof`, `--repl`, `--werror`,
  `-Wno-CODE`, `-I DIR`, `--version`, and a relocatable stdlib search order
  (`$EMERALD_STDLIB`, next to the executable, then the compile-time default).
- **Release plumbing** — `task install` (into a `PREFIX`), `task dist` (a
  relocatable tarball), and CI on macOS and Linux.

### Not in v1

Tuples, comprehensions, dict/set literals, slicing, default and keyword
arguments, f-strings, and bitwise operators are on the roadmap rather than
present — see the README's "Language status" section. Integer arithmetic wraps
silently (fixed-width 64-bit two's-complement).
