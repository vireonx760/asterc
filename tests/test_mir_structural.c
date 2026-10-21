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
  memset(f, 0, sizeof(*f));
}

static int open_fixture(Fixture *f) {
  memset(f, 0, sizeof(*f));
  char root[4096], entry[4096];
  int n = snprintf(root, sizeof(root), "%s/tests/mir_cases/sccp_edges",
                   ASTERASM_SOURCE_DIR);
  if (n <= 0 || (size_t)n >= sizeof(root))
    return 0;
  n = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (n <= 0 || (size_t)n >= sizeof(entry))
    return 0;
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0)
    goto fail;
  const char *paths[] = {entry};
  if (ast_frontend_load(f->frontend, paths, 1u) != 0)
    goto fail;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0)
    goto fail;
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir))
    goto fail;
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  mc.promote_locals = 1u;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir))
    goto fail;
  return 1;
fail:
  fprintf(stderr, "failed SCCP frontend/HIR/MIR fixture\n");
  close_fixture(f);
  return 0;
}

static int fails(Fixture *f, const char *label) {
  if (!ast_mir_verify_structural(f->mir))
    return 1;
  fprintf(stderr, "structural verifier accepted %s\n", label);
  return 0;
}

static int restored(Fixture *f, const char *label) {
  if (ast_mir_verify(f->mir) && ast_mir_verify_structural(f->mir))
    return 1;
  fprintf(stderr, "structural verifier did not recover after %s\n", label);
  return 0;
}

int main(void) {
  Fixture f;
  if (!open_fixture(&f))
    return 1;
  int ok = ast_mir_verify_structural(f.mir);
  int count = 0;
  for (size_t fi = 0; fi < f.mir->function_count && ok; ++fi) {
    AstMirFunction *fn = f.mir->functions[fi];
    for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
      AstMirBlock *block = &fn->blocks[b];
      if (block->term.info.kind == AST_MIR_TERM_COND_BR) {
        AstMirBlockId saved = block->term.info.target_false;
        block->term.info.target_false = fn->block_count;
        ok = fails(&f, "bad CFG edge");
        count++;
        block->term.info.target_false = saved;
        if (ok)
          ok = restored(&f, "bad CFG edge");
        break;
      }
    }
    if (!ok)
      break;
    for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
      AstMirBlock *block = &fn->blocks[b];
      if (block->instruction_count) {
        AstMirInstId saved = block->instructions[0];
        block->instructions[0] = fn->instruction_count;
        ok = fails(&f, "orphan/invalid instruction");
        count++;
        block->instructions[0] = saved;
        if (ok)
          ok = restored(&f, "orphan/invalid instruction");
        break;
      }
    }
    if (!ok)
      break;
    for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
      AstMirBlock *block = &fn->blocks[b];
      if (!block->predecessor_count)
        continue;
      AstMirBlockId saved = block->predecessors[0];
      block->predecessors[0] = fn->block_count;
      ok = fails(&f, "predecessor mismatch");
      count++;
      block->predecessors[0] = saved;
      if (ok)
        ok = restored(&f, "predecessor mismatch");
      break;
    }
    if (!ok)
      break;
    for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
      AstMirBlock *block = &fn->blocks[b];
      for (uint32_t i = 0; i < block->instruction_count && ok; ++i) {
        AstMirInstruction *inst = &fn->instructions[block->instructions[i]];
        if (inst->info.operand_count &&
            inst->info.result != AST_MIR_INVALID_ID) {
          AstMirVReg saved = inst->operands[0];
          inst->operands[0] = inst->info.result; /* self-dependency */
          ok = fails(&f, "same-instruction SSA cycle");
          count++;
          inst->operands[0] = saved;
          if (ok)
            ok = restored(&f, "same-instruction SSA cycle");
          break;
        }
      }
    }
    if (!ok)
      break;
    for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
      AstMirBlock *block = &fn->blocks[b];
      for (uint32_t i = 0; i < block->instruction_count && ok; ++i) {
        AstMirInstruction *inst = &fn->instructions[block->instructions[i]];
        if (mir_instruction_is_effectful(inst)) {
          AstMirMemoryId saved = inst->info.memory_in;
          inst->info.memory_in = AST_MIR_INVALID_ID;
          ok = fails(&f, "broken memory chain");
          count++;
          inst->info.memory_in = saved;
          if (ok)
            ok = restored(&f, "broken memory chain");
          break;
        }
      }
    }
    if (!ok)
      break;
    if (fn->vreg_count && ok) {
      uint32_t vi = 0;
      uint64_t saved = fn->vregs[vi].info.live_start;
      fn->vregs[vi].info.live_start ^= UINT64_C(1);
      ok = fails(&f, "noncanonical SSA liveness");
      count++;
      fn->vregs[vi].info.live_start = saved;
      if (ok)
        ok = restored(&f, "noncanonical SSA liveness");
    }
    if (!ok)
      break;
    for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
      AstMirBlock *block = &fn->blocks[b];
      if (block->term.info.kind != AST_MIR_TERM_BR ||
          block->term.info.true_arg_count == 0)
        continue;
      uint32_t saved = block->term.info.true_arg_count;
      block->term.info.true_arg_count = 0;
      ok = fails(&f, "phi/edge arity");
      count++;
      block->term.info.true_arg_count = saved;
      if (ok)
        ok = restored(&f, "phi/edge arity");
      break;
    }
  }
  /* Structural validation is intentionally independent of HIR fidelity:
   * HIR-origin loss is caught by the origin verifier, but is not a malformed
   * SSA/CFG graph. */
  if (ok) {
    AstMirInstruction *inst = NULL;
    for (size_t fi = 0; fi < f.mir->function_count && !inst; ++fi) {
      AstMirFunction *fn = f.mir->functions[fi];
      for (uint32_t ii = 0; ii < fn->instruction_count; ++ii) {
        if (fn->instructions[ii].info.source_hir_instruction !=
            AST_HIR_INVALID_ID) {
          inst = &fn->instructions[ii];
          break;
        }
      }
    }
    if (!inst)
      ok = 0;
    else {
      AstHirInstId old = inst->info.source_hir_instruction;
      inst->info.source_hir_instruction = AST_HIR_INVALID_ID;
      if (!ast_mir_verify_structural(f.mir) || ast_mir_verify(f.mir))
        ok = 0;
      inst->info.source_hir_instruction = old;
      if (ok)
        ok = restored(&f, "independent HIR provenance");
    }
  }
  if (count < 5) {
    fprintf(stderr, "only %d negative cases\n", count);
    ok = 0;
  }
  if (ok) {
    AstMirOptimizeStats stats = {0};
    if (!ast_mir_optimize_scalar(f.mir, &stats) ||
        !ast_mir_verify_structural(f.mir) || !stats.verifier_runs)
      ok = 0;
  }
  printf("structural CFG/SSA/memory test: %d mutations, %s\n", count,
         ok ? "PASS" : "FAIL");
  close_fixture(&f);
  return ok ? 0 : 1;
}
