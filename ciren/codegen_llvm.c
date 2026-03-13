// ciren/codegen_llvm.c

#include "codegen_llvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

// ─── HELPERS ─────────────────────────────────────────────────────────────────

static void llvm_error(LLVMCodegen *g, AstNode *node, const char *fmt, ...) {
    g->had_error = 1;
    int line = node ? node->line : 0;
    fprintf(stderr, "[%s:%d] llvm codegen error: ", g->filename, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static char *arena_str(Arena *a, const char *s) {
    size_t len = strlen(s);
    char *out  = arena_alloc(a, len + 1);
    memcpy(out, s, len + 1);
    return out;
}

static int new_label(LLVMCodegen *g) {
    return g->label_counter++;
}

// ─── SCOPE ───────────────────────────────────────────────────────────────────

static LLVMScope *scope_push(LLVMCodegen *g) {
    LLVMScope *s = arena_alloc(g->arena, sizeof(LLVMScope));
    memset(s, 0, sizeof(LLVMScope));
    s->parent  = g->scope;
    g->scope   = s;
    return s;
}

static void scope_pop(LLVMCodegen *g) {
    if (g->scope->parent)
        g->scope = g->scope->parent;
}

static uint32_t hash_name(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void scope_set(LLVMCodegen *g, const char *name,
                      LLVMValueRef val, LLVMTypeRef type, int is_alloca) {
    uint32_t idx = hash_name(name) % LLVM_TABLE_BUCKETS;
    LLVMEntry *e = arena_alloc(g->arena, sizeof(LLVMEntry));
    e->name      = arena_str(g->arena, name);
    e->value     = val;
    e->type      = type;
    e->is_alloca = is_alloca;
    e->next      = g->scope->buckets[idx];
    g->scope->buckets[idx] = e;
}

static LLVMEntry *scope_get(LLVMCodegen *g, const char *name) {
    for (LLVMScope *s = g->scope; s; s = s->parent) {
        uint32_t idx = hash_name(name) % LLVM_TABLE_BUCKETS;
        for (LLVMEntry *e = s->buckets[idx]; e; e = e->next)
            if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

// ─── LLVM TYPE MAPPING ───────────────────────────────────────────────────────

static LLVMTypeRef ci_type_to_llvm(LLVMCodegen *g, AstType *type) {
    if (!type) return LLVMVoidTypeInContext(g->ctx);

    switch (type->kind) {
        case TY_VOID:         return LLVMVoidTypeInContext(g->ctx);
        case TY_BOOL:
        case TY_I8:
        case TY_U8:           return LLVMInt8TypeInContext(g->ctx);
        case TY_I16:
        case TY_U16:          return LLVMInt16TypeInContext(g->ctx);
        case TY_INT:
        case TY_UINT:         return LLVMInt32TypeInContext(g->ctx);
        case TY_I64:
        case TY_U64:          return LLVMInt64TypeInContext(g->ctx);
        case TY_F32:          return LLVMFloatTypeInContext(g->ctx);
        case TY_F64:          return LLVMDoubleTypeInContext(g->ctx);
        case TY_CHAR:         return LLVMInt8TypeInContext(g->ctx);
        case TY_CSTR:         return LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0);
        case TY_ANY:          return LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0);
        case TY_INFERRED:     return LLVMInt32TypeInContext(g->ctx); // fallback

        case TY_STR: {
            // Reuse named type _ci_str = { ptr, i64 }
            LLVMTypeRef existing = LLVMGetTypeByName2(g->ctx, "_ci_str");
            if (existing) return existing;
            LLVMTypeRef t = LLVMStructCreateNamed(g->ctx, "_ci_str");
            LLVMTypeRef fields[2] = {
                LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0),
                LLVMInt64TypeInContext(g->ctx)
            };
            LLVMStructSetBody(t, fields, 2, 0);
            return t;
        }

        case TY_POINTER:
        case TY_NULLABLE_PTR: {
            LLVMTypeRef inner = ci_type_to_llvm(g, type->inner);
            return LLVMPointerType(inner, 0);
        }

        case TY_SLICE: {
            // Reuse named type _ci_slice = { ptr, i64 }
            LLVMTypeRef existing = LLVMGetTypeByName2(g->ctx, "_ci_slice");
            if (existing) return existing;
            LLVMTypeRef t = LLVMStructCreateNamed(g->ctx, "_ci_slice");
            LLVMTypeRef fields[2] = {
                LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0),
                LLVMInt64TypeInContext(g->ctx)
            };
            LLVMStructSetBody(t, fields, 2, 0);
            return t;
        }

        case TY_ARRAY: {
            LLVMTypeRef elem = ci_type_to_llvm(g, type->elem_type);
            unsigned size = 0;
            if (type->array_size && type->array_size->kind == NODE_INT_LIT)
                size = (unsigned)type->array_size->as.int_lit.value;
            return LLVMArrayType(elem, size);
        }

        case TY_NAMED:
        case TY_GENERIC: {
            if (type->name && strcmp(type->name, "Result") == 0) {
                // If user defined their own Result enum, use that
                LLVMTypeRef user_result = LLVMGetTypeByName2(g->ctx, "Result");
                if (user_result) return user_result;
                // Otherwise use built-in _ci_result
                LLVMTypeRef existing = LLVMGetTypeByName2(g->ctx, "_ci_result");
                if (existing) return existing;
                LLVMTypeRef t = LLVMStructCreateNamed(g->ctx, "_ci_result");
                LLVMTypeRef fields[2] = {
                    LLVMInt32TypeInContext(g->ctx),
                    LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0)
                };
                LLVMStructSetBody(t, fields, 2, 0);
                return t;
            }
            // Look up the struct type in the LLVM module
            LLVMTypeRef found = LLVMGetTypeByName2(g->ctx, type->name);
            if (found) return found;
            // Unknown named type — emit opaque pointer
            return LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0);
        }

        case TY_FUNC_PTR: {
            LLVMTypeRef ret = ci_type_to_llvm(g, type->return_type);
            LLVMTypeRef *params = malloc(type->param_count * sizeof(LLVMTypeRef));
            for (size_t i = 0; i < type->param_count; i++)
                params[i] = ci_type_to_llvm(g, type->param_types[i]);
            LLVMTypeRef fn = LLVMFunctionType(ret, params, (unsigned)type->param_count, 0);
            free(params);
            return LLVMPointerType(fn, 0);
        }

        default:
            return LLVMInt32TypeInContext(g->ctx);
    }
}

// ─── STRING LITERALS ─────────────────────────────────────────────────────────
// Deduplicated global string constants

static LLVMValueRef get_string_global(LLVMCodegen *g, const char *str) {
    // Check dedup table
    for (size_t i = 0; i < g->str_count; i++)
        if (strcmp(g->str_values[i], str) == 0)
            return g->str_globals[i];

    // New global string constant
    LLVMValueRef glob = LLVMAddGlobal(
        g->module,
        LLVMArrayType(LLVMInt8TypeInContext(g->ctx), (unsigned)strlen(str) + 1),
        ".str"
    );
    LLVMSetGlobalConstant(glob, 1);
    LLVMSetLinkage(glob, LLVMPrivateLinkage);
    LLVMSetInitializer(glob, LLVMConstStringInContext(
        g->ctx, str, (unsigned)strlen(str), 0));
    LLVMSetUnnamedAddress(glob, LLVMGlobalUnnamedAddr);

    // Store in dedup table
    if (g->str_count >= g->str_cap) {
        g->str_cap *= 2;
        g->str_globals = realloc(g->str_globals,
                                  g->str_cap * sizeof(LLVMValueRef));
        g->str_values  = realloc(g->str_values,
                                  g->str_cap * sizeof(char *));
    }
    g->str_globals[g->str_count] = glob;
    g->str_values[g->str_count]  = strdup(str);
    g->str_count++;

    return glob;
}

// Build a _ci_str struct value from a string literal
static LLVMValueRef build_ci_str(LLVMCodegen *g, const char *str) {
    LLVMValueRef glob = get_string_global(g, str);

    // GEP to get i8* from the array global
    LLVMTypeRef  i8_type  = LLVMInt8TypeInContext(g->ctx);
    LLVMTypeRef  i64_type = LLVMInt64TypeInContext(g->ctx);
    LLVMValueRef zero     = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0);
    LLVMValueRef indices[2] = { zero, zero };
    LLVMTypeRef  arr_type = LLVMArrayType(i8_type, (unsigned)strlen(str) + 1);
    LLVMValueRef ptr      = LLVMBuildGEP2(g->builder, arr_type, glob,
                                           indices, 2, "str.ptr");

    LLVMValueRef len = LLVMConstInt(i64_type, strlen(str), 0);

    // Build { ptr, len } struct
    LLVMTypeRef str_type = LLVMGetTypeByName2(g->ctx, "_ci_str");
    if (!str_type) {
        str_type = LLVMStructCreateNamed(g->ctx, "_ci_str");
        LLVMTypeRef fields[2] = { LLVMPointerType(i8_type, 0), i64_type };
        LLVMStructSetBody(str_type, fields, 2, 0);
    }
    LLVMValueRef result  = LLVMGetUndef(str_type);
    result = LLVMBuildInsertValue(g->builder, result, ptr, 0, "str.0");
    result = LLVMBuildInsertValue(g->builder, result, len, 1, "str.1");
    return result;
}

// Look up the index of a named field in a struct declaration
static int get_field_index(LLVMCodegen *g, const char *struct_name,
                            const char *field_name) {
    Symbol *sym = scope_lookup(g->resolver->global, struct_name);
    if (!sym || sym->kind != SYM_STRUCT || !sym->decl) return -1;
    AstNode *decl = sym->decl;
    for (size_t i = 0; i < decl->as.struct_decl.field_count; i++) {
        if (strcmp(decl->as.struct_decl.fields[i].name, field_name) == 0)
            return (int)i;
    }
    return -1;
}

// Get the LLVM type for a named struct
static LLVMTypeRef get_struct_llvm_type(LLVMCodegen *g,
                                         const char *struct_name) {
    LLVMTypeRef t = LLVMGetTypeByName2(g->ctx, struct_name);
    if (!t) llvm_error(g, NULL, "unknown struct type '%s'", struct_name);
    return t;
}

// How many i64 payload slots does a type need?
static unsigned type_word_count(AstType *t) {
    if (!t) return 0;
    if (t->kind == TY_STR || t->kind == TY_SLICE) return 2;
    return 1;
}

static void register_enum_types(LLVMCodegen *g, AstNode *program) {
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_ENUM_DECL) continue;

        // Find max payload words across all variants
        unsigned max_words = 0;
        for (size_t v = 0; v < decl->as.enum_decl.variant_count; v++) {
            AstVariant *var = &decl->as.enum_decl.variants[v];
            unsigned words = 0;
            for (size_t f = 0; f < var->field_count; f++)
                words += type_word_count(var->fields[f].type);
            if (words > max_words) max_words = words;
        }

        // { i32 tag, [max_words x i64] payload }
        LLVMTypeRef i32  = LLVMInt32TypeInContext(g->ctx);
        LLVMTypeRef i64  = LLVMInt64TypeInContext(g->ctx);
        LLVMTypeRef body[2];
        body[0] = i32;
        body[1] = LLVMArrayType(i64, max_words);
        LLVMTypeRef st = LLVMStructCreateNamed(g->ctx, decl->as.enum_decl.name);
        LLVMStructSetBody(st, body, max_words > 0 ? 2 : 1, 0);
    }
}

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────

static LLVMValueRef emit_expr(LLVMCodegen *g, AstNode *node);
static void         emit_stmt(LLVMCodegen *g, AstNode *node);
static void         emit_func(LLVMCodegen *g, AstNode *node);
static void         emit_mono_func(LLVMCodegen *g, AstNode *func,
                        const char *mangled_name,
                        const char **tparams, AstType **ctypes, size_t nparams);

// ─── EXPRESSION EMISSION ─────────────────────────────────────────────────────

