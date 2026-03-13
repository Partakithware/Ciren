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
// ciren/lexer.c

#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ─── HELPERS ─────────────────────────────────────────────────────────────────

static char peek_char(Lexer *l) {
    return *l->current;
}

static char peek_next(Lexer *l) {
    if (*l->current == '\0') return '\0';
    return l->current[1];
}

static char advance(Lexer *l) {
    char c = *l->current++;
    if (c == '\n') { l->line++; l->col = 1; }
    else            { l->col++; }
    return c;
}

static int at_end(Lexer *l) {
    return *l->current == '\0';
}

static Token make_token(Lexer *l, TokenType type) {
    Token t;
    t.type   = type;
    t.start  = l->start;
    t.length = (size_t)(l->current - l->start);
    t.line   = l->line;
    t.col    = l->col - (int)t.length;
    t.lit.str_val = NULL;
    return t;
}

static Token error_token(Lexer *l, const char *msg) {
    Token t;
    t.type        = TOK_ERROR;
    t.start       = msg;
    t.length      = strlen(msg);
    t.line        = l->line;
    t.col         = l->col;
    t.lit.str_val = NULL;
    return t;
}

// ─── SKIP WHITESPACE & COMMENTS ──────────────────────────────────────────────

static void skip_whitespace_and_comments(Lexer *l) {
    for (;;) {
        char c = peek_char(l);

        // Whitespace
        if (c == ' ' || c == '\r' || c == '\t' || c == '\n') {
            advance(l);
            continue;
        }

        // Line comment: // ...
        if (c == '/' && peek_next(l) == '/') {
            while (!at_end(l) && peek_char(l) != '\n')
                advance(l);
            continue;
        }

        // Block comment: /* ... */
        if (c == '/' && peek_next(l) == '*') {
            advance(l); advance(l); // consume /*
            while (!at_end(l)) {
                if (peek_char(l) == '*' && peek_next(l) == '/') {
                    advance(l); advance(l); // consume */
                    break;
                }
                advance(l);
            }
            continue;
        }

        // ASM comment: ; ...
        if (c == ';' ) {
            // Only treat as comment if we're inside an asm block.
            // For now the lexer still emits TOK_SEMICOLON —
            // the parser will contextually ignore asm-style comments.
            // Full asm-block comment handling done in parser pass.
        }

        break;
    }
}

// ─── KEYWORD LOOKUP ──────────────────────────────────────────────────────────

typedef struct { const char *word; TokenType type; } Keyword;

static Keyword keywords[] = {
    // Access
    { "public",    TOK_PUBLIC    },
    { "private",   TOK_PRIVATE   },
    { "internal",  TOK_INTERNAL  },
    // Keywords
    { "using",     TOK_USING     },
    { "as",        TOK_AS        },
    { "asm",       TOK_ASM       },
    { "let",       TOK_LET       },
    { "const",     TOK_CONST     },
    { "return",    TOK_RETURN    },
    { "if",        TOK_IF        },
    { "else",      TOK_ELSE      },
    { "for",       TOK_FOR       },
    { "while",     TOK_WHILE     },
    { "loop",      TOK_LOOP      },
    { "match",     TOK_MATCH     },
    { "in",        TOK_IN        },
    { "break",     TOK_BREAK     },
    { "continue",  TOK_CONTINUE  },
    { "defer",     TOK_DEFER     },
    { "panic",     TOK_PANIC     },
    { "struct",    TOK_STRUCT    },
    { "union",     TOK_UNION     },
    { "enum",      TOK_ENUM      },
    { "interface", TOK_INTERFACE },
    { "impl",      TOK_IMPL },
    { "fn",        TOK_FN },
    { "Self",      TOK_SELF },
    { "embed",     TOK_EMBED     },
    { "module",    TOK_MODULE    },
    { "clobbers",  TOK_CLOBBERS  },
    { "new",       TOK_NEW       },
    { "delete",    TOK_DELETE    },
    { "sizeof",    TOK_SIZEOF    },
    { "alignof",   TOK_ALIGNOF   },
    { "undefined", TOK_UNDEFINED },
    { "true",      TOK_BOOL_LIT  },
    { "false",     TOK_BOOL_LIT  },
    { "null",      TOK_NULL      },
    // Types
    { "int",       TOK_TYPE_INT  },
    { "uint",      TOK_TYPE_UINT },
    { "i8",        TOK_TYPE_I8   },
    { "u8",        TOK_TYPE_U8   },
    { "i16",       TOK_TYPE_I16  },
    { "u16",       TOK_TYPE_U16  },
    { "i64",       TOK_TYPE_I64  },
    { "u64",       TOK_TYPE_U64  },
    { "f32",       TOK_TYPE_F32  },
    { "f64",       TOK_TYPE_F64  },
    { "bool",      TOK_TYPE_BOOL },
    { "char",      TOK_TYPE_CHAR },
    { "str",       TOK_TYPE_STR  },
    { "cstr",      TOK_TYPE_CSTR },
    { "void",      TOK_TYPE_VOID },
    { "any",       TOK_TYPE_ANY  },
    { NULL, 0 }
};

