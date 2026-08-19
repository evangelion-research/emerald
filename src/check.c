/* Emerald type checker: gradual, structural typing in the TypeScript spirit.
 *
 * - Unannotated code is `any` and checks like dynamic Python.
 * - `type Name = {...}` declares a structural alias. Assignability between
 *   records is *width subtyping*: a record with more fields is assignable to
 *   a record type with fewer ("inheritance" without classes).
 * - `A & B` merges two record types (intersection). `A | B` is a union.
 * - Literal types (`3`, `"red"`, `True`) are singleton types; `never` is the
 *   empty type. Together with flow narrowing (`if x == None`, discriminant
 *   fields) they give exhaustiveness proofs: in the impossible branch a value
 *   has type `never`.
 * - Generic aliases (`type Pair[A, B]`) and functions (`def head[T]`) are
 *   checked by unification at each call site; codegen is untyped so no
 *   monomorphization is needed.
 * - Lists are covariant (like TS arrays: convenient, mildly unsound).
 *
 * The checker never mutates the AST; codegen is untyped and independent.
 */
#include "check.h"
#include "diag.h"
#include "dim.h"
#include "xalloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TY_ANY, TY_NEVER, TY_NONE, TY_BOOL, TY_INT, TY_FLOAT, TY_STR,
    TY_LIT, TY_LIST, TY_REC, TY_UNION, TY_VAR, TY_ALIAS, TY_FUNC,
    TY_TENSOR, TY_FIN,
} TyKind;

/* tensor dtype tags used by the checker (the runtime's DType is independent) */
typedef enum { CDT_F32, CDT_F64 } CDType;

/* A tensor shape: a list of canonical dim expressions, or the dynamic
 * escape hatch `?` (Tensor[f32, ?]). `dims` is NULL when dynamic. */
typedef struct Shape {
    bool dynamic;
    DimExpr **dims;
    size_t count;
} Shape;

typedef struct Type Type;
struct Alias;
struct Type {
    TyKind k;
    bool fresh; /* literal born from a literal expression: widens on binding */
    Type *elem;                                             /* TY_LIST */
    struct { char **names; Type **types; size_t count; } rec; /* TY_REC */
    struct { Type **alts; size_t count; } uni;              /* TY_UNION */
    struct { TyKind base; int64_t ival; char *sval; } lit;  /* TY_LIT */
    char *var;                                              /* TY_VAR */
    struct { Type **params; Type *ret; size_t count; } fun; /* TY_FUNC */
    struct { CDType dt; Shape *shape; } tensor;             /* TY_TENSOR */
    DimExpr *fin;                                            /* TY_FIN */
    /* TY_ALIAS: a reference to a named alias. A self-reference encountered
     * while an alias body is being resolved becomes this node (see resolve_name). */
    struct { const struct Alias *al; Type **args; size_t argc; } ref;
};

/* A named type alias. Non-generic aliases resolve eagerly; a self-reference
 * encountered while resolving becomes a TY_ALIAS node instead. */
typedef struct Alias {
    char *name;
    const char *disp;           /* source-level name (linking may mangle `name`) */
    Type *type;                 /* resolved eagerly for non-generic aliases */
    char **params;              /* generic parameters, NULL when non-generic */
    bool *param_dims;           /* parallel: true when `P: dim` (a dimension) */
    size_t param_count;
    const TypeExpr *body;       /* unresolved body for generic aliases */
    bool resolving;             /* guard: currently resolving this alias's body */
} Alias;

/* Expand alias references to their underlying type (an alias may name another
 * alias; iterate with a bound to guard pathological cycles). */
static Type *ty_resolve(const Type *t) {
    for (int i = 0; i < 128 && t->k == TY_ALIAS; i++)
        t = ((const Alias *)t->ref.al)->type;
    return (Type *)t;
}

static Type t_any = {.k = TY_ANY}, t_never = {.k = TY_NEVER},
            t_none = {.k = TY_NONE}, t_bool = {.k = TY_BOOL},
            t_int = {.k = TY_INT}, t_float = {.k = TY_FLOAT},
            t_str = {.k = TY_STR};


static Type *ty_new(TyKind k) {
    Type *t = xmalloc(sizeof(Type));
    memset(t, 0, sizeof(Type));
    t->k = k;
    return t;
}

static Type *ty_list(Type *elem) {
    Type *t = ty_new(TY_LIST);
    t->elem = elem;
    return t;
}

static Type *ty_func(Type **params, size_t count, Type *ret) {
    Type *t = ty_new(TY_FUNC);
    /* own a copy: callers may pass stack arrays (e.g. {t} in a local), and
     * the func type outlives them */
    t->fun.params = xmalloc(sizeof(Type *) * count);
    for (size_t i = 0; i < count; i++)
        t->fun.params[i] = params[i];
    t->fun.count = count;
    t->fun.ret = ret;
    return t;
}

static Type *ty_lit_int(int64_t v) {
    Type *t = ty_new(TY_LIT);
    t->lit.base = TY_INT;
    t->lit.ival = v;
    return t;
}

static Type *ty_lit_str(char *s) {
    Type *t = ty_new(TY_LIT);
    t->lit.base = TY_STR;
    t->lit.sval = s;
    return t;
}

static Type *ty_lit_bool(int64_t v) {
    Type *t = ty_new(TY_LIT);
    t->lit.base = TY_BOOL;
    t->lit.ival = v;
    return t;
}

static Type *ty_var(char *name) {
    Type *t = ty_new(TY_VAR);
    t->var = name;
    return t;
}

static Shape *shape_dynamic(void) {
    static Shape s = { .dynamic = true, .dims = NULL, .count = 0 };
    return &s;
}

static Shape *shape_of(DimExpr **dims, size_t count) {
    Shape *s = xmalloc(sizeof(Shape));
    s->dynamic = false;
    s->dims = dims;
    s->count = count;
    return s;
}

static Type *ty_tensor(CDType dt, Shape *shape) {
    Type *t = ty_new(TY_TENSOR);
    t->tensor.dt = dt;
    t->tensor.shape = shape;
    return t;
}

static Type *ty_fin(DimExpr *bound) {
    Type *t = ty_new(TY_FIN);
    t->fin = bound;
    return t;
}

/* --- tensor shape obligations (SPEC_V2.md W4) --------------------------- */

/* resolve to the underlying TY_TENSOR type, or NULL */
static Type *tensor_of(Type *t) {
    Type *r = ty_resolve(t);
    return r->k == TY_TENSOR ? r : NULL;
}

static bool dim_is_one(const DimExpr *e) {
    return e->kind == DE_LIT && e->lit == 1;
}

/* product of all axes (1 for a scalar/0-d shape) */
static DimExpr *shape_prod(const Shape *s) {
    DimExpr *p = dim_lit(1);
    for (size_t i = 0; i < s->count; i++)
        p = dim_mul(p, s->dims[i]);
    return p;
}

/* result shape of broadcasting `a` and `b`, or NULL when not decidable/valid.
 * Equal ranks only (the strict cut from D3's risk table): any axis must match
 * or be a literal 1. */
