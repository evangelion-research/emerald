/* Codegen: expressions -- literals, collections, calls, operators, slices,
 * comprehensions, f-strings, lambdas, and try/catch lowering. */
#include "codegen_internal.h"

static const char *binop_fn(BinOp op) {
    switch (op) {
    case B_ADD: return "em_add"; case B_SUB: return "em_sub";
    case B_MUL: return "em_mul"; case B_DIV: return "em_div";
    case B_MOD: return "em_mod";
    case B_FLOORDIV: return "em_floordiv"; case B_POW: return "em_pow";
    case B_EQ: return "em_eq";   case B_NE: return "em_ne";
    case B_IN: return "em_contains";
    case B_LT: return "em_lt";   case B_LE: return "em_le";
    case B_GT: return "em_gt";   case B_GE: return "em_ge";
    case B_BITOR: return "em_bitor"; case B_BITXOR: return "em_bitxor";
    case B_BITAND: return "em_bitand"; case B_LSHIFT: return "em_lshift";
    case B_RSHIFT: return "em_rshift";
    default: return "?";
    }
}

static int gen_call(Cg *cg, const Expr *e) {
    size_t argc = e->as.call.count;
    int *args = xmalloc(sizeof(int) * argc);
    for (size_t i = 0; i < argc; i++)
        args[i] = gen_expr(cg, e->as.call.args[i]);

    int t = new_temp(cg);
    char tb[32], ab[32], bb[32];
    SB call = {0};

    const Expr *fn = e->as.call.fn;
    if (fn->kind == E_NAME) {
        const char *name = fn->as.sval;
        const Builtin *b = cg_builtin_find(name);
        if (b && b->cfn) {
            /* fixed-arity cg_builtins all lower the same way: call the runtime
             * entry point with the argument slots. The checker has already
             * verified the arity, so the table's count is what we emit. */
            SB c = {0};
            sb_printf(&c, "%s(", b->cfn);
            for (size_t i = 0; i < argc; i++)
                sb_printf(&c, "%s%s", i ? ", " : "", slotref(args[i], ab));
            sb_printf(&c, ")");
            if (b->rets) {
                emit(cg, "%s = %s;", slotref(t, tb), c.buf);
            } else {
                emit(cg, "%s;", c.buf);
                emit(cg, "%s = em_none();", slotref(t, tb));
            }
            free(c.buf);
        } else if (strcmp(name, "print") == 0 || strcmp(name, "eprint") == 0) {
            /* variadic: the runtime takes the count, then the values */
            sb_printf(&call, "%s(%zu",
                      name[0] == 'e' ? "em_eprint" : "em_print", argc);
            for (size_t i = 0; i < argc; i++)
                sb_printf(&call, ", %s", slotref(args[i], ab));
            sb_printf(&call, ");");
            emit(cg, "%s", call.buf);
            emit(cg, "%s = em_none();", slotref(t, tb));
        } else if (strcmp(name, "range") == 0) {
            if (argc == 1)  /* range(n) starts at 0 */
                emit(cg, "%s = em_range(em_int(0), %s);", slotref(t, tb),
                     slotref(args[0], ab));
            else
                emit(cg, "%s = em_range(%s, %s);", slotref(t, tb),
                     slotref(args[0], ab), slotref(args[1], bb));
        } else if (strcmp(name, "dict") == 0 || strcmp(name, "set") == 0) {
            if (argc == 0)
                emit(cg, "%s = %s(0);", slotref(t, tb),
                     strcmp(name, "dict") == 0 ? "em_dict_litn" : "em_set_litn");
            else if (argc == 1)
                emit(cg, "%s = %s(%s);", slotref(t, tb),
                     strcmp(name, "dict") == 0 ? "em_dict_from" : "em_set_from",
                     slotref(args[0], ab));
            else
                emit(cg, "%s = em_none();", slotref(t, tb));
        } else {
            Access kind;
            int slot;
            if (var_slot(cg, name, &kind, &slot)) {
                /* a closure value held in a variable: indirect call */
                int fslot = gen_var_read(cg, name);
                sb_printf(&call, "%s = em_call(%s, %zu", slotref(t, tb),
                          slotref(fslot, ab), argc);
                for (size_t i = 0; i < argc; i++)
                    sb_printf(&call, ", %s", slotref(args[i], bb));
                sb_printf(&call, ");");
                emit(cg, "%s", call.buf);
            } else {
                FuncInfo *tf = find_top_func(cg, name);
                if (tf) {
                    /* direct calls can fill omitted defaults and reorder
                     * keyword arguments before entering the fixed C ABI. */
                    size_t np = tf->node->as.func.param_count;
                    int *mapped = xmalloc(sizeof(int) * np);
                    for (size_t j = 0; j < np; j++) mapped[j] = -1000000000;
                    size_t pos = 0;
                    for (size_t i = 0; i < argc; i++) {
                        const char *an = e->as.call.arg_names ? e->as.call.arg_names[i] : NULL;
                        size_t j = pos;
                        if (an) {
                            for (j = 0; j < np; j++)
                                if (strcmp(tf->node->as.func.params[j], an) == 0) break;
                        } else { while (j < np && mapped[j] != -1000000000) j++; pos = j + 1; }
                        if (j < np) mapped[j] = args[i];
                    }
                    sb_printf(&call, "%s = %s(NULL", slotref(t, tb), tf->cname);
                    for (size_t j = 0; j < np; j++) {
                        if (mapped[j] != -1000000000) sb_printf(&call, ", %s", slotref(mapped[j], ab));
                        else {
                            int dv = gen_expr(cg, tf->node->as.func.defaults[j]);
                            sb_printf(&call, ", %s", slotref(dv, ab));
                        }
                    }
                    sb_printf(&call, ");"); emit(cg, "%s", call.buf); free(mapped);
                } else {
                    fprintf(stderr,
                            "emeraldc: internal error: unresolved call '%s'\n",
                            name);
                    exit(1);
                }
            }
        }
    } else {
        /* general callee expression: evaluate, then call through em_call */
        int fslot = gen_expr(cg, fn);
        sb_printf(&call, "%s = em_call(%s, %zu", slotref(t, tb),
                  slotref(fslot, ab), argc);
        for (size_t i = 0; i < argc; i++)
            sb_printf(&call, ", %s", slotref(args[i], bb));
        sb_printf(&call, ");");
        emit(cg, "%s", call.buf);
    }
    free(call.buf);
    free(args);
    return t;
}

