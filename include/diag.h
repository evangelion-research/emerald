/* Emerald diagnostics: structured, LLM-friendly error reporting.
 *
 * Every compile-time problem (lexer, parser, type checker) is collected as a
 * `Diag` record carrying a stable machine-readable code, a source location
 * (file, line, column), a human message, and optional structured notes — in
 * particular the `expected`/`actual` types of a mismatch. A `DiagList` owns
 * the collected diagnostics and the source text, and can render them either
 * as human-readable text (with the offending source line and a caret) or as
 * JSON, so the output can be fed back into an LLM to fix the program.
 */
#ifndef EMERALD_DIAG_H
#define EMERALD_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    DIA_SYNTAX,    /* lexer / parser */
    DIA_TYPE,      /* type checker */
    DIA_INTERNAL,  /* compiler bug */
} DiagKind;

typedef enum {
    SEV_ERROR,
    SEV_WARNING,
    SEV_NOTE,
} DiagSeverity;

/* A free-form structured note: a label and its value (often a type). */
typedef struct {
    char *label;
    char *value;
} DiagNote;

typedef struct Diag {
    DiagKind kind;
    DiagSeverity severity;
    const char *code;    /* stable machine-readable code, e.g. "E_TYPE_ASSIGN" */
    const char *file;
    int line;
    int col;             /* 1-based; 0 when unknown */
    char *message;
    char *expected;      /* optional: the expected type, as a string */
    char *actual;        /* optional: the actual type, as a string */
    DiagNote *notes;     /* additional structured notes */
    size_t note_count, note_cap;
} Diag;

/* A file's text, kept so a diagnostic can quote the offending line. */
typedef struct {
    const char *file;
    const char *src;
} DiagSource;

typedef struct {
    Diag *items;
    size_t count, cap;
    const char *src;   /* borrowed source of the entry file (fallback) */
    DiagSource *sources; /* per-file sources, for multi-module programs */
    size_t source_count, source_cap;
    bool json;         /* render as JSON rather than human text */
    bool werror;       /* --werror: promote warnings to errors */
    const char **suppress; /* -Wno-<code>: warning codes to silence */
    size_t suppress_count;
} DiagList;

void diag_init(DiagList *dl, const char *src);

/* Register `src` as the text of `file`, so diagnostics against that file can
 * quote their source line. Both pointers are borrowed and must outlive the
 * DiagList. Registering the same file twice keeps the first registration. */
void diag_add_source(DiagList *dl, const char *file, const char *src);

/* Append a diagnostic and return it so callers can attach expected/actual. */
Diag *diag_add(DiagList *dl, DiagKind kind, const char *code,
               const char *file, int line, int col, const char *fmt, ...);

/* Append a diagnostic with an explicit severity (a warning, not an error). */
Diag *diag_add_sev(DiagList *dl, DiagKind kind, DiagSeverity sev,
                   const char *code, const char *file, int line, int col,
                   const char *fmt, ...);

/* Has `-Wno-code` silenced this warning code? */
bool diag_suppressed(const DiagList *dl, const char *code);

/* Number of SEV_WARNING diagnostics currently collected. */
size_t diag_warning_count(const DiagList *dl);

/* Set the structured expected/actual types on a diagnostic (copies the
 * strings, so a rotating buffer source is safe). */
void diag_set_types(Diag *d, const char *expected, const char *actual);
/* Attach an arbitrary structured note. */
void diag_note(Diag *d, const char *label, const char *value);

void diag_render(const DiagList *dl, FILE *out);

#endif
