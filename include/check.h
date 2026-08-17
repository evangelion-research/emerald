#ifndef EMERALD_CHECK_H
#define EMERALD_CHECK_H

#include "ast.h"
#include "diag.h"

/* Type-checks the program, collecting structured diagnostics (error codes,
 * line/column, expected/actual types) into `diags`. Returns the number of
 * errors (0 = clean).
 *
 * `proof` enables proof mode (--proof): `any` and `partial` are banned, so a
 * clean check means every value has a static type and every function is total.
 */
int check_program(const Program *prog, const char *filename, DiagList *diags,
                  bool proof);

#endif