static LLVMValueRef emit_expr(LLVMCodegen *g, AstNode *node) {
    if (!node) return NULL;

    switch (node->kind) {

        case NODE_INT_LIT:
            return LLVMConstInt(LLVMInt32TypeInContext(g->ctx),
                                (unsigned long long)node->as.int_lit.value, 1);

        case NODE_FLOAT_LIT:
            return LLVMConstReal(LLVMDoubleTypeInContext(g->ctx),
                                 node->as.float_lit.value);

        case NODE_BOOL_LIT:
            return LLVMConstInt(LLVMInt8TypeInContext(g->ctx),
                                node->as.bool_lit.value, 0);

        case NODE_NULL_LIT:
            return LLVMConstNull(
                LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0));

        case NODE_CHAR_LIT:
            return LLVMConstInt(LLVMInt8TypeInContext(g->ctx),
                                (unsigned char)node->as.char_lit.value, 0);

        case NODE_STRING_LIT:
            return build_ci_str(g, node->as.string_lit.value);

        case NODE_CSTRING_LIT: {
            LLVMValueRef glob = get_string_global(g, node->as.cstring_lit.value);
            LLVMTypeRef  i8   = LLVMInt8TypeInContext(g->ctx);
            LLVMTypeRef  arr  = LLVMArrayType(i8,
                (unsigned)strlen(node->as.cstring_lit.value) + 1);
            LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0);
            LLVMValueRef idx[2] = { zero, zero };
            return LLVMBuildGEP2(g->builder, arr, glob, idx, 2, "cstr");
        }

        case NODE_IDENT: {
            const char *name = node->as.ident.name;
            LLVMEntry  *e    = scope_get(g, name);
            if (!e) {
                // Could be a global function reference
                LLVMValueRef fn = LLVMGetNamedFunction(g->module, name);
                if (fn) return fn;
                llvm_error(g, node, "undefined symbol '%s'", name);
                return LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0);
            }
            if (e->is_alloca)
                return LLVMBuildLoad2(g->builder, e->type, e->value, name);
            return e->value;
        }

        case NODE_BINARY: {
            LLVMValueRef left  = emit_expr(g, node->as.binary.left);
            LLVMValueRef right = emit_expr(g, node->as.binary.right);
            if (!left || !right) return NULL;

            LLVMTypeRef lt = LLVMTypeOf(left);
            int is_float   = (LLVMGetTypeKind(lt) == LLVMFloatTypeKind ||
                              LLVMGetTypeKind(lt) == LLVMDoubleTypeKind);
            
            // Implicit integer promotion — widen narrower side to match wider
            {
                LLVMTypeRef rt2 = LLVMTypeOf(right);
                LLVMTypeKind lk = LLVMGetTypeKind(lt);
                LLVMTypeKind rk = LLVMGetTypeKind(rt2);
            if (lk == LLVMIntegerTypeKind && rk == LLVMIntegerTypeKind) {
                unsigned lw = LLVMGetIntTypeWidth(lt);
                unsigned rw = LLVMGetIntTypeWidth(rt2);
                if (lw > rw) {
                    // Left is wider — truncate LEFT down (clears FFI garbage bits)
                    left = LLVMBuildTrunc(g->builder, left, rt2, "itrunc");
                    lt   = rt2;
                    is_float = 0;
                } else if (rw > lw) {
                    // Right is wider — truncate RIGHT down to match left
                    right = LLVMBuildTrunc(g->builder, right, lt, "itrunc");
                }
            /*if (lw > rw) {
                // Left is 64, Right is 32. 
                // Extend the Right side up to 64 so we don't lose data!
                right = LLVMBuildZExt(g->builder, right, lt, "izext");
            } else if (rw > lw) {
                // Right is 64, Left is 32.
                // Extend the Left side up to 64.
                left = LLVMBuildZExt(g->builder, left, rt2, "izext");
                lt = rt2;
            } */
            }
                
            }

            switch (node->as.binary.op) {
                case TOK_PLUS: {
                    LLVMTypeRef lt2 = LLVMTypeOf(left);
                    if (LLVMGetTypeKind(lt2) == LLVMPointerTypeKind) {
                        LLVMTypeRef elem = LLVMInt32TypeInContext(g->ctx);
                        if (node->as.binary.left->kind == NODE_IDENT) {
                            LLVMEntry *e = scope_get(g, node->as.binary.left->as.ident.name);
                            if (e && e->elem_type) elem = e->elem_type;
                        }
                        return LLVMBuildGEP2(g->builder, elem, left, &right, 1, "ptradd");
                    }
                    return is_float
                        ? LLVMBuildFAdd(g->builder, left, right, "fadd")
                        : LLVMBuildAdd(g->builder, left, right, "add");
                }
                case TOK_MINUS: {
                    LLVMTypeRef lt2 = LLVMTypeOf(left);
                    LLVMTypeRef rt2 = LLVMTypeOf(right);
                    // pointer - pointer = int (ptrdiff)
                    if (LLVMGetTypeKind(lt2) == LLVMPointerTypeKind &&
                        LLVMGetTypeKind(rt2) == LLVMPointerTypeKind) {
                        LLVMTypeRef i64t = LLVMInt64TypeInContext(g->ctx);
                        LLVMTypeRef i32t = LLVMInt32TypeInContext(g->ctx);
                        LLVMValueRef li = LLVMBuildPtrToInt(g->builder, left,  i64t, "lptr");
                        LLVMValueRef ri = LLVMBuildPtrToInt(g->builder, right, i64t, "rptr");
                        LLVMValueRef diff = LLVMBuildSub(g->builder, li, ri, "ptrdiff");
                        return LLVMBuildTrunc(g->builder, diff, i32t, "ptrdiff.i32");
                    }
                    return is_float
                        ? LLVMBuildFSub(g->builder, left, right, "fsub")
                        : LLVMBuildSub (g->builder, left, right, "sub");
                }
                case TOK_STAR:
                    return is_float
                        ? LLVMBuildFMul(g->builder, left, right, "fmul")
                        : LLVMBuildMul (g->builder, left, right, "mul");
                case TOK_SLASH:
                    return is_float
                        ? LLVMBuildFDiv(g->builder, left, right, "fdiv")
                        : LLVMBuildSDiv(g->builder, left, right, "sdiv");
                case TOK_PERCENT:
                    return LLVMBuildSRem(g->builder, left, right, "srem");
                case TOK_AMP:
                    return LLVMBuildAnd(g->builder, left, right, "and");
                case TOK_PIPE:
                    return LLVMBuildOr(g->builder, left, right, "or");
                case TOK_CARET:
                    return LLVMBuildXor(g->builder, left, right, "xor");
                case TOK_LSHIFT:
                    return LLVMBuildShl(g->builder, left, right, "shl");
                case TOK_RSHIFT:
                    return LLVMBuildAShr(g->builder, left, right, "ashr");
                case TOK_AND: {
                    LLVMValueRef l1 = LLVMBuildICmp(g->builder, LLVMIntNE,
                        left,  LLVMConstInt(lt, 0, 0), "land.l");
                    LLVMValueRef l2 = LLVMBuildICmp(g->builder, LLVMIntNE,
                        right, LLVMConstInt(LLVMTypeOf(right), 0, 0), "land.r");
                    return LLVMBuildAnd(g->builder, l1, l2, "land");
                }
                case TOK_OR: {
                    LLVMValueRef l1 = LLVMBuildICmp(g->builder, LLVMIntNE,
                        left,  LLVMConstInt(lt, 0, 0), "lor.l");
                    LLVMValueRef l2 = LLVMBuildICmp(g->builder, LLVMIntNE,
                        right, LLVMConstInt(LLVMTypeOf(right), 0, 0), "lor.r");
                    return LLVMBuildOr(g->builder, l1, l2, "lor");
                }
                case TOK_EQ:
                    return is_float
                        ? LLVMBuildFCmp(g->builder, LLVMRealOEQ, left, right, "feq")
                        : LLVMBuildICmp(g->builder, LLVMIntEQ,   left, right, "eq");
                case TOK_NEQ:
                    return is_float
                        ? LLVMBuildFCmp(g->builder, LLVMRealONE, left, right, "fne")
                        : LLVMBuildICmp(g->builder, LLVMIntNE,   left, right, "ne");
                case TOK_LT:
                    return is_float
                        ? LLVMBuildFCmp(g->builder, LLVMRealOLT, left, right, "flt")
                        : LLVMBuildICmp(g->builder, LLVMIntSLT,  left, right, "lt");
                case TOK_GT:
                    return is_float
                        ? LLVMBuildFCmp(g->builder, LLVMRealOGT, left, right, "fgt")
                        : LLVMBuildICmp(g->builder, LLVMIntSGT,  left, right, "gt");
                case TOK_LTE:
                    return is_float
                        ? LLVMBuildFCmp(g->builder, LLVMRealOLE, left, right, "fle")
                        : LLVMBuildICmp(g->builder, LLVMIntSLE,  left, right, "le");
                case TOK_GTE:
                    return is_float
                        ? LLVMBuildFCmp(g->builder, LLVMRealOGE, left, right, "fge")
                        : LLVMBuildICmp(g->builder, LLVMIntSGE,  left, right, "ge");
                default:
                    llvm_error(g, node, "unhandled binary op");
                    return left;
            }
        }

        case NODE_UNARY: {
            LLVMValueRef val = emit_expr(g, node->as.unary.operand);
            if (!val) return NULL;
            switch (node->as.unary.op) {
                case TOK_MINUS:
                    return LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMDoubleTypeKind
                        ? LLVMBuildFNeg(g->builder, val, "fneg")
                        : LLVMBuildNeg (g->builder, val, "neg");
                case TOK_BANG: {
                    LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(val), 0, 0);
                    return LLVMBuildICmp(g->builder, LLVMIntEQ, val, zero, "not");
                }
                case TOK_TILDE:
                    return LLVMBuildNot(g->builder, val, "bnot");
                case TOK_PLUS_PLUS: {
                    // postfix/prefix ++ — load, add 1, store back
                    LLVMEntry *e = scope_get(g, node->as.unary.operand->as.ident.name);
                    if (!e || !e->is_alloca) return val;
                    LLVMValueRef one     = LLVMConstInt(LLVMTypeOf(val), 1, 0);
                    LLVMValueRef newval  = LLVMBuildAdd(g->builder, val, one, "inc");
                    LLVMBuildStore(g->builder, newval, e->value);
                    return node->as.unary.postfix ? val : newval;
                }
                case TOK_MINUS_MINUS: {
                    LLVMEntry *e = scope_get(g, node->as.unary.operand->as.ident.name);
                    if (!e || !e->is_alloca) return val;
                    LLVMValueRef one    = LLVMConstInt(LLVMTypeOf(val), 1, 0);
                    LLVMValueRef newval = LLVMBuildSub(g->builder, val, one, "dec");
                    LLVMBuildStore(g->builder, newval, e->value);
                    return node->as.unary.postfix ? val : newval;
                }
                default: return val;
            }
        }

        case NODE_ADDRESS_OF: {
            AstNode *operand = node->as.unary.operand;
            if (operand->kind == NODE_IDENT) {
                LLVMEntry *e = scope_get(g, operand->as.ident.name);
                if (e && e->is_alloca) return e->value;
            }
            if (operand->kind == NODE_INDEX) {
                LLVMValueRef idx = emit_expr(g, operand->as.index.index);
                if (!idx) return NULL;
                // Get the alloca directly, not the loaded value
                AstNode *tgt_node = operand->as.index.target;
                LLVMEntry *e = (tgt_node->kind == NODE_IDENT)
                    ? scope_get(g, tgt_node->as.ident.name) : NULL;
                if (e && e->is_alloca) {
                    LLVMTypeRef i32 = LLVMInt32TypeInContext(g->ctx);
                    LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
                    LLVMValueRef indices[2] = { zero, idx };
                    return LLVMBuildGEP2(g->builder, e->type, e->value, indices, 2, "addr.idx");
                }
                LLVMValueRef tgt = emit_expr(g, tgt_node);
                LLVMTypeRef elem = LLVMInt32TypeInContext(g->ctx);
                return LLVMBuildGEP2(g->builder, elem, tgt, &idx, 1, "addr.idx");
            }
            llvm_error(g, node, "cannot take address of this expression");
            return NULL;
        }

        case NODE_DEREF: {
            LLVMValueRef ptr = emit_expr(g, node->as.unary.operand);
            if (!ptr) return NULL;
            LLVMTypeRef inner = LLVMInt32TypeInContext(g->ctx); // fallback
            AstNode *op = node->as.unary.operand;
            if (op->kind == NODE_IDENT) {
                LLVMEntry *e = scope_get(g, op->as.ident.name);
                if (e && e->elem_type)
                    inner = e->elem_type;
            } else if (op->kind == NODE_FIELD) {
                // (*obj.field) — look up field type from struct decl
                AstNode *ftgt = op->as.field.target;
                const char *sname = NULL;
                if (ftgt->kind == NODE_IDENT) {
                    LLVMEntry *e = scope_get(g, ftgt->as.ident.name);
                    if (e && e->type && LLVMGetTypeKind(e->type) == LLVMStructTypeKind)
                        sname = LLVMGetStructName(e->type);
                }
                if (sname) {
                    Symbol *sym = scope_lookup(g->resolver->global, sname);
                    if (sym && sym->decl) {
                        AstNode *decl = sym->decl;
                        for (size_t fi = 0; fi < decl->as.struct_decl.field_count; fi++) {
                            if (strcmp(decl->as.struct_decl.fields[fi].name,
                                       op->as.field.field) == 0) {
                                AstType *ft = decl->as.struct_decl.fields[fi].type;
                                if (ft->kind == TY_POINTER && ft->inner) {
                                    inner = ci_type_to_llvm(g, ft->inner);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            return LLVMBuildLoad2(g->builder, inner, ptr, "deref");
        }

        case NODE_PROPAGATE: {
            // expr? — extract Ok value or early return Err
            LLVMValueRef result = emit_expr(g, node->as.propagate.expr);
            if (!result) return NULL;
            LLVMTypeRef result_t = LLVMTypeOf(result);
            // Extract tag (field 0)
            // If result is a pointer, load it first
            if (LLVMGetTypeKind(LLVMTypeOf(result)) == LLVMPointerTypeKind) {
                // Can't load without knowing elem type — this shouldn't happen
                llvm_error(g, node, "'?' applied to non-Result value");
                return NULL;
            }
            if (LLVMGetTypeKind(LLVMTypeOf(result)) != LLVMStructTypeKind) {
                llvm_error(g, node, "'?' applied to non-Result value");
                return NULL;
            }
            LLVMValueRef tag = LLVMBuildExtractValue(g->builder, result, 0, "res.tag");
            LLVMTypeRef i32t = LLVMInt32TypeInContext(g->ctx);
            LLVMValueRef zero = LLVMConstInt(i32t, 0, 0);
            LLVMValueRef is_ok = LLVMBuildICmp(g->builder, LLVMIntEQ, tag, zero, "is_ok");
            LLVMValueRef fn = g->current_func;
            int lbl = new_label(g);
            char ok_name[32], err_name[32];
            snprintf(ok_name,  sizeof(ok_name),  "prop.ok.%d",  lbl);
            snprintf(err_name, sizeof(err_name), "prop.err.%d", lbl);
            LLVMBasicBlockRef ok_bb  = LLVMAppendBasicBlockInContext(g->ctx, fn, ok_name);
            LLVMBasicBlockRef err_bb = LLVMAppendBasicBlockInContext(g->ctx, fn, err_name);
            LLVMBuildCondBr(g->builder, is_ok, ok_bb, err_bb);
            // Err path: return Err(payload)
            LLVMPositionBuilderAtEnd(g->builder, err_bb);
            // Re-wrap as canonical _ci_result and early return
            LLVMValueRef err_val = LLVMBuildExtractValue(g->builder, result, 1, "err.val");
            // err_val is i64 — keep as-is, the callee will re-wrap it
            LLVMTypeRef  ci_result_t = LLVMGetTypeByName2(g->ctx, "_ci_result");
            if (!ci_result_t) ci_result_t = LLVMTypeOf(result);
            LLVMValueRef err_result = LLVMGetUndef(ci_result_t);
            err_result = LLVMBuildInsertValue(g->builder, err_result,
                LLVMConstInt(i32t, 1, 0), 0, "err.tag");
            err_result = LLVMBuildInsertValue(g->builder, err_result,
                err_val, 1, "err.result");
            LLVMBuildRet(g->builder, err_result);
            // Ok path: extract value and truncate i64 → i32
            LLVMPositionBuilderAtEnd(g->builder, ok_bb);
            LLVMValueRef ok64 = LLVMBuildExtractValue(g->builder, result, 1, "ok.val");
            return LLVMBuildTrunc(g->builder, ok64, LLVMInt32TypeInContext(g->ctx), "ok.i32");
        }

        case NODE_ASSIGN: {
            LLVMValueRef rhs = emit_expr(g, node->as.assign.value);
            if (!rhs) return NULL;

            // Resolve the target to its alloca pointer
            AstNode *tgt = node->as.assign.target;
            LLVMValueRef ptr = NULL;

            if (tgt->kind == NODE_IDENT) {
                LLVMEntry *e = scope_get(g, tgt->as.ident.name);
                if (e && e->is_alloca) ptr = e->value;
            } else if (tgt->kind == NODE_DEREF) {
                ptr = emit_expr(g, tgt->as.unary.operand);
            } else if (tgt->kind == NODE_FIELD) {
                AstNode *ftgt = tgt->as.field.target;
                // (*ptr).field = val — deref pointer then GEP
                if (ftgt->kind == NODE_DEREF) {
                    AstNode *inner = ftgt->as.unary.operand;
                    LLVMEntry *pe = (inner->kind == NODE_IDENT)
                        ? scope_get(g, inner->as.ident.name) : NULL;
                    if (pe && pe->elem_type) {
                        LLVMValueRef struct_ptr = LLVMBuildLoad2(g->builder,
                            LLVMPointerType(pe->elem_type, 0), pe->value, "deref.ptr");
                        const char *sname = LLVMGetStructName(pe->elem_type);
                        if (sname) {
                            int idx = get_field_index(g, sname, tgt->as.field.field);
                            if (idx >= 0)
                                ptr = LLVMBuildStructGEP2(g->builder, pe->elem_type,
                                    struct_ptr, (unsigned)idx, "field.ptr");
                        }
                    }
                } else {
                    // Regular field assignment: obj.field = val
                    LLVMEntry *fe = (ftgt->kind == NODE_IDENT)
                        ? scope_get(g, ftgt->as.ident.name) : NULL;
                    if (fe && fe->is_alloca) {
                        LLVMTypeRef btype = fe->type;
                        const char *sname = LLVMGetStructName(btype);
                        if (sname) {
                            // Check if union — bitcast to field type
                            Symbol *tsym = scope_lookup(g->resolver->global, sname);
                            if (tsym && tsym->decl &&
                                tsym->decl->kind == NODE_UNION_DECL) {
                                AstNode *udecl = tsym->decl;
                                AstType *ftype = NULL;
                                for (size_t fi = 0; fi < udecl->as.struct_decl.field_count; fi++) {
                                    if (strcmp(udecl->as.struct_decl.fields[fi].name,
                                               tgt->as.field.field) == 0) {
                                        ftype = udecl->as.struct_decl.fields[fi].type;
                                        break;
                                    }
                                }
                                if (ftype) {
                                    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0);
                                    LLVMValueRef indices[2] = { zero, zero };
                                    ptr = LLVMBuildGEP2(g->builder, btype,
                                        fe->value, indices, 2, "union.assign");
                                }
                            } else {
                                int idx = get_field_index(g, sname, tgt->as.field.field);
                                if (idx >= 0)
                                    ptr = LLVMBuildStructGEP2(g->builder, btype,
                                        fe->value, (unsigned)idx, "field.assign");
                            }
                        }
                    }
                }
            }

            if (!ptr) {
                llvm_error(g, node, "cannot assign to this target");
                return rhs;
            }

            // Compound assignment operators
            if (node->as.assign.op != TOK_ASSIGN) {
                LLVMValueRef cur = LLVMBuildLoad2(g->builder,
                    LLVMGetElementType(LLVMTypeOf(ptr)), ptr, "cur");
                switch (node->as.assign.op) {
                    case TOK_PLUS_ASSIGN:
                        rhs = LLVMBuildAdd(g->builder, cur, rhs, "addtmp"); break;
                    case TOK_MINUS_ASSIGN:
                        rhs = LLVMBuildSub(g->builder, cur, rhs, "subtmp"); break;
                    case TOK_STAR_ASSIGN:
                        rhs = LLVMBuildMul(g->builder, cur, rhs, "multmp"); break;
                    case TOK_SLASH_ASSIGN:
                        rhs = LLVMBuildSDiv(g->builder, cur, rhs, "divtmp"); break;
                    default: break;
                }
            }

            LLVMBuildStore(g->builder, rhs, ptr);
            return rhs;
        }

        case NODE_CALL: {
            // Resolve callee
            LLVMValueRef fn   = NULL;
            LLVMTypeRef  fnty = NULL;
            
            // ── Method call: obj.method(args) ─────────
            if (node->as.call.callee->kind == NODE_FIELD) {
                const char *method_name =
                    node->as.call.callee->as.field.field;
                AstNode *self_node =
                    node->as.call.callee->as.field.target;

                // Resolve function
                // Try mangled name first: method__TypeName
                fn = NULL;
                if (self_node->kind == NODE_IDENT) {
                    LLVMEntry *se2 = scope_get(g, self_node->as.ident.name);
                    if (se2) {
                        LLVMTypeRef sty = se2->type;
                        // For alloca'd structs, type IS the struct type
                        // For pointer-to-struct, dereference
                        if (LLVMGetTypeKind(sty) == LLVMPointerTypeKind && se2->elem_type)
                            sty = se2->elem_type;
                        if (LLVMGetTypeKind(sty) == LLVMStructTypeKind) {
                            const char *sname = LLVMGetStructName(sty);
                            if (sname) {
                                // Strip any "struct." prefix LLVM may add
                                const char *base = sname;
                                if (strncmp(base, "struct.", 7) == 0) base += 7;
                                char mangled[256];
                                snprintf(mangled, sizeof(mangled), "%s__%s",
                                         method_name, base);
                                fn = LLVMGetNamedFunction(g->module, mangled);
                            }
                        }
                    }
                }

                // Fall back to unmangled (regular methods/functions)
                if (!fn) fn = LLVMGetNamedFunction(g->module, method_name);
                if (!fn) {
                    llvm_error(g, node, "unknown method '%s'", method_name);
                    return NULL;
                }
                fnty = LLVMGlobalGetValueType(fn);

                // Build arg list: self first, then explicit args
                unsigned total = (unsigned)(node->as.call.arg_count + 1);
                LLVMValueRef *margs = malloc(total * sizeof(LLVMValueRef));

                // Self: load the value of the target
                LLVMEntry *se = (self_node->kind == NODE_IDENT)
                    ? scope_get(g, self_node->as.ident.name) : NULL;
                if (se && se->is_alloca)
                    margs[0] = LLVMBuildLoad2(g->builder, se->type,
                                               se->value, "self");
                else
                    margs[0] = emit_expr(g, self_node);

                // Remaining args
                for (unsigned i = 0; i < (unsigned)node->as.call.arg_count; i++)
                    margs[i + 1] = emit_expr(g, node->as.call.args[i]);

                LLVMTypeRef ret_type = LLVMGetReturnType(fnty);
                LLVMValueRef result;
                if (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind)
                    result = LLVMBuildCall2(g->builder, fnty, fn,
                                            margs, total, "");
                else
                    result = LLVMBuildCall2(g->builder, fnty, fn,
                                            margs, total, "mcall");
                free(margs);
                return result;
            }

            if (node->as.call.callee->kind == NODE_IDENT) {
                const char *name = node->as.call.callee->as.ident.name;
                // Built-in Result constructors: Ok(val) / Err(val)
                if (strcmp(name, "Ok") == 0 || strcmp(name, "Err") == 0) {
                    int is_ok = strcmp(name, "Ok") == 0;
                    // Result = { i32 tag, <payload> }
                    // tag: 0 = Ok, 1 = Err
                    LLVMTypeRef i32t = LLVMInt32TypeInContext(g->ctx);
                    LLVMValueRef payload = NULL;
                    LLVMTypeRef  payload_t = i32t; // default
                    if (node->as.call.arg_count > 0) {
                        payload   = emit_expr(g, node->as.call.args[0]);
                        payload_t = LLVMTypeOf(payload);
                    } else {
                        payload = LLVMConstInt(i32t, 0, 0);
                    }
                    // Canonical Result = { i32 tag, i64 payload } — bitcast payload to i64
                    LLVMTypeRef result_t = LLVMGetTypeByName2(g->ctx, "_ci_result");
                    if (!result_t) {
                        result_t = LLVMStructCreateNamed(g->ctx, "_ci_result");
                        LLVMTypeRef fields[2] = { i32t, LLVMInt64TypeInContext(g->ctx) };
                        LLVMStructSetBody(result_t, fields, 2, 0);
                    }
                    LLVMTypeRef i64t = LLVMInt64TypeInContext(g->ctx);
                    // Bitcast payload to i64
                    LLVMValueRef pay64;
                    LLVMTypeKind pk = LLVMGetTypeKind(payload_t);
                    if (pk == LLVMIntegerTypeKind)
                        pay64 = LLVMBuildSExt(g->builder, payload, i64t, "pay64");
                    else if (pk == LLVMDoubleTypeKind)
                        pay64 = LLVMBuildBitCast(g->builder, payload, i64t, "pay64");
                    else if (pk == LLVMPointerTypeKind)
                        pay64 = LLVMBuildPtrToInt(g->builder, payload, i64t, "pay64");
                    else if (pk == LLVMStructTypeKind) {
                        // For str/slice: store and load as i64 (first word)
                        LLVMValueRef slot = LLVMBuildAlloca(g->builder, payload_t, "pay.slot");
                        LLVMBuildStore(g->builder, payload, slot);
                        LLVMValueRef slot64 = LLVMBuildBitCast(g->builder, slot,
                            LLVMPointerType(i64t, 0), "slot64");
                        pay64 = LLVMBuildLoad2(g->builder, i64t, slot64, "pay64");
                    } else
                        pay64 = LLVMBuildBitCast(g->builder, payload, i64t, "pay64");
                    LLVMValueRef result = LLVMGetUndef(result_t);
                    result = LLVMBuildInsertValue(g->builder, result,
                        LLVMConstInt(i32t, is_ok ? 0 : 1, 0), 0, "result.tag");
                    result = LLVMBuildInsertValue(g->builder, result, pay64, 1, "result.val");
                    return result;
                }
                // Check if this is a generic function call
                Symbol *fsym = scope_lookup(g->resolver->global, name);
                if (fsym && fsym->kind == SYM_FUNCTION && fsym->decl
                        && fsym->decl->as.func_decl.generic_count > 0
                        && node->as.call.arg_count > 0) {
                    AstNode *fdecl = fsym->decl;
                    size_t ngp = fdecl->as.func_decl.generic_count;
                    // Infer T from first argument type
                    LLVMValueRef first_arg = emit_expr(g, node->as.call.args[0]);
                    LLVMTypeRef  arg_llvm  = first_arg ? LLVMTypeOf(first_arg) : NULL;
                    // Build concrete AstType from LLVM type name
                    AstType *concrete = arena_alloc(g->arena, sizeof(AstType));
                    memset(concrete, 0, sizeof(AstType));
                    if (arg_llvm && LLVMGetTypeKind(arg_llvm) == LLVMStructTypeKind) {
                        concrete->kind = TY_NAMED;
                        const char *sn = LLVMGetStructName(arg_llvm);
                        char *scopy = arena_alloc(g->arena, strlen(sn) + 1);
                        strcpy(scopy, sn);
                        concrete->name = scopy;
                    } else if (arg_llvm && LLVMGetTypeKind(arg_llvm) == LLVMDoubleTypeKind) {
                        concrete->kind = TY_F64;
                    } else if (arg_llvm && LLVMGetTypeKind(arg_llvm) == LLVMFloatTypeKind) {
                        concrete->kind = TY_F32;
                    } else {
                        concrete->kind = TY_INT;
                    }
                    // Build mangled name: func__TypeName
                    char type_suffix[64];
                    if (concrete->kind == TY_NAMED)
                        snprintf(type_suffix, sizeof(type_suffix), "%s", concrete->name);
                    else if (concrete->kind == TY_F64)
                        snprintf(type_suffix, sizeof(type_suffix), "f64");
                    else if (concrete->kind == TY_F32)
                        snprintf(type_suffix, sizeof(type_suffix), "f32");
                    else
                        snprintf(type_suffix, sizeof(type_suffix), "int");
                    char mangled[256];
                    snprintf(mangled, sizeof(mangled), "%s__%s", name, type_suffix);
                    // Emit monomorphized version if not already done
                    const char *tparams[8];
                    AstType    *ctypes[8];
                    for (size_t gi = 0; gi < ngp && gi < 8; gi++) {
                        tparams[gi] = fdecl->as.func_decl.generic_params[gi];
                        ctypes[gi]  = concrete;
                    }
                    emit_mono_func(g, fdecl, mangled, tparams, ctypes, ngp < 8 ? ngp : 8);
                    fn   = LLVMGetNamedFunction(g->module, mangled);
                    fnty = fn ? LLVMGlobalGetValueType(fn) : NULL;
                    // Build arg list manually since we already emitted first_arg
                    if (fn && fnty) {
                        unsigned argc2 = (unsigned)node->as.call.arg_count;
                        LLVMValueRef *argv2 = malloc(argc2 * sizeof(LLVMValueRef));
                        argv2[0] = first_arg;
                        for (unsigned i = 1; i < argc2; i++)
                            argv2[i] = emit_expr(g, node->as.call.args[i]);
                        LLVMTypeRef ret2 = LLVMGetReturnType(fnty);
                        LLVMValueRef res2;
                        if (LLVMGetTypeKind(ret2) == LLVMVoidTypeKind)
                            res2 = LLVMBuildCall2(g->builder, fnty, fn, argv2, argc2, "");
                        else
                            res2 = LLVMBuildCall2(g->builder, fnty, fn, argv2, argc2, "mcall");
                        free(argv2);
                        return res2;
                    }
                }
                fn = LLVMGetNamedFunction(g->module, name);
                if (!fn) {
                    LLVMEntry *e = scope_get(g, name);
                    if (e) { fn = e->value; fnty = e->type; }
                }
                if (fn) fnty = LLVMGlobalGetValueType(fn);
            } else {
                fn = emit_expr(g, node->as.call.callee);
                if (fn) fnty = LLVMGetElementType(LLVMTypeOf(fn));
            }

            if (!fn) {

                // need the callee name for LLVMAddFunction
                const char *name = (node->as.call.callee->kind == NODE_IDENT)
                    ? node->as.call.callee->as.ident.name
                    : "unknown_extern";
                // ── Auto-declare external C function ──────────────────────────
                // Infer signature from call-site argument types, declare as
                // external linkage. Covers any C library (GTK, SDL, etc.)
                // without needing compiler-side knowledge of the library.
                unsigned argc_ad = (unsigned)node->as.call.arg_count;
                LLVMTypeRef  *param_ad = malloc((argc_ad + 1) * sizeof(LLVMTypeRef));
                LLVMValueRef *argv_ad  = malloc((argc_ad + 1) * sizeof(LLVMValueRef));
                for (unsigned i = 0; i < argc_ad; i++) {
                    argv_ad[i] = emit_expr(g, node->as.call.args[i]);
                    if (!argv_ad[i])
                        argv_ad[i] = LLVMConstInt(LLVMInt64TypeInContext(g->ctx), 0, 0);
                    param_ad[i] = LLVMTypeOf(argv_ad[i]);
                }
                // Default return type: i64 — holds ints, pointers, and ignored
                // void returns equally. Caller casts with  expr as *T  if needed.
                LLVMTypeRef ret_ad = LLVMInt64TypeInContext(g->ctx);
                fnty = LLVMFunctionType(ret_ad, param_ad, argc_ad, 0);
                fn   = LLVMAddFunction(g->module, name, fnty);
                LLVMSetLinkage(fn, LLVMExternalLinkage);
                LLVMValueRef res_ad = LLVMBuildCall2(g->builder, fnty, fn,
                                                      argv_ad, argc_ad, "xcall");
                free(param_ad);
                free(argv_ad);
                return res_ad;
            }

            // Build argument list
            unsigned    argc = (unsigned)node->as.call.arg_count;
            LLVMValueRef *argv = malloc(argc * sizeof(LLVMValueRef));
            unsigned nparams = LLVMCountParamTypes(fnty);
            LLVMTypeRef *param_types = malloc(nparams * sizeof(LLVMTypeRef));
            LLVMGetParamTypes(fnty, param_types);
            for (unsigned i = 0; i < argc; i++) {
                argv[i] = emit_expr(g, node->as.call.args[i]);
                if (!argv[i]) { argv[i] = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0); continue; }
                // Coerce arg type to match param type if needed
                if (i < nparams) {
                    LLVMTypeRef pt = param_types[i];
                    LLVMTypeRef at = LLVMTypeOf(argv[i]);
                    LLVMTypeKind pk = LLVMGetTypeKind(pt);
                    LLVMTypeKind ak = LLVMGetTypeKind(at);
                    if (pk == LLVMIntegerTypeKind && ak == LLVMIntegerTypeKind) {
                        unsigned pw = LLVMGetIntTypeWidth(pt);
                        unsigned aw = LLVMGetIntTypeWidth(at);
                        if (pw > aw)
                            argv[i] = LLVMBuildSExt(g->builder, argv[i], pt, "sext");
                        else if (pw < aw)
                            argv[i] = LLVMBuildTrunc(g->builder, argv[i], pt, "trunc");
                    } else if (pk == LLVMPointerTypeKind && ak == LLVMIntegerTypeKind) {
                        argv[i] = LLVMBuildIntToPtr(g->builder, argv[i], pt, "itoptr");
                    } else if (pk == LLVMIntegerTypeKind && ak == LLVMPointerTypeKind) {
                        argv[i] = LLVMBuildPtrToInt(g->builder, argv[i], pt, "ptoi");
                    }
                }
            }
            free(param_types);

            LLVMTypeRef ret_type = LLVMGetReturnType(fnty);
            LLVMValueRef result;
            if (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind)
                result = LLVMBuildCall2(g->builder, fnty, fn, argv, argc, "");
            else
                result = LLVMBuildCall2(g->builder, fnty, fn, argv, argc, "call");

            free(argv);
            return result;
        }

        case NODE_FIELD: {
            AstNode *tgt = node->as.field.target;

            // We need the alloca pointer, not the loaded value,
            // so we can GEP into it
            LLVMEntry *e = NULL;

            // ── Slice fields: .len and .ptr ───────────
            // Check if target is a slice variable
            // ── Str and Slice fields: .len and .ptr ───────────
            if (tgt->kind == NODE_IDENT) {
                LLVMEntry *se = scope_get(g, tgt->as.ident.name);
                if (se && se->is_alloca && LLVMGetTypeKind(se->type) == LLVMStructTypeKind
                        && LLVMCountStructElementTypes(se->type) == 2) {
                    const char *fname = node->as.field.field;
                    if (strcmp(fname, "len") == 0) {
                        LLVMValueRef sv = LLVMBuildLoad2(g->builder, se->type, se->value, "sv");
                        return LLVMBuildExtractValue(g->builder, sv, 1, "sv.len");
                    }
                    if (strcmp(fname, "ptr") == 0) {
                        LLVMValueRef sv = LLVMBuildLoad2(g->builder, se->type, se->value, "sv");
                        return LLVMBuildExtractValue(g->builder, sv, 0, "sv.ptr");
                    }
                }
            }

            LLVMValueRef base_ptr  = NULL;
            LLVMTypeRef  base_type = NULL;
            const char  *struct_name = NULL;

            if (tgt->kind == NODE_IDENT) {
                e = scope_get(g, tgt->as.ident.name);
                if (e && e->is_alloca) {
                    base_ptr  = e->value;
                    base_type = e->type;
                }
            }

            if (!base_ptr) {
                // Handle *ptr — dereference to get struct value
                if (tgt->kind == NODE_DEREF && tgt->as.unary.operand->kind == NODE_IDENT) {
                    const char *pname = tgt->as.unary.operand->as.ident.name;
                    LLVMEntry *pe = scope_get(g, pname);
                    if (pe && pe->elem_type) {
                        // Load the pointer value, use elem_type as base_type
                        LLVMValueRef ptr_val = LLVMBuildLoad2(g->builder,
                            pe->type, pe->value, "ptr.load");
                        base_type = pe->elem_type;
                        base_ptr  = LLVMBuildAlloca(g->builder, base_type, "deref.tmp");
                        LLVMValueRef struct_val = LLVMBuildLoad2(g->builder,
                            base_type, ptr_val, "struct.load");
                        LLVMBuildStore(g->builder, struct_val, base_ptr);
                    }
                }
                if (!base_ptr) {
                    LLVMValueRef val = emit_expr(g, tgt);
                    if (!val) return NULL;
                    base_type = LLVMTypeOf(val);
                    base_ptr  = LLVMBuildAlloca(g->builder, base_type, "tmp.field");
                    LLVMBuildStore(g->builder, val, base_ptr);
                }
            }

            // Determine struct name from LLVM type
            if (base_type) {
                // If it's a pointer, dereference
                if (LLVMGetTypeKind(base_type) == LLVMPointerTypeKind) {
                    // LLVM 18 opaque pointers — get elem type from scope
                    if (e && e->elem_type)
                        base_type = e->elem_type;
                    else
                        base_type = LLVMInt32TypeInContext(g->ctx); // fallback
                    base_ptr = LLVMBuildLoad2(g->builder,
                        LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0),
                        base_ptr, "deref.ptr");
                }
                if (LLVMGetTypeKind(base_type) == LLVMStructTypeKind)
                    struct_name = LLVMGetStructName(base_type);
            }

            // Built-in fields on _ci_str (anonymous struct, no name)
            if (!struct_name) {
                // _ci_str: { ptr=0, len=1 }
                unsigned field_idx = 0;
                LLVMTypeRef field_type = LLVMInt64TypeInContext(g->ctx);
                if (strcmp(node->as.field.field, "ptr") == 0) {
                    field_idx  = 0;
                    field_type = LLVMPointerType(
                        LLVMInt8TypeInContext(g->ctx), 0);
                } else if (strcmp(node->as.field.field, "len") == 0) {
                    field_idx  = 1;
                    field_type = LLVMInt64TypeInContext(g->ctx);
                } else {
                    llvm_error(g, node, "unknown field '%s'",
                               node->as.field.field);
                    return NULL;
                }
                LLVMValueRef gep = LLVMBuildStructGEP2(
                    g->builder, base_type, base_ptr, field_idx, "field.ptr");
                return LLVMBuildLoad2(g->builder, field_type, gep, "field");
            }

            // Named struct field — check if it's a union first
            Symbol *type_sym = scope_lookup(g->resolver->global, struct_name);
            if (type_sym && type_sym->decl &&
                type_sym->decl->kind == NODE_UNION_DECL) {
                // Union: find the field type, bitcast byte array ptr to field ptr
                AstNode *udecl = type_sym->decl;
                AstType *ftype = NULL;
                for (size_t fi = 0; fi < udecl->as.struct_decl.field_count; fi++) {
                    if (strcmp(udecl->as.struct_decl.fields[fi].name,
                               node->as.field.field) == 0) {
                        ftype = udecl->as.struct_decl.fields[fi].type;
                        break;
                    }
                }
                if (!ftype) {
                    llvm_error(g, node, "union '%s' has no field '%s'",
                               struct_name, node->as.field.field);
                    return NULL;
                }
                LLVMTypeRef ft = ci_type_to_llvm(g, ftype);
                // GEP to get ptr to byte array element 0, then load as field type
                LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0);
                LLVMValueRef indices[2] = { zero, zero };
                LLVMValueRef raw_ptr = LLVMBuildGEP2(g->builder, base_type,
                    base_ptr, indices, 2, "union.raw");
                return LLVMBuildLoad2(g->builder, ft, raw_ptr, node->as.field.field);
            }

            int idx = get_field_index(g, struct_name, node->as.field.field);
            if (idx < 0) {
                llvm_error(g, node, "struct '%s' has no field '%s'",
                           struct_name, node->as.field.field);
                return NULL;
            }

            LLVMTypeRef field_type = LLVMStructGetTypeAtIndex(
                base_type, (unsigned)idx);
            LLVMValueRef gep = LLVMBuildStructGEP2(
                g->builder, base_type, base_ptr, (unsigned)idx,
                node->as.field.field);
            return LLVMBuildLoad2(g->builder, field_type, gep,
                                   node->as.field.field);
        }

        case NODE_INDEX: {
            // Range index = slice: arr[0..5] → { ptr, len }
            if (node->as.index.index && node->as.index.index->kind == NODE_RANGE) {
                AstNode *range = node->as.index.index;
                LLVMValueRef from = emit_expr(g, range->as.range.from);
                LLVMValueRef to   = emit_expr(g, range->as.range.to);
                if (!from || !to) return NULL;
                // Get base pointer
                LLVMValueRef base = NULL;
                LLVMTypeRef  elem_t = LLVMInt32TypeInContext(g->ctx);
                AstNode *tgt_node = node->as.index.target;
                if (tgt_node->kind == NODE_IDENT) {
                    LLVMEntry *e = scope_get(g, tgt_node->as.ident.name);
                    if (e && e->is_alloca) {
                        if (LLVMGetTypeKind(e->type) == LLVMArrayTypeKind) {
                            // Array: GEP directly
                            elem_t = LLVMGetElementType(e->type);
                            LLVMTypeRef i32t = LLVMInt32TypeInContext(g->ctx);
                            LLVMValueRef zero = LLVMConstInt(i32t, 0, 0);
                            LLVMValueRef indices[2] = { zero, from };
                            base = LLVMBuildGEP2(g->builder, e->type, e->value, indices, 2, "slice.base");
                        } else if (LLVMGetTypeKind(e->type) == LLVMStructTypeKind
                                   && LLVMCountStructElementTypes(e->type) == 2) {
                            // Slice: extract ptr field, then GEP by from
                            LLVMValueRef sv = LLVMBuildLoad2(g->builder, e->type, e->value, "sv");
                            LLVMValueRef ptr = LLVMBuildExtractValue(g->builder, sv, 0, "sv.ptr");
                            if (e->elem_type) elem_t = e->elem_type;
                            base = LLVMBuildGEP2(g->builder, elem_t, ptr, &from, 1, "slice.base");
                        }
                    }
                }
                if (!base) {
                    LLVMValueRef tgt = emit_expr(g, tgt_node);
                    base = LLVMBuildGEP2(g->builder, elem_t, tgt, &from, 1, "slice.base");
                }
                // Compute len = to - from
                LLVMValueRef len = LLVMBuildSub(g->builder, to, from, "slice.len");
                // Build slice struct { ptr, i64 }
                LLVMTypeRef  i64t     = LLVMInt64TypeInContext(g->ctx);
                LLVMTypeRef  ptr_t    = LLVMPointerType(elem_t, 0);
                LLVMTypeRef  slice_t  = LLVMStructTypeInContext(g->ctx,
                    (LLVMTypeRef[]){ptr_t, i64t}, 2, 0);
                LLVMValueRef slice    = LLVMGetUndef(slice_t);
                LLVMValueRef len64    = LLVMBuildSExt(g->builder, len, i64t, "len64");
                slice = LLVMBuildInsertValue(g->builder, slice, base,  0, "slice.ptr");
                slice = LLVMBuildInsertValue(g->builder, slice, len64, 1, "slice.len");
                return slice;
            }
            // Regular index
            LLVMValueRef idx = emit_expr(g, node->as.index.index);
            if (!idx) return NULL;
            LLVMTypeRef elem = LLVMInt32TypeInContext(g->ctx);
            LLVMValueRef base_ptr = NULL;
            // Check if target is a slice {ptr, i64}
            if (node->as.index.target->kind == NODE_IDENT) {
                LLVMEntry *e = scope_get(g, node->as.index.target->as.ident.name);
                if (e && e->is_alloca && LLVMGetTypeKind(e->type) == LLVMStructTypeKind
                        && LLVMCountStructElementTypes(e->type) == 2) {
                    // It's a slice — extract ptr field
                    LLVMValueRef slice_val = LLVMBuildLoad2(g->builder, e->type, e->value, "slice");
                    base_ptr = LLVMBuildExtractValue(g->builder, slice_val, 0, "slice.ptr");
                    // elem type: get from ptr element type or use i32
                    LLVMTypeRef ptr_elem = LLVMStructGetTypeAtIndex(e->type, 0);
                    if (LLVMGetTypeKind(ptr_elem) == LLVMPointerTypeKind && e->elem_type)
                        elem = e->elem_type;
                    // Truncate idx to i32 if needed
                    LLVMValueRef gep = LLVMBuildGEP2(g->builder, elem, base_ptr, &idx, 1, "sidx");
                    return LLVMBuildLoad2(g->builder, elem, gep, "selem");
                }
                if (e) {
                    if (LLVMGetTypeKind(e->type) == LLVMArrayTypeKind)
                        elem = LLVMGetElementType(e->type);
                    else if (e->elem_type) elem = e->elem_type;
                    else elem = e->type;
                }
            }
            LLVMValueRef tgt = emit_expr(g, node->as.index.target);
            if (!tgt) return NULL;
            LLVMValueRef gep = LLVMBuildGEP2(g->builder, elem, tgt, &idx, 1, "idx");
            return LLVMBuildLoad2(g->builder, elem, gep, "elem");
        }

        case NODE_CAST: {
            LLVMValueRef val  = emit_expr(g, node->as.cast.expr);
            LLVMTypeRef  dest = ci_type_to_llvm(g, node->as.cast.type);
            if (!val) return NULL;

            LLVMTypeRef src     = LLVMTypeOf(val);
            LLVMTypeKind srck   = LLVMGetTypeKind(src);
            LLVMTypeKind destk  = LLVMGetTypeKind(dest);

            if (srck == LLVMIntegerTypeKind && destk == LLVMIntegerTypeKind) {
                unsigned sw = LLVMGetIntTypeWidth(src);
                unsigned dw = LLVMGetIntTypeWidth(dest);
                if (sw < dw) return LLVMBuildSExt (g->builder, val, dest, "sext");
                if (sw > dw) return LLVMBuildTrunc(g->builder, val, dest, "trunc");
                return val;
            }
            if (srck == LLVMIntegerTypeKind &&
                (destk == LLVMFloatTypeKind || destk == LLVMDoubleTypeKind))
                return LLVMBuildSIToFP(g->builder, val, dest, "sitofp");
            if ((srck == LLVMFloatTypeKind || srck == LLVMDoubleTypeKind) &&
                destk == LLVMIntegerTypeKind)
                return LLVMBuildFPToSI(g->builder, val, dest, "fptosi");
            if (srck == LLVMPointerTypeKind && destk == LLVMPointerTypeKind)
                return LLVMBuildBitCast(g->builder, val, dest, "pcast");
            if (srck == LLVMIntegerTypeKind && destk == LLVMPointerTypeKind)
                return LLVMBuildIntToPtr(g->builder, val, dest, "cast");
            if (srck == LLVMPointerTypeKind && destk == LLVMIntegerTypeKind)
                return LLVMBuildPtrToInt(g->builder, val, dest, "cast");
            return LLVMBuildBitCast(g->builder, val, dest, "cast");
        }

        case NODE_SIZEOF: {
            LLVMTypeRef t = ci_type_to_llvm(g, node->as.size_expr.type);
            return LLVMSizeOf(t);
        }

        case NODE_ALIGNOF: {
            LLVMTypeRef t = ci_type_to_llvm(g, node->as.size_expr.type);
            return LLVMAlignOf(t);
        }

        case NODE_IF_EXPR: {
            LLVMValueRef cond = emit_expr(g, node->as.if_stmt.condition);
            if (!cond) return NULL;

            // Convert condition to i1
            LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(cond), 0, 0);
            LLVMValueRef bool_cond = LLVMBuildICmp(g->builder,
                LLVMIntNE, cond, zero, "ifexpr.cond");

            int lbl = new_label(g);
            char then_name[32], else_name[32], merge_name[32];
            snprintf(then_name,  sizeof(then_name),  "ifexpr.then.%d", lbl);
            snprintf(else_name,  sizeof(else_name),  "ifexpr.else.%d", lbl);
            snprintf(merge_name, sizeof(merge_name), "ifexpr.merge.%d", lbl);

            LLVMBasicBlockRef then_bb  = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, then_name);
            LLVMBasicBlockRef else_bb  = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, else_name);
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, merge_name);

            LLVMBuildCondBr(g->builder, bool_cond, then_bb, else_bb);

            // Then
            LLVMPositionBuilderAtEnd(g->builder, then_bb);
            LLVMValueRef then_val = NULL;
            if (node->as.if_stmt.then_block &&
                node->as.if_stmt.then_block->kind == NODE_BLOCK &&
                node->as.if_stmt.then_block->as.block.count == 1) {
                AstNode *s = node->as.if_stmt.then_block->as.block.stmts[0];
                then_val = (s->kind == NODE_EXPR_STMT)
                    ? emit_expr(g, s->as.expr_stmt.expr)
                    : emit_expr(g, s);
            }
            LLVMBuildBr(g->builder, merge_bb);
            LLVMBasicBlockRef then_exit = LLVMGetInsertBlock(g->builder);

            // Else
            LLVMPositionBuilderAtEnd(g->builder, else_bb);
            LLVMValueRef else_val = NULL;
            if (node->as.if_stmt.else_block &&
                node->as.if_stmt.else_block->kind == NODE_BLOCK &&
                node->as.if_stmt.else_block->as.block.count == 1) {
                AstNode *s = node->as.if_stmt.else_block->as.block.stmts[0];
                else_val = (s->kind == NODE_EXPR_STMT)
                    ? emit_expr(g, s->as.expr_stmt.expr)
                    : emit_expr(g, s);
            }
            LLVMBuildBr(g->builder, merge_bb);
            LLVMBasicBlockRef else_exit = LLVMGetInsertBlock(g->builder);

            // Merge + phi
            LLVMPositionBuilderAtEnd(g->builder, merge_bb);
            if (then_val && else_val) {
                LLVMValueRef phi = LLVMBuildPhi(g->builder,
                    LLVMTypeOf(then_val), "ifexpr");
                LLVMValueRef in_vals[2]  = { then_val, else_val };
                LLVMBasicBlockRef in_bbs[2] = { then_exit, else_exit };
                LLVMAddIncoming(phi, in_vals, in_bbs, 2);
                return phi;
            }
            return then_val;
        }

        case NODE_NEW: {
            // new T { fields } → malloc(sizeof(T)) + init
            LLVMTypeRef t = LLVMGetTypeByName2(g->ctx, node->as.new_expr.type_name);
            if (!t) {
                llvm_error(g, node, "unknown type '%s' in new",
                           node->as.new_expr.type_name);
                return NULL;
            }
            LLVMValueRef size = LLVMSizeOf(t);
            LLVMTypeRef i8_ptr_ty = LLVMPointerType(
                LLVMInt8TypeInContext(g->ctx), 0);
            LLVMValueRef malloc_fn = LLVMGetNamedFunction(g->module, "malloc");
            if (!malloc_fn) {
                LLVMTypeRef malloc_ty = LLVMFunctionType(
                    i8_ptr_ty,
                    (LLVMTypeRef[]){ LLVMInt64TypeInContext(g->ctx) }, 1, 0);
                malloc_fn = LLVMAddFunction(g->module, "malloc", malloc_ty);
            }
            LLVMTypeRef malloc_ty = LLVMGlobalGetValueType(malloc_fn);
            LLVMValueRef raw = LLVMBuildCall2(g->builder, malloc_ty,
                malloc_fn, &size, 1, "new.raw");
            return LLVMBuildBitCast(g->builder, raw,
                LLVMPointerType(t, 0), "new.ptr");
        }

        case NODE_STRUCT_LITERAL: {
            if (!node->as.struct_lit.type_name) {
                llvm_error(g, node, "cannot emit inferred struct literal");
                return NULL;
            }
            LLVMTypeRef stype = get_struct_llvm_type(
                g, node->as.struct_lit.type_name);
            if (!stype) return NULL;
            // Union literal: allocate zeroed storage, store first field
            Symbol *ltsym = scope_lookup(g->resolver->global,
                                          node->as.struct_lit.type_name);
            if (ltsym && ltsym->decl && ltsym->decl->kind == NODE_UNION_DECL) {
                LLVMValueRef tmp = LLVMBuildAlloca(g->builder, stype, "union.lit");
                // zero it
                LLVMValueRef zero = LLVMConstNull(stype);
                LLVMBuildStore(g->builder, zero, tmp);
                // store each provided field via bitcast
                AstNode *udecl = ltsym->decl;
                for (size_t i = 0; i < node->as.struct_lit.field_count; i++) {
                    AstNode *fname = node->as.struct_lit.field_names[i];
                    AstType *ftype = NULL;
                    for (size_t fi = 0; fi < udecl->as.struct_decl.field_count; fi++) {
                        if (strcmp(udecl->as.struct_decl.fields[fi].name,
                                   fname->as.ident.name) == 0) {
                            ftype = udecl->as.struct_decl.fields[fi].type;
                            break;
                        }
                    }
                    if (!ftype) continue;
                    LLVMValueRef fval = emit_expr(g, node->as.struct_lit.field_values[i]);
                    if (!fval) continue;
                    LLVMTypeRef ft = ci_type_to_llvm(g, ftype);
                    LLVMValueRef zero2 = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0);
                    LLVMValueRef indices[2] = { zero2, zero2 };
                    LLVMValueRef fptr = LLVMBuildGEP2(g->builder, stype,
                        tmp, indices, 2, "union.fptr");
                    // coerce if needed
                    LLVMTypeRef at = LLVMTypeOf(fval);
                    if (LLVMGetTypeKind(ft) == LLVMIntegerTypeKind &&
                        LLVMGetTypeKind(at) == LLVMIntegerTypeKind) {
                        unsigned ew = LLVMGetIntTypeWidth(ft);
                        unsigned aw = LLVMGetIntTypeWidth(at);
                        if (ew > aw) fval = LLVMBuildSExt(g->builder, fval, ft, "sext");
                        else if (ew < aw) fval = LLVMBuildTrunc(g->builder, fval, ft, "trunc");
                    }
                    LLVMBuildStore(g->builder, fval, fptr);
                }
                return LLVMBuildLoad2(g->builder, stype, tmp, "union.val");
            }
            // Start with undef and insert each field
            LLVMValueRef val = LLVMGetUndef(stype);

            // Build a map of field name → value for this literal
            for (size_t i = 0; i < node->as.struct_lit.field_count; i++) {
                AstNode *fname = node->as.struct_lit.field_names[i];
                int idx = get_field_index(g, node->as.struct_lit.type_name,
                                           fname->as.ident.name);
                if (idx < 0) {
                    llvm_error(g, node, "struct '%s' has no field '%s'",
                               node->as.struct_lit.type_name,
                               fname->as.ident.name);
                    continue;
                }
                LLVMValueRef fval = emit_expr(g,
                    node->as.struct_lit.field_values[i]);
                if (!fval) continue;
                // Coerce fval to match the expected field type
                LLVMTypeRef expected = LLVMStructGetTypeAtIndex(stype, (unsigned)idx);
                LLVMTypeRef actual   = LLVMTypeOf(fval);
                LLVMTypeKind ek = LLVMGetTypeKind(expected);
                LLVMTypeKind ak = LLVMGetTypeKind(actual);
                if (ek == LLVMIntegerTypeKind && ak == LLVMIntegerTypeKind) {
                    unsigned ew = LLVMGetIntTypeWidth(expected);
                    unsigned aw = LLVMGetIntTypeWidth(actual);
                    if (ew > aw)
                        fval = LLVMBuildSExt(g->builder, fval, expected, "sext");
                    else if (ew < aw)
                        fval = LLVMBuildTrunc(g->builder, fval, expected, "trunc");
                } else if (ek == LLVMDoubleTypeKind && ak == LLVMFloatTypeKind) {
                    fval = LLVMBuildFPExt(g->builder, fval, expected, "fpext");
                } else if (ek == LLVMFloatTypeKind && ak == LLVMDoubleTypeKind) {
                    fval = LLVMBuildFPTrunc(g->builder, fval, expected, "fptrunc");
                } else if (ek == LLVMPointerTypeKind && ak == LLVMIntegerTypeKind) {
                    fval = LLVMBuildIntToPtr(g->builder, fval, expected, "itoptr");
                }
                // InsertValue builds a new aggregate with the field set
                val = LLVMBuildInsertValue(g->builder, val, fval,
                                           (unsigned)idx, "slit");
            }
            return val;
        }

        case NODE_ENUM_LITERAL: {
            const char *ename   = node->as.enum_lit.enum_name;
            const char *vname   = node->as.enum_lit.variant;
            LLVMTypeRef etype   = LLVMGetTypeByName2(g->ctx, ename);
            if (!etype) { llvm_error(g, node, "unknown enum '%s'", ename); return NULL; }

            // Find variant index + fields from resolver
            Symbol *esym = scope_lookup(g->resolver->global, ename);
            if (!esym || !esym->decl) { llvm_error(g, node, "enum '%s' not found", ename); return NULL; }
            AstNode *edecl = esym->decl;

            int tag = -1;
            AstVariant *matched_var = NULL;
            for (size_t i = 0; i < edecl->as.enum_decl.variant_count; i++) {
                if (strcmp(edecl->as.enum_decl.variants[i].name, vname) == 0) {
                    tag = (int)i;
                    matched_var = &edecl->as.enum_decl.variants[i];
                    break;
                }
            }
            if (tag < 0) { llvm_error(g, node, "unknown variant '%s::%s'", ename, vname); return NULL; }

            // Alloca enum storage
            LLVMBasicBlockRef cur   = LLVMGetInsertBlock(g->builder);
            LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(g->current_func);
            LLVMValueRef first_inst = LLVMGetFirstInstruction(entry);
            if (first_inst) LLVMPositionBuilderBefore(g->builder, first_inst);
            else            LLVMPositionBuilderAtEnd(g->builder, entry);
            LLVMValueRef alloca = LLVMBuildAlloca(g->builder, etype, "enum.tmp");
            LLVMPositionBuilderAtEnd(g->builder, cur);

            LLVMTypeRef i32 = LLVMInt32TypeInContext(g->ctx);
            //LLVMTypeRef i64 = LLVMInt64TypeInContext(g->ctx);

            // Store tag
            LLVMValueRef tag_ptr = LLVMBuildStructGEP2(g->builder, etype, alloca, 0, "tag.ptr");
            LLVMBuildStore(g->builder, LLVMConstInt(i32, (unsigned long long)tag, 0), tag_ptr);

            // Store payload fields
            if (matched_var && matched_var->field_count > 0) {
                LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
                LLVMValueRef one  = LLVMConstInt(i32, 1, 0);
                LLVMTypeRef arr_type = LLVMStructGetTypeAtIndex(etype, 1);
                LLVMValueRef arr_ptr = LLVMBuildStructGEP2(g->builder, etype, alloca, 1, "pay.ptr");

                unsigned slot = 0;
                for (size_t fi = 0; fi < matched_var->field_count && fi < node->as.enum_lit.arg_count; fi++) {
                    LLVMValueRef arg = emit_expr(g, node->as.enum_lit.args[fi]);
                    if (!arg) continue;
                    LLVMValueRef slot_idx = LLVMConstInt(i32, slot, 0);
                    LLVMValueRef indices[2] = { zero, slot_idx };
                    LLVMValueRef slot_ptr = LLVMBuildGEP2(g->builder, arr_type, arr_ptr, indices, 2, "slot");
                    // Bitcast slot to field type pointer
                    LLVMTypeRef ftype = LLVMTypeOf(arg);
                    LLVMValueRef typed_ptr = LLVMBuildBitCast(g->builder, slot_ptr, LLVMPointerType(ftype, 0), "fptr");
                    LLVMBuildStore(g->builder, arg, typed_ptr);
                    slot += type_word_count(matched_var->fields[fi].type);
                }
                (void)one;
            }

            return LLVMBuildLoad2(g->builder, etype, alloca, "enum.val");
        }

        case NODE_ARRAY_LITERAL: {
            size_t count = node->as.array_lit.count;
            if (count == 0) return NULL;
            LLVMValueRef first = emit_expr(g, node->as.array_lit.elems[0]);
            if (!first) return NULL;
            LLVMTypeRef elem_type = LLVMTypeOf(first);
            LLVMTypeRef arr_type  = LLVMArrayType(elem_type, (unsigned)count);

            // Build array as aggregate value using insertvalue
            LLVMValueRef agg = LLVMGetUndef(arr_type);
            agg = LLVMBuildInsertValue(g->builder, agg, first, 0, "arr.0");
            for (size_t i = 1; i < count; i++) {
                LLVMValueRef v = emit_expr(g, node->as.array_lit.elems[i]);
                if (!v) continue;
                agg = LLVMBuildInsertValue(g->builder, agg, v,
                                           (unsigned)i, "arr.elem");
            }
            return agg;
        }       

        default:
            llvm_error(g, node, "unhandled expression kind %d", node->kind);
            return LLVMConstInt(LLVMInt32TypeInContext(g->ctx), 0, 0);
    }
}

