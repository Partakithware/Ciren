// ciren/typechecker.h

#ifndef CIREN_TYPECHECKER_H
#define CIREN_TYPECHECKER_H

#include "ast.h"
#include "resolver.h"

// ─── TYPE ENVIRONMENT ────────────────────────────────────────────────────────
// Maps variable names → their resolved AstType within the current scope.

typedef struct TypeBinding {
    char            *name;
    AstType         *type;
    int              is_const;
    struct TypeBinding *next;
} TypeBinding;

#define TC_BUCKETS 64

typedef struct TypeScope {
    struct TypeScope *parent;
    TypeBinding      *buckets[TC_BUCKETS];
} TypeScope;

// ─── TYPE CHECKER CONTEXT ────────────────────────────────────────────────────

typedef struct {
    Arena       *arena;
    Resolver    *resolver;
    const char  *filename;
    int          had_error;
    int          warning_count;

    // Scope stack
    TypeScope   *scope;

    // Current function context
    AstType     *return_type;      // expected return type of current function
    char        *func_name;        // for error messages
    int          in_loop;          // for break/continue validation
    int          return_seen;      // did we see a return in all paths?

    // Struct method resolution: which struct are we inside?
    AstNode     *current_struct;

    // Deferred interface checks
    AstNode    **iface_checks;
    size_t       iface_check_count;
    size_t       iface_check_cap;

} TypeChecker;

// ─── API ─────────────────────────────────────────────────────────────────────

TypeChecker *tc_create(Arena *arena, Resolver *resolver, const char *filename);
int          tc_run(TypeChecker *tc, AstNode *program);  // 0 = error

// Exposed for testing
AstType     *tc_check_expr(TypeChecker *tc, AstNode *node);
void         tc_check_stmt(TypeChecker *tc, AstNode *node);
void         tc_check_block(TypeChecker *tc, AstNode *node);

#endif // CIREN_TYPECHECKER_H
