/* The Emerald REPL.
 *
 * Emerald has no interpreter: the only thing that can run an expression is a
 * compiled binary. So the session *is* its source text. Every entry appends to
 * that text and the whole program is compiled and run again — the earlier
 * lines re-run underneath, and only the new entry's output is shown.
 *
 * "Only the new entry's output" is not a diff of lengths, which nondeterminism
 * would garble; it is a sentinel. The generated program prints one improbable
 * marker line between the committed session and the entry, and the REPL shows
 * whatever follows the last marker in the output. An earlier line that prints
 * the time therefore stays invisible however much its text changes.
 *
 * The one cost that cannot be engineered away is that effects repeat: a
 * session that writes a file writes it once per entry. That is the honest
 * price of an ahead-of-time language with no interpreter to hold state.
 *
 * A single-line expression is echoed by wrapping it in print(), the way an
 * interpreter would show its value; a result of None prints nothing, so a call
 * kept for its effect stays quiet. The line itself is still committed either
 * way, so its effects survive into the next entry.
 *
 * An entry is committed only if it both compiles and runs to a zero status, so
 * a typo or an abort leaves the session exactly as it was.
 */
#define _POSIX_C_SOURCE 200809L

#include "repl.h"
#include "xalloc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- interactive startup ------------------------------------------------ */

static void repl_intro(void) {
    static const char *const shades[] = { ".:-=", ":-=+", "-=+*", "=+*#" };
    static const char *const rows[] = {
        "             %c             ",
        "          %c%c%c%c%c%c%c          ",
        "       %c%c%c%c%c%c%c%c%c%c%c%c%c       ",
        "    %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c    ",
        "       %c%c%c%c%c%c%c%c%c%c%c%c%c       ",
        "          %c%c%c%c%c%c%c          ",
        "             %c             ",
    };
    const size_t nrows = sizeof rows / sizeof rows[0];
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 55000000L };

    fputs("\x1b[?25l\x1b[38;2;20;220;120m", stdout);
    for (int frame = 0; frame < 12; frame++) {
        const char *s = shades[frame % 4];
        size_t cell = 0;
        for (size_t row = 0; row < nrows; row++) {
            for (const char *p = rows[row]; *p; p++) {
                if (*p == '%' && p[1] == 'c') {
                    putchar(s[(cell++ + (size_t)frame) % 4]);
                    p++;
                } else {
                    putchar(*p);
                }
            }
            putchar('\n');
        }
        fflush(stdout);
        nanosleep(&pause, NULL);
        fprintf(stdout, "\x1b[%zuA", nrows);
    }
    for (size_t row = 0; row < nrows; row++) fputs("\x1b[2K\n", stdout);
    fprintf(stdout, "\x1b[%zuA\x1b[0m\x1b[?25h", nrows);
}

/* --- a growable list of owned lines -------------------------------------- */

typedef struct {
    char **v;
    size_t n, cap;
} Lines;

static void lines_push(Lines *l, const char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n] = xmalloc(strlen(s) + 1);
    strcpy(l->v[l->n], s);
    l->n++;
}

static void lines_free(Lines *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* --- scanning a line the way the lexer would ----------------------------- */

/* Brace depth and top-level `=` detection both need to ignore text inside
 * strings and comments, which is all the lexing the REPL itself has to do. */
typedef struct {
    int depth;      /* net {} + () + [] nesting across the entry so far */
    bool assigns;   /* an `=` that is not ==, !=, <=, >= appeared at depth 0 */
} Scan;

static void scan_line(Scan *sc, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '#') return;
        if (*p == '"' || *p == '\'') {
            char q = *p;
            for (p++; *p && *p != q; p++)
                if (*p == '\\' && p[1]) p++;
            if (!*p) return; /* unterminated: the parser will say so */
            continue;
        }
        if (*p == '{' || *p == '(' || *p == '[') sc->depth++;
        else if (*p == '}' || *p == ')' || *p == ']') sc->depth--;
        else if (*p == '=' && sc->depth == 0) {
            bool cmp = p[1] == '=' || (p != s && strchr("=!<>+-*/%", p[-1]));
            if (!cmp) sc->assigns = true;
            if (p[1] == '=') p++;
        }
    }
}

static const char *KEYWORDS[] = {
    "def", "import", "from", "export", "type", "dim", "if", "elif", "else",
    "while", "for", "return", "match", "case", "pass", "break", "continue",
    "pure", "struct", "enum", NULL,
};

