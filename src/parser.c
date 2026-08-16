/* Emerald parser: hand-written recursive descent, one token of lookahead.
 *
 * One subtlety: `{` is both a block and a record literal. Like Go, we forbid
 * record literals at the top level of a control-flow header expression
 * (`if x { ... }` — the `{` opens the block). Parenthesize to force a record:
 * `if (p == { x: 1 }) { ... }`. The `no_rec` flag implements this.
 */
#include "parser.h"
#include "lexer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Lexer lx;
    Token cur;
    const char *filename;
    bool no_rec; /* inside a control-flow header: `{` means block, not record */
} Parser;

/* --- infrastructure ----------------------------------------------------- */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    return p;
}

static void perror_at(Parser *p, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: syntax error: ", p->filename, line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void advance(Parser *p) {
    p->cur = lexer_next(&p->lx);
    if (p->cur.kind == TK_ERROR)
        perror_at(p, p->cur.line, "unrecognized or unterminated token starting at '%.1s'",
                  p->cur.start);
}

static bool check(Parser *p, TokKind k) { return p->cur.kind == k; }

static bool match(Parser *p, TokKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

static Token expect(Parser *p, TokKind k, const char *what) {
    if (!check(p, k))
        perror_at(p, p->cur.line, "expected %s, got '%.*s'", what,
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
                perror_at(p, t.line, "unknown escape sequence '\\%c'", e);
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
        v->items = realloc(v->items, sizeof(void *) * v->cap);
        if (!v->items) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    v->items[v->count++] = item;
}

static Expr *new_expr(ExprKind k, int line) {
    Expr *e = xmalloc(sizeof(Expr));
    memset(e, 0, sizeof(Expr));
    e->kind = k;
    e->line = line;
    return e;
}

static Stmt *new_stmt(StmtKind k, int line) {
    Stmt *s = xmalloc(sizeof(Stmt));
    memset(s, 0, sizeof(Stmt));
    s->kind = k;
    s->line = line;
    return s;
}

static TypeExpr *new_type(TypeExprKind k, int line) {
    TypeExpr *t = xmalloc(sizeof(TypeExpr));
    memset(t, 0, sizeof(TypeExpr));
    t->kind = k;
    t->line = line;
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

static TypeExpr *parse_type_atom(Parser *p) {
    int line = p->cur.line;
    if (match(p, TK_LPAREN)) {
        if (check(p, TK_RPAREN)) { /* "() -> R": zero-arg function type */
            advance(p);
            expect(p, TK_ARROW, "'->' in function type");
            TypeExpr *t = new_type(TE_FUNC, line);
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
            TypeExpr *t = new_type(TE_FUNC, line);
            t->fun.params = (TypeExpr **)params.items;
            t->fun.param_count = params.count;
            t->fun.ret = parse_type(p);
            return t;
        }
        expect(p, TK_RPAREN, "')' in type");
        if (match(p, TK_ARROW)) { /* "(A) -> R" */
            TypeExpr *t = new_type(TE_FUNC, line);
            t->fun.params = xmalloc(sizeof(TypeExpr *));
            t->fun.params[0] = first;
            t->fun.param_count = 1;
            t->fun.ret = parse_type(p);
            return t;
        }
        return first; /* "(T)" grouping */
    }
    if (match(p, TK_NONE)) {
        TypeExpr *t = new_type(TE_NAME, line);
        t->name = "None";
        return t;
    }
    if (check(p, TK_INT) || check(p, TK_MINUS)) {
        bool neg = match(p, TK_MINUS);
        Token n = expect(p, TK_INT, "integer literal type");
        TypeExpr *t = new_type(TE_LIT, line);
        t->lit.kind = LIT_INT;
        t->lit.ival = strtoll(tok_text(n), NULL, 10);
        if (neg) t->lit.ival = -t->lit.ival;
        return t;
    }
    if (check(p, TK_STR)) {
        TypeExpr *t = new_type(TE_LIT, line);
        t->lit.kind = LIT_STR;
        t->lit.sval = unescape_string(p, p->cur);
        advance(p);
        return t;
    }
    if (check(p, TK_TRUE) || check(p, TK_FALSE)) {
        TypeExpr *t = new_type(TE_LIT, line);
        t->lit.kind = LIT_BOOL;
        t->lit.ival = check(p, TK_TRUE) ? 1 : 0;
        advance(p);
        return t;
    }
    if (check(p, TK_FLOAT))
        perror_at(p, line, "float literal types are not supported "
                  "(only int, str, and bool literals can be types)");
    if (check(p, TK_LBRACE)) {
        advance(p);
        TypeExpr *t = new_type(TE_REC, line);
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
    if (n.len == 4 && memcmp(n.start, "list", 4) == 0 && check(p, TK_LBRACK)) {
        advance(p);
        TypeExpr *t = new_type(TE_LIST, line);
        t->elem = parse_type(p);
        expect(p, TK_RBRACK, "']' closing list type");
        return t;
    }
    TypeExpr *t = new_type(TE_NAME, line);
    t->name = tok_text(n);
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

/* `[A, B, C]` after a `def` or `type` name: generic parameter list */
static void parse_type_params(Parser *p, char ***out, size_t *out_count) {
    PtrVec params = {0};
    if (match(p, TK_LBRACK)) {
        do {
            Token n = expect(p, TK_IDENT, "type parameter name");
            vec_push(&params, tok_text(n));
        } while (match(p, TK_COMMA));
        expect(p, TK_RBRACK, "']' closing type parameter list");
    }
    *out = (char **)params.items;
    *out_count = params.count;
}

static TypeExpr *parse_type_inter(Parser *p) {
    TypeExpr *t = parse_type_atom(p);
    while (check(p, TK_AMP)) {
        int line = p->cur.line;
        advance(p);
        TypeExpr *i = new_type(TE_INTER, line);
        i->lhs = t;
        i->rhs = parse_type_atom(p);
        t = i;
    }
    return t;
}

static TypeExpr *parse_type(Parser *p) {
    TypeExpr *t = parse_type_inter(p);
    while (check(p, TK_PIPE)) {
        int line = p->cur.line;
        advance(p);
        TypeExpr *u = new_type(TE_UNION, line);
        u->lhs = t;
        u->rhs = parse_type_inter(p);
        t = u;
    }
    return t;
}

/* --- expressions -------------------------------------------------------- */

static Expr *parse_expr(Parser *p);

static Expr *parse_primary(Parser *p) {
    int line = p->cur.line;
    switch (p->cur.kind) {
    case TK_INT: {
        Expr *e = new_expr(E_INT, line);
        e->as.ival = strtoll(tok_text(p->cur), NULL, 10);
        advance(p);
        return e;
    }
    case TK_FLOAT: {
        Expr *e = new_expr(E_FLOAT, line);
        e->as.fval = strtod(tok_text(p->cur), NULL);
        advance(p);
        return e;
    }
    case TK_STR: {
        Expr *e = new_expr(E_STR, line);
        e->as.sval = unescape_string(p, p->cur);
        advance(p);
        return e;
    }
    case TK_TRUE:  advance(p); return new_expr(E_TRUE, line);
    case TK_FALSE: advance(p); return new_expr(E_FALSE, line);
    case TK_NONE:  advance(p); return new_expr(E_NONE, line);
    case TK_IDENT: {
        Expr *e = new_expr(E_NAME, line);
        e->as.sval = tok_text(p->cur);
        advance(p);
        return e;
    }
    case TK_LPAREN: {
        advance(p);
        bool saved = p->no_rec;
        p->no_rec = false; /* parens re-allow record literals */
        Expr *e = parse_expr(p);
        p->no_rec = saved;
        expect(p, TK_RPAREN, "')'");
        return e;
    }
    case TK_LBRACK: {
        advance(p);
        Expr *e = new_expr(E_LIST, line);
        PtrVec items = {0};
        bool saved = p->no_rec;
        p->no_rec = false;
        while (!check(p, TK_RBRACK)) {
            vec_push(&items, parse_expr(p));
            if (!match(p, TK_COMMA)) break;
        }
        p->no_rec = saved;
        expect(p, TK_RBRACK, "']' closing list");
        e->as.list.items = (Expr **)items.items;
        e->as.list.count = items.count;
        return e;
    }
    case TK_LBRACE: {
        if (p->no_rec)
            perror_at(p, line, "record literal not allowed here "
                      "(wrap it in parentheses, or did you mean a block?)");
        advance(p);
        Expr *e = new_expr(E_REC, line);
        PtrVec names = {0}, values = {0};
        bool saved = p->no_rec;
        p->no_rec = false;
        while (!check(p, TK_RBRACE)) {
            Token n = expect(p, TK_IDENT, "field name in record literal");
            expect(p, TK_COLON, "':' after field name");
            vec_push(&names, tok_text(n));
            vec_push(&values, parse_expr(p));
            if (!match(p, TK_COMMA)) break;
        }
        p->no_rec = saved;
        expect(p, TK_RBRACE, "'}' closing record literal");
        e->as.rec.names = (char **)names.items;
        e->as.rec.values = (Expr **)values.items;
        e->as.rec.count = names.count;
        return e;
    }
    default:
        perror_at(p, line, "expected an expression, got '%.*s'",
                  p->cur.len ? p->cur.len : 5,
                  p->cur.kind == TK_EOF ? "<eof>" : p->cur.start);
        return NULL; /* unreachable */
    }
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        int line = p->cur.line;
        if (check(p, TK_LPAREN)) {
            advance(p);
            Expr *call = new_expr(E_CALL, line);
            call->as.call.fn = e;
            PtrVec args = {0};
            bool saved = p->no_rec;
            p->no_rec = false;
            while (!check(p, TK_RPAREN)) {
                vec_push(&args, parse_expr(p));
                if (!match(p, TK_COMMA)) break;
            }
            p->no_rec = saved;
            expect(p, TK_RPAREN, "')' closing call");
            call->as.call.args = (Expr **)args.items;
            call->as.call.count = args.count;
            e = call;
        } else if (check(p, TK_LBRACK)) {
            advance(p);
            Expr *ix = new_expr(E_INDEX, line);
            ix->as.index.seq = e;
            bool saved = p->no_rec;
            p->no_rec = false;
            ix->as.index.idx = parse_expr(p);
            p->no_rec = saved;
            expect(p, TK_RBRACK, "']' closing index");
            e = ix;
        } else if (check(p, TK_DOT)) {
            advance(p);
            Token n = expect(p, TK_IDENT, "field name after '.'");
            Expr *at = new_expr(E_ATTR, line);
            at->as.attr.obj = e;
            at->as.attr.name = tok_text(n);
            e = at;
        } else {
            return e;
        }
    }
}

static Expr *parse_unary(Parser *p) {
    if (check(p, TK_MINUS)) {
        int line = p->cur.line;
        advance(p);
        Expr *e = new_expr(E_UNOP, line);
        e->as.un.op = U_NEG;
        e->as.un.operand = parse_unary(p);
        return e;
    }
    return parse_postfix(p);
}

static Expr *bin(Parser *p, BinOp op, int line, Expr *lhs, Expr *rhs) {
    (void)p;
    Expr *e = new_expr(E_BINOP, line);
    e->as.bin.op = op;
    e->as.bin.lhs = lhs;
    e->as.bin.rhs = rhs;
    return e;
}

static Expr *parse_mul(Parser *p) {
    Expr *e = parse_unary(p);
    for (;;) {
        int line = p->cur.line;
        if (match(p, TK_STAR))         e = bin(p, B_MUL, line, e, parse_unary(p));
        else if (match(p, TK_SLASH))   e = bin(p, B_DIV, line, e, parse_unary(p));
        else if (match(p, TK_PERCENT)) e = bin(p, B_MOD, line, e, parse_unary(p));
        else return e;
    }
}

static Expr *parse_add(Parser *p) {
    Expr *e = parse_mul(p);
    for (;;) {
        int line = p->cur.line;
        if (match(p, TK_PLUS))       e = bin(p, B_ADD, line, e, parse_mul(p));
        else if (match(p, TK_MINUS)) e = bin(p, B_SUB, line, e, parse_mul(p));
        else return e;
    }
}

static Expr *parse_cmp(Parser *p) {
    Expr *e = parse_add(p);
    for (;;) {
        int line = p->cur.line;
        BinOp op;
        if (check(p, TK_EQ)) op = B_EQ;
        else if (check(p, TK_NE)) op = B_NE;
        else if (check(p, TK_LT)) op = B_LT;
        else if (check(p, TK_LE)) op = B_LE;
        else if (check(p, TK_GT)) op = B_GT;
        else if (check(p, TK_GE)) op = B_GE;
        else return e;
        advance(p);
        e = bin(p, op, line, e, parse_add(p));
    }
}

static Expr *parse_not(Parser *p) {
    if (check(p, TK_NOT)) {
        int line = p->cur.line;
        advance(p);
        Expr *e = new_expr(E_UNOP, line);
        e->as.un.op = U_NOT;
        e->as.un.operand = parse_not(p);
        return e;
    }
    return parse_cmp(p);
}

static Expr *parse_and(Parser *p) {
    Expr *e = parse_not(p);
    while (check(p, TK_AND)) {
        int line = p->cur.line;
        advance(p);
        e = bin(p, B_AND, line, e, parse_not(p));
    }
    return e;
}

static Expr *parse_expr(Parser *p) {
    Expr *e = parse_and(p);
    while (check(p, TK_OR)) {
        int line = p->cur.line;
        advance(p);
        e = bin(p, B_OR, line, e, parse_and(p));
    }
    return e;
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
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (match(p, TK_SEMI)) continue; /* semicolons are optional separators */
        vec_push(&stmts, parse_stmt(p));
    }
    expect(p, TK_RBRACE, "'}' closing the block");
    Block b = { (Stmt **)stmts.items, stmts.count };
    return b;
}

static Stmt *parse_func(Parser *p) {
    int line = p->cur.line;
    advance(p); /* def */
    Token name = expect(p, TK_IDENT, "function name after 'def'");
    Stmt *s = new_stmt(S_FUNC, line);
    s->as.func.name = tok_text(name);
    parse_type_params(p, &s->as.func.tparams, &s->as.func.tparam_count);
    expect(p, TK_LPAREN, "'(' after function name");
    PtrVec params = {0}, ptypes = {0};
    while (!check(p, TK_RPAREN)) {
        Token pn = expect(p, TK_IDENT, "parameter name");
        vec_push(&params, tok_text(pn));
        vec_push(&ptypes, match(p, TK_COLON) ? parse_type(p) : NULL);
        if (!match(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN, "')' closing parameter list");
    if (match(p, TK_ARROW)) s->as.func.ret_type = parse_type(p);
    s->as.func.params = (char **)params.items;
    s->as.func.param_types = (TypeExpr **)ptypes.items;
    s->as.func.param_count = params.count;
    s->as.func.body = parse_block(p);
    return s;
}

static Stmt *parse_if(Parser *p) {
    int line = p->cur.line;
    advance(p); /* if */
    Stmt *s = new_stmt(S_IF, line);
    PtrVec conds = {0};
    Block *blocks = NULL;
    size_t nblocks = 0, capblocks = 0;

    for (;;) {
        Expr *cond = parse_header_expr(p);
        Block body = parse_block(p);
        vec_push(&conds, cond);
        if (nblocks == capblocks) {
            capblocks = capblocks ? capblocks * 2 : 4;
            blocks = realloc(blocks, sizeof(Block) * capblocks);
            if (!blocks) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
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

static bool valid_target(const Expr *e) {
    return e->kind == E_NAME || e->kind == E_INDEX || e->kind == E_ATTR;
}

static Stmt *parse_stmt(Parser *p) {
    int line = p->cur.line;

    switch (p->cur.kind) {
    case TK_DEF:   return parse_func(p);
    case TK_IF:    return parse_if(p);
    case TK_WHILE: {
        advance(p);
        Stmt *s = new_stmt(S_WHILE, line);
        s->as.wh.cond = parse_header_expr(p);
        s->as.wh.body = parse_block(p);
        return s;
    }
    case TK_FOR: {
        advance(p);
        Token var = expect(p, TK_IDENT, "loop variable after 'for'");
        expect(p, TK_IN, "'in' in for statement");
        Stmt *s = new_stmt(S_FOR, line);
        s->as.fr.var = tok_text(var);
        s->as.fr.seq = parse_header_expr(p);
        s->as.fr.body = parse_block(p);
        return s;
    }
    case TK_RETURN: {
        advance(p);
        Stmt *s = new_stmt(S_RETURN, line);
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
    case TK_BREAK:    advance(p); return new_stmt(S_BREAK, line);
    case TK_CONTINUE: advance(p); return new_stmt(S_CONTINUE, line);
    case TK_PASS:     advance(p); return new_stmt(S_PASS, line);
    case TK_TYPE: {
        advance(p);
        Token name = expect(p, TK_IDENT, "type alias name after 'type'");
        Stmt *s = new_stmt(S_TYPEDEF, line);
        s->as.tdef.name = tok_text(name);
        parse_type_params(p, &s->as.tdef.params, &s->as.tdef.param_count);
        expect(p, TK_ASSIGN, "'=' in type alias");
        s->as.tdef.value = parse_type(p);
        return s;
    }
    case TK_LBRACE: { /* a bare block just groups statements */
        Stmt *s = new_stmt(S_BLOCK, line);
        s->as.block = parse_block(p);
        return s;
    }
    default: {
        Expr *e = parse_expr(p);
        if (check(p, TK_COLON)) { /* `x: T = v` */
            if (e->kind != E_NAME)
                perror_at(p, line, "type annotations are only allowed on simple names");
            advance(p);
            TypeExpr *ann = parse_type(p);
            expect(p, TK_ASSIGN, "'=' after annotated variable");
            Stmt *s = new_stmt(S_ASSIGN, line);
            s->as.assign.target = e;
            s->as.assign.ann = ann;
            s->as.assign.value = parse_expr(p);
            return s;
        }
        if (match(p, TK_ASSIGN)) {
            if (!valid_target(e))
                perror_at(p, line, "invalid assignment target "
                          "(expected a name, index, or field)");
            Stmt *s = new_stmt(S_ASSIGN, line);
            s->as.assign.target = e;
            s->as.assign.value = parse_expr(p);
            return s;
        }
        Stmt *s = new_stmt(S_EXPR, line);
        s->as.expr = e;
        return s;
    }
    }
}

Program *parse_program(const char *src, const char *filename) {
    Parser p;
    lexer_init(&p.lx, src);
    p.filename = filename;
    p.no_rec = false;
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
