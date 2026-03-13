#include <stdarg.h>
// ciren/resolver.c

#include "resolver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── UTILITIES ───────────────────────────────────────────────────────────────

static void res_error(Resolver *r, AstNode *node, const char *msg) {
    r->had_error = 1;
    int line = node ? node->line : 0;
    int col  = node ? node->col  : 0;
    fprintf(stderr, "[%s:%d:%d] resolver error: %s\n",
            r->filename, line, col, msg);
}

static void res_errorf(Resolver *r, AstNode *node, const char *fmt, ...) {
    r->had_error = 1;
    int line = node ? node->line : 0;
    int col  = node ? node->col  : 0;
    fprintf(stderr, "[%s:%d:%d] resolver error: ", r->filename, line, col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static char *dup_str(Arena *a, const char *s) {
    size_t len = strlen(s);
    char *out  = arena_alloc(a, len + 1);
    memcpy(out, s, len + 1);
    return out;
}

// ─── SCOPE ───────────────────────────────────────────────────────────────────

#define SCOPE_BUCKETS 64

static Scope *scope_create(Arena *a, Scope *parent, int is_global) {
    Scope *s        = arena_alloc(a, sizeof(Scope));
    s->parent       = parent;
    s->bucket_count = SCOPE_BUCKETS;
    s->buckets      = arena_alloc(a, SCOPE_BUCKETS * sizeof(Symbol *));
    s->symbol_count = 0;
    s->is_global    = is_global;
    return s;
}

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

// Insert into a specific scope (no parent walk)
static Symbol *scope_insert(Arena *a, Scope *s, const char *name,
                             SymbolKind kind, AstNode *decl,
                             AccessMod access) {
    uint32_t idx = hash_str(name) % s->bucket_count;

    // Check for duplicate in THIS scope only
    for (Symbol *sym = s->buckets[idx]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) return NULL;  // duplicate
    }

    Symbol *sym   = arena_alloc(a, sizeof(Symbol));
    sym->name     = dup_str(a, name);
    sym->kind     = kind;
    sym->decl     = decl;
    sym->access   = access;
    sym->type     = NULL;
    sym->is_resolved = 0;
    sym->resolving   = 0;
    sym->parent      = NULL;
    sym->next        = s->buckets[idx];
    s->buckets[idx]  = sym;
    s->symbol_count++;
    return sym;
}

// Lookup in this scope and all parents
Symbol *scope_lookup(Scope *s, const char *name) {
    for (Scope *sc = s; sc; sc = sc->parent) {
        uint32_t idx = hash_str(name) % sc->bucket_count;
        for (Symbol *sym = sc->buckets[idx]; sym; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    return NULL;
}

// Lookup in THIS scope only (for duplicate detection)
static Symbol *scope_lookup_local(Scope *s, const char *name) {
    uint32_t idx = hash_str(name) % s->bucket_count;
    for (Symbol *sym = s->buckets[idx]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) return sym;
    }
    return NULL;
}

// ─── SCOPE STACK MANAGEMENT ──────────────────────────────────────────────────

static void push_scope(Resolver *r) {
    r->current = scope_create(r->arena, r->current, 0);
}

static void pop_scope(Resolver *r) {
    if (r->current->parent)
        r->current = r->current->parent;
}

// ─── TYPE RESOLUTION ─────────────────────────────────────────────────────────
// Walk an AstType and verify all named types exist in scope.

static void resolve_type(Resolver *r, AstType *type, AstNode *ctx) {
    if (!type) return;

    switch (type->kind) {
        case TY_NAMED:
        case TY_GENERIC: {
            if (strcmp(type->name, "Self") == 0) break; // resolved at impl site
            if (strcmp(type->name, "Result") == 0) break; // built-in generic
            if (strcmp(type->name, "Option") == 0) break; // built-in generic
            Symbol *sym = scope_lookup(r->current, type->name);
            if (!sym) {
                res_errorf(r, ctx, "unknown type '%s'", type->name);
                return;
            }
            if (sym->kind != SYM_STRUCT    &&
                sym->kind != SYM_ENUM      &&
                sym->kind != SYM_INTERFACE &&
                sym->kind != SYM_GENERIC_PARAM) {
                res_errorf(r, ctx, "'%s' is not a type", type->name);
            }
            // Resolve generic args
            for (size_t i = 0; i < type->type_arg_count; i++)
                resolve_type(r, type->type_args[i], ctx);
            break;
        }
        case TY_POINTER:
        case TY_NULLABLE_PTR:
        case TY_SLICE:
            resolve_type(r, type->inner, ctx);
            break;
        case TY_ARRAY:
            resolve_type(r, type->elem_type, ctx);
            // array size expr resolved separately
            break;
        case TY_FUNC_PTR:
            for (size_t i = 0; i < type->param_count; i++)
                resolve_type(r, type->param_types[i], ctx);
            resolve_type(r, type->return_type, ctx);
            break;
        default:
            break;  // primitives need no resolution
    }
}

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────

static void resolve_node(Resolver *r, AstNode *node);
static void resolve_expr(Resolver *r, AstNode *node);

// ─── PASS 1: COLLECT ALL TOP-LEVEL SYMBOLS ───────────────────────────────────
// Walk the program once and register every top-level name.
// This is what makes declaration order not matter in Ciren.

static void collect_top_level(Resolver *r, AstNode *program) {
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl) continue;

        const char *name   = NULL;
        SymbolKind  kind   = SYM_FUNCTION;
        AccessMod   access = ACCESS_PRIVATE;

        switch (decl->kind) {

            case NODE_FUNC_DECL:
                name   = decl->as.func_decl.name;
                kind   = SYM_FUNCTION;
                access = decl->as.func_decl.access;
                break;

            case NODE_ASM_FUNC_DECL:
                name   = decl->as.asm_func_decl.name;
                kind   = SYM_ASM_FUNCTION;
                access = decl->as.asm_func_decl.access;
                break;

            case NODE_STRUCT_DECL:
            case NODE_UNION_DECL:
                name   = decl->as.struct_decl.name;
                kind   = SYM_STRUCT;
                access = decl->as.struct_decl.access;
                break;

            case NODE_ENUM_DECL:
                name   = decl->as.enum_decl.name;
                kind   = SYM_ENUM;
                access = decl->as.enum_decl.access;
                break;

            case NODE_INTERFACE_DECL:
                name   = decl->as.interface_decl.name;
                kind   = SYM_INTERFACE;
                access = decl->as.interface_decl.access;
                break;

            case NODE_IMPL_DECL:
                // Register each method as a global function
                for (size_t mi = 0; mi < decl->as.impl_decl.method_count; mi++) {
                    AstNode *m = decl->as.impl_decl.methods[mi];
                    if (m->kind == NODE_FUNC_DECL) {
                        scope_insert(r->arena, r->global,
                                     m->as.func_decl.name, SYM_FUNCTION,
                                     m, ACCESS_PUBLIC);
                    }
                }
                continue;

            case NODE_VAR_DECL:
                name   = decl->as.var_decl.name;
                kind   = decl->as.var_decl.is_const ? SYM_CONST : SYM_VAR;
                access = decl->as.var_decl.access;
                break;

            case NODE_USING:
            case NODE_MODULE:
                continue;  // handled separately

            default:
                continue;
        }

        if (!name) continue;

        Symbol *existing = scope_lookup_local(r->global, name);
        if (existing) {
            // Allow overriding built-in placeholder symbols (decl == NULL)
            if (existing->decl == NULL) {
                existing->decl   = decl;
                existing->kind   = kind;
                existing->access = access;
                continue;
            }
            res_errorf(r, decl, "duplicate symbol '%s' (first defined at line %d)",
                       name, existing->decl ? existing->decl->line : 0);
            continue;
        }

        Symbol *sym = scope_insert(r->arena, r->global, name, kind, decl, access);
        if (!sym) {
            res_errorf(r, decl, "failed to register symbol '%s'", name);
            continue;
        }

        // Track exports
        if (access == ACCESS_PUBLIC) {
            if (r->export_count >= r->export_cap) {
                r->export_cap *= 2;
                Symbol **new_exp = arena_alloc(r->arena, r->export_cap * sizeof(Symbol *));
                memcpy(new_exp, r->exports, r->export_count * sizeof(Symbol *));
                r->exports = new_exp;
            }
            r->exports[r->export_count++] = sym;
        }

        // For structs: also register their fields and methods into a nested scope
        // (stored on the symbol for method resolution later)
        if (decl->kind == NODE_STRUCT_DECL || decl->kind == NODE_UNION_DECL) {
            for (size_t m = 0; m < decl->as.struct_decl.method_count; m++) {
                AstNode *method = decl->as.struct_decl.methods[m];
                if (!method || method->kind != NODE_FUNC_DECL) continue;
                // Methods get registered when we resolve their struct
            }
        }

        // For enums: register each variant as a scoped symbol: EnumName.Variant
        if (decl->kind == NODE_ENUM_DECL) {
            for (size_t v = 0; v < decl->as.enum_decl.variant_count; v++) {
                AstVariant *var = &decl->as.enum_decl.variants[v];
                // Build "EnumName.VariantName"
                size_t full_len = strlen(name) + 1 + strlen(var->name) + 1;
                char *full_name = arena_alloc(r->arena, full_len);
                snprintf(full_name, full_len, "%s.%s", name, var->name);
                scope_insert(r->arena, r->global, full_name,
                             SYM_ENUM_VARIANT, decl, access);
            }
        }
    }
}

