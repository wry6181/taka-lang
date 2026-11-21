#include "lexer/tokenizer.h"
#include "parser/parser.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 8192) {
    return 0;
  }
  MemArena* arena = arena_create(MByte(8));
  Tokenizer* tokenizer =
      tokenizer_create(arena, (u64)size + 1, (String8){(u8*)data, (u64)size});
  tokenizer_tokenize(tokenizer);

  jmp_buf error_jump;
  parser_set_error_jmp(&error_jump);
  if (setjmp(error_jump) == 0) {
    parse_program(arena, tokenizer->tokens, tokenizer->token_count);
  }
  parser_set_error_jmp(NULL);
  arena_destroy(arena);
  return 0;
}
