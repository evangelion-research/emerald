/* Runtime: the builtin library -- math, console and file I/O, list growth,
 * slicing, characters, and first-class functions. */
#include "runtime_internal.h"

Value em_len(Value v) {
    if (is_str(v)) return em_int((int64_t)str_len(&v));
    if (is_list(v) || (v.tag == V_OBJ && v.as.o->tag == O_TUPLE))
        return em_int((int64_t)v.as.o->as.list.len);
    if (v.tag == V_OBJ && v.as.o->tag == O_DICT)
        return em_int((int64_t)v.as.o->as.dict.len);
    if (v.tag == V_OBJ && v.as.o->tag == O_SET)
        return em_int((int64_t)v.as.o->as.set.len);
    if (is_rec(v)) return em_int((int64_t)v.as.o->as.rec.len);
    if (is_tensor(v)) {
        if (v.as.o->as.tensor.ndim == 0) return em_int(0);
        return em_int(v.as.o->as.tensor.dims[0]);
    }
    rt_fatal("%s has no len()", type_name(v));
    return em_none();
}

Value em_range(Value lo, Value hi) {
    if (lo.tag != V_INT || hi.tag != V_INT)
        rt_fatal("range() arguments must be int");
    int64_t a = lo.as.i, b = hi.as.i;
    size_t n = b > a ? (size_t)(b - a) : 0;
    Obj *o = list_new(n);
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

/* --- math builtins ------------------------------------------------------- */
Value em_sqrt(Value v) {
    if (!is_num(v)) rt_fatal("sqrt() argument must be a number, got %s",
                             type_name(v));
    double x = as_double(v);
    if (x < 0) rt_fatal("sqrt() of a negative number");
    return em_float(sqrt(x));
}

Value em_tan(Value v) {
    if (!is_num(v)) rt_fatal("tan() argument must be a number, got %s",
                             type_name(v));
    return em_float(tan(as_double(v)));
}

Value em_rand(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return em_float((double)((x * 2685821657736338717ULL) >> 11) /
                    (double)(1ULL << 53));
}

/* seed(n): make the PRNG reproducible. Zero is not a usable xorshift state,
 * so it maps to the default seed rather than wedging the generator. */
void em_seed(Value n) {
    if (n.tag != V_INT)
        rt_fatal("seed_rand() argument must be int, not %s", type_name(n));
    uint64_t s = (uint64_t)n.as.i;
    rng_state = s ? s : 88172645463325252ULL;
}

/* now(): monotonic seconds. The epoch is unspecified; only differences mean
 * anything, which is all a stopwatch or a benchmark harness needs. */
Value em_now(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return em_float((double)ts.tv_sec + (double)ts.tv_nsec * 1e-9);
#endif
    return em_float((double)clock() / (double)CLOCKS_PER_SEC);
}

/* read_line(): one line from stdin without its newline, or None at EOF. The
 * line is read into a growing buffer because stdin has no seekable length. */
Value em_read_line(void) {
    size_t cap = 128, len = 0;
    char *buf = xmalloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) {
        free(buf);
        return em_none();
    }
    /* A trailing \r is dropped so a CRLF file reads the same as an LF one,
     * matching strings.split_lines. */
    if (len > 0 && buf[len - 1] == '\r') len--;
    buf[len] = '\0';
    return str_take(buf, len);
}

/* --- console I/O --------------------------------------------------------- */
/* write() / ewrite(): one value, no separator and no newline. print() is the
 * line-oriented spelling; these are the ones a prompt or a progress dot needs.
 * stdout is line-buffered on a terminal, so a prompt without a newline only
 * appears once it is flushed — write() therefore flushes what it wrote. */
static void write_stream(FILE *f, Value v) {
    SB sb = {0};
    write_value(&sb, v, false);
    if (sb.len) fwrite(sb.buf, 1, sb.len, f);
    fflush(f);
    free(sb.buf);
}

void em_write_out(Value v) { write_stream(stdout, v); }

void em_write_err(Value v) { write_stream(stderr, v); }

