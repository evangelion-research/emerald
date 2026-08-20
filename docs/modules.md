# Modules

An Emerald program is one or more `.rald` files. A file is a **module**: it can
name code in another file with `import`, and the compiler resolves those names
to files, loads the whole graph, and compiles it as a single program.

## Syntax

```
import strings                       # module object; strings.split(...)
import strings as s                  # ... bound as `s` instead
import text.strings                  # dotted path; binds `strings`
from strings import split, join      # names lifted into this module
from strings import join as concat   # ... renamed
```

Imports are only legal at the **top level** of a module — never inside a `def`
or a block.

A plain `import a.b.c` binds the path's **last component** (`c`). Emerald has no
package objects, so there is nothing for `a` alone to name; use `as` when the
last component is not the name you want.

## Resolution

A dotted module path maps to a file. `text.strings` is looked for as
`text/strings.rald` (nested) or `text.strings.rald` (flat), under each search
root **in this order**:

1. the directory of the importing file,
2. the project's `src/` root — the nearest `src/` directory found by walking up
   from the entry file's directory,
3. each `-I <dir>` given on the command line, in the order given,
4. the standard library.

The stdlib root is baked in at build time and overridable with
`$EMERALD_STDLIB`, the way `$EMERALD_SRC` overrides the runtime's location. It
is searched **last** deliberately: a project that defines its own `strings.rald`
shadows the stdlib one, which is the same "first hit wins" precedence the `-I`
roots already follow. In diagnostics it prints as `<stdlib>` rather than an
absolute path, so error output does not depend on where the compiler was built.

`import strings` therefore works with no flags at all — see
[`stdlib/SPEC.md`](../stdlib/SPEC.md).

The first root with a hit wins; a later `-I` never shadows an earlier one. If a
*single* root offers both spellings (`text/strings.rald` and
`text.strings.rald`), that is `E_IMPORT_AMBIGUOUS` — an error, not a silent pick.

Each file is loaded once no matter how many modules import it, so a diamond in
the import graph costs nothing. Module identity is the file's canonical path,
so two different spellings of one file (`shared` from a sibling directory,
`shared` through a `-I` root) still name the same module. A cycle is `E_IMPORT_CYCLE`, reported with the
modules on the cycle — never a hang.

## Exports and privacy

A module exports its top-level `def`s, `type` aliases, and global variables.
The export rule is the cheapest one that fits the language:

> **A leading underscore means private. Everything else is exported.**

No new keyword. `_helper` is reachable from inside its own module and nowhere
else; reaching for it from outside is `E_IMPORT_PRIVATE`, whether you write
`from m import _helper` or `m._helper`.

Naming a name a module doesn't have is `E_IMPORT_NAME`.

An import binding may not collide with one of the importing module's own
top-level names, or with an earlier import in the same file — either would
silently shadow one of the two, so both are `E_IMPORT_REDEFINE`.

## What a module can share

Values, functions (including generic ones), and type aliases all cross a module
boundary:

```
# strings.rald
type Word = str

def shout(s: Word) -> str { return s + "!" }
def _emphasis() -> str { return "!!!" }
```

```
# main.rald
from strings import shout, Word

w: Word = "hey"
print(shout(w))
```

Types have to come across through `from`-import: `m.T` in a *type* position is
rejected, because the type grammar has no qualified names. Values and functions
work either way.

## How linking works

The checker and the code generator know nothing about modules. They see one flat
program, exactly as they did before modules existed. `src/module_*.c` bridges the
gap: it walks the import graph, parses each module once, and **links** the
results into a single `Program` whose statements are ordered dependencies-first.

Linking is a rename pass. Every imported module gets a prefix from its dotted
path, and each reference to one of its top-level names — from inside the module
and from its importers alike — becomes `<module>__<name>`:

```
strings.split(x)   ->  strings__split(x)
text.strings.split ->  text_strings__split
```

So two packages can each define `parse` without colliding. The **entry module is
never renamed**, which keeps its diagnostics reading exactly as they did before.

