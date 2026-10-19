#include "tokenizer.h"
#include <assert.h>

static TokenKind tokenizer_keyword_kind(String8 value);
static TokenKind tokenizer_operator_kind(Tokenizer* tokenizer);
static bool tokenizer_is_two_char(TokenKind kind);
static void tokenizer_advance_token(Tokenizer* tokenizer, TokenKind kind);

bool tokenizer_at_eof(const Tokenizer* tokenizer) {
    if (tokenizer->pos >= tokenizer->source.size) {
        return TRUE;
    }
    return FALSE;
}

u8 tokenizer_peek(const Tokenizer* tokenizer) {
    if(tokenizer_at_eof(tokenizer)) {
        return 0;
    }
    return tokenizer->source.data[tokenizer->pos];
}

u8 tokenizer_peek_next(const Tokenizer* tokenizer) {
    if(tokenizer->pos + 1 >=tokenizer->source.size) {
        return 0;
    }
    return tokenizer->source.data[tokenizer->pos + 1];
}

// Csak a lexer belső használatára: a 3 karakteres operátorokhoz kell.
static u8 tokenizer_peek_next_next(const Tokenizer* tokenizer) {
    if(tokenizer->pos + 2 >= tokenizer->source.size) {
        return 0;
    }
    return tokenizer->source.data[tokenizer->pos + 2];
}

String8 tokenizer_slice(const Tokenizer* tokenizer, u64 start, u64 end){
     assert(start <= end);
     assert(end <= tokenizer->source.size);

     return (String8) {
          .data = tokenizer->source.data + start,
          .size = end - start,
     };
}

bool is_digit(u8 c) {
    return c >= '0' && c <= '9';
}

