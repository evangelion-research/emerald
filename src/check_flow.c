/* Checker: flow narrowing, termination checking, and mutual-recursion (W4)
 * analysis. */
#include "check_internal.h"

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

void nset_restore_from(NSet *ns, size_t mark) {
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
void narrow_cond(Ck *ck, const Expr *e, bool sense, NSet *ns) {
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

bool block_terminates(const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        if (stmt_terminates(b->items[i])) return true;
    return false;
}

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

bool block_returns(const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        if (stmt_returns(b->items[i])) return true;
    return false;
}

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
    case TY_SEQ:
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
const Type *field_type(const Type *t, const char *f) {
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
 * type resolves to `root` (a recursive alias)? Two descent steps are
 * recognized, both sound:
 *
 *   - a `.field` projection whose field lands back on `root` (pointer
 *     equality): `n.succ`, `n.succ.succ`;
 *   - an element access `t.kids[i]` where `kids: seq[root]` — element-of is
 *     strictly smaller. (Only `seq[T]` is accepted here, not `list[T]`, which
 *     could be mutated between the call and the recursion.) */
static bool arg_descends(const Type *root, const char *pname, const Expr *arg);

static bool arg_descends_rec(const Type *r, const char *pname, const Expr *e);

/* `e` denotes the parameter itself (type `r`) or a strict subterm of it. */
static bool is_param_or_subterm(const Type *r, const char *pname,
                                const Expr *e) {
    if (e->kind == E_NAME)
        return strcmp(e->as.sval, pname) == 0;
    return arg_descends_rec(r, pname, e);
}

/* `e` denotes a `seq[r]` value reachable from the parameter: either a field
 * `p.kids` of type `seq[r]`, or a seq subterm reached by deeper descent. */
static bool is_seq_subterm(const Type *r, const char *pname, const Expr *e) {
    if (e->kind == E_ATTR) {
        const Type *ft = field_type(r, e->as.attr.name);
        if (!ft) return false;
        const Type *rft = ty_resolve(ft);
        if (rft->k != TY_SEQ || ty_resolve(rft->elem) != r) return false;
        return is_param_or_subterm(r, pname, e->as.attr.obj);
    }
    return false;
}

static bool arg_descends_rec(const Type *r, const char *pname, const Expr *e) {
    switch (e->kind) {
    case E_NAME:
        return false; /* the parameter itself is not a strict subterm */
    case E_ATTR: {
        /* e = obj.field: one constructor step down iff the field lands on r */
        const Type *ft = field_type(r, e->as.attr.name);
        if (!ft || ty_resolve(ft) != r) return false;
        return is_param_or_subterm(r, pname, e->as.attr.obj);
    }
    case E_INDEX:
        /* e = seq[i]: a descent iff seq is a seq[r] reachable from the param */
        return is_seq_subterm(r, pname, e->as.index.seq);
    default:
        return false;
    }
}

static bool arg_descends(const Type *root, const char *pname, const Expr *arg) {
    Vis vis = {0};
    if (!type_is_recursive(root, &vis)) return false;
    return arg_descends_rec(ty_resolve(root), pname, arg);
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
void check_termination(Ck *ck, const Stmt *s, Type **ptypes) {
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

static bool callees_has(const CalleeSet *s, const char *n) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i], n) == 0) return true;
    return false;
}

static void callees_add(CalleeSet *s, const char *n) {
    if (n && !callees_has(s, n) && s->count < 256)
        s->items[s->count++] = n;
}

static void callees_expr(const Expr *e, CalleeSet *out) {
    if (!e) return;
    switch (e->kind) {
    case E_CALL:
        if (e->as.call.fn->kind == E_NAME)
            callees_add(out, e->as.call.fn->as.sval);
        for (size_t i = 0; i < e->as.call.count; i++)
            callees_expr(e->as.call.args[i], out);
        break;
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            callees_expr(e->as.list.items[i], out);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            callees_expr(e->as.rec.values[i], out);
        break;
    case E_BINOP:
        callees_expr(e->as.bin.lhs, out);
        callees_expr(e->as.bin.rhs, out);
        break;
    case E_UNOP:
        callees_expr(e->as.un.operand, out);
        break;
    case E_INDEX:
        callees_expr(e->as.index.seq, out);
        callees_expr(e->as.index.idx, out);
        break;
    case E_ATTR:
        callees_expr(e->as.attr.obj, out);
        break;
    case E_LAMBDA:
        callees_expr(e->as.lam.body, out);
        break;
    case E_TRY:
        callees_expr(e->as.try_expr, out);
        break;
    case E_CATCH:
        callees_expr(e->as.ctch.subject, out);
        for (size_t i = 0; i < e->as.ctch.count; i++)
            callees_expr(e->as.ctch.arms[i].body, out);
        break;
    default:
        break;
    }
}

static void callees_stmt(const Stmt *s, CalleeSet *out) {
    switch (s->kind) {
    case S_EXPR:
        callees_expr(s->as.expr, out);
        break;
    case S_ASSIGN:
        callees_expr(s->as.assign.value, out);
        break;
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++) {
            callees_expr(s->as.ifs.conds[i], out);
            for (size_t j = 0; j < s->as.ifs.blocks[i].count; j++)
                callees_stmt(s->as.ifs.blocks[i].items[j], out);
        }
        if (s->as.ifs.has_else)
            for (size_t j = 0; j < s->as.ifs.else_block.count; j++)
                callees_stmt(s->as.ifs.else_block.items[j], out);
        break;
    case S_WHILE:
        callees_expr(s->as.wh.cond, out);
        for (size_t j = 0; j < s->as.wh.body.count; j++)
            callees_stmt(s->as.wh.body.items[j], out);
        break;
    case S_FOR:
        callees_expr(s->as.fr.seq, out);
        for (size_t j = 0; j < s->as.fr.body.count; j++)
            callees_stmt(s->as.fr.body.items[j], out);
        break;
    case S_RETURN:
        callees_expr(s->as.ret, out);
        break;
    case S_BLOCK:
        for (size_t j = 0; j < s->as.block.count; j++)
            callees_stmt(s->as.block.items[j], out);
        break;
    case S_FUNC:
        /* a nested def's calls count against the enclosing function */
        for (size_t j = 0; j < s->as.func.body.count; j++)
            callees_stmt(s->as.func.body.items[j], out);
        break;
    case S_MATCH:
        callees_expr(s->as.mtch.subject, out);
        for (size_t j = 0; j < s->as.mtch.count; j++)
            for (size_t k = 0; k < s->as.mtch.blocks[j].count; k++)
                callees_stmt(s->as.mtch.blocks[j].items[k], out);
        break;
    default:
        break;
    }
}