static Shape *broadcast_shapes(const Shape *a, const Shape *b) {
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
static Shape *literal_shape_of_expr(const Expr *e) {
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
static Type *gc_stats_type(void) {
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

/* --- type equality / assignability -------------------------------------- */

/* Cycle-safe type comparison: resolve alias references, then recurse with a
 * memoized set of in-progress pairs (coinductive equality). */
typedef struct {
    const Type *a[256], *b[256];
    size_t count;
} EqVis;

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

static bool type_eq_rec(const Type *a0, const Type *b0, EqVis *v) {
    const Type *a = ty_resolve(a0), *b = ty_resolve(b0);
    if (a == b) return true;
    if (a->k != b->k) return false;
    if (eq_seen_sym(v, a, b)) return true;
    if (v->count < 256) { v->a[v->count] = a; v->b[v->count] = b; v->count++; }
    switch (a->k) {
    case TY_LIST:
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
    default:
        return true;
    }
}

static bool type_eq(const Type *a, const Type *b) {
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
    TyKind sk = src->k == TY_LIT ? src->lit.base : src->k;
    switch (dst->k) {
    case TY_INT:   return sk == TY_INT || sk == TY_BOOL;
    case TY_FLOAT: return sk == TY_FLOAT || sk == TY_INT || sk == TY_BOOL;
    case TY_LIST:  return src->k == TY_LIST && assignable_rec(dst->elem, src->elem, v);
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
        return shape_eq(dst->tensor.shape, src->tensor.shape) ||
               dst->tensor.shape->dynamic || src->tensor.shape->dynamic;
    default:
        return dst->k == sk;
    }
}

static bool assignable(const Type *dst, const Type *src) {
    EqVis v = {0};
    return assignable_rec(dst, src, &v);
}

/* union of two types, flattened and deduplicated */
static Type *ty_join(Type *a, Type *b) {
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
static Type *ty_union_of(Type **alts, size_t n) {
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
static Type *ty_base(Type *t) {
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
static Type *widen(Type *t) {
    switch (t->k) {
    case TY_LIT:
        return t->fresh ? lit_base(t) : t;
    case TY_LIST: {
        Type *e = widen(t->elem);
        return e == t->elem ? t : ty_list(e);
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
    }
}

static const char *type_str(const Type *t) {
    static char bufs[4][512]; /* rotate so two types can appear in one message */
    static int which = 0;
    char *b = bufs[which];
    which = (which + 1) % 4;
    b[0] = '\0';
    type_write(b, 512, t);
    return b;
}

/* --- checker context ----------------------------------------------------- */

typedef struct {
    char *name;
    Type *decl;     /* declared (or widened inferred) type: assignments check this */
    Type *type;     /* current flow-narrowed type: reads see this */
    bool annotated; /* explicit annotations are enforced; inferred ones widen */
    bool bound;     /* false until the first assignment executes */
    bool is_const;  /* declared `const`: reassignment is an error */
    const char *file; /* for globals: the declaring module. An assignment from
                       * another module's function body makes a local, not a
                       * clobber — see updatable_global() */
    int gen;        /* bumped on assignment; invalidates stale narrowings */
} Var;

typedef struct { Var *items; size_t count, cap; } VarEnv;

typedef struct {
    char *name;
    const char *disp;    /* source-level name (differs when a module was linked) */
    char **tparams;      /* generic type parameters, e.g. def head[T] */
    size_t tparam_count;
    Type **params;       /* may contain TY_VAR when generic */
    size_t param_count;
    Type *ret;
    bool pure;           /* declared `pure`: may only call pure functions/builtins */
    bool partial;        /* declared `partial`: exempt from termination checking */
    const Stmt *node;
} FuncSig;

/* type-variable environment used while resolving type expressions */
typedef struct { char **names; Type **types; size_t count; } TyEnv;

/* A lexical scope for one function body: its locals plus the nested `def`s
 * registered directly within it. Scopes chain to the enclosing function, so a
 * nested function can read (capture) enclosing locals and call sibling
 * functions by name. */
typedef struct Scope {
    VarEnv locals;
    FuncSig *funcs;        /* nested `def`s registered in this scope */
    size_t func_count, func_cap;
    struct Scope *parent;  /* enclosing function scope; NULL for a top def */
    bool pure;             /* purity of the enclosing def; nested defs must match */
} Scope;

typedef struct {
    const char *filename;
    DiagList *diags;  /* where diagnostics are collected */
    int errors;
    Alias *aliases;
    size_t alias_count, alias_cap;
    int alias_depth;  /* guards recursive generic alias expansion */
    FuncSig *funcs;   /* top-level function signatures */
    size_t func_count;
    VarEnv globals;
    Scope *scope;     /* current function scope; NULL at top level */
    Type *cur_ret;    /* declared return type of the current function */
    bool cur_pure;    /* purity of the function whose body is being checked */
    bool proof;       /* --proof: `any` and `partial` are banned */
    bool in_sig;      /* resolving a function signature: proof `any` reports here */
    TyEnv *tyenv;     /* type parameters of the function being checked: in
                       * scope for annotations in its body as well as its
                       * signature (`out: list[T] = []` inside def f[T]) */
    int loop_depth;
    /* --- shape system state (Phase 2 / W4) --- */
    char **dim_names;          /* module-level `dim` declarations */
    size_t dim_count, dim_cap;
    char **dim_params;         /* `B: dim` type parameters in scope */
    size_t dim_param_count;
    char **dim_sub_names;      /* active dim-substitution env (generic aliases) */
    DimExpr **dim_sub_values;
    size_t dim_sub_count;
} Ck;

/* D4: how many static<->dynamic shape crossings were inserted this run */
static size_t shape_dyn_crossings = 0;

static void note_shape_crossing(Ck *ck, const Type *dst, const Type *src) {
    (void)ck;
    Type *d = ty_resolve(dst), *s = ty_resolve(src);
    if (d->k == TY_TENSOR && s->k == TY_TENSOR &&
        d->tensor.dt == s->tensor.dt &&
        !d->tensor.shape->dynamic && s->tensor.shape->dynamic)
        shape_dyn_crossings++;
}

static void ck_error(Ck *ck, const char *code, int line, int col,
                     const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_add(ck->diags, DIA_TYPE, code, ck->filename, line, col, "%s", msg);
    ck->errors++;
}

/* A type mismatch: attach the expected/actual types as structured fields so
 * machine consumers (and LLMs) can read them without parsing the prose. */
static void ck_error_t(Ck *ck, const char *code, int line, int col,
                       const Type *expected, const Type *actual,
                       const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Diag *d = diag_add(ck->diags, DIA_TYPE, code, ck->filename, line, col,
                       "%s", msg);
    diag_set_types(d, expected ? type_str(expected) : NULL,
                   actual ? type_str(actual) : NULL);
    ck->errors++;
}

static Var *env_find(VarEnv *env, const char *name) {
    for (size_t i = 0; i < env->count; i++)
        if (strcmp(env->items[i].name, name) == 0) return &env->items[i];
    return NULL;
}

static Var *env_add(VarEnv *env, const char *name, Type *t, bool annotated) {
    if (env->count == env->cap) {
        env->cap = env->cap ? env->cap * 2 : 8;
        env->items = xrealloc(env->items, sizeof(Var) * env->cap);
    }
    Var *v = &env->items[env->count++];
    v->name = (char *)name;
    v->decl = t;
    v->type = t;
    v->annotated = annotated;
    v->bound = false;
    v->is_const = false;
    v->file = NULL;
    v->gen = 0;
    return v;
}

static bool updatable_global(Ck *ck, const char *name, const char *file);

static Var *lookup_var(Ck *ck, const char *name) {
    for (Scope *sc = ck->scope; sc; sc = sc->parent) {
        Var *v = env_find(&sc->locals, name);
        if (v) return v;
    }
    return env_find(&ck->globals, name);
}

/* the shared builtin table (include/builtins.def): `pure` marks the builtins
 * a `pure` Emerald function is allowed to call. The per-builtin typing rules
 * live in infer_call(). */
static const struct { const char *name; bool pure; } builtins[] = {
#define EM_BUILTIN(n, c, a, p) { n, p },
#include "builtins.def"
#undef EM_BUILTIN
#undef EM_BUILTIN_VOID
};

static const char *builtin_find(const char *name, bool *pure) {
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (strcmp(builtins[i].name, name) == 0) {
            if (pure) *pure = builtins[i].pure;
            return builtins[i].name;
        }
    return NULL;
}

static bool is_builtin(const char *name) {
    return builtin_find(name, NULL) != NULL;
}

static FuncSig *find_func(Ck *ck, const char *name) {
    for (Scope *sc = ck->scope; sc; sc = sc->parent)
        for (size_t i = 0; i < sc->func_count; i++)
            if (strcmp(sc->funcs[i].name, name) == 0) return &sc->funcs[i];
    for (size_t i = 0; i < ck->func_count; i++)
        if (strcmp(ck->funcs[i].name, name) == 0) return &ck->funcs[i];
    return NULL;
}

/* --- resolving surface type expressions ---------------------------------- */

static Type *resolve_type(Ck *ck, const TypeExpr *te, const TyEnv *env);

/* is `name` a dim variable in scope: a `: dim` parameter or a declared `dim`? */
static bool dim_in_scope(Ck *ck, const char *name) {
    for (size_t i = 0; i < ck->dim_param_count; i++)
        if (strcmp(ck->dim_params[i], name) == 0) return true;
    for (size_t i = 0; i < ck->dim_count; i++)
        if (strcmp(ck->dim_names[i], name) == 0) return true;
    return false;
}

/* Interpret a type-expression argument as a dimension expression (the value
 * passed for a `: dim` parameter). Only bare dim names and int literals are
 * supported in type-argument position; richer arithmetic belongs inside a
 * `[...]` shape list. */
static DimExpr *type_expr_to_dim(Ck *ck, const TypeExpr *te) {
    if (!te) return NULL;
    if (te->kind == TE_NAME) {
        if (!dim_in_scope(ck, te->name))
            ck_error(ck, "E_SHAPE_UNKNOWN_DIM", te->line, te->col,
                     "unknown dimension '%s' (declare it with `dim`, or bind "
                     "it as a `: dim` parameter)", te->name);
        return dim_var(te->name);
    }
    if (te->kind == TE_LIT && te->lit.kind == LIT_INT)
        return dim_lit(te->lit.ival);
    ck_error(ck, "E_SHAPE_DIM_ARG", te->line, te->col,
             "dimension argument must be a dim name or an int literal");
    return NULL;
}

/* Resolve a dimension expression: apply the active substitution env, then
 * report any variable that is neither a dim parameter nor a declared dim.
 * Returns a fresh tree (the caller owns it). */
static DimExpr *resolve_dim(Ck *ck, const DimExpr *e, int line, int col) {
    if (!e) return NULL;
    switch (e->kind) {
    case DE_VAR:
        if (ck->dim_sub_count)
            for (size_t i = 0; i < ck->dim_sub_count; i++)
                if (strcmp(e->var, ck->dim_sub_names[i]) == 0)
                    return dim_clone(ck->dim_sub_values[i]);
        if (!dim_in_scope(ck, e->var))
            ck_error(ck, "E_SHAPE_UNKNOWN_DIM", line, col,
                     "unknown dimension '%s' (declare it with `dim`, or bind "
                     "it as a `: dim` parameter)", e->var);
        return dim_var(e->var);
    case DE_LIT:
        return dim_lit(e->lit);
    case DE_ADD:
        return dim_add(resolve_dim(ck, e->lhs, line, col),
                       resolve_dim(ck, e->rhs, line, col));
    case DE_MUL:
        return dim_mul(resolve_dim(ck, e->lhs, line, col),
                       resolve_dim(ck, e->rhs, line, col));
    }
    return NULL;
}

static Type *resolve_name(Ck *ck, const TypeExpr *te, const TyEnv *env) {
    /* type variables in scope shadow aliases and builtins */
    if (env)
        for (size_t i = 0; i < env->count; i++)
            if (strcmp(env->names[i], te->name) == 0) {
                if (te->arg_count) {
                    ck_error(ck, "E_TYPE_NOT_GENERIC", te->line, te->col,
                             "type parameter '%s' is not generic", te->name);
                    return &t_any;
                }
                return env->types[i];
            }
    Type *builtin = NULL;
    if (strcmp(te->name, "any") == 0) {
        if (ck->proof && ck->in_sig)
            ck_error(ck, "E_PROOF_ANY", te->line, te->col,
                     "'any' is banned in proof mode: annotate this type");
        builtin = &t_any;
    } else if (strcmp(te->name, "never") == 0) builtin = &t_never;
    else if (strcmp(te->name, "None") == 0) builtin = &t_none;
    else if (strcmp(te->name, "bool") == 0) builtin = &t_bool;
    else if (strcmp(te->name, "int") == 0) builtin = &t_int;
    else if (strcmp(te->name, "float") == 0) builtin = &t_float;
    else if (strcmp(te->name, "str") == 0) builtin = &t_str;
    if (builtin) {
        if (te->arg_count) {
            ck_error(ck, "E_TYPE_NOT_GENERIC", te->line, te->col,
                     "type '%s' is not generic", te->name);
            return &t_any;
        }
        return builtin;
    }
    for (size_t i = 0; i < ck->alias_count; i++) {
        Alias *al = &ck->aliases[i];
        if (strcmp(al->name, te->name) != 0) continue;
        if (al->param_count == 0) {
            if (te->arg_count) {
                ck_error(ck, "E_TYPE_NOT_GENERIC", te->line, te->col,
                         "type '%s' is not generic", al->disp);
                return &t_any;
            }
            if (al->resolving) { /* recursive self-reference */
                Type *r = ty_new(TY_ALIAS);
                r->ref.al = al;
                r->ref.args = NULL;
                r->ref.argc = 0;
                return r;
            }
            return al->type;
        }
        if (te->arg_count != al->param_count) {
            ck_error(ck, "E_TYPE_ARITY", te->line, te->col,
                     "generic type '%s' takes %zu type argument%s, got %zu",
                     te->name, al->param_count,
                     al->param_count == 1 ? "" : "s", te->arg_count);
            return &t_any;
        }
        if (ck->alias_depth > 32) {
            ck_error(ck, "E_TYPE_RECURSIVE_GENERIC", te->line, te->col,
                     "recursive generic type '%s' is not supported", te->name);
            return &t_any;
        }
        /* partition parameters into type variables and `: dim` dimensions */
        size_t ndims = 0;
        for (size_t j = 0; j < al->param_count; j++)
            if (al->param_dims && al->param_dims[j]) ndims++;
        size_t ntypes = al->param_count - ndims;
        TyEnv sub;
        sub.count = ntypes;
        sub.names = xmalloc(sizeof(char *) * ntypes);
        sub.types = xmalloc(sizeof(Type *) * ntypes);
        char **dn = xmalloc(sizeof(char *) * ndims);
        DimExpr **dv = xmalloc(sizeof(DimExpr *) * ndims);
        size_t ti = 0, di = 0;
        for (size_t j = 0; j < al->param_count; j++) {
            bool isdim = al->param_dims && al->param_dims[j];
            if (isdim) {
                dn[di] = al->params[j];
                dv[di] = type_expr_to_dim(ck, te->args[j]);
                di++;
            } else {
                sub.names[ti] = al->params[j];
                sub.types[ti] = resolve_type(ck, te->args[j], env);
                ti++;
            }
        }
        char **saved_dn = ck->dim_sub_names;
        DimExpr **saved_dv = ck->dim_sub_values;
        size_t saved_dc = ck->dim_sub_count;
        ck->dim_sub_names = dn;
        ck->dim_sub_values = dv;
        ck->dim_sub_count = ndims;
        ck->alias_depth++;
        Type *r = resolve_type(ck, al->body, &sub);
        ck->alias_depth--;
        ck->dim_sub_names = saved_dn;
        ck->dim_sub_values = saved_dv;
        ck->dim_sub_count = saved_dc;
        free(sub.names);
        free(sub.types);
        free(dn);
        free(dv);
        return r;
    }
    ck_error(ck, "E_TYPE_UNKNOWN_TYPE", te->line, te->col,
             "unknown type '%s'", te->name);
    return &t_any;
}

static Type *resolve_type(Ck *ck, const TypeExpr *te, const TyEnv *env) {
    if (!te) return &t_any;
    switch (te->kind) {
    case TE_NAME:
        return resolve_name(ck, te, env);
    case TE_LIT:
        switch (te->lit.kind) {
        case LIT_INT:  return ty_lit_int(te->lit.ival);
        case LIT_STR:  return ty_lit_str(te->lit.sval);
        case LIT_BOOL: return ty_lit_bool(te->lit.ival);
        case LIT_NONE: return &t_none;
        }
        return &t_any;
    case TE_LIST:
        return ty_list(resolve_type(ck, te->elem, env));
    case TE_REC: {
        Type *t = ty_new(TY_REC);
        t->rec.names = te->fields.names;
        t->rec.types = xmalloc(sizeof(Type *) * te->fields.count);
        t->rec.count = te->fields.count;
        for (size_t i = 0; i < te->fields.count; i++)
            t->rec.types[i] = resolve_type(ck, te->fields.types[i], env);
        return t;
    }
    case TE_UNION:
        return ty_join(resolve_type(ck, te->lhs, env), resolve_type(ck, te->rhs, env));
    case TE_FUNC: {
        Type **params = xmalloc(sizeof(Type *) * te->fun.param_count);
        for (size_t i = 0; i < te->fun.param_count; i++)
            params[i] = resolve_type(ck, te->fun.params[i], env);
        return ty_func(params, te->fun.param_count, resolve_type(ck, te->fun.ret, env));
    }
    case TE_TENSOR: {
        CDType dt = CDT_F32;
        if (te->tensor.dtype && te->tensor.dtype->kind == TE_NAME) {
            if (strcmp(te->tensor.dtype->name, "f64") == 0) dt = CDT_F64;
            else if (strcmp(te->tensor.dtype->name, "f32") == 0) dt = CDT_F32;
            else
                ck_error(ck, "E_SHAPE_DTYPE", te->line, te->col,
                         "unknown tensor dtype '%s' (Phase 2 supports f32 and f64)",
                         te->tensor.dtype->name);
        }
        if (te->tensor.dynamic)
            return ty_tensor(dt, shape_dynamic());
        DimExpr **dims = xmalloc(sizeof(DimExpr *) *
                                 te->tensor.shape_count);
        for (size_t i = 0; i < te->tensor.shape_count; i++)
            dims[i] = resolve_dim(ck, te->tensor.shape[i], te->line, te->col);
        return ty_tensor(dt, shape_of(dims, te->tensor.shape_count));
    }
    case TE_FIN:
        return ty_fin(resolve_dim(ck, te->fin_dim, te->line, te->col));
    case TE_INTER: {
        Type *a = resolve_type(ck, te->lhs, env);
        Type *b = resolve_type(ck, te->rhs, env);
        if (a->k != TY_REC || b->k != TY_REC) {
            ck_error(ck, "E_TYPE_INTERSECTION", te->line, te->col,
                     "'&' requires two record types, got %s and %s",
                     type_str(a), type_str(b));
            return &t_any;
        }
        /* merge; fields from the right side override */
        Type *t = ty_new(TY_REC);
        size_t max = a->rec.count + b->rec.count;
        t->rec.names = xmalloc(sizeof(char *) * max);
        t->rec.types = xmalloc(sizeof(Type *) * max);
        for (size_t i = 0; i < a->rec.count; i++) {
            t->rec.names[t->rec.count] = a->rec.names[i];
            t->rec.types[t->rec.count] = a->rec.types[i];
            t->rec.count++;
        }
        for (size_t i = 0; i < b->rec.count; i++) {
            bool replaced = false;
            for (size_t j = 0; j < t->rec.count; j++)
                if (strcmp(t->rec.names[j], b->rec.names[i]) == 0) {
                    t->rec.types[j] = b->rec.types[i];
                    replaced = true;
                    break;
                }
            if (!replaced) {
                t->rec.names[t->rec.count] = b->rec.names[i];
                t->rec.types[t->rec.count] = b->rec.types[i];
                t->rec.count++;
            }
        }
        return t;
    }
    }
    return &t_any;
}

/* --- lambda inference ---------------------------------------------------- */

static Type *infer(Ck *ck, const Expr *e);

static Type *infer_lambda_with(Ck *ck, const Expr *e, Type **ptypes) {
    Scope sc;
    memset(&sc, 0, sizeof(sc));
    sc.parent = ck->scope; /* the body may capture enclosing locals */
    for (size_t i = 0; i < e->as.lam.param_count; i++) {
        Var *v = env_add(&sc.locals, e->as.lam.params[i], ptypes[i],
                         e->as.lam.param_types[i] != NULL);
        v->bound = true;
    }
    Scope *saved = ck->scope;
    ck->scope = &sc;
    Type *ret = infer(ck, e->as.lam.body);
    ck->scope = saved;
    free(sc.locals.items);
    return ty_func(ptypes, e->as.lam.param_count, ret);
}

/* infer a lambda against an expected function type: unannotated parameters
 * take the expected parameter types (contextual typing); with no expected
 * type they fall back to `any`. */
static Type *infer_lambda(Ck *ck, const Expr *e, const Type *expected) {
    const Type *ex = expected ? ty_resolve(expected) : NULL;
    Type **ptypes = xmalloc(sizeof(Type *) *
                            e->as.lam.param_count);
    for (size_t i = 0; i < e->as.lam.param_count; i++) {
        if (e->as.lam.param_types[i] == NULL && ex && ex->k == TY_FUNC &&
            i < ex->fun.count)
            ptypes[i] = ex->fun.params[i];
        else
            ptypes[i] = e->as.lam.param_types[i]
                            ? resolve_type(ck, e->as.lam.param_types[i], ck->tyenv)
                            : &t_any;
    }
    return infer_lambda_with(ck, e, ptypes);
}

/* --- generic call-site inference ----------------------------------------- */

typedef struct { char **names; Type **types; size_t count; } Subst;

static Type **subst_slot(Subst *s, const char *name) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return &s->types[i];
    return NULL;
}

static bool contains_var(const Type *t) {
    switch (t->k) {
    case TY_VAR:  return true;
    case TY_LIST: return contains_var(t->elem);
    case TY_REC:
        for (size_t i = 0; i < t->rec.count; i++)
            if (contains_var(t->rec.types[i])) return true;
        return false;
    case TY_UNION:
        for (size_t i = 0; i < t->uni.count; i++)
            if (contains_var(t->uni.alts[i])) return true;
        return false;
    case TY_FUNC:
        for (size_t i = 0; i < t->fun.count; i++)
            if (contains_var(t->fun.params[i])) return true;
        return contains_var(t->fun.ret);
    default:
        return false;
    }
}

/* substitute bindings into t; unbound variables become `any` */
static Type *ty_subst(Type *t, Subst *sub) {
    switch (t->k) {
    case TY_VAR: {
        Type **slot = subst_slot(sub, t->var);
        return (slot && *slot) ? *slot : &t_any;
    }
    case TY_LIST: {
        Type *e = ty_subst(t->elem, sub);
        return e == t->elem ? t : ty_list(e);
    }
    case TY_REC: {
        if (!contains_var(t)) return t;
        Type *r = ty_new(TY_REC);
        r->rec.names = t->rec.names;
        r->rec.count = t->rec.count;
        r->rec.types = xmalloc(sizeof(Type *) * t->rec.count);
        for (size_t i = 0; i < t->rec.count; i++)
            r->rec.types[i] = ty_subst(t->rec.types[i], sub);
        return r;
    }
    case TY_UNION: {
        if (!contains_var(t)) return t;
        Type *r = &t_never;
        for (size_t i = 0; i < t->uni.count; i++)
            r = ty_join(r, ty_subst(t->uni.alts[i], sub));
        return r;
    }
    case TY_FUNC: {
        if (!contains_var(t)) return t;
        Type *r = ty_new(TY_FUNC);
        r->fun.count = t->fun.count;
        r->fun.params = xmalloc(sizeof(Type *) * t->fun.count);
        for (size_t i = 0; i < t->fun.count; i++)
            r->fun.params[i] = ty_subst(t->fun.params[i], sub);
        r->fun.ret = ty_subst(t->fun.ret, sub);
        return r;
    }
    default:
        return t;
    }
}

/* structurally match `arg` against `param`, binding type variables in `sub`.
 * Mismatches bind nothing; the caller re-checks assignability afterwards. */
static void unify(Type *param, Type *arg, Subst *sub) {
    param = ty_resolve(param);
    arg = ty_resolve(arg);
    if (arg->k == TY_ANY || arg->k == TY_NEVER) return;
    switch (param->k) {
    case TY_VAR: {
        Type **slot = subst_slot(sub, param->var);
        if (!slot) return;
        Type *w = widen(arg);
        *slot = *slot ? ty_join(*slot, w) : w;
        return;
    }
    case TY_LIST:
        if (arg->k == TY_LIST) unify(param->elem, arg->elem, sub);
        return;
    case TY_REC:
        if (arg->k != TY_REC) return;
        for (size_t i = 0; i < param->rec.count; i++)
            for (size_t j = 0; j < arg->rec.count; j++)
                if (strcmp(param->rec.names[i], arg->rec.names[j]) == 0) {
                    unify(param->rec.types[i], arg->rec.types[j], sub);
                    break;
                }
        return;
    case TY_UNION: {
        /* union against union: bind from each alternative of the argument in
         * turn. Without this, `def f[T](r: Result[T])` learns nothing from a
         * `Result[int]` argument — the whole-union match below never fires,
         * because no single alternative accepts the entire argument. */
        if (arg->k == TY_UNION) {
            for (size_t j = 0; j < arg->uni.count; j++)
                unify(param, arg->uni.alts[j], sub);
            return;
        }
        /* if the argument already fits a variable-free alternative, done */
        for (size_t i = 0; i < param->uni.count; i++)
            if (!contains_var(param->uni.alts[i]) &&
                assignable(param->uni.alts[i], arg))
                return;
        /* otherwise try each variable-bearing alternative on a trial copy */
        for (size_t i = 0; i < param->uni.count; i++) {
            Type *alt = param->uni.alts[i];
            if (!contains_var(alt)) continue;
            Type **trial = xmalloc(sizeof(Type *) * sub->count);
            memcpy(trial, sub->types, sizeof(Type *) * sub->count);
            Subst tsub = {sub->names, trial, sub->count};
            unify(alt, arg, &tsub);
            if (assignable(ty_subst(alt, &tsub), arg)) {
                memcpy(sub->types, trial, sizeof(Type *) * sub->count);
                free(trial);
                return;
            }
            free(trial);
        }
        return;
    }
    case TY_FUNC: /* invariant params, covariant return */
        if (arg->k != TY_FUNC || arg->fun.count != param->fun.count) return;
        for (size_t i = 0; i < param->fun.count; i++)
            unify(param->fun.params[i], arg->fun.params[i], sub);
        unify(param->fun.ret, arg->fun.ret, sub);
        return;
    default:
        return;
    }
}

/* --- expression inference ------------------------------------------------ */

static Type *infer(Ck *ck, const Expr *e);

static bool is_numeric(const Type *t) {
    return t->k == TY_INT || t->k == TY_FLOAT || t->k == TY_BOOL || t->k == TY_ANY;
}

/* --- tensor primitive typing rules (SPEC_V2.md W4) ----------------------- */

/* If `t` is a tensor, return it. If it is `any`/`never` (the untyped escape
 * hatch), return `&t_any` as a sentinel. Anything else is an error. */
static Type *expect_tensor(Ck *ck, const Expr *e, Type *t, const char *fn) {
    Type *tt = tensor_of(t);
    if (tt) return tt;
    Type *r = ty_resolve(t);
    if (r->k != TY_ANY && r->k != TY_NEVER)
        ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                 "%s() expects a tensor, got %s", fn, type_str(t));
    return &t_any;
}

/* elementwise binary via `+ - * /` (dispatched from the operator functions) */
static Type *infer_tensor_binop(Ck *ck, const Expr *e, Type *a, Type *b) {
    if (a->tensor.dt != b->tensor.dt)
        ck_error(ck, "E_SHAPE_DTYPE", e->line, e->col,
                 "tensor dtypes do not match: %s and %s",
                 type_str(a), type_str(b));
    Shape *rs = broadcast_shapes(a->tensor.shape, b->tensor.shape);
    if (!rs) {
        ck_error(ck, "E_SHAPE_BROADCAST", e->line, e->col,
                 "tensor shapes cannot broadcast: %s and %s (use expand())",
                 type_str(a), type_str(b));
        return ty_tensor(a->tensor.dt, shape_dynamic());
    }
    return ty_tensor(a->tensor.dt, rs);
}

static Type *infer_tensor_matmul(Ck *ck, const Expr *e, Type **argt) {
    Type *a = expect_tensor(ck, e, argt[0], "matmul");
    Type *b = expect_tensor(ck, e, argt[1], "matmul");
    if (a == &t_any || b == &t_any)
        return ty_tensor(a == &t_any ? (b == &t_any ? CDT_F32 : b->tensor.dt)
                                       : a->tensor.dt, shape_dynamic());
    CDType dt = a->tensor.dt;
    if (a->tensor.dt != b->tensor.dt)
        ck_error(ck, "E_SHAPE_DTYPE", e->line, e->col,
                 "matmul() dtype mismatch: %s and %s", type_str(a), type_str(b));
    Shape *sa = a->tensor.shape, *sb = b->tensor.shape;
    if (sa->dynamic || sb->dynamic) return ty_tensor(dt, shape_dynamic());
    if (sa->count != 2 || sb->count != 2) {
        ck_error(ck, "E_SHAPE_RANK", e->line, e->col,
                 "matmul() expects rank-2 tensors, got %s and %s",
                 type_str(a), type_str(b));
        return ty_tensor(dt, shape_dynamic());
    }
    if (!dim_eq(sa->dims[1], sb->dims[0])) {
        /* the exit criterion: both shapes printed, the mismatching axis named */
        Diag *d = diag_add(ck->diags, DIA_TYPE, "E_SHAPE_MATMUL", ck->filename,
                           e->line, e->col, "contracted dimensions do not match");
        diag_note(d, "left", type_str(a));
        diag_note(d, "right", type_str(b));
        char *ks = dim_str(sa->dims[1]), *ns = dim_str(sb->dims[0]);
        char mm[256];
        snprintf(mm, sizeof mm, "%s != %s  (contraction axis)", ks, ns);
        free(ks);
        free(ns);
        diag_note(d, "mismatch", mm);
        ck->errors++;
        return ty_tensor(dt, shape_dynamic());
    }
    DimExpr **dims = xmalloc(sizeof(DimExpr *) * 2);
    dims[0] = sa->dims[0];
    dims[1] = sb->dims[1];
    return ty_tensor(dt, shape_of(dims, 2));
}

static Type *infer_tensor_reshape(Ck *ck, const Expr *e, Type **argt) {
    Type *tt = expect_tensor(ck, e, argt[0], "reshape");
    if (tt == &t_any) return &t_any;
    Shape *src = tt->tensor.shape;
    Shape *dst = literal_shape_of_expr(e->as.call.args[1]);
    if (src->dynamic || !dst) return ty_tensor(tt->tensor.dt, shape_dynamic());
    DimExpr *ps = shape_prod(src), *pd = shape_prod(dst);
    bool ok = dim_eq(ps, pd);
    if (!ok) {
        char *s1 = dim_str(ps), *s2 = dim_str(pd);
        ck_error(ck, "E_SHAPE_RESHAPE", e->line, e->col,
                 "reshape() changes the number of elements: %s elements into a "
                 "shape with %s elements", s1, s2);
        free(s1);
        free(s2);
    }
    dim_free(ps);
    dim_free(pd);
    return ok ? ty_tensor(tt->tensor.dt, dst)
              : ty_tensor(tt->tensor.dt, shape_dynamic());
}

static Type *infer_tensor_transpose(Ck *ck, const Expr *e, Type *t) {
    Type *tt = expect_tensor(ck, e, t, "transpose");
    if (tt == &t_any) return &t_any;
    Shape *s = tt->tensor.shape;
    if (s->dynamic) return ty_tensor(tt->tensor.dt, shape_dynamic());
    if (s->count < 2) {
        ck_error(ck, "E_SHAPE_RANK", e->line, e->col,
                 "transpose() needs a tensor of rank >= 2, got rank %zu",
                 s->count);
        return ty_tensor(tt->tensor.dt, shape_dynamic());
    }
    DimExpr **dims = xmalloc(sizeof(DimExpr *) * s->count);
    for (size_t i = 0; i < s->count; i++) dims[i] = s->dims[i];
    DimExpr *tmp = dims[s->count - 1];
    dims[s->count - 1] = dims[s->count - 2];
    dims[s->count - 2] = tmp;
    return ty_tensor(tt->tensor.dt, shape_of(dims, s->count));
}

static Type *infer_tensor_permute(Ck *ck, const Expr *e, Type **argt) {
    Type *tt = expect_tensor(ck, e, argt[0], "permute");
    if (tt == &t_any) return &t_any;
    Shape *s = tt->tensor.shape;
    if (s->dynamic) return ty_tensor(tt->tensor.dt, shape_dynamic());
    const Expr *pe = e->as.call.args[1];
    size_t n = s->count;
    if (!pe || pe->kind != E_LIST || pe->as.list.count != n) {
        ck_error(ck, "E_SHAPE_PERMUTE", e->line, e->col,
                 "permute() needs a permutation of the %zu axes (a list of %zu "
                 "distinct ints)", n, n);
        return ty_tensor(tt->tensor.dt, shape_dynamic());
    }
    int64_t *perm = xmalloc(sizeof(int64_t) * n);
    for (size_t i = 0; i < n; i++) {
        const Expr *it = pe->as.list.items[i];
        if (it->kind != E_INT) {
            free(perm);
            ck_error(ck, "E_SHAPE_PERMUTE", e->line, e->col,
                     "permute() axes must be int literals");
            return ty_tensor(tt->tensor.dt, shape_dynamic());
        }
        perm[i] = it->as.ival;
    }
    for (size_t i = 0; i < n; i++)
        if (perm[i] < 0 || perm[i] >= (int64_t)n) {
            free(perm);
            ck_error(ck, "E_SHAPE_PERMUTE", e->line, e->col,
                     "permute() axis %lld is out of range for rank %zu",
                     (long long)perm[i], n);
            return ty_tensor(tt->tensor.dt, shape_dynamic());
        }
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < i; j++)
            if (perm[j] == perm[i]) {
                free(perm);
                ck_error(ck, "E_SHAPE_PERMUTE", e->line, e->col,
                         "permute() repeats axis %lld", (long long)perm[i]);
                return ty_tensor(tt->tensor.dt, shape_dynamic());
            }
    DimExpr **dims = xmalloc(sizeof(DimExpr *) * n);
    for (size_t i = 0; i < n; i++) dims[i] = s->dims[perm[i]];
    free(perm);
    return ty_tensor(tt->tensor.dt, shape_of(dims, n));
}

static Type *infer_tensor_reduce(Ck *ck, const Expr *e, Type **argt,
                                 const char *fn) {
    Type *tt = expect_tensor(ck, e, argt[0], fn);
    if (tt == &t_any) return &t_any;
    Shape *s = tt->tensor.shape;
    if (s->dynamic) return ty_tensor(tt->tensor.dt, shape_dynamic());
    Type *ax = ty_resolve(argt[1]);
    if (ax->k != TY_LIT || ax->lit.base != TY_INT)
        return ty_tensor(tt->tensor.dt, shape_dynamic()); /* axis unknown */
    int64_t axis = ax->lit.ival;
    if (axis < 0 || axis >= (int64_t)s->count) {
        ck_error(ck, "E_SHAPE_AXIS", e->line, e->col,
                 "%s() axis %lld is out of range for a rank-%zu tensor",
                 fn, (long long)axis, s->count);
        return ty_tensor(tt->tensor.dt, shape_dynamic());
    }
    size_t out_n = s->count - 1;
    DimExpr **dims = xmalloc(sizeof(DimExpr *) * out_n);
    size_t k = 0;
    for (size_t i = 0; i < s->count; i++)
        if ((int64_t)i != axis) dims[k++] = s->dims[i];
    return ty_tensor(tt->tensor.dt, shape_of(dims, out_n));
}

static Type *infer_tensor_expand(Ck *ck, const Expr *e, Type **argt) {
    Type *tt = expect_tensor(ck, e, argt[0], "expand");
    if (tt == &t_any) return &t_any;
    Shape *src = tt->tensor.shape;
    Shape *dst = literal_shape_of_expr(e->as.call.args[1]);
    if (src->dynamic || !dst) return ty_tensor(tt->tensor.dt, shape_dynamic());
    if (src->count != dst->count) {
        ck_error(ck, "E_SHAPE_BROADCAST", e->line, e->col,
                 "expand() rank mismatch: %zu to %zu axes",
                 src->count, dst->count);
        return ty_tensor(tt->tensor.dt, shape_dynamic());
    }
    for (size_t i = 0; i < src->count; i++)
        if (!dim_eq(src->dims[i], dst->dims[i]) && !dim_is_one(src->dims[i])) {
            ck_error(ck, "E_SHAPE_BROADCAST", e->line, e->col,
                     "expand() cannot grow axis %zu from %s to %s",
                     i, dim_str(src->dims[i]), dim_str(dst->dims[i]));
            return ty_tensor(tt->tensor.dt, shape_dynamic());
        }
    return ty_tensor(tt->tensor.dt, dst);
}

static Type *infer_tensor_slice(Ck *ck, const Expr *e, Type **argt) {
    Type *tt = expect_tensor(ck, e, argt[0], "tslice");
    if (tt == &t_any) return &t_any;
    Shape *s = tt->tensor.shape;
    if (s->dynamic) return ty_tensor(tt->tensor.dt, shape_dynamic());
    Type *ax = ty_resolve(argt[1]);
    if (ax->k != TY_LIT || ax->lit.base != TY_INT)
        return ty_tensor(tt->tensor.dt, shape_dynamic());
    int64_t axis = ax->lit.ival;
    if (axis < 0 || axis >= (int64_t)s->count) {
        ck_error(ck, "E_SHAPE_AXIS", e->line, e->col,
                 "tslice() axis %lld is out of range for a rank-%zu tensor",
                 (long long)axis, s->count);
        return ty_tensor(tt->tensor.dt, shape_dynamic());
    }
    const Expr *lo = e->as.call.args[2], *hi = e->as.call.args[3];
    DimExpr **dims = xmalloc(sizeof(DimExpr *) * s->count);
    for (size_t i = 0; i < s->count; i++) {
        if ((int64_t)i == axis && lo && hi && lo->kind == E_INT &&
            hi->kind == E_INT)
            dims[i] = dim_lit(hi->as.ival - lo->as.ival);
        else if ((int64_t)i == axis)
            return ty_tensor(tt->tensor.dt, shape_dynamic());
        else
            dims[i] = s->dims[i];
    }
    return ty_tensor(tt->tensor.dt, shape_of(dims, s->count));
}

static Type *infer_tensor_astype(Ck *ck, const Expr *e, Type **argt) {
    Type *tt = expect_tensor(ck, e, argt[0], "astype");
    if (tt == &t_any) return &t_any;
    CDType ndt = tt->tensor.dt;
    const Expr *de = e->as.call.args[1];
    if (de && de->kind == E_STR) {
        if (strcmp(de->as.sval, "f64") == 0) ndt = CDT_F64;
        else if (strcmp(de->as.sval, "f32") == 0) ndt = CDT_F32;
        else
            ck_error(ck, "E_SHAPE_DTYPE", e->line, e->col,
                     "unknown tensor dtype '%s'", de->as.sval);
    }
    return ty_tensor(ndt, tt->tensor.shape);
}

static Type *infer_binop(Ck *ck, const Expr *e) {
    /* literal types behave as their base type under operators */
    Type *l = ty_base(infer(ck, e->as.bin.lhs));
    Type *r = ty_base(infer(ck, e->as.bin.rhs));
    BinOp op = e->as.bin.op;

    if (l->k == TY_NEVER || r->k == TY_NEVER) return &t_never;

    switch (op) {
    case B_AND: case B_OR:
        return ty_join(l, r); /* Python semantics: result is one operand */
    case B_EQ: case B_NE:
        return &t_bool;
    case B_LT: case B_LE: case B_GT: case B_GE: {
        bool ok = (is_numeric(l) && is_numeric(r)) ||
                  (l->k == TY_STR && r->k == TY_STR) ||
                  (l->k == TY_LIST && r->k == TY_LIST) ||
                  l->k == TY_ANY || r->k == TY_ANY ||
                  l->k == TY_UNION || r->k == TY_UNION;
        if (!ok)
            ck_error(ck, "E_TYPE_ORDER", e->line, e->col,
                     "cannot order %s and %s", type_str(l), type_str(r));
        return &t_bool;
    }
    case B_PIPE: { /* x |> f == f(x) */
        Type *fnt = ty_resolve(r);
        if (fnt->k != TY_FUNC || fnt->fun.count != 1) {
            ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                     "right side of '|>' must be a unary function, got %s",
                     type_str(fnt));
            return &t_any;
        }
        if (!assignable(fnt->fun.params[0], l))
            ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                       fnt->fun.params[0], l,
                       "cannot pipe %s into a function expecting %s",
                       type_str(l), type_str(fnt->fun.params[0]));
        return fnt->fun.ret;
    }
    case B_COMPOSE: { /* f >> g == x -> g(f(x)) */
        Type *fl = ty_resolve(l);
        Type *fr = ty_resolve(r);
        if (fl->k != TY_FUNC || fl->fun.count != 1 ||
            fr->k != TY_FUNC || fr->fun.count != 1) {
            ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                     "'>>' composes two unary functions, got %s and %s",
                     type_str(fl), type_str(fr));
            return &t_any;
        }
        if (!assignable(fr->fun.params[0], fl->fun.ret))
            ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                       fr->fun.params[0], fl->fun.ret,
                       "composed functions do not chain: %s feeds %s, which "
                       "expects %s", type_str(fl->fun.ret), type_str(fr),
                       type_str(fr->fun.params[0]));
        Type *pt = fl->fun.params[0];
        return ty_func(&pt, 1, fr->fun.ret);
    }
    case B_ADD:
        if (l->k == TY_ANY || r->k == TY_ANY) return &t_any;
        if (l->k == TY_STR && r->k == TY_STR) return &t_str;
        if (l->k == TY_LIST && r->k == TY_LIST)
            return ty_list(ty_join(l->elem, r->elem));
        /* fall through to arithmetic */
        /* FALLTHROUGH */
    case B_SUB: case B_MUL: case B_DIV: case B_MOD: {
        if (l->k == TY_ANY || r->k == TY_ANY) return &t_any;
        /* tensors dispatch elementwise (+ - * /; no % on tensors) */
        if (op != B_MOD) {
            Type *tl = tensor_of(l), *tr = tensor_of(r);
            if (tl || tr) {
                if (tl && tr) return infer_tensor_binop(ck, e, tl, tr);
                /* tensor op scalar (or scalar op tensor): the scalar
                 * broadcasts to the tensor's shape and dtype */
                Type *t = tl ? tl : tr;
                Type *sc = tl ? r : l;
                if (sc->k == TY_INT || sc->k == TY_FLOAT || sc->k == TY_BOOL)
                    return t;
                ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                         "cannot combine a tensor with %s", type_str(sc));
                return &t_any;
            }
        }
        if (op == B_MUL) { /* "ab" * 3, [0] * n */
            if (l->k == TY_STR && r->k == TY_INT) return &t_str;
            if (l->k == TY_INT && r->k == TY_STR) return &t_str;
            if (l->k == TY_LIST && r->k == TY_INT) return l;
            if (l->k == TY_INT && r->k == TY_LIST) return r;
        }
        if (!is_numeric(l) || !is_numeric(r) ||
            l->k == TY_UNION || r->k == TY_UNION) {
            static const char *names[] = {"+", "-", "*", "/", "%"};
            ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                     "unsupported operand types for %s: %s and %s",
                     names[op], type_str(l), type_str(r));
            return &t_any;
        }
        if (op == B_DIV) return &t_float; /* Python 3 semantics */
        if (l->k == TY_FLOAT || r->k == TY_FLOAT) return &t_float;
        return &t_int;
    }
    }
    return &t_any;
}

