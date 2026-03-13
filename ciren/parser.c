// ciren/parser.c

#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Grow an arena-allocated array when count reaches cap
#define ARR_GROW(arena, arr, count, cap, T) do { \
    if ((count) >= (cap)) { \
        size_t _nc = (cap) * 2; \
        T *_na = arena_alloc((arena), _nc * sizeof(T)); \
        memcpy(_na, (arr), (count) * sizeof(T)); \
        (arr) = _na; (cap) = _nc; \
    } \
} while(0)

// ─── HELPERS ─────────────────────────────────────────────────────────────────

static void error_at(Parser *p, Token t, const char *msg) {
    if (p->panic_mode) return;
    p->panic_mode = 1;
    p->had_error  = 1;
    fprintf(stderr, "[%s:%d:%d] error: %s",
        p->filename, t.line, t.col, msg);
    if (t.type == TOK_EOF)
        fprintf(stderr, " (at end of file)");
    else
        fprintf(stderr, " near '%.*s'", (int)t.length, t.start);
    fprintf(stderr, "\n");
}

static void error(Parser *p, const char *msg) {
    error_at(p, p->current, msg);
}

static Token advance_parser(Parser *p) {
    p->previous = p->current;
    p->current  = lexer_next(p->lexer);
    if (p->current.type == TOK_ERROR)
        error(p, "unexpected token");
    return p->previous;
}

static int check(Parser *p, TokenType t) {
    return p->current.type == t;
}

static int match_tok(Parser *p, TokenType t) {
    if (!check(p, t)) return 0;
    advance_parser(p);
    return 1;
}

static Token expect(Parser *p, TokenType t, const char *msg) {
    if (!check(p, t)) { error(p, msg); return p->current; }
    return advance_parser(p);
}

// Duplicate a token's text into arena memory as a null-terminated string
static char *tok_dup(Parser *p, Token t) {
    char *s = arena_alloc(p->arena, t.length + 1);
    memcpy(s, t.start, t.length);
    s[t.length] = '\0';
    return s;
}

static char *str_dup_arena(Parser *p, const char *src) {
    size_t len = strlen(src);
    char *s = arena_alloc(p->arena, len + 1);
    memcpy(s, src, len + 1);
    return s;
}

// Synchronise after an error — skip to next safe point
static void synchronize(Parser *p) {
    p->panic_mode = 0;
    while (p->current.type != TOK_EOF) {
        if (p->previous.type == TOK_RBRACE) return;
        switch (p->current.type) {
            case TOK_PUBLIC:
            case TOK_PRIVATE:
            case TOK_INTERNAL:
            case TOK_USING:
            case TOK_UNION:
            case TOK_STRUCT:
            case TOK_ENUM:
            case TOK_INTERFACE:
            case TOK_RETURN:
                return;
            default: break;
        }
        advance_parser(p);
    }
}

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────

static AstNode *parse_stmt(Parser *p);
static AstNode *parse_expr(Parser *p);
static AstNode *parse_block(Parser *p);
static AstType *parse_type(Parser *p);
static AstNode *parse_declaration(Parser *p);

// ─── INIT ────────────────────────────────────────────────────────────────────

void parser_init(Parser *p, Lexer *l, Arena *a, const char *filename) {
    p->lexer      = l;
    p->arena      = a;
    p->had_error  = 0;
    p->panic_mode = 0;
    p->no_struct_lit = 0;
    p->filename   = filename;
    advance_parser(p);   // prime the first token
}

// ─── TYPE PARSER ─────────────────────────────────────────────────────────────

static AstType *parse_type(Parser *p) {
    AstType *t = type_alloc(p->arena, TY_INFERRED);

    // Nullable pointer prefix: ?*
    int nullable = 0;
    if (match_tok(p, TOK_QUESTION)) nullable = 1;

    // Pointer: *T
    if (match_tok(p, TOK_STAR)) {
        t->kind  = nullable ? TY_NULLABLE_PTR : TY_POINTER;
        t->inner = parse_type(p);
        return t;
    }

    // Slice: []T
    if (match_tok(p, TOK_LBRACKET)) {
        if (match_tok(p, TOK_RBRACKET)) {
            t->kind  = TY_SLICE;
            t->inner = parse_type(p);
            return t;
        }
        // Fixed array: [T; N]
        t->kind      = TY_ARRAY;
        t->elem_type = parse_type(p);
        expect(p, TOK_SEMICOLON, "expected ';' in array type");
        t->array_size = parse_expr(p);
        expect(p, TOK_RBRACKET, "expected ']' after array size");
        return t;
    }

    // Function pointer: (A, B) -> R
    if (match_tok(p, TOK_LPAREN)) {
        t->kind = TY_FUNC_PTR;
        size_t cap = 4;
        t->param_types  = arena_alloc(p->arena, cap * sizeof(AstType *));
        t->param_count  = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                if (t->param_count >= cap) {
                    cap *= 2;
                    AstType **new_arr = arena_alloc(p->arena, cap * sizeof(AstType *));
                    memcpy(new_arr, t->param_types, t->param_count * sizeof(AstType *));
                    t->param_types = new_arr;
                }
                t->param_types[t->param_count++] = parse_type(p);
            } while (match_tok(p, TOK_COMMA));
        }
        expect(p, TOK_RPAREN, "expected ')' in function pointer type");
        expect(p, TOK_ARROW,  "expected '->' in function pointer type");
        t->return_type = parse_type(p);
        return t;
    }

    // Primitive / named types
    switch (p->current.type) {
        case TOK_TYPE_INT:  t->kind = TY_INT;  advance_parser(p); return t;
        case TOK_TYPE_UINT: t->kind = TY_UINT; advance_parser(p); return t;
        case TOK_TYPE_I8:   t->kind = TY_I8;   advance_parser(p); return t;
        case TOK_TYPE_U8:   t->kind = TY_U8;   advance_parser(p); return t;
        case TOK_TYPE_I16:  t->kind = TY_I16;  advance_parser(p); return t;
        case TOK_TYPE_U16:  t->kind = TY_U16;  advance_parser(p); return t;
        case TOK_TYPE_I64:  t->kind = TY_I64;  advance_parser(p); return t;
        case TOK_TYPE_U64:  t->kind = TY_U64;  advance_parser(p); return t;
        case TOK_TYPE_F32:  t->kind = TY_F32;  advance_parser(p); return t;
        case TOK_TYPE_F64:  t->kind = TY_F64;  advance_parser(p); return t;
        case TOK_TYPE_BOOL: t->kind = TY_BOOL; advance_parser(p); return t;
        case TOK_TYPE_CHAR: t->kind = TY_CHAR; advance_parser(p); return t;
        case TOK_TYPE_STR:  t->kind = TY_STR;  advance_parser(p); return t;
        case TOK_TYPE_CSTR: t->kind = TY_CSTR; advance_parser(p); return t;
        case TOK_TYPE_VOID: t->kind = TY_VOID; advance_parser(p); return t;
        case TOK_TYPE_ANY:  t->kind = TY_ANY;  advance_parser(p); return t;
        case TOK_SELF:
            t->kind = TY_NAMED;
            t->name = str_dup_arena(p, "Self");
            advance_parser(p);
            return t;
        case TOK_IDENT: {
            t->kind = TY_NAMED;
            t->name = tok_dup(p, p->current);
            advance_parser(p);
            // Generic args: Vec2<f32>
            if (match_tok(p, TOK_LT)) {
                t->kind = TY_GENERIC;
                size_t cap = 4;
                t->type_args     = arena_alloc(p->arena, cap * sizeof(AstType *));
                t->type_arg_count = 0;
                do {
                    t->type_args[t->type_arg_count++] = parse_type(p);
                } while (match_tok(p, TOK_COMMA));
                expect(p, TOK_GT, "expected '>' after generic type args");
            }
            return t;
        }
        default:
            error(p, "expected type");
            return t;
    }
}

// ─── PARAMETER PARSER ────────────────────────────────────────────────────────

static AstParam parse_param(Parser *p) {
    AstParam param = {0};

    // Variadic: ...name: type
    if (match_tok(p, TOK_DOTDOT)) {
        if (match_tok(p, TOK_DOT)) param.is_variadic = 1; // consume third dot
    }

    Token name = expect(p, TOK_IDENT, "expected parameter name");
    param.name = tok_dup(p, name);
    expect(p, TOK_COLON, "expected ':' after parameter name");
    param.type = parse_type(p);

    // Default value
    if (match_tok(p, TOK_ASSIGN))
        param.default_value = parse_expr(p);

    return param;
}

