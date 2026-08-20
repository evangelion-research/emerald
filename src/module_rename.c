/* Modules: the rename pass that mangles imported names into one flat program. */
#include "module_internal.h"

static bool shadowed(const RScope *sc, const char *name) {
    for (; sc; sc = sc->parent)
        if (names_has(&sc->bound, name)) return true;
    return false;
}

static const Bind *find_bind(const RW *rw, const char *name) {
    for (size_t i = 0; i < rw->nbinds; i++)
        if (strcmp(rw->binds[i].local, name) == 0) return &rw->binds[i];
    return NULL;
}

/* Rewrite a bare name. Returns the new name, or `name` when it stays put. */
static const char *rw_name(RW *rw, const char *name, const RScope *sc,
                           const char **disp) {
    *disp = NULL;
    if (shadowed(sc, name)) return name;
    const Bind *b = find_bind(rw, name);
    if (b && b->target) {
        *disp = b->disp;
        return b->target;
    }
    if (b && b->mod) return name; /* a module object; only valid as `m.f` */
    if (mod_exports(rw->mod, name)) return mangle(rw->mod, name);
    return name;
}

static void rw_dim(RW *rw, DimExpr *e, const RScope *sc) {
    if (!e) return;
    if (e->kind == DE_VAR) {
        /* a local `B: dim` parameter is not a module name: leave it alone */
        if (!names_has(&rw->tvars, e->var)) {
            const char *disp;
            const char *n = rw_name(rw, e->var, sc, &disp);
            if (n != e->var) {
                free(e->var);
                e->var = xstrdup(n);
            }
        }
        return;
    }
    rw_dim(rw, e->lhs, sc);
    rw_dim(rw, e->rhs, sc);
}

static void rw_type(RW *rw, TypeExpr *t, const RScope *sc) {
    if (!t) return;
    switch (t->kind) {
    case TE_NAME: {
        if (!names_has(&rw->tvars, t->name)) {
            const char *disp;
            t->name = (char *)rw_name(rw, t->name, sc, &disp);
        }
        for (size_t i = 0; i < t->arg_count; i++) rw_type(rw, t->args[i], sc);
        break;
    }
    case TE_TENSOR:
        rw_type(rw, t->tensor.dtype, sc);
        for (size_t i = 0; i < t->tensor.shape_count; i++)
            rw_dim(rw, t->tensor.shape[i], sc);
        break;
    case TE_FIN:
        rw_dim(rw, t->fin_dim, sc);
        break;
    case TE_EQ:
        rw_dim(rw, t->eq_lhs, sc);
        rw_dim(rw, t->eq_rhs, sc);
        break;
    case TE_LIST: rw_type(rw, t->elem, sc); break;
    case TE_SEQ:  rw_type(rw, t->elem, sc); break;
    case TE_REC:
        for (size_t i = 0; i < t->fields.count; i++)
            rw_type(rw, t->fields.types[i], sc);
        break;
    case TE_UNION: case TE_INTER:
        rw_type(rw, t->lhs, sc);
        rw_type(rw, t->rhs, sc);
        break;
    case TE_FUNC:
        for (size_t i = 0; i < t->fun.param_count; i++)
            rw_type(rw, t->fun.params[i], sc);
        rw_type(rw, t->fun.ret, sc);
        break;
    case TE_LIT:
        break;
    }
}

