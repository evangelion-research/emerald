/* Codegen: the string builder, name tables, closure-conversion bookkeeping,
 * the Cg context, and name resolution. */
#include "codegen_internal.h"

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

void sb_printf(SB *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(sb, fmt, ap);
    va_end(ap);
}

int names_find(const Names *ns, const char *name) {
    for (size_t i = 0; i < ns->count; i++)
        if (strcmp(ns->names[i], name) == 0) return (int)i;
    return -1;
}

void names_add_file(Names *ns, const char *name, const char *file) {
    if (names_find(ns, name) >= 0) return;
    if (ns->count == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 8;
        ns->names = xrealloc(ns->names, sizeof(char *) * ns->cap);
        ns->files = xrealloc(ns->files, sizeof(char *) * ns->cap);
    }
    ns->files[ns->count] = file;
    ns->names[ns->count++] = (char *)name;
}

void names_add(Names *ns, const char *name) {
    names_add_file(ns, name, NULL);
}

/* Would an assignment in `file` update the global `name`, or shadow it with a
 * local? Only the module that declared a global may update it: see the same
 * rule, and the reasoning, in check.c's updatable_global(). */
bool global_owned_by(const Names *globals, const char *name,
                            const char *file) {
    int i = names_find(globals, name);
    if (i < 0) return false;
    if (!globals->files[i] || !file) return true;
    return strcmp(globals->files[i], file) == 0;
}

const Builtin cg_builtins[] = {
#define EM_BUILTIN(n, c, a, p)      { n, c, a, true },
#define EM_BUILTIN_VOID(n, c, a, p) { n, c, a, false },
#include "builtins.def"
#undef EM_BUILTIN
#undef EM_BUILTIN_VOID
};

const Builtin *cg_builtin_find(const char *name) {
    for (size_t i = 0; i < sizeof cg_builtins / sizeof *cg_builtins; i++)
        if (strcmp(cg_builtins[i].name, name) == 0) return &cg_builtins[i];
    return NULL;
}

int add_local(FuncInfo *fi, const char *name) {
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

void emit(Cg *cg, const char *fmt, ...) {
    for (int i = 0; i < cg->indent; i++) sb_printf(&cg->body, "    ");
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&cg->body, fmt, ap);
    va_end(ap);
    sb_printf(&cg->body, "\n");
}

int new_temp(Cg *cg) {
    int base = cg->fi ? (int)cg->fi->locals.count : 0;
    int slot = base + cg->ntemps++;
    if (cg->ntemps > cg->max_temps) cg->max_temps = cg->ntemps;
    return slot;
}

const char *slotref(int slot, char buf[32]) {
    if (slot >= 0) snprintf(buf, 32, "F[%d]", slot);
    else snprintf(buf, 32, "G[%d]", -slot - 1);
    return buf;
}

/* emit a C string literal for arbitrary bytes */
void sb_c_string(SB *sb, const char *s) {
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

/* Classify a variable name in the current function scope. On success sets
 * `kind` and `slot` (F[] index for locals/cells, __env index for captures,
 * and the negative G[] encoding for globals) and returns true. */
bool var_slot(Cg *cg, const char *name, Access *kind, int *slot) {
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

FuncInfo *find_top_func(Cg *cg, const char *name) {
    for (size_t i = 0; i < cg->top_count; i++)
        if (strcmp(cg->top[i]->name, name) == 0) return cg->top[i];
    return NULL;
}

/* Emit a read of variable `name`, returning the slot holding its value. */
int gen_var_read(Cg *cg, const char *name) {
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
void gen_var_write(Cg *cg, const char *name, const char *val) {
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
const char *cellref(Cg *cg, const char *name, char buf[32]) {
    int i = names_find(&cg->fi->locals, name);
    if (i >= 0) { snprintf(buf, 32, "F[%d]", i); return buf; }
    i = names_find(&cg->fi->captures, name);
    if (i >= 0) { snprintf(buf, 32, "__env[%d]", i); return buf; }
    snprintf(buf, 32, "em_none()"); /* unreachable */
    return buf;
}