static TokenType lookup_keyword(const char *start, size_t len) {
    for (int i = 0; keywords[i].word != NULL; i++) {
        if (strlen(keywords[i].word) == len &&
            memcmp(keywords[i].word, start, len) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

// ─── SCANNERS ────────────────────────────────────────────────────────────────

static Token scan_ident_or_keyword(Lexer *l) {
    while (isalnum(peek_char(l)) || peek_char(l) == '_')
        advance(l);
    size_t len = (size_t)(l->current - l->start);
    TokenType type = lookup_keyword(l->start, len);
    Token t = make_token(l, type);
    // Store bool literal value
    if (type == TOK_BOOL_LIT)
        t.lit.int_val = (l->start[0] == 't') ? 1 : 0;
    return t;
}

static Token scan_number(Lexer *l) {
    int is_float = 0;

    // Hex: 0x... — note: the '0' was already consumed by lexer_next
    // l->start[0] == '0', l->current points at 'x'/'X'
    if (l->start[0] == '0' && (peek_char(l) == 'x' || peek_char(l) == 'X')) {
        advance(l); // consume 'x'
        while (isxdigit(peek_char(l))) advance(l);
        Token t = make_token(l, TOK_INT_LIT);
        t.lit.int_val = strtoll(t.start, NULL, 16);
        return t;
    }

    // Decimal
    while (isdigit(peek_char(l))) advance(l);

    // Float part
    if (peek_char(l) == '.' && isdigit(peek_next(l))) {
        is_float = 1;
        advance(l);
        while (isdigit(peek_char(l))) advance(l);
    }

    // Exponent
    if (peek_char(l) == 'e' || peek_char(l) == 'E') {
        is_float = 1;
        advance(l);
        if (peek_char(l) == '+' || peek_char(l) == '-') advance(l);
        while (isdigit(peek_char(l))) advance(l);
    }

    // Suffixes: f (f32), u (uint), i64, u64, etc.
    if (!is_float && peek_char(l) == 'u') { advance(l); }
    if (!is_float && peek_char(l) == 'f') { is_float = 1; advance(l); }
    if (peek_char(l) == 'i' || peek_char(l) == 'u') {
        advance(l);
        while (isdigit(peek_char(l))) advance(l); // consume 64, 32, etc.
    }

    Token t = make_token(l, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT);
    if (is_float)
        t.lit.float_val = strtod(t.start, NULL);
    else
        t.lit.int_val = strtoll(t.start, NULL, 10);
    return t;
}

static Token scan_string(Lexer *l, int is_cstr) {
    // opening quote already consumed for cstr — here we consume for regular
    while (!at_end(l) && peek_char(l) != '"') {
        if (peek_char(l) == '\\') advance(l); // skip escape prefix
        advance(l);
    }
    if (at_end(l)) return error_token(l, "Unterminated string literal");
    advance(l); // closing "

    // Build a proper null-terminated copy of the string value
    size_t raw_len = (size_t)(l->current - l->start);
    char *buf = malloc(raw_len + 1);
    size_t out = 0;
    const char *s = is_cstr ? l->start : l->start + 1;
    const char *end = l->current - 1;
    while (s < end) {
        if (*s == '\\') {
            s++;
            switch (*s) {
                case 'n':  buf[out++] = '\n'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'r':  buf[out++] = '\r'; break;
                case '"':  buf[out++] = '"';  break;
                case '\\': buf[out++] = '\\'; break;
                case '0':  buf[out++] = '\0'; break;
                default:   buf[out++] = *s;   break;
            }
        } else {
            buf[out++] = *s;
        }
        s++;
    }
    buf[out] = '\0';

    Token t = make_token(l, is_cstr ? TOK_CSTRING_LIT : TOK_STRING_LIT);
    t.lit.str_val = buf;
    return t;
}

static Token scan_char(Lexer *l) {
    char c = advance(l);
    if (c == '\\') {
        char esc = advance(l);
        switch (esc) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c =  0;   break;
            default:  c = esc;  break;
        }
    }
    if (peek_char(l) != '\'') return error_token(l, "Unterminated char literal");
    advance(l); // closing '
    Token t = make_token(l, TOK_CHAR_LIT);
    t.lit.int_val = (int64_t)c;
    return t;
}

// ─── MAIN SCAN ───────────────────────────────────────────────────────────────

Token lexer_next(Lexer *l) {
    skip_whitespace_and_comments(l);
    l->start = l->current;

    if (at_end(l)) return make_token(l, TOK_EOF);

    char c = advance(l);

    // c"..." must be caught before the isalpha check swallows 'c' as an ident
    if (c == 'c' && peek_char(l) == '"') {
        advance(l);           // consume opening "
        l->start = l->current; // reset start to AFTER the " so it's not in the value
        return scan_string(l, 1);
    }


    // Identifier / keyword
    if (isalpha(c) || c == '_') return scan_ident_or_keyword(l);

    // Number
    if (isdigit(c)) return scan_number(l);

    switch (c) {

        // String literals
        case '"': return scan_string(l, 0);
        case '\'': return scan_char(l);

        // Braces / brackets
        case '{': return make_token(l, TOK_LBRACE);
        case '}': return make_token(l, TOK_RBRACE);
        case '(': return make_token(l, TOK_LPAREN);
        case ')': return make_token(l, TOK_RPAREN);
        case '[': return make_token(l, TOK_LBRACKET);
        case ']': return make_token(l, TOK_RBRACKET);
        case ',': return make_token(l, TOK_COMMA);
        case '~': return make_token(l, TOK_TILDE);
        case '^': return make_token(l, TOK_CARET);
        case '#': return make_token(l, TOK_HASH);
        case '@': return make_token(l, TOK_AT);
        case ';': return make_token(l, TOK_SEMICOLON);

        // Dot / range
        case '.':
            if (peek_char(l) == '.') {
                advance(l);
                if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_DOTDOT_EQ); }
                return make_token(l, TOK_DOTDOT);
            }
            return make_token(l, TOK_DOT);

        // Colon / ::
        case ':':
            if (peek_char(l) == ':') { advance(l); return make_token(l, TOK_COLONCOLON); }
            return make_token(l, TOK_COLON);

        // Equals
        case '=':
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_EQ);        }
            if (peek_char(l) == '>') { advance(l); return make_token(l, TOK_FAT_ARROW); }
            return make_token(l, TOK_ASSIGN);

        // Bang
        case '!':
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_NEQ); }
            return make_token(l, TOK_BANG);

        // Less-than
        case '<':
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_LTE);    }
            if (peek_char(l) == '<') { advance(l); return make_token(l, TOK_LSHIFT); }
            return make_token(l, TOK_LT);

        // Greater-than
        case '>':
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_GTE);    }
            if (peek_char(l) == '>') { advance(l); return make_token(l, TOK_RSHIFT); }
            return make_token(l, TOK_GT);

        // Plus
        case '+':
            if (peek_char(l) == '+') { advance(l); return make_token(l, TOK_PLUS_PLUS);   }
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_PLUS_ASSIGN); }
            return make_token(l, TOK_PLUS);

        // Minus / arrow
        case '-':
            if (peek_char(l) == '-') { advance(l); return make_token(l, TOK_MINUS_MINUS);  }
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_MINUS_ASSIGN); }
            if (peek_char(l) == '>') { advance(l); return make_token(l, TOK_ARROW);        }
            return make_token(l, TOK_MINUS);

        // Star
        case '*':
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_STAR_ASSIGN); }
            return make_token(l, TOK_STAR);

        // Slash (comments already stripped above)
        case '/':
            if (peek_char(l) == '=') { advance(l); return make_token(l, TOK_SLASH_ASSIGN); }
            return make_token(l, TOK_SLASH);

        // Percent
        case '%':
            return make_token(l, TOK_PERCENT);

        // Ampersand
        case '&':
            if (peek_char(l) == '&') { advance(l); return make_token(l, TOK_AND); }
            return make_token(l, TOK_AMP);

        // Pipe
        case '|':
            if (peek_char(l) == '|') { advance(l); return make_token(l, TOK_OR); }
            return make_token(l, TOK_PIPE);

        // Question
        case '?': return make_token(l, TOK_QUESTION);
    }

    return error_token(l, "Unexpected character");
}

