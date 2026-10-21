#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

typedef struct TextBuffer {
  char *data;
  size_t length;
  size_t capacity;
} TextBuffer;

static void write_buffer(void *user, const char *text, size_t length) {
  TextBuffer *buffer = (TextBuffer *)user;
  if (!buffer || !text || length == 0u)
    return;
  if (buffer->length > SIZE_MAX - length - 1u)
    return;
  size_t needed = buffer->length + length + 1u;
  if (needed > buffer->capacity) {
    size_t next = buffer->capacity ? buffer->capacity : 4096u;
    while (next < needed) {
      if (next > SIZE_MAX / 2u) {
        next = needed;
        break;
      }
      next *= 2u;
    }
    char *grown = (char *)realloc(buffer->data, next);
    if (!grown)
      return;
    buffer->data = grown;
    buffer->capacity = next;
  }
  memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

static int no_hir_errors(const AstHirProgram *hir) {
  AstHirStats stats = ast_hir_stats(hir);
  if (stats.error_count != 0u)
    return 0;
  size_t count = 0u;
  const AstHirDiagnostic *diags = ast_hir_diagnostics(hir, &count);
  for (size_t i = 0u; i < count; ++i)
    if (diags[i].severity == AST_DIAG_ERROR)
      return 0;
  return 1;
}

static void print_hir_diagnostics(const AstHirProgram *hir) {
  size_t count = 0u;
  const AstHirDiagnostic *diags = ast_hir_diagnostics(hir, &count);
  for (size_t i = 0u; i < count; ++i) {
    fprintf(stderr, "%s:%u:%u: %s[%s]: %s\n",
            diags[i].path ? diags[i].path : "<source>", diags[i].line,
            diags[i].column,
            diags[i].severity == AST_DIAG_ERROR ? "error" : "warning",
            ast_hir_diag_code_name(diags[i].code),
            diags[i].message ? diags[i].message : "");
  }
}

static int run_case(const char *name) {
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/sema_cases/%s",
                    ASTERASM_SOURCE_DIR, name);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 2;

  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 4u;
  AstFrontend *frontend = ast_frontend_create(&fc);
  if (!frontend)
    return 2;
  if (ast_frontend_add_import_root(frontend, root) != 0) {
    ast_frontend_destroy(frontend);
    return 2;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(frontend, entries, 1u) != 0) {
    fprintf(stderr, "HIR corpus frontend failed for %s\n", name);
    ast_print_frontend_diagnostics(stderr, frontend, NULL);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 5u;
  AstSema *sema = ast_sema_create(frontend, &sc);
  if (!sema) {
    ast_frontend_destroy(frontend);
    return 2;
  }
  if (ast_sema_check(sema) != 0) {
    fprintf(stderr, "HIR corpus sema failed for %s\n", name);
    ast_print_sema_diagnostics(stderr, sema, NULL);
    ast_sema_destroy(sema);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstHirConfig serial_cfg = ast_hir_config_default();
  serial_cfg.thread_count = 1u;
  AstHirProgram *serial = ast_hir_build(frontend, sema, &serial_cfg);
  if (!serial || !no_hir_errors(serial) || !ast_hir_verify(serial)) {
    fprintf(stderr, "serial HIR failed for %s\n", name);
    if (serial)
      print_hir_diagnostics(serial);
    ast_hir_destroy(serial);
    ast_sema_destroy(sema);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstHirConfig parallel_cfg = serial_cfg;
  parallel_cfg.thread_count = 5u;
  AstHirProgram *parallel = ast_hir_build(frontend, sema, &parallel_cfg);
  if (!parallel || !no_hir_errors(parallel) || !ast_hir_verify(parallel)) {
    fprintf(stderr, "parallel HIR failed for %s\n", name);
    if (parallel)
      print_hir_diagnostics(parallel);
    ast_hir_destroy(parallel);
    ast_hir_destroy(serial);
    ast_sema_destroy(sema);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstHirStats ss = ast_hir_stats(serial);
  AstHirStats ps = ast_hir_stats(parallel);
  int ok =
      ss.module_count == ps.module_count &&
      ss.function_count == ps.function_count &&
      ss.global_count == ps.global_count && ss.block_count == ps.block_count &&
      ss.instruction_count == ps.instruction_count &&
      ss.value_count == ps.value_count && ss.local_count == ps.local_count &&
      ss.terminator_count == ps.terminator_count;

  TextBuffer a = {0}, b = {0};
  ast_hir_dump_program(serial, write_buffer, &a);
  ast_hir_dump_program(parallel, write_buffer, &b);
  if (a.length != b.length ||
      (a.length && memcmp(a.data, b.data, a.length) != 0))
    ok = 0;
  if (!ok)
    fprintf(stderr, "serial/parallel HIR mismatch for %s\n", name);

  free(a.data);
  free(b.data);
  ast_hir_destroy(parallel);
  ast_hir_destroy(serial);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <sema-positive-case-name>\n", argv[0]);
    return 2;
  }
  return run_case(argv[1]);
}
