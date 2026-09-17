# pme — the Package Manager for Emerald

**Status:** implemented (Go). Registry writes (`publish`, `login`, `yank`) and
remote-cache remain future work — but its one hard
prerequisite, the Emerald module system, has shipped (see §0).
**Target language:** Go (distributed as a single native binary; pme is I/O-bound).
**Distribution model:** central registry.
**Two tools, one seam:** pme is a **package manager** (this document) *and* a
**build system** (`BUILD.md`). Prior art for both is in `REFERENCES.md`.
**Tracks emerald at:** `1facafe` (HEAD, 2026-08-17). The module system itself is
exactly as it shipped at `1f683be`; everything landed since — the functional core
(lambdas, thunks, closures, tail-call optimization), proof mode (`--proof`), the
ray-tracer example, and the Phase-2 plan (`docs/SPEC_V2.md`) — left the resolution
rules and the `-I` contract this design depends on untouched (see §0).

pme is to [Emerald](https://github.com/evangelion-research/emerald) what cargo is to Rust:
it resolves dependencies, materializes them into a local cache, and drives `emeraldc`
with the right module search path. It is a *driver* around the compiler, never a
replacement for it.

---

## 0. Prerequisite: the Emerald module system — **done**

pme cannot exist in a useful form until the language can name code in another file.
It now can. `emerald@1f683be` added `src/module.c`, `docs/modules.md`, and a
`tests/imports/` golden suite (23 cases, `task test:imports`, included in `task test`).
The suite is unchanged at HEAD (`1facafe`), so everything in this section was
re-verified against the current tree, not just against `1f683be`. The upstream doc
is authoritative; this section records only what pme depends on.

### 0.1 Syntax (as shipped)

```
import strings                       # module object; strings.split(...)
import strings as s
import text.strings                  # dotted path — binds `strings`, the LAST component
from strings import split, join
from strings import join as concat
```

Imports are legal only at the **top level** of a module. There are no package objects:
`import a.b.c` binds `c`, and `a` alone names nothing — use `as` when the last component
isn't the name you want.

### 0.2 Resolution (as shipped)

A dotted path maps to a file under each search root, **in this order**:

1. the directory of the importing file,
2. the project's `src/` root — found by walking **up from the entry file's directory**,
3. each `-I <dir>` given on the command line, in the order given.

The first root with a hit wins; a later `-I` never shadows an earlier one.

Two details that differ from the original sketch and that pme must design around:

- **Both spellings are accepted.** `text.strings` matches `text/strings.rald` *or*
  `text.strings.rald`. A single root offering both is `E_IMPORT_AMBIGUOUS` — an error,
  not a silent pick. Package tarballs should therefore never ship both spellings of one
  path; `pme publish` can cheaply check this and reject it before upload (see §7.3).
- **The `src/` root is the *entry's*, not each module's.** A dependency's own `src/` is
  not implicitly a root. So a store package whose internal layout is
  `src/a/b.rald` importing a sibling-of-root `util` resolves only if pme passes that
  package's `src/` as an explicit `-I`. This is what §5.2 already does, but it is now a
  correctness requirement rather than a convenience.

Module identity is the file's **canonical path**, so the same file reached through two
different roots is one module, loaded once. Diamonds in the import graph cost nothing.

### 0.3 The compiler contract — frozen

```
emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald
```

Confirmed against `src/main.c` at HEAD (`1facafe`): `-I` (both `-I dir` and `-Idir`)
is repeatable and order-preserving, `-o` sets the output path, `--json` applies to
any mode. The full flag set is now `--emit-tokens`, `--emit-ast`, `--check`,
`--emit-c`, `--proof`, `--keep-c` — `--proof` (proof mode, §0.5) was added after
`1f683be`; nothing pme consumes changed. `--version` still does not exist (§12 Q2).

The full build compiles the generated C itself: `cc -std=c11 -O2 -I <src> …`,
honoring `$CC` and `$EMERALD_SRC`. There is no debug/release switch to pass through
(see §6), and no way for a driver to inject CFLAGS.

`--emit-tokens` and `--emit-ast` are deliberately **per-file** views and do not follow
an import. `--check`, `--emit-c`, and a full build operate on the linked program — so
`pme check` (should it ever exist) and `pme build` both see the whole graph.

pme's job reduces to: compute the ordered list of `-I` roots, then exec the compiler.
pme never parses `.rald` source. It never rewrites imports. It never generates code.
That seam held through implementation and should stay frozen.

### 0.4 Linking, exports, and mangling (as shipped)

- **Export rule:** a leading underscore means private, everything else is exported —
  for top-level `def`s, `type` aliases, and globals alike. No new keyword, as planned.
- **Linking is a rename pass in `src/module.c`.** The checker and codegen still see one
  flat `Program`, ordered dependencies-first; they know nothing about modules.
- **Mangling** is `<module>__<name>`, with dots flattened to underscores:
  `strings.split` → `strings__split`, `text.strings.split` → `text_strings__split`.
  Two packages can each define `parse`. The **entry module is never renamed**.
- Rewritten AST nodes keep the spelling the user wrote, and diagnostics quote *that* —
  so pme never needs to demangle a symbol before showing an error to a user.
- All modules are concatenated into **one translation unit**, preserving the GC
  shadow-stack setup. Splitting into per-module `.gen.c` is a later change; pme's build
  step (§5.2) is unaffected either way, since it is one `emeraldc` exec regardless.
- Module init order follows link order: an imported module's top-level statements run
  before its importer's.

One language-level limit worth knowing when writing packages: **types must cross a
module boundary via `from`-import.** `m.T` in a *type* position is rejected, because the
type grammar has no qualified names. Values and functions work either way.

### 0.5 Import diagnostics (as shipped)

| code | meaning |
|---|---|
| `E_IMPORT_NOT_FOUND` | module path resolved to no file on the search path; notes list every root searched |
| `E_IMPORT_CYCLE` | import graph contains a cycle; notes list the modules on it |
| `E_IMPORT_PRIVATE` | imported name exists but is private (leading `_`) — whether via `from m import _x` or `m._x` |
| `E_IMPORT_NAME` | imported name does not exist in that module |
| `E_IMPORT_AMBIGUOUS` | one root offers both `a/b.rald` and `a.b.rald`; notes list both candidates |
| `E_IMPORT_REDEFINE` | an import binding collides with a local top-level name or an earlier import |
| `E_IMPORT_MODULE_VALUE` | a module object used as a value, or assigned to; only `m.<name>` is legal |

The last two are new relative to the original sketch. All carry `file:line:column`, quote
the source line, and are available under `--json`.

Note for §8: import errors are emitted with the existing `syntax` **kind**, not a new
`import` kind. The compiler's `DiagKind` is still exactly `syntax | type | internal`
(re-verified in `include/diag.h` at HEAD). Proof mode (`--check --proof`) adds an
`E_PROOF_*` family (`E_PROOF_ANY`, `E_PROOF_PARTIAL`) that stays inside those three
kinds; pme never passes `--proof`, so it never sees them.

### 0.6 Definition of done — met

`tests/imports/` covers basic, aliased, `from`-import, dotted, transitive, same-file
identity, `src/`-root resolution, `-I` precedence and shadowing, module state, and
symbol collision across modules; plus twelve `bad_*` cases compiled with `--check`
whose diagnostics are the golden output — 23 cases total, unchanged at HEAD.
`task test:imports` runs the stage alone; `task test` runs all six stage suites
(lexer, parser, check, proof, e2e, imports; 99 golden tests).

---

## 1. Concepts

| term | meaning |
|---|---|
| **package** | a named, versioned unit of Emerald source, published to the registry |
| **project** | a directory with an `emerald.toml`; may be a package, an app, or both |
| **entry** | the `.rald` file compiled for a binary target |
| **manifest** | `emerald.toml` — what the human writes |
| **lockfile** | `emerald.lock` — what pme resolved; committed to git |
| **store** | the global content-addressed cache at `~/.emerald/store/` |

Package names: `[a-z][a-z0-9_]*`, 2–64 chars. Flat namespace (no `org/name`) — the
registry enforces uniqueness on first-publish-wins. Reserved: anything shipping in the
stdlib.

Versions are strict semver `MAJOR.MINOR.PATCH` with optional `-prerelease`. No build
metadata, no `v` prefix inside the manifest.

---

## 2. Manifest — `emerald.toml`

```toml
[package]
name        = "myapp"
version     = "0.1.0"
description = "A short line."
license     = "MIT"
authors     = ["Sagnik Chatterjee <sagnikchatterjee607@gmail.com>"]
repository  = "https://github.com/evangelion-research/myapp"
emerald     = ">=0.2.0"          # compiler version constraint

[dependencies]
strings = "1.2.0"                # minimum version (see MVS below)
json    = "0.4.1"

[dev-dependencies]
testkit = "0.1.0"

[[bin]]
name  = "myapp"
entry = "src/main.rald"

[lib]
root = "src/lib.rald"            # optional; presence makes this publishable
```

Rules:

- Exactly one of `[lib]` or `[[bin]]` is required; both may be present.
- A dependency value is a bare version string. **There is no git or path dependency
  in the published graph** — but `pme` supports them locally for development:

  ```toml
  [dependencies]
  strings = { path = "../strings" }        # local dev only
  ```

  A manifest with any `path` dependency is rejected by `pme publish`. This keeps the
  registry's dependency graph closed and resolvable offline from the index alone.
- `[dev-dependencies]` are resolved for `pme test` and never for consumers.

### 2.1 Why not a lockfile-free design

Because MVS (below) is reproducible only given a fixed *set* of manifests, and the
registry is mutable in one direction (new versions appear). The lockfile pins exact
versions **and content hashes**, which is also the supply-chain check.

---

## 3. Lockfile — `emerald.lock`

Generated, committed, never hand-edited. TOML, sorted deterministically by name so
diffs are readable.

```toml
version = 1                       # lockfile format version

[[package]]
name     = "strings"
version  = "1.2.3"
checksum = "sha256:9f2c…"         # of the canonical tarball
deps     = ["unicode"]

[[package]]
name     = "unicode"
version  = "0.9.0"
checksum = "sha256:1ab4…"
deps     = []
```

- `pme build` uses the lockfile as-is and **fails** if the manifest asks for something
  the lock doesn't satisfy (rather than silently re-resolving).
- `pme install` re-resolves when the manifest changed, then rewrites the lock.
- `pme install --locked` (for CI) refuses to modify the lock; exits non-zero on drift.
- A checksum mismatch against the store is a hard error, never a re-download.

---

## 4. Resolution — minimal version selection

Use **MVS** (Go-style), not a SAT/PubGrub solver.

Algorithm:

1. Start with the root manifest's direct dependencies as `{name: min_version}`.
2. Repeatedly: for each selected `(name, version)`, fetch its manifest from the index,
   and for each of its dependencies raise the selected version to the max of what's
   already selected and what it requires.
3. Fixed point = the resolution.

Properties that make this the right call here:

- Adding a dependency never silently upgrades an unrelated one.
- The result is a pure function of the manifest set — trivially reproducible and easy
  to test.
- The implementation is ~100 lines, versus a solver you will spend weeks debugging.
- Only one version of a package is ever selected. **pme does not support multiple
  major versions of the same package in one build.** This is now a hard constraint, not
  a preference: mangling is `<module>__<name>` keyed on the *dotted module path* only
  (§0.4), with no version component, so two majors of `strings` would emit colliding
  `strings__split` symbols into one translation unit. Diagnose the conflict clearly
  instead.

`pme update` is the escape hatch: it bumps the recorded minimums in the manifest to the
latest compatible release, then re-resolves.

Failure cases to report well:

- `E_RESOLVE_NOT_FOUND` — package not in the registry.
- `E_RESOLVE_NO_VERSION` — no published version satisfies the minimum.
- `E_RESOLVE_MAJOR_CONFLICT` — two dependents require incompatible majors; the message
  must print both dependency paths (this is what `pme why` renders).
- `E_RESOLVE_COMPILER` — the resolved set needs a newer `emeraldc` than is installed.

---

## 5. The store and the build

### 5.1 Layout

```
~/.emerald/
  store/
    strings-1.2.3-9f2c…/        # extracted, immutable, named by name-version-hash
      emerald.toml
      src/
  cache/
    tarballs/9f2c….tar.gz       # as downloaded, for offline re-extract
  index/                        # cloned/synced registry index
  credentials.toml              # publish token, mode 0600
```

Content-addressed and immutable: two projects on the same machine share one extracted
copy, and a corrupted entry is detectable rather than sticky. Never vendor into the
project directory by default.

### 5.2 What `pme build` actually does

```
read emerald.toml
read emerald.lock            (error if stale)
for each locked package:
    ensure ~/.emerald/store/<name>-<ver>-<hash>/ exists
        else: download tarball → verify sha256 → extract atomically (tmp + rename)
compute -I roots in dependency order
exec: emeraldc -I <root1> -I <root2> … -o target/<binname> src/main.rald
```

Each `-I` root is the package's **`src/` directory**, not its store root — a dependency's
own `src/` is never an implicit root (§0.2). Order matters and is preserved by the
compiler, so pme fixes it deterministically: dependency order, dependencies before
dependents, ties broken by name.

One shadowing hazard to keep in mind: the entry project's `src/` is searched *before*
every `-I`, so an app file named `strings.rald` silently wins over the `strings` package.
That is the compiler's documented precedence and pme should not fight it, but `pme add`
can warn when a new package name collides with a top-level module already in the
project's `src/`.

Output goes to `target/`. `pme build --json` passes `--json` through to `emeraldc` and
emits pme's *own* errors in the same shape (see §8), so one JSON stream describes the
whole build.

There is **no build script mechanism**. Installing a package must never execute code
from that package. This is a deliberate, load-bearing security property — do not add
`[build] script = …` later without a sandbox story.

---

## 6. CLI

Keep it small. Every command takes `--json` and `-q`.

| command | behavior |
|---|---|
| `pme init [name]` | scaffold `emerald.toml`, `src/main.rald`, `.gitignore` |
| `pme add <pkg>[@ver]` | resolve latest (or given), edit manifest preserving comments, update lock |
| `pme remove <pkg>` | inverse of add |
| `pme install` | resolve + fetch + verify; writes lock. `--locked` for CI |
| `pme update [pkg]` | raise minimums to latest compatible; re-resolve |
| `pme build [--release]` | §5.2. `--release` is a placeholder: `emeraldc` always compiles the generated C with `-O2` and exposes no debug/release switch (it honors `$CC`/`$EMERALD_SRC`), so pme passes the flag through as a no-op until upstream adds a distinction |
| `pme run [-- args]` | build, then exec the binary |
| `pme test` | build with dev-deps and run the test target (see §9) |
| `pme tree` | dependency tree with selected versions |
| `pme why <pkg>` | print every path from root to `<pkg>` |
| `pme publish` | §7.3 |
| `pme login` | store a registry token in `~/.emerald/credentials.toml` |
| `pme clean` | remove `target/`; `--store` also prunes unreferenced store entries |
| `pme verify` | re-hash every store entry against the lock |

Exit codes: `0` ok, `1` user/build error, `2` bad usage, `3` network/registry error.

---

## 7. The central registry

### 7.1 Recommended staging

Do **not** build a service first. Ship the registry in two stages:

**Stage 1 — static index, zero servers.**
- Index lives in a git repo `evangelion-research/pme-index`, also served over HTTPS
  from `evangelion-research.github.io/pme-index/`.
- Tarballs are GitHub Release assets on that same index repo (or on each package's own
  repo — pick one and record the URL in the index).
- `pme publish` opens a PR (or pushes, for trusted authors) adding one line to the
  index and uploading the tarball via the GitHub API.
- Reads are a plain HTTPS GET with CDN caching. Auth is a GitHub token.

This gives you a real central namespace, real immutability, and real availability, with
no infrastructure to operate. It is enough for a very long time.

**Stage 2 — a real service**, once publish friction or download volume justifies it.
The client-side protocol below is designed so Stage 2 is a drop-in swap: only the
`registry.base_url` changes.

### 7.2 Index format

One newline-delimited-JSON file per package, at a sharded path so directories stay
small:

```
index/st/ri/strings.json
```

Each line is one published version, append-only:

```json
{"name":"strings","version":"1.2.3","checksum":"sha256:9f2c…","deps":{"unicode":"0.9.0"},"emerald":">=0.2.0","yanked":false,"url":"https://…/strings-1.2.3.tar.gz"}
```

Append-only means a client can cache aggressively and fetch by range. `yanked` is
mutated in place (the one exception) — a yanked version stays downloadable for existing
lockfiles but is never newly selected.

### 7.3 Publish flow

```
pme publish
  ├ validate manifest (name, semver, license, no path deps, [lib] present)
  ├ reject a tree with both `a/b.rald` and `a.b.rald` → would be E_IMPORT_AMBIGUOUS
    for every consumer (§0.2)
  ├ verify version not already published        → immutable, no overwrites ever
  ├ build the tarball from git-tracked files only, honoring .pmeignore
  ├ normalize: sorted entries, fixed mtime/uid/gid/mode → byte-reproducible tarball
  ├ sha256 the tarball
  ├ dry-run compile in a temp dir to prove it builds against its own deps
  └ upload + append the index line
```

Immutability is absolute: a published `name@version` never changes bytes. `pme yank`
marks a version unselectable without deleting it.

### 7.4 Client-side registry protocol

Exactly three operations, so Stage 1 and Stage 2 can both implement it:

- `GET {base}/index/{shard}/{name}.json` → NDJSON version list
- `GET {url}` → tarball (URL comes from the index line)
- `POST {base}/publish` with bearer token → multipart tarball + metadata

---

## 8. Errors

Mirror the compiler's convention from `emerald/docs/diagnostics.md` so tooling —
including `emerald-lsp` and any LLM loop — sees one consistent shape.

Human:

```
error[E_RESOLVE_MAJOR_CONFLICT]: cannot select a single version of `unicode`
  --> emerald.toml
    = myapp 0.1.0 → strings 1.2.3 → unicode ^0.9
    = myapp 0.1.0 → json 0.4.1 → unicode ^1.0
```

`--json`, one object per error:

```json
{"kind":"resolve","severity":"error","code":"E_RESOLVE_MAJOR_CONFLICT",
 "file":"emerald.toml","message":"…","notes":[{"label":"path","value":"…"}]}
```

The compiler's set is currently exactly `syntax`, `type`, `internal` — import errors
reuse `syntax` rather than introducing a kind of their own (re-verified in
`include/diag.h` at HEAD; proof-mode `E_PROOF_*` errors stay inside those three
kinds). pme extends that set with `manifest`, `resolve`, `registry`, `io`; the four
are disjoint from the compiler's, so a consumer can attribute every object in a
merged `--json` stream to one producer.

---

## 9. Testing story

Emerald has no test construct — `tests/` in the compiler repo is golden-file testing
*of the compiler*. So pme needs a convention, and it should be the dumbest one that
works:

- `pme test` compiles and runs every `tests/*.rald` in the project, with dev-deps on
  the `-I` path.
- A test binary passes if it exits `0`. Failures print its stdout.

That needs no language change. A real assertion library can ship later as a package
(`testkit`), and a `test` keyword can come much later if it earns its place.

---

## 10. Implementation in Go

### 10.0 Stack

The CLI is a Go module. Its standard-library implementation covers command
dispatch, HTTP, archive extraction, checksums, process execution, and tests;
the TOML decoder is linked into the released executable. No interpreter or
runtime package installation is required. (The one vendored dependency,
`BurntSushi/toml`, is pinned in `go.mod` and fetched by the normal Go
module proxy — `go build` just works.)

### 10.1 Layout

```
pme/
  go.mod
  cmd/pme/main.go           # CLI, manifest/lock, resolver, registry, store, build driver
  cmd/pme/main_test.go      # Go unit tests
```

### 10.2 Dependency choices

- Go's standard library owns process execution, HTTP, SHA-256, tar/gzip handling,
  filesystem work, and testing.
- TOML parsing is compiled into the binary.
- Emerald semver remains hand-rolled; it is stricter and simpler than PEP 440.

### 10.3 Non-negotiables

- **Every filesystem mutation is atomic**: write to a temp path in the same directory,
  `os.replace`. A `^C` during extract must never leave a half-package in the store.
- **A lock file (`~/.emerald/.lock`, flock) around store writes**, so two concurrent
  `pme build`s don't corrupt each other.
- **Verify before use, always**: hash on extract and on `pme verify`; never trust a
  path just because it exists.
- **No network in `pme build`** when the lock is satisfied by the store. Builds must
  work offline.
- **Never `shell=True`**. `emeraldc` is invoked with an argv list.
- **Fingerprint by content, not mtime** — change detection hashes file contents
  so identical source always produces an identical decision (`BUILD.md` §4).
- **The build plan is a pure function** of (manifest, lockfile, flags) and is
  serialized for tooling (`BUILD.md` §1.2).

### 10.4 Shipping

Ship prebuilt `pme` binaries for supported platforms, plus `go install
github.com/evangelion-research/pme/cmd/pme@latest` for Go users. The manifest,
lockfile, index format, and `emeraldc -I` contract remain unchanged.

---

## 11. Milestones

| # | milestone | done when |
|---|---|---|
| 0 | ~~**imports in emerald**~~ ✅ | **done** — `tests/imports/` green in `task test`; `-I` contract frozen, re-verified at `emerald@1facafe` |
| 1 | manifest + lockfile + semver | round-trip parse/write, property tests on semver |
| 2 | MVS resolver | resolves against a fake in-memory index, conflicts reported |
| 3 | store + build | `pme build` works with `path` deps only — no network yet |
| 4 | registry reads | index fetch, download, verify, extract; `pme add/install/tree/why` |
| 5 | registry writes | reproducible tarball, `pme publish`, `pme login`, Stage-1 index live |
| 6 | polish | `pme test`, `pme update`, `--json` everywhere, `pme verify`, docs |

Milestone 3 is the first genuinely useful build, and it needs no registry at all —
`path` dependencies alone prove the whole `-I` pipeline end to end. With milestone 0
landed, **milestone 1 is the current front of work**, and milestone 3 is now unblocked
by anything upstream. Since `1f683be` the module system has only been exercised
harder: the typed ray tracer is a 13-module program, and the Phase-2 plan
(`docs/SPEC_V2.md`) intends `import tensor` as the first real library — both run
through exactly the `-I` pipeline pme will drive.

### 11.1 Detailed implementation route

The build-system half is `BUILD.md` §11 (steps B1–B6). The package-manager half,
shipped in `cmd/pme/main.go`:

**M1 — manifest + lockfile + semver** *(done)*
1. Semver: parse/compare, prerelease rules, constraint set (`==`, `>=`, `^`, `~`, bare minimum).
2. Manifest parsing and validation: name regex, semver, `[lib]`/`[[bin]]` exclusivity, and path-dependency detection.
3. Lockfile: deterministic TOML emit/parse; round-trip stability.
4. `pme init` scaffolding.

*Gate:* round-trip parse/write stable; `pme init` scaffolds a valid project.

**M2 — MVS resolver** *(done)*
1. Implement §4's fixed-point over an in-memory index (`name → sorted versions + deps`).
2. Encode the failure cases (`E_RESOLVE_NOT_FOUND`, `NO_VERSION`, `MAJOR_CONFLICT`, `COMPILER`) in the §8 shape.
3. Golden tests for `why` paths.

*Gate:* resolves against the fake index; conflict messages print both dependency paths.

**M3 — store + build (path deps, offline)** *(done; see `BUILD.md` B1–B5)*
1. Store: content-addressed layout, atomic extract (tmp+rename), sha256 verify.
2. Build plan + `emeraldc` exec + diagnostics passthrough.

*Gate:* a two-package path-dep project builds and runs; a second build is a no-op.

**M4 — registry reads** *(done)*
1. Registry reads §7.4: `GET index`, `GET tarball`, verify, extract.
2. `pme install`, `tree`, `why`.

*Gate:* `pme install` from a stub index resolves, downloads, extracts, locks.

**M5 — registry writes** *(not started)*
1. Reproducible tarball builder (sorted entries, fixed mtime/uid/gid/mode) + `.pmeignore`.
2. `pme publish` validation (name/semver/license/no-path-deps/`[lib]`, no `a/b.rald`+`a.b.rald` ambiguity) + dry-run compile.
3. `pme login`/`yank`; Stage-1 NDJSON index (git repo + HTTPS).

*Gate:* publish produces byte-identical tarballs across runs; re-publish of an existing version is rejected; a yanked version stays downloadable but unselectable.

**M6 — polish** *(initial pass)*
1. `pme test` (compile+run `tests/*.rald`, §9), `pme verify`, `--json`/`-q` everywhere.
2. Docs.

*Gate:* full CLI surface green on path and registry deps; every command accepts `--json`/`-q`; exit codes match §6.

Read the matching entries in `REFERENCES.md` before each step.

---

## 12. Open questions

1. **Stdlib boundary.** Do `strings`/`json`/`math` ship inside `emeraldc`, or as pme
   packages? Still open — the compiler still ships no stdlib, just sixteen builtins
   (`print`, `len`, `range`, `str`, `int`, `sqrt`, `tan`, `rand`, `map`, `filter`,
   `reduce`, `read_file`, `write_file`, `append_file`, `run`, `gc_stats`) — but
   upstream has moved toward the recommendation below. The Phase-2 plan
   (`docs/SPEC_V2.md` §7, "D-open") proposes the split explicitly: tensor primitives
   that need special typing rules stay builtins, the derived ops become a `tensor`
   module written in Emerald — a small set compiled in, everything else in
   modules/packages, and the module system finally dogfooded by a real library.
   Should still be decided before milestone 5.
2. **Compiler version pinning.** Should `emerald = ">=0.2.0"` be enforced by pme
   invoking `emeraldc --version`? Still open — re-verified against `src/main.c` at HEAD
   (`1facafe`): the driver accepts `--emit-tokens`, `--emit-ast`, `--check`, `--emit-c`,
   `--json`, `--proof`, `--keep-c`, `-o`, `-I`, and nothing else. `--version` still has
   to be added upstream before `E_RESOLVE_COMPILER` (§4) can actually fire. (The full
   build honors `$CC` and `$EMERALD_SRC`, which pme could set if it ever needs to steer
   the C toolchain.)
3. **`emerald-lsp` integration.** The LSP must read `emerald.lock` and pass the same
   `-I` roots, or cross-package go-to-definition breaks. Worth designing alongside
   milestone 3. Unchanged upstream: no LSP work yet, but the compiler's `--json`
   diagnostics are explicitly built for a tool/LLM repair loop (`docs/diagnostics.md`),
   the same consumer a shared lockfile serves.
4. **Prereleases.** MVS + prereleases interact badly. Simplest answer: prereleases are
   never selected automatically, only by exact pin.
5. **Type re-export.** A package can only expose a type through `from m import T`
   (§0.4) — `m.T` in a type position is rejected (re-verified in `docs/modules.md` at
   HEAD). A package with a deep internal module layout therefore has no way to surface
   its types from a single `lib.rald` façade without redeclaring each alias. Tolerable
   at milestone 3; revisit if package authors hit it.