static bool starts_with_keyword(const char *s) {
    for (int i = 0; KEYWORDS[i]; i++) {
        size_t k = strlen(KEYWORDS[i]);
        if (strncmp(s, KEYWORDS[i], k) == 0 && (!s[k] || isspace((unsigned char)s[k])))
            return true;
    }
    return false;
}

/* An entry whose value the REPL should echo: a single line that is an
 * expression rather than a statement. Calls count — a None result prints
 * nothing, so one made for its effect stays quiet by itself. */
static bool is_echoable(const Lines *entry, const Scan *sc) {
    if (entry->n != 1 || sc->assigns || sc->depth != 0) return false;
    const char *s = entry->v[0];
    while (*s == ' ' || *s == '\t') s++;
    return *s && !starts_with_keyword(s);
}

/* --- the session --------------------------------------------------------- */

/* The marker between the session's output and the entry's. It is printed as
 * an ordinary string, so it must be something no program prints by accident. */
#define REPL_MARK "\x01<<emerald-repl>>\x01"

typedef struct {
    Lines src;         /* every committed line, in order */
    char dir[256];     /* scratch directory for this session */
    char rald[300], bin[300], out[300], err[300];
    const char *self;  /* this compiler binary */
    char *iflags;      /* the -I flags, pre-quoted for the shell */
} Session;

/* Read a whole file; the caller frees. */
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = xmalloc(cap);
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (n + 1 >= cap) buf = xrealloc(buf, cap *= 2);
    }
    fclose(f);
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

static void cat_file(const char *path) {
    char *text = slurp(path, NULL);
    if (!text) return;
    fputs(text, stdout);
    free(text);
}

/* Show the run's output from the last marker onwards. `echoed` drops a final
 * "None" the way an interpreter does: a call made for its effect says nothing.
 */
static void show_output(const char *path, bool echoed) {
    size_t n;
    char *text = slurp(path, &n);
    if (!text) return;
    char *tail = text, *hit = text;
    while ((hit = strstr(hit, REPL_MARK)) != NULL) {
        hit += strlen(REPL_MARK);
        if (*hit == '\n') hit++;
        tail = hit;
    }
    size_t len = strlen(tail);
    if (echoed && len >= 5 && strcmp(tail + len - 5, "None\n") == 0)
        len -= 5;
    fwrite(tail, 1, len, stdout);
    fflush(stdout);
    free(text);
}

/* Write the session plus `entry` (optionally echoed) to the scratch .rald. */
static bool write_program(Session *s, const Lines *entry, bool echo) {
    FILE *f = fopen(s->rald, "w");
    if (!f) {
        fprintf(stderr, "emeraldc: cannot write '%s'\n", s->rald);
        return false;
    }
    for (size_t i = 0; i < s->src.n; i++) fprintf(f, "%s\n", s->src.v[i]);
    fprintf(f, "print(\"%s\")\n", REPL_MARK);
    if (echo) fprintf(f, "print(%s)\n", entry->v[0]);
    else
        for (size_t i = 0; i < entry->n; i++) fprintf(f, "%s\n", entry->v[i]);
    fclose(f);
    return true;
}

static int shell(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}

/* Compile the current scratch program; the compiler's own diagnostics land in
 * s->err so a failed entry can show them and change nothing else. */
static bool build(Session *s) {
    return shell("'%s' %s -o '%s' '%s' > '%s' 2>&1", s->self, s->iflags,
                 s->bin, s->rald, s->err) == 0;
}

/* --- evaluating one entry ------------------------------------------------ */

static void eval(Session *s, const Lines *entry, const Scan *sc) {
    bool echo = is_echoable(entry, sc);
    if (!write_program(s, entry, echo)) return;
    if (!build(s)) {
        /* An echoed entry may not be an expression at all (a lone type name,
         * say). Compile it as typed before blaming the user for the wrapper. */
        bool recovered = false;
        if (echo) {
            echo = false;
            recovered = write_program(s, entry, false) && build(s);
        }
        if (!recovered) {
            cat_file(s->err);
            return;
        }
    }

    if (shell("'%s' > '%s' 2>&1", s->bin, s->out) != 0) {
        show_output(s->out, echo);
        fprintf(stderr, "emeraldc: the program exited non-zero; entry discarded\n");
        return;
    }
    show_output(s->out, echo);
    /* The entry is committed as typed — never as its print() wrapper — so the
     * next run repeats its effects without repeating its echo. */
    for (size_t i = 0; i < entry->n; i++) lines_push(&s->src, entry->v[i]);
}

