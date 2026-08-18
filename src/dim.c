/* Dimension-expression solver: normalize to a sum-of-products canonical form
 * and compare structurally. See include/dim.h for the model and D3 in
 * docs/SPEC_V2.md for why there is deliberately no SMT dependency here. */
#include "dim.h"

#include <stdlib.h>
#include <string.h>

/* --- tiny allocation helpers -------------------------------------------- */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    return p;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *d = xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

/* --- constructors ------------------------------------------------------- */

DimExpr *dim_var(const char *name) {
    DimExpr *e = xmalloc(sizeof(DimExpr));
    e->kind = DE_VAR;
    e->var = xstrdup(name);
    e->lit = 0;
    e->lhs = e->rhs = NULL;
    return e;
}
DimExpr *dim_lit(int64_t n) {
    DimExpr *e = xmalloc(sizeof(DimExpr));
    e->kind = DE_LIT;
    e->var = NULL;
    e->lit = n;
    e->lhs = e->rhs = NULL;
    return e;
}
DimExpr *dim_add(DimExpr *a, DimExpr *b) {
    DimExpr *e = xmalloc(sizeof(DimExpr));
    e->kind = DE_ADD;
    e->var = NULL;
    e->lit = 0;
    e->lhs = a;
    e->rhs = b;
    return e;
}
DimExpr *dim_mul(DimExpr *a, DimExpr *b) {
    DimExpr *e = xmalloc(sizeof(DimExpr));
    e->kind = DE_MUL;
    e->var = NULL;
    e->lit = 0;
    e->lhs = a;
    e->rhs = b;
    return e;
}
void dim_free(DimExpr *e) {
    if (!e) return;
    if (e->kind == DE_VAR) free(e->var);
    dim_free(e->lhs);
    dim_free(e->rhs);
    free(e);
}

DimExpr *dim_subst(const DimExpr *e, char *const *names, DimExpr *const *values,
                   size_t count) {
    if (!e) return NULL;
    switch (e->kind) {
    case DE_VAR:
        for (size_t i = 0; i < count; i++)
            if (strcmp(e->var, names[i]) == 0)
                return dim_clone(values[i]);
        return dim_var(e->var);
    case DE_LIT:
        return dim_lit(e->lit);
    case DE_ADD:
        return dim_add(dim_subst(e->lhs, names, values, count),
                       dim_subst(e->rhs, names, values, count));
    case DE_MUL:
        return dim_mul(dim_subst(e->lhs, names, values, count),
                       dim_subst(e->rhs, names, values, count));
    }
    return NULL;
}

DimExpr *dim_clone(const DimExpr *e) { return dim_subst(e, NULL, NULL, 0); }

/* --- normalized polynomials --------------------------------------------- */

typedef struct {
    int64_t coef;
    char **vars;   /* sorted lexicographically, with multiplicity */
    size_t nvars;
} Term;

typedef struct {
    Term *terms;
    size_t count, cap;
} Poly;

static void term_free(Term *t) {
    for (size_t i = 0; i < t->nvars; i++) free(t->vars[i]);
    free(t->vars);
}

static void poly_init(Poly *p) { p->terms = NULL; p->count = p->cap = 0; }

