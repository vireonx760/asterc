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
  const char *root = ASTERASM_SOURCE_DIR "/tests/mir_cases/inlining";
  const char *entry = ASTERASM_SOURCE_DIR "/tests/mir_cases/inlining/main.aa";
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
  return f->mir && ast_mir_verify(f->mir) && ast_mir_optimize(f->mir, NULL);
}

static int check(int ok, const char *what) {
  if (!ok)
    fprintf(stderr, "MIR inlining regression: %s\n", what);
  return ok;
}

static int recovered(Fixture *f) {
  return check(ast_mir_verify_mode(f->mir, AST_MIR_VERIFY_POST_OPT),
               "proof recovery");
}

static int exported_linkage(void) {
  for (unsigned closed = 0; closed <= 1u; ++closed) {
    Fixture f;
    if (!open_fixture(&f, 1)) {
      close_fixture(&f);
      return 0;
    }
    AstMirInlineConfig config = ast_mir_inline_config_default();
    config.closed_world = (uint8_t)closed;
    int ok = check(ast_mir_inline(f.mir, &config, NULL),
                   "linkage-policy inlining failed");
    unsigned calls = 0, definitions = 0;
    for (size_t fi = 0; fi < f.mir->function_count; ++fi) {
      const AstMirFunction *fn = f.mir->functions[fi];
      if (fn->name.span.length == 12u &&
          !memcmp(fn->name.text, "public_value", 12u))
        definitions++;
      for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const AstMirInstructionInfo *inst = &fn->instructions[i].info;
        if (inst->op == AST_MIR_OP_CALL_DIRECT &&
            inst->symbol.span.length == 12u &&
            !memcmp(inst->symbol.text, "public_value", 12u))
          calls++;
      }
    }
    ok = check(definitions == 1u && calls == (closed ? 0u : 1u),
               "export interposition/body retention contract") &&
         ok;
    close_fixture(&f);
    if (!ok)
      return 0;
  }
  return 1;
}

static int limits(void) {
  for (unsigned kind = 0; kind < 7u; ++kind) {
    Fixture f;
    int ok = open_fixture(&f, 1);
    if (!ok) {
      close_fixture(&f);
      return 0;
    }
    AstMirFunction **original = f.mir->functions;
    AstMirInlineConfig config = ast_mir_inline_config_default();
    switch (kind) {
    case 0:
      config.mode = AST_MIR_INLINE_NONE;
      break;
    case 1:
      config.max_sites = 0;
      break;
    case 2:
      config.max_depth = 0;
      break;
    case 3:
      config.max_sites = 1;
      break;
    case 4:
      config.max_depth = 1;
      break;
    case 5:
      config.max_callee_instructions = 0;
      break;
    default:
      config.max_function_instructions = 1;
      break;
    }
    AstMirInlineStats stats = {0};
    ok = check(ast_mir_inline(f.mir, &config, &stats),
               "budgeted transaction failed") &&
         recovered(&f);
    if (kind < 3u || kind > 4u)
      ok = check(!stats.inlined && f.mir->functions == original &&
                     !f.mir->inline_proof,
                 "disabled/exhausted budget published edits") &&
           ok;
    if (kind == 3u)
      ok = check(stats.inlined == 1u, "site budget exceeded") && ok;
    if (kind == 4u)
      ok =
          check(stats.inlined && stats.max_depth == 1u && stats.rejected_budget,
                "depth budget exceeded or not exercised") &&
          ok;
    close_fixture(&f);
    if (!ok)
      return 0;
  }
  return 1;
}

static int reject_field(Fixture *f, uint32_t *field, const char *what) {
  uint32_t saved = *field;
  *field ^= 1u;
  int ok = check(!ast_mir_verify(f->mir), what);
  *field = saved;
  return recovered(f) && ok;
}

