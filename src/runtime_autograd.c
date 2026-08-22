/* Runtime: reverse-mode automatic differentiation over tensors
 * (docs/autograd.md).
 *
 * A value_and_grad(f, x) call records one Tape while f runs: every
 * differentiable tensor primitive appends a node describing the operation,
 * its operands, and whatever metadata its backward rule needs. Recording
 * itself performs no allocation beyond the forward op's own output tensor,
 * so a program that never differentiates pays nothing but one branch.
 *
 * The backward pass walks the nodes in reverse creation order -- parents are
 * always recorded before their children, so this is a topological order of
 * the reversed graph -- accumulating each node's adjoint into its parents'
 * adjoint buffers and finally into the gradient leaf x. Adjoint buffers are
 * allocated lazily inside one rooted frame, and every tensor the tape
 * references is listed in its GC-traced `alive` array, so collection stays
 * precise throughout recording and backward.
 *
 * Gradient policy (documented in docs/autograd.md):
 *   - constants (scalars, tensor constructors, tensors built outside the
 *     recording) receive no gradient;
 *   - item(), argmax(), shape(), ndim(), dtype() and comparisons cut
 *     gradients: they are not recorded;
 *   - relu is differentiated with subgradient 0 at 0; max reductions place
 *     the whole gradient on the first maximal index (matching argmax);
 *   - mixed dtypes cast under an explicit rule: each adjoint carries the
 *     dtype of the tensor it belongs to.
 */
#include "runtime_internal.h"

/* --- recording state ----------------------------------------------------- */

static _Thread_local Tape *rec_tape = NULL; /* active tape, or NULL */
static _Thread_local Obj *rec_obj = NULL;   /* the O_TAPE object (for barriers) */

bool tape_recording(void) { return rec_tape != NULL; }

void tape_end(void) {
    rec_tape = NULL;
    rec_obj = NULL;
}

/* --- GC support ----------------------------------------------------------- */

void tape_mark(Tape *tp, bool minor) {
    if (!tp) return;
    for (size_t i = 0; i < tp->alen; i++)
        gc_mark_value(tp->alive[i], minor);
}

void tape_free(Tape *tp) {
    if (!tp) return;
    for (size_t i = 0; i < tp->len; i++) {
        free(tp->nodes[i].dims);
        free(tp->nodes[i].perm);
        free(tp->nodes[i].idxs);
    }
    free(tp->nodes);
    free(tp->alive);
    free(tp);
}

/* --- recording ------------------------------------------------------------ */

/* list a referenced Obj in the traced array; the write barrier keeps a
 * tenured tape pointing at nursery tensors collectable-correct */
static void alive_push(Tape *tp, Obj *tobj, Value v) {
    if (tp->alen == tp->acap) {
        tp->acap = tp->acap ? tp->acap * 2 : 16;
        tp->alive = xrealloc(tp->alive, sizeof(Value) * tp->acap);
    }
    tp->alive[tp->alen++] = v;
    gc_write_barrier(tobj, v);
}

/* which recorded node produced `o`, or -1 for a constant */
static int32_t find_parent(const Tape *tp, const Obj *o) {
    for (size_t i = tp->len; i > 0; i--)
        if (tp->nodes[i - 1].out == o) return (int32_t)(i - 1);
    return -1;
}

static void push_node(TapeNode n) {
    Tape *tp = rec_tape;
    if (!tp) return;
    if (tp->len == tp->cap) {
        tp->cap = tp->cap ? tp->cap * 2 : 32;
        tp->nodes = xrealloc(tp->nodes, sizeof(TapeNode) * tp->cap);
    }
    n.p0 = n.op0 ? find_parent(tp, n.op0) : -1;
    n.p1 = n.op1 ? find_parent(tp, n.op1) : -1;
    alive_push(tp, rec_obj, obj_val(n.out));
    if (n.op0) alive_push(tp, rec_obj, obj_val(n.op0));
    if (n.op1) alive_push(tp, rec_obj, obj_val(n.op1));
    tp->nodes[tp->len++] = n;
}

static int64_t *copy_i64(const int64_t *src, size_t n) {
    int64_t *dst = xmalloc(sizeof(int64_t) * (n ? n : 1));
    memcpy(dst, src, sizeof(int64_t) * n);
    return dst;
}