static AsmParam parse_asm_param(Parser *p) {
    AsmParam param = {0};
    Token name = expect(p, TOK_IDENT, "expected asm parameter name");
    param.name = tok_dup(p, name);
    expect(p, TOK_COLON,  "expected ':' after asm parameter name");
    param.type = parse_type(p);
    expect(p, TOK_ARROW,  "expected '->' for register binding");
    Token reg = expect(p, TOK_IDENT, "expected register name");
    param.reg  = tok_dup(p, reg);
    return param;
}

// ─── RAW ASM BODY CAPTURE ────────────────────────────────────────────────────
// Capture everything inside { } verbatim for asm blocks.

static char *capture_asm_body(Parser *p) {
    // Current token is '{' — we scan the raw source
    const char *start = p->lexer->current;
    int depth = 1;
    while (*p->lexer->current != '\0' && depth > 0) {
        if (*p->lexer->current == '{') depth++;
        else if (*p->lexer->current == '}') { if (--depth == 0) break; }
        p->lexer->current++;
    }
    size_t len = (size_t)(p->lexer->current - start);
    char *body = arena_alloc(p->arena, len + 1);
    memcpy(body, start, len);
    body[len] = '\0';
    p->lexer->current++;  // consume closing '}'
    // Re-prime the parser's current token from the new lexer position
    p->current = lexer_next(p->lexer);
    return body;
}

// ─── USING DECLARATION ───────────────────────────────────────────────────────

static AstNode *parse_using(Parser *p, int is_reexport) {
    int line = p->current.line, col = p->current.col;
    expect(p, TOK_USING, "expected 'using'");
    AstNode *n = node_alloc(p->arena, NODE_USING, line, col);
    n->as.using_decl.is_reexport = is_reexport;

    // Build dotted path: stdio / net.http / "myfile.ci"
    if (check(p, TOK_STRING_LIT)) {
        Token s = advance_parser(p);
        n->as.using_decl.path = s.lit.str_val
            ? str_dup_arena(p, s.lit.str_val)
            : tok_dup(p, s);
    } else {
        char path_buf[256] = {0};
        Token part = expect(p, TOK_IDENT, "expected module name");
        strncat(path_buf, part.start, part.length);
        while (match_tok(p, TOK_DOT)) {
            strncat(path_buf, ".", 2);
            Token next = expect(p, TOK_IDENT, "expected module name after '.'");
            strncat(path_buf, next.start, next.length);
        }
        n->as.using_decl.path = str_dup_arena(p, path_buf);
    }

    // Alias: as http
    if (match_tok(p, TOK_AS)) {
        Token alias = expect(p, TOK_IDENT, "expected alias name after 'as'");
        n->as.using_decl.alias = tok_dup(p, alias);
    }

    // Selective import: { printf, scanf }
    if (match_tok(p, TOK_LBRACE)) {
        size_t cap = 8;
        char **syms = arena_alloc(p->arena, cap * sizeof(char *));
        size_t count = 0;
        do {
            Token sym = expect(p, TOK_IDENT, "expected symbol name");
            syms[count++] = tok_dup(p, sym);
        } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RBRACE));
        expect(p, TOK_RBRACE, "expected '}' after import list");
        n->as.using_decl.symbols      = syms;
        n->as.using_decl.symbol_count = count;
    }

    expect(p, TOK_SEMICOLON, "expected ';' after using declaration");
    return n;
}

// ─── FUNCTION DECLARATION ────────────────────────────────────────────────────

static AstNode *parse_func_decl(Parser *p, AccessMod access,
                                 AstType *return_type, char *name) {
    int line = p->previous.line, col = p->previous.col;
    AstNode *n = node_alloc(p->arena, NODE_FUNC_DECL, line, col);
    n->as.func_decl.access      = access;
    n->as.func_decl.name        = name;
    n->as.func_decl.return_type = return_type;

    // Generic params: name<T, U>(
    if (match_tok(p, TOK_LT)) {
        size_t cap = 4;
        char **gp = arena_alloc(p->arena, cap * sizeof(char *));
        size_t count = 0;
        do {
            Token tp = expect(p, TOK_IDENT, "expected generic type param");
            gp[count++] = tok_dup(p, tp);
            // Optional bound: T: Bound — consume and ignore for now
            if (match_tok(p, TOK_COLON))
                parse_type(p); // bound type, stored later when we need it
        } while (match_tok(p, TOK_COMMA));
        expect(p, TOK_GT, "expected '>' after generic params");
        n->as.func_decl.generic_params = gp;
        n->as.func_decl.generic_count  = count;
    }

    expect(p, TOK_LPAREN, "expected '(' after function name");

    // Parameter list
    size_t cap = 8;
    AstParam *params = arena_alloc(p->arena, cap * sizeof(AstParam));
    size_t count = 0;

    if (!check(p, TOK_RPAREN)) {
        do {
            // self param
            if (check(p, TOK_IDENT) && p->current.length == 4
                    && memcmp(p->current.start, "self", 4) == 0) {
                advance_parser(p);
                n->as.func_decl.is_method = 1;
                AstParam self_p = {0};
                self_p.name = str_dup_arena(p, "self");
                // Consume optional : Type annotation  (self: Vec2)
                if (match_tok(p, TOK_COLON))
                    self_p.type = parse_type(p);
                params[count++] = self_p;
            } else {
                params[count++] = parse_param(p);
            }
        } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
    }
    expect(p, TOK_RPAREN, "expected ')' after parameters");
    n->as.func_decl.params      = params;
    n->as.func_decl.param_count = count;
    // Parse return type if not already provided: fn foo() -> int { }
    if (!return_type && match_tok(p, TOK_ARROW))
        return_type = parse_type(p);
    n->as.func_decl.return_type = return_type;
    // Interface signatures end with ';', not a body
    if (check(p, TOK_SEMICOLON)) {
        match_tok(p, TOK_SEMICOLON);
        n->as.func_decl.body = NULL;
    } else {
        n->as.func_decl.body = parse_block(p);
    }
    return n;
}

// ─── ASM FUNCTION DECLARATION ────────────────────────────────────────────────

static AstNode *parse_asm_func_decl(Parser *p, AccessMod access) {
    int line = p->current.line, col = p->current.col;
    Token name = expect(p, TOK_IDENT, "expected name after 'asm'");
    AstNode *n = node_alloc(p->arena, NODE_ASM_FUNC_DECL, line, col);
    n->as.asm_func_decl.access = access;
    n->as.asm_func_decl.name   = tok_dup(p, name);

    // Optional param list with register bindings
    if (match_tok(p, TOK_LPAREN)) {
        size_t cap = 8;
        AsmParam *params = arena_alloc(p->arena, cap * sizeof(AsmParam));
        size_t count = 0;
        if (!check(p, TOK_RPAREN)) {
            do { params[count++] = parse_asm_param(p); }
            while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
        }
        expect(p, TOK_RPAREN, "expected ')' after asm params");
        n->as.asm_func_decl.params      = params;
        n->as.asm_func_decl.param_count = count;
    }

    // Return register: -> rax
    if (match_tok(p, TOK_ARROW)) {
        Token reg = expect(p, TOK_IDENT, "expected return register");
        n->as.asm_func_decl.return_reg = tok_dup(p, reg);
    }

    // Capture raw asm body
    expect(p, TOK_LBRACE, "expected '{' for asm function body");
    n->as.asm_func_decl.body = capture_asm_body(p);
    return n;
}

// ─── STRUCT DECLARATION ──────────────────────────────────────────────────────

