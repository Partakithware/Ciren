// ciren/codegen.c

#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ─── EMIT HELPERS ────────────────────────────────────────────────────────────


static void emit(Codegen *g, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(g->out, fmt, args);
    va_end(args);
}

static void emit_indent(Codegen *g) {
    for (int i = 0; i < g->indent; i++)
        fprintf(g->out, "    ");
}

static void emitln(Codegen *g, const char *fmt, ...) {
    emit_indent(g);
    va_list args;
    va_start(args, fmt);
    vfprintf(g->out, fmt, args);
    va_end(args);
    fprintf(g->out, "\n");
}

static void emit_raw(Codegen *g, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(g->out, fmt, args);
    va_end(args);
}

static int new_label(Codegen *g) {
    return g->label_counter++;
}

static void cg_error(Codegen *g, AstNode *node, const char *msg) {
    g->had_error = 1;
    int line = node ? node->line : 0;
    fprintf(stderr, "[%s:%d] codegen error: %s\n", g->filename, line, msg);
}

// Add this helper above emit_expr
static void emit_escaped_string(Codegen *g, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '\n': emit_raw(g, "\\n");  break;
            case '\t': emit_raw(g, "\\t");  break;
            case '\r': emit_raw(g, "\\r");  break;
            case '"':  emit_raw(g, "\\\""); break;
            case '\\': emit_raw(g, "\\\\"); break;
            default:   emit_raw(g, "%c", *s); break;
        }
    }
}

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────

static void emit_type(Codegen *g, AstType *type);
static void emit_expr(Codegen *g, AstNode *node);
static void emit_stmt(Codegen *g, AstNode *node);
static void emit_block(Codegen *g, AstNode *node);
static void emit_func_decl(Codegen *g, AstNode *node);
static void emit_struct_decl(Codegen *g, AstNode *node);
static void emit_enum_decl(Codegen *g, AstNode *node);

// ─── TYPE EMISSION ───────────────────────────────────────────────────────────

static void emit_type(Codegen *g, AstType *type) {
    if (!type) { emit_raw(g, "void"); return; }

    switch (type->kind) {
        case TY_INT:          emit_raw(g, "int32_t");           break;
        case TY_UINT:         emit_raw(g, "uint32_t");          break;
        case TY_I8:           emit_raw(g, "int8_t");            break;
        case TY_U8:           emit_raw(g, "uint8_t");           break;
        case TY_I16:          emit_raw(g, "int16_t");           break;
        case TY_U16:          emit_raw(g, "uint16_t");          break;
        case TY_I64:          emit_raw(g, "int64_t");           break;
        case TY_U64:          emit_raw(g, "uint64_t");          break;
        case TY_F32:          emit_raw(g, "float");             break;
        case TY_F64:          emit_raw(g, "double");            break;
        case TY_BOOL:         emit_raw(g, "uint8_t");           break;
        case TY_CHAR:         emit_raw(g, "char");              break;
        case TY_STR:          emit_raw(g, "_ci_str");           break;
        case TY_CSTR:         emit_raw(g, "char*");             break;
        case TY_VOID:         emit_raw(g, "void");              break;
        case TY_ANY:          emit_raw(g, "void*");             break;
        case TY_INFERRED:     emit_raw(g, "auto");              break;

        case TY_NAMED:
        case TY_GENERIC:
            emit_raw(g, "%s", type->name);
            break;

        case TY_POINTER:
            emit_type(g, type->inner);
            emit_raw(g, "*");
            break;

        case TY_NULLABLE_PTR:
            emit_type(g, type->inner);
            emit_raw(g, "*");   // C has no nullable distinction — just a pointer
            break;

        case TY_SLICE:
            // Slices emit as _ci_slice (a struct with ptr + len)
            emit_raw(g, "_ci_slice");
            break;

        case TY_ARRAY:
            // Fixed arrays: handled at declaration site with [N] suffix
            emit_type(g, type->elem_type);
            break;

        case TY_FUNC_PTR:
            // Function pointer: return_type (*)(params)
            // Emitted at declaration site — here we emit the return type
            emit_type(g, type->return_type);
            emit_raw(g, "(*)(");
            for (size_t i = 0; i < type->param_count; i++) {
                if (i > 0) emit_raw(g, ", ");
                emit_type(g, type->param_types[i]);
            }
            emit_raw(g, ")");
            break;

        default:
            emit_raw(g, "void");
            break;
    }
}

// Emit a type + variable name, handling the special cases where
// the name has to go INSIDE the type (arrays, function pointers)
static void emit_type_with_name(Codegen *g, AstType *type, const char *name) {
    if (!type) {
        emit_raw(g, "void %s", name);
        return;
    }

    switch (type->kind) {
        case TY_ARRAY:
            emit_type(g, type->elem_type);
            emit_raw(g, " %s[", name);
            emit_expr(g, type->array_size);
            emit_raw(g, "]");
            break;

        case TY_FUNC_PTR:
            emit_type(g, type->return_type);
            emit_raw(g, " (*%s)(", name);
            for (size_t i = 0; i < type->param_count; i++) {
                if (i > 0) emit_raw(g, ", ");
                emit_type(g, type->param_types[i]);
            }
            emit_raw(g, ")");
            break;

        default:
            emit_type(g, type);
            emit_raw(g, " %s", name);
            break;
    }
}

// ─── EXPRESSION EMISSION ─────────────────────────────────────────────────────

