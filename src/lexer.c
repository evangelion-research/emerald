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
}

static const struct { const char *word; TokKind kind; } keywords[] = {
    {"def", TK_DEF}, {"if", TK_IF}, {"elif", TK_ELIF}, {"else", TK_ELSE},
    {"while", TK_WHILE}, {"for", TK_FOR}, {"in", TK_IN}, {"return", TK_RETURN},
    {"and", TK_AND}, {"or", TK_OR}, {"not", TK_NOT},
    {"True", TK_TRUE}, {"False", TK_FALSE}, {"None", TK_NONE},
    {"break", TK_BREAK}, {"continue", TK_CONTINUE}, {"pass", TK_PASS},
    {"type", TK_TYPE},
};

static Token make(Lexer *lx, TokKind k, const char *start) {
    Token t;
    t.kind = k;
    t.start = start;
    t.len = (int)(lx->cur - start);
    t.line = lx->line;
    return t;
}

Token lexer_next(Lexer *lx) {
    for (;;) {
        char c = *lx->cur;
        if (c == '\n') { lx->line++; lx->cur++; }
        else if (c == ' ' || c == '\t' || c == '\r') lx->cur++;
        else if (c == '#') { while (*lx->cur && *lx->cur != '\n') lx->cur++; }
        else break;
    }

    const char *start = lx->cur;
    char c = *lx->cur;

    if (c == '\0') return make(lx, TK_EOF, start);

    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)*lx->cur) || *lx->cur == '_') lx->cur++;
        size_t n = (size_t)(lx->cur - start);
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
            if (strlen(keywords[i].word) == n && memcmp(keywords[i].word, start, n) == 0)
                return make(lx, keywords[i].kind, start);
        return make(lx, TK_IDENT, start);
    }

    if (isdigit((unsigned char)c)) {
        while (isdigit((unsigned char)*lx->cur)) lx->cur++;
        TokKind kind = TK_INT;
        if (*lx->cur == '.' && isdigit((unsigned char)lx->cur[1])) {
            kind = TK_FLOAT;
            lx->cur++;
            while (isdigit((unsigned char)*lx->cur)) lx->cur++;
        } else if (*lx->cur == '.' && !isalpha((unsigned char)lx->cur[1]) &&
                   lx->cur[1] != '_') {
            kind = TK_FLOAT; /* `3.` */
            lx->cur++;
        }
        if (*lx->cur == 'e' || *lx->cur == 'E') {
            const char *save = lx->cur;
            lx->cur++;
            if (*lx->cur == '+' || *lx->cur == '-') lx->cur++;
            if (isdigit((unsigned char)*lx->cur)) {
                kind = TK_FLOAT;
                while (isdigit((unsigned char)*lx->cur)) lx->cur++;
            } else {
                lx->cur = save; /* `1else` style: leave the e for the ident */
            }
        }
        return make(lx, kind, start);
    }

    if (c == '"' || c == '\'') {
        char quote = c;
        lx->cur++;
        while (*lx->cur && *lx->cur != quote) {
            if (*lx->cur == '\\' && lx->cur[1]) lx->cur++;
            if (*lx->cur == '\n') lx->line++;
            lx->cur++;
        }
        if (*lx->cur != quote) return make(lx, TK_ERROR, start); /* unterminated */
        lx->cur++;
        return make(lx, TK_STR, start); /* includes quotes; parser unescapes */
    }

    lx->cur++;
    switch (c) {
    case '{': return make(lx, TK_LBRACE, start);
    case '}': return make(lx, TK_RBRACE, start);
    case '(': return make(lx, TK_LPAREN, start);
    case ')': return make(lx, TK_RPAREN, start);
    case '[': return make(lx, TK_LBRACK, start);
    case ']': return make(lx, TK_RBRACK, start);
    case ',': return make(lx, TK_COMMA, start);
    case '.': return make(lx, TK_DOT, start);
    case ':': return make(lx, TK_COLON, start);
    case ';': return make(lx, TK_SEMI, start);
    case '|': return make(lx, TK_PIPE, start);
    case '&': return make(lx, TK_AMP, start);
    case '+': return make(lx, TK_PLUS, start);
    case '*': return make(lx, TK_STAR, start);
    case '/': return make(lx, TK_SLASH, start);
    case '%': return make(lx, TK_PERCENT, start);
    case '-':
        if (*lx->cur == '>') { lx->cur++; return make(lx, TK_ARROW, start); }
        return make(lx, TK_MINUS, start);
    case '=':
        if (*lx->cur == '=') { lx->cur++; return make(lx, TK_EQ, start); }
        return make(lx, TK_ASSIGN, start);
    case '!':
        if (*lx->cur == '=') { lx->cur++; return make(lx, TK_NE, start); }
        return make(lx, TK_ERROR, start);
    case '<':
        if (*lx->cur == '=') { lx->cur++; return make(lx, TK_LE, start); }
        return make(lx, TK_LT, start);
    case '>':
        if (*lx->cur == '=') { lx->cur++; return make(lx, TK_GE, start); }
        return make(lx, TK_GT, start);
    }
    return make(lx, TK_ERROR, start);
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
        [TK_PASS] = "PASS", [TK_TYPE] = "TYPE",
        [TK_LBRACE] = "LBRACE", [TK_RBRACE] = "RBRACE",
        [TK_LPAREN] = "LPAREN", [TK_RPAREN] = "RPAREN",
        [TK_LBRACK] = "LBRACK", [TK_RBRACK] = "RBRACK",
        [TK_COMMA] = "COMMA", [TK_DOT] = "DOT", [TK_COLON] = "COLON",
        [TK_SEMI] = "SEMI", [TK_ASSIGN] = "ASSIGN", [TK_ARROW] = "ARROW",
        [TK_PIPE] = "PIPE", [TK_AMP] = "AMP",
        [TK_PLUS] = "PLUS", [TK_MINUS] = "MINUS", [TK_STAR] = "STAR",
        [TK_SLASH] = "SLASH", [TK_PERCENT] = "PERCENT",
        [TK_EQ] = "EQ", [TK_NE] = "NE", [TK_LT] = "LT", [TK_LE] = "LE",
        [TK_GT] = "GT", [TK_GE] = "GE",
    };
    return names[k] ? names[k] : "?";
}
