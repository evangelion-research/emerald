# Emerald LSP — design

Notes on what it takes to get from `emeraldc` (7,041 lines of C11:
`lexer → parser → module link → check → codegen`, plus a mark-and-sweep
runtime) to a working language server.

The headline is unchanged: **most of the work is in the compiler, not the LSP
server.** The protocol layer is a few hundred lines of glue. The analysis
capabilities the protocol needs — error recovery, source ranges, a type
side-table, a symbol table — do not exist yet and all live in `../emerald/src`.

**What changed since the first draft of these notes.** Emerald grew a module
system (`src/module.c`, `include/module.h`, `docs/modules.md`), a C backend,
and a GC'd runtime. The module system is the consequential one: the earlier
version of this document listed "no cross-file resolution" as a large chunk of
work Emerald let us skip. That is no longer true, and it changes the
architecture — see §2. Separately, `pme` — the package manager for Emerald —
has entered design as a Python driver over the same `-I` seam; what that means
for this server is in the final subsection of §2, and the implementation plan
for pme itself is in the appendix. The compiler-surgery list in §1 is unchanged
and still blocks everything.

---

## 0. Language choice: Python for the server

**Decision: Python + `pygls` for the protocol layer, C stays for analysis.**

The split matters. There are two jobs here and they have opposite
requirements:

| Job | Wants | Language |
|---|---|---|
| Analysis (parse, link, typecheck, resolve names) | speed, direct AST access, the existing 7k lines of compiler | **C** — already written |
| Protocol (JSON-RPC, document sync, position math, debounce) | JSON, string handling, async, fast iteration | **Python** |

Writing the protocol layer in C means hand-rolling a JSON parser, an event
loop, UTF-16 offset arithmetic, and a document store — several hundred lines
of fiddly code that has nothing to do with Emerald, plus it forces the arena
allocator problem (§1c) on day one. That is the overkill path and it buys
nothing: the protocol layer is not the hot path. A keystroke budget is ~100ms;
JSON-RPC decode is sub-millisecond in any language.

### Why `pygls` specifically

`pygls` (with `lsprotocol` for the typed message definitions) gives you for
free the parts that are tedious and easy to get subtly wrong:

- `Content-Length` framing over stdio
- the `initialize` / `initialized` / `shutdown` / `exit` lifecycle and
  capability negotiation
- an in-memory document store fed by `didOpen` / `didChange` / `didClose`,
  including incremental sync if you enable it
- typed request/response models, so a malformed `Hover` is a type error
  rather than a client that silently ignores you
- request cancellation plumbing
- workspace folders and file watching (`workspace/didChangeWatchedFiles`),
  which the module system now makes necessary

What's left for you is a set of decorated handler functions:

```python
@server.feature(TEXT_DOCUMENT_HOVER)
def hover(ls, params: HoverParams) -> Hover | None:
    doc = ls.workspace.get_text_document(params.text_document.uri)
    result = analyze(doc.source, hover_at=params.position)
    ...
```

### Costs of Python, honestly

1. **Distribution.** A Go or Rust server ships as one static binary; a
   Python server needs an interpreter and dependencies. Mitigations, in
   order of preference:
   - publish to PyPI, tell users `uv tool install emerald-lsp` (or `pipx`);
     one command, isolated env, works everywhere
   - for the VS Code extension, bundle a venv or use PyInstaller so users
     install nothing
   - this is a real cost but a one-time packaging problem, not an ongoing
     tax on development
2. **Startup latency.** ~50–150ms for the interpreter plus imports. Paid
   once when the editor launches the server, then never again. Irrelevant.
3. **Per-request latency.** Only matters if you do analysis *in* Python,
   which you are not. See §2 — Python shells out or FFIs into C.
4. **GIL / concurrency.** Not a factor at this scale. `pygls` runs an
   asyncio loop; analysis happens in a subprocess, so it doesn't block.

### The alternative, if you change your mind later

Go (`sourcegraph/jsonrpc2`) is the other sane choice, purely for the
single-binary distribution story. The architecture below is unchanged
either way — the C side exposes JSON, and whoever consumes it is
interchangeable. Don't pick Go now for a distribution problem you don't
have yet. Ship in Python; port the ~400 lines of glue later if packaging
ever becomes the thing users complain about.

---

## 1. Make the compiler survivable as a library

`emeraldc` is a batch tool that assumes "one file, one process, then die."
Four assumptions break under an editor. All four are still open.

### 1a. The parser exits on the first syntax error

`src/parser.c:34` — `perror_at()` renders the diagnostic and calls `exit(1)`
(line 43). An editor buffer is syntactically broken most of the time you are
typing, so this is fatal, not cosmetic. There are ten `perror_at()` call sites
and they all terminate the process. Needed:

