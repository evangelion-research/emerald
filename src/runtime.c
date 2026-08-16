/* Emerald runtime implementation. See runtime.h for the model. */
#include "runtime.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strings of SSO_MAX bytes or fewer live inline in the Value (see runtime.h). */
#define SSO_MAX 7

RootFrame *rt_roots = NULL;

/* ---------------------------------------------------------------------- */
/* GC                                                                      */
/* ---------------------------------------------------------------------- */

/* Two-generation mark-and-sweep. New objects are born in the nursery
 * (gc_young); a minor collection sweeps only the nursery and promotes
 * survivors to gc_old. A major (full) collection sweeps both generations.
 * Tenured objects mutated to reference nursery objects are tracked in
 * gc_remembered so a minor collection finds them without scanning old. */
#define GEN_YOUNG 0
#define GEN_OLD   1

static Obj *gc_young = NULL;       /* nursery: allocated since the last minor GC */
static Obj *gc_old = NULL;         /* tenured survivors */
static Obj *gc_remembered = NULL;  /* tenured objects that may point into the nursery */
static size_t gc_young_count = 0;
static size_t gc_old_count = 0;
static size_t gc_live = 0;         /* objects surviving the last collection */
static size_t gc_young_threshold = 256;
static size_t gc_old_threshold = 256;
static size_t gc_collections = 0;  /* total cycles (minor + major) */

void rt_init(void) { /* nothing yet; reserved for future config */ }

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("emerald: runtime error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) rt_fatal("out of memory");
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) rt_fatal("out of memory");
    return q;
}

static void gc_mark_value(Value v, bool minor);

static void gc_mark_obj(Obj *o, bool minor) {
    if (o == NULL || o->mark) return;
    /* In a minor collection the tenured generation is opaque: only objects in
     * the remembered set (which may point into the nursery) are walked. */
    if (minor && o->gen == GEN_OLD && !o->remembered) return;
    o->mark = true;
    switch (o->tag) {
    case O_STR:
        break;
    case O_LIST:
        for (size_t i = 0; i < o->as.list.len; i++)
            gc_mark_value(o->as.list.items[i], minor);
        break;
    case O_REC:
        for (size_t i = 0; i < o->as.rec.len; i++)
            gc_mark_value(o->as.rec.vals[i], minor);
        break;
    case O_FUNC:
        for (size_t i = 0; i < o->as.func.env_count; i++)
            gc_mark_value(o->as.func.env[i], minor);
        break;
    case O_CELL:
        gc_mark_value(o->as.cell.val, minor);
        break;
    }
}

static void gc_mark_value(Value v, bool minor) {
    if (v.tag == V_OBJ) gc_mark_obj(v.as.o, minor);
}

static void gc_mark_roots(bool minor) {
    for (RootFrame *f = rt_roots; f; f = f->prev)
        for (size_t i = 0; i < f->count; i++)
            gc_mark_value(f->slots[i], minor);
}

static void gc_free_obj(Obj *o) {
    switch (o->tag) {
    case O_STR:  free(o->as.str.data); break;
    case O_LIST: free(o->as.list.items); break;
    case O_REC:  free(o->as.rec.keys); free(o->as.rec.vals); break;
    case O_FUNC: free(o->as.func.env); break;
    case O_CELL: break;
    }
    free(o);
}

/* Write barrier: remember a tenured container that is made to reference a
 * nursery object, so the next minor collection knows to look through it. */
static void gc_write_barrier(Obj *container, Value v) {
    if (container->gen == GEN_OLD && v.tag == V_OBJ &&
        v.as.o->gen == GEN_YOUNG && !container->remembered) {
        container->remembered = true;
        container->rem_next = gc_remembered;
        gc_remembered = container;
    }
}

