#pragma once
#include <stddef.h>

#include "types.h"

// Swappable hash map facade — the ONLY map API used by the project
// (parser lookups today, symbol tables / runtime later).
//
// u64 key -> u64 value, auto-growing, upsert semantics, no delete
// (write-once/read-many tables: no tombstones, trivial invariants).
//
// Backends (one .c per backend, identical API — callers never see a
// backend header):
//   map_khash.c  default: wraps khash/klib (third_party/khash.h),
//                battle-tested malloc-based implementation
//   map_arena.c  native open addressing on a MemArena, no malloc
// Swap with:     make MAP_BACKEND=arena    (or =khash, the default)
//
// Both backends implement the full facade: upsert, get, has, count,
// delete, clear, and slot iteration. Delete strategy differs internally
// (khash: tombstones; arena: backward shift, no tombstones) but the
// observable semantics are identical.
//
// Design notes:
//   - Not threadsafe; the compiler is single-threaded, that is the contract.
//   - map_get returns FALSE for a missing key and leaves *out untouched,
//     so a preloaded default survives a miss (Go zero-value parity).
typedef struct MemArena MemArena;
typedef struct Map Map;

// arena: honored by arena-backed implementations; malloc-backed ones
// ignore it. Free with map_free() (no-op for arena-backed maps).
Map* map_new(MemArena* arena);
void map_free(Map* map);

// Upsert. FALSE only on NULL map or allocation failure.
bool map_set(Map* map, u64 key, u64 value);

// FALSE on miss (out_value not written) or NULL map.
bool map_get(const Map* map, u64 key, u64* out_value);
bool map_has(const Map* map, u64 key);

u64 map_count(const Map* map);

// Delete one key. FALSE on miss (or NULL map).
bool map_del(Map* map, u64 key);

// Remove every entry, keep the allocation.
void map_clear(Map* map);

// Iterate live entries: start *slot at 0; each TRUE call fills out_key /
// out_value (either may be NULL) and advances *slot; FALSE = done.
// Do not mutate the map between calls.
bool map_iter(const Map* map, u64* slot, u64* out_key, u64* out_value);

// Pointer / function-pointer helpers: hide the uintptr_t casts so callers
// store (LedHandler) / (NudHandler) / AST pointers without doing
// integer-conversion plumbing themselves.
static inline void map_set_ptr(Map* map, u64 key, void* ptr) {
    map_set(map, key, (u64)(uintptr_t)ptr);
}

static inline void* map_get_ptr(Map* map, u64 key) {
    u64 value = 0;
    return map_get(map, key, &value) ? (void*)(uintptr_t)value : NULL;
}
