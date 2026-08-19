/* Emerald module loader: resolves `import` statements to files and links the
 * whole import graph into a single Program.
 */
#ifndef EMERALD_MODULE_H
#define EMERALD_MODULE_H

#include "ast.h"
#include "diag.h"

/* Loads `entry` together with every module it imports (transitively) and
 * links them into one Program: dependencies first, the entry module last.
 * Each imported module's top-level names are mangled to `<module>__<name>`
 * so two packages can both define `parse`; the entry module keeps its names.
 *
 * `roots` are the extra `-I` search directories, in command-line order. Every
 * module's source text is registered with `diags`, so a diagnostic against any
 * file can still quote its line. On failure the diagnostics are in `diags`,
 * `*errors` is non-zero, and the return value is NULL.
 */
Program *module_link(const char *entry, const char *const *roots, size_t nroots,
                     DiagList *diags, int *errors);

/* A stdlib search root derived from the executable's location (RELEASE_V1 B3),
 * tried after $EMERALD_STDLIB and before the compile-time default. NULL clears
 * it. The pointer must stay valid for the lifetime of the process. */
void module_set_exe_stdlib(const char *path);

#endif
