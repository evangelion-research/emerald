# Emerald's Garbage Collector

A **precise, two-generation mark-and-sweep** collector in `src/runtime_*.c`,
with rooting done by the generated code itself (a *shadow stack*). No
conservative C-stack scanning, no ref-counting, no leaks-until-exit arena.

## Object model

- `Value` is a 16-byte tagged struct passed by value. `None`/`bool`/`int`/
  `float` never touch the heap.
- **Small-string optimization**: strings of 7 bytes or fewer are stored inline
  in the `Value` itself (`V_STR`), so the most common short strings never
  allocate an `Obj` or participate in collection at all.
- Heap kinds: `O_STR` (strings longer than 7 bytes), `O_LIST`, `O_REC`,
  `O_FUNC` (a first-class function: a C entry point plus its captured env),
  and `O_CELL` (a mutable box for a captured variable). Every `Obj` sits on
  one intrusive `gc_next` list for its generation and has `mark`, `gen`, and
  `remembered` bits. A closure's env is an array of cells; each cell points
  back at the value it holds, so marking a closure marks its captures.
- Backing arrays (list items, record fields, string bytes) are plain
  `malloc` memory owned by their `Obj` and freed at sweep. Growing them can
  never trigger a collection — only `rt_obj_new()` can.

## Rooting: the shadow stack

> With [green threads](concurrency.md) there is one shadow stack per task:
> `rt_roots` is thread-local and the collector walks every live task's stack.
> Only the task holding the scheduling token can allocate, so collection is
> still single-threaded and lock-free.

Generated code gives every function one slot array and registers it:

```c
static Value emf_fib(Value *__env, Value __p0) {
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

## Generations

New objects are born in a **nursery**; a **minor** collection sweeps only
the nursery and promotes survivors to the **tenured** generation. A **major**
collection marks and sweeps both generations. Collection is triggered inside
`rt_obj_new()` by two thresholds (nursery and tenured, both starting at 256,
then `2 × survivors` after each cycle):

1. **Minor**: mark from every `RootFrame` *and* from the remembered set,
   treating the tenured generation as opaque; sweep the nursery, promoting
   survivors and clearing marks.
2. **Major**: mark everything from the roots; sweep both generations.

Because a minor collection never walks the tenured generation, it costs
`O(nursery + remembered)`, not `O(live)`. The **write barrier** in
`em_setindex`/`em_setattr` records any tenured object that is mutated to
reference a nursery object in the **remembered set**, so a minor collection
can still find nursery objects reachable only from the old generation.

Function returns are safe without extra ceremony: `rt_pop_frame()` happens
before `return`, and the returned Value is stored into the *caller's*
rooted slot before the caller can allocate again.

## Observability

`gc_stats()` returns a record of counters — `collections` (total cycles),
`live` (survivors of the last collection), `young`, `old`, and `threshold` —
for tuning and tests.

## Measured

`examples/gc_stress.rald` and the e2e suite churn millions of short-lived
strings/lists/records; a 2M-iteration record churn (~8M objects) peaks at
**~1.6 MB RSS** in ~0.3 s.

## Future work

- Interned long strings (short strings are already inlined via SSO).
- Age-based tenuring / a remembered-set of fixed size (currently every nursery
  survivor is promoted immediately and the remembered set is unbounded).
