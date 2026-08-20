/* Internal interface shared by the codegen implementation files.
 * Not part of the public API: include/codegen.h is. */
#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

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

/* --- name tables --------------------------------------------------------- */
typedef struct {
    char **names;
    const char **files; /* declaring module, for the global table; else NULL */
    size_t count, cap;
} Names;

/* the shared builtin table (include/builtins.def) */
typedef struct {
    const char *name;
    const char *cfn;  /* runtime entry point; NULL = lowered specially below */
    int arity;
    bool rets;        /* the C function returns a Value */
} Builtin;

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

/* --- name resolution ----------------------------------------------------- */
typedef enum { A_LOCAL, A_CELL, A_CAPTURED, A_GLOBAL } Access;

/* --- closure analysis ---------------------------------------------------- */
typedef struct {
    Names *globals;
    int counter;
    FuncInfo **all;      /* every function, in build order */
    size_t all_count, all_cap;
} Ctx;

typedef struct { Names *globals; FuncInfo *fi; } LocalCtx;

extern const Builtin cg_builtins[];

void sb_printf(SB *sb, const char *fmt, ...);
int names_find(const Names *ns, const char *name);
void names_add_file(Names *ns, const char *name, const char *file);
void names_add(Names *ns, const char *name);
bool global_owned_by(const Names *globals, const char *name,
                            const char *file);
const Builtin *cg_builtin_find(const char *name);
int add_local(FuncInfo *fi, const char *name);
void emit(Cg *cg, const char *fmt, ...);
int new_temp(Cg *cg);
const char *slotref(int slot, char buf[32]);
void sb_c_string(SB *sb, const char *s);
bool var_slot(Cg *cg, const char *name, Access *kind, int *slot);
FuncInfo *find_top_func(Cg *cg, const char *name);
int gen_var_read(Cg *cg, const char *name);
void gen_var_write(Cg *cg, const char *name, const char *val);
const char *cellref(Cg *cg, const char *name, char buf[32]);
int gen_expr(Cg *cg, const Expr *e);
void gen_if_arms(Cg *cg, const Stmt *s, size_t arm);
void gen_block(Cg *cg, const Block *b);
void collect_match_pats_block(const Block *b,
                                     void (*fn)(const char *, const char *,
                                                int, void *),
                                     void *ud);
FuncInfo *make_top_root(Ctx *ctx, const Block *body);
FuncInfo *build_func(Ctx *ctx, const Stmt *node, FuncInfo *parent);
void gen_function(FILE *out, FuncInfo *fi, Names *globals,
                         FuncInfo **top, size_t top_count);

#endif
