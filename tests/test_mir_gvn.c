#include "asterasm/asterasm.h"
#include "mir_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

typedef struct Fixture {
  AstFrontend *frontend;
  AstSema *sema;
  AstHirProgram *hir;
  AstMirProgram *mir;
} Fixture;

static void fixture_destroy(Fixture *fixture) {
  if (!fixture)
    return;
  ast_mir_destroy(fixture->mir);
  ast_hir_destroy(fixture->hir);
  ast_sema_destroy(fixture->sema);
  ast_frontend_destroy(fixture->frontend);
  memset(fixture, 0, sizeof(*fixture));
}

static int fixture_open(const char *case_name, int promote_locals,
                        Fixture *out) {
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/mir_cases/%s",
                    ASTERASM_SOURCE_DIR, case_name);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (!out || rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 0;
  memset(out, 0, sizeof(*out));
  AstFrontendConfig frontend_config = ast_frontend_config_default();
  frontend_config.thread_count = 2u;
  out->frontend = ast_frontend_create(&frontend_config);
  if (!out->frontend ||
      ast_frontend_add_import_root(out->frontend, root) != 0) {
    fixture_destroy(out);
    return 0;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(out->frontend, entries, 1u) != 0) {
    fixture_destroy(out);
    return 0;
  }
  AstSemaConfig sema_config = ast_sema_config_default();
  sema_config.thread_count = 2u;
  out->sema = ast_sema_create(out->frontend, &sema_config);
  if (!out->sema || ast_sema_check(out->sema) != 0) {
    fixture_destroy(out);
    return 0;
  }
  AstHirConfig hir_config = ast_hir_config_default();
  hir_config.thread_count = 2u;
  out->hir = ast_hir_build(out->frontend, out->sema, &hir_config);
  if (!out->hir || !ast_hir_verify(out->hir)) {
    fixture_destroy(out);
    return 0;
  }
  AstMirConfig mir_config = ast_mir_config_default();
  mir_config.thread_count = 1u;
  mir_config.promote_locals = promote_locals != 0;
  mir_config.verify_after_build = 0u;
  out->mir = ast_mir_build(out->frontend, out->hir, &mir_config);
  if (!out->mir || !ast_mir_verify(out->mir)) {
    fixture_destroy(out);
    return 0;
  }
  return 1;
}

