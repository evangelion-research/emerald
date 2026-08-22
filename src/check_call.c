/* Checker: call inference -- builtin rules, purity, argument mapping for
 * keyword/default parameters, and generic instantiation. */
#include "check_internal.h"

/* --- local-mutation purity (the standard-library purity convention) ------------------------- */
/*
 * A `pure` function may not have observable effects, and `append` mutates —
 * so the naive rule (a pure function may call only pure builtins) makes it
 * impossible for proof-mode code to build a list at all. The safe escape:
 * `append` is allowed when the target is a list the function allocated
 * itself and has not let escape (a fresh literal, or the result of a
 * fresh-list pure builtin). Mutating such a list is unobservable from the
 * caller, so the function stays pure. Every other target — a parameter, a
 * global, a record field, a list element, anything that could be reachable
 * from outside — is rejected.
 */
/* Does `e` denote a locally-owned list? Fresh list literals and the
 * fresh-list pure builtins (range, map/filter/reduce, a slice of a list)
 * are owned; a name is owned iff its last assignment was to such a value
 * and it has not since escaped. Everything else (a parameter, a global, a
 * field or element read, a user-function call) is not owned. */
static bool expr_owned(Ck *ck, const Expr *e) {
    switch (e->kind) {
    case E_LIST:
        return true;
    case E_NAME: {
        Var *v = NULL;
        if (ck->scope) v = env_find(&ck->scope->locals, e->as.sval);
        if (!v) v = env_find(&ck->globals, e->as.sval);
        return v ? v->owned : false;
    }
    case E_CALL: {
        if (e->as.call.fn->kind != E_NAME) return false;
        const char *n = e->as.call.fn->as.sval;
        if (strcmp(n, "range") == 0) return true;
        if (strcmp(n, "map") == 0 || strcmp(n, "filter") == 0 ||
            strcmp(n, "reduce") == 0)
            return true;
        /* a slice of a list copies, so the result is fresh regardless of the
         * source: `append(slice(xs, 0, 1), v)` is safe even when xs is a
         * parameter (a slice of a string is a string, not a list) */
        if (strcmp(n, "slice") == 0 && e->as.call.count >= 1) {
            Type *s = ty_base(ty_resolve(infer(ck, e->as.call.args[0])));
            return s->k == TY_LIST;
        }
        return false;
    }
    default:
        return false;
    }
}

/* An owned list escapes when it is assigned into a global, a record field,
 * or a list element: from then on, appending to it would be observable from
 * outside the function. Walk the assigned value and revoke ownership on any
 * owned local it carries. */
void mark_escaped(Ck *ck, const Expr *e) {
    switch (e->kind) {
    case E_NAME: {
        Var *v = NULL;
        if (ck->scope) v = env_find(&ck->scope->locals, e->as.sval);
        if (!v) v = env_find(&ck->globals, e->as.sval);
        if (v) v->owned = false;
        break;
    }
    case E_LIST:
        for (size_t i = 0; i < e->as.list.count; i++)
            mark_escaped(ck, e->as.list.items[i]);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            mark_escaped(ck, e->as.rec.values[i]);
        break;
    case E_CALL:
        for (size_t i = 0; i < e->as.call.count; i++)
            mark_escaped(ck, e->as.call.args[i]);
        break;
    default:
        break;
    }
}

/* Update a variable's ownership after an assignment. A global (or any
 * top-level binding) is never owned, and an owned list assigned into one
 * escapes — so the value is walked and its owned locals revoked. */
void assign_owned(Ck *ck, Var *v, const char *name, const Expr *value) {
    bool is_global = !ck->scope || env_find(&ck->globals, name) == v;
    if (is_global) {
        mark_escaped(ck, value);
        v->owned = false;
    } else {
        v->owned = expr_owned(ck, value);
    }
}

static bool func_has_defaults(const FuncSig *f) {
    if (!f->node || !f->node->as.func.defaults) return false;
    for (size_t i=0;i<f->param_count;i++) if (f->node->as.func.defaults[i]) return true;
    return false;
}