void tape_rec_binary(TapeKind k, Value out, Value a, Value b) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = k;
    bool at = is_tensor(a), bt = is_tensor(b);
    if (at && bt) {
        n.layout = TL_TT;
        n.op0 = a.as.o;
        n.op1 = b.as.o;
    } else if (at) { /* tensor OP scalar */
        n.layout = TL_TS;
        n.op0 = a.as.o;
        n.s1 = as_double(b);
    } else {         /* scalar OP tensor */
        n.layout = TL_ST;
        n.s0 = as_double(a);
        n.op1 = b.as.o;
    }
    n.out = out.as.o;
    push_node(n);
}

void tape_rec_unary(TapeKind k, Value out, Value a) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = k;
    n.layout = TL_TT;
    n.op0 = a.as.o;
    n.out = out.as.o;
    push_node(n);
}

void tape_rec_matmul(Value out, Value a, Value b) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.layout = TL_TT;
    n.op0 = a.as.o;
    n.op1 = b.as.o;
    n.out = out.as.o;
    uint8_t an = a.as.o->as.tensor.ndim, bn = b.as.o->as.tensor.ndim;
    n.kind = an == 1 ? (bn == 1 ? TB_MATMUL_VV : TB_MATMUL_VM)
                     : (bn == 1 ? TB_MATMUL_MV : TB_MATMUL_MM);
    push_node(n);
}

void tape_rec_reshape(Value out, Value src) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = TB_RESHAPE;
    n.op0 = src.as.o;
    n.out = out.as.o;
    uint8_t nd = src.as.o->as.tensor.ndim;
    n.dims = copy_i64(src.as.o->as.tensor.dims, nd);
    n.ndim = nd;
    push_node(n);
}

void tape_rec_transpose(Value out, Value src) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = TB_TRANSPOSE;
    n.op0 = src.as.o;
    n.out = out.as.o;
    push_node(n);
}

void tape_rec_permute(Value out, Value src, const int64_t *perm, uint8_t n) {
    if (!rec_tape) return;
    TapeNode nd;
    memset(&nd, 0, sizeof nd);
    nd.kind = TB_PERMUTE;
    nd.op0 = src.as.o;
    nd.out = out.as.o;
    nd.perm = copy_i64(perm, n);
    nd.ndim = n;
    push_node(nd);
}

void tape_rec_reduce(TapeKind k, Value out, Value src, int64_t axis) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = k;
    n.axis = axis;
    n.op0 = src.as.o;
    n.out = out.as.o;
    push_node(n);
}

void tape_rec_max(Value out, Value src, int64_t axis, const int64_t *idxs,
                  size_t nidx) {
    if (!rec_tape) {
        free((void *)idxs);
        return;
    }
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = TB_MAX;
    n.axis = axis;
    n.op0 = src.as.o;
    n.out = out.as.o;
    n.idxs = copy_i64(idxs, nidx);
    push_node(n);
}

void tape_rec_slice(Value out, Value src, int64_t axis, int64_t lo,
                    int64_t hi) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = TB_SLICE;
    n.axis = axis;
    n.lo = lo;
    n.hi = hi;
    n.op0 = src.as.o;
    n.out = out.as.o;
    push_node(n);
}

void tape_rec_expand(Value out, Value src) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = TB_EXPAND;
    n.op0 = src.as.o;
    n.out = out.as.o;
    push_node(n);
}

void tape_rec_cast(Value out, Value src) {
    if (!rec_tape) return;
    TapeNode n;
    memset(&n, 0, sizeof n);
    n.kind = TB_CAST;
    n.op0 = src.as.o;
    n.out = out.as.o;
    push_node(n);
}

/* --- backward kernels ------------------------------------------------------ */

static void ag_unravel(uint8_t ndim, const int64_t *dims, size_t flat,
                       int64_t *idx) {
    for (int d = (int)ndim - 1; d >= 0; d--) {
        idx[d] = (int64_t)(flat % (size_t)dims[d]);
        flat /= (size_t)dims[d];
    }
}

static size_t ag_offset(const Obj *t, const int64_t *idx) {
    size_t off = 0;
    for (uint8_t d = 0; d < t->as.tensor.ndim; d++)
        off += (size_t)(idx[d] * t->as.tensor.strides[d]);
    return off;
}

/* read `src` trailing-aligned against an index space of rank `tn`: absent
 * leading axes read position 0, size-1 axes always read position 0 */
static double bcast_get(const Obj *src, uint8_t tn, const int64_t *tidx) {
    uint8_t sn = src->as.tensor.ndim;
    int shift = (int)tn - (int)sn;
    size_t off = 0;
    for (uint8_t d = 0; d < sn; d++) {
        int64_t sd = src->as.tensor.dims[d];
        int64_t si = sd == 1 ? 0 : tidx[shift + d];
        off += (size_t)(si * src->as.tensor.strides[d]);
    }
    return tensor_elem_get(src, off);
}

