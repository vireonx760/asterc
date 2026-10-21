#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

typedef struct Buffer {
  char *data;
  size_t length;
  size_t capacity;
} Buffer;

static void append(void *user, const char *text, size_t length) {
  Buffer *b = (Buffer *)user;
  if (!b || !text || length == 0u || b->length > SIZE_MAX - length - 1u)
    return;
  size_t need = b->length + length + 1u;
  if (need > b->capacity) {
    size_t cap = b->capacity ? b->capacity : 16384u;
    while (cap < need) {
      if (cap > SIZE_MAX / 2u) {
        cap = need;
        break;
      }
      cap *= 2u;
    }
    char *next = (char *)realloc(b->data, cap);
    if (!next)
      return;
    b->data = next;
    b->capacity = cap;
  }
  memcpy(b->data + b->length, text, length);
  b->length += length;
  b->data[b->length] = '\0';
}

static int stats_equal(AstHirStats a, AstHirStats b) {
  return a.module_count == b.module_count &&
         a.function_count == b.function_count &&
         a.global_count == b.global_count && a.block_count == b.block_count &&
         a.instruction_count == b.instruction_count &&
         a.value_count == b.value_count && a.local_count == b.local_count &&
         a.terminator_count == b.terminator_count &&
         a.unreachable_block_count == b.unreachable_block_count &&
         a.error_count == b.error_count;
}

int main(void) {
  char root[4096], entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/hir_cases/parallel_stress",
                    ASTERASM_SOURCE_DIR);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 2;

  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 5u;
  AstFrontend *frontend = ast_frontend_create(&fc);
  if (!frontend)
    return 2;
  if (ast_frontend_add_import_root(frontend, root) != 0) {
    ast_frontend_destroy(frontend);
    return 2;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(frontend, entries, 1u) != 0) {
    ast_frontend_destroy(frontend);
    return 1;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 5u;
  AstSema *sema = ast_sema_create(frontend, &sc);
  if (!sema || ast_sema_check(sema) != 0) {
    ast_sema_destroy(sema);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstHirConfig one = ast_hir_config_default();
  one.thread_count = 1u;
  AstHirProgram *reference = ast_hir_build(frontend, sema, &one);
  if (!reference || !ast_hir_verify(reference) ||
      ast_hir_stats(reference).error_count != 0u)
    return 1;
  Buffer ref = {0};
  ast_hir_dump_program(reference, append, &ref);
  AstHirStats ref_stats = ast_hir_stats(reference);
  if (ref_stats.function_count != 512u || ref.length == 0u)
    return 1;

  AstHirConfig many = one;
  many.thread_count = 5u;
  int ok = 1;
  for (unsigned repeat = 0u; repeat < 12u && ok; ++repeat) {
    AstHirProgram *parallel = ast_hir_build(frontend, sema, &many);
    if (!parallel || !ast_hir_verify(parallel) ||
        ast_hir_stats(parallel).error_count != 0u ||
        !stats_equal(ref_stats, ast_hir_stats(parallel))) {
      ast_hir_destroy(parallel);
      ok = 0;
      break;
    }
    Buffer got = {0};
    ast_hir_dump_program(parallel, append, &got);
    if (got.length != ref.length ||
        (got.length && memcmp(got.data, ref.data, got.length) != 0))
      ok = 0;
    free(got.data);
    ast_hir_destroy(parallel);
  }
  free(ref.data);
  ast_hir_destroy(reference);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  if (!ok)
    return 1;
  puts("HIR parallel determinism stress passed");
  return 0;
}