- **Error recovery.** On an unexpected token, record the diag and *panic-sync*
  to the next statement boundary — `}`, a newline at statement start, or one
  of `def`, `type`, `import`, `from`, `if`, `while`, `for`, `return` — then
  keep parsing.
- **An `S_ERROR` statement kind** (and `E_ERROR` expression kind) so the AST
  stays well-shaped even where the text is garbage. Completion inside a
  half-typed expression depends on there being *a* node at the cursor.
- `parse_program()` returns a partial `Program*` plus diags, and never exits.
  Its header comment (`include/parser.h`) currently documents the exit; update
  it to document the recovery contract instead.

Also `advance()` at `src/parser.c:46` bails on `TK_ERROR` from the lexer. The
lexer produces `TK_ERROR` for unterminated strings (`src/lexer.c:97`) and
unknown characters (`:128`, `:136`) — all three are constant while typing — so
the lexer must emit a token and continue rather than ending the stream.

### 1b. No end positions

`Expr`, `Stmt`, and `TypeExpr` all carry `line`/`col` only (`include/ast.h`).
Every LSP feature wants a **range**, not a point. Add `end_line`/`end_col` to
all three node structs, populated from the token span — `Token` already has
`len` (`include/lexer.h:26`), so the lexer side costs nothing.

This is a wide, mechanical diff. **Do it before writing any server code**,
because everything downstream assumes ranges exist.

### 1c. Everything leaks by design

`include/ast.h`: "Nodes are malloc'd and never freed." Fine for a process,
fatal for a server open for eight hours across 500 keystrokes. The module
loader makes this worse, not better — a reparse now re-reads and re-allocates
the whole import graph. Two options:

- **Arena per linked program** — one bump allocator threaded through parser,
  module loader, and checker; `arena_free()` on relink. Cleanest, and speeds
  up parsing.
- **Process per request** — don't fix it at all; let the OS reclaim. Given
  the file sizes here this is likely <10ms per query and sidesteps the whole
  problem. See §3.

The process-per-request option pairs naturally with the Python decision and
is the recommended starting point.

### 1d. Output goes to stderr, and OOM calls `exit`

`exit(1)` on allocation failure in ~12 places (`check.c:74`, `diag.c:16`,
`module.c:35`, …) is acceptable. But the parser, loader, and checker also
assume stderr is the reporting channel; analysis modes must write structured
output to stdout and keep stderr for genuine crashes. `main.c` already routes
JSON to stdout when `--json` is set (`main.c:139`, `:145`, `:151`) — extend
that discipline rather than inventing a second convention.

---

## 2. The module system changes the shape of the server

`src/module.c` (810 lines) resolves `import` statements to files, loads the
transitive graph, mangles each imported module's top-level names to
`<module>__<name>`, and links everything into a **single `Program`** —
dependencies first, entry module last. Read `docs/modules.md` before touching
any of this.

Four consequences, in descending order of how much they will hurt.

### 2a. The loader reads from disk; an editor has unsaved buffers

`module.c:211` `read_file()` opens the resolved path with `fopen`. In an
editor, the file on disk is stale for every modified buffer in the workspace —
including the entry file's dependencies, which the user may be editing in
another tab.

**Required: an overlay hook in the loader.** Give `module_link()` a way to
consult caller-provided source text before falling back to the filesystem:

```c
typedef const char *(*ModuleOverlay)(const char *canonical_path, void *ud);

Program *module_link(const char *entry, const char *const *roots, size_t nroots,
                     ModuleOverlay overlay, void *overlay_ud,
                     DiagList *diags, int *errors);
```

`read_file()` calls the overlay first, keyed on the **canonical** path
(`module.c:164` already computes one — module identity is the canonical path,
so the overlay must key on the same thing or a file reached two ways will get
two different texts). On the CLI the overlay is NULL and nothing changes.

On the Python side, the overlay is fed from `pygls`'s document store: every
open, unsaved buffer in the workspace, passed down as a `{path: text}` map with
the analysis request. This is the single most important new piece of work the
module system creates.

### 2b. Analysis is now per-program, not per-file

A change to `strings.rald` changes the diagnostics of every module that imports
it. That means:

- **A reverse-dependency map.** `module_link` already walks the graph; have it
  report the modules it loaded (their canonical paths) so Python can build
  `{dependency → [dependents]}` and know what to re-analyze on a change.
- **Entry points.** LSP hands you a file, not a program. A leaf module compiled
  alone still type-checks (it is its own entry), so the simple, correct policy
  is: **analyze each open file as its own entry point.** Editing a dependency
  then re-runs analysis for every open dependent, which is exactly the reverse
  map's job. Do not attempt to guess a single "main" for the workspace.
- **Diagnostics fan out.** One analysis run yields diagnostics for several
  files. `Diag` carries `file` (`include/diag.h:40`) and `DiagList` keeps
  per-file sources (`diag.h:60`), so the data is already there — but the Python
  layer must group by file and publish a `textDocument/publishDiagnostics` per
  URI, including **clearing** files that went clean. That last part is the
  classic bug: an empty diagnostic list must still be published.
