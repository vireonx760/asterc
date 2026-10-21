/* SPDX-License-Identifier: MPL-2.0 */
#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_stdout(void *user, const char *text, size_t length) {
  ASTER_UNUSED(user);
  (void)fwrite(text, 1u, length, stdout);
}

static void print_mir_diagnostics(const AstMirProgram *mir) {
  size_t count = 0u;
  const AstMirDiagnostic *diagnostics = ast_mir_diagnostics(mir, &count);
  for (size_t i = 0u; i < count; ++i) {
    const AstMirDiagnostic *diag = &diagnostics[i];
    fprintf(stderr, "%s:%u:%u: %s[M%u]: %s\n",
            diag->path ? diag->path : "<mir>", diag->line, diag->column,
            diag->severity == AST_DIAG_ERROR ? "error" : "warning",
            (unsigned)diag->code,
            diag->message ? diag->message : ast_mir_diag_code_name(diag->code));
  }
}

static void usage(void) {
  fputs("usage: aster-mir [-I dir] [-j n] [--no-promote] [--stats] "
        "<file.aa>...\n",
        stderr);
}

int main(int argc, char **argv) {
  AstFrontendConfig frontend_config = ast_frontend_config_default();
  AstHirConfig hir_config = ast_hir_config_default();
  AstMirConfig mir_config = ast_mir_config_default();
  const char **roots = (const char **)calloc((size_t)argc, sizeof(*roots));
  const char **files = (const char **)calloc((size_t)argc, sizeof(*files));
  if (!roots || !files) {
    free(roots);
    free(files);
    return 2;
  }
  size_t root_count = 0u, file_count = 0u;
  int show_stats = 0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage();
      free(roots);
      free(files);
      return 0;
    }
    if (strcmp(argv[i], "--stats") == 0) {
      show_stats = 1;
      continue;
    }
    if (strcmp(argv[i], "--no-promote") == 0) {
      mir_config.promote_locals = 0u;
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
      size_t count = (size_t)strtoull(argv[++i], NULL, 10);
      frontend_config.thread_count = count;
      hir_config.thread_count = count;
      mir_config.thread_count = count;
      continue;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "aster-mir: unknown option: %s\n", argv[i]);
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

  AstFrontend *frontend = ast_frontend_create(&frontend_config);
  if (!frontend) {
    free(roots);
    free(files);
    return 2;
  }
  int rc = 0;
  for (size_t i = 0u; i < root_count; ++i) {
    if (ast_frontend_add_import_root(frontend, roots[i]) != 0) {
      fprintf(stderr, "aster-mir: cannot add import root %s\n", roots[i]);
      rc = 2;
      break;
    }
  }
  AstSema *sema = NULL;
  AstHirProgram *hir = NULL;
  AstMirProgram *mir = NULL;
  if (rc == 0) {
    rc = ast_frontend_load(frontend, files, file_count);
    ast_print_frontend_diagnostics(stderr, frontend, NULL);
  }
  if (rc == 0) {
    AstSemaConfig sema_config = ast_sema_config_default();
    sema_config.thread_count = mir_config.thread_count;
    sema = ast_sema_create(frontend, &sema_config);
    if (!sema || ast_sema_check(sema) != 0)
      rc = 1;
    if (sema)
      ast_print_sema_diagnostics(stderr, sema, NULL);
  }
  if (rc == 0) {
    hir = ast_hir_build(frontend, sema, &hir_config);
    if (!hir || ast_hir_stats(hir).error_count != 0u)
      rc = 1;
  }
  if (rc == 0) {
    mir = ast_mir_build(frontend, hir, &mir_config);
    if (!mir)
      rc = 2;
    else {
      print_mir_diagnostics(mir);
      if (ast_mir_stats(mir).error_count != 0u)
        rc = 1;
    }
  }
  if (mir && rc == 0)
    ast_mir_dump_program(mir, write_stdout, NULL);
  if (show_stats && mir) {
    AstMirStats stats = ast_mir_stats(mir);
    fprintf(stderr,
            "mir_modules=%zu functions=%zu globals=%zu blocks=%zu insts=%zu "
            "vregs=%zu memory=%zu slots=%zu promoted=%zu phis=%zu "
            "memory_phis=%zu errors=%zu\n",
            stats.module_count, stats.function_count, stats.global_count,
            stats.block_count, stats.instruction_count, stats.vreg_count,
            stats.memory_value_count, stats.stack_slot_count,
            stats.promoted_local_count, stats.phi_count, stats.memory_phi_count,
            stats.error_count);
  }
  ast_mir_destroy(mir);
  ast_hir_destroy(hir);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  free(roots);
  free(files);
  return rc == 0 ? 0 : rc;
}