/* The arity check every fixed-arity builtin repeats. Returns false (having
 * reported) when the call site does not match. */
static bool ck_arity(Ck *ck, const Expr *e, const char *name, size_t want) {
    size_t got = e->as.call.count;
    if (got == want) return true;
    ck_error(ck, "E_TYPE_ARITY", e->line, e->col,
             "%s() takes %zu argument%s, got %zu", name, want,
             want == 1 ? "" : "s", got);
    return false;
}

/* the generic signatures of the higher-order list builtins, instantiated
 * with fresh type variables at each call site:
 *   map(f: (T) -> U, xs: list[T]) -> list[U]
 *   filter(f: (T) -> bool, xs: list[T]) -> list[T]
 *   reduce(f: (U, T) -> U, acc: U, xs: list[T]) -> U
 */
static Type *infer_map_like(Ck *ck, const Expr *e, const char *name,
                            Type **argt, const bool *islam) {
    size_t argc = e->as.call.count;
    Type *t = ty_var("T");
    Type *u = ty_var("U");
    Type *f1[] = { t };
    Type *xs = ty_list(t);
    Type *params[3];
    Type *ret;
    size_t want;
    if (strcmp(name, "map") == 0) {
        params[0] = ty_func(f1, 1, u);
        params[1] = xs;
        ret = ty_list(u);
        want = 2;
    } else if (strcmp(name, "filter") == 0) {
        params[0] = ty_func(f1, 1, &t_bool);
        params[1] = xs;
        ret = xs;
        want = 2;
    } else { /* reduce */
        Type *f2[] = { u, t };
        params[0] = ty_func(f2, 2, u);
        params[1] = u;
        params[2] = xs;
        ret = u;
        want = 3;
    }
    if (!ck_arity(ck, e, name, want)) return &t_any;
    char *tvnames[] = { "T", "U" };
    Subst sub;
    sub.names = tvnames;
    sub.count = 2;
    sub.types = xmalloc(sizeof(Type *) * 2);
    memset(sub.types, 0, sizeof(Type *) * 2);
    /* bind what we can from the non-lambda arguments, then give the
     * lambdas the instantiated parameter types, then bind the rest */
    for (size_t i = 0; i < argc; i++)
        if (!islam[i]) unify(params[i], argt[i], &sub);
    /* note: do not pre-fill unbound slots with `any` here — ty_subst already
     * treats NULL as any, and pre-filling would poison the slots: a later
     * unify could never narrow them (ty_join(any, int) is any), so the
     * lambda's inferred types would fail to bind the remaining variables */
    for (size_t i = 0; i < argc; i++)
        if (islam[i])
            argt[i] = infer_lambda(ck, e->as.call.args[i],
                                   ty_subst(params[i], &sub));
    for (size_t i = 0; i < argc; i++)
        unify(params[i], argt[i], &sub);
    for (size_t j = 0; j < 2; j++)
        if (!sub.types[j]) sub.types[j] = &t_any;
    for (size_t i = 0; i < argc; i++) {
        Type *pi = ty_subst(params[i], &sub);
        if (!assignable(pi, argt[i]))
            ck_error_t(ck, "E_TYPE_ARG", e->line, e->col, pi, argt[i],
                       "argument %zu of %s(): expected %s, got %s",
                       i + 1, name, type_str(pi), type_str(argt[i]));
    }
    Type *r = ty_subst(ret, &sub);
    free(sub.types);
    return r;
}

