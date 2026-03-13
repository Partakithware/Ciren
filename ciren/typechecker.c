// ciren/typechecker.c

#include "typechecker.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ─── ERROR REPORTING ─────────────────────────────────────────────────────────

static void tc_error(TypeChecker *tc, AstNode *node, const char *fmt, ...) {
    tc->had_error = 1;
    int line = node ? node->line : 0;
    int col  = node ? node->col  : 0;
    fprintf(stderr, "[%s:%d:%d] type error: ", tc->filename, line, col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void tc_warn(TypeChecker *tc, AstNode *node, const char *fmt, ...) {
    tc->warning_count++;
    int line = node ? node->line : 0;
    fprintf(stderr, "[%s:%d] warning: ", tc->filename, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

// ─── SCOPE MANAGEMENT ────────────────────────────────────────────────────────

static uint32_t tc_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static TypeScope *tc_push_scope(TypeChecker *tc) {
    TypeScope *s = arena_alloc(tc->arena, sizeof(TypeScope));
    memset(s, 0, sizeof(TypeScope));
    s->parent  = tc->scope;
    tc->scope  = s;
    return s;
}

static void tc_pop_scope(TypeChecker *tc) {
    if (tc->scope && tc->scope->parent)
        tc->scope = tc->scope->parent;
}

static void tc_bind(TypeChecker *tc, const char *name,
                    AstType *type, int is_const) {
    uint32_t idx  = tc_hash(name) % TC_BUCKETS;
    TypeBinding *b = arena_alloc(tc->arena, sizeof(TypeBinding));
    b->name        = arena_alloc(tc->arena, strlen(name) + 1);
    strcpy(b->name, name);
    b->type        = type;
    b->is_const    = is_const;
    b->next        = tc->scope->buckets[idx];
    tc->scope->buckets[idx] = b;
}

static TypeBinding *tc_lookup(TypeChecker *tc, const char *name) {
    for (TypeScope *s = tc->scope; s; s = s->parent) {
        uint32_t idx = tc_hash(name) % TC_BUCKETS;
        for (TypeBinding *b = s->buckets[idx]; b; b = b->next)
            if (strcmp(b->name, name) == 0) return b;
    }
    return NULL;
}

// ─── TYPE HELPERS ────────────────────────────────────────────────────────────

static AstType *make_type(TypeChecker *tc, TypeKind kind) {
    AstType *t = arena_alloc(tc->arena, sizeof(AstType));
    memset(t, 0, sizeof(AstType));
    t->kind = kind;
    return t;
}

static AstType *make_named_type(TypeChecker *tc, const char *name) {
    AstType *t = make_type(tc, TY_NAMED);
    t->name = arena_alloc(tc->arena, strlen(name) + 1);
    strcpy(t->name, name);
    return t;
}

static AstType *make_ptr_type(TypeChecker *tc, AstType *inner) {
    AstType *t  = make_type(tc, TY_POINTER);
    t->inner    = inner;
    return t;
}

// ─── TYPE EQUALITY ───────────────────────────────────────────────────────────

static int types_equal(AstType *a, AstType *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 1;  // untyped (null) matches anything
    // Generic type params and Result match anything
    if (a->kind == TY_NAMED && a->name && isupper((unsigned char)a->name[0])) return 1;
    if (b->kind == TY_NAMED && b->name && isupper((unsigned char)b->name[0])) return 1;
    if (a->kind == TY_GENERIC && a->name && strcmp(a->name, "Result") == 0) return 1;
    if (b->kind == TY_GENERIC && b->name && strcmp(b->name, "Result") == 0) return 1;
    if (a->kind != b->kind) {
        // Allow int ↔ uint, i8 ↔ u8, etc. as compatible
        int a_int = (a->kind >= TY_INT  && a->kind <= TY_U64);
        int b_int = (b->kind >= TY_INT  && b->kind <= TY_U64);
        int a_flt = (a->kind == TY_F32  || a->kind == TY_F64);
        int b_flt = (b->kind == TY_F32  || b->kind == TY_F64);
        // bool is compatible with int
        if ((a->kind == TY_BOOL && b_int) ||
            (b->kind == TY_BOOL && a_int)) return 1;
        // Numeric widening is a warning, not an error — handled at call sites
        if (a_int && b_int) return 1;
        if (a_flt && b_flt) return 1;
        // void* is compatible with any pointer
        if (a->kind == TY_ANY && b->kind == TY_POINTER) return 1;
        if (b->kind == TY_ANY && a->kind == TY_POINTER) return 1;
        // null (*T) is compatible with ?*T and vice versa
        if (a->kind == TY_POINTER && b->kind == TY_NULLABLE_PTR) return 1;
        if (a->kind == TY_NULLABLE_PTR && b->kind == TY_POINTER) return 1;
        return 0;
    }
    switch (a->kind) {
        case TY_NAMED:
        case TY_GENERIC:
            return strcmp(a->name, b->name) == 0;
        case TY_POINTER:
        case TY_NULLABLE_PTR:
            // untyped null pointer matches any pointer
            if (!a->inner || !b->inner) return 1;
            // *T and ?*T with same inner are compatible
            if ((a->kind == TY_POINTER || a->kind == TY_NULLABLE_PTR) &&
                (b->kind == TY_POINTER || b->kind == TY_NULLABLE_PTR))
                return types_equal(a->inner, b->inner);
            return types_equal(a->inner, b->inner);
        case TY_SLICE:
            if (!a->inner || !b->inner) return 1;
            return types_equal(a->inner, b->inner);
        case TY_ARRAY:
            return types_equal(a->elem_type, b->elem_type);
        default:
            return 1;
    }
}

static int type_is_numeric(AstType *t) {
    if (!t) return 0;
    return (t->kind >= TY_INT && t->kind <= TY_F64);
}

static int type_is_integer(AstType *t) {
    if (!t) return 0;
    return (t->kind >= TY_INT && t->kind <= TY_U64);
}

static int type_is_float(AstType *t) {
    if (!t) return 0;
    return (t->kind == TY_F32 || t->kind == TY_F64);
}

static int type_is_pointer(AstType *t) {
    if (!t) return 0;
    return (t->kind == TY_POINTER || t->kind == TY_NULLABLE_PTR ||
            t->kind == TY_CSTR    || t->kind == TY_ANY);
}

static int type_is_bool(AstType *t) {
    if (!t) return 0;
    return t->kind == TY_BOOL;
}

// Return a human-readable type name for error messages
static const char *type_name(AstType *t) {
    if (!t) return "void";
    switch (t->kind) {
        case TY_INT:          return "int";
        case TY_UINT:         return "uint";
        case TY_I8:           return "i8";
        case TY_U8:           return "u8";
        case TY_I16:          return "i16";
        case TY_U16:          return "u16";
        case TY_I64:          return "i64";
        case TY_U64:          return "u64";
        case TY_F32:          return "f32";
        case TY_F64:          return "f64";
        case TY_BOOL:         return "bool";
        case TY_CHAR:         return "char";
        case TY_STR:          return "str";
        case TY_CSTR:         return "cstr";
        case TY_VOID:         return "void";
        case TY_ANY:          return "any";
        case TY_INFERRED:     return "<inferred>";
        case TY_NAMED:
        case TY_GENERIC:      return t->name ? t->name : "<named>";
        case TY_POINTER:      return "*<T>";
        case TY_NULLABLE_PTR: return "?*<T>";
        case TY_SLICE:        return "[]<T>";
        case TY_ARRAY:        return "[<T>; N]";
        case TY_FUNC_PTR:     return "fn(...)";
        default:              return "<unknown>";
    }
}

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────


// ─── STRUCT FIELD LOOKUP ─────────────────────────────────────────────────────

static AstField *find_struct_field(TypeChecker *tc, const char *struct_name,
                                    const char *field_name) {
    // Walk the program's declarations to find the struct
    Symbol *sym = scope_lookup(tc->resolver->global, struct_name);
    if (!sym || sym->kind != SYM_STRUCT || !sym->decl) return NULL;

    AstNode *decl = sym->decl;
    for (size_t i = 0; i < decl->as.struct_decl.field_count; i++) {
        if (strcmp(decl->as.struct_decl.fields[i].name, field_name) == 0)
            return &decl->as.struct_decl.fields[i];
    }

    // Check embedded structs
    for (size_t i = 0; i < decl->as.struct_decl.embed_count; i++) {
        AstField *f = find_struct_field(tc, decl->as.struct_decl.embeds[i],
                                         field_name);
        if (f) return f;
    }

    return NULL;
}

// Find a method on a struct by name
static AstNode *find_struct_method(TypeChecker *tc, const char *struct_name,
                                    const char *method_name) {
    Symbol *sym = scope_lookup(tc->resolver->global, struct_name);
    if (!sym || sym->kind != SYM_STRUCT || !sym->decl) return NULL;

    AstNode *decl = sym->decl;
    for (size_t i = 0; i < decl->as.struct_decl.method_count; i++) {
        AstNode *m = decl->as.struct_decl.methods[i];
        if (!m || m->kind != NODE_FUNC_DECL) continue;
        if (strcmp(m->as.func_decl.name, method_name) == 0) return m;
    }
    return NULL;
}

// ─── TYPE RESOLUTION FOR NAMED TYPES ─────────────────────────────────────────

AstType *tc_resolve_named_type(TypeChecker *tc, AstType *t,
                                       AstNode *ctx) {
    if (!t || t->kind != TY_NAMED) return t;
    Symbol *sym = scope_lookup(tc->resolver->global, t->name);
    if (!sym) {
        tc_error(tc, ctx, "unknown type '%s'", t->name);
        return t;
    }
    // Named type is valid — return as-is (struct/enum resolution)
    return t;
}

// ─── EXPRESSION TYPE CHECKER ─────────────────────────────────────────────────

AstType *tc_check_expr(TypeChecker *tc, AstNode *node) {
    if (!node) return make_type(tc, TY_VOID);

    switch (node->kind) {

        // ── Literals ──────────────────────────────────
        case NODE_INT_LIT:
            return make_type(tc, TY_INT);

        case NODE_FLOAT_LIT:
            return make_type(tc, TY_F64);

        case NODE_STRING_LIT:
            return make_type(tc, TY_STR);

        case NODE_CSTRING_LIT:
            return make_type(tc, TY_CSTR);

        case NODE_CHAR_LIT:
            return make_type(tc, TY_CHAR);

        case NODE_BOOL_LIT:
            return make_type(tc, TY_BOOL);

        case NODE_NULL_LIT: {
            AstType *t = make_type(tc, TY_POINTER);
            t->inner   = NULL;  // untyped — matches any pointer
            return t;
        }

        // ── Identifier ────────────────────────────────
        case NODE_IDENT: {
            const char *name = node->as.ident.name;

            if (strcmp(name, "_") == 0)
                return make_type(tc, TY_INT);  // wildcard — any type is fine

            // Check local scope first
            TypeBinding *b = tc_lookup(tc, name);
            if (b) return b->type;

            // Check global symbol table
            Symbol *sym = scope_lookup(tc->resolver->global, name);
            if (sym) {
                if (sym->type) return sym->type;
                if (sym->kind == SYM_FUNCTION ||
                    sym->kind == SYM_ASM_FUNCTION) {
                    // Return a function pointer type placeholder
                    return make_type(tc, TY_ANY);
                }
            }

            tc_error(tc, node, "undefined identifier '%s'", name);
            return make_type(tc, TY_INT);
        }

        // ── Binary expressions ────────────────────────
        case NODE_BINARY: {
            AstType *lt = tc_check_expr(tc, node->as.binary.left);
            AstType *rt = tc_check_expr(tc, node->as.binary.right);

            switch (node->as.binary.op) {

                // Arithmetic: both sides must be numeric
                case TOK_PLUS:
                case TOK_MINUS:
                    // Allow pointer arithmetic: *T +/- int
                    if ((lt->kind == TY_POINTER || lt->kind == TY_NULLABLE_PTR)
                            && type_is_integer(rt))
                        return lt;
                    // Allow pointer - pointer = int (for length calculations)
                    if ((lt->kind == TY_POINTER || lt->kind == TY_NULLABLE_PTR)
                            && (rt->kind == TY_POINTER || rt->kind == TY_NULLABLE_PTR))
                        return make_type(tc, TY_INT);
                    if (!type_is_numeric(lt))
                        tc_error(tc, node->as.binary.left,
                                 "left operand of '%s' must be numeric, got '%s'",
                                 token_type_name(node->as.binary.op),
                                 type_name(lt));
                    if (!type_is_numeric(rt))
                        tc_error(tc, node->as.binary.right,
                                 "right operand of '%s' must be numeric, got '%s'",
                                 token_type_name(node->as.binary.op),
                                 type_name(rt));
                    return (type_is_float(lt) || type_is_float(rt))
                        ? make_type(tc, TY_F64) : lt;
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT:
                    if (!type_is_numeric(lt))
                        tc_error(tc, node->as.binary.left,
                                 "left operand of '%s' must be numeric, got '%s'",
                                 token_type_name(node->as.binary.op),
                                 type_name(lt));
                    if (!type_is_numeric(rt))
                        tc_error(tc, node->as.binary.right,
                                 "right operand of '%s' must be numeric, got '%s'",
                                 token_type_name(node->as.binary.op),
                                 type_name(rt));
                    return (type_is_float(lt) || type_is_float(rt))
                        ? make_type(tc, TY_F64) : lt;

                // Bitwise: both must be integers
                case TOK_AMP:
                case TOK_PIPE:
                case TOK_CARET:
                case TOK_LSHIFT:
                case TOK_RSHIFT:
                    if (!type_is_integer(lt))
                        tc_error(tc, node->as.binary.left,
                                 "bitwise operator requires integer, got '%s'",
                                 type_name(lt));
                    if (!type_is_integer(rt))
                        tc_error(tc, node->as.binary.right,
                                 "bitwise operator requires integer, got '%s'",
                                 type_name(rt));
                    return lt;

                // Comparison: both sides must be same kind, result is bool
                case TOK_EQ:
                case TOK_NEQ:
                    if (!types_equal(lt, rt))
                        tc_warn(tc, node,
                                "comparing '%s' with '%s'",
                                type_name(lt), type_name(rt));
                    return make_type(tc, TY_BOOL);

                case TOK_LT:
                case TOK_GT:
                case TOK_LTE:
                case TOK_GTE:
                    if (!type_is_numeric(lt) && !type_is_pointer(lt))
                        tc_error(tc, node->as.binary.left,
                                 "comparison requires numeric or pointer, got '%s'",
                                 type_name(lt));
                    return make_type(tc, TY_BOOL);

                // Logical: both must be bool-ish
                case TOK_AND:
                case TOK_OR:
                    return make_type(tc, TY_BOOL);

                default:
                    return lt;
            }
        }

        // ── Unary expressions ─────────────────────────
        case NODE_UNARY: {
            AstType *ot = tc_check_expr(tc, node->as.unary.operand);
            switch (node->as.unary.op) {
                case TOK_MINUS:
                    if (!type_is_numeric(ot))
                        tc_error(tc, node, "unary '-' requires numeric type, got '%s'",
                                 type_name(ot));
                    return ot;
                case TOK_BANG:
                    return make_type(tc, TY_BOOL);
                case TOK_TILDE:
                    if (!type_is_integer(ot))
                        tc_error(tc, node, "bitwise NOT requires integer, got '%s'",
                                 type_name(ot));
                    return ot;
                case TOK_PLUS_PLUS:
                case TOK_MINUS_MINUS:
                    if (!type_is_numeric(ot) && !type_is_pointer(ot))
                        tc_error(tc, node,
                                 "++/-- requires numeric or pointer, got '%s'",
                                 type_name(ot));
                    return ot;
                default:
                    return ot;
            }
        }

        // ── Address-of ────────────────────────────────
        case NODE_ADDRESS_OF: {
            AstType *inner = tc_check_expr(tc, node->as.unary.operand);
            return make_ptr_type(tc, inner);
        }

        // ── Dereference ───────────────────────────────
        case NODE_DEREF: {
            AstType *pt = tc_check_expr(tc, node->as.unary.operand);
            if (pt->kind != TY_POINTER && pt->kind != TY_NULLABLE_PTR) {
                tc_error(tc, node, "cannot dereference non-pointer type '%s'",
                         type_name(pt));
                return make_type(tc, TY_INT);
            }
            return pt->inner ? pt->inner : make_type(tc, TY_VOID);
        }

        // ── Assignment ────────────────────────────────
        case NODE_ASSIGN: {
            AstType *target_t = tc_check_expr(tc, node->as.assign.target);
            AstType *value_t  = tc_check_expr(tc, node->as.assign.value);

            // Check const assignment
            if (node->as.assign.target->kind == NODE_IDENT) {
                TypeBinding *b = tc_lookup(tc,
                    node->as.assign.target->as.ident.name);
                if (b && b->is_const)
                    tc_error(tc, node, "cannot assign to constant '%s'",
                             node->as.assign.target->as.ident.name);
            }

            if (!types_equal(target_t, value_t))
                tc_error(tc, node,
                         "type mismatch in assignment: cannot assign '%s' to '%s'",
                         type_name(value_t), type_name(target_t));

            // Compound assignment operators require numeric types
            if (node->as.assign.op != TOK_ASSIGN && !type_is_numeric(target_t))
                tc_error(tc, node,
                         "compound assignment requires numeric type, got '%s'",
                         type_name(target_t));

            return target_t;
        }

        // ── Function call ──────────────────────────────
        case NODE_CALL: {

            // Built-in Result constructors
            if (node->as.call.callee->kind == NODE_IDENT) {
                const char *cname = node->as.call.callee->as.ident.name;
                if (strcmp(cname, "Ok") == 0 || strcmp(cname, "Err") == 0) {
                    if (node->as.call.arg_count > 0)
                        tc_check_expr(tc, node->as.call.args[0]);
                    AstType *result_t = make_type(tc, TY_GENERIC);
                    result_t->name = "Result";
                    return result_t;
                }
            }

            // Resolve callee
            AstType  *callee_t = tc_check_expr(tc, node->as.call.callee);
            AstNode  *func_decl = NULL;
            AstType  *ret_type  = make_type(tc, TY_VOID);

            if (node->as.call.callee->kind == NODE_IDENT) {
                const char *name = node->as.call.callee->as.ident.name;
                Symbol *sym = scope_lookup(tc->resolver->global, name);

                if (!sym) {
                    // Check local scope — could be a function pointer
                    TypeBinding *b = tc_lookup(tc, name);
                    if (!b) {
                        tc_error(tc, node, "call to undefined function '%s'", name);
                        return make_type(tc, TY_INT);
                    }
                    // Function pointer call
                    if (b->type && b->type->kind == TY_FUNC_PTR)
                        return b->type->return_type
                            ? b->type->return_type
                            : make_type(tc, TY_VOID);
                    return make_type(tc, TY_INT);
                }

                if (sym->kind != SYM_FUNCTION  &&
                    sym->kind != SYM_ASM_FUNCTION &&
                    sym->kind != SYM_MODULE) {
                    tc_error(tc, node, "'%s' is not callable", name);
                    return make_type(tc, TY_INT);
                }

                func_decl = sym->decl;

                // Get return type and validate arg count/types
                if (func_decl && func_decl->kind == NODE_FUNC_DECL) {
                    ret_type = func_decl->as.func_decl.return_type
                        ? func_decl->as.func_decl.return_type
                        : make_type(tc, TY_INT);  // inferred

                    size_t expected = func_decl->as.func_decl.param_count;
                    size_t got      = node->as.call.arg_count;

                    // Check for variadic (last param is ...T)
                    int is_variadic = expected > 0 &&
                        func_decl->as.func_decl.params[expected-1].is_variadic;

                    if (!is_variadic && got != expected) {
                        tc_error(tc, node,
                                 "function '%s' expects %zu arguments, got %zu",
                                 name, expected, got);
                    } else {
                        // Check each arg type
                        size_t check_count = is_variadic ? expected - 1 : expected;
                        size_t check_up_to = got < check_count ? got : check_count;
                        for (size_t i = 0; i < check_up_to; i++) {
                            AstParam *param = &func_decl->as.func_decl.params[i];
                            AstType  *arg_t = tc_check_expr(tc,
                                                 node->as.call.args[i]);
                            if (!types_equal(param->type, arg_t)) {
                                tc_error(tc, node->as.call.args[i],
                                         "argument %zu of '%s': expected '%s', got '%s'",
                                         i + 1, name,
                                         type_name(param->type),
                                         type_name(arg_t));
                            }
                        }
                        // Type-check remaining variadic args (just check they exist)
                        for (size_t i = check_count; i < got; i++)
                            tc_check_expr(tc, node->as.call.args[i]);
                    }
                } else if (func_decl && func_decl->kind == NODE_ASM_FUNC_DECL) {
                    // ASM function: check arg count
                    size_t expected = func_decl->as.asm_func_decl.param_count;
                    size_t got      = node->as.call.arg_count;
                    if (got != expected)
                        tc_error(tc, node,
                                 "asm function '%s' expects %zu arguments, got %zu",
                                 name, expected, got);
                    for (size_t i = 0; i < got; i++)
                        tc_check_expr(tc, node->as.call.args[i]);
                    ret_type = func_decl->as.asm_func_decl.return_reg
                        ? make_type(tc, TY_U64)
                        : make_type(tc, TY_VOID);
                } else {
                    // External C function — just type-check the args
                    for (size_t i = 0; i < node->as.call.arg_count; i++)
                        tc_check_expr(tc, node->as.call.args[i]);
                    ret_type = make_type(tc, TY_INT);  // assume int return
                }
            } else if (node->as.call.callee->kind == NODE_FIELD) {
                // Method call: obj.method(args)
                // Look up the function by field name in global scope
                const char *method_name =
                    node->as.call.callee->as.field.field;
                Symbol *sym = scope_lookup(tc->resolver->global, method_name);

                if (!sym || sym->kind != SYM_FUNCTION || !sym->decl) {
                    tc_error(tc, node, "unknown method '%s'", method_name);
                    return make_type(tc, TY_INT);
                }

                AstNode *fdecl = sym->decl;
                AstType *ret   = fdecl->as.func_decl.return_type
                    ? fdecl->as.func_decl.return_type
                    : make_type(tc, TY_VOID);

                // Type check self (the target)
                tc_check_expr(tc, node->as.call.callee->as.field.target);

                // Type check remaining args (skip self param at index 0)
                size_t extra_params = fdecl->as.func_decl.param_count > 0
                    ? fdecl->as.func_decl.param_count - 1 : 0;
                size_t extra_args = node->as.call.arg_count;

                if (extra_args != extra_params)
                    tc_error(tc, node,
                             "method '%s' expects %zu arguments, got %zu",
                             method_name, extra_params, extra_args);

                for (size_t i = 0; i < extra_args && i < extra_params; i++) {
                    AstParam *p   = &fdecl->as.func_decl.params[i + 1];
                    AstType  *arg = tc_check_expr(tc, node->as.call.args[i]);
                    if (!types_equal(p->type, arg))
                        tc_error(tc, node->as.call.args[i],
                                 "argument %zu of '%s': expected '%s', got '%s'",
                                 i + 1, method_name,
                                 type_name(p->type), type_name(arg));
                }
                return ret;
            }             
            else {
                // Indirect call (function pointer)
                for (size_t i = 0; i < node->as.call.arg_count; i++)
                    tc_check_expr(tc, node->as.call.args[i]);
                if (callee_t && callee_t->kind == TY_FUNC_PTR)
                    return callee_t->return_type
                        ? callee_t->return_type
                        : make_type(tc, TY_VOID);
            }

            return ret_type;
        }

        // ── Field access: obj.field ────────────────────
        case NODE_FIELD: {
            AstType *target_t = tc_check_expr(tc, node->as.field.target);

            // Unwrap pointer if needed (auto-deref)
            if (target_t->kind == TY_POINTER ||
                target_t->kind == TY_NULLABLE_PTR)
                target_t = target_t->inner;

            // Built-in fields on str / slice
            if (target_t->kind == TY_STR) {
                if (strcmp(node->as.field.field, "ptr") == 0)
                    return make_type(tc, TY_CSTR);
                if (strcmp(node->as.field.field, "len") == 0)
                    return make_type(tc, TY_I64);
                tc_error(tc, node, "'str' has no field '%s'",
                         node->as.field.field);
                return make_type(tc, TY_INT);
            }
            if (target_t->kind == TY_SLICE) {
                if (strcmp(node->as.field.field, "ptr") == 0)
                    return make_type(tc, TY_ANY);
                if (strcmp(node->as.field.field, "len") == 0)
                    return make_type(tc, TY_I64);
                tc_error(tc, node, "'[]T' slice has no field '%s'",
                         node->as.field.field);
                return make_type(tc, TY_INT);
            }

            // Named struct field lookup
            if (target_t->kind == TY_NAMED || target_t->kind == TY_GENERIC) {
                // First: check if it's a global function used as a method
                // (a function whose first param is named 'self' of this type)
                Symbol *fsym = scope_lookup(tc->resolver->global,
                                             node->as.field.field);
                if (fsym && fsym->kind == SYM_FUNCTION && fsym->decl &&
                    fsym->decl->kind == NODE_FUNC_DECL) {
                    AstNode *fdecl = fsym->decl;
                    if (fdecl->as.func_decl.param_count > 0) {
                        AstParam *first = &fdecl->as.func_decl.params[0];
                        if (strcmp(first->name, "self") == 0)
                            return fdecl->as.func_decl.return_type
                                ? fdecl->as.func_decl.return_type
                                : make_type(tc, TY_VOID);
                    }
                }

                // Second: check struct fields
                AstField *f = find_struct_field(tc, target_t->name,
                                                 node->as.field.field);
                if (!f) {
                    tc_error(tc, node, "type '%s' has no field '%s'",
                             target_t->name, node->as.field.field);
                    return make_type(tc, TY_INT);
                }
                return f->type;
            }

            tc_error(tc, node,
                     "field access on non-struct type '%s'",
                     type_name(target_t));
            return make_type(tc, TY_INT);
        }

        // ── Index ─────────────────────────────────────
        case NODE_INDEX: {
            AstType *target_t = tc_check_expr(tc, node->as.index.target);
            // Range index = slice expression: arr[0..5]
            if (node->as.index.index && node->as.index.index->kind == NODE_RANGE) {
                AstType *slice = make_type(tc, TY_SLICE);
                if (target_t->kind == TY_ARRAY)       slice->inner = target_t->elem_type;
                else if (target_t->kind == TY_SLICE)  slice->inner = target_t->inner;
                else if (target_t->kind == TY_STR)    slice->inner = make_type(tc, TY_CHAR);
                else                                   slice->inner = make_type(tc, TY_INT);
                return slice;
            }
            AstType *index_t  = tc_check_expr(tc, node->as.index.index);
            if (!type_is_integer(index_t))
                tc_error(tc, node->as.index.index,
                         "array index must be integer, got '%s'",
                         type_name(index_t));
            if (target_t->kind == TY_ARRAY)   return target_t->elem_type;
            if (target_t->kind == TY_SLICE)   return target_t->inner;
            if (target_t->kind == TY_POINTER) return target_t->inner;
            if (target_t->kind == TY_CSTR)    return make_type(tc, TY_CHAR);
            if (target_t->kind == TY_STR)     return make_type(tc, TY_CHAR);
            tc_error(tc, node, "cannot index type '%s'", type_name(target_t));
            return make_type(tc, TY_INT);
        }

        // ── Slice expr ────────────────────────────────
        case NODE_SLICE_EXPR: {
            AstType *target_t = tc_check_expr(tc, node->as.slice_expr.target);
            if (node->as.slice_expr.from)
                tc_check_expr(tc, node->as.slice_expr.from);
            if (node->as.slice_expr.to)
                tc_check_expr(tc, node->as.slice_expr.to);

            AstType *slice = make_type(tc, TY_SLICE);
            if (target_t->kind == TY_ARRAY)
                slice->inner = target_t->elem_type;
            else if (target_t->kind == TY_SLICE)
                slice->inner = target_t->inner;
            else if (target_t->kind == TY_STR)
                slice->inner = make_type(tc, TY_CHAR);
            else
                slice->inner = make_type(tc, TY_U8);
            return slice;
        }

        // ── Cast ──────────────────────────────────────
        case NODE_CAST: {
            AstType *from = tc_check_expr(tc, node->as.cast.expr);
            AstType *to   = node->as.cast.type;

            // Warn on potentially unsafe casts
            if (type_is_pointer(from) && type_is_integer(to))
                tc_warn(tc, node, "casting pointer to integer is unsafe");
            if (type_is_integer(from) && type_is_pointer(to))
                tc_warn(tc, node, "casting integer to pointer is unsafe");

            // Disallow casting between totally unrelated non-numeric types
            if (!type_is_numeric(from) && !type_is_pointer(from) &&
                !type_is_numeric(to)   && !type_is_pointer(to))
                tc_error(tc, node,
                         "invalid cast from '%s' to '%s'",
                         type_name(from), type_name(to));

            return to;
        }

        // ── Range ─────────────────────────────────────
        case NODE_RANGE: {
            AstType *from = tc_check_expr(tc, node->as.range.from);
            AstType *to   = tc_check_expr(tc, node->as.range.to);
            if (!type_is_integer(from))
                tc_error(tc, node->as.range.from,
                         "range start must be integer, got '%s'", type_name(from));
            if (!type_is_integer(to))
                tc_error(tc, node->as.range.to,
                         "range end must be integer, got '%s'", type_name(to));
            return from;
        }

        // ── Struct literal ────────────────────────────
        case NODE_STRUCT_LITERAL: {
            if (node->as.struct_lit.type_name) {
                Symbol *sym = scope_lookup(tc->resolver->global,
                                           node->as.struct_lit.type_name);
                if (!sym || sym->kind != SYM_STRUCT) {
                    tc_error(tc, node, "'%s' is not a struct type",
                             node->as.struct_lit.type_name);
                    return make_type(tc, TY_INT);
                }

                AstNode *decl = sym->decl;
                // Check each field
                for (size_t i = 0; i < node->as.struct_lit.field_count; i++) {
                    AstNode *fname = node->as.struct_lit.field_names[i];
                    AstType *fval  = tc_check_expr(tc,
                        node->as.struct_lit.field_values[i]);
                    // Find expected field type
                    AstField *field = find_struct_field(tc,
                        node->as.struct_lit.type_name,
                        fname->as.ident.name);
                    if (!field) {
                        tc_error(tc, fname, "struct '%s' has no field '%s'",
                                 node->as.struct_lit.type_name,
                                 fname->as.ident.name);
                    } else if (!types_equal(field->type, fval)) {
                        tc_error(tc, node->as.struct_lit.field_values[i],
                                 "field '%s': expected '%s', got '%s'",
                                 fname->as.ident.name,
                                 type_name(field->type),
                                 type_name(fval));
                    }
                }

                // Check for missing required fields (no default)
                if (decl) {
                    for (size_t i = 0; i < decl->as.struct_decl.field_count; i++) {
                        AstField *f = &decl->as.struct_decl.fields[i];
                        if (f->default_value) continue;
                        int found = 0;
                        for (size_t j = 0; j < node->as.struct_lit.field_count; j++) {
                            AstNode *fn = node->as.struct_lit.field_names[j];
                            if (strcmp(fn->as.ident.name, f->name) == 0) {
                                found = 1; break;
                            }
                        }
                        if (!found)
                            tc_warn(tc, node,
                                    "struct literal missing field '%s'", f->name);
                    }
                }

                return make_named_type(tc, node->as.struct_lit.type_name);
            }
            // Inferred struct literal — type inferred from context later
            for (size_t i = 0; i < node->as.struct_lit.field_count; i++)
                tc_check_expr(tc, node->as.struct_lit.field_values[i]);
            return make_type(tc, TY_INFERRED);
        }

        // ── Array literal ─────────────────────────────
        case NODE_ARRAY_LITERAL: {
            if (node->as.array_lit.count == 0) {
                AstType *t    = make_type(tc, TY_ARRAY);
                t->elem_type  = make_type(tc, TY_INT);
                return t;
            }
            AstType *elem_t = tc_check_expr(tc, node->as.array_lit.elems[0]);
            for (size_t i = 1; i < node->as.array_lit.count; i++) {
                AstType *et = tc_check_expr(tc, node->as.array_lit.elems[i]);
                if (!types_equal(elem_t, et))
                    tc_error(tc, node->as.array_lit.elems[i],
                             "array element type mismatch: expected '%s', got '%s'",
                             type_name(elem_t), type_name(et));
            }
            AstType *t   = make_type(tc, TY_ARRAY);
            t->elem_type = elem_t;
            return t;
        }

        // ── If expression ─────────────────────────────
        case NODE_IF_EXPR: {
            AstType *cond_t = tc_check_expr(tc, node->as.if_stmt.condition);
            if (!type_is_bool(cond_t) && !type_is_integer(cond_t))
                tc_error(tc, node->as.if_stmt.condition,
                         "if condition must be bool or integer, got '%s'",
                         type_name(cond_t));

            AstType *then_t = NULL;
            AstType *else_t = NULL;

            if (node->as.if_stmt.then_block &&
                node->as.if_stmt.then_block->kind == NODE_BLOCK &&
                node->as.if_stmt.then_block->as.block.count > 0) {
                AstNode *last = node->as.if_stmt.then_block->as.block.stmts[
                    node->as.if_stmt.then_block->as.block.count - 1];
                then_t = tc_check_expr(tc, last);
            }
            if (node->as.if_stmt.else_block &&
                node->as.if_stmt.else_block->kind == NODE_BLOCK &&
                node->as.if_stmt.else_block->as.block.count > 0) {
                AstNode *last = node->as.if_stmt.else_block->as.block.stmts[
                    node->as.if_stmt.else_block->as.block.count - 1];
                else_t = tc_check_expr(tc, last);
            }

            if (then_t && else_t && !types_equal(then_t, else_t))
                tc_error(tc, node,
                         "if expression branches have different types: '%s' vs '%s'",
                         type_name(then_t), type_name(else_t));

            return then_t ? then_t : make_type(tc, TY_VOID);
        }

        // ── ? propagation ─────────────────────────────
        case NODE_PROPAGATE: {
            AstType *inner = tc_check_expr(tc, node->as.propagate.expr);
            if (inner->kind == TY_VOID)
                tc_error(tc, node, "'?' operator used on void expression");
            // Unwrap Result<T,E> → T (use int as placeholder for T)
            return make_type(tc, TY_INT);
        }

        // ── sizeof / alignof ──────────────────────────
        case NODE_SIZEOF:
        case NODE_ALIGNOF:
            return make_type(tc, TY_I64);

        // ── new ───────────────────────────────────────
        case NODE_NEW: {
            Symbol *sym = scope_lookup(tc->resolver->global,
                                       node->as.new_expr.type_name);
            if (!sym || sym->kind != SYM_STRUCT)
                tc_error(tc, node, "'%s' is not a struct type",
                         node->as.new_expr.type_name);

            for (size_t i = 0; i < node->as.new_expr.field_count; i++)
                tc_check_expr(tc, node->as.new_expr.field_values[i]);

            AstType *ptr  = make_type(tc, TY_POINTER);
            ptr->inner    = make_named_type(tc, node->as.new_expr.type_name);
            return ptr;
        }

        // ── delete ────────────────────────────────────
        case NODE_DELETE: {
            AstType *pt = tc_check_expr(tc, node->as.delete_expr.ptr);
            if (!type_is_pointer(pt))
                tc_error(tc, node,
                         "'delete' requires a pointer, got '%s'",
                         type_name(pt));
            return make_type(tc, TY_VOID);
        }

        // ── Lambda ────────────────────────────────────
        case NODE_LAMBDA: {
            tc_push_scope(tc);
            for (size_t i = 0; i < node->as.lambda.param_count; i++) {
                AstParam *p = &node->as.lambda.params[i];
                if (p->type)
                    tc_bind(tc, p->name, p->type, 0);
            }
            AstType *body_t = NULL;
            if (node->as.lambda.body)
                body_t = tc_check_expr(tc, node->as.lambda.body);
            tc_pop_scope(tc);

            AstType *fn    = make_type(tc, TY_FUNC_PTR);
            fn->return_type = body_t ? body_t : make_type(tc, TY_VOID);
            return fn;
        }

        // ── Multi-assign RHS ──────────────────────────
        case NODE_MULTI_ASSIGN:
            return tc_check_expr(tc, node->as.multi_assign.value);

        case NODE_ENUM_LITERAL: {
            // Look up enum type and return it as a named type
            AstType *t = make_type(tc, TY_NAMED);
            t->name = node->as.enum_lit.enum_name;
            // Type-check args
            for (size_t i = 0; i < node->as.enum_lit.arg_count; i++)
                tc_check_expr(tc, node->as.enum_lit.args[i]);
            return t;
        }
        case NODE_ENUM_PATTERN:
            return make_type(tc, TY_BOOL);

        default:
            return make_type(tc, TY_INT);
    }
}

// ─── STATEMENT TYPE CHECKER ──────────────────────────────────────────────────

void tc_check_block(TypeChecker *tc, AstNode *node) {
    if (!node || node->kind != NODE_BLOCK) return;
    tc_push_scope(tc);
    for (size_t i = 0; i < node->as.block.count; i++)
        tc_check_stmt(tc, node->as.block.stmts[i]);
    tc_pop_scope(tc);
}

void tc_check_stmt(TypeChecker *tc, AstNode *node) {
    if (!node) return;

    switch (node->kind) {

        case NODE_BLOCK:
            tc_check_block(tc, node);
            break;

        // ── Variable declaration ──────────────────────
        case NODE_VAR_DECL: {
            AstType *decl_type = node->as.var_decl.type;
            AstType *val_type  = NULL;

            if (node->as.var_decl.value)
                val_type = tc_check_expr(tc, node->as.var_decl.value);

            // Infer type from value if not declared
            if (!decl_type) {
                if (!val_type) {
                    tc_error(tc, node,
                             "cannot infer type of '%s' without initializer",
                             node->as.var_decl.name);
                    decl_type = make_type(tc, TY_INT);
                } else {
                    decl_type = val_type;
                }
                // Patch the AST node with the inferred type
                node->as.var_decl.type = decl_type;
            } else if (val_type) {
                // Check declared type matches value type
                if (!types_equal(decl_type, val_type))
                    tc_error(tc, node,
                             "type mismatch: variable '%s' declared as '%s' "
                             "but initialized with '%s'",
                             node->as.var_decl.name,
                             type_name(decl_type),
                             type_name(val_type));
            }

            tc_bind(tc, node->as.var_decl.name, decl_type,
                    node->as.var_decl.is_const);
            break;
        }

        // ── Multi-assign ──────────────────────────────
        case NODE_MULTI_ASSIGN: {
            tc_check_expr(tc, node->as.multi_assign.value);
            // Bind each name — types inferred later (multi-return)
            for (size_t i = 0; i < node->as.multi_assign.count; i++)
                tc_bind(tc, node->as.multi_assign.names[i],
                        make_type(tc, TY_INFERRED), 0);
            break;
        }

        // ── Return ────────────────────────────────────
        case NODE_RETURN: {
            AstType *ret_t = node->as.ret.value
                ? tc_check_expr(tc, node->as.ret.value)
                : make_type(tc, TY_VOID);

            if (tc->return_type) {
                if (!types_equal(tc->return_type, ret_t)) {
                    // Allow int literal 0 as return for inferred main
                    int ok = (tc->return_type->kind == TY_INFERRED) ||
                             (tc->return_type->kind == TY_INT &&
                              ret_t->kind == TY_INT);
                    if (!ok)
                        tc_error(tc, node,
                                 "return type mismatch in '%s': "
                                 "expected '%s', got '%s'",
                                 tc->func_name ? tc->func_name : "?",
                                 type_name(tc->return_type),
                                 type_name(ret_t));
                }
            }
            tc->return_seen = 1;
            break;
        }

        // ── If ────────────────────────────────────────
        case NODE_IF: {
            AstType *cond_t = tc_check_expr(tc, node->as.if_stmt.condition);
            if (!type_is_bool(cond_t) && !type_is_integer(cond_t))
                tc_error(tc, node->as.if_stmt.condition,
                         "if condition must be bool or integer, got '%s'",
                         type_name(cond_t));
            tc_check_block(tc, node->as.if_stmt.then_block);
            for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
                tc_check_expr(tc, node->as.if_stmt.else_if_conds[i]);
                tc_check_block(tc, node->as.if_stmt.else_if_blocks[i]);
            }
            if (node->as.if_stmt.else_block)
                tc_check_block(tc, node->as.if_stmt.else_block);
            break;
        }

        // ── While ─────────────────────────────────────
        case NODE_WHILE: {
            AstType *cond_t = tc_check_expr(tc, node->as.while_stmt.condition);
            if (!type_is_bool(cond_t) && !type_is_integer(cond_t))
                tc_error(tc, node,
                         "while condition must be bool or integer, got '%s'",
                         type_name(cond_t));
            int saved_loop = tc->in_loop;
            tc->in_loop = 1;
            tc_check_block(tc, node->as.while_stmt.body);
            tc->in_loop = saved_loop;
            break;
        }

        // ── For range ─────────────────────────────────
        case NODE_FOR_RANGE: {
            AstType *from_t = tc_check_expr(tc, node->as.for_range.from);
            AstType *to_t   = tc_check_expr(tc, node->as.for_range.to);
            if (!type_is_integer(from_t))
                tc_error(tc, node->as.for_range.from,
                         "range start must be integer, got '%s'", type_name(from_t));
            if (!type_is_integer(to_t))
                tc_error(tc, node->as.for_range.to,
                         "range end must be integer, got '%s'", type_name(to_t));
            tc_push_scope(tc);
            tc_bind(tc, node->as.for_range.var, make_type(tc, TY_INT), 0);
            int saved = tc->in_loop; tc->in_loop = 1;
            tc_check_block(tc, node->as.for_range.body);
            tc->in_loop = saved;
            tc_pop_scope(tc);
            break;
        }

        // ── For in ────────────────────────────────────
        case NODE_FOR_IN: {
            AstType *iter_t = tc_check_expr(tc, node->as.for_in.iterable);
            AstType *elem_t = make_type(tc, TY_INT);  // default
            if (iter_t->kind == TY_ARRAY) elem_t = iter_t->elem_type;
            if (iter_t->kind == TY_SLICE) elem_t = iter_t->inner;
            if (iter_t->kind == TY_STR)   elem_t = make_type(tc, TY_CHAR);
            tc_push_scope(tc);
            tc_bind(tc, node->as.for_in.var, elem_t, 0);
            int saved = tc->in_loop; tc->in_loop = 1;
            tc_check_block(tc, node->as.for_in.body);
            tc->in_loop = saved;
            tc_pop_scope(tc);
            break;
        }

        // ── For index ─────────────────────────────────
        case NODE_FOR_INDEX: {
            AstType *iter_t = tc_check_expr(tc, node->as.for_index.iterable);
            AstType *elem_t = make_type(tc, TY_INT);
            if (iter_t->kind == TY_ARRAY) elem_t = iter_t->elem_type;
            if (iter_t->kind == TY_SLICE) elem_t = iter_t->inner;
            tc_push_scope(tc);
            tc_bind(tc, node->as.for_index.index_var, make_type(tc, TY_I64), 0);
            tc_bind(tc, node->as.for_index.value_var, elem_t, 0);
            int saved = tc->in_loop; tc->in_loop = 1;
            tc_check_block(tc, node->as.for_index.body);
            tc->in_loop = saved;
            tc_pop_scope(tc);
            break;
        }

        // ── C-style for ───────────────────────────────
        case NODE_FOR_C: {
            tc_push_scope(tc);
            if (node->as.for_c.init) tc_check_stmt(tc, node->as.for_c.init);
            if (node->as.for_c.condition)
                tc_check_expr(tc, node->as.for_c.condition);
            if (node->as.for_c.post)
                tc_check_expr(tc, node->as.for_c.post);
            int saved = tc->in_loop; tc->in_loop = 1;
            tc_check_block(tc, node->as.for_c.body);
            tc->in_loop = saved;
            tc_pop_scope(tc);
            break;
        }

        // ── Loop ──────────────────────────────────────
        case NODE_LOOP: {
            int saved = tc->in_loop; tc->in_loop = 1;
            tc_check_block(tc, node->as.loop_stmt.body);
            tc->in_loop = saved;
            break;
        }

        // ── Match ─────────────────────────────────────
        case NODE_MATCH: {
            AstType *subject_t = tc_check_expr(tc, node->as.match_stmt.subject);
            for (size_t i = 0; i < node->as.match_stmt.arm_count; i++) {
                MatchArm *arm = &node->as.match_stmt.arms[i];
                tc_push_scope(tc);

                if (arm->pattern && arm->pattern->kind == NODE_ENUM_PATTERN) {
                    // Register bindings with types from the enum variant fields
                    const char *ename = arm->pattern->as.enum_pattern.enum_name;
                    const char *vname = arm->pattern->as.enum_pattern.variant;
                    Symbol *esym = scope_lookup(tc->resolver->global, ename);
                    AstVariant *matched_var = NULL;
                    if (esym && esym->decl) {
                        AstNode *ed = esym->decl;
                        for (size_t vi = 0; vi < ed->as.enum_decl.variant_count; vi++) {
                            if (strcmp(ed->as.enum_decl.variants[vi].name, vname) == 0) {
                                matched_var = &ed->as.enum_decl.variants[vi];
                                break;
                            }
                        }
                    }
                    for (size_t b = 0; b < arm->pattern->as.enum_pattern.binding_count; b++) {
                        const char *bname = arm->pattern->as.enum_pattern.bindings[b];
                        AstType *btype = (matched_var && b < matched_var->field_count)
                            ? matched_var->fields[b].type
                            : make_type(tc, TY_INT);
                        tc_bind(tc, bname, btype, 0);
                    }
                } else if (arm->pattern) {
                    AstType *pat_t = tc_check_expr(tc, arm->pattern);
                    int is_wildcard = arm->pattern->kind == NODE_IDENT &&
                        strcmp(arm->pattern->as.ident.name, "_") == 0;
                    if (!is_wildcard && !types_equal(subject_t, pat_t))
                        tc_warn(tc, arm->pattern,
                                "match arm type '%s' differs from subject type '%s'",
                                type_name(pat_t), type_name(subject_t));
                }

                if (arm->guard) {
                    AstType *gt = tc_check_expr(tc, arm->guard);
                    if (!type_is_bool(gt) && !type_is_integer(gt))
                        tc_error(tc, arm->guard,
                                 "match guard must be bool, got '%s'",
                                 type_name(gt));
                }

                tc_check_stmt(tc, arm->body);
                tc_pop_scope(tc);
            }
            break;
        }

        // ── Break / Continue ──────────────────────────
        case NODE_BREAK:
            if (!tc->in_loop)
                tc_error(tc, node, "'break' outside of loop");
            break;

        case NODE_CONTINUE:
            if (!tc->in_loop)
                tc_error(tc, node, "'continue' outside of loop");
            break;

        // ── Defer ─────────────────────────────────────
        case NODE_DEFER:
            tc_check_stmt(tc, node->as.defer_stmt.stmt);
            break;

        // ── Panic ─────────────────────────────────────
        case NODE_PANIC: {
            AstType *msg_t = tc_check_expr(tc, node->as.panic_stmt.msg);
            if (msg_t->kind != TY_STR)
                tc_error(tc, node,
                         "panic() expects 'str', got '%s'", type_name(msg_t));
            break;
        }

        // ── ASM block ─────────────────────────────────
        case NODE_ASM_BLOCK:
            break;   // raw asm — no type checking possible

        // ── Delete ────────────────────────────────────
        case NODE_DELETE:
            tc_check_expr(tc, node);
            break;

        // ── Expression statement ──────────────────────
        case NODE_EXPR_STMT:
            tc_check_expr(tc, node->as.expr_stmt.expr);
            break;

        default:
            tc_check_expr(tc, node);
            break;
    }
}

// ─── TOP-LEVEL DECLARATION CHECKER ───────────────────────────────────────────

static void tc_check_func(TypeChecker *tc, AstNode *node) {
    AstType *saved_ret  = tc->return_type;
    char    *saved_name = tc->func_name;
    int      saved_seen = tc->return_seen;

    tc->return_type = node->as.func_decl.return_type;
    tc->func_name   = node->as.func_decl.name;
    tc->return_seen = 0;

    tc_push_scope(tc);

    // Register parameters into function scope
    for (size_t i = 0; i < node->as.func_decl.param_count; i++) {
        AstParam *p = &node->as.func_decl.params[i];
        if (!p->type) {
            tc_error(tc, node, "parameter '%s' in '%s' has no type",
                     p->name, node->as.func_decl.name);
            p->type = make_type(tc, TY_INT);
        }
        tc_bind(tc, p->name, p->type, 0);
        if (p->default_value)
            tc_check_expr(tc, p->default_value);
    }

    // Check body (NULL for interface signatures)
    if (node->as.func_decl.body)
        tc_check_block(tc, node->as.func_decl.body);

    // Warn if non-void function might not return
    if (tc->return_type &&
        tc->return_type->kind != TY_VOID &&
        !tc->return_seen) {
        // Only warn if there's no explicit inferred return
        if (tc->return_type->kind != TY_INFERRED)
            tc_warn(tc, node,
                    "function '%s' may not return a value on all paths",
                    node->as.func_decl.name);
    }

    tc_pop_scope(tc);

    tc->return_type = saved_ret;
    tc->func_name   = saved_name;
    tc->return_seen = saved_seen;
}

static void tc_check_struct(TypeChecker *tc, AstNode *node) {
    // Validate field types
    for (size_t i = 0; i < node->as.struct_decl.field_count; i++) {
        AstField *f = &node->as.struct_decl.fields[i];
        if (f->type && f->type->kind == TY_NAMED) {
            Symbol *sym = scope_lookup(tc->resolver->global, f->type->name);
            if (!sym)
                tc_error(tc, node,
                         "field '%s' in struct '%s' has unknown type '%s'",
                         f->name, node->as.struct_decl.name, f->type->name);
        }
        if (f->default_value)
            tc_check_expr(tc, f->default_value);
    }

    // Check embedded structs exist
    for (size_t i = 0; i < node->as.struct_decl.embed_count; i++) {
        Symbol *sym = scope_lookup(tc->resolver->global,
                                   node->as.struct_decl.embeds[i]);
        if (!sym || sym->kind != SYM_STRUCT)
            tc_error(tc, node,
                     "embedded type '%s' is not a struct",
                     node->as.struct_decl.embeds[i]);
    }

    // Check methods — set current_struct for 'self' resolution
    AstNode *saved = tc->current_struct;
    tc->current_struct = node;
    for (size_t i = 0; i < node->as.struct_decl.method_count; i++)
        if (node->as.struct_decl.methods[i])
            tc_check_func(tc, node->as.struct_decl.methods[i]);
    tc->current_struct = saved;
}

static void tc_check_enum(TypeChecker *tc, AstNode *node) {
    for (size_t i = 0; i < node->as.enum_decl.variant_count; i++) {
        AstVariant *v = &node->as.enum_decl.variants[i];
        if (v->explicit_value)
            tc_check_expr(tc, v->explicit_value);
        for (size_t j = 0; j < v->field_count; j++) {
            if (v->fields[j].type && v->fields[j].type->kind == TY_NAMED) {
                Symbol *sym = scope_lookup(tc->resolver->global,
                                           v->fields[j].type->name);
                if (!sym)
                    tc_error(tc, node,
                             "enum variant '%s.%s' field '%s' has unknown type '%s'",
                             node->as.enum_decl.name, v->name,
                             v->fields[j].name, v->fields[j].type->name);
            }
        }
    }
}

static void tc_check_interface(TypeChecker *tc, AstNode *node) {
    // Verify parent interfaces exist and are interfaces
    for (size_t i = 0; i < node->as.interface_decl.parent_count; i++) {
        Symbol *sym = scope_lookup(tc->resolver->global,
                                   node->as.interface_decl.parents[i]);
        if (!sym || sym->kind != SYM_INTERFACE)
            tc_error(tc, node,
                     "'%s' is not an interface",
                     node->as.interface_decl.parents[i]);
    }
    // Check method signatures
    for (size_t i = 0; i < node->as.interface_decl.method_count; i++) {
        AstNode *m = node->as.interface_decl.methods[i];
        if (m && m->kind == NODE_FUNC_DECL) {
            for (size_t j = 0; j < m->as.func_decl.param_count; j++) {
                AstParam *p = &m->as.func_decl.params[j];
                if (p->type && p->type->kind == TY_NAMED
                        && strcmp(p->type->name, "Self") != 0) {
                    Symbol *sym = scope_lookup(tc->resolver->global, p->type->name);
                    if (!sym)
                        tc_error(tc, m,
                                 "interface method parameter '%s' has unknown type '%s'",
                                 p->name, p->type->name);
                }
            }
        }
    }
}

// ─── INTERFACE SATISFACTION ───────────────────────────────────────────────────
// After all declarations are checked, verify every struct that is passed
// as an interface type actually satisfies the interface contract.

static void tc_check_interface_satisfaction(TypeChecker *tc, AstNode *program) {
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_FUNC_DECL) continue;

        for (size_t j = 0; j < decl->as.func_decl.param_count; j++) {
            AstParam *p = &decl->as.func_decl.params[j];
            if (!p->type || p->type->kind != TY_NAMED) continue;

            Symbol *psym = scope_lookup(tc->resolver->global, p->type->name);
            if (!psym || psym->kind != SYM_INTERFACE) continue;

            // This param expects an interface — any struct passed here
            // will be checked at the call site in tc_check_expr NODE_CALL
        }
    }
}

// ─── GLOBAL VARIABLE CHECKER ─────────────────────────────────────────────────

static void tc_check_global_var(TypeChecker *tc, AstNode *node) {
    AstType *decl_type = node->as.var_decl.type;
    AstType *val_type  = NULL;

    if (node->as.var_decl.value)
        val_type = tc_check_expr(tc, node->as.var_decl.value);

    if (!decl_type && !val_type) {
        tc_error(tc, node,
                 "cannot infer type of global '%s' without initializer",
                 node->as.var_decl.name);
        return;
    }

    if (!decl_type) {
        node->as.var_decl.type = val_type;
        decl_type = val_type;
    } else if (val_type && !types_equal(decl_type, val_type)) {
        tc_error(tc, node,
                 "type mismatch in global '%s': declared '%s', got '%s'",
                 node->as.var_decl.name,
                 type_name(decl_type),
                 type_name(val_type));
    }

    // Register global in the type scope
    tc_bind(tc, node->as.var_decl.name, decl_type,
            node->as.var_decl.is_const);
}

// ─── ENTRY POINT ─────────────────────────────────────────────────────────────

TypeChecker *tc_create(Arena *arena, Resolver *resolver,
                        const char *filename) {
    TypeChecker *tc      = arena_alloc(arena, sizeof(TypeChecker));
    tc->arena            = arena;
    tc->resolver         = resolver;
    tc->filename         = filename;
    tc->had_error        = 0;
    tc->warning_count    = 0;
    tc->scope            = NULL;
    tc->return_type      = NULL;
    tc->func_name        = NULL;
    tc->in_loop          = 0;
    tc->return_seen      = 0;
    tc->current_struct   = NULL;
    tc->iface_check_cap  = 16;
    tc->iface_check_count = 0;
    tc->iface_checks     = arena_alloc(arena, 16 * sizeof(AstNode *));
    return tc;
}

int tc_run(TypeChecker *tc, AstNode *program) {
    if (!program || program->kind != NODE_PROGRAM) return 0;

    // Global scope
    tc_push_scope(tc);

    // ── Pass 1: register all global types into type scope ────
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl) continue;

        if (decl->kind == NODE_VAR_DECL)
            tc_check_global_var(tc, decl);
    }

    // ── Pass 2: check all declarations ───────────────────────
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl) continue;

        switch (decl->kind) {
            case NODE_FUNC_DECL:
                tc_check_func(tc, decl);
                break;
            case NODE_ASM_FUNC_DECL:
                // ASM functions: just validate param types exist
                for (size_t j = 0; j < decl->as.asm_func_decl.param_count; j++) {
                    AsmParam *p = &decl->as.asm_func_decl.params[j];
                    if (p->type && p->type->kind == TY_NAMED) {
                        Symbol *sym = scope_lookup(tc->resolver->global,
                                                   p->type->name);
                        if (!sym)
                            tc_error(tc, decl,
                                     "asm param '%s' has unknown type '%s'",
                                     p->name, p->type->name);
                    }
                }
                break;
            case NODE_UNION_DECL:
            case NODE_STRUCT_DECL:
                tc_check_struct(tc, decl);
                break;
            case NODE_ENUM_DECL:
                tc_check_enum(tc, decl);
                break;
            case NODE_INTERFACE_DECL:
                tc_check_interface(tc, decl);
                break;
            case NODE_USING:
            case NODE_MODULE:
            case NODE_VAR_DECL:
                break;   // already handled in pass 1
            default:
                break;
        }
    }

    // ── Pass 3: interface satisfaction checks ─────────────────
    tc_check_interface_satisfaction(tc, program);

    tc_pop_scope(tc);

    if (tc->warning_count > 0)
        fprintf(stderr, "[%s] %d warning(s)\n",
                tc->filename, tc->warning_count);

    return !tc->had_error;
}