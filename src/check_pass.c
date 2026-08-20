/* Checker: the top-level passes -- declaration collection, per-function
 * checking, and the proof report. */
#include "check_internal.h"

/* Is `name` a global this file may update by assignment? The docs rule is that
 * assigning a global's name inside a def updates the global — but only within
 * the module that declared it. Across a module boundary the names are
 * unrelated: `for xs in xss` inside a library function must not write to an
 * importer's `xs` just because the linker put them in one translation unit. */
bool updatable_global(Ck *ck, const char *name, const char *file) {
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
    /* a non-`pure` function is conservatively IO (effect inference would split
     * it into the 5 labels; purity checking only needs the pure/impure divide) */
    f->eff = s->as.func.pure ? EFF_PURE : EFF_IO;
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
        ck_proof_taint(ck, f->params[j], s->line, s->col, "parameter");
    }
    if (ck->proof && s->as.func.ret_type == NULL)
        ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                 "missing return type annotation ('any' is banned in proof mode)");
    f->ret = resolve_type(ck, s->as.func.ret_type, te);
    ck_proof_taint(ck, f->ret, s->line, s->col, "return type");
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

void check_func(Ck *ck, Scope *parent, const Stmt *s) {
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
    size_t saved_evid = eq_evidence_count;
    for (size_t i = 0; i < s->as.func.param_count; i++) {
        ptypes[i] = resolve_type(ck, s->as.func.param_types[i], te);
        Var *v = env_add(&sc.locals, s->as.func.params[i], ptypes[i],
                         s->as.func.param_types[i] != NULL);
        v->bound = true;
        /* W7: a parameter `e: Eq[a, b]` puts `a == b` in scope for the body */
        const Type *rt = ty_resolve(ptypes[i]);
        if (rt->k == TY_EQ && rt->eq.lhs && rt->eq.rhs &&
            eq_evidence_count < 64) {
            eq_evidence_l[eq_evidence_count] = rt->eq.lhs;
            eq_evidence_r[eq_evidence_count] = rt->eq.rhs;
            eq_evidence_count++;
        }
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
    eq_evidence_count = saved_evid; /* pop the Eq[a, b] parameters */
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

/* W8: the --proof-report measurement, valid after the last check_program(). */
const ProofReport *proof_report_get(void) {
    static ProofReport rep;
    rep.total_funcs = proof_rep_total_funcs;
    rep.partial_funcs = proof_rep_partial_funcs;
    rep.pure_funcs = proof_rep_pure_funcs;
    rep.vacuous_obligations = proof_rep_vacuous;
    rep.covariance_warnings = proof_rep_covariance;
    rep.taint_sites = proof_rep_taint_sites;
    rep.partial_names = (const char *const *)proof_rep_partial_names;
    rep.partial_name_count = proof_rep_partial_n;
    return &rep;
}

int check_program(const Program *prog, const char *filename, DiagList *diags,
                  bool proof) {
    Ck ck;
    memset(&ck, 0, sizeof(ck));
    shape_dyn_crossings = 0;
    dim_reset_unresolved();
    proof_rep_reset();
    ck.filename = filename;
    ck.diags = diags;
    ck.proof = proof;
    ck_proof_mode = proof;

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

    /* pass 3: mutual recursion (W4) — cycles across the top-level call graph */
    check_mutual_recursion(&ck);

    /* W8: fold the top-level function inventory into the proof report */
    proof_rep_total_funcs = ck.func_count;
    for (size_t i = 0; i < ck.func_count; i++) {
        const FuncSig *f = &ck.funcs[i];
        if (f->pure) proof_rep_pure_funcs++;
        if (f->partial) {
            proof_rep_partial_funcs++;
            if (proof_rep_partial_n == proof_rep_partial_cap) {
                proof_rep_partial_cap = proof_rep_partial_cap ? proof_rep_partial_cap * 2 : 8;
                proof_rep_partial_names = xrealloc(proof_rep_partial_names,
                    sizeof(char *) * proof_rep_partial_cap);
            }
            proof_rep_partial_names[proof_rep_partial_n++] = (char *)f->disp;
        }
    }

    return ck.errors;
}
