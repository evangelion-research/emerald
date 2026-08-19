/* Emerald AST. Nodes are malloc'd and never freed: the compiler is a
 * short-lived process and the whole tree dies with it.
 */
#ifndef EMERALD_AST_H
#define EMERALD_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct DimExpr DimExpr;  /* see include/dim.h */

/* --- type expressions (surface syntax of type annotations) -------------- */

typedef enum {
    TE_NAME,   /* int, float, str, bool, None, any, never, alias, type var */
    TE_LIST,   /* list[T] */
    TE_SEQ,    /* seq[T] — an immutable, covariant, sound sequence */
    TE_REC,    /* { x: int, y: int } */
    TE_UNION,  /* A | B */
    TE_INTER,  /* A & B  (structural "inheritance") */
    TE_LIT,    /* literal type: 42, "red", True */
    TE_FUNC,   /* (A, B) -> C  (function type) */
    TE_TENSOR, /* Tensor[dtype, [dim, ...]] or Tensor[dtype, ?] */
    TE_FIN,    /* Fin[n] — an index provably below the dim `n` */
    TE_EQ,     /* Eq[a, b] — propositional equality of two dim expressions */
} TypeExprKind;

typedef enum { LIT_INT, LIT_STR, LIT_BOOL, LIT_NONE } LitKind;

typedef struct TypeExpr TypeExpr;
struct TypeExpr {
    TypeExprKind kind;
    int line;
    int col;                /* 1-based column of the first token */
    char *name;             /* TE_NAME */
    TypeExpr **args;        /* TE_NAME: generic application `Name[T, ...]` */
    size_t arg_count;
    TypeExpr *elem;         /* TE_LIST */
    struct {                /* TE_REC */
        char **names;
        TypeExpr **types;
        size_t count;
    } fields;
    TypeExpr *lhs, *rhs;    /* TE_UNION / TE_INTER */
    struct {                /* TE_FUNC */
        TypeExpr **params;
        size_t param_count;
        TypeExpr *ret;
    } fun;
    struct {                /* TE_LIT */
        LitKind kind;
        int64_t ival;       /* LIT_INT, LIT_BOOL (0/1) */
        char *sval;         /* LIT_STR */
    } lit;
    struct {                /* TE_TENSOR */
        TypeExpr *dtype;    /* the element dtype: TE_NAME "f32" / "f64" */
        DimExpr **shape;    /* dimension expressions; NULL when `dynamic` */
        size_t shape_count;
        bool dynamic;       /* Tensor[f32, ?]: a dynamic-shape escape hatch */
    } tensor;
    DimExpr *fin_dim;       /* TE_FIN: the bound expression `n` */
    DimExpr *eq_lhs, *eq_rhs; /* TE_EQ: the two sides of Eq[a, b] */
};

/* --- expressions -------------------------------------------------------- */

typedef enum {
    E_INT, E_FLOAT, E_STR, E_TRUE, E_FALSE, E_NONE,
    E_NAME, E_LIST, E_REC,
    E_BINOP, E_UNOP, E_CALL, E_INDEX, E_ATTR,
    E_LAMBDA,   /* (a: int, b) => body: anonymous function value */
    E_TRY,      /* try e: unwrap a Result, propagating its error to the caller */
    E_CATCH,    /* catch e { Tag b -> expr, ... }: handle every expected error */
} ExprKind;

/* One arm of a `catch`. `tag` is the error type's name, or NULL for the
 * catch-all arm `_`. `bind`, when non-NULL, names the error value inside
 * `body` — an arm that does not need the payload omits it. */
typedef struct {
    char *tag;
    char *bind;
    struct Expr *body;
    int line, col;
} CatchArm;

typedef enum {
    B_ADD, B_SUB, B_MUL, B_DIV, B_MOD,
    B_EQ, B_NE, B_LT, B_LE, B_GT, B_GE,
    B_AND, B_OR,
    B_PIPE,     /* x |> f  ==  f(x) */
    B_COMPOSE,  /* f >> g  ==  x -> g(f(x)) */
} BinOp;

typedef enum { U_NEG, U_NOT } UnOp;

typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    int line;
    int col;                /* 1-based column of the first token */
    /* Source spelling, when it differs from the node's own name because the
     * module linker rewrote it (`strings.split` -> `strings__split`). NULL
     * means "the node already spells itself the way the user wrote it".
     * Diagnostics quote this so users never see mangled names. */
    const char *disp;
    union {
        int64_t ival;                 /* E_INT */
        double fval;                  /* E_FLOAT */
        char *sval;                   /* E_STR (unescaped), E_NAME */
        struct { Expr **items; size_t count; } list;   /* E_LIST */
        struct { char **names; Expr **values; size_t count; } rec; /* E_REC */
        struct { BinOp op; Expr *lhs, *rhs; } bin;
        struct { UnOp op; Expr *operand; } un;
        struct { Expr *fn; Expr **args; size_t count; } call; /* fn is E_NAME */
        struct { Expr *seq, *idx; } index;
        struct { Expr *obj; char *name; } attr;
        Expr *try_expr;                   /* E_TRY */
        struct {                          /* E_CATCH */
            Expr *subject;
            CatchArm *arms;
            size_t count;
        } ctch;
        struct {                            /* E_LAMBDA */
            char **params;
            TypeExpr **param_types;          /* entries may be NULL (=any) */
            size_t param_count;
            Expr *body;                      /* single-expression body */
        } lam;
    } as;
};