void em_flush(void) {
    fflush(stdout);
    fflush(stderr);
}

/* input(prompt): the prompt, then a line, or None at EOF. */
Value em_input(Value prompt) {
    write_stream(stdout, prompt);
    return em_read_line();
}

/* read_all(): every remaining byte of stdin as one string (empty at EOF). */
Value em_read_all(void) {
    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    buf[len] = '\0';
    return str_take(buf, len);
}

Value em_gc_collect(void) {
    rt_gc_collect();
    return em_none();
}

Value em_gc_stats(void) {
    return em_rec_litn(7,
        "collections", em_int((int64_t)gc_collections),
        "live",        em_int((int64_t)gc_live),
        "young",       em_int((int64_t)gc_young_count),
        "old",         em_int((int64_t)gc_old_count),
        "threshold",   em_int((int64_t)gc_young_threshold),
        "bytes_young", em_int((int64_t)gc_young_bytes),
        "bytes_old",   em_int((int64_t)gc_old_bytes));
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

void em_append_file(Value path, Value content) {
    if (!is_str(path)) rt_fatal("append_file() path must be str, not %s", type_name(path));
    if (!is_str(content)) rt_fatal("append_file() content must be str, not %s", type_name(content));
    const char *p = str_data(&path);
    FILE *f = fopen(p, "ab");
    if (!f) rt_fatal("cannot open '%s' for appending", p);
    size_t n = str_len(&content);
    fwrite(str_data(&content), 1, n, f);
    fclose(f);
}

Value em_run(Value cmd) {
    if (!is_str(cmd)) rt_fatal("run() command must be str, not %s", type_name(cmd));
    return em_int(system(str_data(&cmd)));
}

/* read_file() aborts on a missing file, which is right for a script and wrong
 * for a library: `io.read` needs the failure as a value. Same reader, None
 * instead of a fatal. */
Value em_read_file_opt(Value path) {
    if (!is_str(path))
        rt_fatal("read_file_opt() path must be str, not %s", type_name(path));
    FILE *f = fopen(str_data(&path), "rb");
    if (!f) return em_none();
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return em_none(); }
    char *buf = xmalloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return em_none();
    }
    buf[size] = '\0';
    fclose(f);
    return str_take(buf, (size_t)size);
}

Value em_file_exists(Value path) {
    if (!is_str(path))
        rt_fatal("file_exists() path must be str, not %s", type_name(path));
    FILE *f = fopen(str_data(&path), "rb");
    if (!f) return em_bool(false);
    fclose(f);
    return em_bool(true);
}

/* --- growth, slicing, characters, process (the stdlib's foundation) ------- */
/* The one operation no amount of Emerald can express: amortized in-place list
 * growth. Without it every list built in a loop is O(n^2). */
void em_append(Value xs, Value v) {
    if (!is_list(xs))
        rt_fatal("append() expects a list, got %s", type_name(xs));
    Obj *o = xs.as.o;
    if (o->as.list.len == o->as.list.cap) {
        size_t oldcap = o->as.list.cap;
        o->as.list.cap = o->as.list.cap ? o->as.list.cap * 2 : 4;
        o->as.list.items =
            xrealloc(o->as.list.items, sizeof(Value) * o->as.list.cap);
        obj_charge(o, (o->as.list.cap - oldcap) * sizeof(Value));
    }
    o->as.list.items[o->as.list.len++] = v;
    gc_write_barrier(o, v);
}

void em_dict_set(Value dict, Value key, Value value) {
    if (!(dict.tag == V_OBJ && dict.as.o->tag == O_DICT))
        rt_fatal("dict assignment expects a dict");
    if (!is_str(key))
        rt_fatal("dict keys must be str, got %s", type_name(key));
    Obj *o = dict.as.o;
    for (size_t i = 0; i < o->as.dict.len; i++)
        if (value_eq(o->as.dict.keys[i], key)) {
            o->as.dict.vals[i] = value; gc_write_barrier(o, value); return;
        }
    if (o->as.dict.len == o->as.dict.cap) {
        size_t old = o->as.dict.cap;
        o->as.dict.cap = old ? old * 2 : 4;
        o->as.dict.keys = xrealloc(o->as.dict.keys, sizeof(Value) * o->as.dict.cap);
        o->as.dict.vals = xrealloc(o->as.dict.vals, sizeof(Value) * o->as.dict.cap);
        obj_charge(o, (o->as.dict.cap - old) * sizeof(Value) * 2);
    }
    o->as.dict.keys[o->as.dict.len] = key;
    o->as.dict.vals[o->as.dict.len++] = value;
    gc_write_barrier(o, key); gc_write_barrier(o, value);
}

