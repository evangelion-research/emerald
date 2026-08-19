/* Emerald module loader and linker.
 *
 * The compiler proper knows nothing about modules: it type-checks and lowers
 * one flat Program. This file bridges the gap. It walks the import graph from
 * the entry file, parses every module once, and *links* the results into a
 * single Program whose statements are ordered dependencies-first.
 *
 * Linking is a rename pass. Each imported module gets a prefix derived from
 * its dotted path, and every reference to one of its top-level names — from
 * inside the module and from its importers — is rewritten to `<prefix>__<name>`.
 * `strings.split(x)` becomes a plain call to `strings__split`, so two packages
 * can each define `split` without colliding. The entry module is left alone,
 * so its diagnostics read exactly as they did before modules existed.
 *
 * Renaming has to respect scoping, or a local named `split` would be captured
 * by the rename. The rule mirrors the checker and codegen exactly: inside a
 * function, the names bound locally are its parameters, its nested `def`s, and
 * every assigned name that is *not* already a module-level global (assigning a
 * global's name updates the global). Those names are left untouched; anything
 * else that names a module-level definition is rewritten.
 *
 * Nothing is freed: like the rest of the compiler, this is a short-lived
 * process and the whole graph dies with it.
 */
#include "module.h"
#include "dim.h"
#include "parser.h"
#include "xalloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* sprintf into a fresh buffer */
static char *xasprintf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *out = xmalloc((size_t)n + 1);
    vsnprintf(out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

/* --- small name sets ----------------------------------------------------- */

typedef struct { const char **items; size_t count, cap; } Names;

static bool names_has(const Names *ns, const char *name) {
    for (size_t i = 0; i < ns->count; i++)
        if (strcmp(ns->items[i], name) == 0) return true;
    return false;
}

static void names_add(Names *ns, const char *name) {
    if (names_has(ns, name)) return;
    if (ns->count == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 8;
        ns->items = xrealloc(ns->items, sizeof(char *) * ns->cap);
    }
    ns->items[ns->count++] = name;
}

typedef struct { void **items; size_t count, cap; } PtrVec;

static void vec_push(PtrVec *v, void *item) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, sizeof(void *) * v->cap);
    }
    v->items[v->count++] = item;
}

/* --- paths --------------------------------------------------------------- */

static bool is_regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The directory containing `path`, as a fresh string ("." when there is none). */
static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    char *d = xmalloc((size_t)(slash - path) + 1);
    memcpy(d, path, (size_t)(slash - path));
    d[slash - path] = '\0';
    return d;
}

static char *path_join(const char *dir, const char *rest) {
    if (strcmp(dir, ".") == 0) return xstrdup(rest);
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '/') return xasprintf("%s%s", dir, rest);
    return xasprintf("%s/%s", dir, rest);
}

/* The project's `src/` root: starting at the entry file's directory, walk up
 * the *textual* path looking for a directory that contains `src`. Staying
 * textual (rather than resolving to an absolute path) keeps diagnostics
 * relative when the compiler is invoked with a relative entry path. */
static char *find_src_root(const char *entry_dir) {
    char *cur = xstrdup(entry_dir);
    for (;;) {
        char *cand = path_join(cur, "src");
        if (is_directory(cand)) return cand;
        free(cand);
        char *slash = strrchr(cur, '/');
        if (!slash) {
            if (strcmp(cur, ".") == 0 || strcmp(cur, "..") == 0) break;
            free(cur);
            cur = xstrdup(".");
            continue;
        }
        if (slash == cur) break; /* reached "/" */
        *slash = '\0';
    }
    free(cur);
    return NULL;
}

/* "text.strings" -> "text/strings" */
static char *dotted_to_slashes(const char *dotted) {
    char *out = xstrdup(dotted);
    for (char *p = out; *p; p++)
        if (*p == '.') *p = '/';
    return out;
}

/* "text.strings" -> "text_strings" (the codegen mangling prefix) */
static char *dotted_to_prefix(const char *dotted) {
    char *out = xstrdup(dotted);
    for (char *p = out; *p; p++)
        if (*p == '.') *p = '_';
    return out;
}

/* The canonical form of `path`, for deciding whether two spellings name the
 * same file. Falls back to the path itself when it cannot be resolved. */
