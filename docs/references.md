# Further Reading

This is the consolidated bibliography for the whole project. Every entry is
external — a paper, book, survey, or project — and each carries one line on why
it matters to Emerald, so you can decide whether to open it without reading it.

Where a stable link exists I give it; otherwise the bibliographic detail is
enough to find the work. The three papers that *define* the thesis are at the
top and are also discussed in
[`research-directions.md`](research-directions.md) §0.

---

## 1. The anchor papers (Emerald's thesis)

The project's one-sentence thesis — *models as morphisms, interpretations as
typed refinements between them, obligations discharged exactly where possible
and statistically where not, with a machine-checkable certificate at the end* —
is the intersection of these three.

| Work | Why it matters here |
|---|---|
| Gauderis, Dooms, Homer, Ayonrinde, Wiggins. *From Mechanistic to Compositional Interpretability.* arXiv:2605.08934 (2026). https://arxiv.org/abs/2605.08934 | An interpretation is a pair of syntactic + semantic mappings that must **commute**; quality is faithfulness vs. description length. This is the direct motivation for `research-directions.md` §4 (`interpretation`, `circuit`, MDL metric). |
| Palumbo, Mangal, Wang, Vijayakumar, Pasareanu, Jha. *Validating Mechanistic Interpretations: An Axiomatic Approach.* ICML 2025. https://arxiv.org/abs/2407.13594 | Borrows program analysis to state axioms under which an interpretation *approximately* captures a network's semantics. The source of §5's `(ε, δ)` obligations and certificates. |
| Perrier. *Typed Chain-of-Thought: A Curry–Howard Framework for Verifying LLM Reasoning.* arXiv:2510.01069 (2025). https://arxiv.org/abs/2510.01069 | A faithful CoT trace is a well-typed program; converting one into a well-typed proof is a certificate of faithfulness. The source of §11 (typed holes, repair diagnostics, proof export). |

---

## 2. Gradual typing and blame

