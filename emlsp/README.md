# Emerald LSP

A language server for the Emerald programming language. The working document is
[`DESIGN.md`](DESIGN.md), which lays out what it takes to get from `emeraldc`
(7,000+ lines of C11: `lexer → parser → module link → check → codegen`, plus a
mark-and-sweep runtime) to a working language server.

The headline of the design: **most of the work is in the compiler, not the LSP
server.** The protocol layer is a few hundred lines of glue; the analysis
capabilities the protocol needs — error recovery, source ranges, a type
side-table, a symbol table — do not exist yet and all live in the compiler.

## Status

**v0.1 — a working server, running ahead of the compiler surgery — now a single Go binary.**
`cmd/emerald-lsp` (Go, stdlib JSON-RPC) replaces the original `emlsp/` Python + `pygls` package (retained for reference). It answers the whole
protocol lifecycle today, in two layers with very different reach:

| Feature | How it works | Limit |
|---|---|---|
| Diagnostics | local unused-code analysis plus `emeraldc --check --json` per document version, debounced | Go-style unused imports and local bindings are reported as errors; type checking still needs `emeraldc`, and it reports one syntax error at a time until the parser recovers (§1a) |
| Semantic tokens | this package's own Emerald lexer | purely lexical, and deliberately so — it survives a broken import graph |
| Document symbols, folding | a token-level outline | no types |
| Hover | declaration text, or the builtin/keyword table | shows the *declaration*, not an inferred type (§4b) |
| Goto-definition | the outline, plus `import` resolution | cross-file for imported names; scope-approximate for locals (§4c) |
| Completion | keywords, builtins, in-scope names; module paths after `import`; a module's exports after `from m import` and after `m.` | field completion on a **value** stays silent — it needs the checker's type for the receiver |
| References, highlights | name matches | this file only |
| Workspace symbols | top-level names across `*.rald` | top-level only |

What is *not* here, and why: inlay hints, rename, and code actions all need the
type side table and symbol table of `DESIGN.md` §4, and formatting needs a
pretty-printer that does not exist. The compiler-surgery list in §1 — error
recovery, end positions, allocation, structured output — still blocks the good
version of every row above. This server is built so that landing it is a
rewrite of the analysis layer only: `emerald_lsp/compiler.py` grows a
`--lsp-index` call, and the handlers stop reading `emerald_lsp/outline.py`.

## Install

The server is a single static Go binary — no Python, no `uvx`, no venv.

**From a release tarball** (recommended for users):

```
# download emerald-lsp_Darwin_arm64.tar.gz from Releases, then:
tar -xzf emerald-lsp_*.tar.gz
sudo mv emerald-lsp /usr/local/bin/
emerald-lsp --version
```

**With Go** (from source, requires Go 1.21+):

```
go install github.com/evangelion-research/emlsp/cmd/emerald-lsp@latest
emerald-lsp --version
```

