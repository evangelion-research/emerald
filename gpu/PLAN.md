# CUDA tensor backend for Emerald

## Context

Emerald's tensor layer is complete in surface but naive in implementation. Every
numeric primitive in `src/runtime_tensor.c` is a scalar C loop dispatching
through a `double (*f)(double,double)` function pointer, with `t_get`/`t_set`
branching on dtype **per element**. `em_tensor_matmul` (`src/runtime_tensor.c:390`)
is a naive triple loop; `t_binary_tt` (`:319`) re-unravels the flat index and
recomputes strides for every element. Nothing vectorizes. That caps Emerald at
toy tensor sizes, which in turn caps the autograd engine — the thing
`docs/REMAINING_FEATURES.md` identifies as the project's most valuable asset —
at toy training runs.

The architecture is unusually well-suited to fixing this. `docs/tensors.md`
records the decision that made it so: tensor operations are **whole-array
runtime calls, not per-element generated code**. `matmul(a, b)` lowers to exactly
one `em_tensor_matmul()` call, and `src/codegen_*.c` never mentions a `Type`.
The consequence is that a GPU backend slots in *behind* the builtin ABI — the
~30 `em_tensor_*` entry points declared in `include/runtime.h:281-313` and
registered in `include/builtins.def:95-127` — with **no change to the lexer,
parser, module loader, checker, or codegen**.

**Intended outcome:** `emeraldc --gpu prog.rald` produces a binary whose tensor
math runs on an NVIDIA GPU, producing the same results as the CPU path, with no
change to any `.rald` source. Every existing example and golden test keeps
passing verbatim, on both paths.

**Explicitly out of scope:** GPU syntax in the language (`gpu def`,
`parallel_for`), kernel fusion, multi-GPU, new dtypes (f16/bf16), convolution.
Fusion is noted at the end as the natural follow-on, because the autograd tape
is already the right IR for it.

## Constraints this plan must respect

1. **`src/runtime_*.c` must not depend on compiler headers.** `task runtime-check`
   enforces this with `-fsyntax-only`. It must stay green, and `runtime_tensor.c`
   must still compile clean *without* `-DEMERALD_CUDA`.
2. **The runtime is recompiled into every generated program** by the `cc` line at
   `src/main.c:340-347`, which globs `'%s'/runtime_*.c`. A `.cu` file is not
   globbed and nvcc is too slow to run per-program — so CUDA code must be
   **prebuilt once** into a static library.
3. **macOS has no CUDA.** The default build and `task test` must remain green on
   the dev machine. `--gpu` without a built library must fail with a clear error.
4. **`task bless` + review the diff** after any change touching `--help` output
   (the `cli` golden suite pins it) or diagnostics.

## Key design decisions

### The `.cu` file never sees `Value` or `Obj`

nvcc compiles `.cu` as C++. Rather than make `include/runtime.h` C++-safe and
drag the GC object model into device code, the CUDA layer's entire interface is
a plain C ABI over raw pointers and integers, declared in a new `include/emcu.h`:

```c
/* include/emcu.h — no Value, no Obj, no compiler headers */
typedef struct { int64_t dims[8]; int64_t strides[8]; uint8_t ndim; } EmcuDesc;

int   emcu_available(void);                       /* 0 if no device */
void *emcu_alloc(size_t bytes);
void  emcu_free(void *dptr);
void  emcu_h2d(void *d, const void *h, size_t n);
void  emcu_d2h(void *h, const void *d, size_t n);

void  emcu_binary(void *out, const void *a, const void *b,
                  const EmcuDesc *oa, const EmcuDesc *ob, const EmcuDesc *oo,
                  int op, int is_f64);
void  emcu_unary (void *out, const void *a, size_t n, int op, int is_f64);
void  emcu_gemm  (void *c, const void *a, const void *b,
                  int64_t m, int64_t k, int64_t n,
                  int64_t lda, int64_t ldb, int at, int bt, int is_f64);
void  emcu_reduce(void *out, const void *a, const EmcuDesc *da,
                  int64_t axis, int kind, void *argout, int is_f64);
```

`runtime_tensor.c` stays C11 and just calls these. `op`/`kind` are the same
integer codes the existing CPU paths already switch on. This keeps
`task runtime-check` trivially green and avoids all C/C++ ABI risk.

### Residency lives in two fields, and one chokepoint enforces it

Extend the tensor arm of `Obj` in `include/runtime.h:85-96`:

