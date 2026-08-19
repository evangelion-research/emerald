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
typedef enum { O_STR, O_LIST, O_REC, O_FUNC, O_CELL, O_TENSOR } OTag;

/* Tensor dtypes. Only f32 and f64 are implemented in Phase 2; the other tags
 * are reserved so the tag width is settled before Phase 4 (quantized models)
 * needs them. */
typedef enum {
    DT_F32, DT_F64,
    DT_F16, DT_BF16, DT_I8, DT_I32,   /* reserved, not implemented */
    DT_COUNT,
} DType;

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
    size_t nbytes;    /* total bytes owned: the Obj header plus every backing
                       * array (string data, list items, record keys/vals,
                       * closure env, tensor data). Drives byte-aware GC: a
                       * single 400 MB tensor must trigger a collection even
                       * though it is one object. */
    union {
        struct { size_t len; char *data; } str;              /* NUL-terminated */
        struct { size_t len, cap; Value *items; } list;
        struct { size_t len, cap; const char **keys; Value *vals; } rec;
        /* O_FUNC: a first-class function. `fn` takes the captured env and a
         * packed array of `arity` arguments; `env` is NULL for top-level
         * functions with no captures. Captured variables are stored in O_CELL
         * boxes so that both the defining scope and every closure that
         * captures them share one mutable cell. */
        struct {
            Value (*fn)(Value *env, Value *args);
            Value *env;
            size_t env_count;
            size_t arity;
        } func;
        struct { Value val; } cell;   /* O_CELL: a mutable captured variable */
        /* O_TENSOR: a strided N-d array of floats. `data` is owned when
         * `base == NULL`; a view (slicing, transpose) keeps `base` non-NULL
         * and points into the owner's buffer, so the GC has one edge to trace
         * and views are zero-copy. */
        struct {
            DType dt;
            uint8_t ndim;
            int64_t *dims;      /* shape: ndim entries */
            int64_t *strides;   /* element strides: ndim entries */
            void *data;         /* numel * dtype_size bytes; NULL for a view */
            Obj *base;          /* owning tensor when this is a view */
        } tensor;
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
Value em_gc_collect(void);  /* force a major collection; returns None */

/* source location of the statement currently executing, for runtime errors */
extern const char *rt_cur_file;
extern int rt_cur_line;

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
void  em_setindex(Value seq, Value idx, Value v);Value em_getattr(Value rec, const char *name);
void em_setattr(Value rec, const char *name, Value v);
bool em_is_record(Value v); /* is `v` a record? (for match tests) */
bool em_rec_has(Value rec, const char *name); /* record has the field? (safe) */

/* `for x in seq`: fetch element i of a list/string; false when exhausted */
bool rt_iter_get(Value seq, int64_t i, Value *out);

/* builtins */
void  em_print(size_t n, ...);
Value em_len(Value v);
Value em_range(Value lo, Value hi);
Value em_str(Value v);
Value em_int_of(Value v);
Value em_sqrt(Value v);  /* square root; fatal on negative input */
Value em_tan(Value v);   /* tangent (radians) */
Value em_rand(void);     /* uniform float in [0, 1) */
Value em_gc_stats(void); /* record of GC counters for observability */
Value em_read_file(Value path);   /* contents of a file as a string */
void  em_write_file(Value path, Value content); /* write a string to a file */
void  em_append_file(Value path, Value content); /* append a string to a file */
Value em_run(Value cmd);          /* run a shell command; returns exit status */
Value em_read_file_opt(Value path); /* contents, or None if unreadable */
Value em_file_exists(Value path);   /* is the path openable for reading? */

/* stdlib foundation: the operations no Emerald code can express */
void  em_append(Value xs, Value v);  /* amortized in-place list growth */
Value em_slice(Value seq, Value lo, Value hi); /* str or list; clamped */
Value em_ord(Value c);               /* first byte of a string, 0..255 */
Value em_chr(Value n);               /* 0..255 as a one-byte string */
Value em_float_of(Value v);          /* float(), the counterpart of int() */
void  em_eprint(size_t n, ...);      /* print(), to stderr */
Value em_argv(void);                 /* the process's argument vector */
Value em_read_line(void);            /* a line from stdin, or None at EOF */
Value em_now(void);                  /* monotonic seconds; only diffs mean anything */
void  em_seed(Value n);              /* reseed the PRNG for reproducibility */
void  em_exit(Value code);           /* terminate with an exit status */
void  rt_set_args(int argc, char **argv); /* called by main() before anything */

/* first-class functions & closures */
Value em_mkclosure(Value (*fn)(Value *env, Value *args), size_t arity,
                   Value *env, size_t env_count);
Value em_call(Value fn, size_t argc, ...);
Value em_cell(Value v);        /* heap-box a value (mutable capture cell) */
Value em_cell_get(Value cell);
void  em_cell_set(Value cell, Value v);

/* higher-order list builtins (fn must be a closure of the right arity) */
Value em_map(Value fn, Value xs);
Value em_filter(Value fn, Value xs);
Value em_reduce(Value fn, Value acc, Value xs);

/* `f >> g`: a closure h(x) = g(f(x)) */
Value em_compose(Value f, Value g);

/* --- tensors (Phase 2 numerics) -----------------------------------------
 * Whole-array runtime calls over an unboxed float buffer (see tensors.md).
 * `shape` is always a list[int]; axes are 0-based ints. `+ - * /` on two
 * tensors dispatch here from the operator functions. */
Value em_tensor_zeros(Value shape);
Value em_tensor_ones(Value shape);
Value em_tensor_full(Value shape, Value fill);
Value em_tensor_arange(Value n);
Value em_tensor_from_list(Value nested);
Value em_tensor_randn(Value shape, Value seed);
Value em_tensor_exp(Value t);
Value em_tensor_log(Value t);
Value em_tensor_tanh(Value t);
Value em_tensor_relu(Value t);
Value em_tensor_add(Value a, Value b);
Value em_tensor_sub(Value a, Value b);
Value em_tensor_mul(Value a, Value b);
Value em_tensor_div(Value a, Value b);
Value em_tensor_matmul(Value a, Value b);
Value em_tensor_reshape(Value t, Value shape);
Value em_tensor_transpose(Value t);
Value em_tensor_permute(Value t, Value perm);
Value em_tensor_sum(Value t, Value axis);
Value em_tensor_mean(Value t, Value axis);
Value em_tensor_max(Value t, Value axis);
Value em_tensor_argmax(Value t, Value axis);
Value em_tensor_slice(Value t, Value axis, Value lo, Value hi);
Value em_tensor_expand(Value t, Value shape);
Value em_tensor_item(Value t);
Value em_tensor_shape(Value t);
Value em_tensor_ndim(Value t);
Value em_tensor_dtype(Value t);
Value em_tensor_astype(Value t, Value dtype);

#endif
