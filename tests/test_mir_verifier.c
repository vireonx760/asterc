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
  int rn = snprintf(root, sizeof(root), "%s/tests/sema_cases/%s",
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

static int has_code(const AstMirProgram *mir, AstMirDiagCode code) {
  size_t count = 0u;
  const AstMirDiagnostic *diagnostics = ast_mir_diagnostics(mir, &count);
  for (size_t i = 0u; i < count; ++i)
    if (diagnostics[i].code == code)
      return 1;
  return 0;
}

static int expect_failure(Fixture *fixture, AstMirDiagCode code,
                          const char *name) {
  int accepted = ast_mir_verify(fixture->mir);
  int ok = !accepted && has_code(fixture->mir, code);
  if (!ok) {
    fprintf(stderr, "MIR verifier mutation '%s' was not rejected with %s\n",
            name, ast_mir_diag_code_name(code));
    size_t count = 0u;
    const AstMirDiagnostic *diagnostics =
        ast_mir_diagnostics(fixture->mir, &count);
    for (size_t i = 0u; i < count; ++i)
      fprintf(stderr, "  %s: %s\n", ast_mir_diag_code_name(diagnostics[i].code),
              diagnostics[i].message ? diagnostics[i].message : "");
  }
  return ok;
}

static AstMirInstruction *find_instruction(AstMirFunction *function,
                                           AstMirOp op, uint32_t occurrence) {
  if (!function)
    return NULL;
  for (uint32_t i = 0u; i < function->instruction_count; ++i) {
    if (function->instructions[i].info.op == op) {
      if (occurrence == 0u)
        return &function->instructions[i];
      occurrence--;
    }
  }
  return NULL;
}

static int test_missing_terminator(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  fixture.mir->functions[0]->blocks[0].term.info.kind = AST_MIR_TERM_NONE;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_MISSING_TERMINATOR,
                          "missing terminator");
  fixture_destroy(&fixture);
  return ok;
}

static int test_invalid_successor(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[0];
  function->blocks[0].term.info.target_true = function->block_count + 9u;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_INVALID_SUCCESSOR,
                          "invalid successor");
  fixture_destroy(&fixture);
  return ok;
}

static int test_branch_argument_count(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[1];
  AstMirTerminator *branch = NULL;
  for (uint32_t bi = 0u; bi < function->block_count && !branch; ++bi) {
    AstMirTerminator *candidate = &function->blocks[bi].term;
    if (candidate->info.kind == AST_MIR_TERM_BR &&
        candidate->info.true_arg_count != 0u)
      branch = candidate;
  }
  if (!branch) {
    fixture_destroy(&fixture);
    return 0;
  }
  branch->info.true_arg_count = 0u;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_BRANCH_ARGUMENT_MISMATCH,
                          "branch argument count");
  fixture_destroy(&fixture);
  return ok;
}

static int test_cross_branch_dominance(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[1];
  AstMirBlock *return_block = NULL;
  AstMirVReg cross_branch = AST_MIR_INVALID_ID;
  for (uint32_t bi = 0u; bi < function->block_count; ++bi) {
    if (function->blocks[bi].term.info.kind == AST_MIR_TERM_RETURN)
      return_block = &function->blocks[bi];
    if (bi != 0u && function->blocks[bi].instruction_count != 0u) {
      AstMirInstruction *instruction =
          &function->instructions[function->blocks[bi].instructions[0]];
      if (instruction->info.result != AST_MIR_INVALID_ID)
        cross_branch = instruction->info.result;
    }
  }
  if (!return_block || cross_branch == AST_MIR_INVALID_ID) {
    fixture_destroy(&fixture);
    return 0;
  }
  return_block->term.info.condition_or_value = cross_branch;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_DOMINANCE, "cross-branch use");
  fixture_destroy(&fixture);
  return ok;
}

static int test_memory_chain(void) {
  Fixture fixture;
  if (!fixture_open("atomic_success", 1, &fixture))
    return 0;
  AstMirInstruction *atomic =
      find_instruction(fixture.mir->functions[0], AST_MIR_OP_ATOMIC, 0u);
  if (!atomic) {
    fixture_destroy(&fixture);
    return 0;
  }
  atomic->info.memory_in = AST_MIR_INVALID_ID;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                          "memory chain");
  fixture_destroy(&fixture);
  return ok;
}