static Type *infer_call(Ck *ck, const Expr *e) {
    const Expr *fn = e->as.call.fn;
    size_t argc = e->as.call.count;
    Type **argt = xmalloc(sizeof(Type *) * argc);
    bool *islam = xmalloc(sizeof(bool) * argc);
    for (size_t i = 0; i < argc; i++) {
        /* lambdas are inferred lazily so unannotated parameters can take the
         * expected type from the callee's signature (contextual typing) */
        islam[i] = e->as.call.args[i]->kind == E_LAMBDA;
        if (!islam[i]) argt[i] = infer(ck, e->as.call.args[i]);
    }

    if (fn->kind == E_NAME) {
        const char *name = fn->as.sval;
        const char *dname = fn->disp ? fn->disp : name;
        /* purity: a pure function may only call pure builtins */
        bool bpure = false;
        if (ck->cur_pure && builtin_find(name, &bpure) && !bpure)
            ck_error(ck, "E_TYPE_PURE_CALL", e->line, e->col,
                     "pure function calls impure builtin '%s'", dname);
        if (strcmp(name, "map") == 0 || strcmp(name, "filter") == 0 ||
            strcmp(name, "reduce") == 0)
            return infer_map_like(ck, e, name, argt, islam);
        FuncSig *f = find_func(ck, name);
        if (!f) /* plain builtins have no typed signature: lambdas are free */
            for (size_t i = 0; i < argc; i++)
                if (islam[i])
                    argt[i] = infer_lambda(ck, e->as.call.args[i], NULL);
        /* the variadic builtins accept any number of arguments */
        if (strcmp(name, "print") == 0 ||
            strcmp(name, "eprint") == 0) return &t_none;
        if (strcmp(name, "len") == 0) {
            if (ck_arity(ck, e, dname, 1)) {
                Type *a = ty_base(argt[0]);
                if (a->k != TY_ANY && a->k != TY_STR && a->k != TY_LIST &&
                    a->k != TY_REC && a->k != TY_UNION && a->k != TY_NEVER)
                    ck_error(ck, "E_TYPE_NO_LEN", e->line, e->col,
                             "%s has no len()", type_str(argt[0]));
            }
            return &t_int;
        }
        if (strcmp(name, "range") == 0) {
            if (argc != 1 && argc != 2)
                ck_error(ck, "E_TYPE_ARITY", e->line, e->col,
                         "range() takes 1 or 2 arguments, got %zu", argc);
            for (size_t i = 0; i < argc; i++)
                if (!assignable(&t_int, argt[i]))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "range() argument %zu must be int, got %s",
                             i + 1, type_str(argt[i]));
            return ty_list(&t_int);
        }
        if (strcmp(name, "str") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_str;
        }
        if (strcmp(name, "int") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_int;
        }
        if (strcmp(name, "gc_stats") == 0) {
            ck_arity(ck, e, dname, 0);
            return gc_stats_type();
        }
        if (strcmp(name, "gc_collect") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_none;
        }
        if (strcmp(name, "read_file") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "read_file() path must be str, got %s", type_str(argt[0]));
            return &t_str;
        }
        if (strcmp(name, "write_file") == 0 || strcmp(name, "append_file") == 0) {
            ck_arity(ck, e, dname, 2);
            return &t_none;
        }
        if (strcmp(name, "run") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "run() command must be str, got %s", type_str(argt[0]));
            return &t_int;
        }
        if (strcmp(name, "sqrt") == 0 || strcmp(name, "tan") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_float, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "%s() argument must be a number, got %s",
                         name, type_str(argt[0]));
            return &t_float;
        }
        if (strcmp(name, "rand") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_float;
        }
        /* --- the stdlib foundation (see stdlib/SPEC.md §1.1) ------------- */
        if (strcmp(name, "append") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_none;
            Type *l = ty_base(ty_resolve(argt[0]));
            if (l->k == TY_LIST) {
                if (!assignable(l->elem, argt[1]))
                    ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                               l->elem, argt[1],
                               "append() to %s: expected %s, got %s",
                               type_str(argt[0]), type_str(l->elem),
                               type_str(argt[1]));
            } else if (l->k != TY_ANY && l->k != TY_NEVER) {
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "append() expects a list, got %s", type_str(argt[0]));
            }
            return &t_none;
        }
        if (strcmp(name, "slice") == 0) {
            if (!ck_arity(ck, e, dname, 3)) return &t_any;
            for (size_t i = 1; i < 3; i++)
                if (!assignable(&t_int, argt[i]))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "slice() bound %zu must be int, got %s", i,
                             type_str(argt[i]));
            /* slicing preserves the sequence's type: str -> str, list[T] ->
             * list[T]. Anything else is a compile error rather than a cast. */
            Type *s = ty_base(ty_resolve(argt[0]));
            if (s->k == TY_STR) return &t_str;
            if (s->k == TY_LIST) return argt[0];
            if (s->k == TY_ANY || s->k == TY_NEVER) return &t_any;
            ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                     "cannot slice %s", type_str(argt[0]));
            return &t_any;
        }
        if (strcmp(name, "ord") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "ord() argument must be str, got %s", type_str(argt[0]));
            return &t_int;
        }
        if (strcmp(name, "chr") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "chr() argument must be int, got %s", type_str(argt[0]));
            return &t_str;
        }
        if (strcmp(name, "float") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_float;
        }
        if (strcmp(name, "argv") == 0) {
            ck_arity(ck, e, dname, 0);
            return ty_list(&t_str);
        }
        if (strcmp(name, "exit") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "exit() status must be int, got %s", type_str(argt[0]));
            /* `never`: exit() does not return, so `return exit(1)` satisfies
             * any return type and a function ending in exit() is not a
             * fall-off-the-end error. */
            return &t_never;
        }
        if (strcmp(name, "read_file_opt") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "read_file_opt() path must be str, got %s",
                         type_str(argt[0]));
            return ty_join(&t_str, &t_none);
        }
        if (strcmp(name, "file_exists") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "file_exists() path must be str, got %s",
                         type_str(argt[0]));
            return &t_bool;
        }

        /* --- tensor primitives: shape-obligation typing rules (W4). Each
         * constructor's shape comes from a runtime list, so it types as the
         * dynamic escape hatch Tensor[f32, ?]; the shape-carrying operations
         * (matmul, elementwise, reshape, permute, reductions) verify their
         * obligations statically and emit E_SHAPE_* diagnostics. */
        if (strcmp(name, "zeros") == 0 || strcmp(name, "ones") == 0 ||
            strcmp(name, "arange") == 0) {
            ck_arity(ck, e, dname, 1);
            return ty_tensor(CDT_F32, shape_dynamic());
        }
        if (strcmp(name, "full") == 0 || strcmp(name, "randn") == 0) {
            ck_arity(ck, e, dname, 2);
            return ty_tensor(CDT_F32, shape_dynamic());
        }
        if (strcmp(name, "tensor") == 0) {
            ck_arity(ck, e, dname, 1);
            return ty_tensor(CDT_F32, shape_dynamic());
        }
        if (strcmp(name, "exp") == 0 || strcmp(name, "log") == 0 ||
            strcmp(name, "tanh") == 0 || strcmp(name, "relu") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return &t_any;
            /* elementwise unary: preserves dtype and shape */
            return expect_tensor(ck, e, argt[0], name);
        }
        if (strcmp(name, "transpose") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return &t_any;
            return infer_tensor_transpose(ck, e, argt[0]);
        }
        if (strcmp(name, "item") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_float;
        }
        if (strcmp(name, "shape") == 0) {
            ck_arity(ck, e, dname, 1);
            return ty_list(&t_int);
        }
        if (strcmp(name, "ndim") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_int;
        }
        if (strcmp(name, "dtype") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_str;
        }
        if (strcmp(name, "astype") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_astype(ck, e, argt);
        }
        if (strcmp(name, "matmul") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_matmul(ck, e, argt);
        }
        if (strcmp(name, "reshape") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_reshape(ck, e, argt);
        }
        if (strcmp(name, "permute") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_permute(ck, e, argt);
        }
        if (strcmp(name, "expand") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_expand(ck, e, argt);
        }
        if (strcmp(name, "sum") == 0 || strcmp(name, "mean") == 0 ||
            strcmp(name, "max") == 0 || strcmp(name, "argmax") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_reduce(ck, e, argt, name);
        }
        if (strcmp(name, "tslice") == 0) {
            if (!ck_arity(ck, e, dname, 4)) return &t_any;
            return infer_tensor_slice(ck, e, argt);
        }

        if (f) {
            /* purity: a pure function may only call other pure functions */
            if (ck->cur_pure && !f->pure)
                ck_error(ck, "E_TYPE_PURE_CALL", e->line, e->col,
                         "pure function calls impure function '%s'", dname);
            if (!ck_arity(ck, e, dname, f->param_count))
                return f->tparam_count ? &t_any : f->ret;
            if (f->tparam_count == 0) {
                /* infer deferred lambdas against the declared parameter types */
                for (size_t i = 0; i < argc; i++)
                    if (islam[i])
                        argt[i] = infer_lambda(ck, e->as.call.args[i],
                                               f->params[i]);
                for (size_t i = 0; i < argc; i++) {
                    note_shape_crossing(ck, f->params[i], argt[i]);
                    if (!assignable(f->params[i], argt[i]))
                        ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                                   f->params[i], argt[i],
                                   "argument %zu of %s(): expected %s, got %s",
                                   i + 1, dname, type_str(f->params[i]),
                                   type_str(argt[i]));
                }
                return f->ret;
            }
            /* generic call: infer type arguments by unification, then re-check */
            Subst sub;
            sub.names = f->tparams;
            sub.count = f->tparam_count;
            sub.types = xmalloc(sizeof(Type *) * f->tparam_count);
            memset(sub.types, 0, sizeof(Type *) * f->tparam_count);
            for (size_t i = 0; i < argc; i++)
                if (!islam[i]) unify(f->params[i], argt[i], &sub);
            for (size_t j = 0; j < f->tparam_count; j++)
                if (!sub.types[j]) sub.types[j] = &t_any;
            /* deferred lambdas get the partially-instantiated param types */
            for (size_t i = 0; i < argc; i++)
                if (islam[i])
                    argt[i] = infer_lambda(ck, e->as.call.args[i],
                                           ty_subst(f->params[i], &sub));
            /* now unify everything, so lambda return types bind the rest */
            for (size_t i = 0; i < argc; i++)
                unify(f->params[i], argt[i], &sub);
            for (size_t j = 0; j < f->tparam_count; j++)
                if (!sub.types[j]) sub.types[j] = &t_any;
            for (size_t i = 0; i < argc; i++) {
                Type *pi = ty_subst(f->params[i], &sub);
                note_shape_crossing(ck, pi, argt[i]);
                if (!assignable(pi, argt[i]))
                    ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                               pi, argt[i],
                               "argument %zu of %s(): expected %s, got %s",
                               i + 1, dname, type_str(pi), type_str(argt[i]));
            }
            Type *ret = ty_subst(f->ret, &sub);
            free(sub.types);
            return ret;
        }
        /* an undefined name being called is its own error */
        if (!lookup_var(ck, name)) {
            ck_error(ck, "E_TYPE_UNDEFINED", e->line, e->col,
                     "call to undefined function '%s'", dname);
            return &t_any;
        }
        /* otherwise a function value held in a variable: indirect call */
    }

    Type *ft = ty_resolve(infer(ck, fn));
    if (ft->k != TY_FUNC) {
        ck_error(ck, "E_TYPE_NOT_CALLABLE", e->line, e->col,
                 "value of type %s is not callable", type_str(ft));
        return &t_any;
    }
    if (argc != ft->fun.count) {
        ck_error(ck, "E_TYPE_ARITY", e->line, e->col,
                 "function takes %zu argument%s, got %zu",
                 ft->fun.count, ft->fun.count == 1 ? "" : "s", argc);
        return ft->fun.ret;
    }
    for (size_t i = 0; i < argc; i++)
        if (islam[i])
            argt[i] = infer_lambda(ck, e->as.call.args[i], ft->fun.params[i]);
    for (size_t i = 0; i < argc; i++) {
        note_shape_crossing(ck, ft->fun.params[i], argt[i]);
        if (!assignable(ft->fun.params[i], argt[i]))
            ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                       ft->fun.params[i], argt[i],
                       "argument %zu: expected %s, got %s",
                       i + 1, type_str(ft->fun.params[i]), type_str(argt[i]));
    }
    return ft->fun.ret;
}

