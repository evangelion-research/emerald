/* Codegen: statements and pattern matching. */
#include "codegen_internal.h"

/* --- pattern matching ---------------------------------------------------- */
/* Append a C boolean expression testing whether `subj` (a C expression)
 * matches pattern `p`. `subj` may itself be an `em_getattr(...)` chain; the
 * tests short-circuit, so no getattr runs unless the record/field checks
 * before it have already passed.
 */
static void gen_pat_test(const Pat *p, const char *subj, SB *out) {
    switch (p->kind) {
    case P_WILD:
    case P_BIND:
        sb_printf(out, "true");
        break;
    case P_LIT:
        switch (p->lit.kind) {
        case LIT_INT:
            sb_printf(out, "em_truthy(em_eq(%s, em_int(%lldLL)))", subj,
                      (long long)p->lit.ival);
            break;
        case LIT_STR:
            sb_printf(out, "em_truthy(em_eq(%s, em_str_new(", subj);
            sb_c_string(out, p->lit.sval);
            sb_printf(out, ")))");
            break;
        case LIT_BOOL:
            sb_printf(out, "em_truthy(em_eq(%s, em_bool(%s)))", subj,
                      p->lit.ival ? "true" : "false");
            break;
        case LIT_NONE:
            sb_printf(out, "em_truthy(em_eq(%s, em_none()))", subj);
            break;
        }
        break;
    case P_REC: {
        sb_printf(out, "(em_is_record(%s)", subj);
        for (size_t i = 0; i < p->rec.count; i++) {
            const Pat *it = p->rec.items[i];
            if (it->kind == P_WILD) continue;
            sb_printf(out, " && em_rec_has(%s, ", subj);
            sb_c_string(out, it->name);
            sb_printf(out, ")");
            if (it->kind == P_LIT || it->kind == P_REC) {
                SB f = {0};
                sb_printf(&f, "em_getattr(%s, ", subj);
                sb_c_string(&f, it->name);
                sb_printf(&f, ")");
                sb_printf(out, " && ");
                gen_pat_test(it, f.buf, out);
                free(f.buf);
            }
        }
        sb_printf(out, ")");
        break;
    }
    }
}

/* Emit variable bindings for the names pattern `p` matches, from the value
 * `subj` (a C expression). Only runs once the arm's test has passed, so
 * em_getattr is safe here. */
static void gen_pat_binds(Cg *cg, const Pat *p, const char *subj) {
    switch (p->kind) {
    case P_BIND:
        gen_var_write(cg, p->bind, subj);
        break;
    case P_REC:
        for (size_t i = 0; i < p->rec.count; i++) {
            const Pat *it = p->rec.items[i];
            if (it->kind != P_BIND && it->kind != P_REC) continue;
            SB f = {0};
            sb_printf(&f, "em_getattr(%s, ", subj);
            sb_c_string(&f, it->name);
            sb_printf(&f, ")");
            gen_pat_binds(cg, it, f.buf);
            free(f.buf);
        }
        break;
    case P_LIT:
    case P_WILD:
        break;
    }
}

static void gen_match_arms(Cg *cg, const Stmt *s, int subj, size_t arm) {
    char ab[32];
    SB cond = {0};
    gen_pat_test(s->as.mtch.pats[arm], slotref(subj, ab), &cond);
    emit(cg, "if (%s) {", cond.buf);
    free(cond.buf);
    cg->indent++;
    gen_pat_binds(cg, s->as.mtch.pats[arm], slotref(subj, ab));
    gen_block(cg, &s->as.mtch.blocks[arm]);
    cg->indent--;
    if (arm + 1 < s->as.mtch.count) {
        emit(cg, "} else {");
        cg->indent++;
        gen_match_arms(cg, s, subj, arm + 1);
        cg->indent--;
        emit(cg, "}");
    } else {
        /* unreachable: the checker proves exhaustiveness */
        emit(cg, "} else {");
        cg->indent++;
        emit(cg, "rt_fatal(\"match: no pattern matched (exhaustiveness check "
                  "failed)\");");
        cg->indent--;
        emit(cg, "}");
    }
}