From a checkout (requires [Task](https://taskfile.dev) + Go 1.21+):

```
task build
./emerald-lsp --help
task cross                  # or: GOOS=linux GOARCH=amd64 go build -o emerald-lsp-linux ./cmd/emerald-lsp
```

> The previous Python distribution (`uv tool install emerald-lsp` / `pipx`) is retained for reference in `emlsp/` but is no longer the release artifact. `emlsp/` will be removed once the Go port reaches parity (it already does).

The server needs `emeraldc` on `PATH` for compiler diagnostics — set
`emerald.compilerPath` or `$EMERALDC` if it lives elsewhere. Without it,
syntax features and unused-code diagnostics still work; the server says so
once and carries on.

To see the exact query the server makes per keystroke:

```
emerald-lsp --check path/to/file.rald
```

## Editor setup

**Neovim** (0.11+):

```lua
vim.filetype.add({ extension = { rald = "emerald" } })
vim.lsp.config.emerald = {
  cmd = { "emerald-lsp" },
  filetypes = { "emerald" },
  root_markers = { "emerald.toml", "src", ".git" },
  settings = { emerald = { includePaths = {} } },
}
vim.lsp.enable("emerald")
```

**Helix** (`languages.toml`):

```toml
[language-server.emerald-lsp]
command = "emerald-lsp"

[[language]]
name = "emerald"
file-types = ["rald"]
comment-token = "#"
language-servers = ["emerald-lsp"]
indent = { tab-width = 4, unit = "    " }
```

**Zed / VS Code**: point the client at the `emerald-lsp` binary over stdio.
`--tcp --port N` is available for debugging a client that cannot spawn a
process.

### Configuration

All keys live under `emerald` in `initializationOptions` or
`workspace/didChangeConfiguration`:

| key | default | meaning |
|---|---|---|
| `compilerPath` | `emeraldc` on `PATH` | the checker to run |
| `includePaths` | `[]` | extra `-I` roots, before the lockfile's |
| `diagnostics.enabled` | `true` | run the checker at all |
| `proof` | `false` | pass `--proof` (promotes warnings to errors) |
| `debounceMs` | `250` | idle time before a check |
| `timeoutSeconds` | `10` | give up on a wedged compiler |

## Development

```
task --list                 # see all tasks
task build                  # -> ./emerald-lsp (3.6M static binary)
task test                   # go vet + go test ./...
task cross                  # linux/darwin/windows binaries
task install                # install to $GOPATH/bin
task clean

# equivalents without Task (if you prefer plain go):
go test ./... && go vet ./...
go build -o emerald-lsp ./cmd/emerald-lsp && ./emerald-lsp --version

# legacy Python suite still runs until emlsp/ is retired:
task pytest                 # or: uv run pytest -q
```

The suite covers the lexer, the outline, position encoding, the diagnostic
mapping, the lockfile `-I` rule, and the features — plus two kinds of test the
design calls out: a truncation fuzz over every prefix of several samples, and
end-to-end sessions that drive the real server process over stdio with a stub
compiler.

Layout (Go, single binary):

```
cmd/emerald-lsp/main.go       CLI (--version, --check, --tcp, stdio)
internal/lexer                Emerald tokenizer (mirrors src/lexer.c)
internal/outline              token-level symbols, scopes, imports
internal/modules              import resolution (mirrors docs/modules.md)
internal/compiler             emeraldc subprocess seam, and pme's -I rule
internal/diagnostics          compiler JSON -> LSP diagnostics + unused check
internal/semantic             semantic tokens
internal/features             hover, definition, completion, symbols
internal/positions            byte<->char columns, path<->URI
internal/server               JSON-RPC stdio/TCP, debounce, diagnostics fan-out
internal/language             builtins / keywords / type atoms
```

Python layout (retained, deprecated) is under `emlsp/` — same module names, kept for diffing.

## pme — the package manager

`pme` — the package manager for Emerald, in design as a Python driver over the
same `-I` seam — is designed alongside this server. The two share one rule:

> **pme resolves, the LSP consumes.** The server reads `emerald.lock` and
> applies pme's frozen `-I` rule (each locked package's `src/` under
> `~/.emerald/store/`, dependencies before dependents). It never reimplements
> resolution — no version solving, no registry reads, no fetches.

`emerald.toml` and `emerald.lock` are analysis inputs: their changes are
watched and invalidate the analysis cache. pme's full implementation plan
(milestones 1–6, from manifest/lockfile/semver through a Stage-1 registry)
lives in the appendix of `DESIGN.md`.

## Related

- **Emerald** — the compiler (`emeraldc`), its module system, and runtime.
- **pme spec** — `evangelion-research/pme`'s `DESIGN.md`; authoritative for
  pme, expanded into an implementation plan in this repo's `DESIGN.md`
  appendix.
- **Design document** — [`DESIGN.md`](DESIGN.md): architecture, the feature
  ladder, testing strategy, and the suggested implementation order.

## License

MIT — see [`LICENSE`](LICENSE). Copyright (c) 2026 Evangelion Research.
