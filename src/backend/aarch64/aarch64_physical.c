#include "../common/div_cost.h"
#include "../common/div_magic.h"
#include "aarch64_internal.h"
#include <limits.h>

typedef struct WorkRegisters {
  uint8_t excluded[MACHINE_REGISTER_UNITS];
  AstPhysicalOperand saved[AST_PHYSICAL_MAX_OPERANDS];
  uint32_t count;
} WorkRegisters;

static int fail_instruction(PhysicalContext *c, const char *message) {
  return regalloc_fail(c->program, AST_REGALLOC_DIAG_UNSUPPORTED_INSTRUCTION,
                       c->function, c->instruction, message);
}
static int borrow_exact(PhysicalContext *c, WorkRegisters *w,
                        AstPhysicalOperand r) {
  if (!r.kind || r.register_unit >= MACHINE_REGISTER_UNITS ||
      w->count >= AST_PHYSICAL_MAX_OPERANDS)
    return 0;
  uint32_t bits = r.bank == AST_MACHINE_BANK_GPR ? 64u
                  : r.bit_width < 128u           ? 128u
                                                 : r.bit_width;
  for (uint32_t v = 0; v < c->machine->info.vreg_count; ++v) {
    const AstRegAllocLocation *loc = &c->fn->locations[v];
    if (loc->kind == AST_REGALLOC_LOCATION_REGISTER &&
        loc->register_unit == r.register_unit && loc->bit_width > bits)
      bits = loc->bit_width;
  }
  r = physical_register(c, r.bank, r.register_unit % 64u, bits);
  uint32_t unit = r.register_unit;
  if (c->fn->work_objects[unit] == AST_MACHINE_INVALID_ID)
    c->fn->work_objects[unit] = physical_temp_object(
        c, ((uint64_t)bits + 7u) / 8u, AST_FRAME_OBJECT_EMERGENCY);
  else if (c->fn->objects[c->fn->work_objects[unit]].size <
           ((uint64_t)bits + 7u) / 8u)
    c->fn->objects[c->fn->work_objects[unit]].size = ((uint64_t)bits + 7u) / 8u;
  uint32_t object = c->fn->work_objects[unit];
  if (object == AST_MACHINE_INVALID_ID ||
      !physical_move(c, physical_frame(object, bits, 0), r))
    return 0;
  w->saved[w->count++] = r;
  w->excluded[unit] = 1u;
  return 1;
}
static AstPhysicalOperand borrow_register(PhysicalContext *c, WorkRegisters *w,
                                          AstMachineRegBank bank,
                                          uint32_t bits) {
  static const uint32_t gprs[] = {16u, 17u, 9u, 10u, 11u, 12u, 13u, 14u, 15u,
                                  0u,  1u,  2u, 3u,  4u,  5u,  6u,  7u,  8u};
  AstPhysicalOperand invalid = {0};
  for (uint32_t i = 0; i < (bank == AST_MACHINE_BANK_GPR ? 18u : 16u); ++i) {
    uint32_t index = bank == AST_MACHINE_BANK_GPR ? gprs[i] : 31u - i;
    AstPhysicalOperand r = physical_register(c, bank, index, bits);
    if (r.kind && !w->excluded[r.register_unit]) {
      if (!borrow_exact(c, w, r))
        return invalid;
      return r;
    }
  }
  return invalid;
}
static int restore_work(PhysicalContext *c, WorkRegisters *w) {
  while (w->count) {
    AstPhysicalOperand r = w->saved[--w->count];
    if (!physical_move(c, r,
                       physical_frame(c->fn->work_objects[r.register_unit],
                                      r.bit_width, 0)))
      return 0;
  }
  return 1;
}
static AstPhysicalOperand ensure_register(PhysicalContext *c, WorkRegisters *w,
                                          AstPhysicalOperand value,
                                          AstMachineRegBank bank) {
  if (value.kind == AST_PHYSICAL_OPERAND_REGISTER)
    return value;
  AstPhysicalOperand r = borrow_register(c, w, bank, value.bit_width);
  if (!r.kind || !physical_move(c, r, value)) {
    AstPhysicalOperand invalid = {0};
    return invalid;
  }
  return r;
}
static AstPhysicalOperand register_width(PhysicalContext *c,
                                         AstPhysicalOperand r, uint32_t bits) {
  return physical_register(c, r.bank, r.register_unit % 64u, bits);
}
static int extend_integer(PhysicalContext *c, AstPhysicalOperand dst,
                          AstPhysicalOperand src, uint32_t source_bits,
                          int sign) {
  dst = register_width(c, dst, dst.bit_width <= 32u ? 32u : 64u);
  src = register_width(c, src, dst.bit_width);
  if (source_bits >= dst.bit_width)
    return physical_move(c, dst, src);
  AstPhysicalOperand args[] = {dst, src, physical_immediate(0u, 8u),
                               physical_immediate(source_bits - 1u, 8u)};
  return physical_emit(c, sign ? AST_MACHINE_A64_SBFM : AST_MACHINE_A64_UBFM,
                       1u, args, 4u);
}
static int lower_cast(PhysicalContext *c, WorkRegisters *w,
                      const MachineInstruction *mi, AstPhysicalOperand dst,
                      AstPhysicalOperand src) {
  uint32_t source_bits = src.bit_width, dest_bits = dst.bit_width;
  const AstSemaType *from =
      c->machine->vregs[mi->info.operands[0].as.vreg].type;
  const AstSemaType *to = c->machine->vregs[mi->info.result_first].type;
  int sign =
      mi->info.token_op == AST_TOK_KW_SEXT ||
      (mi->info.token_op != AST_TOK_KW_ZEXT && ast_sema_type_is_signed(from));
  int raw = mi->info.token_op == AST_TOK_KW_BITCAST ||
            mi->info.token_op == AST_TOK_KW_PTRCAST;
  if (dst.bank == AST_MACHINE_BANK_GPR && src.bank == AST_MACHINE_BANK_GPR)
    return extend_integer(c, dst, src,
                          source_bits < dest_bits ? source_bits : dest_bits,
                          sign && source_bits < dest_bits);
  if (raw) {
    if (source_bits != dest_bits || (source_bits != 32u && source_bits != 64u))
      return 0;
    AstPhysicalOperand args[] = {dst, src};
    return physical_emit(c, AST_MACHINE_A64_FMOV, 1u, args, 2u);
  }
  AstMachineOpcode opcode;
  if (dst.bank == AST_MACHINE_BANK_FP && src.bank == AST_MACHINE_BANK_GPR) {
    if (source_bits < 32u) {
      AstPhysicalOperand normalized =
          borrow_register(c, w, AST_MACHINE_BANK_GPR, 32u);
      if (!normalized.kind ||
          !extend_integer(c, normalized, src, source_bits, sign))
        return 0;
      src = normalized;
    }
    opcode = sign ? AST_MACHINE_A64_SCVTF : AST_MACHINE_A64_UCVTF;
  } else if (dst.bank == AST_MACHINE_BANK_GPR &&
             src.bank == AST_MACHINE_BANK_FP) {
    dst = register_width(c, dst, dest_bits <= 32u ? 32u : 64u);
    opcode = ast_sema_type_is_signed(to) ? AST_MACHINE_A64_FCVTZS
                                         : AST_MACHINE_A64_FCVTZU;
  } else if (dst.bank == AST_MACHINE_BANK_FP &&
             src.bank == AST_MACHINE_BANK_FP) {
    if (source_bits == dest_bits)
      return physical_move(c, dst, src);
    opcode = AST_MACHINE_A64_FCVT;
  } else
    return fail_instruction(c,
                            "Vector casts require explicit lane legalization.");
  AstPhysicalOperand args[] = {dst, src};
  return physical_emit(c, opcode, 1u, args, 2u);
}
static uint32_t comparison_condition(AstMachineOpcode opcode, int sign) {
  switch (opcode) {
  case AST_MACHINE_A64_ICMP_EQ:
  case AST_MACHINE_A64_FCMP_EQ:
    return 0u;
  case AST_MACHINE_A64_ICMP_NE:
  case AST_MACHINE_A64_FCMP_NE:
    return 1u;
  case AST_MACHINE_A64_ICMP_LT:
    return sign ? 11u : 3u;
  case AST_MACHINE_A64_ICMP_LE:
    return sign ? 13u : 9u;
  case AST_MACHINE_A64_ICMP_GT:
    return sign ? 12u : 8u;
  case AST_MACHINE_A64_ICMP_GE:
    return sign ? 10u : 2u;
  case AST_MACHINE_A64_FCMP_LT:
    return 4u;
  case AST_MACHINE_A64_FCMP_LE:
    return 9u;
  case AST_MACHINE_A64_FCMP_GT:
    return 12u;
  case AST_MACHINE_A64_FCMP_GE:
    return 10u;
  default:
    return 14u;
  }
}
static uint32_t add_block(PhysicalContext *c) {
  if (c->fn->block_count == c->fn->block_capacity) {
    uint32_t cap = c->fn->block_capacity ? c->fn->block_capacity * 2u : 8u;
    size_t bytes;
    if (cap < c->fn->block_capacity ||
        ast_size_mul_overflow(cap, sizeof(*c->fn->blocks), &bytes))
      return AST_MACHINE_INVALID_ID;
    PhysicalBlock *next = (PhysicalBlock *)realloc(c->fn->blocks, bytes);
    if (!next)
      return AST_MACHINE_INVALID_ID;
    memset(next + c->fn->block_capacity, 0,
           (size_t)(cap - c->fn->block_capacity) * sizeof(*next));
    c->fn->blocks = next;
    c->fn->block_capacity = cap;
  }
  return c->fn->block_count++;
}
static int lower_branch(PhysicalContext *c, const MachineInstruction *mi) {
  AstPhysicalOperand args[3] = {{0}};
  uint32_t cursor = 0u, count = 0u;
  WorkRegisters work = {0};
  int conditional = mi->info.opcode == AST_MACHINE_A64_COND_BR;
  if (conditional) {
    if (!mi->info.operand_count ||
        mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    AstPhysicalOperand condition =
        physical_home(c, mi->info.operands[0].as.vreg);
    condition = ensure_register(c, &work, condition, AST_MACHINE_BANK_GPR);
    if (!condition.kind ||
        !physical_emit(c, AST_MACHINE_A64_CMP_ZERO, 0u, &condition, 1u))
      return 0;
    /* Reloads and edge copies preserve NZCV. The final branch consumes flags.
     */
    cursor = 1u;
  }
  uint32_t predecessor = c->block;
  for (uint32_t edge = 0; edge < (conditional ? 2u : 1u); ++edge) {
    if (cursor >= mi->info.operand_count ||
        mi->info.operands[cursor].kind != AST_MACHINE_OPERAND_BLOCK)
      return 0;
    uint32_t target = mi->info.operands[cursor++].as.block;
    const MachineBlock *successor = &c->machine->blocks[target];
    uint32_t n = successor->info.argument_count;
    if (n > mi->info.operand_count - cursor)
      return 0;
    size_t copy_capacity = (size_t)n + c->machine->info.vreg_count;
    if (copy_capacity < n)
      return 0;
    PhysicalCopy *copies = (PhysicalCopy *)calloc(
        copy_capacity ? copy_capacity : 1u, sizeof(*copies));
    if (!copies)
      return 0;
    uint32_t used = 0u;
    for (uint32_t a = 0; a < n; ++a) {
      AstPhysicalOperand dst = physical_home_at_slot(
          c, successor->arguments[a], c->fn->liveness.block_begin[target]);
      if (!dst.kind)
        continue;
      if (mi->info.operands[cursor + a].kind != AST_MACHINE_OPERAND_VREG) {
        free(copies);
        return 0;
      }
      copies[used].dst = dst;
      copies[used].src =
          physical_home(c, mi->info.operands[cursor + a].as.vreg);
      ++used;
    }
    cursor += n;
    for (uint32_t v = 0; v < c->machine->info.vreg_count; ++v) {
      if (!machine_liveness_test_bit(
              &c->fn->liveness.blocks[predecessor].live_out, v) ||
          !machine_liveness_test_bit(&c->fn->liveness.blocks[target].live_in,
                                     v))
        continue;
      AstPhysicalOperand src = physical_home(c, v);
      AstPhysicalOperand dst =
          physical_home_at_slot(c, v, c->fn->liveness.block_begin[target]);
      if (!src.kind || !dst.kind || physical_same_location(&src, &dst))
        continue;
      if ((size_t)used >= copy_capacity) {
        free(copies);
        return 0;
      }
      copies[used++] = (PhysicalCopy){dst, src};
    }
    uint32_t branch_target = target;
    if (used) {
      /* All conditional transfers belong to their own edge block, even
         if both branches name the same successor with different values. */
      if (conditional) {
        branch_target = add_block(c);
        if (branch_target == AST_MACHINE_INVALID_ID) {
          free(copies);
          return 0;
        }
        c->block = branch_target;
        c->program->stats.edge_blocks++;
      }
      int ok = physical_parallel_copy(c, copies, used);
      if (conditional && ok) {
        AstPhysicalOperand dest = {0};
        dest.kind = AST_PHYSICAL_OPERAND_BLOCK;
        dest.block = target;
        ok = physical_emit(c, AST_MACHINE_A64_BR, 0u, &dest, 1u);
      }
      c->block = predecessor;
      if (!ok) {
        free(copies);
        return 0;
      }
    }
    free(copies);
    args[count].kind = AST_PHYSICAL_OPERAND_BLOCK;
    args[count++].block = branch_target;
  }
  return cursor == mi->info.operand_count && restore_work(c, &work) &&
         physical_emit(c, mi->info.opcode, 0u, args, count);
}
static int lower_constant(PhysicalContext *c, const MachineInstruction *mi) {
  AstMirInstructionInfo source = {0};
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  if (mi->info.source_instruction != AST_MIR_INVALID_ID &&
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source))
    return 0;
  for (uint32_t part = 0; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    AstPhysicalOperand dst = physical_home(c, v);
    if (!dst.kind || dst.kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
      continue;
    uint64_t bits = part == 0u   ? source.constant.lo
                    : part == 1u ? source.constant.hi
                                 : 0u;
    if (source.constant.is_negative) {
      if (!part)
        bits = UINT64_C(0) - bits;
      else
        bits = ~bits + (source.constant.lo == 0u ? 1u : 0u);
    }
    if (mi->info.opcode == AST_MACHINE_A64_UNDEF)
      bits = 0u;
    if (dst.bit_width > 64u)
      return fail_instruction(
          c, "Wide constants need source-aware lane expansion.");
    if (c->machine->vregs[v].bank == AST_MACHINE_BANK_FP) {
      if (dst.bit_width == 32u) {
        float value = (float)source.constant.floating;
        uint32_t raw;
        memcpy(&raw, &value, sizeof(raw));
        bits = raw;
      } else if (dst.bit_width == 64u)
        memcpy(&bits, &source.constant.floating, sizeof(bits));
      else
        return fail_instruction(
            c, "This floating format needs explicit constant conversion.");
      uint32_t image = physical_temp_object(c, dst.bit_width / 8u,
                                            AST_FRAME_OBJECT_ABI_TEMP);
      if (image == AST_MACHINE_INVALID_ID ||
          !physical_move(c, physical_frame(image, dst.bit_width, 0),
                         physical_immediate(bits, dst.bit_width)) ||
          !physical_move(c, dst, physical_frame(image, dst.bit_width, 0)))
        return 0;
      continue;
    }
    if (!physical_move(c, dst, physical_immediate(bits, dst.bit_width)))
      return 0;
  }
  return 1;
}
static int lower_copy(PhysicalContext *c, const MachineInstruction *mi) {
  if (mi->info.result_count != mi->info.operand_count)
    return 0;
  PhysicalCopy local[AST_PHYSICAL_MAX_OPERANDS];
  int heap = mi->info.result_count > AST_PHYSICAL_MAX_OPERANDS;
  PhysicalCopy *copies =
      heap ? (PhysicalCopy *)malloc((size_t)mi->info.result_count *
                                    sizeof(*copies))
           : local;
  if (!copies)
    return 0;
  uint32_t count = 0u;
  for (uint32_t i = 0; i < mi->info.result_count; ++i) {
    AstPhysicalOperand dst = physical_home(c, mi->info.result_first + i);
    if (!dst.kind)
      continue;
    if (mi->info.operands[i].kind != AST_MACHINE_OPERAND_VREG) {
      if (heap)
        free(copies);
      return 0;
    }
    copies[count].dst = dst;
    copies[count++].src = physical_home(c, mi->info.operands[i].as.vreg);
  }
  int ok = physical_parallel_copy(c, copies, count);
  if (heap)
    free(copies);
  return ok;
}

static int a64_source_constant_u64(PhysicalContext *c, AstMirVReg vreg,
                                   uint64_t *out) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirVRegInfo vi;
  AstMirInstructionInfo def;
  if (!mf || !out || !ast_mir_function_vreg(mf, vreg, &vi) ||
      vi.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
      !ast_mir_function_instruction(mf, vi.definition_index, &def) ||
      def.op != AST_MIR_OP_CONSTANT ||
      def.constant.kind != AST_SEMA_CONST_INTEGER || def.constant.is_negative ||
      def.constant.hi != 0u)
    return 0;
  *out = def.constant.lo;
  return 1;
}
/* Keep MIR's sign-and-magnitude representation: interpreting a negative
   constant's `lo` as two's complement would silently choose the wrong
   magic multiplier. Only 64-bit signed operands reach this helper. */
