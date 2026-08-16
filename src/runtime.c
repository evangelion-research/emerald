/* Emerald runtime implementation. See runtime.h for the model. */
#include "runtime.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RootFrame *rt_roots = NULL;

/* ---------------------------------------------------------------------- */
/* GC                                                                      */
/* ---------------------------------------------------------------------- */

static Obj *gc_all = NULL;     /* every live-or-not object */
static size_t gc_live = 0;     /* objects surviving the last sweep */
static size_t gc_count = 0;    /* objects currently allocated */
static size_t gc_threshold = 256;

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

static void gc_mark_value(Value v);

static void gc_mark_obj(Obj *o) {
    if (o == NULL || o->mark) return;
    o->mark = true;
    switch (o->tag) {
    case O_STR:
        break;
    case O_LIST:
        for (size_t i = 0; i < o->as.list.len; i++)
            gc_mark_value(o->as.list.items[i]);
        break;
    case O_REC:
        for (size_t i = 0; i < o->as.rec.len; i++)
            gc_mark_value(o->as.rec.vals[i]);
        break;
    }
}

static void gc_mark_value(Value v) {
    if (v.tag == V_OBJ) gc_mark_obj(v.as.o);
}

static void gc_free_obj(Obj *o) {
    switch (o->tag) {
    case O_STR:  free(o->as.str.data); break;
    case O_LIST: free(o->as.list.items); break;
    case O_REC:  free(o->as.rec.keys); free(o->as.rec.vals); break;
    }
    free(o);
}

void rt_gc_collect(void) {
    for (RootFrame *f = rt_roots; f; f = f->prev)
        for (size_t i = 0; i < f->count; i++)
            gc_mark_value(f->slots[i]);

    Obj **link = &gc_all;
    while (*link) {
        Obj *o = *link;
        if (o->mark) {
            o->mark = false;
            link = &o->gc_next;
        } else {
            *link = o->gc_next;
            gc_free_obj(o);
            gc_count--;
        }
    }
    gc_live = gc_count;
    gc_threshold = gc_live * 2 < 256 ? 256 : gc_live * 2;
}

static Obj *rt_obj_new(OTag tag) {
    if (gc_count >= gc_threshold) rt_gc_collect();
    Obj *o = xmalloc(sizeof(Obj));
    memset(o, 0, sizeof(Obj));
    o->tag = tag;
    o->gc_next = gc_all;
    gc_all = o;
    gc_count++;
    return o;
}

/* ---------------------------------------------------------------------- */
/* constructors                                                            */
/* ---------------------------------------------------------------------- */

static Value obj_val(Obj *o) { Value v; v.tag = V_OBJ; v.as.o = o; return v; }

static Value str_take(char *data, size_t len) { /* takes ownership of data */
    Obj *o = rt_obj_new(O_STR);
    o->as.str.data = data;
    o->as.str.len = len;
    return obj_val(o);
}

Value em_str_new(const char *cstr) {
    size_t len = strlen(cstr);
    char *d = xmalloc(len + 1);
    memcpy(d, cstr, len + 1);
    return str_take(d, len);
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
    case V_OBJ:
        switch (v.as.o->tag) {
        case O_STR:  return "str";
        case O_LIST: return "list";
        case O_REC:  return "record";
        }
    }
    return "?";
}