// ─── COLLECT USING IMPORTS ───────────────────────────────────────────────────
// Register imported module names as SYM_MODULE symbols so they can be
// referenced as prefixes (http.get, etc.).

static void inject_module_symbols(Resolver *r, const char *module) {
    // Each module pre-populates known symbols into global scope
    // so the resolver doesn't reject them as undefined.
    static const char *stdio_syms[] = {
        "printf", "scanf", "fprintf", "fscanf",
        "fopen", "fclose", "fread", "fwrite", "fgets", "fputs",
        "puts", "gets", "sprintf", "snprintf", "sscanf",
        "fflush", "feof", "ferror", "rewind", "fseek", "ftell",
        "stdin", "stdout", "stderr", NULL
    };
    static const char *memory_syms[] = {
        "malloc", "calloc", "realloc", "free",
        "memcpy", "memset", "memmove", "memcmp", NULL
    };
    static const char *string_syms[] = {
        "strlen", "strcmp", "strncmp", "strcpy", "strncpy",
        "strcat", "strncat", "strchr", "strstr", "strtok",
        "atoi", "atof", "atol", NULL
    };
    static const char *math_syms[] = {
        "sqrt", "pow", "abs", "fabs", "floor", "ceil",
        "sin", "cos", "tan", "log", "log2", "log10",
        "exp", "round", "fmin", "fmax", NULL
    };
    static const char *stdlib_syms[] = {
        "exit", "abort", "getenv", "system",
        "rand", "srand", "qsort", "bsearch", NULL
    };

    const char **syms = NULL;

    if      (strcmp(module, "stdio")  == 0) syms = stdio_syms;
    else if (strcmp(module, "memory") == 0) syms = memory_syms;
    else if (strcmp(module, "string") == 0) syms = string_syms;
    else if (strcmp(module, "math")   == 0) syms = math_syms;
    else if (strcmp(module, "sys")    == 0) syms = stdlib_syms;

    if (!syms) return;

    for (int i = 0; syms[i] != NULL; i++) {
        // Only insert if not already present
        if (!scope_lookup_local(r->global, syms[i]))
            scope_insert(r->arena, r->global, syms[i],
                         SYM_FUNCTION, NULL, ACCESS_PUBLIC);
    }
}

