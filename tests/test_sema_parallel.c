#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int write_source(const char *path, size_t functions, int many_errors) {
  FILE *f = ast_fopen(path, "wb");
  if (!f)
    return -1;
  if (fputs("module parallel.stress;\n"
            "type Box<T:type,N:const usize> = struct { data:[T;N]; };\n"
            "fn id<T:type>(x:T)->T { return x; }\n",
            f) == EOF) {
    fclose(f);
    return -1;
  }
  for (size_t i = 0u; i < functions; ++i) {
    int rc;
    if (many_errors || (i % 7u) == 0u) {
      rc = fprintf(f,
                   "fn bad_%zu(x:u64)->u64 { let b:Box<u64,4>; let "
                   "y:u64=id(x); return missing_%zu + y; }\n",
                   i, i);
    } else {
      rc = fprintf(f,
                   "fn ok_%zu(x:u64,p:ptr<const u64>)->u64 { let "
                   "a:u64=id(x)+3; if a>10 { a=a+load.u64[p]; } return a; }\n",
                   i);
    }
    if (rc < 0) {
      fclose(f);
      return -1;
    }
  }
  return fclose(f) == 0 ? 0 : -1;
}

static int write_module_source(const char *path, const char *module_name,
                               size_t module_index, size_t functions) {
  FILE *f = ast_fopen(path, "wb");
  if (!f)
    return -1;
  if (fprintf(
          f,
          "module %s;\n"
          "type Box<T:type,N:const usize> = struct { data:[T;N]; };\n"
          "export fn id<T:type>(x:T)->T { return x; }\n"
          "export fn api_%zu(x:u64)->u64 { return id(x) + missing_api_%zu; }\n",
          module_name, module_index, module_index) < 0) {
    fclose(f);
    return -1;
  }
  for (size_t i = 0u; i < functions; ++i) {
    if (fprintf(f,
                "fn worker_%zu_%zu(x:u64,p:ptr<const u64>)->u64 { "
                "let box:Box<u64,4>; let y:u64=id(x); "
                "if y>3 { y=y+load.u64[p]; } return y; }\n",
                module_index, i) < 0) {
      fclose(f);
      return -1;
    }
  }
  return fclose(f) == 0 ? 0 : -1;
}

