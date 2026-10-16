#include "utils/utils.h"
#include "tokens.h"

typedef struct Tokenizer Tokenizer;
struct Tokenizer {
    u64 line;
    u64 pos;
    String8 source;
    Token* tokens;
};
