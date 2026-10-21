#include "asterasm/asterasm.h"
#include "mir_internal.h"

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
  if (snprintf(root, sizeof(root), "%s/tests/mir_cases/fold_unsigned",
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
    if (!ast_mir_optimize_scalar(p->mir, &stats) || stats.constant_folded < 10u)
      goto fail;
    /* Division and remainder remain effectful in MIR even with constant
     * nonzero denominators: folding them requires separately proving and
     * rebuilding the memory chain. Never erase a MAY_TRAP node here.
     * Shifts are pure, so require actual post-MIR rewrite certificates. */
    unsigned division = 0u, remainder = 0u, shift_l = 0u, shift_r = 0u;
    for (size_t fi = 0; fi < p->mir->function_count; ++fi) {
      const AstMirFunction *fn = p->mir->functions[fi];
      for (uint32_t id = 0; id < fn->instruction_count; ++id) {
        if (!fn->gvn_certificates || fn->gvn_certificates[id].valid != 3u)
          continue;
        const AstMirInstruction *original = &fn->gvn_certificates[id].original;
        if (original->info.token_op == AST_TOK_SLASH)
          ++division;
        if (original->info.token_op == AST_TOK_PERCENT)
          ++remainder;
        if (original->info.token_op == AST_TOK_SHIFT_LEFT)
          ++shift_l;
        if (original->info.token_op == AST_TOK_SHIFT_RIGHT)
          ++shift_r;
      }
    }
    if (division != 0u || remainder != 0u || shift_l < 2u || shift_r < 2u) {
      fprintf(stderr, "missing certified folds: /=%u %%=%u <<=%u >>=%u\n",
              division, remainder, shift_l, shift_r);
      goto fail;
    }
    fprintf(stderr, "verified constant transfers: %llu\n",
            (unsigned long long)stats.constant_folded);
    /* Deliberately corrupt executable constants and their original SSA
     * proofs.  POST verification must reject each mutation independently,
     * then accept the restored object.  This also exercises folds whose
     * operands are themselves certified folded constants. */
    unsigned challenged = 0u;
    for (size_t fi = 0; fi < p->mir->function_count; ++fi) {
      AstMirFunction *fn = p->mir->functions[fi];
      for (uint32_t id = 0; id < fn->instruction_count; ++id) {
        if (!fn->gvn_certificates || fn->gvn_certificates[id].valid != 3u)
          continue;
        MirGvnCertificate *cert = &fn->gvn_certificates[id];
        if (cert->original.info.token_op != AST_TOK_SHIFT_LEFT &&
            cert->original.info.token_op != AST_TOK_SHIFT_RIGHT)
          continue;
        uint64_t saved = fn->instructions[id].info.constant.lo;
        fn->instructions[id].info.constant.lo ^= UINT64_C(1);
        if (ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
          goto fail;
        fn->instructions[id].info.constant.lo = saved;
        if (!ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
          goto fail;
        ++challenged;
        AstMirVReg operand = cert->original.operands[0];
        cert->original.operands[0] = fn->vreg_count;
        if (ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
          goto fail;
        cert->original.operands[0] = operand;
        if (!ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
          goto fail;
        ++challenged;
        if (challenged >= 16u)
          break;
      }
      if (challenged >= 16u)
        break;
    }
    if (challenged < 8u)
      goto fail;
    fprintf(stderr, "shift-certificate fault injection: %u rejections PASS\n",
            challenged);
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

/* JIT functions have no Clang UBSan -fsanitize=function metadata prefix.
 * Suppress only that call-site instrumentation, preserving ASan and the
 * remaining UBSan checks in the test and compiler libraries. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int compare_native(const TestPipeline *baseline,
                          const TestPipeline *optimized) {
  const char *names[] = {
      "folded_u32",        "folded_u16",        "folded_u8",
      "and_same",          "and_pair",          "or_pair",
      "const_div_mod_u64", "const_shift_u16",   "const_shift_u64",
      "fold_stress_u8_0",  "fold_stress_u8_1",  "fold_stress_u8_2",
      "fold_stress_u16_0", "fold_stress_u16_1", "fold_stress_u16_2",
      "fold_stress_u32_0", "fold_stress_u32_1", "fold_stress_u32_2",
      "fold_stress_u64_0", "fold_stress_u64_1", "fold_stress_u64_2",
      "a64_add_4095",      "a64_add_4096",      "a64_add_4097",
      "a64_add_16773120",  "a64_add_2000000000"};
  for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
    int bi = function_index(baseline, names[i]);
    int oi = function_index(optimized, names[i]);
    if (bi < 0 || oi < 0)
      return 0;
    size_t bn = 0u, on = 0u;
    void *bc = map_code(&baseline->encoded.functions[bi], &bn);
    void *oc = map_code(&optimized->encoded.functions[oi], &on);
    if (!bc || !oc) {
      if (bc)
        munmap(bc, bn);
      if (oc)
        munmap(oc, on);
      return 0;
    }
    Fn1 bf1 = NULL, of1 = NULL;
    memcpy(&bf1, &bc, sizeof(bf1));
    memcpy(&of1, &oc, sizeof(of1));
    Fn2 bf2 = NULL, of2 = NULL;
    memcpy(&bf2, &bc, sizeof(bf2));
    memcpy(&of2, &oc, sizeof(of2));
    FnU32 bu32 = NULL, ou32 = NULL;
    FnU16 bu16 = NULL, ou16 = NULL;
    FnU8 bu8 = NULL, ou8 = NULL;
    memcpy(&bu32, &bc, sizeof(bu32));
    memcpy(&ou32, &oc, sizeof(ou32));
    memcpy(&bu16, &bc, sizeof(bu16));
    memcpy(&ou16, &oc, sizeof(ou16));
    memcpy(&bu8, &bc, sizeof(bu8));
    memcpy(&ou8, &oc, sizeof(ou8));
    for (uint32_t j = 0u; j < 4096u; ++j) {
      uint64_t a =
          j < 8u
              ? ((const uint64_t[]){0u, 1u, 255u, 65535u, UINT64_MAX,
                                    UINT64_C(0x80000000), 4095u, UINT32_MAX})[j]
              : next_random();
      uint64_t base, opt, expected;
      if (i == 0u) {
        base = bu32((uint32_t)a);
        opt = ou32((uint32_t)a);
        expected =
            (uint32_t)((uint32_t)(UINT64_C(2000000000) + UINT64_C(2000000000)) -
                       1234u) ^
            (uint32_t)a;
      } else if (i == 1u) {
        base = bu16((uint16_t)a);
        opt = ou16((uint16_t)a);
        expected = (uint16_t)((uint16_t)(65000u + 4000u) ^ (uint16_t)a);
      } else if (i == 2u) {
        base = bu8((uint8_t)a);
        opt = ou8((uint8_t)a);
        expected = (uint8_t)((uint8_t)(250u + 19u) ^ (uint8_t)a);
      } else if (i <= 5u) {
        uint64_t b = j & 1u ? UINT64_MAX : UINT64_C(0x13572468fedcba98);
        base = bf2(a, b);
        opt = of2(a, b);
        expected = i == 3u ? a : (i == 4u ? a & b : a | b);
      } else if (i == 6u) {
        const uint64_t x = UINT64_C(1234567890123456789);
        const uint64_t quotient = x / UINT64_C(37);
        const uint64_t remainder = x % UINT64_C(37);
        base = bf1(a);
        opt = of1(a);
        expected = (quotient + remainder) ^ a;
      } else if (i == 7u) {
        const uint16_t x = UINT16_C(60000);
        const uint16_t expected16 =
            (uint16_t)((uint16_t)(x << 3u) ^ (uint16_t)(x >> 3u));
        base = bu16((uint16_t)a);
        opt = ou16((uint16_t)a);
        expected = (uint16_t)(expected16 ^ (uint16_t)a);
      } else if (i == 8u) {
        const uint64_t x = UINT64_C(1234567890123456789);
        base = bf1(a);
        opt = of1(a);
        expected = ((x << 15u) ^ (x >> 15u)) ^ a;
      } else if (i >= 21u) {
        static const uint32_t imm[] = {4095u, 4096u, 4097u, 16773120u,
                                       2000000000u};
        base = bu32((uint32_t)a);
        opt = ou32((uint32_t)a);
        expected = (uint32_t)((uint32_t)a + imm[i - 21u]);
      } else {
        unsigned bits = i < 12u ? 8u : i < 15u ? 16u : i < 18u ? 32u : 64u;
        unsigned mode = (unsigned)((i - 9u) % 3u);
        uint64_t mask = bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
        uint64_t x = (mask - 5u) & mask;
        uint64_t fixed = 0u;
        if (mode == 0u) {
          uint64_t p = (x + 17u) & mask;
          uint64_t q = p ^ x;
          fixed = ((q << (bits - 1u)) | (x >> (bits - 1u))) & mask;
        } else if (mode == 1u) {
          fixed = (((mask * UINT64_C(17)) - 5u) & mask) & mask;
        } else {
          fixed = ((UINT64_C(1) << (bits - 1u)) ^ UINT64_C(1)) & mask;
        }
        if (bits == 8u) {
          base = bu8((uint8_t)a);
          opt = ou8((uint8_t)a);
        } else if (bits == 16u) {
          base = bu16((uint16_t)a);
          opt = ou16((uint16_t)a);
        } else if (bits == 32u) {
          base = bu32((uint32_t)a);
          opt = ou32((uint32_t)a);
        } else {
          base = bf1(a);
          opt = of1(a);
        }
        expected = (fixed ^ a) & mask;
      }
      if (base != expected || opt != expected) {
        fprintf(stderr,
                "%s mismatch test %u base=%" PRIu64 " opt=%" PRIu64
                " expect=%" PRIu64 "\n",
                names[i], j, base, opt, expected);
        munmap(bc, bn);
        munmap(oc, on);
        return 0;
      }
    }
    munmap(bc, bn);
    munmap(oc, on);
  }
  puts("unsigned scalar folding/native x86: 106496 differential executions "
       "PASS");
  return 1;
}

#endif

/* Cross-target A/B codegen is a separate qualification from native x86:
 * it detects illegal AArch64 immediate shapes and broken RV64 encoding,
 * without pretending that emitting bytes is executing them on those CPUs. */
static int compare_cross_isa(AstTargetArch arch, AstAbiKind abi) {
  TestPipeline base, opt;
  if (!create_pipeline(&base, 0, arch, abi))
    return 0;
  if (!create_pipeline(&opt, 1, arch, abi)) {
    destroy_pipeline(&base);
    return 0;
  }
  int ok = base.encoded.function_count == opt.encoded.function_count &&
           base.encoded.function_count >= 26u;
  if (ok) {
    for (size_t i = 0; i < base.encoded.function_count; ++i) {
      if (!base.encoded.functions[i].size || !opt.encoded.functions[i].size ||
          base.encoded.functions[i].fixup_count ||
          opt.encoded.functions[i].fixup_count) {
        ok = 0;
        break;
      }
    }
  }
  destroy_pipeline(&base);
  destroy_pipeline(&opt);
  return ok;
}

int main(void) {
  TestPipeline base, opt;
  if (!create_pipeline(&base, 0, AST_TARGET_ARCH_X86_64, AST_ABI_SYSV_AMD64))
    return 1;
  if (!create_pipeline(&opt, 1, AST_TARGET_ARCH_X86_64, AST_ABI_SYSV_AMD64)) {
    destroy_pipeline(&base);
    return 2;
  }
  int ok = 1;
#if defined(__linux__) && defined(__x86_64__)
  ok = compare_native(&base, &opt);
#endif
  destroy_pipeline(&base);
  destroy_pipeline(&opt);
  if (ok && !compare_cross_isa(AST_TARGET_ARCH_AARCH64, AST_ABI_AAPCS64)) {
    fprintf(stderr, "AArch64 complete fixture encode mismatch\n");
    ok = 0;
  }
  if (ok && !compare_cross_isa(AST_TARGET_ARCH_RISCV64, AST_ABI_RISCV_LP64)) {
    fprintf(stderr, "RV64 complete fixture encode mismatch\n");
    ok = 0;
  }
  if (ok)
    puts("AArch64/RV64 opt-on/off encodings: 26 functions each PASS");
  return ok ? 0 : 3;
}
