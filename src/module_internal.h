/* Internal interface shared by the module implementation files.
 * Not part of the public API: include/module.h is. */
#ifndef MODULE_INTERNAL_H
#define MODULE_INTERNAL_H

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

#ifndef EMERALD_STDLIB_DIR
#define EMERALD_STDLIB_DIR "stdlib"
#endif

/* --- small name sets ----------------------------------------------------- */
typedef struct { const char **items; size_t count, cap; } Names;

typedef struct { void **items; size_t count, cap; } PtrVec;

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

typedef struct { Names *bound; const Names *globals; } LocalCtx;

char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);
bool names_has(const Names *ns, const char *name);
void mod_names_add(Names *ns, const char *name);
void mod_vec_push(PtrVec *v, void *item);
char *dir_of(const char *path);
char *find_src_root(const char *entry_dir);
char *dotted_to_prefix(const char *dotted);
char *canonical(const char *path);
void ld_error(Loader *ld, const char *code, const char *file,
                     int line, int col, const char *fmt, ...);
char *read_file(Loader *ld, const char *path);
bool mod_exports(const Mod *m, const char *name);
bool is_private(const char *name);
char *mangle(const Mod *m, const char *name);
void index_module(Mod *m);
char *resolve_module(Loader *ld, const char *dotted,
                            const char *importer_dir, const Stmt *site);
void rw_stmt(RW *rw, Stmt *s, const RScope *sc);

#endif