static Type *infer(Ck *ck, const Expr *e) {
    switch (e->kind) {
    case E_INT: {
        Type *t = ty_lit_int(e->as.ival);
        t->fresh = true;
        return t;
    }
    case E_FLOAT: return &t_float;
    case E_STR: {
        Type *t = ty_lit_str(e->as.sval);
        t->fresh = true;
        return t;
    }
    case E_TRUE: case E_FALSE: {
        Type *t = ty_lit_bool(e->kind == E_TRUE);
        t->fresh = true;
        return t;
    }
    case E_NONE:  return &t_none;
    case E_NAME: {
        Var *v = lookup_var(ck, e->as.sval);
        if (v) {
            /* a variable captured from an enclosing function reads its stable
             * declared type (flow narrowing does not cross a closure boundary) */
            bool captured =
                ck->scope != NULL &&
                env_find(&ck->scope->locals, e->as.sval) == NULL &&
                env_find(&ck->globals, e->as.sval) == NULL;
            return captured ? v->decl : v->type;
        }
        FuncSig *f = find_func(ck, e->as.sval);
        if (f) {
            if (f->tparam_count) return &t_any; /* generics aren't first-class */
            Type **params = xmalloc(sizeof(Type *) * f->param_count);
            for (size_t i = 0; i < f->param_count; i++) params[i] = f->params[i];
            return ty_func(params, f->param_count, f->ret);
        }
        if (is_builtin(e->as.sval)) {
            ck_error(ck, "E_TYPE_BUILTIN_VALUE", e->line, e->col,
                     "'%s' is a builtin; builtins are not values", e->as.sval);
            return &t_any;
        }
        ck_error(ck, "E_TYPE_UNDEFINED", e->line, e->col,
                 "undefined name '%s'", e->as.sval);
        return &t_any;
    }
    case E_LIST: {
        if (e->as.list.count == 0) return ty_list(&t_any);
        Type *elem = infer(ck, e->as.list.items[0]);
        for (size_t i = 1; i < e->as.list.count; i++)
            elem = ty_join(elem, infer(ck, e->as.list.items[i]));
        return ty_list(elem);
    }
    case E_REC: {
        Type *t = ty_new(TY_REC);
        t->rec.names = e->as.rec.names;
        t->rec.types = xmalloc(sizeof(Type *) * e->as.rec.count);
        t->rec.count = e->as.rec.count;
        for (size_t i = 0; i < e->as.rec.count; i++)
            t->rec.types[i] = infer(ck, e->as.rec.values[i]);
        return t;
    }
    case E_BINOP:
        return infer_binop(ck, e);
    case E_UNOP: {
        Type *t = ty_base(infer(ck, e->as.un.operand));
        if (e->as.un.op == U_NOT) return &t_bool;
        if (t->k == TY_NEVER) return &t_never;
        if (t->k == TY_INT || t->k == TY_BOOL) return &t_int;
        if (t->k == TY_FLOAT) return &t_float;
        if (t->k != TY_ANY)
            ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                     "unsupported operand type for unary -: %s", type_str(t));
        return &t_any;
    }
    case E_CALL:
        return infer_call(ck, e);
    case E_LAMBDA: {
        Type **ptypes = xmalloc(sizeof(Type *) *
                                e->as.lam.param_count);
        for (size_t i = 0; i < e->as.lam.param_count; i++)
            ptypes[i] = e->as.lam.param_types[i]
                            ? resolve_type(ck, e->as.lam.param_types[i], ck->tyenv)
                            : &t_any;
        return infer_lambda_with(ck, e, ptypes);
    }
    case E_INDEX: {
        Type *seq = ty_base(infer(ck, e->as.index.seq));
        Type *idx = infer(ck, e->as.index.idx);
        if (!assignable(&t_int, idx) || idx->k == TY_FLOAT)
            ck_error(ck, "E_TYPE_INDEX", e->line, e->col,
                     "index must be int, got %s", type_str(idx));
        if (seq->k == TY_NEVER) return &t_never;
        if (seq->k == TY_LIST) return seq->elem;
        if (seq->k == TY_STR) return &t_str;
        if (seq->k == TY_TENSOR) {
            /* `a[i]` drops the indexed (first) axis, matching the runtime's
             * rank-reduced view semantics (numpy-style) */
            Shape *s = seq->tensor.shape;
            if (s->dynamic) return ty_tensor(seq->tensor.dt, shape_dynamic());
            if (s->count == 0) {
                ck_error(ck, "E_TYPE_INDEX", e->line, e->col,
                         "cannot index a scalar (0-d) tensor");
                return &t_any;
            }
            /* Fin[n] index safety (W5): reject an index provably out of range */
            Type *ix = ty_resolve(idx);
            if (ix->k == TY_FIN) {
                int c = dim_le(ix->fin, s->dims[0]);
                if (c == 0) {
                    char *b = dim_str(s->dims[0]);
                    ck_error(ck, "E_SHAPE_INDEX", e->line, e->col,
                             "index of type %s cannot index an axis of size %s",
                             type_str(ix), b);
                    free(b);
                }
            }
            DimExpr **dims = xmalloc(sizeof(DimExpr *) * (s->count - 1 ? s->count - 1 : 1));
            for (size_t i = 1; i < s->count; i++) dims[i - 1] = s->dims[i];
            return ty_tensor(seq->tensor.dt, shape_of(dims, s->count - 1));
        }
        if (seq->k == TY_ANY || seq->k == TY_UNION) return &t_any;
        ck_error(ck, "E_TYPE_INDEX", e->line, e->col,
                 "%s is not indexable", type_str(seq));
        return &t_any;
    }
    case E_ATTR: {
        Type *obj = ty_resolve(infer(ck, e->as.attr.obj));
        if (obj->k == TY_ANY) return &t_any;
        if (obj->k == TY_NEVER) return &t_never;
        if (obj->k == TY_REC) {
            for (size_t i = 0; i < obj->rec.count; i++)
                if (strcmp(obj->rec.names[i], e->as.attr.name) == 0)
                    return obj->rec.types[i];
            ck_error(ck, "E_TYPE_FIELD", e->line, e->col,
                     "type %s has no field '%s'",
                     type_str(obj), e->as.attr.name);
            return &t_any;
        }
        if (obj->k == TY_UNION) { /* the field must exist on every alternative */
            Type *result = NULL;
            for (size_t i = 0; i < obj->uni.count; i++) {
                Type *alt = obj->uni.alts[i];
                Type *ft = NULL;
                if (alt->k == TY_ANY) { ft = &t_any; }
                else if (alt->k == TY_REC) {
                    for (size_t j = 0; j < alt->rec.count; j++)
                        if (strcmp(alt->rec.names[j], e->as.attr.name) == 0) {
                            ft = alt->rec.types[j];
                            break;
                        }
                }
                if (!ft) {
                    ck_error(ck, "E_TYPE_FIELD", e->line, e->col,
                             "field '%s' does not exist on every alternative of %s",
                             e->as.attr.name, type_str(obj));
                    return &t_any;
                }
                result = result ? ty_join(result, ft) : ft;
            }
            return result ? result : &t_any;
        }
        ck_error(ck, "E_TYPE_FIELD", e->line, e->col,
                 "type %s has no fields (looking for '%s')",
                 type_str(obj), e->as.attr.name);
        return &t_any;
    }
    }
    return &t_any;
}

/* --- flow narrowing ------------------------------------------------------ */

/* A narrowing temporarily overrides Var.type; NSave remembers how to undo it.
 * If the variable was assigned in between (gen changed), restoring falls back
 * to the declared type instead of the stale snapshot. */
typedef struct { Var *var; Type *saved; int gen; } NSave;
typedef struct { NSave items[64]; size_t count; } NSet;

/* ns == NULL applies the narrowing persistently (no undo record) */
static void narrow_apply(NSet *ns, Var *v, Type *newt) {
    if (type_eq(v->type, newt)) return;
    if (ns) {
        if (ns->count >= 64) return;
        ns->items[ns->count].var = v;
        ns->items[ns->count].saved = v->type;
        ns->items[ns->count].gen = v->gen;
        ns->count++;
    }
    v->type = newt;
}

static void nset_restore_from(NSet *ns, size_t mark) {
    while (ns->count > mark) {
        NSave *s = &ns->items[--ns->count];
        s->var->type = (s->var->gen == s->gen) ? s->saved : s->var->decl;
    }
}

/* the literal type of a literal expression, or NULL (None => t_none) */
static Type *lit_of_expr(const Expr *e) {
    switch (e->kind) {
    case E_INT:   return ty_lit_int(e->as.ival);
    case E_STR:   return ty_lit_str(e->as.sval);
    case E_TRUE:  return ty_lit_bool(1);
    case E_FALSE: return ty_lit_bool(0);
    case E_NONE:  return &t_none;
    case E_UNOP:
        if (e->as.un.op == U_NEG && e->as.un.operand->kind == E_INT)
            return ty_lit_int(-e->as.un.operand->as.ival);
        return NULL;
    default:
        return NULL;
    }
}

/* `x == lit`: if the value equals the literal, its type IS the literal */
static Type *narrow_eq(Type *t, Type *lit) {
    t = ty_resolve(t);
    Type **alts = t->k == TY_UNION ? t->uni.alts : &t;
    size_t n = t->k == TY_UNION ? t->uni.count : 1;
    for (size_t i = 0; i < n; i++) {
        Type *alt = alts[i];
        if (alt->k == TY_ANY || alt->k == TY_VAR || assignable(alt, lit))
            return lit;
    }
    return &t_never;
}

/* `x != lit`: drop alternatives that are exactly that literal */
static Type *narrow_ne(Type *t, Type *lit) {
    t = ty_resolve(t);
    Type **alts = t->k == TY_UNION ? t->uni.alts : &t;
    size_t n = t->k == TY_UNION ? t->uni.count : 1;
    Type **keep = xmalloc(sizeof(Type *) * n);
    size_t kn = 0;
    for (size_t i = 0; i < n; i++) {
        Type *alt = alts[i];
        if (type_eq(alt, lit)) continue;
        if (lit->k == TY_LIT && lit->lit.base == TY_BOOL && alt->k == TY_BOOL) {
            keep[kn++] = ty_lit_bool(!lit->lit.ival); /* bool has two values */
            continue;
        }
        keep[kn++] = alt;
    }
    Type *r = ty_union_of(keep, kn);
    free(keep);
    return r;
}

