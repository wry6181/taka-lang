#pragma once
#include "utils/arena.h"
#include "utils/types.h"
#include "utils/utils.h"
#include "tokens.h"

typedef struct Tokenizer Tokenizer;
struct Tokenizer {
    u64 line;
    u64 pos;
    String8 source;
    u64 column;

    Token* tokens;
    u64 token_count;
    u64 token_capacity;
};

bool tokenizer_at_eof(const Tokenizer* tokenizer);
u8 tokenizer_peek(const Tokenizer* tokenizer);
u8 tokenizer_peek_next(const Tokenizer* tokenizer);
String8 tokenizer_slice(const Tokenizer* tokenizer, u64 start, u64 end);
bool is_digit(u8 c);
bool is_alpha(u8 c);
bool is_ident_continue(u8 c);
bool is_whitespace(u8 c);
bool is_hex_digit(u8 c);
bool is_bin_digit(u8 c);
bool is_float_digit(u8 c);

void tokenizer_advance(Tokenizer* tokenizer);
// A token tényleges sáv-koordinátáit is át kell adni (hol kezdődött).
void tokenizer_push(Tokenizer* tokenizer, TokenKind kind, u64 start, u64 line, u64 column);

Tokenizer* tokenizer_create(MemArena* arena, u64 token_capacity, String8 source);
void tokenizer_tokenize(Tokenizer* tokenizer);
