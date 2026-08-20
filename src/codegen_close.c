/* Codegen: closure analysis -- captured variables, nested definitions, and
 * the per-function slot frames the GC roots. */
#include "codegen_internal.h"

static void collect_used_expr(const Expr *e, Names *out);

static void collect_used_block(const Block *b, Names *out);

static void collect_comp_vars_expr(const Expr *e, Names *out);

static void collect_comp_vars_block(const Block *b, Names *out);

static void collect_comp_vars_expr(const Expr *e, Names *out) {
    if (!e) return;
    switch(e->kind) {
    case E_COMP: names_add(out,e->as.comp.var); collect_comp_vars_expr(e->as.comp.seq,out); collect_comp_vars_expr(e->as.comp.cond,out); collect_comp_vars_expr(e->as.comp.elt,out); collect_comp_vars_expr(e->as.comp.key,out); break;
    case E_LIST: case E_TUPLE: for(size_t i=0;i<e->as.list.count;i++)collect_comp_vars_expr(e->as.list.items[i],out); break;
    case E_REC: for(size_t i=0;i<e->as.rec.count;i++)collect_comp_vars_expr(e->as.rec.values[i],out); break;
    case E_DICT: for(size_t i=0;i<e->as.dict.count;i++){collect_comp_vars_expr(e->as.dict.keys[i],out);collect_comp_vars_expr(e->as.dict.values[i],out);} break;
    case E_SET: for(size_t i=0;i<e->as.set.count;i++)collect_comp_vars_expr(e->as.set.items[i],out); break;
    case E_BINOP: collect_comp_vars_expr(e->as.bin.lhs,out);collect_comp_vars_expr(e->as.bin.rhs,out);break;
    case E_UNOP: collect_comp_vars_expr(e->as.un.operand,out);break;
    case E_CALL: collect_comp_vars_expr(e->as.call.fn,out);for(size_t i=0;i<e->as.call.count;i++)collect_comp_vars_expr(e->as.call.args[i],out);break;
    case E_INDEX: collect_comp_vars_expr(e->as.index.seq,out);collect_comp_vars_expr(e->as.index.idx,out);break;
    case E_SLICE: collect_comp_vars_expr(e->as.slice.seq,out);collect_comp_vars_expr(e->as.slice.start,out);collect_comp_vars_expr(e->as.slice.stop,out);collect_comp_vars_expr(e->as.slice.step,out);break;
    case E_ATTR: collect_comp_vars_expr(e->as.attr.obj,out);break;
    case E_FSTR: for(size_t i=0;i<e->as.fstr.count;i++)collect_comp_vars_expr(e->as.fstr.exprs[i],out);break;
    default: break;
    }
}

static void collect_comp_vars_block(const Block *b, Names *out) { for(size_t i=0;i<b->count;i++){const Stmt*s=b->items[i];if(s->kind==S_EXPR)collect_comp_vars_expr(s->as.expr,out);else if(s->kind==S_ASSIGN){collect_comp_vars_expr(s->as.assign.value,out);collect_comp_vars_expr(s->as.assign.target,out);}else if(s->kind==S_RETURN)collect_comp_vars_expr(s->as.ret,out);else if(s->kind==S_IF){for(size_t j=0;j<s->as.ifs.count;j++){collect_comp_vars_expr(s->as.ifs.conds[j],out);collect_comp_vars_block(&s->as.ifs.blocks[j],out);}if(s->as.ifs.has_else)collect_comp_vars_block(&s->as.ifs.else_block,out);}else if(s->kind==S_FOR){collect_comp_vars_expr(s->as.fr.seq,out);collect_comp_vars_block(&s->as.fr.body,out);}else if(s->kind==S_WHILE){collect_comp_vars_expr(s->as.wh.cond,out);collect_comp_vars_block(&s->as.wh.body,out);}else if(s->kind==S_BLOCK){collect_comp_vars_block(&s->as.block,out);}else if(s->kind==S_MATCH){collect_comp_vars_expr(s->as.mtch.subject,out);for(size_t j=0;j<s->as.mtch.count;j++)collect_comp_vars_block(&s->as.mtch.blocks[j],out);}}}