void em_set_add(Value set, Value value) {
    if (!(set.tag == V_OBJ && set.as.o->tag == O_SET))
        rt_fatal("set insertion expects a set");
    Obj *o = set.as.o;
    for (size_t i = 0; i < o->as.set.len; i++)
        if (value_eq(o->as.set.items[i], value)) return;
    if (o->as.set.len == o->as.set.cap) {
        size_t old = o->as.set.cap;
        o->as.set.cap = old ? old * 2 : 4;
        o->as.set.items = xrealloc(o->as.set.items, sizeof(Value) * o->as.set.cap);
        obj_charge(o, (o->as.set.cap - old) * sizeof(Value));
    }
    o->as.set.items[o->as.set.len++] = value;
    gc_write_barrier(o, value);
}

Value set_binary(Value a, Value b, char op) {
    if (!is_set(a) || !is_set(b))
        rt_fatal("set operation requires two sets, got %s and %s",
                 type_name(a), type_name(b));
    Value out = em_set_litn(0);
    const Obj *x = a.as.o, *y = b.as.o;
    if (op == '|' || op == '^' || op == '-')
        for (size_t i = 0; i < x->as.set.len; i++) {
            bool in = false;
            for (size_t j = 0; j < y->as.set.len; j++)
                if (value_eq(x->as.set.items[i], y->as.set.items[j])) { in = true; break; }
            if (op == '|' || (op == '^' && !in) || (op == '-' && !in))
                em_set_add(out, x->as.set.items[i]);
        }
    if (op == '|' || op == '^')
        for (size_t i = 0; i < y->as.set.len; i++) {
            bool in = false;
            for (size_t j = 0; j < x->as.set.len; j++)
                if (value_eq(y->as.set.items[i], x->as.set.items[j])) { in = true; break; }
            if (op == '|' || (op == '^' && !in)) em_set_add(out, y->as.set.items[i]);
        }
    if (op == '&')
        for (size_t i = 0; i < x->as.set.len; i++)
            for (size_t j = 0; j < y->as.set.len; j++)
                if (value_eq(x->as.set.items[i], y->as.set.items[j])) {
                    em_set_add(out, x->as.set.items[i]); break;
                }
    return out;
}

Value em_contains(Value needle, Value haystack) {
    if (haystack.tag == V_OBJ && haystack.as.o->tag == O_DICT) {
        if (!is_str(needle)) rt_fatal("dict keys must be str, got %s", type_name(needle));
        for (size_t i = 0; i < haystack.as.o->as.dict.len; i++)
            if (value_eq(needle, haystack.as.o->as.dict.keys[i])) return em_bool(true);
        return em_bool(false);
    }
    if (is_str(haystack) && is_str(needle))
        return em_bool(strstr(str_data(&haystack), str_data(&needle)) != NULL);
    if (is_list(haystack) || is_tuple(haystack) || is_set(haystack)) {
        size_t n = is_set(haystack) ? haystack.as.o->as.set.len : haystack.as.o->as.list.len;
        for (size_t i = 0; i < n; i++) {
            Value item = is_set(haystack) ? haystack.as.o->as.set.items[i]
                                           : haystack.as.o->as.list.items[i];
            if (value_eq(needle, item)) return em_bool(true);
        }
        return em_bool(false);
    }
    rt_fatal("%s is not a container", type_name(haystack));
    return em_none();
}

/* Python's s[lo:hi] as a function: clamped, never an error, negatives count
 * from the end. Also slices lists, because the alternative is a second name. */