static void emit_expr(Codegen *g, AstNode *node) {
    if (!node) return;

    switch (node->kind) {

        case NODE_INT_LIT:
            emit_raw(g, "%lld", (long long)node->as.int_lit.value);
            break;

        case NODE_FLOAT_LIT:
            emit_raw(g, "%g", node->as.float_lit.value);
            break;

        case NODE_STRING_LIT:
            emit_raw(g, "(_ci_str){ .ptr = \"");
            emit_escaped_string(g, node->as.string_lit.value);
            emit_raw(g, "\", .len = %zu }",
                     strlen(node->as.string_lit.value));
            break;

        case NODE_CSTRING_LIT:
            emit_raw(g, "\"");
            emit_escaped_string(g, node->as.cstring_lit.value);
            emit_raw(g, "\"");
            break;

        case NODE_CHAR_LIT:
            emit_raw(g, "'%c'", node->as.char_lit.value);
            break;

        case NODE_BOOL_LIT:
            emit_raw(g, "%s", node->as.bool_lit.value ? "1" : "0");
            break;

        case NODE_NULL_LIT:
            emit_raw(g, "NULL");
            break;

        case NODE_IDENT:
            emit_raw(g, "%s", node->as.ident.name);
            break;

        case NODE_BINARY: {
            emit_raw(g, "(");
            emit_expr(g, node->as.binary.left);
            switch (node->as.binary.op) {
                case TOK_PLUS:    emit_raw(g, " + ");  break;
                case TOK_MINUS:   emit_raw(g, " - ");  break;
                case TOK_STAR:    emit_raw(g, " * ");  break;
                case TOK_SLASH:   emit_raw(g, " / ");  break;
                case TOK_PERCENT: emit_raw(g, " %% "); break;
                case TOK_EQ:      emit_raw(g, " == "); break;
                case TOK_NEQ:     emit_raw(g, " != "); break;
                case TOK_LT:      emit_raw(g, " < ");  break;
                case TOK_GT:      emit_raw(g, " > ");  break;
                case TOK_LTE:     emit_raw(g, " <= "); break;
                case TOK_GTE:     emit_raw(g, " >= "); break;
                case TOK_AND:     emit_raw(g, " && "); break;
                case TOK_OR:      emit_raw(g, " || "); break;
                case TOK_AMP:     emit_raw(g, " & ");  break;
                case TOK_PIPE:    emit_raw(g, " | ");  break;
                case TOK_CARET:   emit_raw(g, " ^ ");  break;
                case TOK_LSHIFT:  emit_raw(g, " << "); break;
                case TOK_RSHIFT:  emit_raw(g, " >> "); break;
                default:          emit_raw(g, " ? ");  break;
            }
            emit_expr(g, node->as.binary.right);
            emit_raw(g, ")");
            break;
        }

        case NODE_UNARY: {
            if (node->as.unary.postfix) {
                emit_raw(g, "(");
                emit_expr(g, node->as.unary.operand);
                switch (node->as.unary.op) {
                    case TOK_PLUS_PLUS:   emit_raw(g, "++"); break;
                    case TOK_MINUS_MINUS: emit_raw(g, "--"); break;
                    default: break;
                }
                emit_raw(g, ")");
            } else {
                emit_raw(g, "(");
                switch (node->as.unary.op) {
                    case TOK_MINUS:       emit_raw(g, "-");  break;
                    case TOK_BANG:        emit_raw(g, "!");  break;
                    case TOK_TILDE:       emit_raw(g, "~");  break;
                    case TOK_PLUS_PLUS:   emit_raw(g, "++"); break;
                    case TOK_MINUS_MINUS: emit_raw(g, "--"); break;
                    default: break;
                }
                emit_expr(g, node->as.unary.operand);
                emit_raw(g, ")");
            }
            break;
        }

        case NODE_ADDRESS_OF:
            emit_raw(g, "(&");
            emit_expr(g, node->as.unary.operand);
            emit_raw(g, ")");
            break;

        case NODE_DEREF:
            emit_raw(g, "(*");
            emit_expr(g, node->as.unary.operand);
            emit_raw(g, ")");
            break;

        case NODE_ASSIGN: {
            emit_expr(g, node->as.assign.target);
            switch (node->as.assign.op) {
                case TOK_ASSIGN:       emit_raw(g, " = ");   break;
                case TOK_PLUS_ASSIGN:  emit_raw(g, " += ");  break;
                case TOK_MINUS_ASSIGN: emit_raw(g, " -= ");  break;
                case TOK_STAR_ASSIGN:  emit_raw(g, " *= ");  break;
                case TOK_SLASH_ASSIGN: emit_raw(g, " /= ");  break;
                default:               emit_raw(g, " = ");   break;
            }
            emit_expr(g, node->as.assign.value);
            break;
        }

        case NODE_CALL: {
            emit_expr(g, node->as.call.callee);
            emit_raw(g, "(");
            for (size_t i = 0; i < node->as.call.arg_count; i++) {
                if (i > 0) emit_raw(g, ", ");
                emit_expr(g, node->as.call.args[i]);
            }
            emit_raw(g, ")");
            break;
        }

        case NODE_FIELD: {
            // Decide . vs -> based on whether target is a pointer
            // For now emit -> and let C compiler validate
            emit_expr(g, node->as.field.target);
            emit_raw(g, ".%s", node->as.field.field);
            break;
        }

        case NODE_INDEX: {
            emit_expr(g, node->as.index.target);
            emit_raw(g, "[");
            emit_expr(g, node->as.index.index);
            emit_raw(g, "]");
            break;
        }

        case NODE_SLICE_EXPR: {
            // Emit as _ci_slice_make(ptr, from, to)
            emit_raw(g, "_ci_slice_make(");
            emit_expr(g, node->as.slice_expr.target);
            emit_raw(g, ", ");
            if (node->as.slice_expr.from)
                emit_expr(g, node->as.slice_expr.from);
            else
                emit_raw(g, "0");
            emit_raw(g, ", ");
            if (node->as.slice_expr.to) {
                emit_expr(g, node->as.slice_expr.to);
                if (node->as.slice_expr.inclusive)
                    emit_raw(g, " + 1");
            } else {
                // arr[2..] → use .len field of the array
                emit_raw(g, "-1");   // sentinel: codegen runtime resolves full len
            }
            emit_raw(g, ")");
            break;
        }

        case NODE_CAST: {
            emit_raw(g, "((");
            emit_type(g, node->as.cast.type);
            emit_raw(g, ")(");
            emit_expr(g, node->as.cast.expr);
            emit_raw(g, "))");
            break;
        }

        case NODE_STRUCT_LITERAL: {
            if (node->as.struct_lit.type_name)
                emit_raw(g, "(%s)", node->as.struct_lit.type_name);
            emit_raw(g, "{");
            for (size_t i = 0; i < node->as.struct_lit.field_count; i++) {
                if (i > 0) emit_raw(g, ", ");
                AstNode *fname = node->as.struct_lit.field_names[i];
                emit_raw(g, ".%s = ", fname->as.ident.name);
                emit_expr(g, node->as.struct_lit.field_values[i]);
            }
            emit_raw(g, "}");
            break;
        }

        case NODE_ARRAY_LITERAL: {
            emit_raw(g, "{");
            for (size_t i = 0; i < node->as.array_lit.count; i++) {
                if (i > 0) emit_raw(g, ", ");
                emit_expr(g, node->as.array_lit.elems[i]);
            }
            emit_raw(g, "}");
            break;
        }

        case NODE_IF_EXPR: {
            // Ternary where possible, otherwise emit as comma expression
            emit_raw(g, "((");
            emit_expr(g, node->as.if_stmt.condition);
            emit_raw(g, ") ? (");
            // then_block is a NODE_BLOCK with one expr stmt — unwrap it
            if (node->as.if_stmt.then_block &&
                node->as.if_stmt.then_block->kind == NODE_BLOCK &&
                node->as.if_stmt.then_block->as.block.count == 1) {
                AstNode *s = node->as.if_stmt.then_block->as.block.stmts[0];
                if (s && s->kind == NODE_EXPR_STMT)
                    emit_expr(g, s->as.expr_stmt.expr);
                else
                    emit_expr(g, s);
            }
            emit_raw(g, ") : (");
            if (node->as.if_stmt.else_block &&
                node->as.if_stmt.else_block->kind == NODE_BLOCK &&
                node->as.if_stmt.else_block->as.block.count == 1) {
                AstNode *s = node->as.if_stmt.else_block->as.block.stmts[0];
                if (s && s->kind == NODE_EXPR_STMT)
                    emit_expr(g, s->as.expr_stmt.expr);
                else
                    emit_expr(g, s);
            }
            emit_raw(g, "))");
            break;
        }

        case NODE_LAMBDA: {
            // C doesn't have lambdas — emit as a generated static function
            // and reference it by name. In a real compiler this would be
            // a closure capture; for now we emit a top-level static.
            // (Full closure support is a future pass.)
            int lbl = new_label(g);
            // We'll emit the lambda body later in a deferred section.
            // For now reference it by the generated name.
            emit_raw(g, "_ci_lambda_%d", lbl);
            break;
        }

        case NODE_PROPAGATE: {
            // result? → if result is Err, return it early
            // Emits as a statement-expression using GCC extension
            int lbl = new_label(g);
            emit_raw(g, "(__extension__({ "
                        "typeof(");
            emit_expr(g, node->as.propagate.expr);
            emit_raw(g, ") _r_%d = (", lbl);
            emit_expr(g, node->as.propagate.expr);
            emit_raw(g, "); "
                        "if (_r_%d.tag == _CI_ERR) return _r_%d; "
                        "_r_%d.ok; }))",
                     lbl, lbl, lbl);
            break;
        }

        case NODE_SIZEOF:
            emit_raw(g, "sizeof(");
            emit_type(g, node->as.size_expr.type);
            emit_raw(g, ")");
            break;

        case NODE_ALIGNOF:
            emit_raw(g, "_Alignof(");
            emit_type(g, node->as.size_expr.type);
            emit_raw(g, ")");
            break;

        case NODE_NEW: {
            // new Point { x: 1.0, y: 2.0 }
            // → (_ci_new(sizeof(Point), &(Point){ .x = 1.0, .y = 2.0 }))
            emit_raw(g, "((%s*)_ci_new(sizeof(%s), &(%s){",
                     node->as.new_expr.type_name,
                     node->as.new_expr.type_name,
                     node->as.new_expr.type_name);
            for (size_t i = 0; i < node->as.new_expr.field_count; i++) {
                if (i > 0) emit_raw(g, ", ");
                AstNode *fn = node->as.new_expr.field_names[i];
                emit_raw(g, ".%s = ", fn->as.ident.name);
                emit_expr(g, node->as.new_expr.field_values[i]);
            }
            emit_raw(g, "}))");
            break;
        }

        case NODE_RANGE: {
            // Ranges only make sense in for loops — shouldn't appear bare
            cg_error(g, node, "range expression outside of for loop");
            break;
        }

        case NODE_MULTI_ASSIGN:
            // Multi-assign handled in stmt context
            emit_expr(g, node->as.multi_assign.value);
            break;

        default:
            cg_error(g, node, "unhandled expression node");
            break;
    }
}

