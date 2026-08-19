/* The Emerald REPL: an interactive front end over the ordinary compiler.
 *
 * Emerald compiles ahead of time, so there is nothing to evaluate an
 * expression *into*. The REPL therefore keeps the session as source text and
 * rebuilds the whole program on every entry, showing only the output the new
 * entry added (see src/repl.c for what that costs and when it lies).
 */
#ifndef EMERALD_REPL_H
#define EMERALD_REPL_H

#include <stddef.h>

/* Runs the read-eval-print loop until EOF or `:quit`. `self` is the path to
 * this compiler binary (argv[0]) — the REPL re-invokes it to build each entry.
 * `roots`/`nroots` are the -I search directories, passed through unchanged.
 * Returns the process exit status. */
int repl_run(const char *self, const char *const *roots, size_t nroots);

#endif
