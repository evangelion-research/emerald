/* Internal interface shared by the check implementation files.
 * Not part of the public API: include/check.h is. */
#ifndef CHECK_INTERNAL_H
#define CHECK_INTERNAL_H

/* Emerald type checker: gradual, structural typing in the TypeScript spirit.
 *
 * - Unannotated code is `any` and checks like dynamic Python.
 * - `type Name = {...}` declares a structural alias. Assignability between
 *   records is *width subtyping*: a record with more fields is assignable to
 *   a record type with fewer ("inheritance" without classes).
 * - `A & B` merges two record types (intersection). `A | B` is a union.
 * - Literal types (`3`, `"red"`, `True`) are singleton types; `never` is the
 *   empty type. Together with flow narrowing (`if x == None`, discriminant
 *   fields) they give exhaustiveness proofs: in the impossible branch a value
 *   has type `never`.
 * - Generic aliases (`type Pair[A, B]`) and functions (`def head[T]`) are
 *   checked by unification at each call site; codegen is untyped so no
 *   monomorphization is needed.
 * - Lists are covariant (like TS arrays: convenient, mildly unsound).
 *
 * The checker never mutates the AST; codegen is untyped and independent.
 */
#include "check.h"
#include "diag.h"
#include "dim.h"
#include "xalloc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EFF_PURE 0u
#define EFF_IO   1u

typedef enum {
    TY_ANY, TY_NEVER, TY_NONE, TY_BOOL, TY_INT, TY_FLOAT, TY_STR,
    TY_LIT, TY_LIST, TY_SEQ, TY_REC, TY_UNION, TY_VAR, TY_ALIAS, TY_FUNC,
    TY_TENSOR, TY_FIN,
    TY_EQ,      /* Eq[a, b]: propositional equality of two dim expressions */
    TY_OPAQUE,  /* Chan[T], Task[T]: a runtime handle with one element type */
} TyKind;

/* Effect labels. A function type carries the join of its
 * effects; `pure` is the empty mask. Only the impurity `pure` rules out is
 * tracked today -- the finer labels (rand/mut/alloc/nondet) arrive with the
 * rest of D1, and each one is a new bit here. */
typedef unsigned EffMask;

/* tensor dtype tags used by the checker (the runtime's DType is independent) */
typedef enum { CDT_F32, CDT_F64 } CDType;

/* A tensor shape: a list of canonical dim expressions, or the dynamic
 * escape hatch `?` (Tensor[f32, ?]). `dims` is NULL when dynamic. */
typedef struct Shape {
    bool dynamic;
    DimExpr **dims;
    size_t count;
} Shape;

typedef struct Type Type;

struct Type {
    TyKind k;
    bool fresh; /* literal born from a literal expression: widens on binding */
    Type *elem;                                             /* TY_LIST */
    struct { char **names; Type **types; size_t count; } rec; /* TY_REC */
    struct { Type **alts; size_t count; } uni;              /* TY_UNION */
    struct { TyKind base; int64_t ival; char *sval; } lit;  /* TY_LIT */
    char *var;                                              /* TY_VAR */
    struct { Type **params; Type *ret; size_t count; EffMask eff; } fun; /* TY_FUNC */
    struct { CDType dt; Shape *shape; } tensor;             /* TY_TENSOR */
    DimExpr *fin;                                            /* TY_FIN */
    struct { DimExpr *lhs, *rhs; } eq;                       /* TY_EQ */
    /* TY_OPAQUE: the handle's name ("Chan" / "Task") lives in `var` and the
     * value it carries in `elem`. Handles have no structure a program can
     * inspect, so equality is the name plus the element type. */
    /* TY_ALIAS: a reference to a named alias. A self-reference encountered
     * while an alias body is being resolved becomes this node (see resolve_name). */
    struct { const struct Alias *al; Type **args; size_t argc; } ref;
};

/* A named type alias. Non-generic aliases resolve eagerly; a self-reference
 * encountered while resolving becomes a TY_ALIAS node instead. */