// ─── DEFER STACK ─────────────────────────────────────────────────────────────

static void defer_push(Codegen *g, AstNode *stmt) {
    if (g->defer_count >= g->defer_cap) {
        g->defer_cap *= 2;
        AstNode **newstack = arena_alloc(g->arena,
                                         g->defer_cap * sizeof(AstNode *));
        memcpy(newstack, g->defer_stack,
               g->defer_count * sizeof(AstNode *));
        g->defer_stack = newstack;
    }
    g->defer_stack[g->defer_count++] = stmt;
}

// Emit all deferred statements in reverse order
static void emit_defers(Codegen *g) {
    for (int i = (int)g->defer_count - 1; i >= 0; i--) {
        emit_stmt(g, g->defer_stack[i]);
    }
}

// ─── STATEMENT EMISSION ──────────────────────────────────────────────────────

static void emit_block(Codegen *g, AstNode *node) {
    if (!node || node->kind != NODE_BLOCK) return;
    emit_raw(g, "{\n");
    g->indent++;
    for (size_t i = 0; i < node->as.block.count; i++)
        emit_stmt(g, node->as.block.stmts[i]);
    g->indent--;
    emit_indent(g);
    emit_raw(g, "}");
}

static void emit_stmt(Codegen *g, AstNode *node) {
    if (!node) return;

    switch (node->kind) {

        case NODE_BLOCK:
            emit_indent(g);
            emit_block(g, node);
            emit_raw(g, "\n");
            break;

        case NODE_VAR_DECL: {
            emit_indent(g);
            if (node->as.var_decl.is_const) emit_raw(g, "const ");
            if (node->as.var_decl.type) {
                emit_type_with_name(g, node->as.var_decl.type,
                                    node->as.var_decl.name);
            } else {
                // inferred: use __auto_type (GCC) / auto (C23)
                emit_raw(g, "__auto_type %s", node->as.var_decl.name);
            }
            if (node->as.var_decl.value) {
                emit_raw(g, " = ");
                emit_expr(g, node->as.var_decl.value);
            }
            emit_raw(g, ";\n");
            break;
        }

        case NODE_MULTI_ASSIGN: {
            // let a, b = someFunc();
            // Emit: typeof(rhs) _tmp = rhs; a = _tmp.v0; b = _tmp.v1;
            // For now emit each as __auto_type from a temp call.
            int lbl = new_label(g);
            emitln(g, "/* multi-assign */");
            emit_indent(g);
            emit_raw(g, "__auto_type _ma_%d = ", lbl);
            emit_expr(g, node->as.multi_assign.value);
            emit_raw(g, ";\n");
            for (size_t i = 0; i < node->as.multi_assign.count; i++) {
                emitln(g, "__auto_type %s = _ma_%d.v%zu;",
                       node->as.multi_assign.names[i], lbl, i);
            }
            break;
        }

        case NODE_RETURN: {
            emit_defers(g);
            emit_indent(g);
            if (node->as.ret.value) {
                emit_raw(g, "return ");
                emit_expr(g, node->as.ret.value);
                emit_raw(g, ";\n");
            } else {
                emit_raw(g, "return;\n");
            }
            break;
        }

        case NODE_IF: {
            emit_indent(g);
            emit_raw(g, "if (");
            emit_expr(g, node->as.if_stmt.condition);
            emit_raw(g, ") ");
            emit_block(g, node->as.if_stmt.then_block);

            for (size_t i = 0; i < node->as.if_stmt.else_if_count; i++) {
                emit_raw(g, " else if (");
                emit_expr(g, node->as.if_stmt.else_if_conds[i]);
                emit_raw(g, ") ");
                emit_block(g, node->as.if_stmt.else_if_blocks[i]);
            }

            if (node->as.if_stmt.else_block) {
                emit_raw(g, " else ");
                emit_block(g, node->as.if_stmt.else_block);
            }
            emit_raw(g, "\n");
            break;
        }

        case NODE_WHILE: {
            emit_indent(g);
            emit_raw(g, "while (");
            emit_expr(g, node->as.while_stmt.condition);
            emit_raw(g, ") ");
            emit_block(g, node->as.while_stmt.body);
            emit_raw(g, "\n");
            break;
        }

        case NODE_FOR_RANGE: {
            emit_indent(g);
            emit_raw(g, "for (int32_t %s = ", node->as.for_range.var);
            emit_expr(g, node->as.for_range.from);
            emit_raw(g, "; %s %s ", node->as.for_range.var,
                     node->as.for_range.inclusive ? "<=" : "<");
            emit_expr(g, node->as.for_range.to);
            emit_raw(g, "; %s++) ", node->as.for_range.var);
            emit_block(g, node->as.for_range.body);
            emit_raw(g, "\n");
            break;
        }

        case NODE_FOR_IN: {
            // for item in arr → for (size_t _i = 0; _i < arr.len; _i++)
            int lbl = new_label(g);
            emit_indent(g);
            emit_raw(g, "for (size_t _fi_%d = 0; _fi_%d < (", lbl, lbl);
            emit_expr(g, node->as.for_in.iterable);
            emit_raw(g, ").len; _fi_%d++) {\n", lbl);
            g->indent++;
            emitln(g, "__auto_type %s = (", node->as.for_in.var);
            emit_expr(g, node->as.for_in.iterable);
            emit_raw(g, ").ptr[_fi_%d];\n", lbl);
            // emit the body stmts inline (already in a block)
            if (node->as.for_in.body &&
                node->as.for_in.body->kind == NODE_BLOCK) {
                for (size_t i = 0; i < node->as.for_in.body->as.block.count; i++)
                    emit_stmt(g, node->as.for_in.body->as.block.stmts[i]);
            }
            g->indent--;
            emitln(g, "}");
            break;
        }

        case NODE_FOR_INDEX: {
            int lbl = new_label(g);
            emit_indent(g);
            emit_raw(g, "for (size_t %s = 0; %s < (",
                     node->as.for_index.index_var,
                     node->as.for_index.index_var);
            emit_expr(g, node->as.for_index.iterable);
            emit_raw(g, ").len; %s++) {\n", node->as.for_index.index_var);
            g->indent++;
            emit_indent(g);
            emit_raw(g, "__auto_type %s = (", node->as.for_index.value_var);
            emit_expr(g, node->as.for_index.iterable);
            emit_raw(g, ").ptr[%s];\n", node->as.for_index.index_var);
            if (node->as.for_index.body &&
                node->as.for_index.body->kind == NODE_BLOCK) {
                for (size_t i = 0; i < node->as.for_index.body->as.block.count; i++)
                    emit_stmt(g, node->as.for_index.body->as.block.stmts[i]);
            }
            g->indent--;
            emitln(g, "}");
            (void)lbl;
            break;
        }

        case NODE_FOR_C: {
            emit_indent(g);
            emit_raw(g, "for (");
            // init: suppress the trailing newline/indent
            if (node->as.for_c.init) {
                AstNode *init = node->as.for_c.init;
                if (init->kind == NODE_VAR_DECL) {
                    if (init->as.var_decl.type)
                        emit_type_with_name(g, init->as.var_decl.type,
                                            init->as.var_decl.name);
                    else
                        emit_raw(g, "__auto_type %s", init->as.var_decl.name);
                    if (init->as.var_decl.value) {
                        emit_raw(g, " = ");
                        emit_expr(g, init->as.var_decl.value);
                    }
                } else {
                    emit_expr(g, init);
                }
            }
            emit_raw(g, "; ");
            emit_expr(g, node->as.for_c.condition);
            emit_raw(g, "; ");
            emit_expr(g, node->as.for_c.post);
            emit_raw(g, ") ");
            emit_block(g, node->as.for_c.body);
            emit_raw(g, "\n");
            break;
        }

        case NODE_LOOP: {
            if (node->as.loop_stmt.label)
                emitln(g, "%s_loop:", node->as.loop_stmt.label);
            emit_indent(g);
            emit_raw(g, "for (;;) ");
            emit_block(g, node->as.loop_stmt.body);
            emit_raw(g, "\n");
            break;
        }

        case NODE_MATCH: {
            // Match emits as if-else chain (switch only works for integers)
            int lbl = new_label(g);
            emit_indent(g);
            emit_raw(g, "__auto_type _match_%d = ", lbl);
            emit_expr(g, node->as.match_stmt.subject);
            emit_raw(g, ";\n");

            for (size_t i = 0; i < node->as.match_stmt.arm_count; i++) {
                MatchArm *arm = &node->as.match_stmt.arms[i];
                emit_indent(g);

                int is_wildcard = arm->pattern &&
                                  arm->pattern->kind == NODE_IDENT &&
                                  strcmp(arm->pattern->as.ident.name, "_") == 0;

                if (i == 0 && !is_wildcard) emit_raw(g, "if (");
                else if (!is_wildcard)       emit_raw(g, "else if (");
                else                         emit_raw(g, "else ");

                if (!is_wildcard) {
                    // Range pattern: 1..10
                    if (arm->pattern && arm->pattern->kind == NODE_RANGE) {
                        emit_raw(g, "_match_%d >= ", lbl);
                        emit_expr(g, arm->pattern->as.range.from);
                        emit_raw(g, " && _match_%d %s ", lbl,
                                 arm->pattern->as.range.inclusive ? "<=" : "<");
                        emit_expr(g, arm->pattern->as.range.to);
                    } else {
                        emit_raw(g, "_match_%d == ", lbl);
                        emit_expr(g, arm->pattern);
                    }

                    // Guard
                    if (arm->guard) {
                        emit_raw(g, " && (");
                        emit_expr(g, arm->guard);
                        emit_raw(g, ")");
                    }
                    emit_raw(g, ") ");
                }

                // Arm body
                if (arm->body && arm->body->kind == NODE_BLOCK)
                    emit_block(g, arm->body);
                else {
                    emit_raw(g, "{\n");
                    g->indent++;
                    emit_stmt(g, arm->body);
                    g->indent--;
                    emit_indent(g);
                    emit_raw(g, "}");
                }
                emit_raw(g, "\n");
            }
            break;
        }

        case NODE_BREAK: {
            emit_defers(g);
            emit_indent(g);
            if (node->as.jump.label)
                emit_raw(g, "goto %s_break;\n", node->as.jump.label);
            else
                emit_raw(g, "break;\n");
            break;
        }

        case NODE_CONTINUE: {
            emit_indent(g);
            if (node->as.jump.label)
                emit_raw(g, "goto %s_continue;\n", node->as.jump.label);
            else
                emit_raw(g, "continue;\n");
            break;
        }

        case NODE_DEFER: {
            // Push onto defer stack; emit on return/end of scope
            defer_push(g, node->as.defer_stmt.stmt);
            break;
        }

        case NODE_PANIC: {
            emit_indent(g);
            emit_raw(g, "_ci_panic(");
            emit_expr(g, node->as.panic_stmt.msg);
            emit_raw(g, ");\n");
            break;
        }

        case NODE_ASM_BLOCK: {
            // Emit GCC inline asm — wrap raw body in asm volatile
            emitln(g, "__asm__ volatile (");
            g->indent++;
            // Split lines and quote each one
            char *body = strdup(node->as.asm_block.body);
            char *line_ptr = strtok(body, "\n");
            while (line_ptr) {
                // Trim leading whitespace
                while (*line_ptr == ' ' || *line_ptr == '\t') line_ptr++;
                if (*line_ptr != '\0' && *line_ptr != ';')
                    emitln(g, "\"%s\\n\\t\"", line_ptr);
                line_ptr = strtok(NULL, "\n");
            }
            free(body);
            g->indent--;
            emitln(g, ");");
            break;
        }

        case NODE_DELETE: {
            emit_indent(g);
            emit_raw(g, "free(");
            emit_expr(g, node->as.delete_expr.ptr);
            emit_raw(g, ");\n");
            break;
        }

        case NODE_EXPR_STMT: {
            emit_indent(g);
            emit_expr(g, node->as.expr_stmt.expr);
            emit_raw(g, ";\n");
            break;
        }

        default:
            emit_expr(g, node);
            break;
    }
}

