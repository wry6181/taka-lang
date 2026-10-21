#pragma once
#include <stddef.h>

#include "types.h"

// Growable array — the C equivalent of Go's make([]T, 0) + append.
// (The tlaceby/parser-series Parse() builds ast.BlockStmt.Body exactly
// this way; see parser.c parse_program().)
//
// Arena-backed: growth is geometric (doubling). Like the arena map
// backend, the abandoned arrays cannot be freed by a bump allocator —
// they stay until arena_destroy(), and their total is less than the
// current array, so peak arena use from growth is about 2x one array.
//
// Elements are stored BY VALUE (memcpy of elem_size bytes). For pointer
// element types ([]T* workloads: AST node lists) use the vec_push_ptr /
// vec_at_ptr helpers below.
typedef struct MemArena MemArena;
typedef struct Vector Vector;

// cap_hint is rounded up to a power of two, minimum 8.
// Returns NULL on NULL arena or elem_size == 0.
Vector* vec_new(MemArena* arena, u64 elem_size, u64 cap_hint);

// Append one element (copied in). Returns the slot, or NULL on OOM /
// NULL vector.
void* vec_push(Vector* v, const void* elem);

// NULL if the vector is NULL or index is out of range.
void* vec_at(const Vector* v, u64 index);
u64 vec_len(const Vector* v);

// Keep the allocation, drop the elements (reuse as empty).
void vec_clear(Vector* v);

// Pointer-element sugar: []T* vectors without manual &/* plumbing.
static inline void vec_push_ptr(Vector* v, void* ptr) {
    vec_push(v, &ptr);
}

static inline void* vec_at_ptr(const Vector* v, u64 index) {
    void** slot = (void**)vec_at(v, index);
    return slot ? *slot : NULL;
}