typedef struct { uint8_t n; int64_t d[MAX_TDIM]; DType dt; } TSpec;

static void spec_of(const Obj *t, TSpec *s) {
    s->n = t->as.tensor.ndim;
    memcpy(s->d, t->as.tensor.dims, sizeof(int64_t) * s->n);
    s->dt = t->as.tensor.dt;
}

static Value like_filled(const Obj *t, double fill) {
    Value v = tensor_new_zeroed(t->as.tensor.dt, t->as.tensor.ndim,
                                t->as.tensor.dims);
    if (fill != 0.0) {
        size_t nel = (size_t)tensor_numel_of(t->as.tensor.ndim,
                                             t->as.tensor.dims);
        for (size_t i = 0; i < nel; i++) tensor_elem_set(v.as.o, i, fill);
    }
    return v;
}

/* dst += src; both are owned, contiguous, same shape and dtype (the kernels
 * build contributions in the target's shape and dtype by construction) */
static void accum_add(Obj *dst, const Obj *src) {
    if (dst->as.tensor.ndim != src->as.tensor.ndim ||
        dst->as.tensor.dt != src->as.tensor.dt ||
        memcmp(dst->as.tensor.dims, src->as.tensor.dims,
               sizeof(int64_t) * dst->as.tensor.ndim) != 0)
        rt_fatal("internal: adjoint shape mismatch in accumulation");
    size_t nel =
        (size_t)tensor_numel_of(dst->as.tensor.ndim, dst->as.tensor.dims);
    for (size_t i = 0; i < nel; i++)
        tensor_elem_set(dst, i,
                        tensor_elem_get(dst, i) + tensor_elem_get(src, i));
}

/* --- elementwise VJPs ------------------------------------------------------
 *
 * One kernel serves every broadcasting elementwise rule (arithmetic sides,
 * the unary nonlinearities, and expand): iterate the adjoint's cells, apply
 * the local derivative, and accumulate into a zeroed target-shaped buffer.
 * A cell of `g` whose axis was broadcast (source dim 1 under a larger target
 * dim) contributes to *every* corresponding target position; leading axes
 * present in g but absent in the target collapse by summation. */
typedef enum {
    G_ID,       /* c = v                                   add/sub side   */
    G_NEG,      /* c = -v                                  (s - t) side   */
    G_MUL_S,    /* c = v * s                               t * s          */
    G_DIV_S,    /* c = v / s                               t / s          */
    G_MUL_O,    /* c = v * o                               t * u side     */
    G_DIV_O,    /* c = v / o                               t / u side     */
    G_TANH_O,   /* c = v * (1 - o^2)                       tanh           */
    G_RELU_O,   /* c = o > 0 ? v : 0                       relu           */
    G_EXP_O,    /* c = v * o          (o = forward output) exp            */
    G_DIV_ST,   /* c = -(s * v) / o^2                      s / t side     */
    G_DIV_BB,   /* c = -(v * o1) / o2^2                    t / u other    */
} GradFn;

static double grad_apply(GradFn f, double v, double o1, double o2, double s) {
    switch (f) {
    case G_ID:     return v;
    case G_NEG:    return -v;
    case G_MUL_S:  return v * s;
    case G_DIV_S:  return v / s;
    case G_MUL_O:  return v * o1;
    case G_DIV_O:  return v / o1;
    case G_TANH_O: return v * (1.0 - o1 * o1);
    case G_RELU_O: return o1 > 0.0 ? v : 0.0;
    case G_EXP_O:  return v * o1;
    case G_DIV_ST: return -(s * v) / (o1 * o1);
    case G_DIV_BB: return -(v * o1) / (o2 * o2);
    }
    return v;
}

