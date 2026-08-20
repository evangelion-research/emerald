/* Runtime: the cooperative scheduler, channels, spawn/join/yield, and the
 * higher-order list builtins that call back into Emerald code. */
#include "runtime_internal.h"

static pthread_cond_t sch_cv = PTHREAD_COND_INITIALIZER;

static Task *sch_all = NULL;            /* every task that is not T_DONE */

static Task *sch_runq = NULL, *sch_runq_tail = NULL;

static Task *sch_cur = NULL;            /* the token holder, or NULL */

static size_t sch_alive = 0, sch_sleepers = 0;

static size_t sch_spawned = 0, sch_switches = 0;   /* for gc_stats() */

static Task sch_main;                   /* the task the program starts on */

static _Thread_local Task *sch_self = NULL;

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void runq_push(Task *t) {
    t->qnext = NULL;
    if (sch_runq_tail) sch_runq_tail->qnext = t;
    else sch_runq = t;
    sch_runq_tail = t;
}

static Task *runq_pop(void) {
    Task *t = sch_runq;
    if (!t) return NULL;
    sch_runq = t->qnext;
    if (!sch_runq) sch_runq_tail = NULL;
    t->qnext = NULL;
    return t;
}

/* Nobody holds the token, nothing is runnable, and nothing will ever become
 * runnable on its own: every remaining task is parked on a channel or a join
 * that no other task can complete. Report it the way Go does rather than
 * hanging forever. */
static void sch_check_deadlock(void) {
    if (sch_cur == NULL && sch_runq == NULL && sch_sleepers == 0 && sch_alive > 0)
        rt_fatal("all %zu task(s) are blocked forever - deadlock", sch_alive);
}

/* Give up the token. The caller has already parked itself wherever it needs
 * to be found again (a run queue, a channel's wait queue, a join queue). */
static void sch_release(void) {
    sch_cur = NULL;
    sch_switches++;
    pthread_cond_broadcast(&sch_cv);
    sch_check_deadlock();
}

/* Wait for the token. `self` must be RUNNABLE and sitting in the run queue. */
static void sch_acquire(Task *self) {
    for (;;) {
        if (sch_cur == NULL && sch_runq == self) {
            runq_pop();
            sch_cur = self;
            return;
        }
        pthread_cond_wait(&sch_cv, &sch_mu);
    }
}

/* Make a parked task runnable again. Called by whoever satisfies its wait. */
static void sch_wake(Task *t) {
    t->state = T_RUNNABLE;
    runq_push(t);
    pthread_cond_broadcast(&sch_cv);
}

/* Park the running task. The caller has linked `self` into the wait queue
 * that some other task will wake it from, and holds sch_mu. */
static void sch_block(Task *self) {
    self->state = T_BLOCKED;
    sch_release();
    while (self->state != T_RUNNABLE) pthread_cond_wait(&sch_cv, &sch_mu);
    sch_acquire(self);
}

static void sch_yield_locked(Task *self) {
    runq_push(self);
    sch_release();
    sch_acquire(self);
}

/* GC: walk every task's shadow stack and the values the scheduler itself
 * holds on their behalf. Those values (`fn`, `result`, `xfer`) are marked
 * here as roots on every collection, minor ones included, so storing into
 * them needs no write barrier -- unlike a channel's buffer, which is reached
 * through the channel object and therefore does. Only the token holder ever collects, and every other
 * task is parked at a switch point, so all of these are quiescent. */
void sch_mark(bool minor) {
    for (Task *t = sch_all; t; t = t->next) {
        for (RootFrame *f = *t->roots; f; f = f->prev)
            for (size_t i = 0; i < f->count; i++)
                gc_mark_value(f->slots[i], minor);
        gc_mark_value(t->fn, minor);
        gc_mark_value(t->result, minor);
        gc_mark_value(t->xfer, minor);
        if (t->handle) gc_mark_obj(t->handle, minor);
    }
}

void sch_init(void) {
    memset(&sch_main, 0, sizeof sch_main);
    sch_main.state = T_RUNNABLE;
    sch_main.fn = sch_main.result = sch_main.xfer = em_none();
    sch_main.roots = &rt_roots;
    sch_main.thread = pthread_self();
    sch_self = &sch_main;
    sch_all = &sch_main;
    sch_alive = 1;
    sch_cur = &sch_main;
}

/* --- channels ------------------------------------------------------------ */
/* The wait queues are plain FIFOs, so a channel is fair: the longest-waiting
 * sender or receiver is the one a rendezvous picks. */
static void chanq_push(Task **head, Task **tail, Task *t) {
    t->qnext = NULL;
    if (*tail) (*tail)->qnext = t;
    else *head = t;
    *tail = t;
}