typedef struct Alias {
    char *name;
    const char *disp;           /* source-level name (linking may mangle `name`) */
    Type *type;                 /* resolved eagerly for non-generic aliases */
    char **params;              /* generic parameters, NULL when non-generic */
    bool *param_dims;           /* parallel: true when `P: dim` (a dimension) */
    size_t param_count;
    const TypeExpr *body;       /* unresolved body for generic aliases */
    bool resolving;             /* guard: currently resolving this alias's body */
} Alias;

/* --- type equality / assignability -------------------------------------- */
/* Cycle-safe type comparison: resolve alias references, then recurse with a
 * memoized set of in-progress pairs (coinductive equality). */
typedef struct {
    const Type *a[256], *b[256];
    size_t count;
} EqVis;

/* --- checker context ----------------------------------------------------- */
typedef struct {
    char *name;
    Type *decl;     /* declared (or widened inferred) type: assignments check this */
    Type *type;     /* current flow-narrowed type: reads see this */
    bool annotated; /* explicit annotations are enforced; inferred ones widen */
    bool bound;     /* false until the first assignment executes */
    bool is_const;  /* declared `const`: reassignment is an error */
    bool owned;     /* holds a list this function allocated itself (a fresh
                     * literal or fresh-list pure builtin) that has not escaped;
                     * the one target a `pure` function may `append` to — see
                     * the standard-library purity convention and expr_owned(). Globals and
                     * parameters are never owned. */
    const char *file; /* for globals: the declaring module. An assignment from
                       * another module's function body makes a local, not a
                       * clobber — see updatable_global() */
    int gen;        /* bumped on assignment; invalidates stale narrowings */
} Var;

typedef struct { Var *items; size_t count, cap; } VarEnv;

typedef struct {
    char *name;
    const char *disp;    /* source-level name (differs when a module was linked) */
    char **tparams;      /* generic type parameters, e.g. def head[T] */
    size_t tparam_count;
    Type **params;       /* may contain TY_VAR when generic */
    size_t param_count;
    Type *ret;
    bool pure;           /* declared `pure`: may only call pure functions/builtins */
    bool partial;        /* declared `partial`: exempt from termination checking */
    EffMask eff;         /* the function's effect mask (0 when `pure`) */
    const Stmt *node;
} FuncSig;

/* type-variable environment used while resolving type expressions */
typedef struct { char **names; Type **types; size_t count; } TyEnv;

/* A lexical scope for one function body: its locals plus the nested `def`s
 * registered directly within it. Scopes chain to the enclosing function, so a
 * nested function can read (capture) enclosing locals and call sibling
 * functions by name. */
typedef struct Scope {
    VarEnv locals;
    FuncSig *funcs;        /* nested `def`s registered in this scope */
    size_t func_count, func_cap;
    struct Scope *parent;  /* enclosing function scope; NULL for a top def */
    bool pure;             /* purity of the enclosing def; nested defs must match */
} Scope;

typedef struct {
    const char *filename;
    DiagList *diags;  /* where diagnostics are collected */
    int errors;
    Alias *aliases;
    size_t alias_count, alias_cap;
    int alias_depth;  /* guards recursive generic alias expansion */
    FuncSig *funcs;   /* top-level function signatures */
    size_t func_count;
    VarEnv globals;
    Scope *scope;     /* current function scope; NULL at top level */
    Type *cur_ret;    /* declared return type of the current function */
    bool cur_pure;    /* purity of the function whose body is being checked */
    bool proof;       /* --proof: `any` and `partial` are banned */
    bool in_sig;      /* resolving a function signature: proof `any` reports here */
    bool in_lambda;   /* checking a lambda body: `try` has no channel there */
    TyEnv *tyenv;     /* type parameters of the function being checked: in
                       * scope for annotations in its body as well as its
                       * signature (`out: list[T] = []` inside def f[T]) */
    int loop_depth;
    /* --- shape system state --- */
    char **dim_names;          /* module-level `dim` declarations */
    size_t dim_count, dim_cap;
    char **dim_params;         /* `B: dim` type parameters in scope */
    size_t dim_param_count;
    char **dim_sub_names;      /* active dim-substitution env (generic aliases) */
    DimExpr **dim_sub_values;
    size_t dim_sub_count;
} Ck;

