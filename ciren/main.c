/*
 * Copyright (C) 2026 Maxwell Wingate
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// ciren/main.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "resolver.h"
#include "typechecker.h"
#include "codegen.h"
#include "codegen_llvm.h"

// ─── FILE UTILITIES ──────────────────────────────────────────────────────────

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cirenc: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

// ─── LINKER INVOCATION ───────────────────────────────────────────────────────
// Invoke the system C compiler as a linker driver.
// This handles libc, crt0, and all the platform startup boilerplate
// that a raw 'ld' call would require us to specify manually.

static int link_binary(const char *obj_path, const char *out_path,
                        int verbose, const char **extra_libs, int nlibs) {
    // We use 'cc' as the linker driver — it knows where libc, crt0,
    // and all the platform startup objects live. On Linux this is gcc
    // or clang, both accept the same interface for linking.
    const char *linker = "cc";

   /* const char *argv[] = {
        linker,
        obj_path,           // input object file
        "-o", out_path,     // output binary
        "-lm",              // link math library (commonly needed)
        NULL
    }; */

    // Build argv dynamically to support extra -l flags
    const char *argv[64];
    int ai = 0;
    argv[ai++] = linker;
    argv[ai++] = obj_path;
    argv[ai++] = "-o";
    argv[ai++] = out_path;
    argv[ai++] = "-lm";
    for (int i = 0; i < nlibs; i++)
        argv[ai++] = extra_libs[i];
    argv[ai] = NULL;

    if (verbose) {
        fprintf(stderr, "cirenc: linking: ");
        for (int i = 0; argv[i]; i++)
            fprintf(stderr, "%s ", argv[i]);
        fprintf(stderr, "\n");
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("cirenc: fork failed");
        return 0;
    }

    if (pid == 0) {
        // Child: exec the linker
        execvp(linker, (char *const *)argv);
        // If execvp returns, it failed
        fprintf(stderr, "cirenc: cannot find linker '%s'\n", linker);
        exit(127);
    }

    // Parent: wait for linker to finish
    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "cirenc: linker failed (exit %d)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 0;
    }
    return 1;
}

// ── Module loader: find and merge .ci files ──────────────────────────────────
// Search paths for .ci modules
static const char *module_search_paths[] = {
    ".",
    "std",
    NULL
};

static char *find_module_file(const char *mod_name) {
    static char path[512];
    for (int i = 0; module_search_paths[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s.ci",
                 module_search_paths[i], mod_name);
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); return path; }
    }
    return NULL;
}

// Parse a .ci module file and merge its public declarations into program
static void load_ci_module(AstNode *program, const char *mod_path,
                            Arena *arena, const char *mod_name) {
    char *src = read_file(mod_path);
    if (!src) {
        fprintf(stderr, "cirenc: cannot read module '%s'\n", mod_path);
        return;
    }
    Lexer lex;
    lexer_init(&lex, src, mod_path);
    Parser p;
    parser_init(&p, &lex, arena, mod_path);
    AstNode *mod_ast = parse(&p);
    free(src);
    if (p.had_error) {
        fprintf(stderr, "cirenc: errors in module '%s'\n", mod_path);
        return;
    }
    // Merge public declarations into the main program
    for (size_t i = 0; i < mod_ast->as.program.count; i++) {
        AstNode *decl = mod_ast->as.program.decls[i];
        if (!decl) continue;
        // Skip the module declaration itself
        if (decl->kind == NODE_MODULE) continue;
        // Re-export using declarations from modules so their C deps get injected
        if (decl->kind == NODE_USING) {
            size_t n = program->as.program.count;
            AstNode **new_decls = arena_alloc(arena, (n + 1) * sizeof(AstNode *));
            memcpy(new_decls, program->as.program.decls, n * sizeof(AstNode *));
            new_decls[n] = decl;
            program->as.program.decls = new_decls;
            program->as.program.count = n + 1;
            continue;
        }
        // Skip non-public declarations
        // Include all decls — private helpers are needed by public functions
        // Structs/enums/impls always included; functions always included
        // Only skip NODE_MODULE declaration itself (already skipped above)
        int skip = 0;
        (void)skip;
        // include everything — private helpers needed by public functions
        // Grow program decls array and append
        size_t n = program->as.program.count;
        AstNode **new_decls = arena_alloc(arena, (n + 1) * sizeof(AstNode *));
        memcpy(new_decls, program->as.program.decls, n * sizeof(AstNode *));
        new_decls[n] = decl;
        program->as.program.decls = new_decls;
        program->as.program.count = n + 1;
    }
}

// ─── USAGE ───────────────────────────────────────────────────────────────────

