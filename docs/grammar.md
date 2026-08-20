# Emerald Grammar (v2 — as implemented)

Emerald keeps Python's *concepts and semantics* but uses **curly braces for
scope** and adds a **TypeScript-style structural type layer** (records +
gradual annotations) instead of classes. No significant indentation;
statements are separated by newlines (ignored), semicolons, or the grammar
itself.

## Lexical Grammar

```
letter     = "A".."Z" | "a".."z" | "_" ;
digit      = "0".."9" ;
ident      = letter (letter | digit)* ;
int_lit    = digit+ ;
float_lit  = digit+ "." digit* | digit+ ("e"|"E") ("+"|"-")? digit+ ;
string_lit = '"' chars '"' | "'" chars "'" ;
comment    = "#" .* to end of line ;
```

- Whitespace (space, tab, newline) separates tokens; newlines are **not significant**.
- Strings support escapes: `\\`, `\"`, `\'`, `\n`, `\t`, `\r`, `\0`.
- Keywords: `def if else elif while for in return and or not
  True False None break continue pass type pure partial import from as
  const match dim error try catch`.
- Operators/punct: `{ } ( ) [ ] , . : ; = + - * / % == != < <= > >= | & ->
  => |> >>` (`=>` lambda, `|>` pipe, `>>` compose).
- Semicolons `;` are optional statement separators.

## Concrete Syntax (EBNF)

```
program       := import_stmt* statement*

import_stmt   := "import" dotted ["as" IDENT]
               | "from" dotted "import" alias ("," alias)*
dotted        := IDENT ("." IDENT)*
alias         := IDENT ["as" IDENT]

statement     := func_def
               | if_stmt
               | while_stmt
               | for_stmt
               | return_stmt
               | break_stmt | continue_stmt | pass_stmt
               | match_stmt
               | type_def
               | error_def
               | dim_def
               | import_stmt
               | block | simple_stmt

import_stmt   := "import" dotted ["as" IDENT]
               | "from" dotted "import" alias ("," alias)*
dotted        := IDENT ("." IDENT)*
alias         := IDENT ["as" IDENT]

func_def      := "def" IDENT [tparams] "(" [param ("," param)*] ")"
                 ["->" type] ["pure"] ["partial"] block
                 (* `pure`: may only call pure code; `partial`: opts out of
                    termination checking — see type-system.md *)
tparams       := "[" IDENT [":" "dim"] ("," IDENT [":" "dim"])* "]"
                 (* generic type parameters; `: dim` kind one as a dimension *)
param         := IDENT [":" type]
if_stmt       := "if" expr block
                 (("elif" | "else" "if") expr block)*
                 ["else" block]
while_stmt    := "while" expr block
for_stmt      := "for" IDENT "in" expr block
return_stmt   := "return" [expr]
type_def      := "type" IDENT [tparams] "=" type
error_def     := "error" IDENT ["{" [field ("," field)*] "}"]
                 (* sugar: `error E { f: T }` == `type E = { _tag: "E", f: T }` *)
field         := IDENT ":" type
dim_def       := "dim" IDENT ("," IDENT)*   (* nominal dimension names *)
block         := "{" statement* "}"
simple_stmt   := "const" IDENT [":" type] "=" expr  (* immutable binding *)
               | IDENT ":" type "=" expr     (* annotated declaration *)
               | target "=" expr             (* assignment *)
               | expr                        (* expression statement *)
target        := IDENT | postfix "[" expr "]" | postfix "." IDENT

match_stmt    := "match" expr "{" (pattern "->" block)+
                   (pattern "->" block)* "}"
pattern       := "_" | IDENT | literal | "{" pattern_field ("," pattern_field)* "}"
pattern_field := IDENT [":" pattern]        (* `{ x }` binds field x to x *)
```

- `const x = v` (and `const x: T = v`) binds `x` immutably: any later
  assignment is `E_TYPE_CONST`. `const` is the default style for the
  functional core — prefer it over `=` for bindings that never change.
- `match` is exhaustive: the checker proves the patterns cover the subject's
  type and rejects a match with no catch-all when it cannot (`E_TYPE_MATCH`).
  Patterns bind names only in their own arm. `_` matches anything.
