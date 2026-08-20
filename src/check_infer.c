/* Checker: lambda inference, generic call-site unification, and the tensor
 * primitive typing rules. */
#include "check_internal.h"

Type *infer_lambda_with(Ck *ck, const Expr *e, Type **ptypes) {
    Scope sc;
    memset(&sc, 0, sizeof(sc));
    sc.parent = ck->scope; /* the body may capture enclosing locals */
    for (size_t i = 0; i < e->as.lam.param_count; i++) {
        Var *v = env_add(&sc.locals, e->as.lam.params[i], ptypes[i],
                         e->as.lam.param_types[i] != NULL);
        v->bound = true;
    }
    Scope *saved = ck->scope;
    bool saved_lam = ck->in_lambda;
    bool saved_pure = ck->cur_pure;
    ck->scope = &sc;
    ck->in_lambda = true;
    /* a lambda has its *own* effect (captured in its type), not its enclosing
     * function's: an impure body is fine here, and the caller is what must
     * reject an impure lambda if the caller is `pure` */
    ck->cur_pure = false;
    Type *ret = infer(ck, e->as.lam.body);
    ck->cur_pure = saved_pure;
    ck->in_lambda = saved_lam;
    ck->scope = saved;
    free(sc.locals.items);
    Type *ft = ty_func(ptypes, e->as.lam.param_count, ret);
    ft->fun.eff = expr_eff(ck, e->as.lam.body);
    return ft;
}

/* infer a lambda against an expected function type: unannotated parameters
 * take the expected parameter types (contextual typing); with no expected
 * type they fall back to `any`. */
Type *infer_lambda(Ck *ck, const Expr *e, const Type *expected) {
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

static Type **subst_slot(Subst *s, const char *name) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return &s->types[i];
    return NULL;
}

