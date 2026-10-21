#include "asterasm/asterasm.h"
#include "asterasm/thread_pool.h"
#include "compiler_paths.h"
#include "compiler_pipeline.h"
#include "compiler_timing.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum DriverMode {
  DRIVER_BUILD = 0,
  DRIVER_CHECK,
  DRIVER_PARSE
} DriverMode;

static void dump_write(void *user, const char *text, size_t length) {
  (void)fwrite(text, 1u, length, (FILE *)user);
}

static void usage(FILE *out) {
  fputs(
      "AsterASM compiler - version: " ASTERASM_VERSION_STRING "\n"
      "usage: asterc [build|check|parse] [options] <file.aa>...\n"
      "\n"
      "compiler options:\n"
      "  --target <triple>  target triple or "
      "x86_16/x86_32/x86_64/aarch64/riscv64\n"
      "  --feature <name>   enable target feature; repeatable\n"
      "  --abi <name>       cdecl, sysv, win64, aapcs64, aapcs64-sve, "
      "lp64[/f/d/v/fv/dv]\n"
      "  --emit <bin|obj>   raw load image (default) or relocatable object\n"
      "  --format <name>    object format: elf, coff, macho\n"
      "  --base <address>   raw-image base address (default 0x100000)\n"
      "  --reg-limit <n>    restrict allocatable registers (0 = target "
      "default)\n"
      "  --no-promote       disable MIR local promotion\n"
      "  -O0 / -O2          disable / enable verified scalar optimization and "
      "inlining\n"
      "  -Os / -Oz          optimize for compact machine code (default -O0)\n"
      "  --no-inline        preserve call boundaries during MIR optimization\n"
      "  --no-red-zone      reserve stack storage explicitly (freestanding "
      "x86-64)\n"
      "  -o <file>          output path (default a.out.bin/a.out.o)\n"
      "\n"
      "frontend options:\n"
      "  -I <dir>           add module import root\n"
      "  -j <n>             worker count, 0 = auto\n"
      "  --dump-ast         print parsed AST\n"
      "  --dump-hir         print verified high-level IR (build only)\n"
      "  --dump-mir         print verified mid-level IR (build only)\n"
      "  --dump-target      print target legalization plan (build only)\n"
      "  --dump-abi         print ABI-lowered plan (build only)\n"
      "  --dump-machine     print verified machine IR (build only)\n"
      "  --dump-regalloc    print physical IR and allocated registers (build "
      "only)\n"
      "  --stats            print stage statistics\n"
      "  --opt-report <file> write selected optimization/artifact metadata as "
      "JSON\n"
      "  --no-imports       do not resolve imports\n"
      "  --version          print version\n"
      "  -h, --help         show this help\n",
      out);
}

static int parse_u64(const char *text, uint64_t *out) {
  if (!text || !*text || !out || text[0] == '-')
    return 0;
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 0);
  if (errno || !end || *end)
    return 0;
  *out = (uint64_t)value;
  return 1;
}

static int parse_target(const char *text, AstTargetConfig *out) {
  if (!text || !out)
    return 0;
  if (!strcmp(text, "x86_16") || !strcmp(text, "x86_32") ||
      !strcmp(text, "i386") || !strcmp(text, "i686")) {
    *out = ast_target_config_default(!strcmp(text, "x86_16")
                                         ? AST_TARGET_ARCH_X86_16
                                         : AST_TARGET_ARCH_X86_32);
    out->os = !strcmp(text, "x86_16") ? AST_TARGET_OS_BARE_METAL
                                      : AST_TARGET_OS_LINUX;
    return 1;
  }
  if (!strcmp(text, "x86_64") || !strcmp(text, "amd64")) {
    *out = ast_target_config_default(AST_TARGET_ARCH_X86_64);
    out->os = AST_TARGET_OS_LINUX;
    return 1;
  }
  if (!strcmp(text, "aarch64") || !strcmp(text, "arm64")) {
    *out = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
    out->os = AST_TARGET_OS_LINUX;
    return 1;
  }
  if (!strcmp(text, "riscv64") || !strcmp(text, "rv64")) {
    *out = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
    out->os = AST_TARGET_OS_LINUX;
    return 1;
  }
  return ast_target_config_from_triple(text, out);
}

