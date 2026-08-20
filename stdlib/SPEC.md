# Emerald standard library

The standard library is ordinary Emerald source in this directory. It contains
11 maintained modules and is covered by `task test:stdlib`:

| Module | Purpose |
|---|---|
| `result` | `Result`/`Option` values and checked error helpers |
| `chars` | ASCII byte classification and character helpers |
| `strings` | Splitting, searching, replacing, casing, and parsing helpers |
| `builder` | Efficient string construction |
| `lists` | List queries, transforms, sorting helpers, and list algorithms |
| `sort` | Ordering and binary-search helpers |
| `math` | Numeric constants and pure integer/float helpers |
| `io` | Result-oriented file operations and console helpers |
| `sys` | Process arguments and exit helpers |
| `path` | Pure path-component manipulation |
| `fmt` | Small format-string helpers |

Dictionaries and sets are **not** modules. They are dynamic runtime values
constructed with the builtin `dict()` and `set()` functions. Dictionaries are
string-keyed; use indexing, assignment, `len`, iteration, and `in`. Set union,
intersection, difference, and symmetric difference use `|`, `&`, `-`, and `^`.
See [`docs/builtins.md`](../docs/builtins.md) for runtime behavior.

## Importing

The compiler resolves the standard-library directory after the importing file,
`src/`, and explicit `-I` roots. A project can therefore shadow a standard
module intentionally:

```rald
import strings
from result import Result, unwrap_or
```

A module's top-level names are exported unless their names begin with `_`.
Imports are resolved once per compilation graph, dependencies are linked before
the entry module, and the module system rejects cycles and duplicate imports.
See [`docs/modules.md`](../docs/modules.md).

## Library conventions

- There are no classes, methods, exceptions, iterators, or generators.
- Fallible library operations return `Result[T, E]` or `Option[T]`; the runtime
  file builtins retain their documented aborting behavior, while `io` exposes
  result-oriented wrappers where possible.
- Functions that do not mutate or perform I/O are marked `pure`.
- Strings are byte-oriented and character helpers are ASCII-only. `len` counts
  bytes and `ord`/`chr` operate on byte values.
- `append` is a builtin because amortized in-place list growth cannot be
  expressed by ordinary module code. Other collection algorithms are library
  functions and use it where needed.
- Tensor allocation, shape checking, and numeric kernels remain builtin/runtime
  operations, documented in [`docs/tensors.md`](../docs/tensors.md) and
  [`docs/shapes.md`](../docs/shapes.md).

## Testing and maintenance

Each `tests/stdlib/*.rald` file is a small executable contract. The runner
compiles it against the repository's standard-library search path, executes it
with deterministic input, and compares its output with the neighboring
`.expected` file. Optional `.stdin` files provide input.

To regenerate goldens after an intentional behavior change:

```sh
task bless
./tests/run_tests.sh stdlib
```

Review the complete diff and run the full suite before accepting a change. A
new helper should be added only when a real compiler, example, or test needs it;
unused speculative modules belong in a future change, not in this directory.

## Known boundaries

The library intentionally does not provide Unicode text, general hashable-key
maps, channel selection, class-shaped APIs, or network/process abstractions
beyond the existing `io`, `sys`, and `run` surfaces. These are language/runtime
work rather than hidden library gaps; release risks and possible v1.1 work are
tracked in [`docs/REMAINING_V1.md`](../docs/REMAINING_V1.md).
