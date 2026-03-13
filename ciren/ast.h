// ciren/ast.h

#ifndef CIREN_AST_H
#define CIREN_AST_H

#include "lexer.h"
#include <stddef.h>
#include <stdint.h>

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────

typedef struct AstNode     AstNode;
typedef struct AstType     AstType;
typedef struct AstParam    AstParam;
typedef struct AsmParam    AsmParam;
typedef struct AstField    AstField;
typedef struct AstVariant  AstVariant;
typedef struct MatchArm    MatchArm;

// ─── ACCESS MODIFIER ─────────────────────────────────────────────────────────

typedef enum {
    ACCESS_PUBLIC,
    ACCESS_PRIVATE,
    ACCESS_INTERNAL,
} AccessMod;

// ─── TYPE NODES ──────────────────────────────────────────────────────────────

typedef enum {
    TY_INT, TY_UINT,
    TY_I8,  TY_U8,
    TY_I16, TY_U16,
    TY_I64, TY_U64,
    TY_F32, TY_F64,
    TY_BOOL, TY_CHAR,
    TY_STR,  TY_CSTR,
    TY_VOID, TY_ANY,
    TY_NAMED,           // user-defined type: MyStruct, Vec2, etc.
    TY_POINTER,         // *T
    TY_NULLABLE_PTR,    // ?*T
    TY_ARRAY,           // [T; N]
    TY_SLICE,           // []T
    TY_FUNC_PTR,        // (A, B) -> R
    TY_GENERIC,         // T<A, B>
    TY_INFERRED,        // type not written — compiler figures it out
} TypeKind;

struct AstType {
    TypeKind kind;

    // TY_NAMED, TY_GENERIC
    char *name;

    // TY_POINTER, TY_NULLABLE_PTR, TY_SLICE
    AstType *inner;

    // TY_ARRAY
    AstType  *elem_type;
    AstNode  *array_size;   // constant expression

    // TY_FUNC_PTR
    AstType **param_types;
    size_t    param_count;
    AstType  *return_type;

    // TY_GENERIC
    AstType **type_args;
    size_t    type_arg_count;
};

// ─── AST NODE KINDS ──────────────────────────────────────────────────────────

