/* Checker: the expression dispatcher plus `try`/`catch` expected-error typing. */
#include "check_internal.h"

/* Split a result type into its success and failure payloads. Returns false
 * when `t` is not result-shaped, leaving the outputs untouched. */
static bool result_split(const Type *t, Type **val, Type **err) {
    const Type *r = ty_resolve(t);
    size_t n = r->k == TY_UNION ? r->uni.count : 1;
    Type *v = &t_never, *e = &t_never;
    bool ok_seen = false;
    for (size_t i = 0; i < n; i++) {
        const Type *alt = ty_resolve(r->k == TY_UNION ? r->uni.alts[i] : r);
        if (alt->k != TY_REC) return false;
        const Type *ok = field_type(alt, "ok");
        if (!ok) return false;
        ok = ty_resolve(ok);
        if (ok->k != TY_LIT || ok->lit.base != TY_BOOL) return false;
        const Type *load = field_type(alt, ok->lit.ival ? "val" : "err");
        if (!load) return false;
        if (ok->lit.ival) v = ty_join(v, (Type *)load);
        else e = ty_join(e, (Type *)load);
        ok_seen = true;
    }
    if (!ok_seen) return false;
    if (val) *val = v;
    if (err) *err = e;
    return true;
}

/* The alternatives of an error type, as a flat array (a lone error type is a
 * one-element array). The caller owns the returned array. */
static Type **err_alts(Type *err, size_t *count) {
    Type *r = ty_resolve(err);
    size_t n = r->k == TY_UNION ? r->uni.count : (r->k == TY_NEVER ? 0 : 1);
    Type **out = xmalloc(sizeof(Type *) * (n ? n : 1));
    for (size_t i = 0; i < n; i++)
        out[i] = r->k == TY_UNION ? r->uni.alts[i] : r;
    *count = n;
    return out;
}

/* The `_tag` discriminant of an error alternative, or NULL when it has none
 * (a plain record used as an error payload, which `catch` cannot name). */
static const char *err_tag(const Type *t) {
    const Type *tag = field_type(t, "_tag");
    if (!tag) return NULL;
    tag = ty_resolve(tag);
    if (tag->k != TY_LIT || tag->lit.base != TY_STR) return NULL;
    return tag->lit.sval;
}

/* `try e`: unwrap a result, propagating its failure to the caller. The
 * enclosing function must declare a result type whose error side can carry
 * everything `e` can fail with — the same obligation Rust's `?` imposes. */
static Type *infer_try(Ck *ck, const Expr *e) {
    Type *st = infer(ck, e->as.try_expr);
    Type *val = NULL, *err = NULL;
    if (ty_resolve(st)->k == TY_ANY) return &t_any;
    if (!result_split(st, &val, &err)) {
        ck_error_t(ck, "E_TYPE_TRY", e->line, e->col, NULL, st,
                   "'try' needs a result value ({ ok: True, val: T } | "
                   "{ ok: False, err: E }), got %s", type_str(st));
        return &t_any;
    }
    if (ck->in_lambda) {
        ck_error(ck, "E_TYPE_TRY", e->line, e->col,
                 "'try' inside a lambda has no error channel to propagate to: "
                 "give the lambda body a nested 'def' with a declared result "
                 "type, or handle the failure here with 'catch'");
        return val;
    }
    if (!ck->scope || !ck->cur_ret) {
        ck_error(ck, "E_TYPE_TRY", e->line, e->col,
                 "'try' outside of a function");
        return val;
    }
    if (ty_resolve(ck->cur_ret)->k == TY_ANY) return val; /* gradual: unchecked */
    Type *ret_err = NULL;
    if (!result_split(ck->cur_ret, NULL, &ret_err)) {
        ck_error_t(ck, "E_TYPE_ERRCHAN", e->line, e->col, NULL, ck->cur_ret,
                   "'try' can only propagate out of a function that returns a "
                   "result; this one returns %s", type_str(ck->cur_ret));
        return val;
    }
    if (!assignable(ret_err, err))
        ck_error_t(ck, "E_TYPE_ERRCHAN", e->line, e->col, ret_err, err,
                   "unhandled error: 'try' can fail with %s, which this "
                   "function does not declare (it fails with %s)",
                   type_str(err), type_str(ret_err));
    return val;
}

