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
 * The generated C is compiled together with the runtime by the system C
 * compiler ($CC, default "cc"). The runtime location defaults to the macro
 * EMERALD_SRC_DIR (set by the build) and can be overridden with $EMERALD_SRC.
 *
 * Everything from --check onwards operates on the *linked* program: the entry
 * file plus every module it imports, resolved by src/module.c. The two earlier
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

#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef EMERALD_SRC_DIR
#define EMERALD_SRC_DIR "src"
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

/* --- the shape stage's observable surface (--emit-shapes) --------------- */

/* Walk a type expression, printing every tensor annotation it contains (the
 * dtype + shape surface that W4 later checks). */
static void emit_type_shapes(FILE *out, const TypeExpr *t) {
    if (!t) return;
    switch (t->kind) {
    case TE_TENSOR:
        fprintf(out, "Tensor[");
        if (t->tensor.dtype && t->tensor.dtype->kind == TE_NAME)
            fprintf(out, "%s", t->tensor.dtype->name);
        else
            fprintf(out, "?");
        if (t->tensor.dynamic) {
            fprintf(out, ", ?]\n");
        } else {
            fprintf(out, ", [");
            for (size_t i = 0; i < t->tensor.shape_count; i++) {
                if (i) fprintf(out, ", ");
                char *s = dim_str(t->tensor.shape[i]);
                fprintf(out, "%s", s);
                free(s);
            }
            fprintf(out, "]]\n");
        }
        break;
    case TE_FIN: {
        char *s = dim_str(t->fin_dim);
        fprintf(out, "Fin[%s]\n", s);
        free(s);
        break;
    }
    case TE_EQ: {
        char *a = dim_str(t->eq_lhs);
        char *b = dim_str(t->eq_rhs);
        fprintf(out, "Eq[%s, %s]\n", a, b);
        free(a);
        free(b);
        break;
    }
    case TE_NAME:
        for (size_t i = 0; i < t->arg_count; i++)
            emit_type_shapes(out, t->args[i]);
        break;
    case TE_LIST:
        emit_type_shapes(out, t->elem);
        break;
    case TE_SEQ:
        emit_type_shapes(out, t->elem);
        break;
    case TE_REC:
        for (size_t i = 0; i < t->fields.count; i++)
            emit_type_shapes(out, t->fields.types[i]);
        break;
    case TE_UNION:
    case TE_INTER:
        emit_type_shapes(out, t->lhs);
        emit_type_shapes(out, t->rhs);
        break;
    case TE_FUNC:
        for (size_t i = 0; i < t->fun.param_count; i++)
            emit_type_shapes(out, t->fun.params[i]);
        emit_type_shapes(out, t->fun.ret);
        break;
    case TE_LIT:
        break;
    }
}

static void emit_expr_shapes(FILE *out, const Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case E_LAMBDA:
        for (size_t i = 0; i < e->as.lam.param_count; i++)
            emit_type_shapes(out, e->as.lam.param_types[i]);
        emit_expr_shapes(out, e->as.lam.body);
        break;
    case E_TRY:
        emit_expr_shapes(out, e->as.try_expr);
        break;
    case E_CATCH:
        emit_expr_shapes(out, e->as.ctch.subject);
        for (size_t i = 0; i < e->as.ctch.count; i++)
            emit_expr_shapes(out, e->as.ctch.arms[i].body);
        break;
    case E_LIST:
    case E_TUPLE:
        for (size_t i = 0; i < e->as.list.count; i++)
            emit_expr_shapes(out, e->as.list.items[i]);
        break;
    case E_REC:
        for (size_t i = 0; i < e->as.rec.count; i++)
            emit_expr_shapes(out, e->as.rec.values[i]);
        break;
    case E_DICT:
        for(size_t i=0;i<e->as.dict.count;i++){emit_expr_shapes(out,e->as.dict.keys[i]);emit_expr_shapes(out,e->as.dict.values[i]);} break;
    case E_SET:
        for(size_t i=0;i<e->as.set.count;i++)emit_expr_shapes(out,e->as.set.items[i]); break;
    case E_SLICE:
        emit_expr_shapes(out,e->as.slice.seq); if(e->as.slice.start)emit_expr_shapes(out,e->as.slice.start); if(e->as.slice.stop)emit_expr_shapes(out,e->as.slice.stop); if(e->as.slice.step)emit_expr_shapes(out,e->as.slice.step); break;
    case E_COMP:
        emit_expr_shapes(out,e->as.comp.seq); emit_expr_shapes(out,e->as.comp.cond); emit_expr_shapes(out,e->as.comp.elt); emit_expr_shapes(out,e->as.comp.key); break;
    case E_FSTR:
        for(size_t i=0;i<e->as.fstr.count;i++)emit_expr_shapes(out,e->as.fstr.exprs[i]); break;
    case E_BINOP:
        emit_expr_shapes(out, e->as.bin.lhs);
        emit_expr_shapes(out, e->as.bin.rhs);
        break;
    case E_UNOP:
        emit_expr_shapes(out, e->as.un.operand);
        break;
    case E_CALL:
        emit_expr_shapes(out, e->as.call.fn);
        for (size_t i = 0; i < e->as.call.count; i++)
            emit_expr_shapes(out, e->as.call.args[i]);
        break;
    case E_INDEX:
        emit_expr_shapes(out, e->as.index.seq);
        emit_expr_shapes(out, e->as.index.idx);
        break;
    case E_ATTR:
        emit_expr_shapes(out, e->as.attr.obj);
        break;
    default:
        break;
    }
}

