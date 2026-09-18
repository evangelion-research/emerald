# Stability and versioning policy

Emerald is **pre-release**. There is no tagged version, no published artifact,
and no stability guarantee of any kind yet: the changelog keeps everything under
`[Unreleased]`, and the `VERSION` file tracks the working version only.

This document defines what *will* be promised when the first release is cut, so
that the promise is written before the freeze rather than after it.

## Versioning

Emerald follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html) once
the first version is tagged. The version lives in `VERSION` at the repository
root; the Taskfile and every subproject read it from there. Breaking changes
require a major bump; new backward-compatible features a minor bump; fixes a
patch bump.

## Inside the stability boundary (frozen at 1.0)

- **Surface syntax.** Everything the parser accepts today (see `src/parser_*.c`
  and `docs/lang.md`).
- **Standard-library signatures.** Names, parameter orders, and return types of
  `stdlib/*.rald` and the builtin table (`include/builtins.def`).
- **CLI flags of `emeraldc`** as documented in `emeraldc --help`, and their
  exit codes (0 success, 1 compile error, 2 usage error).
- **The `--json` diagnostics schema.** Pinned by the golden files under
  `tests/check/*.json.expected`; `pme` and `emlsp` both parse it.
- **Diagnostic codes.** The `E_*`/`W_*` identifiers are a contract: a code may
  be *added*, never removed or silently renumbered.
- **The `-I` module-resolution contract:** `emeraldc [-I DIR]... [-o OUT] ...`
  with imports resolved across the given roots, in the order given. This is
  what `pme` drives.
- **Runtime semantics observable from Emerald:** GC behavior, `spawn`/`join`
  scheduling fairness, `Result`/`try` propagation, and error-message *codes*
  (message prose may change).

## Outside the boundary (may change in any release)

- **Generated C.** `--emit-c` output is a debugging view, not an interface.
- **Proof-mode acceptance set.** `--proof` may accept *more* over time as the
  analysis improves; it will not silently reject previously accepted programs
  without a minor-version note.
- **`--emit-*` formats** (`--emit-tokens`, `--emit-ast`, `--emit-shapes`) and
  the `--shape-report`/`--proof-report` schemas. Golden files pin them for
  regression purposes only.
- **Runtime internals:** object layout, GC triggers and thresholds, tape
  representation, C entry-point names (`em_*`).
- **The REPL** session protocol and its temp-file mechanics.

## Deprecation

A frozen item is deprecated by: (1) a changelog entry under `[Unreleased]`,
(2) a compiler warning or README note for one full minor release, (3) removal
at the next major. Diagnostic codes and the `--json` schema are never removed
— only superseded.

## Foreign-function interface: decision

**There is no FFI in Emerald, and none is planned before 1.0.** This is a
recorded decision, not an oversight: freezing the surface without an FFI keeps
the proof-mode and purity story sound (any foreign call is a hole in every
guarantee the checker makes), and the current ecosystem ships its system
integration as in-tree builtins instead.

Consequences, accepted: no sockets, TLS, SQLite, BLAS, or third-party native
libraries from Emerald code; capability growth is bounded by the builtin table.
Revisit when a concrete need (and a marshalling + GC-safety design) exists —
retrofitting an FFI after the freeze will require a major version and is
deliberately out of scope.