/* Tarjan's SCC over the top-level call graph. A strongly-connected component
 * of more than one function is mutual recursion: with no single structural
 * parameter to descend on, its termination is not the kind the checker
 * proves. `partial` (the ordinary opt-out) is the escape in normal mode; in
 * proof mode `partial` is already banned, so the cycle is simply rejected. */
static void scc_visit(Ck *ck, size_t v, const bool *adj, size_t n,
                      int *index, int *low, bool *onstack, int *stack,
                      size_t *sp, int *idx) {
    index[v] = low[v] = (*idx)++;
    stack[(*sp)++] = (int)v;
    onstack[v] = true;
    for (size_t w = 0; w < n; w++) {
        if (!adj[v * n + w]) continue;
        if (index[w] < 0) {
            scc_visit(ck, w, adj, n, index, low, onstack, stack, sp, idx);
            if (low[w] < low[v]) low[v] = low[w];
        } else if (onstack[w] && index[w] < low[v]) {
            low[v] = index[w];
        }
    }
    if (low[v] != index[v]) return;
    size_t members[256];
    size_t nm = 0;
    for (;;) {
        int w = stack[--(*sp)];
        onstack[w] = false;
        members[nm++] = (size_t)w;
        if ((size_t)w == v) break;
    }
    if (nm <= 1) return;
    bool any_total = false;
    for (size_t k = 0; k < nm; k++)
        if (!ck->funcs[members[k]].partial) any_total = true;
    if (!any_total) return;
    char buf[512];
    size_t off = 0;
    for (size_t k = 0; k < nm && off < sizeof(buf); k++) {
        const char *d = ck->funcs[members[k]].disp;
        int w = snprintf(buf + off, sizeof(buf) - off, "%s'%s'",
                         k ? ", " : "", d);
        if (w < 0) break;
        off += (size_t)w;
    }
    const Stmt *s = ck->funcs[members[0]].node;
    ck_error(ck, "E_TYPE_TERMINATION", s->line, s->col,
             "mutually recursive functions form a cycle without structural "
             "descent: %s; declare them 'partial' to opt out of termination "
             "checking", buf);
}

void check_mutual_recursion(Ck *ck) {
    size_t n = ck->func_count;
    if (n < 2) return;
    bool *adj = xcalloc(n * n, sizeof(bool));
    for (size_t i = 0; i < n; i++) {
        CalleeSet cs = {0};
        const Stmt *s = ck->funcs[i].node;
        for (size_t k = 0; k < s->as.func.body.count; k++)
            callees_stmt(s->as.func.body.items[k], &cs);
        for (size_t j = 0; j < n; j++)
            if (callees_has(&cs, ck->funcs[j].name)) adj[i * n + j] = true;
    }
    int *index = xmalloc(sizeof(int) * n);
    int *low = xmalloc(sizeof(int) * n);
    bool *onstack = xcalloc(n, sizeof(bool));
    int *stack = xmalloc(sizeof(int) * n);
    for (size_t i = 0; i < n; i++) index[i] = -1;
    size_t sp = 0;
    int idx = 0;
    for (size_t v = 0; v < n; v++)
        if (index[v] < 0)
            scc_visit(ck, v, adj, n, index, low, onstack, stack, &sp, &idx);
    free(adj);
    free(index);
    free(low);
    free(onstack);
    free(stack);
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
Type *check_pattern(Ck *ck, const Pat *p, const Type *st, VarEnv *env) {
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
