#pragma once

#include "sema/sema.h"
#include "utils/utils.h"

typedef struct Codegen Codegen;

Codegen* codegen_create(MemArena* arena, Sema* sema);
void codegen_free(Codegen* codegen);
bool codegen_generate(Codegen* codegen, FILE* out, const char* source_name);
u32 codegen_error_count(Codegen* codegen);
