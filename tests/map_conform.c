// Cross-backend conformance test: a deterministic mixed-op workload whose
// results must be byte-identical on map_khash and map_arena.
// `make test` builds it twice and diffs the outputs.
#include "utils/arena.h"
#include "utils/map.h"

#include <assert.h>
#include <stdio.h>

static u64 rng_state = 0x243F6A8885A308D3ULL;

static u64 rng_next(void) {
    // xorshift64* — deterministic across backends and platforms
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static u64 mix(u64 x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int main(void) {
    MemArena* arena = arena_create(MByte(128));
    assert(arena);
    Map* m = map_new(arena);
    assert(m);

    const u64 OPS = 2000000;
    const u64 KEYSPACE = 1 << 18; // small -> collisions, overwrites, deletes

    u64 ck = 0xCBF29CE484222325ULL; // running checksum over (op, result)
    for (u64 i = 0; i < OPS; ++i) {
        u64 r = rng_next();
        u64 op = (r >> 32) % 100;
        u64 key = (r >> 32) % KEYSPACE;
        u64 val = rng_next();

        if (op < 45) {
            bool ok = map_set(m, key, val);
            ck = ck * 0x100000001B3ULL + (ok ? 0xA5 : 0x5A);
        } else if (op < 75) {
            u64 out = 0;
            bool hit = map_get(m, key, &out);
            ck = ck * 0x100000001B3ULL + (hit ? mix(out) : 0xF1);
            // hit/miss agreement with has()
            assert(hit == map_has(m, key));
        } else if (op < 95) {
            bool gone = map_del(m, key);
            ck = ck * 0x100000001B3ULL + (gone ? 0xD7 : 0x0D);
            // deleted key must miss immediately (both backends)
            assert(!map_has(m, key));
        } else {
            bool has = map_has(m, key);
            ck = ck * 0x100000001B3ULL + (has ? 0xB1 : 0xB0);
        }
    }

    // final state: order-independent aggregate over iteration
    // (slot layout differs per backend — sums/xor do not)
    u64 cnt = 0, sumk = 0, sumv = 0, xr = 0, slot = 0, k = 0, v = 0;
    while (map_iter(m, &slot, &k, &v)) {
        cnt++;
        sumk += k;
        sumv += v;
        xr ^= mix(k * 31 + v);
    }
    assert(cnt == map_count(m));

    printf("conform ops=%llu count=%llu sumk=%llu sumv=%llu xr=%016llx ck=%016llx\n",
           (unsigned long long)OPS, (unsigned long long)cnt, (unsigned long long)sumk,
           (unsigned long long)sumv, (unsigned long long)xr, (unsigned long long)ck);

    map_free(m);
    arena_destroy(arena);
    return 0;
}
