# The Emerald Standard Library — Specification

**Status:** Tier 0 is **implemented**. Twelve modules (`result`, `chars`,
`math`, `builder`, `lists`, `strings`, `sort`, `dict`, `set`, `io`, `sys`,
`path`) compile and are covered by `tests/stdlib/` — `task test:stdlib`. Tier 1
(§5) is not started.

Building it changed three things in the compiler beyond the ten new builtins:
generic type parameters are now in scope inside a function body, unification
looks through unions on both sides, and a global is only updatable from the
module that declared it. The last was a linking bug the library found on its
first day — see §9.

**Shape:** Python's standard library, minus everything that assumes classes,
exceptions, iterators, or threads. The names and the decomposition are Python's
(`strings.split`, `path.join`, `json.parse`) because that is the vocabulary
Emerald's surface syntax already promises. The *signatures* are not Python's,
because Emerald has no methods and no exceptions, and pretending otherwise would
produce a library that reads like Python and behaves like nothing.

**Sequencing thesis:** Tier 0 is what a self-hosted compiler needs, and nothing
else. That constraint — not "what a standard library usually has" — is what
decided the contents. Tier 1 waits for a real program to ask. §7 stages it. The
one part still unwritten from Phase 2 is `tensor` (per `docs/SPEC_V2.md` §7
D-open), specified here only so it lands in the same namespace.

---

## 1. What the language can and cannot do today

This section is load-bearing. Every design decision below follows from it.

Verified against the current compiler:

| Fact | Consequence |
|---|---|
| `s[i]` on a `str` yields a **one-character `str`**; there is no `s[a:b]` in the grammar | A lexer can read characters but cannot take a substring without a loop |
| String concatenation `a + b` allocates a new string | Building a token's text one character at a time is **O(n²)** |
| List concatenation `xs + [v]` allocates a new list; there is **no `append`** | Building any list in a loop is **O(n²)** |
| `xs[i] = v` works and mutates in place (`em_setindex`); `xs[len(xs)] = v` is a runtime index error | In-place algorithms are fine; *growth* is not expressible |
| `r.b = 2` on a record whose type was inferred is `E_TYPE_FIELD`, **even when the binding is annotated `any`** | Records cannot serve as dicts. There is no dict |
| No exceptions, no `try` | Errors are values. Every fallible function returns `Result` |
| No iterators or generators; `range(n)` materializes a list | Everything is eager. Laziness, where it is wanted, is a thunk (`() => e`) |
| Nested generic aliases (`list[list[Entry[V]]]`) and `match` over a `Result` union both typecheck | The core data types below are expressible **today** |
| Recursive *generic* aliases are rejected | A generic linked list / tree cannot be written; use `list[T]` and records |
| Sixteen builtins, not thirteen — `map`/`filter`/`reduce` joined since `docs/builtins.md` was written | Corrected: that doc now documents fifty-two (twenty-seven core, plus the twenty-five Phase 2 tensor primitives) |
| No bitwise operators at all — `\|` and `&` are type-level only | No xor, so no FNV-1a. `dict.hash_str` is djb2 |
| `/` is float division and there is no `//` | Exact integer division is not expressible; `math.floor_div` rounds through a double |

### 1.1 The primitives that must exist first

These cannot be written in Emerald, or cannot be written at an acceptable
complexity. **The stdlib does not start until the blockers land.** Each is a
runtime function plus a `check.c` signature, in the existing builtin style.

All ten below are now **implemented** as builtins; `docs/builtins.md` documents
their behaviour. The reasoning is kept because it is the argument for why each
one had to be a builtin rather than library code.

**Blockers — nothing worth having is writable without these:**

| Primitive | Signature | Why it cannot be library code |
|---|---|---|
| `append` | `append(xs: list[T], v: T) -> None` | Amortized in-place growth. The runtime already does exactly this internally in `em_filter` (`runtime.c:969`); it is not reachable from Emerald. Without it every list-building function in this document is quadratic |
| `slice` | `slice(s: str, lo: int, hi: int) -> str` | One memcpy vs. a character-at-a-time concat loop. A lexer's inner loop |
| `ord` / `chr` | `ord(c: str) -> int`, `chr(n: int) -> str` | No arithmetic on characters otherwise, so: no hashing, no character classes, no escape decoding, no number parsing |