static Value grad_kernel(const Obj *g, const Obj *o1s, const Obj *o2s,
                         GradFn fn, double s, uint8_t tn,
                         const int64_t *tdims, DType T) {
    Value outv = tensor_new_zeroed(T, tn, tdims);
    Obj *out = outv.as.o;
    uint8_t gn = g->as.tensor.ndim;
    int shift = (int)gn - (int)tn;
    /* axes the target expanded away: source dim 1 under target dim > 1 --
     * one g cell fans out to ext[k] target positions along that axis */
    uint8_t nexp = 0;
    int64_t exp_d[MAX_TDIM], ext[MAX_TDIM];
    for (uint8_t d = 0; d < tn; d++) {
        int64_t j = shift + d;
        int64_t gd = j >= 0 ? g->as.tensor.dims[j] : 1;
        if (gd == 1 && tdims[d] > 1) {
            exp_d[nexp] = d;
            ext[nexp] = tdims[d];
            nexp++;
        }
    }
    int64_t gi[MAX_TDIM], ti[MAX_TDIM];
    size_t gn_ = (size_t)tensor_numel_of(gn, g->as.tensor.dims);
    size_t tnel = (size_t)tensor_numel_of(tn, tdims);
    for (size_t i = 0; i < gn_; i++) {
        ag_unravel(gn, g->as.tensor.dims, i, gi);
        double v = tensor_elem_get(g, ag_offset(g, gi));
        double o1 = 0.0, o2 = 0.0;
        if (o1s) o1 = bcast_get(o1s, gn, gi);
        if (o2s) o2 = bcast_get(o2s, gn, gi);
        double c = grad_apply(fn, v, o1, o2, s);
        /* base target position: an axis the target collapsed to 1 sums all
         * of this axis's contributions into position 0 */
        for (uint8_t d = 0; d < tn; d++) {
            int64_t j = shift + d;
            ti[d] = tdims[d] == 1 ? 0 : gi[j];
        }
        if (nexp == 0) {
            size_t tf = 0;
            for (uint8_t d = 0; d < tn; d++)
                tf = tf * (size_t)tdims[d] + (size_t)ti[d];
            tensor_elem_set(out, tf, tensor_elem_get(out, tf) + c);
        } else {
            /* fan out across every combination on the expanded axes */
            int64_t cnt[MAX_TDIM];
            memset(cnt, 0, sizeof(cnt));
            for (;;) {
                for (uint8_t k = 0; k < nexp; k++) ti[exp_d[k]] = cnt[k];
                size_t tf = 0;
                for (uint8_t d = 0; d < tn; d++)
                    tf = tf * (size_t)tdims[d] + (size_t)ti[d];
                if (tf < tnel)
                    tensor_elem_set(out, tf, tensor_elem_get(out, tf) + c);
                uint8_t k = 0;
                for (; k < nexp; k++) {
                    if (++cnt[k] < ext[k]) break;
                    cnt[k] = 0;
                }
                if (k == nexp) break; /* all combinations emitted */
            }
        }
    }
    return outv;
}

/* --- matmul VJPs -----------------------------------------------------------
 * Operands may be strided views, so reads go through logical accessors.
 * Outputs are fresh contiguous buffers shaped like the operand. */
static double rd1(const Obj *t, int64_t i) {
    return tensor_elem_get(t, (size_t)(i * t->as.tensor.strides[0]));
}

static double rd2(const Obj *t, int64_t i, int64_t j) {
    return tensor_elem_get(
        t, (size_t)(i * t->as.tensor.strides[0] + j * t->as.tensor.strides[1]));
}

static void wr1(Obj *t, int64_t i, double v) {
    tensor_elem_set(t, (size_t)(i * t->as.tensor.strides[0]), v);
}

static void wr2(Obj *t, int64_t i, int64_t j, double v) {
    tensor_elem_set(
        t, (size_t)(i * t->as.tensor.strides[0] + j * t->as.tensor.strides[1]),
        v);
}

/* gradient flowing to the first operand of matmul */
static Value matmul_grad_a(const TapeNode *nd, const Obj *gy) {
    const Obj *a = nd->op0, *b = nd->op1;
    switch (nd->kind) {
    case TB_MATMUL_VV: { /* y = a.b : ga_k += gy * b_k */
        int64_t K = a->as.tensor.dims[0];
        Value out = like_filled(a, 0.0);
        double s = tensor_elem_get(gy, 0);
        for (int64_t k = 0; k < K; k++)
            wr1(out.as.o, k, s * rd1(b, k));
        return out;
    }
    case TB_MATMUL_VM: { /* y_j = sum_k a_k B_kj : ga_k = sum_j gy_j B_kj */
        int64_t K = a->as.tensor.dims[0], N = b->as.tensor.dims[1];
        Value out = like_filled(a, 0.0);
        for (int64_t k = 0; k < K; k++) {
            double acc = 0;
            for (int64_t j = 0; j < N; j++)
                acc += rd1(gy, j) * rd2(b, k, j);
            wr1(out.as.o, k, acc);
        }
        return out;
    }
    case TB_MATMUL_MV: { /* y_i = sum_k A_ik b_k : gA_ik += gy_i b_k */
        int64_t M = a->as.tensor.dims[0], K = a->as.tensor.dims[1];
        Value out = like_filled(a, 0.0);
        for (int64_t i = 0; i < M; i++)
            for (int64_t k = 0; k < K; k++)
                wr2(out.as.o, i, k, rd1(gy, i) * rd1(b, k));
        return out;
    }
    default: { /* Y = A.B : gA = gy . B^T */
        int64_t M = a->as.tensor.dims[0], K = a->as.tensor.dims[1],
                N = b->as.tensor.dims[1];
        Value out = like_filled(a, 0.0);
        for (int64_t i = 0; i < M; i++)
            for (int64_t k = 0; k < K; k++) {
                double acc = 0;
                for (int64_t j = 0; j < N; j++)
                    acc += rd2(gy, i, j) * rd2(b, k, j);
                wr2(out.as.o, i, k, acc);
            }
        return out;
    }
    }
}