static AstNode *parse_struct_decl(Parser *p, AccessMod access) {
    int line = p->current.line, col = p->current.col;
    Token name = expect(p, TOK_IDENT, "expected struct name");
    AstNode *n = node_alloc(p->arena, NODE_STRUCT_DECL, line, col);
    n->as.struct_decl.access = access;
    n->as.struct_decl.name   = tok_dup(p, name);

    // Generic params
    if (match_tok(p, TOK_LT)) {
        size_t cap = 4;
        char **gp = arena_alloc(p->arena, cap * sizeof(char *));
        size_t count = 0;
        do {
            Token tp = expect(p, TOK_IDENT, "expected type param");
            gp[count++] = tok_dup(p, tp);
        } while (match_tok(p, TOK_COMMA));
        expect(p, TOK_GT, "expected '>'");
        n->as.struct_decl.generic_params = gp;
        n->as.struct_decl.generic_count  = count;
    }

    expect(p, TOK_LBRACE, "expected '{' after struct name");

    size_t field_cap  = 16, field_count  = 0;
    size_t method_cap = 16, method_count = 0;
    size_t embed_cap  =  4, embed_count  = 0;

    AstField  *fields  = arena_alloc(p->arena, field_cap  * sizeof(AstField));
    AstNode  **methods = arena_alloc(p->arena, method_cap * sizeof(AstNode *));
    char     **embeds  = arena_alloc(p->arena, embed_cap  * sizeof(char *));

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        // Embedded struct
        if (match_tok(p, TOK_EMBED)) {
            Token ename = expect(p, TOK_IDENT, "expected struct name after 'embed'");
            embeds[embed_count++] = tok_dup(p, ename);
            match_tok(p, TOK_COMMA);
            continue;
        }

        // Method (starts with access modifier)
        if (check(p, TOK_PUBLIC) || check(p, TOK_PRIVATE) || check(p, TOK_INTERNAL)) {
            methods[method_count++] = parse_declaration(p);
            continue;
        }

        // Field: name: type,
        Token fname = expect(p, TOK_IDENT, "expected field name");
        expect(p, TOK_COLON, "expected ':' after field name");
        AstType *ftype = parse_type(p);
        AstNode *fdefault = NULL;
        if (match_tok(p, TOK_ASSIGN))
            fdefault = parse_expr(p);
        match_tok(p, TOK_COMMA);

        fields[field_count].name          = tok_dup(p, fname);
        fields[field_count].type          = ftype;
        fields[field_count].default_value = fdefault;
        field_count++;
    }

    expect(p, TOK_RBRACE, "expected '}' after struct body");

    n->as.struct_decl.fields       = fields;
    n->as.struct_decl.field_count  = field_count;
    n->as.struct_decl.methods      = methods;
    n->as.struct_decl.method_count = method_count;
    n->as.struct_decl.embeds       = embeds;
    n->as.struct_decl.embed_count  = embed_count;
    return n;
}

static AstNode *parse_union_decl(Parser *p, AccessMod access) {
    int line = p->current.line, col = p->current.col;
    Token name = expect(p, TOK_IDENT, "expected union name");
    AstNode *n = node_alloc(p->arena, NODE_UNION_DECL, line, col);
    // Reuse struct_decl storage for union fields
    n->as.struct_decl.access = access;
    n->as.struct_decl.name   = tok_dup(p, name);
    n->as.struct_decl.generic_params = NULL;
    n->as.struct_decl.generic_count  = 0;
    expect(p, TOK_LBRACE, "expected '{' after union name");
    size_t field_cap = 16, field_count = 0;
    AstField *fields = arena_alloc(p->arena, field_cap * sizeof(AstField));
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Token fname = expect(p, TOK_IDENT, "expected field name");
        expect(p, TOK_COLON, "expected ':' after field name");
        AstType *ftype = parse_type(p);
        match_tok(p, TOK_COMMA);
        fields[field_count].name          = tok_dup(p, fname);
        fields[field_count].type          = ftype;
        fields[field_count].default_value = NULL;
        field_count++;
    }
    expect(p, TOK_RBRACE, "expected '}' after union body");
    n->as.struct_decl.fields       = fields;
    n->as.struct_decl.field_count  = field_count;
    n->as.struct_decl.methods      = NULL;
    n->as.struct_decl.method_count = 0;
    n->as.struct_decl.embeds       = NULL;
    n->as.struct_decl.embed_count  = 0;
    return n;
}

// ─── ENUM DECLARATION ────────────────────────────────────────────────────────

static AstNode *parse_enum_decl(Parser *p, AccessMod access) {
    int line = p->current.line, col = p->current.col;
    Token name = expect(p, TOK_IDENT, "expected enum name");
    AstNode *n = node_alloc(p->arena, NODE_ENUM_DECL, line, col);
    n->as.enum_decl.access = access;
    n->as.enum_decl.name   = tok_dup(p, name);

    expect(p, TOK_LBRACE, "expected '{' after enum name");

    size_t cap = 16, count = 0;
    AstVariant *variants = arena_alloc(p->arena, cap * sizeof(AstVariant));

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Token vname = expect(p, TOK_IDENT, "expected variant name");
        AstVariant v = {0};
        v.name = tok_dup(p, vname);

        // Explicit value: X = 42
        if (match_tok(p, TOK_ASSIGN)) {
            v.has_value      = 1;
            v.explicit_value = parse_expr(p);
        }

        // Tagged union payload: X(field: Type, ...)
        if (match_tok(p, TOK_LPAREN)) {
            size_t fcap = 4, fcount = 0;
            AstParam *fields = arena_alloc(p->arena, fcap * sizeof(AstParam));
            if (!check(p, TOK_RPAREN)) {
                do { fields[fcount++] = parse_param(p); }
                while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
            }
            expect(p, TOK_RPAREN, "expected ')' after enum variant fields");
            v.fields      = fields;
            v.field_count = fcount;
        }

        variants[count++] = v;
        match_tok(p, TOK_COMMA);
    }

    expect(p, TOK_RBRACE, "expected '}' after enum body");
    n->as.enum_decl.variants     = variants;
    n->as.enum_decl.variant_count = count;
    return n;
}

// ADD before parse_interface_decl: impl_decl
static AstNode *parse_impl_decl(Parser *p) {
    int line = p->current.line, col = p->current.col;
    // impl <InterfaceName> for <TypeName> { ... }
    Token iface = expect(p, TOK_IDENT, "expected interface name");
    expect(p, TOK_FOR,   "expected 'for' after interface name");
    Token type  = expect(p, TOK_IDENT, "expected type name");
    AstNode *n  = node_alloc(p->arena, NODE_IMPL_DECL, line, col);
    n->as.impl_decl.interface_name = tok_dup(p, iface);
    n->as.impl_decl.type_name      = tok_dup(p, type);
    expect(p, TOK_LBRACE, "expected '{' after impl header");
    size_t cap = 16, count = 0;
    AstNode **methods = arena_alloc(p->arena, cap * sizeof(AstNode *));
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (!match_tok(p, TOK_FN)) {
            error(p, "expected 'fn' in impl block");
            advance_parser(p);
            continue;
        }
        // fn name(params) -> RetType { body }
        Token mname = expect(p, TOK_IDENT, "expected method name");
        char *mname_str = tok_dup(p, mname);
        // Parse return type: -> Type  (before the param list is NOT how we do it —
        // return type comes AFTER params in Ciren: fn foo(x: int) -> int { }
        // So pass NULL now — return type parsed after params inside parse_func_decl?
        // Actually parse_func_decl does NOT parse ->. The caller must.
        // parse_func_decl takes params then body with no -> parsing.
        // So we need to parse params, then ->, then call parse_func_decl with a
        // pre-consumed name and return_type. But parse_func_decl also parses params!
        // Solution: just pass NULL return type and patch after.
        // Actually simpler: parse_func_decl parses ( params ) then the body.
        // Return type must be passed in. Let's parse it after params by hand.
        // We'll call parse_func_decl, which will consume ( params ) { body }.
        // Then patch return_type from -> after the call... but it's too late.
        // Best solution: parse the -> return type right here before calling.
        // parse_func_decl consumes starting at '(' — so we need -> BEFORE '('?
        // No — in Ciren syntax it's:  fn name(params) -> RetType { body }
        // parse_func_decl starts at '(' so we need to intercept the '->' AFTER ')'.
        // We can't with current design. So let's just temporarily add -> parsing
        // inside parse_func_decl when return_type is NULL:
        methods[count++] = parse_func_decl(p, ACCESS_PRIVATE, NULL, mname_str);
        if (count >= cap) {
            cap *= 2;
            AstNode **nm = arena_alloc(p->arena, cap * sizeof(AstNode *));
            memcpy(nm, methods, count * sizeof(AstNode *));
            methods = nm;
        }
    }
    expect(p, TOK_RBRACE, "expected '}' after impl body");
    n->as.impl_decl.methods      = methods;
    n->as.impl_decl.method_count = count;
    return n;
}

// ─── INTERFACE DECLARATION ───────────────────────────────────────────────────

static AstNode *parse_interface_decl(Parser *p, AccessMod access) {
    int line = p->current.line, col = p->current.col;
    Token name = expect(p, TOK_IDENT, "expected interface name");
    AstNode *n = node_alloc(p->arena, NODE_INTERFACE_DECL, line, col);
    n->as.interface_decl.access = access;
    n->as.interface_decl.name   = tok_dup(p, name);

    // Parent interfaces: : Drawable, Serializable
    if (match_tok(p, TOK_COLON)) {
        size_t cap = 4, count = 0;
        char **parents = arena_alloc(p->arena, cap * sizeof(char *));
        do {
            Token pname = expect(p, TOK_IDENT, "expected interface name");
            parents[count++] = tok_dup(p, pname);
        } while (match_tok(p, TOK_COMMA));
        n->as.interface_decl.parents      = parents;
        n->as.interface_decl.parent_count = count;
    }

    expect(p, TOK_LBRACE, "expected '{' after interface name");

    size_t cap = 16, count = 0;
    AstNode **methods = arena_alloc(p->arena, cap * sizeof(AstNode *));

    // Interface method signatures (no body)
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (match_tok(p, TOK_FN)) {
            // bare fn signature: fn name(params) -> RetType;
            Token mname = expect(p, TOK_IDENT, "expected method name");
            methods[count++] = parse_func_decl(p, ACCESS_PUBLIC, NULL, tok_dup(p, mname));
        } else if (check(p, TOK_PUBLIC) || check(p, TOK_PRIVATE) || check(p, TOK_INTERNAL)) {
            methods[count++] = parse_declaration(p);
        } else {
            error(p, "expected method signature in interface");
            advance_parser(p);
        }
    }

    expect(p, TOK_RBRACE, "expected '}' after interface body");
    n->as.interface_decl.methods      = methods;
    n->as.interface_decl.method_count = count;
    return n;
}

