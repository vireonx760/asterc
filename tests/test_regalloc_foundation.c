#include "regalloc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_phys_x86(void) {
  AstTargetConfig c = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  c.features |= AST_TARGET_FEATURE_X86_AVX;
  MachinePhysRegView a, b, v;
  return machine_physreg_view(&c, "rax", &a) &&
         machine_physreg_view(&c, "eax", &b) && a.alias_unit == b.alias_unit &&
         machine_physreg_alias(&c, "xmm0", "ymm0") &&
         machine_physreg_view(&c, "rsp", &v) && v.reserved &&
         !machine_physreg_alias(&c, "rip", "rflags");
}

static int test_phys_a64(void) {
  AstTargetConfig c = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
  MachinePhysRegView a, b, v;
  return machine_physreg_view(&c, "x0", &a) &&
         machine_physreg_view(&c, "w0", &b) && a.alias_unit == b.alias_unit &&
         machine_physreg_alias(&c, "d0", "v0") &&
         machine_physreg_view(&c, "sp", &v) && v.reserved &&
         !machine_physreg_alias(&c, "sp", "xzr");
}

static int test_phys_rv(void) {
  AstTargetConfig c = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
  c.features |= AST_TARGET_FEATURE_RV_D;
  MachinePhysRegView a, b, v;
  return machine_physreg_view(&c, "a0", &a) &&
         machine_physreg_view(&c, "x10", &b) && a.alias_unit == b.alias_unit &&
         machine_physreg_alias(&c, "fa0", "f10") &&
         machine_physreg_view(&c, "sp", &v) && v.reserved;
}

static void init_inst(MachineInstruction *m, AstMachineInstId id,
                      AstMachineOpcode op, AstMachineVReg first,
                      uint32_t results, uint32_t off, uint32_t operands) {
  memset(m, 0, sizeof(*m));
  m->info.id = id;
  m->info.opcode = op;
  m->info.result_first = first;
  m->info.result_count = results;
  m->operand_offset = off;
  m->info.operand_count = operands;
}

static int build_liveness_fixture(MachineFunction *fn) {
  memset(fn, 0, sizeof(*fn));
  fn->info.block_count = 2u;
  fn->info.instruction_count = 4u;
  fn->info.vreg_count = 2u;
  fn->blocks = (MachineBlock *)calloc(2u, sizeof(*fn->blocks));
  fn->instructions =
      (MachineInstruction *)calloc(4u, sizeof(*fn->instructions));
  fn->operands = (AstMachineOperand *)calloc(3u, sizeof(*fn->operands));
  if (!fn->blocks || !fn->instructions || !fn->operands)
    return 0;
  fn->operand_count = fn->operand_capacity = 3u;
  for (uint32_t i = 0; i < 2u; ++i) {
    fn->blocks[i].info.id = i;
    fn->blocks[i].info.instruction_count = 2u;
    fn->blocks[i].instructions =
        (AstMachineInstId *)calloc(2u, sizeof(AstMachineInstId));
    if (!fn->blocks[i].instructions)
      return 0;
  }
  fn->blocks[0].instructions[0] = 0u;
  fn->blocks[0].instructions[1] = 1u;
  fn->blocks[1].instructions[0] = 2u;
  fn->blocks[1].instructions[1] = 3u;
  init_inst(&fn->instructions[0], 0u, AST_MACHINE_X86_MOV_IMM, 0u, 1u, 0u, 0u);
  init_inst(&fn->instructions[1], 1u, AST_MACHINE_X86_BR,
            AST_MACHINE_INVALID_ID, 0u, 0u, 1u);
  fn->operands[0].kind = AST_MACHINE_OPERAND_BLOCK;
  fn->operands[0].as.block = 1u;
  init_inst(&fn->instructions[2], 2u, AST_MACHINE_X86_ADD, 1u, 1u, 1u, 1u);
  fn->operands[1].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[1].as.vreg = 0u;
  init_inst(&fn->instructions[3], 3u, AST_MACHINE_X86_RET,
            AST_MACHINE_INVALID_ID, 0u, 2u, 1u);
  fn->operands[2].kind = AST_MACHINE_OPERAND_VREG;
  fn->operands[2].as.vreg = 1u;
  for (uint32_t i = 0; i < 4u; ++i)
    fn->instructions[i].info.operands =
        fn->operands + fn->instructions[i].operand_offset;
  return 1;
}

static void free_liveness_fixture(MachineFunction *fn) {
  if (!fn)
    return;
  if (fn->blocks)
    for (uint32_t i = 0; i < fn->info.block_count; ++i)
      free(fn->blocks[i].instructions);
  free(fn->blocks);
  free(fn->instructions);
  free(fn->operands);
  memset(fn, 0, sizeof(*fn));
}

static int test_liveness_cfg(void) {
  MachineFunction fn;
  MachineLiveness l;
  int ok = 0;
  memset(&l, 0, sizeof(l));
  if (!build_liveness_fixture(&fn)) {
    free_liveness_fixture(&fn);
    return 0;
  }
  if (machine_liveness_build(&fn, &l)) {
    ok = machine_liveness_test_bit(&l.blocks[0].live_out, 0u) &&
         machine_liveness_test_bit(&l.blocks[1].live_in, 0u) &&
         l.intervals[0].valid && l.intervals[0].def_count == 1u &&
         l.intervals[0].use_count == 1u && l.intervals[1].valid &&
         l.intervals[1].def_count == 1u && l.intervals[1].use_count == 1u;
  }
  machine_liveness_destroy(&l);
  free_liveness_fixture(&fn);
  return ok;
}

static int test_liveness_mutation(void) {
  MachineFunction fn;
  MachineLiveness l;
  int ok;
  memset(&l, 0, sizeof(l));
  if (!build_liveness_fixture(&fn)) {
    free_liveness_fixture(&fn);
    return 0;
  }
  fn.operands[1].as.vreg = 99u;
  ok = !machine_liveness_build(&fn, &l);
  machine_liveness_destroy(&l);
  free_liveness_fixture(&fn);
  return ok;
}

static int run_case(int c) {
  switch (c) {
  case 0:
    return test_phys_x86();
  case 1:
    return test_phys_a64();
  case 2:
    return test_phys_rv();
  case 3:
    return test_liveness_cfg();
  case 4:
    return test_liveness_mutation();
  default:
    return 0;
  }
}

int main(int argc, char **argv) {
  char *end = NULL;
  long c;
  if (argc != 2)
    return 2;
  c = strtol(argv[1], &end, 10);
  if (!end || *end || c < 0 || c > 4)
    return 2;
  if (!run_case((int)c)) {
    fprintf(stderr, "regalloc foundation case %ld failed\n", c);
    return 1;
  }
  return 0;
}
