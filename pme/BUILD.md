# pme — the Build System

**Status:** initial implementation. This document is the spec for the
build-system half of pme and its remaining upstream-dependent work. The package-manager
half is `DESIGN.md`; prior art for everything here is `REFERENCES.md`.

pme is *two* tools with one seam:

- a **package manager** (resolve, fetch, verify, publish) — `DESIGN.md`;
- a **build system** (compute the module search path, detect change, drive
  `emeraldc`, cache artifacts, forward diagnostics) — this document.

The seam between them is one frozen contract: an **ordered list of `-I` roots**
and a **lockfile** (`emerald.lock`). pme never parses `.rald`, never rewrites
imports, and never generates code. It computes inputs and execs the compiler.

---

## 1. Build model

### 1.1 Today: one exec per target

`emeraldc` links the whole import graph into **one translation unit** and, on a
full build, compiles the generated C itself (§0.4 of `DESIGN.md`). So a "build"
of one target is a **single subprocess**:

```
emeraldc -I <root1> -I <root2> … -o target/debug/<binname> src/main.rald
```

Consequences that shape the design:

1. **Incrementality is coarse.** There is no per-module object file to skip. If
   any module in the transitive graph changes, the whole target recompiles.
   True per-module incremental compilation requires an upstream change
   (per-module `.gen.c` — see §7). Until then pme's job is to make the
   *all-or-nothing* rebuild correct, cheap, and skippable.
2. **Parallelism is across targets, not within a target.** Two bins and a lib
   can build concurrently; one bin cannot.
3. **The C toolchain is inherited, not owned.** `emeraldc` compiles C with
   `$CC` and `$EMERALD_SRC` (§0.3). pme's build surface is therefore a thin
   orchestrator, exactly like `cargo` in front of `rustc`.

### 1.2 The build plan

`pme build` never invents a graph. It derives one, deterministically, from the
manifest + lockfile. The **build plan** is a pure function:

```
build_plan(manifest, lockfile, target_flags) -> {
  targets:  [ {name, entry, kind: bin|lib, roots: [dir...] , mode} ... ],
  roots:    [ordered -I dirs shared across targets],
  fingerprint: sha256(...)
}
```

- **roots** = each locked package's `src/` directory, in **dependency order
  (dependencies before dependents), ties broken by name** (§5.2 of `DESIGN.md`).
- The entry project's own `src/` is *not* in `roots` — the compiler finds it by
  walking up from the entry file (§0.2), and it is searched before every `-I`.
  pme relies on that precedence and documents it.
- `targets` = every `[[bin]]` plus `[lib]` in the manifest.

The plan is serialized to `target/build-plan.json` so the LSP and other tools
consume the exact same `-I` roots without recomputing them (mirrors §12 Q3 of
`DESIGN.md`).

---

## 2. Targets

From `emerald.toml`:

```toml
[[bin]]
name  = "myapp"
entry = "src/main.rald"

[lib]
root = "src/lib.rald"
```

Rules:

- Exactly one of `[lib]` or `[[bin]]` is required; both may be present (§2 of
  `DESIGN.md`).
- `[[bin]]` is an array: **multiple binaries are legal** and each is its own
  build target. `entry` is required per bin; `name` defaults to the entry file's
  stem if omitted.
- `[lib]` compiles `root` in `--check` mode by default (no binary is emitted for
  a lib unless it has a bin that links it — see §5). `[lib]` presence is what
  makes a package publishable.
- A target is **stale** when its fingerprint differs from the cached one (§4).
- `pme build` with no args builds all targets; `pme build <name>` builds one.
  `pme run [bin]` builds that bin then execs its artifact.

### 2.1 Entry discovery and the `src/` root rule

The compiler resolves a dotted path by searching, in order: (1) the importing
file's directory, (2) the project `src/` found by walking up from the entry,
(3) each `-I` in order (§0.2). pme must therefore:

- pass each dependency's `src/` as an explicit `-I` (correctness requirement, not
  convenience — a dependency's own `src/` is never an implicit root);
