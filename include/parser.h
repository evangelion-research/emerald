#ifndef EMERALD_PARSER_H
#define EMERALD_PARSER_H

#include "ast.h"
#include "diag.h"

/* Parses the whole source, collecting syntax diagnostics into `diags`. On a
 * syntax error, renders the diagnostics and exits with status 1.
 */
Program *parse_program(const char *src, const char *filename, DiagList *diags);

#endif