// ─── STATEMENT EMISSION ──────────────────────────────────────────────────────

static void emit_defers(LLVMCodegen *g) {
    for (int i = (int)g->defer_count - 1; i >= 0; i--)
        emit_stmt(g, g->defer_stack[i]);
}

static void emit_stmt(LLVMCodegen *g, AstNode *node) {
    if (!node) return;
    // Don't emit into a block that already has a terminator
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
        return;

    switch (node->kind) {

        case NODE_BLOCK: {
            scope_push(g);
            for (size_t i = 0; i < node->as.block.count; i++)
                emit_stmt(g, node->as.block.stmts[i]);
            scope_pop(g);
            break;
        }

        case NODE_VAR_DECL: {
            LLVMTypeRef type = node->as.var_decl.type
                ? ci_type_to_llvm(g, node->as.var_decl.type)
                : LLVMInt32TypeInContext(g->ctx);  // inferred fallback

            // If we have an initializer, infer the type from it
            LLVMValueRef init_val = NULL;
            if (node->as.var_decl.value) {
                init_val = emit_expr(g, node->as.var_decl.value);
                if (init_val) {
                    // Use actual type if: no declared type, OR declared type is generic (ptr fallback)
                    int is_generic = node->as.var_decl.type
                        && node->as.var_decl.type->kind == TY_NAMED
                        && node->as.var_decl.type->name
                        && isupper((unsigned char)node->as.var_decl.type->name[0]);
                    if (!node->as.var_decl.type || is_generic)
                        type = LLVMTypeOf(init_val);
                }
            }

            // Alloca in entry block for proper SSA
            LLVMBasicBlockRef cur    = LLVMGetInsertBlock(g->builder);
            LLVMBasicBlockRef entry  = LLVMGetEntryBasicBlock(g->current_func);
            LLVMValueRef first_inst  = LLVMGetFirstInstruction(entry);

            if (first_inst)
                LLVMPositionBuilderBefore(g->builder, first_inst);
            else
                LLVMPositionBuilderAtEnd(g->builder, entry);

            LLVMValueRef alloca = LLVMBuildAlloca(g->builder, type,
                                                   node->as.var_decl.name);
            LLVMPositionBuilderAtEnd(g->builder, cur);

            if (init_val)
                LLVMBuildStore(g->builder, init_val, alloca);

            scope_set(g, node->as.var_decl.name, alloca, type, 1);

            // Track elem_type for slice variables
            if (node->as.var_decl.type && node->as.var_decl.type->kind == TY_SLICE
                    && node->as.var_decl.type->inner) {
                LLVMEntry *ent = scope_get(g, node->as.var_decl.name);
                if (ent) ent->elem_type = ci_type_to_llvm(g, node->as.var_decl.type->inner);
            }
            
            // Track elem_type for pointer variables
            if (node->as.var_decl.type &&
                (node->as.var_decl.type->kind == TY_POINTER ||
                 node->as.var_decl.type->kind == TY_NULLABLE_PTR) &&
                node->as.var_decl.type->inner) {
                LLVMEntry *ent = scope_get(g, node->as.var_decl.name);
                if (ent) ent->elem_type = ci_type_to_llvm(g, node->as.var_decl.type->inner);
            }
            // Track elem_type for array variables (for &arr[0] indexing)
            if (node->as.var_decl.type && node->as.var_decl.type->kind == TY_ARRAY) {
                LLVMEntry *ent = scope_get(g, node->as.var_decl.name);
                if (ent && node->as.var_decl.type->elem_type)
                    ent->elem_type = ci_type_to_llvm(g, node->as.var_decl.type->elem_type);
            }
            break;
        }

        case NODE_RETURN: {
            emit_defers(g);
            if (node->as.ret.value) {
                LLVMValueRef val = emit_expr(g, node->as.ret.value);
                if (!val) { LLVMBuildRetVoid(g->builder); break; }

                // Coerce return value to match declared function return type
                LLVMTypeRef expected = LLVMGetReturnType(
                    LLVMGlobalGetValueType(g->current_func));
                LLVMTypeRef actual = LLVMTypeOf(val);

                if (actual != expected) {
                    LLVMTypeKind ek = LLVMGetTypeKind(expected);
                    LLVMTypeKind ak = LLVMGetTypeKind(actual);

                    // i1 → i32/i64 (bool result used as int return)
                    if (ak == LLVMIntegerTypeKind && ek == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(actual) < LLVMGetIntTypeWidth(expected))
                        val = LLVMBuildZExt(g->builder, val, expected, "zext");
                    // float → double
                    else if (ak == LLVMFloatTypeKind &&
                             ek == LLVMDoubleTypeKind)
                        val = LLVMBuildFPExt(g->builder, val, expected, "fpext");
                    // double → float
                    else if (ak == LLVMDoubleTypeKind &&
                             ek == LLVMFloatTypeKind)
                        val = LLVMBuildFPTrunc(g->builder, val, expected, "fptrunc");
                }

                LLVMBuildRet(g->builder, val);
            } else {
                LLVMBuildRetVoid(g->builder);
            }
            break;
        }

        case NODE_IF: {
            int lbl = new_label(g);
            char merge_n[32];
            snprintf(merge_n, sizeof(merge_n), "if.merge.%d", lbl);
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, merge_n);

            // Build all condition/then/else blocks upfront
            // We chain: if -> else_if[0] -> else_if[1] -> ... -> else -> merge
            size_t eic = node->as.if_stmt.else_if_count;

            // Emit the initial if
            {
                LLVMValueRef cond = emit_expr(g, node->as.if_stmt.condition);
                if (!cond) break;
                LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(cond), 0, 0);
                LLVMValueRef bv = LLVMBuildICmp(g->builder, LLVMIntNE, cond, zero, "if.cond");
                char then_n[32], next_n[32];
                snprintf(then_n, sizeof(then_n), "if.then.%d", lbl);
                snprintf(next_n, sizeof(next_n), "if.next.%d.0", lbl);
                LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(g->ctx, g->current_func, then_n);
                LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(g->ctx, g->current_func, next_n);
                LLVMBuildCondBr(g->builder, bv, then_bb,
                    (eic > 0 || node->as.if_stmt.else_block) ? next_bb : merge_bb);
                LLVMPositionBuilderAtEnd(g->builder, then_bb);
                emit_stmt(g, node->as.if_stmt.then_block);
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                    LLVMBuildBr(g->builder, merge_bb);
                LLVMPositionBuilderAtEnd(g->builder, next_bb);
            }

            // Emit else_if chain
            for (size_t ei = 0; ei < eic; ei++) {
                LLVMValueRef cond = emit_expr(g, node->as.if_stmt.else_if_conds[ei]);
                if (!cond) break;
                LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(cond), 0, 0);
                LLVMValueRef bv = LLVMBuildICmp(g->builder, LLVMIntNE, cond, zero, "elif.cond");
                char then_n[32], next_n[64];
                snprintf(then_n, sizeof(then_n), "elif.then.%d.%zu", lbl, ei);
                snprintf(next_n, sizeof(next_n), "elif.next.%d.%zu", lbl, ei + 1);
                LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(g->ctx, g->current_func, then_n);
                LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(g->ctx, g->current_func, next_n);
                int has_more = (ei + 1 < eic) || (node->as.if_stmt.else_block != NULL);
                LLVMBuildCondBr(g->builder, bv, then_bb, has_more ? next_bb : merge_bb);
                LLVMPositionBuilderAtEnd(g->builder, then_bb);
                emit_stmt(g, node->as.if_stmt.else_if_blocks[ei]);
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                    LLVMBuildBr(g->builder, merge_bb);
                LLVMPositionBuilderAtEnd(g->builder, next_bb);
            }

            // Emit final else
            if (node->as.if_stmt.else_block) {
                emit_stmt(g, node->as.if_stmt.else_block);
            }
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                LLVMBuildBr(g->builder, merge_bb);
            LLVMPositionBuilderAtEnd(g->builder, merge_bb);
            break;
        }

        case NODE_WHILE: {
            int lbl = new_label(g);
            char cond_n[32], body_n[32], exit_n[32];
            snprintf(cond_n, sizeof(cond_n), "while.cond.%d", lbl);
            snprintf(body_n, sizeof(body_n), "while.body.%d", lbl);
            snprintf(exit_n, sizeof(exit_n), "while.exit.%d", lbl);

            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, cond_n);
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, body_n);
            LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, exit_n);

            LLVMBuildBr(g->builder, cond_bb);
            LLVMPositionBuilderAtEnd(g->builder, cond_bb);
            LLVMValueRef cond = emit_expr(g, node->as.while_stmt.condition);
            LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(cond), 0, 0);
            LLVMValueRef bool_val = LLVMBuildICmp(g->builder,
                LLVMIntNE, cond, zero, "while.cond");
            LLVMBuildCondBr(g->builder, bool_val, body_bb, exit_bb);

            LLVMBasicBlockRef saved_break    = g->break_block;
            LLVMBasicBlockRef saved_continue = g->continue_block;
            g->break_block    = exit_bb;
            g->continue_block = cond_bb;

            LLVMPositionBuilderAtEnd(g->builder, body_bb);
            emit_stmt(g, node->as.while_stmt.body);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                LLVMBuildBr(g->builder, cond_bb);

            g->break_block    = saved_break;
            g->continue_block = saved_continue;
            LLVMPositionBuilderAtEnd(g->builder, exit_bb);
            break;
        }


        case NODE_LOOP: {
            int lbl = new_label(g);
            char body_n[32], exit_n[32];
            snprintf(body_n, sizeof(body_n), "loop.body.%d", lbl);
            snprintf(exit_n, sizeof(exit_n), "loop.exit.%d", lbl);

            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, body_n);
            LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, exit_n);

            LLVMBuildBr(g->builder, body_bb);

            LLVMBasicBlockRef saved_break    = g->break_block;
            LLVMBasicBlockRef saved_continue = g->continue_block;
            g->break_block    = exit_bb;
            g->continue_block = body_bb;

            LLVMPositionBuilderAtEnd(g->builder, body_bb);
            emit_stmt(g, node->as.loop_stmt.body);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                LLVMBuildBr(g->builder, body_bb);

            g->break_block    = saved_break;
            g->continue_block = saved_continue;
            LLVMPositionBuilderAtEnd(g->builder, exit_bb);
            break;
        }

        case NODE_BREAK:
            emit_defers(g);
            if (g->break_block)
                LLVMBuildBr(g->builder, g->break_block);
            break;

        case NODE_CONTINUE:
            if (g->continue_block)
                LLVMBuildBr(g->builder, g->continue_block);
            break;

        case NODE_DEFER:
            if (g->defer_count >= g->defer_cap) {
                g->defer_cap *= 2;
                g->defer_stack = realloc(g->defer_stack,
                    g->defer_cap * sizeof(AstNode *));
            }
            g->defer_stack[g->defer_count++] = node->as.defer_stmt.stmt;
            break;

        case NODE_PANIC: {
            LLVMValueRef msg = emit_expr(g, node->as.panic_stmt.msg);
            LLVMValueRef panic_fn = LLVMGetNamedFunction(g->module, "_ci_panic");
            if (panic_fn && msg) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(panic_fn);
                LLVMBuildCall2(g->builder, fty, panic_fn, &msg, 1, "");
            }
            LLVMBuildUnreachable(g->builder);
            break;
        }

        case NODE_EXPR_STMT:
            emit_expr(g, node->as.expr_stmt.expr);
            break;

        case NODE_DELETE: {
            LLVMValueRef ptr  = emit_expr(g, node->as.delete_expr.ptr);
            LLVMValueRef free_fn = LLVMGetNamedFunction(g->module, "free");
            if (free_fn && ptr) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(free_fn);
                // Cast to i8* for free
                LLVMValueRef cast = LLVMBuildBitCast(g->builder, ptr,
                    LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0), "free.cast");
                LLVMBuildCall2(g->builder, fty, free_fn, &cast, 1, "");
            }
            break;
        }

        case NODE_ASM_BLOCK:
            // Inline asm in LLVM IR via LLVMGetInlineAsm
            // For now emit a comment — full inline asm is in the asm function emitter
            fprintf(stderr, "[%s] warning: inline asm blocks in regular "
                            "functions not yet supported in LLVM backend\n",
                    g->filename);
            break;

        case NODE_MATCH: {
            // Emit as if-else chain
            LLVMValueRef subject = emit_expr(g, node->as.match_stmt.subject);
            if (!subject) break;

            int lbl = new_label(g);
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
                g->ctx, g->current_func, "match.merge");

            for (size_t i = 0; i < node->as.match_stmt.arm_count; i++) {
                MatchArm *arm = &node->as.match_stmt.arms[i];
                int is_wildcard = arm->pattern &&
                                  arm->pattern->kind == NODE_IDENT &&
                                  strcmp(arm->pattern->as.ident.name, "_") == 0;

                if (is_wildcard) {
                    // Last arm — just emit body and jump to merge
                    scope_push(g);
                    emit_stmt(g, arm->body);
                    scope_pop(g);
                    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                        LLVMBuildBr(g->builder, merge_bb);
                    break;
                }

                char arm_then[64], arm_next[32];
                snprintf(arm_then, sizeof(arm_then), "match.arm.%d.%zu", lbl, i);
                snprintf(arm_next, sizeof(arm_next), "match.next.%d.%zu", lbl, i);

                LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(
                    g->ctx, g->current_func, arm_then);
                LLVMBasicBlockRef next_bb = LLVMAppendBasicBlockInContext(
                    g->ctx, g->current_func, arm_next);

                // Pattern comparison
                LLVMValueRef cmp;
                AstVariant *enum_matched_var = NULL;
                LLVMValueRef enum_subject_alloca = NULL;
                LLVMTypeRef  enum_etype = NULL;

                if (arm->pattern->kind == NODE_ENUM_PATTERN) {
                    const char *ename = arm->pattern->as.enum_pattern.enum_name;
                    const char *vname = arm->pattern->as.enum_pattern.variant;
                    // Built-in Result patterns: Ok / Err
                    int is_builtin_result = (strcmp(vname, "Ok") == 0 || strcmp(vname, "Err") == 0)
                                         && LLVMGetTypeByName2(g->ctx, "Result") == NULL;
                    if (is_builtin_result) {
                        int tag = (strcmp(vname, "Ok") == 0) ? 0 : 1;
                        enum_etype = LLVMTypeOf(subject);
                        enum_subject_alloca = LLVMBuildAlloca(g->builder, enum_etype, "res.tmp");
                        LLVMBuildStore(g->builder, subject, enum_subject_alloca);
                        LLVMValueRef actual_tag = LLVMBuildExtractValue(g->builder, subject, 0, "res.tag");
                        LLVMValueRef expected   = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), (unsigned long long)tag, 0);
                        cmp = LLVMBuildICmp(g->builder, LLVMIntEQ, actual_tag, expected, "res.cmp");
                        // Fake enum_matched_var to NULL — handle binding below
                        enum_matched_var = NULL;
                        goto result_pattern_done;
                    }
                    Symbol *esym = scope_lookup(g->resolver->global, ename);
                    int tag = 0;
                    if (esym && esym->decl) {
                        AstNode *ed = esym->decl;
                        for (size_t vi = 0; vi < ed->as.enum_decl.variant_count; vi++) {
                            if (strcmp(ed->as.enum_decl.variants[vi].name, vname) == 0) {
                                tag = (int)vi;
                                enum_matched_var = &ed->as.enum_decl.variants[vi];
                                break;
                            }
                        }
                    }
                    enum_etype = LLVMTypeOf(subject);
                    enum_subject_alloca = LLVMBuildAlloca(g->builder, enum_etype, "subj.tmp");
                    LLVMBuildStore(g->builder, subject, enum_subject_alloca);
                    LLVMValueRef tag_ptr = LLVMBuildStructGEP2(g->builder, enum_etype, enum_subject_alloca, 0, "tag.ptr");
                    LLVMValueRef actual_tag = LLVMBuildLoad2(g->builder, LLVMInt32TypeInContext(g->ctx), tag_ptr, "tag");
                    LLVMValueRef expected_tag = LLVMConstInt(LLVMInt32TypeInContext(g->ctx), (unsigned long long)tag, 0);
                    cmp = LLVMBuildICmp(g->builder, LLVMIntEQ, actual_tag, expected_tag, "tag.cmp");
                } else if (arm->pattern->kind == NODE_RANGE) {
                    LLVMValueRef lo = emit_expr(g, arm->pattern->as.range.from);
                    LLVMValueRef hi = emit_expr(g, arm->pattern->as.range.to);
                    LLVMValueRef ge = LLVMBuildICmp(g->builder, LLVMIntSGE, subject, lo, "rge");
                    LLVMValueRef le = arm->pattern->as.range.inclusive
                        ? LLVMBuildICmp(g->builder, LLVMIntSLE, subject, hi, "rle")
                        : LLVMBuildICmp(g->builder, LLVMIntSLT, subject, hi, "rlt");
                    cmp = LLVMBuildAnd(g->builder, ge, le, "in_range");
                } else {
                    LLVMValueRef pat = emit_expr(g, arm->pattern);
                    cmp = LLVMBuildICmp(g->builder, LLVMIntEQ, subject, pat, "match.cmp");
                }

                result_pattern_done:;
                if (arm->guard) {
                    LLVMValueRef guard = emit_expr(g, arm->guard);
                    LLVMValueRef gz = LLVMConstInt(LLVMTypeOf(guard), 0, 0);
                    LLVMValueRef gbool = LLVMBuildICmp(g->builder,
                        LLVMIntNE, guard, gz, "match.guard");
                    cmp = LLVMBuildAnd(g->builder, cmp, gbool, "match.guarded");
                }

                LLVMBuildCondBr(g->builder, cmp, then_bb, next_bb);

                LLVMPositionBuilderAtEnd(g->builder, then_bb);
                scope_push(g);

                // Result pattern bindings: Ok(v) / Err(e)
                int is_result_pat = arm->pattern && arm->pattern->kind == NODE_ENUM_PATTERN
                    && (strcmp(arm->pattern->as.enum_pattern.variant, "Ok") == 0
                     || strcmp(arm->pattern->as.enum_pattern.variant, "Err") == 0);
                if (is_result_pat && arm->pattern->as.enum_pattern.binding_count > 0
                        && enum_subject_alloca && !enum_matched_var) {
                    const char *bname = arm->pattern->as.enum_pattern.bindings[0];
                    const char *vname = arm->pattern->as.enum_pattern.variant;
                    LLVMValueRef sv   = LLVMBuildLoad2(g->builder, enum_etype, enum_subject_alloca, "res.sv");
                    LLVMValueRef pay64 = LLVMBuildExtractValue(g->builder, sv, 1, "pay64");
                    LLVMTypeRef i64t  = LLVMInt64TypeInContext(g->ctx);
                    LLVMTypeRef i32t2 = LLVMInt32TypeInContext(g->ctx);
                    // Determine binding type from match subject's function return type
                    // Heuristic: Ok → try i32 (int), Err → try ptr (str)
                    LLVMTypeRef btyp;
                    LLVMValueRef bval;
                    if (strcmp(vname, "Ok") == 0) {
                        bval = LLVMBuildTrunc(g->builder, pay64, i32t2, bname);
                        btyp = i32t2;
                    } else {
                        // Err — treat as ptr (str pointer)
                        LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0);
                        bval = LLVMBuildIntToPtr(g->builder, pay64, ptr_t, bname);
                        btyp = ptr_t;
                    }
                    LLVMValueRef bal = LLVMBuildAlloca(g->builder, btyp, bname);
                    LLVMBuildStore(g->builder, bval, bal);
                    scope_set(g, bname, bal, btyp, 1);
                }
    
                // Emit enum payload bindings into then_bb
                if (arm->pattern && arm->pattern->kind == NODE_ENUM_PATTERN
                        && enum_matched_var && enum_subject_alloca) {
                    LLVMTypeRef i32 = LLVMInt32TypeInContext(g->ctx);
                    LLVMValueRef zero = LLVMConstInt(i32, 0, 0);
                    LLVMTypeRef arr_type = LLVMStructGetTypeAtIndex(enum_etype, 1);
                    LLVMValueRef arr_ptr = LLVMBuildStructGEP2(g->builder, enum_etype, enum_subject_alloca, 1, "pay");
                    unsigned slot = 0;
                    for (size_t bi = 0; bi < arm->pattern->as.enum_pattern.binding_count; bi++) {
                        const char *bname = arm->pattern->as.enum_pattern.bindings[bi];
                        LLVMTypeRef fllvm = (bi < enum_matched_var->field_count)
                            ? ci_type_to_llvm(g, enum_matched_var->fields[bi].type)
                            : LLVMInt32TypeInContext(g->ctx);
                        LLVMValueRef slot_idx = LLVMConstInt(i32, slot, 0);
                        LLVMValueRef indices[2] = { zero, slot_idx };
                        LLVMValueRef slot_ptr = LLVMBuildGEP2(g->builder, arr_type, arr_ptr, indices, 2, "bslot");
                        LLVMValueRef typed_ptr = LLVMBuildBitCast(g->builder, slot_ptr, LLVMPointerType(fllvm, 0), "bptr");
                        LLVMValueRef bval = LLVMBuildLoad2(g->builder, fllvm, typed_ptr, bname);
                        LLVMValueRef bal = LLVMBuildAlloca(g->builder, fllvm, bname);
                        LLVMBuildStore(g->builder, bval, bal);
                        scope_set(g, bname, bal, fllvm, 1);
                        slot += (bi < enum_matched_var->field_count)
                            ? type_word_count(enum_matched_var->fields[bi].type) : 1;
                    }
                }
                emit_stmt(g, arm->body);
                scope_pop(g);

                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                    LLVMBuildBr(g->builder, merge_bb);

                LLVMPositionBuilderAtEnd(g->builder, next_bb);
            }

            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                LLVMBuildBr(g->builder, merge_bb);
            LLVMPositionBuilderAtEnd(g->builder, merge_bb);
            break;
        }

        case NODE_FOR_RANGE: {
            const char *var = node->as.for_range.var;
            LLVMTypeRef i32  = LLVMInt32TypeInContext(g->ctx);
            LLVMValueRef from = emit_expr(g, node->as.for_range.from);
            LLVMValueRef to   = emit_expr(g, node->as.for_range.to);

            LLVMValueRef slot = LLVMBuildAlloca(g->builder, i32, var);
            LLVMBuildStore(g->builder, from, slot);
            scope_set(g, var, slot, i32, 1);

            LLVMValueRef fn = g->current_func;
            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(g->ctx, fn, "for.cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(g->ctx, fn, "for.body");
            LLVMBasicBlockRef inc_bb  = LLVMAppendBasicBlockInContext(g->ctx, fn, "for.inc");
            LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(g->ctx, fn, "for.end");

            LLVMBuildBr(g->builder, cond_bb);
            LLVMPositionBuilderAtEnd(g->builder, cond_bb);
            LLVMValueRef cur = LLVMBuildLoad2(g->builder, i32, slot, "i");
            // Normalize to to i32 (e.g. slice.len is i64)
            if (LLVMTypeOf(to) != i32)
                to = LLVMBuildTrunc(g->builder, to, i32, "to.i32");
            LLVMValueRef cmp = node->as.for_range.inclusive
                ? LLVMBuildICmp(g->builder, LLVMIntSLE, cur, to, "cmp")
                : LLVMBuildICmp(g->builder, LLVMIntSLT, cur, to, "cmp");
            LLVMBuildCondBr(g->builder, cmp, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(g->builder, body_bb);
            LLVMBasicBlockRef prev_brk  = g->break_block;
            LLVMBasicBlockRef prev_cont = g->continue_block;
            g->break_block    = end_bb;
            g->continue_block = inc_bb;
            emit_stmt(g, node->as.for_range.body);
            g->break_block    = prev_brk;
            g->continue_block = prev_cont;
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder)))
                LLVMBuildBr(g->builder, inc_bb);

            LLVMPositionBuilderAtEnd(g->builder, inc_bb);
            LLVMValueRef next = LLVMBuildAdd(g->builder,
                LLVMBuildLoad2(g->builder, i32, slot, "i2"),
                LLVMConstInt(i32, 1, 0), "inc");
            LLVMBuildStore(g->builder, next, slot);
            LLVMBuildBr(g->builder, cond_bb);

            LLVMPositionBuilderAtEnd(g->builder, end_bb);
            break;
        }

        default:
            // Try emitting as expression statement
            emit_expr(g, node);
            break;
    }
}

