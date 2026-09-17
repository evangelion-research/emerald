# Autograd

Emerald differentiates pure scalar-loss functions over tensors with
**reverse-mode automatic differentiation**. One builtin, `value_and_grad(f, x)`,
records a tape while `f` runs at `x`, then walks it backwards to produce the
value and the gradient. This page documents the implemented surface; the
tensor primitives it differentiates are in [`tensors.md`](tensors.md), and
their static shapes in [`shapes.md`](shapes.md).

## The API

```rald
def sq(t: Tensor[f64, [3]]) -> Tensor[f64, []] pure {
    return sum(t * t, 0)
}

x: Tensor[f64, [3]] = astype(tensor([1.0, 2.0, 3.0]), "f64")
r = value_and_grad(sq, x)
print(item(r.value))          # 14
print(item(tslice(r.grad, 0, 1, 2)))   # 4 — d/dx[1] of the sum of squares
```

`value_and_grad(f, x)` returns `{ value: Tensor[dt, []], grad: Tensor[dt, S] }`,
where `dt` is the dtype and `S` the shape of `f`'s tensor parameter. The
gradient has exactly the leaf's static shape, and that shape is part of the
result *type* — the checker produces it, so downstream code keeps its shape
obligations.

The design is the functional one: there is no `requires_grad` flag, no
`Tensor.grad` field, and no global tape. Recording state lives entirely inside
one `value_and_grad` call, so the builtin is `pure` and may appear in pure code.

## What the checker enforces

Every rule is checked at the call site, before codegen:

| Rule | Diagnostic |
|---|---|
| `f` is a function taking one tensor parameter | `E_TYPE_ARG` |
| `f` is `pure` — differentiation is defined only for pure computations | `E_TYPE_PURE_CALL` |
| `f` returns a scalar tensor (`Tensor[dt, []]`) | `E_TYPE_ARG`, `E_SHAPE_RANK` |
| `x` is a tensor | `E_TYPE_ARG` |
| `x`'s dtype matches the parameter's | `E_SHAPE_DTYPE` |
| `x`'s shape matches the parameter's (when both are static) | `E_SHAPE_DIM_ARG` |

Under `--proof` every shape involved — the parameter's, the argument's, and
the return's — must be static; a dynamically shaped adjoint would silently
drop the shape obligation the mode exists to check (`E_PROOF_SHAPE`).

## The tape

A recording appends one node per differentiable primitive: the operation, its
operands, whatever metadata the backward rule needs, and the output tensor.
Recording allocates nothing beyond the forward operation's own output, so a
program that never differentiates pays one branch per tensor op.

The tape is an `O_TAPE` GC object. Every tensor it references is listed in a
traced `alive` array (with the write barrier keeping a tenured tape correct
over nursery tensors), so collection stays precise during recording and
backward, and the whole graph is collectable the moment the call returns.

The backward pass walks nodes in reverse creation order — parents are always
recorded before children, so that is a topological order of the reversed
graph — accumulating each node's adjoint into its parents and finally into the
gradient leaf. Adjoint buffers are allocated lazily inside one rooted frame.
Gradient accumulation for shared subexpressions (a diamond graph, two slices
of one leaf) is automatic: adjoints add.

## Gradient policy

- **Constants get no gradient.** Scalars, tensor constructors
  (`tensor`, `zeros`, `ones`, `full`, `arange`), and tensors built outside the
  recording are constants.
- **Nondifferentiable operations cut gradients.** `item`, `argmax`, `shape`,
  `ndim`, `dtype`, and comparisons are not recorded; using one does not error,
  it stops the flow of gradient past that point.
- **Boundary conventions.** `relu` has subgradient 0 at 0; a `max` reduction
  places the whole gradient on the first maximal index, matching `argmax`.
- **Mixed dtypes.** Each adjoint carries the dtype of the tensor it belongs
  to; `astype`'s backward rule casts the adjoint back.
- **One backward per call.** The tape is created and consumed inside
  `value_and_grad`; there is no repeated-backward or retain-graph mode.

## The rules

| Operation | Backward rule |
|---|---|
| `+`, `-`, `*`, `/` (tensor-tensor, tensor-scalar, scalar-tensor) | local derivative, with one shared kernel reducing the adjoint over broadcast axes |
| `exp`, `tanh` | multiply by the output (`exp`), `1 − out²` (`tanh`) |
| `log` | divide by the input |
| `relu` | step function; 0 at 0 |
| `matmul` | the two transposed matmuls, for all four vector/matrix layouts |
| `reshape` | reshape the adjoint to the input shape |
| `transpose`, `permute` | apply the (inverse) permutation |
| `sum` | expand the adjoint back over the reduced axis |
| `mean` | as `sum`, then divide by the axis length |
| `max` | scatter each cell's gradient to its saved first-maximal index |
| `tslice` | scatter-add into an input-shaped zero tensor |
| `expand` | sum the adjoint over the expanded axes |
| `astype` | cast the adjoint under the dtype rule above |

There is no Jacobian: every rule is a vector-Jacobian product computed by the
existing forward kernels.

## Verification

- `tests/e2e/autograd_ops.rald` compares every rule against a central finite
  difference of the same loss, in `f64`, including broadcasting, overlapping
  slices, repeated parents, diamond graphs, and unused branches.
- `tests/e2e/autograd_train.rald` fits a linear model and a two-layer MLP by
  gradient descent defined entirely in Emerald, deterministically (the data is
  fixed and `randn` is seeded), asserting the learned parameters and the loss
  decrease.

## Limits

Second-order gradients, detach/no-grad scopes, and an optimizer/loss library
do not exist yet; they are the next milestones in
[`REMAINING_FEATURES.md`](REMAINING_FEATURES.md). Differentiability is not
claimed as a theorem: the contract is "every executed primitive has a
registered VJP rule and the function is pure", with the boundary conventions
above documented rather than proven.
