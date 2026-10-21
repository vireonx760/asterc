#ifndef ASTERASM_ATOMIC_CONTRACT_H
#define ASTERASM_ATOMIC_CONTRACT_H
#include "asterasm/regalloc.h"
enum {
  AT_LOAD,
  AT_STORE,
  AT_XCHG,
  AT_ADD,
  AT_SUB,
  AT_AND,
  AT_OR,
  AT_XOR,
  AT_CAS
};
static inline int
atomic_physical_contract(const AstPhysicalInstructionInfo *i) {
  unsigned n = i->opcode == AST_MACHINE_X86_LOCKED_ATOMIC      ? 5u
               : i->opcode == AST_MACHINE_A64_EXCLUSIVE_ATOMIC ? 6u
               : i->opcode == AST_MACHINE_RISCV_LRSC_ATOMIC    ? 10u
                                                               : 0u;
  if (!n || i->result_count != n || i->operand_count != n + 1u ||
      i->operands[n].kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
    return 0;
  uint64_t code = i->operands[n].immediate_bits;
  unsigned op = (unsigned)(code & 255u), bits = (unsigned)(code >> 8u);
  if (code >> 16u || op > AT_CAS ||
      (bits != 1u && bits != 8u && bits != 16u && bits != 32u && bits != 64u))
    return 0;
  for (unsigned a = 0; a < n; ++a) {
    if (i->operands[a].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        i->operands[a].bank != AST_MACHINE_BANK_GPR ||
        i->operands[a].bit_width != 64u)
      return 0;
    for (unsigned b = 0; b < a; ++b)
      if (i->operands[a].register_unit == i->operands[b].register_unit)
        return 0;
  }
  if (n == 5u && i->operands[0].register_unit % 64u != 0u)
    return 0; /* CMPXCHG accumulator */
  AstMemoryOrder s = i->memory_order, f = i->failure_memory_order;
  if (s < AST_MEMORY_ORDER_RELAXED || s > AST_MEMORY_ORDER_SEQ_CST)
    return 0;
  if (op == AT_LOAD && s != AST_MEMORY_ORDER_RELAXED &&
      s != AST_MEMORY_ORDER_ACQUIRE && s != AST_MEMORY_ORDER_SEQ_CST)
    return 0;
  if (op == AT_STORE && s != AST_MEMORY_ORDER_RELAXED &&
      s != AST_MEMORY_ORDER_RELEASE && s != AST_MEMORY_ORDER_SEQ_CST)
    return 0;
  if (op != AT_CAS)
    return f == AST_MEMORY_ORDER_NONE;
  return f == AST_MEMORY_ORDER_RELAXED ||
         (f == AST_MEMORY_ORDER_ACQUIRE &&
          (s == AST_MEMORY_ORDER_ACQUIRE || s == AST_MEMORY_ORDER_ACQ_REL ||
           s == AST_MEMORY_ORDER_SEQ_CST)) ||
         (f == AST_MEMORY_ORDER_SEQ_CST && s == AST_MEMORY_ORDER_SEQ_CST);
}
#endif
