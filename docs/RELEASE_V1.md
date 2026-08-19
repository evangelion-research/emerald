# Emerald v1 — release readiness

Status of the tree at `main` (working tree includes the uncommitted `seq[T]` /
covariance-soundness work). This is a gap analysis, not a roadmap: everything
below is something that would be noticed by the first outside user of a v1.

**Verdict: not shippable today.** Two blockers (a red test suite, no release
plumbing) and a handful of language holes that a Python-shaped language is
expected to have. The core — lexer, parser, checker, codegen, GC, modules,
errors, green threads, stdlib — is complete and works.

---

## What is done

| Area | State |
|---|---|
| Pipeline | lexer → parser → module loader → checker → C codegen → system `cc`; 13.6 KLOC C11, builds clean under `-Wall -Wextra` (one warning, below) |
| Type system | structural records, `&`/`\|`, literal types, flow narrowing, `never`, generics + bounded generics, exhaustiveness, `pure`/`partial` |
| Errors | `error` declarations, `Result[T, E]`, `try`/`catch`, checked propagation |
| Runtime | two-generation mark-and-sweep GC w/ shadow stacks, 76 builtins |
| Concurrency | cooperative green threads: `spawn`/`join`/`chan`/`send`/`recv`/`sleep`/`task_yield`, deadlock reporting |
| Stdlib | 13 modules in Emerald (`math`, `lists`, `strings`, `sort`, `dict`, `set`, `io`, `sys`, `path`, `chars`, `fmt`, `result`, `builder`) |
| Tensors / shapes | `dim` solver, `Tensor[dtype, shape]`, `Fin[n]`, `--emit-shapes`, `--shape-report` |
| Tooling | `--check`, `--json` diagnostics, `--proof`, `--emit-tokens/-ast/-c`, `--repl`, `--werror`, `-Wno-CODE`, `-I` |
| Tests | 149 golden tests across lexer/parser/check/json/proof/e2e/imports/stdlib/repl/shape |
| Docs | 19 documents in `docs/` + `stdlib/SPEC.md` + README |
| Examples | 12 example programs incl. an MLP that trains and a typed ray tracer, all compile and run (`task examples`) |

---

## Blockers

### B1. The test suite is red — 8 failures

`./tests/run_tests.sh` → **141 passed, 8 failed**, all from the uncommitted
`seq[T]` / `W_UNSOUND_COVARIANCE` work:

- `tests/check/{bad_generics,good_generics,good_structural}.rald` and their
  `.json` variants — new `W_UNSOUND_COVARIANCE` warnings not in the goldens.
- `tests/proof/{good_local_build,good_stdlib}.rald` — proof mode now makes
  `list[T]` invariant, so `doubled([1, 2])` is `E_TYPE_ARG:
  expected list[int], got list[1 | 2]`.

The proof-mode failures are the substantive one: invariance without literal
widening at the argument position makes ordinary list literals unusable under
`--proof`. Either widen list literals to their element supertype at the call
boundary, or the feature makes proof mode unshippable. Decide, then `task bless`
the check/json goldens after reviewing the diff.

### B2. `seq[T]` is half-landed

`TY_SEQ`/`TE_SEQ` exist in the checker and parser and `freeze` is a builtin,
but the feature is uncommitted, undocumented (`docs/type-system.md` does not
mention `seq`), and has no tests of its own. A v1 cannot ship a type constructor
the manual does not describe. Either finish it (docs + tests + stdlib
signatures that take `seq` where they only read) or shelve it behind the commit
line and ship v1 with plain `list`.

### B3. No release plumbing at all

- No `--version` / `-v` flag, and no version constant anywhere in `src/`.
- No `task install` / `task dist`; the compiler is only runnable as
  `./bin/emeraldc` from the source tree, because `EMERALD_STDLIB_DIR` is baked
  in at build time as an absolute `$PWD` path. A copied binary cannot find the
  stdlib. This needs a relocatable search order (`$EMERALD_STDLIB`, then a path
  relative to the executable, then the compile-time default).
- No CI (`.github/workflows/` does not exist). v1 should not be tagged without
  `task test` running on at least macOS + Linux.
- No `CHANGELOG.md`, no install instructions in the README, no tagged release
  process.