/* Free unmarked nursery objects and promote survivors to the old generation. */
static void gc_sweep_young(void) {
    Obj **link = &gc_young;
    while (*link) {
        Obj *o = *link;
        if (o->mark) {
            o->mark = false;
            *link = o->gc_next;
            o->gen = GEN_OLD;
            o->gc_next = gc_old;
            gc_old = o;
            gc_young_count--;
            gc_old_count++;
        } else {
            *link = o->gc_next;
            gc_free_obj(o);
            gc_young_count--;
        }
    }
}

/* Clear the traversal marks left on remembered tenured objects this cycle. */
static void gc_clear_remembered(void) {
    Obj *r = gc_remembered;
    while (r) {
        Obj *n = r->rem_next;
        r->mark = false;
        r->remembered = false;
        r->rem_next = NULL;
        r = n;
    }
    gc_remembered = NULL;
}

static void gc_collect_minor(void) {
    gc_collections++;
    gc_mark_roots(true);
    for (Obj *r = gc_remembered; r; r = r->rem_next)
        gc_mark_obj(r, true);
    gc_sweep_young();
    gc_clear_remembered();
    gc_live = gc_old_count;
    gc_young_threshold = gc_old_count * 2 < 256 ? 256 : gc_old_count * 2;
}

static void gc_collect_major(void) {
    gc_collections++;
    gc_mark_roots(false);
    /* Sweep old first, then promote young survivors, so freshly promoted
     * objects aren't seen (and freed) by the old-generation sweep. */
    Obj **link = &gc_old;
    while (*link) {
        Obj *o = *link;
        if (o->mark) {
            o->mark = false;
            o->remembered = false;
            o->rem_next = NULL;
            link = &o->gc_next;
        } else {
            *link = o->gc_next;
            gc_free_obj(o);
            gc_old_count--;
        }
    }
    gc_sweep_young();
    gc_remembered = NULL;
    gc_live = gc_old_count;
    gc_old_threshold = gc_live * 2 < 256 ? 256 : gc_live * 2;
    gc_young_threshold = gc_live * 2 < 256 ? 256 : gc_live * 2;
}

void rt_gc_collect(void) { gc_collect_major(); }

static Obj *rt_obj_new(OTag tag) {
    if (gc_young_count >= gc_young_threshold) gc_collect_minor();
    if (gc_old_count >= gc_old_threshold) gc_collect_major();
    Obj *o = xmalloc(sizeof(Obj));
    memset(o, 0, sizeof(Obj));
    o->tag = tag;
    o->gen = GEN_YOUNG;
    o->gc_next = gc_young;
    gc_young = o;
    gc_young_count++;
    return o;
}

/* ---------------------------------------------------------------------- */
/* constructors                                                            */
/* ---------------------------------------------------------------------- */

static Value obj_val(Obj *o) { Value v; v.tag = V_OBJ; v.as.o = o; return v; }

/* Copy data[0..len) into a Value: inline (V_STR) when short, heap Obj otherwise. */
static Value str_copy(const char *data, size_t len) {
    if (len <= SSO_MAX) {
        Value v;
        v.tag = V_STR;
        memcpy(v.as.s.bytes, data, len);
        v.as.s.bytes[len] = '\0';
        return v;
    }
    char *d = xmalloc(len + 1);
    memcpy(d, data, len);
    d[len] = '\0';
    Obj *o = rt_obj_new(O_STR);
    o->as.str.data = d;
    o->as.str.len = len;
    return obj_val(o);
}

static Value str_take(char *data, size_t len) { /* takes ownership of data */
    if (len <= SSO_MAX) {
        Value v = str_copy(data, len);
        free(data);
        return v;
    }
    Obj *o = rt_obj_new(O_STR);
    o->as.str.data = data;
    o->as.str.len = len;
    return obj_val(o);
}

Value em_str_new(const char *cstr) {
    return str_copy(cstr, strlen(cstr));
}

Value em_list_litn(size_t n, ...) {
    Obj *o = rt_obj_new(O_LIST);
    o->as.list.items = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.list.cap = n ? n : 1;
    va_list ap;
    va_start(ap, n);
    for (size_t i = 0; i < n; i++)
        o->as.list.items[i] = va_arg(ap, Value);
    va_end(ap);
    o->as.list.len = n;
    return obj_val(o);
}

