#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "../postra/atomic_contract.h"
#include "../postra/x86_system_contract.h"
#include "encoder_internal.h"
#include "regalloc_internal.h"

static unsigned width(const AstPhysicalOperand *o) {
  return o->bit_width == 1u ? 8u : o->bit_width;
}
static int byte(EncoderBuilder *b, uint8_t v) { return encoder_le(b, v, 1u); }
#define X86_HIGH8_FLAG 0x100u
static unsigned reg_code(unsigned r) { return r & 15u; }
static int reg_high8(unsigned r) { return (r & X86_HIGH8_FLAG) != 0u; }
static int gpr(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r) {
  AstTargetRegisterInfo v;
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      o->bank != AST_MACHINE_BANK_GPR || !encoder_reg(b, o, &v) || v.special ||
      v.index >= 16u || v.width != width(o))
    return 0;
  if (v.high8 && width(o) != 8u)
    return 0;
  *r = v.high8 ? (X86_HIGH8_FLAG | (v.index + 4u)) : v.index;
  return width(o) == 8u || width(o) == 16u || width(o) == 32u ||
         width(o) == 64u;
}
static int xmm(EncoderBuilder *b, const AstPhysicalOperand *o, unsigned *r) {
  AstTargetRegisterInfo v;
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      (o->bank != AST_MACHINE_BANK_FP && o->bank != AST_MACHINE_BANK_VECTOR) ||
      !encoder_reg(b, o, &v) || v.width != 128u || v.index >= 16u)
    return 0;
  *r = v.index;
  return o->bit_width == 32u || o->bit_width == 64u || o->bit_width == 128u;
}
static int prefix_ex(EncoderBuilder *b, unsigned bits, unsigned reg,
                     unsigned rm, unsigned index, int force,
                     uint8_t mandatory) {
  int high8 = reg_high8(reg) || reg_high8(rm);
  reg = reg_code(reg);
  rm = reg_code(rm);
  unsigned rex = 0x40u | (bits == 64u ? 8u : 0u) | ((reg >> 3u) << 2u) |
                 (index < 16u ? ((index >> 3u) << 1u) : 0u) | (rm >> 3u);
  if (high8 && (rex != 0x40u || force))
    return 0;
  /* Match the canonical LLVM order for operand-size + LOCK prefixes. */
  if (bits == 16u && mandatory == 0xf0u && !byte(b, 0x66u))
    return 0;
  if (mandatory && !byte(b, mandatory))
    return 0;
  if (bits == 16u && mandatory != 0xf0u && !byte(b, 0x66u))
    return 0;
  return rex == 0x40u && !force ? 1 : byte(b, (uint8_t)rex);
}
static int prefix(EncoderBuilder *b, unsigned bits, unsigned reg, unsigned rm,
                  int force, uint8_t mandatory) {
  return prefix_ex(b, bits, reg, rm, 16u, force, mandatory);
}
/* reg may be a ModR/M opcode extension. Width controls operand size, while
   the register view controls low-byte REX requirements for MOVSX/MOVZX. */
static int rm_op(EncoderBuilder *b, unsigned bits, uint16_t opcode,
                 unsigned reg, const AstPhysicalOperand *rm, uint8_t mandatory,
                 int simd) {
  unsigned base, mod = 3u, index = 16u, scale_bits = 0u;
  AstTargetRegisterInfo ri;
  int memory = rm->kind == AST_PHYSICAL_OPERAND_MEMORY;
  if (memory) {
    if (!encoder_reg(b, rm, &ri) || ri.width != 64u || ri.index >= 16u ||
        ri.special || ri.high8 || rm->displacement < INT32_MIN ||
        rm->displacement > INT32_MAX)
      return 0;
    base = ri.index;
    if (rm->scale) {
      AstPhysicalOperand ix = {0};
      AstTargetRegisterInfo ii;
      ix.kind = AST_PHYSICAL_OPERAND_REGISTER;
      ix.bank = AST_MACHINE_BANK_GPR;
      ix.bit_width = 64u;
      ix.register_unit = rm->index_register_unit;
      memcpy(ix.register_name, rm->index_register_name,
             sizeof(ix.register_name));
      if (!encoder_reg(b, &ix, &ii) || ii.width != 64u || ii.index >= 16u ||
          ii.special || ii.high8)
        return 0;
      if (rm->scale == 1u)
        scale_bits = 0u;
      else if (rm->scale == 2u)
        scale_bits = 1u;
      else if (rm->scale == 4u)
        scale_bits = 2u;
      else if (rm->scale == 8u)
        scale_bits = 3u;
      else
        return 0;
      index = ii.index;
    }
    mod = !rm->displacement && (base & 7u) != 5u                         ? 0u
          : rm->displacement >= INT8_MIN && rm->displacement <= INT8_MAX ? 1u
                                                                         : 2u;
  } else if (simd ? !xmm(b, rm, &base) : !gpr(b, rm, &base))
    return 0;
  int force =
      !simd && ((bits == 8u && !reg_high8(reg) && reg_code(reg) >= 4u) ||
                (!memory && width(rm) == 8u && !reg_high8(base) &&
                 reg_code(base) >= 4u));
  if (!prefix_ex(b, bits, reg, base, index, force, mandatory) ||
      (opcode > 0xffu && !byte(b, (uint8_t)(opcode >> 8u))) ||
      !byte(b, (uint8_t)opcode))
    return 0;
  int sib = memory && (rm->scale || (base & 7u) == 4u);
  if (!byte(b, (uint8_t)((mod << 6u) | ((reg & 7u) << 3u) |
                         (sib ? 4u : base & 7u))))
    return 0;
  if (sib) {
    unsigned sib_index = rm->scale ? (index & 7u) : 4u;
    if (!byte(b,
              (uint8_t)((scale_bits << 6u) | (sib_index << 3u) | (base & 7u))))
      return 0;
  }
  return !memory || !mod
             ? 1
             : encoder_le(b, (uint64_t)rm->displacement, mod == 1u ? 1u : 4u);
}
static int immediate(EncoderBuilder *b, uint64_t value, unsigned bits) {
  if (bits == 64u && value != (uint64_t)(int64_t)(int32_t)(uint32_t)value)
    return 0;
  return encoder_le(b, value, bits == 64u ? 4u : bits / 8u);
}
/* One selected SIMD instruction, using the shortest legal VEX prefix. Legacy
   SSE never receives a redundant operand-size or REX.W prefix. */
