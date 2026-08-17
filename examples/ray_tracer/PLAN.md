# A Type-Theoretic Ray Tracer in Emerald

*Plan for re-implementing [Ray Tracing in One Weekend](https://raytracing.github.io/books/RayTracingInOneWeekend.html)
as a typed, modular, proof-carrying Emerald program.*

The existing [`one_weekend.rald`](one_weekend.rald) is a **transliteration**: one
280-line file, closures for materials, records for everything else, types used
as documentation. It renders the book's final image and it stresses the GC,
which is what it was built for.

This plan describes a different program with the same output. The goal is not a
prettier ray tracer — it is to find out *how much of a real graphics program
Emerald's type system can actually hold*, to state each of the book's implicit
invariants as a proposition, and to record precisely which ones the checker
accepts, which ones degrade to runtime assertions, and which ones the language
cannot express at all. The last category is the deliverable: it is a
requirements list for [`docs/research-directions.md`](../../docs/research-directions.md)
written by a program that wants the features, rather than by a wishlist.

---

## 0. How to read this document

- **§1–§3** — the type-theoretic reading of the book, and an honest inventory of
  what Emerald can encode today.
- **§4–§7** — the design: module graph, core types, the encodings chosen for
  hittables and materials, and the proof obligations each one carries.
- **§8** — milestones mapped onto the book's 13 chapters, each with an exit
  criterion.
- **§9** — testing, determinism, and performance.
- **§10** — the language extensions this exercise motivates, ranked, each with
  "what it buys the ray tracer" and a cost estimate.
- **§11** — non-goals and known dead ends.

Everything in §4–§8 is written against the language **as implemented today**
(see [`docs/grammar.md`](../../docs/grammar.md),
[`docs/type-system.md`](../../docs/type-system.md)). Anything requiring a new
feature is confined to §10 and marked as such.

---

## 1. What the book actually builds

The book is 13 chapters, but structurally it is five things:

| Layer | Book chapters | The mathematical object |
|---|---|---|
| Linear algebra | 2–3 | a 3-dimensional real inner-product space, plus an affine space over it |
| Geometry / intersection | 4–6 | a partial function `Ray × Interval ⇀ Hit` |
| Sampling & shading | 7–11 | a Markov chain on rays; `scatter` is its transition kernel |
| Camera | 11–12 | a map from the unit square (film) into the space of rays |
| Scene & driver | 13 | a finite sum of primitives, and a fold over pixels |

Each layer has invariants the C++ leaves to convention. Enumerating them is the
whole exercise:

1. `unit_vector(v)` returns a vector of length 1. Nothing in C++ says so; several
   later functions (`reflect`, `refract`, `cos_theta`) are **only correct** on
   unit input.
2. Points and directions are both `vec3`, but `point + point` is meaningless and
   `point - point` is a direction. This is an affine space, typed as a vector
   space.
3. `hit_record.normal` is unit-length **and** oriented against the incident ray
   (`front_face` bookkeeping). Two coupled invariants on one field.
4. `t_min < t_max` on every interval, and a returned `t` lies inside it.
5. Colors are in `[0,1]` before gamma, `[0,255]` integers after. The conversion
   is total only because of a clamp that the book adds later — a bug the type
   system should have caught earlier.
6. `scatter` returns a new ray *and* an attenuation, or nothing; the caller must
   handle "nothing" or it loses energy conservation.
7. Recursion in `ray_color` terminates because `depth` strictly decreases.
8. The world is a heterogeneous collection dispatched dynamically — the one place
   C++ reaches for virtual functions.

A "type-theoretic implementation" means: for each of 1–8, either state it as an
Emerald type and let `emeraldc --check` discharge it, or state precisely why it
cannot be stated and what the fallback is.

---

## 2. The dictionary

Reading the ray tracer through [`docs/proofs.md`](../../docs/proofs.md)'s
Curry–Howard table:

| Graphics concept | Type-theoretic reading | Emerald encoding |
|---|---|---|
| `hit()` returning `bool` + out-param | partial function | `-> Hit \| None`, narrowed at the call site |
| virtual `hittable::hit` | existential type `∃S. {state: S, hit: S → …}` | either a record of closures (existential) or a tagged sum (finite universe) — §5.3 |
| `material` | a coalgebra: state → observation + next state | record of closures, or tagged sum + total `scatter` |
| the world | finite sum type `Sphere + Sphere + …` | `list[Prim]` with `Prim` a discriminated union |
| BVH | inductive type (binary tree) | recursive alias, with `never`-checked case analysis |
| `depth` recursion bound | fuel / structural descent | `int` today; `Nat` recursive alias in §10.6 |
| unit vector | a refinement `{v : Vec3 \| ‖v‖ = 1}` | **not expressible** — brand + assert, §5.1 |
| point vs. direction | affine space over a vector space | discriminant-tagged records, §5.1 |
| RGB channel | finite domain | `"r" \| "g" \| "b"` literal union |
| BVH split axis | `Fin 3` | `0 \| 1 \| 2` literal union — a genuine dependent-type win, at 3 elements |
| "the renderer is a function of the seed" | purity | **not expressible** — §10.2 |

Two entries are worth dwelling on because they are where the interesting design
choice lives: the existential encoding of `hittable`, and the impossibility of
refining a float.

---

## 3. Feature inventory: what the checker gives us today

Usable, load-bearing:

- **Structural records + width subtyping** — `Hit`-like records compose by
  intersection (`&`) instead of inheritance.
- **Discriminated unions + flow narrowing** — `if p.kind == "sphere"` refines `p`.
  This is the dispatch mechanism.
- **`never` exhaustiveness** — every case analysis over primitives, materials, or
  BVH nodes gets a proof obligation that reopens when the union grows.
- **Literal-type enumerations** — axes, channels, PPM tags. Note the constraint:
  **integer and string literals only; float literals are not valid types**, which
  is exactly why §5.1 hurts.
- **Generics with opaque type variables** — enough for a genuinely parametric
  sampler and a `Maybe`-shaped combinator library, and each generic signature is
  a universally quantified claim (parametricity).
- **Recursive non-generic aliases** — `type Bvh = Leaf | { kind: "node", left: Bvh, right: Bvh, box: Aabb }` is legal. Recursive *generic* aliases are not.
- **Closures with capture** — materials-as-closures, and the existential encoding.
- **Modules with underscore privacy** — file-level decomposition, `from m import T`
  for types (there is no `m.T` type syntax).

Hard constraints to design around:

- No classes, no methods, no operator overloading → every vector operation is a
  named function call, and expressions get long. Mitigate with a small set of
  fused helpers (`vmuladd`, `vlerp`) rather than deep nesting.
- No float literal types, **no bounded quantification, no refinement types** → the
  unit-vector and `[0,1]`-color invariants cannot be types.
- **No nominal or opaque types.** Structural typing means a "brand" field is
  advisory: any module can write the brand. Privacy is per-*name*, not per-type,
  so there is no abstract type. This is the single biggest gap this program hits.
- **No termination checker** → `never` is inhabited by divergence; proofs are
  claims about well-formed data, not total functions.
- **Covariant `list[T]`** is unsound by design; scene lists are mutated, so treat
  every `list` claim as ergonomic rather than proof-grade.
- Builtins are `print len range str int gc_stats read_file write_file append_file run sqrt tan rand` — **no `abs`, `min`, `max`, `pow`, `sin`, `cos`, `floor`, no `float()`**. A `math.rald` module supplying these in Emerald source is a prerequisite, not a nicety.
- No exceptions → all failure is `T | None` or `T | Err` and must be narrowed.
- Comparison chains associate pairwise, so `tmin < t < tmax` is a bug; write it
  as `t > tmin and t < tmax`. Worth a lint (§10.9).

---

## 4. Module graph

```
examples/ray_tracer/typed/
  math.rald        # min/max/abs/clamp/pow/degrees_to_radians — no deps
  rng.rald         # seeded LCG, explicit state threading  <- math
  vec.rald         # Vec3, Point, Dir, Unit; ops           <- math, rng
  interval.rald    # Interval, smart constructor, contains  <- math
  ray.rald         # Ray, ray_at                            <- vec
  aabb.rald        # Aabb, hit_slab, surrounding_box         <- vec, interval, ray
  color.rald       # Color, Rgb8, gamma, clamp, ppm encode  <- vec, math, interval
  material.rald    # Material union, scatter (total)        <- vec, ray, rng, color
  hittable.rald    # Hit, Prim union, hit_prim, hit_world   <- vec, ray, interval, material, aabb
  bvh.rald         # Bvh inductive type, build, hit         <- hittable, aabb, interval
  camera.rald      # Camera, get_ray                        <- vec, ray, rng, math
  render.rald      # ray_color, render loop, PPM writer     <- everything
  scene.rald       # the book's final scene, as data        <- hittable, material, vec, rng
  main.rald        # entry point, config                    <- render, scene, camera
```

Rules the graph obeys:

- **Acyclic and layered.** `E_IMPORT_CYCLE` is a real error here — `material`
  needs `Hit`-ish data and `hittable` needs `Material`. Resolved by *not* putting
  `Hit` in `hittable`: `material.rald` defines the narrow `Contact` record
  (`p`, `normal`, `front`) it needs, and `hittable.rald` defines
  `type Hit = Contact & { t: float, mat: Material }`. Intersection types make the
  layering work where C++ uses a forward declaration. This is the first place the
  type system pays for itself.
- **Types cross with `from m import T`**, never `m.T` (not valid syntax).
- **Everything private is `_`-prefixed** — notably `_lcg_next` in `rng`, so the
  raw generator cannot be called without threading state.

---

## 5. Core type designs

### 5.1 Vectors: the affine distinction, and the unit-vector problem

The one invariant everything downstream depends on is "this vector has length 1".
Emerald cannot say that: refinements over floats do not exist, and float literals
are not types. Three options, in increasing order of honesty:

**Option A — no distinction** (what `one_weekend.rald` does). `Vec3` everywhere.
Zero cost, zero protection.

**Option B — brand fields.** A phantom discriminant:

```
type Vec3 = { x: float, y: float, z: float }
type Unit = Vec3 & { tag: "unit" }
type Dir  = Vec3 & { tag: "dir"  }
type Pt   = Vec3 & { tag: "pt"   }

def unit(v: Vec3) -> Unit { ... }                # the only intended producer
def reflect(v: Unit, n: Unit) -> Dir { ... }     # signature states the precondition
```

The checker then rejects `reflect(r.dir, n)` where `r.dir: Dir` — which is a real
class of bug (the book's `reflect` is wrong on non-unit input, and the C++ makes
you remember). What it does **not** do is prevent someone writing
`{ x: 3.0, y: 0.0, z: 0.0, tag: "unit" }` by hand: structural typing means the
brand is a convention the checker helps you keep, not an abstraction barrier.

**Option C — brands + a checked boundary.** Option B, plus `unit()` asserting
`‖v‖ ≈ 1` at runtime in debug builds. Static where possible, dynamic at the
boundary, with the boundary *named* — precisely the gradual-verification story
in research-directions §5.

**Decision: Option C**, and the residual gap is logged as the headline motivation
for opaque types (§10.1) and scalar refinements (§10.3).

The affine distinction (`Pt` vs `Dir`) is cheaper and stronger, because it needs
no runtime check at all — it is pure bookkeeping the checker can do:

```
def padd(p: Pt, d: Dir) -> Pt      # point + direction = point
def psub(a: Pt, b: Pt)  -> Dir     # point - point     = direction
def dadd(a: Dir, b: Dir) -> Dir    # direction + direction = direction
#   there is deliberately no `padd(Pt, Pt)`
```

Cost: brands add a field to every allocated vector (GC pressure — measure it,
§9.3), and every literal construction site must name the brand. Mitigate with
constructors `pt(x,y,z)`, `dir(x,y,z)`, and never write a vector literal.

**Open question for the milestone-1 write-up:** whether the brand-field cost is
worth it, measured, not guessed. Run the identical scene under Option A and
Option C and report allocation counts from `gc_stats()`.

### 5.2 Interval: the smart-constructor pattern

```
type Interval = { lo: float, hi: float }         # invariant: lo <= hi

def interval(lo: float, hi: float) -> Interval | None { ... }   # total, checked
def _unchecked(lo: float, hi: float) -> Interval { ... }        # module-private
def surrounds(i: Interval, t: float) -> bool { return t > i.lo and t < i.hi }
```

`interval()` returning `Interval | None` forces every caller to narrow, which is
the honest cost of not having refinements: the proposition `lo ≤ hi` becomes a
value-level obligation, propagated by the option type. Callers that know better
use the private constructor — inside the module only, which is exactly what
underscore privacy gives us.

Note the pairwise-comparison trap: `surrounds` must be written as two
comparisons joined with `and`, never `i.lo < t < i.hi`.

### 5.3 Hittables: existential object vs. tagged sum

This is the central design decision, and the book's `virtual` keyword is the
question in disguise.

**Encoding 1 — existential (record of closures).** What `one_weekend.rald` does
for materials:

```
type Hittable = { hit: (Ray, Interval) -> Hit | None, bbox: () -> Aabb }
```

This is `∃S. {state: S, hit: S × Ray × Interval → Hit?}` in Emerald's closure
form: the state is captured in the closure environment and is genuinely hidden —
closure capture *is* the abstraction barrier that structural records don't give
us (§5.1). Open to extension: a new primitive is a new function, no existing file
changes.

**Encoding 2 — tagged sum + total dispatch.**

```
type Sphere      = { kind: "sphere", center: Pt, radius: float, mat: Material }
type MovingSphere= { kind: "moving", c0: Pt, c1: Pt, radius: float, mat: Material }
type Prim        = Sphere | MovingSphere

def hit_prim(p: Prim, r: Ray, t: Interval) -> Hit | None {
    if p.kind == "sphere" { return _hit_sphere(p, r, t) }
    if p.kind == "moving" { return _hit_moving(p, r, t) }
    impossible: never = p            # exhaustiveness proof
    return None
}
```

Closed to extension, open to new *operations* — and every operation gets a
`never` obligation that reopens the moment `Prim` grows. This is the expression
problem, and Emerald makes both horns available.

**Decision: Encoding 2 for primitives, Encoding 1 for materials.**

Rationale: the book's primitive set is finite and known (sphere, and in Book 2,
quads/boxes/media), and the value of `never` reopening every dispatch site when a
primitive is added is exactly what we want to demonstrate — it is the type system
doing project management. Materials, by contrast, are where users extend, and
their state is heterogeneous (albedo / fuzz / IOR), so existential closure
capture is the better fit *and* it gives us a working comparison of both
encodings inside one program. §8 milestone 9 writes up the diff.