Value em_rec_litn(size_t n, ...) {
    Obj *o = rt_obj_new(O_REC);
    o->as.rec.keys = xmalloc(sizeof(char *) * (n ? n : 1));
    o->as.rec.vals = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.rec.cap = n ? n : 1;
    va_list ap;
    va_start(ap, n);
    for (size_t i = 0; i < n; i++) {
        o->as.rec.keys[i] = va_arg(ap, const char *);
        o->as.rec.vals[i] = va_arg(ap, Value);
    }
    va_end(ap);
    o->as.rec.len = n;
    return obj_val(o);
}

/* ---------------------------------------------------------------------- */
/* helpers                                                                 */
/* ---------------------------------------------------------------------- */

static const char *type_name(Value v) {
    switch (v.tag) {
    case V_NONE:  return "None";
    case V_BOOL:  return "bool";
    case V_INT:   return "int";
    case V_FLOAT: return "float";
    case V_STR:  return "str";
    case V_OBJ:
        switch (v.as.o->tag) {
        case O_STR:  return "str";
        case O_LIST: return "list";
        case O_REC:  return "record";
        case O_FUNC: return "function";
        case O_CELL: return "cell";
        }
    }
    return "?";
}

static bool is_str(Value v)  { return v.tag == V_STR || (v.tag == V_OBJ && v.as.o->tag == O_STR); }
static bool is_list(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_LIST; }
static bool is_rec(Value v)  { return v.tag == V_OBJ && v.as.o->tag == O_REC; }
static bool is_num(Value v)  { return v.tag == V_INT || v.tag == V_FLOAT || v.tag == V_BOOL; }

/* Uniform access to inline (V_STR) and heap (O_STR) strings; both are
 * NUL-terminated, so str_data can be handed to C string functions. Takes a
 * pointer so the returned bytes point into the caller's stable storage. */
static const char *str_data(const Value *v) {
    return v->tag == V_STR ? v->as.s.bytes : v->as.o->as.str.data;
}
static size_t str_len(const Value *v) {
    return v->tag == V_STR ? strlen(v->as.s.bytes) : v->as.o->as.str.len;
}

static double as_double(Value v) {
    switch (v.tag) {
    case V_BOOL:  return v.as.b ? 1.0 : 0.0;
    case V_INT:   return (double)v.as.i;
    case V_FLOAT: return v.as.f;
    default:      return 0.0;
    }
}
static int64_t as_int(Value v) { return v.tag == V_BOOL ? (v.as.b ? 1 : 0) : v.as.i; }

/* ---------------------------------------------------------------------- */
/* string building (plain malloc buffer; never triggers GC mid-build)      */
/* ---------------------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } SB;

static void sb_put(SB *sb, const char *s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 64;
        while (sb->cap < sb->len + n + 1) sb->cap *= 2;
        sb->buf = xrealloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}
static void sb_puts(SB *sb, const char *s) { sb_put(sb, s, strlen(s)); }

static void format_float(SB *sb, double f) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.12g", f);
    sb_puts(sb, tmp);
    if (!strpbrk(tmp, ".eEni")) sb_puts(sb, ".0"); /* 3 -> 3.0 (skip inf/nan) */
}

