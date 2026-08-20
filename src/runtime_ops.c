/* Runtime: operators, indexing, attribute access, and iteration. */
#include "runtime_internal.h"

NUM_BINOP(em_sub, -, "-")

Value em_mul(Value a, Value b) {
    if (is_tensor(a) || is_tensor(b)) return em_tensor_mul(a, b);
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
        size_t l = s.as.o->as.list.len, total = l * (size_t)times;
        Obj *o = list_new(total);
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
    if (is_tensor(a) || is_tensor(b)) return em_tensor_div(a, b);
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

Value em_floordiv(Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT) {
        if (b.as.i == 0) rt_fatal("division by zero");
        /* Python `//`: round toward -inf, consistent with `%` above */
        int64_t r = a.as.i % b.as.i;
        if (r != 0 && ((r < 0) != (b.as.i < 0))) r += b.as.i;
        return em_int((a.as.i - r) / b.as.i);
    }
    if (is_num(a) && is_num(b)) {
        double db = as_double(b);
        if (db == 0.0) rt_fatal("division by zero");
        return em_float(floor(as_double(a) / db));
    }
    rt_fatal("unsupported operand types for //: %s and %s", type_name(a), type_name(b));
    return em_none();
}

Value em_pow(Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT && b.as.i >= 0) {
        int64_t r = 1, base = a.as.i, e = b.as.i;
        while (e > 0) { /* exponentiation by squaring */
            if (e & 1) r *= base;
            base *= base;
            e >>= 1;
        }
        return em_int(r);
    }
    if (is_num(a) && is_num(b))
        return em_float(pow(as_double(a), as_double(b)));
    rt_fatal("unsupported operand types for **: %s and %s", type_name(a), type_name(b));
    return em_none();
}

Value em_neg(Value a) {
    if (is_tensor(a)) return em_tensor_mul(a, em_float(-1.0));
    if (a.tag == V_INT) return em_int(-a.as.i);
    if (a.tag == V_FLOAT) return em_float(-a.as.f);
    if (a.tag == V_BOOL) return em_int(a.as.b ? -1 : 0);
    rt_fatal("unsupported operand type for unary -: %s", type_name(a));
    return em_none();
}

bool value_eq(Value a, Value b) {
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
    case O_TENSOR: return x == y; /* tensors compare by identity */
    case O_DICT:
        if (x->as.dict.len != y->as.dict.len) return false;
        for (size_t i = 0; i < x->as.dict.len; i++) {
            bool found = false;
            for (size_t j = 0; j < y->as.dict.len; j++)
                if (value_eq(x->as.dict.keys[i], y->as.dict.keys[j]) &&
                    value_eq(x->as.dict.vals[i], y->as.dict.vals[j])) { found = true; break; }
            if (!found) return false;
        }
        return true;
    case O_SET:
        if (x->as.set.len != y->as.set.len) return false;
        for (size_t i = 0; i < x->as.set.len; i++) {
            bool found = false;
            for (size_t j = 0; j < y->as.set.len; j++)
                if (value_eq(x->as.set.items[i], y->as.set.items[j])) { found = true; break; }
            if (!found) return false;
        }
        return true;
    case O_CHAN: case O_TASK: return x == y; /* handles compare by identity */
    case O_CELL: return value_eq(x->as.cell.val, y->as.cell.val);
    case O_LIST:
    case O_TUPLE:
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

static Value bitop(Value a, Value b, char op) {
    if (is_set(a) && is_set(b)) return set_binary(a, b, op);
    if (a.tag != V_INT || b.tag != V_INT)
        rt_fatal("bitwise operators require int operands, got %s and %s",
                 type_name(a), type_name(b));
    switch (op) {
    case '|': return em_int(a.as.i | b.as.i);
    case '^': return em_int(a.as.i ^ b.as.i);
    case '&': return em_int(a.as.i & b.as.i);
    case '<':
        if (b.as.i < 0 || b.as.i >= 64) rt_fatal("invalid shift count");
        return em_int(a.as.i << b.as.i);
    case '>':
        if (b.as.i < 0 || b.as.i >= 64) rt_fatal("invalid shift count");
        return em_int(a.as.i >> b.as.i);
    }
    return em_none();
}

Value em_bitor(Value a, Value b) { return bitop(a, b, '|'); }

Value em_bitxor(Value a, Value b) { return bitop(a, b, '^'); }

Value em_bitand(Value a, Value b) { return bitop(a, b, '&'); }

Value em_lshift(Value a, Value b) { return bitop(a, b, '<'); }

Value em_rshift(Value a, Value b) { return bitop(a, b, '>'); }

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
        case O_LIST: case O_TUPLE: return v.as.o->as.list.len != 0;
        case O_REC:  return true; /* records are always truthy, like objects */
        case O_DICT: return v.as.o->as.dict.len != 0;
        case O_SET: return v.as.o->as.set.len != 0;
        case O_FUNC: return true; /* functions are always truthy */
        case O_CELL: return em_truthy(v.as.o->as.cell.val);
        case O_TENSOR: return true; /* tensors are always truthy */
        case O_CHAN: case O_TASK: return true; /* handles are always truthy */
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
    if (seq.tag == V_OBJ && seq.as.o->tag == O_DICT) {
        if (!is_str(idx)) rt_fatal("dict keys must be str, got %s", type_name(idx));
        for (size_t i = 0; i < seq.as.o->as.dict.len; i++)
            if (value_eq(seq.as.o->as.dict.keys[i], idx)) return seq.as.o->as.dict.vals[i];
        rt_fatal("dict key not found");
    }
    if (idx.tag != V_INT) rt_fatal("indices must be int, not %s", type_name(idx));
    if (is_list(seq) || (seq.tag == V_OBJ && seq.as.o->tag == O_TUPLE))
        return seq.as.o->as.list.items[norm_index(idx.as.i, seq.as.o->as.list.len,
                                                   seq.as.o->tag == O_TUPLE ? "tuple" : "list")];
    if (is_str(seq)) {
        size_t i = norm_index(idx.as.i, str_len(&seq), "string");
        return str_copy(str_data(&seq) + i, 1);
    }
    if (is_tensor(seq)) {
        Obj *t = seq.as.o;
        if (t->as.tensor.ndim == 0)
            rt_fatal("cannot index a 0-dimensional tensor");
        int64_t n = t->as.tensor.dims[0];
        int64_t i = idx.as.i < 0 ? idx.as.i + n : idx.as.i;
        if (i < 0 || i >= n)
            rt_fatal("tensor index out of range (index %" PRId64 ", length %" PRId64 ")",
                     idx.as.i, n);
        /* integer indexing drops the indexed axis, like numpy */
        return tensor_view(t, t->as.tensor.dt, (uint8_t)(t->as.tensor.ndim - 1),
                           t->as.tensor.dims + 1, t->as.tensor.strides + 1,
                           (size_t)(i * t->as.tensor.strides[0]));
    }
    rt_fatal("%s is not indexable", type_name(seq));
    return em_none();
}

