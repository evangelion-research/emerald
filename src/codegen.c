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
#include "xalloc.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* --- string builder ------------------------------------------------------ */

typedef struct { char *buf; size_t len, cap; } SB;

static void sb_grow(SB *sb, size_t need) {
    if (sb->len + need + 1 <= sb->cap) return;
    sb->cap = sb->cap ? sb->cap * 2 : 256;
    while (sb->cap < sb->len + need + 1) sb->cap *= 2;
    sb->buf = xrealloc(sb->buf, sb->cap);
}

static void sb_vprintf(SB *sb, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    sb_grow(sb, (size_t)n);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

static void sb_printf(SB *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(sb, fmt, ap);
    va_end(ap);
}

/* --- name tables --------------------------------------------------------- */

typedef struct {
    char **names;
    const char **files; /* declaring module, for the global table; else NULL */
    size_t count, cap;
} Names;

static int names_find(const Names *ns, const char *name) {
    for (size_t i = 0; i < ns->count; i++)
        if (strcmp(ns->names[i], name) == 0) return (int)i;
    return -1;
}

static void names_add_file(Names *ns, const char *name, const char *file) {
    if (names_find(ns, name) >= 0) return;
    if (ns->count == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 8;
        ns->names = xrealloc(ns->names, sizeof(char *) * ns->cap);
        ns->files = xrealloc(ns->files, sizeof(char *) * ns->cap);
    }
    ns->files[ns->count] = file;
    ns->names[ns->count++] = (char *)name;
}

static void names_add(Names *ns, const char *name) {
    names_add_file(ns, name, NULL);
}

/* Would an assignment in `file` update the global `name`, or shadow it with a
 * local? Only the module that declared a global may update it: see the same
 * rule, and the reasoning, in check.c's updatable_global(). */
static bool global_owned_by(const Names *globals, const char *name,
                            const char *file) {
    int i = names_find(globals, name);
    if (i < 0) return false;
    if (!globals->files[i] || !file) return true;
    return strcmp(globals->files[i], file) == 0;
}

/* the shared builtin table (include/builtins.def) */
typedef struct {
    const char *name;
    const char *cfn;  /* runtime entry point; NULL = lowered specially below */
    int arity;
    bool rets;        /* the C function returns a Value */
} Builtin;

static const Builtin builtins[] = {
#define EM_BUILTIN(n, c, a, p)      { n, c, a, true },
#define EM_BUILTIN_VOID(n, c, a, p) { n, c, a, false },
#include "builtins.def"
#undef EM_BUILTIN
#undef EM_BUILTIN_VOID
};

static const Builtin *builtin_find(const char *name) {
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++)
        if (strcmp(builtins[i].name, name) == 0) return &builtins[i];
    return NULL;
}


/* --- function info (closure conversion) ---------------------------------- */

typedef struct FuncInfo FuncInfo;
struct FuncInfo {
    char *cname;         /* mangled C name, e.g. "emf_foo_3" */
    const char *name;    /* source-level name */
    const Stmt *node;
    const Expr *lamb;    /* non-NULL when this is a lambda (E_LAMBDA) */
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
        fi->locals.names = xrealloc(fi->locals.names,
                                   sizeof(char *) * fi->locals.cap);
        fi->local_cell = xrealloc(fi->local_cell,
                                 sizeof(bool) * fi->locals.cap);
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
    FuncInfo *fi;        /* current function (the top-level root at main) */
    FuncInfo **top;      /* top-level functions (for direct calls / values) */
    size_t top_count;
    int ntemps;          /* temporaries currently live */
    int max_temps;
    bool in_tco;         /* function body is wrapped in a tail-call loop */
    int last_line;       /* last emitted source line (for rt_cur_line) */
    const char *last_file; /* last emitted source file (linked programs span several) */
} Cg;