static void collect_used_expr(const Expr *e, Names *out) {
    if (!e) return;
    switch (e->kind) {
    case E_NAME:
        names_add(out, e->as.sval);
        break;
    case E_LIST: case E_TUPLE:
        for (size_t i = 0; i < e->as.list.count; i++) collect_used_expr(e->as.list.items[i], out);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++) collect_used_expr(e->as.rec.values[i], out);
        break;
    case E_DICT:
        for(size_t i=0;i<e->as.dict.count;i++){collect_used_expr(e->as.dict.keys[i],out);collect_used_expr(e->as.dict.values[i],out);} break;
    case E_SET: for(size_t i=0;i<e->as.set.count;i++)collect_used_expr(e->as.set.items[i],out); break;
    case E_SLICE: collect_used_expr(e->as.slice.seq,out);collect_used_expr(e->as.slice.start,out);collect_used_expr(e->as.slice.stop,out);collect_used_expr(e->as.slice.step,out);break;
    case E_COMP: collect_used_expr(e->as.comp.seq,out);collect_used_expr(e->as.comp.cond,out);collect_used_expr(e->as.comp.elt,out);collect_used_expr(e->as.comp.key,out);break;
    case E_FSTR: for(size_t i=0;i<e->as.fstr.count;i++)collect_used_expr(e->as.fstr.exprs[i],out);break;
    case E_BINOP:
        collect_used_expr(e->as.bin.lhs, out);
        collect_used_expr(e->as.bin.rhs, out);
        break;
    case E_UNOP:
        collect_used_expr(e->as.un.operand, out);
        break;
    case E_CALL:
        collect_used_expr(e->as.call.fn, out);
        for (size_t i = 0; i < e->as.call.count; i++)
            collect_used_expr(e->as.call.args[i], out);
        break;
    case E_INDEX:
        collect_used_expr(e->as.index.seq, out);
        collect_used_expr(e->as.index.idx, out);
        break;
    case E_ATTR:
        collect_used_expr(e->as.attr.obj, out);
        break;
    case E_TRY:
        collect_used_expr(e->as.try_expr, out);
        break;
    case E_CATCH:
        collect_used_expr(e->as.ctch.subject, out);
        for (size_t i = 0; i < e->as.ctch.count; i++)
            collect_used_expr(e->as.ctch.arms[i].body, out);
        break;
    case E_LAMBDA:
        /* a lambda is its own function: its free variables are captured by
         * its own FuncInfo, not by the enclosing scope */
        break;
    default:
        break;
    }
}

static void collect_used_stmt(const Stmt *s, Names *out) {
    switch (s->kind) {
    case S_EXPR:
        collect_used_expr(s->as.expr, out);
        break;
    case S_ASSIGN:
        collect_used_expr(s->as.assign.value, out);
        if (s->as.assign.target->kind == E_INDEX) {
            collect_used_expr(s->as.assign.target->as.index.seq, out);
            collect_used_expr(s->as.assign.target->as.index.idx, out);
        } else if (s->as.assign.target->kind == E_ATTR) {
            collect_used_expr(s->as.assign.target->as.attr.obj, out);
        }
        break;
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++) {
            collect_used_expr(s->as.ifs.conds[i], out);
            collect_used_block(&s->as.ifs.blocks[i], out);
        }
        if (s->as.ifs.has_else)
            collect_used_block(&s->as.ifs.else_block, out);
        break;
    case S_WHILE:
        collect_used_expr(s->as.wh.cond, out);
        collect_used_block(&s->as.wh.body, out);
        break;
    case S_FOR:
        collect_used_expr(s->as.fr.seq, out);
        collect_used_block(&s->as.fr.body, out);
        break;
    case S_RETURN:
        if (s->as.ret) collect_used_expr(s->as.ret, out);
        break;
    case S_BLOCK:
        collect_used_block(&s->as.block, out);
        break;
    case S_MATCH:
        collect_used_expr(s->as.mtch.subject, out);
        for (size_t i = 0; i < s->as.mtch.count; i++)
            collect_used_block(&s->as.mtch.blocks[i], out);
        break;
    default:
        break; /* S_FUNC: nested body analyzed on its own */
    }
}