// ─── PEEK (non-consuming) ────────────────────────────────────────────────────

Token lexer_peek(Lexer *l) {
    const char *saved_current = l->current;
    const char *saved_start   = l->start;
    int saved_line = l->line;
    int saved_col  = l->col;

    Token t = lexer_next(l);

    l->current = saved_current;
    l->start   = saved_start;
    l->line    = saved_line;
    l->col     = saved_col;

    return t;
}

// ─── INIT ────────────────────────────────────────────────────────────────────

void lexer_init(Lexer *l, const char *source, const char *filename) {
    l->source   = source;
    l->current  = source;
    l->start    = source;
    l->line     = 1;
    l->col      = 1;
    l->filename = filename;
}

// ─── DEBUG PRINT ─────────────────────────────────────────────────────────────

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_INT_LIT:      return "INT_LIT";
        case TOK_FLOAT_LIT:    return "FLOAT_LIT";
        case TOK_STRING_LIT:   return "STRING_LIT";
        case TOK_CSTRING_LIT:  return "CSTRING_LIT";
        case TOK_CHAR_LIT:     return "CHAR_LIT";
        case TOK_BOOL_LIT:     return "BOOL_LIT";
        case TOK_NULL:         return "NULL";
        case TOK_IDENT:        return "IDENT";
        case TOK_PUBLIC:       return "public";
        case TOK_PRIVATE:      return "private";
        case TOK_INTERNAL:     return "internal";
        case TOK_USING:        return "using";
        case TOK_AS:           return "as";
        case TOK_ASM:          return "asm";
        case TOK_LET:          return "let";
        case TOK_CONST:        return "const";
        case TOK_RETURN:       return "return";
        case TOK_IF:           return "if";
        case TOK_ELSE:         return "else";
        case TOK_FOR:          return "for";
        case TOK_WHILE:        return "while";
        case TOK_LOOP:         return "loop";
        case TOK_MATCH:        return "match";
        case TOK_IN:           return "in";
        case TOK_BREAK:        return "break";
        case TOK_CONTINUE:     return "continue";
        case TOK_DEFER:        return "defer";
        case TOK_PANIC:        return "panic";
        case TOK_STRUCT:       return "struct";
        case TOK_ENUM:         return "enum";
        case TOK_INTERFACE:    return "interface";
        case TOK_EMBED:        return "embed";
        case TOK_MODULE:       return "module";
        case TOK_CLOBBERS:     return "clobbers";
        case TOK_NEW:          return "new";
        case TOK_DELETE:       return "delete";
        case TOK_SIZEOF:       return "sizeof";
        case TOK_ALIGNOF:      return "alignof";
        case TOK_TYPE_INT:     return "int";
        case TOK_TYPE_UINT:    return "uint";
        case TOK_TYPE_I8:      return "i8";
        case TOK_TYPE_U8:      return "u8";
        case TOK_TYPE_I16:     return "i16";
        case TOK_TYPE_U16:     return "u16";
        case TOK_TYPE_I64:     return "i64";
        case TOK_TYPE_U64:     return "u64";
        case TOK_TYPE_F32:     return "f32";
        case TOK_TYPE_F64:     return "f64";
        case TOK_TYPE_BOOL:    return "bool";
        case TOK_TYPE_CHAR:    return "char";
        case TOK_TYPE_STR:     return "str";
        case TOK_TYPE_CSTR:    return "cstr";
        case TOK_TYPE_VOID:    return "void";
        case TOK_TYPE_ANY:     return "any";
        case TOK_LBRACE:       return "{";
        case TOK_RBRACE:       return "}";
        case TOK_LPAREN:       return "(";
        case TOK_RPAREN:       return ")";
        case TOK_LBRACKET:     return "[";
        case TOK_RBRACKET:     return "]";
        case TOK_SEMICOLON:    return ";";
        case TOK_COMMA:        return ",";
        case TOK_DOT:          return ".";
        case TOK_COLON:        return ":";
        case TOK_COLONCOLON:   return "::";
        case TOK_QUESTION:     return "?";
        case TOK_HASH:         return "#";
        case TOK_AT:           return "@";
        case TOK_PLUS:         return "+";
        case TOK_MINUS:        return "-";
        case TOK_STAR:         return "*";
        case TOK_SLASH:        return "/";
        case TOK_PERCENT:      return "%";
        case TOK_AMP:          return "&";
        case TOK_PIPE:         return "|";
        case TOK_CARET:        return "^";
        case TOK_TILDE:        return "~";
        case TOK_BANG:         return "!";
        case TOK_LT:           return "<";
        case TOK_GT:           return ">";
        case TOK_LSHIFT:       return "<<";
        case TOK_RSHIFT:       return ">>";
        case TOK_AND:          return "&&";
        case TOK_OR:           return "||";
        case TOK_ASSIGN:       return "=";
        case TOK_EQ:           return "==";
        case TOK_NEQ:          return "!=";
        case TOK_LTE:          return "<=";
        case TOK_GTE:          return ">=";
        case TOK_PLUS_ASSIGN:  return "+=";
        case TOK_MINUS_ASSIGN: return "-=";
        case TOK_STAR_ASSIGN:  return "*=";
        case TOK_SLASH_ASSIGN: return "/=";
        case TOK_ARROW:        return "->";
        case TOK_FAT_ARROW:    return "=>";
        case TOK_DOTDOT:       return "..";
        case TOK_DOTDOT_EQ:    return "..=";
        case TOK_PLUS_PLUS:    return "++";
        case TOK_MINUS_MINUS:  return "--";
        case TOK_EOF:          return "EOF";
        case TOK_ERROR:        return "ERROR";
        default:               return "UNKNOWN";
    }
}

void token_print(Token t) {
    printf("[%4d:%3d] %-16s | %.*s\n",
        t.line, t.col,
        token_type_name(t.type),
        (int)t.length, t.start);
}
