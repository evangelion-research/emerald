/* Runtime: object constructors, small helpers, string building, and pretty
 * printing. */
#include "runtime_internal.h"

/* Copy data[0..len) into a Value: inline (V_STR) when short, heap Obj otherwise. */
Value str_copy(const char *data, size_t len) {
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
    obj_charge(o, len + 1);
    return obj_val(o);
}

Value str_take(char *data, size_t len) { /* takes ownership of data */
    if (len <= SSO_MAX) {
        Value v = str_copy(data, len);
        free(data);
        return v;
    }
    Obj *o = rt_obj_new(O_STR);
    o->as.str.data = data;
    o->as.str.len = len;
    obj_charge(o, len + 1);
    return obj_val(o);
}

Value em_str_new(const char *cstr) {
    return str_copy(cstr, strlen(cstr));
}

/* A fresh empty list with room for `n` items (len 0). Every list constructor
 * goes through here so the capacity, the byte charge, and the zero-length
 * case are decided once. */
Obj *list_new(size_t n) {
    Obj *o = rt_obj_new(O_LIST);
    o->as.list.items = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.list.cap = n ? n : 1;
    o->as.list.len = 0;
    obj_charge(o, sizeof(Value) * (n ? n : 1));
    return o;
}

Value em_list_litn(size_t n, ...) {
    Obj *o = list_new(n);
    va_list ap;
    va_start(ap, n);
    for (size_t i = 0; i < n; i++)
        o->as.list.items[i] = va_arg(ap, Value);
    va_end(ap);
    o->as.list.len = n;
    return obj_val(o);
}

Value em_tuple_litn(size_t n, ...) {
    Obj *o = rt_obj_new(O_TUPLE);
    o->as.list.items = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.list.cap = o->as.list.len = n ? n : 1;
    obj_charge(o, sizeof(Value) * (n ? n : 1));
    va_list ap; va_start(ap, n);
    for (size_t i = 0; i < n; i++) o->as.list.items[i] = va_arg(ap, Value);
    va_end(ap); o->as.list.len = n;
    return obj_val(o);
}

Value em_dict_litn(size_t n, ...) {
    Obj *o = rt_obj_new(O_DICT);
    o->as.dict.keys = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.dict.vals = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.dict.cap = n ? n : 1;
    o->as.dict.len = 0;
    obj_charge(o, sizeof(Value) * (n ? n : 1) * 2);
    Value out = obj_val(o);
    va_list ap; va_start(ap, n);
    for (size_t i = 0; i < n; i++) {
        Value key = va_arg(ap, Value), value = va_arg(ap, Value);
        em_dict_set(out, key, value);
    }
    va_end(ap);
    return out;
}

Value em_set_litn(size_t n, ...) {
    Obj *o = rt_obj_new(O_SET);
    o->as.set.items = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.set.cap = n ? n : 1;
    o->as.set.len = 0;
    obj_charge(o, sizeof(Value) * (n ? n : 1));
    Value out = obj_val(o);
    va_list ap; va_start(ap, n);
    for (size_t i = 0; i < n; i++) em_set_add(out, va_arg(ap, Value));
    va_end(ap);
    return out;
}

/* Python-style constructors. Dictionaries are deliberately string-keyed;
 * there is no hashability constraint in Emerald's type language. */
static bool is_pair(Value v) {
    return is_list(v) || is_tuple(v);
}

Value em_dict_from(Value iterable) {
    Value out = em_dict_litn(0);
    RootFrame fr;
    rt_push_frame(&fr, &out, 1);
    if (iterable.tag == V_OBJ && iterable.as.o->tag == O_DICT) {
        for (size_t i = 0; i < iterable.as.o->as.dict.len; i++)
            em_dict_set(out, iterable.as.o->as.dict.keys[i], iterable.as.o->as.dict.vals[i]);
    } else {
        int64_t i = 0; Value item;
        while (rt_iter_get(iterable, i++, &item)) {
            if (!is_pair(item) || item.as.o->as.list.len != 2)
                rt_fatal("dict() iterable elements must be 2-item lists or tuples");
            em_dict_set(out, item.as.o->as.list.items[0], item.as.o->as.list.items[1]);
        }
    }
    rt_pop_frame();
    return out;
}