// ─── FUNCTION EMISSION ───────────────────────────────────────────────────────

static void emit_func_signature(Codegen *g, AstNode *node) {
    // Return type
    if (node->as.func_decl.return_type)
        emit_type(g, node->as.func_decl.return_type);
    else
        emit_raw(g, "int");   // inferred default (e.g. main)

    emit_raw(g, " %s(", node->as.func_decl.name);

    // Parameters
    if (node->as.func_decl.param_count == 0) {
        emit_raw(g, "void");
    } else {
        for (size_t i = 0; i < node->as.func_decl.param_count; i++) {
            if (i > 0) emit_raw(g, ", ");
            AstParam *p = &node->as.func_decl.params[i];

            // self → pointer to the enclosing struct type
            if (strcmp(p->name, "self") == 0) {
                emit_raw(g, "void* self");  // type resolved later by type checker
            } else if (p->type) {
                emit_type_with_name(g, p->type, p->name);
            } else {
                emit_raw(g, "void* %s", p->name);
            }
        }
    }
    emit_raw(g, ")");
}

static void emit_func_decl(Codegen *g, AstNode *node) {
    // Save + reset defer stack for this function
    AstNode **saved_stack = g->defer_stack;
    size_t    saved_count = g->defer_count;
    size_t    saved_cap   = g->defer_cap;

    g->defer_cap   = 8;
    g->defer_count = 0;
    g->defer_stack = arena_alloc(g->arena, g->defer_cap * sizeof(AstNode *));
    g->current_return_type = node->as.func_decl.return_type;

    emit_func_signature(g, node);
    emit_raw(g, "\n");
    emit_indent(g);
    emit_raw(g, "{\n");
    g->indent++;

    if (node->as.func_decl.body &&
        node->as.func_decl.body->kind == NODE_BLOCK) {
        for (size_t i = 0; i < node->as.func_decl.body->as.block.count; i++)
            emit_stmt(g, node->as.func_decl.body->as.block.stmts[i]);
    }

    // Emit any remaining deferred statements at end of function
    emit_defers(g);

    g->indent--;
    emitln(g, "}");

    // Restore defer stack
    g->defer_stack = saved_stack;
    g->defer_count = saved_count;
    g->defer_cap   = saved_cap;
}

