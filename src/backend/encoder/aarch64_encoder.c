#include "../../isa/aarch64_data_contract.h"
#include "../../isa/aarch64_scalable.h"
#include "../../isa/aarch64_scalar_contract.h"
#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "../postra/atomic_contract.h"
#include "encoder_internal.h"

static int word(EncoderBuilder *b, uint32_t w) { return encoder_le(b, w, 4u); }
static int gpr(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r,
               int sp) {
  AstTargetRegisterInfo v;
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER || !o->bit_width ||
      o->bit_width > 64u || !encoder_reg(b, o, &v))
    return 0;
  if (sp && o->bit_width == 64u && !strcmp(o->register_name, "sp") &&
      (o->bank == AST_MACHINE_BANK_SPECIAL ||
       o->bank == AST_MACHINE_BANK_GPR)) {
    *r = 31u;
    return 1;
  }
  if (o->bank != AST_MACHINE_BANK_GPR || v.special || v.index >= 31u ||
      v.reg_class != (o->bit_width <= 32u ? AST_TARGET_REGCLASS_GPR32
                                          : AST_TARGET_REGCLASS_GPR64))
    return 0;
  *r = v.index;
  return 1;
}
static int fp(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r) {
  AstTargetRegisterInfo v;
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      o->bank != AST_MACHINE_BANK_FP ||
      (o->bit_width != 32u && o->bit_width != 64u) || !encoder_reg(b, o, &v) ||
      v.width != o->bit_width || v.special || v.index >= 32u ||
      v.reg_class != AST_TARGET_REGCLASS_VEC64)
    return 0;
  *r = v.index;
  return 1;
}
static int vec(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r) {
  AstTargetRegisterInfo v;
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      (o->bank != AST_MACHINE_BANK_VECTOR &&
       !(o->bank == AST_MACHINE_BANK_FP && o->bit_width == 128u)) ||
      (o->bit_width != 64u && o->bit_width != 128u) || !encoder_reg(b, o, &v) ||
      v.special || v.index >= 32u || v.width < o->bit_width ||
      (v.reg_class != AST_TARGET_REGCLASS_VEC128 &&
       v.reg_class != AST_TARGET_REGCLASS_FP64 &&
       v.reg_class != AST_TARGET_REGCLASS_VEC64))
    return 0;
  *r = v.index;
  return 1;
}
static int base(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r) {
  if (o->kind != AST_PHYSICAL_OPERAND_MEMORY)
    return 0;
  AstPhysicalOperand p = *o;
  p.kind = AST_PHYSICAL_OPERAND_REGISTER;
  p.bank = AST_MACHINE_BANK_GPR;
  p.bit_width = 64u;
  return gpr(b, &p, r, 1);
}
static int add_imm(EncoderBuilder *b, unsigned rd, unsigned rn, uint32_t bits,
                   int64_t amount) {
  if (bits != 32u && bits != 64u)
    return 0;
  uint32_t op = amount < 0 ? (bits == 64u ? 0xd1000000u : 0x51000000u)
                           : (bits == 64u ? 0x91000000u : 0x11000000u);
  uint64_t mag = amount < 0 ? UINT64_C(0) - (uint64_t)amount : (uint64_t)amount;
  if (mag > 4095u) {
    if ((mag & 4095u) || (mag >> 12u) > 4095u)
      return 0;
    mag >>= 12u;
    op |= 1u << 22u;
  }
  return word(b, op | ((uint32_t)mag << 10u) | (rn << 5u) | rd);
}
static int address_imm(EncoderBuilder *b, unsigned rd, unsigned rn,
                       int64_t amount) {
  if (amount < INT32_MIN || amount > INT32_MAX)
    return 0;
  uint64_t magnitude =
      amount < 0 ? UINT64_C(0) - (uint64_t)amount : (uint64_t)amount;
  uint64_t high = magnitude >> 12u;
  uint32_t steps = (uint32_t)((high + 4094u) / 4095u) +
                   (uint32_t)((magnitude & 4095u) != 0u);
  if (steps > 3u && rd != rn) {
    /* Reuse the destination, never an undeclared temporary. Extended ADD/
       SUB permits SP as its base; shifted-register ADD would read XZR. */
    uint32_t low = (uint32_t)magnitude & 65535u,
             upper = (uint32_t)(magnitude >> 16u);
    if (!word(b, UINT32_C(0xd2800000) | (low << 5u) | rd))
      return 0;
    if (upper && !word(b, UINT32_C(0xf2a00000) | (upper << 5u) | rd))
      return 0;
    return word(b, (amount < 0 ? UINT32_C(0xcb206000) : UINT32_C(0x8b206000)) |
                       (rd << 16u) | (rn << 5u) | rd);
  }
  if (!magnitude)
    return add_imm(b, rd, rn, 64u, 0);
  while (magnitude) {
    uint64_t part =
        magnitude > 4095u ? (magnitude & ~UINT64_C(4095)) : magnitude;
    if (part > UINT64_C(0xfff000))
      part = UINT64_C(0xfff000);
    int64_t delta = amount < 0 ? -(int64_t)part : (int64_t)part;
    if (!add_imm(b, rd, rn, 64u, delta))
      return 0;
    magnitude -= part;
    rn = rd;
  }
  return 1;
}
static int jump(EncoderBuilder *b, uint32_t target) {
  int64_t delta;
  if (!encoder_delta(b, target, b->out->size, &delta))
    return 0;
  if (delta % 4 || delta < -INT64_C(134217728) || delta > INT64_C(134217724))
    return encoder_fail(b, AST_ENCODER_DIAG_BRANCH_RANGE,
                        "A64 branch exceeds signed imm26 range.");
  return word(b, 0x14000000u | ((uint32_t)(delta / 4) & 0x03ffffffu));
}
static int cond_jump(EncoderBuilder *b, uint32_t target, unsigned cond) {
  int64_t delta;
  if (cond > 15u || !encoder_delta(b, target, b->out->size, &delta))
    return 0;
  if (delta % 4 || delta < -INT64_C(1048576) || delta > INT64_C(1048572))
    return encoder_fail(b, AST_ENCODER_DIAG_BRANCH_RANGE,
                        "A64 conditional branch exceeds signed imm19 range.");
  return word(b,
              0x54000000u | (((uint32_t)(delta / 4) & 0x7ffffu) << 5u) | cond);
}
static int memory(EncoderBuilder *b, const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  int load = i->opcode == AST_MACHINE_A64_LOAD;
  uint32_t access_bits = o[1].bit_width;
  if (access_bits != 1u && access_bits != 8u && access_bits != 16u &&
      access_bits != 32u && access_bits != 64u && access_bits != 128u)
    return 0;
  unsigned r, rn, bytes = (access_bits + 7u) / 8u;
  uint32_t op;
  if (!encoder_shape(i, load ? 1u : 0u, 2u) || !base(b, &o[1], &rn))
    return 0;
  if (gpr(b, &o[0], &r, 0)) {
    if ((bytes != 1u && bytes != 2u && bytes != 4u && bytes != 8u) ||
        o[0].bit_width != (access_bits <= 32u ? 32u : 64u))
      return 0;
    op = bytes == 1u   ? 0x39400000u
         : bytes == 2u ? 0x79400000u
         : bytes == 4u ? 0xb9400000u
                       : 0xf9400000u;
  } else if (fp(b, &o[0], &r) || vec(b, &o[0], &r)) {
    if (o[0].bit_width != access_bits)
      return 0;
    op = bytes == 4u ? 0xbd400000u : bytes == 8u ? 0xfd400000u : 0x3dc00000u;
  } else
    return 0;
  if (!load)
    op &= ~UINT32_C(0x00400000);
  int64_t d = o[1].displacement;
  if (d >= 0 && d % bytes == 0 && d / bytes < 4096)
    op |= (uint32_t)(d / bytes) << 10u;
  else if (d >= -256 && d <= 255)
    op = (op & ~UINT32_C(0x01000000)) | (((uint32_t)d & 511u) << 12u);
  else
    return 0;
  return word(b, op | (rn << 5u) | r);
}

