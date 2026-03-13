// ciren/parser.h

#ifndef CIREN_PARSER_H
#define CIREN_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer    *lexer;
    Arena    *arena;
    Token     current;
    Token     previous;
    int       had_error;
    int       panic_mode;    // suppress cascading errors
    int  no_struct_lit;  // suppress TypeName{} parsing in condition contexts
    const char *filename;
} Parser;

void     parser_init(Parser *p, Lexer *l, Arena *a, const char *filename);
AstNode *parse(Parser *p);   // parse entire .ci file → NODE_PROGRAM

#endif // CIREN_PARSER_H