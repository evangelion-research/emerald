# What remains before Emerald is a publishable language and ecosystem

> **Status: implementation pass of 2026-09-18.** A working pass over this list
> landed in the same tree: P0.1/P0.2/P2.1/P2.6 (the combined
> `libemerald.a` + argv-exec fix), P0.3 (CI, release workflow, VERSION,
> changelog/README truth), P0.4 (platform statement), P0.5 + P1.4 decision
> (`docs/stability.md`), P1.1 (pme add/why/update/--locked, run/test I/O),
> P1.2 (emlsp README), P1.3 (tree-sitter manifest, VS Code extension
> scaffold), P1.5 (getenv/mkdir_all/remove/listdir builtins), P2.2 (`#line`),
> P2.3 (doc honesty), P2.5 (examples in `task test`), and the P3 files.
> Remaining: tagged release + published binaries, the live registry +
> `publish`, stdlib breadth beyond filesystem/env, a formatter, WASM
> playground, and the ecosystem items below that need services. The audit text
> itself is left unchanged below as the record of what was found.

Audit date: 2026-09-18. Working tree at `0bb29b7`, clean.
Re-verified: 2026-09-18, same commit. **Every finding below is still open**; two new
ones were added (P0.3 changelog, P2.6 litter). See [Re-verification log](#re-verification-log).

This is a **shipping/distribution** audit, not a language-design audit. Language and
proof-system gaps are already catalogued in
[`docs/REMAINING_FEATURES.md`](docs/REMAINING_FEATURES.md) and are not duplicated here
except where they block publication. Every claim below was verified by running the
code; the evidence is cited inline.

---

## Verdict

**Emerald is not publishable today.** The quality of the compiler itself is not the
problem — it is genuinely good. The problem is that almost nothing outside the source
checkout works.

Two findings are hard blockers on their own:

1. **The release artifact produced by `task dist` cannot compile a program.** Anyone who
   downloads a release gets a compiler that type-checks and then fails at the `cc` step.
2. **`emeraldc` has a shell command-injection hole** reachable from an ordinary
   command-line argument.

Beyond those, the ecosystem is a set of prototypes that each overclaim in their own
README, there is no CI, no tagged release, no published binary, no working package
registry, no editor extension, no FFI, and no stability policy. The repository presents
itself as `1.0.0`; by the standards a user would apply to that number, it is closer to a
strong `0.4`.

### What is genuinely solid (verified, not taken on faith)

Stating this first because the gap list below is long and it would otherwise misrepresent
the project.

| Claim | How it was verified | Result |
|---|---|---|
| Test suite passes | `task test` | **192 passed, 0 failed**, exit 0 |
| Examples still run | `task examples` | exit 0, no errors |
| Runtime is compiler-independent | `task runtime-check` | clean |
| Compiler is memory-safe on valid input | ASan+UBSan build over all of `tests/check`, `tests/proof`, `tests/parser`, `examples`, `stdlib` | **0 sanitizer reports** |
| Frontend is crash-resistant on invalid input | 1,500 truncation/bit-flip/insertion mutations through the ASan build | **0 sanitizer reports, 0 abnormal exits** |
| No rotting shortcuts in the compiler | `grep -rniE 'TODO\|FIXME\|XXX\|HACK'` over `src/ include/ stdlib/` | **0 hits** |
| Diagnostics are documented | 56 emitted diagnostic codes vs. the docs | 55/56 documented (only `E_TYPE_DICT_KEY` missing) |

15.7k lines of C11, zero dependencies beyond libc, no sanitizer findings, and a clean
192-case golden suite is a real achievement. The work that remains is almost entirely
*around* the compiler.

---

## Re-verification log

Re-run of every claim in this document against `0bb29b7`. No item has been closed.

| Item | Re-check performed | Status |
|---|---|---|
| P0.1 broken tarball | `Taskfile.yml` `dist` copies `bin/emeraldc`, `stdlib/*.rald` → `lib/emerald/stdlib/`, `README.md`, `CHANGELOG.md`. No `src/runtime_*.c`, no `include/`. `install` likewise. | **Open** |
| P0.2 command injection | `emeraldc -o "a'; echo INJECTED_RCE; :'b" q.rald` → `INJECTED_RCE` printed twice. `src/main.c:344-348` still builds a shell string for `system()`. | **Open** |
| P0.2 `sprintf` | `src/main.c:113` still `sprintf(cand, "%s%s", dir, suffixes[i])`. | **Open** |
| P0.3 CI | `.github/` does not exist. | **Open** |
| P0.3 tags | `git tag` → 0 tags. `README.md:50` and `CHANGELOG.md:7` still claim `1.0.0`. | **Open** |
| P0.3 version source | Still only `Taskfile.yml:13 VERSION: 1.0.0`; no `VERSION` file. | **Open** |
| P0.4 portability | `grep -rn '__linux__\|__APPLE__\|_WIN32' src/ include/` → **0 hits**. | **Open** |
| P0.5 stability policy | `docs/stability.md` absent; `grep -niE 'semver\|stability\|deprecat'` over `docs/*.md README.md` → **0 hits**. `docs/RELEASE_V1.md` describes the implemented surface, not a compatibility contract. | **Open** |
| P1.1 `pme add`/`why` | Dispatch at `cmd/pme/main.go:809-869` handles `build check emit-c run tree test clean verify` — no `add`, no `why`; usage string at `:706` still advertises `why`. | **Open** |
| P1.1 `pme run`/`test` I/O | `main.go:833` and `:857` still call `exec.Command(...).Run()` with no `Stdout`/`Stderr` (the `build` path at `:693-697` sets both). | **Open** |
| P1.1 `pme init` `.gitignore` | `grep -n gitignore cmd/pme/main.go` → **0 hits**. | **Open** |
| P1.1 `pme` tests | 23 lines, one file. | **Open** |
| P1.1 stale pin | `pme/README.md` still pins `emerald@1facafe` (2026-08-17). | **Open** |
| P1.2 `emlsp` tests | 119 lines across `internal/lexer` and `internal/positions` only; 9 other packages have none. README (`:139-142`) still claims fuzz + e2e stdio sessions. | **Open** |
| P1.3 tree-sitter | `package.json` still `"version": "0.1.0"`, `"private": true`; one corpus file, 118 lines. | **Open** |
| P1.3 IDE | `emerald-ide` tests total 83 lines; no packaging. | **Open** |
| P1.5 stdlib gaps | No `getenv`, `mkdir`, `listdir`, or `remove` in `stdlib/*.rald` or `include/builtins.def`. Module count still 13. | **Open** |
| P2.2 `#line` | `grep -c '#line' src/codegen*.c` → **0** in all five codegen files. | **Open** |
| P2.5 examples gating | `task test` deps are `[build]` + `runtime-check` + `run_tests.sh`; `examples` is still not among them. | **Open** |
| P3 community files | `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `CITATION.cff`, `PULL_REQUEST_TEMPLATE.md` — all absent. | **Open** |
| P2.4 GPU | `gpu/` still contains exactly one file, `PLAN.md`. | **Open** |
| Baseline still good | `task test` → **192 passed, 0 failed**. | Holding |

---

## P0 — Hard release blockers

### P0.1 `task dist` and `task install` ship a compiler that cannot compile

`src/main.c:336-347` shells out to `cc` with the runtime **sources**, globbed out of a
directory baked in at build time:

```c
const char *srcdir = getenv("EMERALD_SRC");
if (!srcdir || !*srcdir) srcdir = EMERALD_SRC_DIR;   /* = $PWD/src at build time */
...
"%s -std=c11 -O2 -pthread -I '%s' -I '%s/include' -o '%s' '%s' '%s'/runtime_*.c"
```

`Taskfile.yml` `dist` copies **only** `bin/emeraldc`, `stdlib/*.rald`, `README.md`, and
`CHANGELOG.md`. It ships neither `src/runtime_*.c` nor `include/`. `install` is the same.

Reproduced:

```
$ task dist && tar -xzf emerald-1.0.0.tar.gz -C /tmp/d
$ EMERALD_SRC=/tmp/d/emerald/src /tmp/d/emerald/bin/emeraldc -o t t.rald
clang: error: no such file or directory: '/tmp/d/emerald/src/runtime_*.c'
emeraldc: C compilation failed
```

`find_exe_stdlib()` (`src/main.c:95-119`) already does relocatable lookup for the stdlib.
Nothing equivalent exists for the runtime sources or headers, so the installed binary is
permanently bound to the absolute path of the machine that built it. The Taskfile comment
on `dist` — *"smoke-test it outside the checkout before release"* — describes a step that
has evidently never been run.

**Required:**
- Ship `src/runtime_*.c` and `include/` in both `install` and `dist`, under
  `<prefix>/lib/emerald/`.
- Give the runtime the same relocatable search ladder as the stdlib (sibling, `../lib/emerald`,
  `../share/emerald`, then the compile-time default), with `$EMERALD_SRC` as override.
- Better: precompile the runtime once into `libemerald.a` at install time and link it.
  This also fixes P2.1.
- Add a `task dist:verify` that unpacks the tarball into a temp dir, compiles and runs
  hello-world **with the checkout renamed or `PATH`-isolated**, and fails the build if it
  does not. Without this gate the bug returns.

### P0.2 Shell command injection in the `cc` invocation

The same `snprintf` builds a shell string passed to `system()`. Paths are wrapped in
single quotes but never escaped, so a single quote in the output path terminates the
quoting.

Reproduced:

```
$ emeraldc -o "a'; echo INJECTED_RCE; :'b" q.rald
INJECTED_RCE
INJECTED_RCE
```

The default output path is derived from the *input* filename, so the input path is the
same vector. Any build system, CI job, editor integration, or `pme`-style driver that
passes a path containing attacker-influenced text executes arbitrary commands. For a
compiler that is meant to be run by tooling on untrusted repositories, this is
disqualifying.

**Required:** replace `system()` with `posix_spawn`/`fork`+`execvp` and an argv array. The
`runtime_*.c` glob must then be expanded in C (or replaced by the explicit file list, or
by linking `libemerald.a` per P0.1) rather than delegated to the shell. While there:
`src/main.c:113` uses `sprintf` into a heap buffer — switch to `snprintf`; it is the only
compiler warning in the ASan build.

### P0.3 No CI, no tags, no releases, no reproducible build

- `.github/` does not exist. Nothing runs `task test` on push.
- `git tag` is **empty**. `CHANGELOG.md` announces `[1.0.0] - 2026-08-19` and the README
  states *"The current release version is 1.0.0"* — **no such release exists.** Publishing
  a changelog entry for an untagged, unreleased version is the kind of thing that costs a
  project its credibility on day one.
- *(new, found on re-verification)* `CHANGELOG.md:51` opens a section headed
  **`### Added since 1.0.0`** — a fourth-level-of-nowhere bucket holding autograd, the
  unicode layer, the `test` module and more, under no version and no `[Unreleased]`
  heading. The file declares at the top that it follows Keep a Changelog and semver; this
  section conforms to neither, and it means the project's largest features currently belong
  to no release at all — not even the fictional one. Either cut a real `1.0.0` that
  contains them, or move them under `## [Unreleased]`.
- The version lives only as `VERSION: 1.0.0` in `Taskfile.yml`, injected via `-D`. There is
  no single source of truth shared with `pme`, `emlsp`, `emerald-ide`, or the grammar
  (which independently declares `0.1.0`).
- No prebuilt binaries for any platform. The install story is "have a C compiler, have
  go-task, clone the repo."

**Required:** GitHub Actions running `task test`, `task runtime-check`, `task examples`,
plus `go test ./...` in each Go subproject, on macOS **and** Linux, on every push;
a release workflow that tags, builds per-platform tarballs, runs the P0.1 smoke test
against each, and attaches checksums; and one `VERSION` file the Taskfile and every
subproject read.

### P0.4 POSIX-only, single-platform-tested, no cross-compilation

The compiler includes `<unistd.h>` and `<libgen.h>`, the runtime uses `pthread`, and the
`cc` step goes through `sh`. There is **not one `#ifdef __linux__`, `__APPLE__`, or
`_WIN32`** in `src/` or `include/`. Nothing has been built or tested on anything but macOS
(the only build artifact present is a Darwin arm64 binary with a `.dSYM`).

- Windows is not merely unsupported, it is unconsidered. `dirname`, `realpath`,
  `access`, `/`-joined paths, `system()` with `sh` quoting, and the `runtime_*.c` glob all
  need replacing.
- No target selection: the generated program is always compiled for the host with a fixed
  `-O2`. No `--target`, no `-O` passthrough, no static-linking flag, no `-L`/`-l`.

**Required (minimum for a v1):** Linux x86-64 and arm64 in CI with published binaries;
macOS arm64 and x86-64; a written, honest statement that Windows is unsupported —
or MSVC/MinGW support. Pick one and say so in the README.

### P0.5 No language stability or versioning policy

Nothing anywhere states what `1.0.0` promises. There is no document answering:

- Is the surface syntax frozen? Which parts?
- What is the deprecation policy for a builtin, a stdlib function, a diagnostic code?
- Are diagnostic codes and the `--json` schema stable contracts? (`pme` and `emlsp` both
  parse `--json`, and the `--json` schema is pinned only by golden files.)
- Is the `-I` contract that `pme` calls "frozen" actually frozen, and by whom?
- What is the minimum supported C compiler / C standard / OS version?

Publishing `1.0.0` without this means every future change is a potential breaking change
with no way for users to reason about it.

**Required:** a `docs/stability.md` defining the semver boundary, what is inside it (syntax,
stdlib signatures, CLI flags, `--json` schema, diagnostic codes, the `-I` contract), what is
explicitly outside it (generated C, proof-mode acceptance set, runtime internals,
`--emit-*` formats), and the deprecation window.

---

## P1 — Ecosystem blockers

The ecosystem is four separate Go/JS projects vendored into this repo as plain
directories (not submodules; all 61 files are tracked here). Each has its own `LICENSE`,
`go.mod`, `Taskfile.yml`, and its own version number. None is published anywhere.

### P1.1 `pme` — the package manager does not manage packages

903 lines in one file, with **23 lines of test** (one test function). Verified by building
and running it end to end:

| README/usage claim | Reality |
|---|---|
| Milestone 4 "registry reads (`add` / `install` / `tree` / `why`) ✅ done" | `pme add` → `error[E_USAGE]: unknown command`. `pme why` → same. Neither exists in the dispatch at `cmd/pme/main.go:809-869`. |
| `pme why <pkg>` listed in the tool's own usage string | Unimplemented. The usage string advertises a command that always errors. |
| `pme init` scaffolds "`emerald.toml`, `src/main.rald`, `.gitignore`" | No `.gitignore` is written. |
| `pme run` | **Silently swallows the program's output.** `exec.Command(...).Run()` at `cmd/pme/main.go:829` sets neither `Stdout` nor `Stderr` (the `build` path at `:696` does). `pme run` on a fresh `pme init` project printed nothing; running `target/debug/demo` directly printed `Hello, world!`. The child's exit code is also dropped. |
| `pme test` | Same defect — test output invisible, so a failing suite reports only `E_TEST_FAILED` with no detail. |
| Registry at `https://evangelion-research.github.io/pme-index` (`main.go:25`) | Nothing indicates this index exists or has ever been populated. |
| Milestone 5 "registry writes (`publish`, Stage-1 index)" | Marked "not started" — accurate. |

`pme` also pins its status text to `emerald@1facafe` (2026-08-17), a month stale.

**A language without a working package manager and a live registry has no ecosystem.**
This is the single largest gap after P0.

**Required:** fix `run`/`test` I/O forwarding and exit-code propagation; implement `add`,
`why`, `update`, `--locked`; stand up the Stage-1 index with at least one real published
package; implement `publish` with reproducible tarballs and checksum verification; raise
test coverage from 23 lines to something that exercises resolution, the store, and the
build driver; correct the README's milestone table to match the code.

### P1.2 `emlsp` — the README describes tests that do not exist

4,212 lines of Go. The README states:

> *"The suite covers the lexer, the outline, position encoding, the diagnostic mapping,
> the lockfile `-I` rule, and the features — plus ... a truncation fuzz over every prefix
> of several samples, and end-to-end sessions that drive the real server process over
> stdio with a stub compiler."*

`go test ./...` output:

```
ok      .../internal/lexer
ok      .../internal/positions
?       .../internal/server        [no test files]
?       .../internal/features      [no test files]
?       .../internal/diagnostics   [no test files]
?       .../internal/outline       [no test files]
?       .../internal/compiler      [no test files]
?       .../internal/modules       [no test files]
?       .../internal/semantic      [no test files]
?       .../internal/language      [no test files]
?       .../cmd/emerald-lsp        [no test files]
```

**Two** packages have tests, totalling 119 lines. There is no fuzz harness and no
end-to-end stdio session test. The 889-line JSON-RPC server, the 623-line feature layer,
and the 397-line diagnostic mapper are entirely untested. The README is not describing
aspiration — it is describing, in the present tense, tests that are absent.

The server's own capability table is honest about its functional limits (lexical-only
tokens, no inferred-type hover, no rename/inlay/format, scope-approximate goto) and those
limits are acceptable for a v0.1. The false test claim is not.

**Required:** correct the README to describe what exists; add tests for the server
lifecycle, diagnostics mapping, and features before calling it shippable; publish
binaries.

### P1.3 No editor integration is actually distributed

`emlsp`'s README gives Neovim and Helix config snippets and says *"Zed / VS Code: point
the client at the `emerald-lsp` binary over stdio."* That is a instruction to build your
own extension.

Missing entirely:
- A VS Code extension (marketplace or `.vsix`) — this is how the overwhelming majority of
  users would ever try the language.
- A published tree-sitter grammar. `tree-sitter-emerald` is `"private": true` in
  `package.json`, not on npm, not in crates.io, not in `nvim-treesitter`, not in the
  tree-sitter org's grammar list. It has **one** corpus file (118 lines) — a grammar
  covering a 169-rule `grammar.js` with a single 118-line corpus file is essentially untested,
  and there is no test verifying it agrees with `src/lexer.c`/`src/parser_*.c`.
- A TextMate grammar is present (`emlsp/emrald.tmLanguage`) but unwired and misspelled.
- GitHub Linguist registration, so `.rald` files render with highlighting on the site
  where the project lives.
- Any distribution of `emerald-ide` (1,336 lines of Fyne, 83 lines of test, no packaging,
  no signing, no release).

### P1.4 No FFI — a hard ceiling on the ecosystem

There are 167 builtin/runtime symbols in `include/builtins.def` and **no mechanism in the
language to call a C function.** No `extern`, no `dlopen`, no `cimport`, nothing. The only
way to reach a system library is to edit the compiler and add a builtin.

The consequence is structural: no sockets, no TLS, no SQLite, no BLAS, no image codecs, no
GPU library, and no third-party package can ever provide them. Every capability must ship
in-tree forever. The `gpu/PLAN.md` CUDA work is itself a demonstration of this — it has to
be a compiler/runtime change because there is no other door.

**Required for a real ecosystem:** a declared foreign-function interface with typed
signatures, a marshalling story for `Value`, a GC-safety contract for foreign calls,
`-L`/`-l`/`--link` passthrough in the driver, and a `pme` manifest key for native
dependencies. This is a large design task and it should be scoped and decided *before*
`1.0.0` freezes the surface, because retrofitting FFI across a stability boundary is
painful.

### P1.5 Standard library gaps that block ordinary programs

13 modules, ~1,970 lines, ~240 functions. Well-written, but the coverage is that of a
teaching language, not a usable one. Verified absent:

- **Filesystem:** no `mkdir`, `remove`, `rename`, `stat`, `listdir`, `glob`, no temp files,
  no directory traversal. `io` can read and write whole files and that is all.
- **Environment:** no `getenv`/`setenv`. `sys` exposes `argv` and `exit` only. A program
  cannot read `$HOME` or `$PATH`.
- **Time:** the `now` builtin returns a number. No date type, no formatting, no parsing,
  no timezones, no duration, no monotonic clock distinction.
- **Randomness:** `rand`/`seed_rand` builtins only — no distributions, no explicit RNG
  state value (which `docs/REMAINING_FEATURES.md` §6 also flags as a proof-mode problem).
- **No JSON**, **no regex**, **no networking**, **no subprocess control** beyond the
  fire-and-forget `run`, **no hashing/crypto**, **no base64/hex**, **no compression**.
- **Collections:** `dict`/`set` are dynamic builtin values with no module of helpers and no
  static typing; there is no ordered map, no deque, no priority queue, no persistent
  structure (the last is also a P1 item in `docs/REMAINING_FEATURES.md` §7).

Several of these (net, crypto, compression) are unreachable without P1.4.

### P1.6 Missing developer tooling

No formatter (`emeraldfmt`), and `emlsp` cannot implement `textDocument/formatting`
without one — a language shipping in 2026 without a canonical formatter will be judged for
it. No documentation generator for `.rald` source. No coverage tooling. No debugger story
(see P2.2). No `emeraldc --explain E_CODE`. No linter beyond the compiler's warnings. No
playground / browser try-it (the compiler is C11 with no external deps — a WASM build is
unusually tractable and would be the highest-leverage marketing artifact available).

---

## P2 — Implementation quality gaps

### P2.1 Compile time is dominated by recompiling the runtime, every time

Every single `emeraldc` invocation recompiles all nine `src/runtime_*.c` files from source.

```
hello.rald                       0.69s
ray_tracer/one_weekend.rald      0.87s
ray_tracer/typed/main.rald       0.89s   (13 modules)
```

The Emerald frontend is not the cost — the constant ~0.7s floor is. A hello-world that
takes two-thirds of a second to build is a first impression the project cannot afford, and
it makes the REPL (which recompiles the whole session per entry, by design) and any
edit-compile loop sluggish.

**Fix:** build the runtime once into `libemerald.a` at install/build time and link it.
This is the same change P0.1 wants, and it removes the `runtime_*.c` glob that P0.2 needs
gone. Three blockers, one fix — do this first.

Also absent: incremental compilation (the compilation unit is the whole import graph, so
touching one file rebuilds everything), parallel module checking, and any build cache in
`emeraldc` itself.

### P2.2 No debugging or profiling story

`--emit-c` output contains **zero `#line` directives**. Source locations are tracked by
assigning runtime globals before statements:

```c
rt_cur_file = "examples/hello.rald";
rt_cur_line = 1;
```

That is enough for runtime error messages (which do work) and nothing else. Consequences:
lldb/gdb show generated C, not Emerald; `perf`/Instruments symbolize to `em_*` runtime
functions; there is no DWARF mapping, no breakpoints on Emerald lines, no variable
inspection, and no core-dump triage. The per-statement global writes are also unmeasured
overhead.

**Fix:** emit `#line` directives into the generated C. This is cheap and immediately buys
usable lldb/gdb/perf behaviour, after which the globals can likely be dropped from
release builds.

### P2.3 "Cooperative green threads" are OS threads

`src/runtime_task.c` implements `spawn` with `pthread_create` plus a scheduler
mutex/condvar baton (`sch_mu`/`sch_cv`) so exactly one runs at a time. The semantics
match the documentation, but the cost model does not match the name: each `spawn` is a
full OS thread with a full stack, so task counts are bounded by thread limits rather than
by memory, and every yield is a futex round-trip. `docs/concurrency.md` should say this
plainly, and the implementation should eventually move to `ucontext`/stack-switching (with
the portability caveats that implies) if the "cheap tasks" claim is to hold.

### P2.4 Tensor/autograd performance is acknowledged-naive

`gpu/PLAN.md` states it directly: every tensor primitive is a scalar C loop dispatching
through a `double (*)(double,double)` function pointer, with per-element dtype branching;
`em_tensor_matmul` is a naive triple loop. Nothing vectorizes. Since autograd is described
as the project's most valuable asset, the CPU path needs blocking/vectorization and an
optional BLAS link *before* GPU work, not after. The GPU plan is well-reasoned but it is a
plan, not code — `gpu/` contains exactly one file.

### P2.5 Examples are not gated by `task test`

`task test` does not depend on `task examples`. Examples can silently break; today they
happen to pass (verified). Add them to the suite. Likewise `tests/bench` records
deterministic output but no performance thresholds, so a 10x regression passes CI (once CI
exists).

### P2.6 Failed compiles litter the working directory *(new, found on re-verification)*

`src/main.c:349-353` returns on a non-zero `cc` exit **before** reaching
`if (!keep_c) remove(cfile)`. Every failed build therefore leaves a `<out>.gen.c` behind in
whatever directory the user ran from — reproduced while re-testing P0.2, which left
`a'; echo INJECTED_RCE; :'b.gen.c` on disk. Minor next to the injection in the same
function, but it is in the same four lines and should be fixed in the same pass: free the
temp file on both paths.

---

## P3 — Project and community readiness

None of these files exist:

- `CONTRIBUTING.md` — how to build, test, `task bless`, and what a PR must include.
- `SECURITY.md` — where to report P0.2-class bugs. Currently there is nowhere.
- `CODE_OF_CONDUCT.md`.
- `.github/ISSUE_TEMPLATE/`, `PULL_REQUEST_TEMPLATE.md`.
- `CITATION.cff` — this is explicitly a research language; it should be citable.
- A governance/RFC process for language changes.
- A website or rendered documentation site. 3,539 lines of good reference docs exist in
  `docs/`, but they are GitHub markdown only.
- A **tutorial**. There is no "learn Emerald in an hour" path. `docs/` is reference
  material written for someone who already knows the language; `examples/` is 14 files
  plus the ray tracer. A newcomer has no on-ramp.
- Any published artifact at all: no Homebrew formula, no Nix derivation, no AUR package,
  no Docker image, no `go install`-able tools (they are `go install`-able only if the Go
  module paths resolve publicly, which is untested).

### Documentation accuracy defects found during this audit

These are individually small and collectively corrosive — each one is a promise the
repository makes and does not keep.

| Location | Claim | Reality |
|---|---|---|
| `README.md` "Project status" | "The current release version is `1.0.0`" | No tag, no release, no artifact exists |
| `CHANGELOG.md` | `[1.0.0] - 2026-08-19` | Never released |
| `CHANGELOG.md:51` | `### Added since 1.0.0` | Not a version, not `[Unreleased]`; violates the Keep a Changelog format the file claims (P0.3) |
| `Taskfile.yml` `dist` | "smoke-test it outside the checkout before release" | Tarball is non-functional; the smoke test would have caught it (P0.1) |
| `emlsp/README.md` | Describes a test suite covering 6 areas plus fuzz plus e2e | 2 packages, 119 lines, no fuzz, no e2e (P1.2) |
| `pme/README.md` | Milestone 4 including `add` and `why` "✅ done" | Both unimplemented (P1.1) |
| `pme` usage string | Advertises `why` | Always returns `E_USAGE` |
| `pme/README.md` | `pme init` writes `.gitignore` | It does not |
| `pme/README.md` | Tracks compiler at `emerald@1facafe` (2026-08-17) | A month of compiler changes since |
| `tree-sitter-emerald` | Version `0.1.0`, `"private": true` | Inconsistent with a `1.0.0` language; unpublishable as-is |
| `docs/` | `E_TYPE_DICT_KEY` | Emitted but undocumented |

---

## Exit criteria

Emerald is publishable as `1.0.0` when all of the following hold:

**Distribution**
- [ ] A release tarball, unpacked on a machine with no Emerald checkout, compiles and runs
      hello-world — enforced by a CI gate, not by hand.
- [ ] Prebuilt binaries for macOS (arm64, x86-64) and Linux (x86-64, arm64), checksummed.
- [ ] A git tag whose number matches the README, the changelog, and every subproject.
- [ ] Windows is either supported or explicitly and prominently declared unsupported.

**Correctness and safety**
- [ ] No shell interpolation of any path anywhere in the driver.
- [ ] CI runs `task test`, `runtime-check`, `examples`, and all Go suites on every push,
      on macOS and Linux.
- [ ] A sanitizer job and a fuzz job run on a schedule with a persistent corpus.

**Contract**
- [ ] `docs/stability.md` exists and defines the semver boundary, including the `--json`
      schema, diagnostic codes, and the `-I` contract.
- [ ] A decision (either way, written down) on FFI before the surface freezes.

**Ecosystem**
- [ ] `pme install`/`build`/`run`/`test` forward I/O and exit codes correctly, and `add`,
      `why`, and `update` exist.
- [ ] A live registry with at least one published third-party package, and a working
      `pme publish`.
- [ ] A VS Code extension and a published tree-sitter grammar.
- [ ] A formatter, and `emlsp` wired to it.
- [ ] Stdlib covers filesystem, environment, time, JSON, and hashing at minimum.

**Credibility**
- [ ] Every README states only what the code does — the table above is empty.
- [ ] A tutorial exists.
- [ ] `CONTRIBUTING.md`, `SECURITY.md`, `CITATION.cff` exist.

---

## Suggested order

**Do first, this week — the three P0 code bugs are one change.** Build the runtime into
`libemerald.a`, install it with the compiler, add relocatable lookup, and replace
`system()` with `posix_spawn` over an argv array. That closes P0.1 (broken tarball), P0.2
(injection), and P2.1 (0.7s hello-world) together, and removes the glob that forces the
shell.

1. **P0.1 + P0.2 + P2.1** — the combined fix above, plus `#line` emission (P2.2) while the
   codegen is open.
2. **P0.3** — CI on macOS and Linux, then a real tag and real binaries. Nothing downstream
   is trustworthy until the build is watched.
3. **Documentation truth pass** — fix every row of the accuracy table. It is an afternoon,
   and until it is done every other claim in the repo is suspect.
4. **P0.5** — write `docs/stability.md`. This is the gate on freezing `1.0.0`.
5. **P1.4 decision** — scope FFI, or write down that there will be none and accept the
   ceiling. This must precede the freeze.
6. **P1.1** — make `pme` honest and complete, and stand up the registry. Longest pole in
   the ecosystem.
7. **P1.3 + P1.6** — VS Code extension, published grammar, formatter. Highest
   adoption-per-hour ratio of anything on this list. A WASM playground belongs here too.
8. **P1.5** — stdlib breadth, as far as P1.4 allows.
9. **P1.2, P2.3, P2.4, P2.5, P3** — test the LSP, correct the concurrency cost model,
   optimize the CPU tensor path before GPU, gate examples, add community files.

Language-level work — proof soundness, recursive ADTs, constrained generics, autograd
breadth — is tracked separately in [`docs/REMAINING_FEATURES.md`](docs/REMAINING_FEATURES.md)
and is **not** on the critical path to publishing. Emerald can ship as an honest,
well-scoped experimental language with the type system it has today. It cannot ship with a
release tarball that does not work.
