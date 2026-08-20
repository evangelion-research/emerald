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

/* --- infrastructure ----------------------------------------------------- */

static void perror_at(Parser *p, int line, int col, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_add(p->diags, DIA_SYNTAX, "E_SYNTAX", p->filename, line, col,
             "%s", msg);
    diag_render(p->diags, stderr);
    exit(1);
}

static void advance(Parser *p) {
    p->cur = lexer_next(&p->lx);
    if (p->cur.kind == TK_ERROR)
        perror_at(p, p->cur.line, p->cur.col,
                  "unrecognized or unterminated token starting at '%.1s'",
                  p->cur.start);
}

static bool check(Parser *p, TokKind k) { return p->cur.kind == k; }

static bool match(Parser *p, TokKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

/* snapshot/restore the parser+lexer position, for the one ambiguous case in
 * the grammar: `(x) => ...` is a lambda but `(x) + 1` is a grouping. */
typedef struct { Token cur; Lexer lx; } PSave;

static PSave psave(Parser *p) {
    PSave s;
    s.cur = p->cur;
    s.lx = p->lx;
    return s;
}

static void prestore(Parser *p, PSave s) {
    p->cur = s.cur;
    p->lx = s.lx;
}

static Token expect(Parser *p, TokKind k, const char *what) {
    if (!check(p, k))
        perror_at(p, p->cur.line, p->cur.col, "expected %s, got '%.*s'", what,
                  p->cur.len ? p->cur.len : 5,
                  p->cur.kind == TK_EOF ? "<eof>" : p->cur.start);
    Token t = p->cur;
    advance(p);
    return t;
}

static char *tok_text(Token t) {
    char *s = xmalloc((size_t)t.len + 1);
    memcpy(s, t.start, (size_t)t.len);
    s[t.len] = '\0';
    return s;
}

static char *unescape_string(Parser *p, Token t) {
    /* t includes the surrounding quotes */
    char *out = xmalloc((size_t)t.len); /* result is never longer than input */
    const char *s = t.start + 1, *end = t.start + t.len - 1;
    size_t n = 0;
    while (s < end) {
        char c = *s++;
        if (c == '\\' && s < end) {
            char e = *s++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '"': c = '"'; break;
            case '0': c = '\0'; break;
            default:
                perror_at(p, t.line, t.col, "unknown escape sequence '\\%c'", e);
            }
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return out;
}

/* growable pointer array */
typedef struct { void **items; size_t count, cap; } PtrVec;
static void vec_push(PtrVec *v, void *item) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, sizeof(void *) * v->cap);
    }
    v->items[v->count++] = item;
}

static Expr *new_expr(ExprKind k, int line, int col) {
    Expr *e = xmalloc(sizeof(Expr));
    memset(e, 0, sizeof(Expr));
    e->kind = k;
    e->line = line;
    e->col = col;
    return e;
}

static Stmt *new_stmt(Parser *p, StmtKind k, int line, int col) {
    Stmt *s = xmalloc(sizeof(Stmt));
    memset(s, 0, sizeof(Stmt));
    s->kind = k;
    s->line = line;
    s->col = col;
    s->file = p->filename;
    return s;
}

static TypeExpr *new_type(TypeExprKind k, int line, int col) {
    TypeExpr *t = xmalloc(sizeof(TypeExpr));
    memset(t, 0, sizeof(TypeExpr));
    t->kind = k;
    t->line = line;
    t->col = col;
    return t;
}

/* --- type expressions ---------------------------------------------------
 * type      := inter ("|" inter)*
 * inter     := type_atom ("&" type_atom)*
 * type_atom := IDENT ("[" type ("," type)* "]")?
 *            | "None" | "{" fields "}" | "(" type ")"
 *            | INT | "-" INT | STR | "True" | "False"      (literal types)
 */
static TypeExpr *parse_type(Parser *p);
static char *unescape_string(Parser *p, Token t);

/* --- dimension expressions (inside a tensor shape) -----------------------
 * dim_expr := dim_term (("+"|"-") dim_term)*
 * dim_term := dim_factor ("*" dim_factor)*
 * dim_factor := IDENT | INT
 * Only `+` and `*` are supported (SPEC_V2.md D3); a subtraction use case has
 * not appeared in the target ops. */
static DimExpr *parse_dim_factor(Parser *p) {
    if (check(p, TK_INT)) {
        Token t = p->cur;
        advance(p);
        return dim_lit(strtoll(tok_text(t), NULL, 10));
    }
    Token n = expect(p, TK_IDENT, "dimension name or literal");
    return dim_var(tok_text(n));
}

static DimExpr *parse_dim_term(Parser *p) {
    DimExpr *e = parse_dim_factor(p);
    while (check(p, TK_STAR)) {
        advance(p);
        e = dim_mul(e, parse_dim_factor(p));
    }
    return e;
}

static DimExpr *parse_dim_expr(Parser *p) {
    DimExpr *e = parse_dim_term(p);
    while (check(p, TK_PLUS)) {
        advance(p);
        e = dim_add(e, parse_dim_term(p));
    }
    return e;
}