bool is_alpha(u8 c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

bool is_ident_continue(u8 c){
    return is_alpha(c) || is_digit(c);
}

bool is_whitespace(u8 c){
    return c == ' '  ||
           c == '\t' ||
           c == '\n' ||
           c == '\r';
}

bool is_hex_digit(u8 c){
    return is_digit(c) ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool is_bin_digit(u8 c){
    return c == '0' || c == '1';
}

// Deciális vagy lebegőpontos literál folytatása: [0-9_]
bool is_float_digit(u8 c){
    return is_digit(c) || c == '_';
}

void tokenizer_advance(Tokenizer* tokenizer) {
    if (tokenizer_at_eof(tokenizer)) {
        return;
    }

    u8 c = tokenizer->source.data[tokenizer->pos++];
    if (c == '\n') {
        tokenizer->line++;
        tokenizer->column = 1;
    } else {
        tokenizer->column++;
    }

}

void tokenizer_push(Tokenizer* tokenizer, TokenKind kind, u64 start, u64 line, u64 column) {
    assert(start <= tokenizer->pos);
    assert(tokenizer->pos <= tokenizer->source.size);
    assert(tokenizer->token_count < tokenizer->token_capacity);

    String8 value = tokenizer_slice(tokenizer, start, tokenizer->pos);

    tokenizer->tokens[tokenizer->token_count++] =
        token_new(kind, value, line, column);
}

Tokenizer* tokenizer_create(MemArena* arena, u64 token_capacity, String8 source) {
    Tokenizer* tokenizer = PUSH_STRUCT(arena, Tokenizer);

    tokenizer->column = 1;
    tokenizer->line = 1;
    tokenizer->pos = 0;
    tokenizer->source = source;
    tokenizer->token_capacity = token_capacity;
    tokenizer->token_count = 0;
    tokenizer->tokens = PUSH_ARRAY(arena, Token, token_capacity);

    return tokenizer;
}

void tokenizer_tokenize(Tokenizer* t) {
    while (!tokenizer_at_eof(t)) {
        u8 c = tokenizer_peek(t);
        u8 n = tokenizer_peek_next(t);

        if (is_whitespace(c)) {
            tokenizer_advance(t);
            continue;
        }

        u64 start = t->pos;
        u64 line = t->line;
        u64 column = t->column;

        // // sor-komment: a \nt a whitespace-ág eszi meg
        if (c == '/' && n == '/') {
            while (!tokenizer_at_eof(t) && tokenizer_peek(t) != '\n') {
                tokenizer_advance(t);
            }
            continue;
        }

        // /* blokk-komment, egymásba ágyazható (01.tk: "nestable")
        if (c == '/' && n == '*') {
            tokenizer_advance(t);
            tokenizer_advance(t);
            u64 depth = 1;
            while (depth > 0 && !tokenizer_at_eof(t)) {
                if (tokenizer_peek(t) == '/' && tokenizer_peek_next(t) == '*') {
                    tokenizer_advance(t);
                    tokenizer_advance(t);
                    depth++;
                } else if (tokenizer_peek(t) == '*' && tokenizer_peek_next(t) == '/') {
                    tokenizer_advance(t);
                    tokenizer_advance(t);
                    depth--;
                } else {
                    tokenizer_advance(t);
                }
            }
            if (depth > 0) {
                tokenizer_push(t, TOKEN_INVALID, start, line, column);
            }
            continue;
        }

        // String literál: "..."
        if (c == '"') {
            tokenizer_advance(t);
            while (!tokenizer_at_eof(t) &&
                   tokenizer_peek(t) != '"' &&
                   tokenizer_peek(t) != '\n') {
                // escape: a \ jel után 1 karaktert ugrik (parser oldja majd fel)
                if (tokenizer_peek(t) == '\\') {
                    tokenizer_advance(t);
                }
                tokenizer_advance(t);
            }
            if (tokenizer_peek(t) == '"') {
                tokenizer_advance(t);
                tokenizer_push(t, TOKEN_STRING, start, line, column);
            } else {
                // lezáratlan literál: hibajelzés a string kezdeténél
                tokenizer_push(t, TOKEN_INVALID, start, line, column);
            }
            continue;
        }

        // Char literál: 'x'
        if (c == '\'') {
            tokenizer_advance(t);
            if (tokenizer_peek(t) == '\\') {
                tokenizer_advance(t); // a \ jön, móka
                tokenizer_advance(t); // az escape-elt karakter
            } else if (!tokenizer_at_eof(t) &&
                       tokenizer_peek(t) != '\'' &&
                       tokenizer_peek(t) != '\n') {
                tokenizer_advance(t);
            }
            if (tokenizer_peek(t) == '\'') {
                tokenizer_advance(t);
                tokenizer_push(t, TOKEN_CHAR, start, line, column);
            } else {
                tokenizer_push(t, TOKEN_INVALID, start, line, column);
            }
            continue;
        }

        // Identifier vagy keyword
        if (is_alpha(c)) {
            tokenizer_advance(t);

            while (!tokenizer_at_eof(t) &&
                   is_ident_continue(tokenizer_peek(t))) {
                tokenizer_advance(t);
            }

            String8 value = tokenizer_slice(t, start, t->pos);
            TokenKind kind = tokenizer_keyword_kind(value);

            tokenizer_push(t, kind, start, line, column);
            continue;
        }

        // Szám literál: dec / hex / bin, lebegőpontos, aláhúzásokkal
        if (is_digit(c)) {
            if (c == '0' && (n == 'x' || n == 'X')) {
                tokenizer_advance(t); // 0
                tokenizer_advance(t); // x
                while (!tokenizer_at_eof(t) &&
                       (is_hex_digit(tokenizer_peek(t)) || tokenizer_peek(t) == '_')) {
                    tokenizer_advance(t);
                }
            } else if (c == '0' && (n == 'b' || n == 'B')) {
                tokenizer_advance(t); // 0
                tokenizer_advance(t); // b
                while (!tokenizer_at_eof(t) &&
                       (is_bin_digit(tokenizer_peek(t)) || tokenizer_peek(t) == '_')) {
                    tokenizer_advance(t);
                }
            } else {
                while (!tokenizer_at_eof(t) && is_float_digit(tokenizer_peek(t))) {
                    tokenizer_advance(t);
                }
                // 3.14 / 0.5: a . csak szám előtt csinál NUMBER-t
                if (tokenizer_peek(t) == '.' && is_digit(tokenizer_peek_next(t))) {
                    tokenizer_advance(t); // .
                    while (!tokenizer_at_eof(t) && is_float_digit(tokenizer_peek(t))) {
                        tokenizer_advance(t);
                    }
                }
            }
            tokenizer_push(t, TOKEN_NUMBER, start, line, column);
            continue;
        }

        // Operátor vagy írásjel
        TokenKind kind = tokenizer_operator_kind(t);

        if (kind != TOKEN_INVALID) {
            // Az operátor tényleges hosszát ez fogyasztja (1/2/3 karakter).
            tokenizer_advance_token(t, kind);
            tokenizer_push(t, kind, start, line, column);
            continue;
        }

        // Ismeretlen karakter
        tokenizer_advance(t);
        tokenizer_push(t, TOKEN_INVALID, start, line, column);
    }

    tokenizer_push(t, TOKEN_EOF, t->pos, t->line, t->column);
}

static TokenKind tokenizer_keyword_kind(String8 value) {
    #define KEYWORD(s, kind) \
        if (str8_eq(value, str8_lit(s))) return kind

    // Items
    KEYWORD("fn",        TOKEN_FN);
    KEYWORD("import",    TOKEN_IMPORT);
    KEYWORD("type",      TOKEN_TYPE);
    KEYWORD("struct",    TOKEN_STRUCT);
    KEYWORD("enum",      TOKEN_ENUM);
    KEYWORD("effect",    TOKEN_EFFECT);
    KEYWORD("handler",   TOKEN_HANDLER);

    // Visibility & binding
    KEYWORD("pub",       TOKEN_PUB);
    KEYWORD("private",   TOKEN_PRIVATE);
    KEYWORD("mut",       TOKEN_MUT);
    KEYWORD("const",     TOKEN_CONST);
    KEYWORD("auto",      TOKEN_AUTO);
    KEYWORD("where",     TOKEN_WHERE);

    // Control flow
    KEYWORD("if",        TOKEN_IF);
    KEYWORD("else",      TOKEN_ELSE);
    KEYWORD("while",     TOKEN_WHILE);
    KEYWORD("for",       TOKEN_FOR);
    KEYWORD("in",        TOKEN_IN);
    KEYWORD("break",     TOKEN_BREAK);
    KEYWORD("continue",  TOKEN_CONTINUE);
    KEYWORD("return",    TOKEN_RETURN);
    KEYWORD("match",     TOKEN_MATCH);
    KEYWORD("transact",  TOKEN_TRANSACT);

    // Effects
    KEYWORD("perform",   TOKEN_PERFORM);
    KEYWORD("handle",    TOKEN_HANDLE);
    KEYWORD("with",      TOKEN_WITH);
    KEYWORD("spawn",     TOKEN_SPAWN);
    KEYWORD("resume",    TOKEN_RESUME);

    // Misc
    KEYWORD("sizeof",    TOKEN_SIZEOF);
    KEYWORD("offsetof",  TOKEN_OFFSETOF);
    KEYWORD("true",      TOKEN_TRUE);
    KEYWORD("false",     TOKEN_FALSE);

    // Primitive types
    KEYWORD("i8",        TOKEN_I8);
    KEYWORD("i16",       TOKEN_I16);
    KEYWORD("i32",       TOKEN_I32);
    KEYWORD("i64",       TOKEN_I64);
    KEYWORD("u8",        TOKEN_U8);
    KEYWORD("u16",       TOKEN_U16);
    KEYWORD("u32",       TOKEN_U32);
    KEYWORD("u64",       TOKEN_U64);
    KEYWORD("f32",       TOKEN_F32);
    KEYWORD("f64",       TOKEN_F64);
    KEYWORD("char",      TOKEN_CHAR_TYPE);
    KEYWORD("bool",      TOKEN_BOOL);
    KEYWORD("sview",     TOKEN_SVIEW);

    #undef KEYWORD

    return TOKEN_IDENTIFIER;
}

static bool tokenizer_is_two_char(TokenKind kind) {
    switch (kind) {
        case TOKEN_COLON_COLON:      // ::
        case TOKEN_PERCENT_EQUALS:   // %=
        case TOKEN_PLUS_PLUS:        // ++
        case TOKEN_PLUS_EQUALS:      // +=
        case TOKEN_MINUS_MINUS:      // --
        case TOKEN_MINUS_EQUALS:     // -=
        case TOKEN_ARROW:            // ->
        case TOKEN_STAR_EQUALS:      // *=
        case TOKEN_SLASH_EQUALS:     // /=
        case TOKEN_EQUALS:           // ==
        case TOKEN_FAT_ARROW:        // =>
        case TOKEN_NOT_EQUALS:       // !=
        case TOKEN_LESS_LESS:        // <<
        case TOKEN_LESS_EQUALS:      // <=
        case TOKEN_GREATER_GREATER:  // >>
        case TOKEN_GREATER_EQUALS:   // >=
        case TOKEN_AMP_AMP:          // &&
        case TOKEN_AMP_EQUALS:       // &=
        case TOKEN_PIPE_PIPE:        // ||
        case TOKEN_PIPE_EQUALS:      // |=
        case TOKEN_CARET_EQUALS:     // ^=
        case TOKEN_DOT_DOT:          // ..
            return TRUE;
        default:
            return FALSE;
    }
}

// Egy token jelzéséhez ténylegesen elfogyasztott karakterek száma:
// 1, vagy 2 a kétchar-soknál, 3 a háromchar-soknál (<<= >>=).
static void tokenizer_advance_token(Tokenizer* t, TokenKind kind) {
    tokenizer_advance(t);
    if (kind == TOKEN_LESS_LESS_EQUALS || kind == TOKEN_GREATER_GREATER_EQUALS) {
        tokenizer_advance(t);
        tokenizer_advance(t);
    } else if (tokenizer_is_two_char(kind)) {
        tokenizer_advance(t);
    }
}

static TokenKind tokenizer_operator_kind(Tokenizer* t) {
    u8 c = tokenizer_peek(t);
    u8 n = tokenizer_peek_next(t);
    u8 n2 = tokenizer_peek_next_next(t);

    switch (c) {
        case '[': return TOKEN_OPEN_BRACKET;
        case ']': return TOKEN_CLOSE_BRACKET;
        case '{': return TOKEN_OPEN_CURLY;
        case '}': return TOKEN_CLOSE_CURLY;
        case '(': return TOKEN_OPEN_PAREN;
        case ')': return TOKEN_CLOSE_PAREN;
        case ',': return TOKEN_COMMA;
        case ';': return TOKEN_SEMICOLON;
        case ':':
            if (n == ':') return TOKEN_COLON_COLON;
            return TOKEN_COLON;

        case '?': return TOKEN_QUESTION;
        case '~': return TOKEN_TILDE;
        case '%':
            if (n == '=') return TOKEN_PERCENT_EQUALS;
            return TOKEN_PERCENT;

        case '+':
            if (n == '+') return TOKEN_PLUS_PLUS;
            if (n == '=') return TOKEN_PLUS_EQUALS;
            return TOKEN_PLUS;

        case '-':
            if (n == '-') return TOKEN_MINUS_MINUS;
            if (n == '=') return TOKEN_MINUS_EQUALS;
            if (n == '>') return TOKEN_ARROW;
            return TOKEN_MINUS;

        case '*':
            if (n == '=') return TOKEN_STAR_EQUALS;
            return TOKEN_STAR;

        case '/':
            if (n == '=') return TOKEN_SLASH_EQUALS;
            return TOKEN_SLASH;

        case '=':
            if (n == '=') return TOKEN_EQUALS;
            if (n == '>') return TOKEN_FAT_ARROW;
            return TOKEN_ASSIGNMENT;

        case '!':
            if (n == '=') return TOKEN_NOT_EQUALS;
            return TOKEN_NOT;

        case '<':
            if (n == '<') {
                if (n2 == '=') {
                    return TOKEN_LESS_LESS_EQUALS;
                }
                return TOKEN_LESS_LESS;
            }
            if (n == '=') return TOKEN_LESS_EQUALS;
            return TOKEN_LESS;

        case '>':
            if (n == '>') {
                if (n2 == '=') {
                    return TOKEN_GREATER_GREATER_EQUALS;
                }
                return TOKEN_GREATER_GREATER;
            }
            if (n == '=') return TOKEN_GREATER_EQUALS;
            return TOKEN_GREATER;

        case '&':
            if (n == '&') return TOKEN_AMP_AMP;
            if (n == '=') return TOKEN_AMP_EQUALS;
            return TOKEN_AMP;

        case '|':
            if (n == '|') return TOKEN_PIPE_PIPE;
            if (n == '=') return TOKEN_PIPE_EQUALS;
            return TOKEN_PIPE;

        case '^':
            if (n == '=') return TOKEN_CARET_EQUALS;
            return TOKEN_CARET;

        case '.':
            if (n == '.') return TOKEN_DOT_DOT;
            return TOKEN_DOT;

        default:
            return TOKEN_INVALID;
    }
}