static void rw_expr(RW *rw, Expr *e, const RScope *sc) {
    if (!e) return;
    switch (e->kind) {
    case E_NAME: {
        const Bind *b = shadowed(sc, e->as.sval) ? NULL : find_bind(rw, e->as.sval);
        if (b && b->mod) {
            ld_error(rw->ld, "E_IMPORT_MODULE_VALUE", rw->mod->file,
                     e->line, e->col,
                     "'%s' is a module, not a value; use '%s.<name>' to reach "
                     "into it", e->as.sval, e->as.sval);
            return;
        }
        const char *disp;
        const char *n = rw_name(rw, e->as.sval, sc, &disp);
        if (n != e->as.sval) {
            if (!e->disp) e->disp = disp ? disp : e->as.sval;
            e->as.sval = (char *)n;
        }
        break;
    }
    case E_ATTR: {
        Expr *obj = e->as.attr.obj;
        const char *field = e->as.attr.name;
        const Bind *b = (obj->kind == E_NAME && !shadowed(sc, obj->as.sval))
                            ? find_bind(rw, obj->as.sval) : NULL;
        if (!b || !b->mod) { rw_expr(rw, obj, sc); break; }

        /* `m.f` on a module object: resolve it to the module's mangled name */
        Mod *target = b->mod;
        const char *spelling = xasprintf("%s.%s", obj->as.sval, field);
        if (is_private(field))
            ld_error(rw->ld, "E_IMPORT_PRIVATE", rw->mod->file, e->line, e->col,
                     "'%s' is private to module '%s' (names starting with '_' "
                     "are not exported)", field, target->dotted);
        else if (!mod_exports(target, field))
            ld_error(rw->ld, "E_IMPORT_NAME", rw->mod->file, e->line, e->col,
                     "module '%s' has no member '%s'", target->dotted, field);
        e->kind = E_NAME;
        e->disp = spelling;
        e->as.sval = mangle(target, field);
        break;
    }
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            rw_expr(rw, e->as.list.items[i], sc);
        break;
    case E_TUPLE:
        for (size_t i = 0; i < e->as.list.count; i++) rw_expr(rw, e->as.list.items[i], sc);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++) rw_expr(rw, e->as.rec.values[i], sc);
        break;
    case E_DICT:
        for (size_t i = 0; i < e->as.dict.count; i++) { rw_expr(rw,e->as.dict.keys[i],sc); rw_expr(rw,e->as.dict.values[i],sc); }
        break;
    case E_SET:
        for (size_t i = 0; i < e->as.set.count; i++) rw_expr(rw,e->as.set.items[i],sc);
        break;
    case E_SLICE:
        rw_expr(rw,e->as.slice.seq,sc); if(e->as.slice.start)rw_expr(rw,e->as.slice.start,sc); if(e->as.slice.stop)rw_expr(rw,e->as.slice.stop,sc); if(e->as.slice.step)rw_expr(rw,e->as.slice.step,sc); break;
    case E_COMP:
        rw_expr(rw,e->as.comp.seq,sc); if(e->as.comp.cond)rw_expr(rw,e->as.comp.cond,sc); rw_expr(rw,e->as.comp.elt,sc); if(e->as.comp.key)rw_expr(rw,e->as.comp.key,sc); break;
    case E_FSTR:
        for(size_t i=0;i<e->as.fstr.count;i++)rw_expr(rw,e->as.fstr.exprs[i],sc); break;
    case E_BINOP:
        rw_expr(rw, e->as.bin.lhs, sc);
        rw_expr(rw, e->as.bin.rhs, sc);
        break;
    case E_UNOP:
        rw_expr(rw, e->as.un.operand, sc);
        break;
    case E_CALL:
        rw_expr(rw, e->as.call.fn, sc);
        for (size_t i = 0; i < e->as.call.count; i++)
            rw_expr(rw, e->as.call.args[i], sc);
        break;
    case E_INDEX:
        rw_expr(rw, e->as.index.seq, sc);
        rw_expr(rw, e->as.index.idx, sc);
        break;
    case E_TRY:
        rw_expr(rw, e->as.try_expr, sc);
        break;
    case E_CATCH: {
        /* An arm's error tag names a *type*, matched by the `_tag` string the
         * declaration baked in, so it is never renamed. The arm's binding is
         * a fresh local, and shadows anything the linker would rewrite. */
        rw_expr(rw, e->as.ctch.subject, sc);
        for (size_t i = 0; i < e->as.ctch.count; i++) {
            const CatchArm *a = &e->as.ctch.arms[i];
            RScope asc;
            memset(&asc, 0, sizeof(asc));
            asc.parent = sc;
            if (a->bind) mod_names_add(&asc.bound, a->bind);
            rw_expr(rw, a->body, &asc);
            free(asc.bound.items);
        }
        break;
    }
    case E_LAMBDA: {
        /* lambda parameters bind like function parameters */
        RScope lsc;
        memset(&lsc, 0, sizeof(lsc));
        lsc.parent = sc;
        for (size_t i = 0; i < e->as.lam.param_count; i++)
            mod_names_add(&lsc.bound, e->as.lam.params[i]);
        for (size_t i = 0; i < e->as.lam.param_count; i++)
            rw_type(rw, e->as.lam.param_types[i], &lsc);
        rw_expr(rw, e->as.lam.body, &lsc);
        free(lsc.bound.items);
        break;
    }
    default:
        break; /* literals carry no names */
    }
}