static int atomic_a64_branch(EncoderBuilder *b, size_t at, size_t target,
                             uint32_t op) {
  int64_t d = (int64_t)target - (int64_t)at;
  if (d % 4 || d < -1048576 || d > 1048572 || at + 4 > b->out->size)
    return 0;
  uint32_t w = op | (((uint32_t)(d / 4) & 0x7ffffu) << 5u);
  for (unsigned j = 0; j < 4; ++j)
    b->out->bytes[at + j] = (uint8_t)(w >> (j * 8));
  return 1;
}
static int atomic_a64(EncoderBuilder *b, const AstPhysicalInstructionInfo *i) {
  if (!atomic_physical_contract(i))
    return 0;
  unsigned r[6];
  for (unsigned j = 0; j < 6; ++j)
    if (!gpr(b, &i->operands[j], &r[j], 0) || r[j] >= 31u)
      return 0;
  unsigned code = (unsigned)i->operands[6].immediate_bits, op = code & 255u,
           bits = code >> 8u;
  if (bits == 1)
    bits = 8;
  unsigned size = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
  uint32_t sf = bits == 64 ? 0x80000000u : 0;
  AstMemoryOrder order = i->memory_order;
  int acquire = order == AST_MEMORY_ORDER_ACQUIRE ||
                order == AST_MEMORY_ORDER_ACQ_REL ||
                order == AST_MEMORY_ORDER_SEQ_CST;
  int release = order == AST_MEMORY_ORDER_RELEASE ||
                order == AST_MEMORY_ORDER_ACQ_REL ||
                order == AST_MEMORY_ORDER_SEQ_CST;
  if (op == AT_LOAD || op == AT_STORE) {
    uint32_t base = op == AT_LOAD ? (acquire ? 0x08dffc00u : 0x39400000u)
                                  : (release ? 0x089ffc00u : 0x39000000u);
    return word(b, base | (size << 30u) | (r[3] << 5u) |
                       (op == AT_LOAD ? r[0] : r[5]));
  }
  size_t loop = b->out->size, mismatch = 0;
  if (!word(b, 0x085f7c00u | (acquire ? 0x8000u : 0u) | (size << 30u) |
                   (r[3] << 5u) | r[0]))
    return 0;
  if (op == AT_CAS) {
    if (!word(b, sf | 0x6b00001fu | (r[4] << 16u) | (r[0] << 5u)))
      return 0;
    mismatch = b->out->size;
    if (!word(b, 0x54000001u))
      return 0;
  }
  uint32_t arithmetic = op == AT_ADD   ? 0x0b000000u
                        : op == AT_SUB ? 0x4b000000u
                        : op == AT_AND ? 0x0a000000u
                        : op == AT_OR  ? 0x2a000000u
                                       : 0x4a000000u;
  if (op == AT_XCHG || op == AT_CAS) {
    if (!word(b, sf | 0x2a0003e0u | (r[5] << 16u) | r[1]))
      return 0;
  } else if (!word(b, sf | arithmetic | (r[5] << 16u) | (r[0] << 5u) | r[1]))
    return 0;
  if (!word(b, 0x08007c00u | (release ? 0x8000u : 0u) | (size << 30u) |
                   (r[2] << 16u) | (r[3] << 5u) | r[1]))
    return 0;
  size_t retry = b->out->size;
  if (!word(b, 0) || !atomic_a64_branch(b, retry, loop, 0x35000000u | r[2]))
    return 0;
  if (op == AT_CAS) {
    if (!word(b, 0x52800020u | r[0]) || !word(b, 0x14000003u) ||
        !atomic_a64_branch(b, mismatch, b->out->size, 0x54000001u) ||
        !word(b, 0xd5033f5fu) || !word(b, 0x52800000u | r[0]))
      return 0;
  }
  return 1;
}