// ─── DECLARE EXTERNAL C FUNCTIONS ────────────────────────────────────────────
// Declare printf, malloc, free, etc. so calls resolve in LLVM IR

static void declare_externals(LLVMCodegen *g) {
    LLVMTypeRef i8p  = LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0);
    LLVMTypeRef i32  = LLVMInt32TypeInContext(g->ctx);
    LLVMTypeRef i64  = LLVMInt64TypeInContext(g->ctx);
    LLVMTypeRef void_ty = LLVMVoidTypeInContext(g->ctx);

    // printf(i8*, ...) -> i32
    LLVMTypeRef printf_ty = LLVMFunctionType(i32, &i8p, 1, 1);
    LLVMAddFunction(g->module, "printf", printf_ty);

    // fprintf(i8*, i8*, ...) -> i32
    LLVMTypeRef fprintf_args[2] = { i8p, i8p };
    LLVMTypeRef fprintf_ty = LLVMFunctionType(i32, fprintf_args, 2, 1);
    LLVMAddFunction(g->module, "fprintf", fprintf_ty);

    // malloc(i64) -> i8*
    LLVMTypeRef malloc_ty = LLVMFunctionType(i8p, &i64, 1, 0);
    LLVMAddFunction(g->module, "malloc", malloc_ty);

    // free(i8*) -> void
    LLVMTypeRef free_ty = LLVMFunctionType(void_ty, &i8p, 1, 0);
    LLVMAddFunction(g->module, "free", free_ty);

    // memcpy(i8*, i8*, i64) -> i8*
    LLVMTypeRef memcpy_args[3] = { i8p, i8p, i64 };
    LLVMTypeRef memcpy_ty = LLVMFunctionType(i8p, memcpy_args, 3, 0);
    LLVMAddFunction(g->module, "memcpy", memcpy_ty);

    // memset(i8*, i32, i64) -> i8*
    LLVMTypeRef memset_args[3] = { i8p, i32, i64 };
    LLVMTypeRef memset_ty = LLVMFunctionType(i8p, memset_args, 3, 0);
    LLVMAddFunction(g->module, "memset", memset_ty);

    // exit(i32) -> void
    LLVMTypeRef exit_ty = LLVMFunctionType(void_ty, &i32, 1, 0);
    LLVMAddFunction(g->module, "exit", exit_ty);

    // puts(i8*) -> i32
    LLVMTypeRef puts_ty = LLVMFunctionType(i32, &i8p, 1, 0);
    LLVMAddFunction(g->module, "puts", puts_ty);
}