// ─── TOP-LEVEL DECLARATION ───────────────────────────────────────────────────
// This is the big one — handles the [modifier] prefix then dispatches.

static AstNode *parse_declaration(Parser *p) {
    int line = p->current.line, col = p->current.col;

    // Access modifier
    // impl has no access modifier — dispatch early
    if (match_tok(p, TOK_IMPL))
        return parse_impl_decl(p);
    AccessMod access = ACCESS_PRIVATE;
    if      (match_tok(p, TOK_PUBLIC))   access = ACCESS_PUBLIC;
    else if (match_tok(p, TOK_PRIVATE))  access = ACCESS_PRIVATE;
    else if (match_tok(p, TOK_INTERNAL)) access = ACCESS_INTERNAL;
    else { error(p, "expected access modifier (public/private/internal)"); }

        // ADD THIS right after the access modifier block:
    // Handle: public using net.http;  (re-export)
    if (access == ACCESS_PUBLIC && check(p, TOK_USING)) {
        return parse_using(p, 1);
    }

    // ── asm function ─────────────────────────────
    if (match_tok(p, TOK_ASM))
        return parse_asm_func_decl(p, access);

    // ── struct ───────────────────────────────────
    if (match_tok(p, TOK_STRUCT))
        return parse_struct_decl(p, access);

    // ── union ───────────────────────────────────
    if (match_tok(p, TOK_UNION))
        return parse_union_decl(p, access);

    // ── enum ─────────────────────────────────────
    if (match_tok(p, TOK_ENUM))
        return parse_enum_decl(p, access);

    // ── interface ────────────────────────────────
    if (match_tok(p, TOK_INTERFACE))
        return parse_interface_decl(p, access);
    
    // ── impl ─────────────────────────────────────
    if (match_tok(p, TOK_IMPL))
        return parse_impl_decl(p);

    // ── fn keyword (generic/explicit function) ───
    if (match_tok(p, TOK_FN)) {
        // fn name<T: Bound>(params) -> RetType { body }
        Token fname = expect(p, TOK_IDENT, "expected function name");
        return parse_func_decl(p, access, NULL, tok_dup(p, fname));
    }
    
    // ── const ────────────────────────────────────
    if (match_tok(p, TOK_CONST)) {
        AstNode *n = node_alloc(p->arena, NODE_VAR_DECL, line, col);
        Token name = expect(p, TOK_IDENT, "expected constant name");
        n->as.var_decl.access   = access;
        n->as.var_decl.name     = tok_dup(p, name);
        n->as.var_decl.is_const = 1;
        if (match_tok(p, TOK_COLON))
            n->as.var_decl.type = parse_type(p);
        expect(p, TOK_ASSIGN, "expected '=' after const name");
        n->as.var_decl.value = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after const declaration");
        return n;
    }

    // ── let (global var) ─────────────────────────
    if (match_tok(p, TOK_LET)) {
        AstNode *n = node_alloc(p->arena, NODE_VAR_DECL, line, col);
        Token name = expect(p, TOK_IDENT, "expected variable name");
        n->as.var_decl.access   = access;
        n->as.var_decl.name     = tok_dup(p, name);
        n->as.var_decl.is_const = 0;
        if (match_tok(p, TOK_COLON))
            n->as.var_decl.type = parse_type(p);
        if (match_tok(p, TOK_ASSIGN))
            n->as.var_decl.value = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after variable declaration");
        return n;
    }

    // ── function (return_type? name(...) { }) ────
    //
    // The tricky part: Ciren allows return type to be omitted OR present.
    // Strategy: try to parse a type. If the next token after is an IDENT
    // followed by '(' we have: return_type name(  — it's a typed function.
    // If the current token is IDENT followed by '(' with no leading type
    // keyword, the return type is inferred.
    //
    // We use a two-token lookahead heuristic:
    //   current = type keyword OR ident → could be return type OR func name
    //   peek    = ident                 → previous was return type
    //   peek    = '(' or '<'            → current is func name, type inferred

    AstType *return_type = NULL;
    char    *func_name   = NULL;

    // Is current token a type keyword?
    int cur_is_type = (p->current.type >= TOK_TYPE_INT &&
                       p->current.type <= TOK_TYPE_ANY);
    if (cur_is_type || check(p, TOK_STAR)) {
        // Definitely a return type (keyword type or pointer type)
        return_type = parse_type(p);
        Token name = expect(p, TOK_IDENT, "expected function name");
        func_name = tok_dup(p, name);
    } else if (check(p, TOK_IDENT)) {
        // Ambiguous: could be func name with inferred return OR named return type
        // Peek: if the token AFTER the ident is '(' or '<' → it's the func name
        char *first_ident = tok_dup(p, p->current);
        advance_parser(p);

        if (check(p, TOK_LPAREN)) {
            // public main() — inferred return type, no generic
            return_type = NULL;
            func_name   = first_ident;
        } else if (check(p, TOK_LT)) {
            // Could be: generic func name<T> OR generic return type Result<T, E>
            // Peek ahead: after '<' if we see a type keyword or two idents with comma
            // it's a generic return type. If we see just one ident then '(' it's func name.
            // Heuristic: parse it as a generic type, then check if next is IDENT (func name)
            AstType *named_ret = type_alloc(p->arena, TY_NAMED);
            named_ret->name = first_ident;
            // Parse the < type_args >
            advance_parser(p); // consume 
            named_ret->kind = TY_GENERIC;
            size_t cap = 4;
            named_ret->type_args = arena_alloc(p->arena, cap * sizeof(AstType *));
            named_ret->type_arg_count = 0;
            do {
                named_ret->type_args[named_ret->type_arg_count++] = parse_type(p);
            } while (match_tok(p, TOK_COMMA));
            expect(p, TOK_GT, "expected '>' after generic type args");
            // Now: if next is IDENT it's the function name (return type was generic)
            // If next is '(' it was a generic func name (shouldn't happen here)
            if (check(p, TOK_IDENT)) {
                return_type = named_ret;
                Token name = expect(p, TOK_IDENT, "expected function name");
                func_name = tok_dup(p, name);
            } else {
                // generic func — treat first_ident as func name, no return type
                return_type = NULL;
                func_name   = first_ident;
            }
        } else {
            // public Vec2 add(...) — named return type
            AstType *named_ret = type_alloc(p->arena, TY_NAMED);
            named_ret->name = first_ident;
            return_type = named_ret;
            Token name = expect(p, TOK_IDENT, "expected function name");
            func_name = tok_dup(p, name);
        }
    } else if (check(p, TOK_LPAREN)) {
        // Multi-return: (int, bool) funcName(...)
        return_type = parse_type(p);
        Token name = expect(p, TOK_IDENT, "expected function name");
        func_name = tok_dup(p, name);
    } else {
        // void return type
        if (match_tok(p, TOK_TYPE_VOID)) {
            return_type = type_alloc(p->arena, TY_VOID);
        }
        Token name = expect(p, TOK_IDENT, "expected function name");
        func_name = tok_dup(p, name);
    }

    return parse_func_decl(p, access, return_type, func_name);
}

// ─── STATEMENT PARSER ────────────────────────────────────────────────────────

static AstNode *parse_block(Parser *p) {
    int line = p->current.line, col = p->current.col;
    expect(p, TOK_LBRACE, "expected '{'");
    AstNode *n = node_alloc(p->arena, NODE_BLOCK, line, col);

    size_t cap = 32, count = 0;
    AstNode **stmts = arena_alloc(p->arena, cap * sizeof(AstNode *));
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode *s = parse_stmt(p);
        if (s) {
            if (count >= cap) {
                size_t new_cap = cap * 2;
                AstNode **new_stmts = arena_alloc(p->arena, new_cap * sizeof(AstNode *));
                memcpy(new_stmts, stmts, count * sizeof(AstNode *));
                stmts = new_stmts;
                cap = new_cap;
            }
            stmts[count++] = s;
        }
        if (p->panic_mode) synchronize(p);
    }

    expect(p, TOK_RBRACE, "expected '}'");
    n->as.block.stmts = stmts;
    n->as.block.count = count;
    return n;
}