static size_t func_required(const FuncSig *f) {
    size_t n = f->param_count;
    while (n && f->node && f->node->as.func.defaults && f->node->as.func.defaults[n - 1]) n--;
    return n;
}

static bool map_call_args(Ck *ck, const Expr *e, const FuncSig *f, size_t *out) {
    size_t np=f->param_count, pos=0;
    bool *used=xcalloc(np ? np : 1,sizeof(bool)); bool ok=true;
    for(size_t i=0;i<e->as.call.count;i++) {
        const char *name=e->as.call.arg_names ? e->as.call.arg_names[i] : NULL;
        size_t j=pos;
        if(name) { for(j=0;j<np;j++) if(strcmp(f->node->as.func.params[j],name)==0) break; if(j==np) ok=false; }
        else { while(j<np&&used[j])j++; pos=j+1; }
        if(j>=np || used[j]) ok=false; else { used[j]=true; out[i]=j; }
    }
    size_t req=func_required(f), supplied=0; for(size_t j=0;j<np;j++) if(used[j]) supplied++;
    if(supplied < req || supplied > np) ok=false;
    if(!ok) {
        if (!func_has_defaults(f) && e->as.call.count != np)
            ck_error(ck,"E_TYPE_ARITY",e->line,e->col,"%s() takes %zu arguments, got %zu",f->disp,np,e->as.call.count);
        else
            ck_error(ck,"E_TYPE_ARITY",e->line,e->col,"invalid arguments for %s() (expected %zu..%zu arguments, got %zu)",f->disp,req,np,e->as.call.count);
    }
    free(used); return ok;
}

