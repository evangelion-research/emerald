/* Emerald lexer: hand-written scanner over an in-memory source buffer.
 * Newlines are plain whitespace; `#` comments run to end of line.
 */
#include "lexer.h"

#include <ctype.h>
#include <string.h>

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->cur = src;
    lx->line = 1;
    lx->col = 1;
}

static const struct { const char *word; TokKind kind; } keywords[] = {
    {"def", TK_DEF}, {"if", TK_IF}, {"elif", TK_ELIF}, {"else", TK_ELSE},
    {"while", TK_WHILE}, {"for", TK_FOR}, {"in", TK_IN}, {"return", TK_RETURN},
    {"and", TK_AND}, {"or", TK_OR}, {"not", TK_NOT},
    {"True", TK_TRUE}, {"False", TK_FALSE}, {"None", TK_NONE},
    {"break", TK_BREAK}, {"continue", TK_CONTINUE}, {"pass", TK_PASS},
    {"type", TK_TYPE}, {"const", TK_CONST}, {"match", TK_MATCH},
    {"pure", TK_PURE}, {"partial", TK_PARTIAL},
    {"import", TK_IMPORT}, {"from", TK_FROM}, {"as", TK_AS},
    {"dim", TK_DIM},
    {"error", TK_ERROR_KW}, {"try", TK_TRY}, {"catch", TK_CATCH},
};

static Token make(Lexer *lx, TokKind k, const char *start, int start_col) {
    Token t;
    t.kind = k;
    t.start = start;
    t.len = (int)(lx->cur - start);
    t.line = lx->line;
    t.col = start_col;
    return t;
}

Token lexer_next(Lexer *lx) {
    for (;;) {
        char c = *lx->cur;
        if (c == '\n') { lx->line++; lx->col = 1; lx->cur++; }
        else if (c == ' ' || c == '\t' || c == '\r') { lx->col++; lx->cur++; }
        else if (c == '#') { while (*lx->cur && *lx->cur != '\n') { lx->col++; lx->cur++; } }
        else break;
    }

    const char *start = lx->cur;
    int start_col = lx->col;
    char c = *lx->cur;

    if (c == '\0') return make(lx, TK_EOF, start, start_col);

    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)*lx->cur) || *lx->cur == '_') { lx->col++; lx->cur++; }
        size_t n = (size_t)(lx->cur - start);
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
            if (strlen(keywords[i].word) == n && memcmp(keywords[i].word, start, n) == 0)
                return make(lx, keywords[i].kind, start, start_col);
        return make(lx, TK_IDENT, start, start_col);
    }

    if (isdigit((unsigned char)c)) {
        while (isdigit((unsigned char)*lx->cur)) { lx->col++; lx->cur++; }
        TokKind kind = TK_INT;
        if (*lx->cur == '.' && isdigit((unsigned char)lx->cur[1])) {
            kind = TK_FLOAT;
            lx->col++; lx->cur++;
            while (isdigit((unsigned char)*lx->cur)) { lx->col++; lx->cur++; }
        } else if (*lx->cur == '.' && !isalpha((unsigned char)lx->cur[1]) &&
                   lx->cur[1] != '_') {
            kind = TK_FLOAT; /* `3.` */
            lx->col++; lx->cur++;
        }
        if (*lx->cur == 'e' || *lx->cur == 'E') {
            const char *save = lx->cur;
            int save_col = lx->col;
            lx->col++; lx->cur++;
            if (*lx->cur == '+' || *lx->cur == '-') { lx->col++; lx->cur++; }
            if (isdigit((unsigned char)*lx->cur)) {
                kind = TK_FLOAT;
                while (isdigit((unsigned char)*lx->cur)) { lx->col++; lx->cur++; }
            } else {
                lx->cur = save; /* `1else` style: leave the e for the ident */
                lx->col = save_col;
            }
        }
        return make(lx, kind, start, start_col);
    }

    if (c == '"' || c == '\'') {
        char quote = c;
        lx->col++; lx->cur++;
        while (*lx->cur && *lx->cur != quote) {
            if (*lx->cur == '\\' && lx->cur[1]) { lx->col++; lx->cur++; }
            if (*lx->cur == '\n') { lx->line++; lx->col = 1; }
            else lx->col++;
            lx->cur++;
        }
        if (*lx->cur != quote) return make(lx, TK_ERROR, start, start_col); /* unterminated */
        lx->col++; lx->cur++;
        return make(lx, TK_STR, start, start_col); /* includes quotes; parser unescapes */
    }

    lx->col++; lx->cur++;
    switch (c) {
    case '{': return make(lx, TK_LBRACE, start, start_col);
    case '}': return make(lx, TK_RBRACE, start, start_col);
    case '(': return make(lx, TK_LPAREN, start, start_col);
    case ')': return make(lx, TK_RPAREN, start, start_col);
    case '[': return make(lx, TK_LBRACK, start, start_col);
    case ']': return make(lx, TK_RBRACK, start, start_col);
    case ',': return make(lx, TK_COMMA, start, start_col);
    case '.': return make(lx, TK_DOT, start, start_col);
    case ':': return make(lx, TK_COLON, start, start_col);
    case ';': return make(lx, TK_SEMI, start, start_col);
    case '|':
        if (*lx->cur == '>') { lx->col++; lx->cur++; return make(lx, TK_PIPE_GT, start, start_col); }
        return make(lx, TK_PIPE, start, start_col);
    case '&': return make(lx, TK_AMP, start, start_col);
    case '+':
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_PLUS_EQ, start, start_col); }
        return make(lx, TK_PLUS, start, start_col);
    case '*':
        if (*lx->cur == '*') { lx->col++; lx->cur++; return make(lx, TK_POW, start, start_col); }
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_STAR_EQ, start, start_col); }
        return make(lx, TK_STAR, start, start_col);
    case '/':
        if (*lx->cur == '/') { lx->col++; lx->cur++; return make(lx, TK_FLOORDIV, start, start_col); }
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_SLASH_EQ, start, start_col); }
        return make(lx, TK_SLASH, start, start_col);
    case '%': return make(lx, TK_PERCENT, start, start_col);
    case '-':
        if (*lx->cur == '>') { lx->col++; lx->cur++; return make(lx, TK_ARROW, start, start_col); }
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_MINUS_EQ, start, start_col); }
        return make(lx, TK_MINUS, start, start_col);
    case '=':
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_EQ, start, start_col); }
        if (*lx->cur == '>') { lx->col++; lx->cur++; return make(lx, TK_FAT_ARROW, start, start_col); }
        return make(lx, TK_ASSIGN, start, start_col);
    case '!':
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_NE, start, start_col); }
        return make(lx, TK_ERROR, start, start_col);
    case '<':
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_LE, start, start_col); }
        return make(lx, TK_LT, start, start_col);
    case '>':
        if (*lx->cur == '>') { lx->col++; lx->cur++; return make(lx, TK_GTGT, start, start_col); }
        if (*lx->cur == '=') { lx->col++; lx->cur++; return make(lx, TK_GE, start, start_col); }
        return make(lx, TK_GT, start, start_col);
    case '?': return make(lx, TK_QUESTION, start, start_col);
    }
    return make(lx, TK_ERROR, start, start_col);
}

