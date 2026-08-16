#ifndef EMERALD_CODEGEN_H
#define EMERALD_CODEGEN_H

#include "ast.h"
#include <stdio.h>

/* Emits a complete C translation unit for the program (expects to be
 * compiled together with src/runtime.c). The program must already have
 * passed the type checker. `filename` is embedded so runtime errors can
 * report their source location.
 */
void codegen_program(FILE *out, const Program *prog, const char *filename);

#endif
