#ifndef EMERALD_LEXER_H
#define EMERALD_LEXER_H

#include <stddef.h>

typedef enum {
    TK_EOF, TK_ERROR,
    TK_INT, TK_FLOAT, TK_STR, TK_IDENT,
    /* keywords */
    TK_DEF, TK_IF, TK_ELIF, TK_ELSE, TK_WHILE, TK_FOR, TK_IN, TK_RETURN,
    TK_AND, TK_OR, TK_NOT, TK_TRUE, TK_FALSE, TK_NONE,
    TK_BREAK, TK_CONTINUE, TK_PASS, TK_TYPE,
    TK_CONST, TK_MATCH,
    TK_PURE, TK_PARTIAL,
    TK_IMPORT, TK_FROM, TK_AS,
    /* punctuation / operators */
    TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK,
    TK_COMMA, TK_DOT, TK_COLON, TK_SEMI, TK_ASSIGN, TK_ARROW,
    TK_PIPE, TK_AMP,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_FAT_ARROW,  /* =>  (lambda) */
    TK_PIPE_GT,    /* |>  (pipe) */
    TK_GTGT,       /* >>  (composition) */
    TK__COUNT
} TokKind;

typedef struct {
    TokKind kind;
    const char *start; /* points into the source buffer */
    int len;
    int line;
    int col;           /* 1-based column of the token's first character */
} Token;

typedef struct {
    const char *src;
    const char *cur;
    int line;
    int col;           /* 1-based column of `cur` */
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);
const char *token_kind_name(TokKind k);

#endif