- `error Name { ... }` declares an expected failure. It is pure sugar for a
  record type carrying a literal discriminant, so nothing downstream needs a
  special case: `error Empty` is `type Empty = { _tag: "Empty" }`. Build one
  with `Name { field: v }`, which fills in the `_tag` for you. See
  `docs/type-system.md` for how `try` and `catch` use them.

## Expected Errors

```
try_expr      := "try" unary        (* unwrap a result, or return its failure *)
catch_expr    := "catch" expr "{" catch_arm ("," catch_arm)* [","] "}"
catch_arm     := (IDENT | "_") [IDENT] "->" expr
```

- `try e` evaluates `e`, which must have a *result* type — `{ ok: True, val: T }
  | { ok: False, err: E }`, spelled `Result[T, E]` in the standard library. On
  success it is the `T`; on failure the enclosing function returns the failure
  unchanged. It binds tighter than every binary operator, so `try f(x) |> g`
  pipes the unwrapped value.
- The enclosing function must declare a result type whose error side can carry
  everything `e` can fail with, or the escaping error is `E_TYPE_ERRCHAN`. A
  lambda has no declared return type and so no channel: `try` inside one is
  `E_TYPE_TRY` (use a nested `def`).
- `catch e { ... }` is an **expression**: its value is `e`'s success value, or
  the value of the arm that matched. Each arm names an error type and may bind
  it (`NotFound e -> e.key`); `_` is the catch-all, and binds whatever the
  named arms did not take. Every error the subject can produce must be covered
  (`E_TYPE_CATCH`). Arm bodies are single expressions, like lambda bodies —
  a `{` there opens a record literal, not a block.
- Neither form is exception handling: a result is an ordinary value, `try` is
  an early `return`, and there is no stack unwinding or hidden control flow.

See [`errors.md`](errors.md) for the semantics, and
[`core-calculus.md`](core-calculus.md) §5 for `[T-TRY]` and `[T-CATCH]`.

## Type Expressions

```
type      := inter ("|" inter)*              (* union *)
inter     := type_atom ("&" type_atom)*      (* intersection / "extends" *)
type_atom := "int" | "float" | "str" | "bool" | "None" | "any" | "never"
           | "list" "[" type "]"
           | "seq" "[" type "]"              (* immutable, covariant sequence *)
           | "Tensor" "[" type "," ("?" | "[" dim ("," dim)* "]") "]"
                                             (* tensor with static or dynamic shape *)
           | "Fin" "[" dim "]"               (* index provably below `dim` *)
           | "Eq" "[" dim "," dim "]"        (* propositional equality of dims *)
           | "{" [IDENT ":" type ("," IDENT ":" type)* [","]] "}"
           | "(" [type ("," type)*] ")" "->" type   (* function type *)
           | IDENT ["[" type ("," type)* "]"]  (* alias, maybe generic *)
           | int_lit | "-" int_lit | string_lit | "True" | "False"
                                             (* literal types *)
           | "(" type ")"

dim         := dim_term ("+" dim_term)*      (* dimension arithmetic *)
dim_term    := dim_factor ("*" dim_factor)*
dim_factor  := IDENT | int_lit
```

- Type aliases must be declared (at top level) before use. Across modules this
  is automatic: linking orders imported modules before the modules that import
  them. See `docs/modules.md`.
- A qualified type name (`m.T`) is not valid syntax; bring a type across a module
  boundary with `from m import T`.
- `A & B` requires both sides to be record types; fields merge, right wins.
- Literal types make a single value a type: `type Dice = 1|2|3|4|5|6`.
  Float literals are not valid types.
- A name followed by `[...]` applies a generic alias (`Pair[int, str]`) —
  except `list[T]`, `seq[T]`, `Fin[n]`, and `Eq[a, b]`, which are built in.
- `seq[T]` is the immutable, covariant sequence: the checker refuses to mutate
  one, so `seq[Circle]` is soundly a `seq[Shape]`. `freeze(xs)`/`thaw(s)`
  convert, and `[1, 2]` in a `seq` context is a seq literal. Under `--proof`,
  `list[T]` becomes invariant while `seq[T]` stays covariant.