A third encoding — **BVH as an inductive type** — appears in `bvh.rald`:

```
type Leaf = { kind: "leaf", prims: list[Prim], box: Aabb }
type Node = { kind: "node", left: Bvh, right: Bvh, box: Aabb }
type Bvh  = Leaf | Node                       # recursive alias, legal today
```

Recursion here is structural, so it is the natural test case for the induction
principles and termination checking of research-directions §6. Today, `hit_bvh`'s
recursion is unchecked; we note the obligation and move on.

### 5.4 Materials as a coalgebra

```
type Contact = { p: Pt, normal: Unit, front: bool }
type Scatter = { atten: Color, ray: Ray }
type Material = { scatter: (Ray, Contact) -> Scatter | None }
```

Two deliberate changes from `one_weekend.rald`:

- The four positional arguments `(Ray, Vec3, Vec3, bool)` collapse into one
  `Contact` record. Positional `Vec3, Vec3` is exactly the swap bug the brands
  in §5.1 exist to prevent, and naming them removes the possibility entirely.
- `Scatter` becomes `Scatter | None` instead of a record with an `ok: bool` field.
  `{ok: False, atten: ..., ray: ...}` requires constructing an attenuation and a
  ray that mean nothing — the type lies. `None` plus narrowing is both smaller and
  truthful, and the caller cannot read `atten` without narrowing first.

