/* Parser: statements, patterns, `match`, and imports. */
#include "parser_internal.h"

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
