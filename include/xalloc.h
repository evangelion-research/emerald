/* The compiler's one allocation policy: running out of memory is fatal.
 *
 * Header-only `static inline`, so every translation unit can use it with no
 * link dependency — including src/dim.c, which is unit-tested on its own.
 * (src/runtime.c deliberately keeps its own pair: a program that runs out of
 * memory at run time reports through rt_fatal, with a source location.)
 *
 * A zero-byte request allocates one byte, so the result is always a distinct
 * non-NULL pointer and callers never have to special-case an empty array.
 */
#ifndef EMERALD_XALLOC_H
#define EMERALD_XALLOC_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static inline void xoom(void) {
    fputs("emeraldc: out of memory\n", stderr);
    exit(1);
}

static inline void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) xoom();
    return p;
}

static inline void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size);
    if (!p) xoom();
    return p;
}

static inline void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) xoom();
    return q;
}

#endif