// ─── STRUCT TYPE REGISTRATION ────────────────────────────────────────────────

static void register_struct_types(LLVMCodegen *g, AstNode *program) {
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || (decl->kind != NODE_STRUCT_DECL && decl->kind != NODE_UNION_DECL)) continue;
        if (decl->kind == NODE_UNION_DECL) {
            // Union: find largest field, emit as { [N x i8] }
            size_t max_bytes = 0;
            for (size_t j = 0; j < decl->as.struct_decl.field_count; j++) {
                LLVMTypeRef ft = ci_type_to_llvm(g, decl->as.struct_decl.fields[j].type);
                size_t sz = LLVMABISizeOfType(LLVMGetModuleDataLayout(g->module), ft);
                if (sz > max_bytes) max_bytes = sz;
            }
            if (max_bytes == 0) max_bytes = 8;
            LLVMTypeRef byte_arr = LLVMArrayType(LLVMInt8TypeInContext(g->ctx), (unsigned)max_bytes);
            LLVMTypeRef ut = LLVMStructCreateNamed(g->ctx, decl->as.struct_decl.name);
            LLVMStructSetBody(ut, &byte_arr, 1, 0);
            free((void*)0); // no-op, keep symmetry
        } else {
            size_t      fc     = decl->as.struct_decl.field_count;
            LLVMTypeRef *fields = malloc(fc * sizeof(LLVMTypeRef));
            for (size_t j = 0; j < fc; j++)
                fields[j] = ci_type_to_llvm(g, decl->as.struct_decl.fields[j].type);
            LLVMTypeRef st = LLVMStructCreateNamed(g->ctx, decl->as.struct_decl.name);
            LLVMStructSetBody(st, fields, (unsigned)fc, 0);
            free(fields);
        }
    }
}


