/* emeraldc: the Emerald compiler driver.
 *
 *   emeraldc file.rald              compile to a native binary (./file)
 *   emeraldc -I dir ...             add a module search root (repeatable)
 *   emeraldc -o out file.rald       choose the output path
 *   emeraldc --emit-tokens f.rald   dump the token stream (lexer stage)
 *   emeraldc --emit-ast f.rald      dump the AST as s-expressions (parser stage)
 *   emeraldc --check f.rald         type-check only (checker stage)
 *   emeraldc --emit-c f.rald        print the generated C (codegen stage)
 *   emeraldc --keep-c ...           keep the intermediate .gen.c next to the binary
 *
 * The generated C is linked against the precompiled runtime (libemerald.a)
 * by the system C compiler ($CC, default "cc"). The runtime is located
 * relative to the emeraldc executable (build tree, prefix layouts), then the
 * compile-time default EMERALD_LIB_DIR; $EMERALD_LIB overrides everything.
 *
 * Everything from --check onwards operates on the *linked* program: the entry
 * file plus every module it imports, resolved by the module loader. The two earlier
 * stages (--emit-tokens, --emit-ast) are per-file views and never follow an
 * import. This command line is the whole contract between emeraldc and any
 * driver (such as pme) that resolves packages on its behalf:
 *
 *   emeraldc [-I <dir>]... [--json] [-o OUT] <entry>.rald
 */
#include "check.h"
#include "codegen.h"
#include "diag.h"
#include "dim.h"
#include "lexer.h"
#include "module.h"
#include "parser.h"
#include "repl.h"
#include "shape_report.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef EMERALD_LIB_DIR
#define EMERALD_LIB_DIR "lib/emerald"
#endif

#ifndef EMERALD_VERSION
#define EMERALD_VERSION "1.0.0"
#endif

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "emeraldc: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "emeraldc: cannot read '%s'\n", path);
        exit(1);
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void emit_tokens(const char *src) {
    Lexer lx;
    lexer_init(&lx, src);
    for (;;) {
        Token t = lexer_next(&lx);
        printf("%d %s", t.line, token_kind_name(t.kind));
        if (t.kind == TK_INT || t.kind == TK_FLOAT || t.kind == TK_STR || t.kind == TK_FSTR ||
            t.kind == TK_IDENT || t.kind == TK_ERROR)
            printf(" %.*s", t.len, t.start);
        printf("\n");
        if (t.kind == TK_EOF || t.kind == TK_ERROR) break;
    }
}

/* "dir/prog.rald" -> "dir/prog"; any other extension is kept and suffixed */
static char *default_output(const char *path) {
    size_t n = strlen(path);
    char *out = malloc(n + 5);
    if (!out) exit(1);
    strcpy(out, path);
    if (n > 5 && strcmp(out + n - 5, ".rald") == 0) out[n - 5] = '\0';
    else strcat(out, ".out");
    return out;
}

/* "a" + "b" in a fresh malloc'd buffer. */
static char *path_join(const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 1;
    char *p = malloc(n);
    if (!p) exit(1);
    snprintf(p, n, "%s%s", a, b);
    return p;
}

/* A stdlib root guessed from the executable's location: <exe_dir>/stdlib, then
 * <exe_dir>/../stdlib (the `prefix/bin` + `prefix/stdlib` layout). Returns a
 * malloc'd path or NULL. */
static char *find_exe_stdlib(const char *argv0) {
    char real[PATH_MAX];
    if (!argv0 || !*argv0 || !realpath(argv0, real)) return NULL;
    char *copy = strdup(real);
    char *dir = dirname(copy);
    /* Search order relative to the executable: a sibling `stdlib/` next to the
     * binary (source-tree layout), then `../stdlib` (prefix/bin + prefix/stdlib),
     * then the two conventional install layouts. */
    static const char *suffixes[] = {
        "/stdlib",
        "/../stdlib",
        "/../lib/emerald/stdlib",
        "/../share/emerald/stdlib",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *cand = path_join(dir, suffixes[i]);
        if (!cand) break;
        if (access(cand, F_OK) == 0) { free(copy); return cand; }
        free(cand);
    }
    free(copy);
    return NULL;
}