static AstNode *parse_if(Parser *p) {
    int line = p->current.line, col = p->current.col;
    AstNode *n = node_alloc(p->arena, NODE_IF, line, col);
    p->no_struct_lit = 1; n->as.if_stmt.condition = parse_expr(p); p->no_struct_lit = 0;
    n->as.if_stmt.then_block = parse_block(p);

    size_t cap = 4, count = 0;
    AstNode **ei_conds  = arena_alloc(p->arena, cap * sizeof(AstNode *));
    AstNode **ei_blocks = arena_alloc(p->arena, cap * sizeof(AstNode *));

    while (match_tok(p, TOK_ELSE)) {
        if (match_tok(p, TOK_IF)) {
            ei_conds[count]  = parse_expr(p);
            ei_blocks[count] = parse_block(p);
            count++;
        } else {
            n->as.if_stmt.else_block = parse_block(p);
            break;
        }
    }

    n->as.if_stmt.else_if_conds  = ei_conds;
    n->as.if_stmt.else_if_blocks = ei_blocks;
    n->as.if_stmt.else_if_count  = count;
    return n;
}

static AstNode *parse_for(Parser *p) {
    int line = p->current.line, col = p->current.col;

    // C-style: for (let i = 0; ...)
    if (match_tok(p, TOK_LPAREN)) {
        AstNode *n = node_alloc(p->arena, NODE_FOR_C, line, col);
        n->as.for_c.init = parse_stmt(p);
        n->as.for_c.condition = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' in for loop");
        n->as.for_c.post = parse_expr(p);
        expect(p, TOK_RPAREN, "expected ')' after for clauses");
        n->as.for_c.body = parse_block(p);
        return n;
    }

    // for i, item in ...  OR  for item in ...
    Token first = expect(p, TOK_IDENT, "expected variable name in for");
    char *first_name = tok_dup(p, first);

    if (match_tok(p, TOK_COMMA)) {
        // for i, item in iterable
        AstNode *n = node_alloc(p->arena, NODE_FOR_INDEX, line, col);
        Token second = expect(p, TOK_IDENT, "expected value variable after ','");
        expect(p, TOK_IN, "expected 'in' after for variables");
        n->as.for_index.index_var = first_name;
        n->as.for_index.value_var = tok_dup(p, second);
        n->as.for_index.iterable  = parse_expr(p);
        n->as.for_index.body      = parse_block(p);
        return n;
    }

    expect(p, TOK_IN, "expected 'in' after for variable");
    p->no_struct_lit = 1; AstNode *from = parse_expr(p); p->no_struct_lit = 0;

    // parse_expr may have consumed the whole 0..10 as NODE_RANGE
    if (from->kind == NODE_RANGE) {
        AstNode *n = node_alloc(p->arena, NODE_FOR_RANGE, line, col);
        n->as.for_range.var       = first_name;
        n->as.for_range.from      = from->as.range.from;
        n->as.for_range.to        = from->as.range.to;
        n->as.for_range.inclusive = from->as.range.inclusive;
        n->as.for_range.body      = parse_block(p);
        return n;
    }
    // Explicit ..  or ..= (shouldn't normally occur now, but keep as fallback)
    if (check(p, TOK_DOTDOT) || check(p, TOK_DOTDOT_EQ)) {
        int inclusive = check(p, TOK_DOTDOT_EQ);
        advance_parser(p);
        AstNode *n = node_alloc(p->arena, NODE_FOR_RANGE, line, col);
        n->as.for_range.var       = first_name;
        n->as.for_range.from      = from;
        n->as.for_range.to        = parse_expr(p);
        n->as.for_range.inclusive = inclusive;
        n->as.for_range.body      = parse_block(p);
        return n;
    }

    // for item in collection
    AstNode *n = node_alloc(p->arena, NODE_FOR_IN, line, col);
    n->as.for_in.var      = first_name;
    n->as.for_in.iterable = from;
    n->as.for_in.body     = parse_block(p);
    return n;
}

static AstNode *parse_match(Parser *p) {
    int line = p->current.line, col = p->current.col;
    AstNode *n = node_alloc(p->arena, NODE_MATCH, line, col);
    p->no_struct_lit = 1; n->as.match_stmt.subject = parse_expr(p); p->no_struct_lit = 0;
    expect(p, TOK_LBRACE, "expected '{' after match subject");

    size_t cap = 16, count = 0;
    MatchArm *arms = arena_alloc(p->arena, cap * sizeof(MatchArm));

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        MatchArm arm = {0};

        // Wildcard: _
        // Pattern
        if (check(p, TOK_IDENT) && p->current.length == 1
                && p->current.start[0] == '_') {
            arm.pattern = node_alloc(p->arena, NODE_IDENT, line, col);
            arm.pattern->as.ident.name = str_dup_arena(p, "_");
            advance_parser(p);
        } else if (check(p, TOK_IDENT)) {
            int pl = p->current.line, pc2 = p->current.col;
            char *first = tok_dup(p, p->current);
            advance_parser(p);
            if (check(p, TOK_LPAREN)) {
                // Bare variant pattern: Ok(v), Err(e)
                AstNode *pat = node_alloc(p->arena, NODE_ENUM_PATTERN, pl, pc2);
                pat->as.enum_pattern.enum_name      = first;
                pat->as.enum_pattern.variant        = first;
                pat->as.enum_pattern.bindings       = NULL;
                pat->as.enum_pattern.binding_count  = 0;
                advance_parser(p); // consume (
                size_t cap = 8, bcount = 0;
                char **bindings = arena_alloc(p->arena, cap * sizeof(char *));
                if (!check(p, TOK_RPAREN)) {
                    do {
                        ARR_GROW(p->arena, bindings, bcount, cap, char *);
                        Token b = expect(p, TOK_IDENT, "expected binding name");
                        bindings[bcount++] = tok_dup(p, b);
                    } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
                }
                expect(p, TOK_RPAREN, "expected ')' after bindings");
                pat->as.enum_pattern.bindings      = bindings;
                pat->as.enum_pattern.binding_count = bcount;
                arm.pattern = pat;
            } else if (check(p, TOK_COLONCOLON)) {
                advance_parser(p);
                Token vtok = expect(p, TOK_IDENT, "expected variant name");
                AstNode *pat = node_alloc(p->arena, NODE_ENUM_PATTERN, pl, pc2);
                pat->as.enum_pattern.enum_name      = first;
                pat->as.enum_pattern.variant        = tok_dup(p, vtok);
                pat->as.enum_pattern.bindings       = NULL;
                pat->as.enum_pattern.binding_count  = 0;
                if (match_tok(p, TOK_LPAREN)) {
                    size_t cap = 8, count = 0;
                    char **bindings = arena_alloc(p->arena, cap * sizeof(char *));
                    if (!check(p, TOK_RPAREN)) {
                        do {
                            Token b = expect(p, TOK_IDENT, "expected binding name");
                            bindings[count++] = tok_dup(p, b);
                        } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
                    }
                    expect(p, TOK_RPAREN, "expected ')' after bindings");
                    pat->as.enum_pattern.bindings      = bindings;
                    pat->as.enum_pattern.binding_count = count;
                }
                arm.pattern = pat;
            } else {
                AstNode *pat = node_alloc(p->arena, NODE_IDENT, pl, pc2);
                pat->as.ident.name = first;
                arm.pattern = pat;
            }
        } else {
            arm.pattern = parse_expr(p);
        }

        // Guard: if expr
        if (match_tok(p, TOK_IF))
            arm.guard = parse_expr(p);

        expect(p, TOK_FAT_ARROW, "expected '=>' in match arm");

        // Body: block or single expr

        if (check(p, TOK_LBRACE))
            arm.body = parse_block(p);
        else {
            arm.body = parse_expr(p);
        }

        match_tok(p, TOK_COMMA);
        arms[count++] = arm;
    }

    expect(p, TOK_RBRACE, "expected '}' after match arms");
    n->as.match_stmt.arms     = arms;
    n->as.match_stmt.arm_count = count;
    return n;
}