static Task *chanq_pop(Task **head, Task **tail) {
    Task *t = *head;
    if (!t) return NULL;
    *head = t->qnext;
    if (!*head) *tail = NULL;
    t->qnext = NULL;
    return t;
}

static Obj *as_chan(Value v, const char *who) {
    if (!(v.tag == V_OBJ && v.as.o->tag == O_CHAN))
        rt_fatal("%s() expects a channel, got %s", who, type_name(v));
    return v.as.o;
}

static Obj *as_task(Value v, const char *who) {
    if (!(v.tag == V_OBJ && v.as.o->tag == O_TASK))
        rt_fatal("%s() expects a task, got %s", who, type_name(v));
    return v.as.o;
}

Value em_chan(Value cap) {
    if (cap.tag != V_INT || cap.as.i < 0)
        rt_fatal("chan() capacity must be a non-negative int");
    size_t n = (size_t)cap.as.i;
    Value *buf = n ? xmalloc(sizeof(Value) * n) : NULL;
    Chan *c = xmalloc(sizeof(Chan));
    memset(c, 0, sizeof *c);
    c->buf = buf;
    c->cap = n;
    Obj *o = rt_obj_new(O_CHAN);   /* may collect: nothing live is unrooted */
    o->as.chan = c;
    obj_charge(o, sizeof(Chan) + sizeof(Value) * n);
    return obj_val(o);
}

void em_send(Value chv, Value v) {
    Obj *o = as_chan(chv, "send");
    Chan *c = o->as.chan;
    Task *self = sch_self;
    pthread_mutex_lock(&sch_mu);
    if (c->closed) {
        pthread_mutex_unlock(&sch_mu);
        rt_fatal("send on a closed channel");
    }
    Task *r = chanq_pop(&c->recvq, &c->recvq_tail);
    if (r) {                       /* a receiver is parked: hand it over */
        r->xfer = v;
        r->xfer_ok = true;
        sch_wake(r);
        pthread_mutex_unlock(&sch_mu);
        return;
    }
    if (c->len < c->cap) {         /* room in the buffer */
        c->buf[(c->head + c->len) % c->cap] = v;
        c->len++;
        gc_write_barrier(o, v);
        pthread_mutex_unlock(&sch_mu);
        return;
    }
    self->xfer = v;                /* park until a receiver takes it */
    self->send_failed = false;
    chanq_push(&c->sendq, &c->sendq_tail, self);
    sch_block(self);
    bool failed = self->send_failed;
    self->xfer = em_none();
    pthread_mutex_unlock(&sch_mu);
    if (failed) rt_fatal("send on a closed channel");
}

Value em_recv(Value chv) {
    Obj *o = as_chan(chv, "recv");
    Chan *c = o->as.chan;
    Task *self = sch_self;
    pthread_mutex_lock(&sch_mu);
    if (c->len > 0) {              /* take from the buffer, then refill it */
        Value v = c->buf[c->head];
        c->head = (c->head + 1) % c->cap;
        c->len--;
        Task *s = chanq_pop(&c->sendq, &c->sendq_tail);
        if (s) {
            c->buf[(c->head + c->len) % c->cap] = s->xfer;
            c->len++;
            gc_write_barrier(o, s->xfer);
            sch_wake(s);
        }
        pthread_mutex_unlock(&sch_mu);
        return v;
    }
    Task *s = chanq_pop(&c->sendq, &c->sendq_tail);
    if (s) {                       /* unbuffered rendezvous */
        Value v = s->xfer;
        sch_wake(s);
        pthread_mutex_unlock(&sch_mu);
        return v;
    }
    if (c->closed) {               /* drained and closed: None, forever */
        pthread_mutex_unlock(&sch_mu);
        return em_none();
    }
    self->xfer = em_none();
    self->xfer_ok = false;
    chanq_push(&c->recvq, &c->recvq_tail, self);
    sch_block(self);
    Value v = self->xfer_ok ? self->xfer : em_none();
    self->xfer = em_none();
    pthread_mutex_unlock(&sch_mu);
    return v;
}

void em_close(Value chv) {
    Obj *o = as_chan(chv, "close");
    Chan *c = o->as.chan;
    pthread_mutex_lock(&sch_mu);
    if (c->closed) {
        pthread_mutex_unlock(&sch_mu);
        rt_fatal("close of an already closed channel");
    }
    c->closed = true;
    for (Task *t; (t = chanq_pop(&c->recvq, &c->recvq_tail)) != NULL; ) {
        t->xfer_ok = false;        /* woken receivers see the drained channel */
        sch_wake(t);
    }
    for (Task *t; (t = chanq_pop(&c->sendq, &c->sendq_tail)) != NULL; ) {
        t->send_failed = true;     /* a parked send on a closed channel is fatal */
        sch_wake(t);
    }
    pthread_mutex_unlock(&sch_mu);
}

