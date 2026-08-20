/* Codegen: program emission and tail-call optimization. */
#include "codegen_internal.h"

/* --- program ------------------------------------------------------------- */
static void collect_name_cb(const char *name, const char *file, int line,
                            void *ud) {
    (void)line;
    names_add_file((Names *)ud, name, file);
}

/* --- tail-call optimization ---------------------------------------------- */
/* is `e` a direct call to the function named `name`? */
static bool is_self_call(const Expr *e, const char *name) {
    return e->kind == E_CALL && e->as.call.fn->kind == E_NAME &&
           strcmp(e->as.call.fn->as.sval, name) == 0;
}

/* Does the block contain a `return f(...)` self-call? Such a return is a
 * tail call: the function's result is exactly f's result, so it can be
 * rewritten as "reassign params, loop" — constant stack, real tail
 * recursion. Only the body of the function itself is scanned: a self-call
 * inside a nested `def` belongs to that nested function.
 */
static bool has_tail_call_stmt(const Stmt *s, const char *name);

static bool has_tail_call_block(const Block *b, const char *name) {
    for (size_t i = 0; i < b->count; i++)
        if (has_tail_call_stmt(b->items[i], name)) return true;
    return false;
}

static bool has_tail_call_stmt(const Stmt *s, const char *name) {
    switch (s->kind) {
    case S_RETURN:
        return s->as.ret && is_self_call(s->as.ret, name);
    case S_BLOCK:
        return has_tail_call_block(&s->as.block, name);
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++)
            if (has_tail_call_block(&s->as.ifs.blocks[i], name)) return true;
        return s->as.ifs.has_else &&
               has_tail_call_block(&s->as.ifs.else_block, name);
    case S_MATCH:
        for (size_t i = 0; i < s->as.mtch.count; i++)
            if (has_tail_call_block(&s->as.mtch.blocks[i], name)) return true;
        return false;
    case S_WHILE:
        return has_tail_call_block(&s->as.wh.body, name);
    case S_FOR:
        return has_tail_call_block(&s->as.fr.body, name);
    default:
        return false;
    }
}

void gen_function(FILE *out, FuncInfo *fi, Names *globals,
                         FuncInfo **top, size_t top_count) {
    Cg cg;
    memset(&cg, 0, sizeof(cg));
    cg.globals = globals;
    cg.fi = fi;
    cg.top = top;
    cg.top_count = top_count;
    cg.indent = 1;

    bool tco = has_tail_call_block(&fi->node->as.func.body, fi->name);
    cg.in_tco = tco;
    if (tco) {
        /* the whole body runs inside `for (;;)`: a self tail call becomes
         * "reassign params; goto __tail", re-entering the body with the new
         * arguments in place. Real returns (and the implicit fall-through
         * return) pop the frame and exit; the frame is pushed once at entry
         * and the loop never nests it. */
        emit(&cg, "for (;;) {");
        cg.indent++;
        emit(&cg, "__tail:;");
    }
    gen_block(&cg, &fi->node->as.func.body);
    if (tco) {
        emit(&cg, "rt_pop_frame();");
        emit(&cg, "return em_none();");
        cg.indent--;
        emit(&cg, "}");
    }

    size_t nparams = fi->node->as.func.param_count;
    size_t nlocals = fi->locals.count;
    int nslots = (int)nlocals + cg.max_temps;
    if (nslots < 1) nslots = 1;

    fprintf(out, "static Value %s(Value *__env", fi->cname);
    for (size_t i = 0; i < nparams; i++)
        fprintf(out, ", Value __p%zu", i);
    fprintf(out, ") {\n");
    fprintf(out, "    (void)__env;\n");
    fprintf(out, "    Value F[%d];\n", nslots);
    fprintf(out, "    RootFrame __fr;\n");
    fprintf(out, "    for (int __i = 0; __i < %d; __i++) F[__i] = em_none();\n",
            nslots);
    fprintf(out, "    rt_push_frame(&__fr, F, %d);\n", nslots);
    for (size_t i = 0; i < nparams; i++) {
        if (fi->local_cell[i])
            fprintf(out, "    F[%zu] = em_cell(__p%zu);\n", i, i);
        else
            fprintf(out, "    F[%zu] = __p%zu;\n", i, i);
    }
    for (size_t i = nparams; i < nlocals; i++)
        if (fi->local_cell[i])
            fprintf(out, "    F[%zu] = em_cell(em_none());\n", i);

    fwrite(cg.body.buf ? cg.body.buf : "", 1, cg.body.len, out);
    if (!tco) {
        fprintf(out, "    rt_pop_frame();\n");
        fprintf(out, "    return em_none();\n");
    }
    fprintf(out, "}\n\n");

    /* uniform entry point used for first-class function values */
    fprintf(out, "static Value %s_tramp(Value *__env, Value *__args) {\n",
            fi->cname);
    fprintf(out, "    return %s(__env", fi->cname);
    for (size_t i = 0; i < nparams; i++)
        fprintf(out, ", __args[%zu]", i);
    fprintf(out, ");\n}\n\n");

    free(cg.body.buf);
}

