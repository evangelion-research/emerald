# Emerald's Garbage Collector

A **precise mark-and-sweep** collector in `src/runtime.c`, with rooting
done by the generated code itself (a *shadow stack*). No conservative
C-stack scanning, no ref-counting, no leaks-until-exit arena.

## Object model

- `Value` is a 16-byte tagged struct passed by value. `None`/`bool`/`int`/
  `float` never touch the heap.
- Heap kinds: `O_STR`, `O_LIST`, `O_REC`. Every `Obj` sits on one intrusive
  `gc_next` list (the allocation list) and has a `mark` bit.
- Backing arrays (list items, record fields, string bytes) are plain
  `malloc` memory owned by their `Obj` and freed at sweep. Growing them can
  never trigger a collection — only `rt_obj_new()` can.

## Rooting: the shadow stack

Generated code gives every function one slot array and registers it:

```c
static Value emf_fib(Value __p0) {
    Value F[5];                    /* all locals + expression temporaries */
    RootFrame __fr;
    for (int __i = 0; __i < 5; __i++) F[__i] = em_none();
    F[0] = __p0;
    rt_push_frame(&__fr, F, 5);    /* O(1): push onto rt_roots list */
    ...
    rt_pop_frame();
    return em_none();
}
```

Globals live in a `static Value G[n]` registered once in `main`. Because
expressions are lowered so that **every intermediate Value sits in a slot**,
any value alive across an allocation is reachable from a root frame —
that's what makes the collector precise. Runtime internals keep the
invariant by building strings in raw `malloc` buffers and only wrapping
them in an `Obj` at the end.

Slot frugality: temporaries are watermark-allocated per statement, so frame
size tracks expression *depth*, not function length.

## Collection

Triggered inside `rt_obj_new()` when the live-object count passes a
threshold (start 256, then `2 × survivors` after each cycle — a simple
heap-growth heuristic):

1. **Mark**: walk every `RootFrame`, recursively mark reachable objects.
2. **Sweep**: walk the allocation list, free unmarked objects, clear marks.

Function returns are safe without extra ceremony: `rt_pop_frame()` happens
before `return`, and the returned Value is stored into the *caller's*
rooted slot before the caller can allocate again.

## Measured

`examples/gc_stress.rald` and the e2e suite churn millions of short-lived
strings/lists/records; a 2M-iteration record churn (~8M objects) peaks at
**~1.6 MB RSS** in ~0.3 s.

## Future work

- Generational collection (the threshold heuristic is already
  generation-shaped).
- Interned short strings / small-string optimization.
- A `gc_stats()` builtin for observability.