- never shadow a dependency's `src/` with the entry project's `src/` — the
  compiler's precedence does that *by design*, and `pme add` warns on collision
  (§5.2 of `DESIGN.md`).

---

## 3. Output layout

```
target/
  debug/            # default profile (emeraldc always uses -O2 today; see §7)
    <binname>       # executable (or, for --check, nothing)
  release/          # reserved profile; currently identical to debug
  .pme/
    fingerprints/<target>.sha256   # cached input hashes, one per target
    build-plan.json                # the serialized plan for tooling
    keep/                          # generated .c from --keep-c builds
```

- Artifacts live only under `target/`, which is gitignored. `pme clean` removes
  it; `pme clean --store` also prunes unreferenced store entries.
- The **profile split** (`debug/` vs `release/`) exists so a real
  debug/release distinction can be slotted in upstream without changing the
  layout. Today both compile identically (`-O2`); `--release` is a documented
  no-op (§6 of `DESIGN.md`).

---

## 4. Fingerprinting & incrementality

The single most valuable build feature is **correct change detection**: skip the
`emeraldc` exec entirely when nothing relevant changed.

### 4.1 Fingerprint inputs

For each target, hash (in a fixed order) the concatenation of:

1. the **compiler identity** — if `emeraldc --version` exists (it does not yet,
   §7) its version; otherwise the resolved `emeraldc` path's mtime+size as a
   stopgap;
2. the **manifest** content (only the fields that affect the build: targets,
   `emerald` constraint, deps);
3. the **lockfile** content (versions + checksums);
4. every **`.rald` file** reachable in the transitive graph — content, not
   mtime (tup-style, `REFERENCES.md` §5);
5. the **target flags** (profile, `--check`/`--emit-c`/`--keep-c`).

Hash with `sha256`. Store `target/.pme/fingerprints/<target>.sha256`.

### 4.2 Rules

- **Content-hash, not mtime.** Two builds of identical source must be
  byte-identical in decision (and ideally in artifact — see §7.3).
- **Skip if unchanged.** If the stored fingerprint equals the fresh one and the
  artifact exists, do nothing (print nothing unless `-v`).
- **Always rebuild if the artifact is missing** even when the fingerprint
  matches (a deleted binary is a change).
- **Never trust a partial artifact.** Write to `target/.tmp/`, then
  `os.replace` into place (§10.3 of `DESIGN.md`).
- **No network during `pme build`** when the lock is satisfied by the store
  (§10.3 of `DESIGN.md`). Fingerprinting must not require network.

### 4.3 What this buys before per-module compilation

Even though a change recompiles the whole target, fingerprinting makes:
- `pme build` idempotent and near-instant on no-ops;
- CI able to skip the (slow) C compile when sources, lock, and compiler are
  unchanged;
- the future remote cache (`REFERENCES.md` §5) a drop-in: the fingerprint *is*
  the cache key.

---

## 5. Compilation modes

pme forwards the compiler's modes rather than reimplementing them (§0.3):

| pme surface | emeraldc flags | behavior |
|---|---|---|
| `pme build` | `-I … -o target/…` | full build (typecheck + codegen + C compile + link) |
| `pme check` | `--check -I …` | typecheck the linked program, emit nothing |
| `pme emit-c` | `--emit-c -I …` | emit the generated C for inspection |
| `pme build --keep-c` | `--keep-c` | keep the generated `.c` in `target/.pme/keep/` |
| `pme check --proof` | `--check --proof` | proof mode — **not** used by pme by default; exposed for advanced users only (§0.5 note) |

- `--check`, `--emit-c`, and full build all operate on the **linked program**, so
  they all consume the same `-I` roots and the same build plan.
- `--emit-tokens` / `--emit-ast` are per-file views and do **not** follow imports
  (§0.3) — they are out of scope for `pme build` and, if exposed at all, belong
  to a separate dev subcommand.

---

## 6. C toolchain passthrough

