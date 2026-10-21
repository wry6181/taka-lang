// khash (klib) backend — the default. Implements the map.h facade on top
// of the vendored khash (third_party/khash.h): quadratic probing,
// power-of-two capacity, integer-key specialization, malloc/calloc/realloc.
//
// Only this file knows khash; nothing else in the project includes it.
#include "map.h"
#include "third_party/khash.h"

#include <stdlib.h>

KHASH_MAP_INIT_INT64(map, u64)

struct Map {
    khash_t(map)* h;
};

Map* map_new(MemArena* arena) {
    (void)arena; // malloc-backed backend

    Map* map = (Map*)calloc(1, sizeof(Map));
    if (!map) {
        return NULL;
    }
    map->h = kh_init_map();
    if (!map->h) {
        free(map);
        return NULL;
    }
    return map;
}

void map_free(Map* map) {
    if (!map) {
        return;
    }
    kh_destroy_map(map->h);
    free(map);
}

bool map_set(Map* map, u64 key, u64 value) {
    if (!map) {
        return FALSE;
    }
    int ret = 0;
    khiter_t it = kh_put_map(map->h, (khint64_t)key, &ret);
    if (ret < 0) {
        return FALSE; // allocation failure inside khash
    }
    kh_value(map->h, it) = value;
    return TRUE;
}

bool map_get(const Map* map, u64 key, u64* out_value) {
    if (!map) {
        return FALSE;
    }
    khiter_t it = kh_get_map(map->h, (khint64_t)key);
    if (it == kh_end(map->h)) {
        return FALSE;
    }
    if (out_value) {
        *out_value = kh_value(map->h, it);
    }
    return TRUE;
}

bool map_has(const Map* map, u64 key) {
    return map_get(map, key, NULL);
}

u64 map_count(const Map* map) {
    return map ? (u64)kh_size(map->h) : 0;
}

bool map_del(Map* map, u64 key) {
    if (!map) {
        return FALSE;
    }
    khiter_t it = kh_get_map(map->h, (khint64_t)key);
    if (it == kh_end(map->h)) {
        return FALSE;
    }
    kh_del_map(map->h, it); // tombstone; kh_size decremented by khash
    return TRUE;
}

void map_clear(Map* map) {
    if (map) {
        kh_clear_map(map->h);
    }
}

bool map_iter(const Map* map, u64* slot, u64* out_key, u64* out_value) {
    if (!map || !slot) {
        return FALSE;
    }
    khiter_t i = (khiter_t)*slot;
    for (; i < kh_end(map->h); ++i) {
        if (kh_exist(map->h, i)) {
            break; // live entry (skips both empty slots and tombstones)
        }
    }
    if (i >= kh_end(map->h)) {
        *slot = (u64)kh_end(map->h);
        return FALSE;
    }
    if (out_key) {
        *out_key = (u64)kh_key(map->h, i);
    }
    if (out_value) {
        *out_value = kh_value(map->h, i);
    }
    *slot = (u64)i + 1;
    return TRUE;
}
