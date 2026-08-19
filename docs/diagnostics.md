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
| `E_TYPE_PURE_CALL`            | a `pure` function calls an impure builtin or function |
| `E_TYPE_PURE_NESTED`          | an impure nested `def` inside a `pure` function |
| `E_TYPE_TERMINATION`          | recursive call does not descend structurally; declare `partial` to opt out |
| `E_TYPE_CONST`                | assigning to a `const` binding          |
| `E_TYPE_MATCH`                | `match` not exhaustive (no arm covers every remaining value) |
| `E_TYPE_BIND`                 | pattern binding already defined in scope / duplicate binding |
| `E_TYPE_TRY`                  | `try` on a non-result, outside a function, or inside a lambda |
| `E_TYPE_ERRCHAN`              | `try` propagates an error the enclosing function does not declare |
| `E_TYPE_CATCH`                | `catch` not exhaustive, on a non-result, or naming an error that cannot occur |

The three error-handling codes are documented with worked examples in
[`errors.md`](errors.md) §9.

### Shape errors (`E_SHAPE_*`)

The Phase 2 tensor shape system (`docs/shapes.md`). The diagnostic is the
deliverable: shape errors print both shapes (or the mismatching axis) as
structured notes, so `--json` carries `left` / `right` / `mismatch` fields.

| code                          | meaning                                  |
|-------------------------------|------------------------------------------|
| `E_SHAPE_MATMUL`              | `matmul` contraction axes differ (notes: `left`, `right`, `mismatch`) |
| `E_SHAPE_BROADCAST`           | elementwise/`expand` shapes cannot broadcast |
| `E_SHAPE_RESHAPE`             | `reshape` changes the number of elements |
| `E_SHAPE_PERMUTE`             | `permute` axes are not a bijection |
| `E_SHAPE_AXIS`                | reduction/slice axis out of range        |
| `E_SHAPE_RANK`                | tensor rank mismatch (e.g. `matmul` of non-2-D) |
| `E_SHAPE_DTYPE`               | unknown or mismatched tensor dtype       |
| `E_SHAPE_UNKNOWN_DIM`         | dimension name not declared (`dim`) or bound (`: dim`) |
| `E_SHAPE_DUP_DIM`             | a `dim` name declared twice              |
| `E_SHAPE_DIM_ARG`             | `: dim` argument is not a dim name or int literal |
| `E_SHAPE_INDEX`               | `Fin[n]` index provably out of range for the axis |

### Proof-mode errors (`E_PROOF_*`)

Produced only under `emeraldc --check --proof`. A clean proof-mode check is
the claim that every value is statically typed and every function is total:

| code                          | meaning                                  |
|-------------------------------|------------------------------------------|
| `E_PROOF_ANY`                 | `any` appears (annotation, signature, or inferred value); banned in proof mode |
| `E_PROOF_PARTIAL`             | a `partial` function is declared; banned in proof mode |

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