static void collect_imports(Resolver *r, AstNode *program) {
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_USING) continue;

        const char *mod_name = decl->as.using_decl.alias
            ? decl->as.using_decl.alias
            : decl->as.using_decl.path;

        // Inject known symbols from this module into global scope
        inject_module_symbols(r, decl->as.using_decl.path);

        if (decl->as.using_decl.symbol_count > 0) {
            for (size_t s = 0; s < decl->as.using_decl.symbol_count; s++) {
                const char *sym_name = decl->as.using_decl.symbols[s];
                if (!scope_lookup_local(r->global, sym_name))
                    scope_insert(r->arena, r->global, sym_name,
                                 SYM_MODULE, decl, ACCESS_PRIVATE);
            }
        } else {
            if (!scope_lookup_local(r->global, mod_name))
                scope_insert(r->arena, r->global, mod_name,
                             SYM_MODULE, decl, ACCESS_PRIVATE);
        }
    }
}

// ─── PASS 2: RESOLVE EXPRESSIONS ─────────────────────────────────────────────

static void resolve_expr(Resolver *r, AstNode *node) {
    if (!node) return;

    switch (node->kind) {

        // ── Literals — nothing to resolve ────────────
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STRING_LIT:
        case NODE_CSTRING_LIT:
        case NODE_CHAR_LIT:
        case NODE_BOOL_LIT:
        case NODE_NULL_LIT:
            return;

        // ── Identifier reference ──────────────────────
        case NODE_IDENT: {
            const char *name = node->as.ident.name;
            Symbol *sym = scope_lookup(r->current, name);
            if (!sym) {
                // Auto-declare as external C symbol
                scope_insert(r->arena, r->global, name,
                             SYM_FUNCTION, node, ACCESS_PUBLIC);
            }
            return;
        }

        // ── Binary ───────────────────────────────────
        case NODE_BINARY:
            resolve_expr(r, node->as.binary.left);
            resolve_expr(r, node->as.binary.right);
            return;

        // ── Unary / address / deref ───────────────────
        case NODE_UNARY:
        case NODE_ADDRESS_OF:
        case NODE_DEREF:
            resolve_expr(r, node->as.unary.operand);
            return;

        // ── Assignment ───────────────────────────────
        case NODE_ASSIGN:
            resolve_expr(r, node->as.assign.target);
            resolve_expr(r, node->as.assign.value);
            return;

        // ── Function call ────────────────────────────
        case NODE_CALL: {
            resolve_expr(r, node->as.call.callee);
            for (size_t i = 0; i < node->as.call.arg_count; i++)
                resolve_expr(r, node->as.call.args[i]);
            // Resolve explicit generic type args
            for (size_t i = 0; i < node->as.call.type_arg_count; i++)
                resolve_type(r, node->as.call.type_args[i], node);
            return;
        }

        // ── Field access: obj.field ───────────────────
        // We resolve the target; field correctness is deferred to type checker
        case NODE_FIELD:
            resolve_expr(r, node->as.field.target);
            return;

        // ── Index / slice ────────────────────────────
        case NODE_INDEX:
            resolve_expr(r, node->as.index.target);
            resolve_expr(r, node->as.index.index);
            return;

        case NODE_SLICE_EXPR:
            resolve_expr(r, node->as.slice_expr.target);
            if (node->as.slice_expr.from) resolve_expr(r, node->as.slice_expr.from);
            if (node->as.slice_expr.to)   resolve_expr(r, node->as.slice_expr.to);
            return;

        // ── Cast ─────────────────────────────────────
        case NODE_CAST:
            resolve_expr(r, node->as.cast.expr);
            resolve_type(r, node->as.cast.type, node);
            return;

        // ── Range ────────────────────────────────────
        case NODE_RANGE:
            resolve_expr(r, node->as.range.from);
            resolve_expr(r, node->as.range.to);
            return;

        // ── Struct literal ────────────────────────────
        case NODE_STRUCT_LITERAL: {
            // Verify the type exists
            if (node->as.struct_lit.type_name) {
                Symbol *sym = scope_lookup(r->current, node->as.struct_lit.type_name);
                if (!sym) {
                    res_errorf(r, node, "unknown type '%s' in struct literal",
                               node->as.struct_lit.type_name);
                } else if (sym->kind != SYM_STRUCT) {
                    res_errorf(r, node, "'%s' is not a struct",
                               node->as.struct_lit.type_name);
                }
            }
            for (size_t i = 0; i < node->as.struct_lit.field_count; i++)
                resolve_expr(r, node->as.struct_lit.field_values[i]);
            return;
        }

        // ── Array literal ─────────────────────────────
        case NODE_ARRAY_LITERAL:
            for (size_t i = 0; i < node->as.array_lit.count; i++)
                resolve_expr(r, node->as.array_lit.elems[i]);
            return;

        // ── If expression ─────────────────────────────
        case NODE_IF_EXPR:
            resolve_expr(r, node->as.if_stmt.condition);
            resolve_node(r, node->as.if_stmt.then_block);
            if (node->as.if_stmt.else_block)
                resolve_node(r, node->as.if_stmt.else_block);
            return;

        // ── Lambda ───────────────────────────────────
        case NODE_LAMBDA: {
            push_scope(r);
            for (size_t i = 0; i < node->as.lambda.param_count; i++) {
                AstParam *p = &node->as.lambda.params[i];
                resolve_type(r, p->type, node);
                Symbol *sym = scope_insert(r->arena, r->current, p->name,
                                           SYM_PARAM, node, ACCESS_PRIVATE);
                if (!sym)
                    res_errorf(r, node, "duplicate param '%s' in lambda", p->name);
                else
                    sym->type = p->type;
            }
            resolve_type(r, node->as.lambda.return_type, node);
            resolve_node(r, node->as.lambda.body);
            pop_scope(r);
            return;
        }

        // ── ? propagation ────────────────────────────
        case NODE_PROPAGATE:
            resolve_expr(r, node->as.propagate.expr);
            return;

        // ── sizeof / alignof ─────────────────────────
        case NODE_SIZEOF:
        case NODE_ALIGNOF:
            resolve_type(r, node->as.size_expr.type, node);
            return;

        // ── new ──────────────────────────────────────
        case NODE_NEW: {
            Symbol *sym = scope_lookup(r->current, node->as.new_expr.type_name);
            if (!sym)
                res_errorf(r, node, "unknown type '%s' in new expression",
                           node->as.new_expr.type_name);
            else if (sym->kind != SYM_STRUCT)
                res_errorf(r, node, "'%s' is not a struct",
                           node->as.new_expr.type_name);
            for (size_t i = 0; i < node->as.new_expr.field_count; i++)
                resolve_expr(r, node->as.new_expr.field_values[i]);
            return;
        }

        // ── delete ───────────────────────────────────
        case NODE_DELETE:
            resolve_expr(r, node->as.delete_expr.ptr);
            return;

        // ── Multi-assign ─────────────────────────────
        case NODE_MULTI_ASSIGN: {
            resolve_expr(r, node->as.multi_assign.value);
            for (size_t i = 0; i < node->as.multi_assign.count; i++) {
                const char *vname = node->as.multi_assign.names[i];
                Symbol *existing = scope_lookup_local(r->current, vname);
                if (existing)
                    res_errorf(r, node, "duplicate variable '%s' in multi-assign", vname);
                else
                    scope_insert(r->arena, r->current, vname,
                                 SYM_VAR, node, ACCESS_PRIVATE);
            }
            return;
        }

        default:
            return;
    }
}

// ─── PASS 2: RESOLVE STATEMENTS & DECLARATIONS ───────────────────────────────

static void resolve_node(Resolver *r, AstNode *node) {
    if (!node) return;

    switch (node->kind) {

        // ── Block: new scope ──────────────────────────
        case NODE_BLOCK: {
            push_scope(r);
            for (size_t i = 0; i < node->as.block.count; i++)
                resolve_node(r, node->as.block.stmts[i]);
            pop_scope(r);
            return;
        }

        // ── Variable declaration ──────────────────────
        case NODE_VAR_DECL: {
            // Resolve RHS first (so 'let x = x' catches the error properly)
            if (node->as.var_decl.value)
                resolve_expr(r, node->as.var_decl.value);

            resolve_type(r, node->as.var_decl.type, node);

            const char *name = node->as.var_decl.name;
            Symbol *existing = scope_lookup_local(r->current, name);
            // Top-level consts/vars already registered in pass 1 — update type but don't re-insert
            if (existing && r->current == r->global) {
                if (!existing->type) existing->type = node->as.var_decl.type;
                return;
            }
            if (existing)
                res_errorf(r, node, "variable '%s' already declared in this scope", name);
            else {
                SymbolKind kind = node->as.var_decl.is_const ? SYM_CONST : SYM_VAR;
                Symbol *sym = scope_insert(r->arena, r->current, name,
                                           kind, node, node->as.var_decl.access);
                if (sym) sym->type = node->as.var_decl.type;
            }
            return;
        }

        // ── Return ────────────────────────────────────
        case NODE_RETURN:
            if (node->as.ret.value)
                resolve_expr(r, node->as.ret.value);
            return;

        // ── If ────────────────────────────────────────
        case NODE_IF: {
            resolve_expr(r, node->as.if_stmt.condition);
            resolve_node(r, node->as.if_stmt.then_block);
            for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
                resolve_expr(r, node->as.if_stmt.else_if_conds[i]);
                resolve_node(r, node->as.if_stmt.else_if_blocks[i]);
            }
            if (node->as.if_stmt.else_block)
                resolve_node(r, node->as.if_stmt.else_block);
            return;
        }

        // ── While ─────────────────────────────────────
        case NODE_WHILE:
            resolve_expr(r, node->as.while_stmt.condition);
            resolve_node(r, node->as.while_stmt.body);
            return;

        // ── For range: for i in 0..10 ─────────────────
        case NODE_FOR_RANGE: {
            push_scope(r);
            resolve_expr(r, node->as.for_range.from);
            resolve_expr(r, node->as.for_range.to);
            Symbol *ivar = scope_insert(r->arena, r->current,
                                        node->as.for_range.var,
                                        SYM_VAR, node, ACCESS_PRIVATE);
            if (!ivar)
                res_errorf(r, node, "duplicate loop variable '%s'",
                           node->as.for_range.var);
            // Infer type as int (range always int for now)
            if (ivar) {
                ivar->type = type_alloc(r->arena, TY_INT);
                ivar->is_resolved = 1;
            }
            resolve_node(r, node->as.for_range.body);
            pop_scope(r);
            return;
        }

        // ── For in: for item in collection ────────────
        case NODE_FOR_IN: {
            push_scope(r);
            resolve_expr(r, node->as.for_in.iterable);
            scope_insert(r->arena, r->current,
                         node->as.for_in.var,
                         SYM_VAR, node, ACCESS_PRIVATE);
            resolve_node(r, node->as.for_in.body);
            pop_scope(r);
            return;
        }

        // ── For index: for i, item in collection ──────
        case NODE_FOR_INDEX: {
            push_scope(r);
            resolve_expr(r, node->as.for_index.iterable);
            scope_insert(r->arena, r->current,
                         node->as.for_index.index_var,
                         SYM_VAR, node, ACCESS_PRIVATE);
            scope_insert(r->arena, r->current,
                         node->as.for_index.value_var,
                         SYM_VAR, node, ACCESS_PRIVATE);
            resolve_node(r, node->as.for_index.body);
            pop_scope(r);
            return;
        }

        // ── C-style for ───────────────────────────────
        case NODE_FOR_C: {
            push_scope(r);
            resolve_node(r, node->as.for_c.init);
            resolve_expr(r, node->as.for_c.condition);
            resolve_expr(r, node->as.for_c.post);
            resolve_node(r, node->as.for_c.body);
            pop_scope(r);
            return;
        }

        // ── Loop ─────────────────────────────────────
        case NODE_LOOP:
            resolve_node(r, node->as.loop_stmt.body);
            return;

        // ── Match ─────────────────────────────────────
        case NODE_MATCH: {
            resolve_expr(r, node->as.match_stmt.subject);
            for (size_t i = 0; i < node->as.match_stmt.arm_count; i++) {
                MatchArm *arm = &node->as.match_stmt.arms[i];
                push_scope(r);

                // Pattern — could bind a name (e.g. Result.Ok(v))
                if (arm->pattern && arm->pattern->kind == NODE_ENUM_PATTERN) {
                    for (size_t b = 0; b < arm->pattern->as.enum_pattern.binding_count; b++) {
                        const char *bname = arm->pattern->as.enum_pattern.bindings[b];
                        scope_insert(r->arena, r->current, bname, SYM_VAR,
                                     arm->pattern, ACCESS_PRIVATE);
                    }
                } else if (arm->pattern && arm->pattern->kind == NODE_IDENT) {
                    const char *pname = arm->pattern->as.ident.name;
                    if (strcmp(pname, "_") != 0) {
                        Symbol *sym = scope_lookup(r->current, pname);
                        if (!sym || (sym->kind != SYM_ENUM_VARIANT &&
                                     sym->kind != SYM_ENUM)) {
                            if (arm->bind_name)
                                scope_insert(r->arena, r->current,
                                             arm->bind_name, SYM_VAR,
                                             arm->pattern, ACCESS_PRIVATE);
                        }
                    }
                } else if (arm->pattern) {
                    resolve_expr(r, arm->pattern);
                }

                if (arm->guard)  resolve_expr(r, arm->guard);
                resolve_node(r, arm->body);
                pop_scope(r);
            }
            return;
        }

        // ── Defer ─────────────────────────────────────
        case NODE_DEFER:
            resolve_node(r, node->as.defer_stmt.stmt);
            return;

        // ── Panic ─────────────────────────────────────
        case NODE_PANIC:
            resolve_expr(r, node->as.panic_stmt.msg);
            return;

        // ── ASM block — raw text, no resolution ───────
        case NODE_ASM_BLOCK:
            return;

        // ── Expression statement ──────────────────────
        case NODE_EXPR_STMT:
            resolve_expr(r, node->as.expr_stmt.expr);
            return;

        // ── Function declaration ──────────────────────
        case NODE_FUNC_DECL: {
            // Mark the symbol as being resolved (cycle guard)
            Symbol *fn_sym = scope_lookup(r->global,
                                          node->as.func_decl.name);
            if (fn_sym) {
                if (fn_sym->resolving) {
                    res_errorf(r, node, "cyclic resolution of '%s'",
                               node->as.func_decl.name);
                    return;
                }
                fn_sym->resolving = 1;
            }

            push_scope(r);

            // Register generic type params into function scope
            for (size_t i = 0; i < node->as.func_decl.generic_count; i++) {
                const char *tp = node->as.func_decl.generic_params[i];
                scope_insert(r->arena, r->current, tp,
                             SYM_GENERIC_PARAM, node, ACCESS_PRIVATE);
            }

            // Resolve + register parameters
            for (size_t i = 0; i < node->as.func_decl.param_count; i++) {
                AstParam *p = &node->as.func_decl.params[i];
                resolve_type(r, p->type, node);
                if (p->default_value) resolve_expr(r, p->default_value);
                Symbol *sym = scope_insert(r->arena, r->current, p->name,
                                           SYM_PARAM, node, ACCESS_PRIVATE);
                if (!sym)
                    res_errorf(r, node, "duplicate parameter '%s'", p->name);
                else
                    sym->type = p->type;
            }

            resolve_type(r, node->as.func_decl.return_type, node);
            resolve_node(r, node->as.func_decl.body);
            pop_scope(r);

            if (fn_sym) {
                fn_sym->resolving    = 0;
                fn_sym->is_resolved  = 1;
            }
            return;
        }

        // ── ASM function — no expression resolution ───
        case NODE_ASM_FUNC_DECL: {
            // Resolve param types and register in a scope for tooling
            push_scope(r);
            for (size_t i = 0; i < node->as.asm_func_decl.param_count; i++) {
                AsmParam *p = &node->as.asm_func_decl.params[i];
                resolve_type(r, p->type, node);
                scope_insert(r->arena, r->current, p->name,
                             SYM_PARAM, node, ACCESS_PRIVATE);
            }
            pop_scope(r);

            Symbol *sym = scope_lookup(r->global, node->as.asm_func_decl.name);
            if (sym) sym->is_resolved = 1;
            return;
        }

        // ── Struct declaration ────────────────────────
        case NODE_UNION_DECL:
        case NODE_STRUCT_DECL: {
            Symbol *struct_sym = scope_lookup(r->global,
                                              node->as.struct_decl.name);
            if (struct_sym && struct_sym->resolving) {
                res_errorf(r, node, "cyclic struct definition '%s'",
                           node->as.struct_decl.name);
                return;
            }
            if (struct_sym) struct_sym->resolving = 1;

            push_scope(r);

            // Generic params
            for (size_t i = 0; i < node->as.struct_decl.generic_count; i++)
                scope_insert(r->arena, r->current,
                             node->as.struct_decl.generic_params[i],
                             SYM_GENERIC_PARAM, node, ACCESS_PRIVATE);

            // Embedded structs — verify they exist
            for (size_t i = 0; i < node->as.struct_decl.embed_count; i++) {
                const char *ename = node->as.struct_decl.embeds[i];
                Symbol *esym = scope_lookup(r->current, ename);
                if (!esym)
                    res_errorf(r, node, "embedded type '%s' not found", ename);
                else if (esym->kind != SYM_STRUCT)
                    res_errorf(r, node, "'%s' is not a struct and cannot be embedded",
                               ename);
            }

            // Fields
            for (size_t i = 0; i < node->as.struct_decl.field_count; i++) {
                AstField *f = &node->as.struct_decl.fields[i];
                resolve_type(r, f->type, node);
                if (f->default_value) resolve_expr(r, f->default_value);

                // Register field in struct scope
                Symbol *fsym = scope_insert(r->arena, r->current, f->name,
                                            SYM_FIELD, node, ACCESS_PUBLIC);
                if (!fsym)
                    res_errorf(r, node, "duplicate field '%s' in struct '%s'",
                               f->name, node->as.struct_decl.name);
                else {
                    fsym->type   = f->type;
                    fsym->parent = struct_sym;
                }
            }

            // Methods — each method gets its own scope inside the struct scope
            for (size_t i = 0; i < node->as.struct_decl.method_count; i++)
                resolve_node(r, node->as.struct_decl.methods[i]);

            pop_scope(r);
            if (struct_sym) {
                struct_sym->resolving   = 0;
                struct_sym->is_resolved = 1;
            }
            return;
        }

        // ── Enum declaration ──────────────────────────
        case NODE_ENUM_DECL: {
            // Variants already registered in pass 1
            // Resolve tagged union field types
            for (size_t i = 0; i < node->as.enum_decl.variant_count; i++) {
                AstVariant *v = &node->as.enum_decl.variants[i];
                for (size_t j = 0; j < v->field_count; j++)
                    resolve_type(r, v->fields[j].type, node);
                if (v->explicit_value)
                    resolve_expr(r, v->explicit_value);
            }
            Symbol *sym = scope_lookup(r->global, node->as.enum_decl.name);
            if (sym) sym->is_resolved = 1;
            return;
        }

        // ── Interface declaration ─────────────────────
        case NODE_INTERFACE_DECL: {
            // Verify parent interfaces exist
            for (size_t i = 0; i < node->as.interface_decl.parent_count; i++) {
                const char *pname = node->as.interface_decl.parents[i];
                Symbol *psym = scope_lookup(r->global, pname);
                if (!psym)
                    res_errorf(r, node, "unknown parent interface '%s'", pname);
                else if (psym->kind != SYM_INTERFACE)
                    res_errorf(r, node, "'%s' is not an interface", pname);
            }
            // Resolve method signatures
            for (size_t i = 0; i < node->as.interface_decl.method_count; i++)
                resolve_node(r, node->as.interface_decl.methods[i]);

            Symbol *sym = scope_lookup(r->global, node->as.interface_decl.name);
            if (sym) sym->is_resolved = 1;
            return;
        }

        case NODE_IMPL_DECL: {
            const char *type_name = node->as.impl_decl.type_name;
            for (size_t i = 0; i < node->as.impl_decl.method_count; i++) {
                AstNode *m = node->as.impl_decl.methods[i];
                if (m->kind != NODE_FUNC_DECL) continue;
                // Replace 'Self' params with the concrete type
                for (size_t pi = 0; pi < m->as.func_decl.param_count; pi++) {
                    AstParam *param = &m->as.func_decl.params[pi];
                    if (param->type && param->type->kind == TY_NAMED
                            && strcmp(param->type->name, "Self") == 0) {
                        param->type->name = (char *)type_name;
                    }
                }
                resolve_node(r, m);
            }
            return;
        }
        // ── Using / module — handled in pass 1 ────────
        case NODE_USING:
        case NODE_MODULE:
            return;

        // ── Program root ──────────────────────────────
        case NODE_PROGRAM:
            for (size_t i = 0; i < node->as.program.count; i++)
                resolve_node(r, node->as.program.decls[i]);
            return;

        default:
            resolve_expr(r, node);
            return;
    }
}

