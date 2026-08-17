# Diagnostics

Emerald's compiler reports problems as **structured diagnostics**: a stable
machine-readable error code, a precise source location (`file:line:column`),
the offending source line with a caret, and — for type mismatches — the
expected and actual types as separate fields.

This design is deliberately machine-friendly: the output can be fed back into
an LLM (or any other tool) to fix the program and re-run. Both the human and
the JSON renderings carry the same information.

## Human output

Every diagnostic is rendered as:

```
error[CODE]: human-readable message
  --> file.rald:LINE:COLUMN
    |
LINE | <the offending source line>
   | <caret at COLUMN>
   = expected: <type>
   = actual:   <type>
```

The `expected`/`actual` lines only appear when the error is a type mismatch
(assignment, argument, return, store, …). The caret points at the first token
of the construct the error is about.

Example:

```
error[E_TYPE_RETURN]: returning str from a function declared to return int
  --> foo.rald:3:5
    |
 3 |     return y
   |     ^
   = expected: int
   = actual:   str
```

## JSON output

Pass `--json` to any mode (`--check --json`, or a build) to print the same
diagnostics as a JSON array on stdout. Each element has:

| field          | type   | meaning                                          |
|----------------|--------|--------------------------------------------------|
| `kind`         | string | `syntax`, `type`, or `internal`                  |
| `severity`     | string | `error` (warnings/notes reserved)                |
| `code`         | string | stable error code, e.g. `E_TYPE_ASSIGN`          |
| `file`         | string | source path                                      |
| `line`         | int    | 1-based line                                     |
| `column`       | int    | 1-based column of the error's first token        |
| `message`      | string | human-readable message                           |
| `expected`     | string | (type mismatches) the expected type              |
| `actual`       | string | (type mismatches) the actual type                |
| `source_line`  | string | the text of the offending source line            |
| `notes`        | array  | extra structured notes `{label, value}`          |

In a multi-module program every diagnostic names the file it belongs to, and
`source_line` is quoted from *that* file — not from the entry file.

A clean file emits `[]`.

```json
[
  {
    "kind": "type",
    "severity": "error",
    "code": "E_TYPE_ARG",
    "file": "foo.rald",
    "line": 3,
    "column": 2,
    "message": "argument 1 of f(): expected int, got \"x\"",
    "expected": "int",
    "actual": "\"x\"",
    "source_line": "f(\"x\", \"y\")"
  }
]
```

## Error codes

### Syntax (`E_SYNTAX`)

Parser and lexer problems: expected/unknown tokens, unterminated strings, bad
escapes, invalid assignment targets, record literals in control-flow headers,
etc.

### Type errors (`E_TYPE_*`)

| code                          | meaning                                  |
|-------------------------------|------------------------------------------|
| `E_TYPE_ASSIGN`               | value not assignable to target/field/store |
| `E_TYPE_ARG`                  | argument type mismatch                   |
| `E_TYPE_ARITY`                | wrong number of arguments                |
| `E_TYPE_RETURN`               | return value type mismatch               |
| `E_TYPE_MISSING_RETURN`       | function can finish without returning    |
| `E_TYPE_UNDEFINED`            | undefined name or function               |
| `E_TYPE_BUILTIN_VALUE`        | builtin used as a value                  |
| `E_TYPE_NOT_CALLABLE`         | calling a non-function value             |
| `E_TYPE_FIELD`                | no such field / field on every alternative |
| `E_TYPE_INDEX`                | bad index type or not indexable          |
| `E_TYPE_IMMUTABLE`            | assigning into a string                  |
| `E_TYPE_OPERAND`              | unsupported operator operands (incl. unary) |
| `E_TYPE_ORDER`                | `<`/`>` on unordered types               |
| `E_TYPE_NO_LEN`               | `len()` on a type without a length       |
| `E_TYPE_ITER`                 | `for` over a non-iterable                |
| `E_TYPE_UNKNOWN_TYPE`         | unknown type name                        |
| `E_TYPE_NOT_GENERIC`          | type arguments on a non-generic type     |
| `E_TYPE_RECURSIVE_GENERIC`    | unsupported recursive generic alias      |
| `E_TYPE_INTERSECTION`         | `&` on non-record types                  |
| `E_TYPE_REDEFINE`             | redefining a builtin or function         |
| `E_TYPE_BREAK` / `E_TYPE_CONTINUE` | control flow outside a loop          |

### Import errors (`E_IMPORT_*`)

The module loader resolves `import`/`from` statements to files and links the
import graph into one program (see `grammar.md`). Its errors use the same
structured pipeline, so `--json` reports them identically to type errors:

| code                          | meaning                                  |
|-------------------------------|------------------------------------------|
| `E_IMPORT_NOT_FOUND`          | module path resolved to no file on the search path (notes list every root searched) |
| `E_IMPORT_CYCLE`              | import graph contains a cycle; notes list the modules on it |
| `E_IMPORT_PRIVATE`            | imported name exists but is private (leading `_`) |
| `E_IMPORT_NAME`               | imported name does not exist in that module |
| `E_IMPORT_AMBIGUOUS`          | one search root offers both `a/b.rald` and `a.b.rald`; notes list both candidates |
| `E_IMPORT_REDEFINE`           | an import binding collides with a top-level name of the importing module, or with an earlier import |
| `E_IMPORT_MODULE_VALUE`       | a module object used where a value is expected, or assigned to (use `module.name`; imports are read-only) |

### Import errors (`E_IMPORT_*`)

Raised by the module loader before type checking. See `docs/modules.md`.

| code                          | meaning                                  |
|-------------------------------|------------------------------------------|
| `E_IMPORT_NOT_FOUND`          | module path resolved to no file on the search path |
| `E_IMPORT_CYCLE`              | import graph contains a cycle; a note lists it |
| `E_IMPORT_PRIVATE`            | imported name exists but is private (leading `_`) |
| `E_IMPORT_NAME`               | imported name does not exist in that module |
| `E_IMPORT_AMBIGUOUS`          | two files under one root claim the same module path |
| `E_IMPORT_MODULE_VALUE`       | module object used as a value (only `m.<name>` is legal) |
| `E_IMPORT_REDEFINE`           | import binding collides with a local definition or an earlier import |

## Runtime errors

Runtime errors in compiled programs also report their source location. The
generated code tracks the current statement's line, so a crash says exactly
where it happened:

```
emerald: runtime error: division by zero (at foo.rald:7)
emerald: runtime error: list index out of range (index 9, length 3) (at foo.rald:3)
```

## Using diagnostics with an LLM

A typical fix loop:

1. Run `emeraldc --check --json foo.rald` (or `--json` on a build).
2. Feed the JSON array — codes, locations, expected/actual types, and the
   source line — to an LLM with the current source.
3. Apply the suggested fix, re-run, repeat until `[]` comes back clean.

The stable codes and structured `expected`/`actual` fields let the LLM reason
about *why* a program fails without parsing prose, and the `source_line` gives
it the exact context to rewrite.
