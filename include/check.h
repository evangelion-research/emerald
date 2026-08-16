#ifndef EMERALD_CHECK_H
#define EMERALD_CHECK_H

#include "ast.h"
#include "diag.h"

/* Type-checks the program, collecting structured diagnostics (error codes,
 * line/column, expected/actual types) into `diags`. Returns the number of
 * errors (0 = clean).
 */
int check_program(const Program *prog, const char *filename, DiagList *diags);

#endif
