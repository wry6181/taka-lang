#pragma once
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ALIGN_UP_POW2
#define ALIGN_UP_POW2(n, p) (((u64)(n) + ((u64)(p) - 1)) & (~((u64)(p) - 1)))
#endif
#ifndef ARENA_BASE_POS
#define ARENA_BASE_POS (sizeof(MemArena))
#endif
#ifndef ARENA_ALIGN
#define ARENA_ALIGN (sizeof(void *))
#endif

typedef struct MemArena MemArena;
struct MemArena {
  u64 capacity;
  u64 pos;
};


#define PUSH_STRUCT(arena, T) (T *)arena_push((arena), sizeof(T), false)
#define PUSH_STRUCT_ZERO(arena, T) (T *)arena_push((arena), sizeof(T), true)
#define PUSH_ARRAY(arena, T, n) (T *)arena_push((arena), sizeof(T) * n, false)
#define PUSH_ARRAY_ZERO(arena, T, n) \
  (T *)arena_push((arena), sizeof(T) * n, true)

#define ARENA_SAVE_POS(arena) ((arena)->pos)
#define ARENA_RESTORE(arena, pos) arena_pop_to((arena), (pos), false)

MemArena *arena_create(u64 capacity);
void arena_destroy(MemArena *arena);
void *arena_push(MemArena *arena, u64 size, b32 zero);
void arena_pop(MemArena *arena, u64 size, b32 zero);
void arena_pop_to(MemArena *arena, u64 new_pos, b32 zero);
void arena_clear(MemArena *arena, b32 zero);