/* `catch e { Tag b -> body, ... }`: the value of `e` when it succeeded, and
 * the matching arm's value when it failed. Every error the subject can carry
 * must be named by an arm, or by a single catch-all `_`. */
static Type *infer_catch(Ck *ck, const Expr *e) {
    Type *st = infer(ck, e->as.ctch.subject);
    Type *val = NULL, *err = NULL;
    if (!result_split(st, &val, &err)) {
        if (ty_resolve(st)->k == TY_ANY) { val = &t_any; err = &t_any; }
        else {
            ck_error_t(ck, "E_TYPE_CATCH", e->line, e->col, NULL, st,
                       "'catch' needs a result value ({ ok: True, val: T } | "
                       "{ ok: False, err: E }), got %s", type_str(st));
            return &t_any;
        }
    }
    size_t nalts = 0;
    Type **alts = err_alts(err, &nalts);
    bool *covered = xmalloc(sizeof(bool) * (nalts ? nalts : 1));
    for (size_t i = 0; i < nalts; i++) covered[i] = false;

    VarEnv *env = ck->scope ? &ck->scope->locals : &ck->globals;
    Type *result = val;
    bool has_wild = false;
    for (size_t i = 0; i < e->as.ctch.count; i++) {
        const CatchArm *a = &e->as.ctch.arms[i];
        Type *bound = &t_any;
        if (a->tag) {
            bound = &t_never;
            bool found = false;
            for (size_t j = 0; j < nalts; j++) {
                const char *tag = err_tag(alts[j]);
                if (!tag || strcmp(tag, a->tag) != 0) continue;
                if (covered[j])
                    ck_error(ck, "E_TYPE_CATCH", a->line, a->col,
                             "error '%s' is already handled by an earlier arm",
                             a->tag);
                covered[j] = true;
                bound = ty_join(bound, alts[j]);
                found = true;
            }
            if (!found) {
                if (ty_resolve(err)->k == TY_ANY) bound = &t_any;
                else
                    ck_error_t(ck, "E_TYPE_CATCH", a->line, a->col, err, NULL,
                               "'%s' is not one of the errors this expression "
                               "can fail with (%s)", a->tag, type_str(err));
            }
        } else {
            if (has_wild)
                ck_error(ck, "E_TYPE_CATCH", a->line, a->col,
                         "'catch' has more than one catch-all arm");
            /* the catch-all binds whatever the named arms did not take */
            Type *rest = &t_never;
            for (size_t j = 0; j < nalts; j++)
                if (!covered[j]) rest = ty_join(rest, alts[j]);
            bound = ty_resolve(err)->k == TY_ANY ? &t_any : rest;
            has_wild = true;
        }
        size_t mark = env->count; /* the binding is scoped to its own arm */
        if (a->bind) {
            Var *v = env_add(env, a->bind, bound, true);
            v->bound = true;
            v->is_const = true;
        }
        result = ty_join(result, infer(ck, a->body));
        env->count = mark;
    }
    if (!has_wild) {
        if (ty_resolve(err)->k == TY_ANY) {
            /* nothing is known about what this can fail with, so only a
             * catch-all can cover it */
            ck_error(ck, "E_TYPE_CATCH", e->line, e->col,
                     "'catch' on a value of type 'any' needs a catch-all "
                     "arm ('_')");
        } else {
            for (size_t j = 0; j < nalts; j++) {
                if (covered[j]) continue;
                const char *tag = err_tag(alts[j]);
                ck_error_t(ck, "E_TYPE_CATCH", e->line, e->col, err, NULL,
                           "'catch' is not exhaustive: %s is never handled "
                           "(add an arm for it, or a catch-all '_')",
                           tag ? tag : type_str(alts[j]));
            }
        }
    }
    free(alts);
    free(covered);
    return result;
}

