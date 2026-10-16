#include <stdio.h>

#include "utils/utils.h"

int main(void) {
    MemArena* arena = arena_create(MByte(10));
    BinFile bf = read_file(arena, "example/00.tk");
    printf("%s %lu %.*s", bf.data, bf.size, str8_fmt(bf.path));
    arena_destroy(arena);
    return 0;
}
