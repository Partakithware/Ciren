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
// ciren/ast.c

#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── ARENA ───────────────────────────────────────────────────────────────────

Arena *arena_create(size_t capacity) {
    Arena *a = malloc(sizeof(Arena));
    a->base     = malloc(capacity);
    a->used     = 0;
    a->capacity = capacity;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    // align to 8 bytes
    size = (size + 7) & ~(size_t)7;
    if (a->used + size > a->capacity) {
        fprintf(stderr, "[ciren] arena out of memory (used=%zu cap=%zu need=%zu)\n",
                a->used, a->capacity, size);
        exit(1);
    }
    void *ptr = a->base + a->used;
    a->used += size;
    memset(ptr, 0, size);
    return ptr;
}

void arena_destroy(Arena *a) {
    free(a->base);
    free(a);
}

AstNode *node_alloc(Arena *a, NodeKind kind, int line, int col) {
    AstNode *n = arena_alloc(a, sizeof(AstNode));
    n->kind = kind;
    n->line = line;
    n->col  = col;
    return n;
}

AstType *type_alloc(Arena *a, TypeKind kind) {
    AstType *t = arena_alloc(a, sizeof(AstType));
    t->kind = kind;
    return t;
}

// ─── DEBUG PRINTER ───────────────────────────────────────────────────────────