/* gradient flowing to the second operand of matmul */
static Value matmul_grad_b(const TapeNode *nd, const Obj *gy) {
    const Obj *a = nd->op0, *b = nd->op1;
    switch (nd->kind) {
    case TB_MATMUL_VV: { /* gb_k += gy * a_k */
        int64_t K = b->as.tensor.dims[0];
        Value out = like_filled(b, 0.0);
        double s = tensor_elem_get(gy, 0);
        for (int64_t k = 0; k < K; k++)
            wr1(out.as.o, k, s * rd1(a, k));
        return out;
    }
    case TB_MATMUL_VM: { /* gB_kj += a_k * gy_j */
        int64_t K = b->as.tensor.dims[0], N = b->as.tensor.dims[1];
        Value out = like_filled(b, 0.0);
        for (int64_t k = 0; k < K; k++)
            for (int64_t j = 0; j < N; j++)
                wr2(out.as.o, k, j, rd1(a, k) * rd1(gy, j));
        return out;
    }
    case TB_MATMUL_MV: { /* gb_k = sum_i gy_i A_ik */
        int64_t M = a->as.tensor.dims[0], K = a->as.tensor.dims[1];
        Value out = like_filled(b, 0.0);
        for (int64_t k = 0; k < K; k++) {
            double acc = 0;
            for (int64_t i = 0; i < M; i++)
                acc += rd1(gy, i) * rd2(a, i, k);
            wr1(out.as.o, k, acc);
        }
        return out;
    }
    default: { /* gB = A^T . gy */
        int64_t M = a->as.tensor.dims[0], K = a->as.tensor.dims[1],
                N = b->as.tensor.dims[1];
        Value out = like_filled(b, 0.0);
        for (int64_t k = 0; k < K; k++)
            for (int64_t j = 0; j < N; j++) {
                double acc = 0;
                for (int64_t i = 0; i < M; i++)
                    acc += rd2(a, i, k) * rd2(gy, i, j);
                wr2(out.as.o, k, j, acc);
            }
        return out;
    }
    }
}

/* --- shape-op VJPs --------------------------------------------------------- */

/* reshape copies preserve logical element order, so the adjoint copies back
 * in linear order regardless of strides on either side */
static Value reshape_grad(const TapeNode *nd, const Obj *g) {
    Value out = tensor_new_zeroed(g->as.tensor.dt, nd->ndim, nd->dims);
    uint8_t gn = g->as.tensor.ndim;
    size_t nel = (size_t)tensor_numel_of(gn, g->as.tensor.dims);
    int64_t gi[MAX_TDIM];
    for (size_t i = 0; i < nel; i++) {
        ag_unravel(gn, g->as.tensor.dims, i, gi);
        tensor_elem_set(out.as.o, i, tensor_elem_get(g, ag_offset(g, gi)));
    }
    return out;
}

static Value transpose_grad(const TapeNode *nd, const Obj *g) {
    const Obj *a = nd->op0;
    uint8_t n = a->as.tensor.ndim;
    Value out = like_filled(a, 0.0);
    int64_t oi[MAX_TDIM], ii[MAX_TDIM];
    size_t nel = (size_t)tensor_numel_of(n, g->as.tensor.dims);
    for (size_t i = 0; i < nel; i++) {
        ag_unravel(n, g->as.tensor.dims, i, oi);
        for (uint8_t d = 0; d < n; d++) ii[d] = oi[n - 1 - d]; /* full reverse */
        size_t off = ag_offset(a, ii);
        tensor_elem_set(out.as.o, off,
                        tensor_elem_get(out.as.o, off) +
                            tensor_elem_get(g, ag_offset(g, oi)));
    }
    return out;
}