**Needed for self-hosting specifically:**

| Primitive | Signature | Note |
|---|---|---|
| `argv` | `argv() -> list[str]` | A compiler that cannot read its own command line is not a compiler |
| `exit` | `exit(code: int) -> never` | Must return `never` so `exit(1)` satisfies a non-`None` return path. This is a nice second inhabitant of `never` beyond `while True` |
| `float` | `float(x: any) -> float` | `int()` exists; its float counterpart does not. Emerald's own lexer must parse float literals |
| `stderr` | `eprint(...) -> None` | Diagnostics must not go to stdout, or `--emit-c` output is corrupted |

**Wanted, deferrable:**

| Primitive | Signature | Note |
|---|---|---|
| `read_line` | `read_line() -> str \| None` | `None` at EOF. Unlocks REPLs and filters; nothing in the bootstrap needs it. **Not implemented** |
| `now` | `now() -> float` | Monotonic seconds. `time` and any benchmark harness need it. **Not implemented** |
| `seed` | `seed(n: int) -> None` | The reproducibility gap `docs/builtins.md` already admits to. **Not implemented** |
| `list_set` (slice-assign) | — | Not proposed. `xs[i] = v` plus `append` covers it |

Two more were added that this section did not anticipate, both so `io` could
report failure as a value without breaking the aborting builtins every existing
program depends on: `read_file_opt(path) -> str | None` and
`file_exists(path) -> bool`. See D2.

Everything else in this document is ordinary Emerald.

### 1.2 The purity problem `append` creates

`append` mutates, so it is not a pure builtin, so any function that builds a list
with it cannot be `pure`, so `--proof` code cannot use most of this library. That
is a real cost and it is not hypothetical: `lists.map_indexed`, `strings.split`,
and every parser in `json` become impure the moment they are efficient.

Three ways out, in preference order:

1. **Local-mutation purity.** A `pure` function may `append` to a list it
   allocated itself and has not returned yet. This is the standard "safe
   escape" rule and it is checkable with the ownership information the checker
   already tracks for narrowing. Costs real work in `check.c`.
2. **Two tiers.** `append` is impure; the pure subset of the stdlib is smaller
   and explicitly marked. Honest, cheap, and leaves `--proof` weaker than it
   should be.
3. **Persistent lists.** A cons-cell module with structural sharing, pure by
   construction, slow and awkward without recursive generic aliases.

**Recommendation: (2) now, (1) when a proof in `docs/proofs.md` actually needs a
built list.** Do not do (1) speculatively. Mark every stdlib function's purity in
its signature so the migration is mechanical when it happens.

**Done: (2).** Every function that only reads is `pure` — the whole of `chars`
and `math`, the searching half of `strings` and `lists`, both parsers, and all
of `result`. Everything that builds is not. The split is visible in the
signatures, so migrating to (1) means deleting the impurity, not re-deriving it.

The line falls in a slightly surprising place worth recording: `strings.find`,
`strip`, `parse_int` and `parse_float` are pure, but `split` and `join` are not,
because they build. So proof-mode code can *inspect* strings and cannot
*produce* them.

**Purity and proof mode are separate axes, and the second is stricter than it
looks.** `--proof` bans `any` and `partial` — not impurity — but it checks the
*whole linked program*, so one `xs = []` anywhere in an imported stdlib module
takes proof mode away from every program that imports it. The library is
therefore proof-clean throughout, and `tests/proof/good_stdlib.rald` is the
regression test that keeps it that way.

Getting there needed one more checker fix: `xs: list[T] = []` now types the
empty literal from its annotation. Before, `m: list[str] = []` read back as
`list[str]` only until it was copied — `cur = m` made `cur` a `list[any]`,
because the flow type came from the literal rather than the annotation. That is
the "`[]` literals still have element type `any` inside" caveat in
`docs/type-system.md`, and it is now closed for the annotated case.

---

## 2. Conventions

Rules that hold across every module. Deviations must be justified in the module's
own section.

**Errors are values.** No function aborts on bad input except where the
equivalent builtin already does. Fallible operations return `Result[T]`;
operations that are merely absent return `Option[T]`. Both are unions with a
literal discriminant, so `match` proves you handled the failure
(`E_TYPE_MATCH`), which is the whole reason this shape is worth the friction.