/* Locate the precompiled runtime: a directory holding libemerald.a and
 * include/runtime.h. $EMERALD_LIB wins outright; otherwise the same ladder the
 * stdlib uses, relative to the executable: a sibling (build tree: bin/),
 * then the prefix layouts, then the compile-time default. */
static bool find_runtime(const char *argv0, char **libout, char **incout) {
    const char *env = getenv("EMERALD_LIB");
    if (env && *env) {
        char *l = path_join(env, "/libemerald.a");
        char *h = path_join(env, "/include/runtime.h");
        bool ok = access(l, R_OK) == 0 && access(h, R_OK) == 0;
        free(l);
        free(h);
        if (ok) {
            *libout = strdup(env);
            *incout = path_join(env, "/include");
            return true;
        }
    }
    char real[PATH_MAX];
    char *exedir = NULL;
    char *copy = NULL;
    if (argv0 && *argv0 && realpath(argv0, real)) {
        copy = strdup(real);
        if (copy) exedir = dirname(copy); /* keeps `copy` alive */
    }
    static const char *suffixes[] = {"", "/../lib/emerald", "/../share/emerald"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]) + 1; i++) {
        char *dir = (i < sizeof(suffixes) / sizeof(suffixes[0]) && exedir)
                        ? path_join(exedir, suffixes[i])
                        : strdup(EMERALD_LIB_DIR);
        if (!dir) break;
        char *l = path_join(dir, "/libemerald.a");
        bool has_lib = access(l, R_OK) == 0;
        free(l);
        if (has_lib) {
            char *h = path_join(dir, "/include/runtime.h");
            bool has_hdr = access(h, R_OK) == 0;
            free(h);
            if (has_hdr) {
                *libout = dir;
                *incout = path_join(dir, "/include");
                free(copy);
                return true;
            }
            /* build tree: the library sits in bin/, the headers in ../include */
            if (exedir) {
                char *alt = path_join(exedir, "/../include");
                h = path_join(alt, "/runtime.h");
                has_hdr = access(h, R_OK) == 0;
                free(h);
                if (has_hdr) {
                    *libout = dir;
                    *incout = alt;
                    free(copy);
                    return true;
                }
                free(alt);
            }
        }
        free(dir);
    }
    free(copy);
    return false;
}

/* Run argv in a child process and return its exit status. No shell and no
 * string interpolation anywhere: a path containing quotes or semicolons is
 * just a path (the old system()-based invocation executed it). */
static int run_cc(char **argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "emeraldc: cannot exec '%s'\n", argv[0]);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void print_help(void) {
    printf("Emerald compiler %s\n", EMERALD_VERSION);
    puts("");
    puts("Usage:");
    puts("  emeraldc [OPTIONS] file.rald");
    puts("  emeraldc --repl [-I DIR]...");
    puts("");
    puts("Compilation modes (choose at most one):");
    puts("  --emit-tokens       print the lexer token stream");
    puts("  --emit-ast          print the parser AST");
    puts("  --emit-shapes       print tensor and dimension annotations");
    puts("  --check             type-check without generating a binary");
    puts("  --emit-c            print generated C instead of compiling it");
    puts("  --repl              start the interactive compiler session");
    puts("");
    puts("Diagnostics and checking:");
    puts("  --json              emit diagnostics as JSON");
    puts("  --proof             enable proof-mode checks");
    puts("  --proof-report      print proof-mode measurements");
    puts("  --shape-report      print shape solver measurements");
    puts("  --werror            treat warnings as errors");
    puts("  -Wno-CODE           suppress one warning code");
    puts("");
    puts("Build options:");
    puts("  -I DIR              add a module search directory");
    puts("  -o OUT              choose the output binary path");
    puts("  --keep-c            keep the generated .gen.c file");
    puts("  -h, --help          show this help and exit");
    puts("  -v, --version       print the compiler version and exit");
}

static void usage(void) {
    fputs("usage: emeraldc [OPTIONS] file.rald\n"
          "       emeraldc --repl [-I DIR]...\n"
          "try 'emeraldc --help' for more information\n",
          stderr);
    exit(2);
}

