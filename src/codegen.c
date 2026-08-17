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
 *
 * Functions are first-class. A top-level function is called directly
 * (`emf_name_0(NULL, ...)`) and boxed as a closure when used as a value.
 * Nested `def`s become local bindings whose value is a closure; a variable
 * referenced by a nested function (a "capture") is stored in a heap cell
 * (`O_CELL`) shared by the defining scope and every closure that captures it,
 * so mutations are visible across the closure boundary.
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

static bool is_builtin(const char *name) {
    return strcmp(name, "print") == 0 || strcmp(name, "len") == 0 ||
           strcmp(name, "str") == 0 || strcmp(name, "int") == 0 ||
           strcmp(name, "range") == 0 || strcmp(name, "gc_stats") == 0 ||
           strcmp(name, "read_file") == 0 || strcmp(name, "write_file") == 0 ||
           strcmp(name, "run") == 0 || strcmp(name, "sqrt") == 0 ||
           strcmp(name, "tan") == 0 || strcmp(name, "rand") == 0;
}

/* --- function info (closure conversion) ---------------------------------- */

typedef struct FuncInfo FuncInfo;
struct FuncInfo {
    char *cname;         /* mangled C name, e.g. "emf_foo_3" */
    const char *name;    /* source-level name */
    const Stmt *node;
    FuncInfo *parent;
    FuncInfo **children; size_t child_count, child_cap;

    Names locals;        /* F[] slot order: params, assigned locals, nested fns */
    bool *local_cell;    /* parallel to locals.names: stored as a GC cell */
    Names captures;      /* free variables, in __env order */
};

static int add_local(FuncInfo *fi, const char *name) {
    int i = names_find(&fi->locals, name);
    if (i >= 0) return i;
    if (fi->locals.count == fi->locals.cap) {
        fi->locals.cap = fi->locals.cap ? fi->locals.cap * 2 : 8;
        fi->locals.names = realloc(fi->locals.names,
                                   sizeof(char *) * fi->locals.cap);
        fi->local_cell = realloc(fi->local_cell,
                                 sizeof(bool) * fi->locals.cap);
        if (!fi->locals.names || !fi->local_cell) {
            fputs("emeraldc: out of memory\n", stderr);
            exit(1);
        }
    }
    fi->locals.names[fi->locals.count] = (char *)name;
    fi->local_cell[fi->locals.count] = false;
    return (int)fi->locals.count++;
}

/* --- codegen context ----------------------------------------------------- */

