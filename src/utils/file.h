#pragma once
#include "arena.h"
#include "types.h"
#include "string.h"

typedef struct BinFile BinFile;
struct BinFile {
    u8* data;
    String8 path;
    u64 size;
};

static inline BinFile read_file(MemArena* arena, const char* path) {
    BinFile result = {0};
    u64 start_pos = ARENA_SAVE_POS(arena);
    FILE* file = fopen(path, "rb");
    if(!file) {
        return result;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return result;
    }

    i64 file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return result;
    }
    rewind(file);

    u8* data = PUSH_ARRAY(arena, u8, file_size);

    if(!data && file_size > 0) {
        fclose(file);
        arena_pop_to(arena, start_pos, FALSE);
        return result;
    }

    u64 bytes_read = fread(data, sizeof(u8), file_size, file);
    fclose(file);
    if (bytes_read != (size_t)file_size) {
        arena_pop_to(arena, start_pos, FALSE);
        return result;
    }
    return (BinFile) {.size = bytes_read, .data = data, .path = str8(path)};

}

static inline String8 bin_to_str8(BinFile* bin) {
  return (String8) {
    .data = bin->data,
    .size = bin->size,
  };
}