```c
struct {
    DType dt;
    uint8_t ndim;
    int64_t *dims;
    int64_t *strides;
    void *data;         /* host buffer; may be NULL when device-only */
    Obj *base;
    void *ddata;        /* NEW: device buffer (or base->ddata + offset) */
    uint8_t resident;   /* NEW: RES_HOST | RES_DEVICE | RES_BOTH */
} tensor;
```

**The single most important observation in this plan:** every element-level read
and write in the entire runtime funnels through exactly two functions —
`t_get` and `t_set` at `src/runtime_tensor.c:34-43`, exported to other runtime
files as `tensor_elem_get`/`tensor_elem_set` (`:45-47`). There are 27 uses inside
`runtime_tensor.c` and 28 inside `runtime_autograd.c`, and **no other file
dereferences `as.tensor.data` at all** (verified: `grep -rn "as\.tensor\.data"
src/runtime_*.c` yields only `runtime_gc.c:150`, the free path).

So a sync check placed in those two functions makes the whole runtime —
including all 887 lines of `runtime_autograd.c` — automatically correct against
device-resident tensors:

```c
static double t_get(const Obj *t, size_t off) {
    t_sync_host((Obj *)t);            /* no-op unless resident == RES_DEVICE */
    ...existing body...
}
static void t_set(Obj *t, size_t off, double v) {
    t_sync_host(t);
    t->as.tensor.resident = RES_HOST; /* device copy is now stale */
    ...existing body...
}
```

Correctness first, performance second: after Phase 1 every GPU program is
*correct*, and the later phases remove syncs from the hot paths one at a time.

### Views delegate residency to their base

`tensor_view` (`src/runtime_tensor.c:123`) builds a descriptor over the owner's
buffer with a byte offset, and `runtime_ops.c:290` uses it for indexing. Since
view and base share storage, residency is a property of the **base**:

- `tensor_view` sets `ddata = base->ddata ? (char*)base->ddata + byte_off : NULL`.
- `t_sync_host(view)` recurses to the base, then recomputes `view->data` from
  the base's (possibly newly allocated) host buffer.
- `t_to_device(view)` likewise promotes the base and rebases `ddata`.

This is the one genuine correctness hazard in the design and gets dedicated
tests (`gpu/tests/view_residency.rald`): transpose→matmul, slice→sum,
index-then-`item`, and a slice written on host while the base is device-resident.

### Kernels handle strides, like the CPU code does

Two variants per op: a contiguous fast path (flat index) and a general strided
path that reconstructs the offset from `EmcuDesc` (max 8 dims, passed by value).
This mirrors what `t_binary_tt` already does and means transposes and slices
stay on the device instead of forcing a materialization.

### cuBLAS for matmul, with the row-major transpose trick

cuBLAS is column-major. Row-major `C = A·B` is obtained by computing
`Cᵀ = Bᵀ·Aᵀ` — call `cublasSgemm`/`cublasDgemm` with the operands swapped. A
2-D tensor whose strides are a plain row-major or transposed layout maps
directly onto `lda`/`op`; anything else (a permuted or sliced view with a
non-unit innermost stride) is materialized contiguous on device first. The
`TB_MATMUL_VV/VM/MV/MM` split already in the tape (`src/runtime_internal.h:72`)
maps to gemm with a degenerate dimension.

### Two escape hatches, both needed by the test harness

- `EMERALD_GPU=0` — runtime env var that forces the CPU path in a
  `--gpu`-linked binary. Makes A/B parity testing a single env flip, no recompile.
- `EMERALD_GPU_MIN_ELEMS` (default 4096) — tensors below this stay on the CPU,
  because a kernel launch costs more than the work. Set to `0` by the parity
  suite so the existing small-tensor golden tests actually exercise GPU code.

## Files

**New (`gpu/`):**

| Path | Contents |
|---|---|
| `gpu/emcu_mem.cu` | `emcu_available/alloc/free/h2d/d2h`, device context init, error checks |
| `gpu/emcu_elem.cu` | elementwise binary (TT/TS/ST, broadcast) + unary (`exp`,`log`,`tanh`,`relu`), cast |
| `gpu/emcu_reduce.cu` | `sum`/`mean`/`max`/`argmax` over an axis (segmented reduction) |
| `gpu/emcu_blas.cu` | cuBLAS handle, gemm wrappers |
| `gpu/emcu_ag.cu` | autograd backward helpers (Phase 4) |
| `gpu/Taskfile.yml` | `build:gpu` → `bin/libemeraldcu.a`; included from the root Taskfile |
| `gpu/Dockerfile`, `gpu/setup.sh` | reproducible cloud env (`nvidia/cuda:12.x-devel-ubuntu22.04` + go-task) |
| `gpu/README.md` | cloud workflow, env vars, perf notes, what is and isn't on device |
| `gpu/bench/*.rald` | GPU-sized workloads (see Verification) |
| `gpu/tests/*.rald` | residency/view/thrash cases with `.expected` goldens |