void codegen_program(FILE *out, const Program *prog, const char *filename) {
    Names globals = {0};
    ast_collect_assigned(&prog->body, collect_name_cb, &globals);
    collect_match_pats_block(&prog->body, collect_name_cb, &globals);

    /* build the function tree (top-level defs are the root's children) */
    Ctx ctx = { &globals, 0, NULL, 0, 0 };
    FuncInfo **top = NULL;
    size_t top_count = 0, top_cap = 0;
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_FUNC) continue;
        if (top_count == top_cap) {
            top_cap = top_cap ? top_cap * 2 : 8;
            top = xrealloc(top, sizeof(FuncInfo *) * top_cap);
        }
        top[top_count++] = build_func(&ctx, s, NULL);
    }

    /* the implicit top-level function, owning top-level lambdas. Built before
     * the emit loops so its lambda functions get prototypes and bodies too. */
    FuncInfo *root = make_top_root(&ctx, &prog->body);

    fprintf(out, "/* generated by emeraldc; compile with src/runtime_*.c */\n");
    fprintf(out, "#include \"runtime.h\"\n\n");

    SB fname = {0};
    sb_c_string(&fname, filename ? filename : "<unknown>");
    fprintf(out, "static const char *rt_src_file = %s;\n", fname.buf);
    free(fname.buf);

    size_t nglobals = globals.count ? globals.count : 1;
    fprintf(out, "static Value G[%zu];\n\n", nglobals);

    /* prototypes first so functions can be mutually recursive */
    for (size_t i = 0; i < ctx.all_count; i++) {
        fprintf(out, "static Value %s(Value *__env", ctx.all[i]->cname);
        for (size_t j = 0; j < ctx.all[i]->node->as.func.param_count; j++)
            fprintf(out, ", Value");
        fprintf(out, ");\n");
        fprintf(out, "static Value %s_tramp(Value *__env, Value *__args);\n",
                ctx.all[i]->cname);
    }
    fprintf(out, "\n");

    for (size_t i = 0; i < ctx.all_count; i++)
        gen_function(out, ctx.all[i], &globals, top, top_count);

    /* top level: names are globals (G); temporaries live in a local F. The
     * root FuncInfo gives top-level lambdas their own closure functions. */
    Cg cg;
    memset(&cg, 0, sizeof(cg));
    cg.globals = &globals;
    cg.fi = root;
    cg.top = top;
    cg.top_count = top_count;
    cg.indent = 1;
    gen_block(&cg, &prog->body);

    int ntemps = (int)root->locals.count + cg.max_temps;
    if (ntemps < 1) ntemps = 1;
    fprintf(out, "int main(int argc, char **argv) {\n");
    fprintf(out, "    rt_init();\n");
    fprintf(out, "    rt_set_args(argc, argv);\n");
    fprintf(out, "    rt_cur_file = rt_src_file;\n");
    fprintf(out, "    static RootFrame __gfr;\n");
    fprintf(out, "    for (int __i = 0; __i < %zu; __i++) G[__i] = em_none();\n",
            nglobals);
    fprintf(out, "    rt_push_frame(&__gfr, G, %zu);\n", nglobals);
    fprintf(out, "    Value F[%d];\n", ntemps);
    fprintf(out, "    RootFrame __fr;\n");
    fprintf(out, "    for (int __i = 0; __i < %d; __i++) F[__i] = em_none();\n",
            ntemps);
    fprintf(out, "    rt_push_frame(&__fr, F, %d);\n", ntemps);
    fwrite(cg.body.buf ? cg.body.buf : "", 1, cg.body.len, out);
    fprintf(out, "    rt_pop_frame();\n");
    fprintf(out, "    rt_pop_frame();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    free(cg.body.buf);
    free(globals.names);
}
