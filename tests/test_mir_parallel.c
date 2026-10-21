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
  int failed;
} Buffer;

static void append(void *user, const char *text, size_t length) {
  Buffer *buffer = (Buffer *)user;
  if (!buffer || !text || length == 0u || buffer->failed)
    return;
  if (buffer->length > SIZE_MAX - length - 1u) {
    buffer->failed = 1;
    return;
  }
  size_t needed = buffer->length + length + 1u;
  if (needed > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 16384u;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2u) {
        capacity = needed;
        break;
      }
      capacity *= 2u;
    }
    char *next = (char *)realloc(buffer->data, capacity);
    if (!next) {
      buffer->failed = 1;
      return;
    }
    buffer->data = next;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

static int stats_equal(AstMirStats a, AstMirStats b) {
  return a.module_count == b.module_count &&
         a.function_count == b.function_count &&
         a.block_count == b.block_count &&
         a.instruction_count == b.instruction_count &&
         a.vreg_count == b.vreg_count &&
         a.memory_value_count == b.memory_value_count &&
         a.stack_slot_count == b.stack_slot_count &&
         a.promoted_local_count == b.promoted_local_count &&
         a.phi_count == b.phi_count &&
         a.memory_phi_count == b.memory_phi_count &&
         a.terminator_count == b.terminator_count &&
         a.unreachable_block_count == b.unreachable_block_count &&
         a.error_count == b.error_count && a.warning_count == b.warning_count;
}

int main(void) {
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/hir_cases/parallel_stress",
                    ASTERASM_SOURCE_DIR);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 2;

  AstFrontendConfig frontend_config = ast_frontend_config_default();
  frontend_config.thread_count = 5u;
  AstFrontend *frontend = ast_frontend_create(&frontend_config);
  if (!frontend || ast_frontend_add_import_root(frontend, root) != 0)
    return 2;
  const char *entries[] = {entry};
  if (ast_frontend_load(frontend, entries, 1u) != 0) {
    ast_frontend_destroy(frontend);
    return 1;
  }
  AstSemaConfig sema_config = ast_sema_config_default();
  sema_config.thread_count = 5u;
  AstSema *sema = ast_sema_create(frontend, &sema_config);
  if (!sema || ast_sema_check(sema) != 0) {
    ast_sema_destroy(sema);
    ast_frontend_destroy(frontend);
    return 1;
  }
  AstHirConfig hir_config = ast_hir_config_default();
  hir_config.thread_count = 5u;
  AstHirProgram *hir = ast_hir_build(frontend, sema, &hir_config);
  if (!hir || !ast_hir_verify(hir) || ast_hir_stats(hir).function_count != 512u)
    return 1;

  AstMirConfig one = ast_mir_config_default();
  one.thread_count = 1u;
  AstMirProgram *reference = ast_mir_build(frontend, hir, &one);
  if (!reference || !ast_mir_verify(reference) ||
      ast_mir_stats(reference).error_count != 0u ||
      ast_mir_stats(reference).function_count != 512u)
    return 1;
  Buffer expected = {0};
  ast_mir_dump_program(reference, append, &expected);
  AstMirStats reference_stats = ast_mir_stats(reference);
  AstMirConfig many = one;
  many.thread_count = 5u;
  int ok = !expected.failed && expected.length != 0u;
  for (unsigned repeat = 0u; repeat < 12u && ok; ++repeat) {
    AstMirProgram *parallel = ast_mir_build(frontend, hir, &many);
    if (!parallel || !ast_mir_verify(parallel) ||
        ast_mir_stats(parallel).error_count != 0u ||
        !stats_equal(reference_stats, ast_mir_stats(parallel))) {
      ast_mir_destroy(parallel);
      ok = 0;
      break;
    }
    Buffer actual = {0};
    ast_mir_dump_program(parallel, append, &actual);
    if (actual.failed || actual.length != expected.length ||
        (actual.length &&
         memcmp(actual.data, expected.data, actual.length) != 0))
      ok = 0;
    free(actual.data);
    ast_mir_destroy(parallel);
  }
  free(expected.data);
  ast_mir_destroy(reference);
  ast_hir_destroy(hir);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  if (!ok)
    return 1;
  puts("MIR parallel determinism stress passed");
  return 0;
}
