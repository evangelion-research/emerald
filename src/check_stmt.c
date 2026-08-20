/* Checker: statements, pattern matching, and `while` termination. */
#include "check_internal.h"

/* record a successful assignment: reads after this see the value's (widened)
 * type when it is more precise than the declared one (assignment narrowing) */
/* a fresh literal captured by a declared type stops being fresh: its exact
 * shape is now owned by the annotation and must not widen later */
static void defresh(Type *t) {
    switch (t->k) {
    case TY_LIT:  t->fresh = false; break;
    case TY_LIST: defresh(t->elem); break;
    case TY_SEQ:  defresh(t->elem); break;
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
     * so later ops must see the static shape. */
    Type *d = ty_resolve(v->decl), *s = ty_resolve(w);
    if (d->k == TY_TENSOR && s->k == TY_TENSOR &&
        !d->tensor.shape->dynamic && s->tensor.shape->dynamic)
        v->type = v->decl;
    else if (assignable(v->decl, w)) v->type = w;
    else if (assignable(v->decl, val)) { defresh(val); v->type = val; }
    else v->type = v->decl;
}

/* Infer an expression whose value is bound to a known type: a generic call
 * (`m: Map[V] = new_map()`) threads the expected type into its type-argument
 * inference, and an empty list literal takes the expected list/seq element
 * type; anything else infers as usual. */
static Type *infer_expected(Ck *ck, const Expr *e, Type *expected) {
    if (!e) return &t_any;
    if (e->kind == E_CALL) return infer_call(ck, e, expected);
    if (e->kind == E_LIST && e->as.list.count == 0 && expected) {
        Type *er = ty_resolve(expected);
        if (er->k == TY_LIST || er->k == TY_SEQ) return expected;
    }
    return infer(ck, e);
}

static void check_assign(Ck *ck, const Stmt *s) {
    Expr *target = s->as.assign.target;
    /* resolve the annotation once, up front, so a generic call's return-type
     * parameters can be bound from it (`m: Map[V] = new_map()`) and the taint
     * check sees the value *after* contextual typing */
    Type *ann0 = NULL;
    if (target->kind == E_NAME && s->as.assign.ann)
        ann0 = resolve_type(ck, s->as.assign.ann, ck->tyenv);
    Type *val = ann0 ? infer_expected(ck, s->as.assign.value, ann0)
                     : infer(ck, s->as.assign.value);
    /* contextual typing for list/seq literals happens *before* the proof checks,
     * so `xs: list[int] = []` is not flagged as a tainted `list[any]` (W2) */
    if (ann0 && s->as.assign.value->kind == E_LIST) {
        Type *ar = ty_resolve(ann0);
        if (ar->k == TY_LIST && s->as.assign.value->as.list.count == 0)
            val = ann0;
        else if (ar->k == TY_SEQ)
            val = s->as.assign.value->as.list.count == 0 ? ann0 : to_seq(val);
    }
    if (ck->proof && val->k == TY_ANY)
        ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                 "value has type 'any', which is banned in proof mode");
    /* field/element assignments contextually type their own empty lists, so a
     * taint check there happens after that; for a plain name it is here */
    if (ck->proof && target->kind == E_NAME)
        ck_proof_taint(ck, val, s->line, s->col, "value");

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
            Type *ann = ann0;
            /* contextual typing for `c: Chan[int] = chan(0)`: the constructor
             * cannot know the element type, so the annotation is what pins it
             * down for both ends of the channel */
            if (ty_resolve(ann)->k == TY_OPAQUE && ty_resolve(val)->k == TY_OPAQUE &&
                ty_resolve(val)->elem->k == TY_ANY)
                val = ann;
            if (ck->proof && ann->k == TY_ANY)
                ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                         "'any' is banned in proof mode: annotate '%s' with a "
                         "concrete type", name);
            note_shape_crossing(ck, ann, val);
            ck_covariance(ck, ann, val, s->line, s->col);
            /* W2: an obligation (`x: never = e`) discharged by a tainted value
             * proves nothing — warn, outside proof mode (where it is an error) */
            if (!ck->proof && ty_resolve(ann)->k == TY_NEVER &&
                val->k != TY_NEVER && type_tainted(val))
                ck_warn(ck, "W_VACUOUS_PROOF", s->line, s->col,
                        "this 'never' obligation is vacuous: the value has "
                        "tainted type %s", type_str(val));
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
            assign_owned(ck, v, name, s->as.assign.value);
            return;
        }
        if (!v) {
            v = env_add(env, name, widen(val), false);
            if (!ck->scope) v->file = s->file;
            v->is_const = s->as.assign.is_const;
            set_flow(v, val);
            assign_owned(ck, v, name, s->as.assign.value);
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
            assign_owned(ck, v, name, s->as.assign.value);
            return;
        }
        note_shape_crossing(ck, v->decl, val);
        if (assignable(v->decl, val)) {
            set_flow(v, val);
            assign_owned(ck, v, name, s->as.assign.value);
            return;
        }
        if (v->annotated) {
            ck_error_t(ck, "E_TYPE_ASSIGN", s->line, s->col, v->decl, val,
                       "cannot assign %s to '%s' declared as %s",
                       type_str(val), name, type_str(v->decl));
            set_flow(v, val);
            assign_owned(ck, v, name, s->as.assign.value);
        } else {
            v->decl = ty_join(v->decl, widen(val)); /* inferred vars widen */
            set_flow(v, val);
            assign_owned(ck, v, name, s->as.assign.value);
        }
        return;
    }

    if (target->kind == E_INDEX) {
        /* the value escapes into the indexed container; if that container is
         * reachable from outside, owned locals in it are no longer owned */
        mark_escaped(ck, s->as.assign.value);
        Type *seq = ty_base(infer(ck, target->as.index.seq));
        Type *idx = infer(ck, target->as.index.idx);
        if (seq->k != TY_ANY && (!assignable(&t_int, idx) || idx->k == TY_FLOAT))
            ck_error(ck, "E_TYPE_INDEX", s->line, s->col,
                     "index must be int, got %s", type_str(idx));
        if (seq->k == TY_STR) {
            ck_error(ck, "E_TYPE_IMMUTABLE", s->line, s->col,
                     "strings are immutable");
            return;
        }
        if (seq->k == TY_SEQ) {
            ck_error(ck, "E_TYPE_IMMUTABLE", s->line, s->col,
                     "a seq is immutable: thaw() it to get a mutable list");
            return;
        }
        /* `xs[i] = []`: the empty list takes the element's type (W2) */
        if (s->as.assign.value->kind == E_LIST &&
            s->as.assign.value->as.list.count == 0 && seq->k == TY_LIST) {
            Type *er = ty_resolve(seq->elem);
            if (er->k == TY_LIST || er->k == TY_SEQ) val = seq->elem;
        }
        ck_proof_taint(ck, val, s->line, s->col, "value");
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
    /* a value stored into a field escapes into the record, which may be a
     * parameter or global — revoke ownership on any owned locals it carries */
    mark_escaped(ck, s->as.assign.value);
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
            /* `b.parts = []`: the empty list takes the field's type (W2) */
            if (s->as.assign.value->kind == E_LIST &&
                s->as.assign.value->as.list.count == 0) {
                Type *fr = ty_resolve(obj->rec.types[i]);
                if (fr->k == TY_LIST || fr->k == TY_SEQ)
                    val = obj->rec.types[i];
            }
            ck_proof_taint(ck, val, s->line, s->col, "value");
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

/* --- W4: `while` termination -------------------------------------------- */
/* In proof mode a `while` loop is only accepted when its termination is
 * statically evident: a single integer counter that moves monotonically
 * toward a bound each iteration. This is the for-loop-as-while pattern
 * (`while i < n { ...; i = i + 1 }`, `while n > 0 { ...; n = floor_div(n,2) }`),
 * not a general ranking-function synthesis — anything else must use
 * `for i in range(n)` or `partial`. */
static bool int_lit_gt(const Expr *e, int64_t min) {
    return e->kind == E_INT && e->as.ival > min;
}

/* names may be module-mangled (`math__floor_div`): match the bare suffix */
static bool name_is(const Expr *e, const char *suffix) {
    if (e->kind != E_NAME) return false;
    size_t n = strlen(e->as.sval), m = strlen(suffix);
    return n >= m && strcmp(e->as.sval + n - m, suffix) == 0;
}

/* is `e` a progress step on counter `var`? `*up` reports the direction. */
static bool progress_step(const Expr *e, const char *var, bool *up) {
    if (e->kind == E_BINOP) {
        const Expr *l = e->as.bin.lhs, *r = e->as.bin.rhs;
        switch (e->as.bin.op) {
        case B_ADD:
            if (l->kind == E_NAME && strcmp(l->as.sval, var) == 0 &&
                int_lit_gt(r, 0)) { *up = true; return true; }
            if (r->kind == E_NAME && strcmp(r->as.sval, var) == 0 &&
                int_lit_gt(l, 0)) { *up = true; return true; }
            return false;
        case B_SUB:
            if (l->kind == E_NAME && strcmp(l->as.sval, var) == 0 &&
                int_lit_gt(r, 0)) { *up = false; return true; }
            return false;
        case B_DIV:
            if (l->kind == E_NAME && strcmp(l->as.sval, var) == 0 &&
                int_lit_gt(r, 1)) { *up = false; return true; }
            return false;
        default:
            return false;
        }
    }
    if (e->kind == E_CALL) {
        const Expr *fn = e->as.call.fn;
        if (name_is(fn, "floor_div") &&
            e->as.call.count >= 2 &&
            e->as.call.args[0]->kind == E_NAME &&
            strcmp(e->as.call.args[0]->as.sval, var) == 0 &&
            int_lit_gt(e->as.call.args[1], 1)) {
            *up = false;
            return true;
        }
    }
    return false;
}

static bool block_progresses(const Block *b, const char *var, bool want_up);

static bool stmt_progresses(const Stmt *s, const char *var, bool want_up) {
    switch (s->kind) {
    case S_ASSIGN: {
        const Expr *t = s->as.assign.target;
        bool up;
        if (t->kind == E_NAME && strcmp(t->as.sval, var) == 0 &&
            progress_step(s->as.assign.value, var, &up))
            return up == want_up;
        return false;
    }
    case S_BLOCK:
        return block_progresses(&s->as.block, var, want_up);
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++)
            if (block_progresses(&s->as.ifs.blocks[i], var, want_up))
                return true;
        return s->as.ifs.has_else &&
               block_progresses(&s->as.ifs.else_block, var, want_up);
    case S_MATCH:
        for (size_t i = 0; i < s->as.mtch.count; i++)
            if (block_progresses(&s->as.mtch.blocks[i], var, want_up))
                return true;
        return false;
    default: /* nested loops handle their own termination */
        return false;
    }
}