```
type Result[T] = { ok: True, val: T } | { ok: False, err: str }
type Option[T] = { some: True, val: T } | { some: False }
```

**Free functions, not methods.** Python's `s.split(",")` is `strings.split(s, ",")`.
The subject is always the first parameter. There is no `self` and no way to fake
one worth having.

**Immutable by default.** A function that could return a new value does. The
mutating variants are named for it and are the exception: `lists.sorted(xs)`
returns a new list; `lists.sort_in_place(xs)` does not exist until a profile
demands it.

**Strings are bytes.** `len` counts bytes, `s[i]` is a byte, `ord` returns
0–255. There is no Unicode story and this document does not invent one — see §8.
Every function that would differ under UTF-8 says so in its docstring.

**Purity is declared.** Every function carries `pure` where it can. The stdlib is
the largest body of Emerald that will ever exist; if `pure` does not survive
contact with it, `pure` is wrong and that is worth learning here.

**Totality is declared.** Functions are total by default. Where a loop cannot be
proven to descend, `partial` is written explicitly with a one-line comment saying
why. A stdlib full of undocumented `partial` is a stdlib that has quietly opted
out of the language's main claim.

**Privacy is `_`.** Per `docs/modules.md`. Helpers are `_helper`. A module's
public surface is exactly what has no leading underscore, so the surface is
greppable.

**No cycles.** The import graph is a DAG by construction; `E_IMPORT_CYCLE` is the
compiler telling you a layering decision was wrong. The layering is: `chars` →
`strings` → everything; `result` and `math` depend on nothing.

---

## 3. Layout and resolution

```
stdlib/
  SPEC.md      this document
  result.rald  chars.rald   math.rald    builder.rald
  lists.rald   strings.rald sort.rald    dict.rald
  set.rald     io.rald      sys.rald     path.rald
tests/stdlib/
  strings_test.rald + strings_test.expected     (one pair per area)
```

Flat, one file per module, no packages until a module exceeds ~400 lines. Dotted
paths (`text.strings`) are supported by the resolver but buy nothing yet.

The dependency graph is a DAG, deepest first: `result` and `math` depend on
nothing; `chars` on the `ord`/`chr` builtins; `builder` on `append`; `lists` on
`result`; `strings` on `chars`, `builder` and `result`; `sort` on `lists`;
`dict` on `result`; `set` on `dict` and `sort`; `io` and `path` on `strings`;
`sys` on `lists`. `builder` deliberately does not import `strings`, because
`strings` builds its results in `builder`.

**Resolution.** `module.c` searches a final root after the `-I` list, taken from
`$EMERALD_STDLIB` and defaulting to a path baked in at build time, so `import
strings` works with no flags. Being last means a project can shadow a stdlib
module with its own — the mirror of the existing "first hit wins" `-I` rule.

One consequence worth knowing: the *importing file's own directory* is searched
first, so a file named `strings.rald` that does `import strings` imports itself
and gets `E_IMPORT_CYCLE`. That is why the tests are `strings_test.rald` rather
than `strings.rald`.

**Testing.** `tests/stdlib/` is a flat stage rather than the directory-per-case
shape `tests/imports/` uses, because every case is a single file: compile, run,
compare stdout+stderr against `.expected`. `task test:stdlib` runs the stage;
`task test` includes it; `task bless` regenerates the goldens. The tests import
with no `-I`, so they also prove the stdlib root resolves by default. A stdlib
function without a golden test does not exist.

---

## 4. Tier 0 — the bootstrap core

**These are the modules the self-hosted compiler needs, and nothing else.** If a
function in this tier is not called by the Emerald-in-Emerald lexer, parser,
checker, or codegen, it should not be in this tier. That constraint is the
entire value of writing the stdlib during Phase 3 instead of before it.

### `result` — errors as values

Depends on nothing. Pure throughout.

```
type Result[T] = { ok: True, val: T } | { ok: False, err: str }
type Option[T] = { some: True, val: T } | { some: False }

def ok[T](v: T) -> Result[T] pure
def err[T](msg: str) -> Result[T] pure
def some[T](v: T) -> Option[T] pure
def none[T]() -> Option[T] pure

def is_ok[T](r: Result[T]) -> bool pure       # and is_err / is_some / is_none
def unwrap_or[T](r: Result[T], d: T) -> T pure
def opt_or[T](o: Option[T], d: T) -> T pure
def why[T](r: Result[T]) -> str pure          # the message, or "" on success
def map_ok[T, U](r: Result[T], f: (T) -> U) -> Result[U]
def and_then[T, U](r: Result[T], f: (T) -> Result[U]) -> Result[U]
def map_some[T, U](o: Option[T], f: (T) -> U) -> Option[U]
def ok_or[T](o: Option[T], msg: str) -> Result[T] pure
```

