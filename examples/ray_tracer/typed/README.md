# Typed ray tracer (Ray Tracing in One Weekend)

A modular, proof-carrying reimplementation of
[Ray Tracing in One Weekend](https://raytracing.github.io/books/RayTracingInOneWeekend.html),
written per [`PLAN.md`](../PLAN.md). Milestones 0–9 of the plan (the book's 13
chapters). The BVH (plan milestone 10) is Book 2 material and is not included.

Build and run (from `examples/ray_tracer/`):

```
../../bin/emeraldc -o typed/main typed/main.rald
./typed/main          # writes out.ppm (120x67, 4 spp, depth 16, seed 42)
```

Same seed → byte-identical `out.ppm` (verified).

## Module graph

```
math.rald      min/max/abs/clamp/pow/radians/reflectance       (no deps)
rng.rald       Rng, Draw[T], seeded Park–Miller LCG            (no deps)
vec.rald       Vec3 / Dir / Unit / Pt brands, affine ops, samplers  <- math, rng
interval.rald  Interval, checked smart constructor              (no deps)
ray.rald       Ray = { orig: Pt, dir: Dir }, ray_at             <- vec
color.rald     Color, Rgb8, Chan = "r"|"g"|"b", gamma+clamp     <- vec, math
material.rald  Contact, Scatter|None, closure materials         <- vec, ray, color, rng, math
hittable.rald  Sphere/Prim tagged sum, Hit = Contact & {t, mat} <- vec, ray, interval, material
camera.rald    positionable camera + defocus blur               <- vec, ray, rng, math
render.rald    ray_color, render loop, PPM writer               <- everything
scene.rald     the book's final scene as data                   <- hittable, material, vec, color, rng
main.rald      entry point: seed, camera, config, write out.ppm <- scene, camera, render
```

## The type-theoretic choices (what holds, and what doesn't)

The full §6 table from the plan, with results:

| # | Proposition | Result | Where |
|---|---|---|---|
| 1 | Primitive dispatch is exhaustive | **provable** | `hittable.rald` — `impossible: never = p` after the sphere case; reopens if `Prim` grows |
| 2 | Material dispatch is exhaustive | N/A | materials are closures (existential), no sum to dispatch |
| 3 | BVH case analysis exhaustive | skipped | BVH is Book 2 (plan milestone 10) |
| 4 | Split axis ∈ {0,1,2} | skipped | BVH-only |
| 5 | Channel ∈ {r,g,b} | **provable** | `color.rald` — `type Chan = "r" \| "g" \| "b"`, narrowed in `chan_val` |
| 6 | A miss cannot be read as a hit | **provable** | every hit is `Hit \| None`; `ray_color` narrows before reading `.t` |
| 7 | scatter failure carries no fake data | **provable** | `scatter` returns `Scatter \| None`; a miss has no attenuation/ray to read |
| 8 | Point/direction confusion | **provable** | `padd(p: Pt, d: Dir)`, `psub(Pt, Pt) -> Dir`; `padd(p, p)` is a compile error |
| 9 | reflect/refract get unit input | **partly** | `reflect(v: Unit, n: Unit)`; `Unit` has an extra `unit: "unit"` field, so a plain `Dir` won't typecheck — but the brand is forgeable by hand |
| 10 | ‖unit(v)‖ = 1 | **not statable** | `unit()` normalizes (the claim is made good at the boundary); no refinement types, no `assert` |
| 11 | lo ≤ hi on intervals | **not statable** | `interval()` returns `Interval \| None`; callers must narrow (or use private `_unchecked` inside the module) |
| 12 | Color ∈ [0,1] before gamma | **not statable** | `to_rgb8` is total *because* of the clamp |
| 13 | ray_color terminates | **not statable** | `depth` strictly decreases; no termination checker |
| 14 | Render is a function of the seed | **not statable** | true by construction — `Rng` is threaded through every draw and comes back out as `Draw[T]` |
| 15 | Image indices in bounds | **not statable** | no `Fin[n]` |
| 16 | Scene list not aliased | **not statable** | covariant `list` is unsound by design |

Result: 6 provable, 1 partial, 7 out of reach — exactly the ratio the plan
predicts, and the "not statable" rows are the concrete requirements list
(plan §10): opaque types (#9/#10), scalar refinements (#11/#12), effects
(#14), termination (#13), shape types (#15).

## Deviations from the plan's sketch (all documented)

- **`Material.scatter` threads `Rng`**: `(Ray, Contact, Rng) -> Draw[Scatter | None]`
  instead of `(Ray, Contact) -> Scatter | None`. Sampling needs randomness and
  there are no effects; the alternative (global `rand()`) breaks determinism
  (plan §5.6). The plan's §5.4 signature was inconsistent with its own §5.6.
- **No moving spheres**: `Prim = Sphere` only, per the book's 13 chapters.
  The `never` obligation still guards the dispatch site.
- **No gamma on the reference image**: `out.ppm` from `one_weekend.rald` is
  linear; the typed renderer applies the book's sqrt gamma in `to_rgb8`
  (plan §5.5). Compare with `sqrt` applied to the linear values.
- **Unit is a subtype of Dir** via an extra field (`{tag:"dir", unit:"unit"}`)
  rather than a different tag, so a unit vector flows into `dadd`/`ddot`/
  `reflect` freely while a plain `Dir` still fails the `Unit` requirements.

## Verification

- `emeraldc --check typed/main.rald` → `ok` (the whole linked program).
- Deterministic: two runs, same seed → `cmp`-identical `out.ppm`.
- Cross-checked against `one_weekend.rald` at 32 spp with the same camera and
  scene: the sky (randomness-free) matches to a mean channel diff of ~2/255
  after gamma; the three big spheres to ~10–18/255 (the residual is the
  different PRNG — the LCG vs `rand()` — changing the random sphere field
  behind them, and 32 spp sampling noise).
- Runtime at defaults (120×67, 4 spp, depth 16, seed 42): **~10.4 s** vs
  ~9.4 s for the untyped `one_weekend.rald` at the same settings — roughly a
  10% cost for the brands, the `Draw[T]` threading, and the option-typed
  partiality. Plan §9.3's measurement table is the benchmark the user runs.

## Bugs found by the type system / by determinism

- A rejection sampler that forgot to rebind `g` looped forever (drew the same
  vector); caught instantly because the seeded render hung and then
  non-terminated. `g = d.rng` now updates per iteration.
- The defocus disk was not scaled by `lens_radius` (20× too large), smearing
  the image; caught by comparing the deterministic sky against the reference.