// ─── MONOMORPHIZATION ────────────────────────────────────────────────────────

static int mono_already_emitted(LLVMCodegen *g, const char *mangled) {
    for (size_t i = 0; i < g->mono_count; i++)
        if (strcmp(g->mono_cache[i], mangled) == 0) return 1;
    return 0;
}

static void mono_mark_emitted(LLVMCodegen *g, const char *mangled) {
    if (g->mono_count >= g->mono_cap) {
        g->mono_cap = g->mono_cap ? g->mono_cap * 2 : 16;
        char **nc = arena_alloc(g->arena, g->mono_cap * sizeof(char *));
        memcpy(nc, g->mono_cache, g->mono_count * sizeof(char *));
        g->mono_cache = nc;
    }
    char *copy = arena_alloc(g->arena, strlen(mangled) + 1);
    strcpy(copy, mangled);
    g->mono_cache[g->mono_count++] = copy;
}

// Deep-clone an AstNode, substituting generic param 'T' with concrete type.
// For now: only substitutes param types and return type in func_decl.
static AstType *subst_type(Arena *arena, AstType *t,
                            const char *tparam, AstType *concrete) {
    if (!t) return NULL;
    if ((t->kind == TY_NAMED || t->kind == TY_GENERIC)
            && t->name && strcmp(t->name, tparam) == 0)
        return concrete;
    // shallow copy with recursion on inner
    AstType *copy = arena_alloc(arena, sizeof(AstType));
    *copy = *t;
    copy->inner     = subst_type(arena, t->inner, tparam, concrete);
    copy->elem_type = subst_type(arena, t->elem_type, tparam, concrete);
    return copy;
}