static void collect_used_block(const Block *b, Names *out) {
    for (size_t i = 0; i < b->count; i++)
        collect_used_stmt(b->items[i], out);
}

/* collect every nested `def` in a block (function-level scoping) */
static void collect_child_defs(const Block *b, Stmt ***defs, size_t *count,
                               size_t *cap) {
    for (size_t i = 0; i < b->count; i++) {
        const Stmt *s = b->items[i];
        switch (s->kind) {
        case S_FUNC:
            if (*count == *cap) {
                *cap = *cap ? *cap * 2 : 4;
                *defs = xrealloc(*defs, sizeof(Stmt *) * *cap);
            }
            (*defs)[(*count)++] = (Stmt *)s;
            break;
        case S_IF:
            for (size_t j = 0; j < s->as.ifs.count; j++)
                collect_child_defs(&s->as.ifs.blocks[j], defs, count, cap);
            if (s->as.ifs.has_else)
                collect_child_defs(&s->as.ifs.else_block, defs, count, cap);
            break;
        case S_WHILE:
            collect_child_defs(&s->as.wh.body, defs, count, cap);
            break;
        case S_FOR:
            collect_child_defs(&s->as.fr.body, defs, count, cap);
            break;
        case S_BLOCK:
            collect_child_defs(&s->as.block, defs, count, cap);
            break;
        case S_MATCH:
            for (size_t j = 0; j < s->as.mtch.count; j++)
                collect_child_defs(&s->as.mtch.blocks[j], defs, count, cap);
            break;
        default:
            break;
        }
    }
}

/* collect every lambda expression in a block and its nested statements,
 * without descending into nested `def` bodies (those are collected as their
 * own functions) — lambdas become FuncInfo children so their captures get
 * the same closure treatment as nested defs. */
static void collect_lambdas_expr(const Expr *e, Expr ***lams, size_t *count,
                                 size_t *cap);

static void collect_lambdas_block(const Block *b, Expr ***lams, size_t *count,
                                  size_t *cap) {
    for (size_t i = 0; i < b->count; i++) {
        const Stmt *s = b->items[i];
        switch (s->kind) {
        case S_EXPR:
            collect_lambdas_expr(s->as.expr, lams, count, cap);
            break;
        case S_ASSIGN:
            collect_lambdas_expr(s->as.assign.value, lams, count, cap);
            if (s->as.assign.target->kind == E_INDEX)
                collect_lambdas_expr(s->as.assign.target->as.index.seq,
                                     lams, count, cap);
            else if (s->as.assign.target->kind == E_ATTR)
                collect_lambdas_expr(s->as.assign.target->as.attr.obj,
                                     lams, count, cap);
            break;
        case S_IF:
            for (size_t j = 0; j < s->as.ifs.count; j++) {
                collect_lambdas_expr(s->as.ifs.conds[j], lams, count, cap);
                collect_lambdas_block(&s->as.ifs.blocks[j], lams, count, cap);
            }
            if (s->as.ifs.has_else)
                collect_lambdas_block(&s->as.ifs.else_block, lams, count, cap);
            break;
        case S_WHILE:
            collect_lambdas_expr(s->as.wh.cond, lams, count, cap);
            collect_lambdas_block(&s->as.wh.body, lams, count, cap);
            break;
        case S_FOR:
            collect_lambdas_expr(s->as.fr.seq, lams, count, cap);
            collect_lambdas_block(&s->as.fr.body, lams, count, cap);
            break;
        case S_RETURN:
            if (s->as.ret)
                collect_lambdas_expr(s->as.ret, lams, count, cap);
            break;
        case S_BLOCK:
            collect_lambdas_block(&s->as.block, lams, count, cap);
            break;
        case S_MATCH:
            collect_lambdas_expr(s->as.mtch.subject, lams, count, cap);
            for (size_t j = 0; j < s->as.mtch.count; j++)
                collect_lambdas_block(&s->as.mtch.blocks[j], lams, count, cap);
            break;
        default:
            break; /* S_FUNC: nested defs collected separately */
        }
    }
}