// ─── ASM FUNCTION EMISSION ───────────────────────────────────────────────────

static void emit_asm_func_decl(Codegen *g, AstNode *node) {
    // Return type
    if (node->as.asm_func_decl.return_reg)
        emit_raw(g, "uint64_t");  // registers are always 64-bit
    else
        emit_raw(g, "void");

    emit_raw(g, " %s(", node->as.asm_func_decl.name);

    if (node->as.asm_func_decl.param_count == 0) {
        emit_raw(g, "void");
    } else {
        for (size_t i = 0; i < node->as.asm_func_decl.param_count; i++) {
            if (i > 0) emit_raw(g, ", ");
            AsmParam *p = &node->as.asm_func_decl.params[i];
            emit_type(g, p->type);
            emit_raw(g, " %s", p->name);
        }
    }
    emit_raw(g, ")\n{\n");
    g->indent++;

    // Build GCC inline asm with register constraints
    emit_indent(g);
    if (node->as.asm_func_decl.return_reg)
        emit_raw(g, "register uint64_t _ret __asm__(\"%s\");\n",
                 node->as.asm_func_decl.return_reg);

    // Bind each param to its register
    for (size_t i = 0; i < node->as.asm_func_decl.param_count; i++) {
        AsmParam *p = &node->as.asm_func_decl.params[i];
        emit_indent(g);
        emit_raw(g, "register ");
        emit_type(g, p->type);
        emit_raw(g, " _r_%s __asm__(\"%s\") = %s;\n",
                 p->name, p->reg, p->name);
    }

    // Emit raw asm block
    emit_indent(g);
    emit_raw(g, "__asm__ volatile (\n");
    g->indent++;

    char *body = strdup(node->as.asm_func_decl.body);
    char *line_ptr = strtok(body, "\n");
    while (line_ptr) {
        while (*line_ptr == ' ' || *line_ptr == '\t') line_ptr++;
        // Skip asm-style comments and clobbers directive
        if (*line_ptr != '\0' && *line_ptr != ';' &&
            strncmp(line_ptr, "clobbers", 8) != 0)
            emitln(g, "\"%s\\n\\t\"", line_ptr);
        line_ptr = strtok(NULL, "\n");
    }
    free(body);

    // Build constraint string
    emit_indent(g);
    if (node->as.asm_func_decl.return_reg)
        emit_raw(g, ": \"=r\" (_ret)\n");
    else
        emit_raw(g, ":\n");

    emit_indent(g);
    emit_raw(g, ": ");
    for (size_t i = 0; i < node->as.asm_func_decl.param_count; i++) {
        AsmParam *p = &node->as.asm_func_decl.params[i];
        if (i > 0) emit_raw(g, ", ");
        emit_raw(g, "\"r\" (_r_%s)", p->name);
    }
    emit_raw(g, "\n");

    // Clobbers
    emit_indent(g);
    emit_raw(g, ": \"memory\"");
    if (node->as.asm_func_decl.clobber_count > 0) {
        for (size_t i = 0; i < node->as.asm_func_decl.clobber_count; i++)
            emit_raw(g, ", \"%s\"", node->as.asm_func_decl.clobbers[i]);
    }
    emit_raw(g, "\n");

    g->indent--;
    emitln(g, ");");

    if (node->as.asm_func_decl.return_reg)
        emitln(g, "return _ret;");

    g->indent--;
    emitln(g, "}");
}

