# Emerald documentation

Emerald is one project told in three registers, and the docs are organized that
way. Read whichever register you came for.

## 1. The language you can use today

A Python-shaped, brace-scoped, structurally typed language that compiles to a
native binary. Everything here is implemented and covered by `task test`.

| Doc | What it answers |
|---|---|
| [`grammar.md`](grammar.md) | What is the syntax? Lexical rules, EBNF, precedence table, the record-vs-block ambiguity and how it is resolved. |
| [`type-system.md`](type-system.md) | Records, structural subtyping, `&`/`\|`, literal types, flow narrowing, `never`, generics, and where the checker is deliberately gradual. |
| [`errors.md`](errors.md) | Expected errors: `error` declarations, `Result[T, E]`, `try` propagation, exhaustive `catch`, the Effect-style combinators, and what it all compiles to. |
| [`core-calculus.md`](core-calculus.md) | The formal fragment: syntax, subtyping, narrowing, generics, and the shape/`Fin` rules as judgments — the paper-style spec the research track calls for. |
| [`builtins.md`](builtins.md) | The seventy-eight builtins, exactly — and the rule for what earns a place among them rather than a place in the library. |
| [`tensors.md`](tensors.md) | Phase 2 numerics: the tensor runtime model, dtypes, zero-copy views, and byte-aware GC. |
| [`shapes.md`](shapes.md) | Phase 2 shapes: nominal `dim`s, the canonical-form solver, the per-op typing rules, `Fin[n]`, and the gradual boundary. |
| [`../stdlib/SPEC.md`](../stdlib/SPEC.md) | The standard library: twelve modules written in Emerald, why it has Python's shape but not Python's signatures, and the compiler bugs writing it exposed. |
| [`modules.md`](modules.md) | `import` / `from ... import`, path resolution order, privacy by leading underscore, name mangling, and the `emeraldc` CLI contract a package manager would drive. |
| [`diagnostics.md`](diagnostics.md) | Error-code reference and the `--json` schema, designed to be fed back to a tool (or an LLM) that fixes the program and re-runs. |
| [`architecture.md`](architecture.md) | How the compiler is built: stage-by-stage responsibilities, the `Value` model, the driver flags, the test layout. |
| [`concurrency.md`](concurrency.md) | Green threads: `spawn`, channels, `join`, the cooperative one-token scheduler, deadlock reporting, and what the GC had to learn to have one shadow stack per task. |
| [`gc.md`](gc.md) | The two-generation mark-and-sweep collector: object model, shadow-stack rooting, promotion, and measured numbers. |

## 2. The language as an instrument

The type system is strong enough to state propositions and have the compiler
check them. These docs are about what that buys, and — just as carefully —
where it stops.

| Doc | What it answers |
|---|---|
| [`proofs.md`](proofs.md) | The Curry–Howard reading of Emerald: proof by exhaustive case analysis, by enumeration, by parametricity, by impossibility; eliminating partiality; and an honest list of what Emerald *cannot* prove. |
| [`../examples/ray_tracer/PLAN.md`](../examples/ray_tracer/PLAN.md) | The experiment: re-implement *Ray Tracing in One Weekend* as a typed, modular, proof-carrying program to find out how much of a real graphics program the type system can hold. |
| [`../examples/ray_tracer/typed/README.md`](../examples/ray_tracer/typed/README.md) | The result: 16 propositions, scored. 6 provable, 1 partial, 7 out of reach — plus the two real bugs the types and determinism caught. |

The last row is the load-bearing one. The seven "not statable" propositions
were not guessed at; they were produced by a 13-module program that wanted
them, and they become the requirements list for §3.

## 3. Where it is going

| Doc | What it answers |
|---|---|
| [`research-directions.md`](research-directions.md) | The agenda: three papers, an honest gap analysis, and ten research tracks (shape types, effects, interpretations as typed objects, approximate judgments, a meaningful proof fragment, numerics, a typed mech-interp API, metatheory, LLM-target ergonomics, tooling), with a suggested ordering and explicit non-goals. |

The thesis in one sentence: **models as morphisms, interpretations as typed
refinements between them, obligations discharged exactly where possible and
statistically where not, with a machine-checkable certificate at the end.**

## Suggested reading order

- **"I want to write a program."** [`grammar.md`](grammar.md) →
  [`type-system.md`](type-system.md) → [`errors.md`](errors.md) →
  [`builtins.md`](builtins.md) → [`modules.md`](modules.md).
- **"I want to hack on the compiler."** [`architecture.md`](architecture.md) →
  [`diagnostics.md`](diagnostics.md) → [`gc.md`](gc.md), then read
  `src/check.c` with [`type-system.md`](type-system.md) open beside it.
- **"I want to know if the research claim is real."**
  [`proofs.md`](proofs.md) §"What Emerald cannot prove" →
  the ray tracer's [scorecard](../examples/ray_tracer/typed/README.md) →
  [`research-directions.md`](research-directions.md) §1.
- **"Where do I read the cited papers?"** [`references.md`](references.md) —
  the annotated bibliography, grouped by topic, with one line per entry on why
  it matters to Emerald.

## Further reading

[`references.md`](references.md) is the consolidated external bibliography: the
three anchor papers, then gradual typing, structural subtyping, dependent and
shape types, array languages, autodiff, mechanistic interpretability, effects,
proof theory, GC, compiler construction, categorical foundations, and LLM code
generation — each entry annotated with its relevance to this project.
