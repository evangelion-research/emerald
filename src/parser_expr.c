/* Parser: expressions, precedence climbing, and lambda/record disambiguation. */
#include "parser_internal.h"

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

Expr *bin(Parser *p, BinOp op, int line, int col, Expr *lhs, Expr *rhs) {
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

Expr *parse_expr(Parser *p) {
    return parse_pipe(p);
}

/* a header expression: record literals need parens here */
Expr *parse_header_expr(Parser *p) {
    bool saved = p->no_rec;
    p->no_rec = true;
    Expr *e = parse_expr(p);
    p->no_rec = saved;
    return e;
}