Both changes are the same principle: **make the illegal states unrepresentable**,
which is the only form of "correct by construction" this type system supports.

### 5.5 Color, and the clamp the book forgets

```
type Color = { r: float, g: float, b: float }     # nominally [0,1], unenforced
type Rgb8  = { r: int, g: int, b: int }           # nominally [0,255]
type Chan  = "r" | "g" | "b"

def to_rgb8(c: Color, samples: int) -> Rgb8       # gamma + clamp; total by clamp
```

`to_rgb8` is total *because* of the clamp, and that is worth writing down: the
book adds `clamp` in chapter 8 as a fix for over-bright pixels; here it is the
only way to give the conversion a total type. `Chan` as a literal union is a
`Fin 3` and makes a channel-generic fold well-typed — small, but it is a real
dependent-type-shaped win at n=3.

### 5.6 Randomness: explicit state, because there are no effects

The book calls a global `random_double()`. Emerald has a `rand()` builtin, and
`one_weekend.rald` uses it — which makes the renderer non-reproducible and makes
"the image is a function of the seed" unstatable.

The typed version threads a seed explicitly:

```
type Rng = { state: int }
type Draw[T] = { value: T, rng: Rng }             # generic alias, legal today

def next_float(g: Rng) -> Draw[float]
def unit_sphere(g: Rng) -> Draw[Dir]
```