static void collect_lambdas_expr(const Expr *e, Expr ***lams, size_t *count,
                                 size_t *cap) {
    if (!e) return;
    switch (e->kind) {
    case E_LAMBDA:
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *lams = xrealloc(*lams, sizeof(Expr *) * *cap);
        }
        (*lams)[(*count)++] = (Expr *)e;
        collect_lambdas_expr(e->as.lam.body, lams, count, cap);
        break;
    case E_LIST: case E_TUPLE:
        for (size_t i = 0; i < e->as.list.count; i++) collect_lambdas_expr(e->as.list.items[i], lams, count, cap);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++) collect_lambdas_expr(e->as.rec.values[i], lams, count, cap);
        break;
    case E_DICT:
        for(size_t i=0;i<e->as.dict.count;i++){collect_lambdas_expr(e->as.dict.keys[i],lams,count,cap);collect_lambdas_expr(e->as.dict.values[i],lams,count,cap);}break;
    case E_SET: for(size_t i=0;i<e->as.set.count;i++)collect_lambdas_expr(e->as.set.items[i],lams,count,cap);break;
    case E_SLICE: collect_lambdas_expr(e->as.slice.seq,lams,count,cap);collect_lambdas_expr(e->as.slice.start,lams,count,cap);collect_lambdas_expr(e->as.slice.stop,lams,count,cap);collect_lambdas_expr(e->as.slice.step,lams,count,cap);break;
    case E_COMP: collect_lambdas_expr(e->as.comp.seq,lams,count,cap);collect_lambdas_expr(e->as.comp.cond,lams,count,cap);collect_lambdas_expr(e->as.comp.elt,lams,count,cap);collect_lambdas_expr(e->as.comp.key,lams,count,cap);break;
    case E_FSTR: for(size_t i=0;i<e->as.fstr.count;i++)collect_lambdas_expr(e->as.fstr.exprs[i],lams,count,cap);break;
    case E_BINOP:
        collect_lambdas_expr(e->as.bin.lhs, lams, count, cap);
        collect_lambdas_expr(e->as.bin.rhs, lams, count, cap);
        break;
    case E_UNOP:
        collect_lambdas_expr(e->as.un.operand, lams, count, cap);
        break;
    case E_CALL:
        collect_lambdas_expr(e->as.call.fn, lams, count, cap);
        for (size_t i = 0; i < e->as.call.count; i++)
            collect_lambdas_expr(e->as.call.args[i], lams, count, cap);
        break;
    case E_INDEX:
        collect_lambdas_expr(e->as.index.seq, lams, count, cap);
        collect_lambdas_expr(e->as.index.idx, lams, count, cap);
        break;
    case E_ATTR:
        collect_lambdas_expr(e->as.attr.obj, lams, count, cap);
        break;
    case E_TRY:
        collect_lambdas_expr(e->as.try_expr, lams, count, cap);
        break;
    case E_CATCH:
        collect_lambdas_expr(e->as.ctch.subject, lams, count, cap);
        for (size_t i = 0; i < e->as.ctch.count; i++)
            collect_lambdas_expr(e->as.ctch.arms[i].body, lams, count, cap);
        break;
    default:
        break;
    }
}

static void collect_local_cb(const char *name, const char *file, int line,
                             void *ud);