void em_setindex(Value seq, Value idx, Value v) {
    if (seq.tag == V_OBJ && seq.as.o->tag == O_DICT) {
        em_dict_set(seq, idx, v);
        return;
    }
    if (!is_list(seq)) rt_fatal("cannot assign into a %s by index", type_name(seq));
    if (idx.tag != V_INT) rt_fatal("indices must be int, not %s", type_name(idx));
    seq.as.o->as.list.items[norm_index(idx.as.i, seq.as.o->as.list.len, "list")] = v;
    gc_write_barrier(seq.as.o, v);
}

bool em_is_record(Value v) { return is_rec(v); }

bool em_rec_has(Value rec, const char *name) {
    if (!is_rec(rec)) return false;
    Obj *o = rec.as.o;
    for (size_t i = 0; i < o->as.rec.len; i++)
        if (strcmp(o->as.rec.keys[i], name) == 0) return true;
    return false;
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
        size_t oldcap = o->as.rec.cap;
        o->as.rec.cap = o->as.rec.cap ? o->as.rec.cap * 2 : 4;
        o->as.rec.keys = xrealloc(o->as.rec.keys, sizeof(char *) * o->as.rec.cap);
        o->as.rec.vals = xrealloc(o->as.rec.vals, sizeof(Value) * o->as.rec.cap);
        obj_charge(o, (o->as.rec.cap - oldcap) *
                          (sizeof(char *) + sizeof(Value)));
    }
    o->as.rec.keys[o->as.rec.len] = name;
    o->as.rec.vals[o->as.rec.len] = v;
    o->as.rec.len++;
    gc_write_barrier(o, v);
}

bool rt_iter_get(Value seq, int64_t i, Value *out) {
    if (is_list(seq) || (seq.tag == V_OBJ && seq.as.o->tag == O_TUPLE)) {
        if ((size_t)i >= seq.as.o->as.list.len) return false;
        *out = seq.as.o->as.list.items[i];
        return true;
    }
    if (seq.tag == V_OBJ && seq.as.o->tag == O_DICT) {
        if ((size_t)i >= seq.as.o->as.dict.len) return false;
        *out = seq.as.o->as.dict.keys[i]; return true;
    }
    if (seq.tag == V_OBJ && seq.as.o->tag == O_SET) {
        if ((size_t)i >= seq.as.o->as.set.len) return false;
        *out = seq.as.o->as.set.items[i]; return true;
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
