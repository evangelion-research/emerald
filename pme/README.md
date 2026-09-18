# pme — the package manager & build system for Emerald

`pme` is to [Emerald](https://github.com/evangelion-research/emerald) what cargo
is to Rust: it resolves dependencies, materializes them into a local
content-addressed store, and drives `emeraldc` with the right module search
path. It is a *driver* around the compiler — it never parses `.rald`, never
rewrites imports, and never generates code. The whole contract with the
compiler is one frozen flag: ordered `-I` roots.

pme is two tools with one seam:

- the **package manager** — resolve, fetch, verify, publish (`DESIGN.md`);
- the **build system** — compute the search path, detect change, drive
  `emeraldc`, cache artifacts, forward diagnostics (`BUILD.md`).

Prior art for both is collected in [`REFERENCES.md`](REFERENCES.md).

**Status:** initial end-to-end implementation. Package resolution, verified storage,
incremental build driving, and run/test/check modes are implemented, with I/O and
exit codes forwarded correctly. Registry reads (`add` / `why`), `update`, and
`--locked` installs are implemented; registry writes (`publish`, `login`,
`yank`) and remote-cache remain future work. No registry index is live yet, so
`add`/`why`/`install` work against path dependencies and any registry endpoint
set via `PME_REGISTRY`. Its one hard
prerequisite — the Emerald module system — shipped at `emerald@1f683be` and
is still exactly as it shipped: this document tracks the compiler at
`emerald@1facafe` (2026-08-17) plus the stability policy in
[`docs/stability.md`](../docs/stability.md); the `-I` contract pme consumes is
now a frozen contract defined there. It is implemented in
**Go**, distributed as one native `pme` binary.

## Install

With Go available, install the binary once:

```sh
go install github.com/evangelion-research/pme/cmd/pme@latest
```

Release archives contain the same standalone `pme` executable. The only runtime
dependency is `emeraldc` when compiling an Emerald project.

## CLI

| command | behavior |
|---|---|
| `pme init [name]` | scaffold `emerald.toml`, `src/main.rald`, `.gitignore` |
| `pme add <pkg> [constraint]` | add a dependency to `emerald.toml`, resolve, lock, install |
| `pme install` | resolve + fetch + verify; writes `emerald.lock` |
| `pme install --locked` | install strictly from the existing lockfile (fails if stale) |
| `pme update` | re-resolve and refresh the lockfile |
| `pme build` | compute `-I` roots from the lock, exec `emeraldc` |
| `pme check` / `pme emit-c` | typecheck / emit generated C for the linked program |
| `pme run` | build, then exec the binary |
| `pme test` | compile and run `tests/*.rald` |
| `pme tree` / `pme why <pkg>` | dependency tree / every root→package path |
| `pme verify` / `pme clean` | check the store / remove `target/` |

Every command takes `--json` and `-q`. Exit codes: `0` ok, `1` user/build
error, `2` bad usage, `3` network/registry error.

## Design in one paragraph

Manifests are `emerald.toml` — exactly one of `[lib]` or `[[bin]]` (both
allowed), strict semver, path deps legal locally but rejected at publish.
Resolution is **minimal version selection** (MVS): a pure fixed-point
computation, ~100 lines, with one hard constraint — one version per package,
no multi-major support (mangling is keyed on the dotted module path only).
The lockfile `emerald.lock` pins exact versions **and** content hashes and is
committed; `pme build` consumes it as-is and fails rather than silently
re-resolving. Everything lands in a content-addressed, immutable store at
`~/.emerald/store/`, and the build step reduces to: ordered `-I` roots (each
package's `src/` directory, dependencies before dependents, ties broken by
name) → `emeraldc -I <root> … -o target/<bin> <entry>`. The registry starts
as a static, append-only NDJSON index served over HTTPS — no infrastructure
to operate — with tarballs as release assets and publishes as PRs.

There are no build scripts, by design: installing a package never executes
its code.

## Implementation

The detailed step-by-step plan — 17 steps across 6 phases, each with a
verification gate — is in this repo's [`DESIGN.md`](DESIGN.md) (appendix),
expanded from the pme spec's §10–§11. Milestones:

| # | milestone | status |
|---|---|---|
| 0 | imports in emerald (`-I` contract frozen, see `docs/stability.md`) | ✅ done |
| 1 | manifest + lockfile + semver | ✅ done |
| 2 | MVS resolver | ✅ done |
| 3 | store + build (path deps only, no network) | ✅ done |
| 4 | registry reads (`add` / `install` / `tree` / `why`) | ✅ code done — no live index yet |
| 5 | registry writes (reproducible tarball, `publish`, Stage-1 index) | not started |
| 6 | polish (`test`, `update`, `--json` everywhere, docs) | ✅ initial pass |

Milestone 3 is the first genuinely useful build: it needs no registry at all —
`path` dependencies alone prove the whole `-I` pipeline end to end.

The implementation lives under [`cmd/pme`](cmd/pme). `task build` (or
`go build -o pme ./cmd/pme`) produces one executable; no interpreter,
virtual environment, or package manager is required at runtime.

## Compiler contract

`emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald` — re-verified at
`emerald@1facafe`. The full driver flag set is now `--emit-tokens`,
`--emit-ast`, `--check`, `--emit-c`, `--proof` (proof mode), `--keep-c`, `-I`,
`-o`, `--json`; `-I` is repeatable and order-preserving, and `--check`,
`--emit-c`, and a full build operate on the linked program. pme's job is to
compute the ordered `-I` list and exec. The LSP consumes the same lockfile and
the same rule (see the "Package management" section of the LSP design) — pme
never does analysis, the LSP never does resolution.

## Links

- Package-manager + overall design: [`DESIGN.md`](DESIGN.md)
- Build-system design + route: [`BUILD.md`](BUILD.md)
- Prior art: [`REFERENCES.md`](REFERENCES.md)
- pme spec companion: `evangelion-research/pme` (`DESIGN.md`)
- Emerald compiler: `evangelion-research/emerald`
- License: MIT — see `LICENSE`