Value em_chan_len(Value chv) {
    Obj *o = as_chan(chv, "chan_len");
    pthread_mutex_lock(&sch_mu);
    int64_t n = (int64_t)o->as.chan->len;
    pthread_mutex_unlock(&sch_mu);
    return em_int(n);
}

/* --- spawn / join / yield / sleep ---------------------------------------- */
static void *task_trampoline(void *arg) {
    Task *self = arg;
    sch_self = self;
    rt_cur_file = self->src_file;
    pthread_mutex_lock(&sch_mu);
    self->roots = &rt_roots;       /* this thread's own shadow stack */
    sch_acquire(self);             /* wait our turn before touching the heap */
    pthread_mutex_unlock(&sch_mu);

    Value fn = self->fn;
    RootFrame fr;
    rt_push_frame(&fr, &self->result, 1);
    self->result = em_call(fn, 0);
    rt_pop_frame();

    pthread_mutex_lock(&sch_mu);
    self->state = T_DONE;
    self->fn = em_none();
    for (Task *t = self->joinq; t; ) {
        Task *n = t->qnext;
        t->xfer = self->result;
        t->xfer_ok = true;
        sch_wake(t);
        t = n;
    }
    self->joinq = NULL;
    /* Leave the all-tasks list: the Task struct now only has to stay alive
     * for whoever still holds the handle, and the GC owns that decision. */
    for (Task **link = &sch_all; *link; link = &(*link)->next)
        if (*link == self) { *link = self->next; break; }
    self->next = NULL;
    self->roots = NULL;
    sch_alive--;
    sch_release();
    pthread_mutex_unlock(&sch_mu);
    return NULL;
}

Value em_spawn(Value fn) {
    if (!(fn.tag == V_OBJ && fn.as.o->tag == O_FUNC))
        rt_fatal("spawn() expects a function, got %s", type_name(fn));
    if (fn.as.o->as.func.arity != 0)
        rt_fatal("spawn() expects a function of no arguments, got one of %zu",
                 fn.as.o->as.func.arity);
    Task *t = xmalloc(sizeof(Task));
    memset(t, 0, sizeof *t);
    t->state = T_RUNNABLE;
    t->fn = fn;
    t->result = t->xfer = em_none();
    t->src_file = rt_cur_file;
    /* Allocate the handle before the thread exists: rt_obj_new() may collect,
     * and a half-built task must not be reachable when it does. */
    Obj *o = rt_obj_new(O_TASK);
    o->as.task = t;
    t->handle = o;
    obj_charge(o, sizeof(Task));

    pthread_mutex_lock(&sch_mu);
    t->roots = &rt_roots;          /* replaced by the child with its own */
    t->next = sch_all;
    sch_all = t;
    sch_alive++;
    sch_spawned++;
    runq_push(t);                  /* runnable, but the spawner keeps running */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t->thread, &attr, task_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_unlock(&sch_mu);
        rt_fatal("could not start task: %s", strerror(rc));
    }
    pthread_mutex_unlock(&sch_mu);
    return obj_val(o);
}

Value em_join(Value tv) {
    Obj *o = as_task(tv, "join");
    Task *t = o->as.task;
    Task *self = sch_self;
    if (t == self) rt_fatal("join() on the running task would deadlock");
    pthread_mutex_lock(&sch_mu);
    if (t->state == T_DONE) {
        Value r = t->result;
        pthread_mutex_unlock(&sch_mu);
        return r;
    }
    self->qnext = t->joinq;        /* the join queue is a plain stack */
    t->joinq = self;
    self->state = T_BLOCKED;
    sch_release();
    while (self->state != T_RUNNABLE) pthread_cond_wait(&sch_cv, &sch_mu);
    sch_acquire(self);
    Value r = self->xfer;
    self->xfer = em_none();
    pthread_mutex_unlock(&sch_mu);
    return r;
}

/* Observability, mirroring gc_stats(): how many tasks have been started, how
 * many are still alive, and how many times the token has changed hands. */
Value em_task_stats(void) {
    pthread_mutex_lock(&sch_mu);
    int64_t spawned = (int64_t)sch_spawned, alive = (int64_t)sch_alive,
            switches = (int64_t)sch_switches;
    pthread_mutex_unlock(&sch_mu);
    return em_rec_litn(3, "spawned", em_int(spawned), "alive", em_int(alive),
                       "switches", em_int(switches));
}