typedef enum {

    // ── Top-level ────────────────────────────────
    NODE_PROGRAM,           // root: list of top-level declarations
    NODE_USING,             // using stdio; / using net.http as http;
    NODE_MODULE,            // module net.http;

    // ── Declarations ─────────────────────────────
    NODE_FUNC_DECL,         // public void greet(name: str) { ... }
    NODE_ASM_FUNC_DECL,     // public asm doSysCall { mov rax, 1 \n syscall }
    NODE_STRUCT_DECL,       // public struct Vec2 { ... }
    NODE_UNION_DECL,        // public union Val { ... }
    NODE_ENUM_DECL,         // public enum Direction { ... }
    NODE_ENUM_LITERAL,      // Shape::Circle(5.0)
    NODE_ENUM_PATTERN,      // Shape::Circle(r) in match arm
    NODE_INTERFACE_DECL,    // public interface Drawable { ... }
    NODE_IMPL_DECL,         // impl Drawable for Circle { ... }
    NODE_CONST_DECL,        // const PI: f64 = 3.14;
    NODE_VAR_DECL,          // let x = 10; / let x: int = 10;

    // ── Statements ───────────────────────────────
    NODE_BLOCK,             // { stmt; stmt; ... }
    NODE_RETURN,            // return expr;
    NODE_IF,                // if expr { } else if { } else { }
    NODE_WHILE,             // while expr { }
    NODE_FOR_RANGE,         // for i in 0..10 { }
    NODE_FOR_IN,            // for item in array { }
    NODE_FOR_INDEX,         // for i, item in array { }
    NODE_FOR_C,             // for (let i = 0; i < 10; i++) { }
    NODE_LOOP,              // loop { }
    NODE_MATCH,             // match x { ... }
    NODE_BREAK,             // break; / break outer;
    NODE_CONTINUE,          // continue; / continue outer;
    NODE_DEFER,             // defer stmt;
    NODE_PANIC,             // panic("msg");
    NODE_ASM_BLOCK,         // asm { mov rax, 1 }
    NODE_EXPR_STMT,         // expression used as statement

    // ── Expressions ──────────────────────────────
    NODE_INT_LIT,           // 42
    NODE_FLOAT_LIT,         // 3.14
    NODE_STRING_LIT,        // "hello"
    NODE_CSTRING_LIT,       // c"hello"
    NODE_CHAR_LIT,          // 'A'
    NODE_BOOL_LIT,          // true / false
    NODE_NULL_LIT,          // null
    NODE_IDENT,             // myVar
    NODE_BINARY,            // a + b, a == b, etc.
    NODE_UNARY,             // -x, !x, *x, &x
    NODE_ASSIGN,            // x = expr / x += expr
    NODE_CALL,              // greet("world")
    NODE_INDEX,             // arr[i]
    NODE_FIELD,             // obj.field
    NODE_CAST,              // x as int
    NODE_RANGE,             // 0..10 / 0..=10
    NODE_STRUCT_LITERAL,    // Vec2 { x: 1.0, y: 2.0 }
    NODE_INFERRED_LITERAL,  // .{ x: 1.0, y: 2.0 }
    NODE_ARRAY_LITERAL,     // [1, 2, 3]
    NODE_SLICE_EXPR,        // arr[2..5]
    NODE_IF_EXPR,           // if x > 0 { "pos" } else { "neg" }
    NODE_LAMBDA,            // (x: int) => x * x
    NODE_MULTI_ASSIGN,      // let a, b = func()
    NODE_PROPAGATE,         // expr?
    NODE_SIZEOF,            // sizeof(T)
    NODE_ALIGNOF,           // alignof(T)
    NODE_ADDRESS_OF,        // &x
    NODE_DEREF,             // *x
    NODE_NEW,               // new Point { ... }
    NODE_DELETE,            // delete ptr

} NodeKind;

// ─── PARAMETER STRUCTS ───────────────────────────────────────────────────────

struct AstParam {
    char    *name;
    AstType *type;
    AstNode *default_value;   // NULL if no default
    int      is_variadic;     // ...T
};

struct AsmParam {
    char    *name;
    AstType *type;
    char    *reg;             // e.g. "rdi", "rsi"
};

struct AstField {
    char    *name;
    AstType *type;
    AstNode *default_value;
};

struct AstVariant {
    char      *name;
    int        has_value;
    AstNode   *explicit_value;   // enum Foo { X = 42 }
    // Tagged union fields
    AstParam  *fields;
    size_t     field_count;
};

struct MatchArm {
    AstNode  *pattern;       // literal, ident, range, enum variant
    char     *bind_name;     // "n" in "n if n < 0"
    AstNode  *guard;         // if expr
    AstNode  *body;          // expr or block
};

// ─── THE AST NODE ────────────────────────────────────────────────────────────

struct AstNode {
    NodeKind kind;
    int      line;
    int      col;

    union {

        // NODE_PROGRAM
        struct {
            AstNode **decls;
            size_t    count;
        } program;

        // NODE_USING
        struct {
            char  *path;         // "stdio", "net.http", "myfile.ci"
            char  *alias;        // NULL if no 'as'
            char **symbols;      // NULL if no { x, y }
            size_t symbol_count;
            int    is_reexport;  // public using ...
        } using_decl;

        // NODE_MODULE
        struct {
            char *path;
        } module_decl;

        // NODE_FUNC_DECL
        struct {
            AccessMod  access;
            char      *name;
            AstType   *return_type;  // NULL = inferred
            AstParam  *params;
            size_t     param_count;
            char     **generic_params;
            size_t     generic_count;
            AstNode   *body;         // NODE_BLOCK
            int        is_method;    // has 'self' param
            char     **attributes;   // [inline], [extern("C")], etc.
            size_t     attr_count;
        } func_decl;

