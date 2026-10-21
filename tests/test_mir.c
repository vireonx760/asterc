#include "asterasm/asterasm.h"

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

static void close_fixture(Fixture *fixture) {
  if (!fixture)
    return;
  ast_mir_destroy(fixture->mir);
  ast_hir_destroy(fixture->hir);
  ast_sema_destroy(fixture->sema);
  ast_frontend_destroy(fixture->frontend);
  memset(fixture, 0, sizeof(*fixture));
}

static int open_case(const char *group, const char *name, int promote_locals,
                     Fixture *fixture) {
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/%s/%s", ASTERASM_SOURCE_DIR,
                    group, name);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (!fixture || rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 0;
  memset(fixture, 0, sizeof(*fixture));
  AstFrontendConfig frontend_config = ast_frontend_config_default();
  frontend_config.thread_count = 3u;
  fixture->frontend = ast_frontend_create(&frontend_config);
  if (!fixture->frontend ||
      ast_frontend_add_import_root(fixture->frontend, root) != 0) {
    close_fixture(fixture);
    return 0;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(fixture->frontend, entries, 1u) != 0) {
    close_fixture(fixture);
    return 0;
  }
  AstSemaConfig sema_config = ast_sema_config_default();
  sema_config.thread_count = 3u;
  fixture->sema = ast_sema_create(fixture->frontend, &sema_config);
  if (!fixture->sema || ast_sema_check(fixture->sema) != 0) {
    close_fixture(fixture);
    return 0;
  }
  AstHirConfig hir_config = ast_hir_config_default();
  hir_config.thread_count = 3u;
  fixture->hir = ast_hir_build(fixture->frontend, fixture->sema, &hir_config);
  if (!fixture->hir || !ast_hir_verify(fixture->hir)) {
    close_fixture(fixture);
    return 0;
  }
  AstMirConfig mir_config = ast_mir_config_default();
  mir_config.thread_count = 3u;
  mir_config.promote_locals = promote_locals != 0;
  fixture->mir = ast_mir_build(fixture->frontend, fixture->hir, &mir_config);
  if (!fixture->mir || !ast_mir_verify(fixture->mir) ||
      ast_mir_stats(fixture->mir).error_count != 0u) {
    close_fixture(fixture);
    return 0;
  }
  return 1;
}

static int test_promoted_ssa(void) {
  Fixture fixture;
  if (!open_case("sema_cases", "loop_break_init_success", 1, &fixture))
    return 0;
  AstMirStats stats = ast_mir_stats(fixture.mir);
  const AstMirFunction *function = NULL;
  int ok = stats.stack_slot_count == 0u && stats.promoted_local_count == 2u &&
           stats.phi_count == 2u &&
           ast_mir_find_function(fixture.mir, "sema.loop_break_init_success",
                                 "f", &function);
  AstMirFunctionInfo info = {0};
  if (ok && (!ast_mir_function_info(function, &info) ||
             info.block_count != 6u || info.phi_count != 2u))
    ok = 0;
  unsigned phi_arguments = 0u;
  for (uint32_t bi = 0u; ok && bi < info.block_count; ++bi) {
    AstMirBlockInfo block;
    if (!ast_mir_function_block(function, bi, &block)) {
      ok = 0;
      break;
    }
    for (uint32_t ai = 0u; ai < block.argument_count; ++ai) {
      AstMirVRegInfo value;
      if (!ast_mir_function_vreg(function, block.arguments[ai], &value)) {
        ok = 0;
        break;
      }
      if (value.flags & AST_MIR_VREG_PHI) {
        phi_arguments++;
        if (block.argument_source_locals[ai] == AST_HIR_INVALID_ID)
          ok = 0;
      }
    }
  }
  if (phi_arguments != 2u)
    ok = 0;
  close_fixture(&fixture);
  return ok;
}

