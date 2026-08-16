/* Emerald code generator: AST -> C.
 *
 * Strategy: every expression is lowered into simple statements over "slots".
 * Each function gets one `Value F[n]` array holding all locals and all
 * expression temporaries; globals live in a static `Value G[n]`. Both arrays
 * are registered with the GC as root frames, so any Value that exists across
 * an allocation is reachable by the collector (precise rooting, no C-stack
 * scanning).
 *
 * Slot encoding: >= 0 means F[slot]; < 0 means G[-slot - 1].
 * Temporaries are allocated monotonically per statement and released by
 * restoring a watermark, so slot counts stay proportional to expression
 * depth, not program size.
 */
#include "codegen.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* --- string builder ------------------------------------------------------ */

typedef struct { char *buf; size_t len, cap; } SB;

static void sb_grow(SB *sb, size_t need) {
    if (sb->len + need + 1 <= sb->cap) return;
    sb->cap = sb->cap ? sb->cap * 2 : 256;
    while (sb->cap < sb->len + need + 1) sb->cap *= 2;
    sb->buf = realloc(sb->buf, sb->cap);
    if (!sb->buf) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
}

static void sb_printf(SB *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_grow(sb, (size_t)n);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

/* --- name tables --------------------------------------------------------- */

typedef struct { char **names; size_t count, cap; } Names;

static int names_find(const Names *ns, const char *name) {
    for (size_t i = 0; i < ns->count; i++)
        if (strcmp(ns->names[i], name) == 0) return (int)i;
    return -1;
}

static void names_add(Names *ns, const char *name) {
    if (names_find(ns, name) >= 0) return;
    if (ns->count == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 8;
        ns->names = realloc(ns->names, sizeof(char *) * ns->cap);
        if (!ns->names) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    ns->names[ns->count++] = (char *)name;
}

/* --- codegen context ----------------------------------------------------- */

typedef struct {
    SB body;
    int indent;
    Names *globals;
    Names locals;   /* empty at top level (top-level names *are* globals) */
    bool in_func;
    int ntemps;     /* temporaries currently live */
    int max_temps;
} Cg;

static void emit(Cg *cg, const char *fmt, ...) {
    for (int i = 0; i < cg->indent; i++) sb_printf(&cg->body, "    ");
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_grow(&cg->body, (size_t)n);
    vsnprintf(cg->body.buf + cg->body.len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    cg->body.len += (size_t)n;
    sb_printf(&cg->body, "\n");
}

static int new_temp(Cg *cg) {
    int slot = (int)cg->locals.count + cg->ntemps++;
    if (cg->ntemps > cg->max_temps) cg->max_temps = cg->ntemps;
    return slot;
}

/* resolve a variable name to a slot (assumes the checker validated names) */
static int name_slot(Cg *cg, const char *name) {
    if (cg->in_func) {
        int i = names_find(&cg->locals, name);
        if (i >= 0) return i;
    }
    int g = names_find(cg->globals, name);
    if (g >= 0) return -g - 1;
    fprintf(stderr, "emeraldc: internal error: unresolved name '%s'\n", name);
    exit(1);
}

static const char *slotref(int slot, char buf[32]) {
    if (slot >= 0) snprintf(buf, 32, "F[%d]", slot);
    else snprintf(buf, 32, "G[%d]", -slot - 1);
    return buf;
}

/* emit a C string literal for arbitrary bytes */
static void sb_c_string(SB *sb, const char *s) {
    sb_printf(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  sb_printf(sb, "\\\""); break;
        case '\\': sb_printf(sb, "\\\\"); break;
        case '\n': sb_printf(sb, "\\n"); break;
        case '\t': sb_printf(sb, "\\t"); break;
        case '\r': sb_printf(sb, "\\r"); break;
        default:
            if (*p < 32 || *p > 126) sb_printf(sb, "\\%03o", *p);
            else sb_printf(sb, "%c", *p);
        }
    }
    sb_printf(sb, "\"");
}

/* --- expressions --------------------------------------------------------- */

static int gen_expr(Cg *cg, const Expr *e);

static const char *binop_fn(BinOp op) {
    switch (op) {
    case B_ADD: return "em_add"; case B_SUB: return "em_sub";
    case B_MUL: return "em_mul"; case B_DIV: return "em_div";
    case B_MOD: return "em_mod";
    case B_EQ: return "em_eq";   case B_NE: return "em_ne";
    case B_LT: return "em_lt";   case B_LE: return "em_le";
    case B_GT: return "em_gt";   case B_GE: return "em_ge";
    default: return "?";
    }
}

static int gen_call(Cg *cg, const Expr *e) {
    const char *name = e->as.call.fn->as.sval;
    size_t argc = e->as.call.count;
    int *args = malloc(sizeof(int) * (argc ? argc : 1));
    if (!args) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    for (size_t i = 0; i < argc; i++)
        args[i] = gen_expr(cg, e->as.call.args[i]);

    int t = new_temp(cg);
    char tb[32], ab[32];
    SB call = {0};

    if (strcmp(name, "print") == 0) {
        sb_printf(&call, "em_print(%zu", argc);
        for (size_t i = 0; i < argc; i++)
            sb_printf(&call, ", %s", slotref(args[i], ab));
        sb_printf(&call, ");");
        emit(cg, "%s", call.buf);
        emit(cg, "%s = em_none();", slotref(t, tb));
    } else if (strcmp(name, "len") == 0) {
        emit(cg, "%s = em_len(%s);", slotref(t, tb), slotref(args[0], ab));
    } else if (strcmp(name, "str") == 0) {
        emit(cg, "%s = em_str(%s);", slotref(t, tb), slotref(args[0], ab));
    } else if (strcmp(name, "int") == 0) {
        emit(cg, "%s = em_int_of(%s);", slotref(t, tb), slotref(args[0], ab));
    } else if (strcmp(name, "range") == 0) {
        if (argc == 1) {
            emit(cg, "%s = em_range(em_int(0), %s);", slotref(t, tb),
                 slotref(args[0], ab));
        } else {
            char bb[32];
            emit(cg, "%s = em_range(%s, %s);", slotref(t, tb),
                 slotref(args[0], ab), slotref(args[1], bb));
        }
    } else {
        sb_printf(&call, "%s = emf_%s(", slotref(t, tb), name);
        for (size_t i = 0; i < argc; i++)
            sb_printf(&call, "%s%s", i ? ", " : "", slotref(args[i], ab));
        sb_printf(&call, ");");
        emit(cg, "%s", call.buf);
    }
    free(call.buf);
    free(args);
    return t;
}

static int gen_expr(Cg *cg, const Expr *e) {
    char tb[32], ab[32], bb[32];
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
    case E_NAME:
        return name_slot(cg, e->as.sval);
    case E_LIST: {
        size_t n = e->as.list.count;
        int *items = malloc(sizeof(int) * (n ? n : 1));
        if (!items) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
        for (size_t i = 0; i < n; i++)
            items[i] = gen_expr(cg, e->as.list.items[i]);
        int t = new_temp(cg);
        SB call = {0};
        sb_printf(&call, "%s = em_list_litn(%zu", slotref(t, tb), n);
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
        int *vals = malloc(sizeof(int) * (n ? n : 1));
        if (!vals) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
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
    case E_BINOP: {
        BinOp op = e->as.bin.op;
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
    case E_CALL:
        return gen_call(cg, e);
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

/* --- statements ---------------------------------------------------------- */

static void gen_block(Cg *cg, const Block *b);

static void gen_if_arms(Cg *cg, const Stmt *s, size_t arm) {
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

static void gen_stmt(Cg *cg, const Stmt *s) {
    int mark = cg->ntemps; /* release this statement's temporaries at the end */
    char ab[32], bb[32], cb[32];

    switch (s->kind) {
    case S_EXPR:
        gen_expr(cg, s->as.expr);
        break;
    case S_ASSIGN: {
        const Expr *tgt = s->as.assign.target;
        if (tgt->kind == E_NAME) {
            int v = gen_expr(cg, s->as.assign.value);
            int dst = name_slot(cg, tgt->as.sval);
            if (dst != v)
                emit(cg, "%s = %s;", slotref(dst, ab), slotref(v, bb));
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
        int var = name_slot(cg, s->as.fr.var);
        emit(cg, "%s = em_int(0);", slotref(idx, ab));
        emit(cg, "for (;;) {");
        cg->indent++;
        emit(cg, "Value __it;");
        emit(cg, "if (!rt_iter_get(%s, %s.as.i, &__it)) break;",
             slotref(seq, ab), slotref(idx, bb));
        emit(cg, "%s.as.i += 1;", slotref(idx, ab));
        emit(cg, "%s = __it;", slotref(var, cb));
        gen_block(cg, &s->as.fr.body);
        cg->indent--;
        emit(cg, "}");
        break;
    }
    case S_RETURN: {
        if (s->as.ret) {
            int v = gen_expr(cg, s->as.ret);
            emit(cg, "{ Value __r = %s; rt_pop_frame(); return __r; }",
                 slotref(v, ab));
        } else {
            emit(cg, "{ rt_pop_frame(); return em_none(); }");
        }
        break;
    }
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
    case S_TYPEDEF:
        break; /* handled at program level / compile-time only */
    }
    cg->ntemps = mark;
}

static void gen_block(Cg *cg, const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        gen_stmt(cg, b->items[i]);
}

/* --- program ------------------------------------------------------------- */

static void collect_name_cb(const char *name, int line, void *ud) {
    (void)line;
    names_add((Names *)ud, name);
}

typedef struct { Names *locals; Names *globals; } LocalCollect;

static void collect_local_cb(const char *name, int line, void *ud) {
    (void)line;
    LocalCollect *lc = ud;
    /* Python-ish rule from the docs: assigning a name that already exists as
     * a global updates the global rather than creating a local. */
    if (names_find(lc->globals, name) < 0)
        names_add(lc->locals, name);
}

static void gen_function(FILE *out, const Stmt *s, Names *globals) {
    Cg cg;
    memset(&cg, 0, sizeof(cg));
    cg.globals = globals;
    cg.in_func = true;
    cg.indent = 1;

    for (size_t i = 0; i < s->as.func.param_count; i++)
        names_add(&cg.locals, s->as.func.params[i]);
    LocalCollect lc = { &cg.locals, globals };
    ast_collect_assigned(&s->as.func.body, collect_local_cb, &lc);

    gen_block(&cg, &s->as.func.body);

    int nslots = (int)cg.locals.count + cg.max_temps;
    if (nslots < 1) nslots = 1;

    fprintf(out, "static Value emf_%s(", s->as.func.name);
    for (size_t i = 0; i < s->as.func.param_count; i++)
        fprintf(out, "%sValue __p%zu", i ? ", " : "", i);
    if (s->as.func.param_count == 0) fprintf(out, "void");
    fprintf(out, ") {\n");
    fprintf(out, "    Value F[%d];\n", nslots);
    fprintf(out, "    RootFrame __fr;\n");
    fprintf(out, "    for (int __i = 0; __i < %d; __i++) F[__i] = em_none();\n",
            nslots);
    for (size_t i = 0; i < s->as.func.param_count; i++)
        fprintf(out, "    F[%zu] = __p%zu;\n", i, i);
    fprintf(out, "    rt_push_frame(&__fr, F, %d);\n", nslots);
    fwrite(cg.body.buf ? cg.body.buf : "", 1, cg.body.len, out);
    fprintf(out, "    rt_pop_frame();\n");
    fprintf(out, "    return em_none();\n");
    fprintf(out, "}\n\n");
    free(cg.body.buf);
    free(cg.locals.names);
}

void codegen_program(FILE *out, const Program *prog) {
    Names globals = {0};
    ast_collect_assigned(&prog->body, collect_name_cb, &globals);

    fprintf(out, "/* generated by emeraldc; compile with src/runtime.c */\n");
    fprintf(out, "#include \"runtime.h\"\n\n");

    size_t nglobals = globals.count ? globals.count : 1;
    fprintf(out, "static Value G[%zu];\n\n", nglobals);

    /* prototypes first so functions can be mutually recursive */
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_FUNC) continue;
        fprintf(out, "static Value emf_%s(", s->as.func.name);
        for (size_t j = 0; j < s->as.func.param_count; j++)
            fprintf(out, "%sValue", j ? ", " : "");
        if (s->as.func.param_count == 0) fprintf(out, "void");
        fprintf(out, ");\n");
    }
    fprintf(out, "\n");

    for (size_t i = 0; i < prog->body.count; i++)
        if (prog->body.items[i]->kind == S_FUNC)
            gen_function(out, prog->body.items[i], &globals);

    /* top level: names are globals (G); temporaries live in a local F */
    Cg cg;
    memset(&cg, 0, sizeof(cg));
    cg.globals = &globals;
    cg.in_func = false;
    cg.indent = 1;
    gen_block(&cg, &prog->body);

    int ntemps = cg.max_temps < 1 ? 1 : cg.max_temps;
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    rt_init();\n");
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
