#include "asterasm/hash.h"

uint64_t ast_hash_bytes(const void *data, size_t length) {
  const unsigned char *p = (const unsigned char *)data;
  uint64_t h = UINT64_C(14695981039346656037);
  for (size_t i = 0u; i < length; ++i) {
    h ^= (uint64_t)p[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}