/* repr: quotes strings; str: raw strings. Containers always use repr inside. */
static void write_value(SB *sb, Value v, bool repr) {
    char tmp[32];
    switch (v.tag) {
    case V_NONE:  sb_puts(sb, "None"); break;
    case V_BOOL:  sb_puts(sb, v.as.b ? "True" : "False"); break;
    case V_INT:
        snprintf(tmp, sizeof tmp, "%" PRId64, v.as.i);
        sb_puts(sb, tmp);
        break;
    case V_FLOAT: format_float(sb, v.as.f); break;
    case V_STR:
        if (repr) sb_puts(sb, "'");
        sb_put(sb, str_data(&v), str_len(&v));
        if (repr) sb_puts(sb, "'");
        break;
    case V_OBJ:
        switch (v.as.o->tag) {
        case O_STR:
            if (repr) {
                sb_puts(sb, "'");
                sb_put(sb, str_data(&v), str_len(&v));
                sb_puts(sb, "'");
            } else {
                sb_put(sb, str_data(&v), str_len(&v));
            }
            break;
        case O_LIST:
            sb_puts(sb, "[");
            for (size_t i = 0; i < v.as.o->as.list.len; i++) {
                if (i) sb_puts(sb, ", ");
                write_value(sb, v.as.o->as.list.items[i], true);
            }
            sb_puts(sb, "]");
            break;
        case O_REC:
            sb_puts(sb, "{");
            for (size_t i = 0; i < v.as.o->as.rec.len; i++) {
                if (i) sb_puts(sb, ", ");
                sb_puts(sb, v.as.o->as.rec.keys[i]);
                sb_puts(sb, ": ");
                write_value(sb, v.as.o->as.rec.vals[i], true);
            }
            sb_puts(sb, "}");
            break;
        case O_FUNC:
            sb_puts(sb, "<function>");
            break;
        case O_CELL: /* cells are internal; print their contents */
            write_value(sb, v.as.o->as.cell.val, repr);
            break;
        }
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* operators                                                               */
/* ---------------------------------------------------------------------- */

Value em_add(Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT) return em_int(a.as.i + b.as.i);
    if (is_num(a) && is_num(b)) {
        if (a.tag == V_FLOAT || b.tag == V_FLOAT)
            return em_float(as_double(a) + as_double(b));
        return em_int(as_int(a) + as_int(b));
    }
    if (is_str(a) && is_str(b)) {
        size_t la = str_len(&a), lb = str_len(&b);
        char *d = xmalloc(la + lb + 1);
        memcpy(d, str_data(&a), la);
        memcpy(d + la, str_data(&b), lb);
        d[la + lb] = '\0';
        return str_take(d, la + lb);
    }
    if (is_list(a) && is_list(b)) {
        Obj *o = rt_obj_new(O_LIST); /* a and b stay rooted by the caller */
        size_t n = a.as.o->as.list.len + b.as.o->as.list.len;
        o->as.list.items = xmalloc(sizeof(Value) * (n ? n : 1));
        o->as.list.cap = n ? n : 1;
        memcpy(o->as.list.items, a.as.o->as.list.items,
               sizeof(Value) * a.as.o->as.list.len);
        memcpy(o->as.list.items + a.as.o->as.list.len, b.as.o->as.list.items,
               sizeof(Value) * b.as.o->as.list.len);
        o->as.list.len = n;
        return obj_val(o);
    }
    rt_fatal("unsupported operand types for +: %s and %s", type_name(a), type_name(b));
    return em_none();
}

#define NUM_BINOP(name, op, sym)                                              \
    Value name(Value a, Value b) {                                            \
        if (a.tag == V_INT && b.tag == V_INT) return em_int(a.as.i op b.as.i);\
        if (is_num(a) && is_num(b)) {                                         \
            if (a.tag == V_FLOAT || b.tag == V_FLOAT)                         \
                return em_float(as_double(a) op as_double(b));                \
            return em_int(as_int(a) op as_int(b));                            \
        }                                                                     \
        rt_fatal("unsupported operand types for " sym ": %s and %s",          \
                 type_name(a), type_name(b));                                 \
        return em_none();                                                     \
    }

NUM_BINOP(em_sub, -, "-")

Value em_mul(Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT) return em_int(a.as.i * b.as.i);
    if (is_num(a) && is_num(b)) {
        if (a.tag == V_FLOAT || b.tag == V_FLOAT)
            return em_float(as_double(a) * as_double(b));
        return em_int(as_int(a) * as_int(b));
    }
    /* "ab" * 3, [1] * 3 */
    Value s = em_none(), n = em_none();
    if ((is_str(a) || is_list(a)) && b.tag == V_INT) { s = a; n = b; }
    else if ((is_str(b) || is_list(b)) && a.tag == V_INT) { s = b; n = a; }
    if (s.tag == V_OBJ || s.tag == V_STR) {
        int64_t times = n.as.i < 0 ? 0 : n.as.i;
        if (is_str(s)) {
            size_t l = str_len(&s);
            const char *sd = str_data(&s);
            char *d = xmalloc(l * (size_t)times + 1);
            for (int64_t i = 0; i < times; i++)
                memcpy(d + (size_t)i * l, sd, l);
            d[l * (size_t)times] = '\0';
            return str_take(d, l * (size_t)times);
        }
        Obj *o = rt_obj_new(O_LIST);
        size_t l = s.as.o->as.list.len, total = l * (size_t)times;
        o->as.list.items = xmalloc(sizeof(Value) * (total ? total : 1));
        o->as.list.cap = total ? total : 1;
        for (int64_t i = 0; i < times; i++)
            memcpy(o->as.list.items + (size_t)i * l, s.as.o->as.list.items,
                   sizeof(Value) * l);
        o->as.list.len = total;
        return obj_val(o);
    }
    rt_fatal("unsupported operand types for *: %s and %s", type_name(a), type_name(b));
    return em_none();
}

