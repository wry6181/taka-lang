// Vector (growable array) test: growth, by-value elements, pointer sugar,
// bounds and NULL guards. Run under ASan/UBSan via `make test`.
#include "utils/arena.h"
#include "utils/vector.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    u64 id;
    u64 line;
    u64 column;
} FakeToken;

int main(void) {
    MemArena* arena = arena_create(MByte(64));
    assert(arena);

    // NULL / bad-arg guards
    assert(vec_new(NULL, 8, 0) == NULL);
    assert(vec_new(arena, 0, 0) == NULL); // elem_size 0 is nonsense
    assert(vec_len(NULL) == 0);
    assert(vec_at(NULL, 0) == NULL);
    vec_clear(NULL);
    assert(vec_push(NULL, NULL) == NULL);

    // by-value elements: growth far beyond the initial capacity
    Vector* v = vec_new(arena, sizeof(u64), 0); // starts at 8
    assert(v);
    assert(vec_len(v) == 0);
    assert(vec_at(v, 0) == NULL); // empty -> OOB

    const u64 N = 100000; // ~14 growth steps
    for (u64 i = 0; i < N; ++i) {
        assert(vec_push(v, &i) != NULL);
    }
    assert(vec_len(v) == N);
    for (u64 i = 0; i < N; ++i) {
        u64* slot = (u64*)vec_at(v, i);
        assert(slot && *slot == i);
    }
    assert(vec_at(v, N) == NULL); // OOB

    // clear keeps the allocation, vector is reusable
    vec_clear(v);
    assert(vec_len(v) == 0);
    assert(vec_at(v, 0) == NULL);
    for (u64 i = 0; i < 1000; ++i) {
        u64 x = i * 3;
        assert(vec_push(v, &x));
    }
    assert(vec_len(v) == 1000);
    assert(*(u64*)vec_at(v, 999) == 999 * 3);

    // struct elements are copied by value (mutation of the source is
    // invisible to the vector)
    Vector* toks = vec_new(arena, sizeof(FakeToken), 4);
    FakeToken t = {.id = 7, .line = 1, .column = 2};
    assert(vec_push(toks, &t));
    t.id = 999; // source changed...
    FakeToken* stored = (FakeToken*)vec_at(toks, 0);
    assert(stored->id == 7); // ...vector copy is intact

    // pointer sugar ([]T* — the AST-stmt workload)
    Vector* ptrs = vec_new(arena, sizeof(void*), 0);
    static int a, b, c;
    vec_push_ptr(ptrs, &a);
    vec_push_ptr(ptrs, &b);
    vec_push_ptr(ptrs, &c);
    vec_push_ptr(ptrs, NULL); // NULL elements are legal
    assert(vec_len(ptrs) == 4);
    assert(vec_at_ptr(ptrs, 0) == &a);
    assert(vec_at_ptr(ptrs, 1) == &b);
    assert(vec_at_ptr(ptrs, 2) == &c);
    assert(vec_at_ptr(ptrs, 3) == NULL);
    assert(vec_at_ptr(ptrs, 4) == NULL); // OOB -> NULL

    // capacity hint is honored (rounded up to a power of two)
    Vector* hinted = vec_new(arena, sizeof(u64), 100);
    assert(hinted);
    for (u64 i = 0; i < 128; ++i) { // 128 = next pow2 >= 100: no growth until 128
        assert(vec_push(hinted, &i));
    }
    assert(vec_len(hinted) == 128);

    arena_destroy(arena);
    printf("vector test OK (100k u64, structs, ptr sugar)\n");
    return 0;
}