/* --- statements ---------------------------------------------------------- */
/* Emit the closure creation for a nested `def` statement, binding the parent
 * function's local slot for the function name. */
static void gen_nested_def(Cg *cg, const Stmt *s) {
    FuncInfo *child = NULL;
    for (size_t i = 0; i < cg->fi->child_count; i++)
        if (strcmp(cg->fi->children[i]->name, s->as.func.name) == 0) {
            child = cg->fi->children[i];
            break;
        }
    if (!child) return;
    int slot = names_find(&cg->fi->locals, s->as.func.name);
    if (slot < 0) return;
    bool cell = cg->fi->local_cell[slot];
    size_t n = child->captures.count;
    size_t arity = s->as.func.param_count, min=arity;
    while(min && s->as.func.defaults && s->as.func.defaults[min-1]) min--;
    size_t dc=s->as.func.defaults ? arity-min : 0;
    char ab[32];

    if (n == 0) {
        if (!dc) {
            if (cell) emit(cg, "em_cell_set(%s, em_mkclosure(%s_tramp, %zu, NULL, 0, NULL, 0));",slotref(slot,ab),child->cname,arity);
            else emit(cg, "%s = em_mkclosure(%s_tramp, %zu, NULL, 0, NULL, 0);",slotref(slot,ab),child->cname,arity);
        } else {
            emit(cg,"{ Value __defs[%zu];",dc);
            for(size_t j=min;j<arity;j++){int dv=gen_expr(cg,s->as.func.defaults[j]);emit(cg,"  __defs[%zu] = %s;",j-min,slotref(dv,ab));}
            if(cell) emit(cg,"  em_cell_set(%s, em_mkclosure(%s_tramp, %zu, NULL, 0, __defs, %zu));",slotref(slot,ab),child->cname,arity,dc);
            else emit(cg,"  %s = em_mkclosure(%s_tramp, %zu, NULL, 0, __defs, %zu);",slotref(slot,ab),child->cname,arity,dc);
            emit(cg,"}");
        }
    } else {
        emit(cg, "{ Value __cap[%zu];", n);
        for (size_t k = 0; k < n; k++) {
            char cb[32];
            emit(cg, "  __cap[%zu] = %s;", k,
                 cellref(cg, child->captures.names[k], cb));
        }
        if (dc) { emit(cg,"  Value __defs[%zu];",dc); for(size_t j=min;j<arity;j++){int dv=gen_expr(cg,s->as.func.defaults[j]);emit(cg,"  __defs[%zu] = %s;",j-min,slotref(dv,ab));} }
        if (cell)
            emit(cg, dc ? "  em_cell_set(%s, em_mkclosure(%s_tramp, %zu, __cap, %zu, __defs, %zu));" : "  em_cell_set(%s, em_mkclosure(%s_tramp, %zu, __cap, %zu, NULL, 0));", slotref(slot,ab),child->cname,arity,n,dc);
        else
            emit(cg, dc ? "  %s = em_mkclosure(%s_tramp, %zu, __cap, %zu, __defs, %zu);" : "  %s = em_mkclosure(%s_tramp, %zu, __cap, %zu, NULL, 0);", slotref(slot,ab),child->cname,arity,n,dc);
        emit(cg, "}");
    }
}

