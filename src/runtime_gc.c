/* Runtime: the precise two-generation mark-and-sweep collector and its root
 * frames (see docs/gc.md). */
#include "runtime_internal.h"

_Thread_local RootFrame *rt_roots = NULL;

/* xorshift64* PRNG; seeded from the clock at startup */
uint64_t rng_state = 88172645463325252ULL;

static Obj *gc_young = NULL;       /* nursery: allocated since the last minor GC */

static Obj *gc_old = NULL;         /* tenured survivors */

static Obj *gc_remembered = NULL;  /* tenured objects that may point into the nursery */

size_t gc_young_count = 0;

size_t gc_old_count = 0;

size_t gc_live = 0;         /* objects surviving the last collection */

size_t gc_young_bytes = 0;  /* bytes live in the nursery */

size_t gc_old_bytes = 0;    /* bytes live in the tenured generation */

size_t gc_young_threshold = 256;

static size_t gc_old_threshold = 256;

static size_t gc_young_bytes_threshold = 4u << 20;   /* 4 MiB: collect large buffers */

static size_t gc_old_bytes_threshold = 4u << 20;

size_t gc_collections = 0;  /* total cycles (minor + major) */

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

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) rt_fatal("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) rt_fatal("out of memory");
    return q;
}

void gc_mark_obj(Obj *o, bool minor) {
    if (o == NULL || o->mark) return;
    /* In a minor collection the tenured generation is opaque: only objects in
     * the remembered set (which may point into the nursery) are walked. */
    if (minor && o->gen == GEN_OLD && !o->remembered) return;
    o->mark = true;
    switch (o->tag) {
    case O_STR:
        break;
    case O_LIST:
    case O_TUPLE:
        for (size_t i = 0; i < o->as.list.len; i++)
            gc_mark_value(o->as.list.items[i], minor);
        break;
    case O_REC:
        for (size_t i = 0; i < o->as.rec.len; i++)
            gc_mark_value(o->as.rec.vals[i], minor);
        break;
    case O_DICT:
        for (size_t i = 0; i < o->as.dict.len; i++) {
            gc_mark_value(o->as.dict.keys[i], minor);
            gc_mark_value(o->as.dict.vals[i], minor);
        }
        break;
    case O_SET:
        for (size_t i = 0; i < o->as.set.len; i++)
            gc_mark_value(o->as.set.items[i], minor);
        break;
    case O_FUNC:
        for (size_t i = 0; i < o->as.func.env_count; i++) gc_mark_value(o->as.func.env[i], minor);
        for (size_t i = 0; i < o->as.func.default_count; i++) gc_mark_value(o->as.func.defaults[i], minor);
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

void gc_mark_value(Value v, bool minor) {
    if (v.tag == V_OBJ) gc_mark_obj(v.as.o, minor);
}

void gc_mark_roots(bool minor) {
    sch_mark(minor);
}

static void gc_free_obj(Obj *o) {
    switch (o->tag) {
    case O_STR:  free(o->as.str.data); break;
    case O_LIST: case O_TUPLE: free(o->as.list.items); break;
    case O_REC:  free(o->as.rec.keys); free(o->as.rec.vals); break;
    case O_DICT: free(o->as.dict.keys); free(o->as.dict.vals); break;
    case O_SET:  free(o->as.set.items); break;
    case O_FUNC: free(o->as.func.env); free(o->as.func.defaults); break;
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
void obj_charge(Obj *o, size_t bytes) {
    o->nbytes += bytes;
    if (o->gen == GEN_YOUNG) gc_young_bytes += bytes;
    else gc_old_bytes += bytes;
}

/* Write barrier: remember a tenured container that is made to reference a
 * nursery object, so the next minor collection knows to look through it. */
void gc_write_barrier(Obj *container, Value v) {
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

Obj *rt_obj_new(OTag tag) {
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
Value obj_val(Obj *o) { Value v; v.tag = V_OBJ; v.as.o = o; return v; }