/* every name a match pattern binds, recursively (needs an F/G slot: codegen
 * writes the bound value into it when an arm matches) */
static void pat_bind_names(const Pat *p,
                           void (*fn)(const char *, const char *, int, void *),
                           const char *file, void *ud) {
    switch (p->kind) {
    case P_BIND:
        fn(p->bind, file, p->line, ud);
        break;
    case P_REC:
        for (size_t i = 0; i < p->rec.count; i++)
            pat_bind_names(p->rec.items[i], fn, file, ud);
        break;
    default:
        break;
    }
}

/* Names bound by `catch` arms. They live in expressions rather than
 * statements, so this walks expression positions the way pat_bind_names walks
 * a pattern. Lambda bodies are skipped: a lambda is compiled as its own
 * function, and its binds become locals there. */
static void collect_catch_binds(const Expr *e,
                                void (*fn)(const char *, const char *, int,
                                           void *),
                                const char *file, void *ud) {
    if (!e) return;
    switch (e->kind) {
    case E_CATCH:
        collect_catch_binds(e->as.ctch.subject, fn, file, ud);
        for (size_t i = 0; i < e->as.ctch.count; i++) {
            const CatchArm *a = &e->as.ctch.arms[i];
            if (a->bind) fn(a->bind, file, a->line, ud);
            collect_catch_binds(a->body, fn, file, ud);
        }
        break;
    case E_TRY:
        collect_catch_binds(e->as.try_expr, fn, file, ud);
        break;
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            collect_catch_binds(e->as.list.items[i], fn, file, ud);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            collect_catch_binds(e->as.rec.values[i], fn, file, ud);
        break;
    case E_BINOP:
        collect_catch_binds(e->as.bin.lhs, fn, file, ud);
        collect_catch_binds(e->as.bin.rhs, fn, file, ud);
        break;
    case E_UNOP:
        collect_catch_binds(e->as.un.operand, fn, file, ud);
        break;
    case E_CALL:
        collect_catch_binds(e->as.call.fn, fn, file, ud);
        for (size_t i = 0; i < e->as.call.count; i++)
            collect_catch_binds(e->as.call.args[i], fn, file, ud);
        break;
    case E_INDEX:
        collect_catch_binds(e->as.index.seq, fn, file, ud);
        collect_catch_binds(e->as.index.idx, fn, file, ud);
        break;
    case E_ATTR:
        collect_catch_binds(e->as.attr.obj, fn, file, ud);
        break;
    default:
        break;
    }
}

static void collect_match_pats_stmt(const Stmt *s,
                                    void (*fn)(const char *, const char *,
                                               int, void *),
                                    void *ud);

void collect_match_pats_block(const Block *b,
                                     void (*fn)(const char *, const char *,
                                                int, void *),
                                     void *ud) {
    for (size_t i = 0; i < b->count; i++)
        collect_match_pats_stmt(b->items[i], fn, ud);
}

static void collect_match_pats_stmt(const Stmt *s,
                                    void (*fn)(const char *, const char *,
                                               int, void *),
                                    void *ud) {
    switch (s->kind) {
    case S_MATCH:
        collect_catch_binds(s->as.mtch.subject, fn, s->file, ud);
        for (size_t j = 0; j < s->as.mtch.count; j++) {
            pat_bind_names(s->as.mtch.pats[j], fn, s->file, ud);
            collect_match_pats_block(&s->as.mtch.blocks[j], fn, ud);
        }
        break;
    case S_IF:
        for (size_t j = 0; j < s->as.ifs.count; j++) {
            collect_catch_binds(s->as.ifs.conds[j], fn, s->file, ud);
            collect_match_pats_block(&s->as.ifs.blocks[j], fn, ud);
        }
        if (s->as.ifs.has_else)
            collect_match_pats_block(&s->as.ifs.else_block, fn, ud);
        break;
    case S_WHILE:
        collect_catch_binds(s->as.wh.cond, fn, s->file, ud);
        collect_match_pats_block(&s->as.wh.body, fn, ud);
        break;
    case S_FOR:
        collect_catch_binds(s->as.fr.seq, fn, s->file, ud);
        collect_match_pats_block(&s->as.fr.body, fn, ud);
        break;
    case S_BLOCK:
        collect_match_pats_block(&s->as.block, fn, ud);
        break;
    case S_EXPR:
        collect_catch_binds(s->as.expr, fn, s->file, ud);
        break;
    case S_ASSIGN:
        collect_catch_binds(s->as.assign.value, fn, s->file, ud);
        break;
    case S_RETURN:
        collect_catch_binds(s->as.ret, fn, s->file, ud);
        break;
    default:
        break; /* S_FUNC: nested bodies are separate scopes */
    }
}