static int write_entry_source(const char *path, const char *entry_module,
                              char module_names[][96], size_t module_count) {
  FILE *f = ast_fopen(path, "wb");
  if (!f)
    return -1;
  if (fprintf(f, "module %s;\n", entry_module) < 0) {
    fclose(f);
    return -1;
  }
  for (size_t i = 0u; i < module_count; ++i) {
    if (fprintf(f, "import %s;\n", module_names[i]) < 0) {
      fclose(f);
      return -1;
    }
  }
  if (fputs("fn root(x:u64)->u64 {\n    let y:u64=x;\n", f) == EOF) {
    fclose(f);
    return -1;
  }
  for (size_t i = 0u; i < module_count; ++i) {
    if (fprintf(f, "    y = y + %s.id(y);\n", module_names[i]) < 0) {
      fclose(f);
      return -1;
    }
  }
  if (fputs("    return y;\n}\n", f) == EOF) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static AstSema *run_sema(const AstFrontend *frontend, size_t threads,
                         uint32_t max_diagnostics, int *rc_out) {
  AstSemaConfig config = ast_sema_config_default();
  config.thread_count = threads;
  config.max_diagnostics = max_diagnostics;
  AstSema *sema = ast_sema_create(frontend, &config);
  if (!sema)
    return NULL;
  *rc_out = ast_sema_check(sema);
  return sema;
}

static int diagnostics_equal(const AstSema *a, const AstSema *b) {
  size_t ac = 0u, bc = 0u;
  const AstSemaDiagnostic *ad = ast_sema_diagnostics(a, &ac);
  const AstSemaDiagnostic *bd = ast_sema_diagnostics(b, &bc);
  if (ac != bc)
    return 0;
  for (size_t i = 0u; i < ac; ++i) {
    if (ad[i].code != bd[i].code || ad[i].severity != bd[i].severity ||
        ad[i].span.file_id != bd[i].span.file_id ||
        ad[i].span.offset != bd[i].span.offset ||
        ad[i].span.length != bd[i].span.length || ad[i].line != bd[i].line ||
        ad[i].column != bd[i].column)
      return 0;
    if ((ad[i].message == NULL) != (bd[i].message == NULL))
      return 0;
    if (ad[i].message && strcmp(ad[i].message, bd[i].message) != 0)
      return 0;
  }
  return 1;
}

static int run_determinism_test(const AstFrontend *frontend) {
  int serial_rc = 0;
  AstSema *serial = run_sema(frontend, 1u, 4096u, &serial_rc);
  if (!serial || serial_rc != 1) {
    if (serial)
      ast_sema_destroy(serial);
    return 0;
  }
  AstSemaStats serial_stats = ast_sema_stats(serial);
  for (size_t iteration = 0u; iteration < 12u; ++iteration) {
    int parallel_rc = 0;
    AstSema *parallel = run_sema(frontend, 5u, 4096u, &parallel_rc);
    if (!parallel || parallel_rc != serial_rc ||
        !diagnostics_equal(serial, parallel)) {
      if (parallel) {
        fprintf(stderr, "parallel diagnostics differ at iteration %zu\n",
                iteration);
        ast_print_sema_diagnostics(stderr, parallel, NULL);
        ast_sema_destroy(parallel);
      }
      ast_sema_destroy(serial);
      return 0;
    }
    AstSemaStats parallel_stats = ast_sema_stats(parallel);
    if (serial_stats.expression_count != parallel_stats.expression_count ||
        serial_stats.statement_count != parallel_stats.statement_count ||
        serial_stats.constant_expression_count !=
            parallel_stats.constant_expression_count) {
      fprintf(stderr, "parallel semantic counters differ at iteration %zu\n",
              iteration);
      ast_sema_destroy(parallel);
      ast_sema_destroy(serial);
      return 0;
    }
    ast_sema_destroy(parallel);
  }
  ast_sema_destroy(serial);
  return 1;
}

static int run_multimodule_determinism_test(const AstFrontend *frontend) {
  int serial_rc = 0;
  AstSema *serial = run_sema(frontend, 1u, 4096u, &serial_rc);
  if (!serial || serial_rc != 1) {
    if (serial)
      ast_sema_destroy(serial);
    return 0;
  }
  AstSemaStats serial_stats = ast_sema_stats(serial);
  for (size_t iteration = 0u; iteration < 6u; ++iteration) {
    int parallel_rc = 0;
    AstSema *parallel = run_sema(frontend, 5u, 4096u, &parallel_rc);
    if (!parallel || parallel_rc != serial_rc ||
        !diagnostics_equal(serial, parallel)) {
      if (parallel)
        ast_sema_destroy(parallel);
      ast_sema_destroy(serial);
      return 0;
    }
    AstSemaStats parallel_stats = ast_sema_stats(parallel);
    if (serial_stats.expression_count != parallel_stats.expression_count ||
        serial_stats.statement_count != parallel_stats.statement_count ||
        serial_stats.constant_expression_count !=
            parallel_stats.constant_expression_count) {
      ast_sema_destroy(parallel);
      ast_sema_destroy(serial);
      return 0;
    }
    ast_sema_destroy(parallel);
  }
  ast_sema_destroy(serial);
  return 1;
}

static int run_limit_test(const AstFrontend *frontend) {
  int rc = 0;
  AstSema *sema = run_sema(frontend, 5u, 16u, &rc);
  if (!sema || rc != 1) {
    if (sema)
      ast_sema_destroy(sema);
    return 0;
  }
  size_t count = 0u;
  const AstSemaDiagnostic *diags = ast_sema_diagnostics(sema, &count);
  int ok = count == 17u && diags[16].code == AST_SEMA_DIAG_TOO_MANY_DIAGNOSTICS;
  AstSemaStats stats = ast_sema_stats(sema);
  ok = ok && stats.error_count == count;
  if (!ok) {
    fprintf(stderr, "diagnostic limit mismatch: count=%zu errors=%zu\n", count,
            stats.error_count);
    ast_print_sema_diagnostics(stderr, sema, NULL);
  }
  ast_sema_destroy(sema);
  return ok;
}

static void unique_stamp(long long *seconds, long *subsecond) {
#if defined(_WIN32)
  *seconds = (long long)time(NULL);
  *subsecond = (long)clock();
#else
  struct timespec stamp;
  timespec_get(&stamp, TIME_UTC);
  *seconds = (long long)stamp.tv_sec;
  *subsecond = stamp.tv_nsec;
#endif
}

int main(void) {
  long long stamp_seconds = 0;
  long stamp_subsecond = 0;
  unique_stamp(&stamp_seconds, &stamp_subsecond);
  char path[256];
  int n = snprintf(path, sizeof(path), "asterasm_parallel_%lld_%ld.aa",
                   stamp_seconds, stamp_subsecond);
  if (n <= 0 || (size_t)n >= sizeof(path))
    return 2;
  if (write_source(path, 512u, 0) != 0)
    return 2;

  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 5u;
  AstFrontend *frontend = ast_frontend_create(&fc);
  if (!frontend) {
    remove(path);
    return 2;
  }
  const char *entries[] = {path};
  if (ast_frontend_load(frontend, entries, 1u) != 0) {
    ast_print_frontend_diagnostics(stderr, frontend, NULL);
    ast_frontend_destroy(frontend);
    remove(path);
    return 1;
  }
  int ok = run_determinism_test(frontend);
  ast_frontend_destroy(frontend);
  remove(path);
  if (!ok)
    return 1;

  unique_stamp(&stamp_seconds, &stamp_subsecond);
  char prefix[80];
  n = snprintf(prefix, sizeof(prefix), "pstress_%lld_%ld", stamp_seconds,
               stamp_subsecond);
  if (n <= 0 || (size_t)n >= sizeof(prefix))
    return 2;
  enum { MODULE_COUNT = 8 };
  char module_names[MODULE_COUNT][96];
  char module_paths[MODULE_COUNT][128];
  char entry_module[96];
  char entry_path[128];
  for (size_t i = 0u; i < MODULE_COUNT; ++i) {
    n = snprintf(module_names[i], sizeof(module_names[i]), "%s_m%zu", prefix,
                 i);
    if (n <= 0 || (size_t)n >= sizeof(module_names[i]))
      return 2;
    n = snprintf(module_paths[i], sizeof(module_paths[i]), "%s.aa",
                 module_names[i]);
    if (n <= 0 || (size_t)n >= sizeof(module_paths[i]))
      return 2;
    if (write_module_source(module_paths[i], module_names[i], i, 64u) != 0)
      return 2;
  }
  n = snprintf(entry_module, sizeof(entry_module), "%s_entry", prefix);
  if (n <= 0 || (size_t)n >= sizeof(entry_module))
    return 2;
  n = snprintf(entry_path, sizeof(entry_path), "%s.aa", entry_module);
  if (n <= 0 || (size_t)n >= sizeof(entry_path))
    return 2;
  if (write_entry_source(entry_path, entry_module, module_names,
                         MODULE_COUNT) != 0)
    return 2;

  frontend = ast_frontend_create(&fc);
  if (!frontend)
    return 2;
  if (ast_frontend_add_import_root(frontend, ".") != 0) {
    ast_frontend_destroy(frontend);
    return 2;
  }
  entries[0] = entry_path;
  if (ast_frontend_load(frontend, entries, 1u) != 0) {
    ast_print_frontend_diagnostics(stderr, frontend, NULL);
    ast_frontend_destroy(frontend);
    return 1;
  }
  ok = run_multimodule_determinism_test(frontend);
  ast_frontend_destroy(frontend);
  remove(entry_path);
  for (size_t i = 0u; i < MODULE_COUNT; ++i)
    remove(module_paths[i]);
  if (!ok)
    return 1;

  unique_stamp(&stamp_seconds, &stamp_subsecond);
  n = snprintf(path, sizeof(path), "asterasm_parallel_limit_%lld_%ld.aa",
               stamp_seconds, stamp_subsecond);
  if (n <= 0 || (size_t)n >= sizeof(path))
    return 2;
  if (write_source(path, 256u, 1) != 0)
    return 2;
  frontend = ast_frontend_create(&fc);
  if (!frontend) {
    remove(path);
    return 2;
  }
  entries[0] = path;
  if (ast_frontend_load(frontend, entries, 1u) != 0) {
    ast_frontend_destroy(frontend);
    remove(path);
    return 1;
  }
  ok = run_limit_test(frontend);
  ast_frontend_destroy(frontend);
  remove(path);
  return ok ? 0 : 1;
}
