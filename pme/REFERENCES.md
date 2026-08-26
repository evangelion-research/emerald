# References

Curated prior art for building `pme`. Each entry says what we borrow from it and
links to the authoritative source. Read these **before** implementing the
corresponding piece; they answer the hard 20% (resolution, content-addressing,
reproducibility, incremental builds) that you should not rediscover from scratch.

---

## 1. Package managers & registries (what pme is)

| system | what to borrow | links |
|---|---|---|
| **Cargo** (Rust) | manifest + committed lockfile split (`Cargo.toml` vs `Cargo.lock`); "driver around the compiler" philosophy; `cargo build/run/test/tree/publish/yank` command surface; registry index + checksum model; deterministic build dir layout | [Cargo book](https://doc.rust-lang.org/cargo/), [manifest reference](https://doc.rust-lang.org/cargo/reference/manifest.html), [specifying dependencies](https://doc.rust-lang.org/cargo/reference/specifying-dependencies.html), [source replacement](https://doc.rust-lang.org/cargo/reference/source-replacement.html) |
| **Go modules** | **Minimal Version Selection** (the exact resolution algorithm pme uses — see §2); `go.mod`/`go.sum` split; content-addressed module cache at `$GOPATH/pkg/mod`; `GOPROXY` registry protocol; `go mod why` | [Go modules reference](https://go.dev/ref/mod), [MVS](https://go.dev/ref/mod#minimal-version-selection), [module cache](https://go.dev/ref/mod#module-cache), [GOPROXY protocol](https://go.dev/ref/mod#goproxy-protocol) |
| **Pub** (Dart) | `pubspec.yaml` manifest shape; `~/.pub-cache` content-addressed store; `pub get/publish`; how a registry + version solver pairs with a simple language | [pub package layout](https://dart.dev/tools/pub/package-layout), [pubspec format](https://dart.dev/tools/pub/pubspec) |
| **SwiftPM** | manifest-as-declarative-source + explicit target graph (`products`/`targets`); how a compiler is driven with a single search-path contract | [PackageDescription](https://developer.apple.com/documentation/packagedescription), [SwiftPM docs](https://www.swift.org/documentation/package-manager/) |
| **pnpm** | content-addressable store + hardlinks (never duplicate files across projects); the "store is global, node_modules is a view" split | [pnpm motivation](https://pnpm.io/motivation), [store](https://pnpm.io/cli/store) |
| **npm / Bundler / RubyGems** | contrast cases: what *not* to do (mutating lockfile-first design, node_modules flattening, `.gemspec` runtime eval) | [package-lock.json](https://docs.npmjs.com/cli/v10/configuring-npm/package-lock-json), [Bundler](https://bundler.io/man/bundle-install.1.html) |

## 2. Version resolution

- **Minimal version selection (MVS)** — the algorithm §4 of `DESIGN.md` specifies.
  - [Go modules reference: MVS section](https://go.dev/ref/mod#minimal-version-selection)
  - [Russ Cox, "Minimal Version Selection" (2020) — the design post](https://research.swtch.com/vgo-mvs)
  - Key property to preserve: *adding a dependency never upgrades an unrelated one*; the result is a pure function of the manifest set.
- **SemVer** — the exact version grammar pme accepts.
  - [Semantic Versioning 2.0.0](https://semver.org/)
- **Contrast (to know why we did *not* pick it):**
  - [PubGrub](https://github.com/dart-lang/pub/blob/master/doc/solver.md) — a SAT-like solver; overkill and unnecessary when there is one version per package and no backtracking requirement.
  - [SAT solving in general](https://en.wikipedia.org/wiki/Boolean_satisfiability_problem) — what Cargo's resolver and npm's `pacote` effectively reduce to; weeks of debugging pme does not need.
  - [PEP 440](https://peps.python.org/pep-0440/) — Python's version spec; deliberately *not* reused because Emerald semver is stricter and simpler than PEP 440's epochs/post/dev grammar.

## 3. Content-addressed stores & reproducibility

- **Nix** — the canonical content-addressed, immutable store; fixed-output derivations (hash the tarball before/while fetching); `tmp + rename` atomicity; GC by reachability.
  - [Nix store paths](https://nixos.org/manual/nix/stable/store/store-path.html), [fixed-output derivations](https://nixos.org/manual/nix/stable/language/advanced-attributes.html#adv-attr-outputHash)
- **Go module cache** — immutable, hash-named dirs; `partial` + `lock` files during download; verify-on-read.
  - [module cache](https://go.dev/ref/mod#module-cache)
- **Reproducible tarballs** — the normalization pme's `publish` needs (sorted entries, fixed mtime/uid/gid/mode):
  - [Reproducible Builds](https://reproducible-builds.org/), [tar(5) + `--sort=name`](https://www.gnu.org/software/tar/manual/html_node/Option-Summary.html)

## 4. Lockfiles & supply-chain integrity

- **Lockfile formats to copy the spirit of:** [Cargo.lock](https://doc.rust-lang.org/cargo/guide/cargo-toml-vs-cargo-lock.html), [go.sum](https://go.dev/ref/mod#go-sum-files), [package-lock.json](https://docs.npmjs.com/cli/v10/configuring-npm/package-lock-json), [pubspec.lock](https://dart.dev/tools/pub/glossary#lockfile).
- **Checksums + index signing / transparency** — what pme's `checksum` field grows toward:
  - [The Update Framework (TUF)](https://theupdateframework.io/) — the spec for signed, role-based registry metadata (future Stage-2 hardening).
  - [Go checksum database (sum.golang.org)](https://go.dev/ref/mod#checksum-database) — a transparency log for "verify the checksum in the index is what everyone else saw".
  - [Sigstore](https://docs.sigstore.dev/) — signing publish events (optional future).

## 5. Build systems (the other half of pme)

Read §1 and §2 of `BUILD.md` against these.

- **Make / Ninja** — the classic DAG + "is the output older than the input" model; Ninja's build-log/deps-log is the reference for fast, correct incremental re-execution.
  - [Make manual](https://www.gnu.org/software/make/manual/), [Ninja](https://ninja-build.org/), [Ninja deps log](https://ninja-build.org/manual.html#_deps_log)
- **tup** — content-hash-based change detection (hash the *content*, not mtime) — the model pme's fingerprinting follows.
  - [tup](https://gittup.org/tup/)
- **Bazel / Buck** — action graphs, content-addressable action cache, remote caching; the end-state pme should *not* need, but whose concepts (action = command + inputs + outputs, keyed by hash) inform the fingerprint design.
  - [Bazel build](https://bazel.build/basics), [action cache](https://bazel.build/remote/caching)
- **"Build Systems à la Carte"** (Mokhov, Mitchell, Peyton Jones) — the theory behind build graphs; useful vocabulary for describing pme's build graph.
  - [arXiv:1802.00600](https://arxiv.org/abs/1802.00600)
- **ccache / sccache** — compiler cache keyed on input content + flags; the template for a local/remote pme build cache.
  - [ccache](https://ccache.dev/), [sccache](https://github.com/mozilla/sccache)

## 6. Compiler-driven build tools to mimic

pme is "cargo for Emerald": a thin driver that computes search paths and execs the compiler.

- **cargo build** — target graph → units → artifacts; the closest overall shape. [Cargo build pipeline](https://doc.rust-lang.org/cargo/reference/build-scripts.html) (ignore build scripts — pme has none by design).
- **go build / go tool compile** — single package = one compile unit, import path = identity; closest to Emerald's module model. [go build](https://go.dev/ref/mod#go-command), [compile](https://pkg.go.dev/cmd/compile)
- **zig build / zig cc** — how a single `zig build` front-end can later grow native build-graph features without changing the language. [zig build system](https://ziglang.org/learn/overview/#zig-build)

## 7. Tooling for pme itself (the Go implementation)

pme is a Go module and ships as a single native executable.

- **CLI, HTTP, processes, archives, checksums, and tests:** Go standard library.
- **TOML:** a decoder linked into the executable; it is not a user-side runtime dependency.
- **Semver:** hand-rolled because Emerald semver is deliberately stricter and simpler than PEP 440.
- **Packaging:** `go build ./cmd/pme` for development; release archives distribute the resulting binary.

## 8. Emerald upstream (the thing pme drives)

- Compiler repo: https://github.com/evangelion-research/emerald
- Module system doc (authoritative for §0 of `DESIGN.md`): `emerald/docs/modules.md`
- Diagnostics contract: `emerald/docs/diagnostics.md`
- Phase-2 plan (stdlib split, `import tensor`): `emerald/docs/SPEC_V2.md`
- pme spec companion repo: `evangelion-research/pme` (`DESIGN.md`)
- Tracked commits (re-verified in `DESIGN.md` §0): module system at `1f683be`; HEAD at `1facafe`.

**Standing rule:** the compiler contract is `emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald`. Before
adding any build feature, re-check `emerald/src/main.c` at HEAD to confirm the flag set has not
changed, and update the §0 tracking note in `DESIGN.md` if it has.
