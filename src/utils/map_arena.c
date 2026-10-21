// Arena-native backend — implements the map.h facade with open addressing
// (linear probing, splitmix64) on a MemArena. No malloc at all.
//
// Only this file knows the storage layout; nothing else includes it.
//
// Swap in: make MAP_BACKEND=arena
//
// Semantics identical to map_khash.c:
//   - upsert, delete (backward shift), clear, iteration
//   - rehash above 0.7 load factor
//   - keys are unconstrained u64: occupied state lives in a separate
//     flags array (exactly like khash), so even UINT64_MAX is a legal key
//   - map_get misses leave *out untouched
//   - map_free() is a no-op: the arena owns the memory until arena_destroy()
//
// Growth caveat (inherent to bump allocators): the OLD arrays sit BELOW the
// newly pushed ones, so they cannot be freed. With doubling capacity the
// abandoned tables total < one current table — peak arena use from growth
// stays about 2x one current table.
#include "map.h"
#include "arena.h"

#include <string.h>

struct Map {
    u64* keys;
    u64* values;
    u8* flags; // 0 = empty slot, 1 = occupied (khash-style: no reserved key)
    u64 capacity; // power of two
    u64 count;
    MemArena* arena;
};

static u64 map_hash(u64 key) {
    // https://en.wikipedia.org/wiki/SplitMix64
    key += 0x9E3779B97F4A7C15ULL;
    key = (key ^ (key >> 30)) * 0xBF58476D1CE4E5B9ULL;
    key = (key ^ (key >> 27)) * 0x94D049BB133111EBULL;
    return key ^ (key >> 31);
}

static u64 map_index_of(const Map* map, u64 key) {
    return map_hash(key) & (map->capacity - 1);
}

static void map_ensure_empty(Map* map) {
    memset(map->flags, 0, map->capacity);
}

Map* map_new(MemArena* arena) {
    if (!arena) {
        return NULL;
    }

    u64 capacity = 8;

    Map* map = PUSH_STRUCT(arena, Map);
    if (!map) {
        return NULL;
    }
    map->keys = PUSH_ARRAY(arena, u64, capacity);
    map->values = PUSH_ARRAY(arena, u64, capacity);
    map->flags = PUSH_ARRAY(arena, u8, capacity);
    if (!map->keys || !map->values || !map->flags) {
        return NULL;
    }

    map->capacity = capacity;
    map->count = 0;
    map->arena = arena;
    map_ensure_empty(map);
    return map;
}

void map_free(Map* map) {
    (void)map; // arena-owned; released at arena_destroy()
}

// Rehash above 0.7 load: keeps the average probe chain short while wasting
// little memory. (See map.h header for the arena growth caveat.)
static bool map_grow(Map* map) {
    u64 new_capacity = map->capacity << 1;
    u64* new_keys = PUSH_ARRAY(map->arena, u64, new_capacity);
    u64* new_values = PUSH_ARRAY(map->arena, u64, new_capacity);
    u8* new_flags = PUSH_ARRAY(map->arena, u8, new_capacity);
    if (!new_keys || !new_values || !new_flags) {
        return FALSE;
    }

    u64 old_capacity = map->capacity;
    u64* old_keys = map->keys;
    u64* old_values = map->values;
    u8* old_flags = map->flags;

    map->keys = new_keys;
    map->values = new_values;
    map->flags = new_flags;
    map->capacity = new_capacity;
    map->count = 0;
    map_ensure_empty(map);

    for (u64 i = 0; i < old_capacity; ++i) {
        if (!old_flags[i]) {
            continue;
        }
        u64 idx = map_index_of(map, old_keys[i]);
        while (map->flags[idx]) {
            idx = (idx + 1) & (map->capacity - 1);
        }
        map->keys[idx] = old_keys[i];
        map->values[idx] = old_values[i];
        map->flags[idx] = 1;
        map->count++;
    }

    return TRUE;
}

bool map_set(Map* map, u64 key, u64 value) {
    if (!map) {
        return FALSE;
    }

    if (map->count + 1 > (map->capacity * 7) / 10) {
        if (!map_grow(map)) {
            return FALSE;
        }
    }
    if (map->count >= map->capacity) {
        return FALSE;
    }

    u64 mask = map->capacity - 1;
    u64 idx = map_index_of(map, key);
    while (TRUE) {
        if (!map->flags[idx]) {
            map->keys[idx] = key;
            map->values[idx] = value;
            map->flags[idx] = 1;
            map->count++;
            return TRUE;
        }
        if (map->keys[idx] == key) {
            map->values[idx] = value;
            return TRUE;
        }
        idx = (idx + 1) & mask;
    }
}

bool map_get(const Map* map, u64 key, u64* out_value) {
    if (!map || map->count == 0) {
        return FALSE;
    }

    u64 mask = map->capacity - 1;
    u64 idx = map_index_of(map, key);
    while (map->flags[idx]) {
        if (map->keys[idx] == key) {
            if (out_value) {
                *out_value = map->values[idx];
            }
            return TRUE;
        }
        idx = (idx + 1) & mask;
    }
    return FALSE;
}

bool map_has(const Map* map, u64 key) {
    return map_get(map, key, NULL);
}

u64 map_count(const Map* map) {
    return map ? map->count : 0;
}

// Backward-shift deletion (Knuth 6.4R): after removing the entry, pull
// each following entry whose probe chain reaches through the hole one
// step back. No tombstones ever appear, so lookups, iteration and growth
// need no special cases — same observable behavior as khash's tombstones.
bool map_del(Map* map, u64 key) {
    if (!map || map->count == 0) {
        return FALSE;
    }

    u64 mask = map->capacity - 1;
    u64 hole = map_index_of(map, key);
    while (TRUE) {
        if (!map->flags[hole]) {
            return FALSE;
        }
        if (map->keys[hole] == key) {
            break;
        }
        hole = (hole + 1) & mask;
    }

    map->flags[hole] = 0;
    map->count--;

    u64 next = (hole + 1) & mask;
    while (map->flags[next]) {
        u64 k = map->keys[next];
        u64 ideal = map_index_of(map, k);
        // Is the hole on this entry's probe path (earlier than the entry
        // itself)? Then the entry must slide back to keep its chain intact.
        if (((hole - ideal) & mask) < ((next - ideal) & mask)) {
            map->keys[hole] = k;
            map->values[hole] = map->values[next];
            map->flags[hole] = 1;
            map->flags[next] = 0;
            hole = next;
        }
        next = (next + 1) & mask;
    }
    return TRUE;
}

void map_clear(Map* map) {
    if (!map || map->count == 0) {
        return;
    }
    map_ensure_empty(map);
    map->count = 0;
}

bool map_iter(const Map* map, u64* slot, u64* out_key, u64* out_value) {
    if (!map || !slot) {
        return FALSE;
    }
    // No tombstones exist (backward-shift deletion), so empty == dead.
    for (u64 i = *slot; i < map->capacity; ++i) {
        if (!map->flags[i]) {
            continue;
        }
        if (out_key) {
            *out_key = map->keys[i];
        }
        if (out_value) {
            *out_value = map->values[i];
        }
        *slot = i + 1;
        return TRUE;
    }
    *slot = map->capacity;
    return FALSE;
}