static AstNode *parse_stmt(Parser *p) {
    int line = p->current.line, col = p->current.col;

    // let x = ...
    if (match_tok(p, TOK_LET)) {
        AstNode *n = node_alloc(p->arena, NODE_VAR_DECL, line, col);

        // Multi-assign: let a, b = func()
        Token first = expect(p, TOK_IDENT, "expected variable name");
        if (match_tok(p, TOK_COMMA)) {
            AstNode *mn = node_alloc(p->arena, NODE_MULTI_ASSIGN, line, col);
            size_t cap = 4, count = 1;
            char **names = arena_alloc(p->arena, cap * sizeof(char *));
            names[0] = tok_dup(p, first);
            do {
                Token nm = expect(p, TOK_IDENT, "expected variable name");
                names[count++] = tok_dup(p, nm);
            } while (match_tok(p, TOK_COMMA));
            expect(p, TOK_ASSIGN, "expected '=' in multi-assign");
            mn->as.multi_assign.names = names;
            mn->as.multi_assign.count = count;
            mn->as.multi_assign.value = parse_expr(p);
            expect(p, TOK_SEMICOLON, "expected ';'");
            return mn;
        }

        n->as.var_decl.name = tok_dup(p, first);
        if (match_tok(p, TOK_COLON))
            n->as.var_decl.type = parse_type(p);
        if (match_tok(p, TOK_ASSIGN))
            n->as.var_decl.value = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after variable declaration");
        return n;
    }

    // const x = ...
    if (match_tok(p, TOK_CONST)) {
        AstNode *n = node_alloc(p->arena, NODE_VAR_DECL, line, col);
        Token name = expect(p, TOK_IDENT, "expected constant name");
        n->as.var_decl.name     = tok_dup(p, name);
        n->as.var_decl.is_const = 1;
        if (match_tok(p, TOK_COLON))
            n->as.var_decl.type = parse_type(p);
        expect(p, TOK_ASSIGN, "expected '='");
        n->as.var_decl.value = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';'");
        return n;
    }

    if (match_tok(p, TOK_RETURN)) {
        AstNode *n = node_alloc(p->arena, NODE_RETURN, line, col);
        if (!check(p, TOK_SEMICOLON))
            n->as.ret.value = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after return");
        return n;
    }

    if (match_tok(p, TOK_IF))    return parse_if(p);
    if (match_tok(p, TOK_FOR))   return parse_for(p);

    if (match_tok(p, TOK_WHILE)) {
        AstNode *n = node_alloc(p->arena, NODE_WHILE, line, col);
        p->no_struct_lit = 1; n->as.while_stmt.condition = parse_expr(p); p->no_struct_lit = 0;
        n->as.while_stmt.body      = parse_block(p);
        return n;
    }

    if (match_tok(p, TOK_LOOP)) {
        AstNode *n = node_alloc(p->arena, NODE_LOOP, line, col);
        n->as.loop_stmt.body = parse_block(p);
        return n;
    }

    if (match_tok(p, TOK_MATCH))    return parse_match(p);

    if (match_tok(p, TOK_BREAK)) {
        AstNode *n = node_alloc(p->arena, NODE_BREAK, line, col);
        if (check(p, TOK_IDENT)) {
            n->as.jump.label = tok_dup(p, p->current);
            advance_parser(p);
        }
        expect(p, TOK_SEMICOLON, "expected ';' after break");
        return n;
    }

    if (match_tok(p, TOK_CONTINUE)) {
        AstNode *n = node_alloc(p->arena, NODE_CONTINUE, line, col);
        if (check(p, TOK_IDENT)) {
            n->as.jump.label = tok_dup(p, p->current);
            advance_parser(p);
        }
        expect(p, TOK_SEMICOLON, "expected ';' after continue");
        return n;
    }

    if (match_tok(p, TOK_DEFER)) {
        AstNode *n = node_alloc(p->arena, NODE_DEFER, line, col);
        n->as.defer_stmt.stmt = parse_stmt(p);
        return n;
    }

    if (match_tok(p, TOK_PANIC)) {
        AstNode *n = node_alloc(p->arena, NODE_PANIC, line, col);
        expect(p, TOK_LPAREN, "expected '(' after panic");
        n->as.panic_stmt.msg = parse_expr(p);
        expect(p, TOK_RPAREN, "expected ')'");
        expect(p, TOK_SEMICOLON, "expected ';'");
        return n;
    }

    if (match_tok(p, TOK_DELETE)) {
        AstNode *n = node_alloc(p->arena, NODE_DELETE, line, col);
        n->as.delete_expr.ptr = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after delete");
        return n;
    }

    // Inline asm block
    if (match_tok(p, TOK_ASM)) {
        AstNode *n = node_alloc(p->arena, NODE_ASM_BLOCK, line, col);
        expect(p, TOK_LBRACE, "expected '{' after asm");
        n->as.asm_block.body = capture_asm_body(p);
        return n;
    }

    if (match_tok(p, TOK_LBRACE)) {
        if (check(p, TOK_LBRACE))
        return parse_block(p);
    }   

    

    // Expression statement (assignment, call, etc.)
    AstNode *expr = parse_expr(p);
    AstNode *n    = node_alloc(p->arena, NODE_EXPR_STMT, line, col);
    n->as.expr_stmt.expr = expr;
    expect(p, TOK_SEMICOLON, "expected ';' after expression");
    return n;
}

// ─── EXPRESSION PARSER (Pratt) ───────────────────────────────────────────────

typedef enum {
    PREC_NONE,
    PREC_ASSIGN,      // = += -= *= /=
    PREC_OR,          // ||
    PREC_AND,         // &&
    PREC_EQUALITY,    // == !=
    PREC_COMPARE,     // < > <= >=
    PREC_RANGE,       // .. ..=
    PREC_BITWISE_OR,  // |
    PREC_BITWISE_XOR, // ^
    PREC_BITWISE_AND, // &
    PREC_SHIFT,       // << >>
    PREC_TERM,        // + -
    PREC_FACTOR,      // * / %
    PREC_UNARY,       // ! - ~ * &
    PREC_POSTFIX,     // ++ -- . [] () as ?
    PREC_PRIMARY,
} Precedence;

static AstNode *parse_expr_prec(Parser *p, Precedence min_prec);

