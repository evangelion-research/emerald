/* Modules: loading and linking a program with all of its imports. */
#include "module_internal.h"

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
            mod_vec_push(&binds, b);
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
            mod_vec_push(&binds, b);
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
        mod_vec_push(&ld->out, s);
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
