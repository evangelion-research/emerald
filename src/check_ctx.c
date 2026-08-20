/* Checker: the Ck context -- scopes, variable environments, diagnostics,
 * the builtin table, and resolution of surface type expressions. */
#include "check_internal.h"

/* D4: how many static<->dynamic shape crossings were inserted this run */
size_t shape_dyn_crossings = 0;

void note_shape_crossing(Ck *ck, const Type *dst, const Type *src) {
    (void)ck;
    Type *d = ty_resolve(dst), *s = ty_resolve(src);
    if (d->k == TY_TENSOR && s->k == TY_TENSOR &&
        d->tensor.dt == s->tensor.dt &&
        !d->tensor.shape->dynamic && s->tensor.shape->dynamic)
        shape_dyn_crossings++;
}

void ck_error(Ck *ck, const char *code, int line, int col,
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
void ck_error_t(Ck *ck, const char *code, int line, int col,
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

/* A warning (W_ code) is a diagnostic that does not fail the build: the
 * program still checks and runs. `--werror` (or proof mode) promotes it.
 * `-Wno-<code>` silences it before it is ever collected. */
void ck_warn(Ck *ck, const char *code, int line, int col,
                    const char *fmt, ...) {
    if (diag_suppressed(ck->diags, code)) return;
    if (strcmp(code, "W_VACUOUS_PROOF") == 0) proof_rep_vacuous++;
    else if (strcmp(code, "W_UNSOUND_COVARIANCE") == 0) proof_rep_covariance++;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_add_sev(ck->diags, DIA_TYPE, SEV_WARNING, code, ck->filename, line,
                 col, "%s", msg);
}

/* A warning with structured expected/actual types attached. */
static void ck_warn_t(Ck *ck, const char *code, int line, int col,
                      const Type *expected, const Type *actual,
                      const char *fmt, ...) {
    if (diag_suppressed(ck->diags, code)) return;
    if (strcmp(code, "W_UNSOUND_COVARIANCE") == 0) proof_rep_covariance++;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Diag *d = diag_add_sev(ck->diags, DIA_TYPE, SEV_WARNING, code,
                           ck->filename, line, col, "%s", msg);
    diag_set_types(d, expected ? type_str(expected) : NULL,
                   actual ? type_str(actual) : NULL);
}

/* Is `t` a freshly-inferred literal (or a union of them)? Fresh literals are
 * born from literal expressions and widen harmlessly on binding — they carry
 * no aliasing hazard, unlike a *named* `list[Circle]` upcast to `list[Shape]`. */
bool type_is_fresh(const Type *t) {
    const Type *r = ty_resolve(t);
    switch (r->k) {
    case TY_LIT: return r->fresh;
    case TY_LIST: case TY_SEQ: return type_is_fresh(r->elem);
    case TY_REC:
        for (size_t i = 0; i < r->rec.count; i++)
            if (type_is_fresh(r->rec.types[i])) return true;
        return false;
    case TY_UNION:
        for (size_t i = 0; i < r->uni.count; i++)
            if (type_is_fresh(r->uni.alts[i])) return true;
        return false;
    default: return false;
    }
}

static bool type_tainted_rec(const Type *t, TaintVis *v) {
    const Type *r = ty_resolve(t);
    for (size_t i = 0; i < v->count; i++)
        if (v->items[i] == r) return false; /* coinductive: seen on this path */
    if (v->count < 256) v->items[v->count++] = r;
    switch (r->k) {
    case TY_ANY: return true;
    case TY_LIST: case TY_SEQ: return type_tainted_rec(r->elem, v);
    case TY_REC:
        for (size_t i = 0; i < r->rec.count; i++)
            if (type_tainted_rec(r->rec.types[i], v)) return true;
        return false;
    case TY_UNION:
        for (size_t i = 0; i < r->uni.count; i++)
            if (type_tainted_rec(r->uni.alts[i], v)) return true;
        return false;
    case TY_FUNC:
        for (size_t i = 0; i < r->fun.count; i++)
            if (type_tainted_rec(r->fun.params[i], v)) return true;
        return type_tainted_rec(r->fun.ret, v);
    case TY_TENSOR: return r->tensor.shape->dynamic;
    case TY_OPAQUE: return type_tainted_rec(r->elem, v);
    default: return false;
    }
}

bool type_tainted(const Type *t) {
    TaintVis v = {0};
    return type_tainted_rec(t, &v);
}

/* D3 / W5: outside proof mode, a `list[T]` accepted as a `list[U]` by
 * covariance alone (U a strict supertype of T) is unsound — a later `append`
 * of a U could write through the T reference. Warn so the escalation data
 * exists; under --proof the same assignment is a hard error (invariance). */
void ck_covariance(Ck *ck, const Type *dst, const Type *src,
                          int line, int col) {
    if (ck->proof) return;
    const Type *d = ty_resolve(dst), *s = ty_resolve(src);
    if (d->k != TY_LIST || s->k != TY_LIST) return;
    if (type_eq(d->elem, s->elem)) return;
    if (type_is_fresh(s->elem)) return; /* literal widening, not aliasing */
    if (!assignable(d->elem, s->elem)) return;
    ck_warn_t(ck, "W_UNSOUND_COVARIANCE", line, col, d, s,
            "%s used where %s is expected relies on unsound list covariance "
            "(%s widens to %s); use seq[%s] or freeze() for a sound sequence",
            type_str(src), type_str(dst), type_str(s->elem), type_str(d->elem),
            type_str(d->elem));
}

/* W2: proof mode rejects a *tainted* type — one that contains `any` below its
 * top level (list[any], { f: any }, Tensor[f32, ?]). Top-level `any` is caught
 * by the existing checks with their own wording; this closes the hole where
 * `any` hides inside a constructor. */
void ck_proof_taint(Ck *ck, Type *t, int line, int col,
                           const char *noun) {
    if (!ck->proof || t->k == TY_ANY || !type_tainted(t)) return;
    proof_rep_taint_sites++;
    ck_error(ck, "E_PROOF_ANY", line, col,
             "%s has type %s, which contains 'any' (banned in proof mode)",
             noun, type_str(t));
}

Var *env_find(VarEnv *env, const char *name) {
    for (size_t i = 0; i < env->count; i++)
        if (strcmp(env->items[i].name, name) == 0) return &env->items[i];
    return NULL;
}

Var *env_add(VarEnv *env, const char *name, Type *t, bool annotated) {
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
    v->owned = false;
    v->file = NULL;
    v->gen = 0;
    return v;
}

Var *lookup_var(Ck *ck, const char *name) {
    for (Scope *sc = ck->scope; sc; sc = sc->parent) {
        Var *v = env_find(&sc->locals, name);
        if (v) return v;
    }
    return env_find(&ck->globals, name);
}

/* the shared builtin table (include/builtins.def): `pure` marks the builtins
 * a `pure` Emerald function is allowed to call. The per-builtin typing rules
 * live in infer_call(). */
const BuiltinSig builtins[] = {
#define EM_BUILTIN(n, c, a, p) { n, p },
#include "builtins.def"
#undef EM_BUILTIN
#undef EM_BUILTIN_VOID
};

const char *builtin_find(const char *name, bool *pure) {
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (strcmp(builtins[i].name, name) == 0) {
            if (pure) *pure = builtins[i].pure;
            return builtins[i].name;
        }
    return NULL;
}

bool is_builtin(const char *name) {
    return builtin_find(name, NULL) != NULL;
}

FuncSig *find_func(Ck *ck, const char *name) {
    for (Scope *sc = ck->scope; sc; sc = sc->parent)
        for (size_t i = 0; i < sc->func_count; i++)
            if (strcmp(sc->funcs[i].name, name) == 0) return &sc->funcs[i];
    for (size_t i = 0; i < ck->func_count; i++)
        if (strcmp(ck->funcs[i].name, name) == 0) return &ck->funcs[i];
    return NULL;
}

/* The effect of an expression: the join of the effects of everything it calls.
 * A lambda's effect is the effect of its body (creating a closure is pure).
 * Function *values* carry this mask so a `pure` function cannot smuggle an
 * impure callee through `map`/`filter`/`reduce` or an indirect call (D2/W3). */
EffMask expr_eff(Ck *ck, const Expr *e) {
    if (!e) return EFF_PURE;
    EffMask m = EFF_PURE;
    switch (e->kind) {
    case E_CALL: {
        const Expr *fn = e->as.call.fn;
        if (fn->kind == E_NAME) {
            bool bpure = false;
            if (builtin_find(fn->as.sval, &bpure) && !bpure) m |= EFF_IO;
            FuncSig *f = find_func(ck, fn->as.sval);
            if (f) m |= f->eff;
        }
        for (size_t i = 0; i < e->as.call.count; i++)
            m |= expr_eff(ck, e->as.call.args[i]);
        break;
    }
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            m |= expr_eff(ck, e->as.list.items[i]);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            m |= expr_eff(ck, e->as.rec.values[i]);
        break;
    case E_BINOP:
        m |= expr_eff(ck, e->as.bin.lhs);
        m |= expr_eff(ck, e->as.bin.rhs);
        break;
    case E_UNOP:
        m |= expr_eff(ck, e->as.un.operand);
        break;
    case E_INDEX:
        m |= expr_eff(ck, e->as.index.seq);
        m |= expr_eff(ck, e->as.index.idx);
        break;
    case E_ATTR:
        m |= expr_eff(ck, e->as.attr.obj);
        break;
    case E_TRY:
        m |= expr_eff(ck, e->as.try_expr);
        break;
    case E_CATCH:
        m |= expr_eff(ck, e->as.ctch.subject);
        for (size_t i = 0; i < e->as.ctch.count; i++)
            m |= expr_eff(ck, e->as.ctch.arms[i].body);
        break;
    default:
        break;
    }
    return m;
}

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
    if (!builtin &&
        (strcmp(te->name, "Chan") == 0 || strcmp(te->name, "Task") == 0)) {
        if (te->arg_count != 1) {
            ck_error(ck, "E_TYPE_ARITY", te->line, te->col,
                     "type '%s' takes 1 type argument, got %zu",
                     te->name, te->arg_count);
            return &t_any;
        }
        return ty_opaque(strcmp(te->name, "Chan") == 0 ? "Chan" : "Task",
                         resolve_type(ck, te->args[0], env));
    }
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

Type *resolve_type(Ck *ck, const TypeExpr *te, const TyEnv *env) {
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
    case TE_SEQ:
        return ty_seq(resolve_type(ck, te->elem, env));
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
                         "unknown tensor dtype '%s' (v1 supports f32 and f64)",
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
    case TE_EQ:
        return ty_eq(resolve_dim(ck, te->eq_lhs, te->line, te->col),
                     resolve_dim(ck, te->eq_rhs, te->line, te->col));
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
