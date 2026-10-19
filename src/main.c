#include <stdio.h>

#include "utils/utils.h"
#include "lexer/tokenizer.h"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "example/00.tk";

    MemArena* file_arena = arena_create(MByte(10));
    MemArena* tokenizer_arena = arena_create(MByte(10));
    BinFile bf = read_file(file_arena, path);
    if (bf.size == 0) {
        fprintf(stderr, "error: could not read %s\n", path);
        return 1;
    }
    String8 source = bin_to_str8(&bf);

    // +1 a lezáró TOKEN_EOF-nak
    Tokenizer* tokenizer = tokenizer_create(tokenizer_arena, source.size + 1, source);

    tokenizer_tokenize(tokenizer);

    u64 error_count = 0;
    for (u64 i = 0; i < tokenizer->token_count; ++i) {
        Token token = tokenizer->tokens[i];
        debug_token(token);
        if (token.kind == TOKEN_INVALID || token.kind == TOKEN_UNKNOWN) {
            error_count++;
            fprintf(stderr, "error [%s] %.*s [%lu:%lu]\n",
                    path,
                    (int)token.value.size, (char*)token.value.data,
                    token.line, token.column);
        }
    }
    if (error_count > 0) {
        fprintf(stderr, "%lu error(s)\n", error_count);
        return 1;
    }

    arena_destroy(file_arena);
    arena_destroy(tokenizer_arena);
    return 0;
}