static void indent_print(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

void ast_print(AstNode *node, int indent) {
    if (!node) return;
    indent_print(indent);

    switch (node->kind) {
        case NODE_PROGRAM:
            printf("Program (%zu decls)\n", node->as.program.count);
            for (size_t i = 0; i < node->as.program.count; i++)
                ast_print(node->as.program.decls[i], indent + 1);
            break;

        case NODE_USING:
            printf("Using '%s'", node->as.using_decl.path);
            if (node->as.using_decl.alias)
                printf(" as '%s'", node->as.using_decl.alias);
            printf("\n");
            break;

        case NODE_FUNC_DECL:
            printf("FuncDecl [%s] %s (%zu params)\n",
                node->as.func_decl.access == ACCESS_PUBLIC   ? "public"   :
                node->as.func_decl.access == ACCESS_PRIVATE  ? "private"  : "internal",
                node->as.func_decl.name,
                node->as.func_decl.param_count);
            ast_print(node->as.func_decl.body, indent + 1);
            break;

        case NODE_ASM_FUNC_DECL:
            printf("AsmFuncDecl [%s] %s (%zu params)\n",
                node->as.asm_func_decl.access == ACCESS_PUBLIC   ? "public"   :
                node->as.asm_func_decl.access == ACCESS_PRIVATE  ? "private"  : "internal",
                node->as.asm_func_decl.name,
                node->as.asm_func_decl.param_count);
            indent_print(indent + 1);
            printf("asm { %s }\n", node->as.asm_func_decl.body);
            break;

        case NODE_STRUCT_DECL:
            printf("StructDecl %s (%zu fields, %zu methods)\n",
                node->as.struct_decl.name,
                node->as.struct_decl.field_count,
                node->as.struct_decl.method_count);
            break;

        case NODE_ENUM_DECL:
            printf("EnumDecl %s (%zu variants)\n",
                node->as.enum_decl.name,
                node->as.enum_decl.variant_count);
            break;

        case NODE_INTERFACE_DECL:
            printf("InterfaceDecl %s (%zu methods)\n",
                node->as.interface_decl.name,
                node->as.interface_decl.method_count);
            break;

        case NODE_VAR_DECL:
            printf("VarDecl %s '%s'\n",
                node->as.var_decl.is_const ? "const" : "let",
                node->as.var_decl.name);
            if (node->as.var_decl.value)
                ast_print(node->as.var_decl.value, indent + 1);
            break;

        case NODE_BLOCK:
            printf("Block (%zu stmts)\n", node->as.block.count);
            for (size_t i = 0; i < node->as.block.count; i++)
                ast_print(node->as.block.stmts[i], indent + 1);
            break;

        case NODE_RETURN:
            printf("Return\n");
            ast_print(node->as.ret.value, indent + 1);
            break;

        case NODE_IF:
            printf("If\n");
            indent_print(indent + 1); printf("Cond:\n");
            ast_print(node->as.if_stmt.condition, indent + 2);
            indent_print(indent + 1); printf("Then:\n");
            ast_print(node->as.if_stmt.then_block, indent + 2);
            if (node->as.if_stmt.else_block) {
                indent_print(indent + 1); printf("Else:\n");
                ast_print(node->as.if_stmt.else_block, indent + 2);
            }
            break;

        case NODE_WHILE:
            printf("While\n");
            ast_print(node->as.while_stmt.condition, indent + 1);
            ast_print(node->as.while_stmt.body, indent + 1);
            break;

        case NODE_FOR_RANGE:
            printf("ForRange '%s' in %s\n",
                node->as.for_range.var,
                node->as.for_range.inclusive ? "..=" : "..");
            ast_print(node->as.for_range.from, indent + 1);
            ast_print(node->as.for_range.to,   indent + 1);
            ast_print(node->as.for_range.body,  indent + 1);
            break;

        case NODE_FOR_IN:
            printf("ForIn '%s'\n", node->as.for_in.var);
            ast_print(node->as.for_in.iterable, indent + 1);
            ast_print(node->as.for_in.body,     indent + 1);
            break;

        case NODE_LOOP:
            printf("Loop%s\n", node->as.loop_stmt.label ? node->as.loop_stmt.label : "");
            ast_print(node->as.loop_stmt.body, indent + 1);
            break;

        case NODE_MATCH:
            printf("Match (%zu arms)\n", node->as.match_stmt.arm_count);
            ast_print(node->as.match_stmt.subject, indent + 1);
            break;

        case NODE_BREAK:
            printf("Break%s\n", node->as.jump.label ? node->as.jump.label : "");
            break;

        case NODE_CONTINUE:
            printf("Continue%s\n", node->as.jump.label ? node->as.jump.label : "");
            break;

        case NODE_DEFER:
            printf("Defer\n");
            ast_print(node->as.defer_stmt.stmt, indent + 1);
            break;

        case NODE_PANIC:
            printf("Panic\n");
            ast_print(node->as.panic_stmt.msg, indent + 1);
            break;

        case NODE_ASM_BLOCK:
            printf("AsmBlock { %s }\n", node->as.asm_block.body);
            break;

        case NODE_EXPR_STMT:
            printf("ExprStmt\n");
            ast_print(node->as.expr_stmt.expr, indent + 1);
            break;

        case NODE_INT_LIT:    printf("IntLit %lld\n",  (long long)node->as.int_lit.value); break;
        case NODE_FLOAT_LIT:  printf("FloatLit %f\n",  node->as.float_lit.value);          break;
        case NODE_STRING_LIT: printf("StrLit \"%s\"\n", node->as.string_lit.value);        break;
        case NODE_BOOL_LIT:   printf("BoolLit %s\n",   node->as.bool_lit.value ? "true" : "false"); break;
        case NODE_NULL_LIT:   printf("Null\n");                                             break;
        case NODE_IDENT:      printf("Ident '%s'\n",   node->as.ident.name);               break;

        case NODE_BINARY:
            printf("Binary '%s'\n", token_type_name(node->as.binary.op));
            ast_print(node->as.binary.left,  indent + 1);
            ast_print(node->as.binary.right, indent + 1);
            break;

        case NODE_UNARY:
            printf("Unary '%s'\n", token_type_name(node->as.unary.op));
            ast_print(node->as.unary.operand, indent + 1);
            break;

        case NODE_ASSIGN:
            printf("Assign '%s'\n", token_type_name(node->as.assign.op));
            ast_print(node->as.assign.target, indent + 1);
            ast_print(node->as.assign.value,  indent + 1);
            break;

        case NODE_CALL:
            printf("Call (%zu args)\n", node->as.call.arg_count);
            ast_print(node->as.call.callee, indent + 1);
            for (size_t i = 0; i < node->as.call.arg_count; i++)
                ast_print(node->as.call.args[i], indent + 1);
            break;

        case NODE_FIELD:
            printf("Field .%s\n", node->as.field.field);
            ast_print(node->as.field.target, indent + 1);
            break;

        case NODE_INDEX:
            printf("Index\n");
            ast_print(node->as.index.target, indent + 1);
            ast_print(node->as.index.index,  indent + 1);
            break;

        case NODE_CAST:
            printf("Cast\n");
            ast_print(node->as.cast.expr, indent + 1);
            break;

        case NODE_RANGE:
            printf("Range %s\n", node->as.range.inclusive ? "..=" : "..");
            ast_print(node->as.range.from, indent + 1);
            ast_print(node->as.range.to,   indent + 1);
            break;

        case NODE_PROPAGATE:
            printf("Propagate (?)\n");
            ast_print(node->as.propagate.expr, indent + 1);
            break;

        case NODE_SIZEOF:
            printf("Sizeof\n");
            break;

        case NODE_NEW:
            printf("New '%s'\n", node->as.new_expr.type_name);
            break;

        case NODE_DELETE:
            printf("Delete\n");
            ast_print(node->as.delete_expr.ptr, indent + 1);
            break;

        default:
            printf("Node<%d>\n", node->kind);
            break;
    }
}