This is the state monad, unrolled by hand because Emerald has neither `do`
notation nor higher-kinded types. It is verbose — every sampling call site gains
a binding and a rebind of `g` — and that verbosity is the point: it is a measured
argument for the effect system in research-directions §3. **Milestone 6 records
the line-count delta between threaded `Rng` and builtin `rand()`, as the concrete
cost of not having `!{Rand}`.**

Determinism also buys the golden-image tests in §9.

---

## 6. The proof obligations, and their fate

The table this whole exercise exists to produce. Fill in the "result" column as
milestones land.

| # | Proposition | Status today | Mechanism / fallback |
|---|---|---|---|
| 1 | Primitive dispatch is exhaustive | **provable** | `impossible: never = p` |
| 2 | Material dispatch is exhaustive | **provable** (sum form) | same; N/A in closure form |
| 3 | BVH case analysis is exhaustive | **provable** | `never` over `Leaf \| Node` |
| 4 | Split axis ∈ {0,1,2} | **provable** | `type Axis = 0 \| 1 \| 2` |
| 5 | Channel ∈ {r,g,b} | **provable** | literal union |
| 6 | A miss cannot be read as a hit | **provable** | `Hit \| None` + narrowing |
| 7 | `scatter` failure carries no fake data | **provable** | `Scatter \| None` |
| 8 | Point/direction confusion | **provable** | affine brands, §5.1 |
| 9 | `reflect`/`refract` get unit input | **partly** | `Unit` brand, forgeable |
| 10 | `‖unit(v)‖ = 1` | **not statable** | runtime assert at the boundary |
| 11 | `lo ≤ hi` on intervals | **not statable** | smart constructor → `\| None` |
| 12 | Color ∈ [0,1] before gamma | **not statable** | clamp makes conversion total |
| 13 | `ray_color` terminates | **not statable** | depth arg; no termination checker |
| 14 | Render is a function of the seed | **not statable** | explicit `Rng`; no purity |
| 15 | Image buffer indices in bounds | **not statable** | `Fin[n]`, research-directions §2 |
| 16 | Scene list is not aliased/mutated | **not statable** | covariant lists are unsound |

