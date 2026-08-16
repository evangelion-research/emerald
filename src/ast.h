/* Emerald AST. Nodes are malloc'd and never freed: the compiler is a
 * short-lived process and the whole tree dies with it.
 */
#ifndef EMERALD_AST_H
#define EMERALD_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* --- type expressions (surface syntax of type annotations) -------------- */

typedef enum {
    TE_NAME,   /* int, float, str, bool, None, any, or a `type` alias */
    TE_LIST,   /* list[T] */
    TE_REC,    /* { x: int, y: int } */
    TE_UNION,  /* A | B */
    TE_INTER,  /* A & B  (structural "inheritance") */
} TypeExprKind;

typedef struct TypeExpr TypeExpr;
struct TypeExpr {
    TypeExprKind kind;
    int line;
    char *name;             /* TE_NAME */
    TypeExpr *elem;         /* TE_LIST */
    struct {                /* TE_REC */
        char **names;
        TypeExpr **types;
        size_t count;
    } fields;
    TypeExpr *lhs, *rhs;    /* TE_UNION / TE_INTER */
};

/* --- expressions -------------------------------------------------------- */

typedef enum {
    E_INT, E_FLOAT, E_STR, E_TRUE, E_FALSE, E_NONE,
    E_NAME, E_LIST, E_REC,
    E_BINOP, E_UNOP, E_CALL, E_INDEX, E_ATTR,
} ExprKind;

typedef enum {
    B_ADD, B_SUB, B_MUL, B_DIV, B_MOD,
    B_EQ, B_NE, B_LT, B_LE, B_GT, B_GE,
    B_AND, B_OR,
} BinOp;

typedef enum { U_NEG, U_NOT } UnOp;

typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    int line;
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
    } as;
};

/* --- statements --------------------------------------------------------- */

typedef enum {
    S_EXPR, S_ASSIGN, S_IF, S_WHILE, S_FOR,
    S_RETURN, S_BREAK, S_CONTINUE, S_PASS,
    S_BLOCK, S_FUNC, S_TYPEDEF,
} StmtKind;

typedef struct Stmt Stmt;
typedef struct { Stmt **items; size_t count; } Block;

struct Stmt {
    StmtKind kind;
    int line;
    union {
        Expr *expr;                                   /* S_EXPR */
        struct {                                      /* S_ASSIGN */
            Expr *target;      /* E_NAME | E_INDEX | E_ATTR */
            TypeExpr *ann;     /* optional, E_NAME targets only */
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
        Expr *ret;                                    /* S_RETURN, may be NULL */
        Block block;                                  /* S_BLOCK */
        struct {                                      /* S_FUNC */
            char *name;
            char **params;
            TypeExpr **param_types; /* entries may be NULL (=any) */
            size_t param_count;
            TypeExpr *ret_type;     /* NULL = any */
            Block body;
        } func;
        struct { char *name; TypeExpr *value; } tdef; /* S_TYPEDEF */
    } as;
};

typedef struct { Block body; } Program;

/* ast.c */
void ast_print_program(FILE *out, const Program *p);
void ast_collect_assigned(const Block *b, void (*fn)(const char *, int line, void *), void *ud);

#endif
