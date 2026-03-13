// ciren/codegen_llvm.h

#ifndef CIREN_CODEGEN_LLVM_H
#define CIREN_CODEGEN_LLVM_H

#include "ast.h"
#include "resolver.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/IRReader.h>

// ─── LLVM VALUE TABLE ────────────────────────────────────────────────────────
// Maps symbol names → LLVMValueRef so we can look up locals/globals

typedef struct LLVMEntry {
    char          *name;
    LLVMValueRef   value;
    LLVMTypeRef    type;
    LLVMTypeRef   elem_type;  // for pointer vars: the type they point to
    int            is_alloca;   // 1 = pointer to stack slot, load before use
    struct LLVMEntry *next;
} LLVMEntry;

#define LLVM_TABLE_BUCKETS 64

typedef struct LLVMScope {
    struct LLVMScope *parent;
    LLVMEntry        *buckets[LLVM_TABLE_BUCKETS];
} LLVMScope;

// ─── CODEGEN CONTEXT ─────────────────────────────────────────────────────────

typedef struct {
    Arena           *arena;
    Resolver        *resolver;
    const char      *filename;
    int              had_error;

    // LLVM handles
    LLVMContextRef   ctx;
    LLVMModuleRef    module;
    LLVMBuilderRef   builder;

    // Current function state
    LLVMValueRef     current_func;
    LLVMTypeRef      current_func_type;
    LLVMBasicBlockRef current_block;

    // Scope stack
    LLVMScope       *scope;

    // Defer stack
    AstNode        **defer_stack;
    size_t           defer_count;
    size_t           defer_cap;

    // Break/continue targets for loops
    LLVMBasicBlockRef break_block;
    LLVMBasicBlockRef continue_block;

    // Label counter
    int              label_counter;

    // String literal dedup table
    LLVMValueRef    *str_globals;
    char           **str_values;
    size_t           str_count;
    size_t           str_cap;
    // Monomorphization cache: "funcname__TypeName" -> already emitted
    char           **mono_cache;
    size_t           mono_count;
    size_t           mono_cap;

} LLVMCodegen;

// ─── API ─────────────────────────────────────────────────────────────────────

LLVMCodegen *llvm_codegen_create(Arena *arena, Resolver *resolver,
                                  const char *filename);
int          llvm_codegen_run(LLVMCodegen *g, AstNode *program,
                               const char *output_path,
                               int emit_ir,        // 1 = .ll text IR
                               int opt_level);     // 0-3

#endif // CIREN_CODEGEN_LLVM_H