/* every name a pattern binds (recursively through record patterns) */
static void pat_binds(const Pat *p, Names *out) {
    switch (p->kind) {
    case P_BIND:
        mod_names_add(out, p->bind);
        break;
    case P_REC:
        for (size_t i = 0; i < p->rec.count; i++)
            pat_binds(p->rec.items[i], out);
        break;
    default:
        break;
    }
}

static void rw_block(RW *rw, Block *b, const RScope *sc);

static void rw_func(RW *rw, Stmt *s, const RScope *parent);

void rw_stmt(RW *rw, Stmt *s, const RScope *sc) {
    switch (s->kind) {
    case S_EXPR:   rw_expr(rw, s->as.expr, sc); break;
    case S_ASSIGN:
        /* An assignment target is a *binding*, not a use: the module's own
         * globals still mangle (so `count = count + 1` updates counter__count),
         * but an imported name is rebound locally, never rewritten to the
         * imported module's name — writing through an import would clobber
         * the module's global, so it is rejected outright. */
        {
            Expr *tgt = s->as.assign.target;
            if (tgt->kind == E_NAME) {
                if (!shadowed(sc, tgt->as.sval)) {
                    const Bind *b = find_bind(rw, tgt->as.sval);
                    if (b)
                        ld_error(rw->ld, "E_IMPORT_MODULE_VALUE",
                                 rw->mod->file, tgt->line, tgt->col,
                                 "cannot assign to imported name '%s' "
                                 "(imports are read-only)", tgt->as.sval);
                    else if (mod_exports(rw->mod, tgt->as.sval))
                        tgt->as.sval = mangle(rw->mod, tgt->as.sval);
                }
            } else if (tgt->kind == E_INDEX) {
                rw_expr(rw, tgt->as.index.seq, sc);
                rw_expr(rw, tgt->as.index.idx, sc);
            } else { /* E_ATTR */
                Expr *obj = tgt->as.attr.obj;
                const Bind *b = (obj->kind == E_NAME &&
                                 !shadowed(sc, obj->as.sval))
                                    ? find_bind(rw, obj->as.sval) : NULL;
                if (b && b->mod)
                    ld_error(rw->ld, "E_IMPORT_MODULE_VALUE",
                             rw->mod->file, tgt->line, tgt->col,
                             "cannot assign into module '%s'", obj->as.sval);
                else
                    rw_expr(rw, obj, sc);
            }
        }
        rw_type(rw, s->as.assign.ann, sc);
        rw_expr(rw, s->as.assign.value, sc);
        break;
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++) {
            rw_expr(rw, s->as.ifs.conds[i], sc);
            rw_block(rw, &s->as.ifs.blocks[i], sc);
        }
        if (s->as.ifs.has_else) rw_block(rw, &s->as.ifs.else_block, sc);
        break;
    case S_WHILE:
        rw_expr(rw, s->as.wh.cond, sc);
        rw_block(rw, &s->as.wh.body, sc);
        break;
    case S_FOR: {
        /* the loop variable is a binding too: same rules as an assignment
         * target, so an imported name cannot be used as a loop variable */
        if (!shadowed(sc, s->as.fr.var)) {
            const Bind *b = find_bind(rw, s->as.fr.var);
            if (b)
                ld_error(rw->ld, "E_IMPORT_MODULE_VALUE",
                         rw->mod->file, s->line, s->col,
                         "cannot assign to imported name '%s' "
                         "(imports are read-only)", s->as.fr.var);
            else if (mod_exports(rw->mod, s->as.fr.var))
                s->as.fr.var = mangle(rw->mod, s->as.fr.var);
        }
        rw_expr(rw, s->as.fr.seq, sc);
        rw_block(rw, &s->as.fr.body, sc);
        break;
    }
    case S_RETURN: rw_expr(rw, s->as.ret, sc); break;
    case S_BLOCK:  rw_block(rw, &s->as.block, sc); break;
    case S_MATCH:
        rw_expr(rw, s->as.mtch.subject, sc);
        for (size_t i = 0; i < s->as.mtch.count; i++) {
            /* each arm's bindings shadow, like a function's parameters */
            RScope asc;
            memset(&asc, 0, sizeof(asc));
            asc.parent = sc;
            pat_binds(s->as.mtch.pats[i], &asc.bound);
            rw_block(rw, &s->as.mtch.blocks[i], &asc);
            free(asc.bound.items);
        }
        break;
    case S_FUNC:   rw_func(rw, s, sc); break;
    case S_TYPEDEF: {
        size_t mark = rw->tvars.count;
        for (size_t i = 0; i < s->as.tdef.param_count; i++)
            mod_names_add(&rw->tvars, s->as.tdef.params[i]);
        rw_type(rw, s->as.tdef.value, sc);
        rw->tvars.count = mark;
        if (!sc && rw->mod->prefix)
            s->as.tdef.name = mangle(rw->mod, s->as.tdef.name);
        break;
    }
    case S_DIMDECL:
        /* module-level dim declarations are exported names, mangled like types */
        if (!sc && rw->mod->prefix)
            for (size_t i = 0; i < s->as.dim.count; i++)
                s->as.dim.names[i] = mangle(rw->mod, s->as.dim.names[i]);
        break;
    default:
        break;
    }
}

