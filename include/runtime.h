/* Emerald runtime: dynamic Value model + precise two-generation mark-and-sweep GC.
 *
 * Every Emerald value is a small tagged struct passed by value. Short strings
 * (<= 7 bytes) live inline in the Value; longer strings, lists, and records
 * live in GC-managed `Obj` nodes. New objects are born in a nursery and are
 * promoted to a tenured generation when they survive a minor collection; a
 * write barrier remembers tenured objects that reference the nursery. The
 * generated C
 * code keeps all live Values in per-function `Value F[n]` arrays that are
 * registered with the GC as "root frames" (a shadow stack), so collection is
 * precise: the GC never scans the C stack and never guesses.
 *
 * GC can only trigger inside rt_obj_new(), i.e. at allocation points. The
 * generated code guarantees that any Value that must survive an allocation is
 * sitting in a rooted frame slot at that moment.
 */
#ifndef EMERALD_RUNTIME_H
#define EMERALD_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { V_NONE, V_BOOL, V_INT, V_FLOAT, V_OBJ, V_STR } VTag;
typedef enum { O_STR, O_LIST, O_REC } OTag;

typedef struct Obj Obj;

typedef struct Value {
    VTag tag;
    union {
        bool b;
        int64_t i;
        double f;
        Obj *o;
        /* V_STR: small strings (<= 7 bytes, NUL-terminated) live inline in
         * the Value itself instead of a heap Obj. No allocation, no GC. */
        struct { char bytes[8]; } s;
    } as;
} Value;

struct Obj {
    OTag tag;
    bool mark;
    bool gen;         /* 0 = nursery, 1 = tenured */
    bool remembered;  /* tenured object that may reference the nursery */
    Obj *gc_next;     /* intrusive list: links objects within their generation */
    Obj *rem_next;    /* intrusive list: the remembered set */
    union {
        struct { size_t len; char *data; } str;              /* NUL-terminated */
        struct { size_t len, cap; Value *items; } list;
        struct { size_t len, cap; const char **keys; Value *vals; } rec;
    } as;
};

/* --- shadow stack ------------------------------------------------------- */

typedef struct RootFrame {
    Value *slots;
    size_t count;
    struct RootFrame *prev;
} RootFrame;

extern RootFrame *rt_roots;

static inline void rt_push_frame(RootFrame *f, Value *slots, size_t count) {
    f->slots = slots;
    f->count = count;
    f->prev = rt_roots;
    rt_roots = f;
}
static inline void rt_pop_frame(void) { rt_roots = rt_roots->prev; }

/* --- value constructors ------------------------------------------------- */

static inline Value em_none(void)      { Value v; v.tag = V_NONE; v.as.i = 0; return v; }
static inline Value em_bool(bool b)    { Value v; v.tag = V_BOOL; v.as.b = b; return v; }
static inline Value em_int(int64_t i)  { Value v; v.tag = V_INT; v.as.i = i; return v; }
static inline Value em_float(double f) { Value v; v.tag = V_FLOAT; v.as.f = f; return v; }

/* --- runtime services used by generated code ---------------------------- */

void rt_init(void);
void rt_gc_collect(void);
void rt_fatal(const char *fmt, ...);

Value em_str_new(const char *cstr);          /* copy of a C string */
Value em_list_litn(size_t n, ...);           /* n Values, already rooted by caller */
Value em_rec_litn(size_t n, ...);            /* n * (const char *key, Value val) */

/* operators */
Value em_add(Value a, Value b);
Value em_sub(Value a, Value b);
Value em_mul(Value a, Value b);
Value em_div(Value a, Value b);
Value em_mod(Value a, Value b);
Value em_neg(Value a);
Value em_eq(Value a, Value b);
Value em_ne(Value a, Value b);
Value em_lt(Value a, Value b);
Value em_le(Value a, Value b);
Value em_gt(Value a, Value b);
Value em_ge(Value a, Value b);
bool  em_truthy(Value v);

/* indexing / attributes */
Value em_index(Value seq, Value idx);
void  em_setindex(Value seq, Value idx, Value v);
Value em_getattr(Value rec, const char *name);
void  em_setattr(Value rec, const char *name, Value v);

/* `for x in seq`: fetch element i of a list/string; false when exhausted */
bool rt_iter_get(Value seq, int64_t i, Value *out);

/* builtins */
void  em_print(size_t n, ...);
Value em_len(Value v);
Value em_range(Value lo, Value hi);
Value em_str(Value v);
Value em_int_of(Value v);
Value em_gc_stats(void); /* record of GC counters for observability */

#endif