Six provable, three partial, seven out of reach. That ratio *is* the finding, and
it is a more useful contribution than the image.

---

## 7. What the typed API looks like

Sketch of the signature surface, as the spec to implement against:

```
# vec.rald
def pt(x: float, y: float, z: float) -> Pt
def dir(x: float, y: float, z: float) -> Dir
def unit(v: Dir) -> Unit
def dot(a: Dir, b: Dir) -> float
def cross(a: Dir, b: Dir) -> Dir
def scale(a: Dir, t: float) -> Dir
def padd(p: Pt, d: Dir) -> Pt
def psub(a: Pt, b: Pt) -> Dir
def near_zero(a: Dir) -> bool
def reflect(v: Unit, n: Unit) -> Dir
def refract(uv: Unit, n: Unit, eta_ratio: float) -> Dir

# interval.rald
def interval(lo: float, hi: float) -> Interval | None
def surrounds(i: Interval, t: float) -> bool
def clamp_to(i: Interval, t: float) -> float

# hittable.rald
def hit_prim(p: Prim, r: Ray, t: Interval) -> Hit | None
def hit_world(w: list[Prim], r: Ray, t: Interval) -> Hit | None

# material.rald
def lambertian(a: Color) -> Material
def metal(a: Color, fuzz: float) -> Material
def dielectric(ir: float) -> Material

# render.rald
def ray_color(r: Ray, w: Bvh, depth: int, g: Rng) -> Draw[Color]
def render(cam: Camera, w: Bvh, cfg: Config, g: Rng) -> str
```