static void rw_block(RW *rw, Block *b, const RScope *sc) {
    for (size_t i = 0; i < b->count; i++) rw_stmt(rw, b->items[i], sc);
}

/* Collect the nested `def` names of a block (function-level scoping). */
static void collect_nested_defs(const Block *b, Names *out) {
    for (size_t i = 0; i < b->count; i++) {
        const Stmt *s = b->items[i];
        switch (s->kind) {
        case S_FUNC: mod_names_add(out, s->as.func.name); break;
        case S_IF:
            for (size_t j = 0; j < s->as.ifs.count; j++)
                collect_nested_defs(&s->as.ifs.blocks[j], out);
            if (s->as.ifs.has_else) collect_nested_defs(&s->as.ifs.else_block, out);
            break;
        case S_WHILE: collect_nested_defs(&s->as.wh.body, out); break;
        case S_FOR:   collect_nested_defs(&s->as.fr.body, out); break;
        case S_BLOCK: collect_nested_defs(&s->as.block, out); break;
        case S_MATCH:
            for (size_t j = 0; j < s->as.mtch.count; j++)
                collect_nested_defs(&s->as.mtch.blocks[j], out);
            break;
        default: break;
        }
    }
}

static void collect_local_cb(const char *name, const char *file, int line,
                             void *ud) {
    (void)line; (void)file;
    LocalCtx *lc = ud;
    /* assigning a module global's name updates the global, so it is not local */
    if (!names_has(lc->globals, name)) mod_names_add(lc->bound, name);
}

static void rw_func(RW *rw, Stmt *s, const RScope *parent) {
    RScope sc;
    memset(&sc, 0, sizeof(sc));
    sc.parent = parent;
    for (size_t i = 0; i < s->as.func.param_count; i++)
        mod_names_add(&sc.bound, s->as.func.params[i]);
    LocalCtx lc = { &sc.bound, &rw->mod->globals };
    ast_collect_assigned(&s->as.func.body, collect_local_cb, &lc);
    collect_nested_defs(&s->as.func.body, &sc.bound);

    size_t mark = rw->tvars.count;
    for (size_t i = 0; i < s->as.func.tparam_count; i++)
        mod_names_add(&rw->tvars, s->as.func.tparams[i]);
    for (size_t i = 0; i < s->as.func.param_count; i++)
        rw_type(rw, s->as.func.param_types[i], &sc);
    rw_type(rw, s->as.func.ret_type, &sc);
    for (size_t i = 0; i < s->as.func.param_count; i++)
        if (s->as.func.defaults && s->as.func.defaults[i]) rw_expr(rw, s->as.func.defaults[i], &sc);
    rw_block(rw, &s->as.func.body, &sc);
    rw->tvars.count = mark;

    /* a top-level def is a module-level name and gets the module's prefix */
    if (!parent && rw->mod->prefix)
        s->as.func.name = mangle(rw->mod, s->as.func.name);
    free(sc.bound.items);
}
