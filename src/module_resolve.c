/* Modules: name sets, path handling, module records, and import resolution. */
#include "module_internal.h"

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* sprintf into a fresh buffer */
char *xasprintf(const char *fmt, ...) {
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

bool names_has(const Names *ns, const char *name) {
    for (size_t i = 0; i < ns->count; i++)
        if (strcmp(ns->items[i], name) == 0) return true;
    return false;
}

void mod_names_add(Names *ns, const char *name) {
    if (names_has(ns, name)) return;
    if (ns->count == ns->cap) {
        ns->cap = ns->cap ? ns->cap * 2 : 8;
        ns->items = xrealloc(ns->items, sizeof(char *) * ns->cap);
    }
    ns->items[ns->count++] = name;
}

void mod_vec_push(PtrVec *v, void *item) {
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
char *dir_of(const char *path) {
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
char *find_src_root(const char *entry_dir) {
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
char *dotted_to_prefix(const char *dotted) {
    char *out = xstrdup(dotted);
    for (char *p = out; *p; p++)
        if (*p == '.') *p = '_';
    return out;
}

/* The canonical form of `path`, for deciding whether two spellings name the
 * same file. Falls back to the path itself when it cannot be resolved. */
char *canonical(const char *path) {
    char buf[PATH_MAX];
    if (realpath(path, buf)) return xstrdup(buf);
    return xstrdup(path);
}

void ld_error(Loader *ld, const char *code, const char *file,
                     int line, int col, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_add(ld->diags, DIA_SYNTAX, code, file, line, col, "%s", msg);
    ld->errors++;
}

char *read_file(Loader *ld, const char *path) {
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
bool mod_exports(const Mod *m, const char *name) {
    return names_has(&m->defs, name) || names_has(&m->types, name) ||
           names_has(&m->globals, name) || names_has(&m->dims, name);
}

bool is_private(const char *name) { return name[0] == '_'; }

char *mangle(const Mod *m, const char *name) {
    if (!m->prefix) return (char *)name;
    return xasprintf("%s__%s", m->prefix, name);
}

static void collect_global_cb(const char *name, const char *file, int line,
                              void *ud) {
    (void)line; (void)file;
    mod_names_add((Names *)ud, name);
}

/* Record the module's top-level names so importers (and the rename pass) know
 * what it exports. */
void index_module(Mod *m) {
    ast_collect_assigned(&m->prog->body, collect_global_cb, &m->globals);
    for (size_t i = 0; i < m->prog->body.count; i++) {
        const Stmt *s = m->prog->body.items[i];
        if (s->kind == S_FUNC) mod_names_add(&m->defs, s->as.func.name);
        else if (s->kind == S_TYPEDEF) mod_names_add(&m->types, s->as.tdef.name);
        else if (s->kind == S_DIMDECL)
            for (size_t j = 0; j < s->as.dim.count; j++)
                mod_names_add(&m->dims, s->as.dim.names[j]);
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
char *resolve_module(Loader *ld, const char *dotted,
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
