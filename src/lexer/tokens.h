#pragma once
#include "utils/utils.h"
#include <stdio.h>
#include <stdarg.h>

typedef enum TokenKind TokenKind;
enum TokenKind {
    TOKEN_EOF = 0,
    TOKEN_INVALID,

    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_CHAR,     // 'x'
    TOKEN_IDENTIFIER,

    // Brackets
    TOKEN_OPEN_BRACKET,   // [
    TOKEN_CLOSE_BRACKET,  // ]
    TOKEN_OPEN_CURLY,     // {
    TOKEN_CLOSE_CURLY,    // }
    TOKEN_OPEN_PAREN,     // (
    TOKEN_CLOSE_PAREN,    // )

    // Assignment & comparison
    TOKEN_ASSIGNMENT,     // =
    TOKEN_EQUALS,         // ==
    TOKEN_NOT_EQUALS,     // !=
    TOKEN_NOT,            // !
    TOKEN_LESS,           // <
    TOKEN_LESS_EQUALS,    // <=
    TOKEN_GREATER,        // >
    TOKEN_GREATER_EQUALS, // >=

    // Arithmetic
    TOKEN_PLUS,           // +
    TOKEN_PLUS_PLUS,      // ++
    TOKEN_MINUS,          // -
    TOKEN_MINUS_MINUS,    // --
    TOKEN_STAR,           // *
    TOKEN_SLASH,          // /
    TOKEN_PERCENT,        // %

    // Compound assignment
    TOKEN_PLUS_EQUALS,            // +=
    TOKEN_MINUS_EQUALS,           // -=
    TOKEN_STAR_EQUALS,            // *=
    TOKEN_SLASH_EQUALS,           // /=
    TOKEN_PERCENT_EQUALS,         // %=
    TOKEN_AMP_EQUALS,             // &=
    TOKEN_PIPE_EQUALS,            // |=
    TOKEN_CARET_EQUALS,           // ^=
    TOKEN_LESS_LESS_EQUALS,       // <<=
    TOKEN_GREATER_GREATER_EQUALS, // >>=

    // Bitwise & shift
    TOKEN_AMP,             // &
    TOKEN_CARET,           // ^
    TOKEN_PIPE,            // |
    TOKEN_TILDE,           // ~
    TOKEN_LESS_LESS,       // <<
    TOKEN_GREATER_GREATER, // >>

    // Logical & ternary
    TOKEN_AMP_AMP,   // &&
    TOKEN_PIPE_PIPE, // ||
    TOKEN_QUESTION,  // ? (the : half already exists)

    // Punctuation
    TOKEN_COMMA,          // ,
    TOKEN_SEMICOLON,      // ;
    TOKEN_COLON,          // :
    TOKEN_DOT,            // .
    TOKEN_DOT_DOT,        // ..
    TOKEN_COLON_COLON,    // ::
    TOKEN_ARROW,          // ->
    TOKEN_FAT_ARROW,      // =>


    // Keywords — items
    TOKEN_FN,
    TOKEN_IMPORT,
    TOKEN_TYPE,
    TOKEN_STRUCT,
    TOKEN_ENUM,
    TOKEN_EFFECT,
    TOKEN_HANDLER,

    // Keywords — visibility & binding
    TOKEN_PUB,
    TOKEN_PRIVATE,
    TOKEN_MUT,
    TOKEN_CONST,
    TOKEN_AUTO,
    TOKEN_WHERE,

    // Keywords — control flow
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,
    TOKEN_MATCH,
    TOKEN_TRANSACT,

    // Keywords — effects
    TOKEN_PERFORM,
    TOKEN_HANDLE,
    TOKEN_WITH,
    TOKEN_SPAWN,
    TOKEN_RESUME,

    // Keywords — misc
    TOKEN_SIZEOF,
    TOKEN_OFFSETOF,
    TOKEN_TRUE,
    TOKEN_FALSE,

    // Primitive types
    TOKEN_I8,
    TOKEN_I16,
    TOKEN_I32,
    TOKEN_I64,
    TOKEN_U8,
    TOKEN_U16,
    TOKEN_U32,
    TOKEN_U64,
    TOKEN_F32,
    TOKEN_F64,
    TOKEN_CHAR_TYPE, // `char` (TOKEN_CHAR is the 'x' literal)
    TOKEN_BOOL,
    TOKEN_SVIEW,

    TOKEN_COUNT,

};

typedef struct Token Token;
struct Token {
    String8 value;
    TokenKind kind;

};

String8 tokenkind_to_string8(TokenKind kind);

bool token_one_of_many(Token token, int n, ...);

void debug_token(Token token);

Token token_new(TokenKind kind, String8 value);