/* `x.field == lit` (discriminated unions): keep alternatives whose field
 * could hold the literal; for != drop those whose field IS the literal */
static Type *narrow_field(Type *t, const char *fname, Type *lit, bool eq) {
    t = ty_resolve(t);
    Type **alts = t->k == TY_UNION ? t->uni.alts : &t;
    size_t n = t->k == TY_UNION ? t->uni.count : 1;
    Type **keep = xmalloc(sizeof(Type *) * n);
    size_t kn = 0;
    for (size_t i = 0; i < n; i++) {
        Type *alt = alts[i];
        if (alt->k == TY_ANY || alt->k == TY_VAR) { keep[kn++] = alt; continue; }
        if (alt->k != TY_REC) {
            if (!eq) keep[kn++] = alt;
            continue;
        }
        Type *ft = NULL;
        for (size_t j = 0; j < alt->rec.count; j++)
            if (strcmp(alt->rec.names[j], fname) == 0) { ft = alt->rec.types[j]; break; }
        if (eq) {
            if (ft && (ft->k == TY_ANY || ft->k == TY_VAR || assignable(ft, lit)))
                keep[kn++] = alt;
        } else {
            if (!(ft && type_eq(ft, lit)))
                keep[kn++] = alt;
        }
    }
    Type *r = ty_union_of(keep, kn);
    free(keep);
    return r;
}

/* truthiness of a bare `if x`: drop alternatives ruled out by the branch */
static Type *narrow_truthy(Type *t, bool sense) {
    t = ty_resolve(t);
    Type **alts = t->k == TY_UNION ? t->uni.alts : &t;
    size_t n = t->k == TY_UNION ? t->uni.count : 1;
    Type **keep = xmalloc(sizeof(Type *) * n);
    size_t kn = 0;
    for (size_t i = 0; i < n; i++) {
        Type *alt = alts[i];
        bool always_falsy =
            alt->k == TY_NONE ||
            (alt->k == TY_LIT &&
             ((alt->lit.base == TY_BOOL && !alt->lit.ival) ||
              (alt->lit.base == TY_INT && alt->lit.ival == 0) ||
              (alt->lit.base == TY_STR && alt->lit.sval[0] == '\0')));
        bool always_truthy =
            alt->k == TY_LIT &&
            ((alt->lit.base == TY_BOOL && alt->lit.ival) ||
             (alt->lit.base == TY_INT && alt->lit.ival != 0) ||
             (alt->lit.base == TY_STR && alt->lit.sval[0] != '\0'));
        if (sense ? !always_falsy : !always_truthy)
            keep[kn++] = alt;
    }
    Type *r = ty_union_of(keep, kn);
    free(keep);
    return r;
}

/* Derive narrowings from a condition. `sense` is whether the condition is
 * known true (then-branch) or false (else-branch and later elif arms). */
static void narrow_cond(Ck *ck, const Expr *e, bool sense, NSet *ns) {
    switch (e->kind) {
    case E_NAME: {
        Var *v = lookup_var(ck, e->as.sval);
        if (v) narrow_apply(ns, v, narrow_truthy(v->type, sense));
        return;
    }
    case E_UNOP:
        if (e->as.un.op == U_NOT) narrow_cond(ck, e->as.un.operand, !sense, ns);
        return;
    case E_BINOP: {
        BinOp op = e->as.bin.op;
        if (op == B_AND) { /* both facts hold when the conjunction is true */
            if (sense) {
                narrow_cond(ck, e->as.bin.lhs, true, ns);
                narrow_cond(ck, e->as.bin.rhs, true, ns);
            }
            return;
        }
        if (op == B_OR) { /* both facts fail when the disjunction is false */
            if (!sense) {
                narrow_cond(ck, e->as.bin.lhs, false, ns);
                narrow_cond(ck, e->as.bin.rhs, false, ns);
            }
            return;
        }
        if (op != B_EQ && op != B_NE) return;
        bool eq = (op == B_EQ) == sense;
        const Expr *target = e->as.bin.lhs;
        Type *lit = lit_of_expr(e->as.bin.rhs);
        if (!lit) {
            lit = lit_of_expr(e->as.bin.lhs);
            target = e->as.bin.rhs;
        }
        if (!lit) return;
        if (target->kind == E_NAME) {
            Var *v = lookup_var(ck, target->as.sval);
            if (v)
                narrow_apply(ns, v, eq ? narrow_eq(v->type, lit)
                                       : narrow_ne(v->type, lit));
        } else if (target->kind == E_ATTR && target->as.attr.obj->kind == E_NAME) {
            Var *v = lookup_var(ck, target->as.attr.obj->as.sval);
            if (v)
                narrow_apply(ns, v,
                             narrow_field(v->type, target->as.attr.name, lit, eq));
        }
        return;
    }
    default:
        return;
    }
}

/* does this block always leave (return/break/continue) rather than fall out? */
static bool block_terminates(const Block *b);

static bool stmt_terminates(const Stmt *s) {
    switch (s->kind) {
    case S_RETURN: case S_BREAK: case S_CONTINUE:
        return true;
    case S_BLOCK:
        return block_terminates(&s->as.block);
    case S_IF: {
        if (!s->as.ifs.has_else) return false;
        for (size_t i = 0; i < s->as.ifs.count; i++)
            if (!block_terminates(&s->as.ifs.blocks[i])) return false;
        return block_terminates(&s->as.ifs.else_block);
    }
    case S_MATCH: {
        for (size_t i = 0; i < s->as.mtch.count; i++)
            if (!block_terminates(&s->as.mtch.blocks[i])) return false;
        return true;
    }
    default:
        return false;
    }
}

static bool block_terminates(const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        if (stmt_terminates(b->items[i])) return true;
    return false;
}

/* Does this block always leave the *function* — by returning, or by looping
 * forever? Falling off the end of a function returns None, so a function
 * whose declared return type rejects None must satisfy this. Conservative:
 * an unrecognized shape just means "not proven", which only ever asks the
 * programmer for an explicit return. */
static bool block_returns(const Block *b);

/* a `while True` with no `break` reaching this level never finishes */
static bool block_has_break(const Block *b);

static bool stmt_has_break(const Stmt *s) {
    switch (s->kind) {
    case S_BREAK:
        return true;
    case S_BLOCK:
        return block_has_break(&s->as.block);
    case S_IF: {
        for (size_t i = 0; i < s->as.ifs.count; i++)
            if (block_has_break(&s->as.ifs.blocks[i])) return true;
        return s->as.ifs.has_else && block_has_break(&s->as.ifs.else_block);
    }
    case S_MATCH:
        for (size_t i = 0; i < s->as.mtch.count; i++)
            if (block_has_break(&s->as.mtch.blocks[i])) return true;
        return false;
    default: /* nested loops capture their own breaks */
        return false;
    }
}

static bool block_has_break(const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        if (stmt_has_break(b->items[i])) return true;
    return false;
}

static bool stmt_returns(const Stmt *s) {
    switch (s->kind) {
    case S_RETURN:
        return true;
    case S_BLOCK:
        return block_returns(&s->as.block);
    case S_IF: {
        if (!s->as.ifs.has_else) return false;
        for (size_t i = 0; i < s->as.ifs.count; i++)
            if (!block_returns(&s->as.ifs.blocks[i])) return false;
        return block_returns(&s->as.ifs.else_block);
    }
    case S_WHILE: {
        const Expr *c = s->as.wh.cond;
        bool always = c->kind == E_TRUE ||
                      (c->kind == E_INT && c->as.ival != 0);
        return always && !block_has_break(&s->as.wh.body);
    }
    case S_MATCH: {
        for (size_t i = 0; i < s->as.mtch.count; i++)
            if (!block_returns(&s->as.mtch.blocks[i])) return false;
        return true;
    }
    case S_EXPR:
        /* exit() is typed `never`: control does not reach the next statement,
         * so a function ending in one has not fallen off its end */
        return s->as.expr->kind == E_CALL &&
               s->as.expr->as.call.fn->kind == E_NAME &&
               strcmp(s->as.expr->as.call.fn->as.sval, "exit") == 0;
    default:
        return false;
    }
}

static bool block_returns(const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        if (stmt_returns(b->items[i])) return true;
    return false;
}

/* --- termination checking ------------------------------------------------ */

/* Emerald functions are total by default: every recursive call must descend
 * structurally. An argument descends when it is a projection chain from a
 * parameter whose declared type is a recursive alias (`n.succ`, `xs.tail`),
 * with every step landing back on that same alias — the standard "one
 * constructor step smaller" rule, applied to non-generic recursive aliases.
 * A function that cannot be shown to terminate this way must be declared
 * `partial` to opt out. Mutual recursion and descent through `list` elements
 * (`t.kids[0]`) are not recognized yet, so those also need `partial`.
 */

typedef struct { const Type *items[256]; size_t count; } Vis;

static bool vis_has(const Vis *v, const Type *t) {
    for (size_t i = 0; i < v->count; i++)
        if (v->items[i] == t) return true;
    return false;
}

static void vis_add(Vis *v, const Type *t) {
    if (v->count < 256) v->items[v->count++] = t;
}

/* A type is "recursive" iff its structure contains a TY_ALIAS node. Aliases
 * resolve eagerly and cross-alias references are inlined, so any surviving
 * TY_ALIAS node is a self-reference. */
static bool type_is_recursive(const Type *t0, Vis *v) {
    if (t0->k == TY_ALIAS) return true;
    const Type *t = ty_resolve(t0);
    if (vis_has(v, t)) return false;
    vis_add(v, t);
    switch (t->k) {
    case TY_LIST:
        return type_is_recursive(t->elem, v);
    case TY_REC:
        for (size_t i = 0; i < t->rec.count; i++)
            if (type_is_recursive(t->rec.types[i], v)) return true;
        return false;
    case TY_UNION:
        for (size_t i = 0; i < t->uni.count; i++)
            if (type_is_recursive(t->uni.alts[i], v)) return true;
        return false;
    case TY_FUNC:
        for (size_t i = 0; i < t->fun.count; i++)
            if (type_is_recursive(t->fun.params[i], v)) return true;
        return type_is_recursive(t->fun.ret, v);
    default:
        return false;
    }
}

/* the declared type of field `f` inside the resolved structure of `t`
 * (a record, or a union of records); NULL when absent */
static const Type *field_type(const Type *t, const char *f) {
    const Type *r = ty_resolve(t);
    if (r->k == TY_REC) {
        for (size_t i = 0; i < r->rec.count; i++)
            if (strcmp(r->rec.names[i], f) == 0) return r->rec.types[i];
        return NULL;
    }
    if (r->k == TY_UNION)
        for (size_t i = 0; i < r->uni.count; i++) {
            const Type *alt = ty_resolve(r->uni.alts[i]);
            if (alt->k != TY_REC) continue;
            for (size_t j = 0; j < alt->rec.count; j++)
                if (strcmp(alt->rec.names[j], f) == 0) return alt->rec.types[j];
        }
    return NULL;
}

/* is `arg` a strict structural subterm of parameter `pname`, whose declared
 * type resolves to `root` (a recursive alias)? Every projection step must
 * land back on `root` (pointer equality), so the chain stays one level deep
 * in the same inductive structure. */
static bool arg_descends(const Type *root, const char *pname, const Expr *arg) {
    const char *fields[32];
    size_t nf = 0;
    const Expr *e = arg;
    while (e->kind == E_ATTR && nf < 32) {
        fields[nf++] = e->as.attr.name;
        e = e->as.attr.obj;
    }
    if (nf == 0 || e->kind != E_NAME || strcmp(e->as.sval, pname) != 0)
        return false;
    Vis vis = {0};
    if (!type_is_recursive(root, &vis)) return false;
    const Type *cur = ty_resolve(root);
    for (size_t i = 0; i < nf; i++) {
        const Type *ft = field_type(cur, fields[i]);
        if (!ft) return false;
        cur = ty_resolve(ft);
        if (cur != ty_resolve(root)) return false;
    }
    return true;
}

static void collect_calls_expr(const Expr *e, const char *fname,
                               const Expr ***out, size_t *count, size_t *cap);

static void collect_calls_stmt(const Stmt *s, const char *fname,
                               const Expr ***out, size_t *count, size_t *cap) {
    switch (s->kind) {
    case S_EXPR:
        collect_calls_expr(s->as.expr, fname, out, count, cap);
        break;
    case S_ASSIGN:
        collect_calls_expr(s->as.assign.value, fname, out, count, cap);
        break;
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++) {
            collect_calls_expr(s->as.ifs.conds[i], fname, out, count, cap);
            for (size_t j = 0; j < s->as.ifs.blocks[i].count; j++)
                collect_calls_stmt(s->as.ifs.blocks[i].items[j], fname,
                                   out, count, cap);
        }
        if (s->as.ifs.has_else)
            for (size_t j = 0; j < s->as.ifs.else_block.count; j++)
                collect_calls_stmt(s->as.ifs.else_block.items[j], fname,
                                   out, count, cap);
        break;
    case S_WHILE:
        collect_calls_expr(s->as.wh.cond, fname, out, count, cap);
        for (size_t j = 0; j < s->as.wh.body.count; j++)
            collect_calls_stmt(s->as.wh.body.items[j], fname, out, count, cap);
        break;
    case S_FOR:
        collect_calls_expr(s->as.fr.seq, fname, out, count, cap);
        for (size_t j = 0; j < s->as.fr.body.count; j++)
            collect_calls_stmt(s->as.fr.body.items[j], fname, out, count, cap);
        break;
    case S_RETURN:
        collect_calls_expr(s->as.ret, fname, out, count, cap);
        break;
    case S_BLOCK:
        for (size_t j = 0; j < s->as.block.count; j++)
            collect_calls_stmt(s->as.block.items[j], fname, out, count, cap);
        break;
    case S_FUNC:
        /* a nested def may call the enclosing function (recursion through a
         * closure); its calls count against the enclosing function */
        for (size_t j = 0; j < s->as.func.body.count; j++)
            collect_calls_stmt(s->as.func.body.items[j], fname, out, count, cap);
        break;
    case S_MATCH:
        collect_calls_expr(s->as.mtch.subject, fname, out, count, cap);
        for (size_t j = 0; j < s->as.mtch.count; j++)
            for (size_t k = 0; k < s->as.mtch.blocks[j].count; k++)
                collect_calls_stmt(s->as.mtch.blocks[j].items[k], fname,
                                   out, count, cap);
        break;
    default:
        break;
    }
}

static void collect_calls_expr(const Expr *e, const char *fname,
                               const Expr ***out, size_t *count, size_t *cap) {
    if (!e) return;
    switch (e->kind) {
    case E_CALL:
        if (e->as.call.fn->kind == E_NAME &&
            strcmp(e->as.call.fn->as.sval, fname) == 0) {
            if (*count == *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *out = xrealloc(*out, sizeof(Expr *) * *cap);
            }
            (*out)[(*count)++] = e;
        }
        collect_calls_expr(e->as.call.fn, fname, out, count, cap);
        for (size_t i = 0; i < e->as.call.count; i++)
            collect_calls_expr(e->as.call.args[i], fname, out, count, cap);
        break;
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            collect_calls_expr(e->as.list.items[i], fname, out, count, cap);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            collect_calls_expr(e->as.rec.values[i], fname, out, count, cap);
        break;
    case E_BINOP:
        collect_calls_expr(e->as.bin.lhs, fname, out, count, cap);
        collect_calls_expr(e->as.bin.rhs, fname, out, count, cap);
        break;
    case E_UNOP:
        collect_calls_expr(e->as.un.operand, fname, out, count, cap);
        break;
    case E_INDEX:
        collect_calls_expr(e->as.index.seq, fname, out, count, cap);
        collect_calls_expr(e->as.index.idx, fname, out, count, cap);
        break;
    case E_ATTR:
        collect_calls_expr(e->as.attr.obj, fname, out, count, cap);
        break;
    default:
        break;
    }
}