typedef struct {
    SB body;
    int indent;
    Names *globals;
    FuncInfo *fi;        /* current function; NULL at top level */
    FuncInfo **top;      /* top-level functions (for direct calls / values) */
    size_t top_count;
    int ntemps;          /* temporaries currently live */
    int max_temps;
    int last_line;       /* last emitted source line (for rt_cur_line) */
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
    int base = cg->fi ? (int)cg->fi->locals.count : 0;
    int slot = base + cg->ntemps++;
    if (cg->ntemps > cg->max_temps) cg->max_temps = cg->ntemps;
    return slot;
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

/* --- name resolution ----------------------------------------------------- */

typedef enum { A_LOCAL, A_CELL, A_CAPTURED, A_GLOBAL } Access;

/* Classify a variable name in the current function scope. On success sets
 * `kind` and `slot` (F[] index for locals/cells, __env index for captures,
 * and the negative G[] encoding for globals) and returns true. */
static bool var_slot(Cg *cg, const char *name, Access *kind, int *slot) {
    if (cg->fi) {
        int i = names_find(&cg->fi->locals, name);
        if (i >= 0) {
            *kind = cg->fi->local_cell[i] ? A_CELL : A_LOCAL;
            *slot = i;
            return true;
        }
        i = names_find(&cg->fi->captures, name);
        if (i >= 0) { *kind = A_CAPTURED; *slot = i; return true; }
    }
    int g = names_find(cg->globals, name);
    if (g >= 0) { *kind = A_GLOBAL; *slot = -g - 1; return true; }
    return false;
}

static FuncInfo *find_top_func(Cg *cg, const char *name) {
    for (size_t i = 0; i < cg->top_count; i++)
        if (strcmp(cg->top[i]->name, name) == 0) return cg->top[i];
    return NULL;
}

/* Emit a read of variable `name`, returning the slot holding its value. */
static int gen_var_read(Cg *cg, const char *name) {
    Access kind;
    int slot;
    if (!var_slot(cg, name, &kind, &slot)) {
        fprintf(stderr, "emeraldc: internal error: unresolved name '%s'\n", name);
        exit(1);
    }
    if (kind == A_LOCAL || kind == A_GLOBAL)
        return slot; /* the value already lives in that slot */
    int t = new_temp(cg);
    char tb[32], ab[32];
    if (kind == A_CELL)
        emit(cg, "%s = em_cell_get(%s);", slotref(t, tb), slotref(slot, ab));
    else {
        char eb[32];
        snprintf(eb, 32, "__env[%d]", slot);
        emit(cg, "%s = em_cell_get(%s);", slotref(t, tb), eb);
    }
    return t;
}

/* Emit a write of C expression `val` into variable `name`. */
static void gen_var_write(Cg *cg, const char *name, const char *val) {
    Access kind;
    int slot;
    if (!var_slot(cg, name, &kind, &slot)) {
        fprintf(stderr, "emeraldc: internal error: unresolved name '%s'\n", name);
        exit(1);
    }
    char ab[32];
    switch (kind) {
    case A_LOCAL:  emit(cg, "%s = %s;", slotref(slot, ab), val); break;
    case A_CELL:   emit(cg, "em_cell_set(%s, %s);", slotref(slot, ab), val); break;
    case A_GLOBAL: emit(cg, "%s = %s;", slotref(slot, ab), val); break;
    case A_CAPTURED: {
        char eb[32];
        snprintf(eb, 32, "__env[%d]", slot);
        emit(cg, "em_cell_set(%s, %s);", eb, val);
        break;
    }
    }
}

/* A C expression referencing the *cell* holding `name` in the current scope,
 * used to build a closure env. `name` is always a capture of a nested def and
 * therefore already boxed (or already captured) in this scope. */
static const char *cellref(Cg *cg, const char *name, char buf[32]) {
    int i = names_find(&cg->fi->locals, name);
    if (i >= 0) { snprintf(buf, 32, "F[%d]", i); return buf; }
    i = names_find(&cg->fi->captures, name);
    if (i >= 0) { snprintf(buf, 32, "__env[%d]", i); return buf; }
    snprintf(buf, 32, "em_none()"); /* unreachable */
    return buf;
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
    size_t argc = e->as.call.count;
    int *args = malloc(sizeof(int) * (argc ? argc : 1));
    if (!args) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    for (size_t i = 0; i < argc; i++)
        args[i] = gen_expr(cg, e->as.call.args[i]);

    int t = new_temp(cg);
    char tb[32], ab[32], bb[32];
    SB call = {0};

    const Expr *fn = e->as.call.fn;
    if (fn->kind == E_NAME) {
        const char *name = fn->as.sval;
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
            if (argc == 1)
                emit(cg, "%s = em_range(em_int(0), %s);", slotref(t, tb),
                     slotref(args[0], ab));
            else
                emit(cg, "%s = em_range(%s, %s);", slotref(t, tb),
                     slotref(args[0], ab), slotref(args[1], bb));
        } else if (strcmp(name, "gc_stats") == 0) {
            emit(cg, "%s = em_gc_stats();", slotref(t, tb));
        } else if (strcmp(name, "read_file") == 0) {
            emit(cg, "%s = em_read_file(%s);", slotref(t, tb), slotref(args[0], ab));
        } else if (strcmp(name, "write_file") == 0) {
            emit(cg, "em_write_file(%s, %s);", slotref(args[0], ab),
                 slotref(args[1], bb));
            emit(cg, "%s = em_none();", slotref(t, tb));
        } else if (strcmp(name, "append_file") == 0) {
            emit(cg, "em_append_file(%s, %s);", slotref(args[0], ab),
                 slotref(args[1], bb));
            emit(cg, "%s = em_none();", slotref(t, tb));
        } else if (strcmp(name, "run") == 0) {
            emit(cg, "%s = em_run(%s);", slotref(t, tb), slotref(args[0], ab));
        } else if (strcmp(name, "sqrt") == 0) {
            emit(cg, "%s = em_sqrt(%s);", slotref(t, tb), slotref(args[0], ab));
        } else if (strcmp(name, "tan") == 0) {
            emit(cg, "%s = em_tan(%s);", slotref(t, tb), slotref(args[0], ab));
        } else if (strcmp(name, "rand") == 0) {
            emit(cg, "%s = em_rand();", slotref(t, tb));
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
                    /* direct call to a top-level function (env is always NULL) */
                    sb_printf(&call, "%s = %s(NULL", slotref(t, tb), tf->cname);
                    for (size_t i = 0; i < argc; i++)
                        sb_printf(&call, ", %s", slotref(args[i], ab));
                    sb_printf(&call, ");");
                    emit(cg, "%s", call.buf);
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
    case E_NAME: {
        Access kind;
        int slot;
        if (var_slot(cg, e->as.sval, &kind, &slot))
            return gen_var_read(cg, e->as.sval);
        FuncInfo *tf = find_top_func(cg, e->as.sval);
        if (tf) {
            int t = new_temp(cg);
            emit(cg, "%s = em_mkclosure(%s_tramp, %zu, NULL, 0);",
                 slotref(t, tb), tf->cname, tf->node->as.func.param_count);
            return t;
        }
        fprintf(stderr, "emeraldc: internal error: unresolved name '%s'\n",
                e->as.sval);
        exit(1);
    }
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
    size_t arity = s->as.func.param_count;
    char ab[32];

    if (n == 0) {
        if (cell)
            emit(cg, "em_cell_set(%s, em_mkclosure(%s_tramp, %zu, NULL, 0));",
                 slotref(slot, ab), child->cname, arity);
        else
            emit(cg, "%s = em_mkclosure(%s_tramp, %zu, NULL, 0);",
                 slotref(slot, ab), child->cname, arity);
    } else {
        emit(cg, "{ Value __cap[%zu];", n);
        for (size_t k = 0; k < n; k++) {
            char cb[32];
            emit(cg, "  __cap[%zu] = %s;", k,
                 cellref(cg, child->captures.names[k], cb));
        }
        if (cell)
            emit(cg, "  em_cell_set(%s, em_mkclosure(%s_tramp, %zu, __cap, %zu));",
                 slotref(slot, ab), child->cname, arity, n);
        else
            emit(cg, "  %s = em_mkclosure(%s_tramp, %zu, __cap, %zu);",
                 slotref(slot, ab), child->cname, arity, n);
        emit(cg, "}");
    }
}

static void gen_stmt(Cg *cg, const Stmt *s) {
    int mark = cg->ntemps; /* release this statement's temporaries at the end */
    char ab[32], bb[32], cb[32];

    /* track the current source line so runtime errors report a location */
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
        if (cg->fi)
            gen_nested_def(cg, s);
        break; /* top-level bodies are emitted separately */
    case S_TYPEDEF:
        break; /* compile-time only */
    }
    cg->ntemps = mark;
}

static void gen_block(Cg *cg, const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        gen_stmt(cg, b->items[i]);
}

/* --- closure analysis ---------------------------------------------------- */

typedef struct {
    Names *globals;
    Names top_names;
    int counter;
    FuncInfo **all;      /* every function, in build order */
    size_t all_count, all_cap;
} Ctx;

static void collect_used_expr(const Expr *e, Names *out);
static void collect_used_block(const Block *b, Names *out);

static void collect_used_expr(const Expr *e, Names *out) {
    switch (e->kind) {
    case E_NAME:
        names_add(out, e->as.sval);
        break;
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            collect_used_expr(e->as.list.items[i], out);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            collect_used_expr(e->as.rec.values[i], out);
        break;
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
                *defs = realloc(*defs, sizeof(Stmt *) * *cap);
                if (!*defs) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
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
        default:
            break;
        }
    }
}