        // NODE_ASM_FUNC_DECL
        struct {
            AccessMod  access;
            char      *name;
            AsmParam  *params;
            size_t     param_count;
            char      *return_reg;   // NULL if void
            char     **clobbers;
            size_t     clobber_count;
            char      *body;         // raw asm text preserved verbatim
        } asm_func_decl;

        // NODE_STRUCT_DECL
        struct {
            AccessMod  access;
            char      *name;
            char     **generic_params;
            size_t     generic_count;
            AstField  *fields;
            size_t     field_count;
            AstNode  **methods;
            size_t     method_count;
            char     **embeds;       // embed Point
            size_t     embed_count;
            char     **attributes;
            size_t     attr_count;
        } struct_decl;

        // NODE_ENUM_DECL
        struct {
            AccessMod   access;
            char       *name;
            char      **generic_params;
            size_t      generic_count;
            AstVariant *variants;
            size_t      variant_count;
        } enum_decl;

        // NODE_ENUM_LITERAL
        struct {
            char    *enum_name;
            char    *variant;
            AstNode **args;
            size_t   arg_count;
        } enum_lit;

        // NODE_ENUM_PATTERN
        struct {
            char   *enum_name;
            char   *variant;
            char  **bindings;
            size_t  binding_count;
        } enum_pattern;

        // NODE_INTERFACE_DECL
        struct {
            AccessMod  access;
            char      *name;
            char     **parents;       // : Drawable, Serializable
            size_t     parent_count;
            AstNode  **methods;       // signatures only (no body)
            size_t     method_count;
        } interface_decl;

        // NODE_IMPL_DECL
        struct {
            char      *interface_name;  // "Drawable"
            char      *type_name;       // "Circle"
            AstNode  **methods;         // full function bodies
            size_t     method_count;
        } impl_decl;

        // NODE_CONST_DECL / NODE_VAR_DECL
        struct {
            AccessMod  access;
            char      *name;
            AstType   *type;     // NULL = inferred
            AstNode   *value;    // NULL for uninitialized var
            int        is_const;
        } var_decl;

        // NODE_BLOCK
        struct {
            AstNode **stmts;
            size_t    count;
        } block;

        // NODE_RETURN
        struct {
            AstNode *value;   // NULL for bare return
        } ret;

        // NODE_IF / NODE_IF_EXPR
        struct {
            AstNode  *condition;
            AstNode  *then_block;
            AstNode **else_if_conds;
            AstNode **else_if_blocks;
            size_t    else_if_count;
            AstNode  *else_block;    // NULL if no else
        } if_stmt;

        // NODE_WHILE
        struct {
            AstNode *condition;
            AstNode *body;
        } while_stmt;

        // NODE_FOR_RANGE
        struct {
            char    *var;
            AstNode *from;
            AstNode *to;
            int      inclusive;   // ..= vs ..
            AstNode *body;
        } for_range;

        // NODE_FOR_IN
        struct {
            char    *var;
            AstNode *iterable;
            AstNode *body;
        } for_in;

        // NODE_FOR_INDEX
        struct {
            char    *index_var;
            char    *value_var;
            AstNode *iterable;
            AstNode *body;
        } for_index;

        // NODE_FOR_C
        struct {
            AstNode *init;
            AstNode *condition;
            AstNode *post;
            AstNode *body;
        } for_c;

        // NODE_LOOP
        struct {
            char    *label;    // NULL if unlabeled
            AstNode *body;
        } loop_stmt;

        // NODE_MATCH
        struct {
            AstNode   *subject;
            MatchArm  *arms;
            size_t     arm_count;
        } match_stmt;

        // NODE_BREAK / NODE_CONTINUE
        struct {
            char *label;    // NULL if no label
        } jump;

        // NODE_DEFER
        struct {
            AstNode *stmt;
        } defer_stmt;