/* --- patterns (match statement) ----------------------------------------- */

typedef enum {
    P_WILD,   /* _ */
    P_BIND,   /* name */
    P_LIT,    /* 42, "red", True, None */
    P_REC,    /* { kind: "circle", r } */
} PatKind;

typedef struct Pat Pat;
struct Pat {
    PatKind kind;
    int line;
    int col;
    char *name;   /* P_REC: field name; otherwise unused */
    char *bind;   /* P_BIND: the bound variable name */
    struct { LitKind kind; int64_t ival; char *sval; } lit; /* P_LIT */
    struct { Pat **items; size_t count; } rec;              /* P_REC: field sub-patterns */
};

/* --- statements --------------------------------------------------------- */

typedef enum {
    S_EXPR, S_ASSIGN, S_IF, S_WHILE, S_FOR,
    S_RETURN, S_BREAK, S_CONTINUE, S_PASS,
    S_BLOCK, S_FUNC, S_TYPEDEF, S_IMPORT,
    S_MATCH,  /* match e { pat -> { ... } } */
    S_DIMDECL, /* `dim Batch, Seq, DModel` — nominal dimension names */
} StmtKind;

/* one entry of `from <path> import a, b as c` */
typedef struct {
    char *name;   /* the name as it appears in the imported module */
    char *local;  /* the local binding (equal to `name` without `as`) */
    int line, col;
} ImportName;

typedef struct Stmt Stmt;
typedef struct { Stmt **items; size_t count; } Block;

struct Stmt {
    StmtKind kind;
    int line;
    int col;                /* 1-based column of the first token */
    const char *file;       /* source file this statement was parsed from */
    union {
        Expr *expr;                                   /* S_EXPR */
        struct {                                      /* S_ASSIGN */
            Expr *target;      /* E_NAME | E_INDEX | E_ATTR */
            TypeExpr *ann;     /* optional, E_NAME targets only */
            bool is_const;     /* `const x = v` / `const x: T = v` */
            Expr *value;
        } assign;
        struct {                                      /* S_IF */
            Expr **conds;      /* one per if/elif arm */
            Block *blocks;     /* same count as conds */
            size_t count;
            Block else_block;  /* count==0 when absent */
            bool has_else;
        } ifs;
        struct { Expr *cond; Block body; } wh;        /* S_WHILE */
        struct { char *var; Expr *seq; Block body; } fr; /* S_FOR */
        struct {                                      /* S_MATCH */
            Expr *subject;
            Pat **pats;
            Block *blocks;     /* same count as pats; blocks[i] matches pats[i] */
            size_t count;
        } mtch;
        Expr *ret;                                    /* S_RETURN, may be NULL */
        Block block;                                  /* S_BLOCK */
        struct {                                      /* S_FUNC */
            char *name;             /* linker-mangled for imported modules */
            const char *dispname;   /* name as written in the source */
            char **tparams;         /* generic type parameters `def f[T, U]` */
            bool *tparam_dims;      /* parallel: true when `T: dim` (a dimension) */
            size_t tparam_count;
            char **params;
            TypeExpr **param_types; /* entries may be NULL (=any) */
            size_t param_count;
            TypeExpr *ret_type;     /* NULL = any */
            bool pure;              /* `def f(...) -> T pure`: may only call pure code */
            bool partial;           /* `def f(...) -> T partial`: opts out of termination checking */
            Block body;
        } func;
        struct {                                      /* S_TYPEDEF */
            char *name;             /* linker-mangled for imported modules */
            const char *dispname;   /* name as written in the source */
            char **params;          /* generic parameters `type Pair[A, B]` */
            bool *param_dims;       /* parallel: true when `A: dim` */
            size_t param_count;
            TypeExpr *value;
        } tdef;
        struct {                                      /* S_IMPORT */
            char *path;             /* dotted module path, "text.strings" */
            char *alias;            /* `import p as a`; NULL otherwise */
            ImportName *names;      /* `from p import ...`; NULL for plain */
            size_t name_count;
            bool is_from;
        } imp;
        struct {                                      /* S_DIMDECL */
            char **names;           /* `dim Batch, Seq, DModel` */
            size_t count;
        } dim;
    } as;
};

typedef struct { Block body; } Program;

/* ast.c */
void ast_print_program(FILE *out, const Program *p);
/* `file` is the source file of the assigning statement: a global is only
 * updated by an assignment from the module that declared it (see module.c). */
void ast_collect_assigned(const Block *b,
                          void (*fn)(const char *name, const char *file,
                                     int line, void *ud),
                          void *ud);

#endif
