// Robustness / security test: edge cases, adversarial key patterns and
// lifecycle abuse that both backends must survive identically.
// Prints nothing on success except a final OK line (run under ASan/UBSan
// via `make test`).
#include "utils/arena.h"
#include "utils/map.h"

#include <assert.h>
#include <stdio.h>

static u64 rng_state = 0xA4093822299F31D0ULL;

static u64 rng_next(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static void test_null_guards(void) {
    u64 out = 0;
    u64 slot = 0;
    assert(!map_set(NULL, 1, 2));
    assert(!map_get(NULL, 1, &out));
    assert(!map_has(NULL, 1));
    assert(!map_del(NULL, 1));
    assert(map_count(NULL) == 0);
    assert(!map_iter(NULL, &slot, NULL, NULL));
    map_clear(NULL);
    map_free(NULL);
}

static void test_empty_map(MemArena* arena) {
    Map* m = map_new(arena);
    assert(m);
    assert(map_count(m) == 0);

    u64 out = 77;
    u64 slot = 0, k = 0, v = 0;
    assert(!map_get(m, 5, &out) && out == 77);
    assert(!map_has(m, 5));
    assert(!map_del(m, 5));
    assert(!map_iter(m, &slot, &k, &v));
    map_clear(m); // clear on empty is a no-op
    assert(map_count(m) == 0);

    map_free(m);
}

static void test_full_key_range(MemArena* arena) {
    // Every key must be storable — including UINT64_MAX, which the arena
    // backend used to reserve as a sentinel. khash never reserved keys;
    // the flags-array design gives both backends identical freedom.
    Map* m = map_new(arena);
    assert(m);

    const u64 KEYS[] = {0, 1, UINT64_MAX, UINT64_MAX - 1, 1ULL << 63, (1ULL << 63) + 1};
    const u64 N = sizeof(KEYS) / sizeof(KEYS[0]);
    for (u64 i = 0; i < N; ++i) {
        assert(map_set(m, KEYS[i], i + 100));
    }
    assert(map_count(m) == N);
    for (u64 i = 0; i < N; ++i) {
        u64 out = 0;
        assert(map_get(m, KEYS[i], &out) && out == i + 100);
    }
    // delete the extremes, survivors must keep working
    assert(map_del(m, UINT64_MAX));
    assert(map_del(m, 0));
    assert(!map_has(m, UINT64_MAX));
    assert(!map_has(m, 0));
    assert(map_count(m) == N - 2);
    for (u64 i = 0; i < N; ++i) {
        if (KEYS[i] == 0 || KEYS[i] == UINT64_MAX) {
            continue; // deleted above
        }
        u64 out = 0;
        assert(map_get(m, KEYS[i], &out) && out == i + 100);
    }
    map_free(m);
}

static void test_delete_lifecycle(MemArena* arena) {
    Map* m = map_new(arena);
    assert(m);

    // single key: set -> get -> del -> miss -> del again -> re-set
    assert(map_set(m, 42, 1));
    u64 out = 0;
    assert(map_get(m, 42, &out) && out == 1);
    assert(map_del(m, 42));
    assert(!map_get(m, 42, &out));
    assert(!map_del(m, 42));
    assert(map_count(m) == 0);
    assert(map_set(m, 42, 2));
    assert(map_get(m, 42, &out) && out == 2);
    assert(map_del(m, 42)); // leave the map empty for the bulk phase
    assert(map_count(m) == 0);

    // delete ALL keys -> empty map, then fully reusable
    for (u64 i = 0; i < 5000; ++i) {
        assert(map_set(m, i * 2 + 1, i));
    }
    for (u64 i = 0; i < 5000; ++i) {
        assert(map_del(m, i * 2 + 1));
    }
    assert(map_count(m) == 0);
    for (u64 i = 0; i < 5000; ++i) {
        assert(!map_has(m, i * 2 + 1));
    }
    for (u64 i = 0; i < 5000; ++i) {
        assert(map_set(m, i * 2 + 1, i + 1000));
    }
    assert(map_count(m) == 5000);
    for (u64 i = 0; i < 5000; ++i) {
        out = 0;
        assert(map_get(m, i * 2 + 1, &out) && out == i + 1000);
    }
    map_free(m);
}

static void test_adversarial_patterns(MemArena* arena) {
    // Arithmetic progressions are the classic stressor for multiplicative
    // hashes (identical low bits -> identical probe start). Both backends
    // hash them with different functions but must stay correct.
    Map* m = map_new(arena);
    assert(m);

    const u64 N = 20000;
    for (u64 stride = 1; stride <= 1024; stride *= 4) {
        map_clear(m);
        for (u64 i = 0; i < N; ++i) {
            assert(map_set(m, i * stride, i));
        }
        assert(map_count(m) == N);
        for (u64 i = 0; i < N; ++i) {
            u64 out = 0;
            assert(map_get(m, i * stride, &out) && out == i);
        }
        // delete every other key: probe chains get chopped in half
        for (u64 i = 0; i < N; i += 2) {
            assert(map_del(m, i * stride));
        }
        assert(map_count(m) == N / 2);
        for (u64 i = 0; i < N; ++i) {
            u64 out = 0;
            if (i % 2 == 0) {
                assert(!map_get(m, i * stride, &out));
            } else {
                assert(map_get(m, i * stride, &out) && out == i);
            }
        }
    }
    map_free(m);
}

static void test_churn(MemArena* arena) {
    // insert/delete churn near the load-factor boundary: grow, shrink
    // usage, backward shift / tombstone reuse, repeated rehash
    Map* m = map_new(arena);
    assert(m);

    for (u64 round = 0; round < 50; ++round) {
        for (u64 i = 0; i < 2000; ++i) {
            assert(map_set(m, i, round * 2000 + i));
        }
        assert(map_count(m) == 2000);
        for (u64 i = 0; i < 2000; i += 2) {
            assert(map_del(m, i));
        }
        assert(map_count(m) == 1000);
        for (u64 i = 0; i < 2000; i += 2) {
            u64 out = 0;
            assert(!map_get(m, i, &out));
            assert(map_set(m, i, round + i)); // refill holes
        }
        for (u64 i = 0; i < 2000; ++i) {
            u64 out = 0;
            assert(map_get(m, i, &out));
        }
        map_clear(m);
        assert(map_count(m) == 0);
    }
    map_free(m);
}

static void test_iteration_consistency(MemArena* arena) {
    Map* m = map_new(arena);
    assert(m);

    // random population, iterate twice: same multiset (order may differ
    // across backends but must be stable within one)
    for (u64 i = 0; i < 10000; ++i) {
        u64 r = rng_next();
        assert(map_set(m, r, r ^ 0xABCDEF));
    }
    u64 n1 = 0, s1 = 0, slot = 0, k = 0, v = 0;
    while (map_iter(m, &slot, &k, &v)) {
        n1++;
        s1 += k + v;
    }
    u64 n2 = 0, s2 = 0;
    slot = 0;
    while (map_iter(m, &slot, &k, NULL)) {
        n2++;
        s2 += k;
    }
    assert(n1 == n2);
    assert(n1 == map_count(m));
    assert(s1 >= s2); // s1 includes values too; just sanity
    map_free(m);
}

int main(int argc, char** argv) {
    const char* backend = argc > 1 ? argv[1] : "?";
    MemArena* arena = arena_create(MByte(64));
    assert(arena);

    test_null_guards();
    test_empty_map(arena);
    test_full_key_range(arena);
    test_delete_lifecycle(arena);
    test_adversarial_patterns(arena);
    test_churn(arena);
    test_iteration_consistency(arena);

    arena_destroy(arena);
    printf("robust test OK (%s)\n", backend);
    return 0;
}