static AstNode *parse_primary(Parser *p) {
    int line = p->current.line, col = p->current.col;

    // Literals
    if (match_tok(p, TOK_INT_LIT)) {
        AstNode *n = node_alloc(p->arena, NODE_INT_LIT, line, col);
        n->as.int_lit.value = p->previous.lit.int_val;
        return n;
    }
    if (match_tok(p, TOK_FLOAT_LIT)) {
        AstNode *n = node_alloc(p->arena, NODE_FLOAT_LIT, line, col);
        n->as.float_lit.value = p->previous.lit.float_val;
        return n;
    }
    if (match_tok(p, TOK_STRING_LIT)) {
        AstNode *n = node_alloc(p->arena, NODE_STRING_LIT, line, col);
        n->as.string_lit.value = p->previous.lit.str_val
            ? str_dup_arena(p, p->previous.lit.str_val)
            : tok_dup(p, p->previous);
        return n;
    }
    if (match_tok(p, TOK_CSTRING_LIT)) {
        AstNode *n = node_alloc(p->arena, NODE_CSTRING_LIT, line, col);
        n->as.cstring_lit.value = p->previous.lit.str_val
            ? str_dup_arena(p, p->previous.lit.str_val)
            : tok_dup(p, p->previous);
        return n;
    }
    if (match_tok(p, TOK_CHAR_LIT)) {
        AstNode *n = node_alloc(p->arena, NODE_CHAR_LIT, line, col);
        n->as.char_lit.value = (char)p->previous.lit.int_val;
        return n;
    }
    if (match_tok(p, TOK_BOOL_LIT)) {
        AstNode *n = node_alloc(p->arena, NODE_BOOL_LIT, line, col);
        n->as.bool_lit.value = (int)p->previous.lit.int_val;
        return n;
    }
    if (match_tok(p, TOK_NULL)) {
        return node_alloc(p->arena, NODE_NULL_LIT, line, col);
    }

    // Grouped: (expr)
    if (match_tok(p, TOK_LPAREN)) {
        AstNode *e = parse_expr(p);
        expect(p, TOK_RPAREN, "expected ')'");
        return e;
    }

    // sizeof / alignof
    if (match_tok(p, TOK_SIZEOF)) {
        AstNode *n = node_alloc(p->arena, NODE_SIZEOF, line, col);
        expect(p, TOK_LPAREN, "expected '(' after sizeof");
        n->as.size_expr.type = parse_type(p);
        expect(p, TOK_RPAREN, "expected ')' after sizeof type");
        return n;
    }
    if (match_tok(p, TOK_ALIGNOF)) {
        AstNode *n = node_alloc(p->arena, NODE_ALIGNOF, line, col);
        expect(p, TOK_LPAREN, "expected '(' after alignof");
        n->as.size_expr.type = parse_type(p);
        expect(p, TOK_RPAREN, "expected ')' after alignof type");
        return n;
    }

    // new Point { x: 1.0, y: 2.0 }
    if (match_tok(p, TOK_NEW)) {
        AstNode *n = node_alloc(p->arena, NODE_NEW, line, col);
        Token tname = expect(p, TOK_IDENT, "expected type name after 'new'");
        n->as.new_expr.type_name = tok_dup(p, tname);
        if (match_tok(p, TOK_LBRACE)) {
            size_t cap = 8, count = 0;
            AstNode **fnames = arena_alloc(p->arena, cap * sizeof(AstNode *));
            AstNode **fvals  = arena_alloc(p->arena, cap * sizeof(AstNode *));
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                Token fname = expect(p, TOK_IDENT, "expected field name");
                expect(p, TOK_COLON, "expected ':' after field name");
                AstNode *fni = node_alloc(p->arena, NODE_IDENT, line, col);
                fni->as.ident.name = tok_dup(p, fname);
                fnames[count] = fni;
                fvals[count]  = parse_expr(p);
                count++;
                match_tok(p, TOK_COMMA);
            }
            expect(p, TOK_RBRACE, "expected '}' after new initializer");
            n->as.new_expr.field_names  = fnames;
            n->as.new_expr.field_values = fvals;
            n->as.new_expr.field_count  = count;
        }
        return n;
    }

    // Inferred struct literal: .{ x: 1.0, y: 2.0 }
    if (match_tok(p, TOK_DOT)) {
        if (match_tok(p, TOK_LBRACE)) {
            AstNode *n = node_alloc(p->arena, NODE_STRUCT_LITERAL, line, col);
            n->as.struct_lit.type_name = NULL;   // inferred
            size_t cap = 8, count = 0;
            AstNode **fnames = arena_alloc(p->arena, cap * sizeof(AstNode *));
            AstNode **fvals  = arena_alloc(p->arena, cap * sizeof(AstNode *));
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                Token fname = expect(p, TOK_IDENT, "expected field name");
                expect(p, TOK_COLON, "expected ':' in struct literal");
                AstNode *fni = node_alloc(p->arena, NODE_IDENT, line, col);
                fni->as.ident.name = tok_dup(p, fname);
                fnames[count] = fni;
                fvals[count]  = parse_expr(p);
                count++;
                match_tok(p, TOK_COMMA);
            }
            expect(p, TOK_RBRACE, "expected '}' after struct literal");
            n->as.struct_lit.field_names  = fnames;
            n->as.struct_lit.field_values = fvals;
            n->as.struct_lit.field_count  = count;
            return n;
        }
        error(p, "expected '{' after '.' for inferred struct literal");
    }

    // Array literal: [1, 2, 3]
    if (match_tok(p, TOK_LBRACKET)) {
        AstNode *n = node_alloc(p->arena, NODE_ARRAY_LITERAL, line, col);
        size_t cap = 16, count = 0;
        AstNode **elems = arena_alloc(p->arena, cap * sizeof(AstNode *));
        if (!check(p, TOK_RBRACKET)) {
            do { elems[count++] = parse_expr(p); }
            while (match_tok(p, TOK_COMMA) && !check(p, TOK_RBRACKET));
        }
        expect(p, TOK_RBRACKET, "expected ']' after array literal");
        n->as.array_lit.elems = elems;
        n->as.array_lit.count = count;
        return n;
    }

    // Lambda: (x: int) => x * x   OR  (x: int) -> int { return x * x; }
    // (Already consumed '(' above for grouped — we detect lambda via lookahead
    //  in the grouped case. For now, lambdas without parens not supported.)

    // If expression: if x > 0 { "pos" } else { "neg" }
    if (match_tok(p, TOK_IF)) {
        AstNode *n = node_alloc(p->arena, NODE_IF_EXPR, line, col);
        n->as.if_stmt.condition  = parse_expr(p);
        n->as.if_stmt.then_block = parse_block(p);
        if (match_tok(p, TOK_ELSE))
            n->as.if_stmt.else_block = parse_block(p);
        return n;
    }

    // Identifier (or struct literal, or lambda start)
    if (check(p, TOK_IDENT)) {
        Token name = advance_parser(p);
        char *ident_name = tok_dup(p, name);

        // Enum literal: Shape::Circle(5.0) or Shape::Point
        if (check(p, TOK_COLONCOLON)) {
            advance_parser(p);
            Token vtok = expect(p, TOK_IDENT, "expected variant name after '::'");
            AstNode *n = node_alloc(p->arena, NODE_ENUM_LITERAL, line, col);
            n->as.enum_lit.enum_name = ident_name;
            n->as.enum_lit.variant   = tok_dup(p, vtok);
            n->as.enum_lit.args      = NULL;
            n->as.enum_lit.arg_count = 0;
            if (match_tok(p, TOK_LPAREN)) {
                size_t cap = 8, count = 0;
                AstNode **args = arena_alloc(p->arena, cap * sizeof(AstNode *));
                if (!check(p, TOK_RPAREN)) {
                    do { ARR_GROW(p->arena, args, count, cap, AstNode *); args[count++] = parse_expr(p); }
                    while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
                }
                expect(p, TOK_RPAREN, "expected ')' after variant args");
                n->as.enum_lit.args      = args;
                n->as.enum_lit.arg_count = count;
            }
            return n;
        }

        // Struct literal: Vec2 { x: 1.0, y: 2.0 }
        if (check(p, TOK_LBRACE) && !p->no_struct_lit) {
            advance_parser(p);
            AstNode *n = node_alloc(p->arena, NODE_STRUCT_LITERAL, line, col);
            n->as.struct_lit.type_name = ident_name;
            size_t cap = 8, count = 0;
            AstNode **fnames = arena_alloc(p->arena, cap * sizeof(AstNode *));
            AstNode **fvals  = arena_alloc(p->arena, cap * sizeof(AstNode *));
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                ARR_GROW(p->arena, fnames, count, cap, AstNode *);
                ARR_GROW(p->arena, fvals,  count, cap, AstNode *);
                Token fname = expect(p, TOK_IDENT, "expected field name");
                expect(p, TOK_COLON, "expected ':' in struct literal");
                AstNode *fni = node_alloc(p->arena, NODE_IDENT, line, col);
                fni->as.ident.name = tok_dup(p, fname);
                fnames[count] = fni;
                fvals[count]  = parse_expr(p);
                count++;
                match_tok(p, TOK_COMMA);
            }
            expect(p, TOK_RBRACE, "expected '}' after struct literal");
            n->as.struct_lit.field_names  = fnames;
            n->as.struct_lit.field_values = fvals;
            n->as.struct_lit.field_count  = count;
            return n;
        }

        AstNode *n = node_alloc(p->arena, NODE_IDENT, line, col);
        n->as.ident.name = ident_name;
        return n;
    }

    error(p, "expected expression");
    advance_parser(p);
    return node_alloc(p->arena, NODE_INT_LIT, line, col);
}

static int token_prec(TokenType t) {
    switch (t) {
        case TOK_ASSIGN:
        case TOK_PLUS_ASSIGN:
        case TOK_MINUS_ASSIGN:
        case TOK_STAR_ASSIGN:
        case TOK_SLASH_ASSIGN:   return PREC_ASSIGN;
        case TOK_OR:             return PREC_OR;
        case TOK_AND:            return PREC_AND;
        case TOK_EQ:
        case TOK_NEQ:            return PREC_EQUALITY;
        case TOK_LT:
        case TOK_GT:
        case TOK_LTE:
        case TOK_GTE:            return PREC_COMPARE;
        case TOK_DOTDOT:
        case TOK_DOTDOT_EQ:      return PREC_RANGE;
        case TOK_PIPE:           return PREC_BITWISE_OR;
        case TOK_CARET:          return PREC_BITWISE_XOR;
        case TOK_AMP:            return PREC_BITWISE_AND;
        case TOK_LSHIFT:
        case TOK_RSHIFT:         return PREC_SHIFT;
        case TOK_PLUS:
        case TOK_MINUS:          return PREC_TERM;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:        return PREC_FACTOR;
        case TOK_PLUS_PLUS:
        case TOK_MINUS_MINUS:
        case TOK_DOT:
        case TOK_LBRACKET:
        case TOK_LPAREN:
        case TOK_AS:
        case TOK_QUESTION:       return PREC_POSTFIX;
        default:                 return PREC_NONE;
    }
}