- `Eq[a, b]` is the type of evidence that the dim expressions `a` and `b` are
  equal; `refl` inhabits `Eq[a, a]`. A value `e: Eq[a, b]` in scope makes
  `Tensor[f32, [a]]` usable as `Tensor[f32, [b]]`. See `proofs.md`.
- Function types `(T1, T2) -> U` describe first-class functions and lambdas;
  they are structural, so `def f(x: int) -> int` has type `(int) -> int` and
  can be passed anywhere a value of that type is expected.
- `dim Batch, Seq` declares nominally distinct dimension names, usable inside a
  `Tensor[f32, [Batch, Seq]]` shape (or `Fin[Batch]`). `Tensor[f32, ?]` is the
  dynamic-shape escape hatch. See `docs/shapes.md` for the typing rules.

## Expression Precedence (low → high)

```
pipe          := compose ( "|>" compose )*         (* x |> f == f(x) *)
compose       := or ( ">>" or )*                  (* f >> g == x -> g(f(x)) *)
or            := and ( "or" and )*
and           := not ( "and" not )*
not           := "not" not | comparison
comparison     := additive ( ("=="|"!="|"<"|"<="|">"|">=") additive )*
additive       := multiplicative ( ("+"|"-") multiplicative )*
multiplicative := unary ( ("*"|"/"|"%") unary )*
unary          := "-" unary | "try" unary | postfix
postfix        := primary ( "(" [expr ("," expr)*] ")"    (* IDENT callee only *)
                          | "[" expr "]"
                          | "." IDENT )*
primary        := int_lit | float_lit | string_lit
               | "True" | "False" | "None"
               | "refl"                            (* Eq[a, a] evidence; erased *)
               | catch_expr
               | IDENT "{" [IDENT ":" expr ("," IDENT ":" expr)*] "}" (* error literal *)
               | IDENT | "(" expr ")"
               | "(" [param ("," param)*] ")" "=>" expr   (* lambda *)
               | "[" [expr ("," expr)*] "]"
               | "{" [IDENT ":" expr ("," IDENT ":" expr)* [","]] "}"   (* record *)
```

- `|>` pipes a value into a unary function (`x |> f` ≡ `f(x)`), so data flows
  left to right: `xs |> (ys) => map(f, ys)`.
- `>>` composes two unary functions (`f >> g` ≡ `x -> g(f(x))`); it binds
  tighter than `|>` and is left-associative.
- A lambda `(a: int, b) => body` is an anonymous function value. Parameters
  may carry optional type annotations; unannotated ones are inferred
  contextually from the call site (e.g. inside `map(...)`) or fall back to
  `any`. Lambdas capture their enclosing locals by reference.
- A zero-parameter lambda `() => body` is a *thunk* — the standard way to get
  laziness in a strict language: the body is not evaluated until the thunk is
  called. Lambda bodies are single expressions (a `{` after `=>` is a record
  literal); for multi-statement bodies use a nested `def`.

`and`/`or` **short-circuit** and return one of their operands, exactly like
Python (implemented via statement lowering in codegen, so side effects on the
right-hand side are skipped correctly).

### Extended expression forms

Tuples use comma-separated parenthesized expressions (`(a, b)`; `(a,)` is a
one-tuple). Bracket comprehensions support `[expr for name in seq if cond]`;
brace comprehensions support `{expr for name in seq if cond}` and
`{key: value for name in seq if cond}`. Non-record brace literals are dynamic
sets (`{1, 2}`) or dictionaries (`{"name": value}`); identifier keys followed
by a colon remain structural record fields.

Postfix indexing accepts slices with omitted bounds and an optional step:
`xs[lo:hi]`, `xs[:hi]`, `xs[lo:]`, and `xs[::step]`. Function parameters may
have trailing defaults (`def f(x, y = 1)`) and calls may use keyword arguments
(`f(y = 2, x = 3)`). An f-string is an `f`-prefixed quoted string with
`{expression}` interpolations; doubled braces escape literal braces.

Bitwise `|`, `^`, `&`, `<<`, and numeric `>>` operate on integers. The
function-valued form of `>>` retains composition; `>>>` is also accepted as an
unambiguous right-shift spelling.