Value em_task_done(Value tv) {
    Obj *o = as_task(tv, "task_done");
    pthread_mutex_lock(&sch_mu);
    bool done = o->as.task->state == T_DONE;
    pthread_mutex_unlock(&sch_mu);
    return em_bool(done);
}

void em_yield(void) {
    Task *self = sch_self;
    pthread_mutex_lock(&sch_mu);
    if (sch_runq) sch_yield_locked(self);   /* nobody waiting: stay put */
    pthread_mutex_unlock(&sch_mu);
}

void em_sleep(Value secs) {
    double s = secs.tag == V_FLOAT ? secs.as.f
             : secs.tag == V_INT   ? (double)secs.as.i
             : (rt_fatal("sleep() expects a number, got %s", type_name(secs)), 0.0);
    if (s < 0) s = 0;
    Task *self = sch_self;
    double deadline = mono_now() + s;
    pthread_mutex_lock(&sch_mu);
    self->state = T_SLEEPING;
    sch_sleepers++;
    sch_release();
    for (;;) {
        double left = deadline - mono_now();
        if (left <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)left;
        ts.tv_nsec += (long)((left - (double)(time_t)left) * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&sch_cv, &sch_mu, &ts);
    }
    sch_sleepers--;
    self->state = T_RUNNABLE;
    runq_push(self);
    sch_acquire(self);
    pthread_mutex_unlock(&sch_mu);
}

/* --- higher-order list builtins ------------------------------------------ */
Value em_map(Value fn, Value xs) {
    if (!(xs.tag == V_OBJ && xs.as.o->tag == O_LIST))
        rt_fatal("map() expects a list, got %s", type_name(xs));
    Obj *src = xs.as.o;
    size_t n = src->as.list.len;
    Obj *dst = list_new(n);
    /* the result is traced by the GC from here on, so every slot must hold a
     * real Value before the mapping function can allocate */
    for (size_t i = 0; i < n; i++) dst->as.list.items[i] = em_none();
    dst->as.list.len = n;
    Value result = obj_val(dst);
    RootFrame fr;
    rt_push_frame(&fr, &result, 1); /* root the result while fn may allocate */
    for (size_t i = 0; i < n; i++) {
        Value a[1] = { src->as.list.items[i] };
        RootFrame af;
        rt_push_frame(&af, a, 1);
        Value r = call_closure_n(fn, a, 1);
        rt_pop_frame();
        dst->as.list.items[i] = r;
        gc_write_barrier(dst, r);
    }
    rt_pop_frame();
    return result;
}

Value em_filter(Value fn, Value xs) {
    if (!(xs.tag == V_OBJ && xs.as.o->tag == O_LIST))
        rt_fatal("filter() expects a list, got %s", type_name(xs));
    Obj *src = xs.as.o;
    Value result = obj_val(list_new(0));
    RootFrame fr;
    rt_push_frame(&fr, &result, 1);
    for (size_t i = 0; i < src->as.list.len; i++) {
        Value a[1] = { src->as.list.items[i] };
        RootFrame af;
        rt_push_frame(&af, a, 1);
        Value keep = call_closure_n(fn, a, 1);
        rt_pop_frame();
        if (em_truthy(keep)) em_append(result, a[0]);
    }
    rt_pop_frame();
    return result;
}

Value em_reduce(Value fn, Value acc, Value xs) {
    if (!(xs.tag == V_OBJ && xs.as.o->tag == O_LIST))
        rt_fatal("reduce() expects a list, got %s", type_name(xs));
    Obj *src = xs.as.o;
    RootFrame fr;
    rt_push_frame(&fr, &acc, 1);
    for (size_t i = 0; i < src->as.list.len; i++) {
        Value args[2] = { acc, src->as.list.items[i] };
        RootFrame af;
        rt_push_frame(&af, args, 2);
        Value r = call_closure_n(fn, args, 2);
        rt_pop_frame();
        acc = r;
    }
    rt_pop_frame();
    return acc;
}

/* `f >> g`: a closure h(x) = g(f(x)) */
static Value compose_tramp(Value *env, Value *args) {
    Value fx = call_closure_n(env[0], args, 1);
    RootFrame fr;
    rt_push_frame(&fr, &fx, 1); /* root fx while g may allocate */
    Value r = call_closure_n(env[1], &fx, 1);
    rt_pop_frame();
    return r;
}

Value em_compose_or_rshift(Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT) return em_rshift(a, b);
    return em_compose(a, b);
}

Value em_compose(Value f, Value g) {
    Value env[2] = { f, g };
    return em_mkclosure(compose_tramp, 1, env, 2, NULL, 0);
}