static TypeExpr *parse_type_atom(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    if (match(p, TK_LPAREN)) {
        if (check(p, TK_RPAREN)) { /* "() -> R": zero-arg function type */
            advance(p);
            expect(p, TK_ARROW, "'->' in function type");
            TypeExpr *t = new_type(TE_FUNC, line, col);
            t->fun.params = NULL;
            t->fun.param_count = 0;
            t->fun.ret = parse_type(p);
            return t;
        }
        TypeExpr *first = parse_type(p);
        if (match(p, TK_COMMA)) { /* "(A, B, ...) -> R" */
            PtrVec params = {0};
            vec_push(&params, first);
            while (!check(p, TK_RPAREN)) {
                vec_push(&params, parse_type(p));
                if (!match(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN, "')' closing function type parameters");
            expect(p, TK_ARROW, "'->' in function type");
            TypeExpr *t = new_type(TE_FUNC, line, col);
            t->fun.params = (TypeExpr **)params.items;
            t->fun.param_count = params.count;
            t->fun.ret = parse_type(p);
            return t;
        }
        expect(p, TK_RPAREN, "')' in type");
        if (match(p, TK_ARROW)) { /* "(A) -> R" */
            TypeExpr *t = new_type(TE_FUNC, line, col);
            t->fun.params = xmalloc(sizeof(TypeExpr *));
            t->fun.params[0] = first;
            t->fun.param_count = 1;
            t->fun.ret = parse_type(p);
            return t;
        }
        return first; /* "(T)" grouping */
    }
    if (match(p, TK_NONE)) {
        TypeExpr *t = new_type(TE_NAME, line, col);
        t->name = "None";
        return t;
    }
    if (check(p, TK_INT) || check(p, TK_MINUS)) {
        bool neg = match(p, TK_MINUS);
        Token n = expect(p, TK_INT, "integer literal type");
        TypeExpr *t = new_type(TE_LIT, line, col);
        t->lit.kind = LIT_INT;
        t->lit.ival = strtoll(tok_text(n), NULL, 10);
        if (neg) t->lit.ival = -t->lit.ival;
        return t;
    }
    if (check(p, TK_STR)) {
        TypeExpr *t = new_type(TE_LIT, line, col);
        t->lit.kind = LIT_STR;
        t->lit.sval = unescape_string(p, p->cur);
        advance(p);
        return t;
    }
    if (check(p, TK_TRUE) || check(p, TK_FALSE)) {
        TypeExpr *t = new_type(TE_LIT, line, col);
        t->lit.kind = LIT_BOOL;
        t->lit.ival = check(p, TK_TRUE) ? 1 : 0;
        advance(p);
        return t;
    }
    if (check(p, TK_FLOAT))
        perror_at(p, line, col, "float literal types are not supported "
                  "(only int, str, and bool literals can be types)");
    if (check(p, TK_LBRACE)) {
        advance(p);
        TypeExpr *t = new_type(TE_REC, line, col);
        PtrVec names = {0}, types = {0};
        while (!check(p, TK_RBRACE)) {
            Token n = expect(p, TK_IDENT, "field name in record type");
            expect(p, TK_COLON, "':' after field name");
            vec_push(&names, tok_text(n));
            vec_push(&types, parse_type(p));
            if (!match(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE, "'}' closing record type");
        t->fields.names = (char **)names.items;
        t->fields.types = (TypeExpr **)types.items;
        t->fields.count = names.count;
        return t;
    }
    Token n = expect(p, TK_IDENT, "type name");
    if (n.len == 6 && memcmp(n.start, "Tensor", 6) == 0 && check(p, TK_LBRACK)) {
        /* Tensor[dtype, [dim, ...]] or Tensor[dtype, ?] */
        advance(p); /* [ */
        TypeExpr *t = new_type(TE_TENSOR, line, col);
        t->tensor.dtype = parse_type(p);
        expect(p, TK_COMMA, "',' after the tensor dtype");
        if (match(p, TK_QUESTION)) {
            t->tensor.dynamic = true;
            t->tensor.shape = NULL;
            t->tensor.shape_count = 0;
        } else {
            expect(p, TK_LBRACK, "'[' opening the tensor shape");
            PtrVec dims = {0};
            while (!check(p, TK_RBRACK)) {
                vec_push(&dims, parse_dim_expr(p));
                if (!match(p, TK_COMMA)) break;
            }
            expect(p, TK_RBRACK, "']' closing the tensor shape");
            t->tensor.shape = (DimExpr **)dims.items;
            t->tensor.shape_count = dims.count;
        }
        expect(p, TK_RBRACK, "']' closing the tensor type");
        return t;
    }
    if (n.len == 4 && memcmp(n.start, "list", 4) == 0 && check(p, TK_LBRACK)) {
        advance(p);
        TypeExpr *t = new_type(TE_LIST, line, col);
        t->elem = parse_type(p);
        expect(p, TK_RBRACK, "']' closing list type");
        return t;
    }
    if (n.len == 3 && memcmp(n.start, "seq", 3) == 0 && check(p, TK_LBRACK)) {
        advance(p);
        TypeExpr *t = new_type(TE_SEQ, line, col);
        t->elem = parse_type(p);
        expect(p, TK_RBRACK, "']' closing seq type");
        return t;
    }
    if (n.len == 3 && memcmp(n.start, "Fin", 3) == 0 && check(p, TK_LBRACK)) {
        /* Fin[n]: an index provably below the dimension `n` */
        advance(p);
        TypeExpr *t = new_type(TE_FIN, line, col);
        t->fin_dim = parse_dim_expr(p);
        expect(p, TK_RBRACK, "']' closing Fin[n]");
        return t;
    }
    if (n.len == 2 && memcmp(n.start, "Eq", 2) == 0 && check(p, TK_LBRACK)) {
        /* Eq[a, b]: propositional equality of two dim expressions */
        advance(p);
        TypeExpr *t = new_type(TE_EQ, line, col);
        t->eq_lhs = parse_dim_expr(p);
        expect(p, TK_COMMA, "',' between the two sides of Eq[a, b]");
        t->eq_rhs = parse_dim_expr(p);
        expect(p, TK_RBRACK, "']' closing Eq[a, b]");
        return t;
    }
    TypeExpr *t = new_type(TE_NAME, line, col);
    t->name = tok_text(n);
    if (check(p, TK_DOT)) /* `m.T`: types are not reachable through a module object */
        perror_at(p, p->cur.line, p->cur.col,
                  "qualified type names are not supported; write "
                  "'from %s import <type>' and use the type directly", t->name);
    if (match(p, TK_LBRACK)) { /* generic application: Pair[int, str] */
        PtrVec args = {0};
        do {
            vec_push(&args, parse_type(p));
        } while (match(p, TK_COMMA));
        expect(p, TK_RBRACK, "']' closing type arguments");
        t->args = (TypeExpr **)args.items;
        t->arg_count = args.count;
    }
    return t;
}

/* `[A, B, C]` after a `def` or `type` name: generic parameter list. A
 * parameter may be kinded `name: dim` (a nominal dimension); `out_dims[i]`
 * is true exactly for those. */
static void parse_type_params(Parser *p, char ***out, bool **out_dims,
                              size_t *out_count) {
    PtrVec params = {0};
    bool *dims = NULL;
    size_t ndims = 0, capdims = 0;
    if (match(p, TK_LBRACK)) {
        do {
            Token n = expect(p, TK_IDENT, "type parameter name");
            vec_push(&params, tok_text(n));
            bool is_dim = false;
            if (match(p, TK_COLON)) {
                if (!match(p, TK_DIM))
                    perror_at(p, p->cur.line, p->cur.col,
                              "expected 'dim' kind, got '%.*s'",
                              p->cur.len ? p->cur.len : 5,
                              p->cur.kind == TK_EOF ? "<eof>" : p->cur.start);
                is_dim = true;
            }
            if (ndims == capdims) {
                capdims = capdims ? capdims * 2 : 4;
                dims = xrealloc(dims, sizeof(bool) * capdims);
            }
            dims[ndims++] = is_dim;
        } while (match(p, TK_COMMA));
        expect(p, TK_RBRACK, "']' closing type parameter list");
    }
    *out = (char **)params.items;
    *out_dims = ndims ? dims : NULL;
    if (!ndims) free(dims);
    *out_count = params.count;
}

static TypeExpr *parse_type_inter(Parser *p) {
    TypeExpr *t = parse_type_atom(p);
    while (check(p, TK_AMP)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        TypeExpr *i = new_type(TE_INTER, line, col);
        i->lhs = t;
        i->rhs = parse_type_atom(p);
        t = i;
    }
    return t;
}

static TypeExpr *parse_type(Parser *p) {
    TypeExpr *t = parse_type_inter(p);
    while (check(p, TK_PIPE)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        TypeExpr *u = new_type(TE_UNION, line, col);
        u->lhs = t;
        u->rhs = parse_type_inter(p);
        t = u;
    }
    return t;
}

/* --- expressions -------------------------------------------------------- */

static Expr *parse_expr(Parser *p);
static Expr *parse_unary(Parser *p);

static Expr *parse_embedded_expr(Parser *p, const char *src, size_t n) {
    char *text = xmalloc(n + 1);
    memcpy(text, src, n);
    text[n] = '\0';
    Program *sub = parse_program(text, p->filename, p->diags);
    if (!sub || sub->body.count != 1 || sub->body.items[0]->kind != S_EXPR)
        perror_at(p, p->cur.line, p->cur.col, "invalid expression in f-string");
    return sub->body.items[0]->as.expr;
}

static char *ftext_decode(const char *s, size_t n) {
    char *out = xmalloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char c = s[++i];
            switch (c) {
            case 'n': out[k++] = '\n'; break; case 't': out[k++] = '\t'; break;
            case 'r': out[k++] = '\r'; break; case '0': out[k++] = '\0'; break;
            default: out[k++] = c; break;
            }
        } else if (s[i] == '{' && i + 1 < n && s[i + 1] == '{') {
            out[k++] = '{'; i++;
        } else if (s[i] == '}' && i + 1 < n && s[i + 1] == '}') {
            out[k++] = '}'; i++;
        } else out[k++] = s[i];
    }
    out[k] = '\0';
    return out;
}

static Expr *parse_fstr(Parser *p, Token tok) {
    Expr *e = new_expr(E_FSTR, tok.line, tok.col);
    const char *s = tok.start + 2; /* prefix and opening quote */
    size_t n = (size_t)tok.len - 3; /* closing quote */
    PtrVec texts = {0}, exprs = {0};
    size_t start = 0, i = 0;
    while (i < n) {
        if (s[i] == '{' && !(i + 1 < n && s[i + 1] == '{')) {
            vec_push(&texts, ftext_decode(s + start, i - start));
            size_t j = i + 1, depth = 0;
            for (; j < n; j++) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}' && depth == 0) break;
                else if (s[j] == '}') depth--;
            }
            if (j == n) perror_at(p, tok.line, tok.col, "unclosed '{' in f-string");
            vec_push(&exprs, parse_embedded_expr(p, s + i + 1, j - i - 1));
            i = j + 1;
            start = i;
        } else if (s[i] == '}' && !(i + 1 < n && s[i + 1] == '}')) {
            perror_at(p, tok.line, tok.col, "single '}' in f-string");
        } else i++;
    }
    vec_push(&texts, ftext_decode(s + start, n - start));
    e->as.fstr.texts = (char **)texts.items;
    e->as.fstr.exprs = (Expr **)exprs.items;
    e->as.fstr.count = exprs.count;
    advance(p);
    return e;
}
static Expr *bin(Parser *p, BinOp op, int line, int col, Expr *lhs, Expr *rhs);

