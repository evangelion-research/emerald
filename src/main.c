/* emeraldc: the Emerald compiler driver.
 *
 *   emeraldc file.rald              compile to a native binary (./file)
 *   emeraldc -o out file.rald       choose the output path
 *   emeraldc --emit-tokens f.rald   dump the token stream (lexer stage)
 *   emeraldc --emit-ast f.rald      dump the AST as s-expressions (parser stage)
 *   emeraldc --check f.rald         type-check only (checker stage)
 *   emeraldc --emit-c f.rald        print the generated C (codegen stage)
 *   emeraldc --keep-c ...           keep the intermediate .gen.c next to the binary
 *
 * The generated C is compiled together with the runtime by the system C
 * compiler ($CC, default "cc"). The runtime location defaults to the macro
 * EMERALD_SRC_DIR (set by the build) and can be overridden with $EMERALD_SRC.
 */
#include "check.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EMERALD_SRC_DIR
#define EMERALD_SRC_DIR "src"
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
        if (t.kind == TK_INT || t.kind == TK_FLOAT || t.kind == TK_STR ||
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

static void usage(void) {
    fputs("usage: emeraldc [--emit-tokens|--emit-ast|--check|--emit-c]\n"
          "                [--keep-c] [-o OUT] file.rald\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    const char *file = NULL, *out_path = NULL;
    enum { MODE_BUILD, MODE_TOKENS, MODE_AST, MODE_CHECK, MODE_C } mode = MODE_BUILD;
    bool keep_c = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit-tokens") == 0) mode = MODE_TOKENS;
        else if (strcmp(argv[i], "--emit-ast") == 0) mode = MODE_AST;
        else if (strcmp(argv[i], "--check") == 0) mode = MODE_CHECK;
        else if (strcmp(argv[i], "--emit-c") == 0) mode = MODE_C;
        else if (strcmp(argv[i], "--keep-c") == 0) keep_c = true;
        else if (strcmp(argv[i], "-o") == 0) {
            if (++i == argc) usage();
            out_path = argv[i];
        } else if (argv[i][0] == '-') usage();
        else if (file) usage();
        else file = argv[i];
    }
    if (!file) usage();

    char *src = read_file(file);

    if (mode == MODE_TOKENS) {
        emit_tokens(src);
        return 0;
    }

    Program *prog = parse_program(src, file);

    if (mode == MODE_AST) {
        ast_print_program(stdout, prog);
        return 0;
    }

    int errors = check_program(prog, file);
    if (mode == MODE_CHECK) {
        if (errors == 0) printf("ok\n");
        return errors ? 1 : 0;
    }
    if (errors) return 1;

    if (mode == MODE_C) {
        codegen_program(stdout, prog);
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
    codegen_program(cf, prog);
    fclose(cf);

    const char *srcdir = getenv("EMERALD_SRC");
    if (!srcdir || !*srcdir) srcdir = EMERALD_SRC_DIR;
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    size_t cmdlen = strlen(cc) + strlen(cfile) + strlen(out) + 3 * strlen(srcdir) + 256;
    char *cmd = malloc(cmdlen);
    char *srcroot = strdup(srcdir);
    char *lastslash = strrchr(srcroot, '/');
    if (lastslash && !strcmp(lastslash, "/src")) *lastslash = '\0';
    snprintf(cmd, cmdlen,
             "%s -std=c11 -O2 -I '%s' -I '%s/include' -o '%s' '%s' '%s/runtime.c'",
             cc, srcdir, srcroot, out, cfile, srcdir);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "emeraldc: C compilation failed (%s)\n", cmd);
        return 1;
    }
    if (!keep_c) remove(cfile);
    return 0;
}