Note `ray_color` returns `Draw[Color]` — the seed comes back out. The generic
`Draw[T]` alias carrying it is the one place a generic earns its keep in this
program, and it is a proof by parametricity that the sampler cannot inspect the
value it draws.

---

## 8. Milestones

Each milestone is a commit, a test, and a paragraph in the write-up. The "typing
content" column is what the milestone is *for* — the image is a side effect.

| # | Book ch. | Deliverable | Typing content | Exit criterion |
|---|---|---|---|---|
| 0 | — | `math.rald`, `rng.rald` | seeded LCG; `Draw[T]` generic | two runs, same seed, byte-identical output |
| 1 | 2–4 | `vec.rald`, `ray.rald`, PPM gradient | affine brands; A-vs-C cost measurement | `padd(p, p)` is a compile error; brand overhead measured |
| 2 | 5–6 | sphere hit, `Hit \| None` | option-typed partiality; `Contact` split | no `bool` + out-param anywhere |
| 3 | 6 | `interval.rald`, world list | smart constructor; `\| None` propagation | `interval(1.0, 0.0)` cannot be used unnarrowed |
| 4 | 7 | antialiasing, `color.rald` | `Chan` literal union; total `to_rgb8` | clamp proven necessary by the type, not by a bug report |
| 5 | 8–9 | diffuse + `Prim` union | first `never` obligation | adding a primitive breaks the build in exactly N named places |
| 6 | 10 | metal, `Material` closures | existential encoding; **Rng verbosity measured** | line-count delta vs. `rand()` recorded |
| 7 | 11 | dielectrics | `Unit` precondition on `refract` | passing a non-unit `Dir` is a compile error |
| 8 | 12 | positionable camera + defocus | `Config` record; no magic numbers | camera params typed, not positional floats |
| 9 | 13 | final scene, `scene.rald` | encoding comparison write-up | image matches `out.ppm` reference within tolerance |
| 10 | (Book 2) | `aabb.rald`, `bvh.rald` | inductive type + `never`; recursion obligation | BVH build/hit passes; obligation 3 discharged |
| 11 | — | `PLAN.md` results section | §6 table filled in | every row has a result and a pointer |

Milestones 0–9 are the book. 10 exists because the BVH is the only genuinely
inductive structure in the program and therefore the best test case for future
induction/termination work. 11 is the actual output.

---

## 9. Testing, determinism, performance

### 9.1 Golden images
With a seeded `Rng`, a render is deterministic, so a golden PPM is a valid test.
Store small ones (`64×36`, 4 spp, depth 8) under `tests/raytracer/` in the
existing golden-file style used by `tests/imports/`, and compare byte-for-byte.
The full 1200px scene stays out of CI; it is a manual `task render`.

Cross-milestone comparison needs care: milestones 4, 6, 7 legitimately change the
image. Each milestone gets its own golden file rather than a shared one.

### 9.2 Type-level tests
As valuable as the images, and cheaper. Mirror `tests/imports/`'s `expected`-file
convention with negative cases that **must fail to compile**, each with the exact
diagnostic:

- `bad_point_plus_point.rald` — affine violation
- `bad_nonunit_reflect.rald` — `Dir` where `Unit` required
- `bad_missing_prim_case.rald` — `Prim` grown by one, `never` binding fails
- `bad_unnarrowed_hit.rald` — reading `.t` off a `Hit | None`
- `bad_unchecked_interval.rald` — using `interval()`'s result without narrowing

These are the demonstration that the type system does something, and they are
regression tests on the *checker*, not just on the ray tracer.

### 9.3 Performance and GC
Reuse [`stress.sh`](stress.sh). Two numbers matter and both are new information
about the language:

1. **Brand-field overhead** — allocation count and wall time, Option A vs Option C
   (§5.1). Every `Vec3` gaining a `tag: str` field is a real per-allocation cost
   in a program that allocates tens of millions of vectors.
2. **Existential vs. sum dispatch** — closure call through a record field vs.
   a narrowed tagged branch, measured on the same scene (materials can be built
   both ways for one milestone-9 experiment).

Report both from `gc_stats()` plus wall clock, at fixed seed and sample count.

---

## 10. Language extensions this program motivates

Ranked by how much of §6's "not statable" column they close. Each ties back to a
track in [`docs/research-directions.md`](../../docs/research-directions.md).

