#ifndef ASTERASM_RISCV64_MEMORY_CONTRACT_H
#define ASTERASM_RISCV64_MEMORY_CONTRACT_H
#include "riscv64_scalar_contract.h"
#include "system_contract.h"

typedef struct RvMemoryForm {
  const char *name;
  AstMachineOpcode opcode;
  uint32_t bits;
  uint8_t load;
} RvMemoryForm;
static const RvMemoryForm rv_memory_forms[] = {
    {"lb", AST_MACHINE_RISCV_LB, 8u, 1u},
    {"lh", AST_MACHINE_RISCV_LH, 16u, 1u},
    {"lw", AST_MACHINE_RISCV_LW, 32u, 1u},
    {"ld", AST_MACHINE_RISCV_LD, 64u, 1u},
    {"lbu", AST_MACHINE_RISCV_LBU, 8u, 1u},
    {"lhu", AST_MACHINE_RISCV_LHU, 16u, 1u},
    {"lwu", AST_MACHINE_RISCV_LWU, 32u, 1u},
    {"sb", AST_MACHINE_RISCV_SB, 8u, 0u},
    {"sh", AST_MACHINE_RISCV_SH, 16u, 0u},
    {"sw", AST_MACHINE_RISCV_SW, 32u, 0u},
    {"sd", AST_MACHINE_RISCV_SD, 64u, 0u}};
static inline const RvMemoryForm *rv_memory_form(AstMachineOpcode opcode) {
  for (uint32_t j = 0; j < sizeof(rv_memory_forms) / sizeof(*rv_memory_forms);
       ++j)
    if (rv_memory_forms[j].opcode == opcode)
      return &rv_memory_forms[j];
  return NULL;
}
static inline const RvMemoryForm *rv_memory_source(AstTargetArch arch,
                                                   AstName name) {
  if (arch != AST_TARGET_ARCH_RISCV64)
    return NULL;
  for (uint32_t j = 0; j < sizeof(rv_memory_forms) / sizeof(*rv_memory_forms);
       ++j)
    if (system_name(name, rv_memory_forms[j].name))
      return &rv_memory_forms[j];
  return NULL;
}
static inline int rv_memory_contract(const AstPhysicalInstructionInfo *i) {
  const RvMemoryForm *f = rv_memory_form(i->opcode);
  if (!f || i->raw_bytes || i->raw_byte_count || i->operand_count != 2u ||
      i->result_count != f->load || !rv_scalar_register(&i->operands[0]))
    return 0;
  const AstPhysicalOperand *m = &i->operands[1];
  return m->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         m->bank == AST_MACHINE_BANK_GPR && m->bit_width &&
         (m->bit_width == f->bits || (f->bits == 8u && m->bit_width < 8u)) &&
         i->operands[0].bit_width >= m->bit_width && m->register_unit >= 64u &&
         m->register_unit < 96u && !m->scale && !m->index_register_name[0] &&
         !m->index_register_unit &&
         /* Frame verification also checks reconstructed logical addresses.
            Source contracts and the final encoded stream require signed 12. */
         m->displacement >= INT32_MIN && m->displacement <= INT32_MAX;
}
#endif