/* every self-recursive call of a non-`partial` function must descend
 * structurally (see arg_descends); `ptypes` holds the resolved declared
 * parameter types */
static void check_termination(Ck *ck, const Stmt *s, Type **ptypes) {
    const char *fname = s->as.func.name;
    const Expr **calls = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < s->as.func.body.count; i++)
        collect_calls_stmt(s->as.func.body.items[i], fname, &calls, &n, &cap);
    if (!n) { free(calls); return; }
    for (size_t c = 0; c < n; c++) {
        const Expr *call = calls[c];
        bool descends = false;
        for (size_t i = 0; i < s->as.func.param_count &&
                            i < call->as.call.count && !descends; i++)
            descends = arg_descends(ptypes[i], s->as.func.params[i],
                                    call->as.call.args[i]);
        if (!descends)
            ck_error(ck, "E_TYPE_TERMINATION", call->line, call->col,
                     "recursive call to '%s' does not decrease a "
                     "recursive-alias argument; declare the function "
                     "'partial' to opt out of termination checking",
                     s->as.func.dispname);
    }
    free(calls);
}

/* --- pattern matching ---------------------------------------------------- */

static Type *lit_pat_type(const Pat *p) {
    switch (p->lit.kind) {
    case LIT_INT:  return ty_lit_int(p->lit.ival);
    case LIT_STR:  return ty_lit_str(p->lit.sval);
    case LIT_BOOL: return ty_lit_bool(p->lit.ival);
    case LIT_NONE: return &t_none;
    }
    return &t_any;
}

/* bind a pattern variable; Emerald has no shadowing, so a collision with an
 * existing name in the scope is an error */
static void pat_bind(Ck *ck, VarEnv *env, const char *name, const Type *t,
                     int line, int col) {
    if (env_find(env, name))
        ck_error(ck, "E_TYPE_BIND", line, col,
                 "pattern binding '%s' is already defined in this scope", name);
    else
        env_add(env, name, (Type *)t, true);
}

/* check one pattern against the subject type, binding its names into `env`;
 * returns the type of values the pattern covers (for exhaustiveness) */
static Type *check_pattern(Ck *ck, const Pat *p, const Type *st, VarEnv *env) {
    switch (p->kind) {
    case P_WILD:
        return &t_any;
    case P_BIND:
        pat_bind(ck, env, p->bind, st, p->line, p->col);
        return &t_any;
    case P_LIT:
        return lit_pat_type(p);
    case P_REC: {
        Type *t = ty_new(TY_REC);
        t->rec.names = xmalloc(sizeof(char *) *
                               p->rec.count);
        t->rec.types = xmalloc(sizeof(Type *) *
                               p->rec.count);
        t->rec.count = p->rec.count;
        for (size_t i = 0; i < p->rec.count; i++) {
            const Pat *it = p->rec.items[i];
            const Type *ft = field_type(st, it->name);
            if (!ft) {
                ck_error(ck, "E_TYPE_FIELD", it->line, it->col,
                         "type %s has no field '%s' (in pattern)",
                         type_str(st), it->name);
                ft = &t_any;
            }
            t->rec.names[i] = it->name;
            switch (it->kind) {
            case P_BIND:
                pat_bind(ck, env, it->bind, ft, it->line, it->col);
                t->rec.types[i] = (Type *)ft;
                break;
            case P_WILD:
                t->rec.types[i] = (Type *)ft;
                break;
            case P_LIT:
                t->rec.types[i] = lit_pat_type(it);
                break;
            case P_REC:
                t->rec.types[i] = check_pattern(ck, it, ft, env);
                break;
            }
        }
        return t;
    }
    }
    return &t_any;
}

/* --- statements ---------------------------------------------------------- */

static void check_block(Ck *ck, const Block *b);
static void check_func(Ck *ck, Scope *parent, const Stmt *s);

/* record a successful assignment: reads after this see the value's (widened)
 * type when it is more precise than the declared one (assignment narrowing) */
/* a fresh literal captured by a declared type stops being fresh: its exact
 * shape is now owned by the annotation and must not widen later */
static void defresh(Type *t) {
    switch (t->k) {
    case TY_LIT:  t->fresh = false; break;
    case TY_LIST: defresh(t->elem); break;
    case TY_REC:
        for (size_t i = 0; i < t->rec.count; i++) defresh(t->rec.types[i]);
        break;
    case TY_UNION:
        for (size_t i = 0; i < t->uni.count; i++) defresh(t->uni.alts[i]);
        break;
    default:
        break;
    }
}

static void set_flow(Var *v, Type *val) {
    Type *w = widen(val);
    v->gen++;
    v->bound = true;
    /* a dynamic tensor bound to a statically-shaped annotation keeps the
     * annotation's shape on reads: the runtime asserts it at the boundary,
     * so later ops must see the static shape (SPEC_V2.md D4). */
    Type *d = ty_resolve(v->decl), *s = ty_resolve(w);
    if (d->k == TY_TENSOR && s->k == TY_TENSOR &&
        !d->tensor.shape->dynamic && s->tensor.shape->dynamic)
        v->type = v->decl;
    else if (assignable(v->decl, w)) v->type = w;
    else if (assignable(v->decl, val)) { defresh(val); v->type = val; }
    else v->type = v->decl;
}

static void check_assign(Ck *ck, const Stmt *s) {
    Type *val = infer(ck, s->as.assign.value);
    if (ck->proof && val->k == TY_ANY)
        ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                 "value has type 'any', which is banned in proof mode");
    Expr *target = s->as.assign.target;

    if (target->kind == E_NAME) {
        const char *name = target->as.sval;
        if (is_builtin(name) || find_func(ck, name)) {
            ck_error(ck, "E_TYPE_ASSIGN", s->line, s->col,
                     "cannot assign to function name '%s'", name);
            return;
        }
        VarEnv *env = ck->scope ? &ck->scope->locals : &ck->globals;
        Var *v = env_find(env, name);
        /* docs rule: assignment inside a def updates a same-module global */
        if (!v && ck->scope && updatable_global(ck, name, s->file))
            v = env_find(&ck->globals, name);
        if (v && v->is_const && !s->as.assign.is_const) {
            ck_error(ck, "E_TYPE_CONST", s->line, s->col,
                     "cannot assign to const '%s'", name);
            return;
        }
        if (s->as.assign.ann) {
            Type *ann = resolve_type(ck, s->as.assign.ann, ck->tyenv);
            /* contextual typing for `xs: list[T] = []`. An empty literal has
             * element type `any` on its own, and without this the annotation
             * is discarded the moment the variable is read back: `m: list[str]
             * = []` then `cur = m` would make `cur` a `list[any]`, which is
             * both imprecise and (in proof mode) an error. */
            if (s->as.assign.value->kind == E_LIST &&
                s->as.assign.value->as.list.count == 0 &&
                ty_resolve(ann)->k == TY_LIST)
                val = ann;
            if (ck->proof && ann->k == TY_ANY)
                ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                         "'any' is banned in proof mode: annotate '%s' with a "
                         "concrete type", name);
            note_shape_crossing(ck, ann, val);
            if (!assignable(ann, val))
                ck_error_t(ck, "E_TYPE_ASSIGN", s->line, s->col, ann, val,
                           "cannot assign %s to '%s' declared as %s",
                           type_str(val), name, type_str(ann));
            if (!v) {
                v = env_add(env, name, ann, true);
                if (!ck->scope) v->file = s->file;
            }
            v->decl = ann;
            v->annotated = true;
            v->is_const = s->as.assign.is_const;
            set_flow(v, val);
            return;
        }
        if (!v) {
            v = env_add(env, name, widen(val), false);
            if (!ck->scope) v->file = s->file;
            v->is_const = s->as.assign.is_const;
            set_flow(v, val);
            return;
        }
        if (!v->bound) { /* first assignment fixes the inferred type */
            v->is_const = s->as.assign.is_const;
            note_shape_crossing(ck, v->decl, val);
            if (!v->annotated) v->decl = widen(val);
            else if (!assignable(v->decl, val))
                ck_error_t(ck, "E_TYPE_ASSIGN", s->line, s->col, v->decl, val,
                           "cannot assign %s to '%s' declared as %s",
                           type_str(val), name, type_str(v->decl));
            set_flow(v, val);
            return;
        }
        note_shape_crossing(ck, v->decl, val);
        if (assignable(v->decl, val)) { set_flow(v, val); return; }
        if (v->annotated) {
            ck_error_t(ck, "E_TYPE_ASSIGN", s->line, s->col, v->decl, val,
                       "cannot assign %s to '%s' declared as %s",
                       type_str(val), name, type_str(v->decl));
            set_flow(v, val);
        } else {
            v->decl = ty_join(v->decl, widen(val)); /* inferred vars widen */
            set_flow(v, val);
        }
        return;
    }

    if (target->kind == E_INDEX) {
        Type *seq = ty_base(infer(ck, target->as.index.seq));
        Type *idx = infer(ck, target->as.index.idx);
        if (!assignable(&t_int, idx) || idx->k == TY_FLOAT)
            ck_error(ck, "E_TYPE_INDEX", s->line, s->col,
                     "index must be int, got %s", type_str(idx));
        if (seq->k == TY_STR) {
            ck_error(ck, "E_TYPE_IMMUTABLE", s->line, s->col,
                     "strings are immutable");
            return;
        }
        if (seq->k == TY_LIST && !assignable(seq->elem, val))
            ck_error_t(ck, "E_TYPE_ASSIGN", s->line, s->col, seq->elem, val,
                       "cannot store %s in %s",
                       type_str(val), type_str(seq));
        else if (seq->k != TY_LIST && seq->k != TY_ANY && seq->k != TY_UNION &&
                 seq->k != TY_NEVER)
            ck_error(ck, "E_TYPE_ASSIGN", s->line, s->col,
                     "%s does not support item assignment",
                     type_str(seq));
        return;
    }

    /* E_ATTR */
    Type *obj = ty_resolve(infer(ck, target->as.attr.obj));
    if (obj->k == TY_ANY || obj->k == TY_NEVER) return;
    if (obj->k != TY_REC) {
        ck_error(ck, "E_TYPE_FIELD", s->line, s->col,
                 "type %s has no fields (assigning '%s')",
                 type_str(obj), target->as.attr.name);
        return;
    }
    for (size_t i = 0; i < obj->rec.count; i++)
        if (strcmp(obj->rec.names[i], target->as.attr.name) == 0) {
            if (!assignable(obj->rec.types[i], val))
                ck_error_t(ck, "E_TYPE_ASSIGN", s->line, s->col,
                           obj->rec.types[i], val,
                           "cannot assign %s to field '%s' of type %s",
                           type_str(val), target->as.attr.name,
                           type_str(obj->rec.types[i]));
            return;
        }
    ck_error(ck, "E_TYPE_FIELD", s->line, s->col,
             "type %s has no field '%s'",
             type_str(obj), target->as.attr.name);
}

static void check_stmt(Ck *ck, const Stmt *s) {
    switch (s->kind) {
    case S_EXPR: {
        Type *t = infer(ck, s->as.expr);
        if (ck->proof && t->k == TY_ANY)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "expression has type 'any', which is banned in proof mode");
        break;
    }
    case S_ASSIGN:
        check_assign(ck, s);
        break;
    case S_IF: {
        size_t narms = s->as.ifs.count;
        /* interest set: every variable the conditions can narrow. After the
         * if, each gets the join of its type over all paths that fall through
         * (arms that end in return/break/continue contribute nothing). */
        Var *vars[64];
        Type *merged[64];
        size_t nv = 0;
        {
            NSet probe = {.count = 0};
            for (size_t i = 0; i < narms; i++) {
                narrow_cond(ck, s->as.ifs.conds[i], true, &probe);
                narrow_cond(ck, s->as.ifs.conds[i], false, &probe);
            }
            for (size_t j = 0; j < probe.count && nv < 64; j++) {
                bool seen = false;
                for (size_t k = 0; k < nv; k++)
                    if (vars[k] == probe.items[j].var) { seen = true; break; }
                if (!seen) vars[nv++] = probe.items[j].var;
            }
            nset_restore_from(&probe, 0);
        }
        for (size_t j = 0; j < nv; j++) merged[j] = NULL;
        bool any_path = false;

        NSet ns = {.count = 0};
        for (size_t i = 0; i < narms; i++) {
            Type *ct = infer(ck, s->as.ifs.conds[i]);
            if (ck->proof && ct->k == TY_ANY)
                ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                         "condition has type 'any', which is banned in proof "
                         "mode");
            size_t mark = ns.count;
            narrow_cond(ck, s->as.ifs.conds[i], true, &ns);
            check_block(ck, &s->as.ifs.blocks[i]);
            if (!block_terminates(&s->as.ifs.blocks[i])) {
                any_path = true;
                for (size_t j = 0; j < nv; j++)
                    merged[j] = merged[j] ? ty_join(merged[j], vars[j]->type)
                                          : vars[j]->type;
            }
            nset_restore_from(&ns, mark);
            /* this arm did not run: its condition is false from here on */
            narrow_cond(ck, s->as.ifs.conds[i], false, &ns);
        }
        bool fallthrough_live = true;
        if (s->as.ifs.has_else) {
            check_block(ck, &s->as.ifs.else_block);
            fallthrough_live = !block_terminates(&s->as.ifs.else_block);
        }
        if (fallthrough_live) { /* the all-conditions-false path */
            any_path = true;
            for (size_t j = 0; j < nv; j++)
                merged[j] = merged[j] ? ty_join(merged[j], vars[j]->type)
                                      : vars[j]->type;
        }
        nset_restore_from(&ns, 0);
        if (any_path)
            for (size_t j = 0; j < nv; j++) vars[j]->type = merged[j];
        break;
    }
    case S_WHILE: {
        Type *ct = infer(ck, s->as.wh.cond);
        if (ck->proof && ct->k == TY_ANY)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "condition has type 'any', which is banned in proof mode");
        NSet ns = {.count = 0};
        narrow_cond(ck, s->as.wh.cond, true, &ns);
        ck->loop_depth++;
        check_block(ck, &s->as.wh.body);
        ck->loop_depth--;
        nset_restore_from(&ns, 0);
        break;
    }
    case S_FOR: {
        Type *seq = ty_base(infer(ck, s->as.fr.seq));
        if (ck->proof && seq->k == TY_ANY)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "iterated value has type 'any', which is banned in proof "
                     "mode");
        Type *elem = &t_any;
        if (seq->k == TY_LIST) elem = widen(seq->elem);
        else if (seq->k == TY_STR) elem = &t_str;
        else if (seq->k != TY_ANY && seq->k != TY_UNION && seq->k != TY_NEVER)
            ck_error(ck, "E_TYPE_ITER", s->line, s->col,
                     "%s is not iterable", type_str(seq));
        VarEnv *env = ck->scope ? &ck->scope->locals : &ck->globals;
        Var *v = env_find(env, s->as.fr.var);
        if (!v && ck->scope && updatable_global(ck, s->as.fr.var, s->file))
            v = env_find(&ck->globals, s->as.fr.var);
        if (!v) {
            v = env_add(env, s->as.fr.var, elem, false);
            if (!ck->scope) v->file = s->file;
        } else if (!v->annotated) v->decl = v->bound ? ty_join(v->decl, elem) : elem;
        else if (!assignable(v->decl, elem))
            ck_error_t(ck, "E_TYPE_ASSIGN", s->line, s->col, v->decl, elem,
                       "loop assigns %s to '%s' declared as %s",
                       type_str(elem), s->as.fr.var, type_str(v->decl));
        set_flow(v, elem);
        ck->loop_depth++;
        check_block(ck, &s->as.fr.body);
        ck->loop_depth--;
        break;
    }
    case S_RETURN: {
        if (!ck->scope) {
            ck_error(ck, "E_TYPE_RETURN", s->line, s->col,
                     "'return' outside of a function");
            break;
        }
        Type *t = s->as.ret ? infer(ck, s->as.ret) : &t_none;
        if (ck->proof && t->k == TY_ANY)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "returning a value of type 'any', which is banned in "
                     "proof mode");
        note_shape_crossing(ck, ck->cur_ret, t);
        if (!assignable(ck->cur_ret, t))
            ck_error_t(ck, "E_TYPE_RETURN", s->line, s->col, ck->cur_ret, t,
                       "returning %s from a function declared to return %s",
                       type_str(t), type_str(ck->cur_ret));
        break;
    }
    case S_BREAK:
        if (ck->loop_depth == 0)
            ck_error(ck, "E_TYPE_BREAK", s->line, s->col,
                     "'break' outside of a loop");
        break;
    case S_CONTINUE:
        if (ck->loop_depth == 0)
            ck_error(ck, "E_TYPE_CONTINUE", s->line, s->col,
                     "'continue' outside of a loop");
        break;
    case S_PASS:
        break;
    case S_BLOCK:
        check_block(ck, &s->as.block);
        break;
    case S_MATCH: {
        Type *st = ty_resolve(infer(ck, s->as.mtch.subject));
        VarEnv *env = ck->scope ? &ck->scope->locals : &ck->globals;
        Type *covered = &t_never;
        for (size_t i = 0; i < s->as.mtch.count; i++) {
            size_t mark = env->count; /* bindings are scoped per arm */
            Type *cov = check_pattern(ck, s->as.mtch.pats[i], st, env);
            covered = ty_join(covered, cov);
            check_block(ck, &s->as.mtch.blocks[i]);
            env->count = mark;
        }
        if (!assignable(covered, st))
            ck_error(ck, "E_TYPE_MATCH", s->line, s->col,
                     "match is not exhaustive: add a catch-all arm ('_')");
        break;
    }
    case S_FUNC:
        if (ck->scope)
            check_func(ck, ck->scope, s); /* nested def: check its body now */
        /* top-level bodies are checked in a dedicated pass */
        break;
    case S_TYPEDEF:
        /* resolved during the signature pass */
        break;
    case S_IMPORT:
        /* resolved away by the module linker before checking */
        break;
    case S_DIMDECL:
        /* dimension declarations are registered in the signature pass */
        break;
    }
}