static void gen_stmt(Cg *cg, const Stmt *s) {
    int mark = cg->ntemps; /* release this statement's temporaries at the end */
    char ab[32], bb[32], cb[32];

    /* track the current source position so runtime errors report a location;
     * a linked program spans several files, so the file can change too */
    if (s->file && s->file != cg->last_file) {
        cg->last_file = s->file;
        SB f = {0};
        sb_c_string(&f, s->file);
        emit(cg, "rt_cur_file = %s;", f.buf);
        free(f.buf);
    }
    if (s->line != cg->last_line) {
        cg->last_line = s->line;
        emit(cg, "rt_cur_line = %d;", s->line);
    }

    switch (s->kind) {
    case S_EXPR:
        gen_expr(cg, s->as.expr);
        break;
    case S_ASSIGN: {
        const Expr *tgt = s->as.assign.target;
        if (tgt->kind == E_NAME) {
            int v = gen_expr(cg, s->as.assign.value);
            gen_var_write(cg, tgt->as.sval, slotref(v, cb));
        } else if (tgt->kind == E_INDEX) {
            int seq = gen_expr(cg, tgt->as.index.seq);
            int idx = gen_expr(cg, tgt->as.index.idx);
            int v = gen_expr(cg, s->as.assign.value);
            emit(cg, "em_setindex(%s, %s, %s);", slotref(seq, ab),
                 slotref(idx, bb), slotref(v, cb));
        } else { /* E_ATTR */
            int obj = gen_expr(cg, tgt->as.attr.obj);
            int v = gen_expr(cg, s->as.assign.value);
            SB name = {0};
            sb_c_string(&name, tgt->as.attr.name);
            emit(cg, "em_setattr(%s, %s, %s);", slotref(obj, ab), name.buf,
                 slotref(v, bb));
            free(name.buf);
        }
        break;
    }
    case S_IF:
        gen_if_arms(cg, s, 0);
        break;
    case S_WHILE:
        emit(cg, "for (;;) {");
        cg->indent++;
        {
            int c = gen_expr(cg, s->as.wh.cond);
            emit(cg, "if (!em_truthy(%s)) break;", slotref(c, ab));
            /* the condition's temps can be reused by the body */
            cg->ntemps = mark;
            gen_block(cg, &s->as.wh.body);
        }
        cg->indent--;
        emit(cg, "}");
        break;
    case S_FOR: {
        int seq = gen_expr(cg, s->as.fr.seq);
        int idx = new_temp(cg);
        emit(cg, "%s = em_int(0);", slotref(idx, ab));
        emit(cg, "for (;;) {");
        cg->indent++;
        emit(cg, "Value __it;");
        emit(cg, "if (!rt_iter_get(%s, %s.as.i, &__it)) break;",
             slotref(seq, ab), slotref(idx, bb));
        emit(cg, "%s.as.i += 1;", slotref(idx, ab));
        gen_var_write(cg, s->as.fr.var, "__it");
        gen_block(cg, &s->as.fr.body);
        cg->indent--;
        emit(cg, "}");
        break;
    }
    case S_RETURN: {
        if (cg->in_tco && s->as.ret && s->as.ret->kind == E_CALL &&
            s->as.ret->as.call.fn->kind == E_NAME &&
            strcmp(s->as.ret->as.call.fn->as.sval, cg->fi->name) == 0) {
            /* self tail call: evaluate the arguments, reassign the parameter
             * slots, and jump back to the loop header (see gen_function) */
            size_t argc = s->as.ret->as.call.count;
            int *args = xmalloc(sizeof(int) * argc);
            for (size_t i = 0; i < argc; i++)
                args[i] = gen_expr(cg, s->as.ret->as.call.args[i]);
            for (size_t i = 0; i < argc && i < cg->fi->node->as.func.param_count; i++)
                gen_var_write(cg, cg->fi->node->as.func.params[i],
                              slotref(args[i], ab));
            free(args);
            emit(cg, "goto __tail;");
        } else if (s->as.ret) {
            int v = gen_expr(cg, s->as.ret);
            emit(cg, "{ Value __r = %s; rt_pop_frame(); return __r; }",
                 slotref(v, ab));
        } else {
            emit(cg, "{ rt_pop_frame(); return em_none(); }");
        }
        break;
    }
    case S_MATCH:
        gen_match_arms(cg, s, gen_expr(cg, s->as.mtch.subject), 0);
        break;
    case S_BREAK:
        emit(cg, "break;");
        break;
    case S_CONTINUE:
        emit(cg, "continue;");
        break;
    case S_PASS:
        break;
    case S_BLOCK: /* just grouping; no new variable scope */
        gen_block(cg, &s->as.block);
        break;
    case S_FUNC:
        if (cg->fi)
            gen_nested_def(cg, s);
        break; /* top-level bodies are emitted separately */
    case S_TYPEDEF:
    case S_IMPORT:
    case S_DIMDECL:
        break; /* compile-time only (imports are resolved away by linking) */
    }
    cg->ntemps = mark;
}

void gen_block(Cg *cg, const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        gen_stmt(cg, b->items[i]);
}
