#include "machine_internal.h"
#include "regalloc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PeepholeFixture {
  AstMachineProgram program;
  MachineFunction function;
} PeepholeFixture;

static int fixture_init(PeepholeFixture *fixture, int constrained_use) {
  MachineFunction *fn;
  memset(fixture, 0, sizeof(*fixture));
  fn = &fixture->function;
  fixture->program.functions = fn;
  fixture->program.function_count = 1u;
  fn->info.vreg_count = 4u;
  fn->info.instruction_count = 4u;
  fn->info.block_count = 1u;
  fn->vregs = (AstMachineVRegInfo *)calloc(4u, sizeof(*fn->vregs));
  fn->instructions =
      (MachineInstruction *)calloc(4u, sizeof(*fn->instructions));
  fn->blocks = (MachineBlock *)calloc(1u, sizeof(*fn->blocks));
  fn->blocks[0].instructions =
      (AstMachineInstId *)calloc(4u, sizeof(AstMachineInstId));
  fn->operands = (AstMachineOperand *)calloc(6u, sizeof(*fn->operands));
  if (!fn->vregs || !fn->instructions || !fn->blocks ||
      !fn->blocks[0].instructions || !fn->operands)
    return 0;
  fn->operand_count = 6u;
  fn->operand_capacity = 6u;
  fn->blocks[0].info.id = 0u;
  fn->blocks[0].info.instruction_count = 4u;
  fn->blocks[0].info.instructions = fn->blocks[0].instructions;
  for (uint32_t i = 0u; i < 4u; ++i) {
    fn->vregs[i].id = i;
    fn->vregs[i].part_index = 0u;
    fn->vregs[i].part_count = 1u;
    fn->blocks[0].instructions[i] = i;
    fn->instructions[i].info.id = i;
    fn->instructions[i].info.result_first = AST_MACHINE_INVALID_ID;
    fn->instructions[i].info.opcode = AST_MACHINE_G_COPY;
  }
  fn->operands[0].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[0].as.vreg = 0u;
  fn->operands[1].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[1].as.vreg = 1u;
  fn->operands[2].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[2].as.vreg = 0u;
  fn->operands[3].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[3].as.vreg = 0u;
  fn->operands[4].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[4].as.vreg = 0u;
  fn->operands[5].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[5].as.vreg = 2u;
  fn->instructions[0].operand_offset = 0u;
  fn->instructions[0].info.operand_count = 1u;
  fn->instructions[0].info.result_first = 1u;
  fn->instructions[0].info.result_count = 1u;
  fn->instructions[0].info.source_op = AST_MIR_OP_COPY;
  fn->instructions[0].info.source_instruction = 0u;
  fn->instructions[1].operand_offset = 1u;
  fn->instructions[1].info.operand_count = 2u;
  fn->instructions[1].info.result_first = 2u;
  fn->instructions[1].info.result_count = 1u;
  fn->instructions[1].info.source_op = AST_MIR_OP_BINARY;
  fn->instructions[1].info.opcode = AST_MACHINE_G_BINARY;
  fn->instructions[1].info.source_instruction = 1u;
  fn->instructions[2].operand_offset = 3u;
  fn->instructions[2].info.operand_count = 2u;
  fn->instructions[2].info.result_first = 3u;
  fn->instructions[2].info.result_count = 1u;
  fn->instructions[2].info.source_op = AST_MIR_OP_BINARY;
  fn->instructions[2].info.opcode = AST_MACHINE_G_BINARY;
  fn->instructions[2].info.source_instruction = 2u;
  fn->instructions[3].operand_offset = 5u;
  fn->instructions[3].info.operand_count = 1u;
  fn->instructions[3].info.source_op = AST_MIR_OP_INVALID;
  fn->instructions[3].info.source_instruction = AST_MIR_INVALID_ID;
  fn->instructions[3].info.opcode = AST_MACHINE_G_RET;
  if (constrained_use)
    fn->operands[1].flags = AST_MACHINE_OPERAND_FIXED_REGISTER;
  machine_refresh_operand_views(fn);
  return 1;
}

static void fixture_release(PeepholeFixture *fixture) {
  MachineFunction *fn = &fixture->function;
  free(fn->vreg_alias);
  free(fn->operands);
  if (fn->blocks)
    for (uint32_t b = 0; b < fn->info.block_count; ++b)
      free(fn->blocks[b].instructions);
  free(fn->blocks);
  free(fn->instructions);
  free(fn->vregs);
}

static int propagation_and_dce(void) {
  PeepholeFixture f;
  int ok = fixture_init(&f, 0) && machine_run_peephole(&f.program);
  ok = ok && machine_canonical_vreg(&f.function, 1u) == 0u;
  ok = ok && f.function.instructions[1].info.operands[0].as.vreg == 0u;
  ok = ok && f.function.instructions[0].dead &&
       !f.function.instructions[1].dead && f.function.instructions[2].dead &&
       !f.function.instructions[3].dead;
  fixture_release(&f);
  return ok;
}