/* D5/W2: a type is tainted when `any` (or a dynamic tensor shape) is reachable
 * through any constructor. Tainted types prove nothing, so proof mode rejects
 * them and obligations discharged by them are vacuous (W_VACUOUS_PROOF). */
typedef struct { const Type *items[256]; size_t count; } TaintVis;

/* --- generic call-site inference ----------------------------------------- */
typedef struct { char **names; Type **types; size_t count; } Subst;

/* --- flow narrowing ------------------------------------------------------ */
/* A narrowing temporarily overrides Var.type; NSave remembers how to undo it.
 * If the variable was assigned in between (gen changed), restoring falls back
 * to the declared type instead of the stale snapshot. */
typedef struct { Var *var; Type *saved; int gen; } NSave;

typedef struct { NSave items[64]; size_t count; } NSet;

/* --- termination checking ------------------------------------------------ */
/* Emerald functions are total by default: every recursive call must descend
 * structurally. An argument descends when it is a projection chain from a
 * parameter whose declared type is a recursive alias (`n.succ`, `xs.tail`),
 * with every step landing back on that same alias — the standard "one
 * constructor step smaller" rule, applied to non-generic recursive aliases.
 * A function that cannot be shown to terminate this way must be declared
 * `partial` to opt out. Mutual recursion needs `partial`; descent through a
 * `seq[T]` element (`t.kids[0]`) is recognized, `list[T]` is not (mutable).
 */
typedef struct { const Type *items[256]; size_t count; } Vis;

/* --- W4: mutual recursion ------------------------------------------------ */
/* A set of callee names, gathered from one function body. Names are already
 * module-mangled by the linker, so they match the registered `FuncSig` names. */
typedef struct { const char *items[256]; size_t count; } CalleeSet;

/* --- passes -------------------------------------------------------------- */
typedef struct { Ck *ck; VarEnv *env; const char *skip; } DeclCtx;

struct Alias;
extern Type t_any, t_never, t_none, t_bool, t_int, t_float, t_str;
extern bool ck_proof_mode;
extern size_t proof_rep_total_funcs, proof_rep_partial_funcs,
    proof_rep_pure_funcs;
extern char **proof_rep_partial_names;
extern size_t proof_rep_partial_n, proof_rep_partial_cap;
extern size_t proof_rep_vacuous;      /* W_VACUOUS_PROOF emissions */
extern size_t proof_rep_covariance;   /* W_UNSOUND_COVARIANCE emissions */
extern size_t proof_rep_taint_sites;  /* proof-mode tainted-type rejections */
extern const DimExpr *eq_evidence_l[64], *eq_evidence_r[64];
extern size_t eq_evidence_count;
extern size_t shape_dyn_crossings;
typedef struct { const char *name; bool pure; } BuiltinSig;
extern const BuiltinSig builtins[];

Type *ty_resolve(const Type *t);
void proof_rep_reset(void);
Type *ty_new(TyKind k);
Type *ty_list(Type *elem);
Type *ty_seq(Type *elem);
Type *to_seq(Type *t);
Type *ty_opaque(const char *name, Type *elem);
bool is_opaque(const Type *t, const char *name);
Type *ty_func(Type **params, size_t count, Type *ret);
Type *ty_lit_int(int64_t v);
Type *ty_lit_str(char *s);
Type *ty_lit_bool(int64_t v);
Type *ty_var(char *name);
Shape *shape_dynamic(void);
Shape *shape_of(DimExpr **dims, size_t count);
Type *ty_tensor(CDType dt, Shape *shape);
Type *ty_fin(DimExpr *bound);
Type *ty_eq(DimExpr *lhs, DimExpr *rhs);
Type *tensor_of(Type *t);
bool dim_is_one(const DimExpr *e);
DimExpr *shape_prod(const Shape *s);
Shape *broadcast_shapes(const Shape *a, const Shape *b);
Shape *literal_shape_of_expr(const Expr *e);
Type *gc_stats_type(void);
Type *task_stats_type(void);
bool type_eq(const Type *a, const Type *b);
bool assignable(const Type *dst, const Type *src);
Type *ty_join(Type *a, Type *b);
Type *ty_union_of(Type **alts, size_t n);
Type *ty_base(Type *t);
Type *widen(Type *t);
const char *type_str(const Type *t);
void note_shape_crossing(Ck *ck, const Type *dst, const Type *src);
void ck_error(Ck *ck, const char *code, int line, int col,
                     const char *fmt, ...);