static void emit_proof_report(const ProofReport *r, bool json) {
    if (json) {
        printf("{\n");
        printf("  \"functions\": {\"total\": %zu, \"partial\": %zu, \"pure\": %zu},\n",
               r->total_funcs, r->partial_funcs, r->pure_funcs);
        printf("  \"partial_names\": [");
        for (size_t i = 0; i < r->partial_name_count; i++)
            printf("%s\"%s\"", i ? ", " : "", r->partial_names[i]);
        printf("],\n");
        printf("  \"vacuous_obligations\": %zu,\n", r->vacuous_obligations);
        printf("  \"covariance_warnings\": %zu,\n", r->covariance_warnings);
        printf("  \"taint_sites\": %zu\n", r->taint_sites);
        printf("}\n");
        return;
    }
    printf("functions: %zu total, %zu partial, %zu pure\n",
           r->total_funcs, r->partial_funcs, r->pure_funcs);
    if (r->partial_name_count) {
        printf("partial functions:");
        for (size_t i = 0; i < r->partial_name_count; i++)
            printf(" %s", r->partial_names[i]);
        printf("\n");
    }
    printf("obligations: %zu vacuous (tainted)\n", r->vacuous_obligations);
    printf("taint sites: %zu\n", r->taint_sites);
    printf("covariance warnings: %zu\n", r->covariance_warnings);
}

