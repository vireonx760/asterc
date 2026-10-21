#include "hashmap.h"
#include "asterasm/hash.h"

#include <stdlib.h>
#include <string.h>

static size_t ast_map_probe(uint64_t hash, size_t capacity) {
  return (size_t)hash & (capacity - 1u);
}

void ast_string_map_init(AstStringMap *map) {
  if (map)
    memset(map, 0, sizeof(*map));
}

void ast_string_map_destroy(AstStringMap *map) {
  if (!map)
    return;
  free(map->entries);
  memset(map, 0, sizeof(*map));
}

static int ast_string_map_rehash(AstStringMap *map, size_t new_capacity) {
  AstStringMapEntry *entries =
      (AstStringMapEntry *)calloc(new_capacity, sizeof(*entries));
  if (!entries)
    return -1;
  for (size_t i = 0; i < map->capacity; ++i) {
    AstStringMapEntry e = map->entries[i];
    if (!e.key)
      continue;
    size_t slot = ast_map_probe(e.hash, new_capacity);
    while (entries[slot].key)
      slot = (slot + 1u) & (new_capacity - 1u);
    entries[slot] = e;
  }
  free(map->entries);
  map->entries = entries;
  map->capacity = new_capacity;
  return 0;
}

int ast_string_map_get_n(const AstStringMap *map, const char *key,
                         size_t length, uint64_t hash, uint32_t *value_out) {
  if (!map || !key || !map->capacity || length > UINT32_MAX)
    return 0;
  if (hash == 0u)
    hash = ast_hash_bytes(key, length);
  size_t slot = ast_map_probe(hash, map->capacity);
  size_t start = slot;
  do {
    const AstStringMapEntry *e = &map->entries[slot];
    if (!e->key)
      return 0;
    if (e->hash == hash && e->length == (uint32_t)length &&
        memcmp(e->key, key, length) == 0) {
      if (value_out)
        *value_out = e->value;
      return 1;
    }
    slot = (slot + 1u) & (map->capacity - 1u);
  } while (slot != start);
  return 0;
}

int ast_string_map_put_n(AstStringMap *map, const char *key, size_t length,
                         uint64_t hash, uint32_t value, int *inserted_out) {
  if (!map || !key || length > UINT32_MAX)
    return -1;
  if (hash == 0u)
    hash = ast_hash_bytes(key, length);
  if (!map->capacity) {
    if (ast_string_map_rehash(map, 64u) != 0)
      return -1;
  } else if ((map->count + 1u) * 10u >= map->capacity * 7u) {
    if (map->capacity > SIZE_MAX / 2u ||
        ast_string_map_rehash(map, map->capacity * 2u) != 0)
      return -1;
  }
  size_t slot = ast_map_probe(hash, map->capacity);
  for (;;) {
    AstStringMapEntry *e = &map->entries[slot];
    if (!e->key) {
      e->key = key;
      e->hash = hash;
      e->length = (uint32_t)length;
      e->value = value;
      map->count++;
      if (inserted_out)
        *inserted_out = 1;
      return 0;
    }
    if (e->hash == hash && e->length == (uint32_t)length &&
        memcmp(e->key, key, length) == 0) {
      e->value = value;
      if (inserted_out)
        *inserted_out = 0;
      return 0;
    }
    slot = (slot + 1u) & (map->capacity - 1u);
  }
}

int ast_string_map_get(const AstStringMap *map, const char *key,
                       uint32_t *value_out) {
  if (!key)
    return 0;
  size_t length = strlen(key);
  return ast_string_map_get_n(map, key, length, ast_hash_bytes(key, length),
                              value_out);
}

int ast_string_map_put(AstStringMap *map, const char *key, uint32_t value,
                       int *inserted_out) {
  if (!key)
    return -1;
  size_t length = strlen(key);
  return ast_string_map_put_n(map, key, length, ast_hash_bytes(key, length),
                              value, inserted_out);
}