static void check_block(Ck *ck, const Block *b) {
    for (size_t i = 0; i < b->count; i++) {
        /* a linked program spans several files; follow the statement's own */
        const char *saved = ck->filename;
        if (b->items[i]->file) ck->filename = b->items[i]->file;
        check_stmt(ck, b->items[i]);
        ck->filename = saved;
    }
}

/* --- passes -------------------------------------------------------------- */

typedef struct { Ck *ck; VarEnv *env; const char *skip; } DeclCtx;

/* Is `name` a global this file may update by assignment? The docs rule is that
 * assigning a global's name inside a def updates the global — but only within
 * the module that declared it. Across a module boundary the names are
 * unrelated: `for xs in xss` inside a library function must not write to an
 * importer's `xs` just because the linker put them in one translation unit. */
static bool updatable_global(Ck *ck, const char *name, const char *file) {
    Var *g = env_find(&ck->globals, name);
    if (!g) return false;
    if (!g->file || !file) return true;
    return strcmp(g->file, file) == 0;
}

static void declare_local(const char *name, const char *file, int line, void *ud) {
    DeclCtx *dc = ud;
    (void)line;
    if (dc->skip == NULL || !updatable_global(dc->ck, name, file))
        if (!env_find(dc->env, name))
            env_add(dc->env, name, &t_any, false);
}

/* type-variable scope for a generic function's signature and body */
static TyEnv func_tyenv(const Stmt *s) {
    TyEnv env = {s->as.func.tparams, NULL, s->as.func.tparam_count};
    if (env.count) {
        env.types = xmalloc(sizeof(Type *) * env.count);
        for (size_t i = 0; i < env.count; i++)
            env.types[i] = ty_var(s->as.func.tparams[i]);
    }
    return env;
}

/* the `: dim` type parameters of a generic function, in declaration order */
static char **func_dims(const Stmt *s, size_t *out_count) {
    size_t n = 0;
    for (size_t i = 0; i < s->as.func.tparam_count; i++)
        if (s->as.func.tparam_dims && s->as.func.tparam_dims[i]) n++;
    if (!n) { *out_count = 0; return NULL; }
    char **dims = xmalloc(sizeof(char *) * n);
    size_t j = 0;
    for (size_t i = 0; i < s->as.func.tparam_count; i++)
        if (s->as.func.tparam_dims && s->as.func.tparam_dims[i])
            dims[j++] = s->as.func.tparams[i];
    *out_count = n;
    return dims;
}

/* register a function signature into `scope` (nested) or ck->funcs (top). */
static void register_func(Ck *ck, Scope *scope, const Stmt *s) {
    if (is_builtin(s->as.func.name)) {
        ck_error(ck, "E_TYPE_REDEFINE", s->line, s->col,
                 "cannot redefine builtin '%s'", s->as.func.dispname);
        return;
    }
    /* a nested def inside a pure function must itself be pure, or a pure
     * function could smuggle an impure helper past the call check */
    if (scope && scope->pure && !s->as.func.pure)
        ck_error(ck, "E_TYPE_PURE_NESTED", s->line, s->col,
                 "nested function '%s' inside a pure function must be declared "
                 "pure", s->as.func.dispname);
    if (ck->proof && s->as.func.partial)
        ck_error(ck, "E_PROOF_PARTIAL", s->line, s->col,
                 "'partial' functions are banned in proof mode: '%s' must "
                 "terminate structurally", s->as.func.dispname);
    FuncSig *f;
    if (scope) {
        for (size_t i = 0; i < scope->func_count; i++)
            if (strcmp(scope->funcs[i].name, s->as.func.name) == 0) {
                ck_error(ck, "E_TYPE_REDEFINE", s->line, s->col,
                         "function '%s' is already defined",
                         s->as.func.dispname);
                return;
            }
        if (scope->func_count == scope->func_cap) {
            scope->func_cap = scope->func_cap ? scope->func_cap * 2 : 4;
            scope->funcs = xrealloc(scope->funcs, sizeof(FuncSig) * scope->func_cap);
        }
        f = &scope->funcs[scope->func_count++];
    } else {
        for (size_t i = 0; i < ck->func_count; i++)
            if (strcmp(ck->funcs[i].name, s->as.func.name) == 0) {
                ck_error(ck, "E_TYPE_REDEFINE", s->line, s->col,
                         "function '%s' is already defined",
                         s->as.func.dispname);
                return;
            }
        f = &ck->funcs[ck->func_count++];
    }
    TyEnv tenv = func_tyenv(s);
    TyEnv *te = tenv.count ? &tenv : NULL;
    f->name = s->as.func.name;
    f->disp = s->as.func.dispname;
    f->tparams = s->as.func.tparams;
    f->tparam_count = s->as.func.tparam_count;
    f->param_count = s->as.func.param_count;
    f->pure = s->as.func.pure;
    f->partial = s->as.func.partial;
    f->params = xmalloc(sizeof(Type *) * f->param_count);
    bool saved_in_sig = ck->in_sig;
    ck->in_sig = true;
    /* the `: dim` type parameters are in scope for the signature's shapes */
    size_t ndims = 0;
    char **dims = func_dims(s, &ndims);
    char **saved_dp = ck->dim_params;
    size_t saved_dpc = ck->dim_param_count;
    ck->dim_params = dims;
    ck->dim_param_count = ndims;
    for (size_t j = 0; j < f->param_count; j++) {
        if (ck->proof && s->as.func.param_types[j] == NULL)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "parameter '%s' has no type annotation ('any' is banned in "
                     "proof mode)", s->as.func.params[j]);
        f->params[j] = resolve_type(ck, s->as.func.param_types[j], te);
    }
    if (ck->proof && s->as.func.ret_type == NULL)
        ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                 "missing return type annotation ('any' is banned in proof mode)");
    f->ret = resolve_type(ck, s->as.func.ret_type, te);
    ck->dim_params = saved_dp;
    ck->dim_param_count = saved_dpc;
    free(dims);
    ck->in_sig = saved_in_sig;
    f->node = s;
    free(tenv.types);
}

/* pre-register every nested `def` in a block (function-level scoping), so
 * sibling functions can be referenced before their definition appears. */
static void register_nested(Ck *ck, Scope *scope, const Block *b) {
    for (size_t i = 0; i < b->count; i++) {
        const Stmt *s = b->items[i];
        switch (s->kind) {
        case S_FUNC:
            register_func(ck, scope, s);
            break;
        case S_IF:
            for (size_t j = 0; j < s->as.ifs.count; j++)
                register_nested(ck, scope, &s->as.ifs.blocks[j]);
            if (s->as.ifs.has_else)
                register_nested(ck, scope, &s->as.ifs.else_block);
            break;
        case S_WHILE:
            register_nested(ck, scope, &s->as.wh.body);
            break;
        case S_FOR:
            register_nested(ck, scope, &s->as.fr.body);
            break;
        case S_BLOCK:
            register_nested(ck, scope, &s->as.block);
            break;
        case S_MATCH:
            for (size_t j = 0; j < s->as.mtch.count; j++)
                register_nested(ck, scope, &s->as.mtch.blocks[j]);
            break;
        default:
            break;
        }
    }
}

static void check_func(Ck *ck, Scope *parent, const Stmt *s) {
    const char *saved_file = ck->filename;
    if (s->file) ck->filename = s->file;
    TyEnv tenv = func_tyenv(s);
    TyEnv *te = tenv.count ? &tenv : NULL;

    /* `: dim` type parameters are in scope for the body's shape annotations */
    size_t ndims = 0;
    char **dims = func_dims(s, &ndims);
    char **saved_dp = ck->dim_params;
    size_t saved_dpc = ck->dim_param_count;
    ck->dim_params = dims;
    ck->dim_param_count = ndims;

    Scope sc;
    memset(&sc, 0, sizeof(sc));
    sc.parent = parent;
    sc.pure = s->as.func.pure;
    Type **ptypes = xmalloc(sizeof(Type *) *
                            s->as.func.param_count);
    for (size_t i = 0; i < s->as.func.param_count; i++) {
        ptypes[i] = resolve_type(ck, s->as.func.param_types[i], te);
        Var *v = env_add(&sc.locals, s->as.func.params[i], ptypes[i],
                         s->as.func.param_types[i] != NULL);
        v->bound = true;
    }
    /* pre-declare every assigned-in-body name that isn't a global */
    DeclCtx dc = { ck, &sc.locals, "function" };
    ast_collect_assigned(&s->as.func.body, declare_local, &dc);
    register_nested(ck, &sc, &s->as.func.body);

    Scope *saved_scope = ck->scope;
    Type *saved_ret = ck->cur_ret;
    bool saved_pure = ck->cur_pure;
    ck->scope = &sc;
    TyEnv *saved_tyenv = ck->tyenv;
    /* a nested def keeps the enclosing function's type parameters visible */
    if (te) ck->tyenv = te;
    ck->cur_ret = resolve_type(ck, s->as.func.ret_type, te);
    ck->cur_pure = sc.pure;
    /* falling off the end returns None, so a stricter return type demands
     * that every path return explicitly */
    if (!assignable(ck->cur_ret, &t_none) && !block_returns(&s->as.func.body))
        ck_error(ck, "E_TYPE_MISSING_RETURN", s->line, s->col,
                 "%s() can finish without returning a value, but is declared "
                 "to return %s", s->as.func.dispname, type_str(ck->cur_ret));
    check_block(ck, &s->as.func.body);
    if (!s->as.func.partial)
        check_termination(ck, s, ptypes);
    ck->scope = saved_scope;
    ck->cur_ret = saved_ret;
    ck->cur_pure = saved_pure;
    ck->tyenv = saved_tyenv;

    ck->dim_params = saved_dp;
    ck->dim_param_count = saved_dpc;
    free(dims);

    free(ptypes);
    free(sc.locals.items);
    free(sc.funcs);
    free(tenv.types);
    ck->filename = saved_file;
}

size_t check_shape_crossings(void) { return shape_dyn_crossings; }

int check_program(const Program *prog, const char *filename, DiagList *diags,
                  bool proof) {
    Ck ck;
    memset(&ck, 0, sizeof(ck));
    shape_dyn_crossings = 0;
    dim_reset_unresolved();
    ck.filename = filename;
    ck.diags = diags;
    ck.proof = proof;

    /* pass 0: `dim` declarations (nominal dimension names), in file order. They
     * must be in scope for the alias bodies and signatures resolved below. */
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_DIMDECL) continue;
        for (size_t j = 0; j < s->as.dim.count; j++) {
            const char *nm = s->as.dim.names[j];
            bool dup = false;
            for (size_t k = 0; k < ck.dim_count; k++)
                if (strcmp(ck.dim_names[k], nm) == 0) { dup = true; break; }
            if (dup) {
                ck_error(&ck, "E_SHAPE_DUP_DIM", s->line, s->col,
                         "dimension '%s' is declared twice", nm);
                continue;
            }
            if (ck.dim_count == ck.dim_cap) {
                ck.dim_cap = ck.dim_cap ? ck.dim_cap * 2 : 8;
                ck.dim_names = xrealloc(ck.dim_names,
                                       sizeof(char *) * ck.dim_cap);
            }
            ck.dim_names[ck.dim_count++] = s->as.dim.names[j];
        }
    }

    /* pass 1a: type aliases, in file order (use-before-definition is an error).
     * Generic aliases keep their body unresolved and expand at each use. */
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_TYPEDEF) continue;
        if (s->file) ck.filename = s->file;
        if (ck.alias_count == ck.alias_cap) {
            ck.alias_cap = ck.alias_cap ? ck.alias_cap * 2 : 8;
            ck.aliases = xrealloc(ck.aliases, sizeof(*ck.aliases) * ck.alias_cap);
        }
        Alias *al = &ck.aliases[ck.alias_count++];
        al->name = s->as.tdef.name;
        al->disp = s->as.tdef.dispname;
        al->params = s->as.tdef.params;
        al->param_dims = s->as.tdef.param_dims;
        al->param_count = s->as.tdef.param_count;
        al->body = s->as.tdef.value;
        al->resolving = true;
        al->type = al->param_count ? NULL
                                   : resolve_type(&ck, s->as.tdef.value, NULL);
        al->resolving = false;
    }
    ck.filename = filename;

    /* pass 1b: top-level function signatures */
    ck.funcs = xmalloc(sizeof(FuncSig) * prog->body.count);
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_FUNC) continue;
        if (s->file) ck.filename = s->file;
        register_func(&ck, NULL, s);
    }
    ck.filename = filename;

    /* pass 2a: top-level statements (this populates global variable types) */
    check_block(&ck, &prog->body);

    /* pass 2b: function bodies, with all globals known */
    for (size_t i = 0; i < ck.func_count; i++)
        check_func(&ck, NULL, ck.funcs[i].node);

    return ck.errors;
}