- **Watch the workspace.** Register `workspace/didChangeWatchedFiles` for
  `**/*.rald`, plus `emerald.toml` and `emerald.lock` — the manifest and
  lockfile changing on disk changes the set of `-I` roots the analysis runs
  with (final subsection of this section). A dependency edited outside the
  editor, or a newly created module that resolves a previously failing import,
  has to invalidate the cache.

### 2c. Names in the linked program are mangled

After linking, `strings.split` is `strings__split`. The AST carries the source
spelling alongside it in two places:

- `Expr.disp` (`ast.h:78`) — NULL means "already spelled the way the user wrote
  it"
- `Stmt.as.func.dispname` and `Stmt.as.tdef.dispname` (`ast.h:137`, `:148`)

**Everything the server shows the user must go through `disp`/`dispname`.** A
hover that reads `strings__split` is a bug; so is a completion item, a rename,
or a document symbol. Rule of thumb: the mangled name is the *identity* used
for matching, the display name is what reaches the wire.

Two gaps to close while adding ranges (§1b), because they have no display name
at all today:

- `module.c:536` mangles `s->as.fr.var` (the `for` loop variable) in place
- `module.c:493` mangles assignment targets in place

Both lose the original spelling. Add a `disp`-style field, or better, keep the
original in the node and mangle into a separate `linked_name` field so nothing
is ever destructive.

Also note `Stmt.file` (`ast.h:115`) — every statement records which file it was
parsed from, and the checker swaps `ck->filename` as it walks
(`check.c:1790-1793`, `:1899-1900`). `Expr` and `TypeExpr` have **no** file
field, so a position index (§4a) must inherit the file from the enclosing
statement. Build that into the index walk from the start.

### 2d. Imports are new LSP features, and new diagnostics

The module system is not only cost — it unlocks features that are now the most
valuable things a user gets from a language server:

- **Goto-definition across files.** Previously impossible; now the common case.
- **Workspace symbols** (`workspace/symbol`) across every module.
- **Import completion.** Two distinct completions, both high value:
  after `import ` / `from `, complete **module paths** by scanning the
  resolution roots (`docs/modules.md`: importing file's directory, the nearest
  `src/`, then each `-I`); after `from m import `, complete **`m`'s exported
  names** — which is exactly "top-level defs, type aliases, and globals not
  starting with `_`" (`module.c:230` `mod_exports`, `:235` `is_private`).
- **Import diagnostics** already exist and should map straight through:
  `E_IMPORT_CYCLE`, `E_IMPORT_PRIVATE`, `E_IMPORT_NAME`, `E_IMPORT_REDEFINE`,
  `E_IMPORT_AMBIGUOUS`. `E_IMPORT_NAME` and `E_IMPORT_PRIVATE` are natural
  code-action targets ("did you mean …", "make `_helper` public").

One thing to protect: `module_link` returns NULL on failure (`module.h:20`), so
a broken import currently kills *all* analysis for the program, including the
type errors in the file the user is actually looking at. Under an editor the
loader should degrade — report the import diagnostic, substitute an empty
module for the unresolvable one, and keep linking — the same recovery
philosophy as §1a, one level up.

### Package management: pme resolves, the LSP consumes

`pme` — the package manager for Emerald — is in design as a Python driver
around `emeraldc` (implementation plan in the appendix). The compiler seam is
unchanged: `emeraldc` still takes ordered `-I` roots, and the server inherits
nothing new from the compiler. What changed is that a project with dependencies
only resolves imports when each dependency's `src/` is on the path — and that
information lives in pme's outputs, not in the workspace.

**Never reimplement resolution in the LSP.** No version solving, no registry
reads, no fetches. pme owns resolution; the LSP consumes its results — one-way,
exactly like the `-I` seam itself. Concretely: read `emerald.lock` (generated,
committed, plain TOML — trivial to parse) and apply pme's frozen `-I` rule.
Each locked package contributes one `-I`: its `src/` directory under
`~/.emerald/store/`, dependencies before dependents, ties broken by name. The
project's own roots (the importing file's directory, the nearest `src/`) stay
the compiler's business via `find_src_root` — the LSP only supplies package
roots. Store packages are immutable and never open in the editor, so the §2a
overlay only ever covers workspace buffers; `emerald.lock`'s content hashes are
what make that trust safe.

Two consequences for the server:

- **The manifest and lockfile are analysis inputs.** A `pme add` or lockfile
  update changes the `-I` set, so `emerald.toml` and `emerald.lock` join the
  file-watch list (§2b) and their change invalidates the analysis cache.
- **The rule is pme's; copy it, don't re-derive it.** pme's milestone 3
  (`pme build` with path dependencies, no network) is where the rule is
  exercised end to end. The LSP side is a couple of hundred lines,
  golden-testable against a fake store before pme ships (§8). Keep the rule
  documented in one place in pme so the LSP's copy is verifiable against it.