No `unwrap()` that aborts. The point of the type is that the failure is in the
signature; a function that throws it away belongs at the call site, written out,
where the reader can see it.

### `chars` — character classification

Depends on the `ord`/`chr` builtins. Pure throughout. Every function takes a
one-character string; passing a longer one inspects only `s[0]`.

```
def code(c: str) -> int pure                 # ord, re-exported for symmetry
def from_code(n: int) -> str pure

def is_digit(c: str) -> bool pure
def is_hex_digit(c: str) -> bool pure
def is_alpha(c: str) -> bool pure
def is_alnum(c: str) -> bool pure
def is_space(c: str) -> bool pure            # space, tab, CR, LF, FF, VT
def is_upper(c: str) -> bool pure
def is_lower(c: str) -> bool pure
def is_ident_start(c: str) -> bool pure      # letter or _   (Emerald's own rule)
def is_ident_cont(c: str) -> bool pure       # letter, digit, or _

def to_upper(c: str) -> str pure
def to_lower(c: str) -> str pure
def digit_value(c: str) -> int pure          # '7' -> 7, 'f' -> 15, else -1
```

### `strings` — the `str` module Python spells as methods

Depends on `chars`, `builder`, `lists`.

```
# slicing and search  (`slice` itself is a builtin: it works on str and list)
def take(s: str, n: int) -> str pure
def char_at(s: str, i: int) -> str pure
def drop(s: str, n: int) -> str pure
def find(s: str, needle: str) -> int pure             # -1 if absent
def rfind(s: str, needle: str) -> int pure
def contains(s: str, needle: str) -> bool pure
def starts_with(s: str, p: str) -> bool pure
def ends_with(s: str, p: str) -> bool pure
def count(s: str, needle: str) -> int pure            # non-overlapping

# splitting and joining
def split(s: str, sep: str) -> list[str]
def split_lines(s: str) -> list[str]                  # \n, tolerates \r\n
def split_ws(s: str) -> list[str]                     # runs of whitespace
def join(sep: str, parts: list[str]) -> str
def partition(s: str, sep: str) -> { before: str, found: bool, after: str } pure

# transformation
def strip(s: str) -> str pure
def lstrip(s: str) -> str pure
def rstrip(s: str) -> str pure
def replace(s: str, old: str, new: str) -> str
def repeat(s: str, n: int) -> str
def upper(s: str) -> str
def lower(s: str) -> str
def pad_left(s: str, width: int, fill: str) -> str
def pad_right(s: str, width: int, fill: str) -> str

# parsing — Result, not a runtime abort, unlike the int() builtin
def parse_int(s: str) -> Result[int] pure
def parse_int_radix(s: str, radix: int) -> Result[int] pure
def parse_float(s: str) -> Result[float] pure

# escaping — the compiler needs both directions
def escape(s: str) -> str                # -> a valid Emerald string literal body
def unescape(s: str) -> Result[str]      # \n \t \r \0 \\ \" \'  per grammar.md
def to_chars(s: str) -> list[str]
def reverse(s: str) -> str
```

`split` on an empty separator is an error at the call site's expense — it returns
`[s]` rather than exploding into characters. Use a `for c in s` loop for that;
the language already iterates strings.

### `builder` — amortized string building

Depends on `append`, `lists`.

Without this, every function above that builds a string is quadratic. A builder
accumulates fragments in a list and pays one join at the end.

```
type Builder = { parts: list[str], count: int }

def new() -> Builder
def push(b: Builder, s: str) -> None          # mutates; the point of the type
def push_char(b: Builder, c: str) -> None
def push_int(b: Builder, n: int) -> None
def build(b: Builder) -> str
def clear(b: Builder) -> None
def size(b: Builder) -> int pure              # bytes pushed, without building
def is_empty(b: Builder) -> bool pure
def concat_all(parts: list[str]) -> str       # one-shot, no builder variable
```