Type *infer_call(Ck *ck, const Expr *e, Type *expected) {
    const Expr *fn = e->as.call.fn;
    size_t argc = e->as.call.count;
    Type **argt = xmalloc(sizeof(Type *) * argc);
    bool *islam = xmalloc(sizeof(bool) * argc);
    for (size_t i = 0; i < argc; i++) {
        /* lambdas are inferred lazily so unannotated parameters can take the
         * expected type from the callee's signature (contextual typing) */
        islam[i] = e->as.call.args[i]->kind == E_LAMBDA;
        if (!islam[i]) {
            argt[i] = infer(ck, e->as.call.args[i]);
            ck_proof_taint(ck, argt[i], e->line, e->col, "argument");
        }
    }

    if (fn->kind == E_NAME) {
        const char *name = fn->as.sval;
        const char *dname = fn->disp ? fn->disp : name;
        /* purity: a pure function may only call pure builtins — with the one
         * local-mutation escape: `append` to a list this function allocated itself is
         * an unobservable effect, so it is allowed when the target is owned. */
        bool bpure = false;
        if (ck->cur_pure && builtin_find(name, &bpure) && !bpure) {
            bool owned_append = strcmp(name, "append") == 0 && argc >= 1 &&
                                expr_owned(ck, e->as.call.args[0]);
            if (!owned_append)
                ck_error(ck, "E_TYPE_PURE_CALL", e->line, e->col,
                         "pure function calls impure builtin '%s'", dname);
        }
        if (strcmp(name, "map") == 0 || strcmp(name, "filter") == 0 ||
            strcmp(name, "reduce") == 0)
            return infer_map_like(ck, e, name, argt, islam);
        if (strcmp(name, "dict") == 0 || strcmp(name, "set") == 0) {
            if (argc > 1)
                ck_error(ck, "E_TYPE_ARITY", e->line, e->col,
                         "%s() takes 0 or 1 arguments, got %zu", name, argc);
            return &t_any; /* runtime values are intentionally not type constructors */
        }
        FuncSig *f = find_func(ck, name);
        if (!f) /* plain builtins have no typed signature: lambdas are free */
            for (size_t i = 0; i < argc; i++)
                if (islam[i])
                    argt[i] = infer_lambda(ck, e->as.call.args[i], NULL);
        /* the variadic builtins accept any number of arguments */
        if (strcmp(name, "print") == 0 ||
            strcmp(name, "eprint") == 0) return &t_none;
        /* pretty printing: any value, string rendering is pure */
        if (strcmp(name, "pprint") == 0 || strcmp(name, "pprint_err") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_none;
        }
        if (strcmp(name, "pp_format") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_str;
        }
        if (strcmp(name, "len") == 0) {
            if (ck_arity(ck, e, dname, 1)) {
                Type *a = ty_base(argt[0]);
                if (a->k != TY_ANY && a->k != TY_STR && a->k != TY_LIST &&
                    a->k != TY_SEQ && a->k != TY_REC && a->k != TY_UNION &&
                    a->k != TY_NEVER)
                    ck_error(ck, "E_TYPE_NO_LEN", e->line, e->col,
                             "%s has no len()", type_str(argt[0]));
            }
            return &t_int;
        }
        if (strcmp(name, "range") == 0) {
            if (argc != 1 && argc != 2)
                ck_error(ck, "E_TYPE_ARITY", e->line, e->col,
                         "range() takes 1 or 2 arguments, got %zu", argc);
            for (size_t i = 0; i < argc; i++)
                if (!assignable(&t_int, argt[i]))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "range() argument %zu must be int, got %s",
                             i + 1, type_str(argt[i]));
            return ty_list(&t_int);
        }
        if (strcmp(name, "str") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_str;
        }
        if (strcmp(name, "int") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_int;
        }
        if (strcmp(name, "gc_stats") == 0) {
            ck_arity(ck, e, dname, 0);
            return gc_stats_type();
        }
        if (strcmp(name, "gc_collect") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_none;
        }
        if (strcmp(name, "read_file") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "read_file() path must be str, got %s", type_str(argt[0]));
            return &t_str;
        }
        if (strcmp(name, "write_file") == 0 || strcmp(name, "append_file") == 0) {
            ck_arity(ck, e, dname, 2);
            return &t_none;
        }
        if (strcmp(name, "run") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "run() command must be str, got %s", type_str(argt[0]));
            return &t_int;
        }
        if (strcmp(name, "sqrt") == 0 || strcmp(name, "tan") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_float, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "%s() argument must be a number, got %s",
                         name, type_str(argt[0]));
            return &t_float;
        }
        if (strcmp(name, "rand") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_float;
        }
        /* --- the stdlib foundation (see the standard-library builtin boundary) ------------- */
        if (strcmp(name, "append") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_none;
            Type *l = ty_base(ty_resolve(argt[0]));
            if (l->k == TY_SEQ) {
                ck_error(ck, "E_TYPE_IMMUTABLE", e->line, e->col,
                         "append() cannot mutate a seq: thaw() it first");
            } else if (l->k == TY_LIST) {
                if (!assignable(l->elem, argt[1]))
                    ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                               l->elem, argt[1],
                               "append() to %s: expected %s, got %s",
                               type_str(argt[0]), type_str(l->elem),
                               type_str(argt[1]));
            } else if (l->k != TY_ANY && l->k != TY_NEVER) {
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "append() expects a list, got %s", type_str(argt[0]));
            }
            return &t_none;
        }
        if (strcmp(name, "freeze") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return ty_seq(&t_any);
            Type *s = ty_base(ty_resolve(argt[0]));
            if (s->k == TY_LIST) return ty_seq(s->elem);
            if (s->k == TY_ANY || s->k == TY_NEVER) return ty_seq(&t_any);
            ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                     "freeze() expects a list, got %s", type_str(argt[0]));
            return ty_seq(&t_any);
        }
        if (strcmp(name, "thaw") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return ty_list(&t_any);
            Type *s = ty_base(ty_resolve(argt[0]));
            if (s->k == TY_SEQ) return ty_list(s->elem);
            if (s->k == TY_ANY || s->k == TY_NEVER) return ty_list(&t_any);
            ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                     "thaw() expects a seq, got %s", type_str(argt[0]));
            return ty_list(&t_any);
        }
        if (strcmp(name, "slice") == 0) {
            if (!ck_arity(ck, e, dname, 3)) return &t_any;
            for (size_t i = 1; i < 3; i++)
                if (!assignable(&t_int, argt[i]))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "slice() bound %zu must be int, got %s", i,
                             type_str(argt[i]));
            /* slicing preserves the sequence's type: str -> str, list[T] ->
             * list[T]. Anything else is a compile error rather than a cast. */
            Type *s = ty_base(ty_resolve(argt[0]));
            if (s->k == TY_STR) return &t_str;
            if (s->k == TY_LIST) return argt[0];
            if (s->k == TY_SEQ) return argt[0];
            if (s->k == TY_ANY || s->k == TY_NEVER) return &t_any;
            ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                     "cannot slice %s", type_str(argt[0]));
            return &t_any;
        }
        if (strcmp(name, "ord") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "ord() argument must be str, got %s", type_str(argt[0]));
            return &t_int;
        }
        if (strcmp(name, "chr") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "chr() argument must be int, got %s", type_str(argt[0]));
            return &t_str;
        }
        /* --- the UTF-8 code-point layer (stdlib/unicode.rald) ---------- */
        if (strcmp(name, "uc_len") == 0 || strcmp(name, "uc_ord") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "%s() argument must be str, got %s", name,
                         type_str(argt[0]));
            return &t_int;
        }
        if (strcmp(name, "uc_valid") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "uc_valid() argument must be str, got %s", type_str(argt[0]));
            return &t_bool;
        }
        if (strcmp(name, "uc_chr") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "uc_chr() argument must be int, got %s", type_str(argt[0]));
            return &t_str;
        }
        if (strcmp(name, "uc_at") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_str;
            if (!assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "uc_at() argument must be str, got %s", type_str(argt[0]));
            if (!assignable(&t_int, argt[1]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "uc_at() index must be int, got %s", type_str(argt[1]));
            return &t_str;
        }
        if (strcmp(name, "uc_slice") == 0) {
            if (!ck_arity(ck, e, dname, 3)) return &t_str;
            if (!assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "uc_slice() argument must be str, got %s", type_str(argt[0]));
            for (size_t i = 1; i < 3; i++)
                if (!assignable(&t_int, argt[i]))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "uc_slice() bound %zu must be int, got %s", i,
                             type_str(argt[i]));
            return &t_str;
        }
        if (strcmp(name, "uc_chars") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "uc_chars() argument must be str, got %s", type_str(argt[0]));
            return ty_list(&t_str);
        }
        if (strcmp(name, "float") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_float;
        }
        if (strcmp(name, "argv") == 0) {
            ck_arity(ck, e, dname, 0);
            return ty_list(&t_str);
        }
        if (strcmp(name, "exit") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "exit() status must be int, got %s", type_str(argt[0]));
            /* `never`: exit() does not return, so `return exit(1)` satisfies
             * any return type and a function ending in exit() is not a
             * fall-off-the-end error. */
            return &t_never;
        }
        if (strcmp(name, "read_file_opt") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "read_file_opt() path must be str, got %s",
                         type_str(argt[0]));
            return ty_join(&t_str, &t_none);
        }
        if (strcmp(name, "read_line") == 0) {
            ck_arity(ck, e, dname, 0);
            return ty_join(&t_str, &t_none);
        }
        if (strcmp(name, "read_all") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_str;
        }
        if (strcmp(name, "input") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "input() prompt must be str, got %s", type_str(argt[0]));
            return ty_join(&t_str, &t_none);
        }
        if (strcmp(name, "write_out") == 0 || strcmp(name, "write_err") == 0) {
            /* any value, printed the way print() would print it */
            ck_arity(ck, e, dname, 1);
            return &t_none;
        }
        if (strcmp(name, "flush") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_none;
        }
        if (strcmp(name, "now") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_float;
        }
        if (strcmp(name, "seed_rand") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "seed_rand() argument must be int, got %s", type_str(argt[0]));
            return &t_none;
        }
        if (strcmp(name, "file_exists") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_str, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "file_exists() path must be str, got %s",
                         type_str(argt[0]));
            return &t_bool;
        }

        /* --- green threads and channels (docs/concurrency.md) ----------
         * The handle types carry the element type so a channel's traffic is
         * checked at both ends: `chan()` alone produces Chan[any], and an
         * annotation (`c: Chan[int] = chan(0)`) pins it down. */
        if (strcmp(name, "spawn") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return ty_opaque("Task", &t_any);
            Type *fnt = ty_resolve(argt[0]);
            if (fnt->k == TY_ANY) return ty_opaque("Task", &t_any);
            if (fnt->k != TY_FUNC || fnt->fun.count != 0) {
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "spawn() takes a function of no arguments, got %s",
                         type_str(argt[0]));
                return ty_opaque("Task", &t_any);
            }
            return ty_opaque("Task", fnt->fun.ret);
        }
        if (strcmp(name, "join") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return &t_any;
            Type *t = ty_resolve(argt[0]);
            if (t->k == TY_ANY) return &t_any;
            if (!is_opaque(t, "Task")) {
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "join() expects a task, got %s", type_str(argt[0]));
                return &t_any;
            }
            return t->elem;
        }
        if (strcmp(name, "task_done") == 0) {
            if (ck_arity(ck, e, dname, 1)) {
                Type *t = ty_resolve(argt[0]);
                if (t->k != TY_ANY && !is_opaque(t, "Task"))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "task_done() expects a task, got %s", type_str(argt[0]));
            }
            return &t_bool;
        }
        if (strcmp(name, "task_stats") == 0) {
            ck_arity(ck, e, dname, 0);
            return task_stats_type();
        }
        if (strcmp(name, "task_yield") == 0) {
            ck_arity(ck, e, dname, 0);
            return &t_none;
        }
        if (strcmp(name, "sleep") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_float, argt[0]) &&
                !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "sleep() expects a number of seconds, got %s",
                         type_str(argt[0]));
            return &t_none;
        }
        if (strcmp(name, "chan") == 0) {
            if (ck_arity(ck, e, dname, 1) && !assignable(&t_int, argt[0]))
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "chan() capacity must be int, got %s", type_str(argt[0]));
            return ty_opaque("Chan", &t_any);
        }
        if (strcmp(name, "send") == 0) {
            if (ck_arity(ck, e, dname, 2)) {
                Type *c = ty_resolve(argt[0]);
                if (c->k != TY_ANY && !is_opaque(c, "Chan"))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "send() expects a channel, got %s", type_str(argt[0]));
                else if (is_opaque(c, "Chan") && !assignable(c->elem, argt[1]))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "send() on %s cannot carry %s",
                             type_str(argt[0]), type_str(argt[1]));
            }
            return &t_none;
        }
        if (strcmp(name, "recv") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return &t_any;
            Type *c = ty_resolve(argt[0]);
            if (c->k == TY_ANY) return &t_any;
            if (!is_opaque(c, "Chan")) {
                ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                         "recv() expects a channel, got %s", type_str(argt[0]));
                return &t_any;
            }
            /* a closed, drained channel yields None, so every receive has to
             * consider that case -- the same shape as read_line() */
            return c->elem->k == TY_ANY ? &t_any : ty_join(c->elem, &t_none);
        }
        if (strcmp(name, "chan_close") == 0 || strcmp(name, "chan_len") == 0) {
            if (ck_arity(ck, e, dname, 1)) {
                Type *c = ty_resolve(argt[0]);
                if (c->k != TY_ANY && !is_opaque(c, "Chan"))
                    ck_error(ck, "E_TYPE_ARG", e->line, e->col,
                             "%s() expects a channel, got %s", dname,
                             type_str(argt[0]));
            }
            return strcmp(name, "chan_close") == 0 ? &t_none : &t_int;
        }

        /* --- tensor primitives: shape-obligation typing rules (W4). Each
         * constructor's shape comes from a runtime list, so it types as the
         * dynamic escape hatch Tensor[f32, ?]; the shape-carrying operations
         * (matmul, elementwise, reshape, permute, reductions) verify their
         * obligations statically and emit E_SHAPE_* diagnostics. */
        if (strcmp(name, "zeros") == 0 || strcmp(name, "ones") == 0 ||
            strcmp(name, "arange") == 0) {
            ck_arity(ck, e, dname, 1);
            return ty_tensor(CDT_F32, shape_dynamic());
        }
        if (strcmp(name, "full") == 0 || strcmp(name, "randn") == 0) {
            ck_arity(ck, e, dname, 2);
            return ty_tensor(CDT_F32, shape_dynamic());
        }
        if (strcmp(name, "tensor") == 0) {
            ck_arity(ck, e, dname, 1);
            return ty_tensor(CDT_F32, shape_dynamic());
        }
        if (strcmp(name, "exp") == 0 || strcmp(name, "log") == 0 ||
            strcmp(name, "tanh") == 0 || strcmp(name, "relu") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return &t_any;
            /* elementwise unary: preserves dtype and shape */
            return expect_tensor(ck, e, argt[0], name);
        }
        if (strcmp(name, "transpose") == 0) {
            if (!ck_arity(ck, e, dname, 1)) return &t_any;
            return infer_tensor_transpose(ck, e, argt[0]);
        }
        if (strcmp(name, "item") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_float;
        }
        if (strcmp(name, "shape") == 0) {
            ck_arity(ck, e, dname, 1);
            return ty_list(&t_int);
        }
        if (strcmp(name, "ndim") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_int;
        }
        if (strcmp(name, "dtype") == 0) {
            ck_arity(ck, e, dname, 1);
            return &t_str;
        }
        if (strcmp(name, "astype") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_astype(ck, e, argt);
        }
        if (strcmp(name, "matmul") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_matmul(ck, e, argt);
        }
        if (strcmp(name, "reshape") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_reshape(ck, e, argt);
        }
        if (strcmp(name, "permute") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_permute(ck, e, argt);
        }
        if (strcmp(name, "expand") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_expand(ck, e, argt);
        }
        if (strcmp(name, "sum") == 0 || strcmp(name, "mean") == 0 ||
            strcmp(name, "max") == 0 || strcmp(name, "argmax") == 0) {
            if (!ck_arity(ck, e, dname, 2)) return &t_any;
            return infer_tensor_reduce(ck, e, argt, name);
        }
        if (strcmp(name, "tslice") == 0) {
            if (!ck_arity(ck, e, dname, 4)) return &t_any;
            return infer_tensor_slice(ck, e, argt);
        }

        if (f) {
            size_t *argpi = xmalloc(sizeof(size_t) * (argc ? argc : 1));
            bool call_ok = map_call_args(ck, e, f, argpi);
            /* purity: a pure function may only call other pure functions */
            if (ck->cur_pure && !f->pure)
                ck_error(ck, "E_TYPE_PURE_CALL", e->line, e->col,
                         "pure function calls impure function '%s'", dname);
            if (!call_ok) { free(argpi); return f->tparam_count ? &t_any : f->ret; }
            if (f->tparam_count == 0) {
                /* infer deferred lambdas against the declared parameter types */
                for (size_t i = 0; i < argc; i++)
                    if (islam[i])
                        argt[i] = infer_lambda(ck, e->as.call.args[i],
                                               f->params[argpi[i]]);
                for (size_t i = 0; i < argc; i++) {
                    Type *param = f->params[argpi[i]];
                    note_shape_crossing(ck, param, argt[i]);
                    ck_covariance(ck, param, argt[i], e->line, e->col);
                    if (!assignable(param, argt[i]))
                        ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                                   param, argt[i],
                                   "argument %zu of %s(): expected %s, got %s",
                                   i + 1, dname, type_str(param), type_str(argt[i]));
                }
                free(argpi); return f->ret;
            }
            /* generic call: infer type arguments by unification, then re-check */
            Subst sub;
            sub.names = f->tparams;
            sub.count = f->tparam_count;
            sub.types = xmalloc(sizeof(Type *) * f->tparam_count);
            memset(sub.types, 0, sizeof(Type *) * f->tparam_count);
            for (size_t i = 0; i < argc; i++)
                if (!islam[i]) unify(f->params[argpi[i]], argt[i], &sub);
            /* contextual return-type propagation: a type parameter that appears
             * only in the return type (`m: Map[V] = new_map()`) is bound from
             * the expected result. Only still-unbound parameters are filled, so
             * an argument-inferred T (`head([1,2,3])`) is never overwritten. */
            if (expected) {
                Subst esub;
                esub.names = f->tparams;
                esub.count = f->tparam_count;
                esub.types = xmalloc(sizeof(Type *) * f->tparam_count);
                memset(esub.types, 0, sizeof(Type *) * f->tparam_count);
                unify(f->ret, expected, &esub);
                for (size_t j = 0; j < f->tparam_count; j++)
                    if (!sub.types[j] && esub.types[j])
                        sub.types[j] = esub.types[j];
                free(esub.types);
            }
            for (size_t j = 0; j < f->tparam_count; j++)
                if (!sub.types[j]) sub.types[j] = &t_any;
            /* deferred lambdas get the partially-instantiated param types */
            for (size_t i = 0; i < argc; i++)
                if (islam[i])
                    argt[i] = infer_lambda(ck, e->as.call.args[i],
                                           ty_subst(f->params[argpi[i]], &sub));
            /* now unify everything, so lambda return types bind the rest */
            for (size_t i = 0; i < argc; i++)
                unify(f->params[argpi[i]], argt[i], &sub);
            for (size_t j = 0; j < f->tparam_count; j++)
                if (!sub.types[j]) sub.types[j] = &t_any;
            for (size_t i = 0; i < argc; i++) {
                Type *pi = ty_subst(f->params[argpi[i]], &sub);
                note_shape_crossing(ck, pi, argt[i]);
                ck_covariance(ck, pi, argt[i], e->line, e->col);
                if (!assignable(pi, argt[i]))
                    ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                               pi, argt[i],
                               "argument %zu of %s(): expected %s, got %s",
                               i + 1, dname, type_str(pi), type_str(argt[i]));
            }
            Type *ret = ty_subst(f->ret, &sub);
            free(sub.types); free(argpi);
            return ret;
        }
        /* an undefined name being called is its own error */
        if (!lookup_var(ck, name)) {
            ck_error(ck, "E_TYPE_UNDEFINED", e->line, e->col,
                     "call to undefined function '%s'", dname);
            return &t_any;
        }
        /* otherwise a function value held in a variable: indirect call */
    }

    Type *ft = ty_resolve(infer(ck, fn));
    if (ft->k != TY_FUNC) {
        ck_error(ck, "E_TYPE_NOT_CALLABLE", e->line, e->col,
                 "value of type %s is not callable", type_str(ft));
        return &t_any;
    }
    /* W3/D2: an impure function value is not callable from pure code */
    if (ck->cur_pure && ft->fun.eff != EFF_PURE)
        ck_error(ck, "E_TYPE_PURE_CALL", e->line, e->col,
                 "pure function calls impure function value");
    if (argc > ft->fun.count) {
        ck_error(ck, "E_TYPE_ARITY", e->line, e->col,
                 "function takes at most %zu argument%s, got %zu",
                 ft->fun.count, ft->fun.count == 1 ? "" : "s", argc);
        return ft->fun.ret;
    }
    for (size_t i = 0; i < argc; i++)
        if (islam[i])
            argt[i] = infer_lambda(ck, e->as.call.args[i], ft->fun.params[i]);
    for (size_t i = 0; i < argc; i++) {
        note_shape_crossing(ck, ft->fun.params[i], argt[i]);
        ck_covariance(ck, ft->fun.params[i], argt[i], e->line, e->col);
        if (!assignable(ft->fun.params[i], argt[i]))
            ck_error_t(ck, "E_TYPE_ARG", e->line, e->col,
                       ft->fun.params[i], argt[i],
                       "argument %zu: expected %s, got %s",
                       i + 1, type_str(ft->fun.params[i]), type_str(argt[i]));
    }
    return ft->fun.ret;
}