/* --- commands ------------------------------------------------------------ */

static void help(void) {
    puts(":help          this message\n"
         ":list          the session's source so far\n"
         ":undo          drop the last committed entry\n"
         ":reset         start an empty session\n"
         ":cancel        abandon a half-typed block\n"
         ":load FILE     read a .rald file into the session\n"
         ":save FILE     write the session's source to a file\n"
         ":quit          leave (Ctrl-D does too)\n"
         "\n"
         "A bare expression prints its value. Everything else — imports,\n"
         "defs, assignments, loops — is added to the session and re-run on\n"
         "each entry, so effects such as file writes repeat.");
}

static void list_session(const Session *s) {
    for (size_t i = 0; i < s->src.n; i++) printf("%s\n", s->src.v[i]);
}

static void load_file(Session *s, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "emeraldc: cannot open '%s'\n", path);
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (n && line[n - 1] == '\n') line[n - 1] = '\0';
        lines_push(&s->src, line);
    }
    free(line);
    fclose(f);
    printf("loaded %s\n", path);
}

static void save_file(const Session *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "emeraldc: cannot write '%s'\n", path);
        return;
    }
    for (size_t i = 0; i < s->src.n; i++) fprintf(f, "%s\n", s->src.v[i]);
    fclose(f);
    printf("saved %s\n", path);
}

/* true when the loop should end */
static bool command(Session *s, const char *line) {
    const char *arg = strchr(line, ' ');
    while (arg && *arg == ' ') arg++;
    if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) return true;
    else if (strcmp(line, ":help") == 0 || strcmp(line, ":h") == 0) help();
    else if (strcmp(line, ":list") == 0) list_session(s);
    else if (strcmp(line, ":undo") == 0) {
        if (s->src.n) free(s->src.v[--s->src.n]);
    } else if (strcmp(line, ":reset") == 0) {
        lines_free(&s->src);
    } else if (strncmp(line, ":load", 5) == 0 && arg) load_file(s, arg);
    else if (strncmp(line, ":save", 5) == 0 && arg) save_file(s, arg);
    else fprintf(stderr, "unknown command '%s' (try :help)\n", line);
    return false;
}

/* --- the loop ------------------------------------------------------------ */

int repl_run(const char *self, const char *const *roots, size_t nroots) {
    Session s = {0};
    s.self = self;
    snprintf(s.dir, sizeof s.dir, "/tmp/emerald-repl-%d", (int)getpid());
    shell("mkdir -p '%s'", s.dir);
    snprintf(s.rald, sizeof s.rald, "%s/session.rald", s.dir);
    snprintf(s.bin, sizeof s.bin, "%s/session", s.dir);
    snprintf(s.out, sizeof s.out, "%s/out.txt", s.dir);
    snprintf(s.err, sizeof s.err, "%s/err.txt", s.dir);

    size_t flen = 1;
    for (size_t i = 0; i < nroots; i++) flen += strlen(roots[i]) + 8;
    s.iflags = xmalloc(flen);
    s.iflags[0] = '\0';
    for (size_t i = 0; i < nroots; i++) {
        strcat(s.iflags, "-I '");
        strcat(s.iflags, roots[i]);
        strcat(s.iflags, "' ");
    }

    bool tty = isatty(0);
    if (tty) {
        repl_intro();
        puts("emerald repl — :help for commands, :quit to leave");
    }

    Lines entry = {0};
    Scan sc = {0};
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        if (tty) {
            fputs(entry.n ? "..... " : "emerald> ", stdout);
            fflush(stdout);
        }
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;
        if (n && line[n - 1] == '\n') line[n - 1] = '\0';

        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!entry.n && !*t) continue;
        if (*t == ':') {
            /* no Emerald line starts with a colon, so a command is always a
               command — including one that abandons a half-typed block */
            lines_free(&entry);
            sc = (Scan){0};
            if (strcmp(t, ":cancel") == 0) continue;
            if (command(&s, t)) break;
            continue;
        }
        lines_push(&entry, line);
        scan_line(&sc, line);
        if (sc.depth > 0) continue; /* an open brace: keep reading */
        eval(&s, &entry, &sc);
        lines_free(&entry);
        sc = (Scan){0};
    }
    free(line);
    lines_free(&entry);
    lines_free(&s.src);
    free(s.iflags);
    shell("rm -rf '%s'", s.dir);
    if (tty) puts("");
    return 0;
}