This is `io.StringIO`, named for what it does. It is the one place the library
prefers mutation openly, because the alternative is the quadratic behavior this
module exists to remove.

### `lists` — the sequence operations that are not builtins

Depends on `append`. `map`/`filter`/`reduce` stay builtins and are not shadowed.

```
def push[T](xs: list[T], v: T) -> None    # `append` is a builtin, so: push
def extend[T](xs: list[T], ys: list[T]) -> None
def concat[T](xs: list[T], ys: list[T]) -> list[T]
def copy[T](xs: list[T]) -> list[T] pure
def take[T](xs: list[T], n: int) -> list[T] pure       # slice(xs, 0, n)
def drop[T](xs: list[T], n: int) -> list[T] pure
def reverse[T](xs: list[T]) -> list[T]
def chunk[T](xs: list[T], n: int) -> list[list[T]]
def unique[T](xs: list[T]) -> list[T]

def index_of[T](xs: list[T], v: T) -> int pure         # -1 if absent; uses deep ==
def contains[T](xs: list[T], v: T) -> bool pure
def all(xs: list[bool]) -> bool pure
def any(xs: list[bool]) -> bool pure
def count_if[T](xs: list[T], f: (T) -> bool) -> int

def first[T](xs: list[T]) -> Option[T] pure
def last[T](xs: list[T]) -> Option[T] pure
def get[T](xs: list[T], i: int) -> Option[T] pure      # total; no index error

def zip[A, B](xs: list[A], ys: list[B]) -> list[{ a: A, b: B }]
def enumerate[T](xs: list[T]) -> list[{ i: int, v: T }]
def flatten[T](xss: list[list[T]]) -> list[T]
def repeat_list[T](v: T, n: int) -> list[T]
def sum(xs: list[int]) -> int pure
def sum_f(xs: list[float]) -> float pure
```

`sum` and `sum_f` are separate because Emerald has no numeric-tower type
variable and `list[int]` does not fit `list[float]` under invariant unification.
This is friction worth recording rather than papering over — it is evidence for
whatever bounded-polymorphism decision Phase 4 makes.

### `sort` — ordering

Depends on `lists`. Bottom-up merge sort: stable, and it avoids the recursion
depth a total-by-default checker would make you argue about.

```
def sorted[T](xs: list[T], less: (T, T) -> bool) -> list[T]
def sorted_desc[T](xs: list[T], less: (T, T) -> bool) -> list[T]
def sorted_ints(xs: list[int]) -> list[int]         # and _floats, _strs
def less_int(a: int, b: int) -> bool pure           # and less_float, less_str
def sort_by_key[T, K](xs: list[T], key: (T) -> K, less: (K, K) -> bool) -> list[T]
def is_sorted[T](xs: list[T], less: (T, T) -> bool) -> bool
def lower_bound[T](xs: list[T], v: T, less: (T, T) -> bool) -> int
def binary_search[T](xs: list[T], v: T, less: (T, T) -> bool) -> int  # -1 if absent
```

`is_sorted` exists so `docs/proofs.md` has something to state about `sorted`.
Proving the postcondition is a genuinely good exercise for proof mode and a
better advertisement for it than a factorial.

### `dict` — the hash map the language does not have

Depends on `chars`, `lists`, `append`. **String keys only** in Tier 0: every
table in a compiler is string-keyed, and generic keys need a hashing story the
type system cannot yet express (no `T extends Hashable`).

```
type Entry[V] = { key: str, val: V }
type Map[V]   = { buckets: list[list[Entry[V]]], size: int }

def new_map[V]() -> Map[V]
def get[V](m: Map[V], k: str) -> Option[V]
def get_or[V](m: Map[V], k: str, d: V) -> V
def set[V](m: Map[V], k: str, v: V) -> None          # mutates, like Python's d[k]=v
def has[V](m: Map[V], k: str) -> bool
def remove[V](m: Map[V], k: str) -> bool             # True if it was there
def clear[V](m: Map[V]) -> None
def size[V](m: Map[V]) -> int pure
def merge[V](a: Map[V], b: Map[V]) -> Map[V]         # right wins
def bump(m: Map[int], k: str, by: int) -> None       # counters
def keys[V](m: Map[V]) -> list[str]
def values[V](m: Map[V]) -> list[V]
def items[V](m: Map[V]) -> list[Entry[V]]
def from_items[V](es: list[Entry[V]]) -> Map[V]

def hash_str(s: str) -> int pure                     # djb2 over ord(); see §8
```