static int constrained_copy_is_preserved(void) {
  PeepholeFixture f;
  int ok = fixture_init(&f, 1) && machine_run_peephole(&f.program);
  ok = ok && machine_canonical_vreg(&f.function, 1u) == 1u;
  ok = ok && f.function.instructions[1].info.operands[0].as.vreg == 1u;
  ok = ok && !f.function.instructions[0].dead;
  fixture_release(&f);
  return ok;
}

static int dead_chain_and_liveness(void) {
  PeepholeFixture f;
  if (!fixture_init(&f, 0)) {
    fixture_release(&f);
    return 0;
  }
  /* v1 feeds v2 feeds v3, but the return reads v0. All three defs die. */
  f.function.operands[3].as.vreg = 2u;
  f.function.operands[5].as.vreg = 0u;
  int ok = machine_run_peephole(&f.program);
  for (uint32_t i = 0; i < 3u; ++i)
    ok = ok && f.function.instructions[i].dead;
  AstMachineVReg *map = f.function.vreg_alias;
  ok = ok && machine_run_peephole(&f.program) && map == f.function.vreg_alias;
  MachineLiveness l = {0};
  ok = ok && machine_liveness_build(&f.function, &l);
  for (uint32_t v = 1; ok && v < 4u; ++v)
    ok = !l.intervals[v].valid && !l.intervals[v].use_count &&
         !l.intervals[v].def_count;
  ok = ok && l.intervals[0].use_count == 1u;
  machine_liveness_destroy(&l);
  fixture_release(&f);
  return ok;
}

static int preserve_barriers(unsigned test) {
  PeepholeFixture f;
  if (!fixture_init(&f, 0)) {
    fixture_release(&f);
    return 0;
  }
  MachineInstruction *mi = &f.function.instructions[2];
  switch (test) {
  case 0:
    mi->info.effects = AST_SEMA_EFFECT_MAY_TRAP;
    break;
  case 1:
    mi->info.effects = AST_SEMA_EFFECT_READ_MEMORY;
    break;
  case 2:
    mi->info.flags = AST_MIR_INST_CONSTRAINT;
    break;
  case 3:
    mi->info.source_op = AST_MIR_OP_NATIVE_RESULT;
    break;
  case 4:
    f.function.vregs[3].flags = AST_MACHINE_VREG_ABI_DEFERRED;
    break;
  default:
    return 0;
  }
  int ok = machine_run_peephole(&f.program) && !mi->dead;
  fixture_release(&f);
  return ok;
}

static int preserve_metadata_copy(unsigned test) {
  PeepholeFixture f;
  if (!fixture_init(&f, 0)) {
    fixture_release(&f);
    return 0;
  }
  f.function.instructions[1].info.source_op = test == 0u
                                                  ? AST_MIR_OP_CALL_DIRECT
                                              : test == 1u ? AST_MIR_OP_BIND
                                                           : AST_MIR_OP_NATIVE;
  int ok = machine_run_peephole(&f.program) && f.function.vreg_alias[1] == 1u &&
           !f.function.instructions[0].dead;
  fixture_release(&f);
  return ok;
}

static int local_copy_in_cfg(unsigned variant) {
  PeepholeFixture f;
  if (!fixture_init(&f, 0))
    return 0;
  MachineBlock *blocks = calloc(2u, sizeof(*blocks));
  if (!blocks) {
    fixture_release(&f);
    return 0;
  }
  blocks[0] = f.function.blocks[0];
  free(f.function.blocks);
  f.function.blocks = blocks;
  blocks[0].info.instruction_count = 2u;
  blocks[1].instructions = calloc(2u, sizeof(AstMachineInstId));
  if (!blocks[1].instructions) {
    fixture_release(&f);
    return 0;
  }
  blocks[1].instructions[0] = 2u;
  blocks[1].instructions[1] = 3u;
  blocks[1].info.instructions = blocks[1].instructions;
  blocks[1].info.instruction_count = 2u;
  blocks[1].info.id = 1u;
  f.function.info.block_count = 2u;
  if (variant == 1u)
    f.function.operands[3].as.vreg = 1u;
  if (variant == 2u)
    blocks[0].info.loop_depth = 1u;
  int ok = machine_run_peephole(&f.program) &&
           machine_canonical_vreg(&f.function, 1u) == (variant ? 1u : 0u);
  fixture_release(&f);
  return ok;
}

int main(void) {
  for (unsigned t = 0; t < 5u; ++t)
    if (!preserve_barriers(t))
      return 1;
  for (unsigned t = 0; t < 3u; ++t)
    if (!preserve_metadata_copy(t) || !local_copy_in_cfg(t))
      return 1;
  if (!propagation_and_dce() || !constrained_copy_is_preserved() ||
      !dead_chain_and_liveness()) {
    fputs("machine peephole test failed\n", stderr);
    return 1;
  }
  return 0;
}
