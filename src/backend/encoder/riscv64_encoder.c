#include "../../isa/riscv64_branch_contract.h"
#include "../../isa/riscv64_scalar_contract.h"
#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "../postra/atomic_contract.h"
#include "encoder_internal.h"

static int word(EncoderBuilder *b, uint32_t w) { return encoder_le(b, w, 4u); }
static int gpr(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r) {
  AstTargetRegisterInfo v;
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      o->bank != AST_MACHINE_BANK_GPR || !o->bit_width || o->bit_width > 64u ||
      !encoder_reg(b, o, &v) || v.reg_class != AST_TARGET_REGCLASS_GPR64 ||
      v.index >= 32u)
    return 0;
  *r = v.index;
  return 1;
}
static int fp(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r,
              unsigned bits) {
  AstTargetRegisterInfo v;
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      o->bank != AST_MACHINE_BANK_FP || o->bit_width != bits ||
      (bits != 32u && bits != 64u) ||
      !(b->target.features &
        (bits == 64u ? AST_TARGET_FEATURE_RV_D : AST_TARGET_FEATURE_RV_F)) ||
      !encoder_reg(b, o, &v) || v.width < bits || v.index >= 32u ||
      (v.reg_class != AST_TARGET_REGCLASS_FP32 &&
       v.reg_class != AST_TARGET_REGCLASS_FP64))
    return 0;
  *r = v.index;
  return 1;
}
static int base(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r) {
  if (o->kind != AST_PHYSICAL_OPERAND_MEMORY || o->scale ||
      o->index_register_name[0] || o->index_register_unit)
    return 0;
  AstPhysicalOperand p = *o;
  p.kind = AST_PHYSICAL_OPERAND_REGISTER;
  p.bank = AST_MACHINE_BANK_GPR;
  p.bit_width = 64u;
  return gpr(b, &p, r);
}
static int imm12(EncoderBuilder *b, uint32_t op, unsigned rd, unsigned rs,
                 int64_t imm) {
  if (imm < -2048 || imm > 2047)
    return 0;
  return word(b,
              op | (((uint32_t)imm & 4095u) << 20u) | (rs << 15u) | (rd << 7u));
}
/* Construct arbitrary RV64 bit patterns without signed overflow. Recursive
   balanced base-4096 digits use only the destination, never a hidden scratch.
 */