---

## 3. Architecture: C analyzes, Python speaks

The checker is the valuable, irreplaceable part — structural typing, unions,
flow narrowing, generics, `never`-based exhaustiveness (see `docs/proofs.md`;
`--check` is literally a proof checker). None of that gets reimplemented in
Python. So:

**Extend `emeraldc` into a query backend that emits JSON; Python consumes it.**

New modes alongside the existing `--emit-tokens` / `--emit-ast` / `--check` /
`--emit-c` flags, which this fits cleanly:

```
emeraldc --lsp-diagnostics f.rald      # ~ --check --json, plus end positions
emeraldc --lsp-symbols     f.rald      # document symbols from the AST
emeraldc --lsp-hover       f.rald:12:7 # type + declaration at a position
emeraldc --lsp-index       f.rald      # everything: defs, uses, types, ranges
```

Two input mechanisms are needed, and the second is the module-system tax:

- `--stdin` / `-` for the entry file: the editor buffer, not the file on disk,
  is the source of truth.
- **An overlay manifest** for everything else. Since analysis is per-program
  (§2a), passing one buffer on stdin is not enough. Simplest workable form: a
  `--overlay <file>` flag pointing at a JSON `{ "abs/path.rald": "source…" }`
  map that Python writes to a temp file per request. Slightly less pretty than
  a protocol, but it keeps every query a pasteable shell command, which is
  worth a lot during debugging.

Note the stage split in `main.c:125` — `--emit-tokens` and `--emit-ast` are
per-file views that never follow an import, everything from `--check` onward
runs on the *linked* program. The `--lsp-*` modes belong on the linked side,
with one exception: semantic tokens are purely lexical and should stay per-file
so they keep working when the import graph is broken.

### Transport: subprocess first

Start with `subprocess.run(["emeraldc", "--lsp-index", "-", "--overlay", tmp],
input=src)`.

- fresh process per query means the leak in §1c never matters
- crashes are contained — a checker segfault on weird input returns a
  non-zero exit code instead of taking the server down
- trivially debuggable: every query is a shell command you can paste
- golden-testable exactly like the existing stage tests

Prefer **one fat `--lsp-index` call per document version**, cached in Python,
over one subprocess per hover. Compute the whole index on change (debounced),
then answer hover/definition/completion from the cached structure in memory.
That is one process spawn per ~300ms of typing, not one per cursor move.

The index is now naturally multi-file: one run covers the entry module and
everything it imports, so cache it keyed by `(entry, versions-of-all-loaded-
modules)` and serve queries for any file it covered.

If profiling ever shows the spawn cost mattering, the escape hatch is to
build `libemerald.a` and call it via `ctypes`/`cffi` — the JSON contract
stays identical, so it's a swap of one function, not a rewrite. Do not do
this preemptively.

---

## 4. What the compiler must expose

The real work, mostly new code in `check.c`.

### 4a. A position index

Given `(file, line, col)`, find the innermost AST node containing it. A sorted
flat array of `{file, start, end, node*}` built by a post-parse walk; binary
search within the file's slice, innermost wins. Requires §1b, and must thread
the file down from `Stmt.file` (§2c).

### 4b. An expression → type side table

`infer()` (`check.c:1098`) computes a `Type*` for every expression and throws
it away. Record it: `Expr* → Type*`. That one change gives hover, and is the
input to field completion. `type_write()` (`check.c:393`) already renders types
to strings.

### 4c. A symbol table with locations

The checker has `Var` (`check.c:466`), `Alias` (`:50`), and a function table,
but tracks no definition sites and no use sites. Add
`def_file`/`def_line`/`def_col`/`def_end` to each, and append to a
`references[]` list on every name resolution.

With modules, each entry also needs its **owning module** and its display name,
so that goto-definition on `strings__split` lands in `strings.rald` on the
token spelled `split`.

That single table powers goto-definition, find-references, rename, document
symbols, and workspace symbols.

### 4d. Richer diagnostic JSON

The JSON emitted by `diag.c:174` `render_json()` now has `kind`, `severity`,
`code`, `file`, `line`, `column`, `message`, `expected`, `actual`,
`source_line`, and `notes[{label, value}]`. That is most of the way there —
`severity` and `file` and structured notes all landed with the module work.

What's still missing:

- `end_line` / `end_col` — LSP wants a range to underline. Blocked on §1b.
- `related: [{file, line, col, message}]` — the LSP
  `DiagnosticRelatedInformation` shape, for "defined here" / "other module on
  the cycle" notes. `notes[]` is close but has no location, so it renders as
  text only. Import-cycle and redefinition diagnostics are the obvious first
  users.

---

## 5. The Python layer

With `pygls` doing framing, lifecycle, and the document store, what's left:

