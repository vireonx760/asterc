#ifndef ASTERASM_X86_SYSTEM_CONTRACT_H
#define ASTERASM_X86_SYSTEM_CONTRACT_H
#include "asterasm/regalloc.h"
static inline int x86_system_gpr(const AstPhysicalOperand *o, unsigned index,
                                 unsigned width) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_GPR && o->register_unit == 64u + index &&
         o->bit_width == width;
}
static inline int x86_system_contract(const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  if (i->opcode == AST_MACHINE_X86_IN || i->opcode == AST_MACHINE_X86_OUT) {
    unsigned bits = o[0].bit_width;
    return i->operand_count == 2u &&
           i->result_count == (i->opcode == AST_MACHINE_X86_IN ? 1u : 0u) &&
           (bits == 8u || bits == 16u || bits == 32u) &&
           x86_system_gpr(&o[0], 0u, bits) &&
           (x86_system_gpr(&o[1], 2u, 16u) ||
            (o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
             o[1].bit_width == 8u && o[1].immediate_bits <= 255u));
  }
  if (i->opcode == AST_MACHINE_X86_LIDT || i->opcode == AST_MACHINE_X86_LGDT)
    return i->operand_count == 1u && i->result_count == 0u &&
           o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u &&
           o[0].register_unit >= 64u && o[0].register_unit < 80u;
  return 0;
}
#endif