static void collect_local_cb(const char *name, int line, void *ud);

typedef struct { Names *globals; FuncInfo *fi; } LocalCtx;

static void collect_local_cb(const char *name, int line, void *ud) {
    (void)line;
    LocalCtx *lc = ud;
    /* the docs rule: assigning a global's name updates the global */
    if (names_find(lc->globals, name) < 0)
        add_local(lc->fi, name);
}

/* does `name` resolve to a local of some enclosing function? */
static bool bound_in_ancestor(FuncInfo *fi, const char *name) {
    for (FuncInfo *f = fi->parent; f; f = f->parent)
        if (names_find(&f->locals, name) >= 0) return true;
    return false;
}

static void compute_captures(Ctx *ctx, FuncInfo *fi) {
    Names used = {0};
    collect_used_block(&fi->node->as.func.body, &used);
    for (size_t i = 0; i < used.count; i++) {
        const char *n = used.names[i];
        if (names_find(&fi->locals, n) >= 0) continue;   /* bound here */
        if (bound_in_ancestor(fi, n)) {                  /* enclosing local */
            names_add(&fi->captures, n);
            continue;
        }
        if (names_find(ctx->globals, n) >= 0) continue;  /* global */
        if (names_find(&ctx->top_names, n) >= 0) continue; /* top-level fn */
        if (is_builtin(n)) continue;
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

static void compute_boxing(FuncInfo *fi) {
    for (size_t c = 0; c < fi->child_count; c++) {
        FuncInfo *ch = fi->children[c];
        for (size_t i = 0; i < ch->captures.count; i++) {
            int idx = names_find(&fi->locals, ch->captures.names[i]);
            if (idx >= 0) fi->local_cell[idx] = true;
        }
    }
}

static FuncInfo *build_func(Ctx *ctx, const Stmt *node, FuncInfo *parent) {
    FuncInfo *fi = calloc(1, sizeof(FuncInfo));
    if (!fi) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
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
        ctx->all = realloc(ctx->all, sizeof(FuncInfo *) * ctx->all_cap);
        if (!ctx->all) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    ctx->all[ctx->all_count++] = fi;

    /* locals: params, then assigned names, then nested function names */
    for (size_t i = 0; i < node->as.func.param_count; i++)
        add_local(fi, node->as.func.params[i]);
    LocalCtx lc = { ctx->globals, fi };
    ast_collect_assigned(&node->as.func.body, collect_local_cb, &lc);

    Stmt **defs = NULL;
    size_t ndefs = 0, defcap = 0;
    collect_child_defs(&node->as.func.body, &defs, &ndefs, &defcap);
    for (size_t i = 0; i < ndefs; i++)
        add_local(fi, defs[i]->as.func.name);

    fi->child_cap = ndefs ? ndefs : 1;
    fi->children = calloc(fi->child_cap, sizeof(FuncInfo *));
    if (!fi->children) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    fi->child_count = ndefs;
    for (size_t i = 0; i < ndefs; i++)
        fi->children[i] = build_func(ctx, defs[i], fi);
    free(defs);

    compute_captures(ctx, fi);
    compute_boxing(fi);
    return fi;
}

/* --- program ------------------------------------------------------------- */

static void collect_name_cb(const char *name, int line, void *ud) {
    (void)line;
    names_add((Names *)ud, name);
}

static void gen_function(FILE *out, FuncInfo *fi, Names *globals,
                         FuncInfo **top, size_t top_count) {
    Cg cg;
    memset(&cg, 0, sizeof(cg));
    cg.globals = globals;
    cg.fi = fi;
    cg.top = top;
    cg.top_count = top_count;
    cg.indent = 1;

    gen_block(&cg, &fi->node->as.func.body);

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
    fprintf(out, "    rt_pop_frame();\n");
    fprintf(out, "    return em_none();\n");
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

    /* top-level function names */
    Names top_names = {0};
    for (size_t i = 0; i < prog->body.count; i++)
        if (prog->body.items[i]->kind == S_FUNC)
            names_add(&top_names, prog->body.items[i]->as.func.name);

    /* build the function tree (top-level defs are the root's children) */
    Ctx ctx = { &globals, top_names, 0, NULL, 0, 0 };
    FuncInfo **top = NULL;
    size_t top_count = 0, top_cap = 0;
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_FUNC) continue;
        if (top_count == top_cap) {
            top_cap = top_cap ? top_cap * 2 : 8;
            top = realloc(top, sizeof(FuncInfo *) * top_cap);
            if (!top) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
        }
        top[top_count++] = build_func(&ctx, s, NULL);
    }

    fprintf(out, "/* generated by emeraldc; compile with src/runtime.c */\n");
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

    /* top level: names are globals (G); temporaries live in a local F */
    Cg cg;
    memset(&cg, 0, sizeof(cg));
    cg.globals = &globals;
    cg.fi = NULL;
    cg.top = top;
    cg.top_count = top_count;
    cg.indent = 1;
    gen_block(&cg, &prog->body);

    int ntemps = cg.max_temps < 1 ? 1 : cg.max_temps;
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    rt_init();\n");
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
