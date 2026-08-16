# Emerald's Type System

TypeScript-flavored **structural, gradual** typing over a Python-flavored
dynamic core. There are no classes: data is records, "inheritance" is
subset-of-fields.

## The types

| Syntax            | Meaning                                              |
|-------------------|------------------------------------------------------|
| `int` `float` `str` `bool` `None` | primitives                          |
| `any`             | the gradual escape hatch; compatible with everything |
| `list[T]`         | list with element type `T`                           |
| `{ x: int, y: int }` | record (structural, anonymous)                    |
| `A \| B`          | union                                                |
| `A & B`           | intersection of two record types (fields merge, right wins) |
| `type Name = ...` | alias (must be declared before use, top level only)  |

## Structural subtyping = "inheritance" without classes

```
type Point  = { x: int, y: int }
type Point3 = Point & { z: int }     # like `interface Point3 extends Point`

def mag2(p: Point) -> int { return p.x * p.x + p.y * p.y }

p: Point3 = { x: 3, y: 4, z: 5 }
mag2(p)                              # OK: Point3 has everything Point needs
```

Assignability rules (`dst <- src`):

- `any` accepts and flows into everything (gradual typing).
- `bool -> int -> float` widen implicitly (Python numeric tower).
- A record fits a record type if it has **at least** the target's fields,
  with assignable types (width subtyping). Extra fields are fine.
- `src` fits `A | B` if it fits either; `A | B` fits `dst` only if **every**
  alternative fits.
- `list[S]` fits `list[T]` if `S` fits `T` — covariant, i.e. conveniently
  unsound in exactly the way TypeScript arrays are. A `list[int]` passed as
  `list[int | None]` can be mutated to smuggle a `None` back; the runtime's
  tagged values keep this from ever being memory-unsafe.

## Gradualness

- Unannotated parameters and returns are `any`; unannotated code checks
  exactly like Python would run.
- An **annotated** variable (`n: int = 5`) is enforced forever: assigning a
  `str` to it later is a compile error.
- An **inferred** variable takes the type of its first assignment; a
  conflicting later assignment quietly *widens* the variable
  (`x = 1` then `x = "s"` makes `x: int | str`) rather than erroring —
  Python code should stay valid.
- Field access is checked on typed records (`p.z` on a `Point` is a compile
  error, as is assigning a new field); on `any` everything is allowed and
  checked at runtime instead.
- On a union, a field access requires the field to exist on **every**
  alternative.

## What the checker also catches

- Unknown names, unknown types, calls to undefined functions.
- Wrong arity and wrong argument types at every call site.
- Return-type mismatches; `return` outside a function.
- `break`/`continue` outside a loop; nested `def` (unsupported).
- Redefining or shadowing builtins; using a function name as a value.
- Non-iterables in `for`, non-indexables under `[]`, `str` item assignment.

Errors carry `file:line:` and don't stop the checker — you get the full
list. (Top-level errors print before function-body errors, because bodies
are checked in a later pass once global types are known.)

## Deliberate omissions (Phase 2)

- **No narrowing**: `if x != None` does not refine `int | None` to `int`.
  This is the biggest missing piece and the first thing to add.
- No generics beyond the built-in `list[T]`.
- No function types (functions aren't values yet).
- No depth-subtyping error messages tuned for big records — messages print
  the whole structural type.