// ─── STRUCT EMISSION ─────────────────────────────────────────────────────────

static void emit_struct_decl(Codegen *g, AstNode *node) {
    emitln(g, "typedef struct %s {", node->as.struct_decl.name);
    g->indent++;

    // Embedded struct fields (expanded inline)
    for (size_t i = 0; i < node->as.struct_decl.embed_count; i++) {
        emitln(g, "/* embed %s */", node->as.struct_decl.embeds[i]);
        // In full impl: copy fields from embedded struct.
        // For now: emit an anonymous struct field.
        emitln(g, "struct %s _embed_%s;",
               node->as.struct_decl.embeds[i],
               node->as.struct_decl.embeds[i]);
    }

    // Fields
    for (size_t i = 0; i < node->as.struct_decl.field_count; i++) {
        AstField *f = &node->as.struct_decl.fields[i];
        emit_indent(g);
        emit_type_with_name(g, f->type, f->name);
        emit_raw(g, ";\n");
    }

    g->indent--;
    emitln(g, "} %s;", node->as.struct_decl.name);
    emit_raw(g, "\n");

    // Methods: emit as free functions: RetType StructName_methodName(StructName* self, ...)
    for (size_t i = 0; i < node->as.struct_decl.method_count; i++) {
        AstNode *method = node->as.struct_decl.methods[i];
        if (!method || method->kind != NODE_FUNC_DECL) continue;

        // Prefix method name with struct name
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s_%s",
                 node->as.struct_decl.name,
                 method->as.func_decl.name);

        // Patch 'self' param type to point to this struct
        for (size_t j = 0; j < method->as.func_decl.param_count; j++) {
            AstParam *p = &method->as.func_decl.params[j];
            if (strcmp(p->name, "self") == 0) {
                AstType *self_type = type_alloc(g->arena, TY_POINTER);
                AstType *inner     = type_alloc(g->arena, TY_NAMED);
                inner->name        = node->as.struct_decl.name;
                self_type->inner   = inner;
                p->type            = self_type;
            }
        }

        char *saved_name = method->as.func_decl.name;
        method->as.func_decl.name = mangled;
        emit_func_decl(g, method);
        method->as.func_decl.name = saved_name;
        emit_raw(g, "\n");
    }
}