static int test_memory_phi_input(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[1];
  AstMirMemoryInfo *phi = NULL;
  for (uint32_t bi = 0u; bi < function->block_count && !phi; ++bi) {
    AstMirMemoryInfo *candidate =
        &function->memory_values[function->blocks[bi].memory_argument].info;
    if (candidate->incoming_count > 1u)
      phi = candidate;
  }
  if (!phi) {
    fixture_destroy(&fixture);
    return 0;
  }
  ((AstMirMemoryId *)phi->incoming)[0] = AST_MIR_INVALID_ID;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                          "memory phi input");
  fixture_destroy(&fixture);
  return ok;
}

static int test_missing_memory_phi_table(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[1];
  AstMirMemoryInfo *phi = NULL;
  for (uint32_t bi = 0u; bi < function->block_count && !phi; ++bi) {
    AstMirMemoryInfo *candidate =
        &function->memory_values[function->blocks[bi].memory_argument].info;
    if (candidate->incoming_count > 1u)
      phi = candidate;
  }
  if (!phi) {
    fixture_destroy(&fixture);
    return 0;
  }
  const AstMirMemoryId *saved = phi->incoming;
  phi->incoming = NULL;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                          "missing memory phi table");
  phi->incoming = saved;
  fixture_destroy(&fixture);
  return ok;
}

static int test_missing_operand_table(void) {
  Fixture fixture;
  if (!fixture_open("core_success", 1, &fixture))
    return 0;
  AstMirInstruction *instruction = NULL;
  for (size_t fi = 0u; fi < fixture.mir->function_count && !instruction; ++fi) {
    AstMirFunction *function = fixture.mir->functions[fi];
    for (uint32_t ii = 0u; ii < function->instruction_count; ++ii) {
      if (function->instructions[ii].info.operand_count != 0u) {
        instruction = &function->instructions[ii];
        break;
      }
    }
  }
  if (!instruction) {
    fixture_destroy(&fixture);
    return 0;
  }
  AstMirVReg *saved = instruction->operands;
  const AstMirVReg *saved_info = instruction->info.operands;
  instruction->operands = NULL;
  instruction->info.operands = NULL;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_INVALID_OPERAND,
                          "missing operand table");
  instruction->operands = saved;
  instruction->info.operands = saved_info;
  fixture_destroy(&fixture);
  return ok;
}

static int test_effect_flags(void) {
  Fixture fixture;
  if (!fixture_open("facts_success", 1, &fixture))
    return 0;
  AstMirInstruction *load = NULL;
  for (size_t fi = 0u; fi < fixture.mir->function_count && !load; ++fi)
    load = find_instruction(fixture.mir->functions[fi], AST_MIR_OP_LOAD, 0u);
  if (!load) {
    fixture_destroy(&fixture);
    return 0;
  }
  load->info.effects = 0u;
  load->info.flags = 0u;
  int ok =
      expect_failure(&fixture, AST_MIR_DIAG_INVALID_EFFECTS, "load effects");
  fixture_destroy(&fixture);
  return ok;
}

static int test_stack_slot(void) {
  Fixture fixture;
  if (!fixture_open("core_success", 0, &fixture))
    return 0;
  AstMirInstruction *load =
      find_instruction(fixture.mir->functions[0], AST_MIR_OP_STACK_LOAD, 0u);
  if (!load) {
    fixture_destroy(&fixture);
    return 0;
  }
  load->info.stack_slot = UINT32_MAX;
  int ok =
      expect_failure(&fixture, AST_MIR_DIAG_INVALID_STACK_SLOT, "stack slot");
  fixture_destroy(&fixture);
  return ok;
}

static int test_cfg_metadata(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[0];
  function->blocks[1].immediate_dominator = AST_MIR_INVALID_ID;
  int ok =
      expect_failure(&fixture, AST_MIR_DIAG_INVALID_CFG, "immediate dominator");
  fixture_destroy(&fixture);
  return ok;
}

static int test_dominator_cycle(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[0];
  if (function->block_count < 2u) {
    fixture_destroy(&fixture);
    return 0;
  }
  function->blocks[1].immediate_dominator = 1u;
  int ok =
      expect_failure(&fixture, AST_MIR_DIAG_INVALID_CFG, "dominator cycle");
  fixture_destroy(&fixture);
  return ok;
}

