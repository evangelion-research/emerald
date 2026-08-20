/* Internal interface shared by the parser implementation files.
 * Not part of the public API: include/parser.h is. */
#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

/* Emerald parser: hand-written recursive descent, one token of lookahead.
 *
 * One subtlety: `{` is both a block and a record literal. Like Go, we forbid
 * record literals at the top level of a control-flow header expression
 * (`if x { ... }` — the `{` opens the block). Parenthesize to force a record:
 * `if (p == { x: 1 }) { ... }`. The `no_rec` flag implements this.
 */
#include "parser.h"
#include "diag.h"
#include "dim.h"
#include "lexer.h"
#include "xalloc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Lexer lx;
    Token cur;
    const char *filename;
    DiagList *diags; /* where syntax diagnostics are collected */
    bool no_rec; /* inside a control-flow header: `{` means block, not record */
    int block_depth; /* >0 inside a block: imports are top-level only */
} Parser;

/* snapshot/restore the parser+lexer position, for the one ambiguous case in
 * the grammar: `(x) => ...` is a lambda but `(x) + 1` is a grouping. */
typedef struct { Token cur; Lexer lx; } PSave;

/* growable pointer array */
typedef struct { void **items; size_t count, cap; } PtrVec;

void perror_at(Parser *p, int line, int col, const char *fmt, ...);
void advance(Parser *p);
bool check(Parser *p, TokKind k);
bool match(Parser *p, TokKind k);
PSave psave(Parser *p);
void prestore(Parser *p, PSave s);
Token expect(Parser *p, TokKind k, const char *what);
char *tok_text(Token t);
char *unescape_string(Parser *p, Token t);
void vec_push(PtrVec *v, void *item);
Expr *new_expr(ExprKind k, int line, int col);
Stmt *new_stmt(Parser *p, StmtKind k, int line, int col);
TypeExpr *new_type(TypeExprKind k, int line, int col);
void parse_type_params(Parser *p, char ***out, bool **out_dims,
                              size_t *out_count);
TypeExpr *parse_type(Parser *p);
Expr *bin(Parser *p, BinOp op, int line, int col, Expr *lhs, Expr *rhs);
Expr *parse_expr(Parser *p);
Expr *parse_header_expr(Parser *p);
Program *parse_program(const char *src, const char *filename, DiagList *diags);

#endif