// ─── DEFERRED: INTERFACE SATISFACTION CHECK ───────────────────────────────────
// After all symbols are resolved, verify that any struct used as an interface
// type actually implements all the required methods.

static int struct_satisfies_interface(Resolver *r,
                                      AstNode *struct_decl,
                                      AstNode *iface_decl) {
    int ok = 1;
    for (size_t i = 0; i < iface_decl->as.interface_decl.method_count; i++) {
        AstNode *req = iface_decl->as.interface_decl.methods[i];
        if (!req || req->kind != NODE_FUNC_DECL) continue;

        const char *method_name = req->as.func_decl.name;
        int found = 0;

        for (size_t j = 0; j < struct_decl->as.struct_decl.method_count; j++) {
            AstNode *m = struct_decl->as.struct_decl.methods[j];
            if (!m || m->kind != NODE_FUNC_DECL) continue;
            if (strcmp(m->as.func_decl.name, method_name) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            res_errorf(r, struct_decl,
                       "struct '%s' does not implement method '%s' "
                       "required by interface '%s'",
                       struct_decl->as.struct_decl.name,
                       method_name,
                       iface_decl->as.interface_decl.name);
            ok = 0;
        }
    }
    return ok;
}

static void run_deferred_checks(Resolver *r, AstNode *program) {
    // For every struct that is used where an interface is expected,
    // verify it satisfies the interface. We do a broad pass here:
    // check all struct params typed as interfaces.
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_FUNC_DECL) continue;

        for (size_t j = 0; j < decl->as.func_decl.param_count; j++) {
            AstParam *p = &decl->as.func_decl.params[j];
            if (!p->type || p->type->kind != TY_NAMED) continue;

            Symbol *psym = scope_lookup(r->global, p->type->name);
            if (!psym || psym->kind != SYM_INTERFACE) continue;

            // We know this param expects an interface.
            // Full call-site checking happens in the type checker (next stage).
            // Here we just verify the interface itself is well-formed.
        }
    }
}