**Modified:**

| Path | Change |
|---|---|
| `include/runtime.h` | `ddata` + `resident` in the tensor arm; `RES_*` enum |
| `include/emcu.h` *(new)* | the C ABI above; lives in `include/` so `-Iinclude` already finds it |
| `src/runtime_tensor.c` | `t_sync_host`/`t_to_device`; sync in `t_get`/`t_set`; `#ifdef EMERALD_CUDA` dispatch in each `em_tensor_*` |
| `src/runtime_gc.c:147-151` | free `ddata` when the tensor owns it (`base == NULL`); count device bytes in `nbytes` |
| `src/runtime_autograd.c` | Phase 4 only: device paths for four hot helpers |
| `src/main.c` | `--gpu` flag; `-DEMERALD_CUDA`, `-I/-L` for CUDA, `-lcudart -lcublas`, and `libemeraldcu.a` on the `cc` line at `:340-347`; `EMERALD_GPU_DIR` baked in like `EMERALD_SRC_DIR` (`:41`); `--gpu` in `print_help()` |
| `Taskfile.yml` | `includes: gpu: gpu/Taskfile.yml`; `test:gpu` task |
| `tests/run_tests.sh` | `run_gpu()` mirroring `run_e2e()` (`:78`) + `gpu)` case in the dispatch (`:241`) |
| `docs/tensors.md` | a "GPU execution" section documenting residency and the env vars |

## Phases

### Phase 0 — Cloud environment and an honest CPU baseline

Nothing GPU-specific yet; this exists so every later claim is measurable.

1. `gpu/Dockerfile` + `gpu/setup.sh` bringing up CUDA 12.x, go-task, and the repo.
2. Confirm `task test` is fully green on the Linux box with the unchanged CPU path.
3. Write `gpu/bench/*.rald`. **The existing `tests/bench/tensors.rald` is a 16×16
   matmul run 8 times** — meaningless at GPU scale. New workloads: 2048³ matmul,
   16M-element elementwise chains, axis reductions over ~10⁸ elements, and a
   scaled-up version of `tests/e2e/autograd_train.rald`.
4. Record CPU baselines in `gpu/README.md`.

*Exit:* reproducible env, green CPU suite on Linux, recorded baseline numbers.

### Phase 1 — Residency plumbing, one kernel, full parity harness

The de-risking phase. Everything hard about this project is here.

1. `include/emcu.h`; `gpu/emcu_mem.cu`; `gpu/Taskfile.yml` building `bin/libemeraldcu.a`.
2. `ddata` + `resident` fields; `t_sync_host` / `t_to_device`; the two-line sync
   in `t_get`/`t_set`; view base-delegation in `tensor_view`.
3. Device-buffer free in `gc_free` (`src/runtime_gc.c:147`) and byte accounting
   so a device-resident tensor still triggers collection.
4. `--gpu` in `src/main.c`, the link line, and `print_help()`. `task bless` and
   review the `cli` golden diff.
5. **One** op on the GPU: elementwise tensor+tensor add.
6. `run_gpu()` in `tests/run_tests.sh`: recompile every `tests/e2e/*.rald` with
   `--gpu`, run with `EMERALD_GPU_MIN_ELEMS=0`, diff against the *existing*
   `.expected` goldens. Plus `gpu/tests/view_residency.rald`.

*Exit:* `task test` green on macOS (CPU), `task test:gpu` green on the GPU box,
`runtime-check` green, and add on large tensors measurably faster than baseline.

### Phase 2 — Elementwise, unary, broadcast

`add`/`sub`/`mul`/`div` in all three layouts (TT/TS/ST), `exp`/`log`/`tanh`/`relu`,
`astype`. Contiguous fast path plus the general strided path. `t_unary`
(`:249`), `t_scalar_binary` (`:300`), and `t_binary_tt` (`:319`) each grow one
`#ifdef` dispatch at the top and are otherwise untouched.

### Phase 3 — matmul and reductions

`em_tensor_matmul` (`:390`) → cuBLAS via the transpose trick, with a contiguous
materialization fallback. `t_reduce` (`:586`) → segmented reduction kernels for
`sum`/`mean`/`max`/`argmax`.

