/* Runtime: first-class functions, closure environments, and indirect calls. */
#include "runtime_internal.h"

Value em_mkclosure(Value (*fn)(Value *env, Value *args), size_t arity,
                   Value *env, size_t env_count, Value *defaults,
                   size_t default_count) {
    RootFrame env_frame;
    RootFrame defaults_frame;

    rt_push_frame(&env_frame, env, env_count);
    if (default_count)
        rt_push_frame(&defaults_frame, defaults, default_count);

    Obj *closure = rt_obj_new(O_FUNC);
    closure->as.func.fn = fn;
    closure->as.func.arity = arity;
    closure->as.func.default_count = default_count;
    closure->as.func.min_arity = arity - default_count;

    if (default_count) {
        closure->as.func.defaults = xmalloc(sizeof(Value) * default_count);
        memcpy(closure->as.func.defaults, defaults,
               sizeof(Value) * default_count);
        obj_charge(closure, sizeof(Value) * default_count);
    } else {
        closure->as.func.defaults = NULL;
    }

    if (env_count) {
        closure->as.func.env = xmalloc(sizeof(Value) * env_count);
        memcpy(closure->as.func.env, env, sizeof(Value) * env_count);
        closure->as.func.env_count = env_count;
        obj_charge(closure, sizeof(Value) * env_count);
    } else {
        closure->as.func.env = NULL;
        closure->as.func.env_count = 0;
    }

    if (default_count)
        rt_pop_frame();
    rt_pop_frame();
    return obj_val(closure);
}

Value em_cell(Value value) {
    Obj *cell = rt_obj_new(O_CELL);
    cell->as.cell.val = value;
    return obj_val(cell);
}

Value em_cell_get(Value cell) {
    if (!(cell.tag == V_OBJ && cell.as.o->tag == O_CELL))
        rt_fatal("internal: em_cell_get on a non-cell");
    return cell.as.o->as.cell.val;
}

void em_cell_set(Value cell, Value value) {
    if (!(cell.tag == V_OBJ && cell.as.o->tag == O_CELL))
        rt_fatal("internal: em_cell_set on a non-cell");

    Obj *object = cell.as.o;
    object->as.cell.val = value;
    gc_write_barrier(object, value);
}

static void check_call_arity(const Obj *closure, size_t argc) {
    if (argc < closure->as.func.min_arity || argc > closure->as.func.arity)
        rt_fatal("function expects %zu..%zu argument(s), got %zu",
                 closure->as.func.min_arity, closure->as.func.arity, argc);
}

Value em_call(Value fn, size_t argc, ...) {
    if (!(fn.tag == V_OBJ && fn.as.o->tag == O_FUNC))
        rt_fatal("attempt to call a value of type %s", type_name(fn));

    Obj *closure = fn.as.o;
    check_call_arity(closure, argc);

    size_t total = closure->as.func.arity;
    Value *args = xmalloc(sizeof(Value) * (total ? total : 1));
    va_list ap;
    va_start(ap, argc);
    for (size_t i = 0; i < argc; i++)
        args[i] = va_arg(ap, Value);
    va_end(ap);

    for (size_t i = argc; i < total; i++) {
        size_t default_index = i - closure->as.func.min_arity;
        args[i] = closure->as.func.defaults[default_index];
    }

    RootFrame args_frame;
    rt_push_frame(&args_frame, args, total);
    Value result = closure->as.func.fn(closure->as.func.env, args);
    rt_pop_frame();
    free(args);
    return result;
}

/* Call a closure with arguments already packed in a caller-owned array. */
Value call_closure_n(Value fn, Value *args, size_t argc) {
    if (!(fn.tag == V_OBJ && fn.as.o->tag == O_FUNC))
        rt_fatal("attempt to call a value of type %s", type_name(fn));

    Obj *closure = fn.as.o;
    check_call_arity(closure, argc);
    if (argc == closure->as.func.arity)
        return closure->as.func.fn(closure->as.func.env, args);

    size_t total = closure->as.func.arity;
    Value *full_args = xmalloc(sizeof(Value) * total);
    memcpy(full_args, args, sizeof(Value) * argc);
    for (size_t i = argc; i < total; i++) {
        size_t default_index = i - closure->as.func.min_arity;
        full_args[i] = closure->as.func.defaults[default_index];
    }

    Value result = closure->as.func.fn(closure->as.func.env, full_args);
    free(full_args);
    return result;
}
