# What remains before Emerald is a publishable language and ecosystem

> Implementation pass of 2026-09-18: the P0 release blockers (broken tarball,
> shell injection, no CI/tags), the P1 fixes (pme add/why/update/--locked and
> I/O forwarding, emlsp README truth, VS Code scaffold, stdlib filesystem/env
> builtins, `#line` emission, examples gating, community files) and the
> documentation-accuracy defects have landed. The original 561-line audit with
> its evidence log is in git history (`0bb29b7`); this file lists only what is
> still open.

## Distribution

- [ ] A tagged release with per-platform prebuilt binaries (macOS arm64/x86-64,
      Linux x86-64/arm64), checksummed, gated by `task dist:verify` in the
      release workflow.
- [ ] `tree-sitter-emerald` published (npm) and registered with
      `nvim-treesitter` / GitHub Linguist; the VS Code extension packaged as a
      `.vsix` and published to the marketplace.

## Ecosystem

- [ ] A live package registry with at least one published package, and
      `pme publish` (registry writes, milestone 5 in `pme/DESIGN.md`).
- [ ] `emlsp` tests for the server lifecycle, diagnostic mapping, and feature
      layer (only `internal/lexer` and `internal/positions` have tests today).
- [ ] More tree-sitter corpus coverage, plus a test that the grammar agrees
      with `src/lexer.c`/`src/parser_*.c`.

## Language and stdlib

- [ ] Stdlib breadth beyond filesystem/env: time/date, JSON, hashing, random
      with explicit RNG state. (Net/crypto/compression are out of scope until
      the FFI decision in [`docs/stability.md`](docs/stability.md) is revisited.)
- [ ] The proof-mode soundness work, recursive data types, and constrained
      generics tracked in [`docs/REMAINING_FEATURES.md`](docs/REMAINING_FEATURES.md).

## Tooling

- [ ] A formatter (`emeraldfmt`), then `textDocument/formatting` in `emlsp`.
- [ ] A WASM playground (the C11 zero-dependency compiler makes this tractable).
- [ ] A tutorial and rendered documentation site; `docs/` is reference-only.
- [ ] Performance: BLAS/vectorization for the CPU tensor path (see
      `gpu/PLAN.md`), benchmark thresholds in `tests/bench`.
