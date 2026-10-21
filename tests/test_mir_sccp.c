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

static int name_equals(AstName name, const char *text) {
  size_t size = strlen(text);
  return name.text && name.span.length == size &&
         memcmp(name.text, text, size) == 0;
}

int main(void) {
  Fixture f;
  if (!open_fixture(&f))
    return 1;
  int ok = 1;
  uint32_t seen = 0u;
  for (size_t fi = 0u; fi < f.mir->function_count; ++fi) {
    const AstMirFunction *fn = f.mir->functions[fi];
    AstMirSccpResult result = {0};
    if (!ast_mir_sccp_analyze(f.mir, fi, &result) ||
        result.value_count != fn->vreg_count ||
        result.block_count != fn->block_count ||
        result.executable_block_count == 0u || result.iterations == 0u) {
      ok = 0;
      ast_mir_sccp_result_destroy(&result);
      break;
    }
    uint32_t active_edges = 0u, active_blocks = 0u;
    for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
      if (result.executable_blocks[bi])
        active_blocks++;
      const AstMirBlock *b = &fn->blocks[bi];
      for (uint32_t e = 0u; e < b->successor_count; ++e) {
        if (result.executable_edges[(size_t)bi * 2u + e]) {
          active_edges++;
          if (!result.executable_blocks[b->successors[e]])
            ok = 0;
        }
      }
    }
    if (active_edges != result.executable_edge_count ||
        active_blocks != result.executable_block_count)
      ok = 0;
    if (name_equals(fn->name, "constant_true") ||
        name_equals(fn->name, "constant_false")) {
      uint32_t conditional = 0u, known = 0u;
      for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
        if (fn->blocks[bi].term.info.kind != AST_MIR_TERM_COND_BR ||
            !result.executable_blocks[bi])
          continue;
        conditional++;
        uint8_t t = result.executable_edges[(size_t)bi * 2u];
        uint8_t ff = result.executable_edges[(size_t)bi * 2u + 1u];
        if (t != ff)
          known++;
      }
      if (!conditional || !known)
        ok = 0;
      seen++;
    } else if (name_equals(fn->name, "common_phi") ||
               name_equals(fn->name, "conflict_phi")) {
      /* Find the actual SSA block arguments and verify the edge join.
       * Unrelated instructions may be constants as well. */
      uint32_t phi_seen = 0u, right = 0u;
      for (uint32_t b = 0u; b < fn->block_count; ++b) {
        const AstMirBlock *block = &fn->blocks[b];
        for (uint32_t a = 0u; a < block->argument_count; ++a) {
          AstMirVReg vr = block->arguments[a];
          if (fn->vregs[vr].info.flags & AST_MIR_VREG_FUNCTION_ARGUMENT)
            continue;
          phi_seen++;
          AstMirSccpValue v = result.values[vr];
          if (name_equals(fn->name, "common_phi"))
            right += v.kind == AST_MIR_SCCP_CONSTANT && v.value == 42u;
          else
            right += v.kind == AST_MIR_SCCP_OVERDEFINED;
        }
      }
      if (!phi_seen || !right)
        ok = 0;
      seen++;
    } else if (name_equals(fn->name, "loop_phi")) {
      if (result.executable_edge_count == 0u)
        ok = 0;
      seen++;
    } else if (name_equals(fn->name, "memory_effect")) {
      if (!result.effectful_instructions)
        ok = 0;
      seen++;
    } else if (name_equals(fn->name, "simple_math")) {
      if (result.constant_value_count < 3u)
        ok = 0;
      seen++;
    }
    printf("%.*s: blocks %u/%u edges=%u constants=%u overdefined=%u effects=%u "
           "iterations=%u\n",
           (int)fn->name.span.length, fn->name.text,
           result.executable_block_count, result.block_count,
           result.executable_edge_count, result.constant_value_count,
           result.overdefined_value_count, result.effectful_instructions,
           result.iterations);
    ast_mir_sccp_result_destroy(&result);
    if (!ok)
      break;
  }
  if (seen != 7u)
    ok = 0;
  if (ok && !ast_mir_verify(f.mir))
    ok = 0;
  /* The public analyzer must reject malformed CFG through the independent
   * original PRE verifier and must not mutate the source function. */
  if (ok) {
    AstMirFunction *fn = f.mir->functions[0];
    for (uint32_t bi = 0; bi < fn->block_count; ++bi) {
      if (fn->blocks[bi].term.info.kind != AST_MIR_TERM_COND_BR)
        continue;
      AstMirBlockId old = fn->blocks[bi].term.info.target_true;
      fn->blocks[bi].term.info.target_true = fn->block_count;
      AstMirSccpResult rejected = {0};
      if (ast_mir_sccp_analyze(f.mir, 0u, &rejected))
        ok = 0;
      ast_mir_sccp_result_destroy(&rejected);
      fn->blocks[bi].term.info.target_true = old;
      if (!ast_mir_verify(f.mir))
        ok = 0;
      break;
    }
  }
  close_fixture(&f);
  return ok ? 0 : 1;
}