static AstAbiKind parse_abi(const char *text) {
  if (!text || !strcmp(text, "auto"))
    return AST_ABI_INVALID;
  if (!strcmp(text, "cdecl") || !strcmp(text, "i386-cdecl"))
    return AST_ABI_I386_CDECL;
  if (!strcmp(text, "sysv") || !strcmp(text, "sysv-amd64"))
    return AST_ABI_SYSV_AMD64;
  if (!strcmp(text, "win64") || !strcmp(text, "windows-x64"))
    return AST_ABI_WINDOWS_X64;
  if (!strcmp(text, "aapcs64"))
    return AST_ABI_AAPCS64;
  if (!strcmp(text, "aapcs64-sve") || !strcmp(text, "sve"))
    return AST_ABI_AAPCS64_SVE;
  if (!strcmp(text, "lp64"))
    return AST_ABI_RISCV_LP64;
  if (!strcmp(text, "lp64f"))
    return AST_ABI_RISCV_LP64F;
  if (!strcmp(text, "lp64d"))
    return AST_ABI_RISCV_LP64D;
  if (!strcmp(text, "lp64v"))
    return AST_ABI_RISCV_LP64V;
  if (!strcmp(text, "lp64fv"))
    return AST_ABI_RISCV_LP64FV;
  if (!strcmp(text, "lp64dv"))
    return AST_ABI_RISCV_LP64DV;
  return (AstAbiKind)-1;
}

static uint32_t parse_format(const char *text) {
  if (!strcmp(text, "elf"))
    return 1u;
  if (!strcmp(text, "coff"))
    return 2u;
  if (!strcmp(text, "macho") || !strcmp(text, "mach-o"))
    return 3u;
  return 0u;
}

static uint32_t default_object_format(AstTargetOS os) {
  if (os == AST_TARGET_OS_WINDOWS)
    return 2u;
  if (os == AST_TARGET_OS_MACOS)
    return 3u;
  return 1u;
}

