#ifndef ASTERASM_HASH_H
#define ASTERASM_HASH_H

#include "asterasm/common.h"

#ifdef __cplusplus
extern "C" {
#endif

ASTER_API uint64_t ast_hash_bytes(const void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif
