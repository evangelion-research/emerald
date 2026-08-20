/* Checker: type constructors, alias resolution, equality/assignability,
 * tensor shape obligations, and type printing for diagnostics. */
#include "check_internal.h"

struct Alias;

/* Expand alias references to their underlying type (an alias may name another
 * alias; iterate with a bound to guard pathological cycles). */
Type *ty_resolve(const Type *t) {
    for (int i = 0; i < 128 && t->k == TY_ALIAS; i++)
        t = ((const Alias *)t->ref.al)->type;
    return (Type *)t;
}

Type t_any = {.k = TY_ANY}, t_never = {.k = TY_NEVER},
            t_none = {.k = TY_NONE}, t_bool = {.k = TY_BOOL},
            t_int = {.k = TY_INT}, t_float = {.k = TY_FLOAT},
            t_str = {.k = TY_STR};

/* W5/D3: under --proof, `list[T]` assignability is invariant (sound); outside
 * it the covariance inherited from TypeScript still applies but is warned on.
 * assignable() has no Ck argument, so the mode lives in a file-static. */
bool ck_proof_mode = false;

/* W8: --proof-report measurement. Static, reset at the start of each
 * check_program() run, and read back through proof_report_get(). */
size_t proof_rep_total_funcs, proof_rep_partial_funcs,
    proof_rep_pure_funcs;

char **proof_rep_partial_names;

size_t proof_rep_partial_n, proof_rep_partial_cap;

size_t proof_rep_vacuous;      /* W_VACUOUS_PROOF emissions */

size_t proof_rep_covariance;   /* W_UNSOUND_COVARIANCE emissions */

size_t proof_rep_taint_sites;  /* proof-mode tainted-type rejections */

void proof_rep_reset(void) {
    proof_rep_total_funcs = proof_rep_partial_funcs = proof_rep_pure_funcs = 0;
    proof_rep_partial_n = 0;
    proof_rep_vacuous = proof_rep_covariance = proof_rep_taint_sites = 0;
}

Type *ty_new(TyKind k) {
    Type *t = xmalloc(sizeof(Type));
    memset(t, 0, sizeof(Type));
    t->k = k;
    return t;
}

Type *ty_list(Type *elem) {
    Type *t = ty_new(TY_LIST);
    t->elem = elem;
    return t;
}

/* seq[T]: an immutable, covariant sequence (D3 / W5). Sound, unlike list[T]. */
Type *ty_seq(Type *elem) {
    Type *t = ty_new(TY_SEQ);
    t->elem = elem;
    return t;
}

/* A list literal annotated `seq[T]` is the sequence form of its element type:
 * list[T] -> seq[T] (a seq stays a seq). Used by seq-literal contextual typing. */
Type *to_seq(Type *t) {
    Type *r = ty_resolve(t);
    if (r->k == TY_LIST) return ty_seq(r->elem);
    return t;
}

/* Chan[T] / Task[T]: a handle carrying values of type `elem`. */
Type *ty_opaque(const char *name, Type *elem) {
    Type *t = ty_new(TY_OPAQUE);
    t->var = (char *)name;
    t->elem = elem;
    return t;
}

bool is_opaque(const Type *t, const char *name) {
    return t->k == TY_OPAQUE && strcmp(t->var, name) == 0;
}

Type *ty_func(Type **params, size_t count, Type *ret) {
    Type *t = ty_new(TY_FUNC);
    /* own a copy: callers may pass stack arrays (e.g. {t} in a local), and
     * the func type outlives them */
    t->fun.params = xmalloc(sizeof(Type *) * count);
    for (size_t i = 0; i < count; i++)
        t->fun.params[i] = params[i];
    t->fun.count = count;
    t->fun.ret = ret;
    t->fun.eff = EFF_PURE;
    return t;
}

Type *ty_lit_int(int64_t v) {
    Type *t = ty_new(TY_LIT);
    t->lit.base = TY_INT;
    t->lit.ival = v;
    return t;
}

Type *ty_lit_str(char *s) {
    Type *t = ty_new(TY_LIT);
    t->lit.base = TY_STR;
    t->lit.sval = s;
    return t;
}

Type *ty_lit_bool(int64_t v) {
    Type *t = ty_new(TY_LIT);
    t->lit.base = TY_BOOL;
    t->lit.ival = v;
    return t;
}

Type *ty_var(char *name) {
    Type *t = ty_new(TY_VAR);
    t->var = name;
    return t;
}

Shape *shape_dynamic(void) {
    static Shape s = { .dynamic = true, .dims = NULL, .count = 0 };
    return &s;
}

Shape *shape_of(DimExpr **dims, size_t count) {
    Shape *s = xmalloc(sizeof(Shape));
    s->dynamic = false;
    s->dims = dims;
    s->count = count;
    return s;
}

Type *ty_tensor(CDType dt, Shape *shape) {
    Type *t = ty_new(TY_TENSOR);
    t->tensor.dt = dt;
    t->tensor.shape = shape;
    return t;
}

Type *ty_fin(DimExpr *bound) {
    Type *t = ty_new(TY_FIN);
    t->fin = bound;
    return t;
}

Type *ty_eq(DimExpr *lhs, DimExpr *rhs) {
    Type *t = ty_new(TY_EQ);
    t->eq.lhs = lhs;
    t->eq.rhs = rhs;
    return t;
}

/* --- tensor shape obligations ------------------------------------------- */
/* resolve to the underlying TY_TENSOR type, or NULL */
Type *tensor_of(Type *t) {
    Type *r = ty_resolve(t);
    return r->k == TY_TENSOR ? r : NULL;
}

bool dim_is_one(const DimExpr *e) {
    return e->kind == DE_LIT && e->lit == 1;
}

/* product of all axes (1 for a scalar/0-d shape) */
DimExpr *shape_prod(const Shape *s) {
    DimExpr *p = dim_lit(1);
    for (size_t i = 0; i < s->count; i++)
        p = dim_mul(p, s->dims[i]);
    return p;
}

/* result shape of broadcasting `a` and `b`, or NULL when not decidable/valid.
 * Equal ranks only (the strict cut from D3's risk table): any axis must match
 * or be a literal 1. */
Shape *broadcast_shapes(const Shape *a, const Shape *b) {
    if (a->dynamic || b->dynamic) return shape_dynamic();
    if (a->count != b->count) return NULL;
    DimExpr **dims = xmalloc(sizeof(DimExpr *) * a->count);
    for (size_t i = 0; i < a->count; i++) {
        DimExpr *da = a->dims[i], *db = b->dims[i];
        if (dim_eq(da, db)) dims[i] = da;
        else if (dim_is_one(da)) dims[i] = db;
        else if (dim_is_one(db)) dims[i] = da;
        else { free(dims); return NULL; }
    }
    return shape_of(dims, a->count);
}

/* The static shape of a `list` literal whose elements are all int literals
 * (e.g. `[2, 3, 4]`); NULL otherwise. The result is a fresh, owned tree. */
Shape *literal_shape_of_expr(const Expr *e) {
    if (!e || e->kind != E_LIST) return NULL;
    DimExpr **dims = xmalloc(sizeof(DimExpr *) * e->as.list.count);
    for (size_t i = 0; i < e->as.list.count; i++) {
        if (e->as.list.items[i]->kind != E_INT) {
            for (size_t j = 0; j < i; j++) dim_free(dims[j]);
            free(dims);
            return NULL;
        }
        dims[i] = dim_lit(e->as.list.items[i]->as.ival);
    }
    return shape_of(dims, e->as.list.count);
}