static int tests(void) {
  Fixture f;
  if (!fixture_open("gvn_large", 1, &f)) {
    /* The GVN fixture must independently pass frontend, sema and MIR
     * verification. */
    return 0;
  }
  if (!ast_mir_verify_mode(f.mir, AST_MIR_VERIFY_PRE_OPT) ||
      !ast_mir_verify_mode(f.mir, AST_MIR_VERIFY_POST_OPT) ||
      ast_mir_verify_mode(f.mir, (AstMirVerifyMode)99)) {
    fixture_destroy(&f);
    return 0;
  }
  AstMirOptimizeStats stats = {0};
  int ok = ast_mir_optimize_local_gvn(f.mir, &stats) && stats.rewritten >= 2u &&
           stats.verifier_runs == 2u && ast_mir_verify(f.mir);
  if (ok && ast_mir_verify_mode(f.mir, AST_MIR_VERIFY_PRE_OPT))
    ok = 0;
  if (ok && !ast_mir_verify_mode(f.mir, AST_MIR_VERIFY_POST_OPT))
    ok = 0;
  if (ok) {
    uint32_t count_gvn = 0u, count_identity = 0u, count_fold = 0u;
    for (size_t fi = 0u; fi < f.mir->function_count; ++fi) {
      AstMirFunction *fn = f.mir->functions[fi];
      AstMirFunctionInfo info;
      if (!ast_mir_function_info((const AstMirFunction *)fn, &info)) {
        ok = 0;
        break;
      }
      uint32_t function_rewrites = 0u;
      for (uint32_t id = 0u; id < fn->instruction_count; ++id) {
        if (!fn->gvn_certificates || !fn->gvn_certificates[id].valid)
          continue;
        function_rewrites++;
        if (fn->gvn_certificates[id].valid == 1u)
          count_gvn++;
        if (fn->gvn_certificates[id].valid == 2u)
          count_identity++;
        if (fn->gvn_certificates[id].valid == 3u)
          count_fold++;
      }
      if ((info.name.span.length == 15u &&
           memcmp(info.name.text, "division_effects", 15u) == 0 &&
           function_rewrites) ||
          (info.name.span.length == 13u &&
           memcmp(info.name.text, "sibling_paths", 13u) == 0 &&
           function_rewrites)) {
        fprintf(stderr,
                "unexpected rewrite in effectful/sibling-block function\n");
        ok = 0;
      }
    }
    if (count_gvn < 66u || count_identity != 5u ||
        stats.cross_block_rewritten != 1u || count_fold < 3u) {
      fprintf(stderr, "unexpected pass counts: gvn=%u identity=%u fold=%u\n",
              count_gvn, count_identity, count_fold);
      ok = 0;
    }
  }
  if (ok) {
    uint32_t mutations = 0u;
    for (size_t fi = 0; fi < f.mir->function_count; ++fi) {
      AstMirFunction *fn = f.mir->functions[fi];
      for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        if (!fn->gvn_certificates || !fn->gvn_certificates[i].valid)
          continue;
        MirGvnCertificate *cert = &fn->gvn_certificates[i];
        AstMirInstId original_witness = cert->witness;
        cert->witness = i;
        if (ast_mir_verify(f.mir))
          ok = 0;
        cert->witness = original_witness;
        if (!ast_mir_verify(f.mir))
          ok = 0;
        mutations++;

        AstMirVReg original_operand = fn->instructions[i].operands[0];
        fn->instructions[i].operands[0] = fn->vreg_count;
        if (ast_mir_verify(f.mir))
          ok = 0;
        fn->instructions[i].operands[0] = original_operand;
        if (!ast_mir_verify(f.mir))
          ok = 0;
        mutations++;

        uint32_t original_effects = cert->original.info.effects;
        cert->original.info.effects |= AST_SEMA_EFFECT_WRITE_MEMORY;
        if (ast_mir_verify(f.mir))
          ok = 0;
        cert->original.info.effects = original_effects;
        if (!ast_mir_verify(f.mir))
          ok = 0;
        mutations++;

        if (mutations >= 12u)
          break;
      }
      if (mutations >= 12u)
        break;
    }
    if (mutations < 6u)
      ok = 0;
  }
  if (ok) {
    uint32_t negative_checks = 0u;
    for (size_t fi = 0u; fi < f.mir->function_count; ++fi) {
      AstMirFunction *fn = f.mir->functions[fi];
      for (uint32_t id = 0u; id < fn->instruction_count; ++id) {
        if (!fn->gvn_certificates || fn->gvn_certificates[id].valid != 3u)
          continue;
        MirGvnCertificate *cert = &fn->gvn_certificates[id];
        AstMirInstruction *inst = &fn->instructions[id];
        uint64_t saved = inst->info.constant.lo;
        inst->info.constant.lo ^= UINT64_C(1);
        if (ast_mir_verify(f.mir))
          ok = 0;
        inst->info.constant.lo = saved;
        if (!ast_mir_verify(f.mir))
          ok = 0;
        ++negative_checks;

        AstMirVReg original_operand = cert->original.operands[0];
        cert->original.operands[0] = fn->vreg_count;
        if (ast_mir_verify(f.mir))
          ok = 0;
        cert->original.operands[0] = original_operand;
        if (!ast_mir_verify(f.mir))
          ok = 0;
        ++negative_checks;

        AstMirInstId witness = cert->witness;
        cert->witness = id;
        if (ast_mir_verify(f.mir))
          ok = 0;
        cert->witness = witness;
        if (!ast_mir_verify(f.mir))
          ok = 0;
        ++negative_checks;
        if (negative_checks >= 9u)
          break;
      }
      if (negative_checks >= 9u)
        break;
    }
    if (negative_checks < 6u)
      ok = 0;
  }
  if (ok) {
    const AstTargetArch architectures[] = {AST_TARGET_ARCH_X86_64,
                                           AST_TARGET_ARCH_AARCH64,
                                           AST_TARGET_ARCH_RISCV64};
    const AstAbiKind abis[] = {AST_ABI_SYSV_AMD64, AST_ABI_AAPCS64,
                               AST_ABI_RISCV_LP64};
    for (size_t index = 0; index < 3u; ++index) {
      AstTargetConfig tc = ast_target_config_default(architectures[index]);
      tc.os = AST_TARGET_OS_LINUX;
      AstTargetProgram *target = ast_target_legalize(f.mir, &tc);
      if (!target || !ast_target_verify(target))
        ok = 0;
      AstAbiConfig ac = ast_abi_config_default(abis[index]);
      AstAbiProgram *abi = target && ok ? ast_abi_lower(target, &ac) : NULL;
      if (!abi || !ast_abi_verify(abi))
        ok = 0;
      AstBackendConfig bc = ast_backend_config_default();
      AstMachineProgram *machine =
          abi && ok ? ast_backend_lower(abi, &bc) : NULL;
      if (!machine || !ast_machine_verify(machine))
        ok = 0;
      ast_machine_destroy(machine);
      ast_abi_destroy(abi);
      ast_target_destroy(target);
      if (!ok) {
        fprintf(stderr, "optimized codegen failed ISA index=%zu\n", index);
        break;
      }
    }
  }
  if (ok && ast_mir_optimize_local_gvn(f.mir, NULL))
    ok = 0; /* no double rewrite */
  printf("GVN verified %llu rewrites / %llu examined, backend=%s\n",
         (unsigned long long)stats.rewritten,
         (unsigned long long)stats.examined, ok ? "PASS" : "FAIL");
  fixture_destroy(&f);
  return ok;
}

int main(void) { return tests() ? 0 : 1; }