static Value permute_grad(const TapeNode *nd, const Obj *g) {
    const Obj *a = nd->op0;
    uint8_t n = a->as.tensor.ndim;
    int64_t inv[MAX_TDIM];
    for (uint8_t k = 0; k < n; k++) inv[nd->perm[k]] = k;
    Value out = like_filled(a, 0.0);
    int64_t oi[MAX_TDIM], ii[MAX_TDIM];
    size_t nel = (size_t)tensor_numel_of(n, g->as.tensor.dims);
    for (size_t i = 0; i < nel; i++) {
        ag_unravel(n, g->as.tensor.dims, i, oi);
        for (uint8_t d = 0; d < n; d++) ii[d] = oi[inv[d]];
        size_t off = ag_offset(a, ii);
        tensor_elem_set(out.as.o, off,
                        tensor_elem_get(out.as.o, off) +
                            tensor_elem_get(g, ag_offset(g, oi)));
    }
    return out;
}

/* sum / mean: broadcast the reduced adjoint back across `axis`; max:
 * place each cell's gradient at the saved first-maximal index */
static Value reduce_grad(const TapeNode *nd, const Obj *g) {
    const Obj *a = nd->op0;
    uint8_t n = a->as.tensor.ndim;
    int64_t ax = nd->axis;
    Value out = like_filled(a, 0.0);
    Obj *ro = out.as.o;
    int64_t axlen = a->as.tensor.dims[ax];
    if (nd->kind == TB_MAX) {
        int64_t ri[MAX_TDIM], fi[MAX_TDIM];
        /* iterate the incoming gradient's cells (the reduced shape) */
        uint8_t gn = g->as.tensor.ndim;
        size_t rnel = (size_t)tensor_numel_of(gn, g->as.tensor.dims);
        for (size_t i = 0; i < rnel; i++) {
            ag_unravel(gn, g->as.tensor.dims, i, ri);
            uint8_t od = 0;
            for (uint8_t d = 0; d < n; d++)
                fi[d] = d == ax ? nd->idxs[i] : ri[od++];
            size_t off = ag_offset(a, fi);
            tensor_elem_set(ro, off,
                            tensor_elem_get(ro, off) +
                                tensor_elem_get(g, ag_offset(g, ri)));
        }
        return out;
    }
    int64_t ii[MAX_TDIM], ri[MAX_TDIM];
    size_t inel = (size_t)tensor_numel_of(n, a->as.tensor.dims);
    for (size_t i = 0; i < inel; i++) {
        ag_unravel(n, a->as.tensor.dims, i, ii);
        uint8_t od = 0;
        for (uint8_t d = 0; d < n; d++)
            if (d != ax) ri[od++] = ii[d];
        double v = tensor_elem_get(g, ag_offset(g, ri));
        if (nd->kind == TB_MEAN) v /= (double)axlen;
        tensor_elem_set(ro, i, v);
    }
    return out;
}

static Value slice_grad(const TapeNode *nd, const Obj *g) {
    const Obj *a = nd->op0;
    uint8_t n = a->as.tensor.ndim;
    Value out = like_filled(a, 0.0);
    Obj *ro = out.as.o;
    int64_t oi[MAX_TDIM], fi[MAX_TDIM];
    size_t rnel = (size_t)tensor_numel_of(n, g->as.tensor.dims);
    for (size_t i = 0; i < rnel; i++) {
        ag_unravel(n, g->as.tensor.dims, i, oi);
        for (uint8_t d = 0; d < n; d++) fi[d] = oi[d];
        fi[nd->axis] = oi[nd->axis] + nd->lo;
        size_t off = ag_offset(a, fi);
        tensor_elem_set(ro, off,
                        tensor_elem_get(ro, off) +
                            tensor_elem_get(g, ag_offset(g, oi)));
    }
    return out;
}

/* --- backward driver -------------------------------------------------------- */

/* route a contribution to its destination: the parent node's adjoint, or the
 * leaf buffer when the operand is the differentiation variable. Constants
 * drop their gradient. First touch adopts the contribution buffer directly
 * (it already carries the destination's shape and dtype). */