/* `(a: int, b) => body` or `() => body`: the caller has already consumed
 * `(`. Returns NULL when the tokens are not a lambda, so the caller can
 * backtrack. */
static Expr *try_parse_lambda(Parser *p, int line, int col) {
    PtrVec params = {0}, ptypes = {0};
    if (check(p, TK_IDENT)) {
        for (;;) {
            Token pn = p->cur;
            advance(p);
            vec_push(&params, tok_text(pn));
            vec_push(&ptypes, match(p, TK_COLON) ? parse_type(p) : NULL);
            if (!match(p, TK_COMMA)) break;
        }
    }
    if (!match(p, TK_RPAREN)) return NULL;
    if (!match(p, TK_FAT_ARROW)) return NULL;
    Expr *e = new_expr(E_LAMBDA, line, col);
    e->as.lam.params = (char **)params.items;
    e->as.lam.param_types = (TypeExpr **)ptypes.items;
    e->as.lam.param_count = params.count;
    e->as.lam.body = parse_expr(p);
    return e;
}

static Expr *parse_header_expr(Parser *p);

/* Is the parser looking at `Tag { field: v, ... }` or `Tag {}` — a tagged
 * error literal — rather than a name followed by a block? Deciding needs
 * three tokens of lookahead, so snapshot and restore around the probe. */
static bool tagged_rec_ahead(Parser *p) {
    PSave save = psave(p);
    bool yes = false;
    advance(p); /* past the name */
    if (check(p, TK_LBRACE)) {
        advance(p);
        if (check(p, TK_RBRACE)) {
            yes = true;
        } else if (check(p, TK_IDENT)) {
            advance(p);
            yes = check(p, TK_COLON);
        }
    }
    prestore(p, save);
    return yes;
}

/* `catch subject { Tag bind -> expr, _ -> expr }`: handle every error the
 * subject can produce. Arm bodies are single expressions, like lambda
 * bodies; separating commas are optional. */
static Expr *parse_catch(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* catch */
    Expr *e = new_expr(E_CATCH, line, col);
    e->as.ctch.subject = parse_header_expr(p);
    expect(p, TK_LBRACE, "'{' opening catch arms");
    CatchArm *arms = NULL;
    size_t n = 0, cap = 0;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            arms = xrealloc(arms, sizeof(CatchArm) * cap);
        }
        CatchArm *a = &arms[n++];
        memset(a, 0, sizeof *a);
        a->line = p->cur.line;
        a->col = p->cur.col;
        Token t = expect(p, TK_IDENT, "an error type name or '_' in a catch arm");
        a->tag = tok_text(t);
        if (strcmp(a->tag, "_") == 0) a->tag = NULL; /* the catch-all arm */
        if (check(p, TK_IDENT)) {
            a->bind = tok_text(p->cur);
            advance(p);
        }
        expect(p, TK_ARROW, "'->' after a catch arm's error type");
        a->body = parse_expr(p);
        match(p, TK_COMMA); /* optional separator */
    }
    expect(p, TK_RBRACE, "'}' closing catch");
    if (n == 0) perror_at(p, line, col, "catch needs at least one arm");
    e->as.ctch.arms = arms;
    e->as.ctch.count = n;
    return e;
}

