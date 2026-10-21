/* SPDX-License-Identifier: MPL-2.0 */
#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_stdout(void *user, const char *text, size_t length) {
  ASTER_UNUSED(user);
  fwrite(text, 1u, length, stdout);
}

static void print_hir_diags(const AstHirProgram *hir) {
  size_t count = 0u;
  const AstHirDiagnostic *d = ast_hir_diagnostics(hir, &count);
  for (size_t i = 0u; i < count; ++i) {
    fprintf(stderr, "%s:%u:%u: %s[H%u]: %s\n", d[i].path ? d[i].path : "<hir>",
            d[i].line, d[i].column,
            d[i].severity == AST_DIAG_ERROR ? "error" : "warning",
            (unsigned)d[i].code,
            d[i].message ? d[i].message : ast_hir_diag_code_name(d[i].code));
  }
}

static void usage(void) {
  fputs("usage: aster-hir [-I dir] [-j n] [--stats] <file.aa>...\n", stderr);
}

int main(int argc, char **argv) {
  AstFrontendConfig fc = ast_frontend_config_default();
  AstHirConfig hc = ast_hir_config_default();
  const char **roots = (const char **)calloc((size_t)argc, sizeof(*roots));
  const char **files = (const char **)calloc((size_t)argc, sizeof(*files));
  if (!roots || !files) {
    free(roots);
    free(files);
    return 2;
  }
  size_t root_count = 0u, file_count = 0u;
  int stats = 0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage();
      free(roots);
      free(files);
      return 0;
    }
    if (strcmp(argv[i], "--stats") == 0) {
      stats = 1;
      continue;
    }
    if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
      roots[root_count++] = argv[++i];
      continue;
    }
    if (strncmp(argv[i], "-I", 2u) == 0 && argv[i][2] != '\0') {
      roots[root_count++] = argv[i] + 2;
      continue;
    }
    if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
      size_t n = (size_t)strtoull(argv[++i], NULL, 10);
      fc.thread_count = n;
      hc.thread_count = n;
      continue;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "aster-hir: unknown option: %s\n", argv[i]);
      free(roots);
      free(files);
      return 2;
    }
    files[file_count++] = argv[i];
  }
  if (!file_count) {
    usage();
    free(roots);
    free(files);
    return 2;
  }
  AstFrontend *frontend = ast_frontend_create(&fc);
  if (!frontend) {
    free(roots);
    free(files);
    return 2;
  }
  for (size_t i = 0u; i < root_count; ++i)
    if (ast_frontend_add_import_root(frontend, roots[i]) != 0) {
      fprintf(stderr, "aster-hir: cannot add import root %s\n", roots[i]);
      ast_frontend_destroy(frontend);
      free(roots);
      free(files);
      return 2;
    }
  int rc = ast_frontend_load(frontend, files, file_count);
  ast_print_frontend_diagnostics(stderr, frontend, NULL);
  AstSema *sema = NULL;
  AstHirProgram *hir = NULL;
  if (rc == 0) {
    AstSemaConfig sc = ast_sema_config_default();
    sc.thread_count = hc.thread_count;
    sema = ast_sema_create(frontend, &sc);
    if (!sema || ast_sema_check(sema) != 0)
      rc = 1;
    if (sema)
      ast_print_sema_diagnostics(stderr, sema, NULL);
  }
  if (rc == 0) {
    hir = ast_hir_build(frontend, sema, &hc);
    if (!hir)
      rc = 1;
    else {
      print_hir_diags(hir);
      if (ast_hir_stats(hir).error_count)
        rc = 1;
    }
  }
  if (hir && rc == 0)
    ast_hir_dump_program(hir, write_stdout, NULL);
  if (stats && hir) {
    AstHirStats s = ast_hir_stats(hir);
    fprintf(stderr,
            "hir_modules=%zu functions=%zu globals=%zu blocks=%zu insts=%zu "
            "values=%zu locals=%zu unreachable=%zu errors=%zu\n",
            s.module_count, s.function_count, s.global_count, s.block_count,
            s.instruction_count, s.value_count, s.local_count,
            s.unreachable_block_count, s.error_count);
  }
  ast_hir_destroy(hir);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  free(roots);
  free(files);
  return rc == 0 ? 0 : 1;
}