static int test_materialized_locals(void) {
  Fixture fixture;
  if (!open_case("sema_cases", "core_success", 0, &fixture))
    return 0;
  AstMirStats stats = ast_mir_stats(fixture.mir);
  const AstMirFunction *function = ast_mir_function_at(fixture.mir, 0u);
  AstMirFunctionInfo info = {0};
  int ok = function && stats.stack_slot_count == 3u &&
           stats.promoted_local_count == 0u &&
           ast_mir_function_info(function, &info) &&
           info.stack_slot_count == 3u;
  for (uint32_t si = 0u; ok && si < info.stack_slot_count; ++si) {
    AstMirStackSlotInfo slot;
    if (!ast_mir_function_stack_slot(function, si, &slot) || slot.id != si ||
        slot.size != 8u || slot.alignment != 8u ||
        (slot.flags & AST_MIR_STACK_SLOT_ADDRESS_TAKEN))
      ok = 0;
  }
  unsigned loads = 0u, stores = 0u;
  for (uint32_t ii = 0u; ok && ii < info.instruction_count; ++ii) {
    AstMirInstructionInfo instruction;
    if (!ast_mir_function_instruction(function, ii, &instruction)) {
      ok = 0;
      break;
    }
    if (instruction.op == AST_MIR_OP_STACK_LOAD)
      loads++;
    if (instruction.op == AST_MIR_OP_STACK_STORE)
      stores++;
  }
  if (loads != 3u || stores != 3u)
    ok = 0;
  close_fixture(&fixture);
  return ok;
}

static int test_address_taken_local(void) {
  Fixture fixture;
  if (!open_case("mir_cases", "address_taken", 1, &fixture))
    return 0;
  const AstMirFunction *function = ast_mir_function_at(fixture.mir, 0u);
  AstMirFunctionInfo info = {0};
  int ok = function && ast_mir_function_info(function, &info) &&
           info.stack_slot_count == 1u && info.promoted_local_count == 2u;
  AstMirStackSlotInfo slot;
  if (ok && (!ast_mir_function_stack_slot(function, 0u, &slot) ||
             !(slot.flags & AST_MIR_STACK_SLOT_ADDRESS_TAKEN)))
    ok = 0;
  int saw_address = 0;
  for (uint32_t ii = 0u; ok && ii < info.instruction_count; ++ii) {
    AstMirInstructionInfo instruction;
    if (!ast_mir_function_instruction(function, ii, &instruction)) {
      ok = 0;
      break;
    }
    if (instruction.op == AST_MIR_OP_STACK_ADDR) {
      AstMirVRegInfo result;
      saw_address =
          instruction.stack_slot == 0u &&
          ast_mir_function_vreg(function, instruction.result, &result) &&
          result.bank == AST_MIR_BANK_POINTER;
    }
  }
  ok &= saw_address;
  close_fixture(&fixture);
  return ok;
}

static int test_effect_chain(void) {
  Fixture fixture;
  if (!open_case("sema_cases", "atomic_success", 1, &fixture))
    return 0;
  int ok = 1;
  int saw_atomic = 0;
  for (size_t fi = 0u; ok && fi < ast_mir_function_count(fixture.mir); ++fi) {
    const AstMirFunction *function = ast_mir_function_at(fixture.mir, fi);
    AstMirFunctionInfo info = {0};
    if (!ast_mir_function_info(function, &info)) {
      ok = 0;
      break;
    }
    for (uint32_t bi = 0u; ok && bi < info.block_count; ++bi) {
      AstMirBlockInfo block;
      if (!ast_mir_function_block(function, bi, &block)) {
        ok = 0;
        break;
      }
      AstMirMemoryId current = block.memory_argument;
      for (uint32_t position = 0u; position < block.instruction_count;
           ++position) {
        AstMirInstructionInfo instruction;
        if (!ast_mir_function_instruction(
                function, block.instructions[position], &instruction)) {
          ok = 0;
          break;
        }
        if (instruction.memory_in != AST_MIR_INVALID_ID) {
          if (instruction.memory_in != current ||
              instruction.memory_out == AST_MIR_INVALID_ID)
            ok = 0;
          current = instruction.memory_out;
        }
        if (instruction.op == AST_MIR_OP_ATOMIC) {
          saw_atomic = 1;
          if (!(instruction.flags & AST_MIR_INST_ATOMIC))
            ok = 0;
        }
      }
      if (block.terminator.memory != current)
        ok = 0;
    }
  }
  ok &= saw_atomic;
  close_fixture(&fixture);
  return ok;
}