static int test_liveness(void) {
  Fixture fixture;
  if (!fixture_open("facts_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[1];
  if (function->vreg_count == 0u) {
    fixture_destroy(&fixture);
    return 0;
  }
  function->vregs[0].info.spill_weight++;
  int ok =
      expect_failure(&fixture, AST_MIR_DIAG_INVALID_LIVENESS, "spill weight");
  fixture_destroy(&fixture);
  return ok;
}

static int test_hir_origin(void) {
  Fixture fixture;
  if (!fixture_open("facts_success", 1, &fixture))
    return 0;
  AstMirInstruction *instruction = NULL;
  for (size_t fi = 0u; fi < fixture.mir->function_count && !instruction; ++fi) {
    if (fixture.mir->functions[fi]->instruction_count != 0u)
      instruction = &fixture.mir->functions[fi]->instructions[0];
  }
  if (!instruction) {
    fixture_destroy(&fixture);
    return 0;
  }
  instruction->info.source_hir_instruction = UINT32_MAX - 1u;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_HIR_INVALID, "HIR origin");
  fixture_destroy(&fixture);
  return ok;
}

static int test_vreg_bank(void) {
  Fixture fixture;
  if (!fixture_open("core_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[0];
  if (function->vreg_count == 0u) {
    fixture_destroy(&fixture);
    return 0;
  }
  function->vregs[0].info.bank = AST_MIR_BANK_FLOAT;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_INVALID_VREG,
                          "virtual register bank");
  fixture_destroy(&fixture);
  return ok;
}

static int test_hir_cfg_origin(void) {
  Fixture fixture;
  if (!fixture_open("flow_success", 1, &fixture))
    return 0;
  AstMirFunction *function = fixture.mir->functions[0];
  function->blocks[0].hir_argument_count++;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_HIR_INVALID, "HIR CFG origin");
  fixture_destroy(&fixture);
  return ok;
}

static int test_direct_call_target(void) {
  Fixture fixture;
  if (!fixture_open("modules_success", 1, &fixture))
    return 0;
  AstMirFunction *owner = NULL;
  AstMirInstruction *call = NULL;
  for (size_t fi = 0u; fi < fixture.mir->function_count && !call; ++fi) {
    owner = fixture.mir->functions[fi];
    call = find_instruction(owner, AST_MIR_OP_CALL_DIRECT, 0u);
  }
  if (!call || !owner) {
    fixture_destroy(&fixture);
    return 0;
  }
  call->info.symbol = owner->name;
  int ok =
      expect_failure(&fixture, AST_MIR_DIAG_HIR_INVALID, "direct call target");
  fixture_destroy(&fixture);
  return ok;
}

static int test_native_conservative_flag(void) {
  Fixture fixture;
  if (!fixture_open("native_success", 1, &fixture))
    return 0;
  AstMirInstruction *native = NULL;
  for (size_t fi = 0u; fi < fixture.mir->function_count && !native; ++fi)
    native =
        find_instruction(fixture.mir->functions[fi], AST_MIR_OP_NATIVE, 0u);
  if (!native) {
    fixture_destroy(&fixture);
    return 0;
  }
  native->info.flags &= ~(uint32_t)AST_MIR_INST_CONSERVATIVE_EFFECTS;
  int ok = expect_failure(&fixture, AST_MIR_DIAG_INVALID_EFFECTS,
                          "native conservative effects");
  fixture_destroy(&fixture);
  return ok;
}

int main(void) {
  struct Test {
    const char *name;
    int (*run)(void);
  } tests[] = {{"missing_terminator", test_missing_terminator},
               {"invalid_successor", test_invalid_successor},
               {"branch_argument_count", test_branch_argument_count},
               {"cross_branch_dominance", test_cross_branch_dominance},
               {"memory_chain", test_memory_chain},
               {"memory_phi_input", test_memory_phi_input},
               {"missing_memory_phi_table", test_missing_memory_phi_table},
               {"missing_operand_table", test_missing_operand_table},
               {"effect_flags", test_effect_flags},
               {"stack_slot", test_stack_slot},
               {"cfg_metadata", test_cfg_metadata},
               {"dominator_cycle", test_dominator_cycle},
               {"liveness", test_liveness},
               {"hir_origin", test_hir_origin},
               {"vreg_bank", test_vreg_bank},
               {"hir_cfg_origin", test_hir_cfg_origin},
               {"direct_call_target", test_direct_call_target},
               {"native_conservative_flag", test_native_conservative_flag}};
  int ok = 1;
  for (size_t i = 0u; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    int passed = tests[i].run();
    if (!passed)
      fprintf(stderr, "MIR verifier test failed: %s\n", tests[i].name);
    ok &= passed;
  }
  if (!ok)
    return 1;
  puts("MIR verifier mutation tests passed");
  return 0;
}
