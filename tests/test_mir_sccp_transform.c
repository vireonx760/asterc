#include "asterasm/asterasm.h"
#include "mir_internal.h"
#include <inttypes.h>
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

typedef struct Pipeline {
  AstFrontend *frontend;
  AstSema *sema;
  AstHirProgram *hir;
  AstMirProgram *mir;
  AstTargetProgram *target;
  AstAbiProgram *abi;
  AstMachineProgram *machine;
  AstRegAllocProgram *regalloc;
  AstEncoderModuleOutput encoded;
} Pipeline;

static void destroy(Pipeline *p) {
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

static int build(Pipeline *p, int optimized, AstTargetArch arch,
                 AstAbiKind abi) {
  memset(p, 0, sizeof(*p));
  char root[4096], entry[4096];
  if (snprintf(root, sizeof(root), "%s/tests/mir_cases/sccp_transform",
               ASTERASM_SOURCE_DIR) < 0 ||
      snprintf(entry, sizeof(entry), "%s/main.aa", root) < 0)
    return 0;
  const char *stage = "frontend";
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1;
  p->frontend = ast_frontend_create(&fc);
  if (!p->frontend || ast_frontend_add_import_root(p->frontend, root) != 0)
    goto fail;
  const char *sources[] = {entry};
  if (ast_frontend_load(p->frontend, sources, 1) != 0)
    goto fail;
  stage = "sema";
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1;
  p->sema = ast_sema_create(p->frontend, &sc);
  if (!p->sema || ast_sema_check(p->sema) != 0)
    goto fail;
  stage = "HIR";
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1;
  p->hir = ast_hir_build(p->frontend, p->sema, &hc);
  if (!p->hir || !ast_hir_verify(p->hir))
    goto fail;
  stage = "MIR";
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1;
  mc.promote_locals = 1;
  p->mir = ast_mir_build(p->frontend, p->hir, &mc);
  if (!p->mir || !ast_mir_verify(p->mir))
    goto fail;
  stage = "SCCP";
  if (optimized) {
    AstMirOptimizeStats stats = {0};
    if (!ast_mir_optimize_sccp_constants(p->mir, &stats) ||
        stats.rewritten < 70u ||
        !ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
      goto fail;
    printf("SCCP rewrites=%" PRIu64 " checked=%" PRIu64 "\n", stats.rewritten,
           stats.examined);
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
  rc.verify_machine = 0;
  p->regalloc = ast_regalloc_build(p->machine, &rc);
  if (!p->regalloc || !ast_regalloc_finalize(p->regalloc) ||
      !ast_physical_verify(p->regalloc) ||
      !ast_encoder_module(p->regalloc, &p->encoded))
    goto fail;
  return 1;
fail:
  fprintf(stderr, "SCCP pipeline rejected arch=%d opt=%d stage=%s\n", (int)arch,
          optimized, stage);
  destroy(p);
  return 0;
}

#if defined(__linux__) && defined(__x86_64__)
static int function_index(const Pipeline *p, const char *name) {
  for (size_t i = 0; i < p->mir->function_count; ++i) {
    const AstMirFunction *fn = p->mir->functions[i];
    size_t len = strlen(name);
    if (fn->name.text && fn->name.span.length == len &&
        memcmp(fn->name.text, name, len) == 0)
      return (int)i;
  }
  return -1;
}

#endif

static int negative_proofs(Pipeline *p) {
  int seen = 0;
  int semantic_replay_seen = 0;
  for (size_t fi = 0; fi < p->mir->function_count; ++fi) {
    AstMirFunction *fn = p->mir->functions[fi];
    if (!fn->gvn_certificates)
      continue;
    for (uint32_t id = 0; id < fn->instruction_count; ++id) {
      MirGvnCertificate *cert = &fn->gvn_certificates[id];
      if (cert->valid != 4u)
        continue;
      AstMirInstruction *inst = &fn->instructions[id];
      uint64_t saved = inst->info.constant.lo;
      inst->info.constant.lo ^= 1u;
      if (ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
        return 0;
      inst->info.constant.lo = saved;
      if (!ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
        return 0;
      ++seen;
      if (cert->original.info.operand_count) {
        AstMirVReg old = cert->original.operands[0];
        cert->original.operands[0] = fn->vreg_count;
        if (ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
          return 0;
        cert->original.operands[0] = old;
        if (!ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
          return 0;
        ++seen;
      }
      /* Unlike an invalid vreg, this mutation remains *type-correct*.
       * It can pass structural shape checks but MUST fail independent
       * semantic replay against the original executable-edge lattice. */
      if (!semantic_replay_seen && cert->original.info.operand_count &&
          fn->name.span.length == strlen("phi_arithmetic") &&
          memcmp(fn->name.text, "phi_arithmetic", strlen("phi_arithmetic")) ==
              0) {
        AstMirVReg old = cert->original.operands[0];
        for (uint32_t alt = 0; alt < fn->vreg_count; ++alt) {
          if (alt == old || fn->vregs[alt].info.block != fn->entry_block ||
              fn->vregs[alt].info.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
              !mir_type_equal(fn->vregs[alt].info.type,
                              fn->vregs[old].info.type))
            continue;
          AstMirInstId source = fn->vregs[alt].info.definition_index;
          if (source >= fn->instruction_count ||
              fn->instructions[source].info.op != AST_MIR_OP_CONSTANT ||
              fn->gvn_certificates[source].valid)
            continue;
          cert->original.operands[0] = alt;
          if (ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
            return 0;
          cert->original.operands[0] = old;
          if (!ast_mir_verify_mode(p->mir, AST_MIR_VERIFY_POST_OPT))
            return 0;
          semantic_replay_seen = 1;
          ++seen;
          break;
        }
      }
    }
  }
  return seen >= 8 && semantic_replay_seen;
}

#if defined(__linux__) && defined(__x86_64__)
static void *jit(const AstEncoderOutput *enc, size_t *length) {
  if (!enc->size || enc->fixup_count)
    return NULL;
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0 || enc->size > (size_t)ps * 64u)
    return NULL;
  size_t n = ((enc->size + (size_t)ps - 1u) / (size_t)ps) * (size_t)ps;
  void *p =
      mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return NULL;
  memcpy(p, enc->bytes, enc->size);
  if (mprotect(p, n, PROT_READ | PROT_EXEC)) {
    munmap(p, n);
    return NULL;
  }
  *length = n;
  return p;
}

/* Generated entry points have no Clang UBSan function-metadata prefix.
 * All address/remaining undefined-behavior instrumentation stays enabled. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int execute(Pipeline *base, Pipeline *opt) {
  const char *names[] = {"phi_arithmetic",   "phi_arithmetic_u32",
                         "divergent",        "branch_path",
                         "dynamic_with_div", "loop_counter"};
  uint64_t comparisons = 0;
  for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    int b = function_index(base, names[i]), o = function_index(opt, names[i]);
    if (b < 0 || o < 0)
      return 0;
    printf("SCCP %s: code %zu -> %zu bytes\n", names[i],
           base->encoded.functions[b].size, opt->encoded.functions[o].size);
    size_t bl = 0, ol = 0;
    void *bp = jit(&base->encoded.functions[b], &bl);
    void *op = jit(&opt->encoded.functions[o], &ol);
    if (!bp || !op) {
      if (bp)
        munmap(bp, bl);
      if (op)
        munmap(op, ol);
      return 0;
    }
    typedef uint64_t (*Fn1)(uint64_t);
    typedef uint64_t (*Fn2)(uint64_t, uint64_t);
    typedef uint32_t (*Fn32)(uint32_t);
    Fn1 f1 = NULL, g1 = NULL;
    Fn2 f2 = NULL, g2 = NULL;
    Fn32 f32 = NULL, g32 = NULL;
    memcpy(&f1, &bp, sizeof(f1));
    memcpy(&g1, &op, sizeof(g1));
    memcpy(&f2, &bp, sizeof(f2));
    memcpy(&g2, &op, sizeof(g2));
    memcpy(&f32, &bp, sizeof(f32));
    memcpy(&g32, &op, sizeof(g32));
    for (uint32_t j = 0; j < 2048u; ++j) {
      uint64_t v = j < 8 ? (uint64_t[]){0, 1, 2, 7, 42, 200, 255, 512}[j]
                         : (j * UINT64_C(3703929)) ^ (j >> 3u);
      uint64_t expected, vb, vo;
      if (i == 0) {
        expected = 315;
        vb = f1(v & 1u);
        vo = g1(v & 1u);
      } else if (i == 1) {
        expected = 168;
        vb = f32((uint32_t)(v & 1u));
        vo = g32((uint32_t)(v & 1u));
      } else if (i == 2) {
        expected = (v & 1u) ? 24u : 36u;
        vb = f1(v & 1u);
        vo = g1(v & 1u);
      } else if (i == 3) {
        expected = (91u ^ 7u) + 5u;
        vb = f1(v & 1u);
        vo = g1(v & 1u);
      } else if (i == 4) {
        expected = v / (v % 127u + 1u) + 135u;
        vb = f2(v, v % 127u + 1u);
        vo = g2(v, v % 127u + 1u);
      } else {
        expected = v % 200u;
        vb = f1(v % 200u);
        vo = g1(v % 200u);
      }
      if (vb != expected || vo != expected) {
        fprintf(stderr,
                "SCCP mismatch %s sample=%u base=%" PRIu64 " opt=%" PRIu64
                " expected=%" PRIu64 "\n",
                names[i], j, vb, vo, expected);
        munmap(bp, bl);
        munmap(op, ol);
        return 0;
      }
      ++comparisons;
    }
    munmap(bp, bl);
    munmap(op, ol);
  }
  /* Large deterministic corpus: every result is compared to a third,
   * independent unsigned arithmetic reference, not just old backend. */
  for (uint32_t width_idx = 0; width_idx < 2u; ++width_idx) {
    const uint32_t count = width_idx == 0u ? 64u : 32u;
    for (uint32_t i = 0; i < count; ++i) {
      char name[64];
      if (snprintf(name, sizeof(name), "generated_%s_%02u",
                   width_idx == 0u ? "u64" : "u32", i) <= 0)
        return 0;
      int b = function_index(base, name), o = function_index(opt, name);
      if (b < 0 || o < 0)
        return 0;
      size_t bl = 0, ol = 0;
      void *bp = jit(&base->encoded.functions[b], &bl);
      void *op = jit(&opt->encoded.functions[o], &ol);
      if (!bp || !op) {
        if (bp)
          munmap(bp, bl);
        if (op)
          munmap(op, ol);
        return 0;
      }
      typedef uint64_t (*Fn64)(uint64_t);
      typedef uint32_t (*Fn32)(uint32_t);
      Fn64 fb64 = NULL, fo64 = NULL;
      Fn32 fb32 = NULL, fo32 = NULL;
      memcpy(&fb64, &bp, sizeof(fb64));
      memcpy(&fo64, &op, sizeof(fo64));
      memcpy(&fb32, &bp, sizeof(fb32));
      memcpy(&fo32, &op, sizeof(fo32));
      for (uint32_t j = 0; j < 128u; ++j) {
        uint32_t flag = (j * 17u) & 1u;
        uint64_t x =
            (uint64_t)(i * 1023u + 7u) + ((i % 5u == 0u && !flag) ? 1u : 0u);
        uint64_t expected = ((x + (uint64_t)((i * 7u) % 97u + 1u)) *
                             (uint64_t)((i % 13u) + 2u)) ^
                            (uint64_t)(i * 37u + 9u);
        uint64_t original = width_idx == 0u ? fb64(flag) : fb32(flag);
        uint64_t changed = width_idx == 0u ? fo64(flag) : fo32(flag);
        if (original != expected || changed != expected) {
          fprintf(stderr,
                  "SCCP generated case failed width=%u id=%u flag=%u "
                  "original=%" PRIu64 " optimized=%" PRIu64 " expected=%" PRIu64
                  "\n",
                  width_idx, i, flag, original, changed, expected);
          munmap(bp, bl);
          munmap(op, ol);
          return 0;
        }
        ++comparisons;
      }
      munmap(bp, bl);
      munmap(op, ol);
    }
  }
  printf("SCCP native x86 comparisons=%" PRIu64 " PASS\n", comparisons);
  return 1;
}
#endif

int main(void) {
  int ok = 1;
  Pipeline base = {0}, opt = {0};
  for (uint32_t arch = 0; arch < 3u && ok; ++arch) {
    AstTargetArch target = arch == 0u   ? AST_TARGET_ARCH_X86_64
                           : arch == 1u ? AST_TARGET_ARCH_AARCH64
                                        : AST_TARGET_ARCH_RISCV64;
    AstAbiKind abi = arch == 0u   ? AST_ABI_SYSV_AMD64
                     : arch == 1u ? AST_ABI_AAPCS64
                                  : AST_ABI_RISCV_LP64;
    if (!build(&base, 0, target, abi) || !build(&opt, 1, target, abi)) {
      ok = 0;
      break;
    }
    if (!negative_proofs(&opt))
      ok = 0;
    size_t before_bytes = 0u, after_bytes = 0u;
    for (size_t i = 0; i < base.encoded.function_count; ++i)
      before_bytes += base.encoded.functions[i].size;
    for (size_t i = 0; i < opt.encoded.function_count; ++i)
      after_bytes += opt.encoded.functions[i].size;
    printf("SCCP ISA=%u aggregate BIN bytes: original=%zu optimized=%zu\n",
           arch, before_bytes, after_bytes);
#if defined(__linux__) && defined(__x86_64__)
    if (arch == 0u && ok && !execute(&base, &opt))
      ok = 0;
#endif
    printf("SCCP target=%u functions=%zu optimized=%zu\n", arch,
           base.encoded.function_count, opt.encoded.function_count);
    destroy(&base);
    destroy(&opt);
  }
  destroy(&base);
  destroy(&opt);
  return ok ? 0 : 1;
}