Renaming respects scoping, or a local named `split` would be swept up by it. The
rule mirrors the checker and codegen exactly: inside a function, the locally
bound names are its parameters, its nested `def`s, and every assigned name that
is not already a module-level global (assigning a global's name updates the
global). Those are left alone; everything else that names a module-level
definition is rewritten.

### Globals belong to their module

"Assigning a global's name updates the global" holds **only inside the module
that declared the global**. Across a module boundary the two names are
unrelated, and a library function assigning `xs` must not write to an importer's
`xs` just because linking put them in one translation unit.

This is not a style rule, it is a correctness one. Linking leaves function-body
locals unrenamed while the entry module's globals keep their bare names, so
without the restriction:

```
# lists.rald
def flatten[T](xss: list[list[T]]) -> list[T] { for xs in xss { ... } }

# main.rald
xs = [3, 1, 4, 1, 5]
lists.flatten([[1], [2]])     # silently reassigns main's `xs` to [2]
```

The checker (`updatable_global`) and codegen (`global_owned_by`) both compare
the assigning statement's source file against the global's declaring file, so
`xs` inside `flatten` is a local of `flatten` and the importer's global is
untouched. Within one file the Python-style rule is unchanged.

Because renaming can make a symbol unrecognizable, every AST node that gets
rewritten keeps the spelling the user wrote, and diagnostics quote *that*:

```
error[E_TYPE_ARG]: argument 1 of lib.takes_int(): expected int, got "not an int"
```

One thing is deliberately *not* renamed: the `_tag` an `error` declaration
bakes into its values. The type alias is mangled like any other name
(`errs.NotFound` → `errs__NotFound`), but the discriminant stays the
source-level `"NotFound"`, so an error declared in one module is caught by its
written name in another. The consequence is structural, like the rest of the
language: two modules that both declare `error NotFound` produce values nothing
can tell apart. See [`errors.md`](errors.md) §7.

Modules are concatenated into one translation unit, which preserves the existing
GC shadow-stack setup and avoids designing a linking story on day one. Splitting
into separate `.gen.c` files is a later change, when compile times justify it.

Module initialization follows the same order: an imported module's top-level
statements run before those of the module that imported it.

## The compiler contract

This command line is the entire interface between `emeraldc` and any driver
(such as `pme`) that resolves packages on its behalf:

```
emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald
```

A driver's job reduces to computing the ordered list of `-I` roots and then
exec'ing the compiler. It never parses `.rald` source, never rewrites imports,
and never generates code.

The two earliest stages are deliberately *per-file* views and do not follow an
import: `--emit-tokens` and `--emit-ast` show you exactly one file.
`--check`, `--emit-c`, and a full build all operate on the linked program.

## Diagnostics

| code                     | meaning                                                  |
|--------------------------|----------------------------------------------------------|
| `E_IMPORT_NOT_FOUND`     | module path resolved to no file on the search path        |
| `E_IMPORT_CYCLE`         | the import graph contains a cycle; a note lists it        |
| `E_IMPORT_PRIVATE`       | the name exists but is private (leading `_`)              |
| `E_IMPORT_NAME`          | the name does not exist in that module                    |
| `E_IMPORT_AMBIGUOUS`     | two files under one root claim the same module path       |
| `E_IMPORT_MODULE_VALUE`  | a module object used as a value (only `m.<name>` is legal)|
| `E_IMPORT_REDEFINE`      | an import binding collides with a local definition or an earlier import |

Every one carries `file:line:column`, quotes the offending source line, and is
available as JSON under `--json` like any other diagnostic.

## Tests

`tests/imports/` holds one directory per case: a `main.rald` entry, whatever
modules it needs, an optional `flags` file of extra `emeraldc` arguments, and
the golden `expected` output. A case named `bad_*` is compiled with `--check`
and its diagnostics *are* the golden output; every other case is compiled and
run. `task test:imports` runs the stage on its own; `task test` includes it.
