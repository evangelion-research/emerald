/* Shape report: walk the linked AST and print implemented dimension and
 * tensor annotations. */
#include "shape_report.h"
#include "dim.h"

#include <stdlib.h>

static void emit_type_shapes(FILE *out, const TypeExpr *type);
static void emit_expr_shapes(FILE *out, const Expr *expr);
static void emit_stmt_shapes(FILE *out, const Stmt *stmt);

static void emit_block_shapes(FILE *out, const Block *block) {
    for (size_t i = 0; i < block->count; i++)
        emit_stmt_shapes(out, block->items[i]);
}

static void emit_type_shapes(FILE *out, const TypeExpr *type) {
    if (!type) return;

    switch (type->kind) {
    case TE_TENSOR:
        fprintf(out, "Tensor[");
        if (type->tensor.dtype && type->tensor.dtype->kind == TE_NAME)
            fprintf(out, "%s", type->tensor.dtype->name);
        else
            fprintf(out, "?");

        if (type->tensor.dynamic) {
            fprintf(out, ", ?]\n");
        } else {
            fprintf(out, ", [");
            for (size_t i = 0; i < type->tensor.shape_count; i++) {
                if (i) fprintf(out, ", ");
                char *shape = dim_str(type->tensor.shape[i]);
                fprintf(out, "%s", shape);
                free(shape);
            }
            fprintf(out, "]]\n");
        }
        break;

    case TE_FIN: {
        char *bound = dim_str(type->fin_dim);
        fprintf(out, "Fin[%s]\n", bound);
        free(bound);
        break;
    }

    case TE_EQ: {
        char *lhs = dim_str(type->eq_lhs);
        char *rhs = dim_str(type->eq_rhs);
        fprintf(out, "Eq[%s, %s]\n", lhs, rhs);
        free(lhs);
        free(rhs);
        break;
    }

    case TE_NAME:
        for (size_t i = 0; i < type->arg_count; i++)
            emit_type_shapes(out, type->args[i]);
        break;

    case TE_LIST:
    case TE_SEQ:
        emit_type_shapes(out, type->elem);
        break;

    case TE_REC:
        for (size_t i = 0; i < type->fields.count; i++)
            emit_type_shapes(out, type->fields.types[i]);
        break;

    case TE_UNION:
    case TE_INTER:
        emit_type_shapes(out, type->lhs);
        emit_type_shapes(out, type->rhs);
        break;

    case TE_FUNC:
        for (size_t i = 0; i < type->fun.param_count; i++)
            emit_type_shapes(out, type->fun.params[i]);
        emit_type_shapes(out, type->fun.ret);
        break;

    case TE_LIT:
        break;
    }
}

