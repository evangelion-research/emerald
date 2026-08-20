# Tensors

Tensors make Emerald a numeric language. This page documents the **runtime**
half — how tensors exist, run, and interact with the GC. The **type-level** half
(shapes, dimensions, the solver, the typing rules) is in
[`shapes.md`](shapes.md).

## The one decision that makes it work

Tensor operations are **whole-array runtime calls**, not per-element generated
code. `matmul(a, b)` lowers to a single `em_tensor_matmul()`
call; the loop nest lives in `src/runtime_*.c` over an unboxed `float *`. The
`Value` boxing cost is therefore paid once per *operation*, not once per
*element*.

That is what lets the **untyped codegen survive numerics**: `src/codegen_*.c` still
never mentions a `Type`, and a tensor is just another `Value` holding an `Obj`.
No monomorphization is added — none is needed.

## The object model

`O_TENSOR` extends the GC-managed `Obj` union (`include/runtime.h`):

```c
struct {
    DType dt;              /* f32 or f64 in this phase */
    uint8_t ndim;
    int64_t *dims;         /* shape: ndim entries */
    int64_t *strides;      /* element strides: ndim entries */
    void *data;            /* numel * dtype_size bytes; NULL for a view */
    Obj *base;             /* owning tensor when this is a view */
} tensor;
```

A tensor is a **strided N-d array of floats**. `data` is owned when `base ==
NULL`; a view — produced by slicing or `transpose`/`permute` — keeps `base`
non-NULL and points into the owner's buffer. Two consequences:

- Slicing and transpose are **zero-copy**: they build a new descriptor over the
  same backing store.
- The GC has a single edge to trace: a view roots only its `base`, never a
  separate buffer.

### dtypes

| Tag | Status |
|---|---|
| `f32` | implemented |
| `f64` | implemented |
| `f16`, `bf16`, `i8`, `i32` | reserved, not implemented — Phase 4 (quantized models) |

Only `f32` and `f64` exist. The tag width is settled now so kernels nobody calls
are not written yet.

## GC interaction: byte accounting

Before tensors, the collector triggered on **object count** (`gc_young_threshold
= 256` objects). A 400 MB tensor counts as *one object*, so the collector never
fired for large allocations. `Obj` now carries `nbytes` — the header plus every
backing array (string data, list items, record keys/vals, closure env, tensor
data) — and the collector triggers on **either** count or bytes.

`gc_stats()` reports the byte view (`bytes_young` / `bytes_old`), which is what
`tests/e2e/gc_bytes.rald` asserts. See [`gc.md`](gc.md) for the collector
itself.

## The primitives

The tensor vocabulary is deliberately small and closed — it is exactly the set
a Phase 4 source-to-source autodiff transform needs to recognize. The
*shape-obligation* operations are typed in the checker (`shapes.md`); the rest
are plain runtime calls.

### Construction

| call | result |
|---|---|
| `zeros(shape)` / `ones(shape)` | constant tensor |
| `full(shape, fill)` | constant fill value |
| `arange(n)` | 1-D `[0, 1, ..., n-1]` |
| `tensor(nested)` | from a nested list |
| `randn(shape, seed)` | **seeded** standard-normals (Box–Muller over xorshift64*) |

`randn` takes an explicit seed, so experiments are reproducible *by
construction* — there is no ambient unseedable randomness in the tensor layer
(the old `rand()` remains, for scripts).

### Elementwise

`+ - * /` on two tensors, or a tensor and a scalar, broadcast over trailing
dims (numpy-style). Unary `exp`, `log`, `tanh`, `relu` map a tensor to a tensor
of the same shape and dtype.

### Shape-carrying operations

`matmul`, `reshape`, `transpose`/`permute`, `sum`/`mean`/`max`/`argmax` over an
axis, `tslice` (a strided view), and `expand` (broadcast a size-1 dim,
zero-copy). `matmul` is the naive C kernel — kept as the correctness oracle even
after a BLAS/Accelerate backend lands (W6).

### Introspection and scalars

`shape(t) -> list[int]`, `ndim(t) -> int`, `dtype(t) -> str` (`"f32"` /
`"f64"`), `astype(t, "f64")` (casts the buffer), and `item(t) -> float` (the
single element, which also unlocks writing a scalar back into a loop).

## Printing

`str()` of a tensor shows its dtype and shape, then the elements row-major:

```
Tensor[f32, [2, 3]]
[[1, 2, 3], [4, 5, 6]]
```

That rendering is the debugging surface for the whole phase.