`emeraldc`'s full build honors `$CC` and `$EMERALD_SRC` (§0.3). pme must:

1. **never** override these env vars it did not set;
2. pass them through unchanged to every `emeraldc` subprocess;
3. expose them in `pme build`'s environment contract so users can steer the C
   toolchain (e.g. `CC=clang pme build`) without pme intercepting flags.

There is no debug/release switch and no CFLAGS injection point in the compiler
today (§7). Do **not** invent a `[build]` section or a script hook: installing a
package must never execute its code (§5.2 of `DESIGN.md` — a load-bearing
security property).

---

## 7. What's missing upstream (build blockers)

Each item is a dependency pme has on the compiler. Track these in
`DESIGN.md` §0/§12 and re-verify at every milestone.

| # | missing | why pme needs it | workaround today |
|---|---|---|---|
| 1 | `emeraldc --version` | fire `E_RESOLVE_COMPILER`; drive compiler-identity fingerprinting precisely | fingerprint by `emeraldc` path mtime+size |
| 2 | debug/release distinction | make `--release`/`debug` meaningful | both profiles compile identically; `--release` is a documented no-op |
| 3 | per-module `.gen.c` compilation | true incremental builds; intra-target parallelism | coarse all-or-nothing rebuild per target (§4.3) |
| 4 | `$CFLAGS`-style injection point | real C-toolchain steering | only `$CC`/`$EMERALD_SRC` passthrough |
| 5 | watch/filesystem hooks | `pme watch` | out of scope; use a generic watcher |
| 6 | stable artifact metadata (compile hashes/`--version` of linked modules) | byte-reproducible artifacts | fingerprint source, not binary, for cache keys |

Items 1–2 are small upstream changes and should be requested early (they unblock
correct fingerprints and honest profiles). Items 3–4 are larger and arrive with
whatever upstream decides for split codegen.

---

## 8. Parallelism & scheduling

- **Across targets:** build independent targets concurrently (a thread pool or
  `asyncio` subprocesses, bounded by CPU count). Each target is one `emeraldc`
  exec; two execs that do not share a lock write are safe to run together.
- **Within a target:** no parallelism today (single translation unit). Design
  the scheduler so per-module units can be plugged in later without changing the
  plan shape.
- **Store contention:** all store access is serialized by a flock
  (`~/.emerald/.lock`) — §10.3 of `DESIGN.md` — so concurrent builds never
  corrupt the store even when their compiles overlap.

---

## 9. Diagnostics & the JSON contract

`pme build --json` produces **one JSON stream** describing the whole build:

- pme emits its own objects in the §8 of `DESIGN.md` shape (`manifest`,
  `resolve`, `registry`, `io` kinds);
- `emeraldc --json` output is forwarded **verbatim**, attribute-by-attribute,
  so the compiler's `syntax`/`type`/`internal` kinds stay intact.

The two kinds are disjoint (§8 of `DESIGN.md`), so any consumer — including
`emerald-lsp` and an LLM repair loop — can attribute every object to one
producer. pme must not re-wrap, re-order, or swallow compiler diagnostics.

Exit codes are the package-manager contract: `0` ok, `1` user/build error,
`2` bad usage, `3` network/registry error (§6 of `DESIGN.md`). A non-zero
`emeraldc` exit maps to `1` (build error).

---

## 10. Build cache & clean

- **Local:** fingerprints (§4) are the local "cache" — a no-op rebuild is the
  local cache hit. Optionally copy artifacts into `target/.pme/cache/` keyed by
  fingerprint to survive `pme clean` of the profile dir.
- **Remote (future):** the fingerprint doubles as a content-addressable cache
  key; a remote cache is a GET/PUT of `key -> artifact` (Bazel/ccache model,
  `REFERENCES.md` §5). Defer until build times hurt; the fingerprint design makes
  it additive.
- **`pme clean`** removes `target/`. **`pme clean --store`** additionally prunes
  store entries unreferenced by any known lockfile (reachability GC, Nix-style).