static int a64_source_signed_divisor(PhysicalContext *c, AstMirVReg vreg,
                                     uint64_t *magnitude, int *negative) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirVRegInfo vi;
  AstMirInstructionInfo def;
  if (!mf || !magnitude || !negative || !ast_mir_function_vreg(mf, vreg, &vi) ||
      vi.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
      !ast_mir_function_instruction(mf, vi.definition_index, &def) ||
      def.op != AST_MIR_OP_CONSTANT ||
      def.constant.kind != AST_SEMA_CONST_INTEGER || def.constant.hi ||
      !def.constant.lo ||
      (def.constant.is_negative ? def.constant.lo > (UINT64_C(1) << 63u)
                                : def.constant.lo > INT64_MAX))
    return 0;
  *magnitude = def.constant.lo;
  *negative = !!def.constant.is_negative;
  return 1;
}
/* 1.2.0 development: exact unsigned strength reduction on 32/64-bit
   integers. Work in physical registers and avoid emitting a synthetic UDIV;
   UBFM with immr=0 extracts the low k bits for modulo 2^k. Only a
   *source-level* known power-of-two is eligible (never a guessed value of
   a live machine register). The zero and general-divisor paths are unchanged.
 */
static int a64_lower_magic_divrem(PhysicalContext *c,
                                  const MachineInstruction *mi,
                                  WorkRegisters *work, AstPhysicalOperand dst,
                                  AstPhysicalOperand numerator) {
  int signed_op = mi->info.opcode == AST_MACHINE_A64_SDIV ||
                  mi->info.opcode == AST_MACHINE_A64_SREM;
  int division = mi->info.opcode == AST_MACHINE_A64_UDIV ||
                 mi->info.opcode == AST_MACHINE_A64_SDIV;
  if (!division && mi->info.opcode != AST_MACHINE_A64_UREM &&
      mi->info.opcode != AST_MACHINE_A64_SREM)
    return 0;
  if (mi->info.source_instruction == AST_MIR_INVALID_ID ||
      mi->info.operand_count != 2u || mi->info.result_count != 1u ||
      dst.bit_width != 64u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  uint64_t divisor;
  int divisor_negative = 0;
  AstDivMagicU64 m;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_BINARY || source.operand_count != 2u ||
      !(signed_op ? a64_source_signed_divisor(c, source.operands[1], &divisor,
                                              &divisor_negative)
                  : a64_source_constant_u64(c, source.operands[1], &divisor)) ||
      !ast_div_magic_u64(divisor, &m))
    return 0;
  AstDivCostChoice choice = ast_div_cost_select(
      AST_DIV_COST_AARCH64, dst.bit_width, signed_op, !division, divisor, 1,
      c->program->target.optimize_size || ast_div_cost_prefer_size());
  ast_div_cost_report(AST_DIV_COST_AARCH64, dst.bit_width, signed_op, !division,
                      divisor, choice);
  if (choice.mode != AST_DIV_COST_MAGIC)
    return 0;
  numerator = ensure_register(c, work, numerator, AST_MACHINE_BANK_GPR);
  if (!numerator.kind || numerator.bit_width != 64u)
    return -1;
  AstPhysicalOperand unsigned_numerator = numerator;
  AstPhysicalOperand quotient_sign = {0};
  if (signed_op) {
    quotient_sign = borrow_register(c, work, AST_MACHINE_BANK_GPR, 64u);
    unsigned_numerator = borrow_register(c, work, AST_MACHINE_BANK_GPR, 64u);
    if (!quotient_sign.kind || !unsigned_numerator.kind)
      return -1;
    AstPhysicalOperand sign[] = {quotient_sign, numerator,
                                 physical_immediate(63u, 8u)};
    AstPhysicalOperand xor_args[] = {unsigned_numerator, numerator,
                                     quotient_sign};
    AstPhysicalOperand sub_args[] = {unsigned_numerator, unsigned_numerator,
                                     quotient_sign};
    if (!physical_emit(c, AST_MACHINE_A64_ASHR, 1u, sign, 3u) ||
        !physical_emit(c, AST_MACHINE_A64_XOR, 1u, xor_args, 3u) ||
        !physical_emit(c, AST_MACHINE_A64_SUB, 1u, sub_args, 3u))
      return -1;
    if (divisor_negative) {
      AstPhysicalOperand invert[] = {quotient_sign, quotient_sign};
      if (!physical_emit(c, AST_MACHINE_A64_NOT, 1u, invert, 2u))
        return -1;
    }
  }
  AstPhysicalOperand multiplier =
      borrow_register(c, work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand quotient =
      borrow_register(c, work, AST_MACHINE_BANK_GPR, 64u);
  if (!multiplier.kind || !quotient.kind ||
      !physical_move(c, multiplier, physical_immediate(m.multiplier, 64u)))
    return -1;
  AstPhysicalOperand hi[] = {quotient, unsigned_numerator, multiplier};
  if (!physical_emit(c, AST_MACHINE_A64_UMULH, 1u, hi, 3u))
    return -1;
  if (m.add) {
    AstPhysicalOperand temp =
        borrow_register(c, work, AST_MACHINE_BANK_GPR, 64u);
    if (!temp.kind)
      return -1;
    AstPhysicalOperand difference[] = {temp, unsigned_numerator, quotient};
    AstPhysicalOperand shr[] = {temp, temp, physical_immediate(1u, 8u)};
    AstPhysicalOperand sum[] = {quotient, quotient, temp};
    if (!physical_emit(c, AST_MACHINE_A64_SUB, 1u, difference, 3u) ||
        !physical_emit(c, AST_MACHINE_A64_LSHR, 1u, shr, 3u) ||
        !physical_emit(c, AST_MACHINE_A64_ADD, 1u, sum, 3u))
      return -1;
  }
  if (m.shift) {
    AstPhysicalOperand shr[] = {quotient, quotient,
                                physical_immediate(m.shift, 8u)};
    if (!physical_emit(c, AST_MACHINE_A64_LSHR, 1u, shr, 3u))
      return -1;
  }
  if (signed_op) {
    /* Reapply the numerator/divisor sign modulo 2^64. INT64_MIN's
       unsigned magnitude is 2^63 and is handled without signed UB. */
    AstPhysicalOperand xor_args[] = {quotient, quotient, quotient_sign};
    AstPhysicalOperand sub_args[] = {quotient, quotient, quotient_sign};
    if (!physical_emit(c, AST_MACHINE_A64_XOR, 1u, xor_args, 3u) ||
        !physical_emit(c, AST_MACHINE_A64_SUB, 1u, sub_args, 3u))
      return -1;
  }
  if (division)
    return physical_move(c, dst, quotient) ? 1 : -1;
  uint64_t literal =
      signed_op && divisor_negative ? UINT64_C(0) - divisor : divisor;
  if (!physical_move(c, multiplier, physical_immediate(literal, 64u)))
    return -1;
  AstPhysicalOperand submul[] = {dst, quotient, multiplier, numerator};
  return physical_emit(c, AST_MACHINE_A64_MSUB, 1u, submul, 4u) ? 1 : -1;
}

static int a64_lower_pow2_divrem(PhysicalContext *c,
                                 const MachineInstruction *mi,
                                 WorkRegisters *work, AstPhysicalOperand dst,
                                 AstPhysicalOperand numerator) {
  int div = mi->info.opcode == AST_MACHINE_A64_UDIV;
  if (!div && mi->info.opcode != AST_MACHINE_A64_UREM)
    return 0;
  if (mi->info.source_instruction == AST_MIR_INVALID_ID ||
      mi->info.operand_count != 2u || mi->info.result_count != 1u ||
      (dst.bit_width != 32u && dst.bit_width != 64u))
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  uint64_t divisor = 0u;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_BINARY || source.operand_count != 2u ||
      !a64_source_constant_u64(c, source.operands[1], &divisor) || !divisor ||
      (divisor & (divisor - 1u)) ||
      (dst.bit_width == 32u && divisor > UINT32_MAX))
    return 0;
  uint32_t k = 0u;
  for (uint64_t n = divisor; n > 1u; n >>= 1u)
    ++k;
  numerator = ensure_register(c, work, numerator, AST_MACHINE_BANK_GPR);
  if (!numerator.kind)
    return -1;
  if (numerator.bit_width < 32u)
    return 0;
  numerator = register_width(c, numerator, dst.bit_width);
  if (!k)
    return physical_move(
               c, dst, div ? numerator : physical_immediate(0u, dst.bit_width))
               ? 1
               : -1;
  AstPhysicalOperand args[] = {dst, numerator,
                               physical_immediate(div ? k : 0u, 8u)};
  if (div)
    return physical_emit(c, AST_MACHINE_A64_LSHR, 1u, args, 3u) ? 1 : -1;
  AstPhysicalOperand mask[] = {dst, numerator, physical_immediate(0u, 8u),
                               physical_immediate(k - 1u, 8u)};
  return physical_emit(c, AST_MACHINE_A64_UBFM, 1u, mask, 4u) ? 1 : -1;
}

