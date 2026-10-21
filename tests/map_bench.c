// Speed benchmark: identical workload on both backends, run via
// `make bench` (built -O2 WITHOUT sanitizers — timings are meaningless
// under ASan).
//
// Phases: insert 1M random keys, 8M mixed lookups, delete/reinsert churn,
// full iteration, clear.
#define _POSIX_C_SOURCE 200809L // clock_gettime under -std=c11
#include "utils/arena.h"
#include "utils/map.h"

#include <assert.h>
#include <stdio.h>
#include <time.h>

static u64 rng_state = 0x082EFA98EC4E6C89ULL;

static u64 rng_next(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static f64 now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

static void report(const char* phase, u64 ops, f64 dt) {
    printf("  %-10s %10llu ops  %8.1f ms  %8.1f ns/op\n", phase, (unsigned long long)ops,
           dt * 1e3, dt * 1e9 / (f64)ops);
}

int main(int argc, char** argv) {
    const char* backend = argc > 1 ? argv[1] : "?";
    const u64 N = 1000000;
    const u64 LOOKUPS = 8000000;
    const u64 CHURN_ROUNDS = 3;

    MemArena* arena = arena_create(MByte(768));
    assert(arena);
    Map* m = map_new(arena);
    assert(m);

    printf("bench [%s]: N=%llu lookups=%llu churn_rounds=%llu\n", backend,
           (unsigned long long)N, (unsigned long long)LOOKUPS,
           (unsigned long long)CHURN_ROUNDS);

    // pre-generate keys/values so timing measures the map, not the PRNG
    u64* keys = (u64*)malloc(N * sizeof(u64));
    u64* vals = (u64*)malloc(N * sizeof(u64));
    assert(keys && vals);
    for (u64 i = 0; i < N; ++i) {
        keys[i] = rng_next();
        vals[i] = rng_next();
    }

    // phase 1: insert (keyspace >> N so overwrites are rare)
    f64 t0 = now_sec();
    for (u64 i = 0; i < N; ++i) {
        assert(map_set(m, keys[i], vals[i]));
    }
    report("insert", N, now_sec() - t0);
    assert(map_count(m) == N);

    // phase 2: lookups — 90% hits (existing keys), 10% guaranteed misses
    t0 = now_sec();
    u64 hits = 0;
    for (u64 i = 0; i < LOOKUPS; ++i) {
        u64 key = (i % 10 == 9) ? (0xFFFFFFFFFFFFFFFFULL - i) : keys[rng_next() % N];
        u64 out = 0;
        if (map_get(m, key, &out)) {
            hits++;
        }
    }
    f64 dt = now_sec() - t0;
    report("lookup", LOOKUPS, dt);
    printf("           hit rate: %.1f%%\n", 100.0 * (f64)hits / (f64)LOOKUPS);

    // phase 3: churn — delete half, reinsert half, repeated
    t0 = now_sec();
    u64 churn_ops = 0;
    for (u64 round = 0; round < CHURN_ROUNDS; ++round) {
        for (u64 i = 0; i < N; i += 2) {
            assert(map_del(m, keys[i]));
            churn_ops++;
        }
        for (u64 i = 0; i < N; i += 2) {
            assert(map_set(m, keys[i], vals[i] + round));
            churn_ops++;
        }
    }
    report("churn", churn_ops, now_sec() - t0);
    assert(map_count(m) == N);

    // phase 4: full iteration
    t0 = now_sec();
    u64 slot = 0, k = 0, v = 0, seen = 0, acc = 0;
    while (map_iter(m, &slot, &k, &v)) {
        seen++;
        acc += v;
    }
    report("iterate", seen, now_sec() - t0);
    assert(seen == N);
    assert(acc != 0);

    // phase 5: clear
    t0 = now_sec();
    map_clear(m);
    report("clear", 1, now_sec() - t0);
    assert(map_count(m) == 0);

    free(keys);
    free(vals);
    map_free(m);
    arena_destroy(arena);
    return 0;
}