- **`pme verify`** re-hashes every store entry against `emerald.lock`
  (§6 of `DESIGN.md`).

---

## 11. Build-system implementation route

Ordered so each step is independently verifiable and needs nothing from the next.
Steps map to `DESIGN.md` milestones 1–3 and 6.

### B1 — Build plan from path deps only *(no network, no registry)*

**Goal:** `pme build` computes `-I` roots from `emerald.toml` + a path-dep
`emerald.lock` and execs `emeraldc` correctly.

1. Parse manifest (`[lib]`/`[[bin]]`) and lockfile.
2. Compute the topological order of path-dep packages (deps before dependents,
   name tiebreak).
3. Build the argv: `emeraldc -I <each src/> -o target/debug/<bin> <entry>`.
4. Exec with `argv` list only (never `shell=True`), inherit env, forward exit
   code + streams.

**Verify:** a two-package path-dep project (app + `strings` path dep) compiles
and runs; a dependency's own `src/` resolves its internal sibling imports
(§0.2 correctness case). Re-check `emeraldc`'s flag set at HEAD first
(`REFERENCES.md` §8 rule).

### B2 — Fingerprinting & no-op builds

**Goal:** a second `pme build` with no changes does nothing; a source edit
triggers a rebuild.

1. Implement the §4.1 fingerprint over manifest+lock+sources+flags.
2. Persist `target/.pme/fingerprints/<target>.sha256`; compare before exec.
3. Handle the "artifact missing → rebuild" rule.

**Verify:** three builds in a row (no change / touch a dep / delete the binary)
produce skip / rebuild / rebuild respectively; `--json` reports which.

### B3 — Multiple targets & parallelism

**Goal:** many bins + a lib build correctly, concurrently.

1. Enumerate targets from the manifest (§2).
2. Schedule independent targets on a bounded pool; share one flocked store.
3. Emit `target/build-plan.json` after a successful build.

**Verify:** a project with two bins and a lib builds both; `pme build <name>`
builds only one; concurrent builds don't corrupt the store.

### B4 — Profiles, modes, clean

**Goal:** the full §5 mode surface and §3 layout.

1. Route `--check`/`--emit-c`/`--keep-c` to the right flag set (same roots).
2. Implement `debug/` vs `release/` layout (release = no-op today, documented).
3. `pme clean`; `pme verify` (store re-hash).

**Verify:** `pme check` typechecks without emitting; `--keep-c` drops `.c` into
`target/.pme/keep/`; `pme clean` restores a pristine tree.

### B5 — Diagnostics contract & polish

**Goal:** one merged `--json` stream, correct exit codes, `-q`, docs.

1. Forward `emeraldc --json` verbatim; emit pme objects in the §8 of
   `DESIGN.md` shape.
2. Map exit codes per §9.
3. Add `--json`/`-q` to every build command; document the seam.

**Verify:** `pme build --json` on a type error yields a stream where every object
carries a disjoint `kind` and can be attributed to pme or the compiler.

### B6 — (Blocked on upstream) incremental + cache

**Goal:** plug in per-module compilation and remote caching once upstream ships
§7 items 3–4.

1. Replace the per-target exec with per-module units; reuse §8 scheduling.
2. Add remote cache GET/PUT keyed by §4 fingerprints.

**Verify:** recompile only the changed module and its dependents; a cold CI node
restores artifacts from cache.

---

## 12. Definition of done (build system)

- `pme build [--release] [--keep-c]`, `pme check`, `pme emit-c`, `pme run`,
  `pme clean [--store]`, `pme verify` all work end to end on path deps, then on
  registry deps.
- No-op builds skip the compiler; edited sources always rebuild; deleted
  artifacts always rebuild.
- `--json` yields one attributable, merged stream; exit codes match §9.
- Every filesystem write is atomic; store writes are flock-serialized; no
  network is used during build when the lock is satisfied.
- The build never executes package code; no build scripts exist.
- `target/build-plan.json` is emitted and is the single source of `-I` truth for
  the LSP.
