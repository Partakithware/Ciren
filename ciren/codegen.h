// ciren/codegen.h

#ifndef CIREN_CODEGEN_H
#define CIREN_CODEGEN_H

#include "ast.h"
#include "resolver.h"
#include <stdio.h>

// ─── CODEGEN CONTEXT ─────────────────────────────────────────────────────────

typedef struct {
    Arena      *arena;
    Resolver   *resolver;
    FILE       *out;           // output file (or stdout)
    const char *filename;
    int         indent;        // current indentation level
    int         had_error;

    // Deferred defer-statement stacks per function scope
    // Each function tracks its defer chain so we can emit
    // them in reverse order on every exit path.
    AstNode   **defer_stack;
    size_t      defer_count;
    size_t      defer_cap;

    // Unique label counter for generated goto labels
    int         label_counter;

    // Are we currently inside an asm function? (suppress C idioms)
    int         in_asm_func;

    // Current function return type (for implicit void returns)
    AstType    *current_return_type;

    // Track if current function has named returns
    int         has_named_returns;
} Codegen;

// ─── API ─────────────────────────────────────────────────────────────────────

Codegen *codegen_create(Arena *arena, Resolver *resolver,
                        FILE *out, const char *filename);
int      codegen_run(Codegen *g, AstNode *program);

#endif // CIREN_CODEGEN_H