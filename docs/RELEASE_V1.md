# Emerald v1 — release readiness

Status of the tree at `main`. The `seq[T]` / covariance-soundness design is
landed and covered by the checker, proof, and end-to-end suites. This is a gap
analysis, not a roadmap: everything below is something that would be noticed by
the first outside user of a v1.

> **Resolved.** The suggested v1 cut below has been implemented: the test suite
> is green (182 passing), `seq[T]` and the list variance policy are documented
> and tested, `//`/`**`/compound assignment are in, `--version`, `task install`,
> `task dist`, a relocatable stdlib, CI, `CHANGELOG.md`, and the README's
> install instructions / "not in v1" list / overflow rule all landed. Only the
> `v1.0.0` tag remains a manual step. The body below is kept as the original gap
> analysis, with B1/B2 updated to record their resolution.

**Verdict: v1 candidate.** B1/B2 are resolved: the full suite is green and the
sequence/variance design is explicit. The remaining language holes are either
intentional v1 omissions or follow-up polish; the core — lexer, parser, checker,
codegen, GC, modules, errors, green threads, and stdlib — is complete and works.

---

## What is done

| Area | State |
|---|---|
| Pipeline | lexer → parser → module loader → checker → C codegen → system `cc`; 13.6 KLOC C11, builds clean under `-Wall -Wextra` (one warning, below) |
| Type system | structural records, `&`/`\|`, literal types, flow narrowing, `never`, generics, `list`/`seq` variance, exhaustiveness, `pure`/`partial` |
| Errors | `error` declarations, `Result[T, E]`, `try`/`catch`, checked propagation |
| Runtime | two-generation mark-and-sweep GC w/ shadow stacks, 78 builtins |
| Concurrency | cooperative green threads: `spawn`/`join`/`chan`/`send`/`recv`/`sleep`/`task_yield`, deadlock reporting |
| Stdlib | 11 Emerald modules (`math`, `lists`, `strings`, `sort`, `io`, `sys`, `path`, `chars`, `fmt`, `result`, `builder`); dict/set are runtime builtins |
| Tensors / shapes | `dim` solver, `Tensor[dtype, shape]`, `Fin[n]`, `--emit-shapes`, `--shape-report` |
| Tooling | `--check`, `--json` diagnostics, `--proof`, `--emit-tokens/-ast/-c`, `--repl`, `--werror`, `-Wno-CODE`, `-I` |
| Tests | 182 passing golden/unit cases across lexer/parser/check/json/proof/e2e/imports/stdlib/repl/shape |
| Docs | 19 documents in `docs/` + `stdlib/SPEC.md` + README |
| Examples | 12 example programs incl. an MLP that trains and a typed ray tracer, all compile and run (`task examples`) |

---

## Blockers

### B1. Resolved — green suite and explicit variance policy

`./tests/run_tests.sh` is green with **182 passing**. The chosen policy is:
mutable `list[T]` remains covariant in ordinary mode for compatibility, but
unsound upcasts emit `W_UNSOUND_COVARIANCE`; under `--proof`, mutable lists are
invariant (apart from fresh-literal widening and the explicit gradual `any`
escape hatch). The warning, `--werror`, suppression, text/JSON diagnostics, and
proof-mode rejection all have goldens. `task bless` was run after the policy
was fixed and the generated outputs were reviewed.

### B2. Resolved — `seq[T]` is the sound immutable boundary

`seq[T]` is retained and finished rather than shelved. It is covariant in every
mode because the checker rejects `append` and indexed assignment; `freeze` is a
zero-copy list-to-seq view and `thaw` copies back to a mutable list. Indexing,
length, iteration, and `map`/`filter`/`reduce` preserve the sequence kind.
`docs/type-system.md` and `docs/builtins.md` now describe the rules, while
`tests/e2e/seq.rald`, `tests/check/good_seq_descent.rald`, and the new proof-mode
`tests/proof/good_seq.rald` cover contextual literals, read-only operations,
structural descent, covariance, and the thaw boundary.

### B3. Resolved — release plumbing is present

`--version`, relocatable stdlib lookup, `task install`, `task dist`, CI,
`CHANGELOG.md`, README install instructions, and the release process are landed.
The remaining manual step is creating the `v1.0.0` tag.

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
| Dictionaries and sets | builtin `dict()`/`set()` values; dictionaries are string-keyed and set operations use `|`, `&`, `-`, and `^` |

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
  the builtin collection documentation in `docs/builtins.md`.

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

1. B1/B2: decide `seq`/invariance, re-bless, and get the full suite green —
   **resolved**.
2. Add `--version`, a relocatable stdlib path, `task install`, and CI —
   **resolved**.
3. Add `//`, `**`, and compound assignment — **resolved**.
4. Add README install instructions, the explicit omissions list, and the
   integer-overflow rule — **resolved**.
5. Add `CHANGELOG.md`; create the `v1.0.0` tag manually.

Everything else on this page is v1.1.
