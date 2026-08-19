/* Emerald runtime implementation. See runtime.h for the model. */
#include "runtime.h"

#include <inttypes.h>
#include <pthread.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Strings of SSO_MAX bytes or fewer live inline in the Value (see runtime.h). */
#define SSO_MAX 7

_Thread_local RootFrame *rt_roots = NULL;

/* Every green thread has its own shadow stack; the scheduler owns the list of
 * live tasks, so root marking goes through it (see the scheduler section). */
static void sch_mark(bool minor);
static void sch_init(void);

/* A channel is a ring buffer of `cap` slots plus the two wait queues. An
 * unbuffered channel (cap 0) has no slots at all, so a send and a receive
 * have to meet: whichever arrives first parks, and the second one hands the
 * value over directly and wakes it. */
struct Chan {
    Value *buf;
    size_t cap, len, head;
    bool closed;
    Task *sendq, *sendq_tail;
    Task *recvq, *recvq_tail;
};

typedef enum { T_RUNNABLE, T_BLOCKED, T_SLEEPING, T_DONE } TState;

struct Task {
    Obj *handle;            /* the O_TASK object this Task backs */
    pthread_t thread;
    TState state;
    /* GC-visible values. `xfer` is the value in flight across a channel
     * rendezvous: the sender parks it here until a receiver takes it. */
    Value fn, result, xfer;
    bool xfer_ok;           /* recv woke with a value (false: channel closed) */
    bool send_failed;       /* the channel closed while this send was parked */
    RootFrame **roots;      /* &rt_roots of this task's thread */
    const char *src_file;   /* the spawner's source file: the child inherits it
                             * so a runtime error still names a file even
                             * before the task crosses a file boundary */
    Task *qnext;            /* run queue, or one wait queue (never both) */
    Task *joinq;            /* tasks blocked in join() on this one */
    Task *next;             /* every task that has not finished */
};


/* xorshift64* PRNG; seeded from the clock at startup */
static uint64_t rng_state = 88172645463325252ULL;

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
static size_t gc_young_bytes = 0;  /* bytes live in the nursery */
static size_t gc_old_bytes = 0;    /* bytes live in the tenured generation */
static size_t gc_young_threshold = 256;
static size_t gc_old_threshold = 256;
static size_t gc_young_bytes_threshold = 4u << 20;   /* 4 MiB: collect large buffers */
static size_t gc_old_bytes_threshold = 4u << 20;
static size_t gc_collections = 0;  /* total cycles (minor + major) */

_Thread_local const char *rt_cur_file = NULL; /* set by generated code */
_Thread_local int rt_cur_line = 0;

void rt_init(void) {
    sch_init();
    uint64_t t = (uint64_t)time(NULL) ^ ((uint64_t)clock() << 32);
    rng_state = t ? t : 88172645463325252ULL;
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("emerald: runtime error: ", stderr);
    vfprintf(stderr, fmt, ap);
    if (rt_cur_file)
        fprintf(stderr, " (at %s:%d)", rt_cur_file, rt_cur_line);
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
    case O_TENSOR:
        /* a view keeps its owner alive; the owner owns the data buffer */
        if (o->as.tensor.base) gc_mark_obj(o->as.tensor.base, minor);
        break;
    case O_CHAN:
        /* buffered items; values parked in a blocked sender are reached
         * through that sender's task instead */
        for (size_t i = 0; i < o->as.chan->len; i++)
            gc_mark_value(o->as.chan->buf[(o->as.chan->head + i) % o->as.chan->cap],
                          minor);
        break;
    case O_TASK:
        gc_mark_value(o->as.task->fn, minor);
        gc_mark_value(o->as.task->result, minor);
        gc_mark_value(o->as.task->xfer, minor);
        break;
    }
}

static void gc_mark_value(Value v, bool minor) {
    if (v.tag == V_OBJ) gc_mark_obj(v.as.o, minor);
}

static void gc_mark_roots(bool minor) {
    sch_mark(minor);
}

static void gc_free_obj(Obj *o) {
    switch (o->tag) {
    case O_STR:  free(o->as.str.data); break;
    case O_LIST: free(o->as.list.items); break;
    case O_REC:  free(o->as.rec.keys); free(o->as.rec.vals); break;
    case O_FUNC: free(o->as.func.env); break;
    case O_CELL: break;
    case O_TENSOR:
        free(o->as.tensor.dims);
        free(o->as.tensor.strides);
        if (!o->as.tensor.base) free(o->as.tensor.data); /* views don't own */
        break;
    case O_CHAN:
        free(o->as.chan->buf);
        free(o->as.chan);
        break;
    case O_TASK:
        /* only reachable once the task has finished and left the live list */
        free(o->as.task);
        break;
    }
    /* release the object's bytes from its generation's counter */
    if (o->gen == GEN_YOUNG) gc_young_bytes -= o->nbytes;
    else gc_old_bytes -= o->nbytes;
    free(o);
}

/* Charge `bytes` of backing storage to `o`'s generation. Called whenever an
 * object's owned memory grows (list growth, record field add, tensor data),
 * so the byte counters stay exact and a large buffer is never invisible to
 * the collector. */
static void obj_charge(Obj *o, size_t bytes) {
    o->nbytes += bytes;
    if (o->gen == GEN_YOUNG) gc_young_bytes += bytes;
    else gc_old_bytes += bytes;
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
            gc_young_bytes -= o->nbytes;
            gc_old_bytes += o->nbytes;
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
    gc_young_bytes_threshold = gc_old_bytes * 2 < (4u << 20) ? (4u << 20)
                                                             : gc_old_bytes * 2;
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
    gc_old_bytes_threshold = gc_old_bytes * 2 < (4u << 20) ? (4u << 20)
                                                           : gc_old_bytes * 2;
    gc_young_bytes_threshold = gc_old_bytes * 2 < (4u << 20) ? (4u << 20)
                                                             : gc_old_bytes * 2;
}

void rt_gc_collect(void) { gc_collect_major(); }

static Obj *rt_obj_new(OTag tag) {
    if (gc_young_count >= gc_young_threshold) gc_collect_minor();
    if (gc_old_count >= gc_old_threshold) gc_collect_major();
    if (gc_young_bytes >= gc_young_bytes_threshold) gc_collect_minor();
    if (gc_old_bytes >= gc_old_bytes_threshold) gc_collect_major();
    Obj *o = xmalloc(sizeof(Obj));
    memset(o, 0, sizeof(Obj));
    o->tag = tag;
    o->gen = GEN_YOUNG;
    o->gc_next = gc_young;
    gc_young = o;
    gc_young_count++;
    gc_young_bytes += sizeof(Obj);
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
    obj_charge(o, len + 1);
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
    obj_charge(o, len + 1);
    return obj_val(o);
}

Value em_str_new(const char *cstr) {
    return str_copy(cstr, strlen(cstr));
}

/* A fresh empty list with room for `n` items (len 0). Every list constructor
 * goes through here so the capacity, the byte charge, and the zero-length
 * case are decided once. */
static Obj *list_new(size_t n) {
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
        case O_TENSOR: return "tensor";
        case O_CHAN: return "channel";
        case O_TASK: return "task";
        }
    }
    return "?";
}