static int a64_aggregate_element_offset(const AstSemaType *type, uint64_t index,
                                        uint64_t *offset,
                                        const AstSemaType **element) {
  if (!type || !offset || !element)
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  if (kind == AST_SEMA_TYPE_ARRAY) {
    uint64_t count = 0u;
    const AstSemaType *item = ast_sema_type_element(type);
    uint64_t size = item ? ast_sema_type_size(item) : UINT64_MAX;
    if (!item || !ast_sema_type_array_count(type, &count) || index >= count ||
        size == UINT64_MAX || (size && index > UINT64_MAX / size))
      return 0;
    *offset = index * size;
    *element = item;
    return 1;
  }
  if (kind == AST_SEMA_TYPE_TUPLE) {
    uint32_t count = ast_sema_type_tuple_count(type);
    if (index >= count)
      return 0;
    uint64_t cursor = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
      const AstSemaType *item = ast_sema_type_tuple_item(type, i);
      uint64_t size = item ? ast_sema_type_size(item) : UINT64_MAX;
      uint32_t align = item ? ast_sema_type_alignment(item) : 0u;
      if (!item || size == UINT64_MAX || !align || (align & (align - 1u)))
        return 0;
      cursor = (cursor + align - 1u) & ~((uint64_t)align - 1u);
      if (i == index) {
        *offset = cursor;
        *element = item;
        return 1;
      }
      if (cursor > UINT64_MAX - size)
        return 0;
      cursor += size;
    }
  }
  return 0;
}
static int a64_lower_pack(PhysicalContext *c, const MachineInstruction *mi) {
  if (!mi || mi->info.source_op != AST_MIR_OP_PACK ||
      mi->info.operand_count != 2u ||
      (mi->info.result_count != 1u && mi->info.result_count != 2u) ||
      mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG ||
      mi->info.operands[1].kind != AST_MACHINE_OPERAND_VREG)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_PACK ||
      ast_sema_type_kind(source.type) != AST_SEMA_TYPE_TUPLE ||
      ast_sema_type_tuple_count(source.type) != 2u)
    return 0;
  const AstSemaType *first_type = ast_sema_type_tuple_item(source.type, 0u);
  const AstSemaType *second_type = ast_sema_type_tuple_item(source.type, 1u);
  uint64_t first_size =
      first_type ? ast_sema_type_size(first_type) : UINT64_MAX;
  uint64_t second_size =
      second_type ? ast_sema_type_size(second_type) : UINT64_MAX;
  uint64_t total_size = ast_sema_type_size(source.type);
  if (first_size == UINT64_MAX || second_size != 1u ||
      total_size == UINT64_MAX ||
      (first_size != 1u && first_size != 2u && first_size != 4u &&
       first_size != 8u) ||
      !total_size || total_size > 16u)
    return 0;
  uint64_t storage = total_size <= 8u ? 8u : 16u;
  uint32_t object = physical_temp_object(c, storage, AST_FRAME_OBJECT_ABI_TEMP);
  if (object == AST_MACHINE_INVALID_ID)
    return 0;
  for (uint64_t off = 0u; off < storage; off += 8u)
    if (!physical_move(c, physical_frame(object, 64u, (int64_t)off),
                       physical_immediate(0u, 64u)))
      return 0;
  AstPhysicalOperand first = physical_home(c, mi->info.operands[0].as.vreg);
  AstPhysicalOperand flag = physical_home(c, mi->info.operands[1].as.vreg);
  if (!first.kind || !flag.kind)
    return 0;
  first.bit_width = (uint32_t)first_size * 8u;
  flag.bit_width = 8u;
  if (!physical_move(c, physical_frame(object, first.bit_width, 0), first) ||
      !physical_move(c, physical_frame(object, 8u, (int64_t)first_size), flag))
    return 0;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    AstPhysicalOperand dst = physical_home(c, mi->info.result_first + part);
    if (dst.kind &&
        !physical_move(
            c, dst, physical_frame(object, dst.bit_width, (int64_t)part * 8)))
      return 0;
  }
  return 1;
}
static int a64_lower_aggregate_extract(PhysicalContext *c,
                                       const MachineInstruction *mi) {
  if (!mi ||
      (mi->info.source_op != AST_MIR_OP_INDEX &&
       mi->info.source_op != AST_MIR_OP_MEMBER) ||
      mi->info.result_count != 1u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.operand_count < 1u)
    return 0;
  AstMirVRegInfo aggregate_vi;
  if (!ast_mir_function_vreg(mf, source.operands[0], &aggregate_vi) ||
      !aggregate_vi.type)
    return 0;
  uint64_t offset = 0u;
  const AstSemaType *element = NULL;
  if (source.op == AST_MIR_OP_INDEX) {
    if (source.operand_count != 2u)
      return 0;
    uint64_t index = 0u;
    if (!a64_source_constant_u64(c, source.operands[1], &index) ||
        !a64_aggregate_element_offset(aggregate_vi.type, index, &offset,
                                      &element))
      return 0;
  } else {
    if (ast_sema_type_kind(aggregate_vi.type) != AST_SEMA_TYPE_STRUCT ||
        source.aux_index >= ast_sema_type_struct_field_count(aggregate_vi.type))
      return 0;
    AstSemaFieldInfo field;
    if (!ast_sema_type_struct_field(aggregate_vi.type, source.aux_index,
                                    &field))
      return 0;
    offset = field.offset;
    element = field.type;
  }
  uint64_t bytes = element ? ast_sema_type_size(element) : UINT64_MAX;
  uint64_t aggregate_size = ast_sema_type_size(aggregate_vi.type);
  if (bytes == UINT64_MAX || !bytes || bytes > 8u ||
      aggregate_size == UINT64_MAX || offset > aggregate_size ||
      bytes > aggregate_size - offset || aggregate_size > 16u)
    return 0;
  AstTargetValueInfo tv;
  if (!ast_target_value_info(c->program->machine->target, c->function,
                             source.operands[0], &tv))
    return 0;
  uint32_t parts = tv.part_count ? tv.part_count : 1u;
  if (!parts || parts > 2u || mi->info.operand_count < parts)
    return 0;
  uint64_t storage = aggregate_size <= 8u ? 8u : 16u;
  uint32_t object = physical_temp_object(c, storage, AST_FRAME_OBJECT_ABI_TEMP);
  if (object == AST_MACHINE_INVALID_ID)
    return 0;
  for (uint64_t off = 0u; off < storage; off += 8u)
    if (!physical_move(c, physical_frame(object, 64u, (int64_t)off),
                       physical_immediate(0u, 64u)))
      return 0;
  for (uint32_t part = 0u; part < parts; ++part) {
    if (mi->info.operands[part].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    AstPhysicalOperand src = physical_home(c, mi->info.operands[part].as.vreg);
    if (!src.kind ||
        !physical_move(
            c, physical_frame(object, src.bit_width, (int64_t)part * 8), src))
      return 0;
  }
  AstPhysicalOperand dst = physical_home(c, mi->info.result_first);
  if (!dst.kind)
    return 1;
  return physical_move(
      c, dst, physical_frame(object, (uint32_t)bytes * 8u, (int64_t)offset));
}

static AstMachineOpcode a64_scalar_compare_opcode(AstTokenKind token, int fp) {
  if (fp) {
    switch (token) {
    case AST_TOK_EQ_EQ:
      return AST_MACHINE_A64_FCMP_EQ;
    case AST_TOK_BANG_EQ:
      return AST_MACHINE_A64_FCMP_NE;
    case AST_TOK_LT:
      return AST_MACHINE_A64_FCMP_LT;
    case AST_TOK_LT_EQ:
      return AST_MACHINE_A64_FCMP_LE;
    case AST_TOK_GT:
      return AST_MACHINE_A64_FCMP_GT;
    case AST_TOK_GT_EQ:
      return AST_MACHINE_A64_FCMP_GE;
    default:
      return AST_MACHINE_OP_INVALID;
    }
  }
  switch (token) {
  case AST_TOK_EQ_EQ:
    return AST_MACHINE_A64_ICMP_EQ;
  case AST_TOK_BANG_EQ:
    return AST_MACHINE_A64_ICMP_NE;
  case AST_TOK_LT:
    return AST_MACHINE_A64_ICMP_LT;
  case AST_TOK_LT_EQ:
    return AST_MACHINE_A64_ICMP_LE;
  case AST_TOK_GT:
    return AST_MACHINE_A64_ICMP_GT;
  case AST_TOK_GT_EQ:
    return AST_MACHINE_A64_ICMP_GE;
  default:
    return AST_MACHINE_OP_INVALID;
  }
}

static int a64_source_type(PhysicalContext *c, AstMirVReg vreg,
                           AstMirVRegInfo *out) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  return mf && out && ast_mir_function_vreg(mf, vreg, out);
}

static uint32_t a64_value_image(PhysicalContext *c, AstMirVReg source,
                                uint64_t *bytes_out) {
  if (!c || source >= c->machine->source_vreg_count)
    return AST_MACHINE_INVALID_ID;
  uint32_t first = c->machine->source_first[source],
           parts = c->machine->source_parts[source];
  if (first == AST_MACHINE_INVALID_ID || !parts ||
      first > c->machine->info.vreg_count ||
      parts > c->machine->info.vreg_count - first)
    return AST_MACHINE_INVALID_ID;
  uint64_t bytes = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t bits = c->machine->vregs[first + part].bit_width;
    if (!bits || (bits & 7u) || bytes > UINT64_MAX - bits / 8u)
      return AST_MACHINE_INVALID_ID;
    bytes += bits / 8u;
  }
  uint32_t image = physical_temp_object(c, bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (image == AST_MACHINE_INVALID_ID)
    return image;
  uint64_t off = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t v = first + part, bits = c->machine->vregs[v].bit_width;
    AstPhysicalOperand home = physical_home(c, v);
    if (!home.kind ||
        !physical_move(c, physical_frame(image, bits, (int64_t)off), home))
      return AST_MACHINE_INVALID_ID;
    off += bits / 8u;
  }
  if (bytes_out)
    *bytes_out = bytes;
  return image;
}