static void poly_push(Poly *p, int64_t coef, char **vars, size_t nvars) {
    if (p->count == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->terms = realloc(p->terms, sizeof(Term) * p->cap);
        if (!p->terms) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    p->terms[p->count].coef = coef;
    p->terms[p->count].vars = vars;
    p->terms[p->count].nvars = nvars;
    p->count++;
}

static void poly_free(Poly *p) {
    for (size_t i = 0; i < p->count; i++) term_free(&p->terms[i]);
    free(p->terms);
    p->terms = NULL;
    p->count = p->cap = 0;
}

/* lexicographic comparison of two sorted variable lists */
static int vars_cmp(char *const *a, size_t an, char *const *b, size_t bn) {
    size_t n = an < bn ? an : bn;
    for (size_t i = 0; i < n; i++) {
        int c = strcmp(a[i], b[i]);
        if (c) return c;
    }
    return an < bn ? -1 : an > bn ? 1 : 0;
}

static int term_cmp(const void *x, const void *y) {
    const Term *a = x, *b = y;
    if (a->nvars != b->nvars)
        return a->nvars < b->nvars ? -1 : 1;
    int c = vars_cmp(a->vars, a->nvars, b->vars, b->nvars);
    if (c) return c;
    if (a->coef != b->coef) return a->coef < b->coef ? -1 : 1;
    return 0;
}

static bool term_vars_equal(const Term *a, const Term *b) {
    if (a->nvars != b->nvars) return false;
    for (size_t i = 0; i < a->nvars; i++)
        if (strcmp(a->vars[i], b->vars[i]) != 0) return false;
    return true;
}

/* merge two sorted variable lists into one sorted list */
static char **vars_merge(char *const *a, size_t an, char *const *b, size_t bn,
                         size_t *out_n) {
    char **r = xmalloc(sizeof(char *) * (an + bn ? an + bn : 1));
    size_t i = 0, j = 0, k = 0;
    while (i < an && j < bn) {
        int c = strcmp(a[i], b[j]);
        r[k++] = c <= 0 ? xstrdup(a[i++]) : xstrdup(b[j++]);
    }
    while (i < an) r[k++] = xstrdup(a[i++]);
    while (j < bn) r[k++] = xstrdup(b[j++]);
    *out_n = k;
    return r;
}

/* combine like terms, drop zero coefficients, sort canonically */
static void poly_normalize(Poly *p) {
    qsort(p->terms, p->count, sizeof(Term), term_cmp);
    size_t w = 0;
    for (size_t i = 0; i < p->count; i++) {
        if (w > 0 && term_vars_equal(&p->terms[w - 1], &p->terms[i])) {
            p->terms[w - 1].coef += p->terms[i].coef;
            term_free(&p->terms[i]);
        } else {
            p->terms[w++] = p->terms[i];
        }
    }
    p->count = w;
    w = 0;
    for (size_t i = 0; i < p->count; i++) {
        if (p->terms[i].coef != 0) p->terms[w++] = p->terms[i];
        else term_free(&p->terms[i]);
    }
    p->count = w;
}

static Poly expr_poly(const DimExpr *e);

static Poly poly_add(const Poly *a, const Poly *b) {
    Poly r;
    poly_init(&r);
    r.terms = xmalloc(sizeof(Term) * (a->count + b->count ? a->count + b->count : 1));
    r.cap = a->count + b->count ? a->count + b->count : 1;
    for (size_t i = 0; i < a->count; i++) {
        Term t = a->terms[i];
        t.vars = xmalloc(sizeof(char *) * (t.nvars ? t.nvars : 1));
        for (size_t j = 0; j < t.nvars; j++) t.vars[j] = xstrdup(a->terms[i].vars[j]);
        r.terms[r.count++] = t;
    }
    for (size_t i = 0; i < b->count; i++) {
        Term t = b->terms[i];
        t.vars = xmalloc(sizeof(char *) * (t.nvars ? t.nvars : 1));
        for (size_t j = 0; j < t.nvars; j++) t.vars[j] = xstrdup(b->terms[i].vars[j]);
        r.terms[r.count++] = t;
    }
    poly_normalize(&r);
    return r;
}

/* negated sum (used for dim_le's `b - a`) */
static Poly poly_neg(const Poly *p) {
    Poly r;
    poly_init(&r);
    r.terms = xmalloc(sizeof(Term) * (p->count ? p->count : 1));
    r.cap = p->count ? p->count : 1;
    for (size_t i = 0; i < p->count; i++) {
        Term t = p->terms[i];
        t.coef = -t.coef;
        t.vars = xmalloc(sizeof(char *) * (t.nvars ? t.nvars : 1));
        for (size_t j = 0; j < t.nvars; j++) t.vars[j] = xstrdup(p->terms[i].vars[j]);
        r.terms[r.count++] = t;
    }
    poly_normalize(&r);
    return r;
}

static Poly poly_mul(const Poly *a, const Poly *b) {
    Poly r;
    poly_init(&r);
    for (size_t i = 0; i < a->count; i++)
        for (size_t j = 0; j < b->count; j++) {
            size_t n = 0;
            char **v = vars_merge(a->terms[i].vars, a->terms[i].nvars,
                                  b->terms[j].vars, b->terms[j].nvars, &n);
            poly_push(&r, a->terms[i].coef * b->terms[j].coef, v, n);
        }
    poly_normalize(&r);
    return r;
}

static Poly expr_poly(const DimExpr *e) {
    switch (e->kind) {
    case DE_VAR: {
        Poly p;
        poly_init(&p);
        char **v = xmalloc(sizeof(char *));
        v[0] = xstrdup(e->var);
        poly_push(&p, 1, v, 1);
        return p;
    }
    case DE_LIT: {
        Poly p;
        poly_init(&p);
        poly_push(&p, e->lit, NULL, 0);
        return p;
    }
    case DE_ADD: {
        Poly a = expr_poly(e->lhs), b = expr_poly(e->rhs);
        Poly r = poly_add(&a, &b);
        poly_free(&a);
        poly_free(&b);
        return r;
    }
    case DE_MUL: {
        Poly a = expr_poly(e->lhs), b = expr_poly(e->rhs);
        Poly r = poly_mul(&a, &b);
        poly_free(&a);
        poly_free(&b);
        return r;
    }
    }
    { Poly p; poly_init(&p); return p; }
}

/* --- string rendering --------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } SB;

static void sb_put(SB *sb, const char *s) {
    size_t n = strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 64;
        while (sb->cap < sb->len + n + 1) sb->cap *= 2;
        sb->buf = realloc(sb->buf, sb->cap);
        if (!sb->buf) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    memcpy(sb->buf + sb->len, s, n + 1);
    sb->len += n;
}
static void sb_int(SB *sb, int64_t v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    sb_put(sb, tmp);
}

static void term_str(SB *sb, const Term *t, bool first) {
    if (t->nvars == 0) {
        sb_int(sb, t->coef);
        return;
    }
    if (t->coef == 1) {
        if (!first) sb_put(sb, " + ");
    } else if (t->coef == -1) {
        sb_put(sb, " - ");
    } else if (t->coef < 0) {
        if (!first) sb_put(sb, " - ");
        else sb_put(sb, "-");
        sb_int(sb, -t->coef);
        sb_put(sb, "*");
    } else {
        if (!first) sb_put(sb, " + ");
        sb_int(sb, t->coef);
        sb_put(sb, "*");
    }
    for (size_t i = 0; i < t->nvars; i++) {
        if (i) sb_put(sb, "*");
        sb_put(sb, t->vars[i]);
    }
}

char *dim_str(const DimExpr *e) {
    Poly p = expr_poly(e);
    SB sb = {0};
    if (p.count == 0) {
        sb_put(&sb, "0");
    } else {
        for (size_t i = 0; i < p.count; i++)
            term_str(&sb, &p.terms[i], i == 0);
    }
    poly_free(&p);
    if (!sb.buf) sb_put(&sb, "0");
    return sb.buf;
}

/* --- equality / order --------------------------------------------------- */

static bool poly_equal(const Poly *a, const Poly *b) {
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++) {
        if (a->terms[i].coef != b->terms[i].coef) return false;
        if (!term_vars_equal(&a->terms[i], &b->terms[i])) return false;
    }
    return true;
}

