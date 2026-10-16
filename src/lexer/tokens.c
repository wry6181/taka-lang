#include "tokens.h"

String8 tokenkind_to_string8(TokenKind kind) {
    switch (kind) {
        case TOKEN_EOF: return str8_lit("EOF");
        case TOKEN_INVALID: return str8_lit("INVALID");

        case TOKEN_NUMBER: return str8_lit("NUMBER");
        case TOKEN_STRING: return str8_lit("STRING");
        case TOKEN_CHAR: return str8_lit("CHAR");
        case TOKEN_IDENTIFIER: return str8_lit("IDENTIFIER");

        case TOKEN_OPEN_BRACKET: return str8_lit("OPEN_BRACKET");
        case TOKEN_CLOSE_BRACKET: return str8_lit("CLOSE_BRACKET");
        case TOKEN_OPEN_CURLY: return str8_lit("OPEN_CURLY");
        case TOKEN_CLOSE_CURLY: return str8_lit("CLOSE_CURLY");
        case TOKEN_OPEN_PAREN: return str8_lit("OPEN_PAREN");
        case TOKEN_CLOSE_PAREN: return str8_lit("CLOSE_PAREN");

        case TOKEN_ASSIGNMENT: return str8_lit("ASSIGNMENT");
        case TOKEN_EQUALS: return str8_lit("EQUALS");
        case TOKEN_NOT_EQUALS: return str8_lit("NOT_EQUALS");
        case TOKEN_NOT: return str8_lit("NOT");
        case TOKEN_LESS: return str8_lit("LESS");
        case TOKEN_LESS_EQUALS: return str8_lit("LESS_EQUALS");
        case TOKEN_GREATER: return str8_lit("GREATER");
        case TOKEN_GREATER_EQUALS: return str8_lit("GREATER_EQUALS");

        case TOKEN_PLUS: return str8_lit("PLUS");
        case TOKEN_PLUS_PLUS: return str8_lit("PLUS_PLUS");
        case TOKEN_MINUS: return str8_lit("MINUS");
        case TOKEN_MINUS_MINUS: return str8_lit("MINUS_MINUS");
        case TOKEN_STAR: return str8_lit("STAR");
        case TOKEN_SLASH: return str8_lit("SLASH");
        case TOKEN_PERCENT: return str8_lit("PERCENT");

        case TOKEN_PLUS_EQUALS: return str8_lit("PLUS_EQUALS");
        case TOKEN_MINUS_EQUALS: return str8_lit("MINUS_EQUALS");
        case TOKEN_STAR_EQUALS: return str8_lit("STAR_EQUALS");
        case TOKEN_SLASH_EQUALS: return str8_lit("SLASH_EQUALS");
        case TOKEN_PERCENT_EQUALS: return str8_lit("PERCENT_EQUALS");
        case TOKEN_AMP_EQUALS: return str8_lit("AMP_EQUALS");
        case TOKEN_PIPE_EQUALS: return str8_lit("PIPE_EQUALS");
        case TOKEN_CARET_EQUALS: return str8_lit("CARET_EQUALS");
        case TOKEN_LESS_LESS_EQUALS: return str8_lit("LESS_LESS_EQUALS");
        case TOKEN_GREATER_GREATER_EQUALS: return str8_lit("GREATER_GREATER_EQUALS");

        case TOKEN_AMP: return str8_lit("AMP");
        case TOKEN_CARET: return str8_lit("CARET");
        case TOKEN_PIPE: return str8_lit("PIPE");
        case TOKEN_TILDE: return str8_lit("TILDE");
        case TOKEN_LESS_LESS: return str8_lit("LESS_LESS");
        case TOKEN_GREATER_GREATER: return str8_lit("GREATER_GREATER");

        case TOKEN_AMP_AMP: return str8_lit("AMP_AMP");
        case TOKEN_PIPE_PIPE: return str8_lit("PIPE_PIPE");
        case TOKEN_QUESTION: return str8_lit("QUESTION");

        case TOKEN_COMMA: return str8_lit("COMMA");
        case TOKEN_SEMICOLON: return str8_lit("SEMICOLON");
        case TOKEN_COLON: return str8_lit("COLON");
        case TOKEN_DOT: return str8_lit("DOT");
        case TOKEN_DOT_DOT: return str8_lit("DOT_DOT");
        case TOKEN_COLON_COLON: return str8_lit("COLON_COLON");
        case TOKEN_ARROW: return str8_lit("ARROW");
        case TOKEN_FAT_ARROW: return str8_lit("FAT_ARROW");

        case TOKEN_FN: return str8_lit("FN");
        case TOKEN_IMPORT: return str8_lit("IMPORT");
        case TOKEN_TYPE: return str8_lit("TYPE");
        case TOKEN_STRUCT: return str8_lit("STRUCT");
        case TOKEN_ENUM: return str8_lit("ENUM");
        case TOKEN_EFFECT: return str8_lit("EFFECT");
        case TOKEN_HANDLER: return str8_lit("HANDLER");

        case TOKEN_PUB: return str8_lit("PUB");
        case TOKEN_PRIVATE: return str8_lit("PRIVATE");
        case TOKEN_MUT: return str8_lit("MUT");
        case TOKEN_CONST: return str8_lit("CONST");
        case TOKEN_AUTO: return str8_lit("AUTO");
        case TOKEN_WHERE: return str8_lit("WHERE");

        case TOKEN_IF: return str8_lit("IF");
        case TOKEN_ELSE: return str8_lit("ELSE");
        case TOKEN_WHILE: return str8_lit("WHILE");
        case TOKEN_FOR: return str8_lit("FOR");
        case TOKEN_IN: return str8_lit("IN");
        case TOKEN_BREAK: return str8_lit("BREAK");
        case TOKEN_CONTINUE: return str8_lit("CONTINUE");
        case TOKEN_RETURN: return str8_lit("RETURN");
        case TOKEN_MATCH: return str8_lit("MATCH");
        case TOKEN_TRANSACT: return str8_lit("TRANSACT");

        case TOKEN_PERFORM: return str8_lit("PERFORM");
        case TOKEN_HANDLE: return str8_lit("HANDLE");
        case TOKEN_WITH: return str8_lit("WITH");
        case TOKEN_SPAWN: return str8_lit("SPAWN");
        case TOKEN_RESUME: return str8_lit("RESUME");

        case TOKEN_SIZEOF: return str8_lit("SIZEOF");
        case TOKEN_OFFSETOF: return str8_lit("OFFSETOF");
        case TOKEN_TRUE: return str8_lit("TRUE");
        case TOKEN_FALSE: return str8_lit("FALSE");

        case TOKEN_I8: return str8_lit("I8");
        case TOKEN_I16: return str8_lit("I16");
        case TOKEN_I32: return str8_lit("I32");
        case TOKEN_I64: return str8_lit("I64");
        case TOKEN_U8: return str8_lit("U8");
        case TOKEN_U16: return str8_lit("U16");
        case TOKEN_U32: return str8_lit("U32");
        case TOKEN_U64: return str8_lit("U64");
        case TOKEN_F32: return str8_lit("F32");
        case TOKEN_F64: return str8_lit("F64");
        case TOKEN_CHAR_TYPE: return str8_lit("CHAR_TYPE");
        case TOKEN_BOOL: return str8_lit("BOOL");
        case TOKEN_SVIEW: return str8_lit("SVIEW");

        case TOKEN_COUNT: return str8_lit("COUNT");
    }
    return str8_lit("UNKNOWN");
}

bool token_one_of_many(Token token, int n, ...) {
    va_list args;
    va_start(args, n);
    for (int i = 0; i < n; i++) {
        if (token.kind == (TokenKind)va_arg(args, int)) {
            va_end(args);
            return TRUE;
        }
    }
    va_end(args);
    return FALSE;
}

void debug_token(Token token) {
    if (token_one_of_many(token, 3, TOKEN_NUMBER, TOKEN_IDENTIFIER, TOKEN_STRING)) {
        printf("%.*s %.*s\n", str8_fmt(tokenkind_to_string8(token.kind)), str8_fmt(token.value));
    } else {
        printf("%.*s\n", str8_fmt(tokenkind_to_string8(token.kind)));
    }
}



Token token_new(TokenKind kind, String8 value) {
    return (Token) {
        .kind = kind,
        .value = value,
    };
}