static bool is_str(Value v)  { return v.tag == V_STR || (v.tag == V_OBJ && v.as.o->tag == O_STR); }
static bool is_list(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_LIST; }
static bool is_rec(Value v)  { return v.tag == V_OBJ && v.as.o->tag == O_REC; }
static bool is_num(Value v)  { return v.tag == V_INT || v.tag == V_FLOAT || v.tag == V_BOOL; }
static bool is_tensor(Value v) { return v.tag == V_OBJ && v.as.o->tag == O_TENSOR; }

/* zero-copy tensor view (defined in the tensor section below) */
static Value tensor_view(Obj *base, DType dt, uint8_t ndim, const int64_t *dims,
                         const int64_t *strides, size_t elem_offset);

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
        case O_TENSOR: {
            Obj *t = v.as.o;
            sb_puts(sb, "Tensor[");
            sb_puts(sb, t->as.tensor.dt == DT_F64 ? "f64" : "f32");
            sb_puts(sb, ", [");
            for (uint8_t d = 0; d < t->as.tensor.ndim; d++) {
                if (d) sb_puts(sb, ", ");
                char tmp[24];
                snprintf(tmp, sizeof tmp, "%" PRId64, t->as.tensor.dims[d]);
                sb_puts(sb, tmp);
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

/* ---------------------------------------------------------------------- */
/* pretty printing (pprint / pp_format)                                    */
/* ---------------------------------------------------------------------- */
/*
 * `pprint` renders a value the way a human wants to read it: like `str`, but
 * a list or record that would not fit on the line breaks across lines, one
 * element/field per line, PP_INDENT spaces deeper per nesting level. Strings
 * are always quoted, exactly like Python's pprint. Everything that is not a
 * list or record is atomic and never wraps.
 */
#define PP_INDENT 2
#define PP_WIDTH  80

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

#define NUM_BINOP(name, op, sym)                                              \
    Value name(Value a, Value b) {                                            \
        if (is_tensor(a) || is_tensor(b)) return em_tensor_sub(a, b);         \
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

Value em_neg(Value a) {
    if (is_tensor(a)) return em_tensor_mul(a, em_float(-1.0));
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
    case O_TENSOR: return x == y; /* tensors compare by identity */
    case O_CHAN: case O_TASK: return x == y; /* handles compare by identity */
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
    if (idx.tag != V_INT) rt_fatal("indices must be int, not %s", type_name(idx));
    if (is_list(seq))
        return seq.as.o->as.list.items[norm_index(idx.as.i, seq.as.o->as.list.len, "list")];
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
        obj_charge(o, sizeof(Value) * env_count);
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

/* call a closure with `n` arguments packed into `args` (rooted by the caller) */
static Value call_closure_n(Value fn, Value *args, size_t n) {
    if (!(fn.tag == V_OBJ && fn.as.o->tag == O_FUNC))
        rt_fatal("attempt to call a value of type %s", type_name(fn));
    Obj *c = fn.as.o;
    if (c->as.func.arity != n)
        rt_fatal("function expects %zu argument(s), got %zu", c->as.func.arity, n);
    return c->as.func.fn(c->as.func.env, args);
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

static pthread_mutex_t sch_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sch_cv = PTHREAD_COND_INITIALIZER;
static Task *sch_all = NULL;            /* every task that is not T_DONE */
static Task *sch_runq = NULL, *sch_runq_tail = NULL;
static Task *sch_cur = NULL;            /* the token holder, or NULL */
static size_t sch_alive = 0, sch_sleepers = 0;
static size_t sch_spawned = 0, sch_switches = 0;   /* for gc_stats() */
static Task sch_main;                   /* the task the program starts on */

static _Thread_local Task *sch_self = NULL;

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void runq_push(Task *t) {
    t->qnext = NULL;
    if (sch_runq_tail) sch_runq_tail->qnext = t;
    else sch_runq = t;
    sch_runq_tail = t;
}

static Task *runq_pop(void) {
    Task *t = sch_runq;
    if (!t) return NULL;
    sch_runq = t->qnext;
    if (!sch_runq) sch_runq_tail = NULL;
    t->qnext = NULL;
    return t;
}

/* Nobody holds the token, nothing is runnable, and nothing will ever become
 * runnable on its own: every remaining task is parked on a channel or a join
 * that no other task can complete. Report it the way Go does rather than
 * hanging forever. */
static void sch_check_deadlock(void) {
    if (sch_cur == NULL && sch_runq == NULL && sch_sleepers == 0 && sch_alive > 0)
        rt_fatal("all %zu task(s) are blocked forever - deadlock", sch_alive);
}

/* Give up the token. The caller has already parked itself wherever it needs
 * to be found again (a run queue, a channel's wait queue, a join queue). */
static void sch_release(void) {
    sch_cur = NULL;
    sch_switches++;
    pthread_cond_broadcast(&sch_cv);
    sch_check_deadlock();
}

/* Wait for the token. `self` must be RUNNABLE and sitting in the run queue. */
static void sch_acquire(Task *self) {
    for (;;) {
        if (sch_cur == NULL && sch_runq == self) {
            runq_pop();
            sch_cur = self;
            return;
        }
        pthread_cond_wait(&sch_cv, &sch_mu);
    }
}

/* Make a parked task runnable again. Called by whoever satisfies its wait. */
static void sch_wake(Task *t) {
    t->state = T_RUNNABLE;
    runq_push(t);
    pthread_cond_broadcast(&sch_cv);
}

/* Park the running task. The caller has linked `self` into the wait queue
 * that some other task will wake it from, and holds sch_mu. */
static void sch_block(Task *self) {
    self->state = T_BLOCKED;
    sch_release();
    while (self->state != T_RUNNABLE) pthread_cond_wait(&sch_cv, &sch_mu);
    sch_acquire(self);
}

static void sch_yield_locked(Task *self) {
    runq_push(self);
    sch_release();
    sch_acquire(self);
}

/* GC: walk every task's shadow stack and the values the scheduler itself
 * holds on their behalf. Those values (`fn`, `result`, `xfer`) are marked
 * here as roots on every collection, minor ones included, so storing into
 * them needs no write barrier -- unlike a channel's buffer, which is reached
 * through the channel object and therefore does. Only the token holder ever collects, and every other
 * task is parked at a switch point, so all of these are quiescent. */
static void sch_mark(bool minor) {
    for (Task *t = sch_all; t; t = t->next) {
        for (RootFrame *f = *t->roots; f; f = f->prev)
            for (size_t i = 0; i < f->count; i++)
                gc_mark_value(f->slots[i], minor);
        gc_mark_value(t->fn, minor);
        gc_mark_value(t->result, minor);
        gc_mark_value(t->xfer, minor);
        if (t->handle) gc_mark_obj(t->handle, minor);
    }
}

static void sch_init(void) {
    memset(&sch_main, 0, sizeof sch_main);
    sch_main.state = T_RUNNABLE;
    sch_main.fn = sch_main.result = sch_main.xfer = em_none();
    sch_main.roots = &rt_roots;
    sch_main.thread = pthread_self();
    sch_self = &sch_main;
    sch_all = &sch_main;
    sch_alive = 1;
    sch_cur = &sch_main;
}

/* --- channels ------------------------------------------------------------ */

/* The wait queues are plain FIFOs, so a channel is fair: the longest-waiting
 * sender or receiver is the one a rendezvous picks. */
static void chanq_push(Task **head, Task **tail, Task *t) {
    t->qnext = NULL;
    if (*tail) (*tail)->qnext = t;
    else *head = t;
    *tail = t;
}

static Task *chanq_pop(Task **head, Task **tail) {
    Task *t = *head;
    if (!t) return NULL;
    *head = t->qnext;
    if (!*head) *tail = NULL;
    t->qnext = NULL;
    return t;
}

static Obj *as_chan(Value v, const char *who) {
    if (!(v.tag == V_OBJ && v.as.o->tag == O_CHAN))
        rt_fatal("%s() expects a channel, got %s", who, type_name(v));
    return v.as.o;
}

static Obj *as_task(Value v, const char *who) {
    if (!(v.tag == V_OBJ && v.as.o->tag == O_TASK))
        rt_fatal("%s() expects a task, got %s", who, type_name(v));
    return v.as.o;
}

Value em_chan(Value cap) {
    if (cap.tag != V_INT || cap.as.i < 0)
        rt_fatal("chan() capacity must be a non-negative int");
    size_t n = (size_t)cap.as.i;
    Value *buf = n ? xmalloc(sizeof(Value) * n) : NULL;
    Chan *c = xmalloc(sizeof(Chan));
    memset(c, 0, sizeof *c);
    c->buf = buf;
    c->cap = n;
    Obj *o = rt_obj_new(O_CHAN);   /* may collect: nothing live is unrooted */
    o->as.chan = c;
    obj_charge(o, sizeof(Chan) + sizeof(Value) * n);
    return obj_val(o);
}

void em_send(Value chv, Value v) {
    Obj *o = as_chan(chv, "send");
    Chan *c = o->as.chan;
    Task *self = sch_self;
    pthread_mutex_lock(&sch_mu);
    if (c->closed) {
        pthread_mutex_unlock(&sch_mu);
        rt_fatal("send on a closed channel");
    }
    Task *r = chanq_pop(&c->recvq, &c->recvq_tail);
    if (r) {                       /* a receiver is parked: hand it over */
        r->xfer = v;
        r->xfer_ok = true;
        sch_wake(r);
        pthread_mutex_unlock(&sch_mu);
        return;
    }
    if (c->len < c->cap) {         /* room in the buffer */
        c->buf[(c->head + c->len) % c->cap] = v;
        c->len++;
        gc_write_barrier(o, v);
        pthread_mutex_unlock(&sch_mu);
        return;
    }
    self->xfer = v;                /* park until a receiver takes it */
    self->send_failed = false;
    chanq_push(&c->sendq, &c->sendq_tail, self);
    sch_block(self);
    bool failed = self->send_failed;
    self->xfer = em_none();
    pthread_mutex_unlock(&sch_mu);
    if (failed) rt_fatal("send on a closed channel");
}

Value em_recv(Value chv) {
    Obj *o = as_chan(chv, "recv");
    Chan *c = o->as.chan;
    Task *self = sch_self;
    pthread_mutex_lock(&sch_mu);
    if (c->len > 0) {              /* take from the buffer, then refill it */
        Value v = c->buf[c->head];
        c->head = (c->head + 1) % c->cap;
        c->len--;
        Task *s = chanq_pop(&c->sendq, &c->sendq_tail);
        if (s) {
            c->buf[(c->head + c->len) % c->cap] = s->xfer;
            c->len++;
            gc_write_barrier(o, s->xfer);
            sch_wake(s);
        }
        pthread_mutex_unlock(&sch_mu);
        return v;
    }
    Task *s = chanq_pop(&c->sendq, &c->sendq_tail);
    if (s) {                       /* unbuffered rendezvous */
        Value v = s->xfer;
        sch_wake(s);
        pthread_mutex_unlock(&sch_mu);
        return v;
    }
    if (c->closed) {               /* drained and closed: None, forever */
        pthread_mutex_unlock(&sch_mu);
        return em_none();
    }
    self->xfer = em_none();
    self->xfer_ok = false;
    chanq_push(&c->recvq, &c->recvq_tail, self);
    sch_block(self);
    Value v = self->xfer_ok ? self->xfer : em_none();
    self->xfer = em_none();
    pthread_mutex_unlock(&sch_mu);
    return v;
}

void em_close(Value chv) {
    Obj *o = as_chan(chv, "close");
    Chan *c = o->as.chan;
    pthread_mutex_lock(&sch_mu);
    if (c->closed) {
        pthread_mutex_unlock(&sch_mu);
        rt_fatal("close of an already closed channel");
    }
    c->closed = true;
    for (Task *t; (t = chanq_pop(&c->recvq, &c->recvq_tail)) != NULL; ) {
        t->xfer_ok = false;        /* woken receivers see the drained channel */
        sch_wake(t);
    }
    for (Task *t; (t = chanq_pop(&c->sendq, &c->sendq_tail)) != NULL; ) {
        t->send_failed = true;     /* a parked send on a closed channel is fatal */
        sch_wake(t);
    }
    pthread_mutex_unlock(&sch_mu);
}

Value em_chan_len(Value chv) {
    Obj *o = as_chan(chv, "chan_len");
    pthread_mutex_lock(&sch_mu);
    int64_t n = (int64_t)o->as.chan->len;
    pthread_mutex_unlock(&sch_mu);
    return em_int(n);
}

/* --- spawn / join / yield / sleep ---------------------------------------- */

static void *task_trampoline(void *arg) {
    Task *self = arg;
    sch_self = self;
    rt_cur_file = self->src_file;
    pthread_mutex_lock(&sch_mu);
    self->roots = &rt_roots;       /* this thread's own shadow stack */
    sch_acquire(self);             /* wait our turn before touching the heap */
    pthread_mutex_unlock(&sch_mu);

    Value fn = self->fn;
    RootFrame fr;
    rt_push_frame(&fr, &self->result, 1);
    self->result = em_call(fn, 0);
    rt_pop_frame();

    pthread_mutex_lock(&sch_mu);
    self->state = T_DONE;
    self->fn = em_none();
    for (Task *t = self->joinq; t; ) {
        Task *n = t->qnext;
        t->xfer = self->result;
        t->xfer_ok = true;
        sch_wake(t);
        t = n;
    }
    self->joinq = NULL;
    /* Leave the all-tasks list: the Task struct now only has to stay alive
     * for whoever still holds the handle, and the GC owns that decision. */
    for (Task **link = &sch_all; *link; link = &(*link)->next)
        if (*link == self) { *link = self->next; break; }
    self->next = NULL;
    self->roots = NULL;
    sch_alive--;
    sch_release();
    pthread_mutex_unlock(&sch_mu);
    return NULL;
}

Value em_spawn(Value fn) {
    if (!(fn.tag == V_OBJ && fn.as.o->tag == O_FUNC))
        rt_fatal("spawn() expects a function, got %s", type_name(fn));
    if (fn.as.o->as.func.arity != 0)
        rt_fatal("spawn() expects a function of no arguments, got one of %zu",
                 fn.as.o->as.func.arity);
    Task *t = xmalloc(sizeof(Task));
    memset(t, 0, sizeof *t);
    t->state = T_RUNNABLE;
    t->fn = fn;
    t->result = t->xfer = em_none();
    t->src_file = rt_cur_file;
    /* Allocate the handle before the thread exists: rt_obj_new() may collect,
     * and a half-built task must not be reachable when it does. */
    Obj *o = rt_obj_new(O_TASK);
    o->as.task = t;
    t->handle = o;
    obj_charge(o, sizeof(Task));

    pthread_mutex_lock(&sch_mu);
    t->roots = &rt_roots;          /* replaced by the child with its own */
    t->next = sch_all;
    sch_all = t;
    sch_alive++;
    sch_spawned++;
    runq_push(t);                  /* runnable, but the spawner keeps running */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t->thread, &attr, task_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_unlock(&sch_mu);
        rt_fatal("could not start task: %s", strerror(rc));
    }
    pthread_mutex_unlock(&sch_mu);
    return obj_val(o);
}

Value em_join(Value tv) {
    Obj *o = as_task(tv, "join");
    Task *t = o->as.task;
    Task *self = sch_self;
    if (t == self) rt_fatal("join() on the running task would deadlock");
    pthread_mutex_lock(&sch_mu);
    if (t->state == T_DONE) {
        Value r = t->result;
        pthread_mutex_unlock(&sch_mu);
        return r;
    }
    self->qnext = t->joinq;        /* the join queue is a plain stack */
    t->joinq = self;
    self->state = T_BLOCKED;
    sch_release();
    while (self->state != T_RUNNABLE) pthread_cond_wait(&sch_cv, &sch_mu);
    sch_acquire(self);
    Value r = self->xfer;
    self->xfer = em_none();
    pthread_mutex_unlock(&sch_mu);
    return r;
}

/* Observability, mirroring gc_stats(): how many tasks have been started, how
 * many are still alive, and how many times the token has changed hands. */
Value em_task_stats(void) {
    pthread_mutex_lock(&sch_mu);
    int64_t spawned = (int64_t)sch_spawned, alive = (int64_t)sch_alive,
            switches = (int64_t)sch_switches;
    pthread_mutex_unlock(&sch_mu);
    return em_rec_litn(3, "spawned", em_int(spawned), "alive", em_int(alive),
                       "switches", em_int(switches));
}

Value em_task_done(Value tv) {
    Obj *o = as_task(tv, "task_done");
    pthread_mutex_lock(&sch_mu);
    bool done = o->as.task->state == T_DONE;
    pthread_mutex_unlock(&sch_mu);
    return em_bool(done);
}

void em_yield(void) {
    Task *self = sch_self;
    pthread_mutex_lock(&sch_mu);
    if (sch_runq) sch_yield_locked(self);   /* nobody waiting: stay put */
    pthread_mutex_unlock(&sch_mu);
}

void em_sleep(Value secs) {
    double s = secs.tag == V_FLOAT ? secs.as.f
             : secs.tag == V_INT   ? (double)secs.as.i
             : (rt_fatal("sleep() expects a number, got %s", type_name(secs)), 0.0);
    if (s < 0) s = 0;
    Task *self = sch_self;
    double deadline = mono_now() + s;
    pthread_mutex_lock(&sch_mu);
    self->state = T_SLEEPING;
    sch_sleepers++;
    sch_release();
    for (;;) {
        double left = deadline - mono_now();
        if (left <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)left;
        ts.tv_nsec += (long)((left - (double)(time_t)left) * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&sch_cv, &sch_mu, &ts);
    }
    sch_sleepers--;
    self->state = T_RUNNABLE;
    runq_push(self);
    sch_acquire(self);
    pthread_mutex_unlock(&sch_mu);
}

/* --- higher-order list builtins ------------------------------------------ */

Value em_map(Value fn, Value xs) {
    if (!(xs.tag == V_OBJ && xs.as.o->tag == O_LIST))
        rt_fatal("map() expects a list, got %s", type_name(xs));
    Obj *src = xs.as.o;
    size_t n = src->as.list.len;
    Obj *dst = list_new(n);
    /* the result is traced by the GC from here on, so every slot must hold a
     * real Value before the mapping function can allocate */
    for (size_t i = 0; i < n; i++) dst->as.list.items[i] = em_none();
    dst->as.list.len = n;
    Value result = obj_val(dst);
    RootFrame fr;
    rt_push_frame(&fr, &result, 1); /* root the result while fn may allocate */
    for (size_t i = 0; i < n; i++) {
        Value a[1] = { src->as.list.items[i] };
        RootFrame af;
        rt_push_frame(&af, a, 1);
        Value r = call_closure_n(fn, a, 1);
        rt_pop_frame();
        dst->as.list.items[i] = r;
        gc_write_barrier(dst, r);
    }
    rt_pop_frame();
    return result;
}

Value em_filter(Value fn, Value xs) {
    if (!(xs.tag == V_OBJ && xs.as.o->tag == O_LIST))
        rt_fatal("filter() expects a list, got %s", type_name(xs));
    Obj *src = xs.as.o;
    Value result = obj_val(list_new(0));
    RootFrame fr;
    rt_push_frame(&fr, &result, 1);
    for (size_t i = 0; i < src->as.list.len; i++) {
        Value a[1] = { src->as.list.items[i] };
        RootFrame af;
        rt_push_frame(&af, a, 1);
        Value keep = call_closure_n(fn, a, 1);
        rt_pop_frame();
        if (em_truthy(keep)) em_append(result, a[0]);
    }
    rt_pop_frame();
    return result;
}

Value em_reduce(Value fn, Value acc, Value xs) {
    if (!(xs.tag == V_OBJ && xs.as.o->tag == O_LIST))
        rt_fatal("reduce() expects a list, got %s", type_name(xs));
    Obj *src = xs.as.o;
    RootFrame fr;
    rt_push_frame(&fr, &acc, 1);
    for (size_t i = 0; i < src->as.list.len; i++) {
        Value args[2] = { acc, src->as.list.items[i] };
        RootFrame af;
        rt_push_frame(&af, args, 2);
        Value r = call_closure_n(fn, args, 2);
        rt_pop_frame();
        acc = r;
    }
    rt_pop_frame();
    return acc;
}

/* `f >> g`: a closure h(x) = g(f(x)) */
static Value compose_tramp(Value *env, Value *args) {
    Value fx = call_closure_n(env[0], args, 1);
    RootFrame fr;
    rt_push_frame(&fr, &fx, 1); /* root fx while g may allocate */
    Value r = call_closure_n(env[1], &fx, 1);
    rt_pop_frame();
    return r;
}

Value em_compose(Value f, Value g) {
    Value env[2] = { f, g };
    return em_mkclosure(compose_tramp, 1, env, 2);
}

/* ---------------------------------------------------------------------- */
/* tensors                                                                */
/* ---------------------------------------------------------------------- */

#define MAX_TDIM 255

static size_t dt_size(DType dt) {
    switch (dt) {
    case DT_F64: return 8;
    default:     return 4; /* f32 and the reserved tags default to 4 bytes */
    }
}

static int64_t t_numel_of(uint8_t ndim, const int64_t *dims) {
    int64_t n = 1;
    for (uint8_t d = 0; d < ndim; d++) n *= dims[d];
    return n;
}

/* decode a logical flat index into a multi-index over dims[0..ndim) */
static void t_unravel_of(uint8_t ndim, const int64_t *dims, size_t flat,
                         int64_t *idx) {
    for (int d = (int)ndim - 1; d >= 0; d--) {
        idx[d] = (int64_t)(flat % (size_t)dims[d]);
        flat /= (size_t)dims[d];
    }
}

/* element offset of a multi-index under the given strides */
static size_t t_offset_of(uint8_t ndim, const int64_t *strides,
                          const int64_t *idx) {
    size_t off = 0;
    for (uint8_t d = 0; d < ndim; d++)
        off += (size_t)(idx[d] * strides[d]);
    return off;
}

/* element access: data is a float buffer; views point into their owner's */
static double t_get(const Obj *t, size_t off) {
    if (t->as.tensor.dt == DT_F64) return ((const double *)t->as.tensor.data)[off];
    return (double)((const float *)t->as.tensor.data)[off];
}
static void t_set(Obj *t, size_t off, double v) {
    if (t->as.tensor.dt == DT_F64) ((double *)t->as.tensor.data)[off] = v;
    else ((float *)t->as.tensor.data)[off] = (float)v;
}

static DType parse_dtype(const char *s) {
    if (strcmp(s, "f32") == 0) return DT_F32;
    if (strcmp(s, "f64") == 0) return DT_F64;
    rt_fatal("unsupported dtype '%s' (Phase 2 supports f32 and f64)", s);
    return DT_F32;
}

/* read a list[int] into a freshly malloc'd dims array; `out_ndim`/`out_dims`
 * are filled on success. `shape` is rooted by the caller. */
static void parse_shape(Value shape, uint8_t *out_ndim, int64_t **out_dims) {
    if (!is_list(shape))
        rt_fatal("tensor shape must be a list of ints, got %s", type_name(shape));
    size_t n = shape.as.o->as.list.len;
    if (n == 0 || n > MAX_TDIM)
        rt_fatal("tensor shape must have 1..%d dims, got %zu", MAX_TDIM, n);
    int64_t *dims = xmalloc(sizeof(int64_t) * n);
    for (size_t i = 0; i < n; i++) {
        Value d = shape.as.o->as.list.items[i];
        if (d.tag != V_INT || d.as.i < 0) {
            free(dims);
            rt_fatal("tensor shape dims must be non-negative ints");
        }
        dims[i] = d.as.i;
    }
    *out_ndim = (uint8_t)n;
    *out_dims = dims;
}

/* allocate an owned, contiguous, row-major tensor filled with `fill`. The
 * dims array is copied. Charged to the GC so a big buffer triggers collection. */
static Value tensor_new(DType dt, uint8_t ndim, const int64_t *dims, double fill) {
    int64_t numel = t_numel_of(ndim, dims);
    if (numel < 0) rt_fatal("tensor is too large");
    size_t nel = (size_t)numel;
    size_t esz = dt_size(dt);
    int64_t *ds = xmalloc(sizeof(int64_t) * (ndim ? ndim : 1));
    int64_t *st = xmalloc(sizeof(int64_t) * (ndim ? ndim : 1));
    memcpy(ds, dims, sizeof(int64_t) * ndim);
    int64_t acc = 1;
    for (int d = (int)ndim - 1; d >= 0; d--) {
        st[d] = acc;
        acc *= dims[d];
    }
    void *data = xmalloc(nel * esz + esz);
    if (dt == DT_F64) {
        double *p = data;
        for (size_t i = 0; i < nel; i++) p[i] = fill;
    } else {
        float *p = data;
        for (size_t i = 0; i < nel; i++) p[i] = (float)fill;
    }
    Obj *o = rt_obj_new(O_TENSOR);
    o->as.tensor.dt = dt;
    o->as.tensor.ndim = ndim;
    o->as.tensor.dims = ds;
    o->as.tensor.strides = st;
    o->as.tensor.data = data;
    o->as.tensor.base = NULL;
    obj_charge(o, nel * esz);
    return obj_val(o);
}

/* a zero-copy view over `base` (which owns the data buffer, transitively).
 * dims/strides are copied. `elem_offset` is in elements, relative to base->data. */
static Value tensor_view(Obj *base, DType dt, uint8_t ndim, const int64_t *dims,
                         const int64_t *strides, size_t elem_offset) {
    Obj *o = rt_obj_new(O_TENSOR);
    o->as.tensor.dt = dt;
    o->as.tensor.ndim = ndim;
    o->as.tensor.dims = xmalloc(sizeof(int64_t) * (ndim ? ndim : 1));
    o->as.tensor.strides = xmalloc(sizeof(int64_t) * (ndim ? ndim : 1));
    memcpy(o->as.tensor.dims, dims, sizeof(int64_t) * ndim);
    memcpy(o->as.tensor.strides, strides, sizeof(int64_t) * ndim);
    o->as.tensor.data = (char *)base->as.tensor.data + elem_offset * dt_size(dt);
    o->as.tensor.base = base;
    return obj_val(o);
}

/* --- construction ------------------------------------------------------- */

Value em_tensor_zeros(Value shape) {
    uint8_t ndim; int64_t *dims;
    parse_shape(shape, &ndim, &dims);
    Value t = tensor_new(DT_F32, ndim, dims, 0.0);
    free(dims);
    return t;
}

Value em_tensor_ones(Value shape) {
    uint8_t ndim; int64_t *dims;
    parse_shape(shape, &ndim, &dims);
    Value t = tensor_new(DT_F32, ndim, dims, 1.0);
    free(dims);
    return t;
}

Value em_tensor_full(Value shape, Value fill) {
    if (!is_num(fill))
        rt_fatal("full() fill value must be a number, got %s", type_name(fill));
    uint8_t ndim; int64_t *dims;
    parse_shape(shape, &ndim, &dims);
    Value t = tensor_new(DT_F32, ndim, dims, as_double(fill));
    free(dims);
    return t;
}

Value em_tensor_arange(Value n) {
    if (n.tag != V_INT || n.as.i < 0)
        rt_fatal("arange() expects a non-negative int");
    int64_t dims[1] = { n.as.i };
    Value t = tensor_new(DT_F32, 1, dims, 0.0);
    Obj *ro = t.as.o;
    for (int64_t i = 0; i < n.as.i; i++) t_set(ro, (size_t)i, (double)i);
    return t;
}

/* nested list -> tensor. Rank is the list depth; the innermost list must hold
 * only numbers. Rectangularity is validated, not assumed. */
static uint8_t nested_shape(Value v, int64_t *shape, size_t cap) {
    if (!is_list(v)) return 0;
    size_t len = v.as.o->as.list.len;
    if (cap == 0) rt_fatal("tensor(): list nesting too deep");
    shape[0] = (int64_t)len;
    if (len == 0) return 1;
    int64_t first_sub[64];
    uint8_t sub = nested_shape(v.as.o->as.list.items[0], first_sub,
                               sizeof first_sub / sizeof first_sub[0]);
    memcpy(shape + 1, first_sub, sizeof(int64_t) * sub);
    for (size_t i = 1; i < len; i++) {
        int64_t item_sub[64];
        uint8_t s2 = nested_shape(v.as.o->as.list.items[i], item_sub,
                                  sizeof item_sub / sizeof item_sub[0]);
        if (s2 != sub) rt_fatal("tensor(): ragged list (nesting depth differs)");
        for (uint8_t d = 0; d < sub; d++)
            if (item_sub[d] != first_sub[d])
                rt_fatal("tensor(): ragged list (row lengths differ)");
    }
    return sub + 1;
}

static void nested_fill(Value v, Obj *ro, size_t *idx) {
    if (!is_list(v)) {
        if (!is_num(v))
            rt_fatal("tensor(): expected numbers, got %s", type_name(v));
        t_set(ro, (*idx)++, as_double(v));
        return;
    }
    for (size_t i = 0; i < v.as.o->as.list.len; i++)
        nested_fill(v.as.o->as.list.items[i], ro, idx);
}

Value em_tensor_from_list(Value nested) {
    int64_t shape[MAX_TDIM];
    uint8_t ndim = nested_shape(nested, shape, MAX_TDIM);
    if (ndim == 0) rt_fatal("tensor() expects a nested list");
    int64_t numel = t_numel_of(ndim, shape);
    if (numel < 0) rt_fatal("tensor is too large");
    Value t = tensor_new(DT_F32, ndim, shape, 0.0);
    Obj *ro = t.as.o;
    size_t idx = 0;
    nested_fill(nested, ro, &idx);
    return t;
}

/* seeded, deterministic standard-normal samples (Box-Muller over xorshift64*). */
static double next_gauss(uint64_t *st) {
    uint64_t x = *st;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *st = x;
    uint64_t r = x * 2685821657736338717ULL;
    double u1 = (double)(r >> 11) / (double)(1ULL << 53);
    x = *st;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *st = x;
    r = x * 2685821657736338717ULL;
    double u2 = (double)(r >> 11) / (double)(1ULL << 53);
    return sqrt(-2.0 * log(u1 + 1e-300)) * cos(6.283185307179586 * u2);
}

Value em_tensor_randn(Value shape, Value seed) {
    if (seed.tag != V_INT)
        rt_fatal("randn() seed must be an int, got %s", type_name(seed));
    uint8_t ndim; int64_t *dims;
    parse_shape(shape, &ndim, &dims);
    Value t = tensor_new(DT_F32, ndim, dims, 0.0);
    free(dims);
    Obj *ro = t.as.o;
    size_t nel = (size_t)t_numel_of(ro->as.tensor.ndim, ro->as.tensor.dims);
    uint64_t st = (uint64_t)seed.as.i;
    for (size_t i = 0; i < nel; i++) t_set(ro, i, next_gauss(&st));
    return t;
}

/* --- elementwise -------------------------------------------------------- */

static double relu_fn(double x) { return x > 0.0 ? x : 0.0; }

static Value t_unary(Value tv, double (*f)(double)) {
    Obj *t = tv.as.o;
    Value out = tensor_new(t->as.tensor.dt, t->as.tensor.ndim, t->as.tensor.dims,
                           0.0);
    Obj *ro = out.as.o;
    size_t nel = (size_t)t_numel_of(t->as.tensor.ndim, t->as.tensor.dims);
    int64_t idx[MAX_TDIM];
    for (size_t i = 0; i < nel; i++) {
        t_unravel_of(t->as.tensor.ndim, t->as.tensor.dims, i, idx);
        size_t off = t_offset_of(t->as.tensor.ndim, t->as.tensor.strides, idx);
        t_set(ro, i, f(t_get(t, off)));
    }
    return out;
}

Value em_tensor_exp(Value t)  { return t_unary(t, exp); }
Value em_tensor_log(Value t)  { return t_unary(t, log); }
Value em_tensor_tanh(Value t) { return t_unary(t, tanh); }
Value em_tensor_relu(Value t) { return t_unary(t, relu_fn); }

static double op_add(double a, double b) { return a + b; }
static double op_sub(double a, double b) { return a - b; }
static double op_mul(double a, double b) { return a * b; }
static double op_div(double a, double b) {
    if (b == 0.0) rt_fatal("tensor division by zero");
    return a / b;
}

/* apply a binary op to a tensor and a scalar (broadcast the scalar) */
static Value t_scalar_binary(Value tv, Value sv, double (*f)(double, double),
                             bool swap) {
    Obj *t = tv.as.o;
    double s = as_double(sv);
    Value out = tensor_new(t->as.tensor.dt, t->as.tensor.ndim, t->as.tensor.dims,
                           0.0);
    Obj *ro = out.as.o;
    size_t nel = (size_t)t_numel_of(t->as.tensor.ndim, t->as.tensor.dims);
    int64_t idx[MAX_TDIM];
    for (size_t i = 0; i < nel; i++) {
        t_unravel_of(t->as.tensor.ndim, t->as.tensor.dims, i, idx);
        size_t off = t_offset_of(t->as.tensor.ndim, t->as.tensor.strides, idx);
        double x = t_get(t, off);
        t_set(ro, i, swap ? f(s, x) : f(x, s));
    }
    return out;
}

/* tensor ⊕ tensor with numpy-style broadcasting over trailing dims */
static Value t_binary_tt(Value av, Value bv, double (*f)(double, double)) {
    Obj *a = av.as.o, *b = bv.as.o;
    DType dt = (a->as.tensor.dt == DT_F64 || b->as.tensor.dt == DT_F64)
                   ? DT_F64 : DT_F32;
    uint8_t an = a->as.tensor.ndim, bn = b->as.tensor.ndim;
    uint8_t n = an > bn ? an : bn;
    int64_t out_dims[MAX_TDIM];
    for (uint8_t d = 0; d < n; d++) {
        int64_t da = d + an >= n ? a->as.tensor.dims[d + an - n] : 1;
        int64_t db = d + bn >= n ? b->as.tensor.dims[d + bn - n] : 1;
        int64_t o = da > db ? da : db;
        if (!(da == o || da == 1) || !(db == o || db == 1))
            rt_fatal("tensor shapes are not broadcastable");
        out_dims[d] = o;
    }
    Value out = tensor_new(dt, n, out_dims, 0.0);
    Obj *ro = out.as.o;
    size_t nel = (size_t)t_numel_of(n, out_dims);
    int64_t oidx[MAX_TDIM];
    for (size_t i = 0; i < nel; i++) {
        t_unravel_of(n, out_dims, i, oidx);
        size_t aoff = 0, boff = 0;
        for (uint8_t d = 0; d < n; d++) {
            bool ap = d + an >= n, bp = d + bn >= n;
            int64_t da = ap ? a->as.tensor.dims[d + an - n] : 1;
            int64_t db = bp ? b->as.tensor.dims[d + bn - n] : 1;
            int64_t ai = da == 1 ? 0 : oidx[d];
            int64_t bi = db == 1 ? 0 : oidx[d];
            int64_t as = ap ? a->as.tensor.strides[d + an - n] : 0;
            int64_t bs = bp ? b->as.tensor.strides[d + bn - n] : 0;
            aoff += (size_t)(ai * as);
            boff += (size_t)(bi * bs);
        }
        t_set(ro, i, f(t_get(a, aoff), t_get(b, boff)));
    }
    return out;
}

static Value t_binary(Value av, Value bv, double (*f)(double, double)) {
    if (is_tensor(av) && is_tensor(bv)) return t_binary_tt(av, bv, f);
    if (is_tensor(av) && is_num(bv)) return t_scalar_binary(av, bv, f, false);
    if (is_num(av) && is_tensor(bv)) return t_scalar_binary(bv, av, f, true);
    rt_fatal("unsupported tensor operands: %s and %s", type_name(av), type_name(bv));
    return em_none();
}

Value em_tensor_add(Value a, Value b) { return t_binary(a, b, op_add); }
Value em_tensor_sub(Value a, Value b) { return t_binary(a, b, op_sub); }
Value em_tensor_mul(Value a, Value b) { return t_binary(a, b, op_mul); }
Value em_tensor_div(Value a, Value b) { return t_binary(a, b, op_div); }

/* --- matmul ------------------------------------------------------------- */

Value em_tensor_matmul(Value av, Value bv) {
    Obj *a = av.as.o, *b = bv.as.o;
    DType dt = (a->as.tensor.dt == DT_F64 || b->as.tensor.dt == DT_F64)
                   ? DT_F64 : DT_F32;
    uint8_t an = a->as.tensor.ndim, bn = b->as.tensor.ndim;
    if ((an != 1 && an != 2) || (bn != 1 && bn != 2))
        rt_fatal("matmul supports 1-D or 2-D operands, got %d-D and %d-D", an, bn);

    bool avec = an == 1, bvec = bn == 1;
    int64_t am = avec ? 1 : a->as.tensor.dims[0];
    int64_t ak = avec ? a->as.tensor.dims[0] : a->as.tensor.dims[1];
    int64_t bk = bvec ? b->as.tensor.dims[0] : b->as.tensor.dims[0];
    int64_t bn_ = bvec ? 1 : b->as.tensor.dims[1];
    if (ak != bk)
        rt_fatal("matmul shapes do not align: [.., %" PRId64 "] vs [%" PRId64 ", ..]",
                 ak, bk);

    int64_t rdims[2];
    uint8_t rndim;
    if (avec && bvec) { rndim = 0; }
    else if (avec)    { rndim = 1; rdims[0] = bn_; }
    else if (bvec)    { rndim = 1; rdims[0] = am; }
    else              { rndim = 2; rdims[0] = am; rdims[1] = bn_; }

    Value out = tensor_new(dt, rndim, rdims, 0.0);
    Obj *ro = out.as.o;
    int64_t as0 = avec ? 0 : a->as.tensor.strides[0];
    int64_t as1 = avec ? a->as.tensor.strides[0] : a->as.tensor.strides[1];
    int64_t bs0 = bvec ? 0 : b->as.tensor.strides[0];
    int64_t bs1 = bvec ? b->as.tensor.strides[0] : b->as.tensor.strides[1];

    if (rndim == 0) {
        double s = 0;
        for (int64_t k = 0; k < ak; k++)
            s += t_get(a, (size_t)(k * as1)) * t_get(b, (size_t)(k * bs1));
        t_set(ro, 0, s);
        return out;
    }
    if (rndim == 1 && avec) {
        for (int64_t j = 0; j < bn_; j++) {
            double s = 0;
            for (int64_t k = 0; k < ak; k++)
                s += t_get(a, (size_t)(k * as1)) *
                     t_get(b, (size_t)(k * bs0 + j * bs1));
            t_set(ro, (size_t)j, s);
        }
        return out;
    }
    if (rndim == 1) {
        for (int64_t i = 0; i < am; i++) {
            double s = 0;
            for (int64_t k = 0; k < ak; k++)
                s += t_get(a, (size_t)(i * as0 + k * as1)) *
                     t_get(b, (size_t)(k * bs1));
            t_set(ro, (size_t)i, s);
        }
        return out;
    }
    for (int64_t i = 0; i < am; i++)
        for (int64_t j = 0; j < bn_; j++) {
            double s = 0;
            for (int64_t k = 0; k < ak; k++)
                s += t_get(a, (size_t)(i * as0 + k * as1)) *
                     t_get(b, (size_t)(k * bs0 + j * bs1));
            t_set(ro, (size_t)(i * bn_ + j), s);
        }
    return out;
}

/* --- reshape / transpose / permute / expand ----------------------------- */

Value em_tensor_reshape(Value tv, Value shape) {
    Obj *t = tv.as.o;
    /* reshape allows -1 for exactly one inferred dim, so it parses its own
     * shape list rather than going through parse_shape (which rejects -1). */
    if (!is_list(shape))
        rt_fatal("reshape() shape must be a list of ints");
    size_t n = shape.as.o->as.list.len;
    if (n == 0 || n > MAX_TDIM)
        rt_fatal("reshape() shape must have 1..%d dims", MAX_TDIM);
    uint8_t nndim = (uint8_t)n;
    int64_t *ndims = xmalloc(sizeof(int64_t) * n);
    for (size_t i = 0; i < n; i++) {
        Value d = shape.as.o->as.list.items[i];
        if (d.tag != V_INT || d.as.i < -1) {
            free(ndims);
            rt_fatal("reshape() dims must be ints >= -1");
        }
        ndims[i] = d.as.i;
    }
    int64_t srcnumel = t_numel_of(t->as.tensor.ndim, t->as.tensor.dims);
    int64_t prod = 1;
    int infer = -1;
    for (uint8_t d = 0; d < nndim; d++) {
        if (ndims[d] == -1) {
            if (infer >= 0) { free(ndims); rt_fatal("reshape: at most one -1 dim"); }
            infer = d;
        } else {
            prod *= ndims[d];
        }
    }
    if (infer >= 0) {
        if (prod == 0 || srcnumel % prod != 0) {
            free(ndims);
            rt_fatal("reshape: cannot infer the -1 dim");
        }
        ndims[infer] = srcnumel / prod;
        prod = srcnumel;
    }
    if (prod != srcnumel) {
        free(ndims);
        rt_fatal("reshape: total elements differ (%" PRId64 " vs %" PRId64 ")",
                 srcnumel, prod);
    }
    Value out = tensor_new(t->as.tensor.dt, nndim, ndims, 0.0);
    free(ndims);
    Obj *ro = out.as.o;
    size_t nel = (size_t)srcnumel;
    int64_t idx[MAX_TDIM];
    for (size_t i = 0; i < nel; i++) {
        t_unravel_of(t->as.tensor.ndim, t->as.tensor.dims, i, idx);
        size_t off = t_offset_of(t->as.tensor.ndim, t->as.tensor.strides, idx);
        t_set(ro, i, t_get(t, off));
    }
    return out;
}

Value em_tensor_transpose(Value tv) {
    Obj *t = tv.as.o;
    uint8_t ndim = t->as.tensor.ndim;
    int64_t ndims[MAX_TDIM], nstrides[MAX_TDIM];
    for (uint8_t d = 0; d < ndim; d++) {
        ndims[d] = t->as.tensor.dims[ndim - 1 - d];
        nstrides[d] = t->as.tensor.strides[ndim - 1 - d];
    }
    return tensor_view(t, t->as.tensor.dt, ndim, ndims, nstrides, 0);
}

Value em_tensor_permute(Value tv, Value perm) {
    Obj *t = tv.as.o;
    if (!is_list(perm))
        rt_fatal("permute() expects a list of axes");
    size_t n = perm.as.o->as.list.len;
    if (n != t->as.tensor.ndim)
        rt_fatal("permute() must list every axis (%zu axes, got %zu)",
                 (size_t)t->as.tensor.ndim, n);
    int64_t ndims[MAX_TDIM], nstrides[MAX_TDIM];
    bool seen[MAX_TDIM] = { false };
    for (size_t i = 0; i < n; i++) {
        Value p = perm.as.o->as.list.items[i];
        if (p.tag != V_INT || p.as.i < 0 || p.as.i >= (int64_t)n)
            rt_fatal("permute() axis out of range");
        int64_t ax = p.as.i;
        if (seen[ax]) rt_fatal("permute() repeats an axis");
        seen[ax] = true;
        ndims[i] = t->as.tensor.dims[ax];
        nstrides[i] = t->as.tensor.strides[ax];
    }
    return tensor_view(t, t->as.tensor.dt, (uint8_t)n, ndims, nstrides, 0);
}

/* broadcast a tensor to `shape` (a view; expanded dims have stride 0) */
Value em_tensor_expand(Value tv, Value shape) {
    Obj *t = tv.as.o;
    uint8_t nndim; int64_t *ndims;
    parse_shape(shape, &nndim, &ndims);
    if (nndim < t->as.tensor.ndim) {
        free(ndims);
        rt_fatal("expand() cannot drop dimensions");
    }
    int64_t nstrides[MAX_TDIM];
    for (uint8_t d = 0; d < nndim; d++) {
        bool present = d + t->as.tensor.ndim >= nndim;
        int64_t sdim = present ? t->as.tensor.dims[d + t->as.tensor.ndim - nndim] : 1;
        if (sdim != 1 && sdim != ndims[d]) {
            free(ndims);
            rt_fatal("expand(): cannot expand a dim of size %" PRId64 " to %" PRId64,
                     sdim, ndims[d]);
        }
        nstrides[d] = sdim == 1 ? 0
                                : t->as.tensor.strides[d + t->as.tensor.ndim - nndim];
    }
    Value v = tensor_view(t, t->as.tensor.dt, nndim, ndims, nstrides, 0);
    free(ndims);
    return v;
}

/* --- reductions (axis required; the axis is dropped) -------------------- */

static Value t_reduce(Value tv, Value axisv, int kind) {
    Obj *t = tv.as.o;
    if (axisv.tag != V_INT)
        rt_fatal("reduction axis must be an int");
    int64_t ax = axisv.as.i;
    if (ax < 0) ax += t->as.tensor.ndim;
    if (ax < 0 || ax >= t->as.tensor.ndim)
        rt_fatal("reduction axis %" PRId64 " out of range (rank %d)",
                 axisv.as.i, t->as.tensor.ndim);
    uint8_t ndim = t->as.tensor.ndim;
    int64_t rdims[MAX_TDIM];
    uint8_t rndim = 0;
    for (uint8_t d = 0; d < ndim; d++)
        if (d != (uint8_t)ax) rdims[rndim++] = t->as.tensor.dims[d];
    bool argmax = kind == 3;
    Value out = tensor_new(argmax ? DT_F32 : t->as.tensor.dt, rndim, rdims, 0.0);
    Obj *ro = out.as.o;
    size_t rnel = (size_t)t_numel_of(rndim, rdims);
    int64_t axlen = t->as.tensor.dims[ax];
    int64_t oidx[MAX_TDIM];
    for (size_t i = 0; i < rnel; i++) {
        t_unravel_of(rndim, rdims, i, oidx);
        size_t base = 0;
        uint8_t od = 0;
        for (uint8_t d = 0; d < ndim; d++) {
            if (d == (uint8_t)ax) continue;
            int64_t c = oidx[od++];
            base += (size_t)(c * t->as.tensor.strides[d]);
        }
        double acc = (kind == 2 || kind == 3) ? -INFINITY : 0.0;
        int64_t arg = 0;
        for (int64_t k = 0; k < axlen; k++) {
            double v = t_get(t, base + (size_t)(k * t->as.tensor.strides[ax]));
            if (kind == 2 || kind == 3) {
                if (v > acc) { acc = v; arg = k; }
            } else {
                acc += v;
            }
        }
        if (kind == 1) acc /= (double)axlen; /* mean */
        if (kind == 3) acc = (double)arg;    /* argmax -> index as f32 */
        t_set(ro, i, acc);
    }
    return out;
}

Value em_tensor_sum(Value t, Value axis)    { return t_reduce(t, axis, 0); }
Value em_tensor_mean(Value t, Value axis)   { return t_reduce(t, axis, 1); }
Value em_tensor_max(Value t, Value axis)    { return t_reduce(t, axis, 2); }
Value em_tensor_argmax(Value t, Value axis) { return t_reduce(t, axis, 3); }

/* --- slicing / scalar extraction / introspection ------------------------ */

Value em_tensor_slice(Value tv, Value axisv, Value lov, Value hiv) {
    Obj *t = tv.as.o;
    if (axisv.tag != V_INT || lov.tag != V_INT || hiv.tag != V_INT)
        rt_fatal("tensor slice axis and bounds must be ints");
    int64_t ax = axisv.as.i;
    if (ax < 0) ax += t->as.tensor.ndim;
    if (ax < 0 || ax >= t->as.tensor.ndim)
        rt_fatal("tensor slice axis out of range");
    int64_t n = t->as.tensor.dims[ax];
    int64_t lo = lov.as.i, hi = hiv.as.i;
    if (lo < 0) lo += n;
    if (hi < 0) hi += n;
    if (lo < 0) lo = 0;
    if (hi > n) hi = n;
    if (hi < lo) hi = lo;
    int64_t ndims[MAX_TDIM];
    memcpy(ndims, t->as.tensor.dims, sizeof(int64_t) * t->as.tensor.ndim);
    ndims[ax] = hi - lo;
    return tensor_view(t, t->as.tensor.dt, t->as.tensor.ndim, ndims,
                       t->as.tensor.strides, (size_t)(lo * t->as.tensor.strides[ax]));
}

Value em_tensor_item(Value tv) {
    Obj *t = tv.as.o;
    if (t_numel_of(t->as.tensor.ndim, t->as.tensor.dims) != 1)
        rt_fatal("item() requires a single-element tensor");
    return em_float(t_get(t, 0));
}

Value em_tensor_shape(Value tv) {
    Obj *t = tv.as.o;
    size_t n = t->as.tensor.ndim;
    Obj *o = list_new(n);
    for (uint8_t d = 0; d < n; d++)
        o->as.list.items[d] = em_int(t->as.tensor.dims[d]);
    o->as.list.len = n;
    return obj_val(o);
}

Value em_tensor_ndim(Value tv) {
    return em_int((int64_t)tv.as.o->as.tensor.ndim);
}

Value em_tensor_dtype(Value tv) {
    return em_str_new(tv.as.o->as.tensor.dt == DT_F64 ? "f64" : "f32");
}

Value em_tensor_astype(Value tv, Value dtyp) {
    Obj *t = tv.as.o;
    if (!is_str(dtyp))
        rt_fatal("astype() dtype must be a str");
    DType dt = parse_dtype(str_data(&dtyp));
    if (t->as.tensor.dt == dt) return tv;
    Value out = tensor_new(dt, t->as.tensor.ndim, t->as.tensor.dims, 0.0);
    Obj *ro = out.as.o;
    size_t nel = (size_t)t_numel_of(t->as.tensor.ndim, t->as.tensor.dims);
    int64_t idx[MAX_TDIM];
    for (size_t i = 0; i < nel; i++) {
        t_unravel_of(t->as.tensor.ndim, t->as.tensor.dims, i, idx);
        size_t off = t_offset_of(t->as.tensor.ndim, t->as.tensor.strides, idx);
        t_set(ro, i, t_get(t, off));
    }
    return out;
}