static int vector_instruction(EncoderBuilder *b,
                              const AstPhysicalInstructionInfo *i) {
  const VectorForm *f = vector_physical_form(i);
  if (!f || !vector_physical_contract(i) || !vector_features(&b->target, i))
    return 0;
  const AstPhysicalOperand *o = i->operands;
  uint32_t bits = o[0].bit_width;
  int evex = vector_evex(f);
  uint32_t count = f->shape == VECTOR_X86_BINARY ||
                           f->shape == VECTOR_X86_VBINARY ||
                           f->shape == VECTOR_X86_EBINARY
                       ? 3u
                       : 2u;
  const AstPhysicalOperand *rm = &o[count - 1u];
  AstTargetRegisterInfo d, v, r;
  if (!encoder_reg(b, &o[0], &d) || d.index >= (evex ? 32u : 16u) ||
      d.width != bits)
    return 0;
  unsigned base, index = 16u, scale = 0u, mod = 3u, source = 0u;
  int64_t displacement = rm->displacement;
  if (f->shape == VECTOR_X86_VBINARY || f->shape == VECTOR_X86_EBINARY) {
    if (!encoder_reg(b, &o[1], &v) || v.width != bits ||
        v.index >= (evex ? 32u : 16u))
      return 0;
    source = v.index;
  }
  if (!encoder_reg(b, rm, &r))
    return 0;
  base = r.index;
  if (rm->kind == AST_PHYSICAL_OPERAND_MEMORY) {
    if (r.width != 64u || r.special || r.high8 || base >= 16u)
      return 0;
    if (rm->scale) {
      AstPhysicalOperand ix = {0};
      AstTargetRegisterInfo view;
      ix.kind = AST_PHYSICAL_OPERAND_REGISTER;
      ix.bank = AST_MACHINE_BANK_GPR;
      ix.bit_width = 64u;
      ix.register_unit = rm->index_register_unit;
      memcpy(ix.register_name, rm->index_register_name,
             sizeof(ix.register_name));
      if (!encoder_reg(b, &ix, &view) || view.width != 64u ||
          view.index >= 16u || view.index == 4u)
        return 0;
      index = view.index;
      scale = rm->scale == 8u   ? 3u
              : rm->scale == 4u ? 2u
              : rm->scale == 2u ? 1u
                                : 0u;
    }
    mod = !rm->displacement && (base & 7u) != 5u                         ? 0u
          : rm->displacement >= INT8_MIN && rm->displacement <= INT8_MAX ? 1u
                                                                         : 2u;
    if (evex && mod) {
      int64_t tuple = (int64_t)bits / 8;
      if (!(displacement % tuple) && displacement / tuple >= INT8_MIN &&
          displacement / tuple <= INT8_MAX) {
        mod = 1u;
        displacement /= tuple;
      } else
        mod = 2u;
    }
  } else if (r.width != bits || base >= (evex ? 32u : 16u))
    return 0;
  unsigned map = (f->encoding >> 8u) & 3u, pp = (f->encoding >> 10u) & 3u;
  unsigned opcode = f->encoding & 255u;
  if (!i->result_count)
    opcode = (f->encoding >> 16u) & 255u;
  if (evex) {
    if (o[count].kind == AST_PHYSICAL_OPERAND_REGISTER) {
      AstTargetRegisterInfo k;
      if (!encoder_reg(b, &o[count], &k) ||
          k.reg_class != AST_TARGET_REGCLASS_MASK ||
          k.index != o[count].register_unit - 256u)
        return 0;
    }
    unsigned x = rm->kind == AST_PHYSICAL_OPERAND_MEMORY
                     ? (index == 16u ? 0u : (index >> 3u) & 1u)
                     : (base >> 4u) & 1u;
    unsigned p0 = (d.index & 8u ? 0u : 0x80u) | (x ? 0u : 0x40u) |
                  (base & 8u ? 0u : 0x20u) | (d.index & 16u ? 0u : 0x10u) | map;
    unsigned p1 =
        ((f->encoding >> 24u) & 1u) << 7u | ((~source & 15u) << 3u) | 4u | pp;
    unsigned mask = o[count].kind == AST_PHYSICAL_OPERAND_REGISTER
                        ? o[count].register_unit - 256u
                        : 0u;
    unsigned p2 = (o[count + 1u].immediate_bits ? 0x80u : 0u) |
                  (bits == 512u   ? 0x40u
                   : bits == 256u ? 0x20u
                                  : 0u) |
                  (source & 16u ? 0u : 8u) | mask;
    if (!byte(b, 0x62u) || !byte(b, (uint8_t)p0) || !byte(b, (uint8_t)p1) ||
        !byte(b, (uint8_t)p2))
      return 0;
  } else if (vector_vex(f)) {
    unsigned last = ((~source & 15u) << 3u) | (bits == 256u ? 4u : 0u) | pp;
    if (map == 1u && base < 8u && (index == 16u || index < 8u)) {
      if (!byte(b, 0xc5u) ||
          !byte(b, (uint8_t)((d.index < 8u ? 0x80u : 0u) | last)))
        return 0;
    } else if (!byte(b, 0xc4u) ||
               !byte(b, (uint8_t)((d.index < 8u ? 0x80u : 0u) |
                                  (index == 16u || index < 8u ? 0x40u : 0u) |
                                  (base < 8u ? 0x20u : 0u) | map)) ||
               !byte(b, (uint8_t)last))
      return 0;
  } else {
    static const uint8_t prefixes[] = {0u, 0x66u, 0xf3u, 0xf2u};
    if (!prefix_ex(b, 32u, d.index, base, index, 0, prefixes[pp]) ||
        !byte(b, 0x0fu))
      return 0;
    if (map > 1u && !byte(b, map == 2u ? 0x38u : 0x3au))
      return 0;
  }
  int sib = rm->kind == AST_PHYSICAL_OPERAND_MEMORY &&
            (rm->scale || (base & 7u) == 4u);
  if (!byte(b, (uint8_t)opcode) ||
      !byte(b, (uint8_t)((mod << 6u) | ((d.index & 7u) << 3u) |
                         (sib ? 4u : base & 7u))))
    return 0;
  if (sib &&
      !byte(b, (uint8_t)((scale << 6u) | ((rm->scale ? index & 7u : 4u) << 3u) |
                         (base & 7u))))
    return 0;
  return rm->kind != AST_PHYSICAL_OPERAND_MEMORY || !mod ||
         encoder_le(b, (uint64_t)displacement, mod == 1u ? 1u : 4u);
}
static int branch(EncoderBuilder *b, uint32_t target, int cc) {
  if (b->branch_count >= b->branch_capacity)
    return encoder_fail(b, AST_ENCODER_DIAG_INVALID_PHYSICAL,
                        "Too many local branches.");
  size_t index = b->branch_count++;
  int compact = b->short_branches[index] != 0;
  unsigned size = compact ? 2u : cc < 0 ? 5u : 6u;
  b->local_branches[index] =
      (EncoderLocalBranch){b->out->size, target, (uint8_t)size};
  int64_t d;
  if (!encoder_delta(b, target, b->out->size + size, &d))
    return 0;
  if (compact) {
    if (d < INT8_MIN || d > INT8_MAX)
      return encoder_fail(b, AST_ENCODER_DIAG_BRANCH_RANGE,
                          "x86 rel8 branch is out of range.");
    return byte(b, cc < 0 ? 0xebu : (uint8_t)(0x70u + (unsigned)cc)) &&
           encoder_le(b, (uint8_t)d, 1u);
  }
  if (d < INT32_MIN || d > INT32_MAX)
    return encoder_fail(b, AST_ENCODER_DIAG_BRANCH_RANGE,
                        "x86 rel32 branch is out of range.");
  return (cc < 0
              ? byte(b, 0xe9u)
              : byte(b, 0x0fu) && byte(b, (uint8_t)(0x80u + (unsigned)cc))) &&
         encoder_le(b, (uint32_t)d, 4u);
}
static unsigned icmp_cc(AstMachineOpcode opcode, int is_signed) {
  return opcode == AST_MACHINE_X86_ICMP_EQ   ? 4u
         : opcode == AST_MACHINE_X86_ICMP_NE ? 5u
         : opcode == AST_MACHINE_X86_ICMP_LT ? (is_signed ? 12u : 2u)
         : opcode == AST_MACHINE_X86_ICMP_LE ? (is_signed ? 14u : 6u)
         : opcode == AST_MACHINE_X86_ICMP_GT ? (is_signed ? 15u : 7u)
                                             : (is_signed ? 13u : 3u);
}
static int compare_values(EncoderBuilder *b, const AstPhysicalOperand *lhs,
                          const AstPhysicalOperand *rhs) {
  unsigned r, bits = width(lhs);
  if (!gpr(b, lhs, &r) || width(rhs) != bits ||
      (bits != 8u && bits != 16u && bits != 32u && bits != 64u))
    return 0;
  if (rhs->kind == AST_PHYSICAL_OPERAND_REGISTER) {
    unsigned s;
    return gpr(b, rhs, &s) &&
           rm_op(b, bits, bits == 8u ? 0x38u : 0x39u, s, lhs, 0u, 0);
  }
  if (rhs->kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
    return 0;
  /* CMP r/m, imm uses 83 /7 when the wider immediate is representable as
     sign-extended imm8; otherwise 80 /7 for byte or 81 /7 for imm16/32
     (imm32 sign-extended for r/m64). */
  uint64_t mask = bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
  if (bits != 8u &&
      (rhs->immediate_bits & mask) ==
          ((uint64_t)(int64_t)(int8_t)(uint8_t)rhs->immediate_bits & mask))
    return rm_op(b, bits, 0x83u, 7u, lhs, 0u, 0) &&
           byte(b, (uint8_t)rhs->immediate_bits);
  if (!rm_op(b, bits, bits == 8u ? 0x80u : 0x81u, 7u, lhs, 0u, 0))
    return 0;
  return immediate(b, rhs->immediate_bits, bits);
}
static int conditional_targets(EncoderBuilder *b, uint32_t yes, uint32_t no,
                               unsigned cc) {
  if (no == b->next_block)
    return branch(b, yes, (int)cc);
  if (yes == b->next_block)
    return branch(b, no, (int)(cc ^ 1u));
  return branch(b, yes, (int)cc) && branch(b, no, -1);
}
static int move(EncoderBuilder *b, const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  unsigned r, s, bits = width(&o[0]);
  int store = i->opcode == AST_MACHINE_X86_STORE;
  if (!encoder_shape(i, store ? 0u : 1u, 2u) || width(&o[1]) != bits)
    return 0;
  if ((bits == 256u || bits == 512u) && o[0].bank == AST_MACHINE_BANK_VECTOR &&
      !i->nontemporal) {
    AstPhysicalInstructionInfo wide = *i;
    wide.opcode = AST_MACHINE_X86_VECTOR;
    wide.operand_count = 3u;
    if (bits == 512u) {
      wide.operands[2].kind = wide.operands[3].kind =
          AST_PHYSICAL_OPERAND_IMMEDIATE;
      wide.operands[2].bit_width = wide.operands[3].bit_width = 64u;
      wide.operands[2].immediate_bits = wide.operands[3].immediate_bits = 0u;
      wide.operand_count = 5u;
    }
    uint32_t tag = wide.operand_count - 1u;
    wide.operands[tag].kind = AST_PHYSICAL_OPERAND_IMMEDIATE;
    wide.operands[tag].bit_width = 16u;
    wide.operands[tag].immediate_bits =
        bits == 512u ? VECTOR_X86_EVMOVDQU64 : VECTOR_X86_VMOVDQU;
    return vector_instruction(b, &wide);
  }
  if (i->opcode == AST_MACHINE_X86_MOV_IMM) {
    if (!gpr(b, &o[0], &r) || o[1].kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
      return 0;
    if (bits == 64u && o[1].immediate_bits <= UINT32_MAX)
      return prefix(b, 32u, 0u, r, 0, 0u) &&
             byte(b, (uint8_t)(0xb8u + (r & 7u))) &&
             encoder_le(b, o[1].immediate_bits,
                        4u); /* r32 writes zero-extend, preserving flags. */
    if (bits == 64u &&
        o[1].immediate_bits ==
            (uint64_t)(int64_t)(int32_t)(uint32_t)o[1].immediate_bits)
      return rm_op(b, 64u, 0xc7u, 0u, &o[0], 0u, 0) &&
             encoder_le(b, o[1].immediate_bits, 4u);
    return prefix(b, bits, 0u, r,
                  bits == 8u && !reg_high8(r) && reg_code(r) >= 4u, 0u) &&
           byte(b, (uint8_t)((bits == 8u ? 0xb0u : 0xb8u) + (r & 7u))) &&
           encoder_le(b, o[1].immediate_bits, bits / 8u);
  }
  if (i->nontemporal) {
    if (!store || o[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        o[1].kind != AST_PHYSICAL_OPERAND_MEMORY)
      return 0;
    if (gpr(b, &o[0], &r) && (bits == 32u || bits == 64u))
      return rm_op(b, bits, 0x0fc3u, r, &o[1], 0u,
                   0); /* MOVNTI m32/64, r32/64 */
    if (xmm(b, &o[0], &r) && bits == 128u)
      return rm_op(b, 32u, 0x0fe7u, r, &o[1], 0x66u, 1); /* MOVNTDQ m128, xmm */
    return 0;
  }
  if (store && o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    if (o[1].kind != AST_PHYSICAL_OPERAND_MEMORY)
      return 0;
    return rm_op(b, bits, bits == 8u ? 0xc6u : 0xc7u, 0u, &o[1], 0u, 0) &&
           immediate(b, o[0].immediate_bits, bits);
  }
  if (gpr(b, &o[0], &r)) {
    if (store)
      return o[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
             rm_op(b, bits, bits == 8u ? 0x88u : 0x89u, r, &o[1], 0u, 0);
    if (i->opcode == AST_MACHINE_X86_MOV) {
      return gpr(b, &o[1], &s) &&
             rm_op(b, bits, bits == 8u ? 0x88u : 0x89u, s, &o[0], 0u, 0);
    }
    return o[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
           rm_op(b, bits, bits == 8u ? 0x8au : 0x8bu, r, &o[1], 0u, 0);
  }
  if (!xmm(b, &o[0], &r))
    return 0;
  if (store && o[1].kind != AST_PHYSICAL_OPERAND_MEMORY)
    return 0;
  uint16_t op =
      bits == 128u ? (store ? 0x0f7fu : 0x0f6fu) : (store ? 0x0f11u : 0x0f10u);
  return rm_op(b, 32u, op, r, &o[1], bits == 64u ? 0xf2u : 0xf3u, 1);
}
static AstPhysicalOperand atomic_x86_view(AstPhysicalOperand r, unsigned bits) {
  /* Register names are validated as 64-bit before creating narrower views. */
  static const char *const names[4][16] = {
      {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b",
       "r11b", "r12b", "r13b", "r14b", "r15b"},
      {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w",
       "r11w", "r12w", "r13w", "r14w", "r15w"},
      {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d",
       "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"},
      {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9",
       "r10", "r11", "r12", "r13", "r14", "r15"}};
  r.bit_width = bits;
  const char *name = names[bits == 8u    ? 0u
                           : bits == 16u ? 1u
                           : bits == 32u ? 2u
                                         : 3u][r.register_unit % 64u];
  _Static_assert(AST_MACHINE_REGISTER_NAME_CAP >= 5u,
                 "x86 register name buffer is too small");
  memcpy(r.register_name, name, strlen(name) + 1u);
  return r;
}
static int atomic_x86(EncoderBuilder *b, const AstPhysicalInstructionInfo *i) {
  if (!atomic_physical_contract(i))
    return 0;
  unsigned r[5];
  for (unsigned j = 0; j < 5; ++j)
    if (!gpr(b, &i->operands[j], &r[j]) || r[j] == 4u || r[j] == 5u)
      return 0;
  unsigned code = (unsigned)i->operands[5].immediate_bits, op = code & 255u,
           bits = code >> 8u;
  if (bits == 1u)
    bits = 8u;
  AstPhysicalOperand a = atomic_x86_view(i->operands[0], bits),
                     t = atomic_x86_view(i->operands[1], bits);
  AstPhysicalOperand e = atomic_x86_view(i->operands[3], bits),
                     v = atomic_x86_view(i->operands[4], bits);
  AstPhysicalOperand mem = i->operands[2];
  mem.kind = AST_PHYSICAL_OPERAND_MEMORY;
  mem.bit_width = bits;
  if (op == AT_LOAD)
    return rm_op(b, bits, bits == 8 ? 0x8a : 0x8b, r[0], &mem, 0, 0);
  if (op == AT_STORE) {
    if (i->memory_order == AST_MEMORY_ORDER_SEQ_CST)
      return rm_op(b, bits, bits == 8 ? 0x86 : 0x87, r[4], &mem, 0, 0);
    return rm_op(b, bits, bits == 8 ? 0x88 : 0x89, r[4], &mem, 0, 0);
  }
  if (op == AT_XCHG)
    return rm_op(b, bits, bits == 8 ? 0x86 : 0x87, r[4], &mem, 0, 0) &&
           rm_op(b, bits, bits == 8 ? 0x88 : 0x89, r[4], &a, 0, 0);
  if (op == AT_CAS) {
    return rm_op(b, bits, bits == 8 ? 0x88 : 0x89, r[3], &a, 0, 0) &&
           rm_op(b, bits, bits == 8 ? 0x0fb0 : 0x0fb1, r[4], &mem, 0xf0, 0) &&
           rm_op(b, 8, 0x0f94, 0,
                 &(AstPhysicalOperand){.kind = AST_PHYSICAL_OPERAND_REGISTER,
                                       .bank = AST_MACHINE_BANK_GPR,
                                       .bit_width = 8,
                                       .register_unit = a.register_unit,
                                       .register_name = "al"},
                 0, 0);
  }
  (void)e;
  if (op == AT_ADD || op == AT_SUB) {
    if (op == AT_SUB && !rm_op(b, bits, bits == 8 ? 0xf6 : 0xf7, 3, &v, 0, 0))
      return 0;
    return rm_op(b, bits, bits == 8 ? 0x0fc0 : 0x0fc1, r[4], &mem, 0xf0, 0) &&
           rm_op(b, bits, bits == 8 ? 0x88 : 0x89, r[4], &a, 0, 0);
  }
  if (!rm_op(b, bits, bits == 8 ? 0x8a : 0x8b, r[0], &mem, 0, 0))
    return 0;
  size_t loop = b->out->size;
  uint16_t arithmetic = op == AT_ADD   ? 0x01
                        : op == AT_SUB ? 0x29
                        : op == AT_AND ? 0x21
                        : op == AT_OR  ? 0x09
                                       : 0x31;
  if (!rm_op(b, bits, bits == 8 ? 0x88 : 0x89, r[0], &t, 0, 0) ||
      !rm_op(b, bits, bits == 8 ? arithmetic - 1 : arithmetic, r[4], &t, 0,
             0) ||
      !rm_op(b, bits, bits == 8 ? 0x0fb0 : 0x0fb1, r[1], &mem, 0xf0, 0))
    return 0;
  int64_t d = (int64_t)loop - (int64_t)(b->out->size + 2);
  return d >= -128 && byte(b, 0x75) && byte(b, (uint8_t)d);
}
static int test_condition(EncoderBuilder *b, const AstPhysicalOperand *o) {
  unsigned r, bits = width(o);
  if (o->kind == AST_PHYSICAL_OPERAND_MEMORY)
    return (bits == 8u || bits == 16u || bits == 32u || bits == 64u) &&
           rm_op(b, bits, bits == 8u ? 0x80u : 0x83u, 7u, o, 0u, 0) &&
           byte(b, 0u);
  return gpr(b, o, &r) &&
         rm_op(b, bits, bits == 8u ? 0x84u : 0x85u, r, o, 0u, 0);
}
static int select_move(EncoderBuilder *b, AstPhysicalOperand dst,
                       AstPhysicalOperand src) {
  AstPhysicalInstructionInfo copy = {0};
  copy.opcode = src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE
                    ? AST_MACHINE_X86_MOV_IMM
                : src.kind == AST_PHYSICAL_OPERAND_MEMORY ? AST_MACHINE_X86_LOAD
                                                          : AST_MACHINE_X86_MOV;
  copy.result_count = 1u;
  copy.operand_count = 2u;
  copy.operands[0] = dst;
  copy.operands[1] = src;
  return move(b, &copy);
}
static int patch_short(EncoderBuilder *b, size_t field) {
  if (field >= b->out->size || b->out->size - field - 1u > 127u)
    return 0;
  b->out->bytes[field] = (uint8_t)(b->out->size - field - 1u);
  return 1;
}
static int fp_cast(EncoderBuilder *b, AstMachineOpcode op,
                   const AstPhysicalOperand *dst,
                   const AstPhysicalOperand *src) {
  unsigned rd, rs;
  if ((width(dst) != 32u && width(dst) != 64u) ||
      (width(src) != 32u && width(src) != 64u))
    return 0;
  if (op == AST_MACHINE_X86_FMOV_BITS) {
    if (width(dst) != width(src))
      return 0;
    if (dst->bank == AST_MACHINE_BANK_FP && xmm(b, dst, &rd) &&
        gpr(b, src, &rs))
      return rm_op(b, width(src), 0x0f6eu, rd, src, 0x66u, 0);
    if (src->bank == AST_MACHINE_BANK_FP && xmm(b, src, &rs) &&
        gpr(b, dst, &rd))
      return rm_op(b, width(dst), 0x0f7eu, rs, dst, 0x66u, 0);
    return 0;
  }
  if (op == AST_MACHINE_X86_SINT_TO_FP)
    return dst->bank == AST_MACHINE_BANK_FP && xmm(b, dst, &rd) &&
           gpr(b, src, &rs) &&
           rm_op(b, width(src), 0x0f2au, rd, src,
                 width(dst) == 64u ? 0xf2u : 0xf3u, 0);
  if (op == AST_MACHINE_X86_FP_TO_SINT)
    return src->bank == AST_MACHINE_BANK_FP && xmm(b, src, &rs) &&
           gpr(b, dst, &rd) &&
           rm_op(b, width(dst), 0x0f2cu, rd, src,
                 width(src) == 64u ? 0xf2u : 0xf3u, 1);
  return op == AST_MACHINE_X86_FP_CONVERT && dst->bank == AST_MACHINE_BANK_FP &&
         src->bank == AST_MACHINE_BANK_FP && width(dst) != width(src) &&
         xmm(b, dst, &rd) && xmm(b, src, &rs) &&
         rm_op(b, 32u, 0x0f5au, rd, src, width(src) == 64u ? 0xf2u : 0xf3u, 1);
}
static int unsigned_cast(EncoderBuilder *b,
                         const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  unsigned rd, scratch, src;
  if (i->opcode == AST_MACHINE_X86_UINT64_TO_FP) {
    if (!encoder_shape(i, 2u, 3u) || o[0].bank != AST_MACHINE_BANK_FP ||
        !xmm(b, &o[0], &rd) || !gpr(b, &o[1], &scratch) ||
        width(&o[1]) != 64u || !gpr(b, &o[2], &src) || width(&o[2]) != 64u ||
        scratch == src)
      return 0;
    if (!test_condition(b, &o[2]) || !byte(b, 0x78u) || !byte(b, 0))
      return 0;
    size_t high = b->out->size - 1;
    if (!fp_cast(b, AST_MACHINE_X86_SINT_TO_FP, &o[0], &o[2]) ||
        !byte(b, 0xebu) || !byte(b, 0))
      return 0;
    size_t end = b->out->size - 1;
    /* Round once: convert ((x >> 1) | (x & 1)), then double exactly.
       The carry from SHR carries the discarded bit; OR avoids double rounding.
     */
    if (!patch_short(b, high) || !select_move(b, o[1], o[2]) ||
        !rm_op(b, 64u, 0xd1u, 5u, &o[1], 0, 0) || !byte(b, 0x73u) ||
        !byte(b, 0))
      return 0;
    size_t even = b->out->size - 1;
    if (!rm_op(b, 64u, 0x83u, 1u, &o[1], 0, 0) || !byte(b, 1) ||
        !patch_short(b, even) ||
        !fp_cast(b, AST_MACHINE_X86_SINT_TO_FP, &o[0], &o[1]) ||
        !rm_op(b, 32u, 0x0f58u, rd, &o[0], width(&o[0]) == 64u ? 0xf2u : 0xf3u,
               1))
      return 0;
    return patch_short(b, end);
  }
  unsigned temp;
  if (!encoder_shape(i, 3u, 4u) || !gpr(b, &o[0], &rd) || width(&o[0]) != 64u ||
      !gpr(b, &o[1], &scratch) || width(&o[1]) != 64u || scratch == rd ||
      o[2].bank != AST_MACHINE_BANK_FP || o[3].bank != AST_MACHINE_BANK_FP ||
      !xmm(b, &o[2], &temp) || !xmm(b, &o[3], &src) || temp == src ||
      width(&o[2]) != width(&o[3]) ||
      (width(&o[2]) != 32u && width(&o[2]) != 64u))
    return 0;
  AstPhysicalOperand constant = {0};
  constant.kind = AST_PHYSICAL_OPERAND_IMMEDIATE;
  constant.bit_width = 64u;
  constant.immediate_bits =
      width(&o[2]) == 64u ? UINT64_C(0x43e0000000000000) : UINT64_C(0x5f000000);
  AstPhysicalOperand raw = o[1];
  if (width(&o[2]) == 32u) {
    raw.bit_width = 32u;
    if (!machine_physreg_name(&b->target, AST_MACHINE_BANK_GPR, scratch, 32u,
                              raw.register_name))
      return 0;
  }
  if (!select_move(b, o[1], constant) ||
      !fp_cast(b, AST_MACHINE_X86_FMOV_BITS, &o[2], &raw) ||
      !rm_op(b, 32u, 0x0f2eu, src, &o[2], width(&o[2]) == 64u ? 0x66u : 0, 1) ||
      !byte(b, 0x72u) || !byte(b, 0))
    return 0;
  size_t low = b->out->size - 1;
  /* For [2^63,2^64), 2^63-x is exactly representable and fits signed i64. */
  if (!rm_op(b, 32u, 0x0f5cu, temp, &o[3], width(&o[2]) == 64u ? 0xf2u : 0xf3u,
             1) ||
      !fp_cast(b, AST_MACHINE_X86_FP_TO_SINT, &o[0], &o[2]) ||
      !rm_op(b, 64u, 0xf7u, 3u, &o[0], 0, 0) ||
      !rm_op(b, 64u, 0x0fbau, 5u, &o[0], 0, 0) || !byte(b, 63) ||
      !byte(b, 0xebu) || !byte(b, 0))
    return 0;
  size_t end = b->out->size - 1;
  return patch_short(b, low) &&
         fp_cast(b, AST_MACHINE_X86_FP_TO_SINT, &o[0], &o[3]) &&
         patch_short(b, end);
}

static int system_instruction(EncoderBuilder *b,
                              const AstPhysicalInstructionInfo *i) {
  const SystemForm *f = system_physical_form(i);
  if (!f || (f->feature & ~b->target.features) || !system_physical_contract(i))
    return 0;
  const AstPhysicalOperand *o = i->operands;
  unsigned r = 0u;
  for (uint32_t j = 0; j + 1u < i->operand_count; ++j)
    if (o[j].kind == AST_PHYSICAL_OPERAND_REGISTER && !gpr(b, &o[j], &r))
      return 0;
  switch (f->shape) {
  case SYSTEM_SHAPE_FIXED:
  case SYSTEM_SHAPE_IMPLICIT:
    return encoder_le(b, f->encoding, f->encoding > 0xffffu ? 3u : 2u);
  case SYSTEM_SHAPE_X86_READ_SPECIAL:
  case SYSTEM_SHAPE_X86_WRITE_SPECIAL: {
    unsigned special = (unsigned)o[1].immediate_bits;
    if (!gpr(b, &o[0], &r))
      return 0;
    uint8_t rex = (uint8_t)(0x40u | ((special >> 3u) << 2u) | (r >> 3u));
    return (rex == 0x40u || byte(b, rex)) && byte(b, 0x0fu) &&
           byte(b, (uint8_t)f->encoding) &&
           byte(b, (uint8_t)(0xc0u | ((special & 7u) << 3u) | (r & 7u)));
  }
  case SYSTEM_SHAPE_X86_ADDRESS: {
    AstPhysicalOperand memory = o[0];
    memory.kind = AST_PHYSICAL_OPERAND_MEMORY;
    memory.bit_width = f == &system_forms[SYSTEM_X86_INVLPG] ? 8u : 80u;
    return rm_op(b, 32u, 0x0f01u, f->encoding, &memory, 0u, 0);
  }
  case SYSTEM_SHAPE_X86_SELECTOR_READ:
  case SYSTEM_SHAPE_X86_SELECTOR_WRITE:
    return rm_op(b, f->shape == SYSTEM_SHAPE_X86_SELECTOR_READ ? 16u : 32u,
                 0x0f00u, f->encoding, &o[0], 0u, 0);
  default:
    return 0;
  }
}
int encoder_x86_instruction(EncoderBuilder *b,
                            const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  unsigned r, s, bits = width(&o[0]);
  switch (i->opcode) {
  case AST_MACHINE_X86_SYSTEM:
    return system_instruction(b, i);
  case AST_MACHINE_X86_VECTOR:
    return vector_instruction(b, i);
  case AST_MACHINE_X86_LOCKED_ATOMIC:
    return atomic_x86(b, i);
  case AST_MACHINE_X86_IN:
  case AST_MACHINE_X86_OUT: {
    if (!x86_system_contract(i) || !gpr(b, &o[0], &r) || r != 0u)
      return 0;
    int input = i->opcode == AST_MACHINE_X86_IN;
    int immediate_port = o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE;
    if (!immediate_port && (!gpr(b, &o[1], &s) || s != 2u))
      return 0;
    uint8_t op = (uint8_t)((input ? 0xe4u : 0xe6u) + (bits == 8u ? 0u : 1u) +
                           (immediate_port ? 0u : 8u));
    return (bits != 16u || byte(b, 0x66u)) && byte(b, op) &&
           (!immediate_port || byte(b, (uint8_t)o[1].immediate_bits));
  }
  case AST_MACHINE_X86_LIDT:
  case AST_MACHINE_X86_LGDT: {
    if (!x86_system_contract(i) || !gpr(b, &o[0], &r))
      return 0;
    AstPhysicalOperand memory = o[0];
    memory.kind = AST_PHYSICAL_OPERAND_MEMORY;
    memory.bit_width = 80u;
    memory.displacement = 0;
    return rm_op(b, 32u, 0x0f01u, i->opcode == AST_MACHINE_X86_LIDT ? 3u : 2u,
                 &memory, 0u, 0);
  }
  case AST_MACHINE_X86_NOP:
    return encoder_shape(i, 0u, 0u) && byte(b, 0x90u);
  case AST_MACHINE_X86_CLI:
    return encoder_shape(i, 0u, 0u) && byte(b, 0xfau);
  case AST_MACHINE_X86_STI:
    return encoder_shape(i, 0u, 0u) && byte(b, 0xfbu);
  case AST_MACHINE_X86_HLT:
    return encoder_shape(i, 0u, 0u) && byte(b, 0xf4u);
  case AST_MACHINE_X86_CLD:
    return encoder_shape(i, 0u, 0u) && byte(b, 0xfcu);

  case AST_MACHINE_X86_RET:
    return encoder_shape(i, 0u, 0u) && byte(b, 0xc3u);
  case AST_MACHINE_X86_TRAP:
    return encoder_shape(i, 0u, 0u) && byte(b, 0x0fu) && byte(b, 0x0bu);
  case AST_MACHINE_X86_FENCE:
    return encoder_shape(i, 0u, 0u) && byte(b, 0x0fu) && byte(b, 0xaeu) &&
           byte(b, 0xf0u);
  case AST_MACHINE_X86_MOV:
  case AST_MACHINE_X86_MOV_IMM:
  case AST_MACHINE_X86_LOAD:
  case AST_MACHINE_X86_STORE:
    return move(b, i);
  case AST_MACHINE_X86_FMOV_BITS:
  case AST_MACHINE_X86_SINT_TO_FP:
  case AST_MACHINE_X86_FP_TO_SINT:
  case AST_MACHINE_X86_FP_CONVERT:
    return encoder_shape(i, 1u, 2u) && fp_cast(b, i->opcode, &o[0], &o[1]);
  case AST_MACHINE_X86_UINT64_TO_FP:
  case AST_MACHINE_X86_FP_TO_UINT64:
    return unsigned_cast(b, i);
  case AST_MACHINE_X86_LEA:
    if (!encoder_shape(i, 1u, 2u) || !gpr(b, &o[0], &r) || bits != 64u)
      return 0;
    if (o[1].kind == AST_PHYSICAL_OPERAND_MEMORY)
      return rm_op(b, bits, 0x8du, r, &o[1], 0u, 0);
    if (o[1].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        !prefix(b, 64u, r, 0u, 0, 0u) || !byte(b, 0x8du) ||
        !byte(b, (uint8_t)(5u | ((r & 7u) << 3u))) || !encoder_le(b, 0u, 4u))
      return 0;
    return encoder_fixup(b, b->out->size - 4u, 4u, AST_ENCODER_FIXUP_PC_REL32,
                         o[1].module, o[1].symbol);
  case AST_MACHINE_X86_ADD:
  case AST_MACHINE_X86_SUB:
  case AST_MACHINE_X86_AND:
  case AST_MACHINE_X86_OR:
  case AST_MACHINE_X86_XOR: {
    if (!encoder_shape(i, 1u, 3u) || !gpr(b, &o[0], &r) ||
        !encoder_same(&o[0], &o[1]) || width(&o[2]) != bits)
      return 0;
    unsigned ext = i->opcode == AST_MACHINE_X86_ADD   ? 0u
                   : i->opcode == AST_MACHINE_X86_OR  ? 1u
                   : i->opcode == AST_MACHINE_X86_AND ? 4u
                   : i->opcode == AST_MACHINE_X86_SUB ? 5u
                                                      : 6u;
    if (gpr(b, &o[2], &s))
      return rm_op(b, bits, (uint16_t)(ext * 8u + (bits == 8u ? 0u : 1u)), s,
                   &o[0], 0u, 0);
    if (o[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
      return 0;
    uint64_t mask = bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
    if (bits != 8u &&
        (o[2].immediate_bits & mask) ==
            ((uint64_t)(int64_t)(int8_t)(uint8_t)o[2].immediate_bits & mask))
      return rm_op(b, bits, 0x83u, ext, &o[0], 0u, 0) &&
             byte(b, (uint8_t)o[2].immediate_bits);
    return rm_op(b, bits, bits == 8u ? 0x80u : 0x81u, ext, &o[0], 0u, 0) &&
           immediate(b, o[2].immediate_bits, bits == 64u ? 32u : bits);
  }
  case AST_MACHINE_X86_MUL:
    if (!encoder_shape(i, 1u, 3u) || bits == 8u ||
        !encoder_same(&o[0], &o[1]) || width(&o[2]) != bits ||
        !gpr(b, &o[0], &r))
      return 0;
    if (gpr(b, &o[2], &s))
      return rm_op(b, bits, 0x0fafu, r, &o[2], 0u, 0);
    if (o[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
      return 0;
    if (bits == 64u &&
        o[2].immediate_bits !=
            (uint64_t)(int64_t)(int32_t)(uint32_t)o[2].immediate_bits)
      return 0;
    if ((o[2].immediate_bits &
         (bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u)) ==
        ((uint64_t)(int64_t)(int8_t)(uint8_t)o[2].immediate_bits &
         (bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u)))
      return rm_op(b, bits, 0x6bu, r, &o[1], 0u, 0) &&
             byte(b, (uint8_t)o[2].immediate_bits);
    return rm_op(b, bits, 0x69u, r, &o[1], 0u, 0) &&
           immediate(b, o[2].immediate_bits, bits == 64u ? 32u : bits);
  case AST_MACHINE_X86_UMUL_WIDE:
    /* x86 MUL r/m64 implicitly writes both RDX and RAX.  The two
       implicit outputs must be represented in Physical IR and
       checked independently, not inferred from a generic IMUL. */
    if (!encoder_shape(i, 2u, 4u) || (bits != 32u && bits != 64u) ||
        !gpr(b, &o[0], &r) || r != 0u || !gpr(b, &o[1], &s) || s != 2u ||
        !encoder_same(&o[0], &o[2]) || width(&o[1]) != bits ||
        o[3].kind != AST_PHYSICAL_OPERAND_REGISTER || width(&o[3]) != bits)
      return 0;
    return rm_op(b, bits, 0xf7u, 4u, &o[3], 0u, 0);
  case AST_MACHINE_X86_NEG:
  case AST_MACHINE_X86_NOT:
    return encoder_shape(i, 1u, 2u) && encoder_same(&o[0], &o[1]) &&
           gpr(b, &o[0], &r) &&
           rm_op(b, bits, bits == 8u ? 0xf6u : 0xf7u,
                 i->opcode == AST_MACHINE_X86_NEG ? 3u : 2u, &o[0], 0u, 0);
  case AST_MACHINE_X86_SHL:
  case AST_MACHINE_X86_ASHR:
  case AST_MACHINE_X86_LSHR: {
    if (!encoder_shape(i, 1u, 3u) || !encoder_same(&o[0], &o[1]))
      return 0;
    unsigned ext = i->opcode == AST_MACHINE_X86_SHL    ? 4u
                   : i->opcode == AST_MACHINE_X86_ASHR ? 7u
                                                       : 5u;
    if (gpr(b, &o[2], &r))
      return r == 1u && width(&o[2]) == 8u &&
             rm_op(b, bits, bits == 8u ? 0xd2u : 0xd3u, ext, &o[0], 0u, 0);
    return o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           rm_op(b, bits, bits == 8u ? 0xc0u : 0xc1u, ext, &o[0], 0u, 0) &&
           byte(b, (uint8_t)o[2].immediate_bits);
  }
  case AST_MACHINE_X86_CDQ:
  case AST_MACHINE_X86_CQO:
    return encoder_shape(i, 1u, 2u) && gpr(b, &o[0], &r) && r == 2u &&
           gpr(b, &o[1], &s) && s == 0u &&
           bits == (i->opcode == AST_MACHINE_X86_CQO ? 64u : 32u) &&
           width(&o[1]) == bits && prefix(b, bits, 0u, 0u, 0, 0u) &&
           byte(b, 0x99u);
  case AST_MACHINE_X86_SDIV:
  case AST_MACHINE_X86_UDIV:
    if (!encoder_shape(i, 2u, 5u) || (bits != 32u && bits != 64u) ||
        !encoder_same(&o[0], &o[2]) || !encoder_same(&o[1], &o[3]) ||
        !gpr(b, &o[0], &r) || r != 0u || !gpr(b, &o[1], &s) || s != 2u ||
        width(&o[1]) != bits || width(&o[4]) != bits)
      return 0;
    return rm_op(b, bits, 0xf7u, i->opcode == AST_MACHINE_X86_SDIV ? 7u : 6u,
                 &o[4], 0u, 0);
  case AST_MACHINE_X86_MOVSX:
  case AST_MACHINE_X86_MOVZX: {
    unsigned from = width(&o[1]);
    if (!encoder_shape(i, 1u, 2u) || !gpr(b, &o[0], &r) || bits <= from ||
        (from != 8u && from != 16u && from != 32u))
      return 0;
    if (from == 32u && i->opcode == AST_MACHINE_X86_MOVZX) {
      if (gpr(b, &o[1], &s))
        return rm_op(b, 32u, 0x89u, s, &o[0], 0u, 0);
      return rm_op(b, 32u, 0x8bu, r, &o[1], 0u, 0);
    }
    uint16_t op =
        from == 32u
            ? 0x63u
            : (uint16_t)((i->opcode == AST_MACHINE_X86_MOVSX ? 0x0fbeu
                                                             : 0x0fb6u) +
                         (from == 16u ? 1u : 0u));
    return rm_op(b, bits, op, r, &o[1], 0u, 0);
  }
  case AST_MACHINE_X86_ICMP_EQ:
  case AST_MACHINE_X86_ICMP_NE:
  case AST_MACHINE_X86_ICMP_LT:
  case AST_MACHINE_X86_ICMP_LE:
  case AST_MACHINE_X86_ICMP_GT:
  case AST_MACHINE_X86_ICMP_GE: {
    if (!encoder_shape(i, 1u, 3u) || !gpr(b, &o[0], &r) || bits != 8u ||
        !compare_values(b, &o[1], &o[2]))
      return 0;
    unsigned cc = icmp_cc(i->opcode, i->is_signed);
    return rm_op(b, 8u, (uint16_t)(0x0f90u + cc), 0u, &o[0], 0u, 0);
  }
  case AST_MACHINE_X86_FADD:
  case AST_MACHINE_X86_FSUB:
  case AST_MACHINE_X86_FMUL:
  case AST_MACHINE_X86_FDIV: {
    if (!encoder_shape(i, 1u, 3u) || !encoder_same(&o[0], &o[1]) ||
        !xmm(b, &o[0], &r) || o[0].bank != AST_MACHINE_BANK_FP ||
        (bits != 32u && bits != 64u) || width(&o[2]) != bits)
      return 0;
    uint16_t op = i->opcode == AST_MACHINE_X86_FADD   ? 0x0f58u
                  : i->opcode == AST_MACHINE_X86_FSUB ? 0x0f5cu
                  : i->opcode == AST_MACHINE_X86_FMUL ? 0x0f59u
                                                      : 0x0f5eu;
    return rm_op(b, 32u, op, r, &o[2], bits == 64u ? 0xf2u : 0xf3u, 1);
  }
  case AST_MACHINE_X86_FCMP_EQ:
  case AST_MACHINE_X86_FCMP_NE:
  case AST_MACHINE_X86_FCMP_LT:
  case AST_MACHINE_X86_FCMP_LE:
  case AST_MACHINE_X86_FCMP_GT:
  case AST_MACHINE_X86_FCMP_GE: {
    if (!encoder_shape(i, 1u, 3u) || bits != 8u || !gpr(b, &o[0], &r) ||
        o[1].bank != AST_MACHINE_BANK_FP || width(&o[1]) != width(&o[2]) ||
        (width(&o[1]) != 32u && width(&o[1]) != 64u) || !xmm(b, &o[1], &s) ||
        !rm_op(b, 32u, 0x0f2eu, s, &o[2], width(&o[1]) == 64u ? 0x66u : 0u, 1))
      return 0;
    unsigned cc = i->opcode == AST_MACHINE_X86_FCMP_EQ   ? 4u
                  : i->opcode == AST_MACHINE_X86_FCMP_NE ? 5u
                  : i->opcode == AST_MACHINE_X86_FCMP_LT ? 2u
                  : i->opcode == AST_MACHINE_X86_FCMP_LE ? 6u
                  : i->opcode == AST_MACHINE_X86_FCMP_GT ? 7u
                                                         : 3u;
    if (cc == 7u || cc == 3u)
      return rm_op(b, 8u, (uint16_t)(0x0f90u + cc), 0u, &o[0], 0u, 0);
    /* Unordered comparisons set PF. Preserve IEEE ordered semantics
       without borrowing an undeclared scratch register. SETcc keeps flags. */
    if (!rm_op(b, 8u, i->opcode == AST_MACHINE_X86_FCMP_NE ? 0x0f9au : 0x0f9bu,
               0u, &o[0], 0u, 0) ||
        !byte(b, 0x7au) || !byte(b, 0u))
      return 0;
    size_t skip = b->out->size - 1u;
    return rm_op(b, 8u, (uint16_t)(0x0f90u + cc), 0u, &o[0], 0u, 0) &&
           patch_short(b, skip);
  }
  case AST_MACHINE_X86_SELECT: {
    if (!encoder_shape(i, 1u, 4u) || width(&o[2]) != bits ||
        width(&o[3]) != bits)
      return 0;
    if (o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
      return select_move(b, o[0], o[o[1].immediate_bits ? 2 : 3]);
    /* Register alternatives are already evaluated and cannot fault. TEST
       must precede the initial MOV: the condition may alias the destination.
       Keep memory alternatives on the branch path so an unselected address
       is never speculatively read. CMOV has no byte-register form. */
    unsigned yes, no;
    if ((bits == 16u || bits == 32u || bits == 64u) && gpr(b, &o[0], &r) &&
        gpr(b, &o[2], &yes) && gpr(b, &o[3], &no)) {
      if (!test_condition(b, &o[1]))
        return 0;
      if (r == yes)
        return rm_op(b, bits, 0x0f44u, r, &o[3], 0u, 0);
      return (r == no || select_move(b, o[0], o[3])) &&
             rm_op(b, bits, 0x0f45u, r, &o[2], 0u, 0);
    }
    if (!test_condition(b, &o[1]) || !byte(b, 0x74u) || !byte(b, 0u))
      return 0;
    size_t otherwise = b->out->size - 1u;
    if (!select_move(b, o[0], o[2]) || !byte(b, 0xebu) || !byte(b, 0u))
      return 0;
    size_t end = b->out->size - 1u;
    return patch_short(b, otherwise) && select_move(b, o[0], o[3]) &&
           patch_short(b, end);
  }
  case AST_MACHINE_X86_POPCNT:
    return (b->target.features & AST_TARGET_FEATURE_X86_POPCNT) &&
           encoder_shape(i, 1u, 2u) && gpr(b, &o[0], &r) && bits != 8u &&
           width(&o[1]) == bits && rm_op(b, bits, 0x0fb8u, r, &o[1], 0xf3u, 0);
  case AST_MACHINE_X86_BSF:
  case AST_MACHINE_X86_BSR:
    return encoder_shape(i, 1u, 2u) &&
           (bits == 16u || bits == 32u || bits == 64u) && gpr(b, &o[0], &r) &&
           width(&o[1]) == bits &&
           rm_op(b, bits, i->opcode == AST_MACHINE_X86_BSF ? 0x0fbcu : 0x0fbdu,
                 r, &o[1], 0u, 0);
  case AST_MACHINE_X86_BSWAP:
    return encoder_shape(i, 1u, 2u) && encoder_same(&o[0], &o[1]) &&
           (bits == 32u || bits == 64u) && gpr(b, &o[0], &r) &&
           /* BSWAP's register lives in opcode +rd, extended by REX.B. */
           prefix(b, bits, 0u, r, 0, 0u) && byte(b, 0x0fu) &&
           byte(b, 0xc8u + (r & 7u));
  case AST_MACHINE_X86_ROL:
  case AST_MACHINE_X86_ROR:
    return encoder_shape(i, 1u, 3u) && encoder_same(&o[0], &o[1]) &&
           (bits == 8u || bits == 16u || bits == 32u || bits == 64u) &&
           gpr(b, &o[2], &r) && r == 1u && width(&o[2]) == 8u &&
           rm_op(b, bits, bits == 8u ? 0xd2u : 0xd3u,
                 i->opcode == AST_MACHINE_X86_ROR ? 1u : 0u, &o[0], 0u, 0);
  case AST_MACHINE_X86_CALL:
    if (!encoder_shape(i, 0u, 1u) || o[0].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        !byte(b, 0xe8u) || !encoder_le(b, 0u, 4u))
      return 0;
    return encoder_fixup(b, b->out->size - 4u, 4u,
                         AST_ENCODER_FIXUP_X86_CALL_REL32, o[0].module,
                         o[0].symbol);
  case AST_MACHINE_X86_CALL_INDIRECT:
    return encoder_shape(i, 0u, 1u) && bits == 64u &&
           rm_op(b, 32u, 0xffu, 2u, &o[0], 0u, 0);
  case AST_MACHINE_X86_PUSH:
    return encoder_shape(i, 0u, 1u) && gpr(b, &o[0], &r) && bits == 64u &&
           prefix(b, 32u, 0u, r, 0, 0u) && byte(b, (uint8_t)(0x50u + (r & 7u)));
  case AST_MACHINE_X86_POP:
    return encoder_shape(i, 1u, 1u) && gpr(b, &o[0], &r) && bits == 64u &&
           prefix(b, 32u, 0u, r, 0, 0u) && byte(b, (uint8_t)(0x58u + (r & 7u)));
  case AST_MACHINE_X86_JMP:
    if (!encoder_shape(i, 0u, 1u) || o[0].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        !byte(b, 0xe9u) || !encoder_le(b, 0u, 4u))
      return 0;
    return encoder_fixup(b, b->out->size - 4u, 4u, AST_ENCODER_FIXUP_PC_REL32,
                         o[0].module, o[0].symbol);
  case AST_MACHINE_X86_JMP_INDIRECT:
    return encoder_shape(i, 0u, 1u) && bits == 64u &&
           rm_op(b, 32u, 0xffu, 4u, &o[0], 0u, 0);
  case AST_MACHINE_X86_RAW_BYTES:
    if (!encoder_shape(i, 0u, 0u) || !i->raw_bytes || !i->raw_byte_count)
      return 0;
    for (uint32_t j = 0; j < i->raw_byte_count; ++j)
      if (!byte(b, i->raw_bytes[j]))
        return 0;
    return 1;
  case AST_MACHINE_X86_BR:
    if (!encoder_shape(i, 0u, 1u) || o[0].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    return o[0].block == b->next_block || branch(b, o[0].block, -1);
  case AST_MACHINE_X86_COND_BR:
    if (!encoder_shape(i, 0u, 3u) || o[1].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        o[2].kind != AST_PHYSICAL_OPERAND_BLOCK)
      return 0;
    if (o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint32_t target = o[o[0].immediate_bits ? 1 : 2].block;
      return target == b->next_block || branch(b, target, -1);
    }
    if (!test_condition(b, &o[0]))
      return 0;
    return conditional_targets(b, o[1].block, o[2].block, 5u);
  case AST_MACHINE_X86_ICMP_BR: {
    if (!encoder_shape(i, 0u, 5u) ||
        o[0].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
        o[0].immediate_bits < AST_MACHINE_X86_ICMP_EQ ||
        o[0].immediate_bits > AST_MACHINE_X86_ICMP_GE ||
        o[3].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        o[4].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        !compare_values(b, &o[1], &o[2]))
      return 0;
    unsigned cc = icmp_cc((AstMachineOpcode)o[0].immediate_bits, i->is_signed);
    return conditional_targets(b, o[3].block, o[4].block, cc);
  }
  default:
    return 0;
  }
}
