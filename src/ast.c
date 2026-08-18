/* S-expression pretty-printer for the AST (used by `emeraldc --emit-ast`
 * and the parser golden tests).
 */
#include "ast.h"
#include "dim.h"

#include <stdlib.h>

static void print_expr(FILE *out, const Expr *e);
static void print_stmt(FILE *out, const Stmt *s, int indent);
static void print_pat(FILE *out, const Pat *p);
static void print_str_escaped(FILE *out, const char *s);

static void print_dim(FILE *out, const DimExpr *e) {
    char *s = dim_str(e);
    fputs(s, out);
    free(s);
}

static void ind(FILE *out, int n) {
    for (int i = 0; i < n; i++) fputs("  ", out);
}

static const char *binop_name(BinOp op) {
    switch (op) {
    case B_ADD: return "+";  case B_SUB: return "-";  case B_MUL: return "*";
    case B_DIV: return "/";  case B_MOD: return "%";
    case B_EQ: return "==";  case B_NE: return "!=";
    case B_LT: return "<";   case B_LE: return "<=";
    case B_GT: return ">";   case B_GE: return ">=";
    case B_AND: return "and"; case B_OR: return "or";
    case B_PIPE: return "|>"; case B_COMPOSE: return ">>";
    }
    return "?";
}

static void print_type(FILE *out, const TypeExpr *t) {
    if (!t) { fputs("any", out); return; }
    switch (t->kind) {
    case TE_NAME:
        fputs(t->name, out);
        if (t->arg_count) {
            fputs("[", out);
            for (size_t i = 0; i < t->arg_count; i++) {
                if (i) fputs(", ", out);
                print_type(out, t->args[i]);
            }
            fputs("]", out);
        }
        break;
    case TE_LIT:
        switch (t->lit.kind) {
        case LIT_INT:  fprintf(out, "%lld", (long long)t->lit.ival); break;
        case LIT_STR:  print_str_escaped(out, t->lit.sval); break;
        case LIT_BOOL: fputs(t->lit.ival ? "True" : "False", out); break;
        case LIT_NONE: fputs("None", out); break;
        }
        break;
    case TE_LIST:
        fputs("list[", out);
        print_type(out, t->elem);
        fputs("]", out);
        break;
    case TE_REC:
        fputs("{", out);
        for (size_t i = 0; i < t->fields.count; i++) {
            if (i) fputs(", ", out);
            fprintf(out, "%s: ", t->fields.names[i]);
            print_type(out, t->fields.types[i]);
        }
        fputs("}", out);
        break;
    case TE_UNION:
        fputs("(| ", out);
        print_type(out, t->lhs);
        fputs(" ", out);
        print_type(out, t->rhs);
        fputs(")", out);
        break;
    case TE_INTER:
        fputs("(& ", out);
        print_type(out, t->lhs);
        fputs(" ", out);
        print_type(out, t->rhs);
        fputs(")", out);
        break;
    case TE_FUNC:
        fputs("(-> (", out);
        for (size_t i = 0; i < t->fun.param_count; i++) {
            if (i) fputs(" ", out);
            print_type(out, t->fun.params[i]);
        }
        fputs(") ", out);
        print_type(out, t->fun.ret);
        fputs(")", out);
        break;
    case TE_TENSOR:
        fputs("(tensor ", out);
        print_type(out, t->tensor.dtype);
        if (t->tensor.dynamic) {
            fputs(" ?)", out);
        } else {
            fputs(" [", out);
            for (size_t i = 0; i < t->tensor.shape_count; i++) {
                if (i) fputs(" ", out);
                print_dim(out, t->tensor.shape[i]);
            }
            fputs("])", out);
        }
        break;
    case TE_FIN:
        fputs("(fin ", out);
        print_dim(out, t->fin_dim);
        fputs(")", out);
        break;
    }
}

static void print_str_escaped(FILE *out, const char *s) {
    fputc('"', out);
    for (; *s; s++) {
        switch (*s) {
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        default:   fputc(*s, out);
        }
    }
    fputc('"', out);
}