void ck_error_t(Ck *ck, const char *code, int line, int col,
                       const Type *expected, const Type *actual,
                       const char *fmt, ...);
void ck_warn(Ck *ck, const char *code, int line, int col,
                    const char *fmt, ...);
bool type_is_fresh(const Type *t);
bool type_tainted(const Type *t);
void ck_covariance(Ck *ck, const Type *dst, const Type *src,
                          int line, int col);
void ck_proof_taint(Ck *ck, Type *t, int line, int col,
                           const char *noun);
Var *env_find(VarEnv *env, const char *name);
Var *env_add(VarEnv *env, const char *name, Type *t, bool annotated);
Var *lookup_var(Ck *ck, const char *name);
const char *builtin_find(const char *name, bool *pure);
bool is_builtin(const char *name);
FuncSig *find_func(Ck *ck, const char *name);
EffMask expr_eff(Ck *ck, const Expr *e);
Type *resolve_type(Ck *ck, const TypeExpr *te, const TyEnv *env);
Type *infer_lambda_with(Ck *ck, const Expr *e, Type **ptypes);
Type *infer_lambda(Ck *ck, const Expr *e, const Type *expected);
Type *ty_subst(Type *t, Subst *sub);
void unify(Type *param, Type *arg, Subst *sub);
Type *expect_tensor(Ck *ck, const Expr *e, Type *t, const char *fn);
Type *infer_tensor_matmul(Ck *ck, const Expr *e, Type **argt);
Type *infer_tensor_reshape(Ck *ck, const Expr *e, Type **argt);
Type *infer_tensor_transpose(Ck *ck, const Expr *e, Type *t);
Type *infer_tensor_permute(Ck *ck, const Expr *e, Type **argt);
Type *infer_tensor_reduce(Ck *ck, const Expr *e, Type **argt,
                                 const char *fn);
Type *infer_tensor_expand(Ck *ck, const Expr *e, Type **argt);
Type *infer_tensor_slice(Ck *ck, const Expr *e, Type **argt);
Type *infer_tensor_astype(Ck *ck, const Expr *e, Type **argt);
Type *infer_binop(Ck *ck, const Expr *e);
bool ck_arity(Ck *ck, const Expr *e, const char *name, size_t want);
Type *infer_map_like(Ck *ck, const Expr *e, const char *name,
                            Type **argt, const bool *islam);
void mark_escaped(Ck *ck, const Expr *e);
void assign_owned(Ck *ck, Var *v, const char *name, const Expr *value);
Type *infer_call(Ck *ck, const Expr *e, Type *expected);
Type *infer(Ck *ck, const Expr *e);
void nset_restore_from(NSet *ns, size_t mark);
void narrow_cond(Ck *ck, const Expr *e, bool sense, NSet *ns);
bool block_terminates(const Block *b);
bool block_returns(const Block *b);
const Type *field_type(const Type *t, const char *f);
void check_termination(Ck *ck, const Stmt *s, Type **ptypes);
void check_mutual_recursion(Ck *ck);
Type *check_pattern(Ck *ck, const Pat *p, const Type *st, VarEnv *env);
void check_block(Ck *ck, const Block *b);
bool updatable_global(Ck *ck, const char *name, const char *file);
void check_func(Ck *ck, Scope *parent, const Stmt *s);
const ProofReport *proof_report_get(void);
int check_program(const Program *prog, const char *filename, DiagList *diags,
                  bool proof);

#endif