static bool is_str(Value v)  { return v.tag == V_OBJ && v.as.o->tag == O_STR; }
static bool is_list(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_LIST; }
static bool is_rec(Value v)  { return v.tag == V_OBJ && v.as.o->tag == O_REC; }
static bool is_num(Value v)  { return v.tag == V_INT || v.tag == V_FLOAT || v.tag == V_BOOL; }

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
    case V_OBJ:
        switch (v.as.o->tag) {
        case O_STR:
            if (repr) {
                sb_puts(sb, "'");
                sb_put(sb, v.as.o->as.str.data, v.as.o->as.str.len);
                sb_puts(sb, "'");
            } else {
                sb_put(sb, v.as.o->as.str.data, v.as.o->as.str.len);
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
        size_t la = a.as.o->as.str.len, lb = b.as.o->as.str.len;
        char *d = xmalloc(la + lb + 1);
        memcpy(d, a.as.o->as.str.data, la);
        memcpy(d + la, b.as.o->as.str.data, lb);
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
    if (s.tag == V_OBJ) {
        int64_t times = n.as.i < 0 ? 0 : n.as.i;
        if (s.as.o->tag == O_STR) {
            size_t l = s.as.o->as.str.len;
            char *d = xmalloc(l * (size_t)times + 1);
            for (int64_t i = 0; i < times; i++)
                memcpy(d + (size_t)i * l, s.as.o->as.str.data, l);
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
    if (a.tag != b.tag) return false;
    if (a.tag == V_NONE) return true;
    if (a.tag != V_OBJ) return false; /* unreachable */
    if (a.as.o->tag != b.as.o->tag) return false;
    Obj *x = a.as.o, *y = b.as.o;
    switch (x->tag) {
    case O_STR:
        return x->as.str.len == y->as.str.len &&
               memcmp(x->as.str.data, y->as.str.data, x->as.str.len) == 0;
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
        size_t la = a.as.o->as.str.len, lb = b.as.o->as.str.len;
        size_t n = la < lb ? la : lb;
        int c = memcmp(a.as.o->as.str.data, b.as.o->as.str.data, n);
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
    case V_OBJ:
        switch (v.as.o->tag) {
        case O_STR:  return v.as.o->as.str.len != 0;
        case O_LIST: return v.as.o->as.list.len != 0;
        case O_REC:  return true; /* records are always truthy, like objects */
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
        size_t i = norm_index(idx.as.i, seq.as.o->as.str.len, "string");
        char *d = xmalloc(2);
        d[0] = seq.as.o->as.str.data[i];
        d[1] = '\0';
        return str_take(d, 1);
    }
    rt_fatal("%s is not indexable", type_name(seq));
    return em_none();
}

void em_setindex(Value seq, Value idx, Value v) {
    if (!is_list(seq)) rt_fatal("cannot assign into a %s by index", type_name(seq));
    if (idx.tag != V_INT) rt_fatal("indices must be int, not %s", type_name(idx));
    seq.as.o->as.list.items[norm_index(idx.as.i, seq.as.o->as.list.len, "list")] = v;
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
        if (strcmp(o->as.rec.keys[i], name) == 0) { o->as.rec.vals[i] = v; return; }
    /* new field: keys emitted by codegen are static strings, safe to keep */
    if (o->as.rec.len == o->as.rec.cap) {
        o->as.rec.cap = o->as.rec.cap ? o->as.rec.cap * 2 : 4;
        o->as.rec.keys = xrealloc(o->as.rec.keys, sizeof(char *) * o->as.rec.cap);
        o->as.rec.vals = xrealloc(o->as.rec.vals, sizeof(Value) * o->as.rec.cap);
    }
    o->as.rec.keys[o->as.rec.len] = name;
    o->as.rec.vals[o->as.rec.len] = v;
    o->as.rec.len++;
}

bool rt_iter_get(Value seq, int64_t i, Value *out) {
    if (is_list(seq)) {
        if ((size_t)i >= seq.as.o->as.list.len) return false;
        *out = seq.as.o->as.list.items[i];
        return true;
    }
    if (is_str(seq)) {
        if ((size_t)i >= seq.as.o->as.str.len) return false;
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
    if (is_str(v)) return em_int((int64_t)v.as.o->as.str.len);
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
    Value s = str_take(sb.buf ? sb.buf : xmalloc(1), sb.len);
    if (!sb.buf) s.as.o->as.str.data[0] = '\0';
    return s;
}

Value em_int_of(Value v) {
    switch (v.tag) {
    case V_BOOL:  return em_int(v.as.b ? 1 : 0);
    case V_INT:   return v;
    case V_FLOAT: return em_int((int64_t)v.as.f); /* truncate toward zero */
    case V_OBJ:
        if (v.as.o->tag == O_STR) {
            char *end;
            long long r = strtoll(v.as.o->as.str.data, &end, 10);
            while (*end == ' ') end++;
            if (end == v.as.o->as.str.data || *end != '\0')
                rt_fatal("invalid literal for int(): '%s'", v.as.o->as.str.data);
            return em_int(r);
        }
        break;
    default: break;
    }
    rt_fatal("cannot convert %s to int", type_name(v));
    return em_none();
}
