# Emerald — Phase 1 (implemented)

Emerald (`.rald`) is a Python-flavored language that stays close to Python's
concepts (dynamic values, truthiness, `None`, lists, `for x in iterable`)
but uses **curly braces `{ }`** for scope, adds **TypeScript-style
structural typing** (records + gradual annotations, no classes), and
compiles to a **native binary** with a **mark-and-sweep garbage collector**.

Status: everything below is built and covered by tests (`task test`).

## Goals (Phase 1) — all delivered

- A working, from-scratch compiler in **standard C (C11)**, warning-clean
  under `-Wall -Wextra`.
- Source-to-C compiler: Emerald → C → native executable via the system `cc`.
- Language subset:
  - Integers, floats, strings, booleans, `None`, lists, **records**.
  - Arithmetic/comparison/logical operators with Python-like precedence;
    `and`/`or` genuinely short-circuit.
  - `def name(params) -> T { ... }` with recursion, mutual recursion, `return`.
  - `if` / `elif` / `else`, `while`, `for ... in <list|str>`, `break`,
    `continue`, `pass`.
  - Assignment with inference; optional annotations `x: T = v`; function &
    global scoping per the Phase-1 rule.
  - Builtins: `print`, `len`, `range`, `str`, `int`.
  - Lists: literals, `+`, `*`, indexing (negative too), `xs[i] = v`, nesting.
  - Records: `{ x: 1 }`, `p.x`, `p.x = v`, deep `==`.
- **Type layer** (see `type-system.md`): `type` aliases, record types,
  `A & B` intersection ("inheritance"), `A | B` unions, `list[T]`, `any`,
  structural width subtyping, gradual checking.
- **GC**: precise mark-and-sweep with shadow-stack rooting (see `gc.md`).
  8M-object churn runs in ~1.6 MB peak RSS.
- Comments `# ...`; semicolons optional (`a = 1; b = 2` and `a = 1 b = 2`
  both valid).

## Compiler Pipeline

```
foo.rald ─► [Lexer] ─► tokens ─► [Parser] ─► AST ─► [Checker] ─► AST
                                                                  │
                                                            [Codegen]
                                                                  │
                                                             foo.gen.c ─► cc ─► ./foo
```

| Stage    | File            | Responsibility                                             | Test hook        |
|----------|-----------------|------------------------------------------------------------|------------------|
| Lexer    | `src/lexer.c`   | Tokens, keywords, numbers, strings, comments.              | `--emit-tokens`  |
| Parser   | `src/parser.c`  | Recursive descent → AST; record/block disambiguation.      | `--emit-ast`     |
| Checker  | `src/check.c`   | Gradual structural type checking; scope/flow validation.   | `--check`        |
| Codegen  | `src/codegen.c` | AST → C with GC-rooted slot frames; short-circuit lowering.| `--emit-c`       |
| Runtime  | `src/runtime.c` | Tagged `Value` model, operators, builtins, mark-sweep GC.  | `runtime-check`  |
| Driver   | `src/main.c`    | CLI, file I/O, invokes `cc`, cleanup.                      | —                |

Every stage is exposed as a driver flag so each has its own golden test
suite under `tests/{lexer,parser,check,e2e}`.

## Runtime Value Model

```c
typedef enum { V_NONE, V_BOOL, V_INT, V_FLOAT, V_OBJ } VTag;
typedef struct { VTag tag; union { bool b; int64_t i; double f; Obj *o; } as; } Value;
/* Obj: O_STR | O_LIST | O_REC, GC-managed (see gc.md) */
```

Values are 16-byte structs passed by value; only strings/lists/records hit
the heap. The Phase-1 arena is gone — replaced by the real collector.

## Build & Test (go-task)

```
task            # build bin/emeraldc
task test       # all stage suites (16 golden tests)
task test:lexer / test:parser / test:check / test:e2e
task examples   # compile & run examples/*.rald
task bless      # regenerate golden files (review the diff!)
task clean
```

## Driver usage

```
bin/emeraldc examples/fib.rald && ./examples/fib
bin/emeraldc -o out prog.rald        # choose output path
bin/emeraldc --keep-c prog.rald      # keep prog.gen.c for inspection
bin/emeraldc --emit-c prog.rald      # print generated C to stdout
```

`$CC` overrides the C compiler; `$EMERALD_SRC` overrides the runtime
location baked in at build time.

## Phase 2 candidates

- ~~**Type narrowing**~~ — done, along with literal types, `never`,
  exhaustiveness checking, and generics. See
  [`type-system.md`](type-system.md) and [`proofs.md`](proofs.md).
- ~~**Functions as values + closures**~~ — done: arrow types `(A, B) -> C`,
  first-class function values, indirect calls, and nested `def`s that
  capture enclosing locals by shared mutable cell.
- ~~**Recursive type aliases**~~ — done (non-generic aliases may reference
  themselves).
- ~~**File/process I/O**~~ — done (`read_file`, `write_file`, `run`).
- Methods-by-convention: `f(rec, ...)` sugar as `rec.f(...)`.
- Exceptions with tracebacks (runtime errors currently exit with a message).
- Dict/`str` method library; `//`, chained comparisons, `+=`.
- Self-hosting: rewrite lexer/parser/codegen in Emerald, keep the C runtime.
