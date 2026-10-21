/* Included after VectorForm and the common operand helpers. */
#ifndef ASTERASM_VECTOR_OTHER_CONTRACT_H
#define ASTERASM_VECTOR_OTHER_CONTRACT_H
static inline int vector_neon_reg(const AstPhysicalOperand *r, uint32_t bits) {
  return r->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         r->bank == AST_MACHINE_BANK_VECTOR && r->bit_width == bits &&
         (bits == 64u || bits == 128u) && r->register_unit >= 128u &&
         r->register_unit < 160u;
}
static inline int vector_rvv_reg(const AstPhysicalOperand *r) {
  return r->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         r->bank == AST_MACHINE_BANK_VECTOR && !r->bit_width &&
         r->register_unit >= 192u && r->register_unit < 224u;
}
static inline int vector_rvv_mask(const AstPhysicalOperand *r) {
  return system_imm(r, 1u) ? r->immediate_bits == 1u
                           : vector_rvv_reg(r) && r->register_unit == 192u;
}
static inline int vector_vtype(const AstPhysicalOperand *o) {
  return system_imm(o, 255u) && (o->immediate_bits & 7u) != 4u &&
         ((o->immediate_bits >> 3u) & 7u) <= 3u;
}
static inline int vector_other_contract(const VectorForm *f,
                                        const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  uint32_t n = i->operand_count - 1u, d = i->result_count,
           bits = o[0].bit_width;
  if (f->arch == AST_TARGET_ARCH_AARCH64) {
    if (!vector_neon_reg(&o[0], bits))
      return 0;
    if (f->shape == VECTOR_A64_LOAD || f->shape == VECTOR_A64_STORE) {
      const AstPhysicalOperand *m = &o[1];
      return n == 2u && d == (f->shape == VECTOR_A64_LOAD ? 1u : 0u) &&
             m->kind == AST_PHYSICAL_OPERAND_MEMORY && m->bit_width == bits &&
             ((m->bank == AST_MACHINE_BANK_GPR && m->register_unit >= 64u &&
               m->register_unit < 95u) ||
              (m->bank == AST_MACHINE_BANK_SPECIAL &&
               m->register_unit == 320u &&
               memchr(m->register_name, 0, sizeof(m->register_name)) &&
               !strcmp(m->register_name, "sp"))) &&
             !m->scale && !m->index_register_unit &&
             !m->index_register_name[0] && m->displacement >= 0 &&
             (uint64_t)m->displacement % (bits / 8u) == 0u &&
             (uint64_t)m->displacement / (bits / 8u) <= 4095u;
    }
    if (n != 4u || d != 1u || !vector_neon_reg(&o[1], bits) ||
        !vector_neon_reg(&o[2], bits) || !system_imm(&o[3], 64u))
      return 0;
    uint64_t element = o[3].immediate_bits;
    if (f->shape == VECTOR_A64_LOGIC)
      return element == 8u;
    if (f->shape == VECTOR_A64_FLOAT)
      return element == 32u || (element == 64u && bits == 128u);
    return element == 8u || element == 16u || element == 32u ||
           (element == 64u && bits == 128u && f->shape == VECTOR_A64_INTEGER);
  }
  if (f->arch != AST_TARGET_ARCH_RISCV64)
    return 0;
  switch (f->shape) {
  case VECTOR_RV_CONFIG:
  case VECTOR_RV_ICONFIG:
  case VECTOR_RV_RCONFIG:
    return n == 3u && d == 1u && system_gpr(&o[0], 64u, 32u) &&
           (f->shape == VECTOR_RV_ICONFIG ? system_imm(&o[1], 31u)
                                          : system_gpr(&o[1], 64u, 32u)) &&
           (f->shape == VECTOR_RV_RCONFIG ? system_gpr(&o[2], 64u, 32u)
                                          : vector_vtype(&o[2]));
  case VECTOR_RV_LOAD:
  case VECTOR_RV_STORE:
    return n == (f->shape == VECTOR_RV_LOAD ? 4u : 3u) &&
           d == (f->shape == VECTOR_RV_LOAD ? 1u : 0u) &&
           vector_rvv_reg(&o[0]) && system_gpr(&o[1], 64u, 32u) &&
           vector_rvv_mask(&o[2]) &&
           (f->shape != VECTOR_RV_LOAD ||
            (vector_rvv_reg(&o[3]) &&
             o[3].register_unit == o[0].register_unit &&
             (o[2].kind != AST_PHYSICAL_OPERAND_REGISTER ||
              o[0].register_unit != 192u)));
  case VECTOR_RV_VV:
  case VECTOR_RV_VX:
  case VECTOR_RV_VI:
  case VECTOR_RV_UIMM:
    if (n != 5u || d != 1u || !vector_rvv_reg(&o[0]) ||
        !vector_rvv_reg(&o[1]) || !vector_rvv_mask(&o[3]) ||
        !vector_rvv_reg(&o[4]) || o[4].register_unit != o[0].register_unit ||
        (o[3].kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o[0].register_unit == 192u))
      return 0;
    if (f->shape == VECTOR_RV_VV)
      return vector_rvv_reg(&o[2]);
    if (f->shape == VECTOR_RV_VX)
      return system_gpr(&o[2], 64u, 32u);
    if (f->shape == VECTOR_RV_UIMM)
      return system_imm(&o[2], 31u);
    return o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[2].bit_width == 64u &&
           (o[2].immediate_bits <= 15u ||
            o[2].immediate_bits >= UINT64_MAX - 15u);
  default:
    return 0;
  }
}
static inline uint32_t vector_word(const VectorForm *f,
                                   const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  if (f->arch == AST_TARGET_ARCH_AARCH64) {
    uint32_t rd = o[0].register_unit - 128u;
    if (f->shape == VECTOR_A64_LOAD || f->shape == VECTOR_A64_STORE) {
      uint32_t base =
          o[0].bit_width == 128u ? UINT32_C(0x3d800000) : UINT32_C(0xfd000000);
      if (f->shape == VECTOR_A64_LOAD)
        base |= UINT32_C(0x00400000);
      return base |
             ((uint32_t)((uint64_t)o[1].displacement / (o[0].bit_width / 8u))
              << 10u) |
             ((o[1].register_unit == 320u ? 31u : o[1].register_unit - 64u)
              << 5u) |
             rd;
    }
    uint32_t element = (uint32_t)o[3].immediate_bits, size = 0u;
    if (f->shape == VECTOR_A64_FLOAT)
      size = element == 64u ? 1u : 0u;
    else if (f->shape != VECTOR_A64_LOGIC)
      size = element == 64u   ? 3u
             : element == 32u ? 2u
             : element == 16u ? 1u
                              : 0u;
    return f->encoding | (o[0].bit_width == 128u ? UINT32_C(0x40000000) : 0u) |
           (size << 22u) | ((o[2].register_unit - 128u) << 16u) |
           ((o[1].register_unit - 128u) << 5u) | rd;
  }
  uint32_t rd = o[0].register_unit % 64u;
  if (f->shape == VECTOR_RV_CONFIG || f->shape == VECTOR_RV_ICONFIG ||
      f->shape == VECTOR_RV_RCONFIG) {
    uint32_t avl = f->shape == VECTOR_RV_ICONFIG ? (uint32_t)o[1].immediate_bits
                                                 : o[1].register_unit - 64u;
    uint32_t type = f->shape == VECTOR_RV_RCONFIG
                        ? o[2].register_unit - 64u
                        : (uint32_t)o[2].immediate_bits;
    return f->encoding | (type << 20u) | (avl << 15u) | (rd << 7u);
  }
  if (f->shape == VECTOR_RV_LOAD || f->shape == VECTOR_RV_STORE)
    return f->encoding |
           (o[2].kind == AST_PHYSICAL_OPERAND_REGISTER ? 0u
                                                       : UINT32_C(0x02000000)) |
           ((o[1].register_unit - 64u) << 15u) | (rd << 7u);
  uint32_t rs1 = f->shape == VECTOR_RV_VI || f->shape == VECTOR_RV_UIMM
                     ? (uint32_t)o[2].immediate_bits & 31u
                     : o[2].register_unit % 64u;
  return f->encoding |
         (o[3].kind == AST_PHYSICAL_OPERAND_REGISTER ? 0u
                                                     : UINT32_C(0x02000000)) |
         ((o[1].register_unit - 192u) << 20u) | (rs1 << 15u) | (rd << 7u);
}
#endif
