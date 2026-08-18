/* Direct unit harness for the dim-expression solver (include/dim.h). This is
 * the first non-golden test in the project: it compiles src/dim.c standalone
 * and asserts the normalizer's invariants, so a silent solver bug cannot hide
 * behind a golden diff. Exit status 0 = pass. */
#include "dim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fails++;                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

static void check_str(DimExpr *e, const char *want) {
    char *got = dim_str(e);
    if (strcmp(got, want) != 0) {
        fails++;
        printf("FAIL dim_str: got \"%s\", want \"%s\"\n", got, want);
    }
    free(got);
}

int main(void) {
    DimExpr *b = dim_var("B");
    DimExpr *s = dim_var("S");
    DimExpr *d = dim_var("D");

    /* commutativity of multiplication */
    CHECK(dim_eq(dim_mul(b, s), dim_mul(s, b)));

    /* distributivity: 2*(B + 3) == 2*B + 6 */
    CHECK(dim_eq(dim_mul(dim_lit(2), dim_add(b, dim_lit(3))),
                 dim_add(dim_mul(dim_lit(2), b), dim_lit(6))));

    /* (B + 1) * (B + 1) == B*B + 2*B + 1 */
    CHECK(dim_eq(dim_mul(dim_add(b, dim_lit(1)), dim_add(b, dim_lit(1))),
                 dim_add(dim_mul(b, b),
                         dim_add(dim_mul(dim_lit(2), b), dim_lit(1)))));

    /* B*(S + D) == B*S + B*D */
    CHECK(dim_eq(dim_mul(b, dim_add(s, d)),
                 dim_add(dim_mul(b, s), dim_mul(b, d))));

    /* literals */
    CHECK(dim_eq(dim_lit(7), dim_lit(7)));
    CHECK(!dim_eq(dim_lit(7), dim_lit(8)));

    /* a variable is not a literal, and different names are distinct */
    CHECK(!dim_eq(b, dim_lit(1)));
    CHECK(!dim_eq(b, s));

    /* canonical string forms */
    check_str(dim_lit(5), "5");
    check_str(dim_var("B"), "B");
    check_str(dim_mul(dim_lit(2), dim_var("B")), "2*B");
    check_str(dim_add(dim_var("B"), dim_lit(1)), "1 + B");
    check_str(dim_add(dim_mul(dim_var("B"), dim_var("S")),
                      dim_mul(dim_lit(2), dim_var("D"))),
              "2*D + B*S");

    /* dim_le: the decidable fragment */
    CHECK(dim_le(b, b) == 1);                       /* equal */
    CHECK(dim_le(dim_lit(3), dim_lit(5)) == 1);     /* literal comparison */
    CHECK(dim_le(dim_lit(5), dim_lit(3)) == 0);
    CHECK(dim_le(b, dim_add(b, dim_lit(3))) == 1);  /* a <= a + k */
    CHECK(dim_le(dim_add(b, dim_lit(3)), b) == 0);  /* a + k <= a is false */
    CHECK(dim_le(b, dim_add(b, dim_lit(0))) == 1);  /* a <= a + 0 */
    /* outside the fragment: unknown, and recorded in the escalation log */
    size_t before = dim_unresolved_count();
    CHECK(dim_le(b, s) == -1);
    CHECK(dim_le(dim_mul(dim_lit(2), b), dim_lit(9)) == -1);
    CHECK(dim_unresolved_count() == before + 2);

    dim_reset_unresolved();
    CHECK(dim_unresolved_count() == 0);

    /* dim_free must not crash on a non-trivial tree */
    dim_free(dim_mul(dim_add(b, s), dim_add(d, dim_lit(1))));

    if (fails) {
        printf("%d dim solver check(s) failed\n", fails);
        return 1;
    }
    printf("dim solver: all checks passed\n");
    return 0;
}
