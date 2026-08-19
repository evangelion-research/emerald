# Emerald-core: the formal fragment

This is the paper-style specification of Emerald's typed core — the syntax,
types, subtyping, and typing rules the checker implements. It exists so that
the metatheory questions in
[`research-directions.md`](research-directions.md) §10 ("write the core calculus
down") have a written answer to argue against, even before anything is
mechanized in Lean or Rocq.

**Notation.** `S <: T` reads "`S` is assignable to `T`" (equivalently, a value
of type `S` may flow where `T` is expected). `Γ ⊢ e : T` reads "under
environment `Γ`, expression `e` has type `T`". `Γ ⊢ S <: T` is subtyping under
`Γ`. Rules are written as premises over conclusions; `[R-NAME]` tags a rule so
it can be cited.

This is a *description of what is implemented*, not an aspirational spec. Where
the implementation is deliberately unsound or partial, that is stated in the
rule, not hidden.

---

## 1. Sorts

There are two sorts of things a type variable can stand for:

```
k  ::=  Type   |  Dim
```

`Dim` is the sort of dimension names (`dim Batch`) and dimension expressions
(`B * S + 1`). `Type` is everything else. A type parameter may be kinded
`T: dim` (§7); unkinded parameters are `Type`.

## 2. Types

```
T, S  ::=  int | float | str | bool | None      (base)
        |  any                                   (gradual top/bottom)
        |  never                                 (empty)
        |  c  (-c)  "s"  True  False             (literal types, c a constant)
        |  list[T]
        |  { x1: T1, ..., xn: Tn }               (record; n ≥ 0)
        |  T | S                                 (union)
        |  T & S                                 (intersection of records)
        |  (T1, ..., Tn) -> U                     (function, n ≥ 0)
        |  Tensor[dt, shape]                     (tensor)
        |  Fin[δ]                                (index below δ)
        |  α                                     (type variable, sort Type)
        |  Name[T1, ..., Tn]                     (alias application)
```

`shape` is either `?` (dynamic) or a list of dimension expressions (§4).
`dt` is a dtype name: `f32` or `f64` in this phase.

Literal types are inhabited by exactly one value. Float literals are **not**
types; negative literals are (`-1`). `None` is a base type, not a literal —
there is only one `None`.

## 3. Dimensions

```
δ, γ  ::=  d            (dimension variable, sort Dim)
        |  n            (integer literal, n ≥ 0)
        |  δ + γ
        |  δ * γ
```

Dimensions are normalized to a **sum-of-products canonical form** before
comparison: multiply out, collect terms over sorted variables, compare
term-for-term. Two expressions are equal (`δ ≡ γ`) iff their normal forms are
identical.

```
  dim_eq(δ, γ)   :=   normalize(δ) = normalize(γ)          [D-EQ]
```

`dim_le(δ, γ)` — "δ ≤ γ" — is defined on a **decidable fragment** only:

```
  dim_eq(δ, γ)                              [D-LE-REFL]
  δ ≡ n,  γ ≡ m,  n ≤ m                     [D-LE-LIT]
  γ ≡ δ + k,  k a literal,  k ≥ 0           [D-LE-PLUS]
```

Anything outside this fragment is **not decided** (it is logged, not guessed —
see [`shapes.md`](shapes.md) §"The solver"). This is why the system is a
normalizer plus a tiny partial order, not an SMT solver.

## 4. Subtyping

Subtyping is the assignability relation. `never` is the bottom; `any` is both
top and bottom (that is exactly what "gradual" means, and exactly what proof
mode bans).

```
                                [S-NEVER-L]        any <: T           [S-ANY-L]
  never <: T
  T <: any                      [S-ANY-R]
```

A literal is assignable to its base type, and `bool`→`int`→`float` widen:

```
  c <: base(c)                  [S-LIT]      bool <: int <: float     [S-NUM]
```

Records use **width subtyping** (extra fields are fine) with pointwise field
assignability:

```
  { x: Si } ⊇ { x: Ti }    ∀i. Γ ⊢ Si <: Ti
  ------------------------------------------   [S-REC-WIDTH]
  Γ ⊢ { x1: S1, ..., xm: Sm } <: { x1: T1, ..., xn: Tn }   (m ≥ n)
```

Unions and intersections follow the standard set-like rules:

```
  Γ ⊢ S <: T1 | T2   if  Γ ⊢ S <: T1  or  Γ ⊢ S <: T2       [S-UNION-R]
  Γ ⊢ T1 | T2 <: U   if  Γ ⊢ T1 <: U  and Γ ⊢ T2 <: U       [S-UNION-L]
  Γ ⊢ S <: T1 & T2   if  Γ ⊢ S <: T1  and Γ ⊢ S <: T2       [S-INTER-R]
```

`T & S` requires both sides to be records; fields merge with the right operand
winning on conflict. Intersection is not general: it is the "extends" operator.

Functions are **invariant in parameters, covariant in result**:

```
  Γ ⊢ Ti <: Si (contravariant, but parameterized types are invariant in
                practice: the checker requires Si ≡ Ti)        Γ ⊢ U <: V
  ----------------------------------------------------------------------   [S-FUNC]
  Γ ⊢ (S1, ..., Sn) -> U <: (T1, ..., Tn) -> V
```

The doc statement in [`type-system.md`](type-system.md) is the honest version:
parameters are invariant (`Si ≡ Ti`), return is covariant.

Lists are **covariant, deliberately unsound** — the TypeScript choice:

```
  Γ ⊢ S <: T
  ------------   [S-LIST]  (unsound: a list[S] can be mutated through
  list[S] <: list[T]        a list[T | U] alias to smuggle a U back)
```

Tensors: a dynamic shape (`?`) accepts any static shape and flows into any
static shape (gradual, like `any`); two static shapes must be equal by
`dim_eq`, and dtypes must match.

```
  Tensor[dt, ?] <: Tensor[dt, σ]   and   Tensor[dt, σ] <: Tensor[dt, ?]  [S-TENSOR-DYN]
  dt1 ≡ dt2    dim_eq(σ1, σ2)
  ---------------------------   [S-TENSOR]
  Tensor[dt1, σ1] <: Tensor[dt2, σ2]
```

`Fin` is the index fragment:

```
  dim_le(δ, γ)
  ------------   [S-FIN]          Fin[δ] <: int                    [S-FIN-INT]
  Fin[δ] <: Fin[γ]
```

`α` is opaque (§7): `α <: α`, and nothing else (an unconstrained `α` falls back
to `any`, which is the gradual escape).

## 5. Typing: the core rules

The environment `Γ` maps names to their **declared** type; a second, mutable map
`Δ` (the *current* types) tracks flow narrowing per variable and is threaded
through the rules. A rule may be written `Γ; Δ ⊢ e : T` when narrowing matters.

```
  Γ(x) = T
  --------   [T-VAR]            n : int      s : str      b : bool   [T-LIT]
  Γ ⊢ x : T
```

```
  Γ ⊢ e1 : S1  ...  Γ ⊢ en : Sn
  ------------------------------   [T-REC]
  Γ ⊢ { x1: e1, ..., xn: en } : { x1: S1, ..., xn: Sn }
```

Field access requires the field to exist, and on a union, on **every**
alternative:

```
  Γ ⊢ e : T    T has field x with type S   (if T is a union: ∀ alternatives)
  ----------------------------------------------------------------------  [T-FIELD]
  Γ ⊢ e.x : S
```

Assignment and declaration interact with the declared/current split:

- `x: T = e` requires `Γ ⊢ e <: T`, binds `x` at both `Γ(x)=T` and `Δ(x)=T`,
  and the annotation is enforced on every later assignment (`[T-ANNOT]`).
- `x = e` with no prior annotation **infers** `Δ(x)` from `e`; a conflicting
  later assignment *widens* to the union rather than erroring (`[T-WIDEN]`).
- `const x = e` / `const x: T = e` binds immutably; any later assignment is
  rejected (`[T-CONST]`).

Function application instantiates generics at the call site (§7) and checks
each argument:

```
  Γ ⊢ f : (T1, ..., Tn) -> U    ∀i. Γ ⊢ ei : Si    Γ ⊢ Si <: Ti
  ----------------------------------------------------------------   [T-APP]
  Γ ⊢ f(e1, ..., en) : U
```

Lambdas are anonymous functions; unannotated parameters are typed
contextually from the expected function type, else `any`:

```
  Γ, x1:T1, ..., xn:Tn ⊢ body : U
  --------------------------------------------------   [T-LAMBDA]
  Γ ⊢ (x1: T1, ..., xn: Tn) => body : (T1, ..., Tn) -> U
```

`never` is inhabited only by divergence or by exhaustive elimination:

```
  Γ ⊢ e : never        Γ ⊢ e : never
  --------------       -------------------------------   [T-NEVER]
  Γ ⊢ e : T (any T)    Γ ⊢ unreachable: never = e  ✓
```

### Flow narrowing

A condition refines `Δ` inside the branch it guards, and the refinement is
discarded when control leaves. The interesting cases:

```
  if x == c { ... }        Δ := Δ[x ↦ Δ(x) with alternative c kept]   [N-EQ]
  if x.field == c { ... }  Δ := Δ[x ↦ alternatives whose field can hold c]  [N-DISC]
  if x { ... }             Δ := Δ[x ↦ Δ(x) \ {None, 0, "", False}]    [N-TRUTHY]
  not c                    inverts N(c)
  a and b                  both N(a), N(b) hold in the then-branch
  a or b                   both ¬N(a), ¬N(b) hold in the else-branch
```

A branch that always leaves (`return`, `break`, `continue`) contributes nothing
to the join after the `if` — that is what makes guard style work. Otherwise the
paths **join** (`[N-JOIN]`): the variable's type after the `if` is the union of
its narrowed types on each surviving path, and an assignment inside a branch
replaces (invalidates) the stale narrowing.

Narrowing applies to locals, parameters, and globals; field/index narrowing
goes *through* the base variable. It does **not** cross a closure boundary: a
captured variable reads its stable declared type inside a nested function.

### Exhaustiveness

`match` and the `never`-binding idiom are the elimination rules for unions of
literal-tagged records. The checker proves the patterns cover the subject's
declared type; a match with no catch-all arm is rejected when the proof fails
(`E_TYPE_MATCH`).

### Expected errors

A **result type** is any type of the shape

```
  R(T, E)  =  { ok: True, val: T } | { ok: False, err: E }
```

recognised structurally, not by name (`Result[T, E]` in the standard library is
one spelling of it). An **error type** is a record carrying a literal
discriminant, `{ _tag: "N", ... }`, which is what `error N { ... }` declares.

`try` eliminates a result inside a function whose own return type is a result,
and the rule is the propagation obligation stated as a subtyping side condition
(`ret` is the enclosing function's declared return type):

```
  Γ ⊢ e : R(T, E)    ret = R(U, F)    Γ ⊢ E <: F
  ------------------------------------------------   [T-TRY]
  Γ ⊢ try e : T
```

When the side condition fails the escaping alternatives are reported
(`E_TYPE_ERRCHAN`); when `ret` is `any` the rule is not applied (gradualness);
when there is no enclosing `def` — at top level, or in a lambda, which has no
declared return type — `try` is rejected outright.

`catch` eliminates the error side of a result by case analysis on `_tag`. With
`E = E1 | ... | En` (each `Ei` an error type with tag `ti`) and arms
`t1 x -> b1, ..., tn x -> bn`:

```
  Γ ⊢ e : R(T, E)    E = E1|...|En    ∀i. Γ, x:Ei ⊢ bi : Si
  ------------------------------------------------------------   [T-CATCH]
  Γ ⊢ catch e { t1 x -> b1, ..., tn x -> bn } : T ⊔ S1 ⊔ ... ⊔ Sn
```

The premise ranges over **all** alternatives of `E`: that universal quantifier
is the exhaustiveness proof, and failing it is `E_TYPE_CATCH`. A catch-all arm
`_ x -> b` discharges the remaining alternatives at once, binding `x` to their
union (not to `any`), which keeps the rule total without weakening the
narrowing:

```
  covered = {Ei : some arm names ti}     rest = ⊔ (E \ covered)
  Γ ⊢ e : R(T, E)    Γ, x:rest ⊢ b : S   (arms as above for `covered`)
  -----------------------------------------------------------------   [T-CATCH-ALL]
  Γ ⊢ catch e { ..., _ x -> b } : T ⊔ S1 ⊔ ... ⊔ S
```

Both rules are ordinary elimination forms over the union machinery of §4 — no
effect row, no monadic type, and no rule that mentions control flow. The
operational reading of `try` is `return`, and of `catch` is a conditional.

## 6. The shape-obligation rules

Each shape-carrying tensor operation generates an obligation that must be
discharged by `dim_eq`/`dim_le` (else the program is rejected; there is no
implicit runtime fallback except through an explicit `?`):

```
  a : Tensor[dt, [M, K]]   b : Tensor[dt, [K2, N]]   dim_eq(K, K2)
  ----------------------------------------------------------------   [SH-MATMUL]
  matmul(a, b) : Tensor[dt, [M, N]]

  a : Tensor[dt, σ]   b : Tensor[dt, γ]   broadcastable(σ, γ)
  -------------------------------------------------------------   [SH-BINOP]
  a ⊕ b : Tensor[dt, broadcast(σ, γ)]

  t : Tensor[dt, σ]   s a shape list   dim_eq(prod(σ), prod(s))
  --------------------------------------------------------------   [SH-RESHAPE]
  reshape(t, s) : Tensor[dt, s]

  t : Tensor[dt, σ]   p a bijection on axes(σ)
  --------------------------------------------------   [SH-PERMUTE]
  permute(t, p) : Tensor[dt, σ∘p]

  t : Tensor[dt, σ]   axis a literal in range(rank(σ))
  ------------------------------------------------------   [SH-REDUCE]
  sum/mean/max/argmax(t, axis) : Tensor[dt, σ \ axis]
```

Broadcasting is **strict**: it is only applied when statically decidable;
otherwise an explicit `expand` is required. `broadcastable(σ, γ)` holds when the
ranks agree and each axis pair is equal or one is `1`. `Fin` connects to these
rules through `[SH-INDEX]`:

```
  t : Tensor[dt, [..., δ, ...]]   i : Fin[γ]   dim_le(γ, δ)
  ----------------------------------------------------------   [SH-INDEX-SAFE]
  t[i] : dt

  i : Fin[γ]   dim_le(γ, δ) does NOT hold and δ ≤ γ is provable
  ------------------------------------------------------------   [SH-INDEX-REJECT]
  t[i] is a compile error (E_SHAPE_INDEX)
```

Bounds-check *elimination* is deliberately not claimed: codegen is untyped
(D1), so the runtime check remains; `Fin` buys static rejection of
provably-bad indices only.

## 7. Generics

Type parameters are bound in brackets and instantiated per call site by
unification:

```
  def f[α1, ..., αn](x: T1, ..., xm: Tm) -> U

  At call f(e1, ..., em):
    unify Ti with the types of ei, binding α; substitute into U.
```

- Inside `f`, `α` is **opaque**: `α <: α` only. `return 5` from a `-> α`
  function is rejected. This is what makes a generic signature a universally
  quantified statement (`∀α. ...`) rather than a hint.
- Unification looks through unions; several arguments binding one variable
  take the join (union); inferred arguments are widened; an unconstrained
  variable falls back to `any` (gradual).
- Kinded parameters `α: dim` restrict `α` to sort `Dim`, so it may appear in a
  `Tensor[..., [α]]` shape or a `Fin[α]` bound. `parse_type_params` serves both
  `def f[B: dim]` and `type Vec[N: dim]`, so this is one rule for both.

## 8. Totality, purity, and proof mode

Three flags change what a type *means*:

- **Totality** (`partial` absence): every recursive call must **descend
  structurally** — an argument that is a projection chain from a parameter of
  recursive-alias type (`n.succ`, `xs.tail`), staying inside the same inductive
  structure. A non-descending recursion is `E_TYPE_TERMINATION` unless the
  function is declared `partial`. This makes `never` an honest bottom within the
  checked fragment; `while True` and mutual recursion still escape.
- **Purity** (`pure`): a pure function may call only pure functions and pure
  builtins. Enforced on *calls*, not on global state or function values — a
  promise about what the function invokes, not a full effect system.
- **Proof mode** (`--check --proof`): bans `any` (annotations, signatures, and
  inferred values) and `partial`. A clean run means every value is statically
  typed and every function terminates structurally — the minimal meaning of
  "this is a proof."

## 9. Soundness, stated honestly

The claims the checker *does* make, and where each is conditional:

| Claim | Status |
|---|---|
| Well-typed programs never hit a type-tag mismatch at runtime | Holds for the static fragment; `any` reintroduces runtime checks (gradual). |
| `never` has no values | Holds only outside divergence: `while True` and `partial` inhabit it. |
| `list[T]` means "elements are `T`" | Unsound: covariant lists admit mutation through a supertype alias. |
| Shape obligations are correct | As correct as `dim_eq`/`dim_le`; the decidable fragment is conservative (it may *reject*, it never silently *accepts* a false equality). |
| `Fin[δ]` indices are in bounds | Statically, for provably-bad cases only; the runtime check is not eliminated. |
| A function's declared errors are all it can fail with | Holds for `try` inside an annotated `def`; an `any` return type is unchecked, and a `catch` arm may still call an aborting builtin. |

None of these are secrets — they are the content of
[`research-directions.md`](research-directions.md) §1 and §6. The point of this
document is to make the *current* calculus precise enough that the next sound
fragment (`seq[T]`, induction principles, taint propagation, full termination)
is a diff against written rules rather than against `src/check.c`'s behavior.

## 10. What is deliberately absent

- **No induction principle** — recursive aliases exist, but there is no
  eliminator, so claims about inductive data are limited to parametricity.
- **No dependent product/equality types** — `Eq[a, b]` and Π-types are §6
  (Track E/F) future work; shapes are the only index arithmetic.
- **No effect rows** — `pure` is a call-level check, not a type. The error
  channel of §5 is *not* an effect row either: it is the second type argument
  of an ordinary union, visible in the signature, with `try` as its only
  introduction rule and `return` as its only semantics.
- **No bounds on type parameters** — no `T: Numeric`, no type classes. This is
  what stops the standard library from writing
  `catch_tag[T, E: { _tag: str }]`; see [`errors.md`](errors.md) §5.

See [`type-system.md`](type-system.md) §"Deliberate omissions" for the
language-level account of the same boundaries.