int main(int argc, char **argv) {
  AstThreadPool *pool = NULL, *previous_pool = NULL;
  AstFrontendConfig frontend_config = ast_frontend_config_default();
  AsterCompilerOptions compiler;
  memset(&compiler, 0, sizeof(compiler));
  compiler.target = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  compiler.target.os = AST_TARGET_OS_LINUX;
  compiler.abi = AST_ABI_INVALID;
  compiler.emit = ASTER_COMPILER_EMIT_BINARY;
  compiler.base_address = UINT64_C(0x100000);
  compiler.promote_locals = 1u;

  const char **roots = (const char **)calloc((size_t)argc, sizeof(*roots));
  const char **files = (const char **)calloc((size_t)argc, sizeof(*files));
  const char **features =
      (const char **)calloc((size_t)argc, sizeof(*features));
  if (!roots || !files || !features) {
    free(roots);
    free(files);
    free(features);
    return 2;
  }
  size_t root_count = 0u, file_count = 0u, feature_count = 0u;
  int dump_ast = 0, stats = 0;
  const char *abi_text = NULL;
  const char *format_text = NULL;
  DriverMode mode = DRIVER_BUILD;

  int argi = 1;
  if (argi < argc && strcmp(argv[argi], "build") == 0) {
    mode = DRIVER_BUILD;
    ++argi;
  } else if (argi < argc && strcmp(argv[argi], "check") == 0) {
    mode = DRIVER_CHECK;
    ++argi;
  } else if (argi < argc && strcmp(argv[argi], "parse") == 0) {
    mode = DRIVER_PARSE;
    ++argi;
  }

  for (; argi < argc; ++argi) {
    const char *arg = argv[argi];
    if (!strcmp(arg, "--version")) {
      printf("asterc %s\n", ASTERASM_VERSION_STRING);
      free(roots);
      free(files);
      free(features);
      return 0;
    }
    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      usage(stdout);
      free(roots);
      free(files);
      free(features);
      return 0;
    }
    if (!strcmp(arg, "--dump-ast")) {
      dump_ast = 1;
      continue;
    }
    if (!strcmp(arg, "--dump-hir")) {
      compiler.dump_hir = 1u;
      continue;
    }
    if (!strcmp(arg, "--dump-mir")) {
      compiler.dump_mir = 1u;
      continue;
    }
    if (!strcmp(arg, "--dump-target")) {
      compiler.dump_target = 1u;
      continue;
    }
    if (!strcmp(arg, "--dump-abi")) {
      compiler.dump_abi = 1u;
      continue;
    }
    if (!strcmp(arg, "--dump-machine")) {
      compiler.dump_machine = 1u;
      continue;
    }
    if (!strcmp(arg, "--dump-regalloc")) {
      compiler.dump_regalloc = 1u;
      continue;
    }
    if (!strcmp(arg, "--stats")) {
      stats = 1;
      continue;
    }
    if (!strcmp(arg, "--opt-report")) {
      if (++argi >= argc || !argv[argi][0]) {
        fputs("asterc: --opt-report requires a path\n", stderr);
        goto bad_args;
      }
      compiler.optimization_report = argv[argi];
      continue;
    }
    if (!strcmp(arg, "--no-imports")) {
      frontend_config.flags &= ~(uint32_t)AST_FRONTEND_RESOLVE_IMPORTS;
      continue;
    }
    if (!strcmp(arg, "--no-red-zone")) {
      compiler.disable_red_zone = 1u;
      continue;
    }
    if (!strcmp(arg, "--no-promote")) {
      compiler.promote_locals = 0u;
      continue;
    }
    if (!strcmp(arg, "-O0")) {
      compiler.optimize_mir = compiler.optimize_size = 0u;
      continue;
    }
    if (!strcmp(arg, "-O2")) {
      compiler.optimize_mir = 1u;
      compiler.optimize_size = 0u;
      continue;
    }
    if (!strcmp(arg, "-Os") || !strcmp(arg, "-Oz")) {
      compiler.optimize_mir = compiler.optimize_size = 1u;
      continue;
    }
    if (!strcmp(arg, "--no-inline")) {
      compiler.disable_inlining = 1u;
      continue;
    }
    if (!strcmp(arg, "-I")) {
      if (++argi >= argc) {
        fputs("asterc: -I requires a directory\n", stderr);
        goto bad_args;
      }
      roots[root_count++] = argv[argi];
      continue;
    }
    if (!strncmp(arg, "-I", 2u) && arg[2] != '\0') {
      roots[root_count++] = arg + 2;
      continue;
    }
    if (!strcmp(arg, "-j")) {
      uint64_t value;
      if (++argi >= argc || !parse_u64(argv[argi], &value) ||
          value > SIZE_MAX) {
        fputs("asterc: -j requires a valid number\n", stderr);
        goto bad_args;
      }
      frontend_config.thread_count = (size_t)value;
      continue;
    }
    if (!strcmp(arg, "--target")) {
      if (++argi >= argc || !parse_target(argv[argi], &compiler.target)) {
        fputs("asterc: invalid --target\n", stderr);
        goto bad_args;
      }
      continue;
    }
    if (!strcmp(arg, "--feature")) {
      if (++argi >= argc) {
        fputs("asterc: --feature requires a name\n", stderr);
        goto bad_args;
      }
      features[feature_count++] = argv[argi];
      continue;
    }
    if (!strcmp(arg, "--abi")) {
      if (++argi >= argc) {
        fputs("asterc: --abi requires a name\n", stderr);
        goto bad_args;
      }
      abi_text = argv[argi];
      continue;
    }
    if (!strcmp(arg, "--emit")) {
      if (++argi >= argc) {
        fputs("asterc: --emit requires bin or obj\n", stderr);
        goto bad_args;
      }
      if (!strcmp(argv[argi], "bin") || !strcmp(argv[argi], "binary"))
        compiler.emit = ASTER_COMPILER_EMIT_BINARY;
      else if (!strcmp(argv[argi], "obj") || !strcmp(argv[argi], "object"))
        compiler.emit = ASTER_COMPILER_EMIT_OBJECT;
      else {
        fputs("asterc: --emit requires bin or obj\n", stderr);
        goto bad_args;
      }
      continue;
    }
    if (!strcmp(arg, "--format")) {
      if (++argi >= argc) {
        fputs("asterc: --format requires elf, coff, or macho\n", stderr);
        goto bad_args;
      }
      format_text = argv[argi];
      continue;
    }
    if (!strcmp(arg, "--base")) {
      if (++argi >= argc || !parse_u64(argv[argi], &compiler.base_address)) {
        fputs("asterc: --base requires an unsigned address\n", stderr);
        goto bad_args;
      }
      continue;
    }
    if (!strcmp(arg, "--reg-limit")) {
      uint64_t value;
      if (++argi >= argc || !parse_u64(argv[argi], &value) ||
          value > UINT32_MAX) {
        fputs("asterc: --reg-limit requires a 32-bit number\n", stderr);
        goto bad_args;
      }
      compiler.register_limit = (uint32_t)value;
      continue;
    }
    if (!strcmp(arg, "-o")) {
      if (++argi >= argc) {
        fputs("asterc: -o requires a path\n", stderr);
        goto bad_args;
      }
      compiler.output_path = argv[argi];
      continue;
    }
    if (arg[0] == '-') {
      fprintf(stderr, "asterc: unknown option: %s\n", arg);
      goto bad_args;
    }
    files[file_count++] = arg;
  }
  if (!file_count) {
    usage(stderr);
    goto bad_args;
  }
  if (mode != DRIVER_BUILD &&
      (compiler.dump_hir || compiler.dump_mir || compiler.dump_target ||
       compiler.dump_abi || compiler.dump_machine || compiler.dump_regalloc ||
       compiler.optimize_mir || compiler.optimization_report)) {
    fputs("asterc: intermediate stage dumps require build mode\n", stderr);
    goto bad_args;
  }

  for (size_t i = 0; i < feature_count; ++i) {
    uint64_t feature =
        ast_target_feature_from_name(compiler.target.arch, features[i]);
    if (!feature) {
      fprintf(stderr, "asterc: unknown feature for %s: %s\n",
              ast_target_arch_name(compiler.target.arch), features[i]);
      goto bad_args;
    }
    compiler.target.features |= feature;
  }
  compiler.target.features = ast_target_feature_closure(
      compiler.target.arch, compiler.target.features);
  if (abi_text) {
    compiler.abi = parse_abi(abi_text);
    if (compiler.abi == (AstAbiKind)-1) {
      fprintf(stderr, "asterc: unknown ABI: %s\n", abi_text);
      goto bad_args;
    }
  }
  if (compiler.abi == AST_ABI_INVALID)
    compiler.abi = ast_abi_default_for_target(&compiler.target);
  if (compiler.abi == AST_ABI_INVALID) {
    fputs("asterc: target has no default ABI; pass --abi\n", stderr);
    goto bad_args;
  }
  if (format_text) {
    compiler.object_format = parse_format(format_text);
    if (!compiler.object_format) {
      fprintf(stderr, "asterc: unknown object format: %s\n", format_text);
      goto bad_args;
    }
  } else
    compiler.object_format = default_object_format(compiler.target.os);
  if (!compiler.output_path)
    compiler.output_path =
        compiler.emit == ASTER_COMPILER_EMIT_OBJECT ? "a.out.o" : "a.out.bin";
  if (compiler.optimization_report) {
    if (compiler_paths_alias(compiler.optimization_report,
                             compiler.output_path)) {
      fputs("asterc: optimization report and artifact paths must differ\n",
            stderr);
      goto bad_args;
    }
    for (size_t i = 0u; i < file_count; ++i)
      if (compiler_paths_alias(compiler.optimization_report, files[i])) {
        fputs("asterc: optimization report must not overwrite a source file\n",
              stderr);
        goto bad_args;
      }
  }
  compiler.thread_count = frontend_config.thread_count;
  compiler.print_stats = (uint8_t)stats;

  pool = ast_thread_pool_create(compiler.thread_count);
  if (!pool) {
    fputs("asterc: cannot create worker pool\n", stderr);
    goto failure;
  }
  previous_pool = ast_thread_pool_bind(pool);

  AstFrontend *frontend = ast_frontend_create(&frontend_config);
  if (!frontend) {
    fputs("asterc: cannot create frontend\n", stderr);
    goto failure;
  }
  for (size_t i = 0; i < root_count; ++i) {
    if (ast_frontend_add_import_root(frontend, roots[i]) != 0) {
      fprintf(stderr, "asterc: cannot add import root: %s\n", roots[i]);
      ast_frontend_destroy(frontend);
      goto failure;
    }
  }
  double frontend_begin = compiler_seconds();
  int rc = ast_frontend_load(frontend, files, file_count);
  compiler.frontend_seconds =
      compiler_elapsed(frontend_begin, compiler_seconds());
  ast_print_frontend_diagnostics(stderr, frontend, NULL);
  /* Diagnostics are the user-visible authority.  Do not allow a future
   * frontend-stage status regression to turn emitted errors into exit 0. */
  AstFrontendStats loaded_stats = ast_frontend_stats(frontend);
  if (loaded_stats.lex_error_count || loaded_stats.parse_error_count ||
      loaded_stats.frontend_error_count)
    rc = rc != 0 ? rc : 1;
  AstSema *sema = NULL;
  double sema_begin = compiler_seconds();
  if (rc == 0 && mode != DRIVER_PARSE) {
    AstSemaConfig sc = ast_sema_config_default();
    sc.pointer_bits = compiler.target.pointer_bits;
    sc.thread_count = frontend_config.thread_count;
    sema = ast_sema_create(frontend, &sc);
    if (!sema) {
      fputs("asterc: cannot create semantic analyzer\n", stderr);
      rc = -1;
    } else {
      int sema_rc = ast_sema_check(sema);
      ast_print_sema_diagnostics(stderr, sema, NULL);
      AstSemaStats checked_stats = ast_sema_stats(sema);
      if (sema_rc != 0)
        rc = sema_rc;
      if (checked_stats.error_count != 0u)
        rc = rc != 0 ? rc : 1;
    }
  }
  compiler.sema_seconds = compiler_elapsed(sema_begin, compiler_seconds());
  if (dump_ast) {
    for (size_t i = 0; i < ast_frontend_file_count(frontend); ++i) {
      AstFrontendFile file;
      if (!ast_frontend_get_file(frontend, i, &file) || !file.parsed ||
          !file.parsed->module)
        continue;
      printf("== %s ==\n", file.path);
      ast_dump_module(file.parsed->module, 0, dump_write, stdout);
    }
  }
  if (stats) {
    AstFrontendStats s = ast_frontend_stats(frontend);
    fprintf(
        stderr,
        "files=%zu modules=%zu imports=%zu bytes=%zu tokens=%zu ast_nodes=%zu "
        "lex_errors=%zu parse_errors=%zu frontend_errors=%zu\n",
        s.file_count, s.module_count, s.import_edge_count, s.source_bytes,
        s.token_count, s.ast_nodes, s.lex_error_count, s.parse_error_count,
        s.frontend_error_count);
    if (sema) {
      AstSemaStats ss = ast_sema_stats(sema);
      fprintf(
          stderr,
          "sema_modules=%zu symbols=%zu types=%zu exprs=%zu stmts=%zu "
          "const_exprs=%zu functions=%zu sema_errors=%zu sema_warnings=%zu\n",
          ss.module_count, ss.symbol_count, ss.type_count, ss.expression_count,
          ss.statement_count, ss.constant_expression_count, ss.function_count,
          ss.error_count, ss.warning_count);
    }
  }
  if (rc == 0 && mode == DRIVER_BUILD)
    rc = aster_compiler_build(frontend, sema, &compiler) ? 0 : 1;
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  if (stats) {
    AstThreadPoolStats ps = ast_thread_pool_stats(pool);
    fprintf(stderr, "workers: available=%zu peak=%zu created=%zu jobs=%zu\n",
            ast_available_cpu_count(), ps.peak_workers,
            ps.helper_threads_created, ps.jobs);
  }
  ast_thread_pool_bind(previous_pool);
  ast_thread_pool_destroy(pool);
  free(roots);
  free(files);
  free(features);
  return rc == 0 ? 0 : 1;

bad_args:
  free(roots);
  free(files);
  free(features);
  return 2;
failure:
  if (pool) {
    ast_thread_pool_bind(previous_pool);
    ast_thread_pool_destroy(pool);
  }
  free(roots);
  free(files);
  free(features);
  return 1;
}