**Expect golden-test friction here.** GPU reductions reassociate the sum, so
f32 results differ in the last bits from the sequential CPU order. `run_gpu()`
needs a tolerance comparator for float output rather than the exact `diff` that
`report()` uses — this is a real deviation from the repo's exact-golden
convention and should be a deliberate, documented choice, applied *only* to the
GPU suite.

### Phase 4 — Autograd backward on device

Forward autograd is free after Phases 2–3 (the tape replays the same
`em_tensor_*` primitives). **Backward is not.** `runtime_autograd.c` hand-rolls
its own scalar loops and has 28 direct `tensor_elem_get/set` calls — after
Phase 1 those are *correct* but each one drags a tensor back to host, so an
unported backward pass will thrash and can be slower than pure CPU.

Four helpers carry essentially all backward cost and are the whole of this phase:

| Helper | Location | Becomes |
|---|---|---|
| `ag_accum` | `src/runtime_autograd.c:300` | device elementwise add-into |
| `ag_zeros_like` | `:287` | `cudaMemset` / fill kernel |
| `ag_reduce_broadcast` | `:356` | device reduction over broadcast axes |
| matmul backward | `:442-520` | two cuBLAS gemms |

*Exit:* the scaled training loop from Phase 0 shows end-to-end speedup, and
`tests/e2e/autograd_train.rald` still matches its golden.

### Not now

The `TapeKind` graph (`src/runtime_internal.h:72-103`) is already a
topologically ordered op graph carrying shapes, axes, and parent indices — a
ready-made fusion IR. Kernel fusion is the obvious next milestone and is
deliberately excluded from this plan.

## Verification

**On macOS (every phase, before anything leaves the laptop):**
```sh
task                 # builds; must be unaffected by all of this
task test            # full suite green, CPU path
task runtime-check   # runtime_tensor.c still compiles clean without -DEMERALD_CUDA
bin/emeraldc --gpu examples/shapes.rald   # must fail with a clear "no CUDA" error
```

**On the cloud GPU box:**
```sh
./gpu/setup.sh
task && task gpu:build:gpu      # bin/emeraldc + bin/libemeraldcu.a
task test                       # CPU path still green on Linux
task test:gpu                   # parity: every e2e golden, recompiled with --gpu
```

**Parity is the core check.** `run_gpu()` compiles each `tests/e2e/*.rald` with
`--gpu`, runs it under `EMERALD_GPU_MIN_ELEMS=0` so even 2×2 tensors take the
device path, and compares against the goldens the CPU path already pins. A GPU
regression therefore shows up as a diff against output that is already trusted.

**A/B correctness on one binary** — same executable, same inputs, one env flip:
```sh
bin/emeraldc --gpu gpu/bench/matmul_2048.rald -o /tmp/mm
EMERALD_GPU=0 /tmp/mm > /tmp/cpu.txt
EMERALD_GPU=1 /tmp/mm > /tmp/gpu.txt
diff /tmp/cpu.txt /tmp/gpu.txt     # within f32 tolerance
```

**Memory correctness:**
```sh
compute-sanitizer --tool memcheck  /tmp/mm    # no invalid device accesses
compute-sanitizer --tool initcheck /tmp/mm    # no uninitialized device reads
```
Plus a GC stress case: allocate and drop device tensors in a loop and assert via
`gc_stats()` that device bytes return to baseline — the leak this design is most
exposed to is a `ddata` freed on the CPU side but not the device side.

**Performance**, reported as a table in `gpu/README.md` per phase: each
`gpu/bench/*.rald` workload timed CPU vs GPU, with the transfer time called out
separately so thrash is visible rather than averaged away.

## Risks

- **Residency of views.** The one place where a wrong answer is plausible rather
  than merely slow. Mitigated by base-delegation plus dedicated tests; worth
  writing those tests before the implementation.
- **f32 reduction reassociation** breaks exact golden matching (Phase 3). Handled
  by a tolerance comparator scoped to the GPU suite only.
- **Transfer thrash.** A loop alternating `matmul` and `item()` ping-pongs across
  PCIe. `EMERALD_GPU_MIN_ELEMS` and the residency tags mitigate it; the benchmark
  table is what makes it visible.
- **f64 on consumer GeForce cards** runs at 1/32 the f32 rate. Most of the
  autograd tests use `f64` (`astype(..., "f64")` is required by
  `tests/e2e/autograd_train.rald`). Benchmark both dtypes and note the hardware
  in the results table, or f64 numbers from a GeForce box will look like a bug.
- **No CUDA on the dev machine** means the `--gpu` code path is only ever
  compiled on the cloud box. Keeping `task runtime-check` and the full CPU suite
  green locally is the guard against drift between sessions.