/* The arms of a `catch`, as an if/else chain over the error's `_tag`. The
 * checker has already proved the chain is exhaustive, so the final `else`
 * only exists to keep the generated C total. */
static void gen_catch_arms(Cg *cg, const Expr *e, int err, int dst,
                           size_t arm) {
    char eb[32], db[32], vb[32];
    const CatchArm *a = &e->as.ctch.arms[arm];
    bool last = arm + 1 >= e->as.ctch.count;
    if (a->tag) {
        SB tag = {0};
        sb_c_string(&tag, a->tag);
        emit(cg, "if (em_truthy(em_eq(em_getattr(%s, \"_tag\"), "
                 "em_str_new(%s)))) {", slotref(err, eb), tag.buf);
        free(tag.buf);
        cg->indent++;
    }
    if (a->bind) gen_var_write(cg, a->bind, slotref(err, eb));
    int v = gen_expr(cg, a->body);
    emit(cg, "%s = %s;", slotref(dst, db), slotref(v, vb));
    if (a->tag) {
        cg->indent--;
        emit(cg, "} else {");
        cg->indent++;
        if (last)
            emit(cg, "rt_fatal(\"catch: unhandled error (exhaustiveness "
                     "check failed)\");");
        else
            gen_catch_arms(cg, e, err, dst, arm + 1);
        cg->indent--;
        emit(cg, "}");
    }
}