- **Analysis cache**: `{entry_uri: (versions, index)}` where `versions` covers
  every module the run loaded, plus the reverse-dependency map from §2b.
  Recompute on `didChange` to any module in the set; serve everything else from
  cache.
- **Debounce**: ~150–300ms idle before spawning the checker; cancel/discard
  in-flight work when a newer document version lands. `asyncio` task
  cancellation, roughly 20 lines. With multi-file analysis, also coalesce:
  one edit to a widely imported module should trigger one batch, not one run
  per dependent in flight.
- **Overlay assembly**: collect every dirty buffer from `ls.workspace` into the
  overlay map on each request (§3). Cheap, and skipping it is the source of the
  most confusing possible bug class — analysis that reflects the disk, not the
  screen.
- **Position encoding** — *the trap*. LSP positions are **0-based lines** and
  **UTF-16 code units**. `emeraldc` uses **1-based lines** and **byte
  columns** (`Token.col`, `Diag.col`, every AST node). Any non-ASCII character
  in a string literal silently shifts every subsequent range on that line.
  Build one conversion module, test it with emoji and accented characters, use
  it at every boundary and nowhere else. LSP 3.17 allows negotiating
  `positionEncoding: "utf-8"` — do that when the client supports it, but keep
  the converter for those that don't.
- **Path ↔ URI**: the compiler speaks canonical filesystem paths
  (`module.c:164`), LSP speaks `file://` URIs. One conversion module, same
  discipline as position encoding. Canonicalize on the Python side too, or
  cache keys will miss on symlinked workspaces.
- **Mapping**: compiler JSON → `lsprotocol` types. Mechanical.

---

## 6. Feature ladder

Ordered by value per unit of work.

1. **Diagnostics** — ~90% done in the compiler. Needs §1a error recovery, §1b
   end positions, and the §2a overlay to be correct for multi-file workspaces.
   Ship alone as v0.1; it is most of the perceived value of any language
   server.
2. **Semantic tokens** — nearly free. The lexer already emits
   `{kind, line, col, len}`; map `TokKind` → LSP token types. Better
   highlighting than any TextMate grammar, it needs no type information, and
   being per-file it survives a broken import graph.
3. **Document symbols + folding ranges** — a walk over `S_FUNC`, `S_TYPEDEF`,
   top-level `S_ASSIGN`, `S_IMPORT`. Gives the outline view and breadcrumbs.
4. **Hover** — position index (§4a) + type side table (§4b). Rendered type
   plus the declaration site, with `disp`/`dispname` on the way out (§2c).
5. **Goto definition** — the symbol table (§4c). Cross-file now, which is
   both more work and much more useful than the single-file version would
   have been.
6. **Completion** — keywords and in-scope names first. Then the two good ones:
   after `.`, resolve the object expression's type and list its record fields
   (structural typing makes this genuinely useful rather than decorative); and
   import completion (§2d), which is cheap because `mod_exports` already
   encodes the rule.
7. **Workspace symbols** — the same table as (5), indexed across every module
   under the workspace roots.
8. **Find references / rename** — the use list; rename is references plus a
   multi-file `WorkspaceEdit`. Renaming an exported name touches every
   importing module, so this is where the reverse-dependency map earns its
   keep — and where mangling will bite if §2c was done sloppily.
9. **Inlay hints** — inferred types on unannotated bindings. Cheap once the
   type table exists, and the best available showcase of the checker.
10. **Code actions** — start with the import diagnostics: `E_IMPORT_NAME` →
    "did you mean", `E_IMPORT_PRIVATE` → "remove the leading underscore",
    unresolved name → "add `from m import x`". Each is a small, well-scoped
    `WorkspaceEdit` and they read as magic.
11. **Formatting** — needs a real pretty-printer over the AST
    (`ast_print_program` emits s-expressions, not source) *and* comment
    retention, which the AST currently discards entirely. A separate project.
    Do it last.

---

## 7. Editor integration

- **VS Code**: `package.json` contributing the `.rald` language,
  `language-configuration.json` (line comment `#`, brackets, auto-closing
  pairs), and ~30 lines of client code launching the server. Bundle the
  Python env so users install nothing. Register a file watcher for `**/*.rald`
  (§2b).
- **Neovim / Helix / Zed**: no extension needed, just config pointing at the
  server binary. Ship copy-pasteable snippets in the README. Set the root
  detection to the nearest `src/` directory, matching the module resolver's own
  rule (`module.c:126` `find_src_root`).
- A minimal TextMate grammar is still worth having as the fallback before
  the server attaches, and for GitHub syntax highlighting.

---

## 8. Testing

Extend the existing golden-test culture rather than inventing something new.
`tests/run_tests.sh` already runs five suites — `lexer`, `parser`, `check`,
`e2e`, `imports` — and `task bless` regenerates every `.expected`. The
`imports` suite is the model to copy: a directory per case, `main.rald` plus
siblings, an optional `flags` file, `bad_*` prefixes for expected failures.