static uint32_t a64_result_image(PhysicalContext *c,
                                 const MachineInstruction *mi,
                                 uint64_t *bytes_out) {
  if (!c || !mi || !mi->info.result_count)
    return AST_MACHINE_INVALID_ID;
  uint64_t bytes = 0u;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    if (v >= c->machine->info.vreg_count)
      return AST_MACHINE_INVALID_ID;
    uint32_t bits = c->machine->vregs[v].bit_width;
    if (!bits || (bits & 7u) || bytes > UINT64_MAX - bits / 8u)
      return AST_MACHINE_INVALID_ID;
    bytes += bits / 8u;
  }
  uint32_t image = physical_temp_object(c, bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (image == AST_MACHINE_INVALID_ID)
    return image;
  for (uint64_t off = 0u; off < bytes;) {
    uint32_t bits = (uint32_t)((bytes - off >= 8u ? 8u : bytes - off) * 8u);
    if (!physical_move(c, physical_frame(image, bits, (int64_t)off),
                       physical_immediate(0u, bits)))
      return AST_MACHINE_INVALID_ID;
    off += bits / 8u;
  }
  if (bytes_out)
    *bytes_out = bytes;
  return image;
}

static int a64_commit_result_image(PhysicalContext *c,
                                   const MachineInstruction *mi,
                                   uint32_t image) {
  uint64_t off = 0u;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    AstPhysicalOperand home = physical_home(c, v);
    uint32_t bits = c->machine->vregs[v].bit_width;
    if (home.kind &&
        !physical_move(c, home, physical_frame(image, bits, (int64_t)off)))
      return 0;
    off += bits / 8u;
  }
  return 1;
}

static int a64_vector_shape(const AstSemaType *type,
                            const AstSemaType **element_out,
                            uint64_t *lanes_out, uint32_t *bits_out) {
  if (!type || ast_sema_type_kind(type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = ast_sema_type_element(type);
  uint64_t lanes = 0u;
  uint32_t bits = element ? ast_sema_type_bit_width(element) : 0u;
  if (!element || !ast_sema_type_vector_lanes(type, &lanes) || !lanes ||
      (bits != 8u && bits != 16u && bits != 32u && bits != 64u))
    return 0;
  if (element_out)
    *element_out = element;
  if (lanes_out)
    *lanes_out = lanes;
  if (bits_out)
    *bits_out = bits;
  return 1;
}

/* A64 ADD/SUB (immediate) encodes an unsigned 12-bit value, optionally
 * shifted left by 12.  Other integer constants must be materialized in a
 * scratch register before selecting the register-register instruction.
 * Do not let an arbitrary 32/64-bit virtual constant reach the encoder as
 * an immediate; the physical verifier cannot manufacture the missing MOVK. */
static int a64_addsub_immediate_fits(uint64_t value) {
  return value <= UINT64_C(4095) ||
         ((value & UINT64_C(4095)) == 0u && (value >> 12u) <= UINT64_C(4095));
}

static AstMachineOpcode a64_scalar_binary_opcode(AstTokenKind token, int fp) {
  if (fp) {
    switch (token) {
    case AST_TOK_PLUS:
      return AST_MACHINE_A64_FADD;
    case AST_TOK_MINUS:
      return AST_MACHINE_A64_FSUB;
    case AST_TOK_STAR:
      return AST_MACHINE_A64_FMUL;
    case AST_TOK_SLASH:
      return AST_MACHINE_A64_FDIV;
    default:
      return AST_MACHINE_OP_INVALID;
    }
  }
  switch (token) {
  case AST_TOK_PLUS:
    return AST_MACHINE_A64_ADD;
  case AST_TOK_MINUS:
    return AST_MACHINE_A64_SUB;
  case AST_TOK_STAR:
    return AST_MACHINE_A64_MUL;
  case AST_TOK_AMP:
  case AST_TOK_AMP_AMP:
    return AST_MACHINE_A64_AND;
  case AST_TOK_PIPE:
  case AST_TOK_PIPE_PIPE:
    return AST_MACHINE_A64_OR;
  case AST_TOK_CARET:
    return AST_MACHINE_A64_XOR;
  case AST_TOK_SHIFT_LEFT:
    return AST_MACHINE_A64_SHL;
  case AST_TOK_SHIFT_RIGHT:
    return AST_MACHINE_A64_LSHR;
  default:
    return AST_MACHINE_OP_INVALID;
  }
}

static int lower_a64_vector_binary(PhysicalContext *c,
                                   const MachineInstruction *mi) {
  if (!c || !mi || mi->info.source_instruction == AST_MIR_INVALID_ID ||
      !mi->info.result_count)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo result_vi, rhs_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_BINARY || source.result == AST_MIR_INVALID_ID ||
      source.operand_count != 2u ||
      !a64_source_type(c, source.result, &result_vi) || !result_vi.type ||
      ast_sema_type_kind(result_vi.type) != AST_SEMA_TYPE_VECTOR ||
      !a64_source_type(c, source.operands[1], &rhs_vi) || !rhs_vi.type)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!a64_vector_shape(result_vi.type, &element, &lanes, &bits))
    return 0;
  int fp = ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT;
  int rhs_vector = ast_sema_type_kind(rhs_vi.type) == AST_SEMA_TYPE_VECTOR;
  int shift = source.token_op == AST_TOK_SHIFT_LEFT ||
              source.token_op == AST_TOK_SHIFT_RIGHT;
  if (fp && (bits != 32u && bits != 64u))
    return 0;
  if (!rhs_vector && !shift)
    return 0;
  AstMachineOpcode op = a64_scalar_binary_opcode(source.token_op, fp);
  if (op == AST_MACHINE_OP_INVALID)
    return 0;
  int signed_element = !fp && ast_sema_type_is_signed(element);
  if (source.token_op == AST_TOK_SHIFT_RIGHT && signed_element)
    op = AST_MACHINE_A64_ASHR;

  uint64_t lhs_bytes = 0u, rhs_bytes = 0u, out_bytes = 0u;
  uint32_t lhs_image = a64_value_image(c, source.operands[0], &lhs_bytes);
  uint32_t rhs_image = AST_MACHINE_INVALID_ID;
  if (lhs_image == AST_MACHINE_INVALID_ID)
    return 0;
  if (rhs_vector) {
    rhs_image = a64_value_image(c, source.operands[1], &rhs_bytes);
    if (rhs_image == AST_MACHINE_INVALID_ID || rhs_bytes != lhs_bytes)
      return 0;
  }
  uint32_t out_image = a64_result_image(c, mi, &out_bytes);
  uint64_t logical = ast_sema_type_size(result_vi.type);
  if (out_image == AST_MACHINE_INVALID_ID || !logical ||
      logical == UINT64_MAX || logical > lhs_bytes || logical > out_bytes)
    return 0;

  WorkRegisters work = {0};
  uint32_t compute_bits = fp ? bits : (bits <= 32u ? 32u : 64u);
  AstPhysicalOperand lhs = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, compute_bits);
  AstPhysicalOperand rhs = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, compute_bits);
  AstPhysicalOperand dst = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, compute_bits);
  if (!lhs.kind || !rhs.kind || !dst.kind)
    return 0;
  if (shift) {
    uint32_t first = c->machine->source_first[source.operands[1]];
    if (first == AST_MACHINE_INVALID_ID ||
        c->machine->source_parts[source.operands[1]] != 1u)
      return 0;
    AstPhysicalOperand count_home = physical_home(c, first);
    AstPhysicalOperand mask =
        borrow_register(c, &work, AST_MACHINE_BANK_GPR, compute_bits);
    if (!count_home.kind || !mask.kind ||
        !extend_integer(c, rhs, count_home,
                        count_home.bit_width < compute_bits
                            ? count_home.bit_width
                            : compute_bits,
                        0) ||
        !physical_move(c, mask, physical_immediate(bits - 1u, compute_bits)) ||
        !physical_emit(c, AST_MACHINE_A64_AND, 1u,
                       (AstPhysicalOperand[]){rhs, rhs, mask}, 3u))
      return 0;
  }
  uint64_t lane_bytes = bits / 8u;
  for (uint64_t lane = 0u; lane < lanes; ++lane) {
    uint64_t off = lane * lane_bytes;
    if (fp) {
      AstPhysicalOperand l = lhs, r = rhs, d = dst;
      l.bit_width = r.bit_width = d.bit_width = bits;
      if (!physical_move(c, l, physical_frame(lhs_image, bits, (int64_t)off)))
        return 0;
      if (rhs_vector &&
          !physical_move(c, r, physical_frame(rhs_image, bits, (int64_t)off)))
        return 0;
      if (!physical_emit(c, op, 1u, (AstPhysicalOperand[]){d, l, r}, 3u) ||
          !physical_move(c, physical_frame(out_image, bits, (int64_t)off), d))
        return 0;
      continue;
    }
    AstPhysicalOperand raw = physical_frame(lhs_image, bits, (int64_t)off);
    AstPhysicalOperand lhs_raw = register_width(c, lhs, bits);
    if (!lhs_raw.kind || !physical_move(c, lhs_raw, raw) ||
        (bits < compute_bits &&
         !extend_integer(c, lhs, lhs_raw, bits,
                         signed_element &&
                             source.token_op == AST_TOK_SHIFT_RIGHT)))
      return 0;
    if (!shift) {
      AstPhysicalOperand rraw = physical_frame(rhs_image, bits, (int64_t)off);
      AstPhysicalOperand rhs_raw = register_width(c, rhs, bits);
      if (!rhs_raw.kind || !physical_move(c, rhs_raw, rraw) ||
          (bits < compute_bits && !extend_integer(c, rhs, rhs_raw, bits, 0)))
        return 0;
    }
    if (!physical_emit(c, op, 1u, (AstPhysicalOperand[]){dst, lhs, rhs}, 3u))
      return 0;
    AstPhysicalOperand low = register_width(c, dst, bits <= 32u ? bits : 64u);
    if (!physical_move(c, physical_frame(out_image, bits, (int64_t)off), low))
      return 0;
  }
  if (!restore_work(c, &work))
    return 0;
  return a64_commit_result_image(c, mi, out_image);
}

