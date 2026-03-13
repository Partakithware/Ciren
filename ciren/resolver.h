// ciren/resolver.h

#ifndef CIREN_RESOLVER_H
#define CIREN_RESOLVER_H

#include "ast.h"
#include <stddef.h>

// ─── SYMBOL KINDS ────────────────────────────────────────────────────────────

typedef enum {
    SYM_FUNCTION,       // regular function
    SYM_ASM_FUNCTION,   // asm function
    SYM_STRUCT,         // struct type
    SYM_ENUM,           // enum type
    SYM_INTERFACE,      // interface type
    SYM_CONST,          // compile-time constant
    SYM_VAR,            // variable (local or global)
    SYM_PARAM,          // function parameter
    SYM_FIELD,          // struct field
    SYM_ENUM_VARIANT,   // enum variant
    SYM_MODULE,         // imported module
    SYM_GENERIC_PARAM,  // T in func<T>
} SymbolKind;

// ─── SYMBOL ──────────────────────────────────────────────────────────────────

typedef struct Symbol {
    SymbolKind   kind;
    char        *name;
    AstNode     *decl;        // the declaration node that defined this symbol
    AstType     *type;        // resolved type (NULL if not yet resolved)
    AccessMod    access;
    int          is_resolved; // has this symbol been fully resolved?
    int          resolving;   // cycle detection flag

    // For struct fields / enum variants: back-pointer to parent
    struct Symbol *parent;

    // Linked list for hash bucket chaining
    struct Symbol *next;
} Symbol;

// ─── SCOPE ───────────────────────────────────────────────────────────────────

typedef struct Scope {
    struct Scope *parent;     // enclosing scope
    Symbol      **buckets;    // hash table
    size_t        bucket_count;
    size_t        symbol_count;
    int           is_global;  // top-level module scope
} Scope;

// ─── RESOLVER ────────────────────────────────────────────────────────────────

typedef struct {
    Arena       *arena;
    Scope       *global;      // module-level scope (populated in pass 1)
    Scope       *current;     // current scope during pass 2
    const char  *filename;
    int          had_error;

    // The module's collected exports (public symbols)
    Symbol     **exports;
    size_t       export_count;
    size_t       export_cap;

    // Deferred checks (e.g. interface satisfaction) run after full resolution
    AstNode    **deferred;
    size_t       deferred_count;
    size_t       deferred_cap;
} Resolver;

// ─── API ─────────────────────────────────────────────────────────────────────

Resolver *resolver_create(Arena *arena, const char *filename);
int       resolver_run(Resolver *r, AstNode *program);   // returns 0 on error

// Exposed for multi-file use (linker / module system later)
Symbol   *scope_lookup(Scope *s, const char *name);

#endif // CIREN_RESOLVER_H
