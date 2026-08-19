# Green threads

Emerald's concurrency is Go-shaped: cheap tasks that communicate over
channels, not threads that share memory behind locks. `spawn` starts a task,
`chan` connects two tasks, `join` waits for one to finish.

```rald
jobs: Chan[int] = chan(8)

def worker(name: str) -> int {
    handled = 0
    while True {
        n = recv(jobs)              # None once the channel is closed
        if n == None { break }
        handled = handled + 1
    }
    return handled
}

w = spawn(() => worker("w0"))
for n in range(2, 40) { send(jobs, n) }
chan_close(jobs)
print("handled", join(w))
```

`examples/tasks.rald` is the full worker pool this is cut from.

## The model: cooperative, one at a time

There is exactly **one running task at any moment**. A task keeps the CPU until
it reaches a switch point, and those are all named in the source:

- `spawn(f)` — the child becomes runnable, but the *spawner keeps running*
- `send` / `recv` that cannot complete immediately
- `join(t)` on a task that has not finished
- `sleep(secs)`
- `task_yield()`, which exists only to hand over control

Between two switch points a task runs alone. That is the property everything
else rests on: a statement is never interrupted halfway, `xs[i] = xs[i] + 1` is
atomic, and there are no locks in the language because there is nothing to
lock. It is concurrency for structuring a program — overlapping waits, pipeline
stages, fan-out/fan-in — not parallelism for using more cores. Two tasks never
execute at the same instant, so a CPU-bound program gains nothing.

The flip side is real: a task that never reaches a switch point never gives up
the CPU. A `while True {}` with no channel operation in it hangs the whole
program, exactly as it would in a single-threaded loop.

## Channels

`chan(n)` makes a channel with `n` buffer slots. `chan(0)` is a **rendezvous**:
a send blocks until a receiver arrives and vice versa, so it also synchronises
the two tasks. With `n > 0` a send completes immediately while the buffer has
room.

- `send` on a **closed** channel is a runtime error (a fatal one, like an
  out-of-bounds index): closing means "no more values will be produced", and a
  producer that sends after that is a bug in the program's structure.
- `recv` on a closed channel drains the remaining buffered values and then
  returns `None` forever. This is why `recv` types as `T | None` — the closed
  case is in the type, so the checker makes every receive loop handle it.
- Both wait queues are FIFO, so the longest-waiting task wins a rendezvous.

Closing is how a producer tells an unknown number of consumers to stop. There
is no `select` yet: a task waits on one channel at a time, so fan-in from
several channels is written as one collector task per channel feeding a shared
result channel.

## Deadlock is reported, not hung

When no task holds the CPU, none is runnable, and none is sleeping, no task can
ever become runnable again. The runtime says so instead of hanging:

```
emerald: runtime error: all 3 task(s) are blocked forever - deadlock (at prog.rald:12)
```

`join()` on the running task is caught the same way.

## How it works

Each task gets a real OS thread — the generated code is ordinary C, and a task
that blocks needs a C stack to block on — but a single **scheduling token**
decides which thread runs. The token moves under one mutex and one broadcast
condition variable: a task may take it when it is at the head of the run queue
and nobody holds it. Threads are created detached with a 1 MiB stack; when the
main task returns, the process exits and any still-running task dies with it,
as in Go.

Using threads this way (rather than `ucontext` or hand-rolled stack switching)
buys portability and debuggability — every task is a normal thread in a
debugger — at the cost of ~1 MiB of address space per task and a mutex
handoff per switch. Switching costs a condvar broadcast, which is the reason
the switch points are channel-granular rather than per-statement.

### What the GC had to learn

The collector stays precise and lock-free, because only the token holder can
allocate and therefore only the token holder can collect. Every other task is
parked at a switch point with a consistent shadow stack. Two things changed:

- `rt_roots` is now `_Thread_local`: one shadow stack per task. Each live task
  registers the address of its own head pointer, and `gc_mark_roots` walks all
  of them (`sch_mark` in `runtime.c`).
- A task's `fn`, `result`, and the value in flight across a rendezvous (`xfer`)
  are marked as roots on every collection, minor ones included, so writing them
  needs no write barrier. A channel's *buffer* is reached through the channel
  object, so `send` does take the barrier.

`rt_cur_file` / `rt_cur_line` are thread-local for the same reason: several
tasks are mid-statement at once, and a runtime error must name the line of the
task that hit it. A spawned task inherits the spawner's file so its errors are
located even before it crosses a file boundary.

`O_CHAN` and `O_TASK` are ordinary GC objects. A finished task leaves the live
list, so its handle and its side struct are collected once nothing holds them;
`join()` on a finished task reads the result straight out of the handle.

## Types

`Chan[T]` and `Task[T]` are writable in annotations and checked at both ends:
`send` must carry a `T`, `recv` yields `T | None`, `spawn` demands a
zero-argument function and remembers its return type, and `join` gives it back.
`chan()` produces `Chan[any]`, which the annotation on the binding refines:

```rald
c: Chan[int] = chan(0)     # Chan[int], not Chan[any]
```

Handles are **invariant** in the element type — a `Chan[int]` passed where a
`Chan[str]` is expected would let the other end send the wrong thing — with
`Chan[any]` as the escape hatch in both directions, for code that has not
annotated anything.

Since `spawn` takes a function of no arguments, anything a task needs is
captured by the lambda that wraps it. Watch the loop-variable trap: a `for`
variable is one cell shared by all iterations, so

```rald
for id in range(0, 3) { append(ts, spawn(() => worker(id))) }   # every task sees id = 2
```

Bind the value in a helper (`def start(id: int) -> Task[None] { return spawn(() => worker(id)) }`)
to give each task its own.

## Status

Deliberately absent for now: `select`, timeouts on a receive, cancellation,
task-local state, and any form of parallelism. Each of those changes what the
scheduler has to guarantee, and the single-token model is what currently keeps
the collector and the generated code free of synchronisation.