Value em_set_from(Value iterable) {
    Value out = em_set_litn(0);
    RootFrame fr;
    rt_push_frame(&fr, &out, 1);
    if (iterable.tag == V_OBJ && iterable.as.o->tag == O_DICT) {
        for (size_t i = 0; i < iterable.as.o->as.dict.len; i++)
            em_set_add(out, iterable.as.o->as.dict.keys[i]);
    } else {
        int64_t i = 0; Value item;
        while (rt_iter_get(iterable, i++, &item)) em_set_add(out, item);
    }
    rt_pop_frame();
    return out;
}

Value em_rec_litn(size_t n, ...) {
    Obj *o = rt_obj_new(O_REC);
    o->as.rec.keys = xmalloc(sizeof(char *) * (n ? n : 1));
    o->as.rec.vals = xmalloc(sizeof(Value) * (n ? n : 1));
    o->as.rec.cap = n ? n : 1;
    obj_charge(o, sizeof(char *) * (n ? n : 1) + sizeof(Value) * (n ? n : 1));
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
const char *type_name(Value v) {
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
        case O_TUPLE: return "tuple";
        case O_REC:  return "record";
        case O_DICT: return "dict";
        case O_SET: return "set";
        case O_FUNC: return "function";
        case O_CELL: return "cell";
        case O_TENSOR: return "tensor";
        case O_CHAN: return "channel";
        case O_TASK: return "task";
        }
    }
    return "?";
}

bool is_str(Value v)  { return v.tag == V_STR || (v.tag == V_OBJ && v.as.o->tag == O_STR); }

bool is_list(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_LIST; }

bool is_tuple(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_TUPLE; }

bool is_set(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_SET; }

bool is_rec(Value v)  { return v.tag == V_OBJ && v.as.o->tag == O_REC; }

bool is_num(Value v)  { return v.tag == V_INT || v.tag == V_FLOAT || v.tag == V_BOOL; }

bool is_tensor(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_TENSOR; }

/* Uniform access to inline (V_STR) and heap (O_STR) strings; both are
 * NUL-terminated, so str_data can be handed to C string functions. Takes a
 * pointer so the returned bytes point into the caller's stable storage. */
const char *str_data(const Value *v) {
    return v->tag == V_STR ? v->as.s.bytes : v->as.o->as.str.data;
}

size_t str_len(const Value *v) {
    return v->tag == V_STR ? strlen(v->as.s.bytes) : v->as.o->as.str.len;
}

double as_double(Value v) {
    switch (v.tag) {
    case V_BOOL:  return v.as.b ? 1.0 : 0.0;
    case V_INT:   return (double)v.as.i;
    case V_FLOAT: return v.as.f;
    default:      return 0.0;
    }
}

int64_t as_int(Value v) { return v.tag == V_BOOL ? (v.as.b ? 1 : 0) : v.as.i; }

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

void sb_puts(SB *sb, const char *s) { sb_put(sb, s, strlen(s)); }

static void format_float(SB *sb, double f) {
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.12g", f);
    sb_puts(sb, tmp);
    if (!strpbrk(tmp, ".eEni")) sb_puts(sb, ".0"); /* 3 -> 3.0 (skip inf/nan) */
}

/* repr: quotes strings; str: raw strings. Containers always use repr inside. */
void write_value(SB *sb, Value v, bool repr) {
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
        case O_TUPLE:
            sb_puts(sb, v.as.o->tag == O_TUPLE ? "(" : "[");
            for (size_t i = 0; i < v.as.o->as.list.len; i++) {
                if (i) sb_puts(sb, ", ");
                write_value(sb, v.as.o->as.list.items[i], true);
            }
            if (v.as.o->tag == O_TUPLE && v.as.o->as.list.len == 1) sb_puts(sb, ",");
            sb_puts(sb, v.as.o->tag == O_TUPLE ? ")" : "]");
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
        case O_DICT:
            sb_puts(sb, "{");
            for (size_t i = 0; i < v.as.o->as.dict.len; i++) {
                if (i) sb_puts(sb, ", ");
                write_value(sb, v.as.o->as.dict.keys[i], true);
                sb_puts(sb, ": ");
                write_value(sb, v.as.o->as.dict.vals[i], true);
            }
            sb_puts(sb, "}");
            break;
        case O_SET:
            sb_puts(sb, "{");
            for (size_t i = 0; i < v.as.o->as.set.len; i++) {
                if (i) sb_puts(sb, ", ");
                write_value(sb, v.as.o->as.set.items[i], true);
            }
            sb_puts(sb, "}");
            break;
        case O_FUNC:
            sb_puts(sb, "<function>");
            break;
        case O_CELL: /* cells are internal; print their contents */
            write_value(sb, v.as.o->as.cell.val, repr);
            break;
        case O_TENSOR: {
            Obj *t = v.as.o;
            sb_puts(sb, "Tensor[");
            sb_puts(sb, t->as.tensor.dt == DT_F64 ? "f64" : "f32");
            sb_puts(sb, ", [");
            for (uint8_t d = 0; d < t->as.tensor.ndim; d++) {
                if (d) sb_puts(sb, ", ");
                char buf[24];
                snprintf(buf, sizeof buf, "%" PRId64, t->as.tensor.dims[d]);
                sb_puts(sb, buf);
            }
            sb_puts(sb, "]]");
            break;
        }
        case O_CHAN: sb_puts(sb, "<channel>"); break;
        case O_TASK: sb_puts(sb, "<task>"); break;
        }
        break;
    }
}