// Emit a monomorphized version of a generic function.
// generic_params[0] = "T", concrete_types[0] = AstType for Point, etc.
static void emit_mono_func(LLVMCodegen *g, AstNode *func,
                            const char *mangled_name,
                            const char **tparams, AstType **ctypes, size_t nparams) {
    if (mono_already_emitted(g, mangled_name)) return;
    mono_mark_emitted(g, mangled_name);

    // Clone the func node shallowly and patch name + param types
    AstNode *clone = arena_alloc(g->arena, sizeof(AstNode));
    *clone = *func;
    clone->as.func_decl.name = arena_alloc(g->arena, strlen(mangled_name) + 1);
    strcpy(clone->as.func_decl.name, mangled_name);

    // Clone params with substituted types
    size_t pc = func->as.func_decl.param_count;
    AstParam *new_params = arena_alloc(g->arena, pc * sizeof(AstParam));
    for (size_t i = 0; i < pc; i++) {
        new_params[i] = func->as.func_decl.params[i];
        AstType *pt = func->as.func_decl.params[i].type;
        for (size_t j = 0; j < nparams; j++)
            pt = subst_type(g->arena, pt, tparams[j], ctypes[j]);
        new_params[i].type = pt;
    }
    clone->as.func_decl.params = new_params;

    // Substitute return type
    AstType *rt = func->as.func_decl.return_type;
    for (size_t j = 0; j < nparams; j++)
        rt = subst_type(g->arena, rt, tparams[j], ctypes[j]);
    clone->as.func_decl.return_type = rt;

    // Zero out generic params so emit_func treats it as concrete
    clone->as.func_decl.generic_params = NULL;
    clone->as.func_decl.generic_count  = 0;

    // Save current codegen state
    LLVMValueRef      saved_func      = g->current_func;
    LLVMTypeRef       saved_func_type = g->current_func_type;
    LLVMBasicBlockRef saved_insert    = LLVMGetInsertBlock(g->builder);
    LLVMBasicBlockRef saved_break     = g->break_block;
    LLVMBasicBlockRef saved_cont      = g->continue_block;

    emit_func(g, clone);

    // Restore
    g->current_func      = saved_func;
    g->current_func_type = saved_func_type;
    g->break_block       = saved_break;
    g->continue_block    = saved_cont;
    if (saved_insert)
        LLVMPositionBuilderAtEnd(g->builder, saved_insert);
}

// ─── FUNCTION EMISSION ───────────────────────────────────────────────────────

static void emit_func(LLVMCodegen *g, AstNode *node) {
    // Build LLVM function type
    size_t       pc     = node->as.func_decl.param_count;
    LLVMTypeRef *params = malloc(pc * sizeof(LLVMTypeRef));

    for (size_t i = 0; i < pc; i++) {
        AstParam *p = &node->as.func_decl.params[i];
        params[i]   = p->type ? ci_type_to_llvm(g, p->type)
                              : LLVMInt32TypeInContext(g->ctx);
    }

    LLVMTypeRef ret_type = node->as.func_decl.return_type
        ? ci_type_to_llvm(g, node->as.func_decl.return_type)
        : LLVMInt32TypeInContext(g->ctx);  // inferred default

    LLVMTypeRef  fn_type = LLVMFunctionType(ret_type, params, (unsigned)pc, 0);
    LLVMValueRef fn      = LLVMAddFunction(g->module,
                                            node->as.func_decl.name, fn_type);
    free(params);

    // Linkage
    if (node->as.func_decl.access != ACCESS_PUBLIC)
        LLVMSetLinkage(fn, LLVMInternalLinkage);

    // Entry block
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(
        g->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(g->builder, entry);

    LLVMValueRef saved_func = g->current_func;
    g->current_func         = fn;

    // Save + reset defer stack
    AstNode **saved_defer  = g->defer_stack;
    size_t    saved_count  = g->defer_count;
    size_t    saved_cap    = g->defer_cap;
    g->defer_cap   = 8;
    g->defer_count = 0;
    g->defer_stack = malloc(g->defer_cap * sizeof(AstNode *));

    scope_push(g);

    // Register params as allocas
    for (size_t i = 0; i < node->as.func_decl.param_count; i++) {
        AstParam    *p    = &node->as.func_decl.params[i];
        LLVMValueRef pval = LLVMGetParam(fn, (unsigned)i);
        LLVMTypeRef  pty  = LLVMTypeOf(pval);
        LLVMValueRef al   = LLVMBuildAlloca(g->builder, pty, p->name);
        LLVMBuildStore(g->builder, pval, al);
        scope_set(g, p->name, al, pty, 1);
        // If param is a pointer type, record what it points to
        if (p->type && (p->type->kind == TY_POINTER || p->type->kind == TY_NULLABLE_PTR)) {
            LLVMEntry *e = scope_get(g, p->name);
            if (e && p->type->inner)
                e->elem_type = ci_type_to_llvm(g, p->type->inner);
        }
    }

    // Emit body
    if (node->as.func_decl.body)
        emit_stmt(g, node->as.func_decl.body);

    // Auto-insert return if block has no terminator
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->builder))) {
        emit_defers(g);
        LLVMTypeRef ret_type = LLVMGetReturnType(
            LLVMGlobalGetValueType(g->current_func));
        if (LLVMGetTypeKind(ret_type) == LLVMVoidTypeKind)
            LLVMBuildRetVoid(g->builder);
        else {
            LLVMTypeKind rk = LLVMGetTypeKind(ret_type);
            if (rk == LLVMFloatTypeKind || rk == LLVMDoubleTypeKind)
                LLVMBuildRet(g->builder, LLVMConstReal(ret_type, 0.0));
            else
                LLVMBuildRet(g->builder, LLVMConstInt(ret_type, 0, 0));
        }
    }

    scope_pop(g);

    free(g->defer_stack);
    g->defer_stack = saved_defer;
    g->defer_count = saved_count;
    g->defer_cap   = saved_cap;
    g->current_func = saved_func;
}

// ─── ASM FUNCTION EMISSION ───────────────────────────────────────────────────

