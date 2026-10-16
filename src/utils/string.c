#include "string.h"
#include "arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

String8 str8_append(MemArena *arena, String8 a, String8 b) {
  String8 out = {0};
  out.size = a.size + b.size;
  out.data = (u8 *)arena_push(arena, out.size, false);
  if (!out.data) {
    return out;
  }
  memcpy(out.data, a.data, a.size);
  memcpy(out.data + a.size, b.data, b.size);
  return out;
}

String8 str8_to_cstr(MemArena *arena, String8 a) {
  String8 out = {0};
  out.size = a.size + 1;
  out.data = (u8 *)arena_push(arena, out.size, false);
  if (!out.data) {
    return out;
  }
  memcpy(out.data, a.data, a.size);
  out.data[a.size] = '\0';
  return out;
}

String8 str8_copy(MemArena *arena, String8 s) {
  String8 out = {0};
  out.data = (u8 *)arena_push(arena, s.size + 1, false);
  if (!out.data)
    return out;
  out.size = s.size;
  memcpy(out.data, s.data, s.size);
  out.data[s.size] = '\0';
  return out;
}

String8 str8_pushfv(MemArena *arena, char const *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  i32 n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n <= 0)
    return (String8){0};
  char *buf = PUSH_ARRAY(arena, char, (u64)n + 1);
  vsnprintf(buf, (u64)n + 1, fmt, ap);
  return (String8){(u8 *)buf, (u64)n};
}

String8 str8_pushf(MemArena *arena, char const *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  String8 result = str8_pushfv(arena, fmt, ap);
  va_end(ap);
  return result;
}

void arena_print_mem(String8 name, MemArena *arena) {
  u8 *base = (u8 *)arena;
  printf("this arena: %.*s \n", str8_fmt(name));
  for (u64 i = 0; i < arena->pos; ++i) {
    printf("%02X ", base[i]);
    if ((i + 1) % 8 == 0) {
      printf("\n");
    }
  }
  printf("\n");
}