- **Analysis goldens**: `--lsp-index` output diffed against checked-in JSON,
  in `tests/lsp/` beside the five existing suites, using the same
  directory-per-case shape so multi-module scenarios are expressible. This is
  where most of the real coverage lives, and it needs no LSP client at all.
- **Overlay tests**: a case where the on-disk text and the overlay text
  disagree, asserting the analysis reflects the overlay. This is the one bug
  class that no other test catches and that users will hit constantly.
- **Lockfile-driven roots**: a fixture workspace with a fake
  `~/.emerald/store` and an `emerald.lock`, asserting that imports of locked
  packages resolve as if their `-I` roots had been passed, and that editing
  the lockfile invalidates the cache. This is the test that keeps the LSP
  honest about whose resolution it is consuming — pme's, not its own.
- **Session tests**: pipe a scripted JSON-RPC conversation into the server
  and golden-diff the responses. `pygls` ships test helpers for driving a
  server in-process, which is easier than framing messages by hand. Include a
  scenario that edits a dependency and asserts the dependent's diagnostics
  update.
- **Truncation fuzz**: take each `examples/*.rald` (including the multi-module
  `examples/modules/` and `examples/ray_tracer/`), cut at every byte offset,
  assert the parser terminates without crashing and produces *some* AST. This
  one test catches the majority of error-recovery bugs, and it is ~15 lines of
  Python.
- **Position-encoding tests**: a file with emoji and combining characters,
  asserting that ranges land on the right characters in every editor.

---

## Suggested order

```
end positions in AST  →  parser error recovery  →  loader overlay hook
      →  lockfile-driven -I roots (final subsection of §2)
      →  --lsp-index JSON (multi-file)  →  pygls skeleton + sync + diagnostics
      →  semantic tokens  →  type side table  →  hover  →  symbol table
      →  goto-def (cross-file)  →  completion (incl. imports)
      →  workspace symbols  →  references / rename  →  inlay hints
      →  code actions
```

The first three steps are unglamorous compiler surgery with nothing to demo;
the fourth — lockfile-driven `-I` roots — is invisible until a package exists
to resolve. It is tempting to skip ahead to the JSON-RPC loop where progress is
visible. Don't. Every feature past diagnostics is built on source ranges, on
parsing broken text, and — now that modules and packages exist — on analyzing
the buffers the user actually has open rather than the files on disk.
Retrofitting any of the three surgical steps later means touching every handler
a second time.

---

## Appendix: pme in Python — implementation plan

The working plan for `pme` (spec: `evangelion-research/pme`'s `DESIGN.md`),
expanded from that spec's §10–§11 into concrete, ordered steps. The pme spec
is authoritative — this is the *doing* order. It lives here because the LSP
and pme share the `-I` seam (final subsection of §2) and the LSP integration
is designed alongside pme milestone 3. Nothing below is code: each step names
its module, its invariants, and the gate that says it's done.

Per the pme spec: **Python 3.11+**, I/O-bound not CPU-bound, distributed on
PyPI as `emerald-pme` (`pipx install pme` / `uv tool install pme`). Milestone 0
(the module system, `emerald@1f683be`) is done; **milestone 1 — manifest +
lockfile + semver — is the current front of work**.

### The skeleton (pme spec §10.1)

| module | responsibility |
|---|---|
| `pme/cli.py` | argparse dispatch, exit codes, `--json` / `-q` plumbing |
| `pme/manifest.py` | `emerald.toml` read/validate (tomllib) and comment-preserving write (tomlkit) |
| `pme/lockfile.py` | `emerald.lock` read/write, deterministic ordering |
| `pme/semver.py` | parse/compare/constrain, hand-rolled (~150 lines) |
| `pme/resolve.py` | MVS |
| `pme/registry.py` | index fetch, tarball download, publish (httpx) |
| `pme/store.py` | content-addressed cache, atomic extract, verify |
| `pme/build.py` | `-I` computation, `emeraldc` invocation, diagnostic passthrough |
| `pme/diagnostics.py` | the §8 error shapes, human + JSON renderers |
| `pme/errors.py` | `PmeError` hierarchy → exit codes |

Dependencies: `tomllib` (stdlib) to read, `tomlkit` to write, `httpx` for
HTTP, `pytest` + `pytest-httpx` for tests. **No `packaging`** — Emerald semver
is stricter than PEP 440.

Cross-cutting rules, in force from step 1:

- **Atomicity.** Every filesystem mutation writes to a temp path in the same
  directory and `os.replace`s into place; a `^C` mid-extract leaves nothing
  behind.
- **Locking.** `~/.emerald/.lock`, flock, around every store write — two
  concurrent `pme build`s must not corrupt each other.
- **Verify before use, always.** Hash on extract and on demand; never trust a
  store path just because it exists.
- **Offline builds.** `pme build` makes no network calls when the lock is
  satisfied by the store.
- **No `shell=True`.** `emeraldc` is invoked with an argv list.
- **Error shapes from day one.** pme's kinds (`manifest`, `resolve`,
  `registry`, `io`) are disjoint from the compiler's (`syntax`, `type`,
  `internal`), so a merged `--json` stream is attributable to one producer per
  object.

