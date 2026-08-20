/* Emerald lexer: hand-written scanner over an in-memory source buffer. */
#include "lexer.h"
#include <ctype.h>
#include <string.h>

void lexer_init(Lexer *lx, const char *src) {
  lx->src = src;
  lx->cur = src;
  lx->line = 1;
  lx->col = 1;
}
static const struct {
  const char *word;
  TokKind kind;
} keywords[] = {
    {"def", TK_DEF},        {"if", TK_IF},
    {"elif", TK_ELIF},      {"else", TK_ELSE},
    {"while", TK_WHILE},    {"for", TK_FOR},
    {"in", TK_IN},          {"return", TK_RETURN},
    {"and", TK_AND},        {"or", TK_OR},
    {"not", TK_NOT},        {"True", TK_TRUE},
    {"False", TK_FALSE},    {"None", TK_NONE},
    {"break", TK_BREAK},    {"continue", TK_CONTINUE},
    {"pass", TK_PASS},      {"type", TK_TYPE},
    {"const", TK_CONST},    {"match", TK_MATCH},
    {"pure", TK_PURE},      {"partial", TK_PARTIAL},
    {"import", TK_IMPORT},  {"from", TK_FROM},
    {"as", TK_AS},          {"dim", TK_DIM},
    {"error", TK_ERROR_KW}, {"try", TK_TRY},
    {"catch", TK_CATCH},
};
static Token make(Lexer *lx, TokKind k, const char *start, int col) {
  Token t = {k, start, (int)(lx->cur - start), lx->line, col};
  return t;
}
Token lexer_next(Lexer *lx) {
  for (;;) {
    char c = *lx->cur;
    if (c == '\n') {
      lx->line++;
      lx->col = 1;
      lx->cur++;
    } else if (c == ' ' || c == '\t' || c == '\r') {
      lx->col++;
      lx->cur++;
    } else if (c == '#') {
      while (*lx->cur && *lx->cur != '\n') {
        lx->col++;
        lx->cur++;
      }
    } else
      break;
  }
  const char *start = lx->cur;
  int col = lx->col;
  char c = *lx->cur;
  if (c == '\0')
    return make(lx, TK_EOF, start, col);
  if (c == 'f' && (lx->cur[1] == '"' || lx->cur[1] == '\'')) {
    char quote = lx->cur[1];
    lx->cur += 2;
    lx->col += 2;
    while (*lx->cur && *lx->cur != quote) {
      if (*lx->cur == '\\' && lx->cur[1]) {
        lx->cur += 2;
        lx->col += 2;
        continue;
      }
      if (*lx->cur == '\n') {
        lx->line++;
        lx->col = 1;
        lx->cur++;
      } else {
        lx->cur++;
        lx->col++;
      }
    }
    if (*lx->cur != quote)
      return make(lx, TK_ERROR, start, col);
    lx->cur++;
    lx->col++;
    return make(lx, TK_FSTR, start, col);
  }
  if (isalpha((unsigned char)c) || c == '_') {
    while (isalnum((unsigned char)*lx->cur) || *lx->cur == '_') {
      lx->cur++;
      lx->col++;
    }
    size_t n = (size_t)(lx->cur - start);
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
      if (strlen(keywords[i].word) == n && !memcmp(keywords[i].word, start, n))
        return make(lx, keywords[i].kind, start, col);
    return make(lx, TK_IDENT, start, col);
  }
  if (isdigit((unsigned char)c)) {
    while (isdigit((unsigned char)*lx->cur)) {
      lx->cur++;
      lx->col++;
    }
    TokKind k = TK_INT;
    if (*lx->cur == '.' && isdigit((unsigned char)lx->cur[1])) {
      k = TK_FLOAT;
      lx->cur++;
      lx->col++;
      while (isdigit((unsigned char)*lx->cur)) {
        lx->cur++;
        lx->col++;
      }
    } else if (*lx->cur == '.' && !isalpha((unsigned char)lx->cur[1]) &&
               lx->cur[1] != '_') {
      k = TK_FLOAT;
      lx->cur++;
      lx->col++;
    }
    if (*lx->cur == 'e' || *lx->cur == 'E') {
      const char *save = lx->cur;
      int sc = lx->col;
      lx->cur++;
      lx->col++;
      if (*lx->cur == '+' || *lx->cur == '-') {
        lx->cur++;
        lx->col++;
      }
      if (isdigit((unsigned char)*lx->cur)) {
        k = TK_FLOAT;
        while (isdigit((unsigned char)*lx->cur)) {
          lx->cur++;
          lx->col++;
        }
      } else {
        lx->cur = save;
        lx->col = sc;
      }
    }
    return make(lx, k, start, col);
  }
  if (c == '"' || c == '\'') {
    char q = c;
    lx->cur++;
    lx->col++;
    while (*lx->cur && *lx->cur != q) {
      if (*lx->cur == '\\' && lx->cur[1]) {
        lx->cur++;
        lx->col++;
      }
      if (*lx->cur == '\n') {
        lx->line++;
        lx->col = 1;
      } else
        lx->col++;
      lx->cur++;
    }
    if (*lx->cur != q)
      return make(lx, TK_ERROR, start, col);
    lx->cur++;
    lx->col++;
    return make(lx, TK_STR, start, col);
  }
  lx->cur++;
  lx->col++;
  switch (c) {
  case '{':
    return make(lx, TK_LBRACE, start, col);
  case '}':
    return make(lx, TK_RBRACE, start, col);
  case '(':
    return make(lx, TK_LPAREN, start, col);
  case ')':
    return make(lx, TK_RPAREN, start, col);
  case '[':
    return make(lx, TK_LBRACK, start, col);
  case ']':
    return make(lx, TK_RBRACK, start, col);
  case ',':
    return make(lx, TK_COMMA, start, col);
  case '.':
    return make(lx, TK_DOT, start, col);
  case ':':
    return make(lx, TK_COLON, start, col);
  case ';':
    return make(lx, TK_SEMI, start, col);
  case '|':
    if (*lx->cur == '>') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_PIPE_GT, start, col);
    }
    return make(lx, TK_PIPE, start, col);
  case '&':
    return make(lx, TK_AMP, start, col);
  case '^':
    return make(lx, TK_CARET, start, col);
  case '+':
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_PLUS_EQ, start, col);
    }
    return make(lx, TK_PLUS, start, col);
  case '*':
    if (*lx->cur == '*') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_POW, start, col);
    }
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_STAR_EQ, start, col);
    }
    return make(lx, TK_STAR, start, col);
  case '/':
    if (*lx->cur == '/') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_FLOORDIV, start, col);
    }
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_SLASH_EQ, start, col);
    }
    return make(lx, TK_SLASH, start, col);
  case '%':
    return make(lx, TK_PERCENT, start, col);
  case '-':
    if (*lx->cur == '>') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_ARROW, start, col);
    }
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_MINUS_EQ, start, col);
    }
    return make(lx, TK_MINUS, start, col);
  case '=':
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_EQ, start, col);
    }
    if (*lx->cur == '>') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_FAT_ARROW, start, col);
    }
    return make(lx, TK_ASSIGN, start, col);
  case '!':
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_NE, start, col);
    }
    return make(lx, TK_ERROR, start, col);
  case '<':
    if (*lx->cur == '<') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_LSHIFT, start, col);
    }
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_LE, start, col);
    }
    return make(lx, TK_LT, start, col);
  case '>':
    if (*lx->cur == '>') {
      lx->cur++;
      lx->col++;
      if (*lx->cur == '>') {
        lx->cur++;
        lx->col++;
        return make(lx, TK_RSHIFT, start, col);
      }
      return make(lx, TK_GTGT, start, col);
    }
    if (*lx->cur == '=') {
      lx->cur++;
      lx->col++;
      return make(lx, TK_GE, start, col);
    }
    return make(lx, TK_GT, start, col);
  case '?':
    return make(lx, TK_QUESTION, start, col);
  }
  return make(lx, TK_ERROR, start, col);
}
const char *token_kind_name(TokKind k) {
  static const char *n[TK__COUNT] = {[TK_EOF] = "EOF",
                                     [TK_ERROR] = "ERROR",
                                     [TK_INT] = "INT",
                                     [TK_FLOAT] = "FLOAT",
                                     [TK_STR] = "STR",
                                     [TK_FSTR] = "FSTR",
                                     [TK_IDENT] = "IDENT",
                                     [TK_DEF] = "DEF",
                                     [TK_IF] = "IF",
                                     [TK_ELIF] = "ELIF",
                                     [TK_ELSE] = "ELSE",
                                     [TK_WHILE] = "WHILE",
                                     [TK_FOR] = "FOR",
                                     [TK_IN] = "IN",
                                     [TK_RETURN] = "RETURN",
                                     [TK_AND] = "AND",
                                     [TK_OR] = "OR",
                                     [TK_NOT] = "NOT",
                                     [TK_TRUE] = "TRUE",
                                     [TK_FALSE] = "FALSE",
                                     [TK_NONE] = "NONE",
                                     [TK_BREAK] = "BREAK",
                                     [TK_CONTINUE] = "CONTINUE",
                                     [TK_PASS] = "PASS",
                                     [TK_TYPE] = "TYPE",
                                     [TK_CONST] = "CONST",
                                     [TK_MATCH] = "MATCH",
                                     [TK_PURE] = "PURE",
                                     [TK_PARTIAL] = "PARTIAL",
                                     [TK_IMPORT] = "IMPORT",
                                     [TK_FROM] = "FROM",
                                     [TK_AS] = "AS",
                                     [TK_DIM] = "DIM",
                                     [TK_ERROR_KW] = "ERROR_KW",
                                     [TK_TRY] = "TRY",
                                     [TK_CATCH] = "CATCH",
                                     [TK_LBRACE] = "LBRACE",
                                     [TK_RBRACE] = "RBRACE",
                                     [TK_LPAREN] = "LPAREN",
                                     [TK_RPAREN] = "RPAREN",
                                     [TK_LBRACK] = "LBRACK",
                                     [TK_RBRACK] = "RBRACK",
                                     [TK_COMMA] = "COMMA",
                                     [TK_DOT] = "DOT",
                                     [TK_COLON] = "COLON",
                                     [TK_SEMI] = "SEMI",
                                     [TK_ASSIGN] = "ASSIGN",
                                     [TK_ARROW] = "ARROW",
                                     [TK_PIPE] = "PIPE",
                                     [TK_AMP] = "AMP",
                                     [TK_PLUS] = "PLUS",
                                     [TK_MINUS] = "MINUS",
                                     [TK_STAR] = "STAR",
                                     [TK_SLASH] = "SLASH",
                                     [TK_PERCENT] = "PERCENT",
                                     [TK_FLOORDIV] = "FLOORDIV",
                                     [TK_POW] = "POW",
                                     [TK_CARET] = "CARET",
                                     [TK_LSHIFT] = "LSHIFT",
                                     [TK_RSHIFT] = "RSHIFT",
                                     [TK_PLUS_EQ] = "PLUS_EQ",
                                     [TK_MINUS_EQ] = "MINUS_EQ",
                                     [TK_STAR_EQ] = "STAR_EQ",
                                     [TK_SLASH_EQ] = "SLASH_EQ",
                                     [TK_EQ] = "EQ",
                                     [TK_NE] = "NE",
                                     [TK_LT] = "LT",
                                     [TK_LE] = "LE",
                                     [TK_GT] = "GT",
                                     [TK_GE] = "GE",
                                     [TK_FAT_ARROW] = "FAT_ARROW",
                                     [TK_PIPE_GT] = "PIPE_GT",
                                     [TK_GTGT] = "GTGT",
                                     [TK_QUESTION] = "QUESTION"};
  return n[k] ? n[k] : "?";
}