static void emit(Cg *cg, const char *fmt, ...) {
    for (int i = 0; i < cg->indent; i++) sb_printf(&cg->body, "    ");
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&cg->body, fmt, ap);
    va_end(ap);
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
    int *args = xmalloc(sizeof(int) * argc);
    for (size_t i = 0; i < argc; i++)
        args[i] = gen_expr(cg, e->as.call.args[i]);

    int t = new_temp(cg);
    char tb[32], ab[32], bb[32];
    SB call = {0};

    const Expr *fn = e->as.call.fn;
    if (fn->kind == E_NAME) {
        const char *name = fn->as.sval;
        const Builtin *b = builtin_find(name);
        if (b && b->cfn) {
            /* fixed-arity builtins all lower the same way: call the runtime
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
        int *items = xmalloc(sizeof(int) * n);
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
            emit(cg, "%s = em_compose(%s, %s);", slotref(t, tb),
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
            emit(cg, "%s = em_mkclosure(%s_tramp, %zu, NULL, 0);",
                 slotref(t, tb), child->cname, arity);
        } else {
            emit(cg, "{ Value __cap[%zu];", n);
            for (size_t k = 0; k < n; k++) {
                char cb[32];
                emit(cg, "  __cap[%zu] = %s;", k,
                     cellref(cg, child->captures.names[k], cb));
            }
            emit(cg, "  %s = em_mkclosure(%s_tramp, %zu, __cap, %zu);",
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

static void gen_block(Cg *cg, const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        gen_stmt(cg, b->items[i]);
}

/* --- closure analysis ---------------------------------------------------- */

typedef struct {
    Names *globals;
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
    switch (e->kind) {
    case E_LAMBDA:
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *lams = xrealloc(*lams, sizeof(Expr *) * *cap);
        }
        (*lams)[(*count)++] = (Expr *)e;
        collect_lambdas_expr(e->as.lam.body, lams, count, cap);
        break;
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            collect_lambdas_expr(e->as.list.items[i], lams, count, cap);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            collect_lambdas_expr(e->as.rec.values[i], lams, count, cap);
        break;
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

static void collect_match_pats_stmt(const Stmt *s,
                                    void (*fn)(const char *, const char *,
                                               int, void *),
                                    void *ud);

static void collect_match_pats_block(const Block *b,
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
        for (size_t j = 0; j < s->as.mtch.count; j++) {
            pat_bind_names(s->as.mtch.pats[j], fn, s->file, ud);
            collect_match_pats_block(&s->as.mtch.blocks[j], fn, ud);
        }
        break;
    case S_IF:
        for (size_t j = 0; j < s->as.ifs.count; j++)
            collect_match_pats_block(&s->as.ifs.blocks[j], fn, ud);
        if (s->as.ifs.has_else)
            collect_match_pats_block(&s->as.ifs.else_block, fn, ud);
        break;
    case S_WHILE:
        collect_match_pats_block(&s->as.wh.body, fn, ud);
        break;
    case S_FOR:
        collect_match_pats_block(&s->as.fr.body, fn, ud);
        break;
    case S_BLOCK:
        collect_match_pats_block(&s->as.block, fn, ud);
        break;
    default:
        break; /* S_FUNC: nested bodies are separate scopes */
    }
}

typedef struct { Names *globals; FuncInfo *fi; } LocalCtx;

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

static FuncInfo *build_func(Ctx *ctx, const Stmt *node, FuncInfo *parent);
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
static FuncInfo *make_top_root(Ctx *ctx, const Block *body) {
    FuncInfo *fi = xcalloc(1, sizeof(FuncInfo));
    fi->parent = NULL;
    fi->name = "<top>";
    fi->node = NULL;
    Expr **lams = NULL;
    size_t n = 0, cap = 0;
    collect_lambdas_block(body, &lams, &n, &cap);
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

static FuncInfo *build_func(Ctx *ctx, const Stmt *node, FuncInfo *parent) {
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

static void gen_function(FILE *out, FuncInfo *fi, Names *globals,
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

    int ntemps = cg.max_temps < 1 ? 1 : cg.max_temps;
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