static void print_expr(FILE *out, const Expr *e) {
    switch (e->kind) {
    case E_INT:   fprintf(out, "%lld", (long long)e->as.ival); break;
    case E_FLOAT: fprintf(out, "%g", e->as.fval); break;
    case E_STR:   print_str_escaped(out, e->as.sval); break;
    case E_TRUE:  fputs("True", out); break;
    case E_FALSE: fputs("False", out); break;
    case E_NONE:  fputs("None", out); break;
    case E_NAME:  fputs(e->as.sval, out); break;
    case E_LIST:
        fputs("(list", out);
        for (size_t i = 0; i < e->as.list.count; i++) {
            fputs(" ", out);
            print_expr(out, e->as.list.items[i]);
        }
        fputs(")", out);
        break;
    case E_REC:
        fputs("(record", out);
        for (size_t i = 0; i < e->as.rec.count; i++) {
            fprintf(out, " (%s ", e->as.rec.names[i]);
            print_expr(out, e->as.rec.values[i]);
            fputs(")", out);
        }
        fputs(")", out);
        break;
    case E_BINOP:
        fprintf(out, "(%s ", binop_name(e->as.bin.op));
        print_expr(out, e->as.bin.lhs);
        fputs(" ", out);
        print_expr(out, e->as.bin.rhs);
        fputs(")", out);
        break;
    case E_UNOP:
        fprintf(out, "(%s ", e->as.un.op == U_NEG ? "neg" : "not");
        print_expr(out, e->as.un.operand);
        fputs(")", out);
        break;
    case E_CALL:
        fputs("(call ", out);
        print_expr(out, e->as.call.fn);
        for (size_t i = 0; i < e->as.call.count; i++) {
            fputs(" ", out);
            print_expr(out, e->as.call.args[i]);
        }
        fputs(")", out);
        break;
    case E_INDEX:
        fputs("(index ", out);
        print_expr(out, e->as.index.seq);
        fputs(" ", out);
        print_expr(out, e->as.index.idx);
        fputs(")", out);
        break;
    case E_ATTR:
        fputs("(attr ", out);
        print_expr(out, e->as.attr.obj);
        fprintf(out, " %s)", e->as.attr.name);
        break;
    case E_LAMBDA:
        fputs("(lambda (", out);
        for (size_t i = 0; i < e->as.lam.param_count; i++) {
            if (i) fputs(" ", out);
            fprintf(out, "%s:", e->as.lam.params[i]);
            print_type(out, e->as.lam.param_types[i]);
        }
        fputs(") -> ", out);
        print_expr(out, e->as.lam.body);
        fputs(")", out);
        break;
    }
}

static void print_block(FILE *out, const Block *b, int indent) {
    for (size_t i = 0; i < b->count; i++)
        print_stmt(out, b->items[i], indent);
}

static void print_stmt(FILE *out, const Stmt *s, int indent) {
    ind(out, indent);
    switch (s->kind) {
    case S_EXPR:
        fputs("(expr ", out);
        print_expr(out, s->as.expr);
        fputs(")\n", out);
        break;
    case S_ASSIGN:
        fputs(s->as.assign.is_const ? "(const " : "(assign ", out);
        print_expr(out, s->as.assign.target);
        if (s->as.assign.ann) {
            fputs(" : ", out);
            print_type(out, s->as.assign.ann);
        }
        fputs(" ", out);
        print_expr(out, s->as.assign.value);
        fputs(")\n", out);
        break;
    case S_IF:
        fputs("(if\n", out);
        for (size_t i = 0; i < s->as.ifs.count; i++) {
            ind(out, indent + 1);
            fputs("(arm ", out);
            print_expr(out, s->as.ifs.conds[i]);
            fputs("\n", out);
            print_block(out, &s->as.ifs.blocks[i], indent + 2);
            ind(out, indent + 1);
            fputs(")\n", out);
        }
        if (s->as.ifs.has_else) {
            ind(out, indent + 1);
            fputs("(else\n", out);
            print_block(out, &s->as.ifs.else_block, indent + 2);
            ind(out, indent + 1);
            fputs(")\n", out);
        }
        ind(out, indent);
        fputs(")\n", out);
        break;
    case S_WHILE:
        fputs("(while ", out);
        print_expr(out, s->as.wh.cond);
        fputs("\n", out);
        print_block(out, &s->as.wh.body, indent + 1);
        ind(out, indent);
        fputs(")\n", out);
        break;
    case S_FOR:
        fprintf(out, "(for %s ", s->as.fr.var);
        print_expr(out, s->as.fr.seq);
        fputs("\n", out);
        print_block(out, &s->as.fr.body, indent + 1);
        ind(out, indent);
        fputs(")\n", out);
        break;
    case S_RETURN:
        fputs("(return", out);
        if (s->as.ret) {
            fputs(" ", out);
            print_expr(out, s->as.ret);
        }
        fputs(")\n", out);
        break;
    case S_BREAK:    fputs("(break)\n", out); break;
    case S_CONTINUE: fputs("(continue)\n", out); break;
    case S_PASS:     fputs("(pass)\n", out); break;
    case S_BLOCK:
        fputs("(block\n", out);
        print_block(out, &s->as.block, indent + 1);
        ind(out, indent);
        fputs(")\n", out);
        break;
    case S_MATCH:
        fputs("(match ", out);
        print_expr(out, s->as.mtch.subject);
        fputs("\n", out);
        for (size_t i = 0; i < s->as.mtch.count; i++) {
            ind(out, indent + 1);
            fputs("(arm ", out);
            print_pat(out, s->as.mtch.pats[i]);
            fputs("\n", out);
            print_block(out, &s->as.mtch.blocks[i], indent + 2);
            ind(out, indent + 1);
            fputs(")\n", out);
        }
        ind(out, indent);
        fputs(")\n", out);
        break;
    case S_FUNC:
        fprintf(out, "(def %s ", s->as.func.name);
        if (s->as.func.tparam_count) {
            fputs("[", out);
            for (size_t i = 0; i < s->as.func.tparam_count; i++) {
                if (i) fputs(" ", out);
                fputs(s->as.func.tparams[i], out);
                if (s->as.func.tparam_dims && s->as.func.tparam_dims[i])
                    fputs(":dim", out);
            }
            fputs("] ", out);
        }
        fputs("(", out);
        for (size_t i = 0; i < s->as.func.param_count; i++) {
            if (i) fputs(" ", out);
            fprintf(out, "%s:", s->as.func.params[i]);
            print_type(out, s->as.func.param_types[i]);
        }
        fputs(") -> ", out);
        print_type(out, s->as.func.ret_type);
        if (s->as.func.pure) fputs(" pure", out);
        if (s->as.func.partial) fputs(" partial", out);
        fputs("\n", out);
        print_block(out, &s->as.func.body, indent + 1);
        ind(out, indent);
        fputs(")\n", out);
        break;
    case S_IMPORT:
        if (!s->as.imp.is_from) {
            fprintf(out, "(import %s as %s)\n", s->as.imp.path, s->as.imp.alias);
            break;
        }
        fprintf(out, "(from %s import", s->as.imp.path);
        for (size_t i = 0; i < s->as.imp.name_count; i++)
            fprintf(out, " (%s as %s)", s->as.imp.names[i].name,
                    s->as.imp.names[i].local);
        fputs(")\n", out);
        break;
    case S_TYPEDEF:
        fprintf(out, "(type %s ", s->as.tdef.name);
        if (s->as.tdef.param_count) {
            fputs("[", out);
            for (size_t i = 0; i < s->as.tdef.param_count; i++) {
                if (i) fputs(" ", out);
                fputs(s->as.tdef.params[i], out);
                if (s->as.tdef.param_dims && s->as.tdef.param_dims[i])
                    fputs(":dim", out);
            }
            fputs("] ", out);
        }
        print_type(out, s->as.tdef.value);
        fputs(")\n", out);
        break;
    case S_DIMDECL:
        fputs("(dim", out);
        for (size_t i = 0; i < s->as.dim.count; i++)
            fprintf(out, " %s", s->as.dim.names[i]);
        fputs(")\n", out);
        break;
    }
}