static void emit_stmt_shapes(FILE *out, const Stmt *s);

static void emit_block_shapes(FILE *out, const Block *b) {
    for (size_t i = 0; i < b->count; i++)
        emit_stmt_shapes(out, b->items[i]);
}

static void emit_stmt_shapes(FILE *out, const Stmt *s) {
    switch (s->kind) {
    case S_DIMDECL:
        fprintf(out, "dim");
        for (size_t i = 0; i < s->as.dim.count; i++)
            fprintf(out, " %s", s->as.dim.names[i]);
        fprintf(out, "\n");
        break;
    case S_ASSIGN:
        emit_type_shapes(out, s->as.assign.ann);
        emit_expr_shapes(out, s->as.assign.value);
        break;
    case S_EXPR:
        emit_expr_shapes(out, s->as.expr);
        break;
    case S_IF:
        for (size_t i = 0; i < s->as.ifs.count; i++) {
            emit_expr_shapes(out, s->as.ifs.conds[i]);
            emit_block_shapes(out, &s->as.ifs.blocks[i]);
        }
        if (s->as.ifs.has_else) emit_block_shapes(out, &s->as.ifs.else_block);
        break;
    case S_WHILE:
        emit_expr_shapes(out, s->as.wh.cond);
        emit_block_shapes(out, &s->as.wh.body);
        break;
    case S_FOR:
        emit_expr_shapes(out, s->as.fr.seq);
        emit_block_shapes(out, &s->as.fr.body);
        break;
    case S_RETURN:
        emit_expr_shapes(out, s->as.ret);
        break;
    case S_BLOCK:
        emit_block_shapes(out, &s->as.block);
        break;
    case S_MATCH:
        emit_expr_shapes(out, s->as.mtch.subject);
        for (size_t i = 0; i < s->as.mtch.count; i++)
            emit_block_shapes(out, &s->as.mtch.blocks[i]);
        break;
    case S_FUNC:
        for (size_t i = 0; i < s->as.func.param_count; i++)
            emit_type_shapes(out, s->as.func.param_types[i]);
        emit_type_shapes(out, s->as.func.ret_type);
        emit_block_shapes(out, &s->as.func.body);
        break;
    case S_TYPEDEF:
        emit_type_shapes(out, s->as.tdef.value);
        break;
    case S_IMPORT:
    case S_BREAK:
    case S_CONTINUE:
    case S_PASS:
        break;
    }
}

static void emit_shapes(FILE *out, const Program *p) {
    emit_block_shapes(out, &p->body);
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
        char *cand = malloc(strlen(dir) + strlen(suffixes[i]) + 1);
        if (!cand) break;
        sprintf(cand, "%s%s", dir, suffixes[i]);
        if (access(cand, F_OK) == 0) { free(copy); return cand; }
        free(cand);
    }
    free(copy);
    return NULL;
}

static void usage(void) {
    fputs("usage: emeraldc [--emit-tokens|--emit-ast|--emit-shapes|--check|--emit-c]\n"
          "                [--json] [--proof] [--shape-report] [--proof-report] [--keep-c]\n"
          "                [--werror] [-Wno-CODE]... [-I DIR]... [-o OUT] file.rald\n"
          "       emeraldc --repl [-I DIR]...\n",
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
    /* RELEASE_V1 B3: a copied binary still finds the stdlib relative to itself */
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
             "%s -std=c11 -O2 -pthread -I '%s' -I '%s/include' -o '%s' '%s' '%s/runtime.c'",
             cc, srcdir, srcroot, out, cfile, srcdir);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "emeraldc: C compilation failed (%s)\n", cmd);
        return 1;
    }
    if (!keep_c) remove(cfile);
    return 0;
}
