/* Internal interface shared by the runtime implementation files.
 * Not part of the public API: include/runtime.h is. */
#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

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
#define NUM_BINOP(name, op, sym)                                              \
    Value name(Value a, Value b) {                                            \
        if (is_set(a) && is_set(b)) return set_binary(a, b, '-');              \
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
/* ---------------------------------------------------------------------- */
/* tensors                                                                */
/* ---------------------------------------------------------------------- */
#define MAX_TDIM 255

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

/* ---------------------------------------------------------------------- */
/* string building (plain malloc buffer; never triggers GC mid-build)      */
/* ---------------------------------------------------------------------- */
typedef struct { char *buf; size_t len, cap; } SB;

extern _Thread_local RootFrame *rt_roots;
extern uint64_t rng_state;
extern size_t gc_young_count;
extern size_t gc_old_count;
extern size_t gc_live;
extern size_t gc_young_bytes;
extern size_t gc_old_bytes;
extern size_t gc_young_threshold;
extern size_t gc_collections;
extern _Thread_local const char *rt_cur_file;
extern pthread_mutex_t sch_mu;

void rt_fatal(const char *fmt, ...);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void gc_mark_obj(Obj *o, bool minor);
void gc_mark_value(Value v, bool minor);
void gc_mark_roots(bool minor);
void obj_charge(Obj *o, size_t bytes);
void gc_write_barrier(Obj *container, Value v);
void rt_gc_collect(void);
Obj *rt_obj_new(OTag tag);
Value obj_val(Obj *o);
Value str_copy(const char *data, size_t len);
Value str_take(char *data, size_t len);
Value em_str_new(const char *cstr);
Obj *list_new(size_t n);
Value em_set_litn(size_t n, ...);
Value em_rec_litn(size_t n, ...);
const char *type_name(Value v);
bool is_str(Value v);
bool is_list(Value v);
bool is_tuple(Value v);
bool is_set(Value v);
bool is_rec(Value v);
bool is_num(Value v);
bool is_tensor(Value v);
const char *str_data(const Value *v);
size_t str_len(const Value *v);
double as_double(Value v);
int64_t as_int(Value v);
void sb_puts(SB *sb, const char *s);
void write_value(SB *sb, Value v, bool repr);
bool value_eq(Value a, Value b);
Value em_rshift(Value a, Value b);
bool em_truthy(Value v);
bool rt_iter_get(Value seq, int64_t i, Value *out);
void em_append(Value xs, Value v);
void em_dict_set(Value dict, Value key, Value value);
void em_set_add(Value set, Value value);
Value set_binary(Value a, Value b, char op);
Value em_mkclosure(Value (*fn)(Value *env, Value *args), size_t arity,
                   Value *env, size_t env_count, Value *defaults,
                   size_t default_count);
Value em_call(Value fn, size_t argc, ...);
Value call_closure_n(Value fn, Value *args, size_t n);
void sch_mark(bool minor);
void sch_init(void);
Value tensor_view(Obj *base, DType dt, uint8_t ndim, const int64_t *dims,
                         const int64_t *strides, size_t elem_offset);
Value em_tensor_add(Value a, Value b);
Value em_tensor_mul(Value a, Value b);
Value em_tensor_div(Value a, Value b);

#endif