const char *token_kind_name(TokKind k) {
    static const char *names[TK__COUNT] = {
        [TK_EOF] = "EOF", [TK_ERROR] = "ERROR",
        [TK_INT] = "INT", [TK_FLOAT] = "FLOAT", [TK_STR] = "STR",
        [TK_IDENT] = "IDENT",
        [TK_DEF] = "DEF", [TK_IF] = "IF", [TK_ELIF] = "ELIF", [TK_ELSE] = "ELSE",
        [TK_WHILE] = "WHILE", [TK_FOR] = "FOR", [TK_IN] = "IN",
        [TK_RETURN] = "RETURN", [TK_AND] = "AND", [TK_OR] = "OR",
        [TK_NOT] = "NOT", [TK_TRUE] = "TRUE", [TK_FALSE] = "FALSE",
        [TK_NONE] = "NONE", [TK_BREAK] = "BREAK", [TK_CONTINUE] = "CONTINUE",
        [TK_PASS] = "PASS", [TK_TYPE] = "TYPE", [TK_CONST] = "CONST",
        [TK_MATCH] = "MATCH",
        [TK_PURE] = "PURE", [TK_PARTIAL] = "PARTIAL",
        [TK_IMPORT] = "IMPORT", [TK_FROM] = "FROM", [TK_AS] = "AS",
        [TK_DIM] = "DIM",
        [TK_ERROR_KW] = "ERROR_KW", [TK_TRY] = "TRY",
        [TK_CATCH] = "CATCH",
        [TK_LBRACE] = "LBRACE", [TK_RBRACE] = "RBRACE",
        [TK_LPAREN] = "LPAREN", [TK_RPAREN] = "RPAREN",
        [TK_LBRACK] = "LBRACK", [TK_RBRACK] = "RBRACK",
        [TK_COMMA] = "COMMA", [TK_DOT] = "DOT", [TK_COLON] = "COLON",
        [TK_SEMI] = "SEMI", [TK_ASSIGN] = "ASSIGN", [TK_ARROW] = "ARROW",
        [TK_PIPE] = "PIPE", [TK_AMP] = "AMP",
        [TK_PLUS] = "PLUS", [TK_MINUS] = "MINUS", [TK_STAR] = "STAR",
        [TK_SLASH] = "SLASH", [TK_PERCENT] = "PERCENT",
        [TK_FLOORDIV] = "FLOORDIV", [TK_POW] = "POW",
        [TK_PLUS_EQ] = "PLUS_EQ", [TK_MINUS_EQ] = "MINUS_EQ",
        [TK_STAR_EQ] = "STAR_EQ", [TK_SLASH_EQ] = "SLASH_EQ",
        [TK_EQ] = "EQ", [TK_NE] = "NE", [TK_LT] = "LT", [TK_LE] = "LE",
        [TK_GT] = "GT", [TK_GE] = "GE",        [TK_FAT_ARROW] = "FAT_ARROW",
        [TK_PIPE_GT] = "PIPE_GT", [TK_GTGT] = "GTGT",
        [TK_QUESTION] = "QUESTION",
    };
    return names[k] ? names[k] : "?";
}
