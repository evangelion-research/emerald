/* Runtime: tensors -- construction, elementwise ops, matmul, reshaping,
 * reductions, and slicing over an unboxed float buffer. */
#include "runtime_internal.h"

static size_t dt_size(DType dt) {
    return dt == DT_F64 ? sizeof(double) : sizeof(float);
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
    rt_fatal("unsupported dtype '%s' (v1 supports f32 and f64)", s);
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
Value tensor_view(Obj *base, DType dt, uint8_t ndim, const int64_t *dims,
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
static Value tensor_from_shape(Value shape, double fill) {
    uint8_t ndim;
    int64_t *dims;
    parse_shape(shape, &ndim, &dims);
    Value t = tensor_new(DT_F32, ndim, dims, fill);
    free(dims);
    return t;
}

Value em_tensor_zeros(Value shape) { return tensor_from_shape(shape, 0.0); }

Value em_tensor_ones(Value shape) { return tensor_from_shape(shape, 1.0); }

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