### The `{` ambiguity

`{` opens both blocks and record literals. Like Go, a record literal is
forbidden at the top level of a control-flow header expression — in
`if x { ... }` the `{` always opens the block. Wrap in parentheses to force a
record there: `if (p == { x: 1 }) { ... }`. Everywhere else (assignments,
call arguments, list elements, nested in parens/brackets) records parse
normally.

The error literal `Name { field: v }` extends that rule: `IDENT {` opens one
only when the brace is followed by `field:` or closes immediately, and only
where a record literal is allowed at all. The one construction it takes away
is a statement that is a bare name followed by a block statement — write the
block's first statement first, or parenthesize the name.

## Semantics Notes

- **Values are runtime-tagged** (`None`, `bool`, `int`, `float`, `str`,
  `list`, `record`); annotations are checked at compile time and erased.
- **Records instead of classes**: `p = { x: 1, y: 2 }`, `p.x`, `p.x = 3`.
  Structural typing: any record with at least the fields of `Point` is a
  `Point` (width subtyping). Records compare by value with `==`.
- **Scoping** mirrors the Phase-1 rule: names assigned at top level are
  *globals*; names assigned inside `def` are *locals*, **unless the name
  already exists as a global — then the assignment updates the global**.
  Watch out: this applies to `for` loop variables too (a function reusing a
  global's name as its loop variable will clobber the global). That rule stops
  at the module boundary — a global is only updatable from the file that
  declared it, so a library cannot clobber its importer's globals (see
  [`modules.md`](modules.md)). Blocks `{ }` do **not** create a new variable
  scope — they only group statements.
- **Truthiness** matches Python: `None`, `False`, `0`, `0.0`, `""`, `[]` are
  falsy. Records are always truthy (they model objects, not dicts).
- `5 / 2` is float division (`2.5`); `%` is Python modulo (sign of divisor).
- Comparisons work across int/float/bool; strings and lists compare
  lexicographically; `==` is deep for lists and records.
- **Modules**: `import text.strings` binds the last path component
  (`strings`) as a module object; `from text.strings import split as s`
  binds `split` (as `s`) directly. Only top-level `def`s, `type` aliases,
  and globals are exported; names starting with `_` are private. A module
  is resolved against the importing file's directory, then the project's
  `src/` root, then each `-I` root in command-line order (first hit wins,
  so an earlier `-I` root shadows a later one). Each module is parsed once
  and linked dependencies-first; its top-level names are mangled to
  `<module>__<name>` internally so two modules can both define `parse`
  without colliding. `import` is only allowed at the top level.
- **Builtins**: `print(*args)`, `eprint(*args)`, `len(x)`, `range(n)` /
  `range(a, b)`, `str(x)`, `int(x)`, `float(x)`, `sqrt(x)`, `tan(x)`, `rand()`,
  `gc_stats()`, `append(xs, v)`, `slice(seq, lo, hi)`, `ord(c)`, `chr(n)`,
  `argv()`, `exit(code)`, `map`/`filter`/`reduce`, `read_file(path)`,
  `read_file_opt(path)`, `file_exists(path)`, `write_file(path, s)`,
  `append_file(path, s)`, `run(cmd)`. Builtins cannot be shadowed, redefined,
  or used as values. Signatures and semantics: [`builtins.md`](builtins.md).
- **Standard library**: ordinary Emerald modules in `stdlib/`, resolved with no
  `-I` flag (`import strings`). See [`../stdlib/SPEC.md`](../stdlib/SPEC.md).
- **Functions are values**. A function type is written `(A, B) -> C`; a
  top-level function name reads as a closure, can be stored, passed, and
  called indirectly (`f(x)`). A `def` may be nested inside another `def`; a
  nested function **captures** enclosing locals (and parameters) by shared,
  mutable cell, so assignments in the enclosing scope are visible through
  the closure. Assigning a name inside a nested function still makes it a
  *local* of that function (as with globals, no `nonlocal`); mutate a
  captured list/record to share state.
- **Known limitations**: no classes/methods, no exceptions, no `//` floor
  division, comparison chains associate pairwise (`a < b < c` is
  `(a < b) < c`, unlike Python).
