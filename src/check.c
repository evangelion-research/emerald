/* Emerald type checker: gradual, structural typing in the TypeScript spirit.
 *
 * - Unannotated code is `any` and checks like dynamic Python.
 * - `type Name = {...}` declares a structural alias. Assignability between
 *   records is *width subtyping*: a record with more fields is assignable to
 *   a record type with fewer ("inheritance" without classes).
 * - `A & B` merges two record types (intersection). `A | B` is a union.
 * - Lists are covariant (like TS arrays: convenient, mildly unsound).
 *
 * The checker never mutates the AST; codegen is untyped and independent.
 */
#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TY_ANY, TY_NONE, TY_BOOL, TY_INT, TY_FLOAT, TY_STR,
    TY_LIST, TY_REC, TY_UNION,
} TyKind;

typedef struct Type Type;
struct Type {
    TyKind k;
    Type *elem;                                             /* TY_LIST */
    struct { char **names; Type **types; size_t count; } rec; /* TY_REC */
    struct { Type **alts; size_t count; } uni;              /* TY_UNION */
};

static Type t_any = {TY_ANY, 0, {0}, {0}}, t_none = {TY_NONE, 0, {0}, {0}},
            t_bool = {TY_BOOL, 0, {0}, {0}}, t_int = {TY_INT, 0, {0}, {0}},
            t_float = {TY_FLOAT, 0, {0}, {0}}, t_str = {TY_STR, 0, {0}, {0}};

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    return p;
}

static Type *ty_list(Type *elem) {
    Type *t = xmalloc(sizeof(Type));
    memset(t, 0, sizeof(Type));
    t->k = TY_LIST;
    t->elem = elem;
    return t;
}

/* --- type equality / assignability -------------------------------------- */