static void deliver(Value *adj, size_t leaf_slot, Obj *leaf,
                    int32_t p, Obj *pop, Value c) {
    if (c.tag != V_OBJ) return;
    if (p >= 0) {
        if (adj[p].tag != V_OBJ) {
            adj[p] = c;
            return;
        }
        accum_add(adj[p].as.o, c.as.o);
    } else if (pop && pop == leaf) {
        if (adj[leaf_slot].tag != V_OBJ) {
            adj[leaf_slot] = c;
            return;
        }
        accum_add(adj[leaf_slot].as.o, c.as.o);
    }
}

static Value tape_backward(Tape *tp, Obj *tobj, Obj *leaf, Obj *y) {
    size_t N = tp->len;
    size_t XSLOT = N; /* adj[N]: the leaf's accumulator */
    Obj *x = leaf;    /* alias for readability below */
    int32_t seed = -1;
    for (size_t i = 0; i < N; i++)
        if (tp->nodes[i].out == y) seed = (int32_t)i;

    Value *adj = xmalloc(sizeof(Value) * (N + 1));
    for (size_t i = 0; i <= N; i++) adj[i] = em_none();
    RootFrame bf;
    rt_push_frame(&bf, adj, N + 1);

    if (seed < 0) {
        /* no recorded primitive produced the output: either f returned x
         * unchanged (identity: gradient ones) or something unrelated
         * (constant: gradient zeros) */
        adj[XSLOT] = y == x ? like_filled(x, 1.0) : like_filled(x, 0.0);
        Value grad = adj[XSLOT];
        alive_push(tp, tobj, grad);
        rt_pop_frame();
        free(adj);
        return grad;
    }

    adj[seed] = like_filled(y, 1.0);

    for (int64_t i = (int64_t)N - 1; i >= 0; i--) {
        TapeNode *nd = &tp->nodes[i];
        if (adj[i].tag != V_OBJ) continue; /* never received gradient */
        Obj *g = adj[i].as.o;

        switch (nd->kind) {
        case TB_ADD: case TB_SUB: case TB_MUL: case TB_DIV: {
            /* operand specs depend on the layout: a scalar operand has no
             * tensor and therefore no gradient path */
            TSpec sa, sb;
            bool want0 = false, want1 = false;
            if (nd->layout == TL_TT) {
                spec_of(nd->op0, &sa);
                spec_of(nd->op1, &sb);
                want0 = nd->p0 >= 0 || nd->op0 == leaf;
                want1 = nd->p1 >= 0 || nd->op1 == leaf;
            } else if (nd->layout == TL_TS) {
                spec_of(nd->op0, &sa);
                want0 = nd->p0 >= 0 || nd->op0 == leaf;
                want1 = false;
            } else { /* TL_ST */
                spec_of(nd->op1, &sb);
                want0 = false;
                want1 = nd->p1 >= 0 || nd->op1 == leaf;
            }
            if (want0) {
                Value c0;
                switch (nd->kind) {
                case TB_MUL:
                    c0 = nd->layout == TL_TS
                             ? grad_kernel(g, NULL, NULL, G_MUL_S, nd->s1, sa.n, sa.d, sa.dt)
                             : grad_kernel(g, nd->op1, NULL, G_MUL_O, 0, sa.n, sa.d, sa.dt);
                    break;
                case TB_DIV:
                    c0 = nd->layout == TL_TS
                             ? grad_kernel(g, NULL, NULL, G_DIV_S, nd->s1, sa.n, sa.d, sa.dt)
                             : grad_kernel(g, nd->op1, NULL, G_DIV_O, 0, sa.n, sa.d, sa.dt);
                    break;
                default: /* ADD and SUB: d(a±b)/da = +1 */
                    c0 = grad_kernel(g, NULL, NULL, G_ID, 0, sa.n, sa.d, sa.dt);
                }
                deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c0);
            }
            if (want1) {
                Value c1;
                switch (nd->kind) {
                case TB_ADD:
                    c1 = grad_kernel(g, NULL, NULL, G_ID, 0, sb.n, sb.d, sb.dt);
                    break;
                case TB_SUB:
                    c1 = grad_kernel(g, NULL, NULL, G_NEG, 0, sb.n, sb.d, sb.dt);
                    break;
                case TB_MUL:
                    /* TT: d(a*b)/db = a -- ST: d(s*t)/dt = s */
                    c1 = nd->layout == TL_TT
                             ? grad_kernel(g, nd->op0, NULL, G_MUL_O, 0, sb.n, sb.d, sb.dt)
                             : grad_kernel(g, NULL, NULL, G_MUL_S, nd->s0, sb.n, sb.d, sb.dt);
                    break;
                default: /* TB_DIV */
                    c1 = nd->layout == TL_TT
                             ? grad_kernel(g, nd->op0, nd->op1, G_DIV_BB, 0, sb.n, sb.d, sb.dt)
                             : grad_kernel(g, nd->op1, NULL, G_DIV_ST, nd->s0, sb.n, sb.d, sb.dt);
                }
                deliver(adj, XSLOT, leaf, nd->p1, nd->op1, c1);
            }
            break;
        }
        case TB_EXP:
        case TB_LOG:
        case TB_TANH:
        case TB_RELU: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            TSpec sa;
            spec_of(nd->op0, &sa);
            const Obj *oth = NULL;
            GradFn fn = G_ID;
            switch (nd->kind) {
            case TB_EXP:  oth = nd->out; fn = G_EXP_O; break;
            case TB_LOG:  oth = nd->op0; fn = G_DIV_O; break;
            case TB_TANH: oth = nd->out; fn = G_TANH_O; break;
            default:      oth = nd->op0; fn = G_RELU_O; break;
            }
            Value c = grad_kernel(g, oth, NULL, fn, 0, sa.n, sa.d, sa.dt);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        case TB_MATMUL_VV: case TB_MATMUL_VM:
        case TB_MATMUL_MV: case TB_MATMUL_MM: {
            if (nd->p0 >= 0 || nd->op0 == leaf) {
                Value ca = matmul_grad_a(nd, g);
                deliver(adj, XSLOT, leaf, nd->p0, nd->op0, ca);
            }
            if (nd->p1 >= 0 || nd->op1 == leaf) {
                Value cb = matmul_grad_b(nd, g);
                deliver(adj, XSLOT, leaf, nd->p1, nd->op1, cb);
            }
            break;
        }
        case TB_RESHAPE: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            Value c = reshape_grad(nd, g);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        case TB_TRANSPOSE: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            Value c = transpose_grad(nd, g);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        case TB_PERMUTE: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            Value c = permute_grad(nd, g);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        case TB_SUM: case TB_MEAN: case TB_MAX: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            Value c = reduce_grad(nd, g);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        case TB_SLICE: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            Value c = slice_grad(nd, g);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        case TB_EXPAND: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            TSpec sa;
            spec_of(nd->op0, &sa);
            Value c = grad_kernel(g, NULL, NULL, G_ID, 0, sa.n, sa.d, sa.dt);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        case TB_CAST: {
            if (nd->p0 < 0 && nd->op0 != leaf) break;
            TSpec sa;
            spec_of(nd->op0, &sa);
            Value c = grad_kernel(g, NULL, NULL, G_ID, 0, sa.n, sa.d, sa.dt);
            deliver(adj, XSLOT, leaf, nd->p0, nd->op0, c);
            break;
        }
        }
    }

    if (adj[XSLOT].tag != V_OBJ) adj[XSLOT] = like_filled(x, 0.0);
    Value grad = adj[XSLOT];
    /* the record built after backward allocates once more: keep the gradient
     * reachable through the tape until then */
    alive_push(tp, tobj, grad);
    rt_pop_frame();
    free(adj);
    return grad;
}