static int test_memory_phi_edges(void) {
  Fixture fixture;
  if (!open_case("mir_cases", "address_taken", 1, &fixture))
    return 0;
  const char *function_names[] = {"branch_memory", "loop_memory"};
  int ok = 1;
  for (size_t fi = 0u;
       ok && fi < sizeof(function_names) / sizeof(function_names[0]); ++fi) {
    const AstMirFunction *function = NULL;
    AstMirFunctionInfo info = {0};
    if (!ast_mir_find_function(fixture.mir, "mir.address_taken",
                               function_names[fi], &function) ||
        !ast_mir_function_info(function, &info) ||
        info.memory_phi_count == 0u) {
      ok = 0;
      break;
    }
    int saw_phi = 0;
    for (uint32_t bi = 0u; ok && bi < info.block_count; ++bi) {
      AstMirBlockInfo block;
      AstMirMemoryInfo memory;
      if (!ast_mir_function_block(function, bi, &block) ||
          !ast_mir_function_memory(function, block.memory_argument, &memory)) {
        ok = 0;
        break;
      }
      if (block.predecessor_count > 1u) {
        saw_phi = 1;
        if (memory.kind != AST_MIR_MEMORY_BLOCK_ARGUMENT ||
            memory.incoming_count != block.predecessor_count ||
            !memory.incoming) {
          ok = 0;
          break;
        }
        for (uint32_t pi = 0u; pi < block.predecessor_count; ++pi) {
          AstMirBlockId predecessor = AST_MIR_INVALID_ID;
          AstMirBlockInfo predecessor_block;
          if (!ast_mir_function_predecessor(function, bi, pi, &predecessor) ||
              !ast_mir_function_block(function, predecessor,
                                      &predecessor_block) ||
              memory.incoming[pi] != predecessor_block.terminator.memory) {
            ok = 0;
            break;
          }
        }
      }
    }
    ok &= saw_phi;
  }
  close_fixture(&fixture);
  return ok;
}

static int test_self_loop_analysis(void) {
  Fixture fixture;
  if (!open_case("sema_cases", "labeled_continue_success", 1, &fixture))
    return 0;
  const AstMirFunction *function = ast_mir_function_at(fixture.mir, 0u);
  AstMirFunctionInfo info = {0};
  int ok = function && ast_mir_function_info(function, &info);
  int saw_loop = 0;
  for (uint32_t bi = 0u; ok && bi < info.block_count; ++bi) {
    AstMirBlockInfo block;
    if (!ast_mir_function_block(function, bi, &block)) {
      ok = 0;
      break;
    }
    if (block.loop_depth > 0u)
      saw_loop = 1;
  }
  ok &= saw_loop;
  close_fixture(&fixture);
  return ok;
}

static int test_pure_unsafe_operation(void) {
  Fixture fixture;
  if (!open_case("sema_cases", "ptrcast_unsafe_success", 1, &fixture))
    return 0;
  const AstMirFunction *function = ast_mir_function_at(fixture.mir, 0u);
  AstMirFunctionInfo info = {0};
  int ok = function && ast_mir_function_info(function, &info);
  int saw_ptrcast = 0;
  for (uint32_t ii = 0u; ok && ii < info.instruction_count; ++ii) {
    AstMirInstructionInfo instruction;
    if (!ast_mir_function_instruction(function, ii, &instruction)) {
      ok = 0;
      break;
    }
    if (instruction.op == AST_MIR_OP_CAST &&
        instruction.token_op == AST_TOK_KW_PTRCAST) {
      saw_ptrcast = 1;
      if (!(instruction.flags & AST_MIR_INST_UNSAFE) ||
          instruction.memory_in != AST_MIR_INVALID_ID ||
          instruction.memory_out != AST_MIR_INVALID_ID)
        ok = 0;
    }
  }
  ok &= saw_ptrcast;
  close_fixture(&fixture);
  return ok;
}

static int test_direct_calls(void) {
  Fixture fixture;
  if (!open_case("sema_cases", "modules_success", 1, &fixture))
    return 0;
  const AstMirFunction *function = NULL;
  AstMirFunctionInfo info = {0};
  int ok = ast_mir_find_function(fixture.mir, "sema.modules_success", "go",
                                 &function) &&
           ast_mir_function_info(function, &info);
  int saw_direct = 0, saw_address = 0, saw_indirect = 0;
  for (uint32_t ii = 0u; ok && ii < info.instruction_count; ++ii) {
    AstMirInstructionInfo inst;
    if (!ast_mir_function_instruction(function, ii, &inst)) {
      ok = 0;
      break;
    }
    if (inst.op == AST_MIR_OP_CALL_DIRECT) {
      saw_direct++;
      if (!ast_name_equal_cstr(inst.module, "lib.math") ||
          !ast_name_equal_cstr(inst.symbol, "twice") ||
          inst.operand_count != 1u || !(inst.flags & AST_MIR_INST_CALL))
        ok = 0;
    } else if (inst.op == AST_MIR_OP_FUNCTION_ADDR) {
      saw_address++;
    } else if (inst.op == AST_MIR_OP_CALL_INDIRECT) {
      saw_indirect++;
    }
  }
  ok &= saw_direct == 1 && saw_address == 0 && saw_indirect == 0;
  close_fixture(&fixture);
  return ok;
}

