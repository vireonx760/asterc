#ifndef ASTERASM_RISCV64_SCALAR_CONTRACT_H
#define ASTERASM_RISCV64_SCALAR_CONTRACT_H
#include "asterasm/regalloc.h"
#include <ctype.h>
#include <string.h>

typedef enum RvScalarShape {
  RV_SCALAR_R,
  RV_SCALAR_I,
  RV_SCALAR_S64,
  RV_SCALAR_S32,
  RV_SCALAR_U,
  RV_SCALAR_N
} RvScalarShape;
typedef enum RvScalarId {
#define AST_RV_SCALAR(op, name, shape, word, feature) RV_SCALAR_ID_##op,
#include "asterasm/detail/riscv64_scalar_instructions.inc"
#undef AST_RV_SCALAR
  RV_SCALAR_COUNT
} RvScalarId;
typedef struct RvScalarForm {
  const char *name;
  AstMachineOpcode opcode;
  RvScalarShape shape;
  uint32_t encoding;
  uint64_t feature;
} RvScalarForm;
static const RvScalarForm rv_scalar_forms[RV_SCALAR_COUNT] = {
#define AST_RV_SCALAR(op, name, shape, word, feature)                          \
  {name, AST_MACHINE_RISCV_##op, RV_SCALAR_##shape, word, feature},
#include "asterasm/detail/riscv64_scalar_instructions.inc"
#undef AST_RV_SCALAR
};
static inline const RvScalarForm *rv_scalar_form(AstMachineOpcode opcode) {
  switch (opcode) {
#define AST_RV_SCALAR(op, name, shape, word, feature)                          \
  case AST_MACHINE_RISCV_##op:                                                 \
    return &rv_scalar_forms[RV_SCALAR_ID_##op];
#include "asterasm/detail/riscv64_scalar_instructions.inc"
#undef AST_RV_SCALAR
  default:
    return NULL;
  }
}
static inline const RvScalarForm *rv_scalar_source(AstTargetArch arch,
                                                   AstName name) {
  if (arch != AST_TARGET_ARCH_RISCV64 || !name.text)
    return NULL;
  for (uint32_t j = 0; j < RV_SCALAR_COUNT; ++j) {
    const char *s = rv_scalar_forms[j].name;
    if (strlen(s) != name.span.length)
      continue;
    uint32_t n = 0u;
    while (n < name.span.length &&
           (unsigned char)s[n] ==
               (unsigned char)tolower((unsigned char)name.text[n]))
      ++n;
    if (n == name.span.length)
      return &rv_scalar_forms[j];
  }
  return NULL;
}
static inline int rv_scalar_register(const AstPhysicalOperand *o) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_GPR && o->register_unit >= 64u &&
         o->register_unit < 96u && o->bit_width && o->bit_width <= 64u;
}
static inline int rv_scalar_contract(const AstPhysicalInstructionInfo *i) {
  const RvScalarForm *f = rv_scalar_form(i->opcode);
  if (!f || i->raw_bytes || i->raw_byte_count || i->result_count != 1u ||
      i->operand_count !=
          (f->shape == RV_SCALAR_U || f->shape == RV_SCALAR_N ? 2u : 3u) ||
      !rv_scalar_register(&i->operands[0]))
    return 0;
  if (f->shape != RV_SCALAR_U && !rv_scalar_register(&i->operands[1]))
    return 0;
  if (f->shape == RV_SCALAR_N)
    return 1;
  const AstPhysicalOperand *last = &i->operands[i->operand_count - 1u];
  if (f->shape == RV_SCALAR_R)
    return rv_scalar_register(last);
  if (last->kind != AST_PHYSICAL_OPERAND_IMMEDIATE || !last->bit_width ||
      last->bit_width > 64u)
    return 0;
  uint64_t v = last->immediate_bits;
  if (f->shape == RV_SCALAR_I)
    return v <= 2047u || v >= UINT64_C(0xfffffffffffff800);
  return v < (f->shape == RV_SCALAR_S32   ? 32u
              : f->shape == RV_SCALAR_S64 ? 64u
                                          : UINT64_C(0x100000));
}
static inline uint32_t rv_scalar_word(const RvScalarForm *f,
                                      const AstPhysicalInstructionInfo *i) {
  uint32_t result = f->encoding | ((i->operands[0].register_unit - 64u) << 7u);
  if (f->shape == RV_SCALAR_U)
    return result | ((uint32_t)i->operands[1].immediate_bits << 12u);
  result |= (i->operands[1].register_unit - 64u) << 15u;
  if (f->shape == RV_SCALAR_N)
    return result;
  return result |
         (f->shape == RV_SCALAR_R
              ? (i->operands[2].register_unit - 64u) << 20u
              : ((uint32_t)i->operands[2].immediate_bits & 4095u) << 20u);
}
static inline const RvScalarForm *rv_scalar_decode(uint32_t word) {
  for (uint32_t j = 0; j < RV_SCALAR_COUNT; ++j) {
    const RvScalarForm *f = &rv_scalar_forms[j];
    uint32_t mask = f->shape == RV_SCALAR_N ? UINT32_C(0xfff0707f)
                    : f->shape == RV_SCALAR_R || f->shape == RV_SCALAR_S32
                        ? UINT32_C(0xfe00707f)
                    : f->shape == RV_SCALAR_S64 ? UINT32_C(0xfc00707f)
                    : f->shape == RV_SCALAR_I   ? UINT32_C(0x707f)
                                                : UINT32_C(0x7f);
    if ((word & mask) == f->encoding)
      return f;
  }
  return NULL;
}
#endif