static Expr *parse_primary(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    switch (p->cur.kind) {
    case TK_INT: {
        Expr *e = new_expr(E_INT, line, col);
        e->as.ival = strtoll(tok_text(p->cur), NULL, 10);
        advance(p);
        return e;
    }
    case TK_FLOAT: {
        Expr *e = new_expr(E_FLOAT, line, col);
        e->as.fval = strtod(tok_text(p->cur), NULL);
        advance(p);
        return e;
    }
    case TK_STR: {
        Expr *e = new_expr(E_STR, line, col);
        e->as.sval = unescape_string(p, p->cur);
        advance(p);
        return e;
    }
    case TK_FSTR:
        return parse_fstr(p, p->cur);
    case TK_TRUE:  advance(p); return new_expr(E_TRUE, line, col);
    case TK_FALSE: advance(p); return new_expr(E_FALSE, line, col);
    case TK_NONE:  advance(p); return new_expr(E_NONE, line, col);
    case TK_CATCH:
        return parse_catch(p);
    case TK_IDENT: {
        if (!p->no_rec && tagged_rec_ahead(p)) {
            /* `ParseError { line: 1 }` — a record carrying the discriminant
             * `_tag: "ParseError"`, which is exactly what `error ParseError`
             * declared as a type. Construction needs no special node. */
            char *tag = tok_text(p->cur);
            advance(p);
            advance(p); /* past '{' */
            Expr *e = new_expr(E_REC, line, col);
            PtrVec names = {0}, values = {0};
            Expr *tage = new_expr(E_STR, line, col);
            tage->as.sval = tag;
            vec_push(&names, (char *)"_tag");
            vec_push(&values, tage);
            bool saved = p->no_rec;
            p->no_rec = false;
            while (!check(p, TK_RBRACE)) {
                Token nm = expect(p, TK_IDENT, "field name in error literal");
                expect(p, TK_COLON, "':' after field name");
                vec_push(&names, tok_text(nm));
                vec_push(&values, parse_expr(p));
                if (!match(p, TK_COMMA)) break;
            }
            p->no_rec = saved;
            expect(p, TK_RBRACE, "'}' closing error literal");
            e->as.rec.names = (char **)names.items;
            e->as.rec.values = (Expr **)values.items;
            e->as.rec.count = names.count;
            return e;
        }
        Expr *e = new_expr(E_NAME, line, col);
        e->as.sval = tok_text(p->cur);
        advance(p);
        return e;
    }
    case TK_LPAREN: {
        /* `(a: int, b) => body` is a lambda; `(expr)` is a grouping. The two
         * only overlap on `(x) => ...` vs `(x) + 1`, which we resolve by
         * tentatively parsing a lambda and backtracking if there is no `=>`. */
        PSave save = psave(p);
        advance(p);
        if (check(p, TK_IDENT) || check(p, TK_RPAREN)) {
            Expr *lam = try_parse_lambda(p, line, col);
            if (lam) return lam;
        }
        prestore(p, save);
        advance(p);
        if (check(p, TK_RPAREN)) {
            advance(p);
            Expr *e = new_expr(E_TUPLE, line, col);
            e->as.list.items = NULL;
            e->as.list.count = 0;
            return e;
        }
        bool saved = p->no_rec;
        p->no_rec = false; /* parens re-allow record literals */
        Expr *first = parse_expr(p);
        if (!match(p, TK_COMMA)) {
            p->no_rec = saved;
            expect(p, TK_RPAREN, "')'");
            return first;
        }
        Expr *e = new_expr(E_TUPLE, line, col);
        PtrVec items = {0};
        vec_push(&items, first);
        while (!check(p, TK_RPAREN)) {
            vec_push(&items, parse_expr(p));
            if (!match(p, TK_COMMA)) break;
        }
        p->no_rec = saved;
        expect(p, TK_RPAREN, "')' closing tuple");
        e->as.list.items = (Expr **)items.items;
        e->as.list.count = items.count;
        return e;
    }
    case TK_LBRACK: {
        advance(p);
        Expr *e = new_expr(E_LIST, line, col);
        PtrVec items = {0};
        bool saved = p->no_rec;
        p->no_rec = false;
        if (!check(p, TK_RBRACK)) {
            Expr *first = parse_expr(p);
            if (match(p, TK_FOR)) {
                Token vn = expect(p, TK_IDENT, "comprehension variable");
                expect(p, TK_IN, "'in' in comprehension");
                Expr *seq = parse_expr(p);
                Expr *cond = NULL;
                if (match(p, TK_IF)) cond = parse_expr(p);
                expect(p, TK_RBRACK, "']' closing comprehension");
                e->kind = E_COMP;
                e->as.comp.kind = COMP_LIST;
                e->as.comp.var = tok_text(vn);
                e->as.comp.seq = seq;
                e->as.comp.cond = cond;
                e->as.comp.elt = first;
                e->as.comp.key = NULL;
                p->no_rec = saved;
                return e;
            }
            vec_push(&items, first);
            while (!check(p, TK_RBRACK)) {
                if (!match(p, TK_COMMA)) break;
                if (check(p, TK_RBRACK)) break;
                vec_push(&items, parse_expr(p));
            }
        }
        p->no_rec = saved;
        expect(p, TK_RBRACK, "']' closing list");
        e->as.list.items = (Expr **)items.items;
        e->as.list.count = items.count;
        return e;
    }
    case TK_LBRACE: {
        if (p->no_rec)
            perror_at(p, line, col, "record literal not allowed here "
                      "(wrap it in parentheses, or did you mean a block?)");
        advance(p);
        bool is_record = false;
        if (check(p, TK_IDENT)) {
            PSave probe = psave(p);
            advance(p);
            is_record = check(p, TK_COLON);
            prestore(p, probe);
        }
        bool saved = p->no_rec;
        p->no_rec = false;
        if (is_record) {
            Expr *e = new_expr(E_REC, line, col);
            PtrVec names = {0}, values = {0};
            while (!check(p, TK_RBRACE)) {
                Token n = expect(p, TK_IDENT, "field name in record literal");
                expect(p, TK_COLON, "':' after field name");
                char *ntext = tok_text(n);
                vec_push(&names, ntext);
                Expr *value = parse_expr(p);
                vec_push(&values, value);
                if (match(p, TK_FOR)) {
                    Token vn = expect(p, TK_IDENT, "comprehension variable");
                    expect(p, TK_IN, "'in' in comprehension");
                    Expr *seq = parse_expr(p); Expr *cond = NULL;
                    if (match(p, TK_IF)) cond = parse_expr(p);
                    expect(p, TK_RBRACE, "'}' closing dict comprehension");
                    Expr *key = new_expr(E_NAME, n.line, n.col); key->as.sval = ntext;
                    e->kind = E_COMP; e->as.comp.kind = COMP_DICT; e->as.comp.var = tok_text(vn);
                    e->as.comp.seq = seq; e->as.comp.cond = cond; e->as.comp.key = key; e->as.comp.elt = value;
                    p->no_rec = saved; return e;
                }
                if (!match(p, TK_COMMA)) break;
            }
            p->no_rec = saved;
            expect(p, TK_RBRACE, "'}' closing record literal");
            e->as.rec.names = (char **)names.items;
            e->as.rec.values = (Expr **)values.items;
            e->as.rec.count = names.count;
            return e;
        }
        /* `{}` is an empty dict; non-record braces are dicts or sets. */
        if (check(p, TK_RBRACE)) {
            advance(p);
            Expr *e = new_expr(E_DICT, line, col);
            e->as.dict.keys = e->as.dict.values = NULL;
            e->as.dict.count = 0;
            p->no_rec = saved;
            return e;
        }
        Expr *first = parse_expr(p);
        if (match(p, TK_FOR)) {
            Token vn = expect(p, TK_IDENT, "comprehension variable");
            expect(p, TK_IN, "'in' in comprehension");
            Expr *seq = parse_expr(p);
            Expr *cond = NULL;
            if (match(p, TK_IF)) cond = parse_expr(p);
            expect(p, TK_RBRACE, "'}' closing set comprehension");
            Expr *e = new_expr(E_COMP, line, col);
            e->as.comp.kind = COMP_SET;
            e->as.comp.var = tok_text(vn); e->as.comp.seq = seq;
            e->as.comp.cond = cond; e->as.comp.elt = first; e->as.comp.key = NULL;
            p->no_rec = saved;
            return e;
        }
        if (match(p, TK_COLON)) {
            Expr *value = parse_expr(p);
            Expr *e = new_expr(E_DICT, line, col);
            PtrVec keys = {0}, vals = {0};
            vec_push(&keys, first); vec_push(&vals, value);
            if (match(p, TK_FOR)) {
                Token vn = expect(p, TK_IDENT, "comprehension variable");
                expect(p, TK_IN, "'in' in comprehension");
                Expr *seq = parse_expr(p); Expr *cond = NULL;
                if (match(p, TK_IF)) cond = parse_expr(p);
                expect(p, TK_RBRACE, "'}' closing dict comprehension");
                e->kind = E_COMP; e->as.comp.kind = COMP_DICT;
                e->as.comp.var = tok_text(vn); e->as.comp.seq = seq;
                e->as.comp.cond = cond; e->as.comp.key = first;
                e->as.comp.elt = value;
                p->no_rec = saved; return e;
            }
            while (!check(p, TK_RBRACE)) {
                if (!match(p, TK_COMMA)) break;
                if (check(p, TK_RBRACE)) break;
                vec_push(&keys, parse_expr(p));
                expect(p, TK_COLON, "':' in dict literal");
                vec_push(&vals, parse_expr(p));
            }
            expect(p, TK_RBRACE, "'}' closing dict literal");
            e->as.dict.keys = (Expr **)keys.items; e->as.dict.values = (Expr **)vals.items;
            e->as.dict.count = keys.count; p->no_rec = saved; return e;
        }
        Expr *e = new_expr(E_SET, line, col);
        PtrVec items = {0}; vec_push(&items, first);
        while (!check(p, TK_RBRACE)) {
            if (!match(p, TK_COMMA)) break;
            if (check(p, TK_RBRACE)) break;
            vec_push(&items, parse_expr(p));
        }
        expect(p, TK_RBRACE, "'}' closing set literal");
        e->as.set.items = (Expr **)items.items; e->as.set.count = items.count;
        p->no_rec = saved; return e;
    }
    default:
        perror_at(p, line, col, "expected an expression, got '%.*s'",
                  p->cur.len ? p->cur.len : 5,
                  p->cur.kind == TK_EOF ? "<eof>" : p->cur.start);
        return NULL; /* unreachable */
    }
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        int line = p->cur.line, col = p->cur.col;
        if (check(p, TK_LPAREN)) {
            advance(p);
            Expr *call = new_expr(E_CALL, line, col);
            call->as.call.fn = e;
            PtrVec args = {0}, arg_names = {0};
            bool saved = p->no_rec;
            p->no_rec = false;
            while (!check(p, TK_RPAREN)) {
                char *arg_name = NULL;
                if (check(p, TK_IDENT)) {
                    PSave probe = psave(p);
                    Token nm = p->cur; advance(p);
                    if (match(p, TK_ASSIGN)) arg_name = tok_text(nm);
                    else prestore(p, probe);
                }
                vec_push(&arg_names, arg_name);
                vec_push(&args, parse_expr(p));
                if (!match(p, TK_COMMA)) break;
            }
            p->no_rec = saved;
            expect(p, TK_RPAREN, "')' closing call");
            call->as.call.args = (Expr **)args.items;
            call->as.call.arg_names = (char **)arg_names.items;
            call->as.call.count = args.count;
            e = call;
        } else if (check(p, TK_LBRACK)) {
            advance(p);
            bool saved = p->no_rec;
            p->no_rec = false;
            Expr *start = NULL, *stop = NULL, *step = NULL;
            if (!check(p, TK_COLON)) start = parse_expr(p);
            if (match(p, TK_COLON)) {
                if (!check(p, TK_COLON) && !check(p, TK_RBRACK)) stop = parse_expr(p);
                if (match(p, TK_COLON) && !check(p, TK_RBRACK)) step = parse_expr(p);
                expect(p, TK_RBRACK, "']' closing slice");
                Expr *sl = new_expr(E_SLICE, line, col);
                sl->as.slice.seq = e; sl->as.slice.start = start;
                sl->as.slice.stop = stop; sl->as.slice.step = step;
                e = sl;
            } else {
                Expr *ix = new_expr(E_INDEX, line, col);
                ix->as.index.seq = e; ix->as.index.idx = start;
                expect(p, TK_RBRACK, "']' closing index");
                e = ix;
            }
            p->no_rec = saved;
        } else if (check(p, TK_DOT)) {
            advance(p);
            Token n = expect(p, TK_IDENT, "field name after '.'");
            Expr *at = new_expr(E_ATTR, line, col);
            at->as.attr.obj = e;
            at->as.attr.name = tok_text(n);
            e = at;
        } else {
            return e;
        }
    }
}

/* `a ** b`: exponentiation. Right-associative and tighter than unary minus,
 * Python-style: `-2 ** 2` is `-(2 ** 2)`, and `2 ** 3 ** 2` is `2 ** (3 ** 2)`. */
static Expr *parse_power(Parser *p) {
    Expr *e = parse_postfix(p);
    if (check(p, TK_POW)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        e = bin(p, B_POW, line, col, e, parse_unary(p));
    }
    return e;
}

static Expr *parse_unary(Parser *p) {
    if (check(p, TK_TRY)) {
        /* `try e` unwraps a Result, returning its error from the enclosing
         * function when there is one. It binds tighter than every binary
         * operator, so `try f(x) |> g` pipes the *unwrapped* value. */
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *e = new_expr(E_TRY, line, col);
        e->as.try_expr = parse_unary(p);
        return e;
    }
    if (check(p, TK_MINUS)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *e = new_expr(E_UNOP, line, col);
        e->as.un.op = U_NEG;
        e->as.un.operand = parse_unary(p);
        return e;
    }
    return parse_power(p);
}

static Expr *bin(Parser *p, BinOp op, int line, int col, Expr *lhs, Expr *rhs) {
    (void)p;
    Expr *e = new_expr(E_BINOP, line, col);
    e->as.bin.op = op;
    e->as.bin.lhs = lhs;
    e->as.bin.rhs = rhs;
    return e;
}

static Expr *parse_mul(Parser *p) {
    Expr *e = parse_unary(p);
    for (;;) {
        int line = p->cur.line, col = p->cur.col;
        if (match(p, TK_STAR))            e = bin(p, B_MUL, line, col, e, parse_unary(p));
        else if (match(p, TK_SLASH))      e = bin(p, B_DIV, line, col, e, parse_unary(p));
        else if (match(p, TK_FLOORDIV))   e = bin(p, B_FLOORDIV, line, col, e, parse_unary(p));
        else if (match(p, TK_PERCENT))    e = bin(p, B_MOD, line, col, e, parse_unary(p));
        else return e;
    }
}

static Expr *parse_add(Parser *p) {
    Expr *e = parse_mul(p);
    for (;;) {
        int line = p->cur.line, col = p->cur.col;
        if (match(p, TK_PLUS))       e = bin(p, B_ADD, line, col, e, parse_mul(p));
        else if (match(p, TK_MINUS)) e = bin(p, B_SUB, line, col, e, parse_mul(p));
        else return e;
    }
}

static Expr *parse_shift(Parser *p) {
    Expr *e = parse_add(p);
    while (check(p, TK_LSHIFT) || check(p, TK_RSHIFT)) {
        int line = p->cur.line, col = p->cur.col;
        BinOp op = check(p, TK_LSHIFT) ? B_LSHIFT : B_RSHIFT;
        advance(p);
        e = bin(p, op, line, col, e, parse_add(p));
    }
    return e;
}

static Expr *parse_bitand(Parser *p) {
    Expr *e = parse_shift(p);
    while (check(p, TK_AMP)) {
        int line = p->cur.line, col = p->cur.col; advance(p);
        e = bin(p, B_BITAND, line, col, e, parse_shift(p));
    }
    return e;
}

static Expr *parse_bitxor(Parser *p) {
    Expr *e = parse_bitand(p);
    while (check(p, TK_CARET)) {
        int line = p->cur.line, col = p->cur.col; advance(p);
        e = bin(p, B_BITXOR, line, col, e, parse_bitand(p));
    }
    return e;
}

static Expr *parse_bitor(Parser *p) {
    Expr *e = parse_bitxor(p);
    while (check(p, TK_PIPE)) {
        int line = p->cur.line, col = p->cur.col; advance(p);
        e = bin(p, B_BITOR, line, col, e, parse_bitxor(p));
    }
    return e;
}

static Expr *parse_cmp(Parser *p) {
    Expr *e = parse_bitor(p);
    for (;;) {
        int line = p->cur.line, col = p->cur.col;
        BinOp op;
        if (check(p, TK_EQ)) op = B_EQ;
        else if (check(p, TK_NE)) op = B_NE;
        else if (check(p, TK_LT)) op = B_LT;
        else if (check(p, TK_LE)) op = B_LE;
        else if (check(p, TK_GT)) op = B_GT;
        else if (check(p, TK_GE)) op = B_GE;
        else if (check(p, TK_IN)) op = B_IN;
        else return e;
        advance(p);
        e = bin(p, op, line, col, e, parse_bitor(p));
    }
}

static Expr *parse_not(Parser *p) {
    if (check(p, TK_NOT)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *e = new_expr(E_UNOP, line, col);
        e->as.un.op = U_NOT;
        e->as.un.operand = parse_not(p);
        return e;
    }
    return parse_cmp(p);
}

static Expr *parse_and(Parser *p) {
    Expr *e = parse_not(p);
    while (check(p, TK_AND)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        e = bin(p, B_AND, line, col, e, parse_not(p));
    }
    return e;
}

static Expr *parse_or(Parser *p) {
    Expr *e = parse_and(p);
    while (check(p, TK_OR)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        e = bin(p, B_OR, line, col, e, parse_and(p));
    }
    return e;
}

/* `f >> g`: composition (binds tighter than `|>`), left-associative */
static Expr *parse_compose(Parser *p) {
    Expr *e = parse_or(p);
    while (check(p, TK_GTGT)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        e = bin(p, B_COMPOSE, line, col, e, parse_or(p));
    }
    return e;
}

/* `x |> f`: pipe (lowest precedence), left-associative */
static Expr *parse_pipe(Parser *p) {
    Expr *e = parse_compose(p);
    while (check(p, TK_PIPE_GT)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        e = bin(p, B_PIPE, line, col, e, parse_compose(p));
    }
    return e;
}

static Expr *parse_expr(Parser *p) {
    return parse_pipe(p);
}

/* a header expression: record literals need parens here */
static Expr *parse_header_expr(Parser *p) {
    bool saved = p->no_rec;
    p->no_rec = true;
    Expr *e = parse_expr(p);
    p->no_rec = saved;
    return e;
}

/* --- statements --------------------------------------------------------- */

static Stmt *parse_stmt(Parser *p);

static Block parse_block(Parser *p) {
    expect(p, TK_LBRACE, "'{' opening a block");
    PtrVec stmts = {0};
    p->block_depth++;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (match(p, TK_SEMI)) continue; /* semicolons are optional separators */
        vec_push(&stmts, parse_stmt(p));
    }
    p->block_depth--;
    expect(p, TK_RBRACE, "'}' closing the block");
    Block b = { (Stmt **)stmts.items, stmts.count };
    return b;
}