static void emit_expr_shapes(FILE *out, const Expr *expr) {
    if (!expr) return;

    switch (expr->kind) {
    case E_LAMBDA:
        for (size_t i = 0; i < expr->as.lam.param_count; i++)
            emit_type_shapes(out, expr->as.lam.param_types[i]);
        emit_expr_shapes(out, expr->as.lam.body);
        break;

    case E_TRY:
        emit_expr_shapes(out, expr->as.try_expr);
        break;

    case E_CATCH:
        emit_expr_shapes(out, expr->as.ctch.subject);
        for (size_t i = 0; i < expr->as.ctch.count; i++)
            emit_expr_shapes(out, expr->as.ctch.arms[i].body);
        break;

    case E_LIST:
    case E_TUPLE:
        for (size_t i = 0; i < expr->as.list.count; i++)
            emit_expr_shapes(out, expr->as.list.items[i]);
        break;

    case E_REC:
        for (size_t i = 0; i < expr->as.rec.count; i++)
            emit_expr_shapes(out, expr->as.rec.values[i]);
        break;

    case E_DICT:
        for (size_t i = 0; i < expr->as.dict.count; i++) {
            emit_expr_shapes(out, expr->as.dict.keys[i]);
            emit_expr_shapes(out, expr->as.dict.values[i]);
        }
        break;

    case E_SET:
        for (size_t i = 0; i < expr->as.set.count; i++)
            emit_expr_shapes(out, expr->as.set.items[i]);
        break;

    case E_SLICE:
        emit_expr_shapes(out, expr->as.slice.seq);
        emit_expr_shapes(out, expr->as.slice.start);
        emit_expr_shapes(out, expr->as.slice.stop);
        emit_expr_shapes(out, expr->as.slice.step);
        break;

    case E_COMP:
        emit_expr_shapes(out, expr->as.comp.seq);
        emit_expr_shapes(out, expr->as.comp.cond);
        emit_expr_shapes(out, expr->as.comp.elt);
        emit_expr_shapes(out, expr->as.comp.key);
        break;

    case E_FSTR:
        for (size_t i = 0; i < expr->as.fstr.count; i++)
            emit_expr_shapes(out, expr->as.fstr.exprs[i]);
        break;

    case E_BINOP:
        emit_expr_shapes(out, expr->as.bin.lhs);
        emit_expr_shapes(out, expr->as.bin.rhs);
        break;

    case E_UNOP:
        emit_expr_shapes(out, expr->as.un.operand);
        break;

    case E_CALL:
        emit_expr_shapes(out, expr->as.call.fn);
        for (size_t i = 0; i < expr->as.call.count; i++)
            emit_expr_shapes(out, expr->as.call.args[i]);
        break;

    case E_INDEX:
        emit_expr_shapes(out, expr->as.index.seq);
        emit_expr_shapes(out, expr->as.index.idx);
        break;

    case E_ATTR:
        emit_expr_shapes(out, expr->as.attr.obj);
        break;

    default:
        break;
    }
}

static void emit_stmt_shapes(FILE *out, const Stmt *stmt) {
    switch (stmt->kind) {
    case S_DIMDECL:
        fprintf(out, "dim");
        for (size_t i = 0; i < stmt->as.dim.count; i++)
            fprintf(out, " %s", stmt->as.dim.names[i]);
        fprintf(out, "\n");
        break;

    case S_ASSIGN:
        emit_type_shapes(out, stmt->as.assign.ann);
        emit_expr_shapes(out, stmt->as.assign.value);
        break;

    case S_EXPR:
        emit_expr_shapes(out, stmt->as.expr);
        break;

    case S_IF:
        for (size_t i = 0; i < stmt->as.ifs.count; i++) {
            emit_expr_shapes(out, stmt->as.ifs.conds[i]);
            emit_block_shapes(out, &stmt->as.ifs.blocks[i]);
        }
        if (stmt->as.ifs.has_else)
            emit_block_shapes(out, &stmt->as.ifs.else_block);
        break;

    case S_WHILE:
        emit_expr_shapes(out, stmt->as.wh.cond);
        emit_block_shapes(out, &stmt->as.wh.body);
        break;

    case S_FOR:
        emit_expr_shapes(out, stmt->as.fr.seq);
        emit_block_shapes(out, &stmt->as.fr.body);
        break;

    case S_RETURN:
        emit_expr_shapes(out, stmt->as.ret);
        break;

    case S_BLOCK:
        emit_block_shapes(out, &stmt->as.block);
        break;

    case S_MATCH:
        emit_expr_shapes(out, stmt->as.mtch.subject);
        for (size_t i = 0; i < stmt->as.mtch.count; i++)
            emit_block_shapes(out, &stmt->as.mtch.blocks[i]);
        break;

    case S_FUNC:
        for (size_t i = 0; i < stmt->as.func.param_count; i++)
            emit_type_shapes(out, stmt->as.func.param_types[i]);
        emit_type_shapes(out, stmt->as.func.ret_type);
        emit_block_shapes(out, &stmt->as.func.body);
        break;

    case S_TYPEDEF:
        emit_type_shapes(out, stmt->as.tdef.value);
        break;

    case S_IMPORT:
    case S_BREAK:
    case S_CONTINUE:
    case S_PASS:
        break;
    }
}

void emit_shapes(FILE *out, const Program *program) {
    emit_block_shapes(out, &program->body);
}