int gen_expr(Cg *cg, const Expr *e) {
    char tb[32], ab[32], bb[32], cb[32];
    switch (e->kind) {
    case E_INT: {
        int t = new_temp(cg);
        emit(cg, "%s = em_int(%lldLL);", slotref(t, tb), (long long)e->as.ival);
        return t;
    }
    case E_FLOAT: {
        int t = new_temp(cg);
        emit(cg, "%s = em_float(%.17g);", slotref(t, tb), e->as.fval);
        return t;
    }
    case E_STR: {
        int t = new_temp(cg);
        SB lit = {0};
        sb_c_string(&lit, e->as.sval);
        emit(cg, "%s = em_str_new(%s);", slotref(t, tb), lit.buf);
        free(lit.buf);
        return t;
    }
    case E_TRUE: {
        int t = new_temp(cg);
        emit(cg, "%s = em_bool(true);", slotref(t, tb));
        return t;
    }
    case E_FALSE: {
        int t = new_temp(cg);
        emit(cg, "%s = em_bool(false);", slotref(t, tb));
        return t;
    }
    case E_NONE: {
        int t = new_temp(cg);
        emit(cg, "%s = em_none();", slotref(t, tb));
        return t;
    }
    case E_NAME: {
        /* refl is proof evidence (Eq[a, a]): it is erased at runtime */
        if (strcmp(e->as.sval, "refl") == 0) {
            int t = new_temp(cg);
            emit(cg, "%s = em_none();", slotref(t, tb));
            return t;
        }
        Access kind;
        int slot;
        if (var_slot(cg, e->as.sval, &kind, &slot))
            return gen_var_read(cg, e->as.sval);
        FuncInfo *tf = find_top_func(cg, e->as.sval);
        if (tf) {
            int t = new_temp(cg); size_t np=tf->node->as.func.param_count, min=np;
            while(min && tf->node->as.func.defaults && tf->node->as.func.defaults[min-1]) min--;
            size_t dc=(tf->node->as.func.defaults) ? np-min : 0;
            if(!dc) emit(cg, "%s = em_mkclosure(%s_tramp, %zu, NULL, 0, NULL, 0);", slotref(t,tb),tf->cname,np);
            else {
                emit(cg, "{ Value __defs[%zu];", dc);
                for(size_t j=min;j<np;j++){ int dv=gen_expr(cg,tf->node->as.func.defaults[j]); emit(cg,"  __defs[%zu] = %s;",j-min,slotref(dv,ab)); }
                emit(cg, "  %s = em_mkclosure(%s_tramp, %zu, NULL, 0, __defs, %zu);",slotref(t,tb),tf->cname,np,dc);
                emit(cg, "}");
            }
            return t;
        }
        fprintf(stderr, "emeraldc: internal error: unresolved name '%s'\n",
                e->as.sval);
        exit(1);
    }
    case E_LIST:
    case E_TUPLE: {
        size_t n = e->as.list.count;
        int *items = xmalloc(sizeof(int) * n);
        for (size_t i = 0; i < n; i++)
            items[i] = gen_expr(cg, e->as.list.items[i]);
        int t = new_temp(cg);
        SB call = {0};
        sb_printf(&call, "%s = %s(%zu", slotref(t, tb),
                  e->kind == E_TUPLE ? "em_tuple_litn" : "em_list_litn", n);
        for (size_t i = 0; i < n; i++)
            sb_printf(&call, ", %s", slotref(items[i], ab));
        sb_printf(&call, ");");
        emit(cg, "%s", call.buf);
        free(call.buf);
        free(items);
        return t;
    }
    case E_REC: {
        size_t n = e->as.rec.count;
        int *vals = xmalloc(sizeof(int) * n);
        for (size_t i = 0; i < n; i++)
            vals[i] = gen_expr(cg, e->as.rec.values[i]);
        int t = new_temp(cg);
        SB call = {0};
        sb_printf(&call, "%s = em_rec_litn(%zu", slotref(t, tb), n);
        for (size_t i = 0; i < n; i++) {
            sb_printf(&call, ", ");
            sb_c_string(&call, e->as.rec.names[i]);
            sb_printf(&call, ", %s", slotref(vals[i], ab));
        }
        sb_printf(&call, ");");
        emit(cg, "%s", call.buf);
        free(call.buf);
        free(vals);
        return t;
    }
    case E_DICT: {
        size_t n = e->as.dict.count; int *keys = xmalloc(sizeof(int) * n);
        int *vals = xmalloc(sizeof(int) * n);
        for (size_t i = 0; i < n; i++) { keys[i] = gen_expr(cg, e->as.dict.keys[i]); vals[i] = gen_expr(cg, e->as.dict.values[i]); }
        int t = new_temp(cg); SB call = {0};
        sb_printf(&call, "%s = em_dict_litn(%zu", slotref(t, tb), n);
        for (size_t i = 0; i < n; i++) sb_printf(&call, ", %s, %s", slotref(keys[i], ab), slotref(vals[i], bb));
        sb_printf(&call, ");"); emit(cg, "%s", call.buf); free(call.buf); free(keys); free(vals); return t;
    }
    case E_SET: {
        size_t n = e->as.set.count; int *items = xmalloc(sizeof(int) * n);
        for (size_t i = 0; i < n; i++) items[i] = gen_expr(cg, e->as.set.items[i]);
        int t = new_temp(cg); SB call = {0}; sb_printf(&call, "%s = em_set_litn(%zu", slotref(t, tb), n);
        for (size_t i = 0; i < n; i++) sb_printf(&call, ", %s", slotref(items[i], ab));
        sb_printf(&call, ");"); emit(cg, "%s", call.buf); free(call.buf); free(items); return t;
    }
    case E_SLICE: {
        int seq = gen_expr(cg, e->as.slice.seq);
        int lo = e->as.slice.start ? gen_expr(cg, e->as.slice.start) : -1;
        int hi = e->as.slice.stop ? gen_expr(cg, e->as.slice.stop) : -1;
        int st = e->as.slice.step ? gen_expr(cg, e->as.slice.step) : -1;
        int t = new_temp(cg);
        char db[32];
        emit(cg, "%s = em_slice_ex(%s, %s, %s, %s);", slotref(t, tb), slotref(seq, ab),
             lo < 0 ? "em_none()" : slotref(lo, bb), hi < 0 ? "em_none()" : slotref(hi, cb),
             st < 0 ? "em_none()" : slotref(st, db));
        return t;
    }
    case E_FSTR: {
        int t = new_temp(cg); SB lit = {0}; sb_c_string(&lit, e->as.fstr.texts[0]);
        emit(cg, "%s = em_str_new(%s);", slotref(t, tb), lit.buf); free(lit.buf);
        for (size_t i = 0; i < e->as.fstr.count; i++) {
            int v = gen_expr(cg, e->as.fstr.exprs[i]); int sv = new_temp(cg);
            emit(cg, "%s = em_str(%s);", slotref(sv, cb), slotref(v, ab));
            emit(cg, "%s = em_add(%s, %s);", slotref(t, tb), slotref(t, bb), slotref(sv, cb));
            SB tail = {0}; sb_c_string(&tail, e->as.fstr.texts[i + 1]);
            emit(cg, "%s = em_add(%s, em_str_new(%s));", slotref(t, tb), slotref(t, bb), tail.buf); free(tail.buf);
        }
        return t;
    }
    case E_COMP: {
        int seq = gen_expr(cg, e->as.comp.seq); int out = new_temp(cg);
        const char *ctor = e->as.comp.kind == COMP_LIST ? "em_list_litn(0)" :
                           e->as.comp.kind == COMP_SET ? "em_set_litn(0)" : "em_dict_litn(0)";
        emit(cg, "%s = %s;", slotref(out, tb), ctor);
        int idx = new_temp(cg); emit(cg, "%s = em_int(0);", slotref(idx, ab));
        emit(cg, "for (;;) {"); cg->indent++; emit(cg, "Value __it;");
        emit(cg, "if (!rt_iter_get(%s, %s.as.i, &__it)) break;", slotref(seq, ab), slotref(idx, bb));
        emit(cg, "%s.as.i += 1;", slotref(idx, ab)); gen_var_write(cg, e->as.comp.var, "__it");
        if (e->as.comp.cond) { int c = gen_expr(cg, e->as.comp.cond); emit(cg, "if (!em_truthy(%s)) continue;", slotref(c, cb)); }
        if (e->as.comp.kind == COMP_DICT) { int k = gen_expr(cg, e->as.comp.key); int v = gen_expr(cg, e->as.comp.elt); emit(cg, "em_dict_set(%s, %s, %s);", slotref(out, ab), slotref(k, bb), slotref(v, cb)); }
        else { int v = gen_expr(cg, e->as.comp.elt); if (e->as.comp.kind == COMP_SET) emit(cg, "em_set_add(%s, %s);", slotref(out, ab), slotref(v, bb)); else emit(cg, "em_append(%s, %s);", slotref(out, ab), slotref(v, bb)); }
        cg->indent--; emit(cg, "}"); return out;
    }
    case E_BINOP: {
        BinOp op = e->as.bin.op;
        if (op == B_PIPE) { /* x |> f == f(x) */
            int l = gen_expr(cg, e->as.bin.lhs);
            int f = gen_expr(cg, e->as.bin.rhs);
            int t = new_temp(cg);
            emit(cg, "%s = em_call(%s, 1, %s);", slotref(t, tb),
                 slotref(f, ab), slotref(l, bb));
            return t;
        }
        if (op == B_COMPOSE) { /* f >> g == x -> g(f(x)) */
            int f = gen_expr(cg, e->as.bin.lhs);
            int g = gen_expr(cg, e->as.bin.rhs);
            int t = new_temp(cg);
            emit(cg, "%s = em_compose_or_rshift(%s, %s);", slotref(t, tb),
                 slotref(f, ab), slotref(g, bb));
            return t;
        }
        if (op == B_AND || op == B_OR) {
            /* short-circuit; Python semantics: the result is one operand */
            int lhs = gen_expr(cg, e->as.bin.lhs);
            int t = new_temp(cg);
            emit(cg, "%s = %s;", slotref(t, tb), slotref(lhs, ab));
            emit(cg, "if (%sem_truthy(%s)) {", op == B_AND ? "" : "!",
                 slotref(t, tb));
            cg->indent++;
            int rhs = gen_expr(cg, e->as.bin.rhs);
            emit(cg, "%s = %s;", slotref(t, tb), slotref(rhs, ab));
            cg->indent--;
            emit(cg, "}");
            return t;
        }
        int lhs = gen_expr(cg, e->as.bin.lhs);
        int rhs = gen_expr(cg, e->as.bin.rhs);
        int t = new_temp(cg);
        emit(cg, "%s = %s(%s, %s);", slotref(t, tb), binop_fn(op),
             slotref(lhs, ab), slotref(rhs, bb));
        return t;
    }
    case E_UNOP: {
        int s = gen_expr(cg, e->as.un.operand);
        int t = new_temp(cg);
        if (e->as.un.op == U_NEG)
            emit(cg, "%s = em_neg(%s);", slotref(t, tb), slotref(s, ab));
        else
            emit(cg, "%s = em_bool(!em_truthy(%s));", slotref(t, tb),
                 slotref(s, ab));
        return t;
    }
    case E_TRY: {
        /* `try e`: hand the whole failed result straight back to the caller —
         * it is already the value this function must return, so no error is
         * rebuilt and nothing is copied. */
        int r = gen_expr(cg, e->as.try_expr);
        emit(cg, "if (!em_truthy(em_getattr(%s, \"ok\"))) "
                 "{ Value __r = %s; rt_pop_frame(); return __r; }",
             slotref(r, ab), slotref(r, ab));
        int t = new_temp(cg);
        emit(cg, "%s = em_getattr(%s, \"val\");", slotref(t, tb),
             slotref(r, ab));
        return t;
    }
    case E_CATCH: {
        int r = gen_expr(cg, e->as.ctch.subject);
        int t = new_temp(cg);
        emit(cg, "if (em_truthy(em_getattr(%s, \"ok\"))) {", slotref(r, ab));
        cg->indent++;
        emit(cg, "%s = em_getattr(%s, \"val\");", slotref(t, tb),
             slotref(r, ab));
        cg->indent--;
        emit(cg, "} else {");
        cg->indent++;
        int err = new_temp(cg);
        emit(cg, "%s = em_getattr(%s, \"err\");", slotref(err, bb),
             slotref(r, ab));
        gen_catch_arms(cg, e, err, t, 0);
        cg->indent--;
        emit(cg, "}");
        return t;
    }
    case E_CALL:
        return gen_call(cg, e);
    case E_LAMBDA: {
        /* find the FuncInfo built for this lambda and emit its closure */
        FuncInfo *child = NULL;
        if (cg->fi) {
            for (size_t i = 0; i < cg->fi->child_count; i++)
                if (cg->fi->children[i]->lamb == e) {
                    child = cg->fi->children[i];
                    break;
                }
        }
        if (!child) {
            fprintf(stderr,
                    "emeraldc: internal error: missing lambda function\n");
            exit(1);
        }
        size_t n = child->captures.count;
        size_t arity = e->as.lam.param_count;
        int t = new_temp(cg);
        if (n == 0) {
            emit(cg, "%s = em_mkclosure(%s_tramp, %zu, NULL, 0, NULL, 0);",
                 slotref(t, tb), child->cname, arity);
        } else {
            emit(cg, "{ Value __cap[%zu];", n);
            for (size_t k = 0; k < n; k++) {
                char cb[32];
                emit(cg, "  __cap[%zu] = %s;", k,
                     cellref(cg, child->captures.names[k], cb));
            }
            emit(cg, "  %s = em_mkclosure(%s_tramp, %zu, __cap, %zu, NULL, 0);",
                 slotref(t, tb), child->cname, arity, n);
            emit(cg, "}");
        }
        return t;
    }
    case E_INDEX: {
        int seq = gen_expr(cg, e->as.index.seq);
        int idx = gen_expr(cg, e->as.index.idx);
        int t = new_temp(cg);
        emit(cg, "%s = em_index(%s, %s);", slotref(t, tb), slotref(seq, ab),
             slotref(idx, bb));
        return t;
    }
    case E_ATTR: {
        int obj = gen_expr(cg, e->as.attr.obj);
        int t = new_temp(cg);
        SB name = {0};
        sb_c_string(&name, e->as.attr.name);
        emit(cg, "%s = em_getattr(%s, %s);", slotref(t, tb), slotref(obj, ab),
             name.buf);
        free(name.buf);
        return t;
    }
    }
    return 0; /* unreachable */
}

void gen_if_arms(Cg *cg, const Stmt *s, size_t arm) {
    char ab[32];
    int c = gen_expr(cg, s->as.ifs.conds[arm]);
    emit(cg, "if (em_truthy(%s)) {", slotref(c, ab));
    cg->indent++;
    gen_block(cg, &s->as.ifs.blocks[arm]);
    cg->indent--;
    if (arm + 1 < s->as.ifs.count) {
        emit(cg, "} else {");
        cg->indent++;
        gen_if_arms(cg, s, arm + 1);
        cg->indent--;
        emit(cg, "}");
    } else if (s->as.ifs.has_else) {
        emit(cg, "} else {");
        cg->indent++;
        gen_block(cg, &s->as.ifs.else_block);
        cg->indent--;
        emit(cg, "}");
    } else {
        emit(cg, "}");
    }
}