Value em_div(Value a, Value b) { /* always float division, like Python 3 */
    if (is_num(a) && is_num(b)) {
        double db = as_double(b);
        if (db == 0.0) rt_fatal("division by zero");
        return em_float(as_double(a) / db);
    }
    rt_fatal("unsupported operand types for /: %s and %s", type_name(a), type_name(b));
    return em_none();
}

Value em_mod(Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT) {
        if (b.as.i == 0) rt_fatal("modulo by zero");
        int64_t r = a.as.i % b.as.i; /* Python-style: result has sign of divisor */
        if (r != 0 && ((r < 0) != (b.as.i < 0))) r += b.as.i;
        return em_int(r);
    }
    rt_fatal("unsupported operand types for %%: %s and %s", type_name(a), type_name(b));
    return em_none();
}

Value em_neg(Value a) {
    if (a.tag == V_INT) return em_int(-a.as.i);
    if (a.tag == V_FLOAT) return em_float(-a.as.f);
    if (a.tag == V_BOOL) return em_int(a.as.b ? -1 : 0);
    rt_fatal("unsupported operand type for unary -: %s", type_name(a));
    return em_none();
}

static bool value_eq(Value a, Value b) {
    if (is_num(a) && is_num(b)) return as_double(a) == as_double(b);
    if (is_str(a) && is_str(b)) {
        size_t la = str_len(&a), lb = str_len(&b);
        return la == lb && memcmp(str_data(&a), str_data(&b), la) == 0;
    }
    if (a.tag != b.tag) return false;
    if (a.tag == V_NONE) return true;
    if (a.tag != V_OBJ) return false; /* unreachable */
    if (a.as.o->tag != b.as.o->tag) return false;
    Obj *x = a.as.o, *y = b.as.o;
    switch (x->tag) {
    case O_STR:  break; /* unreachable: both-strings handled above */
    case O_FUNC: return x == y; /* functions compare by identity */
    case O_CELL: return value_eq(x->as.cell.val, y->as.cell.val);
    case O_LIST:
        if (x->as.list.len != y->as.list.len) return false;
        for (size_t i = 0; i < x->as.list.len; i++)
            if (!value_eq(x->as.list.items[i], y->as.list.items[i])) return false;
        return true;
    case O_REC:
        if (x->as.rec.len != y->as.rec.len) return false;
        for (size_t i = 0; i < x->as.rec.len; i++) {
            bool found = false;
            for (size_t j = 0; j < y->as.rec.len; j++)
                if (strcmp(x->as.rec.keys[i], y->as.rec.keys[j]) == 0) {
                    if (!value_eq(x->as.rec.vals[i], y->as.rec.vals[j])) return false;
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        return true;
    }
    return false;
}

Value em_eq(Value a, Value b) { return em_bool(value_eq(a, b)); }
Value em_ne(Value a, Value b) { return em_bool(!value_eq(a, b)); }

static int value_cmp(Value a, Value b) { /* -1 / 0 / 1; fatal on bad types */
    if (is_num(a) && is_num(b)) {
        double x = as_double(a), y = as_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (is_str(a) && is_str(b)) {
        size_t la = str_len(&a), lb = str_len(&b);
        size_t n = la < lb ? la : lb;
        int c = memcmp(str_data(&a), str_data(&b), n);
        if (c) return c < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    if (is_list(a) && is_list(b)) {
        size_t la = a.as.o->as.list.len, lb = b.as.o->as.list.len;
        size_t n = la < lb ? la : lb;
        for (size_t i = 0; i < n; i++) {
            int c = value_cmp(a.as.o->as.list.items[i], b.as.o->as.list.items[i]);
            if (c) return c;
        }
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    rt_fatal("cannot order %s and %s", type_name(a), type_name(b));
    return 0;
}

Value em_lt(Value a, Value b) { return em_bool(value_cmp(a, b) < 0); }
Value em_le(Value a, Value b) { return em_bool(value_cmp(a, b) <= 0); }
Value em_gt(Value a, Value b) { return em_bool(value_cmp(a, b) > 0); }
Value em_ge(Value a, Value b) { return em_bool(value_cmp(a, b) >= 0); }

bool em_truthy(Value v) {
    switch (v.tag) {
    case V_NONE:  return false;
    case V_BOOL:  return v.as.b;
    case V_INT:   return v.as.i != 0;
    case V_FLOAT: return v.as.f != 0.0;
    case V_STR:  return str_len(&v) != 0;
    case V_OBJ:
        switch (v.as.o->tag) {
        case O_STR:  return str_len(&v) != 0;
        case O_LIST: return v.as.o->as.list.len != 0;
        case O_REC:  return true; /* records are always truthy, like objects */
        case O_FUNC: return true; /* functions are always truthy */
        case O_CELL: return em_truthy(v.as.o->as.cell.val);
        }
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* indexing / attributes / iteration                                       */
/* ---------------------------------------------------------------------- */

static size_t norm_index(int64_t i, size_t len, const char *what) {
    if (i < 0) i += (int64_t)len;
    if (i < 0 || (size_t)i >= len)
        rt_fatal("%s index out of range (index %" PRId64 ", length %zu)",
                 what, i, len);
    return (size_t)i;
}

Value em_index(Value seq, Value idx) {
    if (idx.tag != V_INT) rt_fatal("indices must be int, not %s", type_name(idx));
    if (is_list(seq))
        return seq.as.o->as.list.items[norm_index(idx.as.i, seq.as.o->as.list.len, "list")];
    if (is_str(seq)) {
        size_t i = norm_index(idx.as.i, str_len(&seq), "string");
        return str_copy(str_data(&seq) + i, 1);
    }
    rt_fatal("%s is not indexable", type_name(seq));
    return em_none();
}

void em_setindex(Value seq, Value idx, Value v) {
    if (!is_list(seq)) rt_fatal("cannot assign into a %s by index", type_name(seq));
    if (idx.tag != V_INT) rt_fatal("indices must be int, not %s", type_name(idx));
    seq.as.o->as.list.items[norm_index(idx.as.i, seq.as.o->as.list.len, "list")] = v;
    gc_write_barrier(seq.as.o, v);
}

Value em_getattr(Value rec, const char *name) {
    if (!is_rec(rec)) rt_fatal("%s has no fields (looking for .%s)", type_name(rec), name);
    Obj *o = rec.as.o;
    for (size_t i = 0; i < o->as.rec.len; i++)
        if (strcmp(o->as.rec.keys[i], name) == 0) return o->as.rec.vals[i];
    rt_fatal("record has no field '%s'", name);
    return em_none();
}

void em_setattr(Value rec, const char *name, Value v) {
    if (!is_rec(rec)) rt_fatal("%s has no fields (assigning .%s)", type_name(rec), name);
    Obj *o = rec.as.o;
    for (size_t i = 0; i < o->as.rec.len; i++)
        if (strcmp(o->as.rec.keys[i], name) == 0) {
            o->as.rec.vals[i] = v;
            gc_write_barrier(o, v);
            return;
        }
    /* new field: keys emitted by codegen are static strings, safe to keep */
    if (o->as.rec.len == o->as.rec.cap) {
        o->as.rec.cap = o->as.rec.cap ? o->as.rec.cap * 2 : 4;
        o->as.rec.keys = xrealloc(o->as.rec.keys, sizeof(char *) * o->as.rec.cap);
        o->as.rec.vals = xrealloc(o->as.rec.vals, sizeof(Value) * o->as.rec.cap);
    }
    o->as.rec.keys[o->as.rec.len] = name;
    o->as.rec.vals[o->as.rec.len] = v;
    o->as.rec.len++;
    gc_write_barrier(o, v);
}

bool rt_iter_get(Value seq, int64_t i, Value *out) {
    if (is_list(seq)) {
        if ((size_t)i >= seq.as.o->as.list.len) return false;
        *out = seq.as.o->as.list.items[i];
        return true;
    }
    if (is_str(seq)) {
        if ((size_t)i >= str_len(&seq)) return false;
        *out = em_index(seq, em_int(i));
        return true;
    }
    rt_fatal("%s is not iterable", type_name(seq));
    return false;
}

/* ---------------------------------------------------------------------- */
/* builtins                                                                */
/* ---------------------------------------------------------------------- */

void em_print(size_t n, ...) {
    SB sb = {0};
    va_list ap;
    va_start(ap, n);
    for (size_t i = 0; i < n; i++) {
        if (i) sb_puts(&sb, " ");
        write_value(&sb, va_arg(ap, Value), false);
    }
    va_end(ap);
    sb_puts(&sb, "\n");
    fwrite(sb.buf ? sb.buf : "\n", 1, sb.len, stdout);
    free(sb.buf);
}

Value em_len(Value v) {
    if (is_str(v)) return em_int((int64_t)str_len(&v));
    if (is_list(v)) return em_int((int64_t)v.as.o->as.list.len);
    if (is_rec(v)) return em_int((int64_t)v.as.o->as.rec.len);
    rt_fatal("%s has no len()", type_name(v));
    return em_none();
}

Value em_range(Value lo, Value hi) {
    if (lo.tag != V_INT || hi.tag != V_INT)
        rt_fatal("range() arguments must be int");
    Obj *o = rt_obj_new(O_LIST);
    int64_t a = lo.as.i, b = hi.as.i;
    size_t n = b > a ? (size_t)(b - a) : 0;
    o->as.list.items = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.list.cap = n ? n : 1;
    for (size_t i = 0; i < n; i++)
        o->as.list.items[i] = em_int(a + (int64_t)i);
    o->as.list.len = n;
    return obj_val(o);
}

Value em_str(Value v) {
    SB sb = {0};
    write_value(&sb, v, false);
    Value s = str_copy(sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    return s;
}

Value em_int_of(Value v) {
    switch (v.tag) {
    case V_BOOL:  return em_int(v.as.b ? 1 : 0);
    case V_INT:   return v;
    case V_FLOAT: return em_int((int64_t)v.as.f); /* truncate toward zero */
    case V_STR:
    case V_OBJ:
        if (is_str(v)) {
            const char *s = str_data(&v);
            char *end;
            long long r = strtoll(s, &end, 10);
            while (*end == ' ') end++;
            if (end == s || *end != '\0')
                rt_fatal("invalid literal for int(): '%s'", s);
            return em_int(r);
        }
        break;
    default: break;
    }
    rt_fatal("cannot convert %s to int", type_name(v));
    return em_none();
}

Value em_gc_stats(void) {
    return em_rec_litn(5,
        "collections", em_int((int64_t)gc_collections),
        "live",        em_int((int64_t)gc_live),
        "young",       em_int((int64_t)gc_young_count),
        "old",         em_int((int64_t)gc_old_count),
        "threshold",   em_int((int64_t)gc_young_threshold));
}

/* --- file / process I/O (self-hosting escape hatches) -------------------- */

Value em_read_file(Value path) {
    if (!is_str(path)) rt_fatal("read_file() path must be str, not %s", type_name(path));
    const char *p = str_data(&path);
    FILE *f = fopen(p, "rb");
    if (!f) rt_fatal("cannot open '%s' for reading", p);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size)
        rt_fatal("cannot read '%s'", p);
    buf[size] = '\0';
    fclose(f);
    return str_take(buf, (size_t)size);
}

void em_write_file(Value path, Value content) {
    if (!is_str(path)) rt_fatal("write_file() path must be str, not %s", type_name(path));
    if (!is_str(content)) rt_fatal("write_file() content must be str, not %s", type_name(content));
    const char *p = str_data(&path);
    FILE *f = fopen(p, "wb");
    if (!f) rt_fatal("cannot open '%s' for writing", p);
    size_t n = str_len(&content);
    fwrite(str_data(&content), 1, n, f);
    fclose(f);
}

Value em_run(Value cmd) {
    if (!is_str(cmd)) rt_fatal("run() command must be str, not %s", type_name(cmd));
    return em_int(system(str_data(&cmd)));
}

/* --- first-class functions ------------------------------------------------ */

Value em_mkclosure(Value (*fn)(Value *env, Value *args), size_t arity,
                   Value *env, size_t env_count) {
    RootFrame fr;
    rt_push_frame(&fr, env, env_count); /* root env while the Obj may allocate */
    Obj *o = rt_obj_new(O_FUNC);
    o->as.func.fn = fn;
    o->as.func.arity = arity;
    if (env_count) {
        o->as.func.env = xmalloc(sizeof(Value) * env_count);
        memcpy(o->as.func.env, env, sizeof(Value) * env_count);
        o->as.func.env_count = env_count;
    } else {
        o->as.func.env = NULL;
        o->as.func.env_count = 0;
    }
    rt_pop_frame();
    return obj_val(o);
}

Value em_cell(Value v) {
    Obj *o = rt_obj_new(O_CELL);
    o->as.cell.val = v;
    return obj_val(o);
}

Value em_cell_get(Value cell) {
    if (!(cell.tag == V_OBJ && cell.as.o->tag == O_CELL))
        rt_fatal("internal: em_cell_get on a non-cell");
    return cell.as.o->as.cell.val;
}

void em_cell_set(Value cell, Value v) {
    if (!(cell.tag == V_OBJ && cell.as.o->tag == O_CELL))
        rt_fatal("internal: em_cell_set on a non-cell");
    Obj *o = cell.as.o;
    o->as.cell.val = v;
    gc_write_barrier(o, v);
}

Value em_call(Value fn, size_t argc, ...) {
    if (!(fn.tag == V_OBJ && fn.as.o->tag == O_FUNC))
        rt_fatal("attempt to call a value of type %s", type_name(fn));
    Obj *c = fn.as.o;
    if (c->as.func.arity != argc)
        rt_fatal("function expects %zu argument(s), got %zu", c->as.func.arity, argc);
    Value *args = xmalloc(sizeof(Value) * (argc ? argc : 1));
    va_list ap;
    va_start(ap, argc);
    for (size_t i = 0; i < argc; i++) args[i] = va_arg(ap, Value);
    va_end(ap);
    RootFrame fr;
    rt_push_frame(&fr, args, argc); /* root args while the callee may allocate */
    Value r = c->as.func.fn(c->as.func.env, args);
    rt_pop_frame();
    free(args);
    return r;
}