int main(void) {
  int ok = limits() && exported_linkage(), mutations = 0;
  for (int promote = 0; promote <= 1 && ok; ++promote)
    for (unsigned mode = 1u; mode <= 2u && ok; ++mode) {
      Fixture f;
      if (!open_fixture(&f, promote)) {
        close_fixture(&f);
        fprintf(stderr, "inlining frontend fixture failed\n");
        return 1;
      }
      AstMirInlineConfig config = ast_mir_inline_config_default();
      config.mode = (AstMirInlineMode)mode;
      AstMirFunction **before = f.mir->functions;
      AstMirInlineStats stats = {0};
      AstMirInlineConfig bad = config;
      bad.max_depth = 65u;
      ok = check(!ast_mir_inline(f.mir, &bad, NULL) &&
                     f.mir->functions == before,
                 "bad config changed program") &&
           recovered(&f);
      bad = config;
      bad.closed_world = 2u;
      ok = check(!ast_mir_inline(f.mir, &bad, NULL) &&
                     f.mir->functions == before,
                 "invalid linkage policy accepted") &&
           recovered(&f) && ok;
      if (ok)
        ok = check(ast_mir_inline(f.mir, &config, &stats), "inline failed");
      if (!ok) {
        size_t count = 0;
        const AstMirDiagnostic *d = ast_mir_diagnostics(f.mir, &count);
        for (size_t i = 0; i < count; ++i)
          fprintf(stderr, "%s\n", d[i].message);
        close_fixture(&f);
        break;
      }
      ok = check(stats.inlined >= 5u && stats.rejected_recursive &&
                     stats.rejected_contract,
                 "positive/recursive/escape cases not exercised") &&
           check(f.mir->inline_proof && f.mir->inline_proof->original == before,
                 "missing owned source snapshot") &&
           recovered(&f) &&
           check(!ast_mir_inline(f.mir, &config, NULL),
                 "second inline accepted") &&
           check(!ast_mir_verify_mode(f.mir, AST_MIR_VERIFY_PRE_OPT),
                 "inlined graph accepted as original") &&
           recovered(&f);
      if (ok) {
        size_t saved = f.mir->inline_proof->function_count;
        f.mir->inline_proof->function_count--;
        ok =
            check(!ast_mir_verify(f.mir), "changed source table size accepted");
        f.mir->inline_proof->function_count = saved;
        ok = recovered(&f) && ok;
        mutations++;
        uint32_t sites = f.mir->inline_proof->config.max_sites;
        f.mir->inline_proof->config.max_sites = 0;
        ok = check(!ast_mir_verify(f.mir),
                   "inconsistent replay budget accepted") &&
             ok;
        f.mir->inline_proof->config.max_sites = sites;
        ok = recovered(&f) && ok;
        mutations++;
      }
      for (size_t fi = 0; fi < f.mir->function_count && ok; ++fi) {
        AstMirFunction *fn = f.mir->functions[fi];
        if (!fn->inline_site_count)
          continue;
        AstMirInlineSiteInfo source;
        ok = check(ast_mir_function_inline_site(fn, 1u, &source) &&
                       source.callee_name.text,
                   "missing source site API");
        uint32_t parent = fn->inline_sites[0].parent;
        fn->inline_sites[0].parent = fn->inline_site_count;
        ok = check(!ast_mir_verify(f.mir), "cyclic source site accepted") && ok;
        fn->inline_sites[0].parent = parent;
        ok = recovered(&f) && ok;
        mutations++;
        ok = reject_field(&f, &fn->flags,
                          "changed inline function contract accepted") &&
             ok;
        mutations++;
        int changed_args = 0, changed_memory = 0, changed_slot = 0;
        for (uint32_t i = 0; i < fn->instruction_count && ok; ++i) {
          AstMirInstructionInfo *inst = &fn->instructions[i].info;
          if (inst->inline_site) {
            ok = reject_field(&f, &inst->inline_site,
                              "changed instruction origin accepted") &&
                 ok;
            mutations++;
            /* Only one origin mutation per function keeps replay tests bounded.
             */
            break;
          }
        }
        for (uint32_t i = 0; i < fn->instruction_count && ok; ++i) {
          AstMirInstruction *inst = &fn->instructions[i];
          if (!changed_args && inst->info.operand_count) {
            ok = reject_field(&f, &inst->operands[0],
                              "changed inline SSA operand accepted") &&
                 ok;
            changed_args = 1;
            mutations++;
          }
          if (!changed_memory && inst->info.memory_in != AST_MIR_INVALID_ID) {
            ok = reject_field(&f, &inst->info.memory_in,
                              "changed inline memory chain accepted") &&
                 ok;
            changed_memory = 1;
            mutations++;
          }
          if (!changed_slot && inst->info.stack_slot != AST_MIR_INVALID_ID) {
            ok = reject_field(&f, &inst->info.stack_slot,
                              "changed inline stack ownership accepted") &&
                 ok;
            changed_slot = 1;
            mutations++;
          }
        }
        for (uint32_t b = 0; b < fn->block_count && ok; ++b) {
          AstMirTerminatorInfo *term = &fn->blocks[b].term.info;
          if (term->kind != AST_MIR_TERM_BR &&
              term->kind != AST_MIR_TERM_COND_BR)
            continue;
          ok = reject_field(&f, &term->target_true,
                            "changed inline CFG edge accepted") &&
               ok;
          mutations++;
          break;
        }
        for (uint32_t i = 0; i < fn->instruction_count && ok; ++i) {
          AstMirInstructionInfo *inst = &fn->instructions[i].info;
          if (inst->op != AST_MIR_OP_CONSTANT)
            continue;
          uint64_t constant = inst->constant.lo;
          inst->constant.lo ^= 1u;
          ok = check(!ast_mir_verify(f.mir), "changed inlined result accepted");
          inst->constant.lo = constant;
          ok = recovered(&f) && ok;
          mutations++;
          break;
        }
      }
      printf("MIR inline promote=%d mode=%u sites=%llu depth=%u recursive=%llu "
             "contract=%llu\n",
             promote, mode, (unsigned long long)stats.inlined, stats.max_depth,
             (unsigned long long)stats.rejected_recursive,
             (unsigned long long)stats.rejected_contract);
      close_fixture(&f);
    }
  printf("MIR inline: %d source/proof mutations\n", mutations);
  return ok && mutations >= 8 ? 0 : 1;
}
