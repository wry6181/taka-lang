// Arena-backed growable array — implements the vector.h facade.
// Only this file knows the storage layout.
#include "vector.h"
#include "arena.h"

#include <string.h>

struct Vector {
    u8* data; // elem_size * cap bytes, contiguous
    u64 len;
    u64 cap; // power of two
    u64 elem_size;
    MemArena* arena;
};

Vector* vec_new(MemArena* arena, u64 elem_size, u64 cap_hint) {
    if (!arena || elem_size == 0) {
        return NULL;
    }

    u64 cap = 8;
    while (cap < cap_hint) {
        cap <<= 1;
    }

    Vector* v = PUSH_STRUCT(arena, Vector);
    if (!v) {
        return NULL;
    }
    v->data = PUSH_ARRAY(arena, u8, cap * elem_size);
    if (!v->data) {
        return NULL;
    }

    v->len = 0;
    v->cap = cap;
    v->elem_size = elem_size;
    v->arena = arena;
    return v;
}

// See vector.h for the growth caveat: the old array stays in the arena.
static bool vec_grow(Vector* v) {
    u64 new_cap = v->cap << 1;
    u8* new_data = PUSH_ARRAY(v->arena, u8, new_cap * v->elem_size);
    if (!new_data) {
        return FALSE;
    }
    memcpy(new_data, v->data, v->len * v->elem_size);
    v->data = new_data;
    v->cap = new_cap;
    return TRUE;
}

void* vec_push(Vector* v, const void* elem) {
    if (!v) {
        return NULL;
    }
    if (v->len == v->cap) {
        if (!vec_grow(v)) {
            return NULL;
        }
    }
    void* slot = v->data + v->len * v->elem_size;
    memcpy(slot, elem, v->elem_size);
    v->len++;
    return slot;
}

void* vec_at(const Vector* v, u64 index) {
    if (!v || index >= v->len) {
        return NULL;
    }
    return (void*)(v->data + index * v->elem_size);
}

u64 vec_len(const Vector* v) {
    return v ? v->len : 0;
}

void vec_clear(Vector* v) {
    if (v) {
        v->len = 0;
    }
}