static bool contains_var(const Type *t) {
    switch (t->k) {
    case TY_VAR:  return true;
    case TY_LIST: return contains_var(t->elem);
    case TY_SEQ:  return contains_var(t->elem);
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
Type *ty_subst(Type *t, Subst *sub) {
    switch (t->k) {
    case TY_VAR: {
        Type **slot = subst_slot(sub, t->var);
        return (slot && *slot) ? *slot : &t_any;
    }
    case TY_LIST: {
        Type *e = ty_subst(t->elem, sub);
        return e == t->elem ? t : ty_list(e);
    }
    case TY_SEQ: {
        Type *e = ty_subst(t->elem, sub);
        return e == t->elem ? t : ty_seq(e);
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
void unify(Type *param, Type *arg, Subst *sub) {
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
    case TY_SEQ:
        if (arg->k == TY_SEQ) unify(param->elem, arg->elem, sub);
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

static bool is_numeric(const Type *t) {
    return t->k == TY_INT || t->k == TY_FLOAT || t->k == TY_BOOL || t->k == TY_ANY;
}

/* --- tensor primitive typing rules --------------------------------------- */
/* If `t` is a tensor, return it. If it is `any`/`never` (the untyped escape
 * hatch), return `&t_any` as a sentinel. Anything else is an error. */
Type *expect_tensor(Ck *ck, const Expr *e, Type *t, const char *fn) {
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

Type *infer_tensor_matmul(Ck *ck, const Expr *e, Type **argt) {
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

Type *infer_tensor_reshape(Ck *ck, const Expr *e, Type **argt) {
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

Type *infer_tensor_transpose(Ck *ck, const Expr *e, Type *t) {
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

Type *infer_tensor_permute(Ck *ck, const Expr *e, Type **argt) {
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

Type *infer_tensor_reduce(Ck *ck, const Expr *e, Type **argt,
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

Type *infer_tensor_expand(Ck *ck, const Expr *e, Type **argt) {
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

Type *infer_tensor_slice(Ck *ck, const Expr *e, Type **argt) {
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

Type *infer_tensor_astype(Ck *ck, const Expr *e, Type **argt) {
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

Type *infer_binop(Ck *ck, const Expr *e) {
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
    case B_IN:
        if (r->k != TY_ANY && r->k != TY_LIST && r->k != TY_SEQ &&
            r->k != TY_STR && r->k != TY_UNION)
            ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                     "right side of 'in' is not a container: %s", type_str(r));
        return &t_bool;
    case B_BITOR: case B_BITXOR: case B_BITAND: case B_LSHIFT: case B_RSHIFT:
        if (l->k == TY_ANY || r->k == TY_ANY) return &t_any;
        if ((l->k != TY_INT && l->k != TY_BOOL) ||
            (r->k != TY_INT && r->k != TY_BOOL))
            ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                     "bitwise operators require int operands, got %s and %s",
                     type_str(l), type_str(r));
        return &t_int;
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
    case B_COMPOSE: { /* functions compose; numeric operands are right shift */
        if (is_numeric(l) && is_numeric(r)) return &t_int;
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
    case B_SUB: case B_MUL: case B_DIV: case B_MOD:
    case B_FLOORDIV: case B_POW: {
        if (l->k == TY_ANY || r->k == TY_ANY) return &t_any;
        /* tensors dispatch elementwise (+ - * / only; no % // ** on tensors) */
        if (op == B_ADD || op == B_SUB || op == B_MUL || op == B_DIV) {
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
            static const char *names[] = {"+", "-", "*", "/", "%", "//", "**"};
            ck_error(ck, "E_TYPE_OPERAND", e->line, e->col,
                     "unsupported operand types for %s: %s and %s",
                     names[op], type_str(l), type_str(r));
            return &t_any;
        }
        if (op == B_DIV) return &t_float; /* Python 3 semantics */
        if (op == B_POW) {
            /* int ** (nonnegative int literal) stays int; a negative or
             * fractional exponent produces a fraction */
            if (l->k == TY_INT && r->k == TY_INT &&
                e->as.bin.rhs->kind == E_INT && e->as.bin.rhs->as.ival >= 0)
                return &t_int;
            return &t_float;
        }
        if (l->k == TY_FLOAT || r->k == TY_FLOAT) return &t_float;
        return &t_int; /* // stays int on int operands, like + - * % */
    }
    }
    return &t_any;
}

/* The arity check every fixed-arity builtin repeats. Returns false (having
 * reported) when the call site does not match. */
bool ck_arity(Ck *ck, const Expr *e, const char *name, size_t want) {
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
Type *infer_map_like(Ck *ck, const Expr *e, const char *name,
                            Type **argt, const bool *islam) {
    size_t argc = e->as.call.count;
    /* map/filter/reduce are overloaded over list and seq: the result keeps the
     * container kind of the sequence argument (reduce has no container). */
    int seq_arg = strcmp(name, "reduce") == 0 ? 2 : 1;
    bool is_seq = false;
    if (seq_arg < (int)argc && !islam[seq_arg]) {
        Type *sarg = ty_resolve(argt[seq_arg]);
        if (sarg->k == TY_SEQ) is_seq = true;
    }
    Type *t = ty_var("T");
    Type *u = ty_var("U");
    Type *f1[] = { t };
    Type *xs = is_seq ? ty_seq(t) : ty_list(t);
    Type *params[3];
    Type *ret;
    size_t want;
    if (strcmp(name, "map") == 0) {
        params[0] = ty_func(f1, 1, u);
        params[1] = xs;
        ret = is_seq ? ty_seq(u) : ty_list(u);
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
    /* W3/D2: a pure function may not smuggle an impure callee through a
     * higher-order builtin */
    if (ck->cur_pure) {
        Type *ft = ty_resolve(argt[0]);
        if (ft->k == TY_FUNC && ft->fun.eff != EFF_PURE)
            ck_error(ck, "E_TYPE_PURE_CALL", e->line, e->col,
                     "pure function passes an impure function to %s()", name);
    }
    Type *r = ty_subst(ret, &sub);
    free(sub.types);
    return r;
}
