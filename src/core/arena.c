#include "asterasm/arena.h"

#include <stdlib.h>
#include <string.h>

struct AstArenaBlock {
  struct AstArenaBlock *next;
  size_t capacity;
  size_t used;
  AstMaxAlign data[];
};

static AstArenaBlock *ast_arena_new_block(size_t capacity) {
  size_t total;
  if (ast_size_add_overflow(sizeof(AstArenaBlock), capacity, &total))
    return NULL;
  AstArenaBlock *block = (AstArenaBlock *)malloc(total);
  if (!block)
    return NULL;
  block->next = NULL;
  block->capacity = capacity;
  block->used = 0u;
  return block;
}

void ast_arena_init(AstArena *arena, size_t block_size) {
  if (!arena)
    return;
  memset(arena, 0, sizeof(*arena));
  arena->block_size = block_size ? block_size : (64u * 1024u);
}

void ast_arena_destroy(AstArena *arena) {
  if (!arena)
    return;
  AstArenaBlock *block = arena->head;
  while (block) {
    AstArenaBlock *next = block->next;
    free(block);
    block = next;
  }
  memset(arena, 0, sizeof(*arena));
}

void ast_arena_absorb(AstArena *dst, AstArena *src) {
  if (!dst || !src || dst == src || !src->head)
    return;
  if (!dst->head)
    dst->head = src->head;
  else
    dst->tail->next = src->head;
  dst->tail = src->tail;
  dst->current = dst->current ? dst->current : src->current;
  dst->bytes_reserved += src->bytes_reserved;
  dst->bytes_used += src->bytes_used;
  dst->allocation_count += src->allocation_count;
  src->head = NULL;
  src->tail = NULL;
  src->current = NULL;
  src->bytes_reserved = 0u;
  src->bytes_used = 0u;
  src->allocation_count = 0u;
}

void ast_arena_reset(AstArena *arena) {
  if (!arena)
    return;
  for (AstArenaBlock *block = arena->head; block; block = block->next)
    block->used = 0u;
  arena->current = arena->head;
  arena->bytes_used = 0u;
  arena->allocation_count = 0u;
}

void *ast_arena_alloc(AstArena *arena, size_t size, size_t alignment) {
  if (!arena || size == 0u)
    return NULL;
  if (alignment == 0u)
    alignment = ASTER_ALIGNOF(AstMaxAlign);
  if ((alignment & (alignment - 1u)) != 0u)
    return NULL;

  AstArenaBlock *block = arena->current;
  while (block) {
    size_t begin = ast_common_align_up(block->used, alignment);
    if (begin <= block->capacity && size <= block->capacity - begin) {
      block->used = begin + size;
      arena->bytes_used += size;
      arena->allocation_count++;
      return ((unsigned char *)block->data) + begin;
    }
    block = block->next;
    arena->current = block;
  }

  size_t need;
  if (ast_size_add_overflow(size, alignment, &need))
    return NULL;
  size_t capacity = arena->block_size;
  if (capacity < need)
    capacity = ast_common_align_up(need, 4096u);
  AstArenaBlock *fresh = ast_arena_new_block(capacity);
  if (!fresh)
    return NULL;

  if (!arena->head)
    arena->head = fresh;
  else
    arena->tail->next = fresh;
  arena->tail = fresh;
  arena->current = fresh;
  arena->bytes_reserved += capacity;

  size_t begin = ast_common_align_up(fresh->used, alignment);
  fresh->used = begin + size;
  arena->bytes_used += size;
  arena->allocation_count++;
  return ((unsigned char *)fresh->data) + begin;
}

void *ast_arena_calloc(AstArena *arena, size_t count, size_t size,
                       size_t alignment) {
  size_t total;
  if (ast_size_mul_overflow(count, size, &total))
    return NULL;
  void *ptr = ast_arena_alloc(arena, total, alignment);
  if (ptr)
    memset(ptr, 0, total);
  return ptr;
}

char *ast_arena_strndup(AstArena *arena, const char *text, size_t length) {
  char *copy = (char *)ast_arena_alloc(arena, length + 1u, 1u);
  if (!copy)
    return NULL;
  if (length)
    memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

char *ast_arena_strdup(AstArena *arena, const char *text) {
  if (!text)
    return NULL;
  return ast_arena_strndup(arena, text, strlen(text));
}
