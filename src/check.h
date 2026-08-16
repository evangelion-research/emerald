#ifndef EMERALD_CHECK_H
#define EMERALD_CHECK_H

#include "ast.h"

/* Type-checks the program. Prints `file:line: type error: ...` for each
 * problem and returns the number of errors (0 = clean).
 */
int check_program(const Program *prog, const char *filename);

#endif