static void slice_bounds(int64_t lo, int64_t hi, size_t n, size_t *out_lo,
                         size_t *out_hi) {
    int64_t len = (int64_t)n;
    if (lo < 0) lo += len;
    if (hi < 0) hi += len;
    if (lo < 0) lo = 0;
    if (hi > len) hi = len;
    if (lo > len) lo = len;
    if (hi < lo) hi = lo;
    *out_lo = (size_t)lo;
    *out_hi = (size_t)hi;
}

Value em_slice(Value seq, Value lo, Value hi) {
    if (lo.tag != V_INT || hi.tag != V_INT)
        rt_fatal("slice() bounds must be int, not %s/%s", type_name(lo),
                 type_name(hi));
    if (is_str(seq)) {
        size_t a, b;
        slice_bounds(lo.as.i, hi.as.i, str_len(&seq), &a, &b);
        return str_copy(str_data(&seq) + a, b - a);
    }
    if (is_list(seq)) {
        size_t a, b;
        Obj *src = seq.as.o;
        slice_bounds(lo.as.i, hi.as.i, src->as.list.len, &a, &b);
        size_t n = b - a;
        Obj *o = list_new(n);
        /* src cannot move: rt_obj_new may collect, but seq is rooted by the
         * caller's frame and the copy happens after the allocation. */
        memcpy(o->as.list.items, seq.as.o->as.list.items + a, sizeof(Value) * n);
        o->as.list.len = n;
        return obj_val(o);
    }
    rt_fatal("cannot slice a %s", type_name(seq));
    return em_none();
}

Value em_slice_ex(Value seq, Value lo, Value hi, Value step) {
    int64_t st = step.tag == V_NONE ? 1 : step.as.i;
    if (step.tag != V_NONE && step.tag != V_INT) rt_fatal("slice step must be int");
    if (st == 0) rt_fatal("slice step cannot be zero");
    size_t n = is_str(seq) ? str_len(&seq) :
               (is_list(seq) || (seq.tag == V_OBJ && seq.as.o->tag == O_TUPLE))
                   ? seq.as.o->as.list.len : 0;
    if (!is_str(seq) && !is_list(seq) && !(seq.tag == V_OBJ && seq.as.o->tag == O_TUPLE))
        rt_fatal("cannot slice a %s", type_name(seq));
    int64_t a = lo.tag == V_NONE ? (st > 0 ? 0 : (int64_t)n - 1) : lo.as.i;
    int64_t b = hi.tag == V_NONE ? (st > 0 ? (int64_t)n : -1) : hi.as.i;
    if (a < 0) a += (int64_t)n; if (b < 0 && hi.tag != V_NONE) b += (int64_t)n;
    if (st > 0) {
        if (a < 0) a = 0; if (a > (int64_t)n) a = (int64_t)n;
        if (b < 0) b = 0; if (b > (int64_t)n) b = (int64_t)n;
    } else {
        if (a < -1) a = -1; if (a >= (int64_t)n) a = (int64_t)n - 1;
        if (b < -1) b = -1; if (b >= (int64_t)n) b = (int64_t)n - 1;
    }
    size_t count = 0;
    for (int64_t i = a; st > 0 ? i < b : i > b; i += st) count++;
    if (is_str(seq)) {
        char *buf = xmalloc(count + 1); size_t k = 0;
        for (int64_t i = a; st > 0 ? i < b : i > b; i += st) buf[k++] = str_data(&seq)[i];
        buf[k] = '\0'; return str_take(buf, count);
    }
    if (seq.as.o->tag == O_TUPLE) {
        Obj *o = rt_obj_new(O_TUPLE); o->as.list.items = xmalloc(sizeof(Value) * (count ? count : 1));
        o->as.list.cap = o->as.list.len = count; obj_charge(o, sizeof(Value) * (count ? count : 1));
        size_t k = 0; for (int64_t i = a; st > 0 ? i < b : i > b; i += st) o->as.list.items[k++] = seq.as.o->as.list.items[i];
        return obj_val(o);
    }
    Obj *o = list_new(count); size_t k = 0;
    for (int64_t i = a; st > 0 ? i < b : i > b; i += st) o->as.list.items[k++] = seq.as.o->as.list.items[i];
    o->as.list.len = count; return obj_val(o);
}