static bool type_eq(const Type *a, const Type *b) {
    if (a == b) return true;
    if (a->k != b->k) return false;
    switch (a->k) {
    case TY_LIST:
        return type_eq(a->elem, b->elem);
    case TY_REC: {
        if (a->rec.count != b->rec.count) return false;
        for (size_t i = 0; i < a->rec.count; i++) {
            bool found = false;
            for (size_t j = 0; j < b->rec.count; j++)
                if (strcmp(a->rec.names[i], b->rec.names[j]) == 0) {
                    if (!type_eq(a->rec.types[i], b->rec.types[j])) return false;
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    }
    case TY_UNION: {
        if (a->uni.count != b->uni.count) return false;
        for (size_t i = 0; i < a->uni.count; i++) {
            bool found = false;
            for (size_t j = 0; j < b->uni.count; j++)
                if (type_eq(a->uni.alts[i], b->uni.alts[j])) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    default:
        return true;
    }
}

static bool assignable(const Type *dst, const Type *src) {
    if (dst->k == TY_ANY || src->k == TY_ANY) return true;
    if (src->k == TY_UNION) { /* every alternative must fit */
        for (size_t i = 0; i < src->uni.count; i++)
            if (!assignable(dst, src->uni.alts[i])) return false;
        return true;
    }
    if (dst->k == TY_UNION) { /* some alternative must accept it */
        for (size_t i = 0; i < dst->uni.count; i++)
            if (assignable(dst->uni.alts[i], src)) return true;
        return false;
    }
    switch (dst->k) {
    case TY_INT:   return src->k == TY_INT || src->k == TY_BOOL;
    case TY_FLOAT: return src->k == TY_FLOAT || src->k == TY_INT || src->k == TY_BOOL;
    case TY_LIST:  return src->k == TY_LIST && assignable(dst->elem, src->elem);
    case TY_REC:   /* structural width subtyping */
        if (src->k != TY_REC) return false;
        for (size_t i = 0; i < dst->rec.count; i++) {
            bool found = false;
            for (size_t j = 0; j < src->rec.count; j++)
                if (strcmp(dst->rec.names[i], src->rec.names[j]) == 0) {
                    if (!assignable(dst->rec.types[i], src->rec.types[j]))
                        return false;
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    default:
        return dst->k == src->k;
    }
}

/* union of two types, flattened and deduplicated */
static Type *ty_join(Type *a, Type *b) {
    if (type_eq(a, b)) return a;
    if (a->k == TY_ANY || b->k == TY_ANY) return &t_any;
    Type **alts = xmalloc(sizeof(Type *) *
        ((a->k == TY_UNION ? a->uni.count : 1) +
         (b->k == TY_UNION ? b->uni.count : 1)));
    size_t n = 0;
    Type *parts[2] = {a, b};
    for (int p = 0; p < 2; p++) {
        Type *t = parts[p];
        size_t cnt = t->k == TY_UNION ? t->uni.count : 1;
        for (size_t i = 0; i < cnt; i++) {
            Type *alt = t->k == TY_UNION ? t->uni.alts[i] : t;
            bool dup = false;
            for (size_t j = 0; j < n; j++)
                if (type_eq(alts[j], alt)) { dup = true; break; }
            if (!dup) alts[n++] = alt;
        }
    }
    if (n == 1) return alts[0];
    Type *u = xmalloc(sizeof(Type));
    memset(u, 0, sizeof(Type));
    u->k = TY_UNION;
    u->uni.alts = alts;
    u->uni.count = n;
    return u;
}

/* --- printing types in error messages ----------------------------------- */

static void type_write(char *buf, size_t cap, const Type *t);

static void tw_append(char *buf, size_t cap, const char *s) {
    size_t l = strlen(buf);
    if (l + 1 < cap) snprintf(buf + l, cap - l, "%s", s);
}

static void type_write(char *buf, size_t cap, const Type *t) {
    switch (t->k) {
    case TY_ANY:   tw_append(buf, cap, "any"); break;
    case TY_NONE:  tw_append(buf, cap, "None"); break;
    case TY_BOOL:  tw_append(buf, cap, "bool"); break;
    case TY_INT:   tw_append(buf, cap, "int"); break;
    case TY_FLOAT: tw_append(buf, cap, "float"); break;
    case TY_STR:   tw_append(buf, cap, "str"); break;
    case TY_LIST:
        tw_append(buf, cap, "list[");
        type_write(buf, cap, t->elem);
        tw_append(buf, cap, "]");
        break;
    case TY_REC:
        tw_append(buf, cap, "{");
        for (size_t i = 0; i < t->rec.count; i++) {
            if (i) tw_append(buf, cap, ", ");
            tw_append(buf, cap, t->rec.names[i]);
            tw_append(buf, cap, ": ");
            type_write(buf, cap, t->rec.types[i]);
        }
        tw_append(buf, cap, "}");
        break;
    case TY_UNION:
        for (size_t i = 0; i < t->uni.count; i++) {
            if (i) tw_append(buf, cap, " | ");
            type_write(buf, cap, t->uni.alts[i]);
        }
        break;
    }
}

static const char *type_str(const Type *t) {
    static char bufs[4][256]; /* rotate so two types can appear in one message */
    static int which = 0;
    char *b = bufs[which];
    which = (which + 1) % 4;
    b[0] = '\0';
    type_write(b, 256, t);
    return b;
}

/* --- checker context ----------------------------------------------------- */

typedef struct {
    char *name;
    Type *type;
    bool annotated; /* explicit annotations are enforced; inferred ones widen */
    bool bound;     /* false until the first assignment executes */
} Var;

typedef struct { Var *items; size_t count, cap; } VarEnv;

typedef struct {
    char *name;
    Type **params;
    size_t param_count;
    Type *ret;
    const Stmt *node;
} FuncSig;

typedef struct {
    const char *filename;
    int errors;
    struct { char *name; Type *type; } *aliases;
    size_t alias_count, alias_cap;
    FuncSig *funcs;
    size_t func_count;
    VarEnv globals;
    VarEnv *locals;   /* NULL at top level */
    Type *cur_ret;    /* declared return type of the current function */
    int loop_depth;
} Ck;

static void ck_error(Ck *ck, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: type error: ", ck->filename, line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    ck->errors++;
}

static Var *env_find(VarEnv *env, const char *name) {
    for (size_t i = 0; i < env->count; i++)
        if (strcmp(env->items[i].name, name) == 0) return &env->items[i];
    return NULL;
}

static Var *env_add(VarEnv *env, const char *name, Type *t, bool annotated) {
    if (env->count == env->cap) {
        env->cap = env->cap ? env->cap * 2 : 8;
        env->items = realloc(env->items, sizeof(Var) * env->cap);
        if (!env->items) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    Var *v = &env->items[env->count++];
    v->name = (char *)name;
    v->type = t;
    v->annotated = annotated;
    v->bound = false;
    return v;
}

static bool is_builtin(const char *name) {
    return strcmp(name, "print") == 0 || strcmp(name, "len") == 0 ||
           strcmp(name, "range") == 0 || strcmp(name, "str") == 0 ||
           strcmp(name, "int") == 0;
}

static FuncSig *find_func(Ck *ck, const char *name) {
    for (size_t i = 0; i < ck->func_count; i++)
        if (strcmp(ck->funcs[i].name, name) == 0) return &ck->funcs[i];
    return NULL;
}

/* --- resolving surface type expressions ---------------------------------- */

static Type *resolve_type(Ck *ck, const TypeExpr *te) {
    if (!te) return &t_any;
    switch (te->kind) {
    case TE_NAME:
        if (strcmp(te->name, "any") == 0) return &t_any;
        if (strcmp(te->name, "None") == 0) return &t_none;
        if (strcmp(te->name, "bool") == 0) return &t_bool;
        if (strcmp(te->name, "int") == 0) return &t_int;
        if (strcmp(te->name, "float") == 0) return &t_float;
        if (strcmp(te->name, "str") == 0) return &t_str;
        for (size_t i = 0; i < ck->alias_count; i++)
            if (strcmp(ck->aliases[i].name, te->name) == 0)
                return ck->aliases[i].type;
        ck_error(ck, te->line, "unknown type '%s'", te->name);
        return &t_any;
    case TE_LIST:
        return ty_list(resolve_type(ck, te->elem));
    case TE_REC: {
        Type *t = xmalloc(sizeof(Type));
        memset(t, 0, sizeof(Type));
        t->k = TY_REC;
        t->rec.names = te->fields.names;
        t->rec.types = xmalloc(sizeof(Type *) * (te->fields.count ? te->fields.count : 1));
        t->rec.count = te->fields.count;
        for (size_t i = 0; i < te->fields.count; i++)
            t->rec.types[i] = resolve_type(ck, te->fields.types[i]);
        return t;
    }
    case TE_UNION:
        return ty_join(resolve_type(ck, te->lhs), resolve_type(ck, te->rhs));
    case TE_INTER: {
        Type *a = resolve_type(ck, te->lhs);
        Type *b = resolve_type(ck, te->rhs);
        if (a->k != TY_REC || b->k != TY_REC) {
            ck_error(ck, te->line, "'&' requires two record types, got %s and %s",
                     type_str(a), type_str(b));
            return &t_any;
        }
        /* merge; fields from the right side override */
        Type *t = xmalloc(sizeof(Type));
        memset(t, 0, sizeof(Type));
        t->k = TY_REC;
        size_t max = a->rec.count + b->rec.count;
        t->rec.names = xmalloc(sizeof(char *) * (max ? max : 1));
        t->rec.types = xmalloc(sizeof(Type *) * (max ? max : 1));
        for (size_t i = 0; i < a->rec.count; i++) {
            t->rec.names[t->rec.count] = a->rec.names[i];
            t->rec.types[t->rec.count] = a->rec.types[i];
            t->rec.count++;
        }
        for (size_t i = 0; i < b->rec.count; i++) {
            bool replaced = false;
            for (size_t j = 0; j < t->rec.count; j++)
                if (strcmp(t->rec.names[j], b->rec.names[i]) == 0) {
                    t->rec.types[j] = b->rec.types[i];
                    replaced = true;
                    break;
                }
            if (!replaced) {
                t->rec.names[t->rec.count] = b->rec.names[i];
                t->rec.types[t->rec.count] = b->rec.types[i];
                t->rec.count++;
            }
        }
        return t;
    }
    }
    return &t_any;
}

/* --- expression inference ------------------------------------------------ */

static Type *infer(Ck *ck, const Expr *e);

static bool is_numeric(const Type *t) {
    return t->k == TY_INT || t->k == TY_FLOAT || t->k == TY_BOOL || t->k == TY_ANY;
}

static Type *infer_binop(Ck *ck, const Expr *e) {
    Type *l = infer(ck, e->as.bin.lhs);
    Type *r = infer(ck, e->as.bin.rhs);
    BinOp op = e->as.bin.op;

    switch (op) {
    case B_AND: case B_OR:
        return ty_join(l, r); /* Python semantics: result is one operand */
    case B_EQ: case B_NE:
        return &t_bool;
    case B_LT: case B_LE: case B_GT: case B_GE: {
        bool ok = (is_numeric(l) && is_numeric(r)) ||
                  (l->k == TY_STR && r->k == TY_STR) ||
                  (l->k == TY_LIST && r->k == TY_LIST) ||
                  l->k == TY_ANY || r->k == TY_ANY ||
                  l->k == TY_UNION || r->k == TY_UNION;
        if (!ok)
            ck_error(ck, e->line, "cannot order %s and %s", type_str(l), type_str(r));
        return &t_bool;
    }
    case B_ADD:
        if (l->k == TY_ANY || r->k == TY_ANY) return &t_any;
        if (l->k == TY_STR && r->k == TY_STR) return &t_str;
        if (l->k == TY_LIST && r->k == TY_LIST)
            return ty_list(ty_join(l->elem, r->elem));
        /* fall through to arithmetic */
        /* FALLTHROUGH */
    case B_SUB: case B_MUL: case B_DIV: case B_MOD: {
        if (l->k == TY_ANY || r->k == TY_ANY) return &t_any;
        if (op == B_MUL) { /* "ab" * 3, [0] * n */
            if (l->k == TY_STR && r->k == TY_INT) return &t_str;
            if (l->k == TY_INT && r->k == TY_STR) return &t_str;
            if (l->k == TY_LIST && r->k == TY_INT) return l;
            if (l->k == TY_INT && r->k == TY_LIST) return r;
        }
        if (!is_numeric(l) || !is_numeric(r) ||
            l->k == TY_UNION || r->k == TY_UNION) {
            static const char *names[] = {"+", "-", "*", "/", "%"};
            ck_error(ck, e->line, "unsupported operand types for %s: %s and %s",
                     names[op], type_str(l), type_str(r));
            return &t_any;
        }
        if (op == B_DIV) return &t_float; /* Python 3 semantics */
        if (l->k == TY_FLOAT || r->k == TY_FLOAT) return &t_float;
        return &t_int;
    }
    }
    return &t_any;
}

static Type *infer_call(Ck *ck, const Expr *e) {
    const char *name = e->as.call.fn->as.sval;
    size_t argc = e->as.call.count;
    Type **argt = xmalloc(sizeof(Type *) * (argc ? argc : 1));
    for (size_t i = 0; i < argc; i++)
        argt[i] = infer(ck, e->as.call.args[i]);

    if (strcmp(name, "print") == 0) return &t_none;
    if (strcmp(name, "len") == 0) {
        if (argc != 1)
            ck_error(ck, e->line, "len() takes 1 argument, got %zu", argc);
        else if (argt[0]->k != TY_ANY && argt[0]->k != TY_STR &&
                 argt[0]->k != TY_LIST && argt[0]->k != TY_REC &&
                 argt[0]->k != TY_UNION)
            ck_error(ck, e->line, "%s has no len()", type_str(argt[0]));
        return &t_int;
    }
    if (strcmp(name, "range") == 0) {
        if (argc != 1 && argc != 2)
            ck_error(ck, e->line, "range() takes 1 or 2 arguments, got %zu", argc);
        for (size_t i = 0; i < argc; i++)
            if (!assignable(&t_int, argt[i]))
                ck_error(ck, e->line, "range() argument %zu must be int, got %s",
                         i + 1, type_str(argt[i]));
        return ty_list(&t_int);
    }
    if (strcmp(name, "str") == 0) {
        if (argc != 1)
            ck_error(ck, e->line, "str() takes 1 argument, got %zu", argc);
        return &t_str;
    }
    if (strcmp(name, "int") == 0) {
        if (argc != 1)
            ck_error(ck, e->line, "int() takes 1 argument, got %zu", argc);
        return &t_int;
    }

    FuncSig *f = find_func(ck, name);
    if (!f) {
        ck_error(ck, e->line, "call to undefined function '%s'", name);
        return &t_any;
    }
    if (argc != f->param_count) {
        ck_error(ck, e->line, "%s() takes %zu argument%s, got %zu", name,
                 f->param_count, f->param_count == 1 ? "" : "s", argc);
        return f->ret;
    }
    for (size_t i = 0; i < argc; i++)
        if (!assignable(f->params[i], argt[i]))
            ck_error(ck, e->line,
                     "argument %zu of %s(): expected %s, got %s",
                     i + 1, name, type_str(f->params[i]), type_str(argt[i]));
    return f->ret;
}

static Type *infer(Ck *ck, const Expr *e) {
    switch (e->kind) {
    case E_INT:   return &t_int;
    case E_FLOAT: return &t_float;
    case E_STR:   return &t_str;
    case E_TRUE: case E_FALSE: return &t_bool;
    case E_NONE:  return &t_none;
    case E_NAME: {
        Var *v = ck->locals ? env_find(ck->locals, e->as.sval) : NULL;
        if (!v) v = env_find(&ck->globals, e->as.sval);
        if (v) return v->type;
        if (find_func(ck, e->as.sval) || is_builtin(e->as.sval)) {
            ck_error(ck, e->line,
                     "'%s' is a function; functions are not values in Emerald",
                     e->as.sval);
            return &t_any;
        }
        ck_error(ck, e->line, "undefined name '%s'", e->as.sval);
        return &t_any;
    }
    case E_LIST: {
        if (e->as.list.count == 0) return ty_list(&t_any);
        Type *elem = infer(ck, e->as.list.items[0]);
        for (size_t i = 1; i < e->as.list.count; i++)
            elem = ty_join(elem, infer(ck, e->as.list.items[i]));
        return ty_list(elem);
    }
    case E_REC: {
        Type *t = xmalloc(sizeof(Type));
        memset(t, 0, sizeof(Type));
        t->k = TY_REC;
        t->rec.names = e->as.rec.names;
        t->rec.types = xmalloc(sizeof(Type *) * (e->as.rec.count ? e->as.rec.count : 1));
        t->rec.count = e->as.rec.count;
        for (size_t i = 0; i < e->as.rec.count; i++)
            t->rec.types[i] = infer(ck, e->as.rec.values[i]);
        return t;
    }
    case E_BINOP:
        return infer_binop(ck, e);
    case E_UNOP: {
        Type *t = infer(ck, e->as.un.operand);
        if (e->as.un.op == U_NOT) return &t_bool;
        if (t->k == TY_INT || t->k == TY_BOOL) return &t_int;
        if (t->k == TY_FLOAT) return &t_float;
        if (t->k != TY_ANY)
            ck_error(ck, e->line, "unsupported operand type for unary -: %s",
                     type_str(t));
        return &t_any;
    }
    case E_CALL:
        return infer_call(ck, e);
    case E_INDEX: {
        Type *seq = infer(ck, e->as.index.seq);
        Type *idx = infer(ck, e->as.index.idx);
        if (!assignable(&t_int, idx) || idx->k == TY_FLOAT)
            ck_error(ck, e->line, "index must be int, got %s", type_str(idx));
        if (seq->k == TY_LIST) return seq->elem;
        if (seq->k == TY_STR) return &t_str;
        if (seq->k == TY_ANY || seq->k == TY_UNION) return &t_any;
        ck_error(ck, e->line, "%s is not indexable", type_str(seq));
        return &t_any;
    }
    case E_ATTR: {
        Type *obj = infer(ck, e->as.attr.obj);
        if (obj->k == TY_ANY) return &t_any;
        if (obj->k == TY_REC) {
            for (size_t i = 0; i < obj->rec.count; i++)
                if (strcmp(obj->rec.names[i], e->as.attr.name) == 0)
                    return obj->rec.types[i];
            ck_error(ck, e->line, "type %s has no field '%s'",
                     type_str(obj), e->as.attr.name);
            return &t_any;
        }
        if (obj->k == TY_UNION) { /* the field must exist on every alternative */
            Type *result = NULL;
            for (size_t i = 0; i < obj->uni.count; i++) {
                Type *alt = obj->uni.alts[i];
                Type *ft = NULL;
                if (alt->k == TY_ANY) { ft = &t_any; }
                else if (alt->k == TY_REC) {
                    for (size_t j = 0; j < alt->rec.count; j++)
                        if (strcmp(alt->rec.names[j], e->as.attr.name) == 0) {
                            ft = alt->rec.types[j];
                            break;
                        }
                }
                if (!ft) {
                    ck_error(ck, e->line,
                             "field '%s' does not exist on every alternative of %s",
                             e->as.attr.name, type_str(obj));
                    return &t_any;
                }
                result = result ? ty_join(result, ft) : ft;
            }
            return result ? result : &t_any;
        }
        ck_error(ck, e->line, "type %s has no fields (looking for '%s')",
                 type_str(obj), e->as.attr.name);
        return &t_any;
    }
    }
    return &t_any;
}

/* --- statements ---------------------------------------------------------- */

static void check_block(Ck *ck, const Block *b);

static void check_assign(Ck *ck, const Stmt *s) {
    Type *val = infer(ck, s->as.assign.value);
    Expr *target = s->as.assign.target;

    if (target->kind == E_NAME) {
        const char *name = target->as.sval;
        if (is_builtin(name) || find_func(ck, name)) {
            ck_error(ck, s->line, "cannot assign to function name '%s'", name);
            return;
        }
        VarEnv *env = ck->locals ? ck->locals : &ck->globals;
        Var *v = env_find(env, name);
        if (!v && ck->locals) v = env_find(&ck->globals, name); /* docs rule */
        if (s->as.assign.ann) {
            Type *ann = resolve_type(ck, s->as.assign.ann);
            if (!assignable(ann, val))
                ck_error(ck, s->line, "cannot assign %s to '%s' declared as %s",
                         type_str(val), name, type_str(ann));
            if (v) {
                v->type = ann;
                v->annotated = true;
                v->bound = true;
            } else {
                env_add(env, name, ann, true)->bound = true;
            }
            return;
        }
        if (!v) {
            env_add(env, name, val, false)->bound = true;
            return;
        }
        if (!v->bound) { /* first assignment fixes the inferred type */
            v->type = v->annotated ? v->type : val;
            v->bound = true;
            if (v->annotated && !assignable(v->type, val))
                ck_error(ck, s->line, "cannot assign %s to '%s' declared as %s",
                         type_str(val), name, type_str(v->type));
            return;
        }
        if (assignable(v->type, val)) return;
        if (v->annotated)
            ck_error(ck, s->line, "cannot assign %s to '%s' declared as %s",
                     type_str(val), name, type_str(v->type));
        else
            v->type = ty_join(v->type, val); /* inferred vars widen, gradual-style */
        return;
    }

    if (target->kind == E_INDEX) {
        Type *seq = infer(ck, target->as.index.seq);
        Type *idx = infer(ck, target->as.index.idx);
        if (!assignable(&t_int, idx) || idx->k == TY_FLOAT)
            ck_error(ck, s->line, "index must be int, got %s", type_str(idx));
        if (seq->k == TY_STR) {
            ck_error(ck, s->line, "strings are immutable");
            return;
        }
        if (seq->k == TY_LIST && !assignable(seq->elem, val))
            ck_error(ck, s->line, "cannot store %s in %s",
                     type_str(val), type_str(seq));
        else if (seq->k != TY_LIST && seq->k != TY_ANY && seq->k != TY_UNION)
            ck_error(ck, s->line, "%s does not support item assignment",
                     type_str(seq));
        return;
    }

    /* E_ATTR */
    Type *obj = infer(ck, target->as.attr.obj);
    if (obj->k == TY_ANY) return;
    if (obj->k != TY_REC) {
        ck_error(ck, s->line, "type %s has no fields (assigning '%s')",
                 type_str(obj), target->as.attr.name);
        return;
    }
    for (size_t i = 0; i < obj->rec.count; i++)
        if (strcmp(obj->rec.names[i], target->as.attr.name) == 0) {
            if (!assignable(obj->rec.types[i], val))
                ck_error(ck, s->line, "cannot assign %s to field '%s' of type %s",
                         type_str(val), target->as.attr.name,
                         type_str(obj->rec.types[i]));
            return;
        }
    ck_error(ck, s->line, "type %s has no field '%s'",
             type_str(obj), target->as.attr.name);
}

static void check_stmt(Ck *ck, const Stmt *s) {
    switch (s->kind) {
    case S_EXPR:
        infer(ck, s->as.expr);
        break;
    case S_ASSIGN:
        check_assign(ck, s);
        break;
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++) {
            infer(ck, s->as.ifs.conds[i]);
            check_block(ck, &s->as.ifs.blocks[i]);
        }
        if (s->as.ifs.has_else) check_block(ck, &s->as.ifs.else_block);
        break;
    case S_WHILE:
        infer(ck, s->as.wh.cond);
        ck->loop_depth++;
        check_block(ck, &s->as.wh.body);
        ck->loop_depth--;
        break;
    case S_FOR: {
        Type *seq = infer(ck, s->as.fr.seq);
        Type *elem = &t_any;
        if (seq->k == TY_LIST) elem = seq->elem;
        else if (seq->k == TY_STR) elem = &t_str;
        else if (seq->k != TY_ANY && seq->k != TY_UNION)
            ck_error(ck, s->line, "%s is not iterable", type_str(seq));
        VarEnv *env = ck->locals ? ck->locals : &ck->globals;
        Var *v = env_find(env, s->as.fr.var);
        if (!v && ck->locals) v = env_find(&ck->globals, s->as.fr.var);
        if (!v) v = env_add(env, s->as.fr.var, elem, false);
        else if (!v->annotated) v->type = v->bound ? ty_join(v->type, elem) : elem;
        else if (!assignable(v->type, elem))
            ck_error(ck, s->line, "loop assigns %s to '%s' declared as %s",
                     type_str(elem), s->as.fr.var, type_str(v->type));
        v->bound = true;
        ck->loop_depth++;
        check_block(ck, &s->as.fr.body);
        ck->loop_depth--;
        break;
    }
    case S_RETURN: {
        if (!ck->locals) {
            ck_error(ck, s->line, "'return' outside of a function");
            break;
        }
        Type *t = s->as.ret ? infer(ck, s->as.ret) : &t_none;
        if (!assignable(ck->cur_ret, t))
            ck_error(ck, s->line, "returning %s from a function declared to return %s",
                     type_str(t), type_str(ck->cur_ret));
        break;
    }
    case S_BREAK:
        if (ck->loop_depth == 0)
            ck_error(ck, s->line, "'break' outside of a loop");
        break;
    case S_CONTINUE:
        if (ck->loop_depth == 0)
            ck_error(ck, s->line, "'continue' outside of a loop");
        break;
    case S_PASS:
        break;
    case S_BLOCK:
        check_block(ck, &s->as.block);
        break;
    case S_FUNC:
        if (ck->locals)
            ck_error(ck, s->line, "nested functions are not supported yet");
        /* bodies are checked in a dedicated pass; nothing to do here */
        break;
    case S_TYPEDEF:
        /* resolved during the signature pass */
        break;
    }
}

static void check_block(Ck *ck, const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        check_stmt(ck, b->items[i]);
}

/* --- passes -------------------------------------------------------------- */

typedef struct { Ck *ck; VarEnv *env; const char *skip; } DeclCtx;

static void declare_local(const char *name, int line, void *ud) {
    DeclCtx *dc = ud;
    (void)line;
    /* the docs rule: assigning a global's name inside a def updates the global */
    if (dc->skip == NULL || !env_find(&dc->ck->globals, name))
        if (!env_find(dc->env, name))
            env_add(dc->env, name, &t_any, false);
}

static void check_func_body(Ck *ck, const Stmt *s) {
    VarEnv locals = {0};
    for (size_t i = 0; i < s->as.func.param_count; i++) {
        Type *pt = resolve_type(ck, s->as.func.param_types[i]);
        Var *v = env_add(&locals, s->as.func.params[i], pt,
                         s->as.func.param_types[i] != NULL);
        v->bound = true;
    }
    /* pre-declare every assigned-in-body name that isn't a global */
    DeclCtx dc = { ck, &locals, "function" };
    ast_collect_assigned(&s->as.func.body, declare_local, &dc);

    ck->locals = &locals;
    ck->cur_ret = resolve_type(ck, s->as.func.ret_type);
    check_block(ck, &s->as.func.body);
    ck->locals = NULL;
    ck->cur_ret = NULL;
    free(locals.items);
}

int check_program(const Program *prog, const char *filename) {
    Ck ck;
    memset(&ck, 0, sizeof(ck));
    ck.filename = filename;

    /* pass 1a: type aliases, in file order (use-before-definition is an error) */
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_TYPEDEF) continue;
        if (ck.alias_count == ck.alias_cap) {
            ck.alias_cap = ck.alias_cap ? ck.alias_cap * 2 : 8;
            ck.aliases = realloc(ck.aliases, sizeof(*ck.aliases) * ck.alias_cap);
            if (!ck.aliases) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
        }
        Type *t = resolve_type(&ck, s->as.tdef.value);
        ck.aliases[ck.alias_count].name = s->as.tdef.name;
        ck.aliases[ck.alias_count].type = t;
        ck.alias_count++;
    }

    /* pass 1b: function signatures */
    ck.funcs = xmalloc(sizeof(FuncSig) * (prog->body.count ? prog->body.count : 1));
    for (size_t i = 0; i < prog->body.count; i++) {
        const Stmt *s = prog->body.items[i];
        if (s->kind != S_FUNC) continue;
        if (is_builtin(s->as.func.name)) {
            ck_error(&ck, s->line, "cannot redefine builtin '%s'", s->as.func.name);
            continue;
        }
        if (find_func(&ck, s->as.func.name)) {
            ck_error(&ck, s->line, "function '%s' is already defined", s->as.func.name);
            continue;
        }
        FuncSig *f = &ck.funcs[ck.func_count++];
        f->name = s->as.func.name;
        f->param_count = s->as.func.param_count;
        f->params = xmalloc(sizeof(Type *) * (f->param_count ? f->param_count : 1));
        for (size_t j = 0; j < f->param_count; j++)
            f->params[j] = resolve_type(&ck, s->as.func.param_types[j]);
        f->ret = resolve_type(&ck, s->as.func.ret_type);
        f->node = s;
    }

    /* pass 2a: top-level statements (this populates global variable types) */
    check_block(&ck, &prog->body);

    /* pass 2b: function bodies, with all globals known */
    for (size_t i = 0; i < ck.func_count; i++)
        check_func_body(&ck, ck.funcs[i].node);

    return ck.errors;
}
