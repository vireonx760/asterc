#ifndef ASTERASM_ARENA_H
#define ASTERASM_ARENA_H

#include "asterasm/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstArenaBlock AstArenaBlock;

typedef struct AstArena {
  AstArenaBlock *head;
  AstArenaBlock *tail;
  AstArenaBlock *current;
  size_t block_size;
  size_t bytes_reserved;
  size_t bytes_used;
  size_t allocation_count;
} AstArena;

ASTER_API void ast_arena_init(AstArena *arena, size_t block_size);
ASTER_API void ast_arena_destroy(AstArena *arena);
ASTER_API void ast_arena_reset(AstArena *arena);
ASTER_API void ast_arena_absorb(AstArena *dst, AstArena *src);
ASTER_API void *ast_arena_alloc(AstArena *arena, size_t size, size_t alignment);
ASTER_API void *ast_arena_calloc(AstArena *arena, size_t count, size_t size,
                                 size_t alignment);
ASTER_API char *ast_arena_strndup(AstArena *arena, const char *text,
                                  size_t length);
ASTER_API char *ast_arena_strdup(AstArena *arena, const char *text);

#define AST_ARENA_NEW(arena, T)                                                \
  ((T *)ast_arena_calloc((arena), 1u, sizeof(T), ASTER_ALIGNOF(T)))
#define AST_ARENA_ARRAY(arena, T, n)                                           \
  ((T *)ast_arena_calloc((arena), (n), sizeof(T), ASTER_ALIGNOF(T)))

#ifdef __cplusplus
}
#endif

#endif