static Stmt *parse_func(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* def */
    Token name = expect(p, TK_IDENT, "function name after 'def'");
    Stmt *s = new_stmt(p, S_FUNC, line, col);
    s->as.func.name = tok_text(name);
    s->as.func.dispname = s->as.func.name;
    parse_type_params(p, &s->as.func.tparams, &s->as.func.tparam_dims,
                      &s->as.func.tparam_count);
    expect(p, TK_LPAREN, "'(' after function name");
    PtrVec params = {0}, ptypes = {0}, defaults = {0};
    bool saw_default = false;
    while (!check(p, TK_RPAREN)) {
        Token pn = expect(p, TK_IDENT, "parameter name");
        vec_push(&params, tok_text(pn));
        vec_push(&ptypes, match(p, TK_COLON) ? parse_type(p) : NULL);
        Expr *def = NULL;
        if (match(p, TK_ASSIGN)) { def = parse_expr(p); saw_default = true; }
        else if (saw_default)
            perror_at(p, pn.line, pn.col, "non-default parameter follows a default parameter");
        vec_push(&defaults, def);
        if (!match(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN, "')' closing parameter list");
    if (match(p, TK_ARROW)) s->as.func.ret_type = parse_type(p);
    /* optional declarations after the return type: `pure`, `partial` */
    if (match(p, TK_PURE)) s->as.func.pure = true;
    if (match(p, TK_PARTIAL)) s->as.func.partial = true;
    s->as.func.params = (char **)params.items;
    s->as.func.param_types = (TypeExpr **)ptypes.items;
    s->as.func.defaults = (Expr **)defaults.items;
    s->as.func.param_count = params.count;
    s->as.func.body = parse_block(p);
    return s;
}

static Stmt *parse_if(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* if */
    Stmt *s = new_stmt(p, S_IF, line, col);
    PtrVec conds = {0};
    Block *blocks = NULL;
    size_t nblocks = 0, capblocks = 0;

    for (;;) {
        Expr *cond = parse_header_expr(p);
        Block body = parse_block(p);
        vec_push(&conds, cond);
        if (nblocks == capblocks) {
            capblocks = capblocks ? capblocks * 2 : 4;
            blocks = xrealloc(blocks, sizeof(Block) * capblocks);
        }
        blocks[nblocks++] = body;

        if (match(p, TK_ELIF)) continue;
        if (check(p, TK_ELSE)) {
            advance(p);
            if (match(p, TK_IF)) continue; /* `else if` chains too */
            s->as.ifs.else_block = parse_block(p);
            s->as.ifs.has_else = true;
        }
        break;
    }
    s->as.ifs.conds = (Expr **)conds.items;
    s->as.ifs.blocks = blocks;
    s->as.ifs.count = conds.count;
    return s;
}

/* --- patterns and match ---------------------------------------------------
 * pattern     := "_" | IDENT | literal | "{" pattern_field ("," pattern_field)* "}"
 * pattern_field := IDENT [":" pattern]      (* `{ x }` binds x to field x *)
 * match_stmt  := "match" expr "{" pattern "->" block (pattern "->" block)* "}"
 */

static Pat *parse_pattern(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    if (check(p, TK_IDENT)) {
        Token t = p->cur;
        advance(p);
        char *name = tok_text(t);
        Pat *q = xmalloc(sizeof(Pat));
        q->line = line;
        q->col = col;
        if (strcmp(name, "_") == 0) {
            q->kind = P_WILD;
        } else {
            q->kind = P_BIND;
            q->bind = name;
        }
        return q;
    }
    if (check(p, TK_INT)) {
        Token t = p->cur;
        advance(p);
        Pat *q = xmalloc(sizeof(Pat));
        q->kind = P_LIT;
        q->line = line;
        q->col = col;
        q->lit.kind = LIT_INT;
        q->lit.ival = strtoll(tok_text(t), NULL, 10);
        return q;
    }
    if (check(p, TK_STR)) {
        Token t = p->cur;
        advance(p);
        Pat *q = xmalloc(sizeof(Pat));
        q->kind = P_LIT;
        q->line = line;
        q->col = col;
        q->lit.kind = LIT_STR;
        q->lit.sval = unescape_string(p, t);
        return q;
    }
    if (check(p, TK_TRUE) || check(p, TK_FALSE)) {
        Pat *q = xmalloc(sizeof(Pat));
        q->kind = P_LIT;
        q->line = line;
        q->col = col;
        q->lit.kind = LIT_BOOL;
        q->lit.ival = check(p, TK_TRUE) ? 1 : 0;
        advance(p);
        return q;
    }
    if (check(p, TK_NONE)) {
        advance(p);
        Pat *q = xmalloc(sizeof(Pat));
        q->kind = P_LIT;
        q->line = line;
        q->col = col;
        q->lit.kind = LIT_NONE;
        return q;
    }
    if (check(p, TK_LBRACE)) {
        advance(p);
        Pat *q = xmalloc(sizeof(Pat));
        q->kind = P_REC;
        q->line = line;
        q->col = col;
        PtrVec items = {0};
        while (!check(p, TK_RBRACE)) {
            Token n = expect(p, TK_IDENT, "field name in pattern");
            Pat *it = xmalloc(sizeof(Pat));
            it->line = n.line;
            it->col = n.col;
            it->name = tok_text(n);
            if (match(p, TK_COLON)) {
                Pat *sub = parse_pattern(p);
                it->kind = sub->kind;
                it->bind = sub->bind;
                it->lit = sub->lit;
                it->rec = sub->rec;
            } else {
                it->kind = P_BIND;   /* `{ x }` binds the field x to x */
                it->bind = it->name;
            }
            vec_push(&items, it);
            if (!match(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE, "'}' closing pattern");
        q->rec.items = (Pat **)items.items;
        q->rec.count = items.count;
        return q;
    }
    perror_at(p, line, col,
              "expected a pattern (literal, name, '_', or { field: pattern }), "
              "got '%.*s'", p->cur.len ? p->cur.len : 5,
              p->cur.kind == TK_EOF ? "<eof>" : p->cur.start);
    return NULL; /* unreachable */
}

static Stmt *parse_match(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* match */
    Stmt *s = new_stmt(p, S_MATCH, line, col);
    s->as.mtch.subject = parse_header_expr(p);
    expect(p, TK_LBRACE, "'{' opening match arms");
    PtrVec pats = {0};
    Block *blocks = NULL;
    size_t nblocks = 0, capblocks = 0;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        vec_push(&pats, parse_pattern(p));
        expect(p, TK_ARROW, "'->' after pattern");
        if (nblocks == capblocks) {
            capblocks = capblocks ? capblocks * 2 : 4;
            blocks = xrealloc(blocks, sizeof(Block) * capblocks);
        }
        blocks[nblocks++] = parse_block(p);
    }
    expect(p, TK_RBRACE, "'}' closing match");
    if (pats.count == 0)
        perror_at(p, line, col, "match needs at least one arm");
    s->as.mtch.pats = (Pat **)pats.items;
    s->as.mtch.blocks = blocks;
    s->as.mtch.count = pats.count;
    return s;
}

/* --- imports -------------------------------------------------------------
 * import_stmt := "import" dotted ["as" IDENT]
 *              | "from" dotted "import" alias ("," alias)*
 * dotted      := IDENT ("." IDENT)*
 * alias       := IDENT ["as" IDENT]
 */

/* A dotted module path, returned as a single "a.b.c" string. */
static char *parse_dotted(Parser *p) {
    Token first = expect(p, TK_IDENT, "module name");
    char *path = tok_text(first);
    while (check(p, TK_DOT)) {
        advance(p);
        Token n = expect(p, TK_IDENT, "module name component after '.'");
        char *seg = tok_text(n);
        size_t len = strlen(path) + 1 + strlen(seg) + 1;
        char *joined = xmalloc(len);
        snprintf(joined, len, "%s.%s", path, seg);
        path = joined;
    }
    return path;
}

/* The local binding a plain `import a.b.c` introduces: its last component. */
static char *last_component(const char *path) {
    const char *dot = strrchr(path, '.');
    return (char *)(dot ? dot + 1 : path);
}

static Stmt *parse_import(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    if (p->block_depth > 0)
        perror_at(p, line, col,
                  "imports are only allowed at the top level of a module");
    bool is_from = check(p, TK_FROM);
    advance(p); /* import | from */

    Stmt *s = new_stmt(p, S_IMPORT, line, col);
    s->as.imp.path = parse_dotted(p);
    s->as.imp.is_from = is_from;

    if (!is_from) {
        if (match(p, TK_AS)) s->as.imp.alias = tok_text(expect(p, TK_IDENT, "alias after 'as'"));
        else s->as.imp.alias = last_component(s->as.imp.path);
        return s;
    }

    expect(p, TK_IMPORT, "'import' after the module path in a 'from' import");
    PtrVec names = {0};
    do {
        Token n = expect(p, TK_IDENT, "imported name");
        ImportName *in = xmalloc(sizeof(ImportName));
        in->name = tok_text(n);
        in->line = n.line;
        in->col = n.col;
        in->local = match(p, TK_AS)
                        ? tok_text(expect(p, TK_IDENT, "alias after 'as'"))
                        : in->name;
        vec_push(&names, in);
    } while (match(p, TK_COMMA));

    /* flatten the pointer vector into a contiguous array */
    s->as.imp.names = xmalloc(sizeof(ImportName) * names.count);
    for (size_t i = 0; i < names.count; i++)
        s->as.imp.names[i] = *(ImportName *)names.items[i];
    s->as.imp.name_count = names.count;
    free(names.items);
    return s;
}

static bool valid_target(const Expr *e) {
    return e->kind == E_NAME || e->kind == E_INDEX || e->kind == E_ATTR;
}

static Stmt *parse_stmt(Parser *p) {
    int line = p->cur.line, col = p->cur.col;

    switch (p->cur.kind) {
    case TK_DEF:   return parse_func(p);
    case TK_IMPORT: case TK_FROM: return parse_import(p);
    case TK_IF:    return parse_if(p);
    case TK_WHILE: {
        advance(p);
        Stmt *s = new_stmt(p, S_WHILE, line, col);
        s->as.wh.cond = parse_header_expr(p);
        s->as.wh.body = parse_block(p);
        return s;
    }
    case TK_FOR: {
        advance(p);
        Token var = expect(p, TK_IDENT, "loop variable after 'for'");
        expect(p, TK_IN, "'in' in for statement");
        Stmt *s = new_stmt(p, S_FOR, line, col);
        s->as.fr.var = tok_text(var);
        s->as.fr.seq = parse_header_expr(p);
        s->as.fr.body = parse_block(p);
        return s;
    }
    case TK_RETURN: {
        advance(p);
        Stmt *s = new_stmt(p, S_RETURN, line, col);
        /* `return` with no value: next token can't start an expression */
        switch (p->cur.kind) {
        case TK_RBRACE: case TK_EOF: case TK_SEMI:
        case TK_DEF: case TK_IF: case TK_WHILE: case TK_FOR:
        case TK_RETURN: case TK_BREAK: case TK_CONTINUE: case TK_PASS:
        case TK_TYPE:
            break;
        default:
            s->as.ret = parse_expr(p);
        }
        return s;
    }
    case TK_BREAK:    advance(p); return new_stmt(p, S_BREAK, line, col);
    case TK_CONTINUE: advance(p); return new_stmt(p, S_CONTINUE, line, col);
    case TK_PASS:     advance(p); return new_stmt(p, S_PASS, line, col);
    case TK_CONST: {
        /* `const x = v` / `const x: T = v`: an immutable binding */
        advance(p);
        Token name = expect(p, TK_IDENT, "name after 'const'");
        Expr *target = new_expr(E_NAME, line, col);
        target->as.sval = tok_text(name);
        TypeExpr *ann = NULL;
        if (match(p, TK_COLON)) ann = parse_type(p);
        expect(p, TK_ASSIGN, "'=' in const declaration");
        Stmt *s = new_stmt(p, S_ASSIGN, line, col);
        s->as.assign.target = target;
        s->as.assign.ann = ann;
        s->as.assign.is_const = true;
        s->as.assign.value = parse_expr(p);
        return s;
    }
    case TK_MATCH:
        return parse_match(p);
    case TK_DIM: {
        /* `dim Batch, Seq, DModel`: nominal dimension declarations */
        advance(p);
        Stmt *s = new_stmt(p, S_DIMDECL, line, col);
        PtrVec names = {0};
        do {
            Token n = expect(p, TK_IDENT, "dimension name after 'dim'");
            vec_push(&names, tok_text(n));
        } while (match(p, TK_COMMA));
        s->as.dim.names = (char **)names.items;
        s->as.dim.count = names.count;
        return s;
    }
    case TK_ERROR_KW: {
        /* `error Name { field: T }` is sugar for a type alias whose record
         * carries a literal discriminant:
         *     type Name = { _tag: "Name", field: T }
         * Nothing downstream needs to know about `error`: the checker sees an
         * ordinary tagged record, so unions of errors narrow and prove
         * exhaustive with the machinery `match` already uses. */
        advance(p);
        Token name = expect(p, TK_IDENT, "error type name after 'error'");
        Stmt *s = new_stmt(p, S_TYPEDEF, line, col);
        s->as.tdef.name = tok_text(name);
        s->as.tdef.dispname = s->as.tdef.name;
        TypeExpr *rec = new_type(TE_REC, line, col);
        PtrVec fnames = {0}, ftypes = {0};
        TypeExpr *tag = new_type(TE_LIT, line, col);
        tag->lit.kind = LIT_STR;
        tag->lit.sval = s->as.tdef.name;
        vec_push(&fnames, (char *)"_tag");
        vec_push(&ftypes, tag);
        if (match(p, TK_LBRACE)) { /* a payload-free error may omit the braces */
            while (!check(p, TK_RBRACE)) {
                Token f = expect(p, TK_IDENT, "field name in error declaration");
                expect(p, TK_COLON, "':' after field name");
                vec_push(&fnames, tok_text(f));
                vec_push(&ftypes, parse_type(p));
                if (!match(p, TK_COMMA)) break;
            }
            expect(p, TK_RBRACE, "'}' closing error declaration");
        }
        rec->fields.names = (char **)fnames.items;
        rec->fields.types = (TypeExpr **)ftypes.items;
        rec->fields.count = fnames.count;
        s->as.tdef.value = rec;
        return s;
    }
    case TK_TYPE: {
        advance(p);
        Token name = expect(p, TK_IDENT, "type alias name after 'type'");
        Stmt *s = new_stmt(p, S_TYPEDEF, line, col);
        s->as.tdef.name = tok_text(name);
        s->as.tdef.dispname = s->as.tdef.name;
        parse_type_params(p, &s->as.tdef.params, &s->as.tdef.param_dims,
                          &s->as.tdef.param_count);
        expect(p, TK_ASSIGN, "'=' in type alias");
        s->as.tdef.value = parse_type(p);
        return s;
    }
    case TK_LBRACE: { /* a bare block just groups statements */
        Stmt *s = new_stmt(p, S_BLOCK, line, col);
        s->as.block = parse_block(p);
        return s;
    }
    default: {
        Expr *e = parse_expr(p);
        if (check(p, TK_COLON)) { /* `x: T = v` */
            if (e->kind != E_NAME)
                perror_at(p, line, col, "type annotations are only allowed on simple names");
            advance(p);
            TypeExpr *ann = parse_type(p);
            expect(p, TK_ASSIGN, "'=' after annotated variable");
            Stmt *s = new_stmt(p, S_ASSIGN, line, col);
            s->as.assign.target = e;
            s->as.assign.ann = ann;
            s->as.assign.value = parse_expr(p);
            return s;
        }
        /* compound assignment: `x op= v` desugars to `x = x op v` (the target
         * expression is reused for the read side — fine for the name/index/
         * field targets the checker accepts) */
        BinOp cop = B_ADD;
        bool compound = false;
        if (match(p, TK_PLUS_EQ)) { cop = B_ADD; compound = true; }
        else if (match(p, TK_MINUS_EQ)) { cop = B_SUB; compound = true; }
        else if (match(p, TK_STAR_EQ)) { cop = B_MUL; compound = true; }
        else if (match(p, TK_SLASH_EQ)) { cop = B_DIV; compound = true; }
        if (compound) {
            if (!valid_target(e))
                perror_at(p, line, col, "invalid assignment target "
                          "(expected a name, index, or field)");
            Stmt *s = new_stmt(p, S_ASSIGN, line, col);
            s->as.assign.target = e;
            s->as.assign.value = bin(p, cop, line, col, e, parse_expr(p));
            return s;
        }
        if (match(p, TK_ASSIGN)) {
            if (!valid_target(e))
                perror_at(p, line, col, "invalid assignment target "
                          "(expected a name, index, or field)");
            Stmt *s = new_stmt(p, S_ASSIGN, line, col);
            s->as.assign.target = e;
            s->as.assign.value = parse_expr(p);
            return s;
        }
        Stmt *s = new_stmt(p, S_EXPR, line, col);
        s->as.expr = e;
        return s;
    }
    }
}

Program *parse_program(const char *src, const char *filename, DiagList *diags) {
    Parser p;
    lexer_init(&p.lx, src);
    p.filename = filename;
    p.diags = diags;
    p.no_rec = false;
    p.block_depth = 0;
    advance(&p);

    Program *prog = xmalloc(sizeof(Program));
    PtrVec stmts = {0};
    while (!check(&p, TK_EOF)) {
        if (match(&p, TK_SEMI)) continue; /* semicolons are optional separators */
        vec_push(&stmts, parse_stmt(&p));
    }
    prog->body.items = (Stmt **)stmts.items;
    prog->body.count = stmts.count;
    return prog;
}