static void usage(void) {
    fprintf(stderr,
        "usage: cirenc [options] <file.ci>\n"
        "\n"
        "options:\n"
        "  -o <file>         output file\n"
        "                    default: binary named after input (hello_world.ci → hello_world)\n"
        "  --emit-c          emit C source instead of compiling\n"
        "  --emit-ir         emit LLVM IR (.ll) instead of compiling\n"
        "  --emit-obj        emit object file (.o) instead of linking\n"
        "  -O0 -O1 -O2 -O3  optimization level (default: O0)\n"
        "  --dump-tokens     print token stream and exit\n"
        "  --dump-ast        print AST and exit\n"
        "  --verbose         print linker command\n"
        "  --help            show this message\n"
        "\n"
        "examples:\n"
        "  cirenc hello.ci               → ./hello\n"
        "  cirenc hello.ci -o greet      → ./greet\n"
        "  cirenc hello.ci --emit-ir     → hello.ll\n"
        "  cirenc hello.ci --emit-obj    → hello.o\n"
        "  cirenc hello.ci -O2           → ./hello (optimized)\n"
    );
}

// Derive a default output filename from the input filename.
// hello_world.ci → hello_world
// src/foo.ci     → foo
static char *derive_output_name(const char *input, const char *ext) {
    // Find the basename
    const char *base = strrchr(input, '/');
    base = base ? base + 1 : input;

    // Strip .ci extension
    char *out = strdup(base);
    char *dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".ci") == 0) *dot = '\0';

    // Append new extension if provided
    if (ext && ext[0] != '\0') {
        char *with_ext = malloc(strlen(out) + strlen(ext) + 1);
        strcpy(with_ext, out);
        strcat(with_ext, ext);
        free(out);
        return with_ext;
    }
    return out;
}

// ─── MAIN ────────────────────────────────────────────────────────────────────

    // ── Module → linker flags table ──────────────────────────────────────────
    // Any C library can be used in Ciren. Add entries here for auto-linking,
    // or pass -l flags manually via: cirenc myfile.ci -lmylib
    typedef struct { const char *mod; const char *flag; } ModLib;
    static const ModLib mod_libs[] = {
        { "gtk",    "-lgtk-3"    },
        { "gio",    "-lgio-2.0"     },
        { "glib",   "-lglib-2.0" },
        { "gobject","-lgobject-2.0" },
        { "cairo",  "-lcairo"    },
        { "sdl",    "-lSDL2"     },
        { "sdl2",   "-lSDL2"     },
        { "gl",     "-lGL"       },
        { "glfw",   "-lglfw"     },
        { "ncurses","-lncurses"  },
        { "pthread","-lpthread"  },
        { "z",      "-lz"        },
        { "ssl",    "-lssl"      },
        { "crypto", "-lcrypto"   },
        { NULL, NULL }
    };
    const char *extra_libs[64]; int nlibs = 0;

