#ifndef EMERALD_PARSER_H
#define EMERALD_PARSER_H

#include "ast.h"

/* Parses the whole source. On a syntax error, prints `file:line: syntax
 * error: ...` to stderr and exits with status 1.
 */
Program *parse_program(const char *src, const char *filename);

#endif
