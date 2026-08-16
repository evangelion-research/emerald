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
  True False None break continue pass type`.
- Operators/punct: `{ } ( ) [ ] , . : ; = + - * / % == != < <= > >= | & ->`.
- Semicolons `;` are optional statement separators.

## Concrete Syntax (EBNF)

```
program       := statement*

statement     := func_def
               | if_stmt
               | while_stmt
               | for_stmt
               | return_stmt
               | break_stmt | continue_stmt | pass_stmt
               | type_def
               | block | simple_stmt

func_def      := "def" IDENT [tparams] "(" [param ("," param)*] ")"
                 ["->" type] block
tparams       := "[" IDENT ("," IDENT)* "]"  (* generic type parameters *)
param         := IDENT [":" type]
if_stmt       := "if" expr block
                 (("elif" | "else" "if") expr block)*
                 ["else" block]
while_stmt    := "while" expr block
for_stmt      := "for" IDENT "in" expr block
return_stmt   := "return" [expr]
type_def      := "type" IDENT [tparams] "=" type
block         := "{" statement* "}"
simple_stmt   := IDENT ":" type "=" expr     (* annotated declaration *)
               | target "=" expr             (* assignment *)
               | expr                        (* expression statement *)
target        := IDENT | postfix "[" expr "]" | postfix "." IDENT
```

## Type Expressions

```
type      := inter ("|" inter)*              (* union *)
inter     := type_atom ("&" type_atom)*      (* intersection / "extends" *)
type_atom := "int" | "float" | "str" | "bool" | "None" | "any" | "never"
           | "list" "[" type "]"
           | "{" [IDENT ":" type ("," IDENT ":" type)* [","]] "}"
           | IDENT ["[" type ("," type)* "]"]  (* alias, maybe generic *)
           | int_lit | "-" int_lit | string_lit | "True" | "False"
                                             (* literal types *)
           | "(" type ")"
```

- Type aliases must be declared (at top level) before use.
- `A & B` requires both sides to be record types; fields merge, right wins.
- Literal types make a single value a type: `type Dice = 1|2|3|4|5|6`.
  Float literals are not valid types.
- A name followed by `[...]` applies a generic alias (`Pair[int, str]`) —
  except `list[T]`, which is built in.

## Expression Precedence (low → high)

```
or             := and ( "or" and )*
and            := not ( "and" not )*
not            := "not" not | comparison
comparison     := additive ( ("=="|"!="|"<"|"<="|">"|">=") additive )*
additive       := multiplicative ( ("+"|"-") multiplicative )*
multiplicative := unary ( ("*"|"/"|"%") unary )*
unary          := "-" unary | postfix
postfix        := primary ( "(" [expr ("," expr)*] ")"    (* IDENT callee only *)
                          | "[" expr "]"
                          | "." IDENT )*
primary        := int_lit | float_lit | string_lit
               | "True" | "False" | "None"
               | IDENT | "(" expr ")"
               | "[" [expr ("," expr)*] "]"
               | "{" [IDENT ":" expr ("," IDENT ":" expr)* [","]] "}"   (* record *)
```

`and`/`or` **short-circuit** and return one of their operands, exactly like
Python (implemented via statement lowering in codegen, so side effects on the
right-hand side are skipped correctly).

### The `{` ambiguity

`{` opens both blocks and record literals. Like Go, a record literal is
forbidden at the top level of a control-flow header expression — in
`if x { ... }` the `{` always opens the block. Wrap in parentheses to force a
record there: `if (p == { x: 1 }) { ... }`. Everywhere else (assignments,
call arguments, list elements, nested in parens/brackets) records parse
normally.

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
  global's name as its loop variable will clobber the global). Blocks `{ }`
  do **not** create a new variable scope — they only group statements.
- **Truthiness** matches Python: `None`, `False`, `0`, `0.0`, `""`, `[]` are
  falsy. Records are always truthy (they model objects, not dicts).
- `5 / 2` is float division (`2.5`); `%` is Python modulo (sign of divisor).
- Comparisons work across int/float/bool; strings and lists compare
  lexicographically; `==` is deep for lists and records.
- **Builtins**: `print(*args)`, `len(x)`, `range(n)` / `range(a, b)`,
  `str(x)`, `int(x)`, `gc_stats()`, `read_file(path)`, `write_file(path, s)`,
  `run(cmd)`. Builtins cannot be shadowed or redefined.
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