int encoder_a64_instruction(EncoderBuilder *b,
                            const AstPhysicalInstructionInfo *i) {
  if (i->opcode == AST_MACHINE_A64_SCALABLE) {
    const A64ScalableForm *f = a64_scalable_form(i);
    return f && a64_scalable_contract(i) &&
           a64_scalable_features(&b->target, f) &&
           word(b, a64_scalable_word(f, i));
  }
  if (i->opcode == AST_MACHINE_A64_DATA) {
    const A64DataForm *f = a64_data_form(i);
    return f && a64_data_contract(i) && word(b, a64_data_word(f, i));
  }
  if (i->opcode == AST_MACHINE_A64_SCALAR) {
    const A64ScalarForm *f = a64_scalar_form(i);
    return f && a64_scalar_contract(i) && word(b, a64_scalar_word(f, i));
  }
  if (i->opcode == AST_MACHINE_A64_VECTOR) {
    const VectorForm *f = vector_physical_form(i);
    if (!f || !vector_physical_contract(i) || !vector_features(&b->target, i))
      return 0;
    for (uint32_t j = 0; j < i->operand_count; ++j)
      if (i->operands[j].kind == AST_PHYSICAL_OPERAND_REGISTER ||
          i->operands[j].kind == AST_PHYSICAL_OPERAND_MEMORY) {
        AstTargetRegisterInfo view;
        if (!encoder_reg(b, &i->operands[j], &view))
          return 0;
      }
    return word(b, vector_word(f, i));
  }
  if (i->opcode == AST_MACHINE_A64_SYSTEM) {
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
  unsigned rd, rn, rm, ra, bits = o[0].bit_width;
  uint32_t sf = bits == 64u ? 0x80000000u : 0u, op;
  switch (i->opcode) {
  case AST_MACHINE_A64_EXCLUSIVE_ATOMIC:
    return atomic_a64(b, i);
  case AST_MACHINE_A64_RET:
    return encoder_shape(i, 0u, 0u) && word(b, 0xd65f03c0u);
  case AST_MACHINE_A64_TRAP:
    return encoder_shape(i, 0u, 0u) && word(b, 0xd4200000u);
  case AST_MACHINE_A64_FENCE:
    return encoder_shape(i, 0u, 0u) && word(b, 0xd5033bbfu);
  case AST_MACHINE_A64_LOAD:
  case AST_MACHINE_A64_STORE:
    return memory(b, i);
  case AST_MACHINE_A64_MOV:
    if (!encoder_shape(i, 1u, 2u) || bits != o[1].bit_width)
      return 0;
    if (gpr(b, &o[0], &rd, 0) && gpr(b, &o[1], &rn, 0))
      return word(b, sf | 0x2a0003e0u | (rn << 16u) | rd);
    if (fp(b, &o[0], &rd) && fp(b, &o[1], &rn))
      return word(b, 0x1e204000u | (bits == 64u ? 1u << 22u : 0u) | (rn << 5u) |
                         rd);
    return vec(b, &o[0], &rd) && vec(b, &o[1], &rn) &&
           word(b, 0x0ea01c00u | (bits == 128u ? 1u << 30u : 0u) | (rn << 16u) |
                       (rn << 5u) | rd);
  case AST_MACHINE_A64_MOVZ:
  case AST_MACHINE_A64_MOVK:
    return encoder_shape(i, 1u, 3u) && gpr(b, &o[0], &rd, 0) &&
           (bits == 32u || bits == 64u) &&
           o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[1].immediate_bits <= 65535u &&
           o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           !(o[2].immediate_bits % 16u) && o[2].immediate_bits < bits &&
           word(b, sf |
                       (i->opcode == AST_MACHINE_A64_MOVZ ? 0x52800000u
                                                          : 0x72800000u) |
                       ((uint32_t)o[2].immediate_bits / 16u << 21u) |
                       ((uint32_t)o[1].immediate_bits << 5u) | rd);
  case AST_MACHINE_A64_LEA:
    if (!encoder_shape(i, 1u, 2u) || bits != 64u || !gpr(b, &o[0], &rd, 0))
      return 0;
    if (base(b, &o[1], &rn))
      return address_imm(b, rd, rn, o[1].displacement);
    if (o[1].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        !word(b, 0x90000000u | rd) || !word(b, 0x91000000u | (rd << 5u) | rd))
      return 0;
    return encoder_fixup(b, b->out->size - 8u, 8u,
                         AST_ENCODER_FIXUP_A64_ADRP_ADD, o[1].module,
                         o[1].symbol);
  case AST_MACHINE_A64_ADD:
  case AST_MACHINE_A64_SUB:
    if (encoder_shape(i, 1u, 3u) && (bits == 32u || bits == 64u) &&
        o[1].bit_width == bits && o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
        o[2].immediate_bits <= INT64_MAX && gpr(b, &o[0], &rd, bits == 64u) &&
        gpr(b, &o[1], &rn, bits == 64u))
      return add_imm(b, rd, rn, bits,
                     i->opcode == AST_MACHINE_A64_SUB
                         ? -(int64_t)o[2].immediate_bits
                         : (int64_t)o[2].immediate_bits);
    /* fall through */
  case AST_MACHINE_A64_MUL:
  case AST_MACHINE_A64_AND:
  case AST_MACHINE_A64_OR:
  case AST_MACHINE_A64_XOR:
  case AST_MACHINE_A64_SHL:
  case AST_MACHINE_A64_LSHR:
  case AST_MACHINE_A64_ASHR:
  case AST_MACHINE_A64_SDIV:
  case AST_MACHINE_A64_UDIV:
    if ((i->opcode == AST_MACHINE_A64_SHL ||
         i->opcode == AST_MACHINE_A64_LSHR ||
         i->opcode == AST_MACHINE_A64_ASHR) &&
        encoder_shape(i, 1u, 3u) && (bits == 32u || bits == 64u) &&
        o[1].bit_width == bits && o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
        o[2].immediate_bits < bits && gpr(b, &o[0], &rd, 0) &&
        gpr(b, &o[1], &rn, 0)) {
      uint32_t amount = (uint32_t)o[2].immediate_bits;
      uint32_t immr, imms, shift_op;
      if (i->opcode == AST_MACHINE_A64_SHL) {
        immr = (bits - amount) & (bits - 1u);
        imms = bits - 1u - amount;
        shift_op = 0x53000000u; /* UBFM / LSL alias. */
      } else {
        immr = amount;
        imms = bits - 1u;
        shift_op =
            i->opcode == AST_MACHINE_A64_ASHR ? 0x13000000u : 0x53000000u;
      }
      if (bits == 64u)
        shift_op |= (1u << 31u) | (1u << 22u);
      return word(b,
                  shift_op | (immr << 16u) | (imms << 10u) | (rn << 5u) | rd);
    }
    if (!encoder_shape(i, 1u, 3u) || (bits != 32u && bits != 64u) ||
        bits != o[1].bit_width || bits != o[2].bit_width ||
        !gpr(b, &o[0], &rd, 0) || !gpr(b, &o[1], &rn, 0) ||
        !gpr(b, &o[2], &rm, 0))
      return 0;
    switch (i->opcode) {
    case AST_MACHINE_A64_ADD:
      op = 0x0b000000u;
      break;
    case AST_MACHINE_A64_SUB:
      op = 0x4b000000u;
      break;
    case AST_MACHINE_A64_MUL:
      op = 0x1b007c00u;
      break;
    case AST_MACHINE_A64_AND:
      op = 0x0a000000u;
      break;
    case AST_MACHINE_A64_OR:
      op = 0x2a000000u;
      break;
    case AST_MACHINE_A64_XOR:
      op = 0x4a000000u;
      break;
    case AST_MACHINE_A64_SHL:
      op = 0x1ac02000u;
      break;
    case AST_MACHINE_A64_LSHR:
      op = 0x1ac02400u;
      break;
    case AST_MACHINE_A64_ASHR:
      op = 0x1ac02800u;
      break;
    case AST_MACHINE_A64_SDIV:
      op = 0x1ac00c00u;
      break;
    default:
      op = 0x1ac00800u;
      break;
    }
    return word(b, sf | op | (rm << 16u) | (rn << 5u) | rd);
  case AST_MACHINE_A64_UMULH:
    return encoder_shape(i, 1u, 3u) && bits == 64u && o[1].bit_width == 64u &&
           o[2].bit_width == 64u && gpr(b, &o[0], &rd, 0) &&
           gpr(b, &o[1], &rn, 0) && gpr(b, &o[2], &rm, 0) &&
           word(b, UINT32_C(0x9bc07c00) | (rm << 16u) | (rn << 5u) | rd);
  case AST_MACHINE_A64_MSUB:
    return encoder_shape(i, 1u, 4u) && (bits == 32u || bits == 64u) &&
           bits == o[1].bit_width && bits == o[2].bit_width &&
           bits == o[3].bit_width && gpr(b, &o[0], &rd, 0) &&
           gpr(b, &o[1], &rn, 0) && gpr(b, &o[2], &rm, 0) &&
           gpr(b, &o[3], &ra, 0) &&
           word(b,
                sf | 0x1b008000u | (rm << 16u) | (ra << 10u) | (rn << 5u) | rd);
  case AST_MACHINE_A64_NEG:
  case AST_MACHINE_A64_NOT:
    return encoder_shape(i, 1u, 2u) && (bits == 32u || bits == 64u) &&
           bits == o[1].bit_width && gpr(b, &o[0], &rd, 0) &&
           gpr(b, &o[1], &rn, 0) &&
           word(b, sf |
                       (i->opcode == AST_MACHINE_A64_NEG ? 0x4b0003e0u
                                                         : 0x2a2003e0u) |
                       (rn << 16u) | rd);
  case AST_MACHINE_A64_SBFM:
  case AST_MACHINE_A64_UBFM:
    return encoder_shape(i, 1u, 4u) && (bits == 32u || bits == 64u) &&
           bits == o[1].bit_width && gpr(b, &o[0], &rd, 0) &&
           gpr(b, &o[1], &rn, 0) &&
           o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[2].immediate_bits < bits &&
           o[3].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[3].immediate_bits < bits &&
           word(b, sf | (bits == 64u ? 1u << 22u : 0u) |
                       (i->opcode == AST_MACHINE_A64_SBFM ? 0x13000000u
                                                          : 0x53000000u) |
                       ((uint32_t)o[2].immediate_bits << 16u) |
                       ((uint32_t)o[3].immediate_bits << 10u) | (rn << 5u) |
                       rd);
  case AST_MACHINE_A64_CMP:
  case AST_MACHINE_A64_CMP_ZERO:
    if (!encoder_shape(i, 0u, i->opcode == AST_MACHINE_A64_CMP ? 2u : 1u) ||
        !gpr(b, &o[0], &rn, 0))
      return 0;
    if (i->opcode == AST_MACHINE_A64_CMP_ZERO)
      return word(b, sf | 0x7100001fu | (rn << 5u));
    return bits == o[1].bit_width && gpr(b, &o[1], &rm, 0) &&
           word(b, sf | 0x6b00001fu | (rm << 16u) | (rn << 5u));
  case AST_MACHINE_A64_CSET:
    return encoder_shape(i, 1u, 2u) && bits == 32u && gpr(b, &o[0], &rd, 0) &&
           o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[1].immediate_bits < 14u &&
           word(b, 0x1a9f07e0u | (((uint32_t)o[1].immediate_bits ^ 1u) << 12u) |
                       rd);
  case AST_MACHINE_A64_CSEL:
  case AST_MACHINE_A64_FCSEL: {
    int is_fp = i->opcode == AST_MACHINE_A64_FCSEL;
    if (!encoder_shape(i, 1u, 4u) || (bits != 32u && bits != 64u) ||
        bits != o[1].bit_width || bits != o[2].bit_width ||
        o[3].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
        o[3].immediate_bits >= 14u)
      return 0;
    if (is_fp) {
      if (!fp(b, &o[0], &rd) || !fp(b, &o[1], &rn) || !fp(b, &o[2], &rm))
        return 0;
    } else {
      if (!gpr(b, &o[0], &rd, 0) || !gpr(b, &o[1], &rn, 0) ||
          !gpr(b, &o[2], &rm, 0))
        return 0;
    }
    op =
        is_fp ? 0x1e200c00u | (bits == 64u ? 1u << 22u : 0u) : sf | 0x1a800000u;
    return word(b, op | (rm << 16u) | ((uint32_t)o[3].immediate_bits << 12u) |
                       (rn << 5u) | rd);
  }
  case AST_MACHINE_A64_FADD:
  case AST_MACHINE_A64_FSUB:
  case AST_MACHINE_A64_FMUL:
  case AST_MACHINE_A64_FDIV:
    if (!encoder_shape(i, 1u, 3u) || bits != o[1].bit_width ||
        bits != o[2].bit_width || !fp(b, &o[0], &rd) || !fp(b, &o[1], &rn) ||
        !fp(b, &o[2], &rm))
      return 0;
    op = i->opcode == AST_MACHINE_A64_FADD   ? 0x1e202800u
         : i->opcode == AST_MACHINE_A64_FSUB ? 0x1e203800u
         : i->opcode == AST_MACHINE_A64_FMUL ? 0x1e200800u
                                             : 0x1e201800u;
    return word(b, op | (bits == 64u ? 1u << 22u : 0u) | (rm << 16u) |
                       (rn << 5u) | rd);
  case AST_MACHINE_A64_FNEG:
    return encoder_shape(i, 1u, 2u) && bits == o[1].bit_width &&
           fp(b, &o[0], &rd) && fp(b, &o[1], &rn) &&
           word(b,
                0x1e214000u | (bits == 64u ? 1u << 22u : 0u) | (rn << 5u) | rd);
  case AST_MACHINE_A64_FCMP:
    return encoder_shape(i, 0u, 2u) && bits == o[1].bit_width &&
           fp(b, &o[0], &rn) && fp(b, &o[1], &rm) &&
           word(b, 0x1e202000u | (bits == 64u ? 1u << 22u : 0u) | (rm << 16u) |
                       (rn << 5u));
  case AST_MACHINE_A64_FMOV: {
    int to_fp = o[0].bank == AST_MACHINE_BANK_FP;
    if (!encoder_shape(i, 1u, 2u) || bits != o[1].bit_width)
      return 0;
    if (to_fp) {
      if (!fp(b, &o[0], &rd) || !gpr(b, &o[1], &rn, 0))
        return 0;
    } else {
      if (!gpr(b, &o[0], &rd, 0) || !fp(b, &o[1], &rn))
        return 0;
    }
    return word(b, 0x1e260000u | (to_fp ? 1u << 16u : 0u) |
                       (bits == 64u ? 0x80400000u : 0u) | (rn << 5u) | rd);
  }
  case AST_MACHINE_A64_SCVTF:
  case AST_MACHINE_A64_UCVTF:
  case AST_MACHINE_A64_FCVTZS:
  case AST_MACHINE_A64_FCVTZU: {
    int to_fp = i->opcode == AST_MACHINE_A64_SCVTF ||
                i->opcode == AST_MACHINE_A64_UCVTF;
    if (!encoder_shape(i, 1u, 2u) || (bits != 32u && bits != 64u) ||
        (o[1].bit_width != 32u && o[1].bit_width != 64u))
      return 0;
    if (to_fp) {
      if (!fp(b, &o[0], &rd) || !gpr(b, &o[1], &rn, 0))
        return 0;
    } else {
      if (!gpr(b, &o[0], &rd, 0) || !fp(b, &o[1], &rn))
        return 0;
    }
    op = to_fp ? 0x1e220000u : 0x1e380000u;
    if (i->opcode == AST_MACHINE_A64_UCVTF ||
        i->opcode == AST_MACHINE_A64_FCVTZU)
      op |= 1u << 16u;
    if (o[to_fp ? 1 : 0].bit_width == 64u)
      op |= 1u << 31u;
    if (o[to_fp ? 0 : 1].bit_width == 64u)
      op |= 1u << 22u;
    return word(b, op | (rn << 5u) | rd);
  }
  case AST_MACHINE_A64_FCVT:
    return encoder_shape(i, 1u, 2u) && bits != o[1].bit_width &&
           fp(b, &o[0], &rd) && fp(b, &o[1], &rn) &&
           word(b, (bits == 64u ? 0x1e22c000u : 0x1e624000u) | (rn << 5u) | rd);
  case AST_MACHINE_A64_VEC_AND:
  case AST_MACHINE_A64_VEC_OR:
  case AST_MACHINE_A64_VEC_XOR:
    if (!encoder_shape(i, 1u, 3u) || bits != o[1].bit_width ||
        bits != o[2].bit_width || !vec(b, &o[0], &rd) || !vec(b, &o[1], &rn) ||
        !vec(b, &o[2], &rm))
      return 0;
    op = i->opcode == AST_MACHINE_A64_VEC_AND  ? 0x0e201c00u
         : i->opcode == AST_MACHINE_A64_VEC_OR ? 0x0ea01c00u
                                               : 0x2e201c00u;
    return word(b, op | (bits == 128u ? 1u << 30u : 0u) | (rm << 16u) |
                       (rn << 5u) | rd);
  case AST_MACHINE_A64_CALL:
    if (!encoder_shape(i, 0u, 1u) || o[0].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        !word(b, 0x94000000u))
      return 0;
    return encoder_fixup(b, b->out->size - 4u, 4u, AST_ENCODER_FIXUP_A64_CALL26,
                         o[0].module, o[0].symbol);
  case AST_MACHINE_A64_CALL_INDIRECT:
    return encoder_shape(i, 0u, 1u) && bits == 64u && gpr(b, &o[0], &rn, 0) &&
           word(b, 0xd63f0000u | (rn << 5u));
  case AST_MACHINE_A64_NATIVE_CBZ:
  case AST_MACHINE_A64_NATIVE_CBNZ: {
    if (!encoder_shape(i, 0u, 2u) || !gpr(b, &o[0], &rn, 0) ||
        o[1].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
        (o[0].bit_width != 32u && o[0].bit_width != 64u))
      return 0;
    uint64_t raw = o[1].immediate_bits;
    int64_t off = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
    if ((off & 3) || off < -INT64_C(1048576) || off > INT64_C(1048572))
      return 0;
    uint32_t base_op = i->opcode == AST_MACHINE_A64_NATIVE_CBZ
                           ? UINT32_C(0x34000000)
                           : UINT32_C(0x35000000);
    if (o[0].bit_width == 64u)
      base_op |= UINT32_C(0x80000000);
    return word(
        b, base_op | (((uint32_t)((uint64_t)off >> 2u) & 0x7ffffu) << 5u) | rn);
  }
  case AST_MACHINE_A64_NATIVE_BL:
    if (encoder_shape(i, 0u, 1u) &&
        o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint64_t raw = o[0].immediate_bits;
      int64_t off = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
      return !(off & 3) && off >= -INT64_C(134217728) &&
             off <= INT64_C(134217724) &&
             word(b, UINT32_C(0x94000000) |
                         ((uint32_t)((uint64_t)off >> 2u) & 0x03ffffffu));
    }
    return 0;
  case AST_MACHINE_A64_BR:
    if (encoder_shape(i, 0u, 1u) &&
        o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint64_t raw = o[0].immediate_bits;
      int64_t off = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
      return !(off & 3) && off >= -INT64_C(134217728) &&
             off <= INT64_C(134217724) &&
             word(b, 0x14000000u |
                         ((uint32_t)((uint64_t)off >> 2u) & 0x03ffffffu));
    }
    if (!encoder_shape(i, 0u, 1u) || o[0].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    return o[0].block == b->next_block || jump(b, o[0].block);
  case AST_MACHINE_A64_CMP_BR: {
    if (!encoder_shape(i, 0u, 5u) ||
        o[0].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
        o[0].immediate_bits >= 14u ||
        o[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        o[2].kind != AST_PHYSICAL_OPERAND_REGISTER || o[1].bank != o[2].bank ||
        o[1].bit_width != o[2].bit_width ||
        o[3].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        o[4].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    bits = o[1].bit_width;
    if (bits != 32u && bits != 64u)
      return 0;
    if (o[1].bank == AST_MACHINE_BANK_GPR) {
      if (!gpr(b, &o[1], &rn, 0) || !gpr(b, &o[2], &rm, 0) ||
          !word(b, (bits == 64u ? 1u << 31u : 0u) | 0x6b00001fu | (rm << 16u) |
                       (rn << 5u)))
        return 0;
    } else if (o[1].bank == AST_MACHINE_BANK_FP) {
      if (!fp(b, &o[1], &rn) || !fp(b, &o[2], &rm) ||
          !word(b, 0x1e202000u | (bits == 64u ? 1u << 22u : 0u) | (rm << 16u) |
                       (rn << 5u)))
        return 0;
    } else
      return 0;
    uint32_t yes = o[3].block, no = o[4].block,
             cond = (uint32_t)o[0].immediate_bits;
    if (yes == no)
      return yes == b->next_block || jump(b, yes);
    if (no == b->next_block)
      return cond_jump(b, yes, cond);
    if (yes == b->next_block)
      return cond_jump(b, no, cond ^ 1u);
    return cond_jump(b, yes, cond) && jump(b, no);
  }
  case AST_MACHINE_A64_COND_BR: {
    if (!encoder_shape(i, 0u, 2u) || o[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        o[1].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    uint32_t yes = o[0].block, no = o[1].block;
    if (yes == no)
      return yes == b->next_block || jump(b, yes);
    /* CMP_ZERO leaves Z=0 for the true path. Prefer one B.cond when
       either successor is the laid-out fallthrough; otherwise use a
       direct conditional branch plus one long-range B. */
    if (no == b->next_block)
      return cond_jump(b, yes, 1u); /* b.ne */
    if (yes == b->next_block)
      return cond_jump(b, no, 0u); /* b.eq */
    return cond_jump(b, yes, 1u) && jump(b, no);
  }
  default:
    return 0;
  }
}
