#include "asterasm/asterasm.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

typedef struct TestPipeline {
  AstFrontend *frontend;
  AstSema *sema;
  AstHirProgram *hir;
  AstMirProgram *mir;
  AstTargetProgram *target;
  AstAbiProgram *abi;
  AstMachineProgram *machine;
  AstRegAllocProgram *regalloc;
  AstEncoderModuleOutput encoded;
} TestPipeline;

static void destroy_pipeline(TestPipeline *p) {
  ast_encoder_module_output_destroy(&p->encoded);
  ast_regalloc_destroy(p->regalloc);
  ast_machine_destroy(p->machine);
  ast_abi_destroy(p->abi);
  ast_target_destroy(p->target);
  ast_mir_destroy(p->mir);
  ast_hir_destroy(p->hir);
  ast_sema_destroy(p->sema);
  ast_frontend_destroy(p->frontend);
  memset(p, 0, sizeof(*p));
}

static int create_pipeline(TestPipeline *p, int optimized, AstTargetArch arch,
                           AstAbiKind abi) {
  memset(p, 0, sizeof(*p));
  const char *stage = "begin";
  char root[4096], entry[4096];
  if (snprintf(root, sizeof(root), "%s/tests/mir_cases/gvn_large",
               ASTERASM_SOURCE_DIR) < 0 ||
      snprintf(entry, sizeof(entry), "%s/main.aa", root) < 0)
    return 0;
  stage = "frontend";
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  p->frontend = ast_frontend_create(&fc);
  if (!p->frontend || ast_frontend_add_import_root(p->frontend, root) != 0)
    goto fail;
  const char *entries[] = {entry};
  if (ast_frontend_load(p->frontend, entries, 1u) != 0)
    goto fail;
  stage = "sema";
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  p->sema = ast_sema_create(p->frontend, &sc);
  if (!p->sema || ast_sema_check(p->sema) != 0)
    goto fail;
  stage = "HIR";
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  p->hir = ast_hir_build(p->frontend, p->sema, &hc);
  if (!p->hir || !ast_hir_verify(p->hir))
    goto fail;
  stage = "MIR";
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  p->mir = ast_mir_build(p->frontend, p->hir, &mc);
  if (!p->mir || !ast_mir_verify(p->mir))
    goto fail;
  stage = "MIR opt";
  if (optimized) {
    AstMirOptimizeStats stats;
    if (!ast_mir_optimize_local_gvn(p->mir, &stats) || stats.rewritten < 5u)
      goto fail;
  }
  stage = "target";
  AstTargetConfig tc = ast_target_config_default(arch);
  tc.os = AST_TARGET_OS_LINUX;
  p->target = ast_target_legalize(p->mir, &tc);
  if (!p->target || !ast_target_verify(p->target))
    goto fail;
  stage = "ABI";
  AstAbiConfig ac = ast_abi_config_default(abi);
  p->abi = ast_abi_lower(p->target, &ac);
  if (!p->abi || !ast_abi_verify(p->abi))
    goto fail;
  stage = "machine";
  p->machine = ast_backend_lower(p->abi, NULL);
  if (!p->machine || !ast_machine_verify(p->machine))
    goto fail;
  stage = "regalloc";
  AstRegAllocConfig rc = ast_regalloc_config_default();
  rc.verify_machine = 0u;
  p->regalloc = ast_regalloc_build(p->machine, &rc);
  if (!p->regalloc || !ast_regalloc_finalize(p->regalloc) ||
      !ast_physical_verify(p->regalloc))
    goto fail;
  stage = "encoder";
  if (!ast_encoder_module(p->regalloc, &p->encoded))
    goto fail;
  return 1;
fail:
  fprintf(stderr, "pipeline failure target=%d optimized=%d at %s\n", (int)arch,
          optimized, stage);
  destroy_pipeline(p);
  return 0;
}