Emerald's type system is *gradual*: unannotated code is `any` and checks like
Python; annotations are enforced. This is the literature that invented that
idea and worked out how to keep it sound (and where it isn't).

| Work | Why it matters here |
|---|---|
| Siek & Taha. *Gradual Typing for Functional Languages.* Scheme and Functional Programming Workshop, 2006. | The founding paper. Defines the `any`-as-top-and-bottom dynamic type and the consistency relation. |
| Siek, Vitousek, Cimini, Boyland. *Refined Criteria for Gradual Typing.* SNAPL 2015. | The grading rubric (`S`, `G`, `G G`) for gradual type systems; useful for auditing Emerald's own soundness story. |
| Wadler & Findler. *Well-Typed Programs Can't Be Blamed.* ESOP 2009. | Introduces the **blame calculus** — the precise account of *which* side of a type boundary is wrong. This is the formal target for §15.1's blame calculus. |
| Garcia, Clark, Tanter. *Abstracting Gradual Typing.* POPL 2016. | Abstract interpretation view of gradual typing; the cleanest framework for "where does `any` taint flow" (§6). |
| Cimini & Siek. *The Gradualizer: Methodology and Algorithm for Generating Gradual Type Systems.* POPL 2016. | How to derive a gradual system from a static one; relevant if Emerald's static fragment is ever generated rather than hand-written. |
| Tobin-Hochstadt & Felleisen. *The Design and Implementation of Typed Scheme.* POPL 2008. | The closest real-world sibling: a gradually-typed language compiled to native code, used in production. |
| Flanagan. *Hybrid Type Checking.* POPL 2006. | Type checking that defers to runtime assertions where it cannot decide statically — the direct ancestor of the static↔dynamic **shape boundary** (§5 of shapes.md, the `--shape-report` counter). |

---

## 3. Structural subtyping and type-theory foundations

Emerald has no classes; "inheritance" is subset-of-fields. That is structural
typing, and these are its foundations.

| Work | Why it matters here |
|---|---|
| Cardelli. *Structural Subtyping and the Notion of Power Type.* POPL 1988. | The canonical account of record width subtyping and why it is convenient and, for mutable records, unsound. |
| Pierce. *Types and Programming Languages.* MIT Press, 2002. | The standard textbook; every rule in [`core-calculus.md`](core-calculus.md) is written in its notation. |
| Harper. *Practical Foundations for Programming Languages.* Cambridge, 2e 2016. | A more rigorous, judgment-based treatment of the same material. |
| Cardelli. *Type Systems.* In *The Computer Science and Engineering Handbook*, 1997. | A compact survey; good for the union/intersection/literal vocabulary Emerald uses. |

---

## 4. Dependent types, index systems, and shape

Shapes are "practical dependent types": enough type-level arithmetic to talk
about sizes without a full dependent theory. This is the lineage
[`shapes.md`](shapes.md) and
[`research-directions.md`](research-directions.md) §2 mine.

| Work | Why it matters here |
|---|---|
| Xi & Pfenning. *Dependent Types in Practical Programming.* POPL 1999. https://doi.org/10.1145/292540.292560 | **The** precedent for `Fin[n]` and static index safety: Dependent ML used dependent types to check array bounds. |
| Xi & Pfenning. *Eliminating Array Bound Checking Through Dependent Types.* PLDI 1998. | The follow-on that shows bounds-check *elimination* (Emerald deliberately defers this to Phase 4). |
| Xi. *Dependent ML: An Approach to Practical Programming with Dependent Types.* JFP 2007. | The journal version; the type-level index arithmetic and its decidable fragments are the model for `dim_eq`/`dim_le`. |
| McBride. *Faking It: Simulating Dependent Types in Haskell.* 2002. | Singleton types as a way to get indexed types without a dependent core — the bridge between "shapes" and "full dependent types". |
| Norell. *Towards a Practical Programming Language Based on Dependent Type Theory* (Agda). PhD thesis, 2007. | What the *other* end of the spectrum looks like; §14 argues for not going here. |
| Eisenberg. *Dependent Types in Haskell: Theory and Practice.* PhD thesis, 2016. | How a real compiler adds indexed types incrementally; relevant to the §2 escalation decision. |
| Brady. *Type-Driven Development with Idris.* Manning, 2017. | The ergonomic argument for dependent types, and the ergonomic cost Emerald is trying to avoid. |
| Vazou, Seidel, Jhala, Vytiniotis, Peyton Jones. *Refinement Types for Haskell.* ICFP 2014. | Liquid-style refinement on scalars (`{ x: float | 0 <= x <= 1 }`) is §6's proposal; this is the implementation that makes it real. |
| de Moura & Ullrich. *The Lean 4 Theorem Prover and Programming Language.* CADE 2021. | The proof assistant Emerald's §10 metatheory would mechanize in. |
| Leino. *Dafny: An Automatic Program Verifier for Functional Correctness.* LPAR 2010. | A language that keeps verification *usable* by leaning on an SMT solver — the escalation path §2 deliberately avoids for now. |

---

## 5. Array and shape languages

The tensor layer (`docs/tensors.md`, `docs/shapes.md`) is a small, closed set
of array primitives. These are the languages that already solved the
array-programming half.

| Work | Why it matters here |
|---|---|
| Iverson. *Notation as a Tool of Thought.* ACM Turing Award Lecture, CACM 1980. | APL's array-level programming; the intellectual ancestor of every whole-array operation Emerald uses (D1). |
| Henriksen, Serup, Elsman, Henglein, Oancea. *Futhark: Purely Functional GPU-Programming with Nested Parallelism and In-Place Array Updates.* PLDI 2017. | A purely functional array language with static shapes; the closest relative to `Tensor[f32, [B, S, D]]`. |
| Paszke, Johnson, Radul, Maclaurin. *Getting to the Point: Index Sets and Parallelism-Preserving Autodiff for Pointful Array Programming.* ICFP 2021. https://arxiv.org/abs/2104.05372 | Dex; the "index set" approach to typed array shapes that §2 and §7 (Track G) both cite. |
| Harris et al. *Array Programming with NumPy.* Nature 585, 2020. | The broadcasting and indexing semantics Emerald's tensor ops follow. |
| Ragan-Kelley, Barnes, Adams, Paris, Durand, Amarasinghe. *Halide: A Language and Compiler for Optimizing Parallelism, Locality, and Recomputation.* PLDI 2013. | Scheduling separated from algorithm; the right shape for any future kernel work. |
| Chen et al. *TVM: An Automated End-to-End Optimizing Compiler for Deep Learning.* OSDI 2018. https://arxiv.org/abs/1802.04799 | The argument for emitting to an existing compiler stack rather than writing a GPU backend (§14). |
| Kidger. *jaxtyping.* https://github.com/google-deepmind/jaxtyping | Runtime-checked array shapes in Python type annotations; the ergonomic surface Emerald makes *static*. |
| Paszke et al. *PyTorch: An Imperative Style, High-Performance Deep Learning Library.* NeurIPS 2019. https://arxiv.org/abs/1912.01703 | The ecosystem Emerald is explicitly *not* trying to out-run (checkability, not throughput). |
| Frostig, Johnson, Leary. *Compiling Machine Learning Programs via High-Level Tracing.* SysML 2018. | JAX's tracing model; the relevant half of what a Phase 4 source-to-source autodiff transform has to do. |
| Bradbury et al. *JAX: Composable Transformations of Python+NumPy Programs.* 2018. | The `grad`-as-composable-transform design §7 (Track G) wants to type. |

---

## 6. Automatic differentiation

Phase 4 is "autodiff as a typed source-to-source transformation." These define
the space.

| Work | Why it matters here |
|---|---|
| Elliott. *The Simple Essence of Automatic Differentiation.* ICFP 2018. https://arxiv.org/abs/1804.00746 | The categorical account: AD is a functor between derivative categories. The theoretical shape of §7's tangent-type functor. |
| Baydin, Pearlmutter, Radul, Siskind. *Automatic Differentiation in Machine Learning: A Survey.* JMLR 2018. https://arxiv.org/abs/1502.05767 | The field map: forward vs. reverse mode, when each wins, what's hard. |
| Griewank & Walther. *Evaluating Derivatives: Principles and Techniques of Algorithmic Differentiation.* SIAM, 2e 2008. | The engineering bible; where the "naive kernel as oracle" strategy (W6) comes from. |
| Pearlmutter & Siskind. *Reverse-Mode AD in a Functional Framework: Lambda the Ultimate Backpropagator.* TOPLAS 2008. | Reverse mode over higher-order functions — the hard case a typed transform must handle. |

---

## 7. Mechanistic interpretability

The application Emerald exists for (§8 and §9 of the research directions). These
are the works a typed hook/interpretation API would make checkable.

| Work | Why it matters here |
|---|---|
| Elhage et al. *A Mathematical Framework for Transformer Circuits.* Transformer Circuits Thread, 2021. https://transformer-circuits.pub/2021/framework/index.html | The residual-stream framing that a `Circuit[A, B]` IR (§4.1) formalizes. |
| Olsson et al. *In-context Learning and Induction Heads.* Transformer Circuits Thread, 2022. https://transformer-circuits.pub/2022/in-context-learning-and-induction-heads/index.html | The canonical case study; §15 names induction heads as the demo target. |
| Nanda et al. *Progress Measures for Grokking via Mechanistic Interpretability.* ICLR 2023. https://arxiv.org/abs/2301.05217 | Shows interpretations can be *measured*; the premise of the description-length metric in §4.2. |
| Bricken et al. *Towards Monosemanticity: Decomposing Language Models With Dictionary Learning.* Transformer Circuits Thread, 2023. https://transformer-circuits.pub/2023/monosemantic-features/index.html | The SAE/feature-dictionary types §8 wants to give a real type. |
| Geiger et al. *Causal Abstraction: A Theoretical Foundation for Mechanistic Interpretability.* arXiv:2301.04709 (2023). | The "interchange intervention" formalization — the same commuting-square idea as §4, from the causality side. |
| Chan, Conmy, et al. *Towards Automated Circuit Discovery for Mechanistic Interpretability.* NeurIPS 2023. https://arxiv.org/abs/2304.14997 | Automatic circuit discovery; the kind of search a description-length metric would guide. |
| Nanda. *200 Concrete Open Problems in Mechanistic Interpretability.* 2023. | The requirements list; a good place to mine §8/§9 for concrete first demos. |
| Cunningham, Ewart, Riggs, Huben, Sharkey. *Sparse Autoencoders Find Highly Interpretable Features in Language Models.* ICLR 2024. | SAE training methodology; what a typed `Dict[NFeatures, DModel]` (§8) would reconstruct. |
| Nanda & Bloom. *TransformerLens.* (project) | The string-keyed hook API Emerald's §8 typed hooks are reacting against. |

---

## 8. Effects, linearity, and purity

`pure` already exists as a call-level check; §3 (Track B) wants effect rows and
affine tensors.

| Work | Why it matters here |
|---|---|
| Lucassen & Gifford. *Polymorphic Effect Systems.* POPL 1988. | Effect systems with polymorphism; the formal basis for `!{Rand, Mut, IO}`. |
| Leijen. *Koka: Programming with Row Polymorphic Effect Types.* 2014. | The modern ergonomic reference for row-based, inferred effect types (§3). |
| Plotkin & Pretnar. *Handlers of Algebraic Effects.* ESOP 2009. | Algebraic effects and handlers; the semantics of §8's `with patch(site, value) { ... }` scoped effects. |
| Wadler. *Linear Types Can Change the World!* 1990. | Linear logic applied to programming; the theory behind §3's affine-tensor proposal. |
| Wadler & Blott. *How to Make Ad-Hoc Polymorphism Less Ad Hoc.* POPL 1989. | Type classes; the `Numeric`/`Differentiable` bounds §6 (Track F) wants. |

---

## 9. Proofs and the Curry–Howard correspondence

[`proofs.md`](proofs.md) treats the checker as a proof checker for a small logic.
This is the literature that logic comes from.

| Work | Why it matters here |
|---|---|
| Howard. *The Formulae-as-Types Notion of Construction.* 1980. | The original Curry–Howard paper; the correspondence table in proofs.md is this. |
| Wadler. *Propositions as Types.* CACM 58(12), 2015. | The modern, readable statement of the same correspondence. |
| Girard, Lafont, Taylor. *Proofs and Types.* Cambridge, 1989. | The rigorous treatment, including what happens when the logic is *inconsistent* — the exact trap `never`-by-divergence is (proofs.md §1). |
| Dybjer. *Inductive Families.* 1994. | The theory of inductive data and its eliminators — what §6's "derive the induction principle" would implement. |
| Friedman & Christiansen. *The Little Typer.* MIT Press, 2018. | Dependent types taught by example; a gentler on-ramp than the papers. |

---

## 10. Garbage collection

[`gc.md`](gc.md) documents a precise, two-generation mark-and-sweep collector
with a shadow stack and byte-aware triggering.

| Work | Why it matters here |
|---|---|
| Lieberman & Hewitt. *A Real-Time Garbage Collector Based on the Lifetimes of Objects.* CACM 26(6), 1983. | The generational hypothesis — the empirical claim the young/old split rests on. |
| Ungar. *Generation Scavenging: A Non-Disruptive High Performance Storage Reclamation Algorithm.* SIGPLAN Notices 19(5), 1984. | The classic generational collector design Emerald's minor/major split follows. |
| Jones & Lins. *Garbage Collection: Algorithms for Automatic Dynamic Memory Management.* Wiley, 1996. | The reference for mark-and-sweep, write barriers, and remembered sets. |
| Wilson. *Uniprocessor Garbage Collection Techniques.* 1992 (survey). | The taxonomy that places Emerald's choices against the alternatives (conservative, copying, ref-counting). |
| Blackburn, Cheng, McKinley. *Myths and Realities: The Performance Impact of Garbage Collection.* SIGMETRICS 2004. | Evidence-based myth-busting on GC performance; useful when the byte-accounting numbers are questioned. |

---

## 11. Compilers and code generation

[`architecture.md`](architecture.md) is a seven-stage pipeline; these are the
textbooks the pipeline's decisions come from.

| Work | Why it matters here |
|---|---|
| Appel. *Modern Compiler Implementation in C.* Cambridge, 1998. | The stage-by-stage structure (`lexer → parser → … → codegen`) in the language Emerald itself is written in. |
| Aho, Lam, Sethi, Ullman. *Compilers: Principles, Techniques, and Tools.* Addison-Wesley, 2e 2006. | The Dragon Book; where the "no stage observable only through the next" test rule gets its discipline. |
| Cooper & Torczon. *Engineering a Compiler.* Morgan Kaufmann, 3e 2022. | A more engineering-forward treatment, useful for the shadow-stack rooting and frame-slot codegen. |

---

## 12. Categorical and compositional foundations

§4's `Circuit[A, B]` IR and `--emit-diagram` come from categorical semantics.

| Work | Why it matters here |
|---|---|
| Coecke & Kissinger. *Picturing Quantum Processes.* Cambridge, 2017. | String diagrams and symmetric monoidal categories — the formalism behind §4.1's morphisms `>>` and `⊗`. |
| Fong & Spivak. *Seven Sketches in Compositionality.* 2018. https://arxiv.org/abs/1803.05316 | An applied, readable introduction to categories-as-composition; the §4.1 surface. |
| Selinger. *A Survey of Graphical Languages for Monoidal Categories.* 2010. | When string diagrams and categorical structure *coincide* — what would justify trusting `--emit-diagram` output. |

---

## 13. LLM code generation and repair

§11 (Track J) is about making Emerald a target a model can *aim at* and be
corrected by. These are the surrounding results.

| Work | Why it matters here |
|---|---|
| Austin et al. *Program Synthesis with Large Language Models.* arXiv:2108.07732 (2021). | The scaling evidence for LLMs-as-program-synthesizers that motivates the loop. |
| Chen et al. *Evaluating Large Language Models Trained on Code.* arXiv:2107.03374 (2021). | Codex; the baseline a repair-oriented diagnostic loop is measured against. |
| (Perrier, above, §1) | The paper that turns "typed = faithful" into the §11 benchmark. |

---

## How to read this list

The deliberate order is *inside-out*: §2–§4 are the type system Emerald already
has; §5–§8 are the features the research tracks add; §9–§12 are the
foundations; §13 is the loop that closes it. If you want one thing per track,
the `research-directions.md` §0 table names it — this page is where you go to
actually read the cited work.