/* --- the public entry point -------------------------------------------------- */

Value em_value_and_grad(Value fv, Value xv) {
    if (!(fv.tag == V_OBJ && fv.as.o->tag == O_FUNC))
        rt_fatal("value_and_grad(): first argument must be a function");
    if (!is_tensor(xv))
        rt_fatal("value_and_grad(): second argument must be a tensor");

    Obj *tobj = rt_obj_new(O_TAPE);
    Tape *tp = xmalloc(sizeof *tp);
    memset(tp, 0, sizeof *tp);
    tobj->as.tape = tp;
    Value tapev = obj_val(tobj);

    RootFrame fr;
    Value roots[3] = { tapev, fv, xv };
    rt_push_frame(&fr, roots, 3);

    rec_tape = tp;
    rec_obj = tobj;
    alive_push(tp, tobj, xv);

    Value out = call_closure_n(fv, &xv, 1);

    tape_end(); /* stop recording before anything else can run primitives */

    if (!is_tensor(out)) {
        rt_pop_frame();
        rt_fatal("value_and_grad(): the function must return a tensor");
    }
    alive_push(tp, tobj, out);

    Value grad = tape_backward(tp, tobj, xv.as.o, out.as.o);

    Value res = em_rec_litn(2, "value", out, "grad", grad);
    rt_pop_frame();
    return res;
}
