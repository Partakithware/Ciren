// ciren/lexer.h

#ifndef CIREN_LEXER_H
#define CIREN_LEXER_H

#include <stddef.h>
#include <stdint.h>

// ─── TOKEN TYPES ────────────────────────────────────────────────────────────

typedef enum {
    // Literals
    TOK_INT_LIT,        // 42
    TOK_FLOAT_LIT,      // 3.14
    TOK_STRING_LIT,     // "hello"
    TOK_CSTRING_LIT,    // c"hello"
    TOK_CHAR_LIT,       // 'A'
    TOK_BOOL_LIT,       // true / false
    TOK_NULL,           // null

    // Identifiers
    TOK_IDENT,          // myVar, greet, doSysCall

    // Access modifiers
    TOK_PUBLIC,         // public
    TOK_PRIVATE,        // private
    TOK_INTERNAL,       // internal

    // Keywords
    TOK_USING,          // using
    TOK_AS,             // as
    TOK_ASM,            // asm
    TOK_LET,            // let
    TOK_CONST,          // const
    TOK_RETURN,         // return
    TOK_IF,             // if
    TOK_ELSE,           // else
    TOK_FOR,            // for
    TOK_WHILE,          // while
    TOK_LOOP,           // loop
    TOK_MATCH,          // match
    TOK_IN,             // in
    TOK_BREAK,          // break
    TOK_CONTINUE,       // continue
    TOK_DEFER,          // defer
    TOK_PANIC,          // panic
    TOK_STRUCT,         // struct
    TOK_UNION,          // union
    TOK_ENUM,           // enum
    TOK_INTERFACE,      // interface
    TOK_IMPL,           // impl
    TOK_FN,             // fn
    TOK_SELF,           // Self
    TOK_EMBED,          // embed
    TOK_MODULE,         // module
    TOK_CLOBBERS,       // clobbers (inside asm blocks)
    TOK_NEW,            // new
    TOK_DELETE,         // delete
    TOK_SIZEOF,         // sizeof
    TOK_ALIGNOF,        // alignof
    TOK_UNDEFINED,      // undefined

    // Primitive types
    TOK_TYPE_INT,       // int
    TOK_TYPE_UINT,      // uint
    TOK_TYPE_I8,        // i8
    TOK_TYPE_U8,        // u8
    TOK_TYPE_I16,       // i16
    TOK_TYPE_U16,       // u16
    TOK_TYPE_I64,       // i64
    TOK_TYPE_U64,       // u64
    TOK_TYPE_F32,       // f32
    TOK_TYPE_F64,       // f64
    TOK_TYPE_BOOL,      // bool
    TOK_TYPE_CHAR,      // char
    TOK_TYPE_STR,       // str
    TOK_TYPE_CSTR,      // cstr
    TOK_TYPE_VOID,      // void
    TOK_TYPE_ANY,       // any

    // Symbols & delimiters
    TOK_LBRACE,         // {
    TOK_RBRACE,         // }
    TOK_LPAREN,         // (
    TOK_RPAREN,         // )
    TOK_LBRACKET,       // [
    TOK_RBRACKET,       // ]
    TOK_SEMICOLON,      // ;
    TOK_COMMA,          // ,
    TOK_DOT,            // .
    TOK_COLON,          // :
    TOK_COLONCOLON,     // ::
    TOK_QUESTION,       // ?
    TOK_HASH,           // #
    TOK_AT,             // @

    // Operators
    TOK_PLUS,           // +
    TOK_MINUS,          // -
    TOK_STAR,           // *
    TOK_SLASH,          // /
    TOK_PERCENT,        // %
    TOK_AMP,            // &
    TOK_PIPE,           // |
    TOK_CARET,          // ^
    TOK_TILDE,          // ~
    TOK_BANG,           // !
    TOK_LT,             // 
    TOK_GT,             // >
    TOK_LSHIFT,         // 
    TOK_RSHIFT,         // >>
    TOK_AND,            // &&
    TOK_OR,             // ||

    // Assignment & comparison
    TOK_ASSIGN,         // =
    TOK_EQ,             // ==
    TOK_NEQ,            // !=
    TOK_LTE,            // <=
    TOK_GTE,            // >=
    TOK_PLUS_ASSIGN,    // +=
    TOK_MINUS_ASSIGN,   // -=
    TOK_STAR_ASSIGN,    // *=
    TOK_SLASH_ASSIGN,   // /=

    // Special arrows
    TOK_ARROW,          // ->
    TOK_FAT_ARROW,      // =>

    // Range operators
    TOK_DOTDOT,         // ..
    TOK_DOTDOT_EQ,      // ..=

    // Increment / decrement
    TOK_PLUS_PLUS,      // ++
    TOK_MINUS_MINUS,    // --

    // Special
    TOK_EOF,
    TOK_ERROR,
} TokenType;

// ─── TOKEN STRUCT ────────────────────────────────────────────────────────────

typedef struct {
    TokenType   type;
    const char *start;   // pointer into source
    size_t      length;
    int         line;
    int         col;

    union {
        int64_t  int_val;
        double   float_val;
        char    *str_val;   // heap-allocated, null-terminated copy
    } lit;
} Token;

// ─── LEXER STRUCT ────────────────────────────────────────────────────────────

typedef struct {
    const char *source;
    const char *current;
    const char *start;
    int         line;
    int         col;
    const char *filename;
} Lexer;

// ─── API ─────────────────────────────────────────────────────────────────────

void  lexer_init(Lexer *l, const char *source, const char *filename);
Token lexer_next(Lexer *l);
Token lexer_peek(Lexer *l);
void  token_print(Token t);
const char *token_type_name(TokenType t);

#endif // CIREN_LEXER_H