static bool block_progresses(const Block *b, const char *var, bool want_up) {
    for (size_t i = 0; i < b->count; i++)
        if (stmt_progresses(b->items[i], var, want_up)) return true;
    return false;
}

/* does this `while` loop have an evident monotone integer counter? */
static bool while_terminates(const Stmt *s) {
    const Expr *c = s->as.wh.cond;
    if (c->kind != E_BINOP) return false;
    bool want_up;
    switch (c->as.bin.op) {
    case B_LT: case B_LE: want_up = true; break;
    case B_GT: case B_GE: want_up = false; break;
    default: return false;
    }
    const Expr *v = c->as.bin.lhs;
    if (v->kind != E_NAME) return false;
    return block_progresses(&s->as.wh.body, v->as.sval, want_up);
}

static void check_stmt(Ck *ck, const Stmt *s) {
    switch (s->kind) {
    case S_EXPR: {
        Type *t = infer(ck, s->as.expr);
        if (ck->proof && t->k == TY_ANY)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "expression has type 'any', which is banned in proof mode");
        ck_proof_taint(ck, t, s->line, s->col, "expression");
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
            ck_proof_taint(ck, ct, s->line, s->col, "condition");
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
        /* W4: in proof mode a `while` loop must have an evident monotone
         * integer counter (the for-loop-as-while pattern); anything else
         * cannot be shown to terminate. */
        if (ck->proof && !while_terminates(s))
            ck_error(ck, "E_TYPE_TERMINATION", s->line, s->col,
                     "'while' cannot be shown to terminate in proof mode: use "
                     "a monotone counter or `for i in range(n)` instead");
        Type *ct = infer(ck, s->as.wh.cond);
        if (ck->proof && ct->k == TY_ANY)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "condition has type 'any', which is banned in proof mode");
        ck_proof_taint(ck, ct, s->line, s->col, "condition");
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
        ck_proof_taint(ck, seq, s->line, s->col, "iterated value");
        Type *elem = &t_any;
        if (seq->k == TY_LIST) elem = widen(seq->elem);
        else if (seq->k == TY_SEQ) elem = widen(seq->elem);
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
        v->owned = false; /* a loop variable holds elements, never a fresh list */
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
        Type *t = s->as.ret ? infer_expected(ck, s->as.ret, ck->cur_ret)
                            : &t_none;
        if (ck->proof && t->k == TY_ANY)
            ck_error(ck, "E_PROOF_ANY", s->line, s->col,
                     "returning a value of type 'any', which is banned in "
                     "proof mode");
        ck_proof_taint(ck, t, s->line, s->col, "returned value");
        note_shape_crossing(ck, ck->cur_ret, t);
        ck_covariance(ck, ck->cur_ret, t, s->line, s->col);
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
        else if (!ck->proof && type_tainted(st))
            ck_warn(ck, "W_VACUOUS_PROOF", s->line, s->col,
                    "match exhaustiveness over %s is vacuous: the subject is "
                    "tainted by 'any'", type_str(st));
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

void check_block(Ck *ck, const Block *b) {
    for (size_t i = 0; i < b->count; i++) {
        /* a linked program spans several files; follow the statement's own */
        const char *saved = ck->filename;
        if (b->items[i]->file) ck->filename = b->items[i]->file;
        check_stmt(ck, b->items[i]);
        ck->filename = saved;
    }
}
