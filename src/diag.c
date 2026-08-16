/* Structured diagnostics: collection and rendering (human + JSON). */
#include "diag.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void diag_init(DiagList *dl, const char *src) {
    memset(dl, 0, sizeof(*dl));
    dl->src = src;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    memcpy(p, s, n);
    return p;
}

void diag_free(DiagList *dl) {
    for (size_t i = 0; i < dl->count; i++) {
        Diag *d = &dl->items[i];
        free(d->message);
        free(d->expected);
        free(d->actual);
        for (size_t j = 0; j < d->note_count; j++) {
            free(d->notes[j].label);
            free(d->notes[j].value);
        }
        free(d->notes);
    }
    free(dl->items);
    memset(dl, 0, sizeof(*dl));
}

static Diag *diag_addv(DiagList *dl, DiagKind kind, const char *code,
                       const char *file, int line, int col,
                       const char *fmt, va_list ap) {
    if (dl->count == dl->cap) {
        dl->cap = dl->cap ? dl->cap * 2 : 16;
        dl->items = realloc(dl->items, sizeof(Diag) * dl->cap);
        if (!dl->items) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    Diag *d = &dl->items[dl->count++];
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    d->severity = SEV_ERROR;
    d->code = code;
    d->file = file;
    d->line = line;
    d->col = col;

    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    d->message = malloc((size_t)n + 1);
    if (!d->message) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    vsnprintf(d->message, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return d;
}

Diag *diag_add(DiagList *dl, DiagKind kind, const char *code,
               const char *file, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Diag *d = diag_addv(dl, kind, code, file, line, col, fmt, ap);
    va_end(ap);
    return d;
}

void diag_set_types(Diag *d, const char *expected, const char *actual) {
    if (expected) d->expected = xstrdup(expected);
    if (actual) d->actual = xstrdup(actual);
}

void diag_note(Diag *d, const char *label, const char *value) {
    if (d->note_count == d->note_cap) {
        d->note_cap = d->note_cap ? d->note_cap * 2 : 4;
        d->notes = realloc(d->notes, sizeof(DiagNote) * d->note_cap);
        if (!d->notes) { fputs("emeraldc: out of memory\n", stderr); exit(1); }
    }
    d->notes[d->note_count].label = xstrdup(label);
    d->notes[d->note_count].value = xstrdup(value);
    d->note_count++;
}

int diag_error_count(const DiagList *dl) {
    int n = 0;
    for (size_t i = 0; i < dl->count; i++)
        if (dl->items[i].severity == SEV_ERROR) n++;
    return n;
}

/* Extract the text of source line `line` (1-based). Returns a pointer into
 * src and sets *len; returns NULL if the line is out of range. */
static const char *src_line(const char *src, int line, size_t *len) {
    if (!src || line < 1) return NULL;
    const char *p = src;
    for (int i = 1; i < line; i++) {
        p = strchr(p, '\n');
        if (!p) return NULL;
        p++;
    }
    const char *end = strchr(p, '\n');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n && p[n - 1] == '\r') n--; /* tolerate CRLF */
    *len = n;
    return p;
}

/* --- JSON rendering ------------------------------------------------------ */

static void json_str(FILE *out, const char *s) {
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        default:
            if (c < 0x20) fprintf(out, "\\u%04x", c);
            else fputc(c, out);
        }
    }
    fputc('"', out);
}

static const char *kind_name(DiagKind k) {
    switch (k) {
    case DIA_SYNTAX:   return "syntax";
    case DIA_TYPE:     return "type";
    case DIA_INTERNAL: return "internal";
    }
    return "unknown";
}

static const char *sev_name(DiagSeverity s) {
    switch (s) {
    case SEV_ERROR:   return "error";
    case SEV_WARNING: return "warning";
    case SEV_NOTE:    return "note";
    }
    return "error";
}

static void render_json(const DiagList *dl, FILE *out) {
    fputs("[", out);
    for (size_t i = 0; i < dl->count; i++) {
        const Diag *d = &dl->items[i];
        size_t slen = 0;
        const char *sline = src_line(dl->src, d->line, &slen);

        fprintf(out, "%s\n  {\n", i ? "," : "");
        fputs("    \"kind\": ", out);
        json_str(out, kind_name(d->kind));
        fputs(",\n    \"severity\": ", out);
        json_str(out, sev_name(d->severity));
        fputs(",\n    \"code\": ", out);
        json_str(out, d->code);
        fputs(",\n    \"file\": ", out);
        json_str(out, d->file ? d->file : "");
        fprintf(out, ",\n    \"line\": %d,\n    \"column\": %d", d->line, d->col);
        fputs(",\n    \"message\": ", out);
        json_str(out, d->message);
        if (d->expected) {
            fputs(",\n    \"expected\": ", out);
            json_str(out, d->expected);
        }
        if (d->actual) {
            fputs(",\n    \"actual\": ", out);
            json_str(out, d->actual);
        }
        if (sline) {
            fputs(",\n    \"source_line\": ", out);
            fputc('"', out);
            for (size_t k = 0; k < slen; k++) {
                char c = sline[k];
                switch (c) {
                case '"':  fputs("\\\"", out); break;
                case '\\': fputs("\\\\", out); break;
                case '\t': fputs("\\t", out); break;
                case '\r': fputs("\\r", out); break;
                default:
                    if ((unsigned char)c < 0x20) fprintf(out, "\\u%04x", (unsigned char)c);
                    else fputc(c, out);
                }
            }
            fputc('"', out);
        }
        if (d->note_count) {
            fputs(",\n    \"notes\": [", out);
            for (size_t j = 0; j < d->note_count; j++) {
                fprintf(out, "%s\n      { \"label\": ", j ? "," : "");
                json_str(out, d->notes[j].label);
                fputs(", \"value\": ", out);
                json_str(out, d->notes[j].value);
                fputs(" }", out);
            }
            fputs("\n    ]", out);
        }
        fputs("\n  }", out);
    }
    if (dl->count) fputs("\n", out);
    fputs("]\n", out);
}

/* --- human rendering ----------------------------------------------------- */

static void render_human(const DiagList *dl, FILE *out) {
    for (size_t i = 0; i < dl->count; i++) {
        const Diag *d = &dl->items[i];
        const char *sev = d->severity == SEV_ERROR ? "error"
                        : d->severity == SEV_WARNING ? "warning" : "note";
        fprintf(out, "%s[%s]: %s\n", sev, d->code, d->message);
        fprintf(out, "  --> %s:%d:%d\n", d->file ? d->file : "", d->line, d->col);

        size_t slen = 0;
        const char *sline = src_line(dl->src, d->line, &slen);
        if (sline) {
            int gutter = snprintf(NULL, 0, "%d", d->line);
            fprintf(out, "%*s |\n", gutter + 2, "");
            fprintf(out, " %d | %.*s\n", d->line, (int)slen, sline);
            int col = d->col > 0 ? d->col : 1;
            fprintf(out, " %*s | %*s^\n", gutter, "", col - 1, "");
        }
        if (d->expected) fprintf(out, "   = expected: %s\n", d->expected);
        if (d->actual)   fprintf(out, "   = actual:   %s\n", d->actual);
        for (size_t j = 0; j < d->note_count; j++)
            fprintf(out, "   = %s: %s\n", d->notes[j].label, d->notes[j].value);
    }
}

void diag_render(const DiagList *dl, FILE *out) {
    if (dl->json) render_json(dl, out);
    else render_human(dl, out);
}