bool dim_eq(const DimExpr *a, const DimExpr *b) {
    Poly pa = expr_poly(a), pb = expr_poly(b);
    bool r = poly_equal(&pa, &pb);
    poly_free(&pa);
    poly_free(&pb);
    return r;
}

/* --- escalation log (D3) ------------------------------------------------ */

static size_t unresolved_count = 0;
#define DIM_LOG_MAX 64
static struct { char *a, *b; } dim_log[DIM_LOG_MAX];
static size_t dim_log_count = 0;

static void dim_log_pair(const DimExpr *a, const DimExpr *b) {
    unresolved_count++;
    if (dim_log_count < DIM_LOG_MAX) {
        dim_log[dim_log_count].a = dim_str(a);
        dim_log[dim_log_count].b = dim_str(b);
        dim_log_count++;
    }
}

size_t dim_unresolved_count(void) { return unresolved_count; }
void dim_reset_unresolved(void) { unresolved_count = 0; }

void dim_log_dump(FILE *out) {
    for (size_t i = 0; i < dim_log_count; i++)
        fprintf(out, "unresolved: %s <= %s\n", dim_log[i].a, dim_log[i].b);
}

int dim_le(const DimExpr *a, const DimExpr *b) {
    if (dim_eq(a, b)) return 1;
    /* decidable fragment: b - a is a non-negative literal */
    Poly pa = expr_poly(a), pb = expr_poly(b);
    Poly na = poly_neg(&pa);
    Poly diff = poly_add(&pb, &na);
    int r = -1;
    if (diff.count == 0) r = 1; /* b - a == 0, but dim_eq already handled that */
    else if (diff.count == 1 && diff.terms[0].nvars == 0) {
        r = diff.terms[0].coef >= 0 ? 1 : 0;
    }
    if (r == -1) dim_log_pair(a, b);
    poly_free(&pa);
    poly_free(&pb);
    poly_free(&na);
    poly_free(&diff);
    return r;
}