static int constant(EncoderBuilder *b, unsigned rd, uint64_t value) {
  int64_t lo = (int64_t)(value & 4095u);
  if (lo >= 2048)
    lo -= 4096;
  if (value == (uint64_t)lo)
    return imm12(b, 0x13u, rd, 0u, lo);
  if (value == (uint64_t)(int64_t)(int32_t)(uint32_t)value) {
    uint32_t hi = (uint32_t)((value + 2048u) & UINT64_C(0xfffff000));
    return word(b, hi | (rd << 7u) | 0x37u) &&
           (!lo || imm12(b, 0x1bu, rd, rd, lo));
  }
  uint64_t upper = (value - (uint64_t)lo) >> 12u;
  if (upper & (UINT64_C(1) << 51u))
    upper |= UINT64_C(0xfff0000000000000);
  return constant(b, rd, upper) &&
         word(b, 0x00c01013u | (rd << 15u) | (rd << 7u)) &&
         (!lo || imm12(b, 0x13u, rd, rd, lo));
}
static int jump(EncoderBuilder *b, uint32_t target) {
  int64_t d;
  if (!encoder_delta(b, target, b->out->size, &d))
    return 0;
  if (d % 2 || d < -INT64_C(1048576) || d > INT64_C(1048574))
    return encoder_fail(b, AST_ENCODER_DIAG_BRANCH_RANGE,
                        "RV JAL exceeds signed 21-bit range.");
  uint32_t u = (uint32_t)d;
  return word(b, 0x6fu | (((u >> 20u) & 1u) << 31u) |
                     (((u >> 1u) & 1023u) << 21u) | (((u >> 11u) & 1u) << 20u) |
                     (u & 0xff000u));
}
static int branch_cond(EncoderBuilder *b, unsigned rs, unsigned rt,
                       uint32_t target, unsigned funct3) {
  int64_t d;
  if (!encoder_delta(b, target, b->out->size, &d))
    return 0;
  if (d % 2 || d < -4096 || d > 4094)
    return 0;
  uint32_t u = (uint32_t)d;
  return word(b, 0x63u | (funct3 << 12u) | (rs << 15u) | (rt << 20u) |
                     ((u & 0x1000u) << 19u) | ((u & 0x7e0u) << 20u) |
                     ((u & 0x1eu) << 7u) | ((u & 0x800u) >> 4u));
}
static int branch_cond_or_jump(EncoderBuilder *b, unsigned rs, unsigned rt,
                               uint32_t target, unsigned funct3) {
  if (b->branch_count >= b->branch_capacity || !b->short_branches ||
      !b->local_branches)
    return encoder_fail(b, AST_ENCODER_DIAG_INVALID_PHYSICAL,
                        "Missing RV branch relaxation state.");
  size_t id = b->branch_count++;
  b->local_branches[id] = (EncoderLocalBranch){b->out->size, target, 8u};
  if (b->short_branches[id])
    return branch_cond(b, rs, rt, target, funct3);
  return word(b, 0x63u | ((funct3 ^ 1u) << 12u) | (rs << 15u) | (rt << 20u) |
                     (8u << 7u)) &&
         jump(b, target);
}
static int memory(EncoderBuilder *b, const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  unsigned rd, rs, bytes, funct;
  int load, is_fp = 0;
  switch (i->opcode) {
  case AST_MACHINE_RISCV_LB:
    load = 1;
    funct = 0;
    bytes = 1;
    break;
  case AST_MACHINE_RISCV_LBU:
    load = 1;
    funct = 4;
    bytes = 1;
    break;
  case AST_MACHINE_RISCV_LH:
    load = 1;
    funct = 1;
    bytes = 2;
    break;
  case AST_MACHINE_RISCV_LHU:
    load = 1;
    funct = 5;
    bytes = 2;
    break;
  case AST_MACHINE_RISCV_LW:
    load = 1;
    funct = 2;
    bytes = 4;
    break;
  case AST_MACHINE_RISCV_LWU:
    load = 1;
    funct = 6;
    bytes = 4;
    break;
  case AST_MACHINE_RISCV_LD:
    load = 1;
    funct = 3;
    bytes = 8;
    break;
  case AST_MACHINE_RISCV_SB:
    load = 0;
    funct = 0;
    bytes = 1;
    break;
  case AST_MACHINE_RISCV_SH:
    load = 0;
    funct = 1;
    bytes = 2;
    break;
  case AST_MACHINE_RISCV_SW:
    load = 0;
    funct = 2;
    bytes = 4;
    break;
  case AST_MACHINE_RISCV_SD:
    load = 0;
    funct = 3;
    bytes = 8;
    break;
  case AST_MACHINE_RISCV_FLW:
    load = 1;
    funct = 2;
    bytes = 4;
    is_fp = 1;
    break;
  case AST_MACHINE_RISCV_FLD:
    load = 1;
    funct = 3;
    bytes = 8;
    is_fp = 1;
    break;
  case AST_MACHINE_RISCV_FSW:
    load = 0;
    funct = 2;
    bytes = 4;
    is_fp = 1;
    break;
  case AST_MACHINE_RISCV_FSD:
    load = 0;
    funct = 3;
    bytes = 8;
    is_fp = 1;
    break;
  default:
    return 0;
  }
  if (!encoder_shape(i, load ? 1u : 0u, 2u) || !o[1].bit_width ||
      (o[1].bit_width + 7u) / 8u != bytes || o[0].bit_width < o[1].bit_width ||
      !base(b, &o[1], &rs) ||
      (is_fp ? !fp(b, &o[0], &rd, bytes * 8u) : !gpr(b, &o[0], &rd)))
    return 0;
  int64_t d = o[1].displacement;
  if (load)
    return imm12(b, (is_fp ? 0x07u : 0x03u) | (funct << 12u), rd, rs, d);
  if (d < -2048 || d > 2047)
    return 0;
  uint32_t u = (uint32_t)d & 4095u;
  return word(b, (is_fp ? 0x27u : 0x23u) | ((u >> 5u) << 25u) | (rd << 20u) |
                     (rs << 15u) | (funct << 12u) | ((u & 31u) << 7u));
}
static int convert(EncoderBuilder *b, const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  unsigned rd, rs, bits, intbits, selector;
  uint32_t op;
  if (!encoder_shape(i, 1u, 3u) ||
      o[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE || o[2].immediate_bits > 1u)
    return 0;
  if (i->opcode >= AST_MACHINE_RISCV_FCVT_S_W &&
      i->opcode <= AST_MACHINE_RISCV_FCVT_LU_D) {
    unsigned index = (unsigned)(i->opcode - AST_MACHINE_RISCV_FCVT_S_W);
    int to_fp = !(index & 1u);
    bits = index < 8u ? 32u : 64u;
    selector = (index % 8u) / 2u;
    intbits = selector < 2u ? 32u : 64u;
    if (o[to_fp ? 1 : 0].bit_width < intbits)
      return 0;
    if (to_fp) {
      if (!fp(b, &o[0], &rd, bits) || !gpr(b, &o[1], &rs))
        return 0;
    } else {
      if (!gpr(b, &o[0], &rd) || !fp(b, &o[1], &rs, bits))
        return 0;
    }
    op = (to_fp ? 0xd0000053u : 0xc0000053u) | (bits == 64u ? 1u << 25u : 0u) |
         (selector << 20u);
  } else {
    bits = i->opcode == AST_MACHINE_RISCV_FCVT_S_D ? 32u : 64u;
    if (!fp(b, &o[0], &rd, bits) || !fp(b, &o[1], &rs, bits == 32u ? 64u : 32u))
      return 0;
    op = bits == 32u ? 0x40100053u : 0x42000053u;
  }
  return word(b, op | (rs << 15u) | ((uint32_t)o[2].immediate_bits << 12u) |
                     (rd << 7u));
}

static uint32_t atomic_rv_r(uint32_t op, unsigned d, unsigned s, unsigned t) {
  return op | (t << 20u) | (s << 15u) | (d << 7u);
}
static int atomic_rv_branch(EncoderBuilder *b, size_t at, size_t target,
                            unsigned s, unsigned t) {
  int64_t d = (int64_t)target - (int64_t)at;
  if (d % 2 || d < -4096 || d > 4094 || at + 4 > b->out->size)
    return 0;
  uint32_t u = (uint32_t)d;
  uint32_t w = 0x1063u | (s << 15u) | (t << 20u) | ((u & 0x1000u) << 19u) |
               ((u & 0x7e0u) << 20u) | ((u & 0x1eu) << 7u) |
               ((u & 0x800u) >> 4u);
  for (unsigned j = 0; j < 4; ++j)
    b->out->bytes[at + j] = (uint8_t)(w >> (8 * j));
  return 1;
}
static int atomic_rv(EncoderBuilder *b, const AstPhysicalInstructionInfo *i) {
  if (!atomic_physical_contract(i) ||
      !(b->target.features & AST_TARGET_FEATURE_RV_A))
    return 0;
  unsigned r[10];
  for (unsigned j = 0; j < 10; ++j)
    if (!gpr(b, &i->operands[j], &r[j]) || !r[j] || r[j] == 2u || r[j] == 3u ||
        r[j] == 4u)
      return 0;
  unsigned code = (unsigned)i->operands[10].immediate_bits, op = code & 255u,
           bits = code >> 8u;
  if (bits == 1)
    bits = 8;
  unsigned size = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
  AstMemoryOrder order = i->memory_order;
  int acquire = order == AST_MEMORY_ORDER_ACQUIRE ||
                order == AST_MEMORY_ORDER_ACQ_REL ||
                order == AST_MEMORY_ORDER_SEQ_CST;
  int release = order == AST_MEMORY_ORDER_RELEASE ||
                order == AST_MEMORY_ORDER_ACQ_REL ||
                order == AST_MEMORY_ORDER_SEQ_CST;
  int seq = order == AST_MEMORY_ORDER_SEQ_CST;
  if (op == AT_LOAD)
    return (!seq || word(b, 0x0330000fu)) &&
           imm12(b, 0x03u | ((size == 3 ? 3u : size + 4u) << 12u), r[0], r[7],
                 0) &&
           (!acquire || word(b, 0x0230000fu));
  if (op == AT_STORE)
    return (!release || word(b, 0x0310000fu)) &&
           word(b, 0x23u | (size << 12u) | (r[7] << 15u) | (r[9] << 20u)) &&
           (!seq || word(b, 0x0330000fu));
  if (bits >= 32 && op != AT_CAS) {
    unsigned function = op == AT_XCHG  ? 1u
                        : op == AT_XOR ? 4u
                        : op == AT_AND ? 12u
                        : op == AT_OR  ? 8u
                                       : 0u;
    if (op == AT_SUB && !word(b, atomic_rv_r(0x40000033u, r[9], 0, r[9])))
      return 0;
    return word(b, (function << 27u) | (acquire ? 1u << 26u : 0u) |
                       (release ? 1u << 25u : 0u) | (r[9] << 20u) |
                       (r[7] << 15u) | ((bits == 64 ? 3u : 2u) << 12u) |
                       (r[0] << 7u) | 0x2fu);
  }
  /* Leading fence also makes a seq_cst failed CAS compatible with older
     RVWMO compiler mappings. No extra fence occurs inside the LR/SC loop. */
  if (op == AT_CAS && i->failure_memory_order == AST_MEMORY_ORDER_SEQ_CST &&
      !word(b, 0x0330000fu))
    return 0;
  /* Subword updates preserve all adjacent bits through a masked LR.W/SC.W
     loop. Natural alignment is required; MMIO is not an atomic storage area. */
  if (bits < 32) {
    if (!imm12(b, 0x7013u, r[4], r[7], -4) ||
        !imm12(b, 0x7013u, r[5], r[7], 3) ||
        !imm12(b, 0x1013u, r[5], r[5], 3) ||
        !constant(b, r[6], bits == 8 ? 255 : 65535) ||
        !word(b, atomic_rv_r(0x7033u, r[8], r[8], r[6])))
      return 0;
  } else {
    if (!imm12(b, 0x13u, r[4], r[7], 0))
      return 0;
    if (bits == 32 && (!imm12(b, 0x1013u, r[8], r[8], 32) ||
                       !imm12(b, 0x5013u, r[8], r[8], 32)))
      return 0;
  }
  size_t loop = b->out->size, mismatch = 0;
  uint32_t widthbits = bits == 64 ? 0x3000u : 0x2000u;
  if (!word(b, 0x1000002fu | (acquire ? 1u << 26u : 0u) |
                   (seq ? 1u << 25u : 0u) | widthbits | (r[4] << 15u) |
                   (r[3] << 7u)))
    return 0; /* LR.aqrl */
  if (bits < 32) {
    if (!word(b, atomic_rv_r(0x5033u, r[0], r[3], r[5])) ||
        !word(b, atomic_rv_r(0x7033u, r[0], r[0], r[6])))
      return 0;
  } else if (bits == 32) {
    if (!imm12(b, 0x1013u, r[0], r[3], 32) ||
        !imm12(b, 0x5013u, r[0], r[0], 32))
      return 0;
  } else if (!imm12(b, 0x13u, r[0], r[3], 0))
    return 0;
  if (op == AT_CAS) {
    mismatch = b->out->size;
    if (!word(b, 0))
      return 0;
  }
  uint32_t arithmetic = op == AT_ADD   ? 0x33u
                        : op == AT_SUB ? 0x40000033u
                        : op == AT_AND ? 0x7033u
                        : op == AT_OR  ? 0x6033u
                                       : 0x4033u;
  if (op == AT_XCHG || op == AT_CAS) {
    if (!imm12(b, 0x13u, r[1], r[9], 0))
      return 0;
  } else if (!word(b, atomic_rv_r(arithmetic, r[1], r[0], r[9])))
    return 0;
  if (bits < 32) {
    if (!word(b, atomic_rv_r(0x7033u, r[1], r[1], r[6])) ||
        !word(b, atomic_rv_r(0x1033u, r[1], r[1], r[5])) ||
        !word(b, atomic_rv_r(0x1033u, r[2], r[6], r[5])) ||
        !imm12(b, 0x4013u, r[2], r[2], -1) ||
        !word(b, atomic_rv_r(0x7033u, r[3], r[3], r[2])) ||
        !word(b, atomic_rv_r(0x6033u, r[1], r[3], r[1])))
      return 0;
  }
  if (!word(b, 0x1800002fu | (release ? 1u << 25u : 0u) | widthbits |
                   (r[1] << 20u) | (r[4] << 15u) | (r[2] << 7u)))
    return 0; /* SC.rl */
  size_t retry = b->out->size;
  if (!word(b, 0) || !atomic_rv_branch(b, retry, loop, r[2], 0))
    return 0;
  /* Keep every reservation loop constrained (at most 16 base instructions). */
  if (b->out->size - loop > 64u)
    return 0;
  if (op == AT_CAS) {
    if (!imm12(b, 0x13u, r[0], 0, 1) || !word(b, 0x0080006fu) ||
        !atomic_rv_branch(b, mismatch, b->out->size, r[0], r[8]) ||
        !imm12(b, 0x13u, r[0], 0, 0))
      return 0;
  }
  return 1;
}

int encoder_rv_instruction(EncoderBuilder *b,
                           const AstPhysicalInstructionInfo *i) {
  const RvScalarForm *scalar = rv_scalar_form(i->opcode);
  if (scalar) {
    if (!rv_scalar_contract(i) || (scalar->feature & ~b->target.features))
      return 0;
    for (uint32_t j = 0; j < i->operand_count; ++j)
      if (i->operands[j].kind == AST_PHYSICAL_OPERAND_REGISTER) {
        unsigned reg;
        if (!gpr(b, &i->operands[j], &reg))
          return 0;
      }
    return word(b, rv_scalar_word(scalar, i));
  }
  if (i->opcode == AST_MACHINE_RISCV_VECTOR) {
    const VectorForm *f = vector_physical_form(i);
    if (!f || !vector_physical_contract(i) || !vector_features(&b->target, i))
      return 0;
    for (uint32_t j = 0; j < i->operand_count; ++j)
      if (i->operands[j].kind == AST_PHYSICAL_OPERAND_REGISTER) {
        AstTargetRegisterInfo view;
        if (!encoder_reg(b, &i->operands[j], &view))
          return 0;
      }
    return word(b, vector_word(f, i));
  }
  if (i->opcode == AST_MACHINE_RISCV_SYSTEM) {
    const SystemForm *f = system_physical_form(i);
    for (uint32_t j = 0; j < i->operand_count && j < AST_PHYSICAL_MAX_OPERANDS;
         ++j)
      if (i->operands[j].kind == AST_PHYSICAL_OPERAND_REGISTER) {
        AstTargetRegisterInfo view;
        if (!encoder_reg(b, &i->operands[j], &view))
          return 0;
      }
    return f && !(f->feature & ~b->target.features) &&
           system_physical_contract(i) && word(b, system_word(f, i));
  }
  const AstPhysicalOperand *o = i->operands;
  unsigned rd, rs, rt, bits;
  uint32_t op;
  if ((i->opcode >= AST_MACHINE_RISCV_LB && i->opcode <= AST_MACHINE_RISCV_FSD))
    return memory(b, i);
  if (i->opcode >= AST_MACHINE_RISCV_FCVT_S_W &&
      i->opcode <= AST_MACHINE_RISCV_FCVT_D_S)
    return convert(b, i);
  switch (i->opcode) {
  case AST_MACHINE_RISCV_LRSC_ATOMIC:
    return atomic_rv(b, i);
  case AST_MACHINE_RISCV_RET:
    return encoder_shape(i, 0u, 0u) && word(b, 0x00008067u);
  case AST_MACHINE_RISCV_EBREAK:
    return encoder_shape(i, 0u, 0u) && word(b, 0x00100073u);
  case AST_MACHINE_RISCV_FENCE:
    return encoder_shape(i, 0u, 0u) && word(b, 0x0330000fu);
  case AST_MACHINE_RISCV_LI:
    return encoder_shape(i, 1u, 2u) && gpr(b, &o[0], &rd) && rd &&
           o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           constant(b, rd, o[1].immediate_bits);
  case AST_MACHINE_RISCV_LA:
    if (!encoder_shape(i, 1u, 2u) || o[0].bit_width != 64u ||
        !gpr(b, &o[0], &rd) || !rd ||
        o[1].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        !word(b, 0x17u | (rd << 7u)) || !imm12(b, 0x13u, rd, rd, 0))
      return 0;
    return encoder_fixup(b, b->out->size - 8u, 8u,
                         AST_ENCODER_FIXUP_RISCV_PCREL_ADDRESS, o[1].module,
                         o[1].symbol);
  case AST_MACHINE_RISCV_MV:
  case AST_MACHINE_RISCV_NEG:
  case AST_MACHINE_RISCV_NOT:
    if (!encoder_shape(i, 1u, 2u) || !gpr(b, &o[0], &rd) || !gpr(b, &o[1], &rs))
      return 0;
    if (i->opcode == AST_MACHINE_RISCV_NEG)
      return word(b, 0x40000033u | (rs << 20u) | (rd << 7u));
    return imm12(b, i->opcode == AST_MACHINE_RISCV_NOT ? 0x4013u : 0x13u, rd,
                 rs, i->opcode == AST_MACHINE_RISCV_NOT ? -1 : 0);
  case AST_MACHINE_RISCV_FMV_S:
  case AST_MACHINE_RISCV_FMV_D:
  case AST_MACHINE_RISCV_FNEG_S:
  case AST_MACHINE_RISCV_FNEG_D:
    bits = i->opcode == AST_MACHINE_RISCV_FMV_S ||
                   i->opcode == AST_MACHINE_RISCV_FNEG_S
               ? 32u
               : 64u;
    return encoder_shape(i, 1u, 2u) && fp(b, &o[0], &rd, bits) &&
           fp(b, &o[1], &rs, bits) &&
           word(b, 0x20000053u | (bits == 64u ? 1u << 25u : 0u) |
                       (i->opcode == AST_MACHINE_RISCV_FNEG_S ||
                                i->opcode == AST_MACHINE_RISCV_FNEG_D
                            ? 1u << 12u
                            : 0u) |
                       (rs << 20u) | (rs << 15u) | (rd << 7u));
  case AST_MACHINE_RISCV_FMV_W_X:
  case AST_MACHINE_RISCV_FMV_D_X:
  case AST_MACHINE_RISCV_FMV_X_W:
  case AST_MACHINE_RISCV_FMV_X_D: {
    int to_fp = i->opcode == AST_MACHINE_RISCV_FMV_W_X ||
                i->opcode == AST_MACHINE_RISCV_FMV_D_X;
    bits = i->opcode == AST_MACHINE_RISCV_FMV_W_X ||
                   i->opcode == AST_MACHINE_RISCV_FMV_X_W
               ? 32u
               : 64u;
    if (!encoder_shape(i, 1u, 2u) || o[to_fp ? 1 : 0].bit_width < bits)
      return 0;
    if (to_fp) {
      if (!fp(b, &o[0], &rd, bits) || !gpr(b, &o[1], &rs))
        return 0;
    } else {
      if (!gpr(b, &o[0], &rd) || !fp(b, &o[1], &rs, bits))
        return 0;
    }
    return word(b, (to_fp ? 0xf0000053u : 0xe0000053u) |
                       (bits == 64u ? 1u << 25u : 0u) | (rs << 15u) |
                       (rd << 7u));
  }
  case AST_MACHINE_RISCV_FADD_S:
    op = 0x00000053u;
    bits = 32u;
    goto float_binary;
  case AST_MACHINE_RISCV_FADD_D:
    op = 0x02000053u;
    bits = 64u;
    goto float_binary;
  case AST_MACHINE_RISCV_FSUB_S:
    op = 0x08000053u;
    bits = 32u;
    goto float_binary;
  case AST_MACHINE_RISCV_FSUB_D:
    op = 0x0a000053u;
    bits = 64u;
    goto float_binary;
  case AST_MACHINE_RISCV_FMUL_S:
    op = 0x10000053u;
    bits = 32u;
    goto float_binary;
  case AST_MACHINE_RISCV_FMUL_D:
    op = 0x12000053u;
    bits = 64u;
    goto float_binary;
  case AST_MACHINE_RISCV_FDIV_S:
    op = 0x18000053u;
    bits = 32u;
    goto float_binary;
  case AST_MACHINE_RISCV_FDIV_D:
    op = 0x1a000053u;
    bits = 64u;
  float_binary:
    /* Explicit RNE agrees with the physical interpreter's contract. */
    return encoder_shape(i, 1u, 3u) && fp(b, &o[0], &rd, bits) &&
           fp(b, &o[1], &rs, bits) && fp(b, &o[2], &rt, bits) &&
           word(b, op | (rt << 20u) | (rs << 15u) | (rd << 7u));
  case AST_MACHINE_RISCV_FEQ_S:
    op = 0xa0002053u;
    bits = 32u;
    goto float_compare;
  case AST_MACHINE_RISCV_FEQ_D:
    op = 0xa2002053u;
    bits = 64u;
    goto float_compare;
  case AST_MACHINE_RISCV_FLT_S:
    op = 0xa0001053u;
    bits = 32u;
    goto float_compare;
  case AST_MACHINE_RISCV_FLT_D:
    op = 0xa2001053u;
    bits = 64u;
    goto float_compare;
  case AST_MACHINE_RISCV_FLE_S:
    op = 0xa0000053u;
    bits = 32u;
    goto float_compare;
  case AST_MACHINE_RISCV_FLE_D:
    op = 0xa2000053u;
    bits = 64u;
  float_compare:
    return encoder_shape(i, 1u, 3u) && gpr(b, &o[0], &rd) &&
           fp(b, &o[1], &rs, bits) && fp(b, &o[2], &rt, bits) &&
           word(b, op | (rt << 20u) | (rs << 15u) | (rd << 7u));
  case AST_MACHINE_RISCV_CALL:
    if (!encoder_shape(i, 0u, 1u) || o[0].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        !word(b, 0x00000097u) || !word(b, 0x000080e7u))
      return 0;
    return encoder_fixup(b, b->out->size - 8u, 8u, AST_ENCODER_FIXUP_RISCV_CALL,
                         o[0].module, o[0].symbol);
  case AST_MACHINE_RISCV_CALL_INDIRECT:
    return encoder_shape(i, 0u, 1u) && o[0].bit_width == 64u &&
           gpr(b, &o[0], &rs) && imm12(b, 0x67u, 1u, rs, 0);
  case AST_MACHINE_RISCV_NATIVE_JALR:
    return encoder_shape(i, 0u, 3u) && o[0].bit_width == 64u &&
           o[1].bit_width == 64u && gpr(b, &o[0], &rd) &&
           (rd == 0u || rd == 1u) && gpr(b, &o[1], &rs) &&
           o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           imm12(b, 0x67u, rd, rs,
                 o[2].immediate_bits <= INT64_MAX
                     ? (int64_t)o[2].immediate_bits
                     : -(int64_t)(~o[2].immediate_bits) - 1);
  case AST_MACHINE_RISCV_NATIVE_BEQ:
  case AST_MACHINE_RISCV_NATIVE_BNE:
  case AST_MACHINE_RISCV_NATIVE_BLT:
  case AST_MACHINE_RISCV_NATIVE_BGE:
  case AST_MACHINE_RISCV_NATIVE_BLTU:
  case AST_MACHINE_RISCV_NATIVE_BGEU: {
    if (!encoder_shape(i, 0u, 3u) || !gpr(b, &o[0], &rs) ||
        !gpr(b, &o[1], &rt) || o[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
      return 0;
    uint64_t raw = o[2].immediate_bits;
    int64_t off = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
    if ((off & 3) || off < -INT64_C(4096) || off > INT64_C(4092))
      return 0;
    uint32_t v = (uint32_t)off;
    return word(b, UINT32_C(0x63) |
                       ((uint32_t)rv_native_branch_funct3(i->opcode) << 12u) |
                       (rs << 15u) | (rt << 20u) | ((v & 0x1000u) << 19u) |
                       ((v & 0x7e0u) << 20u) | ((v & 0x1eu) << 7u) |
                       ((v & 0x800u) >> 4u));
  }
  case AST_MACHINE_RISCV_NATIVE_JAL:
    if (encoder_shape(i, 0u, 1u) &&
        o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint64_t raw = o[0].immediate_bits;
      int64_t off = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
      if ((off & 3) || off < -INT64_C(1048576) || off > INT64_C(1048572))
        return 0;
      uint32_t v = (uint32_t)off;
      return word(b, UINT32_C(0xef) | ((v >> 20u & 1u) << 31u) |
                         ((v >> 1u & 0x3ffu) << 21u) |
                         ((v >> 11u & 1u) << 20u) |
                         ((v >> 12u & 0xffu) << 12u));
    }
    return 0;
  case AST_MACHINE_RISCV_J:
    if (encoder_shape(i, 0u, 1u) &&
        o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint64_t raw = o[0].immediate_bits;
      int64_t off = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
      if ((off & 3) || off < -INT64_C(1048576) || off > INT64_C(1048572))
        return 0;
      uint32_t v = (uint32_t)off;
      return word(
          b, 0x6fu | ((v >> 20u & 1u) << 31u) | ((v >> 1u & 0x3ffu) << 21u) |
                 ((v >> 11u & 1u) << 20u) | ((v >> 12u & 0xffu) << 12u));
    }
    if (!encoder_shape(i, 0u, 1u) || o[0].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    return o[0].block == b->next_block || jump(b, o[0].block);
  case AST_MACHINE_RISCV_CMP_BR: {
    if (!encoder_shape(i, 0u, 5u) ||
        o[0].kind != AST_PHYSICAL_OPERAND_IMMEDIATE || !gpr(b, &o[1], &rs) ||
        !gpr(b, &o[2], &rt) || o[3].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        o[4].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    unsigned cond = (unsigned)o[0].immediate_bits;
    if (cond != 0u && cond != 1u && cond != 4u && cond != 5u && cond != 6u &&
        cond != 7u)
      return 0;
    uint32_t yes = o[3].block, no = o[4].block;
    if (yes == no)
      return yes == b->next_block || jump(b, yes);
    if (no == b->next_block)
      return branch_cond_or_jump(b, rs, rt, yes, cond);
    if (yes == b->next_block)
      return branch_cond_or_jump(b, rs, rt, no, cond ^ 1u);
    return branch_cond_or_jump(b, rs, rt, yes, cond) && jump(b, no);
  }
  case AST_MACHINE_RISCV_BNE:
    if (!encoder_shape(i, 0u, 3u) || !gpr(b, &o[0], &rs) ||
        !gpr(b, &o[1], &rt) || o[2].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    /* Use the native BNE when the target fits its ±4 KiB range. For
       distant blocks retain the previous BEQ-over-JAL expansion. */
    return branch_cond_or_jump(b, rs, rt, o[2].block, 1u);
  default:
    return 0;
  }
}