static char *canonical(const char *path) {
    char buf[PATH_MAX];
    if (realpath(path, buf)) return xstrdup(buf);
    return xstrdup(path);
}

/* --- modules ------------------------------------------------------------- */

typedef enum { M_LOADING, M_DONE } ModState;

typedef struct Mod {
    char *dotted;      /* dotted module path; "" for the entry module */
    char *prefix;      /* mangling prefix, or NULL for the entry module */
    char *file;        /* path the module was loaded from, as written */
    char *key;         /* canonical path: module identity, so two spellings
                        * of one file (`lib.rald` from here, `../x/lib.rald`
                        * from there) load it once */
    char *dir;         /* directory of `file`, for relative resolution */
    char *src;         /* source text (borrowed by the diagnostics) */
    Program *prog;
    ModState state;
    Names globals;     /* top-level assigned names */
    Names defs;        /* top-level `def` names */
    Names types;       /* top-level `type` names */
    Names dims;        /* top-level `dim` names */
} Mod;

typedef struct {
    Mod **mods; size_t count, cap;
    const char *const *roots; size_t nroots;
    char *src_root;              /* project src/ root, or NULL */
    DiagList *diags;
    int errors;
    Mod **stack; size_t depth, stack_cap;  /* modules currently being loaded */
    PtrVec out;                  /* linked top-level statements */
} Loader;

static void ld_error(Loader *ld, const char *code, const char *file,
                     int line, int col, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_add(ld->diags, DIA_SYNTAX, code, file, line, col, "%s", msg);
    ld->errors++;
}

static char *read_file(Loader *ld, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    (void)ld;
    return buf;
}

/* Does this module export `name`? (Top-level defs, types, and globals.) */
static bool mod_exports(const Mod *m, const char *name) {
    return names_has(&m->defs, name) || names_has(&m->types, name) ||
           names_has(&m->globals, name) || names_has(&m->dims, name);
}

static bool is_private(const char *name) { return name[0] == '_'; }

static char *mangle(const Mod *m, const char *name) {
    if (!m->prefix) return (char *)name;
    return xasprintf("%s__%s", m->prefix, name);
}

static void collect_global_cb(const char *name, const char *file, int line,
                              void *ud) {
    (void)line; (void)file;
    names_add((Names *)ud, name);
}

/* Record the module's top-level names so importers (and the rename pass) know
 * what it exports. */
static void index_module(Mod *m) {
    ast_collect_assigned(&m->prog->body, collect_global_cb, &m->globals);
    for (size_t i = 0; i < m->prog->body.count; i++) {
        const Stmt *s = m->prog->body.items[i];
        if (s->kind == S_FUNC) names_add(&m->defs, s->as.func.name);
        else if (s->kind == S_TYPEDEF) names_add(&m->types, s->as.tdef.name);
        else if (s->kind == S_DIMDECL)
            for (size_t j = 0; j < s->as.dim.count; j++)
                names_add(&m->dims, s->as.dim.names[j]);
    }
}

/* --- resolution ---------------------------------------------------------- */

/* Find the file a dotted module path names under one search root. Sets
 * `*ambiguous` when the root offers both `a/b.rald` and `a.b.rald`. */
static char *resolve_in_root(const char *root, const char *dotted,
                             bool *ambiguous) {
    *ambiguous = false;
    char *slashed = dotted_to_slashes(dotted);
    char *nested = path_join(root, xasprintf("%s.rald", slashed));
    free(slashed);
    char *flat = path_join(root, xasprintf("%s.rald", dotted));

    bool has_nested = is_regular_file(nested);
    bool has_flat = strcmp(nested, flat) != 0 && is_regular_file(flat);
    if (has_nested && has_flat) { *ambiguous = true; return nested; }
    if (has_nested) return nested;
    if (has_flat) return flat;
    free(nested);
    free(flat);
    return NULL;
}

#ifndef EMERALD_STDLIB_DIR
#define EMERALD_STDLIB_DIR "stdlib"
#endif

/* The standard library's root, searched last so a project can shadow a stdlib
 * module with one of its own. $EMERALD_STDLIB overrides the built-in path the
 * same way $EMERALD_SRC overrides the runtime's; next a path relative to the
 * executable (set by main), then the compile-time default. */