/* does `name` resolve to a local of some enclosing function? */
static bool bound_in_ancestor(FuncInfo *fi, const char *name) {
    for (FuncInfo *f = fi->parent; f; f = f->parent)
        if (names_find(&f->locals, name) >= 0) return true;
    return false;
}

static void collect_local_cb(const char *name, const char *file, int line,
                             void *ud) {
    (void)line;
    LocalCtx *lc = ud;
    /* the docs rule: assigning a same-module global's name updates the global;
     * and an assignment to an enclosing function's local is a capture, not a
     * new local — it updates the shared cell, so closures observe the mutation */
    if (!global_owned_by(lc->globals, name, file) &&
        !bound_in_ancestor(lc->fi, name))
        add_local(lc->fi, name);
}

static void compute_captures(FuncInfo *fi, const Block *body) {
    Names used = {0};
    collect_used_block(body, &used);
    for (size_t i = 0; i < used.count; i++) {
        const char *n = used.names[i];
        if (names_find(&fi->locals, n) >= 0) continue;   /* bound here */
        if (bound_in_ancestor(fi, n)) {                  /* enclosing local */
            names_add(&fi->captures, n);
            continue;
        }
        /* anything else (global, top-level function, builtin) needs no
         * capture slot */
    }
    /* forward children's captures that aren't bound here (deep capture) */
    for (size_t c = 0; c < fi->child_count; c++) {
        FuncInfo *ch = fi->children[c];
        for (size_t i = 0; i < ch->captures.count; i++) {
            const char *n = ch->captures.names[i];
            if (names_find(&fi->locals, n) >= 0) continue;
            names_add(&fi->captures, n);
        }
    }
    free(used.names);
}

static void compute_boxing(FuncInfo *fi);

/* Build the FuncInfo for a lambda expression: the lambda is lowered to a
 * synthetic `def __lamN(x, ...) { return body }` so it flows through the
 * exact same closure machinery as nested defs (captures, boxing, env). */
static FuncInfo *build_lambda(Ctx *ctx, const Expr *lam, FuncInfo *parent) {
    char name[32];
    snprintf(name, sizeof(name), "__lam%d", ctx->counter);
    Stmt *ret = xcalloc(1, sizeof(Stmt));
    ret->kind = S_RETURN;
    ret->line = lam->line;
    ret->col = lam->col;
    ret->as.ret = (Expr *)lam->as.lam.body;
    Stmt *func = xcalloc(1, sizeof(Stmt));
    func->kind = S_FUNC;
    func->line = lam->line;
    func->col = lam->col;
    func->as.func.name = strdup(name);
    func->as.func.dispname = func->as.func.name;
    func->as.func.params = lam->as.lam.params;
    func->as.func.param_types = lam->as.lam.param_types;
    func->as.func.param_count = lam->as.lam.param_count;
    func->as.func.body.count = 1;
    func->as.func.body.items = xmalloc(sizeof(Stmt *));
    func->as.func.body.items[0] = ret;
    FuncInfo *fi = build_func(ctx, func, parent);
    fi->lamb = lam;
    return fi;
}