### Phase 1 — foundations (milestone 1: manifest + lockfile + semver)

**Step 1 — repo scaffold.** `pyproject.toml` (hatchling;
`requires-python = ">=3.11"`; console script `pme = pme.cli:main`),
`src/pme/` package, `tests/`. Build the CLI skeleton first: argparse
subcommands, `--json` / `-q` accepted by every command from day one, and the
exit-code contract (0 ok, 1 user/build error, 2 bad usage, 3
network/registry). `main()` catches the `PmeError` base class and renders
through `diagnostics.py` (stub renderers for now).
*Gate:* `pme --help` and `pme badcmd` behave; bad usage exits 2; `--json`
accepted everywhere.

**Step 2 — `semver.py`.** Parse `MAJOR.MINOR.PATCH` with optional
`-prerelease`; compare; evaluate a minimum-version constraint — MVS only ever
needs "satisfies this minimum". Prerelease rule (pme spec open question 4):
never selected automatically, only by exact pin. Property tests: parse
round-trip, comparison transitivity, constraint boundaries (equal,
one-patch-up, major bump, prerelease ordering).
*Gate:* pytest green; module stays ~150 lines.

**Step 3 — `manifest.py` (read/validate).** tomllib parse plus the spec's
validation rules: name `[a-z][a-z0-9_]*` 2–64 chars; version strict semver;
exactly one of `[lib]` or `[[bin]]` (both allowed); license and authors
present; `emerald` compiler constraint parsed; `path` deps detected and
flagged — legal locally, fatal at publish. Violations become `E_*` manifest
diagnostics in the §8 shape.
*Gate:* golden tests for good and bad manifests; every message names the
offending field.

**Step 4 — `manifest.py` (write) + `lockfile.py`.** The tomlkit write path —
what `pme add` uses, and it must preserve the user's comments and formatting,
so never dump-and-replace. Lockfile read/write: TOML `version = 1`,
`[[package]]` entries sorted deterministically by name (readable diffs), each
with `version`, `checksum`, `deps`. Reject unknown fields — a lockfile is
generated and never hand-edited.
*Gate:* manifest round-trip preserves comments; lockfile goldens; an
unknown-field lockfile fails loudly.

**Step 5 — `pme init`.** Scaffold `emerald.toml`, `src/main.rald`,
`.gitignore`. Reuse the step-3 validator on the scaffold to prove it
round-trips.
*Gate:* `pme init` in a temp dir yields a manifest that validates and a
`src/main.rald` the compiler accepts.

### Phase 2 — resolution (milestone 2: MVS)

**Step 6 — `resolve.py`.** A **pure function**: root manifest + an index → a
resolution or a set of resolve errors. Define the index as a narrow interface
first (fetch the manifest for `(name, version)`), implemented with an
in-memory fake before anything touches the network. MVS fixed point: seed with
the root's direct deps at their minimums; repeatedly, for each selected
package, raise each dependency to the max of what's selected and what it
requires; stop when nothing moves. **One version per package, no multi-major
support, ever** — mangling is keyed on the dotted module path only, so two
majors of one package would collide in one translation unit; diagnose the
conflict instead.
The four failure modes, all with good messages: `E_RESOLVE_NOT_FOUND`,
`E_RESOLVE_NO_VERSION`, `E_RESOLVE_MAJOR_CONFLICT` (must print **both**
dependency paths — this is what `pme why` renders), and `E_RESOLVE_COMPILER`
(guarded: only fires once `emeraldc --version` exists upstream — pme spec
open question 2; keep the error type and the check behind a flag until then).
*Gate:* resolver tests against the fake index — diamond graph, major conflict
with both paths printed, missing package, no satisfying version, prerelease
pin-by-exact-version only.

### Phase 3 — store + build (milestone 3: offline, path deps only)

**Step 7 — `store.py`.** Layout `~/.emerald/store/<name>-<ver>-<hash>/`,
extracted once and immutable — content-addressing means two projects share one
copy and corruption is detectable. Atomic extract (temp dir in the same
filesystem, then `os.replace`), flock around mutations, sha256 on extract.
Never vendor into the project directory.
*Gate:* a kill-mid-extract test leaves no partial entry; two concurrent
extracts of one package both succeed and share one entry.