/* Record type returned by the gc_stats() builtin (all counters are ints). */
Type *gc_stats_type(void) {
    static Type *t;
    if (!t) {
        static char *names[] = {"collections", "live", "young", "old",
                               "threshold", "bytes_young", "bytes_old"};
        t = ty_new(TY_REC);
        t->rec.count = 7;
        t->rec.names = xmalloc(sizeof(char *) * 7);
        t->rec.types = xmalloc(sizeof(Type *) * 7);
        for (size_t i = 0; i < 7; i++) {
            t->rec.names[i] = names[i];
            t->rec.types[i] = &t_int;
        }
    }
    return t;
}

/* The record task_stats() returns: three counters, like gc_stats(). */
Type *task_stats_type(void) {
    static Type *t;
    if (!t) {
        static char *names[] = {"spawned", "alive", "switches"};
        t = ty_new(TY_REC);
        t->rec.count = 3;
        t->rec.names = xmalloc(sizeof(char *) * 3);
        t->rec.types = xmalloc(sizeof(Type *) * 3);
        for (size_t i = 0; i < 3; i++) {
            t->rec.names[i] = names[i];
            t->rec.types[i] = &t_int;
        }
    }
    return t;
}

static bool eq_seen_sym(const EqVis *v, const Type *a, const Type *b) {
    for (size_t i = 0; i < v->count; i++)
        if ((v->a[i] == a && v->b[i] == b) || (v->a[i] == b && v->b[i] == a))
            return true;
    return false;
}

/* two shapes are equal iff both are dynamic, or both static with dim_eq on
 * each axis (the canonical-form solver decides; see dim.h) */
static bool shape_eq(const Shape *a, const Shape *b) {
    if (a->dynamic || b->dynamic) return a->dynamic && b->dynamic;
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++)
        if (!dim_eq(a->dims[i], b->dims[i])) return false;
    return true;
}

/* W7: dim equalities currently in scope, from `e: Eq[a, b]` bindings (function
 * parameters and locals). Evidence is erased at runtime and only consulted by
 * tensor-shape assignability below. */
const DimExpr *eq_evidence_l[64], *eq_evidence_r[64];

size_t eq_evidence_count;

static bool dim_eq_evid(const DimExpr *a, const DimExpr *b) {
    if (dim_eq(a, b)) return true;
    for (size_t i = 0; i < eq_evidence_count; i++)
        if ((dim_eq(a, eq_evidence_l[i]) && dim_eq(b, eq_evidence_r[i])) ||
            (dim_eq(a, eq_evidence_r[i]) && dim_eq(b, eq_evidence_l[i])))
            return true;
    return false;
}

/* shape equality modulo the Eq[a, b] evidence in scope */
static bool shape_eq_evid(const Shape *a, const Shape *b) {
    if (a->dynamic || b->dynamic) return a->dynamic && b->dynamic;
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++)
        if (!dim_eq_evid(a->dims[i], b->dims[i])) return false;
    return true;
}

