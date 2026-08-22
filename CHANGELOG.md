# Changelog

All notable changes to Emerald are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Unicode layer** — the `unicode` standard-library module and its seven
  `uc_*` builtins (`uc_len`, `uc_ord`, `uc_chr`, `uc_at`, `uc_slice`,
  `uc_chars`, `uc_valid`): UTF-8 code-point length, indexing, slicing, and
  iteration over the existing byte-oriented strings, beside rather than
  replacing the byte semantics.

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
  stacks, and 77 builtins compiled straight into runtime calls.
- **Concurrency** — cooperative green threads: `spawn`/`join`/`chan`/`send`/
  `recv`/`sleep`/`task_yield`, with deadlock reporting.
- **Standard library** — 11 modules in Emerald: `math`, `lists`, `strings`,
  `sort`, `io`, `sys`, `path`, `chars`, `fmt`, `result`, and `builder`.
  Dictionaries and sets are builtin runtime values exposed through
  Python-style `dict()` and `set()` constructors.
- **Tensors and shapes** — a `dim` solver, `Tensor[dtype, shape]`, `Fin[n]`,
  and `--shape-report`.
- **Proof mode** — `--proof` with warning-based taint rejection
  (`W_VACUOUS_PROOF`), `seq[T]` with `freeze`/`thaw`, covariant-`seq` and
  invariant-`list` soundness (`W_UNSOUND_COVARIANCE`), monotone-counter `while`
  termination, mutual-recursion rejection, and `--proof-report` (text and
  JSON).
- **Propositions** — `Eq[a, b]` with `refl` and dimension-level elimination
  across function boundaries.
- **Expressions** — tuples, list/set/dict comprehensions, dynamic dictionary
  and set literals, slicing, default and keyword arguments, f-strings, and
  integer bitwise operators.
- **Operators** — floor division `//`, exponentiation `**`, compound
  assignment `+= -= *= /=`, and numeric shifts.
- **Tooling** — `--json` diagnostics, `--proof`, `--repl`, `--werror`,
  `-Wno-CODE`, `-I DIR`, `--version`, and a relocatable stdlib search order
  (`$EMERALD_STDLIB`, next to the executable, then the compile-time default).
- **Release plumbing** — `task install` installs the compiler and standard
  library under a `PREFIX`; `task dist` builds a release tarball.
