# Expected Errors

Emerald has no exceptions. A function that can fail says so in its return type,
and the checker holds both sides to that claim: the callee's declared errors are
the complete list of what it can fail with, and every caller either handles them
or declares that it passes them on.

The obligations are Rust's — `Result`, explicit propagation, no invisible
unwinding. The vocabulary is [Effect's](https://www.effect.website/docs/v3/error-management/expected-errors):
errors are *expected*, tagged, tracked per-function, and recovered from with
named combinators. The syntax is Emerald's own structural, TypeScript-shaped
surface: no enum to declare, no `Ok`/`Err` constructors to wrap in, no monad to
explain.

Everything here is implemented and covered by `task test`
(`tests/e2e/errors.rald`, `tests/check/bad_try_channel.rald`,
`tests/check/bad_catch_arms.rald`). A worked program is
[`examples/errors.rald`](../examples/errors.rald).

## 1. Declaring a failure

```
error NotFound { key: str }
error Malformed { key: str, saw: str }
error Empty
```

`error N { ... }` is **sugar for a type alias** carrying a literal
discriminant. These two lines mean exactly the same thing:

```
error NotFound { key: str }
type NotFound = { _tag: "NotFound", key: str }
```

That is the whole mechanism. An error is an ordinary record, a set of errors is
an ordinary union, and `_tag` is an ordinary literal-typed field — so the
machinery that already proves a `match` exhaustive (§"Exhaustiveness proofs with
`never`" in [`type-system.md`](type-system.md)) is the machinery that proves a
`catch` exhaustive. Nothing downstream of the parser knows the keyword exists.

A payload-free error omits the braces (`error Empty`), and the desugaring still
gives it its tag: `{ _tag: "Empty" }`.

Build one by naming it in front of a record literal — the `_tag` is filled in
for you:

```
err(NotFound { key: "port" })
err(Empty {})
```

Unlike an ordinary string-literal field, a `_tag` does **not** widen to `str`
when it flows through a generic. Without that rule an error would stop matching
its own declaration the moment it passed through `err(...)`, which is the one
place every error goes.

## 2. The result type

```
type Result[T, E] = { ok: True, val: T } | { ok: False, err: E }
```

`Result` lives in [`stdlib/result.rald`](../stdlib/result.rald) and is not
built in. What the *language* recognises is the shape: any type discriminated
by a boolean-literal `ok` field, carrying `val` on the `True` side and `err` on
the `False` side. A program that grows its own result type keeps `try` and
`catch`.

`E` is usually a union of `error` declarations, and it is the interesting half:

```
def field(key: str) -> Result[str, NotFound | Malformed | Empty] { ... }
```

That signature is a closed statement about failure. Not "this may throw
something"; *these three, and nothing else.*

## 3. `try` — propagate

`try e` is the value of `e` when it succeeded, and an early `return` of the
failure when it did not.

```
def port() -> Result[int, NotFound | Malformed] {
    const raw = try field("port")     # field can fail with NotFound
    const n = try digits_of(raw)      # digits_of can fail with Malformed
    return ok(n)
}
```

The obligation is on the enclosing function: its declared error type must be
able to carry everything the `try` can fail with. Drop `Malformed` from that
signature and the checker names the error that would escape:

```
error[E_TYPE_ERRCHAN]: unhandled error: 'try' can fail with
  {_tag: "Malformed", key: str, saw: str}, which this function does not
  declare (it fails with {_tag: "NotFound", key: str})
```

Details worth knowing:

- **Precedence.** `try` binds tighter than every binary operator, so
  `try f(x) |> g` pipes the *unwrapped* value into `g`.
- **Gradualness.** A function whose return type is unannotated is `any`, and an
  `any` channel is unchecked — the same bargain the rest of the type system
  makes.
- **Lambdas are rejected.** A lambda has no declared return type, so there is
  no channel to check against, and silently returning from the lambda would
  give it a type that lies. `try` inside one is `E_TYPE_TRY`; use a nested
  `def` with a declared result type, or handle the failure on the spot.
- **Nested `def`s work normally.** The channel checked is the innermost
  enclosing `def`'s.

## 4. `catch` — handle, exhaustively

`catch` is an **expression**. Its value is the subject's success value, or the
value of the arm that matched:

```
const n = catch port() {
    NotFound e -> 80
    Malformed e -> 0 - 1
}
```

Rules:

- Every error the subject can produce must be named by an arm, or by a single
  catch-all `_`. A missing arm is `E_TYPE_CATCH` — the same class of error as a
  non-exhaustive `match`, reported at compile time.
- Naming an error the subject *cannot* produce is also `E_TYPE_CATCH`, as is a
  second arm for an error already handled. Dead handlers are usually a rename
  that went half-finished.
- An arm may bind the error to read its fields (`NotFound e -> e.key`). The
  binding is scoped to its own arm and is `const`.
- The catch-all binds whatever the named arms did not take, narrowed to exactly
  those alternatives:

  ```
  catch parse(s) {
      ParseError e -> e.line
      _ rest -> 0 - 1        # `rest` is the remaining errors, not `any`
  }
  ```

- The type of the whole expression is the join of the success type and the
  arms' types. Arms that all produce the success type give you exactly that
  type back; arms that produce something else give you a union, and the checker
  will say so at the binding.
- Arm bodies are **single expressions**, like lambda bodies. A `{` there opens
  a record literal, not a block; for multi-statement handling, call a `def`.

`catch` on a value of type `any` requires a catch-all: nothing is known about
what it can fail with, so nothing else could cover it.

## 5. The combinators

[`stdlib/result.rald`](../stdlib/result.rald) carries the function-value
equivalents, for the cases where a value reads better than an expression —
mirroring Effect's expected-error API. The full signature list is in
[`stdlib/SPEC.md`](../stdlib/SPEC.md#result--errors-as-values).

| Effect | Emerald | Notes |
|---|---|---|
| `Effect.fail` | `err(e)` | |
| `Effect.succeed` | `ok(v)` | |
| `mapError` | `map_error(r, f)` | rewrite the error channel |
| `catchAll` | `catch_all(r, f)` | recover to a value; cannot fail afterwards |
| `catchAll` (fallible) | `catch_then(r, f)` | recover into another result |
| `catchTag` | `catch_if(r, pred, f)` / `catch_tag(r, tag, f)` | see the caveat below |
| `orElse` | `or_else(r, thunk)` | |
| `retry` | `retry(thunk, attempts)` | |
| `tapError` | `tap_error(r, f)` | run for effect, result unchanged |
| `either` | `either(r)` | move the failure into the value channel |
| `option` | `option(r)` | discard the error, keep an `Option` |
| `Effect.all` | `all(rs)` | first failure wins, short-circuits |

`either` deserves a note, because it is what lets a function that declares *no*
errors still inspect one:

```
def inspect(s: str) -> Result[str, never] {
    const outcome = try either(parse(s))     # cannot fail: `never` channel
    if outcome.ok == True { return ok("ok " + str(outcome.val)) }
    return ok("failed with " + tag_of(outcome.err))
}
```

### The `catch_tag` caveat

Emerald's generics are **unbounded**: a `[E]` could be an `int`, so nothing may
read `_tag` off it. That leaves two shapes, and the library ships both:

- `catch_if(r, pred, f)` takes a predicate and keeps `E` exact. Pair it with
  `tag_of` when you want tag matching without losing the channel:
  `catch_if(load(p), (e) => tag_of(e) == "NotFound", (e) => "")`.
- `catch_tag(r, tag, f)` matches the tag itself, and therefore has to name the
  error type structurally — which widens the result's error channel to
  `{ _tag: str }`, so it no longer flows back into a declared union.

When the narrowing has to be *proved*, use the `catch` expression. That is the
form the checker reasons about; the combinators are runtime dispatch.

## 6. What this compiles to

Nothing exotic, which is the point:

- `try e` → evaluate `e`, test its `ok` field, and on failure `return` the
  result value **unchanged** (it is already what this function must return, so
  no error is rebuilt and nothing is copied).
- `catch e { ... }` → evaluate `e`, branch on `ok`, and on the failure side run
  an if/else chain comparing `_tag`. The checker has proved the chain total, so
  the final `else` exists only to keep the generated C total.

There is no unwinding, no handler search, no setjmp, no allocation on the error
path beyond the error record itself, and no hidden control flow. `try` is a
comparison and a `return`; the cost is what it looks like.

Both forms are expressions in the codegen's slot model, so they compose
anywhere an expression goes — including inside loops, `match` arms, and other
`catch` arms. A `catch` binding is an ordinary local, so a closure may capture
it and it is boxed like any other captured local.

## 7. Across module boundaries

An error's `_tag` is its **source-level** name and is never rewritten by the
module linker, even though the type alias itself is (`errs.NotFound` becomes
`errs__NotFound`). So an error declared in one module is caught by its written
name in another:

```
from errs import NotFound
...
catch load(p) { NotFound e -> "" ... }
```

The consequence to know: two modules that both declare `error NotFound`
produce values the runtime cannot tell apart. Errors are structural, like
everything else in Emerald. If that matters, put the distinction in the name.

## 8. What is deliberately absent

- **No exceptions, and no `throw`.** A failure is a value; the only control
  flow is `return`.
- **No `unwrap()` that aborts.** A caller that wants to give up on a failure
  writes that out — `unwrap_or(r, d)`, or a `catch` arm that calls `exit` —
  where a reader can see it.
- **No error channel in the signature *syntax*.** `-> Result[T, E]` names both
  channels explicitly rather than hiding one behind a `!` or an effect row.
  The wrapper is visible because the wrapper is real.
- **No stack traces.** An error carries the fields you gave it. Runtime
  location reporting is a separate mechanism (`rt_cur_file`/`rt_cur_line`, see
  [`diagnostics.md`](diagnostics.md#runtime-errors)).
- **No automatic conversion** between error types on propagation (Rust's
  `From`). `try` requires the caller's channel to already carry the callee's
  errors; widen it, or convert explicitly with `map_error`.

## 9. Diagnostics

| code | meaning |
|---|---|
| `E_TYPE_TRY` | `try` on a non-result, outside a function, or inside a lambda |
| `E_TYPE_ERRCHAN` | `try` propagates an error the enclosing function does not declare |
| `E_TYPE_CATCH` | `catch` not exhaustive, on a non-result, or naming an error that cannot occur |

Each carries structured `expected`/`actual` types under `--json`, so the error
union that would escape is machine-readable. See
[`diagnostics.md`](diagnostics.md).