// ─── ENTRY POINT ─────────────────────────────────────────────────────────────

Resolver *resolver_create(Arena *arena, const char *filename) {
    Resolver *r      = arena_alloc(arena, sizeof(Resolver));
    r->arena         = arena;
    r->filename      = filename;
    r->had_error     = 0;
    r->global        = scope_create(arena, NULL, 1);
    r->current       = r->global;
    // Inject built-in types and constructors
    if (!scope_lookup_local(r->global, "Result"))
        scope_insert(arena, r->global, "Result", SYM_STRUCT,   NULL, ACCESS_PUBLIC);
    if (!scope_lookup_local(r->global, "Ok"))
        scope_insert(arena, r->global, "Ok",     SYM_FUNCTION, NULL, ACCESS_PUBLIC);
    if (!scope_lookup_local(r->global, "Err"))
        scope_insert(arena, r->global, "Err",    SYM_FUNCTION, NULL, ACCESS_PUBLIC);
    if (!scope_lookup_local(r->global, "Some"))
        scope_insert(arena, r->global, "Some",   SYM_FUNCTION, NULL, ACCESS_PUBLIC);
    if (!scope_lookup_local(r->global, "None"))
        scope_insert(arena, r->global, "None",   SYM_FUNCTION, NULL, ACCESS_PUBLIC);
    r->export_cap    = 32;
    r->exports       = arena_alloc(arena, r->export_cap * sizeof(Symbol *));
    r->export_count  = 0;
    r->deferred_cap  = 16;
    r->deferred      = arena_alloc(arena, r->deferred_cap * sizeof(AstNode *));
    r->deferred_count = 0;
    return r;
}

int resolver_run(Resolver *r, AstNode *program) {
    // ── PASS 1: collect all top-level symbols ────────
    collect_imports(r, program);
    collect_top_level(r, program);

    if (r->had_error) return 0;

    // ── PASS 2: resolve all references ───────────────
    resolve_node(r, program);

    if (r->had_error) return 0;

    // ── PASS 3: deferred checks ───────────────────────
    run_deferred_checks(r, program);

    return !r->had_error;
}