---

## Language holes a first user will hit

These are all reachable in a five-minute session. None are hard; all are
absent.

| Missing | Today |
|---|---|
| Floor division `//` | `E_SYNTAX: expected an expression, got '/'` |
| Exponentiation `**` | `E_SYNTAX: expected an expression, got '*'` |
| Compound assignment `+= -= *= /=` | `E_SYNTAX` — every counter is `x = x + 1` |
| Tuples `(a, b)` | `E_SYNTAX` — no multiple return without a record |
| Comprehensions `[f(x) for x in xs]` | `E_SYNTAX` — `map`/lambda only |
| Dict/set literals `{1: 2}`, `{1, 2}` | `{` is always a record literal |
| Slicing `xs[1:3]` | `E_SYNTAX` |
| Default / keyword arguments | `E_SYNTAX` at `=` in the parameter list |
| f-strings | parsed as `f` applied to a string → `undefined name 'f'` |
| Bitwise operators | none; `\|` and `&` are type-level only (`stdlib/dict.rald` works around this with a `% 2^32` djb2) |

At minimum I would ship `//`, `**`, and compound assignment for v1 — they are
lexer + parser + one codegen case each, and their absence reads as
unfinished rather than as a design choice. Tuples, comprehensions, dict
literals, and slicing are legitimately v2, but the README should say so
explicitly, because "Python-flavored" sets the expectation that they exist.

### Semantics worth a decision before v1

- **Silent integer overflow.** `9223372036854775807 + 1` wraps to
  `-9223372036854775808` with no diagnostic. Pick one and document it: wrap
  (say so), trap (runtime error), or promote. Right now it is undocumented
  wrapping, which is the one option that cannot be defended after the fact.
- **No `dict`/`set` as builtin types.** They are stdlib modules over records,
  string-keyed only (`dict` cannot express "K must be hashable"). Fine as a v1
  position, but it belongs in the README's limitations, not only in
  `stdlib/dict.rald`'s header comment.

---

## Quality and polish

- **Build warning:** `src/check.c:834: unused function 'ck_warn_t'` — part of
  the uncommitted work. v1 should compile silent.
- **Builtin count drift:** `docs/README.md` says "the seventy-four builtins";
  `include/builtins.def` has 76. Regenerate that line from the `.def` file, or
  drop the number.
- **`--help` is thin.** It lists flags but not what they do, and there is no
  `emeraldc --help <flag>`, no man page. One paragraph per mode would do.
- **No `-O`/optimization passthrough.** The generated C is handed to `cc` with
  fixed flags; a user cannot ask for `-O0 -g` for debugging or `-O3` for a
  benchmark.
- **Compiler memory is never fully freed** (arena-per-run in practice). Fine
  for a batch compiler; matters for `--repl` sessions and any future LSP. Worth
  a note in `docs/architecture.md` so it is a known decision, not a leak.
- **No formatter and no editor support.** `scripts/check-format.sh` formats the
  compiler's C, not `.rald` source. A v1 language with no syntax highlighting
  for any editor is a real adoption tax; a TextMate grammar is an afternoon and
  covers VS Code, Zed, and Sublime at once.

## Robustness (checked, and fine)

- Random binary input, truncated `def f(`, and truncated `x: int =` all produce
  a proper `E_SYNTAX` diagnostic and exit 1 — no crash, no hang.
- Out-of-range list index gives `emerald: runtime error: list index out of
  range (index 10, length 3) (at file:line)` and exit 1.

Untested, and worth doing before tagging: a real fuzz run (AFL or
`libFuzzer` over `--check`), and a run of the whole suite under ASan/UBSan.
Neither has a task entry today.

---

## Suggested cut for v1

1. Resolve B1/B2 — decide on `seq`/invariance, re-bless, get to green.
2. Add `--version`, a relocatable stdlib path, `task install`, and CI.
3. Add `//`, `**`, and compound assignment.
4. README: install instructions + an explicit "not in v1" list (tuples,
   comprehensions, dict/set literals, slicing, default args, f-strings,
   bitwise ops) and the integer-overflow rule.
5. `CHANGELOG.md`, tag `v1.0.0`.

Everything else on this page is v1.1.