static AstNode *parse_expr_prec(Parser *p, Precedence min_prec) {
    int line = p->current.line, col = p->current.col;

    // Prefix / unary
    AstNode *left = NULL;
    TokenType cur = p->current.type;

    if (cur == TOK_MINUS || cur == TOK_BANG || cur == TOK_TILDE) {
        advance_parser(p);
        AstNode *n = node_alloc(p->arena, NODE_UNARY, line, col);
        n->as.unary.op      = cur;
        n->as.unary.operand = parse_expr_prec(p, PREC_UNARY);
        left = n;
    } else if (cur == TOK_STAR) {
        // dereference: *ptr
        advance_parser(p);
        AstNode *n = node_alloc(p->arena, NODE_DEREF, line, col);
        n->as.unary.op      = TOK_STAR;
        n->as.unary.operand = parse_expr_prec(p, PREC_UNARY);
        left = n;
    } else if (cur == TOK_AMP) {
        // address-of: &x
        advance_parser(p);
        AstNode *n = node_alloc(p->arena, NODE_ADDRESS_OF, line, col);
        n->as.unary.op      = TOK_AMP;
        n->as.unary.operand = parse_expr_prec(p, PREC_UNARY);
        left = n;
    } else if (cur == TOK_PLUS_PLUS || cur == TOK_MINUS_MINUS) {
        // prefix ++/--
        advance_parser(p);
        AstNode *n = node_alloc(p->arena, NODE_UNARY, line, col);
        n->as.unary.op      = cur;
        n->as.unary.postfix = 0;
        n->as.unary.operand = parse_expr_prec(p, PREC_UNARY);
        left = n;
    } else {
        left = parse_primary(p);
    }

    // Infix / postfix loop
    while (1) {
        int prec = token_prec(p->current.type);
        if (prec <= (int)min_prec) break;

        TokenType op = p->current.type;
        int op_line  = p->current.line;
        int op_col   = p->current.col;
        advance_parser(p);

        // Postfix ++/--
        if (op == TOK_PLUS_PLUS || op == TOK_MINUS_MINUS) {
            AstNode *n = node_alloc(p->arena, NODE_UNARY, op_line, op_col);
            n->as.unary.op      = op;
            n->as.unary.postfix = 1;
            n->as.unary.operand = left;
            left = n;
            continue;
        }

        // Field access: a.b
        if (op == TOK_DOT) {
            Token fname = expect(p, TOK_IDENT, "expected field name after '.'");
            AstNode *n = node_alloc(p->arena, NODE_FIELD, op_line, op_col);
            n->as.field.target = left;
            n->as.field.field  = tok_dup(p, fname);
            left = n;
            continue;
        }

        // Subscript / slice: a[i] or a[2..5]
        if (op == TOK_LBRACKET) {
            AstNode *idx = parse_expr(p);
            if (check(p, TOK_DOTDOT) || check(p, TOK_DOTDOT_EQ)) {
                int inclusive = check(p, TOK_DOTDOT_EQ);
                advance_parser(p);
                AstNode *n = node_alloc(p->arena, NODE_SLICE_EXPR, op_line, op_col);
                n->as.slice_expr.target    = left;
                n->as.slice_expr.from      = idx;
                n->as.slice_expr.to        = check(p, TOK_RBRACKET) ? NULL : parse_expr(p);
                n->as.slice_expr.inclusive = inclusive;
                expect(p, TOK_RBRACKET, "expected ']'");
                left = n;
            } else {
                AstNode *n = node_alloc(p->arena, NODE_INDEX, op_line, op_col);
                n->as.index.target = left;
                n->as.index.index  = idx;
                expect(p, TOK_RBRACKET, "expected ']'");
                left = n;
            }
            continue;
        }

        // Function call: callee(args)
        if (op == TOK_LPAREN) {
            AstNode *n = node_alloc(p->arena, NODE_CALL, op_line, op_col);
            n->as.call.callee = left;
            size_t cap = 8, count = 0;
            AstNode **args = arena_alloc(p->arena, cap * sizeof(AstNode *));
            if (!check(p, TOK_RPAREN)) {
                do {
                    if (count >= cap) {
                        size_t new_cap = cap * 2;
                        AstNode **new_args = arena_alloc(p->arena, new_cap * sizeof(AstNode *));
                        memcpy(new_args, args, count * sizeof(AstNode *));
                        args = new_args;
                        cap  = new_cap;
                    }
                    ARR_GROW(p->arena, args, count, cap, AstNode *); args[count++] = parse_expr(p);
                } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
            }
            expect(p, TOK_RPAREN, "expected ')' after arguments");
            n->as.call.args      = args;
            n->as.call.arg_count = count;
            left = n;
            continue;
        }

        // Cast: expr as Type
        if (op == TOK_AS) {
            AstNode *n = node_alloc(p->arena, NODE_CAST, op_line, op_col);
            n->as.cast.expr = left;
            n->as.cast.type = parse_type(p);
            left = n;
            continue;
        }

        // ? propagation
        if (op == TOK_QUESTION) {
            AstNode *n = node_alloc(p->arena, NODE_PROPAGATE, op_line, op_col);
            n->as.propagate.expr = left;
            left = n;
            continue;
        }

        // Range: 0..10 / 0..=10
        if (op == TOK_DOTDOT || op == TOK_DOTDOT_EQ) {
            AstNode *n = node_alloc(p->arena, NODE_RANGE, op_line, op_col);
            n->as.range.from      = left;
            n->as.range.to        = parse_expr_prec(p, PREC_RANGE);
            n->as.range.inclusive = (op == TOK_DOTDOT_EQ);
            left = n;
            continue;
        }

        // Assignment operators
        if (op == TOK_ASSIGN || op == TOK_PLUS_ASSIGN ||
            op == TOK_MINUS_ASSIGN || op == TOK_STAR_ASSIGN ||
            op == TOK_SLASH_ASSIGN) {
            AstNode *n = node_alloc(p->arena, NODE_ASSIGN, op_line, op_col);
            n->as.assign.op     = op;
            n->as.assign.target = left;
            n->as.assign.value  = parse_expr_prec(p, PREC_ASSIGN - 1);
            left = n;
            continue;
        }

        // Binary operators
        AstNode *n = node_alloc(p->arena, NODE_BINARY, op_line, op_col);
        n->as.binary.op    = op;
        n->as.binary.left  = left;
        n->as.binary.right = parse_expr_prec(p, prec);
        left = n;
    }

    return left;
}

static AstNode *parse_expr(Parser *p) {
    return parse_expr_prec(p, PREC_NONE);
}

// ─── PROGRAM ROOT ────────────────────────────────────────────────────────────

AstNode *parse(Parser *p) {
    AstNode *root = node_alloc(p->arena, NODE_PROGRAM, 0, 0);
    size_t cap = 64, count = 0;
    AstNode **decls = arena_alloc(p->arena, cap * sizeof(AstNode *));
#define PROGRAM_GROW() do { \
    if (count >= cap) { \
        size_t new_cap = cap * 2; \
        AstNode **nd = arena_alloc(p->arena, new_cap * sizeof(AstNode *)); \
        memcpy(nd, decls, count * sizeof(AstNode *)); \
        decls = nd; cap = new_cap; \
    } \
} while(0)
    while (!check(p, TOK_EOF)) {
        // using declarations (top-level only, optionally re-exported)
        if (check(p, TOK_USING)) {
            PROGRAM_GROW();
            decls[count++] = parse_using(p, 0);
            continue;
        }

        // module declaration
        if (match_tok(p, TOK_MODULE)) {
            int line = p->previous.line, col = p->previous.col;
            AstNode *n = node_alloc(p->arena, NODE_MODULE, line, col);
            char path_buf[256] = {0};
            Token part = expect(p, TOK_IDENT, "expected module name");
            strncat(path_buf, part.start, part.length);
            while (match_tok(p, TOK_DOT)) {
                strncat(path_buf, ".", 2);
                Token next = expect(p, TOK_IDENT, "expected module name");
                strncat(path_buf, next.start, next.length);
            }
            expect(p, TOK_SEMICOLON, "expected ';' after module declaration");
            n->as.module_decl.path = str_dup_arena(p, path_buf);
            PROGRAM_GROW();
            decls[count++] = n;
            continue;
        }

        // Everything else is a declaration with access modifier
        if (check(p, TOK_PUBLIC) || check(p, TOK_PRIVATE) || check(p, TOK_INTERNAL)) {
            AstNode *decl = parse_declaration(p);
            if (decl) { PROGRAM_GROW(); decls[count++] = decl; }
            if (p->panic_mode) synchronize(p);
            continue;
        }
        // impl and interface have no access modifier
        if (match_tok(p, TOK_IMPL)) {
            AstNode *decl = parse_impl_decl(p);
            if (decl) { PROGRAM_GROW(); decls[count++] = decl; }
            if (p->panic_mode) synchronize(p);
            continue;
        }
        if (match_tok(p, TOK_INTERFACE)) {
            AstNode *decl = parse_interface_decl(p, ACCESS_PUBLIC);
            if (decl) { PROGRAM_GROW(); decls[count++] = decl; }
            if (p->panic_mode) synchronize(p);
            continue;
        }
        
        error(p, "expected top-level declaration");
        synchronize(p);
    }

    root->as.program.decls = decls;
    root->as.program.count = count;
    return root;
}