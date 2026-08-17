# Compiler architecture

How `emeraldc` is built: ~7,000 lines of C11, warning-clean under
`-Wall -Wextra`, no dependencies beyond libc and a C compiler to shell out to.

Everything described here is implemented and covered by `task test`
(87 golden tests across six suites).

## The pipeline

```
a.rald ─┐
b.rald ─┼► [Lexer] ─► tokens ─► [Parser] ─► AST ─┐
c.rald ─┘                                        │
                                          [Module loader]  ← resolves imports,
                                                 │           orders the graph
                                          one linked AST
                                                 │
                                            [Checker] ─► typed AST
                                                 │
                                            [Codegen]
                                                 │
                                            a.gen.c ─► cc ─► ./a
```

| Stage    | File            | Responsibility                                                        | Driver flag      |
|----------|-----------------|-----------------------------------------------------------------------|------------------|
| Lexer    | `src/lexer.c`   | Tokens, keywords, numbers, strings, comments.                          | `--emit-tokens`  |
| Parser   | `src/parser.c`  | Recursive descent → AST; record/block disambiguation.                  | `--emit-ast`     |
| Modules  | `src/module.c`  | Import resolution, cycle detection, name mangling, linking.            | `-I <dir>`       |
| Checker  | `src/check.c`   | Structural type checking, flow narrowing, scope/return validation.     | `--check`        |
| Codegen  | `src/codegen.c` | AST → C with GC-rooted slot frames; short-circuit lowering.            | `--emit-c`       |
| Runtime  | `src/runtime.c` | Tagged `Value` model, operators, builtins, generational GC.            | `runtime-check`  |
| Diags    | `src/diag.c`    | Structured errors: code, location, caret, expected/actual, JSON.       | `--json`         |
| Driver   | `src/main.c`    | CLI, file I/O, invokes `cc`, cleanup.                                  | —                |

Every stage is exposed as a driver flag, so every stage has its own golden test
suite. That is the main structural decision in the compiler: no stage is
observable only through the stage after it.

`src/check.c` is the largest file (~2,000 lines) and it is where the interesting
work is — see [`type-system.md`](type-system.md).

## Compilation unit

The unit of compilation is the **import graph**, not the file. `emeraldc a.rald`
loads `a.rald`, follows its imports, resolves each against the importer's
directory → the project `src/` root → each `-I <dir>` in order (first hit wins),
detects cycles, topologically orders the modules, mangles each module's
top-level names to `<module>__<name>`, and hands the checker one program. The
checker and codegen never see modules; by the time they run, linking has already
happened. Details and the CLI contract are in [`modules.md`](modules.md).

## Runtime value model

```c
typedef enum { V_NONE, V_BOOL, V_INT, V_FLOAT, V_OBJ } VTag;
typedef struct { VTag tag; union { bool b; int64_t i; double f; Obj *o; } as; } Value;
/* Obj: O_STR | O_LIST | O_REC | O_FUNC, GC-managed (see gc.md) */
```

Values are 16-byte structs passed by value. Only strings, lists, records, and
closures reach the heap; ints, floats, bools, and `None` never allocate. Short
strings are stored inline (SSO) rather than as a separate allocation.

Types are **erased**. The checker's work is entirely compile-time: the generated
C manipulates `Value`, and a `Point3` and a `{x,y,z}` record literal are the
same thing at runtime. This is what makes structural typing cheap — there is no
class, vtable, or nominal tag to carry.

Closures are heap objects holding a function pointer plus a captured
environment. A nested `def` capturing an enclosing local captures it **by
shared mutable cell**, so two closures over the same variable observe each
other's writes.

Generated C roots every live local in a shadow-stack frame so the collector can
find them precisely — see [`gc.md`](gc.md).

## Tests

```
tests/lexer/    3 suites   token streams              (--emit-tokens)
tests/parser/   5 suites   AST dumps                  (--emit-ast)
tests/check/   17 suites   diagnostics, human + JSON  (--check, --check --json)
tests/proof/    4 suites   proof mode                 (--check --proof)
tests/e2e/     18 suites   compile, run, compare stdout
tests/imports/ 23 suites   module resolution and linking errors
```

`tests/check/` holds two golden files per case (`.expected` and
`.json.expected`) so the JSON diagnostic schema is pinned as tightly as the
human-readable output. `tests/imports/` cases are directories — a whole
multi-file program each — with `bad_*` cases asserting the error and the rest
asserting the linked program's output.

`task bless` regenerates every golden file. Review that diff before committing;
it is the only thing standing between a refactor and a silently changed
language.

## Build and test

```
task              # build bin/emeraldc
task test         # every suite (87 golden tests) + runtime-check
task test:lexer / test:parser / test:check / test:proof / test:e2e / test:imports
task examples     # compile & run examples/*.rald and examples/*/main.rald
task bless        # regenerate golden files (review the diff!)
task clean
```

`task runtime-check` compiles `src/runtime.c` standalone under strict flags,
proving the runtime stays independent of the compiler's headers — it is
compiled into every generated program, not into `emeraldc`.

## Driver usage

```
emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald

emeraldc examples/fib.rald && ./examples/fib
emeraldc -o out prog.rald        # choose output path
emeraldc --check prog.rald       # typecheck only
emeraldc --check --proof prog.rald  # proof mode: ban `any` and `partial`
emeraldc --keep-c prog.rald      # keep prog.gen.c for inspection
emeraldc --emit-c prog.rald      # print generated C to stdout
```

`$CC` overrides the C compiler. `$EMERALD_SRC` overrides the runtime source
location baked in at build time (`-DEMERALD_SRC_DIR`), which is how a built
`emeraldc` finds `runtime.c` to compile alongside your program.

## Near-term engineering (not research)

The research agenda lives in
[`research-directions.md`](research-directions.md). What follows is the
ordinary language work that is still missing and does not need a paper:

- Methods-by-convention: `f(rec, ...)` callable as `rec.f(...)`.
- Exceptions with tracebacks (runtime errors currently abort with a located
  message).
- A dict type and a string-method library, written as Emerald modules resolved
  from a `src/` root rather than as more builtins — see
  [`builtins.md`](builtins.md).
- `//`, chained comparisons, `+=`.
- Separate compilation and a module cache; today the whole import graph is
  re-parsed and re-checked on every build.
- Self-hosting: rewrite lexer/parser/codegen in Emerald, keep the C runtime.
