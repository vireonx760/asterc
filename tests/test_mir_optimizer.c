#include "asterasm/asterasm.h"
#include "mir_internal.h"

#include <stdio.h>
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

static void close_fixture(Fixture *f) {
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
}

static int open_fixture(Fixture *f, int promote) {
  memset(f, 0, sizeof(*f));
  const char *root = ASTERASM_SOURCE_DIR "/tests/mir_cases/optimizer";
  const char *entry = ASTERASM_SOURCE_DIR "/tests/mir_cases/optimizer/main.aa";
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0 ||
      ast_frontend_load(f->frontend, &entry, 1u) != 0)
    return 0;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0)
    return 0;
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir))
    return 0;
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  mc.promote_locals = (uint8_t)promote;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  return f->mir && ast_mir_verify_mode(f->mir, AST_MIR_VERIFY_PRE_OPT);
}

static int check(int success, const char *what) {
  if (!success)
    fprintf(stderr, "optimizer regression: %s\n", what);
  return success;
}

static int rejected_then_restored(Fixture *f, uint64_t *field,
                                  const char *what) {
  uint64_t before = *field;
  *field ^= UINT64_C(1);
  int ok = check(!ast_mir_verify_mode(f->mir, AST_MIR_VERIFY_POST_OPT), what);
  *field = before;
  return check(ast_mir_verify_mode(f->mir, AST_MIR_VERIFY_POST_OPT),
               "recovery after mutation") &&
         ok;
}

static uint32_t effects(const AstMirFunction *fn) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < fn->instruction_count; ++i)
    if (mir_instruction_is_effectful(&fn->instructions[i]))
      count++;
  return count;
}

int main(void) {
  int ok = 1, mutations = 0;
  for (int promote = 0; promote <= 1 && ok; ++promote) {
    Fixture f;
    if (!open_fixture(&f, promote)) {
      close_fixture(&f);
      return 1;
    }
    AstMirFunction *original = f.mir->functions[0];
    AstMirOptimizeStats stats = {0};
    /* Invalid input must fail before publishing any replacement. */
    uint32_t saved = original->blocks[0].term.info.target_true;
    original->blocks[0].term.info.target_true = original->block_count;
    ok = check(!ast_mir_optimize(f.mir, &stats) &&
                   f.mir->functions[0] == original,
               "transaction accepted malformed input");
    original->blocks[0].term.info.target_true = saved;
    ok = ok && check(ast_mir_verify(f.mir), "original input recovery") &&
         check(ast_mir_optimize(f.mir, &stats), "optimization failed") &&
         check(ast_mir_verify_mode(f.mir, AST_MIR_VERIFY_POST_OPT),
               "POST verification failed");
    if (promote)
      ok = ok && check(stats.blocks_removed && stats.instructions_removed &&
                           stats.phis_removed && stats.branch_folded,
                       "structural/PHI/dead-cycle reductions absent");
    if (!ok) {
      close_fixture(&f);
      break;
    }
    ok = check(!ast_mir_verify_mode(f.mir, AST_MIR_VERIFY_PRE_OPT),
               "optimized input accepted as original") &&
         check(!ast_mir_optimize(f.mir, NULL),
               "second pass overwrote source proof") &&
         check(ast_mir_verify(f.mir), "POST recovery after PRE rejection");
    for (size_t fi = 0; fi < f.mir->function_count && ok; ++fi) {
      AstMirFunction *fn = f.mir->functions[fi];
      if (!fn->optimization_proof) {
        ok = 0;
        break;
      }
      ok = check(effects(fn) == effects(fn->optimization_proof->original),
                 "observable operation removed");
      for (uint32_t i = 0; i < fn->instruction_count && ok; ++i) {
        AstMirInstructionInfo *info = &fn->instructions[i].info;
        if (info->op != AST_MIR_OP_CONSTANT)
          continue;
        ok = rejected_then_restored(&f, &info->constant.lo,
                                    "changed constant accepted");
        mutations++;
        break;
      }
      if (!ok)
        break;
      AstMirFunction *proof_source = fn->optimization_proof->original;
      fn->optimization_proof->original = NULL;
      ok = check(!ast_mir_verify(f.mir), "missing source proof accepted");
      fn->optimization_proof->original = proof_source;
      ok = check(ast_mir_verify(f.mir), "source proof recovery") && ok;
      mutations++;
      for (uint32_t i = 0; i < proof_source->instruction_count && ok; ++i) {
        AstMirInstructionInfo *info = &proof_source->instructions[i].info;
        if (info->op != AST_MIR_OP_CONSTANT)
          continue;
        ok = rejected_then_restored(&f, &info->constant.lo,
                                    "changed original snapshot accepted");
        mutations++;
        break;
      }
      for (uint32_t i = 0; i < fn->instruction_count && ok; ++i) {
        AstMirInstructionInfo *info = &fn->instructions[i].info;
        if (info->op != AST_MIR_OP_BINARY || info->token_op != AST_TOK_PLUS)
          continue;
        AstTokenKind before = info->token_op;
        info->token_op = AST_TOK_CARET;
        ok = check(!ast_mir_verify(f.mir),
                   "type-correct changed operator accepted");
        info->token_op = before;
        ok = check(ast_mir_verify(f.mir), "operator recovery") && ok;
        mutations++;
        break;
      }
      uint32_t flags = fn->flags;
      fn->flags ^= AST_HIR_FUNCTION_NAKED;
      ok = check(!ast_mir_verify(f.mir), "changed function contract accepted");
      fn->flags = flags;
      ok = check(ast_mir_verify(f.mir), "function contract recovery") && ok;
      mutations++;
      for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
        AstMirTerminatorInfo *term = &fn->blocks[b].term.info;
        if (term->kind == AST_MIR_TERM_RETURN) {
          AstMirVReg value = term->condition_or_value;
          term->condition_or_value = AST_MIR_INVALID_ID;
          ok = check(!ast_mir_verify_structural(f.mir),
                     "structural checker accepted missing return value");
          term->condition_or_value = value;
          ok = check(ast_mir_verify(f.mir), "return value recovery") && ok;
          mutations++;
        }
        if (term->kind != AST_MIR_TERM_COND_BR)
          continue;
        uint32_t target = term->target_true;
        term->target_true = fn->block_count;
        ok = check(!ast_mir_verify(f.mir), "changed CFG accepted");
        term->target_true = target;
        ok = check(ast_mir_verify(f.mir), "CFG recovery") && ok;
        mutations++;
        break;
      }
    }
    printf("MIR optimizer promote=%d blocks=%llu instructions=%llu phis=%llu "
           "rounds=%llu\n",
           promote, (unsigned long long)stats.blocks_removed,
           (unsigned long long)stats.instructions_removed,
           (unsigned long long)stats.phis_removed,
           (unsigned long long)stats.rounds);
    close_fixture(&f);
  }
  if (!check(mutations >= 20, "mutation corpus not exercised"))
    ok = 0;
  printf("MIR optimizer: %d proof/CFG/type mutations checked\n", mutations);
  return ok ? 0 : 1;
}