        // NODE_PANIC
        struct {
            AstNode *msg;
        } panic_stmt;

        // NODE_ASM_BLOCK
        struct {
            char *body;      // raw asm text preserved verbatim
        } asm_block;

        // NODE_EXPR_STMT
        struct {
            AstNode *expr;
        } expr_stmt;

        // ── Literals ──────────────────────────────
        struct { int64_t value;  } int_lit;
        struct { double  value;  } float_lit;
        struct { char   *value;  } string_lit;   // heap copy
        struct { char   *value;  } cstring_lit;
        struct { char    value;  } char_lit;
        struct { int     value;  } bool_lit;

        // NODE_IDENT
        struct {
            char *name;
        } ident;

        // NODE_BINARY
        struct {
            TokenType  op;
            AstNode   *left;
            AstNode   *right;
        } binary;

        // NODE_UNARY
        struct {
            TokenType  op;
            AstNode   *operand;
            int        postfix;   // x++ vs ++x
        } unary;

        // NODE_ASSIGN
        struct {
            TokenType  op;     // TOK_ASSIGN, TOK_PLUS_ASSIGN, etc.
            AstNode   *target;
            AstNode   *value;
        } assign;

        // NODE_CALL
        struct {
            AstNode  *callee;
            AstNode **args;
            size_t    arg_count;
            AstType **type_args;   // explicit generic: max<int>(...)
            size_t    type_arg_count;
        } call;

        // NODE_INDEX
        struct {
            AstNode *target;
            AstNode *index;
        } index;

        // NODE_FIELD
        struct {
            AstNode *target;
            char    *field;
        } field;

        // NODE_CAST
        struct {
            AstNode *expr;
            AstType *type;
        } cast;

        // NODE_RANGE
        struct {
            AstNode *from;
            AstNode *to;
            int      inclusive;
        } range;

        // NODE_STRUCT_LITERAL
        struct {
            char     *type_name;    // NULL for inferred .{ }
            AstNode **field_names;  // NODE_IDENT
            AstNode **field_values;
            size_t    field_count;
        } struct_lit;

        // NODE_ARRAY_LITERAL
        struct {
            AstNode **elems;
            size_t    count;
        } array_lit;

        // NODE_SLICE_EXPR
        struct {
            AstNode *target;
            AstNode *from;      // NULL = start
            AstNode *to;        // NULL = end
            int      inclusive;
        } slice_expr;

        // NODE_LAMBDA
        struct {
            AstParam *params;
            size_t    param_count;
            AstType  *return_type;  // NULL = inferred
            AstNode  *body;         // block OR single expr (fat arrow)
            int       is_expr;      // 1 = => shorthand
        } lambda;

        // NODE_MULTI_ASSIGN
        struct {
            char    **names;
            size_t    count;
            AstNode  *value;    // rhs — usually a call
        } multi_assign;

        // NODE_PROPAGATE
        struct {
            AstNode *expr;
        } propagate;

        // NODE_SIZEOF / NODE_ALIGNOF
        struct {
            AstType *type;
        } size_expr;

        // NODE_NEW
        struct {
            char     *type_name;
            AstNode **field_names;
            AstNode **field_values;
            size_t    field_count;
        } new_expr;

        // NODE_DELETE
        struct {
            AstNode *ptr;
        } delete_expr;

    } as;
};

// ─── ALLOCATOR ───────────────────────────────────────────────────────────────
// Simple arena — nodes are never individually freed during compilation.

typedef struct {
    char  *base;
    size_t used;
    size_t capacity;
} Arena;

Arena  *arena_create(size_t capacity);
void   *arena_alloc(Arena *a, size_t size);
void    arena_destroy(Arena *a);

AstNode *node_alloc(Arena *a, NodeKind kind, int line, int col);
AstType *type_alloc(Arena *a, TypeKind kind);

// ─── DEBUG ───────────────────────────────────────────────────────────────────

void ast_print(AstNode *node, int indent);

#endif // CIREN_AST_H