static const char *exe_stdlib = NULL;

void module_set_exe_stdlib(const char *path) { exe_stdlib = path; }

static const char *stdlib_root(void) {
    const char *p = getenv("EMERALD_STDLIB");
    if (p && *p) return p;
    if (exe_stdlib) return exe_stdlib;
    return EMERALD_STDLIB_DIR;
}

/* Resolve a module path against, in order: the importing file's directory,
 * the project's src/ root, each -I root in the order given, then the stdlib.
 * First hit wins; a later root never shadows an earlier one. */
static char *resolve_module(Loader *ld, const char *dotted,
                            const char *importer_dir, const Stmt *site) {
    const char *roots[64];
    size_t nroots = 0;
    roots[nroots++] = importer_dir;
    if (ld->src_root && strcmp(ld->src_root, importer_dir) != 0)
        roots[nroots++] = ld->src_root;
    for (size_t i = 0; i < ld->nroots && nroots < 64; i++)
        roots[nroots++] = ld->roots[i];
    if (nroots < 64) roots[nroots++] = stdlib_root();

    for (size_t i = 0; i < nroots; i++) {
        bool ambiguous = false;
        char *hit = resolve_in_root(roots[i], dotted, &ambiguous);
        if (ambiguous) {
            char *slashed = dotted_to_slashes(dotted);
            ld_error(ld, "E_IMPORT_AMBIGUOUS", site->file, site->line, site->col,
                     "module '%s' is claimed by two files under '%s'",
                     dotted, roots[i]);
            Diag *d = &ld->diags->items[ld->diags->count - 1];
            diag_note(d, "candidate", xasprintf("%s/%s.rald", roots[i], slashed));
            diag_note(d, "candidate", xasprintf("%s/%s.rald", roots[i], dotted));
            free(slashed);
            return NULL;
        }
        if (hit) return hit;
    }

    ld_error(ld, "E_IMPORT_NOT_FOUND", site->file, site->line, site->col,
             "cannot find module '%s' on the search path", dotted);
    Diag *d = &ld->diags->items[ld->diags->count - 1];
    for (size_t i = 0; i < nroots; i++)
        /* the stdlib root is an absolute build-time path; naming it would make
         * every diagnostic machine-specific, so it reports as <stdlib> */
        diag_note(d, "searched",
                  roots[i] == stdlib_root() ? "<stdlib>" : roots[i]);
    return NULL;
}

/* --- the rename pass ----------------------------------------------------- */

/* One name a module's import statements bind locally. Exactly one of `mod`
 * (a module object, used as `m.f`) and `target` (a direct name import) is set. */
typedef struct {
    const char *local;
    Mod *mod;
    const char *target;
    const char *disp;   /* how the user spelled it, for diagnostics */
} Bind;

/* The names bound inside one function body; chains to the enclosing function. */
typedef struct RScope {
    Names bound;
    const struct RScope *parent;
} RScope;

typedef struct {
    Loader *ld;
    Mod *mod;
    Bind *binds; size_t nbinds;
    Names tvars;   /* generic type parameters currently in scope */
} RW;

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
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            rw_expr(rw, e->as.rec.values[i], sc);
        break;
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
            if (a->bind) names_add(&asc.bound, a->bind);
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
            names_add(&lsc.bound, e->as.lam.params[i]);
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
        names_add(out, p->bind);
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