/* The implicit top-level function: the program body generated into main().
 * It owns the top-level lambdas as children (so they get closure treatment)
 * but has no locals of its own — everything at the top level is a global. */
FuncInfo *make_top_root(Ctx *ctx, const Block *body) {
    FuncInfo *fi = xcalloc(1, sizeof(FuncInfo));
    fi->parent = NULL;
    fi->name = "<top>";
    fi->node = NULL;
    Expr **lams = NULL;
    size_t n = 0, cap = 0;
    collect_lambdas_block(body, &lams, &n, &cap);
    Names comp = {0}; collect_comp_vars_block(body, &comp);
    for (size_t ci = 0; ci < comp.count; ci++) add_local(fi, comp.names[ci]);
    free(comp.names);
    fi->child_cap = n ? n : 1;
    fi->children = xcalloc(fi->child_cap, sizeof(FuncInfo *));
    fi->child_count = n;
    for (size_t i = 0; i < n; i++)
        fi->children[i] = build_lambda(ctx, lams[i], fi);
    free(lams);
    compute_captures(fi, body);
    compute_boxing(fi);
    return fi;
}

static void compute_boxing(FuncInfo *fi) {
    for (size_t c = 0; c < fi->child_count; c++) {
        FuncInfo *ch = fi->children[c];
        for (size_t i = 0; i < ch->captures.count; i++) {
            int idx = names_find(&fi->locals, ch->captures.names[i]);
            if (idx >= 0) fi->local_cell[idx] = true;
        }
    }
}

FuncInfo *build_func(Ctx *ctx, const Stmt *node, FuncInfo *parent) {
    FuncInfo *fi = xcalloc(1, sizeof(FuncInfo));
    fi->node = node;
    fi->parent = parent;
    fi->name = node->as.func.name;
    {
        SB sb = {0};
        sb_printf(&sb, "emf_%s_%d", node->as.func.name, ctx->counter++);
        fi->cname = sb.buf;
    }

    if (ctx->all_count == ctx->all_cap) {
        ctx->all_cap = ctx->all_cap ? ctx->all_cap * 2 : 8;
        ctx->all = xrealloc(ctx->all, sizeof(FuncInfo *) * ctx->all_cap);
    }
    ctx->all[ctx->all_count++] = fi;

    /* locals: params, then assigned names + match-pattern bindings, then
     * nested function names */
    for (size_t i = 0; i < node->as.func.param_count; i++)
        add_local(fi, node->as.func.params[i]);
    LocalCtx lc = { ctx->globals, fi };
    ast_collect_assigned(&node->as.func.body, collect_local_cb, &lc);
    Names comp = {0}; collect_comp_vars_block(&node->as.func.body, &comp);
    for (size_t ci = 0; ci < comp.count; ci++) add_local(fi, comp.names[ci]);
    free(comp.names);
    collect_match_pats_block(&node->as.func.body, collect_local_cb, &lc);

    Stmt **defs = NULL;
    size_t ndefs = 0, defcap = 0;
    collect_child_defs(&node->as.func.body, &defs, &ndefs, &defcap);
    for (size_t i = 0; i < ndefs; i++)
        add_local(fi, defs[i]->as.func.name);

    Expr **lams = NULL;
    size_t nlams = 0, lamcap = 0;
    collect_lambdas_block(&node->as.func.body, &lams, &nlams, &lamcap);

    fi->child_cap = (ndefs + nlams) ? (ndefs + nlams) : 1;
    fi->children = xcalloc(fi->child_cap, sizeof(FuncInfo *));
    fi->child_count = 0;
    for (size_t i = 0; i < ndefs; i++)
        fi->children[fi->child_count++] = build_func(ctx, defs[i], fi);
    for (size_t i = 0; i < nlams; i++)
        fi->children[fi->child_count++] = build_lambda(ctx, lams[i], fi);
    free(defs);
    free(lams);

    compute_captures(fi, &node->as.func.body);
    compute_boxing(fi);
    return fi;
}