// ─── ENUM EMISSION ───────────────────────────────────────────────────────────

static void emit_enum_decl(Codegen *g, AstNode *node) {
    int is_tagged = 0;
    for (size_t i = 0; i < node->as.enum_decl.variant_count; i++) {
        if (node->as.enum_decl.variants[i].field_count > 0) {
            is_tagged = 1;
            break;
        }
    }

    if (!is_tagged) {
        // Simple enum → C enum
        emitln(g, "typedef enum {");
        g->indent++;
        for (size_t i = 0; i < node->as.enum_decl.variant_count; i++) {
            AstVariant *v = &node->as.enum_decl.variants[i];
            emit_indent(g);
            emit_raw(g, "%s__%s", node->as.enum_decl.name, v->name);
            if (v->has_value && v->explicit_value) {
                emit_raw(g, " = ");
                emit_expr(g, v->explicit_value);
            }
            emit_raw(g, ",\n");
        }
        g->indent--;
        emitln(g, "} %s;", node->as.enum_decl.name);
    } else {
        // Tagged union enum → C tagged union
        emitln(g, "typedef struct %s {", node->as.enum_decl.name);
        g->indent++;
        emitln(g, "enum {");
        g->indent++;
        for (size_t i = 0; i < node->as.enum_decl.variant_count; i++) {
            emitln(g, "%s__%s,",
                   node->as.enum_decl.name,
                   node->as.enum_decl.variants[i].name);
        }
        g->indent--;
        emitln(g, "} tag;");
        emitln(g, "union {");
        g->indent++;
        for (size_t i = 0; i < node->as.enum_decl.variant_count; i++) {
            AstVariant *v = &node->as.enum_decl.variants[i];
            if (v->field_count == 0) continue;
            emitln(g, "struct {");
            g->indent++;
            for (size_t j = 0; j < v->field_count; j++) {
                AstParam *f = &v->fields[j];
                emit_indent(g);
                emit_type_with_name(g, f->type, f->name);
                emit_raw(g, ";\n");
            }
            g->indent--;
            emitln(g, "} %s;", v->name);
        }
        g->indent--;
        emitln(g, "} as;");
        g->indent--;
        emitln(g, "} %s;", node->as.enum_decl.name);
    }
    emit_raw(g, "\n");
}

// ─── RUNTIME PREAMBLE ────────────────────────────────────────────────────────
// Emitted at the top of every generated .c file.