#if defined(__linux__) && defined(__x86_64__)
static int function_index(const TestPipeline *p, const char *name) {
  for (size_t i = 0u; i < ast_mir_function_count(p->mir); ++i) {
    AstMirFunctionInfo info;
    if (!ast_mir_function_info(ast_mir_function_at(p->mir, i), &info))
      continue;
    size_t n = strlen(name);
    if (info.name.text && info.name.span.length == n &&
        memcmp(info.name.text, name, n) == 0)
      return (int)i;
  }
  return -1;
}

typedef uint64_t (*Fn2)(uint64_t, uint64_t);
typedef uint64_t (*Fn3)(uint64_t, uint64_t, uint64_t);
typedef uint64_t (*Fn1)(uint64_t);
typedef uint32_t (*FnU32)(uint32_t);
typedef uint16_t (*FnU16)(uint16_t);
typedef uint8_t (*FnU8)(uint8_t);

static uint64_t rng = UINT64_C(0x1fcbab1248690137);
static uint64_t next_random(void) {
  rng ^= rng >> 12u;
  rng ^= rng << 25u;
  rng ^= rng >> 27u;
  return rng * UINT64_C(2685821657736338717);
}

static void *map_code(const AstEncoderOutput *enc, size_t *length) {
  if (!enc || !enc->size || enc->fixup_count)
    return NULL;
  long pagesize = sysconf(_SC_PAGESIZE);
  if (pagesize <= 0 || enc->size > (size_t)pagesize * 64u)
    return NULL;
  size_t size = ((enc->size + (size_t)pagesize - 1u) / (size_t)pagesize) *
                (size_t)pagesize;
  void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED)
    return NULL;
  memcpy(memory, enc->bytes, enc->size);
  if (mprotect(memory, size, PROT_READ | PROT_EXEC) != 0) {
    munmap(memory, size);
    return NULL;
  }
  *length = size;
  return memory;
}