/* freeze/thaw: a `seq` is an O_LIST the checker refuses to mutate, so
 * freeze is a no-op at runtime and thaw copies — mutating the thawed list
 * must never be visible through the frozen sequence it was copied from. */
Value em_freeze(Value xs) {
    if (!is_list(xs)) rt_fatal("freeze() expects a list, got %s", type_name(xs));
    return xs;
}

Value em_thaw(Value xs) {
    if (!is_list(xs)) rt_fatal("thaw() expects a seq, got %s", type_name(xs));
    Obj *src = xs.as.o;
    Obj *o = list_new(src->as.list.len);
    memcpy(o->as.list.items, src->as.list.items, sizeof(Value) * src->as.list.len);
    o->as.list.len = src->as.list.len;
    return obj_val(o);
}

Value em_ord(Value c) {
    if (!is_str(c)) rt_fatal("ord() expects a str, got %s", type_name(c));
    if (str_len(&c) == 0) rt_fatal("ord() of an empty string");
    return em_int((int64_t)(unsigned char)str_data(&c)[0]);
}

Value em_chr(Value n) {
    if (n.tag != V_INT) rt_fatal("chr() expects an int, got %s", type_name(n));
    if (n.as.i < 0 || n.as.i > 255)
        rt_fatal("chr() argument out of range (0..255): %" PRId64, n.as.i);
    char b[1] = { (char)(unsigned char)n.as.i };
    return str_copy(b, 1);
}

Value em_float_of(Value v) {
    if (is_num(v)) return em_float(as_double(v));
    if (is_str(v)) {
        const char *s = str_data(&v);
        char *end;
        double r = strtod(s, &end);
        while (*end == ' ') end++;
        if (end == s || *end != '\0')
            rt_fatal("invalid literal for float(): '%s'", s);
        return em_float(r);
    }
    rt_fatal("cannot convert %s to float", type_name(v));
    return em_none();
}

void em_eprint(size_t n, ...) {
    SB sb = {0};
    va_list ap;
    va_start(ap, n);
    for (size_t i = 0; i < n; i++) {
        if (i) sb_puts(&sb, " ");
        write_value(&sb, va_arg(ap, Value), false);
    }
    va_end(ap);
    sb_puts(&sb, "\n");
    fwrite(sb.buf ? sb.buf : "\n", 1, sb.len, stderr);
    free(sb.buf);
}

/* argv, captured by main() before any Emerald code runs */
static int rt_argc = 0;

static char **rt_argv = NULL;

void rt_set_args(int argc, char **argv) {
    rt_argc = argc;
    rt_argv = argv;
}

Value em_argv(void) {
    size_t n = rt_argc > 0 ? (size_t)rt_argc : 0;
    Obj *o = list_new(n);
    Value list = obj_val(o);
    RootFrame fr;
    rt_push_frame(&fr, &list, 1);
    for (size_t i = 0; i < n; i++) {
        Value s = em_str_new(rt_argv[i]);
        /* str_copy may collect; the list is rooted, so append after */
        em_append(list, s);
    }
    rt_pop_frame();
    return list;
}

void em_exit(Value code) {
    if (code.tag != V_INT)
        rt_fatal("exit() expects an int, got %s", type_name(code));
    exit((int)code.as.i);
}

