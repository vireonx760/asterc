#ifndef ASTERASM_VECTOR_CONTRACT_H
#define ASTERASM_VECTOR_CONTRACT_H
#include "system_contract.h"
#include <limits.h>

typedef enum VectorShape {
  VECTOR_X86_BINARY,
  VECTOR_X86_VBINARY,
  VECTOR_X86_UNARY,
  VECTOR_X86_VUNARY,
  VECTOR_X86_MOVE,
  VECTOR_X86_VMOVE,
  VECTOR_X86_EBINARY,
  VECTOR_X86_EUNARY,
  VECTOR_X86_EMOVE,
  VECTOR_A64_INTEGER,
  VECTOR_A64_SMALL_INTEGER,
  VECTOR_A64_LOGIC,
  VECTOR_A64_FLOAT,
  VECTOR_A64_LOAD,
  VECTOR_A64_STORE,
  VECTOR_RV_VV,
  VECTOR_RV_VX,
  VECTOR_RV_VI,
  VECTOR_RV_UIMM,
  VECTOR_RV_CONFIG,
  VECTOR_RV_ICONFIG,
  VECTOR_RV_RCONFIG,
  VECTOR_RV_LOAD,
  VECTOR_RV_STORE
} VectorShape;
typedef enum VectorId {
#define AST_VECTOR_FORM(id, name, arch, shape, code, feature, wide) VECTOR_##id,
#include "asterasm/detail/vector_instructions.inc"
#undef AST_VECTOR_FORM
  VECTOR_COUNT
} VectorId;
typedef struct VectorForm {
  const char *name;
  AstTargetArch arch;
  VectorShape shape;
  uint32_t encoding;
  uint64_t feature, wide_feature;
} VectorForm;
static const VectorForm vector_forms[VECTOR_COUNT] = {
#define AST_VECTOR_FORM(id, name, arch, shape, code, feature, wide)            \
  {name, AST_TARGET_ARCH_##arch, VECTOR_##shape, code, feature, wide},
#include "asterasm/detail/vector_instructions.inc"
#undef AST_VECTOR_FORM
};
static inline int vector_vex(const VectorForm *f) {
  return f->shape == VECTOR_X86_VBINARY || f->shape == VECTOR_X86_VUNARY ||
         f->shape == VECTOR_X86_VMOVE;
}
static inline int vector_evex(const VectorForm *f) {
  return f->shape == VECTOR_X86_EBINARY || f->shape == VECTOR_X86_EUNARY ||
         f->shape == VECTOR_X86_EMOVE;
}
static inline const VectorForm *vector_source_form(AstTargetArch arch,
                                                   AstName mnemonic) {
  for (uint32_t j = 0; j < VECTOR_COUNT; ++j)
    if (vector_forms[j].arch == arch &&
        system_name(mnemonic, vector_forms[j].name))
      return &vector_forms[j];
  return NULL;
}
static inline const VectorForm *
vector_statement_form(AstTargetArch arch, const AstHirNativeStmtInfo *s) {
  const VectorForm *first = vector_source_form(arch, s->mnemonic);
  if (!first)
    return NULL;
  if (arch == AST_TARGET_ARCH_AARCH64) {
    const AstHirNativeOperandInfo *r =
        system_role(s, first->shape == VECTOR_A64_STORE ? "src" : "dst");
    AstHirNativeExprInfo e;
    if (!r || r->is_memory || !ast_hir_native_expr_info(r->value, &e) ||
        e.kind != AST_HIR_NATIVE_EXPR_NAME ||
        e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !e.name.text ||
        e.name.span.length < 2u)
      return NULL;
    int prefix = tolower((unsigned char)e.name.text[0]);
    return prefix == 'v' || prefix == 'q' || prefix == 'd' ? first : NULL;
  }
  if (arch != AST_TARGET_ARCH_X86_64)
    return first;
  int evex = system_role(s, "mask") != NULL || system_role(s, "zero") != NULL;
  for (uint32_t j = 0; j < s->operand_count; ++j)
    if (!s->operands[j].is_memory) {
      AstHirNativeExprInfo e;
      if (!ast_hir_native_expr_info(s->operands[j].value, &e) ||
          e.kind != AST_HIR_NATIVE_EXPR_NAME ||
          e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET ||
          e.name.span.length < 4u)
        continue;
      if (tolower((unsigned char)e.name.text[0]) == 'z')
        evex = 1;
      if ((tolower((unsigned char)e.name.text[0]) == 'x' ||
           tolower((unsigned char)e.name.text[0]) == 'y') &&
          e.name.span.length == 5u &&
          (e.name.text[3] > '1' ||
           (e.name.text[3] == '1' && e.name.text[4] >= '6')))
        evex = 1;
    }
  if (!evex || vector_evex(first))
    return first;
  for (uint32_t j = 0; j < VECTOR_COUNT; ++j)
    if (vector_forms[j].arch == arch && vector_evex(&vector_forms[j]) &&
        system_name(s->mnemonic, vector_forms[j].name))
      return &vector_forms[j];
  return NULL;
}
static inline AstMachineOpcode vector_opcode(AstTargetArch arch) {
  return arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_VECTOR
         : arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_VECTOR
                                           : AST_MACHINE_RISCV_VECTOR;
}
static inline const VectorForm *
vector_physical_form(const AstPhysicalInstructionInfo *i) {
  if (!i || !i->operand_count || i->operand_count > AST_PHYSICAL_MAX_OPERANDS)
    return NULL;
  const AstPhysicalOperand *tag = &i->operands[i->operand_count - 1u];
  if (tag->kind != AST_PHYSICAL_OPERAND_IMMEDIATE || tag->bit_width != 16u ||
      tag->immediate_bits >= VECTOR_COUNT)
    return NULL;
  const VectorForm *f = &vector_forms[tag->immediate_bits];
  return i->opcode == vector_opcode(f->arch) ? f : NULL;
}
static inline int vector_register(const AstPhysicalOperand *o, uint32_t bits) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_VECTOR && o->bit_width == bits &&
         o->register_unit >= 128u && o->register_unit < 144u;
}
static inline int vector_evex_register(const AstPhysicalOperand *o,
                                       uint32_t bits) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_VECTOR && o->bit_width == bits &&
         o->register_unit >= 128u && o->register_unit < 160u;
}
static inline int vector_memory(const AstPhysicalOperand *o, uint32_t bits) {
  if (o->kind != AST_PHYSICAL_OPERAND_MEMORY ||
      o->bank != AST_MACHINE_BANK_GPR || o->bit_width != bits ||
      o->register_unit < 64u || o->register_unit >= 80u ||
      o->displacement < INT32_MIN || o->displacement > INT32_MAX)
    return 0;
  if (!o->scale)
    return !o->index_register_unit && !o->index_register_name[0];
  return (o->scale == 1u || o->scale == 2u || o->scale == 4u ||
          o->scale == 8u) &&
         o->index_register_unit >= 64u && o->index_register_unit < 80u &&
         o->index_register_unit != 68u;
}
static inline int vector_source(const AstPhysicalOperand *o, uint32_t bits) {
  return vector_register(o, bits) || vector_memory(o, bits);
}
#include "vector_other_contract.h"
static inline int
vector_physical_contract(const AstPhysicalInstructionInfo *i) {
  const VectorForm *f = vector_physical_form(i);
  if (!f || i->raw_bytes || i->raw_byte_count || i->nontemporal)
    return 0;
  if (f->arch != AST_TARGET_ARCH_X86_64)
    return vector_other_contract(f, i);
  const AstPhysicalOperand *o = i->operands;
  uint32_t bits = o[0].bit_width, n = i->operand_count - 1u;
  if (vector_evex(f)) {
    uint32_t count = f->shape == VECTOR_X86_EBINARY ? 3u : 2u;
    if (bits != 128u && bits != 256u && bits != 512u)
      return 0;
    if (n < count + 2u || !vector_evex_register(&o[0], bits))
      return 0;
    int masked = o[count].kind == AST_PHYSICAL_OPERAND_REGISTER;
    if (masked ? !(o[count].bank == AST_MACHINE_BANK_MASK &&
                   o[count].bit_width == 64u && o[count].register_unit > 256u &&
                   o[count].register_unit < 264u)
               : !system_imm(&o[count], 0u))
      return 0;
    if (!system_imm(&o[count + 1u], 1u) ||
        (o[count + 1u].immediate_bits && (!masked || !i->result_count)))
      return 0;
    int merge = masked && !o[count + 1u].immediate_bits && i->result_count;
    if (n != count + 2u + (uint32_t)merge ||
        (merge && (!vector_evex_register(&o[count + 2u], bits) ||
                   o[count + 2u].register_unit != o[0].register_unit)))
      return 0;
    const AstPhysicalOperand *src = &o[count - 1u];
    if (f->shape == VECTOR_X86_EMOVE && !i->result_count)
      return vector_memory(src, bits);
    return i->result_count == 1u &&
           (vector_evex_register(src, bits) || vector_memory(src, bits)) &&
           (f->shape != VECTOR_X86_EBINARY ||
            vector_evex_register(&o[1], bits));
  }
  if (bits != 128u && !(bits == 256u && vector_vex(f) && f->wide_feature))
    return 0;
  switch (f->shape) {
  case VECTOR_X86_BINARY:
    return n == 3u && i->result_count == 1u && vector_register(&o[0], bits) &&
           vector_register(&o[1], bits) &&
           o[0].register_unit == o[1].register_unit &&
           vector_source(&o[2], bits);
  case VECTOR_X86_VBINARY:
    return n == 3u && i->result_count == 1u && vector_register(&o[0], bits) &&
           vector_register(&o[1], bits) && vector_source(&o[2], bits);
  case VECTOR_X86_UNARY:
  case VECTOR_X86_VUNARY:
    return n == 2u && i->result_count == 1u && vector_register(&o[0], bits) &&
           vector_source(&o[1], bits);
  case VECTOR_X86_MOVE:
  case VECTOR_X86_VMOVE:
    return n == 2u && ((i->result_count == 1u && vector_register(&o[0], bits) &&
                        vector_source(&o[1], bits)) ||
                       (!i->result_count && vector_register(&o[0], bits) &&
                        vector_memory(&o[1], bits)));
  default:
    break;
  }
  return 0;
}
static inline int vector_features(const AstTargetConfig *target,
                                  const AstPhysicalInstructionInfo *i) {
  const VectorForm *f = vector_physical_form(i);
  if (!f)
    return 0;
  uint64_t required =
      f->feature |
      (vector_evex(f)
           ? (i->operands[0].bit_width < 512u ? f->wide_feature : 0u)
           : (i->operands[0].bit_width == 256u ? f->wide_feature : 0u));
  return !(required & ~target->features);
}
#endif