/* Walk every name assigned inside a block (assignments and for-loop
 * variables), without descending into nested `def`s. Used by the checker and
 * codegen to implement Python-style scoping: a name assigned anywhere in a
 * scope belongs to that scope.
 */
void ast_collect_assigned(const Block *b,
                          void (*fn)(const char *, const char *, int, void *),
                          void *ud) {
    for (size_t i = 0; i < b->count; i++) {
        const Stmt *s = b->items[i];
        switch (s->kind) {
        case S_ASSIGN:
            if (s->as.assign.target->kind == E_NAME)
                fn(s->as.assign.target->as.sval, s->file, s->line, ud);
            break;
        case S_IF:
            for (size_t j = 0; j < s->as.ifs.count; j++)
                ast_collect_assigned(&s->as.ifs.blocks[j], fn, ud);
            if (s->as.ifs.has_else)
                ast_collect_assigned(&s->as.ifs.else_block, fn, ud);
            break;
        case S_WHILE:
            ast_collect_assigned(&s->as.wh.body, fn, ud);
            break;
        case S_FOR:
            fn(s->as.fr.var, s->file, s->line, ud);
            ast_collect_assigned(&s->as.fr.body, fn, ud);
            break;
        case S_BLOCK:
            ast_collect_assigned(&s->as.block, fn, ud);
            break;
        case S_MATCH:
            for (size_t j = 0; j < s->as.mtch.count; j++)
                ast_collect_assigned(&s->as.mtch.blocks[j], fn, ud);
            break;
        default:
            break;
        }
    }
}

static void print_pat(FILE *out, const Pat *p) {
    switch (p->kind) {
    case P_WILD:
        fputs("_", out);
        break;
    case P_BIND:
        fputs(p->bind, out);
        break;
    case P_LIT:
        switch (p->lit.kind) {
        case LIT_INT:  fprintf(out, "%lld", (long long)p->lit.ival); break;
        case LIT_STR:  print_str_escaped(out, p->lit.sval); break;
        case LIT_BOOL: fputs(p->lit.ival ? "True" : "False", out); break;
        case LIT_NONE: fputs("None", out); break;
        }
        break;
    case P_REC:
        fputs("{", out);
        for (size_t i = 0; i < p->rec.count; i++) {
            if (i) fputs(", ", out);
            fprintf(out, "%s: ", p->rec.items[i]->name);
            print_pat(out, p->rec.items[i]);
        }
        fputs("}", out);
        break;
    }
}

void ast_print_program(FILE *out, const Program *p) {
    fputs("(program\n", out);
    print_block(out, &p->body, 1);
    fputs(")\n", out);
}
