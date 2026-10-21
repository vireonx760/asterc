#include "physical_internal.h"
#include <stdio.h>

typedef struct Fixture {
  AstRegAllocProgram program;
  AllocationFunction function;
  MachineFunction machine;
  PhysicalContext context;
  PhysicalBlock block;
  AstFrameObjectInfo objects[2];
  AstPhysicalInstructionInfo instructions[12];
} Fixture;
static void init(Fixture *f) {
  memset(f, 0, sizeof(*f));
  f->program.target = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  f->context.program = &f->program;
  f->context.fn = &f->function;
  f->context.machine = &f->machine;
  f->function.blocks = &f->block;
  f->function.block_count = 1u;
  f->function.objects = f->objects;
  f->function.object_count = 2u;
  f->objects[0].kind = AST_FRAME_OBJECT_SPILL;
  f->objects[1].kind = AST_FRAME_OBJECT_LOCAL;
  f->block.instructions = f->instructions;
  f->block.capacity = 12u;
}
static AstPhysicalOperand reg(Fixture *f, const char *name, uint32_t bits) {
  return physical_named_register(&f->context, name, bits);
}
static void emit(Fixture *f, AstMachineOpcode op, AstPhysicalOperand a,
                 AstPhysicalOperand b) {
  AstPhysicalInstructionInfo *i = &f->instructions[f->block.count++];
  i->opcode = op;
  i->operand_count = 2u;
  i->result_count = op == AST_MACHINE_X86_STORE ||
                            op == AST_MACHINE_A64_STORE ||
                            op == AST_MACHINE_RISCV_SD
                        ? 0u
                        : 1u;
  i->operands[0] = a;
  i->operands[1] = b;
}
static int forwarding(unsigned variant) {
  Fixture f;
  init(&f);
  AstPhysicalOperand a = reg(&f, "rdi", 64), b = reg(&f, "rcx", 64);
  AstPhysicalOperand slot = physical_frame(variant == 1u ? 1u : 0u, 64u, 0);
  emit(&f, AST_MACHINE_X86_STORE, a, slot);
  if (variant == 2u) {
    AstPhysicalInstructionInfo *i = &f.instructions[f.block.count++];
    i->opcode = AST_MACHINE_X86_CALL;
  }
  if (variant == 3u)
    emit(&f, AST_MACHINE_X86_MOV_IMM, reg(&f, "edi", 32),
         physical_immediate(99, 32));
  if (variant == 4u) {
    slot.bit_width = 32u;
    b = reg(&f, "ecx", 32);
  }
  emit(&f, AST_MACHINE_X86_LOAD, b, slot);
  emit(&f, AST_MACHINE_X86_STORE, b, slot);
  if (!physical_run_peephole(&f.context))
    return 0;
  if (!variant)
    return f.block.count == 2u &&
           f.instructions[0].opcode == AST_MACHINE_X86_MOV &&
           !strcmp(f.instructions[0].operands[1].register_name, "rdi") &&
           f.instructions[1].opcode == AST_MACHINE_X86_STORE;
  uint32_t load = variant == 2u || variant == 3u ? 2u : 1u;
  return f.instructions[load].opcode == AST_MACHINE_X86_LOAD;
}
static int aliases(void) {
  Fixture f;
  init(&f);
  emit(&f, AST_MACHINE_X86_MOV, reg(&f, "ax", 16), reg(&f, "ax", 16));
  emit(&f, AST_MACHINE_X86_MOV, reg(&f, "eax", 32), reg(&f, "eax", 32));
  emit(&f, AST_MACHINE_X86_MOV, reg(&f, "al", 8), reg(&f, "ah", 8));
  if (!physical_run_peephole(&f.context) || f.block.count != 2u)
    return 0;
  return f.instructions[0].operands[0].bit_width == 32u &&
         !strcmp(f.instructions[1].operands[1].register_name, "ah");
}
static int chain_and_kill(void) {
  Fixture f;
  init(&f);
  emit(&f, AST_MACHINE_X86_MOV, reg(&f, "r11", 64), reg(&f, "rax", 64));
  emit(&f, AST_MACHINE_X86_MOV, reg(&f, "rdi", 64), reg(&f, "r11", 64));
  emit(&f, AST_MACHINE_X86_MOV_IMM, reg(&f, "r11", 64),
       physical_immediate(42, 64));
  return physical_run_peephole(&f.context) && f.block.count == 2u &&
         !strcmp(f.instructions[0].operands[1].register_name, "rax") &&
         !strcmp(f.instructions[0].operands[0].register_name, "rdi");
}
static int high_byte_write(void) {
  Fixture f;
  init(&f);
  emit(&f, AST_MACHINE_X86_MOV, reg(&f, "rdi", 64), reg(&f, "rax", 64));
  emit(&f, AST_MACHINE_X86_MOV_IMM, reg(&f, "ah", 8), physical_immediate(1, 8));
  emit(&f, AST_MACHINE_X86_MOV, reg(&f, "rcx", 64), reg(&f, "rdi", 64));
  return physical_run_peephole(&f.context) && f.block.count == 3u &&
         !strcmp(f.instructions[2].operands[1].register_name, "rdi");
}
static int diamond(unsigned variant) {
  Fixture f;
  init(&f);
  AstPhysicalInstructionInfo yes[2] = {0}, no[2] = {0}, join = {0};
  PhysicalBlock blocks[4] = {
      {f.instructions, 1u, 12u}, {yes, 2u, 2u}, {no, 2u, 2u}, {&join, 1u, 1u}};
  f.function.blocks = blocks;
  f.function.block_count = 4u;
  f.instructions[0].opcode = AST_MACHINE_X86_COND_BR;
  f.instructions[0].operand_count = 3u;
  f.instructions[0].operands[0] = reg(&f, "al", 8);
  for (unsigned a = 0; a < 2u; ++a) {
    f.instructions[0].operands[a + 1u].kind = AST_PHYSICAL_OPERAND_BLOCK;
    f.instructions[0].operands[a + 1u].block = a + 1u;
    AstPhysicalInstructionInfo *arm = a ? no : yes;
    arm[0].opcode = AST_MACHINE_X86_MOV;
    arm[0].result_count = 1u;
    arm[0].operand_count = 2u;
    arm[0].operands[0] = reg(&f, "rax", 64);
    arm[0].operands[1] = reg(&f, a ? "rdx" : "rcx", 64);
    arm[1].opcode = AST_MACHINE_X86_BR;
    arm[1].operand_count = 1u;
    arm[1].operands[0].kind = AST_PHYSICAL_OPERAND_BLOCK;
    arm[1].operands[0].block = 3u;
  }
  join.opcode = AST_MACHINE_X86_RET;
  if (variant == 1u) {
    yes[0].opcode = AST_MACHINE_X86_LOAD;
    yes[0].operands[1] = physical_frame(0u, 64u, 0);
  }
  if (variant == 2u) {
    join.opcode = AST_MACHINE_X86_BR;
    join.operand_count = 1u;
    join.operands[0].kind = AST_PHYSICAL_OPERAND_BLOCK;
    join.operands[0].block = 1u;
  }
  return physical_run_peephole(&f.context) &&
         f.instructions[0].opcode ==
             (variant ? AST_MACHINE_X86_COND_BR : AST_MACHINE_X86_SELECT) &&
         blocks[0].count == (variant ? 1u : 2u);
}
static int portable_spills(AstTargetArch arch) {
  Fixture f;
  init(&f);
  f.program.target = ast_target_config_default(arch);
  AstPhysicalOperand a =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 29u, 64u);
  AstPhysicalOperand b =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 30u, 64u);
  AstPhysicalOperand slot = physical_frame(0u, 64u, 0);
  AstMachineOpcode store = arch == AST_TARGET_ARCH_AARCH64
                               ? AST_MACHINE_A64_STORE
                               : AST_MACHINE_RISCV_SD;
  AstMachineOpcode load = arch == AST_TARGET_ARCH_AARCH64
                              ? AST_MACHINE_A64_LOAD
                              : AST_MACHINE_RISCV_LD;
  AstMachineOpcode copy = arch == AST_TARGET_ARCH_AARCH64
                              ? AST_MACHINE_A64_MOV
                              : AST_MACHINE_RISCV_MV;
  emit(&f, store, a, slot);
  emit(&f, load, b, slot);
  emit(&f, store, b, slot);
  return physical_run_peephole(&f.context) && f.block.count == 2u &&
         f.instructions[0].opcode == copy &&
         f.instructions[0].operands[1].register_unit == a.register_unit;
}
static int portable_self_move(AstTargetArch arch) {
  Fixture f;
  init(&f);
  f.program.target = ast_target_config_default(arch);
  AstPhysicalOperand a =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 10u, 32u);
  emit(&f,
       arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_MOV
                                       : AST_MACHINE_RISCV_MV,
       a, a);
  return physical_run_peephole(&f.context) &&
         f.block.count == (arch == AST_TARGET_ARCH_AARCH64 ? 1u : 0u);
}
static int rv_select_alias(unsigned variant) {
  Fixture f;
  init(&f);
  f.program.target = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
  MachineInstruction source = {0};
  uint32_t slot = 0u, offsets[MACHINE_REGISTER_UNITS + 1u] = {0};
  RegAllocUnitRange dummy = {0};
  f.machine.instructions = &source;
  f.machine.info.instruction_count = 1u;
  f.function.liveness.instruction_slots = &slot;
  f.function.unit_range_offsets = offsets;
  f.function.unit_range_storage = &dummy;
  AstPhysicalOperand yes =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 10u, 64u);
  AstPhysicalOperand no =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 11u, 64u);
  AstPhysicalOperand condition =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 12u, 64u);
  AstPhysicalOperand dst = variant ? no : yes;
  int handled = 0;
  if (!physical_rv_select(&f.context, dst, condition, yes, no, &handled) ||
      !handled || f.block.count != 5u)
    return 0;
  return f.instructions[0].opcode ==
             (variant ? AST_MACHINE_RISCV_SLTIU : AST_MACHINE_RISCV_SLTU) &&
         f.instructions[4].operands[1].register_unit ==
             (variant ? yes.register_unit : no.register_unit);
}
static int portable_diamond(AstTargetArch arch, int extra_predecessor) {
  Fixture f;
  init(&f);
  f.program.target = ast_target_config_default(arch);
  AstPhysicalInstructionInfo yes[2] = {0}, no[2] = {0}, join = {0};
  PhysicalBlock blocks[4] = {
      {f.instructions, 2u, 12u}, {yes, 2u, 2u}, {no, 2u, 2u}, {&join, 1u, 1u}};
  f.function.blocks = blocks;
  f.function.block_count = 4u;
  MachineInstruction source = {0};
  uint32_t slot = 0u, offsets[MACHINE_REGISTER_UNITS + 1u] = {0};
  RegAllocUnitRange dummy = {0};
  f.machine.instructions = &source;
  f.machine.info.instruction_count = 1u;
  f.function.liveness.instruction_slots = &slot;
  f.function.unit_range_offsets = offsets;
  f.function.unit_range_storage = &dummy;
  AstPhysicalOperand condition =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 12u, 64u);
  if (arch == AST_TARGET_ARCH_AARCH64) {
    f.instructions[0].opcode = AST_MACHINE_A64_CMP_ZERO;
    f.instructions[0].operand_count = 1u;
    f.instructions[0].operands[0] = condition;
    f.instructions[1].opcode = AST_MACHINE_A64_COND_BR;
    f.instructions[1].operand_count = 2u;
    for (unsigned edge = 0; edge < 2u; ++edge) {
      f.instructions[1].operands[edge].kind = AST_PHYSICAL_OPERAND_BLOCK;
      f.instructions[1].operands[edge].block = edge + 1u;
    }
  } else {
    f.instructions[0].opcode = AST_MACHINE_RISCV_BNE;
    f.instructions[0].operand_count = 3u;
    f.instructions[0].operands[0] = condition;
    f.instructions[0].operands[1] =
        physical_register(&f.context, AST_MACHINE_BANK_GPR, 0u, 64u);
    f.instructions[0].operands[2].kind = AST_PHYSICAL_OPERAND_BLOCK;
    f.instructions[0].operands[2].block = 1u;
    f.instructions[1].opcode = AST_MACHINE_RISCV_J;
    f.instructions[1].operand_count = 1u;
    f.instructions[1].operands[0].kind = AST_PHYSICAL_OPERAND_BLOCK;
    f.instructions[1].operands[0].block = 2u;
  }
  for (unsigned edge = 0; edge < 2u; ++edge) {
    AstPhysicalInstructionInfo *arm = edge ? no : yes;
    arm[0].opcode = arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_MOV
                                                    : AST_MACHINE_RISCV_MV;
    arm[0].result_count = 1u;
    arm[0].operand_count = 2u;
    arm[0].operands[0] =
        physical_register(&f.context, AST_MACHINE_BANK_GPR, 10u, 64u);
    arm[0].operands[1] = physical_register(&f.context, AST_MACHINE_BANK_GPR,
                                           edge ? 11u : 13u, 64u);
    arm[1].opcode = arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_BR
                                                    : AST_MACHINE_RISCV_J;
    arm[1].operand_count = 1u;
    arm[1].operands[0].kind = AST_PHYSICAL_OPERAND_BLOCK;
    arm[1].operands[0].block = 3u;
  }
  join.opcode = arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_RET
                                                : AST_MACHINE_RISCV_RET;
  if (extra_predecessor) {
    join = yes[1];
    join.operands[0].block = 1u;
  }
  if (!physical_run_peephole(&f.context))
    return 0;
  if (extra_predecessor)
    return blocks[0].count == 2u;
  return arch == AST_TARGET_ARCH_AARCH64
             ? blocks[0].count == 3u &&
                   f.instructions[1].opcode == AST_MACHINE_A64_CSEL
             : blocks[0].count == 6u &&
                   f.instructions[0].opcode == AST_MACHINE_RISCV_SLTU;
}
static int rv_zero_register(void) {
  Fixture f;
  init(&f);
  f.program.target = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
  AstPhysicalOperand zero =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 0u, 64u);
  AstPhysicalOperand a =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 10u, 64u);
  AstPhysicalOperand b =
      physical_register(&f.context, AST_MACHINE_BANK_GPR, 11u, 64u);
  emit(&f, AST_MACHINE_RISCV_MV, zero, a);
  emit(&f, AST_MACHINE_RISCV_MV, b, zero);
  return physical_run_peephole(&f.context) && f.block.count == 2u &&
         f.instructions[1].operands[1].register_unit == zero.register_unit;
}
int main(void) {
  for (unsigned v = 0; v < 5u; ++v)
    if (!forwarding(v)) {
      fprintf(stderr, "spill forwarding case %u\n", v);
      return 1;
    }
  if (!aliases() || !chain_and_kill() || !high_byte_write())
    return 1;
  for (unsigned v = 0; v < 3u; ++v)
    if (!diamond(v))
      return 1;
  for (unsigned arch = AST_TARGET_ARCH_AARCH64; arch <= AST_TARGET_ARCH_RISCV64;
       ++arch)
    if (!portable_spills((AstTargetArch)arch) ||
        !portable_self_move((AstTargetArch)arch) ||
        !portable_diamond((AstTargetArch)arch, 0) ||
        !portable_diamond((AstTargetArch)arch, 1))
      return 1;
  if (!rv_select_alias(0u) || !rv_select_alias(1u) || !rv_zero_register())
    return 1;
  return 0;
}