int main(int argc, char **argv) {
    const char *file = NULL, *out_path = NULL;
    enum { MODE_BUILD, MODE_TOKENS, MODE_AST, MODE_SHAPES, MODE_CHECK, MODE_C,
           MODE_REPL }
        mode = MODE_BUILD;
    bool keep_c = false;
    bool json_errors = false;
    bool proof = false;
    bool shape_report = false;
    bool proof_report = false;
    bool werror = false;
    const char **inc = malloc(sizeof(char *) * (size_t)argc);
    size_t ninc = 0;
    const char **suppress = malloc(sizeof(char *) * (size_t)argc);
    size_t nsuppress = 0;
    if (!inc || !suppress) return 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit-tokens") == 0) mode = MODE_TOKENS;
        else if (strcmp(argv[i], "--emit-ast") == 0) mode = MODE_AST;
        else if (strcmp(argv[i], "--emit-shapes") == 0) mode = MODE_SHAPES;
        else if (strcmp(argv[i], "--check") == 0) mode = MODE_CHECK;
        else if (strcmp(argv[i], "--repl") == 0) mode = MODE_REPL;
        else if (strcmp(argv[i], "--emit-c") == 0) mode = MODE_C;
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("emeraldc %s\n", EMERALD_VERSION);
            return 0;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        else if (strcmp(argv[i], "--json") == 0) json_errors = true;
        else if (strcmp(argv[i], "--proof") == 0) proof = true;
        else if (strcmp(argv[i], "--shape-report") == 0) shape_report = true;
        else if (strcmp(argv[i], "--proof-report") == 0) proof_report = true;
        else if (strcmp(argv[i], "--keep-c") == 0) keep_c = true;
        else if (strcmp(argv[i], "--werror") == 0) werror = true;
        else if (strncmp(argv[i], "-Wno-", 5) == 0)
            suppress[nsuppress++] = argv[i] + 5;
        else if (strcmp(argv[i], "-o") == 0) {
            if (++i == argc) usage();
            out_path = argv[i];
        } else if (strcmp(argv[i], "-I") == 0) {
            if (++i == argc) usage();
            inc[ninc++] = argv[i];
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            inc[ninc++] = argv[i] + 2;
        } else if (argv[i][0] == '-') usage();
        else if (file) usage();
        else file = argv[i];
    }
    /* `emeraldc` with nothing to compile is an invitation to type at it */
    if (mode == MODE_REPL || (!file && mode == MODE_BUILD && !out_path))
        return repl_run(argv[0], inc, ninc);
    if (!file) usage();

    DiagList diags;
    diag_init(&diags, NULL);
    diags.json = json_errors;
    /* proof mode promotes warnings to errors (a proof must be honest), and
     * -Wno-<code> silences specific warning codes */
    diags.werror = werror || proof;
    diags.suppress = suppress;
    diags.suppress_count = nsuppress;

    /* the first two stages are per-file views: they never follow an import */
    if (mode == MODE_TOKENS || mode == MODE_AST) {
        char *src = read_file(file);
        diag_add_source(&diags, file, src);
        if (mode == MODE_TOKENS) {
            emit_tokens(src);
            return 0;
        }
        ast_print_program(stdout, parse_program(src, file, &diags));
        return 0;
    }

    int errors = 0;
    /* A copied binary finds the stdlib relative to itself. */
    module_set_exe_stdlib(find_exe_stdlib(argv[0]));
    Program *prog = module_link(file, inc, ninc, &diags, &errors);
    if (!prog) {
        if (diags.count) diag_render(&diags, diags.json ? stdout : stderr);
        return 1;
    }

    if (mode == MODE_SHAPES) {
        /* the shape stage's own observable surface: dim declarations and every
         * tensor annotation, dumped from the linked program */
        emit_shapes(stdout, prog);
        return 0;
    }

    errors = check_program(prog, file, &diags, proof);
    if (shape_report) {
        fprintf(stderr, "shape-crossings: %zu\n", check_shape_crossings());
        fprintf(stderr, "dim-unresolved: %zu\n", dim_unresolved_count());
        dim_log_dump(stderr);
    }
    /* a warning still compiles; only --werror (or proof mode) promotes it */
    if (diags.werror && diag_warning_count(&diags)) errors++;
    if (mode == MODE_CHECK) {
        if (proof_report) {
            /* the report is the primary output; diagnostics stay on stderr so
             * a --json report is a single clean JSON document */
            emit_proof_report(proof_report_get(), json_errors);
            if (!json_errors) {
                diag_render(&diags, stderr);
                if (errors == 0) printf("ok\n");
            }
        } else if (diags.json) {
            diag_render(&diags, stdout);
        } else {
            diag_render(&diags, stderr);
            if (errors == 0) printf("ok\n");
        }
        return errors ? 1 : 0;
    }
    if (proof_report) emit_proof_report(proof_report_get(), json_errors);
    if (errors) {
        diag_render(&diags, diags.json ? stdout : stderr);
        return 1;
    }
    /* warnings are shown but do not stop the build (unless --werror bumped
     * `errors` above) */
    if (diag_warning_count(&diags) && !diags.json)
        diag_render(&diags, stderr);

    if (mode == MODE_C) {
        codegen_program(stdout, prog, file);
        return 0;
    }

    /* full build: write C, invoke the system compiler, optionally clean up */
    char *out = out_path ? strdup(out_path) : default_output(file);
    size_t clen = strlen(out) + 8;
    char *cfile = malloc(clen);
    snprintf(cfile, clen, "%s.gen.c", out);

    FILE *cf = fopen(cfile, "w");
    if (!cf) {
        fprintf(stderr, "emeraldc: cannot write '%s'\n", cfile);
        return 1;
    }
    codegen_program(cf, prog, file);
    fclose(cf);

    char *libdir, *incdir;
    if (!find_runtime(argv[0], &libdir, &incdir)) {
        fprintf(stderr, "emeraldc: cannot locate the Emerald runtime "
                        "(libemerald.a + include/runtime.h); set $EMERALD_LIB\n");
        if (!keep_c) remove(cfile);
        return 1;
    }

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    /* $CC may be several words ("ccache cc"); split on whitespace. */
    char *ccbuf = strdup(cc);
    char *ccargv[8];
    size_t ncc = 0;
    for (char *t = strtok(ccbuf, " \t"); t && ncc < 7; t = strtok(NULL, " \t"))
        ccargv[ncc++] = t;

    char *iflag = path_join("-I", incdir);
    char *lib = path_join(libdir, "/libemerald.a");
    char *cc_argv[16];
    size_t na = 0;
    for (size_t i = 0; i < ncc; i++) cc_argv[na++] = ccargv[i];
    cc_argv[na++] = "-std=c11";
    cc_argv[na++] = "-O2";
    cc_argv[na++] = "-pthread";
    cc_argv[na++] = "-lm";
    cc_argv[na++] = iflag;
    cc_argv[na++] = "-o";
    cc_argv[na++] = out;
    cc_argv[na++] = cfile;
    cc_argv[na++] = lib;
    cc_argv[na] = NULL;

    int rc = run_cc(cc_argv);
    if (rc != 0) {
        fprintf(stderr, "emeraldc: C compilation failed (exit %d)\n", rc);
        if (!keep_c) remove(cfile);
        return 1;
    }
    if (!keep_c) remove(cfile);
    return 0;
}