static void emit_preamble(Codegen *g) {
    emit(g,
        "/* Generated by the Ciren compiler — do not edit */\n"
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "#include <stdlib.h>\n"
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "\n"
        "/* ── Ciren runtime types ─────────────────── */\n"
        "\n"
        "typedef struct { const char *ptr; size_t len; } _ci_str;\n"
        "typedef struct { void *ptr; size_t len; }       _ci_slice;\n"
        "\n"
        "typedef enum  { _CI_OK, _CI_ERR }               _ci_result_tag;\n"
        "typedef struct { _ci_result_tag tag; union { void *ok; _ci_str err; }; } _ci_result;\n"
        "\n"
        "/* ── Ciren runtime functions ─────────────── */\n"
        "\n"
        "static inline void* _ci_new(size_t size, const void *init) {\n"
        "    void *p = malloc(size);\n"
        "    if (!p) { fprintf(stderr, \"[ciren] out of memory\\n\"); exit(1); }\n"
        "    if (init) memcpy(p, init, size);\n"
        "    return p;\n"
        "}\n"
        "\n"
        "static inline _ci_slice _ci_slice_make(void *ptr, size_t from, size_t to) {\n"
        "    _ci_slice s;\n"
        "    s.ptr = (char*)ptr + from;\n"
        "    s.len = (to == (size_t)-1) ? 0 : (to - from);\n"
        "    return s;\n"
        "}\n"
        "\n"
        "static inline void _ci_panic(_ci_str msg) {\n"
        "    fprintf(stderr, \"[ciren] panic: %%.*s\\n\", (int)msg.len, msg.ptr);\n"
        "    exit(1);\n"
        "}\n"
        "\n"
        "/* ── Ciren using stdio ───────────────────── */\n"
        "/* (real module system replaces this stub)  */\n"
        "\n"
    );
}

// ─── FORWARD DECLARATIONS PASS ───────────────────────────────────────────────
// Emit C forward declarations for every function so any order works.

static void emit_forward_decls(Codegen *g, AstNode *program) {
    emit(g, "/* ── Forward declarations ───────────────── */\n\n");
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl) continue;

        if (decl->kind == NODE_FUNC_DECL) {
            if (decl->as.func_decl.access != ACCESS_PUBLIC)
                emit_raw(g, "static ");
            emit_func_signature(g, decl);
            emit_raw(g, ";\n");
        } else if (decl->kind == NODE_ASM_FUNC_DECL) {
            if (decl->as.asm_func_decl.access != ACCESS_PUBLIC)
                emit_raw(g, "static ");
            if (decl->as.asm_func_decl.return_reg)
                emit_raw(g, "uint64_t %s(", decl->as.asm_func_decl.name);
            else
                emit_raw(g, "void %s(", decl->as.asm_func_decl.name);
            if (decl->as.asm_func_decl.param_count == 0) {
                emit_raw(g, "void");
            } else {
                for (size_t j = 0; j < decl->as.asm_func_decl.param_count; j++) {
                    if (j > 0) emit_raw(g, ", ");
                    AsmParam *p = &decl->as.asm_func_decl.params[j];
                    emit_type(g, p->type);
                    emit_raw(g, " %s", p->name);
                }
            }
            emit_raw(g, ");\n");
        }
    }
    emit_raw(g, "\n");
}

// ─── MAIN PROGRAM EMISSION ───────────────────────────────────────────────────

static void emit_program(Codegen *g, AstNode *program) {
    emit_preamble(g);

    // Emit struct/enum type definitions first (types before functions)
    emit(g, "/* ── Type definitions ───────────────────── */\n\n");
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl) continue;
        if (decl->kind == NODE_STRUCT_DECL)
            emit_struct_decl(g, decl);
        else if (decl->kind == NODE_ENUM_DECL)
            emit_enum_decl(g, decl);
    }

    // Emit global variables and constants
    emit(g, "/* ── Globals ────────────────────────────── */\n\n");
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl || decl->kind != NODE_VAR_DECL) continue;

        if (decl->as.var_decl.is_const) emit_raw(g, "const ");

        // Global scope: file-static unless public
        if (decl->as.var_decl.access != ACCESS_PUBLIC)
            emit_raw(g, "static ");

        if (decl->as.var_decl.type)
            emit_type_with_name(g, decl->as.var_decl.type,
                                decl->as.var_decl.name);
        else
            emit_raw(g, "__auto_type %s", decl->as.var_decl.name);

        if (decl->as.var_decl.value) {
            emit_raw(g, " = ");
            emit_expr(g, decl->as.var_decl.value);
        }
        emit_raw(g, ";\n");
    }
    emit_raw(g, "\n");

    // Forward declarations (enables order-independence in C output too)
    emit_forward_decls(g, program);

    // Emit functions and asm functions
    emit(g, "/* ── Functions ──────────────────────────── */\n\n");
    for (size_t i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.decls[i];
        if (!decl) continue;

        if (decl->kind == NODE_FUNC_DECL) {
            // static if not public
            if (decl->as.func_decl.access != ACCESS_PUBLIC)
                emit_raw(g, "static ");
            emit_func_decl(g, decl);
            emit_raw(g, "\n");
        } else if (decl->kind == NODE_ASM_FUNC_DECL) {
            if (decl->as.asm_func_decl.access != ACCESS_PUBLIC)
                emit_raw(g, "static ");
            emit_asm_func_decl(g, decl);
            emit_raw(g, "\n");
        }
    }
}

// ─── ENTRY POINT ─────────────────────────────────────────────────────────────

Codegen *codegen_create(Arena *arena, Resolver *resolver,
                        FILE *out, const char *filename) {
    Codegen *g        = arena_alloc(arena, sizeof(Codegen));
    g->arena          = arena;
    g->resolver       = resolver;
    g->out            = out;
    g->filename       = filename;
    g->indent         = 0;
    g->had_error      = 0;
    g->label_counter  = 0;
    g->in_asm_func    = 0;
    g->defer_cap      = 16;
    g->defer_count    = 0;
    g->defer_stack    = arena_alloc(arena, 16 * sizeof(AstNode *));
    g->current_return_type = NULL;
    return g;
}

int codegen_run(Codegen *g, AstNode *program) {
    if (!program || program->kind != NODE_PROGRAM) {
        fprintf(stderr, "[ciren] codegen: expected program node\n");
        return 0;
    }
    emit_program(g, program);
    return !g->had_error;
}