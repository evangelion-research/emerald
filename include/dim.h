/* Type-level natural expressions: a standalone canonical-form solver for the
 * shape system (SPEC_V2.md W2 / D3).
 *
 * A `DimExpr` is a dimension expression over named dimension variables and
 * integer literals, built from `+` and `*`. The solver normalizes each
 * expression to a sum-of-products canonical form and compares structurally,
 * deliberately avoiding an SMT dependency. `dim_le` implements the decidable
 * fragment needed for `Fin[n]` (W5): equal normal forms, literal comparison,
 * and `a <= a + k` for literal `k >= 0`. Anything else is reported as
 * *unresolved* so the SMT-vs-canonical decision (D3) is made from data.
 *
 * This file is standalone: it depends on nothing from the compiler, so it can
 * be unit-tested directly (tests/shape/dim_unit.c).
 */
#ifndef EMERALD_DIM_H
#define EMERALD_DIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { DE_VAR, DE_LIT, DE_ADD, DE_MUL } DimKind;

typedef struct DimExpr {
    DimKind kind;
    char *var;          /* DE_VAR: the dimension variable's name (owned) */
    int64_t lit;        /* DE_LIT: an integer literal */
    struct DimExpr *lhs; /* DE_ADD / DE_MUL */
    struct DimExpr *rhs;
} DimExpr;

/* constructors (take ownership of their arguments where applicable) */
DimExpr *dim_var(const char *name);
DimExpr *dim_lit(int64_t n);
DimExpr *dim_add(DimExpr *a, DimExpr *b);
DimExpr *dim_mul(DimExpr *a, DimExpr *b);
void dim_free(DimExpr *e);

/* Deep-copy an expression. */
DimExpr *dim_clone(const DimExpr *e);
/* Render an expression in a stable, human-readable form ("B*S + 2*D"). The
 * returned string is malloc'd and must be freed by the caller. */
char *dim_str(const DimExpr *e);

/* Structural equality: normalize both sides to canonical form and compare.
 * Always decidable. */
bool dim_eq(const DimExpr *a, const DimExpr *b);

/* `a <= b` over the decidable fragment. Returns 1 (true), 0 (false), or -1
 * (unknown; recorded by the escalation log). */
int dim_le(const DimExpr *a, const DimExpr *b);

/* --- escalation log (D3) ------------------------------------------------ */

/* Number of unresolved `dim_le` queries since the last reset. */
size_t dim_unresolved_count(void);
void dim_reset_unresolved(void);
/* Dump the recorded unresolved pairs (one per line) to `out`. */
void dim_log_dump(FILE *out);

#endif