static bool type_eq_rec(const Type *a0, const Type *b0, EqVis *v) {
    const Type *a = ty_resolve(a0), *b = ty_resolve(b0);
    if (a == b) return true;
    if (a->k != b->k) return false;
    if (eq_seen_sym(v, a, b)) return true;
    if (v->count < 256) { v->a[v->count] = a; v->b[v->count] = b; v->count++; }
    switch (a->k) {
    case TY_LIST:
        return type_eq_rec(a->elem, b->elem, v);
    case TY_SEQ:
        return type_eq_rec(a->elem, b->elem, v);
    case TY_LIT:
        if (a->lit.base != b->lit.base) return false;
        if (a->lit.base == TY_STR) return strcmp(a->lit.sval, b->lit.sval) == 0;
        return a->lit.ival == b->lit.ival;
    case TY_VAR:
        return strcmp(a->var, b->var) == 0;
    case TY_REC: {
        if (a->rec.count != b->rec.count) return false;
        for (size_t i = 0; i < a->rec.count; i++) {
            bool found = false;
            for (size_t j = 0; j < b->rec.count; j++)
                if (strcmp(a->rec.names[i], b->rec.names[j]) == 0) {
                    if (!type_eq_rec(a->rec.types[i], b->rec.types[j], v))
                        return false;
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    }
    case TY_UNION: {
        if (a->uni.count != b->uni.count) return false;
        for (size_t i = 0; i < a->uni.count; i++) {
            bool found = false;
            for (size_t j = 0; j < b->uni.count; j++)
                if (type_eq_rec(a->uni.alts[i], b->uni.alts[j], v)) {
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    }
    case TY_FUNC: {
        if (a->fun.count != b->fun.count) return false;
        for (size_t i = 0; i < a->fun.count; i++)
            if (!type_eq_rec(a->fun.params[i], b->fun.params[i], v)) return false;
        return type_eq_rec(a->fun.ret, b->fun.ret, v);
    }
    case TY_TENSOR:
        return a->tensor.dt == b->tensor.dt &&
               shape_eq(a->tensor.shape, b->tensor.shape);
    case TY_FIN:
        return dim_eq(a->fin, b->fin);
    case TY_EQ:
        return dim_eq(a->eq.lhs, b->eq.lhs) && dim_eq(a->eq.rhs, b->eq.rhs);
    case TY_OPAQUE:
        return strcmp(a->var, b->var) == 0 && type_eq_rec(a->elem, b->elem, v);
    default:
        return true;
    }
}

bool type_eq(const Type *a, const Type *b) {
    EqVis v = {0};
    return type_eq_rec(a, b, &v);
}

static bool eq_seen_dir(const EqVis *v, const Type *a, const Type *b) {
    for (size_t i = 0; i < v->count; i++)
        if (v->a[i] == a && v->b[i] == b) return true;
    return false;
}

static bool assignable_rec(const Type *dst0, const Type *src0, EqVis *v) {
    const Type *dst = ty_resolve(dst0), *src = ty_resolve(src0);
    if (dst == src) return true;
    if (dst->k == TY_ANY || src->k == TY_ANY) return true;
    if (src->k == TY_NEVER) return true;  /* ⊥ is a subtype of everything */
    if (dst->k == TY_NEVER) return false; /* ...and nothing else fits it */
    if (eq_seen_dir(v, dst, src)) return true; /* coinductive: assume on cycle */
    if (v->count < 256) { v->a[v->count] = dst; v->b[v->count] = src; v->count++; }
    if (src->k == TY_UNION) { /* every alternative must fit */
        for (size_t i = 0; i < src->uni.count; i++)
            if (!assignable_rec(dst, src->uni.alts[i], v)) return false;
        return true;
    }
    if (dst->k == TY_UNION) { /* some alternative must accept it */
        for (size_t i = 0; i < dst->uni.count; i++)
            if (assignable_rec(dst->uni.alts[i], src, v)) return true;
        return false;
    }
    if (dst->k == TY_LIT) /* only the identical literal inhabits a literal */
        return src->k == TY_LIT && type_eq(dst, src);
    if (dst->k == TY_VAR || src->k == TY_VAR)
        return dst->k == TY_VAR && src->k == TY_VAR && type_eq(dst, src);
    /* Fin[n] is inhabited by indices provably below n: Fin[a] <: Fin[b] iff
     * a <= b (the decidable fragment of dim_le); Fin[n] <: int. */
    if (dst->k == TY_FIN) {
        if (src->k != TY_FIN) return false;
        return dim_le(src->fin, dst->fin) == 1;
    }
    if (src->k == TY_FIN) return dst->k == TY_INT;
    /* Eq[a, b] is inhabited only by refl, whose type is Eq[a, a]. A refl
     * marker (NULL sides) fits Eq[a, b] exactly when a == b. */
    if (dst->k == TY_EQ) {
        if (src->k != TY_EQ) return false;
        if (src->eq.lhs == NULL && src->eq.rhs == NULL)
            return dim_eq(dst->eq.lhs, dst->eq.rhs);
        return type_eq(dst, src);
    }
    if (src->k == TY_EQ) return false;
    TyKind sk = src->k == TY_LIT ? src->lit.base : src->k;
    switch (dst->k) {
    case TY_INT:   return sk == TY_INT || sk == TY_BOOL;
    case TY_FLOAT: return sk == TY_FLOAT || sk == TY_INT || sk == TY_BOOL;
    case TY_LIST:
        if (src->k != TY_LIST) return false;
        /* invariant under proof (D3/W5), except the two harmless cases:
         * a fresh literal element (widens, no aliasing) and the `any` escape
         * hatch (handled separately by W2's taint rejection). */
        if (ck_proof_mode && dst->elem->k != TY_ANY && src->elem->k != TY_ANY &&
            !type_is_fresh(src->elem))
            return type_eq(dst->elem, src->elem);
        return assignable_rec(dst->elem, src->elem, v);
    case TY_SEQ:   /* covariant and sound: a seq is immutable */
        return src->k == TY_SEQ && assignable_rec(dst->elem, src->elem, v);
    case TY_REC:   /* structural width subtyping */
        if (src->k != TY_REC) return false;
        for (size_t i = 0; i < dst->rec.count; i++) {
            bool found = false;
            for (size_t j = 0; j < src->rec.count; j++)
                if (strcmp(dst->rec.names[i], src->rec.names[j]) == 0) {
                    if (!assignable_rec(dst->rec.types[i], src->rec.types[j], v))
                        return false;
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    case TY_FUNC: /* invariant params, covariant return */
        if (src->k != TY_FUNC || src->fun.count != dst->fun.count) return false;
        for (size_t i = 0; i < dst->fun.count; i++)
            if (!type_eq(dst->fun.params[i], src->fun.params[i])) return false;
        return assignable_rec(dst->fun.ret, src->fun.ret, v);
    case TY_TENSOR: /* same dtype; dynamic shapes are the shape-level `any` */
        if (src->k != TY_TENSOR || dst->tensor.dt != src->tensor.dt) return false;
        return shape_eq_evid(dst->tensor.shape, src->tensor.shape) ||
               dst->tensor.shape->dynamic || src->tensor.shape->dynamic;
    case TY_OPAQUE:
        /* invariant in the element type -- a Chan[int] handed to code that
         * expects Chan[any] would let that code send a str into it -- except
         * that an unannotated handle (Chan[any], what chan() produces) is the
         * escape hatch in both directions. */
        if (src->k != TY_OPAQUE || strcmp(dst->var, src->var) != 0) return false;
        return type_eq(dst->elem, src->elem) ||
               dst->elem->k == TY_ANY || src->elem->k == TY_ANY;
    default:
        return dst->k == sk;
    }
}

bool assignable(const Type *dst, const Type *src) {
    EqVis v = {0};
    return assignable_rec(dst, src, &v);
}

/* union of two types, flattened and deduplicated */
Type *ty_join(Type *a, Type *b) {
    if (a->k == TY_NEVER) return b;
    if (b->k == TY_NEVER) return a;
    if (type_eq(a, b)) return a;
    if (a->k == TY_ANY || b->k == TY_ANY) return &t_any;
    Type **alts = xmalloc(sizeof(Type *) *
        ((a->k == TY_UNION ? a->uni.count : 1) +
         (b->k == TY_UNION ? b->uni.count : 1)));
    size_t n = 0;
    Type *parts[2] = {a, b};
    for (int p = 0; p < 2; p++) {
        Type *t = parts[p];
        size_t cnt = t->k == TY_UNION ? t->uni.count : 1;
        for (size_t i = 0; i < cnt; i++) {
            Type *alt = t->k == TY_UNION ? t->uni.alts[i] : t;
            bool dup = false;
            for (size_t j = 0; j < n; j++)
                if (type_eq(alts[j], alt)) { dup = true; break; }
            if (!dup) alts[n++] = alt;
        }
    }
    if (n == 1) return alts[0];
    Type *u = ty_new(TY_UNION);
    u->uni.alts = alts;
    u->uni.count = n;
    return u;
}

/* union of an alternative list (0 alternatives = never) */
Type *ty_union_of(Type **alts, size_t n) {
    if (n == 0) return &t_never;
    Type *t = alts[0];
    for (size_t i = 1; i < n; i++) t = ty_join(t, alts[i]);
    return t;
}

static Type *lit_base(const Type *t) {
    switch (t->lit.base) {
    case TY_INT:  return &t_int;
    case TY_STR:  return &t_str;
    case TY_BOOL: return &t_bool;
    default:      return (Type *)t;
    }
}

/* collapse literal types to their base for operators: "a" -> str, 1|2 -> int.
 * Applies regardless of freshness (a Dice = 1|..|6 still adds like an int). */
Type *ty_base(Type *t) {
    if (t->k == TY_ALIAS) return ty_base(ty_resolve(t));
    if (t->k == TY_LIT) return lit_base(t);
    if (t->k == TY_UNION) {
        bool has_lit = false;
        for (size_t i = 0; i < t->uni.count; i++)
            if (t->uni.alts[i]->k == TY_LIT) { has_lit = true; break; }
        if (!has_lit) return t;
        Type *r = &t_never;
        for (size_t i = 0; i < t->uni.count; i++)
            r = ty_join(r, ty_base(t->uni.alts[i]));
        return r;
    }
    return t;
}

/* literal widening: 3 -> int, "a"|"b" -> str, applied recursively — but only
 * to FRESH literals (those inferred from literal expressions). Literals that
 * came from annotations (discriminant fields, `type Dice = 1|...`) survive,
 * so iterating a list[Shape] keeps the union narrowable. Used when binding
 * unannotated variables and inferring generic type arguments, so mutation
 * stays convenient (TS `let` widening). */
Type *widen(Type *t) {
    switch (t->k) {
    case TY_LIT:
        return t->fresh ? lit_base(t) : t;
    case TY_LIST: {
        Type *e = widen(t->elem);
        return e == t->elem ? t : ty_list(e);
    }
    case TY_SEQ: {
        Type *e = widen(t->elem);
        return e == t->elem ? t : ty_seq(e);
    }
    case TY_REC: {
        bool changed = false;
        for (size_t i = 0; i < t->rec.count; i++)
            if (widen(t->rec.types[i]) != t->rec.types[i]) { changed = true; break; }
        if (!changed) return t;
        Type *r = ty_new(TY_REC);
        r->rec.names = t->rec.names;
        r->rec.count = t->rec.count;
        r->rec.types = xmalloc(sizeof(Type *) * t->rec.count);
        for (size_t i = 0; i < t->rec.count; i++)
            r->rec.types[i] = widen(t->rec.types[i]);
        return r;
    }
    case TY_UNION: {
        Type *r = &t_never;
        for (size_t i = 0; i < t->uni.count; i++)
            r = ty_join(r, widen(t->uni.alts[i]));
        return r;
    }
    default:
        return t;
    }
}

/* --- printing types in error messages ----------------------------------- */
static void type_write(char *buf, size_t cap, const Type *t);

static void tw_append(char *buf, size_t cap, const char *s) {
    size_t l = strlen(buf);
    if (l + 1 < cap) snprintf(buf + l, cap - l, "%s", s);
}

static void type_write(char *buf, size_t cap, const Type *t) {
    char tmp[32];
    switch (t->k) {
    case TY_ANY:   tw_append(buf, cap, "any"); break;
    case TY_NEVER: tw_append(buf, cap, "never"); break;
    case TY_NONE:  tw_append(buf, cap, "None"); break;
    case TY_BOOL:  tw_append(buf, cap, "bool"); break;
    case TY_INT:   tw_append(buf, cap, "int"); break;
    case TY_FLOAT: tw_append(buf, cap, "float"); break;
    case TY_STR:   tw_append(buf, cap, "str"); break;
    case TY_VAR:   tw_append(buf, cap, t->var); break;
    case TY_ALIAS: tw_append(buf, cap, ((const Alias *)t->ref.al)->disp); break;
    case TY_FUNC:
        tw_append(buf, cap, "(");
        for (size_t i = 0; i < t->fun.count; i++) {
            if (i) tw_append(buf, cap, ", ");
            type_write(buf, cap, t->fun.params[i]);
        }
        tw_append(buf, cap, ") -> ");
        type_write(buf, cap, t->fun.ret);
        break;
    case TY_LIT:
        switch (t->lit.base) {
        case TY_INT:
            snprintf(tmp, sizeof tmp, "%lld", (long long)t->lit.ival);
            tw_append(buf, cap, tmp);
            break;
        case TY_STR:
            tw_append(buf, cap, "\"");
            tw_append(buf, cap, t->lit.sval);
            tw_append(buf, cap, "\"");
            break;
        default:
            tw_append(buf, cap, t->lit.ival ? "True" : "False");
            break;
        }
        break;
    case TY_LIST:
        tw_append(buf, cap, "list[");
        type_write(buf, cap, t->elem);
        tw_append(buf, cap, "]");
        break;
    case TY_SEQ:
        tw_append(buf, cap, "seq[");
        type_write(buf, cap, t->elem);
        tw_append(buf, cap, "]");
        break;
    case TY_REC:
        tw_append(buf, cap, "{");
        for (size_t i = 0; i < t->rec.count; i++) {
            if (i) tw_append(buf, cap, ", ");
            tw_append(buf, cap, t->rec.names[i]);
            tw_append(buf, cap, ": ");
            type_write(buf, cap, t->rec.types[i]);
        }
        tw_append(buf, cap, "}");
        break;
    case TY_UNION:
        for (size_t i = 0; i < t->uni.count; i++) {
            if (i) tw_append(buf, cap, " | ");
            type_write(buf, cap, t->uni.alts[i]);
        }
        break;
    case TY_OPAQUE:
        tw_append(buf, cap, t->var);
        tw_append(buf, cap, "[");
        type_write(buf, cap, t->elem);
        tw_append(buf, cap, "]");
        break;
    case TY_TENSOR: {
        tw_append(buf, cap, "Tensor[");
        tw_append(buf, cap, t->tensor.dt == CDT_F64 ? "f64" : "f32");
        tw_append(buf, cap, ", ");
        const Shape *sh = t->tensor.shape;
        if (sh->dynamic) {
            tw_append(buf, cap, "?");
        } else {
            tw_append(buf, cap, "[");
            for (size_t i = 0; i < sh->count; i++) {
                if (i) tw_append(buf, cap, ", ");
                char *s = dim_str(sh->dims[i]);
                tw_append(buf, cap, s);
                free(s);
            }
            tw_append(buf, cap, "]");
        }
        tw_append(buf, cap, "]");
        break;
    }
    case TY_FIN: {
        tw_append(buf, cap, "Fin[");
        char *s = dim_str(t->fin);
        tw_append(buf, cap, s);
        free(s);
        tw_append(buf, cap, "]");
        break;
    }
    case TY_EQ: {
        if (!t->eq.lhs) {
            tw_append(buf, cap, "refl"); /* the erased refl marker */
            break;
        }
        tw_append(buf, cap, "Eq[");
        char *a = dim_str(t->eq.lhs);
        char *b = dim_str(t->eq.rhs);
        tw_append(buf, cap, a);
        tw_append(buf, cap, ", ");
        tw_append(buf, cap, b);
        free(a);
        free(b);
        tw_append(buf, cap, "]");
        break;
    }
    }
}

const char *type_str(const Type *t) {
    static char bufs[4][512]; /* rotate so two types can appear in one message */
    static int which = 0;
    char *b = bufs[which];
    which = (which + 1) % 4;
    b[0] = '\0';
    type_write(b, 512, t);
    return b;
}