Type *infer(Ck *ck, const Expr *e) {
    switch (e->kind) {
    case E_TRY: return infer_try(ck, e);
    case E_CATCH: return infer_catch(ck, e);
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
        /* refl : Eq[a, a]. The marker (NULL sides) is resolved by assignable()
         * against the expected Eq[a, b] via dim_eq(a, b). */
        if (strcmp(e->as.sval, "refl") == 0)
            return ty_eq(NULL, NULL);
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
            Type *ft = ty_func(params, f->param_count, f->ret);
            ft->fun.eff = f->eff;
            return ft;
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
    case E_TUPLE:
        for (size_t i = 0; i < e->as.list.count; i++) infer(ck, e->as.list.items[i]);
        return &t_any;
    case E_DICT:
        for (size_t i = 0; i < e->as.dict.count; i++) {
            Type *key = ty_base(infer(ck, e->as.dict.keys[i]));
            infer(ck, e->as.dict.values[i]);
            if (key->k != TY_STR && key->k != TY_ANY)
                ck_error(ck, "E_TYPE_DICT_KEY", e->line, e->col,
                         "dict keys must be str, got %s", type_str(key));
        }
        return &t_any;
    case E_SET:
        for (size_t i = 0; i < e->as.set.count; i++) infer(ck,e->as.set.items[i]);
        return &t_any;
    case E_SLICE: {
        Type *s = ty_base(infer(ck,e->as.slice.seq));
        if(e->as.slice.start) infer(ck,e->as.slice.start); if(e->as.slice.stop) infer(ck,e->as.slice.stop); if(e->as.slice.step) infer(ck,e->as.slice.step);
        if(s->k==TY_LIST||s->k==TY_SEQ||s->k==TY_STR) return s;
        return &t_any;
    }
    case E_FSTR:
        for(size_t i=0;i<e->as.fstr.count;i++) infer(ck,e->as.fstr.exprs[i]);
        return &t_str;
    case E_COMP: {
        infer(ck,e->as.comp.seq); Scope sc; memset(&sc,0,sizeof(sc)); sc.parent=ck->scope;
        Var *v=env_add(&sc.locals,e->as.comp.var,&t_any,false); v->bound=true;
        Scope *saved=ck->scope; ck->scope=&sc;
        if(e->as.comp.cond) infer(ck,e->as.comp.cond);
        Type *et=infer(ck,e->as.comp.elt);
        if (e->as.comp.key) {
            Type *key = ty_base(infer(ck, e->as.comp.key));
            if (key->k != TY_STR && key->k != TY_ANY)
                ck_error(ck, "E_TYPE_DICT_KEY", e->line, e->col,
                         "dict comprehension keys must be str, got %s", type_str(key));
        }
        ck->scope=saved; free(sc.locals.items);
        if(e->as.comp.kind==COMP_LIST) return ty_list(et); return &t_any;
    }
    case E_REC: {
        Type *t = ty_new(TY_REC);
        t->rec.names = e->as.rec.names;
        t->rec.types = xmalloc(sizeof(Type *) * e->as.rec.count);
        t->rec.count = e->as.rec.count;
        for (size_t i = 0; i < e->as.rec.count; i++) {
            Type *ft = infer(ck, e->as.rec.values[i]);
            /* `_tag` is a discriminant, not a string that happens to be
             * constant here: it keeps its literal type instead of widening to
             * `str`, so a tagged error still matches the type `error Name`
             * declared even after passing through a generic like `err`. */
            if (strcmp(e->as.rec.names[i], "_tag") == 0 && ft->k == TY_LIT &&
                ft->fresh && ft->lit.base == TY_STR) {
                Type *lit = ty_lit_str(ft->lit.sval);
                ft = lit; /* born non-fresh: widen() leaves it alone */
            }
            t->rec.types[i] = ft;
        }
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
        return infer_call(ck, e, NULL);
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
        if (seq->k != TY_ANY && (!assignable(&t_int, idx) || idx->k == TY_FLOAT))
            ck_error(ck, "E_TYPE_INDEX", e->line, e->col,
                     "index must be int, got %s", type_str(idx));
        if (seq->k == TY_NEVER) return &t_never;
        if (seq->k == TY_LIST) return seq->elem;
        if (seq->k == TY_SEQ) return seq->elem;
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