static void write_pretty(SB *sb, Value v, int depth, int col);

static void write_pretty(SB *sb, Value v, int depth, int col) {
    if (!is_list(v) && !is_rec(v)) { /* atomic: never breaks */
        write_value(sb, v, true);
        return;
    }
    /* does the whole thing fit on the current line? */
    SB tmp = {0};
    write_value(&tmp, v, true);
    bool fits = tmp.len + (size_t)col <= PP_WIDTH;
    free(tmp.buf);
    if (fits) {
        write_value(sb, v, true);
        return;
    }
    if (is_list(v)) {
        Obj *o = v.as.o;
        sb_puts(sb, "[");
        for (size_t i = 0; i < o->as.list.len; i++) {
            if (i) sb_puts(sb, ",");
            sb_puts(sb, "\n");
            int cc = (depth + 1) * PP_INDENT;
            for (int d = 0; d < cc; d++) sb_puts(sb, " ");
            write_pretty(sb, o->as.list.items[i], depth + 1, cc);
        }
        if (o->as.list.len) {
            sb_puts(sb, "\n");
            for (int d = 0; d < depth * PP_INDENT; d++) sb_puts(sb, " ");
        }
        sb_puts(sb, "]");
        return;
    }
    /* record */
    Obj *o = v.as.o;
    sb_puts(sb, "{");
    for (size_t i = 0; i < o->as.rec.len; i++) {
        if (i) sb_puts(sb, ",");
        sb_puts(sb, "\n");
        int cc = (depth + 1) * PP_INDENT;
        for (int d = 0; d < cc; d++) sb_puts(sb, " ");
        sb_puts(sb, o->as.rec.keys[i]);
        sb_puts(sb, ": ");
        write_pretty(sb, o->as.rec.vals[i], depth + 1,
                     cc + (int)strlen(o->as.rec.keys[i]) + 2);
    }
    if (o->as.rec.len) {
        sb_puts(sb, "\n");
        for (int d = 0; d < depth * PP_INDENT; d++) sb_puts(sb, " ");
    }
    sb_puts(sb, "}");
}

void em_pprint(Value v) {
    SB sb = {0};
    write_pretty(&sb, v, 0, 0);
    sb_puts(&sb, "\n");
    fwrite(sb.buf ? sb.buf : "\n", 1, sb.len, stdout);
    free(sb.buf);
}

void em_pprint_err(Value v) {
    SB sb = {0};
    write_pretty(&sb, v, 0, 0);
    sb_puts(&sb, "\n");
    fwrite(sb.buf ? sb.buf : "\n", 1, sb.len, stderr);
    free(sb.buf);
}

Value em_pp_format(Value v) {
    SB sb = {0};
    write_pretty(&sb, v, 0, 0);
    Value s = str_copy(sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    return s;
}

/* ---------------------------------------------------------------------- */
/* operators                                                               */
/* ---------------------------------------------------------------------- */
Value em_add(Value a, Value b) {
    if (is_tensor(a) || is_tensor(b)) return em_tensor_add(a, b);
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
        /* a and b stay rooted by the caller */
        size_t n = a.as.o->as.list.len + b.as.o->as.list.len;
        Obj *o = list_new(n);
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