/* --- first-class functions ------------------------------------------------ */
Value em_mkclosure(Value (*fn)(Value *env, Value *args), size_t arity,
                   Value *env, size_t env_count, Value *defaults,
                   size_t default_count) {
    RootFrame fr, df;
    rt_push_frame(&fr, env, env_count); /* root env while the Obj may allocate */
    if (default_count) rt_push_frame(&df, defaults, default_count);
    Obj *o = rt_obj_new(O_FUNC);
    o->as.func.fn = fn;
    o->as.func.arity = arity;
    o->as.func.default_count = default_count;
    o->as.func.min_arity = arity - default_count;
    if (default_count) {
        o->as.func.defaults = xmalloc(sizeof(Value) * default_count);
        memcpy(o->as.func.defaults, defaults, sizeof(Value) * default_count);
        obj_charge(o, sizeof(Value) * default_count);
    } else o->as.func.defaults = NULL;
    if (env_count) {
        o->as.func.env = xmalloc(sizeof(Value) * env_count);
        memcpy(o->as.func.env, env, sizeof(Value) * env_count);
        o->as.func.env_count = env_count;
        obj_charge(o, sizeof(Value) * env_count);
    } else {
        o->as.func.env = NULL;
        o->as.func.env_count = 0;
    }
    if (default_count) rt_pop_frame();
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
    if (argc < c->as.func.min_arity || argc > c->as.func.arity)
        rt_fatal("function expects %zu..%zu argument(s), got %zu", c->as.func.min_arity, c->as.func.arity, argc);
    size_t passed = argc;
    size_t total = c->as.func.arity;
    Value *args = xmalloc(sizeof(Value) * (total ? total : 1));
    va_list ap;
    va_start(ap, argc);
    for (size_t i = 0; i < passed; i++) args[i] = va_arg(ap, Value);
    va_end(ap);
    for (size_t i = passed; i < total; i++) args[i] = c->as.func.defaults[i - c->as.func.min_arity];
    RootFrame fr;
    rt_push_frame(&fr, args, total); /* root args while the callee may allocate */
    Value r = c->as.func.fn(c->as.func.env, args);
    rt_pop_frame();
    free(args);
    return r;
}

/* call a closure with `n` arguments packed into `args` (rooted by the caller) */
Value call_closure_n(Value fn, Value *args, size_t n) {
    if (!(fn.tag == V_OBJ && fn.as.o->tag == O_FUNC))
        rt_fatal("attempt to call a value of type %s", type_name(fn));
    Obj *c = fn.as.o;
    if (n < c->as.func.min_arity || n > c->as.func.arity)
        rt_fatal("function expects %zu..%zu argument(s), got %zu", c->as.func.min_arity, c->as.func.arity, n);
    if (n == c->as.func.arity) return c->as.func.fn(c->as.func.env, args);
    Value *full = xmalloc(sizeof(Value) * c->as.func.arity);
    memcpy(full, args, sizeof(Value) * n);
    for (size_t i=n;i<c->as.func.arity;i++) full[i]=c->as.func.defaults[i-c->as.func.min_arity];
    Value r=c->as.func.fn(c->as.func.env,full); free(full); return r;
}

/* ---------------------------------------------------------------------- */
/* green threads: the cooperative scheduler                                */
/* ---------------------------------------------------------------------- */
/* Emerald tasks are green threads: many tasks, one at a time. Each task gets
 * a real OS thread for its C stack (the generated code is ordinary C, so a
 * task needs a stack it can block on), but a single scheduling token decides
 * which one runs. A task holds the token from the moment it is resumed until
 * it reaches a switch point -- spawn's child start, yield(), sleep(), join(),
 * or a channel operation that cannot complete immediately -- so there is
 * never more than one thread inside the runtime.
 *
 * That is what keeps the rest of the runtime unchanged: no locks around the
 * allocator, no atomics in the write barrier, and a GC that still stops
 * exactly one mutator. The only thing the collector has to learn is that
 * there are now several shadow stacks instead of one, so `rt_roots` is
 * thread-local and every live task registers the address of its own head
 * pointer (see gc_mark_roots).
 *
 * The token is handed over through one mutex and one broadcast condition
 * variable. A task may take it when it is at the head of the run queue and
 * nobody holds it; every state change broadcasts, and each waiter re-tests
 * its own predicate. That is O(waiters) wakeups per switch, which is the
 * right trade here: switches happen at channel granularity, not per
 * instruction, and the alternative (a condvar per task) buys throughput at
 * the cost of a much subtler handoff. */
pthread_mutex_t sch_mu = PTHREAD_MUTEX_INITIALIZER;