**Step 8 — `build.py`.** The heart of pme and the seam the whole design
freezes: compute the ordered `-I` roots — each locked package's `src/`
directory (not its store root), dependencies before dependents, ties broken by
name — then exec `emeraldc -I <root> … -o target/<bin> <entry>`. argv list,
never a shell string. Pass `--json` through when asked and render pme's own
errors into the same stream shape. **No build scripts, ever**: installing a
package must never execute its code — that property is load-bearing.
*Gate:* a multi-package project with `path` deps builds fully offline; a
broken dependency produces one merged JSON error stream.

**Step 9 — `pme run` / `pme tree` / `pme why` (local-only).** Run = build
then exec the binary. Tree renders the resolved graph with selected versions;
why prints every root→package path, reusing the conflict-path machinery from
step 6.
*Gate:* correct output on the path-dep sample project — milestone 3 is the
first genuinely useful build and needs no registry at all.

### Phase 4 — registry reads (milestone 4)

**Step 10 — `registry.py` (index).** Fetch the per-package NDJSON index at its
sharded path (`index/st/ri/strings.json`), one line per published version,
append-only. Append-only means aggressive client caching (conditional GET /
TTL); `yanked` is the one in-place mutation — a yanked version stays
downloadable for existing lockfiles but is never newly selected. httpx with
real timeouts and retries; network failure → exit 3.
*Gate:* pytest-httpx tests with recorded fixtures; replayable offline.

**Step 11 — download → store.** Fetch the tarball from the URL in the index
line, verify the sha256 from the same line, extract atomically into the
store. A checksum mismatch is a hard error and never a re-download.
*Gate:* install a fixture package; a corrupted fixture fails verification
with the hash printed.

**Step 12 — `pme add` / `remove` / `install` / `--locked`.** `add` resolves
the latest (or the given version), edits the manifest through the tomlkit
path from step 4 (comments survive), rewrites the lock. `install` re-resolves
only when the manifest changed, then rewrites the lock; `--locked` refuses to
touch the lock and exits non-zero on drift. `build` consumes the lock as-is
and **fails** on a stale lock rather than silently re-resolving —
reproducibility is the point of the lockfile.
*Gate:* the full happy path on a fixture registry (add → install → build),
plus the `--locked` drift cases.

### Phase 5 — registry writes (milestone 5)

**Step 13 — reproducible tarball builder.** Package the git-tracked files
only, honoring `.pmeignore`; normalized entries (sorted, fixed
mtime/uid/gid/mode) so the same tree hashes identically every time; sha256 the
result. Reject any tree containing both `a/b.rald` and `a.b.rald` — that
would be `E_IMPORT_AMBIGUOUS` for every consumer (pme spec §0.2).
*Gate:* building the same tree twice yields an identical sha256.

**Step 14 — `pme login` / `publish` / `yank`.** `login` writes
`~/.emerald/credentials.toml`, mode 0600. `publish` runs the spec's flow in
order: validate the manifest (no path deps, `[lib]` present), reject the
both-spellings tree, verify the version isn't already published — immutability
is absolute, no overwrites ever — build the tarball, dry-run compile in a temp
dir against its own deps, upload, append the index line. `yank` marks a
version unselectable without deleting it.
*Gate:* publish a fixture package to a **local** git-backed index; yank it;
fresh resolution no longer selects it but an existing lockfile still installs
it.

**Step 15 — Stage-1 registry live.** Stand up the static index: a git repo
served over HTTPS (e.g. `pme-index` on GitHub Pages), tarballs as release
assets, publishes as PRs for untrusted authors. Publish the first real package
and install it from a clean machine. **Decide the stdlib boundary (pme spec
open question 1) before this step** — if stdlib ships as packages, every build
carries stdlib entries in its lockfile. The client protocol is only three
operations, so a Stage-2 service later is a `base_url` swap.
*Gate:* fresh-machine `pme install` of the published package, end to end, with
no local state.

### Phase 6 — polish (milestone 6)

**Step 16 — the rest of the CLI.** `pme test` (compile and run every
`tests/*.rald`, dev-deps on the `-I` path, pass = exit 0), `pme update` (bump
recorded minimums to the latest compatible, then re-resolve), `pme verify`
(re-hash every store entry against the lock), `pme clean` (remove `target/`;
`--store` prunes unreferenced entries).

**Step 17 — `--json` audit, docs, and the LSP handoff.** Audit that every
command emits the §8 shape; write the README. Then the emerald-lsp
integration (final subsection of §2): the LSP reads `emerald.lock`, computes
the same `-I` roots, never re-resolves. Keep the `-I` computation rule visible
and documented in one place in pme so the LSP's copy is trivially verifiable
against it.

### Ordering notes

- Steps 1–5 are the current front of work (pme milestone 1).
- Step 8 is where the frozen compiler seam is actually exercised — the most
  important thing to get right, because everything after it is plumbing
  around the same exec.
- The LSP integration (step 17 / final subsection of §2) is worth
  *designing* alongside step 8 even though it lands later — pme spec open
  question 3.