static int lower_a64_vector_index(PhysicalContext *c,
                                  const MachineInstruction *mi) {
  if (!c || !mi || mi->info.source_instruction == AST_MIR_INVALID_ID ||
      mi->info.result_count != 1u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo base_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_INDEX || source.operand_count != 2u ||
      !a64_source_type(c, source.operands[0], &base_vi) || !base_vi.type ||
      ast_sema_type_kind(base_vi.type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!a64_vector_shape(base_vi.type, &element, &lanes, &bits))
    return 0;
  uint64_t image_bytes = 0u;
  uint32_t image = a64_value_image(c, source.operands[0], &image_bytes);
  if (image == AST_MACHINE_INVALID_ID)
    return 0;
  uint64_t index = 0u;
  AstPhysicalOperand home = physical_home(c, mi->info.result_first);
  if (!home.kind)
    return 1;
  if (a64_source_constant_u64(c, source.operands[1], &index)) {
    if (index >= lanes)
      return 0;
    return physical_move(
        c, home, physical_frame(image, bits, (int64_t)(index * (bits / 8u))));
  }
  uint32_t first = c->machine->source_first[source.operands[1]];
  if (first == AST_MACHINE_INVALID_ID ||
      c->machine->source_parts[source.operands[1]] != 1u)
    return 0;
  AstPhysicalOperand index_home = physical_home(c, first);
  if (!index_home.kind)
    return 0;
  WorkRegisters work = {0};
  if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[home.register_unit] = 1u;
  if (index_home.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[index_home.register_unit] = 1u;
  AstPhysicalOperand address =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand offset =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!address.kind || !offset.kind ||
      !physical_emit(
          c, AST_MACHINE_A64_LEA, 1u,
          (AstPhysicalOperand[]){address, physical_frame(image, 64u, 0)}, 2u) ||
      !extend_integer(c, offset, index_home,
                      index_home.bit_width < 64u ? index_home.bit_width : 64u,
                      0))
    return 0;
  uint32_t lane_bytes = bits / 8u;
  if (lane_bytes > 1u) {
    uint32_t shift_amount = lane_bytes == 2u ? 1u : lane_bytes == 4u ? 2u : 3u;
    AstPhysicalOperand shift =
        borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
    if (!shift.kind ||
        !physical_move(c, shift, physical_immediate(shift_amount, 64u)) ||
        !physical_emit(c, AST_MACHINE_A64_SHL, 1u,
                       (AstPhysicalOperand[]){offset, offset, shift}, 3u))
      return 0;
  }
  if (!physical_emit(c, AST_MACHINE_A64_ADD, 1u,
                     (AstPhysicalOperand[]){address, address, offset}, 3u))
    return 0;
  AstPhysicalOperand memory = address;
  memory.kind = AST_PHYSICAL_OPERAND_MEMORY;
  memory.bit_width = bits;
  memory.displacement = 0;
  if (!physical_move(c, home, memory))
    return 0;
  return restore_work(c, &work);
}

static int lower_a64_vector_compare(PhysicalContext *c,
                                    const MachineInstruction *mi) {
  if (!c || !mi || !mi->info.result_count || !mi->info.operand_count ||
      (mi->info.operand_count & 1u))
    return 0;
  uint32_t parts = mi->info.operand_count / 2u;
  const AstMachineOperand *first = &mi->info.operands[0];
  if (first->kind != AST_MACHINE_OPERAND_VREG ||
      first->as.vreg >= c->machine->info.vreg_count)
    return 0;
  const AstSemaType *vector_type = c->machine->vregs[first->as.vreg].type;
  const AstSemaType *element = ast_sema_type_element(vector_type);
  uint64_t lanes = 0u;
  uint32_t element_bits = element ? ast_sema_type_bit_width(element) : 0u;
  int fp = element && ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT;
  int is_signed = !fp && element && ast_sema_type_is_signed(element);
  if (!element || !ast_sema_type_vector_lanes(vector_type, &lanes) || !lanes ||
      (element_bits != 8u && element_bits != 16u && element_bits != 32u &&
       element_bits != 64u) ||
      (fp && element_bits != 32u && element_bits != 64u))
    return 0;
  AstMachineOpcode compare = a64_scalar_compare_opcode(mi->info.token_op, fp);
  if (compare == AST_MACHINE_OP_INVALID)
    return 0;

  uint64_t image_bytes = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    const AstMachineOperand *left = &mi->info.operands[part];
    const AstMachineOperand *right = &mi->info.operands[parts + part];
    if (left->kind != AST_MACHINE_OPERAND_VREG ||
        right->kind != AST_MACHINE_OPERAND_VREG ||
        left->as.vreg >= c->machine->info.vreg_count ||
        right->as.vreg >= c->machine->info.vreg_count)
      return 0;
    uint32_t bits = c->machine->vregs[left->as.vreg].bit_width;
    if (!bits || (bits % 8u) ||
        bits != c->machine->vregs[right->as.vreg].bit_width)
      return 0;
    image_bytes += (uint64_t)bits / 8u;
  }
  uint32_t left_image =
      physical_temp_object(c, image_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  uint32_t right_image =
      physical_temp_object(c, image_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (left_image == AST_MACHINE_INVALID_ID ||
      right_image == AST_MACHINE_INVALID_ID)
    return 0;

  WorkRegisters work = {0};
  uint64_t offset = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t lv = mi->info.operands[part].as.vreg;
    uint32_t rv = mi->info.operands[parts + part].as.vreg;
    uint32_t bits = c->machine->vregs[lv].bit_width;
    AstPhysicalOperand lh = physical_home(c, lv), rh = physical_home(c, rv);
    if (!lh.kind || !rh.kind ||
        !physical_move(c, physical_frame(left_image, bits, (int64_t)offset),
                       lh) ||
        !physical_move(c, physical_frame(right_image, bits, (int64_t)offset),
                       rh))
      return 0;
    if (lh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[lh.register_unit] = 1u;
    if (rh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[rh.register_unit] = 1u;
    offset += (uint64_t)bits / 8u;
  }
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand home = physical_home(c, mi->info.result_first + rp);
    if (!home.kind)
      return 0;
    if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[home.register_unit] = 1u;
  }

  AstPhysicalOperand accumulator =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand bit_register =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand left_scalar =
      borrow_register(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                      fp ? element_bits : (element_bits <= 32u ? 32u : 64u));
  AstPhysicalOperand right_scalar =
      borrow_register(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                      fp ? element_bits : (element_bits <= 32u ? 32u : 64u));
  AstPhysicalOperand shift_register =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!accumulator.kind || !bit_register.kind || !left_scalar.kind ||
      !right_scalar.kind || !shift_register.kind)
    return 0;

  uint64_t lane = 0u;
  uint64_t lane_bytes = element_bits / 8u;
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand home = physical_home(c, mi->info.result_first + rp);
    uint32_t output_bits = home.bit_width;
    if (!output_bits || output_bits > 64u)
      return 0;
    if (!physical_move(c, accumulator, physical_immediate(0u, 64u)))
      return 0;
    uint64_t remaining = lanes - lane;
    uint32_t chunk = (uint32_t)(remaining > 64u ? 64u : remaining);
    if (chunk > output_bits)
      chunk = output_bits;
    for (uint32_t li = 0u; li < chunk; ++li, ++lane) {
      uint64_t byte_offset = lane * lane_bytes;
      AstPhysicalOperand lhs = left_scalar, rhs = right_scalar;
      if (fp) {
        lhs.bit_width = rhs.bit_width = element_bits;
        if (!physical_move(c, lhs,
                           physical_frame(left_image, element_bits,
                                          (int64_t)byte_offset)) ||
            !physical_move(c, rhs,
                           physical_frame(right_image, element_bits,
                                          (int64_t)byte_offset)))
          return 0;
      } else {
        uint32_t machine_bits = element_bits <= 32u ? 32u : 64u;
        lhs = physical_register(c, AST_MACHINE_BANK_GPR,
                                left_scalar.register_unit % 64u, machine_bits);
        rhs = physical_register(c, AST_MACHINE_BANK_GPR,
                                right_scalar.register_unit % 64u, machine_bits);
        AstPhysicalOperand lhs_raw =
            physical_register(c, AST_MACHINE_BANK_GPR,
                              left_scalar.register_unit % 64u, element_bits);
        AstPhysicalOperand rhs_raw =
            physical_register(c, AST_MACHINE_BANK_GPR,
                              right_scalar.register_unit % 64u, element_bits);
        if (!physical_move(c, lhs_raw,
                           physical_frame(left_image, element_bits,
                                          (int64_t)byte_offset)) ||
            !physical_move(c, rhs_raw,
                           physical_frame(right_image, element_bits,
                                          (int64_t)byte_offset)))
          return 0;
        if (element_bits < machine_bits &&
            (!extend_integer(c, lhs, lhs_raw, element_bits, is_signed) ||
             !extend_integer(c, rhs, rhs_raw, element_bits, is_signed)))
          return 0;
      }
      AstMachineOpcode raw_compare =
          fp ? AST_MACHINE_A64_FCMP : AST_MACHINE_A64_CMP;
      AstPhysicalOperand cmp_args[] = {lhs, rhs};
      if (!physical_emit(c, raw_compare, 0u, cmp_args, 2u))
        return 0;
      uint32_t condition = comparison_condition(compare, is_signed);
      AstPhysicalOperand bit32 = physical_register(
          c, AST_MACHINE_BANK_GPR, bit_register.register_unit % 64u, 32u);
      AstPhysicalOperand cset_args[] = {bit32,
                                        physical_immediate(condition, 8u)};
      if (!physical_emit(c, AST_MACHINE_A64_CSET, 1u, cset_args, 2u))
        return 0;
      AstPhysicalOperand bit64 = physical_register(
          c, AST_MACHINE_BANK_GPR, bit_register.register_unit % 64u, 64u);
      if (li) {
        if (!physical_move(c, shift_register, physical_immediate(li, 64u)))
          return 0;
        AstPhysicalOperand shift_args[] = {bit64, bit64, shift_register};
        if (!physical_emit(c, AST_MACHINE_A64_SHL, 1u, shift_args, 3u))
          return 0;
      }
      AstPhysicalOperand or_args[] = {accumulator, accumulator, bit64};
      if (!physical_emit(c, AST_MACHINE_A64_OR, 1u, or_args, 3u))
        return 0;
    }
    AstPhysicalOperand packed = physical_register(
        c, AST_MACHINE_BANK_GPR, accumulator.register_unit % 64u,
        output_bits <= 32u ? 32u : 64u);
    if (!physical_move(c, home, packed))
      return 0;
  }
  return lane == lanes && restore_work(c, &work);
}