static int test_native_conservative_boundary(void) {
  Fixture fixture;
  if (!open_case("sema_cases", "native_success", 1, &fixture))
    return 0;
  int ok = 1, saw_native = 0, saw_result = 0;
  for (size_t fi = 0u; ok && fi < ast_mir_function_count(fixture.mir); ++fi) {
    const AstMirFunction *function = ast_mir_function_at(fixture.mir, fi);
    AstMirFunctionInfo info = {0};
    if (!ast_mir_function_info(function, &info)) {
      ok = 0;
      break;
    }
    for (uint32_t ii = 0u; ok && ii < info.instruction_count; ++ii) {
      AstMirInstructionInfo inst;
      if (!ast_mir_function_instruction(function, ii, &inst)) {
        ok = 0;
        break;
      }
      if (inst.op == AST_MIR_OP_NATIVE) {
        saw_native = 1;
        uint32_t required = AST_MIR_INST_READS_MEMORY |
                            AST_MIR_INST_WRITES_MEMORY | AST_MIR_INST_UNSAFE |
                            AST_MIR_INST_CONSTRAINT |
                            AST_MIR_INST_CONSERVATIVE_EFFECTS;
        if ((inst.flags & required) != required ||
            inst.memory_in == AST_MIR_INVALID_ID ||
            inst.memory_out == AST_MIR_INVALID_ID)
          ok = 0;
      } else if (inst.op == AST_MIR_OP_NATIVE_RESULT) {
        saw_result = 1;
        if (!(inst.flags & AST_MIR_INST_CONSTRAINT) ||
            (inst.flags & AST_MIR_INST_CONSERVATIVE_EFFECTS) ||
            inst.memory_in != AST_MIR_INVALID_ID ||
            inst.memory_out != AST_MIR_INVALID_ID)
          ok = 0;
      }
    }
  }
  ok &= saw_native && saw_result;
  close_fixture(&fixture);
  return ok;
}

static int test_nested_loop_depth(void) {
  Fixture fixture;
  if (!open_case("mir_cases", "nested_continue", 1, &fixture))
    return 0;
  const AstMirFunction *function = NULL;
  AstMirFunctionInfo info = {0};
  int ok = ast_mir_find_function(fixture.mir, "mir.nested_continue", "spin",
                                 &function) &&
           ast_mir_function_info(function, &info) &&
           info.stack_slot_count == 0u && info.phi_count == 4u &&
           info.memory_phi_count == 2u;
  uint32_t depth1 = 0u, depth2 = 0u, deeper = 0u;
  for (uint32_t bi = 0u; ok && bi < info.block_count; ++bi) {
    AstMirBlockInfo block;
    if (!ast_mir_function_block(function, bi, &block)) {
      ok = 0;
      break;
    }
    if (!block.reachable)
      continue;
    if (block.loop_depth == 1u)
      depth1++;
    else if (block.loop_depth == 2u)
      depth2++;
    else if (block.loop_depth > 2u)
      deeper++;
  }
  ok &= depth1 >= 2u && depth2 >= 2u && deeper == 0u;
  close_fixture(&fixture);
  return ok;
}

int main(void) {
  struct Test {
    const char *name;
    int (*run)(void);
  } tests[] = {
      {"promoted_ssa", test_promoted_ssa},
      {"materialized_locals", test_materialized_locals},
      {"address_taken_local", test_address_taken_local},
      {"effect_chain", test_effect_chain},
      {"memory_phi_edges", test_memory_phi_edges},
      {"self_loop_analysis", test_self_loop_analysis},
      {"pure_unsafe_operation", test_pure_unsafe_operation},
      {"direct_calls", test_direct_calls},
      {"native_conservative_boundary", test_native_conservative_boundary},
      {"nested_loop_depth", test_nested_loop_depth}};
  int ok = 1;
  for (size_t i = 0u; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    int passed = tests[i].run();
    if (!passed)
      fprintf(stderr, "MIR core test failed: %s\n", tests[i].name);
    ok &= passed;
  }
  if (!ok)
    return 1;
  puts("MIR core API tests passed");
  return 0;
}