Both types above were confirmed to typecheck against the current compiler,
including `Entry[V]` nested inside `Map[V]`'s `list[list[...]]`.

Iteration order is bucket order — **unspecified and not stable across
insertions**. Anything that needs determinism (and a compiler's diagnostics do)
calls `sort.sorted_strs(dict.keys(m))`. Python's insertion-order guarantee is
deliberately not copied; matching it means a second index and this library has
no profile justifying that yet.

### `set` — membership

`Set = Map[True]`, thin. `new_set`, `add`, `has`, `remove`, `size`, `elements`,
`union`, `intersect`, `difference`. Exists because the checker will want visited
sets and the alternative is `Map[bool]` written out at every call site.

### `math` — numeric helpers

Depends on nothing. Pure throughout. `examples/ray_tracer/typed/math.rald`
already implements the first half of this.

That file was **deliberately not migrated**. It would be the module's natural
acceptance test, but its functions are `min`/`max`/`abs`/`pow` where these are
`min_f`/`max_f`/`abs_f`/`pow_f`, so the swap means editing call sites across
thirteen modules of the experiment that `docs/README.md` §2 scores propositions
against — and `pow` there takes an `int` exponent, so the numerics would change
too. Churning the thesis experiment to demonstrate a library is the wrong trade.
Migrate it when the ray tracer is being touched for its own reasons.

```
const PI: float
const E: float
const INF: float

def abs_i(x: int) -> int pure
def abs_f(x: float) -> float pure
def min_i(a: int, b: int) -> int pure
def max_i(a: int, b: int) -> int pure
def min_f(a: float, b: float) -> float pure
def max_f(a: float, b: float) -> float pure
def clamp(x: float, lo: float, hi: float) -> float pure
def clamp_i(x: int, lo: int, hi: int) -> int pure
def sign_i(x: int) -> int pure                # and sign_f
def floor(x: float) -> int pure
def ceil(x: float) -> int pure
def round(x: float) -> int pure
def floor_div(a: int, b: int) -> int pure     # Python's //, which % already matches
def pow_i(base: int, e: int) -> int pure
def pow_f(base: float, e: float) -> float pure
def exp(x: float) -> float pure
def log(x: float) -> float pure               # x > 0; NEG_INF otherwise
def log2(x: float) -> float pure
def log10(x: float) -> float pure
def sin(x: float) -> float pure
def cos(x: float) -> float pure               # tan is a builtin; these are not
def radians(deg: float) -> float pure
def degrees(rad: float) -> float pure
def gcd(a: int, b: int) -> int pure
def lcm(a: int, b: int) -> int pure
```

The `_i`/`_f` suffix duplication is the same invariance problem as `sum`, and it
is ugly. It is written out rather than hidden behind `any` because `any` here
would silently discard the checking that makes this language interesting.
`exp`/`log`/`sin`/`cos` are computed in Emerald — range reduction plus a
truncated series, agreeing with the `tan` builtin and with known values to about
1e-15. If Phase 2's tensor work makes libm-backed builtins necessary anyway they
move down to the runtime and this module re-exports them; until then, a language
that cannot write its own `exp` has a gap worth knowing about.

`log` of a non-positive number returns `NEG_INF` rather than aborting. That is a
lie the caller should not catch by accident, and the honest fix — returning
`Result[float]` — poisons the ergonomics of every use. Flagged rather than
solved.

### `io` — files and streams

Wraps the existing file builtins so failure is a `Result` rather than an abort.

```
def read(path: str) -> Result[str]
def read_lines(path: str) -> Result[list[str]]
def write(path: str, content: str) -> Result[None]
def append_to(path: str, content: str) -> Result[None]   # `append` is a builtin
def write_lines(path: str, lines: list[str]) -> Result[None]
def exists(path: str) -> bool
def say(line: str) -> None                   # stdout
def warn(line: str) -> None                  # stderr, via the eprint builtin
```

`io.read` cannot actually be implemented until the runtime stops aborting inside
`read_file`. Either `read_file` gains a non-aborting sibling or it starts
returning `Result` and the builtin's current behavior moves into `io`. **Prefer
the latter**: one fallible spelling of each operation, and the builtin becomes
`_read_file_raw`. This is a breaking change to the builtin surface and should
happen in the same commit as the rest of Tier 0.

### `sys` — the process

```
def args() -> list[str]                      # argv[1:], like Python's sys.argv[1:]
def all_args() -> list[str]                  # argv, including argv[0]
def program() -> str                         # argv[0]
def arg_count() -> int
def exit_with(code: int) -> never
def die(msg: str) -> never                   # warn, then exit(1)
```

### `path` — filename manipulation

Pure string work, no syscalls; `os.path`, not `pathlib` (no classes).

```
def join(a: str, b: str) -> str pure
def dirname(p: str) -> str pure
def basename(p: str) -> str pure
def ext(p: str) -> str pure                  # ".rald", or "" if none
def without_ext(p: str) -> str pure
def is_abs(p: str) -> bool pure
def normalize(p: str) -> str                 # collapses . and .. lexically
```

`normalize` is lexical only — it does not resolve symlinks, and says so, because
the difference is where the CVEs live.

---

## 5. Tier 1 — after the bootstrap runs

Built when a real program asks for them, in roughly this order.

| Module | Python analogue | Notes |
|---|---|---|
| `json` | `json` | Parse to a `Json` union (`{kind:"obj", ...} \| {kind:"arr", ...} \| ...`), not to native records — a structural type system cannot describe an unknown object's fields. Serialization is the easy half. The parser is the best exhaustiveness-proof demo in the library |
| `fmt` | `str.format`, f-strings | `fmt.f("{} of {}", [a, b])` with positional holes. Real f-strings are a *lexer* feature and belong in the language, not here |
| `testing` | `unittest` | `assert_eq`, `assert_true`, a runner returning a nonzero exit. The golden-file harness is shell today; this makes assertions writable in Emerald |
| `random` | `random` | Seeded LCG, promoting `examples/ray_tracer/typed/rng.rald` — which already threads state explicitly and is a better design than the `rand()` builtin. `choice`, `shuffle`, `uniform`, `normal` |
| `time` | `time` | `now()`, `elapsed`, a `stopwatch` record. Needs the `now` builtin |
| `iter` | `itertools` | Thunk-based lazy sequences: `type Stream[T] = () -> Option[{ head: T, tail: Stream[T] }]`. **Blocked** — this needs recursive generic aliases, which are rejected today. Track it as a language item, not a library one |
| `argparse` | `argparse` | Flags, positionals, `--help`. The self-hosted compiler's own driver is the first user |
| `csv` | `csv` | Trivial once `strings` exists; good stdlib-shaped test material |
| `bytes` | `bytes`/`bytearray` | `list[int]` with helpers. Only if binary I/O appears |

## 6. Never

Not "later" — **not this language**, and saying so up front is cheaper than
fielding it repeatedly.

- `threading`, `asyncio`, `multiprocessing` — no concurrency model, and inventing
  one is a research track (`docs/research-directions.md`), not a library.
- `socket`, `http`, `urllib` — `run()` and `curl` cover it. A network stack is
  not on the path to a neural network in Emerald.
- `pickle`, `copy` (deep) — no object graph identity, no cycles worth preserving.
- `re` — a regex engine is a semester, and the compiler that motivates this
  library hand-writes its lexer anyway. Reconsider only if a real program asks.
- `pathlib`, `datetime`, `decimal` — all class-shaped. Their free-function cores
  are `path` and `time`; the rest is API surface for methods that cannot exist.
- `typing` — the type system is the language.

## 7. Staging

| When | What | Why then |
|---|---|---|
| **Phase 2** | `tensor` only, per `SPEC_V2.md` §7 | Dogfoods the module system against real library code before the compiler depends on it. Nothing else in this document blocks Phase 2 |
| **Done** | The §1.1 primitives, as ten builtins | Nothing above is writable first |
| **Done** | `result`, `chars`, `builder`, `strings`, `lists` | What a lexer needs |
| **Done** | `dict`, `set`, `sort`, `math` | What a parser and symbol table need |
| **Done** | `io`, `sys`, `path` | What a compiler driver needs |
| **Next (Phase 3)** | The Emerald lexer, written against these | The first real user. Delete what it never calls |
| **Phase 3, after bootstrap** | Tier 1, on demand | By construction, nothing there is speculative |

The rule that keeps this honest: **a Tier 0 function survives when the
self-hosted compiler calls it.** The modules above were written from a
prediction of what a compiler needs; the prediction has not been tested yet.
When it is, some of this will turn out to be dead weight, and the right response
is to delete it rather than defend it. See D6.

## 8. What the implementation changed

Recorded because a spec that quietly diverges from its implementation is worse
than no spec.

**Three compiler changes the library forced.** None were anticipated here:

1. **Type parameters are in scope in a function body**, not just its signature.
   `out: list[T] = []` inside `def f[T](...)` was `E_TYPE_UNKNOWN_TYPE`, which
   made a generic function unable to declare a local of its own type parameter —
   i.e. barely generic. `check.c` now carries the function's `TyEnv` on the
   checker context.
2. **Unification looks through unions on both sides.** `def map_ok[T, U](r:
   Result[T], f: (T) -> U)` bound `T = any` from a `Result[int]`, because no
   single alternative of the parameter union accepts the whole argument union.
   Function parameters are invariant, so the lambda then failed to typecheck.
3. **A global is only updatable from the module that declared it.** This was a
   linking bug, not a style issue: `lists.flatten` writes `for xs in xss`, and
   the importer's global `xs` was silently reassigned to the last element.
   Details in `docs/modules.md`. Found within an hour of the first module
   existing, which is the argument for writing the library in the language.

**Four places the implementation departs from §4:**

- `dict.hash_str` is **djb2, not FNV-1a**. FNV mixes with xor and Emerald has no
  bitwise operators. The modulo keeps the multiply under 2^32 so the int64
  arithmetic cannot overflow.
- `strings.partition` returns `{ before, found, after }`, not `sep_found`.
- `io.append` would collide with the `append` builtin (`E_TYPE_REDEFINE`), so it
  is **`io.append_to`**. Same for `lists.push` rather than `lists.append`. The
  builtin namespace is flat and shared, which is a real cost of adding ten
  builtins and worth watching as the library grows.
- `sort` is iterative merge sort rather than recursive, because a recursive one
  splitting on `len(xs) / 2` is not a structural descent and would need
  `partial` — the library would have opted out of totality on its first sort.

**One thing that worked better than expected:** `slice` covers lists as well as
strings, so `strings.slice` and `lists.slice_list` both disappeared.

## 9. Open decisions

**D1 — Unicode.** Strings are bytes. `len` counts bytes, `s[i]` and `ord` are
bytes, `chars.*` is ASCII-only, and every affected function says so. A language
aimed at ML will meet UTF-8 in a tokenizer eventually. **Decided: stay bytes**,
add a `utf8` module in Tier 1 that decodes to `list[int]` codepoints, and never
change what `len` means.

**D2 — Does `io` replace the file builtins or wrap them?** §4 argued replace.
**Decided: wrap.** `read_file` still aborts; `read_file_opt` and `file_exists`
were added beside it and `io` builds the `Result` from those. Replacing would
have broken every existing program and example for a library that had no users
yet, and the aborting spelling is genuinely the right default for a script that
cannot continue without the file.

The cost is honest and small: `io.write` and `io.append_to` return a `Result`
that is always `ok`, because the runtime has no non-aborting writer. The
signature is the one that will still be right when it does.

**D3 — Generic dict keys.** `Map[V]` is string-keyed because there is no way to
say "keys must be hashable". Unchanged, and now backed by a concrete need rather
than a guess. This is the first real argument for bounded type parameters, which
`docs/type-system.md` lists as a deliberate omission.

**D4 — Purity and `append`.** Resolved as tier (2); see §1.2 for where the line
actually fell.

**D5 — Namespace collisions.** `lists.take` and `strings.take` coexist under
module qualification, and `from lists import take` then `from strings import
take` is `E_IMPORT_REDEFINE` — the right error. Confirmed tolerable across
twelve modules. The collision that did bite was with *builtins*, not between
modules (see §8).

**D6 — new: how much of this survives self-hosting?** Tier 0 was written from
the spec's prediction of what a compiler needs, not from a compiler that failed
to build without it. That is one step better than guessing and one step worse
than the sequencing thesis in the header. When the Emerald lexer is written
against these modules, the honest thing is to record which functions it never
called — and delete them.