/* Raw generated functions have no Clang UBSan type-metadata prefix. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int compare_native(const TestPipeline *baseline,
                          const TestPipeline *optimized) {
  const char *names[] = {"repeated",         "across_blocks", "different_ops",
                         "division_effects", "identities",    "sibling_paths",
                         "stress",           "folded_u64"};
  uint64_t calls = 0u;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    int base_index = function_index(baseline, names[i]);
    int opt_index = function_index(optimized, names[i]);
    if (base_index < 0 || opt_index < 0 ||
        (size_t)base_index >= baseline->encoded.function_count ||
        (size_t)opt_index >= optimized->encoded.function_count)
      return 0;
    if (i == 6u) {
      printf("stress u64 arithmetic machine size: original=%zu optimized=%zu "
             "bytes\n",
             baseline->encoded.functions[base_index].size,
             optimized->encoded.functions[opt_index].size);
      if (optimized->encoded.functions[opt_index].size >=
          baseline->encoded.functions[base_index].size)
        return 0;
    }
    size_t base_len = 0u, opt_len = 0u;
    void *base = map_code(&baseline->encoded.functions[base_index], &base_len);
    void *opt = map_code(&optimized->encoded.functions[opt_index], &opt_len);
    if (!base || !opt) {
      if (base)
        munmap(base, base_len);
      if (opt)
        munmap(opt, opt_len);
      return 0;
    }
    Fn2 fbase = NULL, fopt = NULL;
    Fn3 fbase3 = NULL, fopt3 = NULL;
    Fn1 fbase1 = NULL, fopt1 = NULL;
    memcpy(&fbase, &base, sizeof(fbase));
    memcpy(&fopt, &opt, sizeof(fopt));
    memcpy(&fbase3, &base, sizeof(fbase3));
    memcpy(&fopt3, &opt, sizeof(fopt3));
    memcpy(&fbase1, &base, sizeof(fbase1));
    memcpy(&fopt1, &opt, sizeof(fopt1));
    for (uint32_t j = 0u; j < 2048u; ++j) {
      uint64_t a =
          j < 8u
              ? ((uint64_t[]){0u, 1u, UINT64_MAX, UINT64_C(0x8000000000000000),
                              7u, 31u, 65535u, UINT64_C(0x7fffffffffffffff)})[j]
              : next_random();
      uint64_t b = j < 8u
                       ? ((uint64_t[]){1u, 3u, 7u, 17u, 31u, UINT64_MAX,
                                       UINT64_C(0x8000000000000000), 65535u})[j]
                       : next_random();
      if (!b)
        b = 1u;
      uint64_t actual, reference;
      if (i == 1u || i == 5u) {
        uint64_t flag = j & 1u;
        actual = fopt3(a, b, flag);
        reference = fbase3(a, b, flag);
      } else if (i == 4u || i == 7u) {
        actual = fopt1(a);
        reference = fbase1(a);
      } else if (i == 8u) {
        FnU32 ub = NULL, uo = NULL;
        memcpy(&ub, &base, sizeof(ub));
        memcpy(&uo, &opt, sizeof(uo));
        actual = uo((uint32_t)a);
        reference = ub((uint32_t)a);
      } else if (i == 9u) {
        FnU16 ub = NULL, uo = NULL;
        memcpy(&ub, &base, sizeof(ub));
        memcpy(&uo, &opt, sizeof(uo));
        actual = uo((uint16_t)a);
        reference = ub((uint16_t)a);
      } else if (i == 10u) {
        FnU8 ub = NULL, uo = NULL;
        memcpy(&ub, &base, sizeof(ub));
        memcpy(&uo, &opt, sizeof(uo));
        actual = uo((uint8_t)a);
        reference = ub((uint8_t)a);
      } else {
        actual = fopt(a, b);
        reference = fbase(a, b);
      }
      uint64_t expected;
      switch (i) {
      case 0u:
        expected = (a * b) * UINT64_C(3);
        break;
      case 1u:
        expected = (a + b) * ((j & 1u) ? UINT64_C(2) : UINT64_C(1));
        break;
      case 2u:
        expected = (a + b) ^ (a - b);
        break;
      case 3u:
        expected = (a / b) * UINT64_C(2);
        break;
      case 5u:
        expected = a + b;
        break;
      case 6u:
        expected = (a * b) * UINT64_C(64);
        break;
      case 7u:
        expected = ((UINT64_C(131) * 17u + 97u) ^ 27u) + a;
        break;
      default:
        expected = a;
        break;
      }
      if (actual != reference || actual != expected) {
        fprintf(stderr,
                "native differential %s [%u] a=%" PRIu64 " b=%" PRIu64
                " baseline=%" PRIu64 " optimized=%" PRIu64 " expected=%" PRIu64
                "\n",
                names[i], j, a, b, reference, actual, expected);
        munmap(base, base_len);
        munmap(opt, opt_len);
        return 0;
      }
      ++calls;
    }
    munmap(base, base_len);
    munmap(opt, opt_len);
  }
  printf("MIR opt-in native x86 differential: %" PRIu64 " calls PASS\n", calls);
  return 1;
}
#endif

int main(void) {
  const AstTargetArch arch[] = {AST_TARGET_ARCH_X86_64, AST_TARGET_ARCH_AARCH64,
                                AST_TARGET_ARCH_RISCV64};
  const AstAbiKind abi[] = {AST_ABI_SYSV_AMD64, AST_ABI_AAPCS64,
                            AST_ABI_RISCV_LP64};
  for (size_t i = 0u; i < 3u; ++i) {
    TestPipeline baseline, optimized;
    if (!create_pipeline(&baseline, 0, arch[i], abi[i]))
      return 1;
    if (!create_pipeline(&optimized, 1, arch[i], abi[i])) {
      destroy_pipeline(&baseline);
      return 2;
    }
    if (baseline.encoded.function_count != optimized.encoded.function_count) {
      destroy_pipeline(&baseline);
      destroy_pipeline(&optimized);
      return 3;
    }
#if defined(__linux__) && defined(__x86_64__)
    if (i == 0u && !compare_native(&baseline, &optimized)) {
      destroy_pipeline(&baseline);
      destroy_pipeline(&optimized);
      return 4;
    }
#endif
    printf("MIR local scalar full encoding target=%zu functions=%zu PASS\n", i,
           optimized.encoded.function_count);
    destroy_pipeline(&baseline);
    destroy_pipeline(&optimized);
  }
  return 0;
}