static void rw_stmt(RW *rw, Stmt *s, const RScope *sc) {
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
            names_add(&rw->tvars, s->as.tdef.params[i]);
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
        case S_FUNC: names_add(out, s->as.func.name); break;
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

typedef struct { Names *bound; const Names *globals; } LocalCtx;

static void collect_local_cb(const char *name, const char *file, int line,
                             void *ud) {
    (void)line; (void)file;
    LocalCtx *lc = ud;
    /* assigning a module global's name updates the global, so it is not local */
    if (!names_has(lc->globals, name)) names_add(lc->bound, name);
}

static void rw_func(RW *rw, Stmt *s, const RScope *parent) {
    RScope sc;
    memset(&sc, 0, sizeof(sc));
    sc.parent = parent;
    for (size_t i = 0; i < s->as.func.param_count; i++)
        names_add(&sc.bound, s->as.func.params[i]);
    LocalCtx lc = { &sc.bound, &rw->mod->globals };
    ast_collect_assigned(&s->as.func.body, collect_local_cb, &lc);
    collect_nested_defs(&s->as.func.body, &sc.bound);

    size_t mark = rw->tvars.count;
    for (size_t i = 0; i < s->as.func.tparam_count; i++)
        names_add(&rw->tvars, s->as.func.tparams[i]);
    for (size_t i = 0; i < s->as.func.param_count; i++)
        rw_type(rw, s->as.func.param_types[i], &sc);
    rw_type(rw, s->as.func.ret_type, &sc);
    rw_block(rw, &s->as.func.body, &sc);
    rw->tvars.count = mark;

    /* a top-level def is a module-level name and gets the module's prefix */
    if (!parent && rw->mod->prefix)
        s->as.func.name = mangle(rw->mod, s->as.func.name);
    free(sc.bound.items);
}

/* --- loading ------------------------------------------------------------- */

static Mod *load_module(Loader *ld, const char *dotted, const char *file,
                        char *src, const Stmt *site);

/* An import binding that collides with one of this module's own top-level
 * names, or with an earlier import, would silently shadow one of the two.
 * Reject it instead. */
static void check_binding(Loader *ld, Mod *m, const PtrVec *sofar,
                          const char *local, const Stmt *site,
                          int line, int col) {
    if (mod_exports(m, local)) {
        ld_error(ld, "E_IMPORT_REDEFINE", site->file, line, col,
                 "import of '%s' collides with a top-level definition of the "
                 "same name in this module", local);
        return;
    }
    for (size_t i = 0; i < sofar->count; i++)
        if (strcmp(((Bind *)sofar->items[i])->local, local) == 0) {
            ld_error(ld, "E_IMPORT_REDEFINE", site->file, line, col,
                     "'%s' is already bound by an earlier import", local);
            return;
        }
}

/* Turn a module's `import` statements into local bindings, loading each
 * imported module first. */
static Bind *bind_imports(Loader *ld, Mod *m, size_t *out_count) {
    PtrVec binds = {0};
    for (size_t i = 0; i < m->prog->body.count; i++) {
        Stmt *s = m->prog->body.items[i];
        if (s->kind != S_IMPORT) continue;

        char *path = resolve_module(ld, s->as.imp.path, m->dir, s);
        if (!path) continue;
        char *src = read_file(ld, path);
        if (!src) {
            ld_error(ld, "E_IMPORT_NOT_FOUND", s->file, s->line, s->col,
                     "cannot read module file '%s'", path);
            continue;
        }
        Mod *dep = load_module(ld, s->as.imp.path, path, src, s);
        if (!dep) continue;

        if (!s->as.imp.is_from) {
            check_binding(ld, m, &binds, s->as.imp.alias, s, s->line, s->col);
            Bind *b = xmalloc(sizeof(Bind));
            b->local = s->as.imp.alias;
            b->mod = dep;
            b->target = NULL;
            b->disp = s->as.imp.alias;
            vec_push(&binds, b);
            continue;
        }
        for (size_t j = 0; j < s->as.imp.name_count; j++) {
            const ImportName *in = &s->as.imp.names[j];
            if (is_private(in->name))
                ld_error(ld, "E_IMPORT_PRIVATE", s->file, in->line, in->col,
                         "'%s' is private to module '%s' (names starting with "
                         "'_' are not exported)", in->name, dep->dotted);
            else if (!mod_exports(dep, in->name))
                ld_error(ld, "E_IMPORT_NAME", s->file, in->line, in->col,
                         "module '%s' has no member '%s'", dep->dotted, in->name);
            check_binding(ld, m, &binds, in->local, s, in->line, in->col);
            Bind *b = xmalloc(sizeof(Bind));
            b->local = in->local;
            b->mod = NULL;
            b->target = mangle(dep, in->name);
            b->disp = in->local;
            vec_push(&binds, b);
        }
    }

    /* flatten into a contiguous array */
    Bind *out = xmalloc(sizeof(Bind) * binds.count);
    for (size_t i = 0; i < binds.count; i++) out[i] = *(Bind *)binds.items[i];
    *out_count = binds.count;
    free(binds.items);
    return out;
}

/* Report an import cycle, naming every module on it. */
static void report_cycle(Loader *ld, Mod *m, const Stmt *site) {
    ld_error(ld, "E_IMPORT_CYCLE", site ? site->file : m->file,
             site ? site->line : 1, site ? site->col : 1,
             "import cycle: module '%s' imports itself, directly or indirectly",
             m->dotted[0] ? m->dotted : m->file);
    Diag *d = &ld->diags->items[ld->diags->count - 1];
    bool on_cycle = false;
    for (size_t i = 0; i < ld->depth; i++) {
        if (ld->stack[i] == m) on_cycle = true;
        if (on_cycle)
            diag_note(d, "cycle", ld->stack[i]->dotted[0] ? ld->stack[i]->dotted
                                                          : ld->stack[i]->file);
    }
    diag_note(d, "cycle", m->dotted[0] ? m->dotted : m->file);
}

/* Parse `file` (if it isn't loaded already), load everything it imports, and
 * append its linked top-level statements to the output program. */
static Mod *load_module(Loader *ld, const char *dotted, const char *file,
                        char *src, const Stmt *site) {
    char *key = canonical(file);
    for (size_t i = 0; i < ld->count; i++) {
        if (strcmp(ld->mods[i]->key, key) != 0) continue;
        Mod *m = ld->mods[i];
        free(key);
        if (m->state == M_LOADING) {
            report_cycle(ld, m, site);
            return NULL;
        }
        return m;
    }

    Mod *m = xmalloc(sizeof(Mod));
    memset(m, 0, sizeof(*m));
    m->dotted = xstrdup(dotted);
    m->prefix = dotted[0] ? dotted_to_prefix(dotted) : NULL;
    m->file = xstrdup(file);
    m->key = key;
    m->dir = dir_of(file);
    m->src = src;
    m->state = M_LOADING;
    diag_add_source(ld->diags, m->file, m->src);

    if (ld->count == ld->cap) {
        ld->cap = ld->cap ? ld->cap * 2 : 8;
        ld->mods = xrealloc(ld->mods, sizeof(Mod *) * ld->cap);
    }
    ld->mods[ld->count++] = m;

    if (ld->depth == ld->stack_cap) {
        ld->stack_cap = ld->stack_cap ? ld->stack_cap * 2 : 8;
        ld->stack = xrealloc(ld->stack, sizeof(Mod *) * ld->stack_cap);
    }
    ld->stack[ld->depth++] = m;

    m->prog = parse_program(m->src, m->file, ld->diags);
    index_module(m);

    size_t nbinds = 0;
    Bind *binds = bind_imports(ld, m, &nbinds);

    RW rw;
    memset(&rw, 0, sizeof(rw));
    rw.ld = ld;
    rw.mod = m;
    rw.binds = binds;
    rw.nbinds = nbinds;
    for (size_t i = 0; i < m->prog->body.count; i++) {
        Stmt *s = m->prog->body.items[i];
        if (s->kind == S_IMPORT) continue; /* imports are not executable */
        rw_stmt(&rw, s, NULL);
        vec_push(&ld->out, s);
    }
    free(rw.tvars.items);

    ld->depth--;
    m->state = M_DONE;
    return m;
}

Program *module_link(const char *entry, const char *const *roots, size_t nroots,
                     DiagList *diags, int *errors) {
    Loader ld;
    memset(&ld, 0, sizeof(ld));
    ld.roots = roots;
    ld.nroots = nroots;
    ld.diags = diags;

    char *entry_dir = dir_of(entry);
    ld.src_root = find_src_root(entry_dir);

    char *src = read_file(&ld, entry);
    if (!src) {
        fprintf(stderr, "emeraldc: cannot open '%s'\n", entry);
        *errors = 1;
        return NULL;
    }

    /* The entry module has no dotted path, so its names are never mangled. */
    load_module(&ld, "", entry, src, NULL);
    free(entry_dir);

    *errors = ld.errors;
    if (ld.errors) return NULL;

    Program *prog = xmalloc(sizeof(Program));
    prog->body.items = (Stmt **)ld.out.items;
    prog->body.count = ld.out.count;
    return prog;
}
