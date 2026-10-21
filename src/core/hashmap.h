#ifndef ASTERASM_HASHMAP_H
#define ASTERASM_HASHMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct AstStringMapEntry {
  const char *key;
  uint64_t hash;
  uint32_t length;
  uint32_t value;
} AstStringMapEntry;

typedef struct AstStringMap {
  AstStringMapEntry *entries;
  size_t capacity;
  size_t count;
} AstStringMap;

void ast_string_map_init(AstStringMap *map);
void ast_string_map_destroy(AstStringMap *map);
int ast_string_map_get_n(const AstStringMap *map, const char *key,
                         size_t length, uint64_t hash, uint32_t *value_out);
int ast_string_map_put_n(AstStringMap *map, const char *key, size_t length,
                         uint64_t hash, uint32_t value, int *inserted_out);
int ast_string_map_get(const AstStringMap *map, const char *key,
                       uint32_t *value_out);
int ast_string_map_put(AstStringMap *map, const char *key, uint32_t value,
                       int *inserted_out);

#endif