static void emit_asm_func(LLVMCodegen *g, AstNode *node) {
    // Build param types
    size_t       pc     = node->as.asm_func_decl.param_count;
    LLVMTypeRef *params = malloc(pc * sizeof(LLVMTypeRef));
    for (size_t i = 0; i < pc; i++)
        params[i] = ci_type_to_llvm(g, node->as.asm_func_decl.params[i].type);

    LLVMTypeRef ret_type = node->as.asm_func_decl.return_reg
        ? LLVMInt64TypeInContext(g->ctx)
        : LLVMVoidTypeInContext(g->ctx);

    LLVMTypeRef  fn_type = LLVMFunctionType(ret_type, params, (unsigned)pc, 0);
    LLVMValueRef fn      = LLVMAddFunction(g->module,
                                            node->as.asm_func_decl.name, fn_type);
    free(params);

    if (node->as.asm_func_decl.access != ACCESS_PUBLIC)
        LLVMSetLinkage(fn, LLVMInternalLinkage);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(g->builder, entry);

    LLVMValueRef saved = g->current_func;
    g->current_func    = fn;

    // Build inline asm string and constraint string
    // Constraints: input params bound to their registers
    char asm_str[4096]   = {0};
    char constraints[512] = {0};

    // Copy raw asm body, strip ciren-style ; comments
    const char *body = node->as.asm_func_decl.body;
    char *line = strdup(body);
    char *tok  = strtok(line, "\n");
    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok && *tok != ';' && strncmp(tok, "clobbers", 8) != 0) {
            strcat(asm_str, tok);
            strcat(asm_str, "\n");
        }
        tok = strtok(NULL, "\n");
    }
    free(line);

    // Build constraint string: outputs first, then inputs
    if (node->as.asm_func_decl.return_reg) {
        snprintf(constraints + strlen(constraints),
                 sizeof(constraints) - strlen(constraints),
                 "={%s}", node->as.asm_func_decl.return_reg);
    }
    for (size_t i = 0; i < pc; i++) {
        if (i > 0 || node->as.asm_func_decl.return_reg)
            strcat(constraints, ",");
        snprintf(constraints + strlen(constraints),
                 sizeof(constraints) - strlen(constraints),
                 "{%s}", node->as.asm_func_decl.params[i].reg);
    }
    // Clobbers
    strcat(constraints, ",~{memory},~{dirflag},~{fpsr},~{flags}");

    // Collect input values (the function params)
    LLVMValueRef *in_vals = malloc(pc * sizeof(LLVMValueRef));
    LLVMTypeRef  *in_tys  = malloc(pc * sizeof(LLVMTypeRef));
    for (size_t i = 0; i < pc; i++) {
        in_vals[i] = LLVMGetParam(fn, (unsigned)i);
        in_tys[i]  = LLVMTypeOf(in_vals[i]);
    }

    LLVMTypeRef asm_fn_ty = LLVMFunctionType(ret_type, in_tys, (unsigned)pc, 0);
    LLVMValueRef asm_val  = LLVMGetInlineAsm(
        asm_fn_ty,
        asm_str,    strlen(asm_str),
        constraints, strlen(constraints),
        1,   // hasSideEffects
        0,   // isAlignStack
        LLVMInlineAsmDialectATT,
        0    // canThrow
    );

    LLVMValueRef result = LLVMBuildCall2(g->builder, asm_fn_ty,
        asm_val, in_vals, (unsigned)pc,
        node->as.asm_func_decl.return_reg ? "asm.ret" : "");

    free(in_vals);
    free(in_tys);

    if (node->as.asm_func_decl.return_reg)
        LLVMBuildRet(g->builder, result);
    else
        LLVMBuildRetVoid(g->builder);

    g->current_func = saved;
}

// ─── OPTIMIZATION ────────────────────────────────────────────────────────────

static void run_optimizations(LLVMCodegen *g, LLVMTargetMachineRef tm,
                               int opt_level) {
    if (opt_level == 0) return;

    char opt_str[16];
    snprintf(opt_str, sizeof(opt_str), "default<O%d>", opt_level);

    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMPassBuilderOptionsSetLoopUnrolling(opts, 1);
    LLVMPassBuilderOptionsSetMergeFunctions(opts, 1);

    LLVMErrorRef err = LLVMRunPasses(g->module, opt_str, tm, opts);
    if (err) {
        char *msg = LLVMGetErrorMessage(err);
        fprintf(stderr, "[ciren] optimization error: %s\n", msg);
        LLVMDisposeErrorMessage(msg);
    }
    LLVMDisposePassBuilderOptions(opts);
}

// ─── ENTRY POINT ─────────────────────────────────────────────────────────────

LLVMCodegen *llvm_codegen_create(Arena *arena, Resolver *resolver,
                                  const char *filename) {
    LLVMCodegen *g = arena_alloc(arena, sizeof(LLVMCodegen));
    g->arena       = arena;
    g->resolver    = resolver;
    g->filename    = filename;
    g->had_error   = 0;

    g->ctx     = LLVMContextCreate();
    g->module  = LLVMModuleCreateWithNameInContext("ciren", g->ctx);
    g->builder = LLVMCreateBuilderInContext(g->ctx);

    g->scope         = NULL;
    g->current_func  = NULL;
    g->break_block   = NULL;
    g->continue_block = NULL;
    g->label_counter = 0;

    g->defer_cap   = 16;
    g->defer_count = 0;
    g->defer_stack = malloc(g->defer_cap * sizeof(AstNode *));

    g->str_cap     = 16;
    g->str_count   = 0;
    g->str_globals = malloc(g->str_cap * sizeof(LLVMValueRef));
    g->str_values  = malloc(g->str_cap * sizeof(char *));

    return g;
}

int llvm_codegen_run(LLVMCodegen *g, AstNode *program,
                     const char *output_path, int emit_ir, int opt_level) {
    // ── Initialize LLVM targets ──────────────────
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    // ── Set target triple ────────────────────────
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(g->module, triple);

    char *error = NULL;
    LLVMTargetRef target;
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        fprintf(stderr, "[ciren] target error: %s\n", error);
        LLVMDisposeMessage(error);
        LLVMDisposeMessage(triple);
        return 0;
    }

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, "generic", "",
        opt_level > 0 ? LLVMCodeGenLevelDefault : LLVMCodeGenLevelNone,
        LLVMRelocPIC,
        LLVMCodeModelDefault
    );
    LLVMDisposeMessage(triple);

    // Set data layout
    LLVMTargetDataRef data_layout = LLVMCreateTargetDataLayout(tm);
    LLVMSetModuleDataLayout(g->module, data_layout);

    // ── Declare externals ────────────────────────
    declare_externals(g);

    // ── Pre-declare C stdlib functions ───────────
    {
        LLVMTypeRef i8p  = LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0);
        LLVMTypeRef i32t = LLVMInt32TypeInContext(g->ctx);
        LLVMTypeRef i64t = LLVMInt64TypeInContext(g->ctx);
        LLVMTypeRef voit = LLVMVoidTypeInContext(g->ctx);
        // stdio
        LLVMTypeRef printf_args[] = { i8p };
        LLVMAddFunction(g->module, "printf",
            LLVMFunctionType(i32t, printf_args, 1, 1));
        LLVMAddFunction(g->module, "puts",
            LLVMFunctionType(i32t, (LLVMTypeRef[]){i8p}, 1, 0));
        // string
        LLVMAddFunction(g->module, "strlen",
            LLVMFunctionType(i64t, (LLVMTypeRef[]){i8p}, 1, 0));
        LLVMAddFunction(g->module, "strcmp",
            LLVMFunctionType(i32t, (LLVMTypeRef[]){i8p, i8p}, 2, 0));
        LLVMAddFunction(g->module, "strncmp",
            LLVMFunctionType(i32t, (LLVMTypeRef[]){i8p, i8p, i64t}, 3, 0));
        LLVMAddFunction(g->module, "strcpy",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i8p}, 2, 0));
        LLVMAddFunction(g->module, "strcat",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i8p}, 2, 0));
        LLVMAddFunction(g->module, "strchr",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i32t}, 2, 0));
        LLVMAddFunction(g->module, "strstr",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i8p}, 2, 0));
        LLVMAddFunction(g->module, "sprintf",
            LLVMFunctionType(i32t, (LLVMTypeRef[]){i8p, i8p}, 2, 1));
        LLVMAddFunction(g->module, "snprintf",
            LLVMFunctionType(i32t, (LLVMTypeRef[]){i8p, i64t, i8p}, 3, 1));
        LLVMAddFunction(g->module, "atoi",
            LLVMFunctionType(i32t, (LLVMTypeRef[]){i8p}, 1, 0));
        // memory
        LLVMAddFunction(g->module, "malloc",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i64t}, 1, 0));
        LLVMAddFunction(g->module, "calloc",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i64t, i64t}, 2, 0));
        LLVMAddFunction(g->module, "realloc",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i64t}, 2, 0));
        LLVMAddFunction(g->module, "free",
            LLVMFunctionType(voit, (LLVMTypeRef[]){i8p}, 1, 0));
        LLVMAddFunction(g->module, "memcpy",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i8p, i64t}, 3, 0));
        LLVMAddFunction(g->module, "memset",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i32t, i64t}, 3, 0));
        LLVMAddFunction(g->module, "memmove",
            LLVMFunctionType(i8p,  (LLVMTypeRef[]){i8p, i8p, i64t}, 3, 0));
        // math
        LLVMAddFunction(g->module, "sqrt",
            LLVMFunctionType(LLVMDoubleTypeInContext(g->ctx),
                (LLVMTypeRef[]){LLVMDoubleTypeInContext(g->ctx)}, 1, 0));
        // sys
        LLVMAddFunction(g->module, "exit",
            LLVMFunctionType(voit, (LLVMTypeRef[]){i32t}, 1, 0));
    }

    // ── Register struct types ─────────────────────
    // ── Register built-in types ───────────────────
    {
        LLVMTypeRef i32t  = LLVMInt32TypeInContext(g->ctx);
        LLVMTypeRef ptr_t = LLVMPointerType(LLVMInt8TypeInContext(g->ctx), 0);
        LLVMTypeRef i64t  = LLVMInt64TypeInContext(g->ctx);
        // _ci_result = { i32 tag, i64 payload } — payload bitcast to/from i64
        LLVMTypeRef result_t = LLVMStructCreateNamed(g->ctx, "_ci_result");
        LLVMTypeRef result_fields[2] = { i32t, i64t };
        LLVMStructSetBody(result_t, result_fields, 2, 0);
        // _ci_str = { ptr, i64 }
        if (!LLVMGetTypeByName2(g->ctx, "_ci_str")) {
            LLVMTypeRef str_t = LLVMStructCreateNamed(g->ctx, "_ci_str");
            LLVMTypeRef str_fields[2] = { ptr_t, i64t };
            LLVMStructSetBody(str_t, str_fields, 2, 0);
        }
        // _ci_slice = { ptr, i64 }
        if (!LLVMGetTypeByName2(g->ctx, "_ci_slice")) {
            LLVMTypeRef slice_t = LLVMStructCreateNamed(g->ctx, "_ci_slice");
            LLVMTypeRef slice_fields[2] = { ptr_t, i64t };
            LLVMStructSetBody(slice_t, slice_fields, 2, 0);
        }
    }
    register_struct_types(g, program);
    register_enum_types(g, program);

    // ── Pre-emit all impl methods (so method calls work from generic functions) ──
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_IMPL_DECL) continue;
        const char *tname = decl->as.impl_decl.type_name;
        for (size_t mi = 0; mi < decl->as.impl_decl.method_count; mi++) {
            AstNode *m = decl->as.impl_decl.methods[mi];
            if (m->kind != NODE_FUNC_DECL) continue;
            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s__%s", m->as.func_decl.name, tname);
            char *mn = arena_alloc(g->arena, strlen(mangled) + 1);
            strcpy(mn, mangled);
            char *orig = m->as.func_decl.name;
            m->as.func_decl.name = mn;
            emit_func(g, m);
            m->as.func_decl.name = orig;
        }
    }
    // ── Emit all declarations ─────────────────────
    scope_push(g);
    // Pass 0: emit all constants first so functions can reference them
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_VAR_DECL) continue;
        if (!decl->as.var_decl.is_const || !decl->as.var_decl.value) continue;
        LLVMTypeRef type = decl->as.var_decl.type
            ? ci_type_to_llvm(g, decl->as.var_decl.type)
            : LLVMInt32TypeInContext(g->ctx);
        AstNode *val = decl->as.var_decl.value;
        LLVMValueRef cval = NULL;
        if (val->kind == NODE_INT_LIT)
            cval = LLVMConstInt(type, (unsigned long long)val->as.int_lit.value, 1);
        else if (val->kind == NODE_FLOAT_LIT)
            cval = LLVMConstReal(type, val->as.float_lit.value);
        else if (val->kind == NODE_BOOL_LIT)
            cval = LLVMConstInt(type, val->as.bool_lit.value, 0);
        if (cval)
            scope_set(g, decl->as.var_decl.name, cval, type, 0);
    }
    // Pass 1: all non-main functions first so main can call them
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_FUNC_DECL) continue;
        if (strcmp(decl->as.func_decl.name, "main") == 0) continue;
        if (decl->as.func_decl.generic_count == 0)
            emit_func(g, decl);
    }
    // Pass 2: everything else including main
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl) continue;
        switch (decl->kind) {
            case NODE_FUNC_DECL:
                if (strcmp(decl->as.func_decl.name, "main") != 0) break;
                if (decl->as.func_decl.generic_count == 0)
                    emit_func(g, decl);
                break;
            case NODE_IMPL_DECL:
                break; // already emitted in pre-pass
            case NODE_ASM_FUNC_DECL:
                emit_asm_func(g, decl);
                break;
            case NODE_VAR_DECL:
                if (decl->as.var_decl.is_const && decl->as.var_decl.value) {
                    // Const: emit as immediate value, not a global
                    LLVMTypeRef type = decl->as.var_decl.type
                        ? ci_type_to_llvm(g, decl->as.var_decl.type)
                        : LLVMInt32TypeInContext(g->ctx);
                    AstNode *val = decl->as.var_decl.value;
                    LLVMValueRef cval = NULL;
                    if (val->kind == NODE_INT_LIT)
                        cval = LLVMConstInt(type, (unsigned long long)val->as.int_lit.value, 1);
                    else if (val->kind == NODE_FLOAT_LIT)
                        cval = LLVMConstReal(type, val->as.float_lit.value);
                    else if (val->kind == NODE_BOOL_LIT)
                        cval = LLVMConstInt(type, val->as.bool_lit.value, 0);
                    if (cval)
                        scope_set(g, decl->as.var_decl.name, cval, type, 0);
                } else if (decl->as.var_decl.type) {
                    LLVMTypeRef  type = ci_type_to_llvm(g, decl->as.var_decl.type);
                    LLVMValueRef glob = LLVMAddGlobal(g->module, type,
                                                       decl->as.var_decl.name);
                    if (decl->as.var_decl.access != ACCESS_PUBLIC)
                        LLVMSetLinkage(glob, LLVMInternalLinkage);
                    LLVMSetInitializer(glob, LLVMConstNull(type));
                    scope_set(g, decl->as.var_decl.name, glob, type, 1);
                }
                break;
            default:
                break;
        }
    }
    scope_pop(g);

    // ── Verify module ────────────────────────────
    if (LLVMVerifyModule(g->module, LLVMPrintMessageAction, &error)) {
        fprintf(stderr, "[ciren] LLVM verification failed: %s\n", error);
        LLVMDisposeMessage(error);
        g->had_error = 1;
    }

    if (g->had_error) return 0;

    // ── Optimize ─────────────────────────────────
    run_optimizations(g, tm, opt_level);

    // ── Emit output ──────────────────────────────
    if (emit_ir) {
        // Emit human-readable .ll file
        if (LLVMPrintModuleToFile(g->module, output_path, &error)) {
            fprintf(stderr, "[ciren] IR write error: %s\n", error);
            LLVMDisposeMessage(error);
            return 0;
        }
    } else {
        // Emit native object file
        if (LLVMTargetMachineEmitToFile(tm, g->module,
                                         (char *)output_path,
                                         LLVMObjectFile, &error)) {
            fprintf(stderr, "[ciren] object emit error: %s\n", error);
            LLVMDisposeMessage(error);
            return 0;
        }
    }

    fprintf(stderr, "ciren: wrote %s\n", output_path);

    LLVMDisposeTargetData(data_layout);
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeBuilder(g->builder);
    LLVMDisposeModule(g->module);
    LLVMContextDispose(g->ctx);

    return 1;
}