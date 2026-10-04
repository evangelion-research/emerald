# NOTES.md — literature pinned for M0.1

Task M0.1 of `PLAN.md`: read the two papers, then record the exception list,
the `KTS`/`NKTS` correspondence, and the difference-method device with
citations. Status: **partially pinned**. What follows states exactly what I
checked and what I am only repeating from a secondary source, so nothing here
is stronger than its evidence.

Confidence tags: **[verified against source]** (I read it), **[secondary]**
(from an encyclopedia's summary of the primary source), **[folklore-known]**
(standard, and its correctness is also exercised by code in this directory).

## 1. The four known non-solvable instances

For `G = C_{m_1} ∪ … ∪ C_{m_t}` with `Σ m_i = n`:

| Instance | n | Solvable? |
|---|---|---|
| `OP(3,3)` = `OP(3^2)` | 6 | **no** |
| `OP(4,5)` | 9 | **no** |
| `OP(3,3,5)` | 11 | **no** |
| `OP(3,3,3,3)` = `OP(3^4)` | 12 | **no** |

all other admissible instances are believed to be solvable.

- **[secondary]** Wikipedia, *Oberwolfach problem* (accessed 2026-10-03),
  "Known results", lists exactly `OP(3^2)`, `OP(3^4)`, `OP(4,5)`, `OP(3,3,5)`
  as the instances known to have no solution, attributed to Alspach–Häggkvist
  (1985) and the references there. This matches `PLAN.md` §2.

## 2. Resolution for large n

- **[verified against source]** Glock, Joos, Kim, Kühn, Osthus, *Resolution of
  the Oberwolfach problem*, arXiv:1806.04644v2, J. Eur. Math. Soc. 23 (2021)
  2511–2547. Abstract (read on arXiv): the Oberwolfach problem "asks for a
  decomposition of `K_{2n+1}` into edge-disjoint copies of a given 2-factor.
  We show that this can be achieved for all large `n`." The same paper resolves
  the Hamilton–Waterloo problem for large `n`. Note the abstract states the
  odd-order form; the paper's more general result covers the even variant too.
- **[secondary]** Wikipedia further says a constructive solution is known for
  all instances with `n ≤ 60` other than the four exceptions, citing Deza et
  al. and another paper. `PLAN.md` §2 says `18 ≤ n ≤ 40` was settled by
  Deza–Franek–Hua–Meszka–Rosa. The two statements are consistent (40 < 60) but
  the "≤ 60" figure is from a source I did not read. **Action for M0.1: read
  the Deza et al. PDF to pin the exact range.** URL recorded in §5.

## 3. The `KTS`/`NKTS` correspondence (was "medium confidence" in PLAN §2)

- **[secondary]** Kirkman's schoolgirl problem is `OP(3^5)` (Wikipedia,
  *Oberwolfach problem*). More generally a resolvable Steiner triple system of
  order `v` (`KTS(v)`) is a triangle decomposition of `K_v` whose triangles
  partition into parallel classes — exactly `OP(3^{v/3})` for odd `v`.
- **[folklore-known]** `KTS(v)` exists iff `v ≡ 3 (mod 6)` (Kirkman 1847 /
  Ray-Chaudhuri–Wilson 1971); the existence is standard and is quoted in the
  accessible literature.
- **[unpinned]** For even `n = 6k`, the claim that the analogue — a resolvable
  triangle decomposition of `K_n` minus a 1-factor, so a "near Kirkman triple
  system" `NKTS(n)` — exists for every `n ≡ 0 (mod 6)` except `n = 6` and
  `n = 12`. This is exactly the two all-triangle entries in the exception list,
  and it is *why* `OP(3^2)` and `OP(3^4)` are the two triangle exceptions. I
  could not confirm the `NKTS` terminology and spectrum from a primary source
  in the time available. What is solid is the direction that matters for us:
  **`OP(3^k)` is solved except `k = 2, 4`** (Wikipedia, "all instances
  `OP(x^y)` except `OP(3^2)` and `OP(3^4)`"), which is the same statement.
  The `M2` run on `OP(3^4)` reproduces a known non-existence, and is
  cross-checkable against this.

## 4. The difference-method device (for `T1` and `T2`)

Vertices `Z_n`. For `d` with `1 ≤ d ≤ n/2`, the graph `D_d` with edge set
`{ {x, x+d} : x ∈ Z_n }` is 2-regular. **[folklore-known]** and used by the
`T1`/`T2` code:

- `D_d` is a disjoint union of `g = gcd(d, n)` cycles, each of length
  `n/g`. So `D_d ≅ OP(n; (n/g)^g)` — a uniform instance. `d` and `n-d` give
  the same graph, so the distinct classes are `d = 1, …, ⌊n/2⌋`.
- The `⌊(n-1)/2⌋` classes with `gcd(d,n)=1` are Hamiltonian cycles; together
  with the others they are the raw material of `T1` and the orbit search of
  `T2`.

The group-invariant `T2` search takes a group `G` acting on the vertices and
looks for orbit representatives whose translates tile the edge set exactly
(`PLAN.md` §8). **[unpinned]** the precise "starter / difference-family"
formulation used by Deza et al.; `M0.1`'s remaining job is to read their PDF
and transcribe the device rather than guess it, as `PLAN.md` §13 warns.

## 5. Sources and URLs

- Glock–Joos–Kim–Kühn–Osthus, *Resolution of the Oberwolfach problem*,
  arXiv:1806.04644; JEMS 23 (2021) 2511–2547.
  https://arxiv.org/abs/1806.04644
- Deza–Franek–Hua–Meszka–Rosa, *Solutions to the Oberwolfach problem for
  orders 18 to 40*, J. Combin. Math. Combin. Comput. 74 (2010) 95–102.
  http://www.cas.mcmaster.ca/~deza/jcmcc2010.pdf
- Alspach–Schellenberg–Stinson–Wagner, *The Oberwolfach problem and factors of
  uniform odd length cycles*, JCTA 52 (1989) 20–43.
- Wikipedia, *Oberwolfach problem*.
  https://en.wikipedia.org/wiki/Oberwolfach_problem
- Burgess, *A survey on constructive methods for the Oberwolfach problem*,
  arXiv:2308.04307 (survey; useful for `T2`).

## 6. Corrections to `PLAN.md` §2

1. §2 says the all-triangle cases "can be cross-checked against" the
   `KTS`/`NKTS` result. The cross-check is real for the direction stated in §3
   above (`OP(3^k)` solvable except `k = 2, 4`), but the specific spectrum
   claim for `NKTS` is **not yet pinned** — see §3. Treat it as evidence, not
   as a citation, until the `NKTS` paper is read.
2. §2's "`18 ≤ n ≤ 40` was settled computationally by Deza et al." is the
   source the plan names; the secondary literature credits a wider range
   (`n ≤ 60`) to overlapping work. Not a contradiction, but the plan's range is
   asserted more narrowly than the literature states. Re-check against the
   PDF in the remaining `M0.1` work.
3. §2 says `n ≤ 17` "was settled before 2010". I did not pin a citation for
   the pre-2010 boundary in this pass; it does not affect any milestone, since
   `M1` re-derives it from an exhaustive search regardless.
 