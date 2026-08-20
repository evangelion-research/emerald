/* Parser: token plumbing, error recovery, and type/dimension expressions. */
#include "parser_internal.h"

/* --- infrastructure ----------------------------------------------------- */
void perror_at(Parser *p, int line, int col, const char *fmt, ...) {
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

void advance(Parser *p) {
    p->cur = lexer_next(&p->lx);
    if (p->cur.kind == TK_ERROR)
        perror_at(p, p->cur.line, p->cur.col,
                  "unrecognized or unterminated token starting at '%.1s'",
                  p->cur.start);
}

bool check(Parser *p, TokKind k) { return p->cur.kind == k; }

bool match(Parser *p, TokKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

PSave psave(Parser *p) {
    PSave s;
    s.cur = p->cur;
    s.lx = p->lx;
    return s;
}

void prestore(Parser *p, PSave s) {
    p->cur = s.cur;
    p->lx = s.lx;
}

Token expect(Parser *p, TokKind k, const char *what) {
    if (!check(p, k))
        perror_at(p, p->cur.line, p->cur.col, "expected %s, got '%.*s'", what,
                  p->cur.len ? p->cur.len : 5,
                  p->cur.kind == TK_EOF ? "<eof>" : p->cur.start);
    Token t = p->cur;
    advance(p);
    return t;
}

char *tok_text(Token t) {
    char *s = xmalloc((size_t)t.len + 1);
    memcpy(s, t.start, (size_t)t.len);
    s[t.len] = '\0';
    return s;
}

char *unescape_string(Parser *p, Token t) {
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

void vec_push(PtrVec *v, void *item) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, sizeof(void *) * v->cap);
    }
    v->items[v->count++] = item;
}

Expr *new_expr(ExprKind k, int line, int col) {
    Expr *e = xmalloc(sizeof(Expr));
    memset(e, 0, sizeof(Expr));
    e->kind = k;
    e->line = line;
    e->col = col;
    return e;
}

Stmt *new_stmt(Parser *p, StmtKind k, int line, int col) {
    Stmt *s = xmalloc(sizeof(Stmt));
    memset(s, 0, sizeof(Stmt));
    s->kind = k;
    s->line = line;
    s->col = col;
    s->file = p->filename;
    return s;
}

TypeExpr *new_type(TypeExprKind k, int line, int col) {
    TypeExpr *t = xmalloc(sizeof(TypeExpr));
    memset(t, 0, sizeof(TypeExpr));
    t->kind = k;
    t->line = line;
    t->col = col;
    return t;
}

/* --- dimension expressions (inside a tensor shape) -----------------------
 * dim_expr := dim_term (("+"|"-") dim_term)*
 * dim_term := dim_factor ("*" dim_factor)*
 * dim_factor := IDENT | INT
 * Only `+` and `*` are supported in dimension expressions; a subtraction use case has
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
void parse_type_params(Parser *p, char ***out, bool **out_dims,
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

TypeExpr *parse_type(Parser *p) {
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