int main(int argc, char **argv) {
    const char *input_file  = NULL;
    const char *output_file = NULL;
    int dump_tokens = 0;
    int dump_ast    = 0;
    int emit_c      = 0;
    int emit_ir     = 0;
    int emit_obj    = 0;
    int opt_level   = 0;
    int verbose     = 0;
    
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--dump-tokens") == 0) dump_tokens = 1;
        else if (strcmp(argv[i], "--dump-ast")    == 0) dump_ast    = 1;
        else if (strcmp(argv[i], "--emit-c")      == 0) emit_c      = 1;
        else if (strcmp(argv[i], "--emit-ir")     == 0) emit_ir     = 1;
        else if (strcmp(argv[i], "--emit-obj")    == 0) emit_obj    = 1;
        else if (strcmp(argv[i], "--verbose")     == 0) verbose     = 1;
        else if (strcmp(argv[i], "-O0")           == 0) opt_level   = 0;
        else if (strcmp(argv[i], "-O1")           == 0) opt_level   = 1;
        else if (strcmp(argv[i], "-O2")           == 0) opt_level   = 2;
        else if (strcmp(argv[i], "-O3")           == 0) opt_level   = 3;
        else if (strcmp(argv[i], "--help")        == 0) { usage(); return 0; }
        else if (strncmp(argv[i], "-l", 2) == 0) {
            extra_libs[nlibs++] = argv[i];
        }
        else if (strncmp(argv[i], "-I", 2) == 0) {
            // Ignore include paths — Ciren doesn't use C headers
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            // Ignore preprocessor defines — not needed
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_file = argv[++i];
        else if (argv[i][0] != '-')
            input_file = argv[i];
        else {
            fprintf(stderr, "cirenc: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!input_file) { usage(); return 1; }

    // ── Derive output filename if not given ──────
    char *derived_output = NULL;
    if (!output_file) {
        if      (emit_c)   derived_output = derive_output_name(input_file, ".c");
        else if (emit_ir)  derived_output = derive_output_name(input_file, ".ll");
        else if (emit_obj) derived_output = derive_output_name(input_file, ".o");
        else               derived_output = derive_output_name(input_file, "");
        output_file = derived_output;
    }

    // ── Read source ──────────────────────────────
    char *source = read_file(input_file);
    if (!source) { free(derived_output); return 1; }

    // ── Lex ──────────────────────────────────────
    Lexer lexer;
    lexer_init(&lexer, source, input_file);

    if (dump_tokens) {
        Token t;
        while ((t = lexer_next(&lexer)).type != TOK_EOF)
            token_print(t);
        free(source);
        free(derived_output);
        return 0;
    }

    // ── Parse ─────────────────────────────────────
    Arena  *arena = arena_create(512 * 1024 * 1024);
    Parser  parser;
    parser_init(&parser, &lexer, arena, input_file);
    AstNode *program = parse(&parser);

    if (parser.had_error) {
        fprintf(stderr, "cirenc: parse failed\n");
        arena_destroy(arena);
        free(source);
        free(derived_output);
        return 1;
    }

    if (dump_ast) {
        ast_print(program, 0);
        arena_destroy(arena);
        free(source);
        free(derived_output);
        return 0;
    }
    

    // ── Load .ci modules ─────────────────────────
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_USING) continue;
        const char *mod = decl->as.using_decl.path;
        char *mod_file = find_module_file(mod);
        if (mod_file) {
            load_ci_module(program, mod_file, arena, mod);
        }
        // else: fall through to C symbol injection in resolver
    }

    // Collect -l flags from using declarations
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *d = program->as.program.decls[i];
        if (d->kind != NODE_USING) continue;
        const char *path = d->as.using_decl.path;
        for (int m = 0; mod_libs[m].mod; m++) {
            if (strcmp(path, mod_libs[m].mod) == 0) {
                extra_libs[nlibs++] = mod_libs[m].flag;
                break;
            }
        }
    }
    // Also accept -l flags on the cirenc command line
    // (add this in the arg parsing loop)
    

    // ── Resolve ───────────────────────────────────
    Resolver *resolver = resolver_create(arena, input_file);
    if (!resolver_run(resolver, program)) {
        fprintf(stderr, "cirenc: resolve failed\n");
        arena_destroy(arena);
        free(source);
        free(derived_output);
        return 1;
    }

    // ── Type check ────────────────────────────────
    TypeChecker *tc = tc_create(arena, resolver, input_file);
    if (!tc_run(tc, program)) {
        fprintf(stderr, "cirenc: type check failed\n");
        arena_destroy(arena);
        free(source);
        free(derived_output);
        return 1;
    }

    // ── Codegen ───────────────────────────────────
    int ok = 0;

    if (emit_c) {
        FILE *out = fopen(output_file, "w");
        if (!out) {
            fprintf(stderr, "cirenc: cannot write '%s'\n", output_file);
            arena_destroy(arena);
            free(source);
            free(derived_output);
            return 1;
        }
        Codegen *cg = codegen_create(arena, resolver, out, input_file);
        ok = codegen_run(cg, program);
        fclose(out);
        if (ok) fprintf(stderr, "cirenc: wrote %s\n", output_file);

    } else if (emit_ir) {
        LLVMCodegen *cg = llvm_codegen_create(arena, resolver, input_file);
        ok = llvm_codegen_run(cg, program, output_file, 1, opt_level);

    } else if (emit_obj) {
        LLVMCodegen *cg = llvm_codegen_create(arena, resolver, input_file);
        ok = llvm_codegen_run(cg, program, output_file, 0, opt_level);

    } else {
        // ── Full compilation: obj → binary ────────
        // Write object to a temporary file, then link it
        char tmp_obj[256];
        snprintf(tmp_obj, sizeof(tmp_obj), "/tmp/cirenc_%d.o", (int)getpid());

        LLVMCodegen *cg = llvm_codegen_create(arena, resolver, input_file);
        ok = llvm_codegen_run(cg, program, tmp_obj, 0, opt_level);

        if (ok) {
            ok = link_binary(tmp_obj, output_file, verbose, extra_libs, nlibs);
            if (ok)
                fprintf(stderr, "cirenc: wrote %s\n", output_file);
        }

        // Clean up temp object regardless of success
        unlink(tmp_obj);
    }

    arena_destroy(arena);
    free(source);
    free(derived_output);
    return ok ? 0 : 1;
}
