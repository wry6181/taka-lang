#pragma once

#include "types.h"
#include <stdarg.h>
#include <string.h>

// Forward declare arena to avoid circular include (see core/logger.h:5 pattern).
// string.h does NOT include arena.h; arena-dependent functions are declared
// with opaque mem_arena* and defined in string.c which includes both.
typedef struct MemArena MemArena;

#define str8_fmt(str8) (int)(str8).size, (str8).data
#define str8_lit(str) (String8){(u8 *)(str), (sizeof((str)) - 1)}

#define str8(cstr) (String8){(u8 *)(cstr), (u64)strlen(cstr)}

#define str8_cstr(cstr) (String8){(u8 *)(cstr), (cstr) ? strlen(cstr) : 0}

static inline String8 str8_substr(String8 *str, u64 start, u64 end) {
  end = MIN(end, str->size);
  start = MIN(start, end);
  return (String8){.data = str->data + start, .size = end - start};
}

static inline void str8_cut_left(String8 *str, u64 amount) {
  if (str->size < amount) {
    return;
  }
  str->data += amount;
  str->size -= amount;
}

static inline void str8_cut_right(String8 *str, u64 amount) {
  if (str->size < amount) {
    return;
  }
  str->size -= amount;
}

static inline void str8_trim(String8 *str) {
  while (0 < str->size && str->data[0] == ' ') {
    str8_cut_left(str, 1);
  }
  while (0 < str->size && str->data[str->size - 1] == ' ') {
    str8_cut_right(str, 1);
  }
}

static inline String8 str8_slice_at(String8 *str, char c) {
  String8 result = {0};
  for (u64 i = 0; i < str->size; ++i) {
    if (str->data[i] == c) {
      result.data = &str->data[0];
      result.size = i;
      str8_cut_left(str, i + 1);
      return result;
    }
  }
  return *str;
}

static inline b32 str8_eq(String8 a, String8 b) {
  if (a.size != b.size) {
    return FALSE;
  }
  if (a.size == 0) {
    return TRUE;
  }
  return memcmp(a.data, b.data, a.size) == 0;
}

static inline b32 str8_eq_ic(String8 a, String8 b) {
  if (a.size != b.size) {
    return FALSE;
  }
  for (u64 i = 0; i < a.size; i++) {
    char ca = (char)a.data[i], cb = (char)b.data[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca += 32;
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb += 32;
    }
    if (ca != cb) {
      return FALSE;
    }
  }
  return TRUE;
}

static inline String8 str8_triml(String8 s) {
  u64 i = 0;
  while (i < s.size && (s.data[i] == ' ' || s.data[i] == '\t')) {
    i++;
  }
  return (String8){s.data + i, s.size - i};
}

static inline u64 str8_to_u64(String8 s) {
  u64 val = 0;
  for (u64 i = 0; i < s.size; i++) {
    if (s.data[i] < '0' || s.data[i] > '9') {
      break;
    }
    val = val * 10 + (u64)(s.data[i] - '0');
  }
  return val;
}

static inline i32 str8_len(u8 *s) {
  if (!s) {
    return 0;
  }
  i32 len = 0;
  while (s[len] != '\0') {
    len++;
  }
  return len;
}

static inline String8 str8_find_header(String8 headers, String8 name) {
  for (u64 i = 0; i + name.size + 2 <= headers.size; i++) {
    if (str8_eq_ic((String8){headers.data + i, name.size},
                   (String8){name.data, name.size})) {
      u64 j = i + name.size;
      while (j < headers.size && headers.data[j] == ' ')
        j++;
      if (j < headers.size && headers.data[j] == ':') {
        j++;
        while (j < headers.size && headers.data[j] == ' ')
          j++;
        u64 start = j;
        while (j < headers.size && headers.data[j] != '\r')
          j++;
        return (String8){headers.data + start, j - start};
      }
    }
  }
  return (String8){0};
}

static inline b8 str8_is_empty(String8 *s) {
  if (!s) {
    return FALSE;
  }
  if (s->size == 0) {
    return TRUE;
  }
  return FALSE;
}

// Arena-dependent helpers — forward-declared here, defined in string.c
// (string.c includes arena.h to get arena_push). No circular include.
String8 str8_append(MemArena *arena, String8 a, String8 b);
String8 str8_to_cstr(MemArena *arena, String8 a);
String8 str8_copy(MemArena *arena, String8 s);
String8 str8_pushfv(MemArena *arena, char const *fmt, va_list ap);
String8 str8_pushf(MemArena *arena, char const *fmt, ...);

// Debug helper that needs both String8 and arena — also defined in string.c
void arena_print_mem(String8 name, MemArena *arena);
#define ARENA_MEM_PRINT(x) arena_print_mem(str8_lit(#x), x)