**10.1 Opaque / nominal types with module-level abstraction** *(closes #9, #10, #11 partly)*
— research-directions §7, "module-level abstraction". The single highest-value
addition *for this program*. Something like:

```
opaque type Unit = Vec3         # outside this module, Unit is not a Vec3 literal
```

so `unit()` is the only constructor and the brand stops being forgeable.
Structural typing has no abstraction barrier today, and closures are currently
the only way to get one. Modest cost: a nominal-identity flag on aliases plus an
export rule. **This is the recommendation to act on first.**

**10.2 Effects and purity** *(closes #14)* — research-directions §3. `!{Rand}`
would delete the entire `Draw[T]` threading in §5.6, and `pure` would let us
*state* that the render is a function of the seed. Milestone 6 produces the
concrete line-count evidence for the cost of not having it.

**10.3 Scalar refinement types** *(closes #10, #11, #12)* — research-directions
§7. `{ x: float | 0.0 <= x <= 1.0 }` for colors, `{ v: Vec3 | norm(v) == 1.0 }`
for units. The color case is decidable with the interval arithmetic the shape
solver already needs; the norm case needs real arithmetic and is probably
`assume` + runtime check territory. Worth splitting: **refined scalars are
tractable and useful; refined vectors are research.**

**10.4 `Fin[n]` and shape types** *(closes #15)* — research-directions §2. An
image is `Tensor[f32, [H, W, 3]]` and pixel indexing is exactly the bounds-check
elimination case. A ray tracer is a smaller, more legible motivating example for
shape types than a transformer, and it is a good first client for the solver.

**10.5 Termination checking** *(closes #13)* — research-directions §6. Structural
descent handles `Bvh` immediately; `ray_color`'s `depth` needs a decreasing-int
measure, which is the standard extension. Until then `never` means "modulo
halting" and every proof in §6 carries that footnote.

**10.6 `Nat` as an inductive fuel type** — a cheaper partial version of 10.5: make
`depth` a recursive alias `type Nat = "z" | { s: Nat }`, and recursion is
structurally decreasing by construction. Ugly, allocates, but it is *statable
today* and worth one experimental branch to see how bad it is.

**10.7 Operator overloading or a trait system** — purely ergonomic, but this
program is where the absence hurts most: `vadd(vscale(a, t), vscale(b, u))` is
unreadable compared to `a*t + b*u`. `Numeric`-style bounded quantification
(research-directions §7) would also let `min`/`max`/`clamp` be written once.

**10.8 Math builtins** — `abs`, `min`, `max`, `pow`, `sin`, `cos`, `floor`,
`float()`. Not research, just missing. An Emerald-source `math.rald` covers most
of it, but `sin`/`cos` cannot be written without them and the camera wants them
for Book 2's motion blur and textures.

**10.9 Two lints, nearly free** — flag `a < b < c` chains (a real footgun given
pairwise associativity, and this program is full of interval tests), and flag
record literals annotated with a brand outside the branding module (a cheap
stand-in for 10.1 until it exists).

---

## 11. Non-goals and dead ends

- **No parallelism.** No threads in the language; the render loop stays serial.
- **No Book 2/3 features** beyond the BVH (milestone 10) — textures, volumes,
  and importance sampling add code without adding type-theoretic content.
- **Don't try to encode `‖v‖ = 1` with tricks.** Phantom-typed nested records,
  Peano-encoded floats, and closure-guarded constructors all fail against
  structural typing or explode the allocation count. Write the assert, log the
  gap, and let §10.1/§10.3 fix it properly.
- **Don't replace `one_weekend.rald`.** It stays as the GC/allocator stress test
  and as the untyped baseline for every measurement in §9.3. The typed version
  lives beside it in `typed/`.
- **Don't chase parity with C++ speed.** The claim is checkability. Record the
  slowdown honestly and move on.

---

## 12. Definition of done

1. `typed/` renders the book's final scene, matching the existing `out.ppm`
   within sampling tolerance.
2. `task test` covers the golden images and every negative type test in §9.2.
3. The §6 table has a result in every row, with a file:line pointer to where each
   obligation is discharged or where the fallback lives.
4. §9.3's two measurements are recorded with numbers.
5. §10 is rewritten as a prioritized, evidence-backed list — each item citing the
   milestone that demonstrated the need — and folded into
   `docs/research-directions.md` as a case study.