static int a64_lower_mask_index(PhysicalContext *c,
                                const MachineInstruction *mi) {
  if (!c || !mi || mi->info.source_op != AST_MIR_OP_INDEX ||
      mi->info.result_count != 1u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo base_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_INDEX || source.operand_count != 2u ||
      !ast_mir_function_vreg(mf, source.operands[0], &base_vi) ||
      !base_vi.type || ast_sema_type_kind(base_vi.type) != AST_SEMA_TYPE_MASK)
    return 0;
  uint64_t lanes = 0u;
  if (!ast_sema_type_vector_lanes(base_vi.type, &lanes) || !lanes)
    return 0;
  uint32_t parts = c->machine->source_parts[source.operands[0]];
  if (!parts || mi->info.operand_count < parts + 1u)
    return 0;
  uint64_t constant_index = 0u;
  int constant =
      a64_source_constant_u64(c, source.operands[1], &constant_index);
  uint32_t part = 0u, bit = 0u;
  if (constant) {
    if (constant_index >= lanes)
      return 0;
    part = (uint32_t)(constant_index / 64u);
    bit = (uint32_t)(constant_index & 63u);
    if (part >= parts)
      return 0;
  } else if (parts != 1u) {
    return fail_instruction(
        c, "Dynamic indexing of a split mask needs part-selection lowering.");
  }
  const AstMachineOperand *mask_op = &mi->info.operands[part];
  const AstMachineOperand *index_op = &mi->info.operands[parts];
  if (mask_op->kind != AST_MACHINE_OPERAND_VREG ||
      index_op->kind != AST_MACHINE_OPERAND_VREG)
    return 0;
  AstPhysicalOperand mask = physical_home(c, mask_op->as.vreg);
  AstPhysicalOperand index = physical_home(c, index_op->as.vreg);
  AstPhysicalOperand home = physical_home(c, mi->info.result_first);
  if (!mask.kind || !home.kind || (!constant && !index.kind))
    return 0;
  WorkRegisters work = {0};
  if (mask.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[mask.register_unit] = 1u;
  if (index.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[index.register_unit] = 1u;
  if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[home.register_unit] = 1u;
  AstPhysicalOperand value =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand shift =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand one = borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!value.kind || !shift.kind || !one.kind ||
      !extend_integer(c, value, mask, mask.bit_width, 0))
    return 0;
  if (constant) {
    if (bit && !physical_move(c, shift, physical_immediate(bit, 64u)))
      return 0;
  } else {
    if (!extend_integer(c, shift, index, index.bit_width, 0))
      return 0;
  }
  if ((!constant || bit) &&
      !physical_emit(c, AST_MACHINE_A64_LSHR, 1u,
                     (AstPhysicalOperand[]){value, value, shift}, 3u))
    return 0;
  if (!physical_move(c, one, physical_immediate(1u, 64u)) ||
      !physical_emit(c, AST_MACHINE_A64_AND, 1u,
                     (AstPhysicalOperand[]){value, value, one}, 3u))
    return 0;
  AstPhysicalOperand low = physical_register(
      c, AST_MACHINE_BANK_GPR, value.register_unit % 64u, home.bit_width);
  return physical_move(c, home, low) && restore_work(c, &work);
}

static int a64_lower_vector_select(PhysicalContext *c,
                                   const MachineInstruction *mi) {
  if (!c || !mi || !mi->info.result_count ||
      (mi->info.source_op != AST_MIR_OP_SELECT &&
       mi->info.source_op != AST_MIR_OP_INTRINSIC))
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo result_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      (source.op != AST_MIR_OP_SELECT &&
       !(source.op == AST_MIR_OP_INTRINSIC &&
         source.intrinsic == AST_HIR_INTRINSIC_SELECT)) ||
      source.operand_count != 3u ||
      !ast_mir_function_vreg(mf, source.result, &result_vi) ||
      !result_vi.type ||
      ast_sema_type_kind(result_vi.type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = ast_sema_type_element(result_vi.type);
  uint64_t lanes = 0u;
  uint32_t element_bits = element ? ast_sema_type_bit_width(element) : 0u;
  int fp = element && ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT;
  if (!element || !ast_sema_type_vector_lanes(result_vi.type, &lanes) ||
      !lanes ||
      (element_bits != 8u && element_bits != 16u && element_bits != 32u &&
       element_bits != 64u) ||
      (fp && element_bits != 32u && element_bits != 64u))
    return 0;
  uint32_t mask_parts = c->machine->source_parts[source.operands[0]];
  uint32_t value_parts = c->machine->source_parts[source.operands[1]];
  if (!mask_parts ||
      value_parts != c->machine->source_parts[source.operands[2]] ||
      value_parts != mi->info.result_count ||
      mi->info.operand_count != mask_parts + value_parts * 2u)
    return 0;
  uint64_t total_bytes = ast_sema_type_size(result_vi.type);
  if (!total_bytes || total_bytes == UINT64_MAX)
    return 0;
  uint32_t true_image =
      physical_temp_object(c, total_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  uint32_t false_image =
      physical_temp_object(c, total_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  uint32_t out_image =
      physical_temp_object(c, total_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (true_image == AST_MACHINE_INVALID_ID ||
      false_image == AST_MACHINE_INVALID_ID ||
      out_image == AST_MACHINE_INVALID_ID)
    return 0;
  WorkRegisters work = {0};
  uint64_t offset = 0u;
  for (uint32_t part = 0u; part < value_parts; ++part) {
    uint32_t tv = mi->info.operands[mask_parts + part].as.vreg;
    uint32_t fv = mi->info.operands[mask_parts + value_parts + part].as.vreg;
    if (mi->info.operands[mask_parts + part].kind != AST_MACHINE_OPERAND_VREG ||
        mi->info.operands[mask_parts + value_parts + part].kind !=
            AST_MACHINE_OPERAND_VREG ||
        tv >= c->machine->info.vreg_count || fv >= c->machine->info.vreg_count)
      return 0;
    uint32_t bits = c->machine->vregs[tv].bit_width;
    AstPhysicalOperand th = physical_home(c, tv), fh = physical_home(c, fv);
    if (!th.kind || !fh.kind || bits != c->machine->vregs[fv].bit_width ||
        !physical_move(c, physical_frame(true_image, bits, (int64_t)offset),
                       th) ||
        !physical_move(c, physical_frame(false_image, bits, (int64_t)offset),
                       fh))
      return 0;
    if (th.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[th.register_unit] = 1u;
    if (fh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[fh.register_unit] = 1u;
    offset += bits / 8u;
  }
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand h = physical_home(c, mi->info.result_first + rp);
    if (!h.kind)
      return 0;
    if (h.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[h.register_unit] = 1u;
  }
  for (uint32_t mp = 0u; mp < mask_parts; ++mp) {
    if (mi->info.operands[mp].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    AstPhysicalOperand mh = physical_home(c, mi->info.operands[mp].as.vreg);
    if (!mh.kind)
      return 0;
    if (mh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[mh.register_unit] = 1u;
  }
  AstPhysicalOperand mask_value =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand shift =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand scalar_t =
      borrow_register(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                      fp ? element_bits : (element_bits <= 32u ? 32u : 64u));
  AstPhysicalOperand scalar_f =
      borrow_register(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                      fp ? element_bits : (element_bits <= 32u ? 32u : 64u));
  AstPhysicalOperand scalar_r =
      borrow_register(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                      fp ? element_bits : (element_bits <= 32u ? 32u : 64u));
  if (!mask_value.kind || !shift.kind || !scalar_t.kind || !scalar_f.kind ||
      !scalar_r.kind)
    return 0;
  uint64_t lane_bytes = element_bits / 8u;
  for (uint64_t lane = 0u; lane < lanes; ++lane) {
    uint32_t mp = (uint32_t)(lane / 64u), bit = (uint32_t)(lane & 63u);
    if (mp >= mask_parts)
      return 0;
    AstPhysicalOperand mh = physical_home(c, mi->info.operands[mp].as.vreg);
    if (!extend_integer(c, mask_value, mh, mh.bit_width, 0))
      return 0;
    if (bit) {
      if (!physical_move(c, shift, physical_immediate(bit, 64u)) ||
          !physical_emit(c, AST_MACHINE_A64_LSHR, 1u,
                         (AstPhysicalOperand[]){mask_value, mask_value, shift},
                         3u))
        return 0;
    }
    AstPhysicalOperand cmp_bit = physical_register(
        c, AST_MACHINE_BANK_GPR, mask_value.register_unit % 64u, 32u);
    if (!physical_emit(c, AST_MACHINE_A64_CMP_ZERO, 0u, &cmp_bit, 1u))
      return 0;
    uint64_t byte_offset = lane * lane_bytes;
    AstPhysicalOperand t = scalar_t, f = scalar_f, r = scalar_r;
    if (fp) {
      t.bit_width = f.bit_width = r.bit_width = element_bits;
      if (!physical_move(
              c, t,
              physical_frame(true_image, element_bits, (int64_t)byte_offset)) ||
          !physical_move(
              c, f,
              physical_frame(false_image, element_bits, (int64_t)byte_offset)))
        return 0;
    } else {
      uint32_t machine_bits = element_bits <= 32u ? 32u : 64u;
      AstPhysicalOperand traw = physical_register(
          c, AST_MACHINE_BANK_GPR, scalar_t.register_unit % 64u, element_bits);
      AstPhysicalOperand fraw = physical_register(
          c, AST_MACHINE_BANK_GPR, scalar_f.register_unit % 64u, element_bits);
      t = physical_register(c, AST_MACHINE_BANK_GPR,
                            scalar_t.register_unit % 64u, machine_bits);
      f = physical_register(c, AST_MACHINE_BANK_GPR,
                            scalar_f.register_unit % 64u, machine_bits);
      r = physical_register(c, AST_MACHINE_BANK_GPR,
                            scalar_r.register_unit % 64u, machine_bits);
      if (!physical_move(
              c, traw,
              physical_frame(true_image, element_bits, (int64_t)byte_offset)) ||
          !physical_move(
              c, fraw,
              physical_frame(false_image, element_bits, (int64_t)byte_offset)))
        return 0;
      if (element_bits < machine_bits &&
          (!extend_integer(c, t, traw, element_bits, 0) ||
           !extend_integer(c, f, fraw, element_bits, 0)))
        return 0;
    }
    AstPhysicalOperand sel[] = {r, t, f, physical_immediate(1u, 8u)};
    if (!physical_emit(c, fp ? AST_MACHINE_A64_FCSEL : AST_MACHINE_A64_CSEL, 1u,
                       sel, 4u))
      return 0;
    AstPhysicalOperand store = r;
    store.bit_width = element_bits;
    if (!physical_move(
            c, physical_frame(out_image, element_bits, (int64_t)byte_offset),
            store))
      return 0;
  }
  offset = 0u;
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand h = physical_home(c, mi->info.result_first + rp);
    uint32_t bits = c->machine->vregs[mi->info.result_first + rp].bit_width;
    if (!physical_move(c, h, physical_frame(out_image, bits, (int64_t)offset)))
      return 0;
    offset += bits / 8u;
  }
  return restore_work(c, &work);
}

static int lower_regular(PhysicalContext *c, const MachineInstruction *mi) {
  if (lower_a64_vector_binary(c, mi))
    return 1;
  if (mi->info.opcode == AST_MACHINE_A64_VEC_CMP)
    return lower_a64_vector_compare(c, mi);
  if (mi->info.opcode == AST_MACHINE_A64_SELECT &&
      (mi->info.source_op == AST_MIR_OP_SELECT ||
       mi->info.source_op == AST_MIR_OP_INTRINSIC)) {
    const AstMirFunction *mf =
        ast_mir_function_at(c->program->machine->mir, c->function);
    AstMirInstructionInfo source;
    AstMirVRegInfo vi;
    if (mf &&
        ast_mir_function_instruction(mf, mi->info.source_instruction,
                                     &source) &&
        source.result != AST_MIR_INVALID_ID &&
        ast_mir_function_vreg(mf, source.result, &vi) && vi.type &&
        ast_sema_type_kind(vi.type) == AST_SEMA_TYPE_VECTOR)
      return a64_lower_vector_select(c, mi);
  }
  if (mi->info.source_op == AST_MIR_OP_STACK_LOAD ||
      mi->info.source_op == AST_MIR_OP_STACK_STORE) {
    if (!mi->info.operand_count)
      return 0;
    const AstMachineOperand *slot =
        &mi->info.operands[mi->info.operand_count - 1u];
    AstMirFunctionInfo info = {0};
    if (!ast_mir_function_info(
            ast_mir_function_at(c->program->machine->mir, c->function),
            &info) ||
        slot->kind != AST_MACHINE_OPERAND_STACK_SLOT ||
        slot->as.stack_slot >= info.stack_slot_count)
      return 0;
    int load = mi->info.source_op == AST_MIR_OP_STACK_LOAD;
    uint32_t parts = load ? mi->info.result_count : mi->info.operand_count - 1u;
    uint32_t object = c->locals[slot->as.stack_slot];
    if (object >= c->fn->object_count)
      return 0;
    uint64_t offset = 0u, size = c->fn->objects[object].size;
    for (uint32_t part = 0; part < parts; ++part) {
      if (!load && mi->info.operands[part].kind != AST_MACHINE_OPERAND_VREG)
        return 0;
      uint32_t v =
          load ? mi->info.result_first + part : mi->info.operands[part].as.vreg;
      AstPhysicalOperand home = physical_home(c, v);
      uint64_t bytes = ((uint64_t)c->machine->vregs[v].bit_width + 7u) / 8u;
      if (offset >= size)
        return 0;
      if (bytes > size - offset)
        bytes = size - offset;
      uint32_t bits = (uint32_t)bytes * 8u;
      if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
        home = physical_register(c, home.bank, home.register_unit % 64u, bits);
      else
        home.bit_width = bits;
      AstPhysicalOperand memory = physical_frame(object, bits, (int64_t)offset);
      if (home.kind &&
          !physical_move(c, load ? home : memory, load ? memory : home))
        return 0;
      offset += bytes;
    }
    return offset == size;
  }
  WorkRegisters work = {0};
  AstPhysicalOperand uses[AST_PHYSICAL_MAX_OPERANDS] = {{0}};
  AstPhysicalOperand result = {0}, home = {0};
  uint32_t count = 0u;
  if (mi->info.result_count > 1u || mi->info.operand_count > 6u)
    return fail_instruction(c,
                            "Multi-part arithmetic needs legalized carry/lane "
                            "operations before physical lowering.");
  if (mi->info.result_count) {
    home = physical_home(c, mi->info.result_first);
    if (!home.kind)
      return 1; /* Unused, pure definitions only enter here. */
    if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[home.register_unit] = 1u;
  }
  for (uint32_t i = 0; i < mi->info.operand_count; ++i) {
    const AstMachineOperand *op = &mi->info.operands[i];
    if (op->kind == AST_MACHINE_OPERAND_VREG) {
      uses[count] = physical_home(c, op->as.vreg);
      if (uses[count].kind == AST_PHYSICAL_OPERAND_REGISTER)
        work.excluded[uses[count].register_unit] = 1u;
    } else if (op->kind == AST_MACHINE_OPERAND_STACK_SLOT) {
      const AstMirFunction *mir =
          ast_mir_function_at(c->program->machine->mir, c->function);
      AstMirFunctionInfo info = {0};
      if (!ast_mir_function_info(mir, &info) ||
          op->as.stack_slot >= info.stack_slot_count)
        return 0;
      uses[count] =
          physical_frame(c->locals[op->as.stack_slot], home.bit_width, 0);
    } else if (op->kind == AST_MACHINE_OPERAND_SYMBOL) {
      uses[count].kind = AST_PHYSICAL_OPERAND_SYMBOL;
      uses[count].symbol = op->as.symbol;
      uses[count].module = op->module;
    } else if (op->kind == AST_MACHINE_OPERAND_IMMEDIATE) {
      uses[count] =
          physical_immediate((uint64_t)op->as.immediate, home.bit_width);
    } else
      return 0;
    ++count;
  }
  if (home.kind) {
    result = home.kind == AST_PHYSICAL_OPERAND_REGISTER
                 ? home
                 : borrow_register(
                       c, &work, c->machine->vregs[mi->info.result_first].bank,
                       home.bit_width);
    if (!result.kind)
      return 0;
  }
  int ok = 0;
  if (mi->info.opcode == AST_MACHINE_A64_LOAD ||
      mi->info.opcode == AST_MACHINE_A64_LEA) {
    if (count != 1u)
      return fail_instruction(
          c, "Load/address requires exactly one selected address.");
    AstPhysicalOperand address = uses[0];
    if (mi->info.operands[0].kind == AST_MACHINE_OPERAND_VREG) {
      address = ensure_register(c, &work, address, AST_MACHINE_BANK_GPR);
      if (!address.kind)
        return 0;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    if (mi->info.opcode == AST_MACHINE_A64_LOAD &&
        address.kind == AST_PHYSICAL_OPERAND_SYMBOL) {
      AstPhysicalOperand pointer =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
      AstPhysicalOperand address_args[] = {pointer, address};
      if (!pointer.kind ||
          !physical_emit(c, AST_MACHINE_A64_LEA, 1u, address_args, 2u))
        return 0;
      address = pointer;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    uint32_t access_bits = result.bit_width;
    if (result.bank == AST_MACHINE_BANK_GPR && access_bits < 32u)
      result = register_width(c, result, 32u);
    address.bit_width = access_bits;
    AstPhysicalOperand args[] = {result, address};
    ok = physical_emit(c, mi->info.opcode, 1u, args, 2u);
  } else if (mi->info.opcode == AST_MACHINE_A64_STORE) {
    if (count != 2u)
      return fail_instruction(c, "Store requires a value and an address.");
    AstPhysicalOperand value, address;
    uint32_t value_index;
    if (mi->info.source_op == AST_MIR_OP_STORE) {
      address = ensure_register(c, &work, uses[0], AST_MACHINE_BANK_GPR);
      if (!address.kind)
        return 0;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      value = uses[1];
      value_index = 1u;
    } else {
      value = uses[0];
      address = uses[1];
      value_index = 0u;
    }
    if (value.kind != AST_PHYSICAL_OPERAND_REGISTER &&
        value.kind != AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint32_t v = mi->info.operands[value_index].as.vreg;
      value = ensure_register(c, &work, value, c->machine->vregs[v].bank);
    }
    if (address.kind == AST_PHYSICAL_OPERAND_SYMBOL) {
      AstPhysicalOperand pointer =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
      AstPhysicalOperand address_args[] = {pointer, address};
      if (!pointer.kind ||
          !physical_emit(c, AST_MACHINE_A64_LEA, 1u, address_args, 2u))
        return 0;
      address = pointer;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    uint32_t access_bits = value.bit_width;
    if (value.kind == AST_PHYSICAL_OPERAND_REGISTER &&
        value.bank == AST_MACHINE_BANK_GPR && access_bits < 32u)
      value = register_width(c, value, 32u);
    address.bit_width = access_bits;
    ok = physical_move(c, address, value);
  } else {
    /* Recognize constant divisors before materializing the RHS register.
       The result/borrowed registers have already been selected above. */
    if (count == 2u && mi->info.operands[0].kind == AST_MACHINE_OPERAND_VREG &&
        mi->info.operands[1].kind == AST_MACHINE_OPERAND_VREG &&
        (mi->info.opcode == AST_MACHINE_A64_UDIV ||
         mi->info.opcode == AST_MACHINE_A64_UREM ||
         mi->info.opcode == AST_MACHINE_A64_SDIV ||
         mi->info.opcode == AST_MACHINE_A64_SREM)) {
      int reduced = a64_lower_pow2_divrem(c, mi, &work, result, uses[0]);
      if (reduced == 0)
        reduced = a64_lower_magic_divrem(c, mi, &work, result, uses[0]);
      if (reduced < 0)
        return 0;
      if (reduced > 0) {
        AstPhysicalOperand target = result;
        if (target.kind == AST_PHYSICAL_OPERAND_REGISTER &&
            target.bit_width != home.bit_width && home.kind)
          target = register_width(c, target, home.bit_width);
        return (!home.kind || physical_move(c, home, target)) &&
               restore_work(c, &work);
      }
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (mi->info.operands[i].kind != AST_MACHINE_OPERAND_VREG)
        return 0;
      uint32_t v = mi->info.operands[i].as.vreg;
      int immediate_rhs =
          i == 1u && uses[i].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
          (((mi->info.opcode == AST_MACHINE_A64_ADD ||
             mi->info.opcode == AST_MACHINE_A64_SUB) &&
            a64_addsub_immediate_fits(uses[i].immediate_bits)) ||
           mi->info.opcode == AST_MACHINE_A64_SHL ||
           mi->info.opcode == AST_MACHINE_A64_LSHR ||
           mi->info.opcode == AST_MACHINE_A64_ASHR ||
           (mi->info.opcode == AST_MACHINE_A64_MUL &&
            (uses[i].immediate_bits == 1u ||
             (uses[i].immediate_bits &&
              !(uses[i].immediate_bits & (uses[i].immediate_bits - 1u))))));
      if (!immediate_rhs)
        uses[i] = ensure_register(c, &work, uses[i], c->machine->vregs[v].bank);
      if (!uses[i].kind)
        return 0;
      if (uses[i].kind != AST_PHYSICAL_OPERAND_IMMEDIATE &&
          mi->info.opcode != AST_MACHINE_A64_CAST &&
          uses[i].bank == AST_MACHINE_BANK_GPR && uses[i].bit_width < 32u) {
        AstPhysicalOperand normalized =
            borrow_register(c, &work, AST_MACHINE_BANK_GPR, 32u);
        if (!normalized.kind ||
            !extend_integer(c, normalized, uses[i], uses[i].bit_width,
                            ast_sema_type_is_signed(c->machine->vregs[v].type)))
          return 0;
        uses[i] = normalized;
      }
    }
    if (result.bank == AST_MACHINE_BANK_GPR && result.bit_width < 32u)
      result = register_width(c, result, 32u);
    int comparison = (mi->info.opcode >= AST_MACHINE_A64_ICMP_EQ &&
                      mi->info.opcode <= AST_MACHINE_A64_ICMP_GE) ||
                     (mi->info.opcode >= AST_MACHINE_A64_FCMP_EQ &&
                      mi->info.opcode <= AST_MACHINE_A64_FCMP_GE);
    int unary = mi->info.opcode == AST_MACHINE_A64_NEG ||
                mi->info.opcode == AST_MACHINE_A64_NOT;
    if (mi->info.opcode == AST_MACHINE_A64_CAST) {
      ok = count == 1u &&
           lower_cast(c, &work, mi, register_width(c, result, home.bit_width),
                      uses[0]);
    } else if (comparison) {
      int fp = mi->info.opcode >= AST_MACHINE_A64_FCMP_EQ;
      const AstSemaType *type =
          c->machine->vregs[mi->info.operands[0].as.vreg].type;
      AstPhysicalOperand args[] = {
          result, physical_immediate(
                      comparison_condition(mi->info.opcode,
                                           ast_sema_type_is_signed(type)),
                      8u)};
      ok = count == 2u &&
           physical_emit(c, fp ? AST_MACHINE_A64_FCMP : AST_MACHINE_A64_CMP, 0u,
                         uses, 2u) &&
           physical_emit(c, AST_MACHINE_A64_CSET, 1u, args, 2u);
    } else if (mi->info.opcode == AST_MACHINE_A64_SELECT) {
      AstPhysicalOperand args[] = {result, uses[1], uses[2],
                                   physical_immediate(1u, 8u)};
      ok = count == 3u &&
           physical_emit(c, AST_MACHINE_A64_CMP_ZERO, 0u, uses, 1u) &&
           physical_emit(c,
                         result.bank == AST_MACHINE_BANK_FP
                             ? AST_MACHINE_A64_FCSEL
                             : AST_MACHINE_A64_CSEL,
                         1u, args, 4u);
    } else if (mi->info.opcode == AST_MACHINE_A64_NOT &&
               mi->info.token_op == AST_TOK_BANG) {
      AstPhysicalOperand args[] = {result, physical_immediate(0u, 8u)};
      ok = count == 1u &&
           physical_emit(c, AST_MACHINE_A64_CMP_ZERO, 0u, uses, 1u) &&
           physical_emit(c, AST_MACHINE_A64_CSET, 1u, args, 2u);
    } else {
      if (count != (unary ? 1u : 2u))
        return 0;
      AstPhysicalOperand args[] = {result, uses[0], unary ? uses[0] : uses[1]};
      if (mi->info.opcode == AST_MACHINE_A64_MUL && !unary &&
          uses[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
        uint64_t k = uses[1].immediate_bits;
        if (k == 1u)
          ok = physical_move(c, result, uses[0]);
        else if (k && !(k & (k - 1u))) {
          uint32_t shift = 0u;
          while ((UINT64_C(1) << shift) != k && shift < 63u)
            ++shift;
          args[2] = physical_immediate(shift, result.bit_width);
          ok = physical_emit(c, AST_MACHINE_A64_SHL, 1u, args, 3u);
        } else {
          uses[1] = ensure_register(c, &work, uses[1], AST_MACHINE_BANK_GPR);
          args[2] = uses[1];
          ok = uses[1].kind &&
               physical_emit(c, AST_MACHINE_A64_MUL, 1u, args, 3u);
        }
      } else if (mi->info.opcode == AST_MACHINE_A64_SREM ||
                 mi->info.opcode == AST_MACHINE_A64_UREM) {
        AstPhysicalOperand quotient =
            borrow_register(c, &work, AST_MACHINE_BANK_GPR, result.bit_width);
        AstPhysicalOperand divargs[] = {quotient, uses[0], uses[1]};
        AstPhysicalOperand remargs[] = {result, quotient, uses[1], uses[0]};
        ok = quotient.kind &&
             physical_emit(c,
                           mi->info.opcode == AST_MACHINE_A64_SREM
                               ? AST_MACHINE_A64_SDIV
                               : AST_MACHINE_A64_UDIV,
                           1u, divargs, 3u) &&
             physical_emit(c, AST_MACHINE_A64_MSUB, 1u, remargs, 4u);
      } else
        ok = physical_emit(c,
                           mi->info.opcode == AST_MACHINE_A64_NEG &&
                                   result.bank == AST_MACHINE_BANK_FP
                               ? AST_MACHINE_A64_FNEG
                               : mi->info.opcode,
                           1u, args, unary ? 2u : 3u);
    }
  }
  if (result.kind == AST_PHYSICAL_OPERAND_REGISTER &&
      result.bit_width != home.bit_width && home.kind)
    result = register_width(c, result, home.bit_width);
  if (!ok || (home.kind && !physical_move(c, home, result)))
    return 0;
  return restore_work(c, &work);
}

static int a64_late_cfg_cleanup(PhysicalContext *c) {
  if (!c || !c->fn || !c->machine)
    return 0;
  for (uint32_t b = 0; b < c->fn->block_count; ++b) {
    PhysicalBlock *pb = &c->fn->blocks[b];
    if (pb->count < 4u)
      continue;
    AstPhysicalInstructionInfo *raw_cmp = &pb->instructions[pb->count - 4u];
    AstPhysicalInstructionInfo *cset = &pb->instructions[pb->count - 3u];
    AstPhysicalInstructionInfo *zero_cmp = &pb->instructions[pb->count - 2u];
    AstPhysicalInstructionInfo *branch = &pb->instructions[pb->count - 1u];
    if ((raw_cmp->opcode != AST_MACHINE_A64_CMP &&
         raw_cmp->opcode != AST_MACHINE_A64_FCMP) ||
        raw_cmp->result_count || raw_cmp->operand_count != 2u ||
        cset->opcode != AST_MACHINE_A64_CSET || cset->result_count != 1u ||
        cset->operand_count != 2u ||
        zero_cmp->opcode != AST_MACHINE_A64_CMP_ZERO ||
        zero_cmp->result_count || zero_cmp->operand_count != 1u ||
        branch->opcode != AST_MACHINE_A64_COND_BR || branch->result_count ||
        branch->operand_count != 2u ||
        raw_cmp->source_instruction != cset->source_instruction ||
        cset->operands[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        zero_cmp->operands[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        cset->operands[0].bank != AST_MACHINE_BANK_GPR ||
        zero_cmp->operands[0].bank != AST_MACHINE_BANK_GPR ||
        cset->operands[0].register_unit !=
            zero_cmp->operands[0].register_unit ||
        cset->operands[1].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
        cset->operands[1].immediate_bits >= 14u ||
        branch->operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        branch->operands[1].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        cset->source_instruction >= c->machine->info.instruction_count)
      continue;

    if (branch->source_instruction >= c->machine->info.instruction_count ||
        cset->source_instruction >= c->machine->info.instruction_count ||
        b >= c->machine->info.block_count)
      continue;
    const MachineInstruction *branch_source =
        &c->machine->instructions[branch->source_instruction];
    const MachineInstruction *compare_source =
        &c->machine->instructions[cset->source_instruction];
    if (!branch_source->info.operand_count ||
        branch_source->info.operands[0].kind != AST_MACHINE_OPERAND_VREG ||
        compare_source->info.result_count != 1u)
      continue;
    uint32_t condition_vreg = machine_canonical_vreg(
        c->machine, branch_source->info.operands[0].as.vreg);
    uint32_t compare_vreg =
        machine_canonical_vreg(c->machine, compare_source->info.result_first);
    if (condition_vreg >= c->fn->liveness.vreg_count ||
        compare_vreg != condition_vreg ||
        machine_liveness_test_bit(&c->fn->liveness.blocks[b].live_out,
                                  condition_vreg))
      continue;

    AstPhysicalInstructionInfo fused = *branch;
    memset(fused.operands, 0, sizeof(fused.operands));
    fused.opcode = AST_MACHINE_A64_CMP_BR;
    fused.result_count = 0u;
    fused.operand_count = 5u;
    fused.operands[0] = cset->operands[1];
    fused.operands[1] = raw_cmp->operands[0];
    fused.operands[2] = raw_cmp->operands[1];
    fused.operands[3] = branch->operands[0];
    fused.operands[4] = branch->operands[1];
    pb->instructions[pb->count - 4u] = fused;
    pb->count -= 3u;
  }
  return 1;
}

int physical_lower_aarch64(AstRegAllocProgram *p, uint32_t f) {
  AllocationFunction *af = &p->functions[f];
  const MachineFunction *machine = &p->machine->functions[f];
  PhysicalContext c = {.program = p,
                       .fn = af,
                       .machine = machine,
                       .function = f,
                       .block = machine->info.entry_block,
                       .instruction = AST_MACHINE_INVALID_ID};
  const AstMirFunction *mir = ast_mir_function_at(p->machine->mir, f);
  AstMirFunctionInfo info;
  if (!ast_mir_function_info(mir, &info))
    return 0;
  if ((info.flags & AST_HIR_FUNCTION_NAKED) ||
      info.frame_policy == AST_HIR_FRAME_NAKED ||
      info.frame_policy == AST_HIR_FRAME_VALIDATE) {
    int handled = 0;
    return physical_native_function_policy(&c, &handled) && handled;
  }
  if (!frame_init_register_objects(af))
    goto fail;
  c.locals = (uint32_t *)malloc(
      (info.stack_slot_count ? info.stack_slot_count : 1u) * sizeof(uint32_t));
  c.call_sites = (uint32_t *)malloc(
      (machine->info.instruction_count ? machine->info.instruction_count : 1u) *
      sizeof(uint32_t));
  if (!af->callee_save_objects || !af->emergency_objects || !af->work_objects ||
      !c.locals || !c.call_sites)
    goto fail;
  for (uint32_t i = 0; i < machine->info.instruction_count; ++i)
    c.call_sites[i] = AST_MACHINE_INVALID_ID;
  for (uint32_t j = 0; j < machine->info.instruction_count; ++j) {
    AstMirOp op = machine->instructions[j].info.source_op;
    if (op != AST_MIR_OP_CALL_DIRECT && op != AST_MIR_OP_CALL_INDIRECT)
      continue;
    size_t index;
    if (!abi_find_call(p->machine->abi, f,
                       machine->instructions[j].info.source_instruction, &index,
                       NULL) ||
        index > UINT32_MAX)
      goto fail;
    af->frame.has_calls = 1u;
    c.call_sites[j] = (uint32_t)index;
  }
  for (uint32_t local = 0; local < info.stack_slot_count; ++local) {
    AstMirStackSlotInfo slot;
    if (!ast_mir_function_stack_slot(mir, local, &slot) || !slot.size ||
        slot.alignment > 16u ||
        (slot.flags & AST_MIR_STACK_SLOT_DYNAMIC_SIZE)) {
      regalloc_fail(
          p, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, f, AST_MACHINE_INVALID_ID,
          "Dynamic or over-aligned local stack objects are unsupported.");
      goto fail;
    }
    c.locals[local] =
        frame_add_object(af, AST_FRAME_OBJECT_LOCAL, slot.size, slot.alignment);
    if (c.locals[local] == AST_MACHINE_INVALID_ID)
      goto fail;
  }
  for (uint32_t v = 0; v < machine->info.vreg_count; ++v)
    for (uint32_t pi = 0; pi < regalloc_location_piece_count(af, v); ++pi) {
      const AstRegAllocLocation *loc = regalloc_location_piece(af, v, pi);
      MachinePhysRegView view;
      if (loc->kind != AST_REGALLOC_LOCATION_REGISTER)
        continue;
      if (!machine_physreg_view(&p->target, loc->register_name, &view))
        goto fail;
      uint32_t preserved = machine_physreg_preserved_bits(p->abi, &view);
      if (preserved &&
          af->callee_save_objects[view.alias_unit] == AST_MACHINE_INVALID_ID) {
        uint32_t bytes = preserved / 8u;
        af->callee_save_objects[view.alias_unit] =
            frame_add_object(af, AST_FRAME_OBJECT_CALLEE_SAVE, bytes,
                             bytes >= 16u ? 16u : bytes);
        if (af->callee_save_objects[view.alias_unit] == AST_MACHINE_INVALID_ID)
          goto fail;
      }
    }
  if (af->frame.has_calls) {
    uint32_t lr = MACHINE_PHYSREG_FILE_GPR * 64u + 30u;
    af->callee_save_objects[lr] =
        frame_add_object(af, AST_FRAME_OBJECT_CALLEE_SAVE, 8u, 8u);
    if (af->callee_save_objects[lr] == AST_MACHINE_INVALID_ID)
      goto fail;
  }
  for (uint32_t b = 0; b < machine->info.block_count; ++b)
    if (add_block(&c) == AST_MACHINE_INVALID_ID)
      goto fail;
  if (!physical_abi_entry(&c))
    goto fail;
  for (uint32_t b = 0; b < machine->info.block_count; ++b) {
    c.block = b;
    const MachineBlock *block = &machine->blocks[b];
    for (uint32_t i = 0; i < block->info.instruction_count; ++i) {
      c.instruction = block->instructions[i];
      const MachineInstruction *mi = &machine->instructions[c.instruction];
      int ok;
      if (mi->dead)
        ok = 1;
      else
        switch (mi->info.opcode) {
        case AST_MACHINE_A64_MOV_IMM:
        case AST_MACHINE_A64_UNDEF:
          ok = lower_constant(&c, mi);
          break;
        case AST_MACHINE_A64_MOV:
          ok = lower_copy(&c, mi);
          break;
        case AST_MACHINE_A64_BR:
        case AST_MACHINE_A64_COND_BR:
          ok = lower_branch(&c, mi);
          break;
        case AST_MACHINE_A64_RET:
          ok = physical_abi_return(&c, mi);
          break;
        case AST_MACHINE_A64_CALL:
        case AST_MACHINE_A64_CALL_INDIRECT:
          ok = c.call_sites[c.instruction] != AST_MACHINE_INVALID_ID &&
               physical_abi_call(&c, mi, c.call_sites[c.instruction]);
          break;
        case AST_MACHINE_A64_TRAP:
        case AST_MACHINE_A64_FENCE:
          ok = physical_emit(&c, mi->info.opcode, 0u, NULL, 0u);
          break;
        case AST_MACHINE_A64_BIND:
          ok = regalloc_verify_bind_instruction(p, f, mi);
          break;
        case AST_MACHINE_A64_NATIVE:
        case AST_MACHINE_A64_NATIVE_RESULT:
          ok = physical_native_instruction(&c, mi);
          break;
        case AST_MACHINE_A64_INTRINSIC:
          ok = lower_a64_vector_binary(&c, mi) ||
               (mi->info.source_op == AST_MIR_OP_PACK &&
                a64_lower_pack(&c, mi)) ||
               fail_instruction(&c, "Selected intrinsic still requires "
                                    "target-specific constraint expansion.");
          break;
        case AST_MACHINE_A64_INDEX:
        case AST_MACHINE_A64_MEMBER:
          ok = (mi->info.source_op == AST_MIR_OP_INDEX &&
                a64_lower_mask_index(&c, mi)) ||
               (mi->info.source_op == AST_MIR_OP_INDEX &&
                lower_a64_vector_index(&c, mi)) ||
               a64_lower_aggregate_extract(&c, mi);
          break;
        case AST_MACHINE_A64_ATOMIC:
          ok = physical_atomic_instruction(&c, mi);
          break;
        case AST_MACHINE_A64_FREM:
          ok = fail_instruction(&c, "Selected opcode still requires "
                                    "target-specific constraint expansion.");
          break;
        default:
          ok = lower_regular(&c, mi);
          break;
        }
      if (!ok) {
        if (!p->stats.error_count)
          fail_instruction(&c, "Selected instruction cannot be materialized "
                               "with its operand contract.");
        goto fail;
      }
    }
  }
  if (!physical_run_peephole(&c) || !a64_late_cfg_cleanup(&c) ||
      !frame_layout(p, f))
    goto fail;
  c.block = machine->info.entry_block;
  c.instruction = AST_MACHINE_INVALID_ID;
  if (!physical_emit_prologue(&c) || !physical_resolve_frames(&c))
    goto fail;
  free(c.locals);
  free(c.call_sites);
  return 1;
fail:
  free(c.locals);
  free(c.call_sites);
  return 0;
}
