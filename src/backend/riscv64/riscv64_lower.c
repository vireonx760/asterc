#include "../common/div_cost.h"
#include "../common/div_magic.h"
#include "riscv64_internal.h"
#include <limits.h>
static int fail_instruction(RvContext *c, const char *message) {
  return regalloc_fail(c->program, AST_REGALLOC_DIAG_UNSUPPORTED_INSTRUCTION,
                       c->function, c->instruction, message);
}
static int rv_parallel_copy(RvContext *c, PhysicalCopy *copies,
                            uint32_t count) {
  int register_only = 1;
  for (uint32_t i = 0; i < count; ++i) {
    if (!copies[i].src.bit_width ||
        copies[i].src.bit_width != copies[i].dst.bit_width)
      return 0;
    if (physical_same_location(&copies[i].dst, &copies[i].src))
      continue;
    if (copies[i].src.kind != AST_PHYSICAL_OPERAND_REGISTER ||
        copies[i].dst.kind != AST_PHYSICAL_OPERAND_REGISTER)
      register_only = 0;
  }
  if (register_only && count) {
    uint8_t local_active[AST_PHYSICAL_MAX_OPERANDS];
    int heap = count > AST_PHYSICAL_MAX_OPERANDS;
    uint8_t *active = heap ? (uint8_t *)malloc(count) : local_active;
    if (!active)
      return 0;
    uint32_t remaining = 0u;
    for (uint32_t i = 0; i < count; ++i) {
      active[i] =
          (uint8_t)!physical_same_location(&copies[i].dst, &copies[i].src);
      remaining += active[i] != 0u;
    }
    while (remaining) {
      int progress = 0;
      for (uint32_t i = 0; i < count; ++i) {
        if (!active[i])
          continue;
        int needed = 0;
        for (uint32_t j = 0; j < count; ++j) {
          if (i == j || !active[j] ||
              copies[j].src.kind != AST_PHYSICAL_OPERAND_REGISTER)
            continue;
          if (copies[i].dst.register_unit == copies[j].src.register_unit) {
            needed = 1;
            break;
          }
        }
        if (needed)
          continue;
        if (!rv_move(c, copies[i].dst, copies[i].src)) {
          if (heap)
            free(active);
          return 0;
        }
        active[i] = 0u;
        --remaining;
        progress = 1;
      }
      if (progress)
        continue;
      uint32_t cycle = AST_MACHINE_INVALID_ID;
      for (uint32_t i = 0; i < count; ++i)
        if (active[i]) {
          cycle = i;
          break;
        }
      if (cycle == AST_MACHINE_INVALID_ID)
        break;
      uint64_t bytes = ((uint64_t)copies[cycle].src.bit_width + 7u) / 8u;
      if (c->fn->copy_temp == AST_MACHINE_INVALID_ID)
        c->fn->copy_temp = rv_temp(c, bytes, AST_FRAME_OBJECT_COPY_TEMP);
      else if (c->fn->objects[c->fn->copy_temp].size < bytes)
        c->fn->objects[c->fn->copy_temp].size = bytes;
      if (c->fn->copy_temp == AST_MACHINE_INVALID_ID) {
        if (heap)
          free(active);
        return 0;
      }
      AstPhysicalOperand temp =
          physical_frame(c->fn->copy_temp, copies[cycle].src.bit_width, 0);
      if (!rv_move(c, temp, copies[cycle].src)) {
        if (heap)
          free(active);
        return 0;
      }
      copies[cycle].src = temp;
    }
    if (heap)
      free(active);
    return 1;
  }
  uint64_t size = 0u;
  for (uint32_t i = 0; i < count; ++i)
    if (!physical_same_location(&copies[i].dst, &copies[i].src))
      size += (copies[i].src.bit_width + 7u) / 8u;
  if (!size)
    return 1;
  if (c->fn->copy_temp == AST_MACHINE_INVALID_ID)
    c->fn->copy_temp = rv_temp(c, size, AST_FRAME_OBJECT_COPY_TEMP);
  if (c->fn->copy_temp == AST_MACHINE_INVALID_ID)
    return 0;
  if (c->fn->objects[c->fn->copy_temp].size < size)
    c->fn->objects[c->fn->copy_temp].size = size;
  uint64_t off = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    if (physical_same_location(&copies[i].dst, &copies[i].src))
      continue;
    if (!rv_move(c,
                 physical_frame(c->fn->copy_temp, copies[i].src.bit_width,
                                (int64_t)off),
                 copies[i].src))
      return 0;
    off += (copies[i].src.bit_width + 7u) / 8u;
  }
  for (uint32_t phase = 0; phase < 2u; ++phase) {
    off = 0u;
    for (uint32_t i = 0; i < count; ++i) {
      if (physical_same_location(&copies[i].dst, &copies[i].src))
        continue;
      AstPhysicalOperand temp = physical_frame(
          c->fn->copy_temp, copies[i].src.bit_width, (int64_t)off);
      if ((copies[i].dst.kind == AST_PHYSICAL_OPERAND_MEMORY) ==
              (phase == 0u) &&
          !rv_move(c, copies[i].dst, temp))
        return 0;
      off += (copies[i].src.bit_width + 7u) / 8u;
    }
  }
  return 1;
}
static uint32_t add_block(RvContext *c) {
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
static int lower_branch(RvContext *c, const MachineInstruction *mi,
                        uint32_t source_block) {
  int conditional = mi->info.opcode == AST_MACHINE_RISCV_COND_BR;
  uint32_t cursor = 0u, predecessor = c->block, targets[2] = {0};
  RvWork work = {0};
  AstPhysicalOperand condition = {0};
  if (conditional) {
    if (!mi->info.operand_count ||
        mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    condition = rv_home(c, mi->info.operands[cursor++].as.vreg);
    if (condition.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[condition.register_unit] = 1u;
    condition = rv_ensure(c, &work, condition, AST_MACHINE_BANK_GPR);
    if (!condition.kind)
      return 0;
  }
  for (uint32_t edge = 0; edge < (conditional ? 2u : 1u); ++edge) {
    if (cursor >= mi->info.operand_count ||
        mi->info.operands[cursor].kind != AST_MACHINE_OPERAND_BLOCK)
      return 0;
    uint32_t target = mi->info.operands[cursor++].as.block;
    if (target >= c->machine->info.block_count)
      return 0;
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
    uint32_t used = 0;
    for (uint32_t v = 0; v < n; ++v) {
      if (mi->info.operands[cursor + v].kind != AST_MACHINE_OPERAND_VREG) {
        free(copies);
        return 0;
      }
      AstPhysicalOperand dst = physical_home_at_slot(
          c, successor->arguments[v], c->fn->liveness.block_begin[target]);
      if (dst.kind) {
        copies[used].dst = dst;
        copies[used++].src = rv_home(c, mi->info.operands[cursor + v].as.vreg);
      }
    }
    cursor += n;
    for (uint32_t v = 0; v < c->machine->info.vreg_count; ++v) {
      if (!machine_liveness_test_bit(
              &c->fn->liveness.blocks[source_block].live_out, v) ||
          !machine_liveness_test_bit(&c->fn->liveness.blocks[target].live_in,
                                     v))
        continue;
      AstPhysicalOperand src = rv_home(c, v);
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
    targets[edge] = target;
    if (conditional) {
      targets[edge] = add_block(c);
      if (targets[edge] == AST_MACHINE_INVALID_ID) {
        free(copies);
        return 0;
      }
      c->block = targets[edge];
      ++c->program->stats.edge_blocks;
    }
    int ok = rv_restore(c, &work) && rv_parallel_copy(c, copies, used);
    AstPhysicalOperand dest = rv_block(target);
    if (ok)
      ok = rv_emit(c, AST_MACHINE_RISCV_J, 0u, &dest, 1u);
    free(copies);
    c->block = predecessor;
    if (!ok)
      return 0;
  }
  if (cursor != mi->info.operand_count)
    return 0;
  if (!conditional)
    return 1;
  AstPhysicalOperand args[] = {
      condition, rv_reg(c, AST_MACHINE_BANK_GPR, 0u, condition.bit_width),
      rv_block(targets[0])};
  AstPhysicalOperand dest = rv_block(targets[1]);
  return rv_emit(c, AST_MACHINE_RISCV_BNE, 0u, args, 3u) &&
         rv_emit(c, AST_MACHINE_RISCV_J, 0u, &dest, 1u);
}
static int lower_constant(RvContext *c, const MachineInstruction *mi) {
  AstMirInstructionInfo source = {0};
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  if (mi->info.source_instruction != AST_MIR_INVALID_ID &&
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source))
    return 0;
  for (uint32_t part = 0; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    AstPhysicalOperand dst = rv_home(c, v);
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
    if (mi->info.opcode == AST_MACHINE_RISCV_UNDEF)
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
      uint32_t image =
          rv_temp(c, dst.bit_width / 8u, AST_FRAME_OBJECT_ABI_TEMP);
      if (image == AST_MACHINE_INVALID_ID ||
          !rv_move(c, physical_frame(image, dst.bit_width, 0),
                   physical_immediate(bits, dst.bit_width)) ||
          !rv_move(c, dst, physical_frame(image, dst.bit_width, 0)))
        return 0;
      continue;
    }
    if (!rv_move(c, dst, physical_immediate(bits, dst.bit_width)))
      return 0;
  }
  return 1;
}
static int lower_copy(RvContext *c, const MachineInstruction *mi) {
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
    AstPhysicalOperand dst = rv_home(c, mi->info.result_first + i);
    if (!dst.kind)
      continue;
    if (mi->info.operands[i].kind != AST_MACHINE_OPERAND_VREG) {
      if (heap)
        free(copies);
      return 0;
    }
    copies[count].dst = dst;
    copies[count++].src = rv_home(c, mi->info.operands[i].as.vreg);
  }
  int ok = rv_parallel_copy(c, copies, count);
  if (heap)
    free(copies);
  return ok;
}
static int lower_cast(RvContext *c, RvWork *w, const MachineInstruction *mi,
                      AstPhysicalOperand result, AstPhysicalOperand source) {
  const AstSemaType *from =
      c->machine->vregs[mi->info.operands[0].as.vreg].type;
  const AstSemaType *to = c->machine->vregs[mi->info.result_first].type;
  uint32_t src_bits = source.bit_width, dst_bits = result.bit_width;
  int sign =
      mi->info.token_op == AST_TOK_KW_SEXT ||
      (mi->info.token_op != AST_TOK_KW_ZEXT && ast_sema_type_is_signed(from));
  if (result.bank == AST_MACHINE_BANK_GPR &&
      source.bank == AST_MACHINE_BANK_GPR)
    return rv_normalize(c, result, source,
                        src_bits < dst_bits ? src_bits : dst_bits,
                        sign && src_bits < dst_bits);
  if (mi->info.token_op == AST_TOK_KW_BITCAST ||
      mi->info.token_op == AST_TOK_KW_PTRCAST)
    return src_bits == dst_bits && (src_bits == 32u || src_bits == 64u) &&
           rv_move(c, result, source);
  AstMachineOpcode opcode;
  uint32_t rounding =
      0u; /* RNE; casts from floating point to integer use RTZ. */
  if (result.bank == AST_MACHINE_BANK_FP &&
      source.bank == AST_MACHINE_BANK_GPR) {
    if (src_bits < 32u) {
      AstPhysicalOperand normalized =
          rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
      if (!normalized.kind ||
          !rv_normalize(c, normalized, source, src_bits, sign))
        return 0;
      source = normalized;
    }
    if (dst_bits == 32u)
      opcode = src_bits <= 32u ? (sign ? AST_MACHINE_RISCV_FCVT_S_W
                                       : AST_MACHINE_RISCV_FCVT_S_WU)
                               : (sign ? AST_MACHINE_RISCV_FCVT_S_L
                                       : AST_MACHINE_RISCV_FCVT_S_LU);
    else if (dst_bits == 64u)
      opcode = src_bits <= 32u ? (sign ? AST_MACHINE_RISCV_FCVT_D_W
                                       : AST_MACHINE_RISCV_FCVT_D_WU)
                               : (sign ? AST_MACHINE_RISCV_FCVT_D_L
                                       : AST_MACHINE_RISCV_FCVT_D_LU);
    else
      return 0;
    source = rv_reg(c, AST_MACHINE_BANK_GPR, source.register_unit % 64u, 64u);
  } else if (result.bank == AST_MACHINE_BANK_GPR &&
             source.bank == AST_MACHINE_BANK_FP) {
    sign = ast_sema_type_is_signed(to);
    rounding = 1u;
    if (src_bits == 32u)
      opcode = dst_bits <= 32u ? (sign ? AST_MACHINE_RISCV_FCVT_W_S
                                       : AST_MACHINE_RISCV_FCVT_WU_S)
                               : (sign ? AST_MACHINE_RISCV_FCVT_L_S
                                       : AST_MACHINE_RISCV_FCVT_LU_S);
    else if (src_bits == 64u)
      opcode = dst_bits <= 32u ? (sign ? AST_MACHINE_RISCV_FCVT_W_D
                                       : AST_MACHINE_RISCV_FCVT_WU_D)
                               : (sign ? AST_MACHINE_RISCV_FCVT_L_D
                                       : AST_MACHINE_RISCV_FCVT_LU_D);
    else
      return 0;
    result = rv_reg(c, AST_MACHINE_BANK_GPR, result.register_unit % 64u, 64u);
  } else if (result.bank == AST_MACHINE_BANK_FP &&
             source.bank == AST_MACHINE_BANK_FP) {
    if (src_bits == dst_bits)
      return rv_move(c, result, source);
    if (src_bits == 32u && dst_bits == 64u)
      opcode = AST_MACHINE_RISCV_FCVT_D_S;
    else if (src_bits == 64u && dst_bits == 32u)
      opcode = AST_MACHINE_RISCV_FCVT_S_D;
    else
      return 0;
  } else
    return fail_instruction(c,
                            "Vector casts require explicit lane legalization.");
  AstPhysicalOperand args[] = {result, source,
                               physical_immediate(rounding, 8u)};
  return rv_emit(c, opcode, 1u, args, 3u);
}
/* Baseline RV64I has no integer multiply/divide instructions.  Keep the
 * freestanding target self-contained instead of silently introducing an M
 * requirement or compiler-runtime helper.  Operands arrive normalized to
 * XLEN by lower_regular(); the final rv_normalize restores the source width. */
static int rv_soft_mul(RvContext *c, RvWork *w, AstPhysicalOperand dst,
                       AstPhysicalOperand lhs, AstPhysicalOperand rhs,
                       uint32_t width, int sign) {
  AstPhysicalOperand a = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand b = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand acc = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand t = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand count = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  if (!a.kind || !b.kind || !acc.kind || !t.kind || !count.kind ||
      !rv_move(c, a, lhs) || !rv_move(c, b, rhs) ||
      !rv_move(c, acc, physical_immediate(0u, 64u)) ||
      !rv_move(c, count, physical_immediate(width, 64u)))
    return 0;
  uint32_t predecessor = c->block;
  uint32_t loop = add_block(c), join = add_block(c);
  if (loop == AST_MACHINE_INVALID_ID || join == AST_MACHINE_INVALID_ID)
    return 0;
  AstPhysicalOperand loop_target = rv_block(loop), join_target = rv_block(join);
  c->block = predecessor;
  if (!rv_emit(c, AST_MACHINE_RISCV_J, 0u, &loop_target, 1u))
    return 0;
  c->block = loop;
  AstPhysicalOperand shlmask[] = {t, b, physical_immediate(63u, 64u)};
  AstPhysicalOperand sarmask[] = {t, t, physical_immediate(63u, 64u)};
  AstPhysicalOperand andterm[] = {t, a, t};
  AstPhysicalOperand addacc[] = {acc, acc, t};
  AstPhysicalOperand shla[] = {a, a, physical_immediate(1u, 64u)};
  AstPhysicalOperand shrb[] = {b, b, physical_immediate(1u, 64u)};
  AstPhysicalOperand dec[] = {count, count,
                              physical_immediate((uint64_t)-1, 64u)};
  AstPhysicalOperand branch[] = {
      count, rv_reg(c, AST_MACHINE_BANK_GPR, 0u, 64u), loop_target};
  if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, shlmask, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SRAI, 1u, sarmask, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_AND, 1u, andterm, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_ADD, 1u, addacc, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, shla, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, shrb, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_ADDI, 1u, dec, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_BNE, 0u, branch, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_J, 0u, &join_target, 1u))
    return 0;
  c->block = join;
  return rv_normalize(c, dst, acc, width, sign);
}

static int rv_soft_divrem(RvContext *c, RvWork *w, AstPhysicalOperand dst,
                          AstPhysicalOperand lhs, AstPhysicalOperand rhs,
                          uint32_t width, int sign, int remainder) {
  AstPhysicalOperand n = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand d = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand q = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand r = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand t = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand count = rv_borrow(c, w, AST_MACHINE_BANK_GPR, 64u);
  if (!n.kind || !d.kind || !q.kind || !r.kind || !t.kind || !count.kind ||
      !rv_move(c, n, lhs) || !rv_move(c, d, rhs))
    return 0;
  if (sign) {
    /* Reuse t/count for the sign masks before the loop.  They are
     * recomputed from the untouched source operands after the loop,
     * keeping the signed i32 path inside the eight-scratch contract. */
    AstPhysicalOperand s1[] = {t, n, physical_immediate(63u, 64u)};
    AstPhysicalOperand s2[] = {count, d, physical_immediate(63u, 64u)};
    AstPhysicalOperand x1[] = {n, n, t};
    AstPhysicalOperand x2[] = {d, d, count};
    AstPhysicalOperand sub1[] = {n, n, t};
    AstPhysicalOperand sub2[] = {d, d, count};
    if (!rv_emit(c, AST_MACHINE_RISCV_SRAI, 1u, s1, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SRAI, 1u, s2, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, x1, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, x2, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, sub1, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, sub2, 3u))
      return 0;
  }
  /* The restoring loop consumes the most significant dividend bit on
   * every iteration. Narrow 8/16/32-bit inputs must be left-aligned in
   * XLEN before the loop, otherwise successive shifts expose several bits
   * at once (e.g. u8(7)/3 could spuriously produce 3 instead of 2).
   * Work on the unsigned absolute magnitude for signed division. */
  if (width < 64u) {
    AstPhysicalOperand align_n[] = {n, n, physical_immediate(64u - width, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, align_n, 3u))
      return 0;
  }
  if (!rv_move(c, q, physical_immediate(0u, 64u)) ||
      !rv_move(c, r, physical_immediate(0u, 64u)) ||
      !rv_move(c, count, physical_immediate(width, 64u)))
    return 0;
  uint32_t predecessor = c->block;
  uint32_t loop = add_block(c), join = add_block(c);
  if (loop == AST_MACHINE_INVALID_ID || join == AST_MACHINE_INVALID_ID)
    return 0;
  AstPhysicalOperand loop_target = rv_block(loop), join_target = rv_block(join);
  c->block = predecessor;
  if (!rv_emit(c, AST_MACHINE_RISCV_J, 0u, &loop_target, 1u))
    return 0;
  c->block = loop;
  /* Restoring unsigned division, written branchlessly inside one compact
   * physical loop.  With d==0 it naturally yields DIVU=-1 and
   * REMU=dividend, matching RV64M. */
  AstPhysicalOperand top[] = {t, n, physical_immediate(63u, 64u)};
  AstPhysicalOperand rsh[] = {r, r, physical_immediate(1u, 64u)};
  AstPhysicalOperand ror[] = {r, r, t};
  AstPhysicalOperand nsh[] = {n, n, physical_immediate(1u, 64u)};
  AstPhysicalOperand lt[] = {t, r, d};
  AstPhysicalOperand ge[] = {t, t, physical_immediate(1u, 64u)};
  AstPhysicalOperand qsh[] = {q, q, physical_immediate(1u, 64u)};
  AstPhysicalOperand qor[] = {q, q, t};
  AstPhysicalOperand neg[] = {t, t};
  AstPhysicalOperand maskd[] = {t, d, t};
  AstPhysicalOperand subr[] = {r, r, t};
  AstPhysicalOperand dec[] = {count, count,
                              physical_immediate((uint64_t)-1, 64u)};
  AstPhysicalOperand branch[] = {
      count, rv_reg(c, AST_MACHINE_BANK_GPR, 0u, 64u), loop_target};
  if (!rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, top, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, rsh, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_OR, 1u, ror, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, nsh, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SLTU, 1u, lt, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_XORI, 1u, ge, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, qsh, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_OR, 1u, qor, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_NEG, 1u, neg, 2u) ||
      !rv_emit(c, AST_MACHINE_RISCV_AND, 1u, maskd, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, subr, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_ADDI, 1u, dec, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_BNE, 0u, branch, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_J, 0u, &join_target, 1u))
    return 0;
  c->block = join;
  if (sign) {
    /* Quotient sign is lhs^rhs; remainder follows lhs.  Recompute both
     * sign masks from the immutable normalized operands now that count is
     * dead.  A zero divisor is fixed up to signed DIV=-1. */
    AstPhysicalOperand lsign[] = {t, lhs, physical_immediate(63u, 64u)};
    AstPhysicalOperand rsign[] = {count, rhs, physical_immediate(63u, 64u)};
    AstPhysicalOperand sx[] = {count, t, count};
    AstPhysicalOperand qx[] = {q, q, count};
    AstPhysicalOperand qs[] = {q, q, count};
    AstPhysicalOperand rx[] = {r, r, t};
    AstPhysicalOperand rs[] = {r, r, t};
    if (!rv_emit(c, AST_MACHINE_RISCV_SRAI, 1u, lsign, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SRAI, 1u, rsign, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, sx, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, qx, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, qs, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, rx, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, rs, 3u))
      return 0;
    AstPhysicalOperand z[] = {t, d, physical_immediate(1u, 64u)};
    AstPhysicalOperand zm[] = {t, t};
    AstPhysicalOperand qfix[] = {q, q, t};
    if (!rv_emit(c, AST_MACHINE_RISCV_SLTIU, 1u, z, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_NEG, 1u, zm, 2u) ||
        !rv_emit(c, AST_MACHINE_RISCV_OR, 1u, qfix, 3u))
      return 0;
  }
  return rv_normalize(c, dst, remainder ? r : q, width, sign);
}

static int rv_source_constant_u64(RvContext *c, AstMirVReg vreg, uint64_t *out);

static int rv_source_type(RvContext *c, AstMirVReg vreg, AstMirVRegInfo *out) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  return mf && out && ast_mir_function_vreg(mf, vreg, out);
}

static uint32_t rv_value_image(RvContext *c, AstMirVReg source,
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
    if (!bits || bits > 64u || (bits & 7u) || bytes > UINT64_MAX - bits / 8u)
      return AST_MACHINE_INVALID_ID;
    bytes += bits / 8u;
  }
  uint32_t image = rv_temp(c, bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (image == AST_MACHINE_INVALID_ID)
    return image;
  uint64_t off = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t v = first + part, bits = c->machine->vregs[v].bit_width;
    AstPhysicalOperand home = rv_home(c, v);
    if (!home.kind ||
        !rv_move(c, physical_frame(image, bits, (int64_t)off), home))
      return AST_MACHINE_INVALID_ID;
    off += bits / 8u;
  }
  if (bytes_out)
    *bytes_out = bytes;
  return image;
}

static uint32_t rv_result_image(RvContext *c, const MachineInstruction *mi,
                                uint64_t *bytes_out) {
  if (!c || !mi || !mi->info.result_count)
    return AST_MACHINE_INVALID_ID;
  uint64_t bytes = 0u;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    if (v >= c->machine->info.vreg_count)
      return AST_MACHINE_INVALID_ID;
    uint32_t bits = c->machine->vregs[v].bit_width;
    if (!bits || bits > 64u || (bits & 7u) || bytes > UINT64_MAX - bits / 8u)
      return AST_MACHINE_INVALID_ID;
    bytes += bits / 8u;
  }
  uint32_t image = rv_temp(c, bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (image == AST_MACHINE_INVALID_ID)
    return image;
  for (uint64_t off = 0u; off < bytes; off += 8u) {
    uint64_t remain = bytes - off;
    uint32_t bits = (uint32_t)(remain >= 8u ? 64u : remain * 8u);
    if (!rv_move(c, physical_frame(image, bits, (int64_t)off),
                 physical_immediate(0u, bits)))
      return AST_MACHINE_INVALID_ID;
  }
  if (bytes_out)
    *bytes_out = bytes;
  return image;
}

static int rv_commit_result_image(RvContext *c, const MachineInstruction *mi,
                                  uint32_t image) {
  uint64_t off = 0u;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    AstPhysicalOperand home = rv_home(c, v);
    uint32_t bits = c->machine->vregs[v].bit_width;
    if (home.kind &&
        !rv_move(c, home, physical_frame(image, bits, (int64_t)off)))
      return 0;
    off += bits / 8u;
  }
  return 1;
}

static int rv_vector_shape(const AstSemaType *type,
                           const AstSemaType **element_out, uint64_t *lanes_out,
                           uint32_t *element_bits_out) {
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
  if (element_bits_out)
    *element_bits_out = bits;
  return 1;
}

static int rv_load_integer_lane(RvContext *c, AstPhysicalOperand dst,
                                uint32_t image, uint32_t bits, uint64_t offset,
                                int sign) {
  if (!rv_move(c, dst, physical_frame(image, bits, (int64_t)offset)))
    return 0;
  return bits == 64u || rv_normalize(c, dst, dst, bits, sign);
}

static int rv_scalar_integer_binary(RvContext *c, AstTokenKind token,
                                    AstPhysicalOperand dst,
                                    AstPhysicalOperand lhs,
                                    AstPhysicalOperand rhs, uint32_t bits,
                                    int sign) {
  AstMachineOpcode op = AST_MACHINE_OP_INVALID;
  switch (token) {
  case AST_TOK_PLUS:
    op = AST_MACHINE_RISCV_ADD;
    break;
  case AST_TOK_MINUS:
    op = AST_MACHINE_RISCV_SUB;
    break;
  case AST_TOK_AMP:
  case AST_TOK_AMP_AMP:
    op = AST_MACHINE_RISCV_AND;
    break;
  case AST_TOK_PIPE:
  case AST_TOK_PIPE_PIPE:
    op = AST_MACHINE_RISCV_OR;
    break;
  case AST_TOK_CARET:
    op = AST_MACHINE_RISCV_XOR;
    break;
  case AST_TOK_STAR:
    if (c->program->target.features & AST_TARGET_FEATURE_RV_M) {
      op = AST_MACHINE_RISCV_MUL;
    } else {
      RvWork inner = {0};
      if (lhs.kind == AST_PHYSICAL_OPERAND_REGISTER)
        inner.excluded[lhs.register_unit] = 1u;
      if (rhs.kind == AST_PHYSICAL_OPERAND_REGISTER)
        inner.excluded[rhs.register_unit] = 1u;
      if (dst.kind == AST_PHYSICAL_OPERAND_REGISTER)
        inner.excluded[dst.register_unit] = 1u;
      int ok = rv_soft_mul(c, &inner, dst, lhs, rhs, bits, 0);
      return ok && rv_restore(c, &inner);
    }
    break;
  case AST_TOK_SLASH:
  case AST_TOK_PERCENT:
    if (!(c->program->target.features & AST_TARGET_FEATURE_RV_M)) {
      RvWork inner = {0};
      if (lhs.kind == AST_PHYSICAL_OPERAND_REGISTER)
        inner.excluded[lhs.register_unit] = 1u;
      if (rhs.kind == AST_PHYSICAL_OPERAND_REGISTER)
        inner.excluded[rhs.register_unit] = 1u;
      if (dst.kind == AST_PHYSICAL_OPERAND_REGISTER)
        inner.excluded[dst.register_unit] = 1u;
      int ok = rv_soft_divrem(c, &inner, dst, lhs, rhs, bits, sign,
                              token == AST_TOK_PERCENT);
      return ok && rv_restore(c, &inner);
    }
    op = token == AST_TOK_PERCENT
             ? (sign ? AST_MACHINE_RISCV_REM : AST_MACHINE_RISCV_REMU)
             : (sign ? AST_MACHINE_RISCV_DIV : AST_MACHINE_RISCV_DIVU);
    break;
  case AST_TOK_SHIFT_LEFT:
  case AST_TOK_SHIFT_RIGHT: {
    uint32_t keep = bits == 8u ? 3u : bits == 16u ? 4u : bits == 32u ? 5u : 6u;
    AstPhysicalOperand count = rhs;
    AstPhysicalOperand shl[] = {count, count,
                                physical_immediate(64u - keep, 64u)};
    AstPhysicalOperand shr[] = {count, count,
                                physical_immediate(64u - keep, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, shl, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, shr, 3u))
      return 0;
    if (token == AST_TOK_SHIFT_RIGHT && sign) {
      if (bits < 64u && !rv_normalize(c, lhs, lhs, bits, 1))
        return 0;
      op = AST_MACHINE_RISCV_SRA;
    } else {
      if (bits < 64u && !rv_normalize(c, lhs, lhs, bits, 0))
        return 0;
      op = token == AST_TOK_SHIFT_LEFT ? AST_MACHINE_RISCV_SLL
                                       : AST_MACHINE_RISCV_SRL;
    }
    AstPhysicalOperand args[] = {dst, lhs, count};
    return rv_emit(c, op, 1u, args, 3u);
  }
  default:
    return 0;
  }
  if (sign && (token == AST_TOK_SLASH || token == AST_TOK_PERCENT)) {
    if (bits < 64u && (!rv_normalize(c, lhs, lhs, bits, 1) ||
                       !rv_normalize(c, rhs, rhs, bits, 1)))
      return 0;
  } else if ((token == AST_TOK_SLASH || token == AST_TOK_PERCENT) &&
             bits < 64u) {
    if (!rv_normalize(c, lhs, lhs, bits, 0) ||
        !rv_normalize(c, rhs, rhs, bits, 0))
      return 0;
  }
  AstPhysicalOperand args[] = {dst, lhs, rhs};
  return op != AST_MACHINE_OP_INVALID && rv_emit(c, op, 1u, args, 3u);
}

static int rv_lower_fixed_vector_binary(RvContext *c,
                                        const MachineInstruction *mi,
                                        const AstMirInstructionInfo *source) {
  if (!c || !mi || !source || source->op != AST_MIR_OP_BINARY ||
      source->operand_count != 2u || source->result == AST_MIR_INVALID_ID ||
      !mi->info.result_count)
    return 0;
  AstMirVRegInfo result_vi, lhs_vi, rhs_vi;
  if (!rv_source_type(c, source->result, &result_vi) ||
      !rv_source_type(c, source->operands[0], &lhs_vi) ||
      !rv_source_type(c, source->operands[1], &rhs_vi) || !result_vi.type ||
      ast_sema_type_kind(result_vi.type) != AST_SEMA_TYPE_VECTOR ||
      !lhs_vi.type || ast_sema_type_kind(lhs_vi.type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!rv_vector_shape(result_vi.type, &element, &lanes, &bits) ||
      ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT)
    return 0;
  uint64_t lhs_bytes = 0u, rhs_bytes = 0u, out_bytes = 0u;
  uint32_t lhs_image = rv_value_image(c, source->operands[0], &lhs_bytes);
  if (lhs_image == AST_MACHINE_INVALID_ID)
    return 0;
  int rhs_vector = ast_sema_type_kind(rhs_vi.type) == AST_SEMA_TYPE_VECTOR;
  uint32_t rhs_image = AST_MACHINE_INVALID_ID;
  if (rhs_vector) {
    rhs_image = rv_value_image(c, source->operands[1], &rhs_bytes);
    if (rhs_image == AST_MACHINE_INVALID_ID || rhs_bytes != lhs_bytes)
      return 0;
  } else if (source->token_op != AST_TOK_SHIFT_LEFT &&
             source->token_op != AST_TOK_SHIFT_RIGHT)
    return 0;
  uint32_t out_image = rv_result_image(c, mi, &out_bytes);
  uint64_t logical = ast_sema_type_size(result_vi.type);
  if (out_image == AST_MACHINE_INVALID_ID || logical == UINT64_MAX ||
      !logical || logical > lhs_bytes || logical > out_bytes)
    return 0;

  RvWork work = {0};
  AstPhysicalOperand lhs = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand rhs = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand dst = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!lhs.kind || !rhs.kind || !dst.kind)
    return 0;
  AstPhysicalOperand scalar_rhs = {0};
  if (!rhs_vector) {
    uint32_t first = c->machine->source_first[source->operands[1]];
    if (first == AST_MACHINE_INVALID_ID ||
        c->machine->source_parts[source->operands[1]] != 1u)
      return 0;
    scalar_rhs = rv_home(c, first);
    if (!scalar_rhs.kind || !rv_move(c, rhs, scalar_rhs))
      return 0;
  }
  int sign = ast_sema_type_is_signed(element);
  uint64_t lane_bytes = bits / 8u;
  for (uint64_t lane = 0u; lane < lanes; ++lane) {
    uint64_t off = lane * lane_bytes;
    int signed_lhs = sign && (source->token_op == AST_TOK_SLASH ||
                              source->token_op == AST_TOK_PERCENT ||
                              source->token_op == AST_TOK_SHIFT_RIGHT);
    if (!rv_load_integer_lane(c, lhs, lhs_image, bits, off, signed_lhs))
      return 0;
    if (rhs_vector) {
      int signed_rhs = sign && (source->token_op == AST_TOK_SLASH ||
                                source->token_op == AST_TOK_PERCENT);
      if (!rv_load_integer_lane(c, rhs, rhs_image, bits, off, signed_rhs))
        return 0;
    }
    if (!rv_scalar_integer_binary(c, source->token_op, dst, lhs, rhs, bits,
                                  sign))
      return 0;
    if (!rv_move(c, physical_frame(out_image, bits, (int64_t)off), dst))
      return 0;
  }
  if (!rv_restore(c, &work))
    return 0;
  if (!rv_commit_result_image(c, mi, out_image))
    return 0;
  return 1;
}

static int rv_integer_compare_lane(RvContext *c, AstTokenKind token,
                                   AstPhysicalOperand dst,
                                   AstPhysicalOperand lhs,
                                   AstPhysicalOperand rhs, int sign) {
  AstPhysicalOperand args[] = {dst, lhs, rhs};
  int invert = 0;
  if (token == AST_TOK_EQ_EQ || token == AST_TOK_BANG_EQ) {
    if (!rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, args, 3u))
      return 0;
    args[1] = dst;
    args[2] = physical_immediate(1u, 64u);
    if (!rv_emit(c, AST_MACHINE_RISCV_SLTIU, 1u, args, 3u))
      return 0;
    invert = token == AST_TOK_BANG_EQ;
  } else {
    if (token == AST_TOK_GT || token == AST_TOK_LT_EQ) {
      args[1] = rhs;
      args[2] = lhs;
    }
    invert = token == AST_TOK_LT_EQ || token == AST_TOK_GT_EQ;
    if (!rv_emit(c, sign ? AST_MACHINE_RISCV_SLT : AST_MACHINE_RISCV_SLTU, 1u,
                 args, 3u))
      return 0;
  }
  if (invert) {
    AstPhysicalOperand inv[] = {dst, dst, physical_immediate(1u, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_XORI, 1u, inv, 3u))
      return 0;
  }
  return 1;
}

static int rv_float_compare_lane(RvContext *c, AstTokenKind token,
                                 AstPhysicalOperand dst, AstPhysicalOperand lhs,
                                 AstPhysicalOperand rhs, uint32_t bits) {
  AstMachineOpcode op;
  int invert = 0, reverse = 0;
  if (token == AST_TOK_EQ_EQ || token == AST_TOK_BANG_EQ) {
    op = bits == 64u ? AST_MACHINE_RISCV_FEQ_D : AST_MACHINE_RISCV_FEQ_S;
    invert = token == AST_TOK_BANG_EQ;
  } else if (token == AST_TOK_LT || token == AST_TOK_GT) {
    op = bits == 64u ? AST_MACHINE_RISCV_FLT_D : AST_MACHINE_RISCV_FLT_S;
    reverse = token == AST_TOK_GT;
  } else if (token == AST_TOK_LT_EQ || token == AST_TOK_GT_EQ) {
    op = bits == 64u ? AST_MACHINE_RISCV_FLE_D : AST_MACHINE_RISCV_FLE_S;
    reverse = token == AST_TOK_GT_EQ;
  } else
    return 0;
  AstPhysicalOperand args[] = {dst, reverse ? rhs : lhs, reverse ? lhs : rhs};
  if (!rv_emit(c, op, 1u, args, 3u))
    return 0;
  if (invert) {
    AstPhysicalOperand inv[] = {dst, dst, physical_immediate(1u, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_XORI, 1u, inv, 3u))
      return 0;
  }
  return 1;
}

static int rv_lower_fixed_vector_compare(RvContext *c,
                                         const MachineInstruction *mi,
                                         const AstMirInstructionInfo *source) {
  if (!c || !mi || !source || source->op != AST_MIR_OP_BINARY ||
      source->operand_count != 2u || source->result == AST_MIR_INVALID_ID ||
      mi->info.result_count != 1u)
    return 0;
  AstMirVRegInfo result_vi, lhs_vi, rhs_vi;
  if (!rv_source_type(c, source->result, &result_vi) ||
      !rv_source_type(c, source->operands[0], &lhs_vi) ||
      !rv_source_type(c, source->operands[1], &rhs_vi) || !result_vi.type ||
      !lhs_vi.type || !rhs_vi.type ||
      ast_sema_type_kind(result_vi.type) != AST_SEMA_TYPE_MASK ||
      ast_sema_type_kind(lhs_vi.type) != AST_SEMA_TYPE_VECTOR ||
      ast_sema_type_kind(rhs_vi.type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!rv_vector_shape(lhs_vi.type, &element, &lanes, &bits) || lanes > 64u)
    return 0;
  uint64_t lhs_bytes = 0u, rhs_bytes = 0u;
  uint32_t lhs_image = rv_value_image(c, source->operands[0], &lhs_bytes);
  uint32_t rhs_image = rv_value_image(c, source->operands[1], &rhs_bytes);
  if (lhs_image == AST_MACHINE_INVALID_ID ||
      rhs_image == AST_MACHINE_INVALID_ID || lhs_bytes != rhs_bytes)
    return 0;
  uint32_t mask_image = rv_temp(c, 8u, AST_FRAME_OBJECT_ABI_TEMP);
  if (mask_image == AST_MACHINE_INVALID_ID)
    return 0;
  RvWork work = {0};
  int fp = ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT;
  if (fp && ((bits == 32u &&
              !(c->program->target.features & AST_TARGET_FEATURE_RV_F)) ||
             (bits == 64u &&
              !(c->program->target.features & AST_TARGET_FEATURE_RV_D))))
    return fail_instruction(c, "RV64 floating vector comparison requires F/D "
                               "until software FP lowering is enabled.");
  AstPhysicalOperand lhs =
      rv_borrow(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                fp ? bits : 64u);
  AstPhysicalOperand rhs =
      rv_borrow(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                fp ? bits : 64u);
  AstPhysicalOperand bit = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand acc = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!lhs.kind || !rhs.kind || !bit.kind || !acc.kind ||
      !rv_move(c, acc, physical_immediate(0u, 64u)))
    return 0;
  int sign = !fp && ast_sema_type_is_signed(element);
  uint64_t lane_bytes = bits / 8u;
  for (uint64_t lane = 0u; lane < lanes; ++lane) {
    uint64_t off = lane * lane_bytes;
    if (fp) {
      if (!rv_move(c, lhs, physical_frame(lhs_image, bits, (int64_t)off)) ||
          !rv_move(c, rhs, physical_frame(rhs_image, bits, (int64_t)off)) ||
          !rv_float_compare_lane(c, source->token_op, bit, lhs, rhs, bits))
        return 0;
    } else {
      if (!rv_load_integer_lane(c, lhs, lhs_image, bits, off, sign) ||
          !rv_load_integer_lane(c, rhs, rhs_image, bits, off, sign) ||
          !rv_integer_compare_lane(c, source->token_op, bit, lhs, rhs, sign))
        return 0;
    }
    if (lane) {
      AstPhysicalOperand sh[] = {bit, bit, physical_immediate(lane, 64u)};
      if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, sh, 3u))
        return 0;
    }
    AstPhysicalOperand or_args[] = {acc, acc, bit};
    if (!rv_emit(c, AST_MACHINE_RISCV_OR, 1u, or_args, 3u))
      return 0;
  }
  if (!rv_move(c, physical_frame(mask_image, 64u, 0), acc) ||
      !rv_restore(c, &work))
    return 0;
  AstPhysicalOperand home = rv_home(c, mi->info.result_first);
  if (!home.kind)
    return 1;
  return rv_move(c, home, physical_frame(mask_image, home.bit_width, 0));
}

static int rv_lower_fixed_vector_select(RvContext *c,
                                        const MachineInstruction *mi,
                                        const AstMirInstructionInfo *source) {
  if (!c || !mi || !source || source->result == AST_MIR_INVALID_ID ||
      source->operand_count != 3u ||
      (source->op != AST_MIR_OP_SELECT &&
       !(source->op == AST_MIR_OP_INTRINSIC &&
         source->intrinsic == AST_HIR_INTRINSIC_SELECT)))
    return 0;
  AstMirVRegInfo result_vi, mask_vi;
  if (!rv_source_type(c, source->result, &result_vi) ||
      !rv_source_type(c, source->operands[0], &mask_vi) || !result_vi.type ||
      !mask_vi.type ||
      ast_sema_type_kind(result_vi.type) != AST_SEMA_TYPE_VECTOR ||
      ast_sema_type_kind(mask_vi.type) != AST_SEMA_TYPE_MASK)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!rv_vector_shape(result_vi.type, &element, &lanes, &bits) || lanes > 64u)
    return 0;
  uint64_t true_bytes = 0u, false_bytes = 0u, out_bytes = 0u;
  uint32_t true_image = rv_value_image(c, source->operands[1], &true_bytes);
  uint32_t false_image = rv_value_image(c, source->operands[2], &false_bytes);
  uint32_t out_image = rv_result_image(c, mi, &out_bytes);
  if (true_image == AST_MACHINE_INVALID_ID ||
      false_image == AST_MACHINE_INVALID_ID ||
      out_image == AST_MACHINE_INVALID_ID || true_bytes != false_bytes)
    return 0;
  uint32_t mask_first = c->machine->source_first[source->operands[0]];
  if (mask_first == AST_MACHINE_INVALID_ID ||
      c->machine->source_parts[source->operands[0]] != 1u)
    return 0;
  AstPhysicalOperand mask_home = rv_home(c, mask_first);
  if (!mask_home.kind)
    return 0;
  RvWork work = {0};
  AstPhysicalOperand mask = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand lane_mask = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand inv = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand yes = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand no = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand out = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!mask.kind || !lane_mask.kind || !inv.kind || !yes.kind || !no.kind ||
      !out.kind || !rv_normalize(c, mask, mask_home, mask_home.bit_width, 0))
    return 0;
  uint64_t lane_bytes = bits / 8u;
  for (uint64_t lane = 0u; lane < lanes; ++lane) {
    if (!rv_move(c, lane_mask, mask))
      return 0;
    if (lane) {
      AstPhysicalOperand sh[] = {lane_mask, lane_mask,
                                 physical_immediate(lane, 64u)};
      if (!rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, sh, 3u))
        return 0;
    }
    AstPhysicalOperand isolate[] = {lane_mask, lane_mask,
                                    physical_immediate(63u, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, isolate, 3u))
      return 0;
    AstPhysicalOperand signbit[] = {lane_mask, lane_mask,
                                    physical_immediate(63u, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_SRAI, 1u, signbit, 3u))
      return 0;
    uint64_t off = lane * lane_bytes;
    if (!rv_load_integer_lane(c, yes, true_image, bits, off, 0) ||
        !rv_load_integer_lane(c, no, false_image, bits, off, 0))
      return 0;
    AstPhysicalOperand not_args[] = {inv, lane_mask};
    AstPhysicalOperand and_yes[] = {yes, yes, lane_mask};
    AstPhysicalOperand and_no[] = {no, no, inv};
    AstPhysicalOperand or_out[] = {out, yes, no};
    if (!rv_emit(c, AST_MACHINE_RISCV_NOT, 1u, not_args, 2u) ||
        !rv_emit(c, AST_MACHINE_RISCV_AND, 1u, and_yes, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_AND, 1u, and_no, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_OR, 1u, or_out, 3u) ||
        !rv_move(c, physical_frame(out_image, bits, (int64_t)off), out))
      return 0;
  }
  if (!rv_restore(c, &work))
    return 0;
  return rv_commit_result_image(c, mi, out_image);
}

static int rv_lower_mask_index(RvContext *c, const MachineInstruction *mi,
                               const AstMirInstructionInfo *source) {
  if (!c || !mi || !source || source->op != AST_MIR_OP_INDEX ||
      source->operand_count != 2u || mi->info.result_count != 1u)
    return 0;
  AstMirVRegInfo base_vi;
  if (!rv_source_type(c, source->operands[0], &base_vi) || !base_vi.type ||
      ast_sema_type_kind(base_vi.type) != AST_SEMA_TYPE_MASK)
    return 0;
  uint64_t lanes = 0u;
  if (!ast_sema_type_vector_lanes(base_vi.type, &lanes) || !lanes ||
      lanes > 64u)
    return 0;
  uint32_t mask_first = c->machine->source_first[source->operands[0]];
  if (mask_first == AST_MACHINE_INVALID_ID ||
      c->machine->source_parts[source->operands[0]] != 1u)
    return 0;
  AstPhysicalOperand mask_home = rv_home(c, mask_first);
  AstPhysicalOperand result_home = rv_home(c, mi->info.result_first);
  if (!mask_home.kind || !result_home.kind)
    return 0;
  RvWork work = {0};
  AstPhysicalOperand value = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!value.kind || !rv_normalize(c, value, mask_home, mask_home.bit_width, 0))
    return 0;
  uint64_t constant = 0u;
  if (rv_source_constant_u64(c, source->operands[1], &constant)) {
    if (constant >= lanes)
      return 0;
    if (constant) {
      AstPhysicalOperand shr[] = {value, value,
                                  physical_immediate(constant, 64u)};
      if (!rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, shr, 3u))
        return 0;
    }
  } else {
    uint32_t index_first = c->machine->source_first[source->operands[1]];
    if (index_first == AST_MACHINE_INVALID_ID ||
        c->machine->source_parts[source->operands[1]] != 1u)
      return 0;
    AstPhysicalOperand index_home = rv_home(c, index_first);
    AstPhysicalOperand index = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
    if (!index_home.kind || !index.kind ||
        !rv_normalize(c, index, index_home, index_home.bit_width, 0))
      return 0;
    AstPhysicalOperand shr[] = {value, value, index};
    if (!rv_emit(c, AST_MACHINE_RISCV_SRL, 1u, shr, 3u))
      return 0;
  }
  AstPhysicalOperand left[] = {value, value, physical_immediate(63u, 64u)};
  AstPhysicalOperand right[] = {value, value, physical_immediate(63u, 64u)};
  if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, left, 3u) ||
      !rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, right, 3u))
    return 0;
  uint32_t image = rv_temp(c, 1u, AST_FRAME_OBJECT_ABI_TEMP);
  if (image == AST_MACHINE_INVALID_ID ||
      !rv_move(c, physical_frame(image, 8u, 0), value) || !rv_restore(c, &work))
    return 0;
  return rv_move(c, result_home,
                 physical_frame(image, result_home.bit_width, 0));
}

static int rv_lower_vector_index(RvContext *c, const MachineInstruction *mi,
                                 const AstMirInstructionInfo *source) {
  if (!c || !mi || !source || source->op != AST_MIR_OP_INDEX ||
      source->operand_count != 2u || mi->info.result_count != 1u)
    return 0;
  AstMirVRegInfo base_vi, result_vi;
  if (!rv_source_type(c, source->operands[0], &base_vi) ||
      !rv_source_type(c, source->result, &result_vi) || !base_vi.type ||
      !result_vi.type ||
      ast_sema_type_kind(base_vi.type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!rv_vector_shape(base_vi.type, &element, &lanes, &bits))
    return 0;
  uint64_t image_bytes = 0u;
  uint32_t image = rv_value_image(c, source->operands[0], &image_bytes);
  if (image == AST_MACHINE_INVALID_ID)
    return 0;
  uint64_t constant = 0u;
  int is_constant = rv_source_constant_u64(c, source->operands[1], &constant);
  AstPhysicalOperand home = rv_home(c, mi->info.result_first);
  if (!home.kind)
    return 1;
  if (is_constant) {
    if (constant >= lanes)
      return 0;
    return rv_move(
        c, home,
        physical_frame(image, bits, (int64_t)(constant * (bits / 8u))));
  }
  uint32_t index_first = c->machine->source_first[source->operands[1]];
  if (index_first == AST_MACHINE_INVALID_ID ||
      c->machine->source_parts[source->operands[1]] != 1u)
    return 0;
  AstPhysicalOperand index_home = rv_home(c, index_first);
  if (!index_home.kind)
    return 0;
  RvWork work = {0};
  AstPhysicalOperand address = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand index = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!address.kind || !index.kind ||
      !rv_address(c, address, physical_frame(image, 64u, 0)) ||
      !rv_normalize(c, index, index_home, index_home.bit_width, 0))
    return 0;
  uint32_t lane_bytes = bits / 8u;
  if (lane_bytes > 1u) {
    uint32_t shamt = lane_bytes == 2u ? 1u : lane_bytes == 4u ? 2u : 3u;
    AstPhysicalOperand sh[] = {index, index, physical_immediate(shamt, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, sh, 3u))
      return 0;
  }
  AstPhysicalOperand add[] = {address, address, index};
  if (!rv_emit(c, AST_MACHINE_RISCV_ADD, 1u, add, 3u))
    return 0;
  uint32_t scalar_image = rv_temp(c, 8u, AST_FRAME_OBJECT_ABI_TEMP);
  if (scalar_image == AST_MACHINE_INVALID_ID ||
      !rv_move(c, physical_frame(scalar_image, bits, 0),
               rv_memory(address, bits, 0)) ||
      !rv_restore(c, &work))
    return 0;
  return rv_move(c, home, physical_frame(scalar_image, bits, 0));
}

static int rv_try_fixed_vector_lowering(RvContext *c,
                                        const MachineInstruction *mi) {
  if (!c || !mi || mi->info.source_instruction == AST_MIR_INVALID_ID)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source))
    return 0;
  if (source.op == AST_MIR_OP_BINARY) {
    AstMirVRegInfo result_vi, lhs_vi;
    if (!rv_source_type(c, source.result, &result_vi) ||
        !rv_source_type(c, source.operands[0], &lhs_vi))
      return 0;
    if (result_vi.type &&
        ast_sema_type_kind(result_vi.type) == AST_SEMA_TYPE_VECTOR)
      return rv_lower_fixed_vector_binary(c, mi, &source);
    if (result_vi.type &&
        ast_sema_type_kind(result_vi.type) == AST_SEMA_TYPE_MASK &&
        lhs_vi.type && ast_sema_type_kind(lhs_vi.type) == AST_SEMA_TYPE_VECTOR)
      return rv_lower_fixed_vector_compare(c, mi, &source);
  }
  if (source.op == AST_MIR_OP_SELECT ||
      (source.op == AST_MIR_OP_INTRINSIC &&
       source.intrinsic == AST_HIR_INTRINSIC_SELECT)) {
    AstMirVRegInfo result_vi;
    if (rv_source_type(c, source.result, &result_vi) && result_vi.type &&
        ast_sema_type_kind(result_vi.type) == AST_SEMA_TYPE_VECTOR)
      return rv_lower_fixed_vector_select(c, mi, &source);
  }
  if (source.op == AST_MIR_OP_INDEX) {
    AstMirVRegInfo base_vi;
    if (!rv_source_type(c, source.operands[0], &base_vi) || !base_vi.type)
      return 0;
    if (ast_sema_type_kind(base_vi.type) == AST_SEMA_TYPE_MASK)
      return rv_lower_mask_index(c, mi, &source);
    if (ast_sema_type_kind(base_vi.type) == AST_SEMA_TYPE_VECTOR)
      return rv_lower_vector_index(c, mi, &source);
  }
  return 0;
}

/* MIR integer constants are stored as sign + positive magnitude. */
static int rv_source_signed_divisor(RvContext *c, AstMirVReg vreg,
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
/* RV64I strength reduction; this optimization does not require the M
   extension. Keeping the first slice at 64-bit unsigned operands avoids
   mixing the RV64 32-bit sign-extension ABI with zero-extension semantics. */
static int rv_lower_magic_divrem(RvContext *c, const MachineInstruction *mi,
                                 RvWork *work, AstPhysicalOperand dst,
                                 AstPhysicalOperand numerator) {
  int signed_op = mi->info.opcode == AST_MACHINE_RISCV_DIV ||
                  mi->info.opcode == AST_MACHINE_RISCV_REM;
  int division = mi->info.opcode == AST_MACHINE_RISCV_DIVU ||
                 mi->info.opcode == AST_MACHINE_RISCV_DIV;
  if (!division && mi->info.opcode != AST_MACHINE_RISCV_REMU &&
      mi->info.opcode != AST_MACHINE_RISCV_REM)
    return 0;
  if (!(c->program->target.features & AST_TARGET_FEATURE_RV_M) ||
      mi->info.source_instruction == AST_MIR_INVALID_ID ||
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
      !(signed_op ? rv_source_signed_divisor(c, source.operands[1], &divisor,
                                             &divisor_negative)
                  : rv_source_constant_u64(c, source.operands[1], &divisor)) ||
      !ast_div_magic_u64(divisor, &m))
    return 0;
  AstDivCostChoice choice = ast_div_cost_select(
      AST_DIV_COST_RV64, dst.bit_width, signed_op, !division, divisor, 1,
      c->program->target.optimize_size || ast_div_cost_prefer_size());
  ast_div_cost_report(AST_DIV_COST_RV64, dst.bit_width, signed_op, !division,
                      divisor, choice);
  if (choice.mode != AST_DIV_COST_MAGIC)
    return 0;
  numerator = rv_ensure(c, work, numerator, AST_MACHINE_BANK_GPR);
  if (!numerator.kind || numerator.bit_width != 64u)
    return -1;
  AstPhysicalOperand unsigned_numerator = numerator;
  AstPhysicalOperand quotient_sign = {0};
  if (signed_op) {
    quotient_sign = rv_borrow(c, work, AST_MACHINE_BANK_GPR, 64u);
    unsigned_numerator = rv_borrow(c, work, AST_MACHINE_BANK_GPR, 64u);
    if (!quotient_sign.kind || !unsigned_numerator.kind)
      return -1;
    AstPhysicalOperand sign[] = {quotient_sign, numerator,
                                 physical_immediate(63u, 64u)};
    AstPhysicalOperand xor_args[] = {unsigned_numerator, numerator,
                                     quotient_sign};
    AstPhysicalOperand sub_args[] = {unsigned_numerator, unsigned_numerator,
                                     quotient_sign};
    if (!rv_emit(c, AST_MACHINE_RISCV_SRAI, 1u, sign, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, xor_args, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, sub_args, 3u))
      return -1;
    if (divisor_negative) {
      AstPhysicalOperand invert[] = {quotient_sign, quotient_sign,
                                     physical_immediate(UINT64_MAX, 64u)};
      if (!rv_emit(c, AST_MACHINE_RISCV_XORI, 1u, invert, 3u))
        return -1;
    }
  }
  AstPhysicalOperand multiplier = rv_borrow(c, work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand quotient = rv_borrow(c, work, AST_MACHINE_BANK_GPR, 64u);
  if (!multiplier.kind || !quotient.kind ||
      !rv_move(c, multiplier, physical_immediate(m.multiplier, 64u)))
    return -1;
  AstPhysicalOperand high[] = {quotient, unsigned_numerator, multiplier};
  if (!rv_emit(c, AST_MACHINE_RISCV_MULHU, 1u, high, 3u))
    return -1;
  if (m.add) {
    AstPhysicalOperand temp = rv_borrow(c, work, AST_MACHINE_BANK_GPR, 64u);
    if (!temp.kind)
      return -1;
    AstPhysicalOperand difference[] = {temp, unsigned_numerator, quotient};
    AstPhysicalOperand shr[] = {temp, temp, physical_immediate(1u, 64u)};
    AstPhysicalOperand sum[] = {quotient, quotient, temp};
    if (!rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, difference, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, shr, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_ADD, 1u, sum, 3u))
      return -1;
  }
  if (m.shift) {
    AstPhysicalOperand shr[] = {quotient, quotient,
                                physical_immediate(m.shift, 64u)};
    if (!rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, shr, 3u))
      return -1;
  }
  if (signed_op) {
    AstPhysicalOperand xor_args[] = {quotient, quotient, quotient_sign};
    AstPhysicalOperand sub_args[] = {quotient, quotient, quotient_sign};
    if (!rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, xor_args, 3u) ||
        !rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, sub_args, 3u))
      return -1;
  }
  if (division)
    return rv_move(c, dst, quotient) ? 1 : -1;
  uint64_t literal =
      signed_op && divisor_negative ? UINT64_C(0) - divisor : divisor;
  if (!rv_move(c, multiplier, physical_immediate(literal, 64u)))
    return -1;
  AstPhysicalOperand mul[] = {quotient, quotient, multiplier};
  AstPhysicalOperand sub[] = {dst, numerator, quotient};
  return rv_emit(c, AST_MACHINE_RISCV_MUL, 1u, mul, 3u) &&
                 rv_emit(c, AST_MACHINE_RISCV_SUB, 1u, sub, 3u)
             ? 1
             : -1;
}

static int rv_lower_pow2_divrem(RvContext *c, const MachineInstruction *mi,
                                RvWork *work, AstPhysicalOperand dst,
                                AstPhysicalOperand numerator) {
  int div = mi->info.opcode == AST_MACHINE_RISCV_DIVU;
  if (!div && mi->info.opcode != AST_MACHINE_RISCV_REMU)
    return 0;
  if (mi->info.source_instruction == AST_MIR_INVALID_ID ||
      mi->info.operand_count != 2u || mi->info.result_count != 1u ||
      dst.bit_width != 64u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  uint64_t divisor = 0u;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_BINARY || source.operand_count != 2u ||
      !rv_source_constant_u64(c, source.operands[1], &divisor) || !divisor ||
      (divisor & (divisor - 1u)))
    return 0;
  uint32_t k = 0u;
  for (uint64_t n = divisor; n > 1u; n >>= 1u)
    ++k;
  numerator = rv_ensure(c, work, numerator, AST_MACHINE_BANK_GPR);
  if (!numerator.kind || numerator.bit_width != 64u)
    return -1;
  if (!k)
    return rv_move(c, dst, div ? numerator : physical_immediate(0u, 64u)) ? 1
                                                                          : -1;
  if (div) {
    AstPhysicalOperand args[] = {dst, numerator, physical_immediate(k, 64u)};
    return rv_emit(c, AST_MACHINE_RISCV_SRLI, 1u, args, 3u) ? 1 : -1;
  }
  AstPhysicalOperand mask = rv_borrow(c, work, AST_MACHINE_BANK_GPR, 64u);
  if (!mask.kind || !rv_move(c, mask, physical_immediate(divisor - 1u, 64u)))
    return -1;
  AstPhysicalOperand args[] = {dst, numerator, mask};
  return rv_emit(c, AST_MACHINE_RISCV_AND, 1u, args, 3u) ? 1 : -1;
}

static int lower_regular(RvContext *c, const MachineInstruction *mi) {
  /* Compact fixed vectors may legalize to a single <=64-bit machine vreg.
     They still require lane-wise lowering: treating packed lanes as one
     scalar causes cross-lane carries and makes operations such as vec<3,u8>
     multiplication reach the RV64 scalar arithmetic path as an illegal
     24-bit integer.  Always give semantic fixed-vector lowering first refusal.
   */
  if (rv_try_fixed_vector_lowering(c, mi))
    return 1;
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
    uint64_t offset = 0u;
    for (uint32_t part = 0; part < parts; ++part) {
      if (!load && mi->info.operands[part].kind != AST_MACHINE_OPERAND_VREG)
        return 0;
      uint32_t v =
          load ? mi->info.result_first + part : mi->info.operands[part].as.vreg;
      AstPhysicalOperand home = rv_home(c, v);
      uint32_t bits = c->machine->vregs[v].bit_width;
      AstPhysicalOperand memory =
          physical_frame(c->locals[slot->as.stack_slot], bits, (int64_t)offset);
      if (home.kind && !rv_move(c, load ? home : memory, load ? memory : home))
        return 0;
      offset += (bits + 7u) / 8u;
    }
    return 1;
  }
  RvWork work = {0};
  AstPhysicalOperand uses[AST_PHYSICAL_MAX_OPERANDS] = {{0}};
  AstPhysicalOperand result = {0}, home = {0};
  uint32_t count = 0u;
  if (mi->info.result_count > 1u || mi->info.operand_count > 6u)
    return fail_instruction(c,
                            "Multi-part arithmetic needs legalized carry/lane "
                            "operations before physical lowering.");
  if (mi->info.result_count) {
    home = rv_home(c, mi->info.result_first);
    if (!home.kind)
      return 1; /* Unused, pure definitions only enter here. */
    if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[home.register_unit] = 1u;
  }
  for (uint32_t i = 0; i < mi->info.operand_count; ++i) {
    const AstMachineOperand *op = &mi->info.operands[i];
    if (op->kind == AST_MACHINE_OPERAND_VREG) {
      uses[count] = rv_home(c, op->as.vreg);
      if (uses[count].kind == AST_PHYSICAL_OPERAND_REGISTER)
        work.excluded[uses[count].register_unit] = 1u;
    } else if (op->kind == AST_MACHINE_OPERAND_STACK_SLOT) {
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
    result =
        home.kind == AST_PHYSICAL_OPERAND_REGISTER
            ? home
            : rv_borrow(c, &work, c->machine->vregs[mi->info.result_first].bank,
                        home.bit_width);
    if (!result.kind)
      return 0;
  }
  int ok = 0;
  if (mi->info.opcode == AST_MACHINE_RISCV_LOAD ||
      mi->info.opcode == AST_MACHINE_RISCV_LA) {
    if (count != 1u)
      return fail_instruction(
          c, "Load/address requires exactly one selected address.");
    AstPhysicalOperand address = uses[0];
    if (mi->info.operands[0].kind == AST_MACHINE_OPERAND_VREG) {
      address = rv_ensure(c, &work, address, AST_MACHINE_BANK_GPR);
      if (!address.kind)
        return 0;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    if (mi->info.opcode == AST_MACHINE_RISCV_LOAD &&
        address.kind == AST_PHYSICAL_OPERAND_SYMBOL) {
      AstPhysicalOperand pointer =
          rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
      if (!pointer.kind || !rv_address(c, pointer, address))
        return 0;
      address = pointer;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    address.bit_width = result.bit_width;
    ok = mi->info.opcode == AST_MACHINE_RISCV_LA
             ? rv_address(c, result, address)
             : rv_move(c, result, address);
  } else if (mi->info.opcode == AST_MACHINE_RISCV_STORE) {
    if (count != 2u)
      return fail_instruction(c, "Store requires a value and an address.");
    AstPhysicalOperand value, address;
    uint32_t value_index;
    if (mi->info.source_op == AST_MIR_OP_STORE) {
      address = rv_ensure(c, &work, uses[0], AST_MACHINE_BANK_GPR);
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
      value = rv_ensure(c, &work, value, c->machine->vregs[v].bank);
    }
    if (address.kind == AST_PHYSICAL_OPERAND_SYMBOL) {
      AstPhysicalOperand pointer =
          rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
      if (!pointer.kind || !rv_address(c, pointer, address))
        return 0;
      address = pointer;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    address.bit_width = value.bit_width;
    ok = rv_move(c, address, value);
  } else {
    if (count == 2u && mi->info.operands[0].kind == AST_MACHINE_OPERAND_VREG &&
        mi->info.operands[1].kind == AST_MACHINE_OPERAND_VREG &&
        (mi->info.opcode == AST_MACHINE_RISCV_DIVU ||
         mi->info.opcode == AST_MACHINE_RISCV_REMU ||
         mi->info.opcode == AST_MACHINE_RISCV_DIV ||
         mi->info.opcode == AST_MACHINE_RISCV_REM)) {
      int reduced = rv_lower_pow2_divrem(c, mi, &work, result, uses[0]);
      if (reduced == 0)
        reduced = rv_lower_magic_divrem(c, mi, &work, result, uses[0]);
      if (reduced < 0)
        return 0;
      if (reduced > 0)
        return (!home.kind || rv_move(c, home, result)) && rv_restore(c, &work);
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (mi->info.operands[i].kind != AST_MACHINE_OPERAND_VREG)
        return 0;
      uint32_t v = mi->info.operands[i].as.vreg;
      uses[i] = rv_ensure(c, &work, uses[i], c->machine->vregs[v].bank);
      if (!uses[i].kind)
        return 0;
      if (mi->info.opcode != AST_MACHINE_RISCV_CAST &&
          uses[i].bank == AST_MACHINE_BANK_GPR && uses[i].bit_width < 64u) {
        AstPhysicalOperand normalized =
            rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
        if (!normalized.kind ||
            !rv_normalize(c, normalized, uses[i], uses[i].bit_width,
                          ast_sema_type_is_signed(c->machine->vregs[v].type)))
          return 0;
        uses[i] = normalized;
      }
    }
    AstPhysicalOperand args[AST_PHYSICAL_MAX_OPERANDS] = {{0}};
    args[0] = result;
    for (uint32_t i = 0; i < count; ++i)
      args[i + 1u] = uses[i];
    AstMachineOpcode op = mi->info.opcode;
    if (op == AST_MACHINE_RISCV_CMP || op == AST_MACHINE_RISCV_FCMP) {
      if (count != 2u)
        return 0;
      AstTokenKind tok = mi->info.token_op;
      int fp = op == AST_MACHINE_RISCV_FCMP, wide = uses[0].bit_width == 64u;
      int invert = tok == AST_TOK_BANG_EQ || tok == AST_TOK_GT_EQ ||
                   tok == AST_TOK_LT_EQ;
      if (tok == AST_TOK_GT || tok == AST_TOK_LT_EQ) {
        args[1] = uses[1];
        args[2] = uses[0];
      }
      if (fp) {
        invert = tok == AST_TOK_BANG_EQ;
        if (tok == AST_TOK_GT || tok == AST_TOK_GT_EQ) {
          args[1] = uses[1];
          args[2] = uses[0];
        } else {
          args[1] = uses[0];
          args[2] = uses[1];
        }
        op = tok == AST_TOK_EQ_EQ || tok == AST_TOK_BANG_EQ
                 ? (wide ? AST_MACHINE_RISCV_FEQ_D : AST_MACHINE_RISCV_FEQ_S)
             : tok == AST_TOK_LT_EQ || tok == AST_TOK_GT_EQ
                 ? (wide ? AST_MACHINE_RISCV_FLE_D : AST_MACHINE_RISCV_FLE_S)
             : wide ? AST_MACHINE_RISCV_FLT_D
                    : AST_MACHINE_RISCV_FLT_S;
        ok = rv_emit(c, op, 1u, args, 3u);
      } else if (tok == AST_TOK_EQ_EQ || tok == AST_TOK_BANG_EQ) {
        ok = rv_emit(c, AST_MACHINE_RISCV_XOR, 1u, args, 3u);
        args[1] = result;
        args[2] = physical_immediate(1u, 64u);
        if (ok)
          ok = rv_emit(c, AST_MACHINE_RISCV_SLTIU, 1u, args, 3u);
      } else {
        const AstSemaType *type =
            c->machine->vregs[mi->info.operands[0].as.vreg].type;
        ok = rv_emit(c,
                     ast_sema_type_is_signed(type) ? AST_MACHINE_RISCV_SLT
                                                   : AST_MACHINE_RISCV_SLTU,
                     1u, args, 3u);
      }
      if (ok && invert) {
        args[1] = result;
        args[2] = physical_immediate(1u, 64u);
        ok = rv_emit(c, AST_MACHINE_RISCV_XORI, 1u, args, 3u);
      }
    } else if (op == AST_MACHINE_RISCV_CAST) {
      ok = count == 1u && lower_cast(c, &work, mi, result, uses[0]);
    } else if (op == AST_MACHINE_RISCV_SELECT) {
      if (count != 3u)
        return 0;
      int handled = 0;
      ok = physical_rv_select(c, result, uses[0], uses[1], uses[2], &handled);
      if (!ok)
        return 0;
      if (handled) {
        if (home.kind && !rv_move(c, home, result))
          return 0;
        return rv_restore(c, &work);
      }
      uint32_t predecessor = c->block;
      uint32_t yes = add_block(c), no = add_block(c), join = add_block(c);
      if (yes == AST_MACHINE_INVALID_ID || no == AST_MACHINE_INVALID_ID ||
          join == AST_MACHINE_INVALID_ID)
        return 0;
      AstPhysicalOperand branch[] = {
          uses[0], rv_reg(c, AST_MACHINE_BANK_GPR, 0u, 64u), rv_block(yes)};
      AstPhysicalOperand no_target = rv_block(no), join_target = rv_block(join);
      c->block = predecessor;
      ok = rv_emit(c, AST_MACHINE_RISCV_BNE, 0u, branch, 3u) &&
           rv_emit(c, AST_MACHINE_RISCV_J, 0u, &no_target, 1u);
      c->block = yes;
      if (ok)
        ok = rv_move(c, result, uses[1]) &&
             rv_emit(c, AST_MACHINE_RISCV_J, 0u, &join_target, 1u);
      c->block = no;
      if (ok)
        ok = rv_move(c, result, uses[2]) &&
             rv_emit(c, AST_MACHINE_RISCV_J, 0u, &join_target, 1u);
      c->block = join;
    } else if (op == AST_MACHINE_RISCV_NOT &&
               mi->info.token_op == AST_TOK_BANG) {
      args[2] = physical_immediate(1u, 64u);
      ok = rv_emit(c, AST_MACHINE_RISCV_SLTIU, 1u, args, 3u);
    } else if (!(c->program->target.features & AST_TARGET_FEATURE_RV_M) &&
               count == 2u &&
               (op == AST_MACHINE_RISCV_MUL || op == AST_MACHINE_RISCV_MULW ||
                op == AST_MACHINE_RISCV_DIV || op == AST_MACHINE_RISCV_DIVW ||
                op == AST_MACHINE_RISCV_DIVU || op == AST_MACHINE_RISCV_DIVUW ||
                op == AST_MACHINE_RISCV_REM || op == AST_MACHINE_RISCV_REMW ||
                op == AST_MACHINE_RISCV_REMU ||
                op == AST_MACHINE_RISCV_REMUW)) {
      const AstSemaType *type =
          c->machine->vregs[mi->info.operands[0].as.vreg].type;
      uint32_t width = type ? ast_sema_type_bit_width(type) : 0u;
      int sign = type && ast_sema_type_is_signed(type);
      if (!width || width > 64u)
        return fail_instruction(c, "RV64I software arithmetic requires an "
                                   "integer width in 1..64 bits.");
      if (op == AST_MACHINE_RISCV_MUL || op == AST_MACHINE_RISCV_MULW)
        ok = rv_soft_mul(c, &work, result, uses[0], uses[1], width, sign);
      else
        ok = rv_soft_divrem(
            c, &work, result, uses[0], uses[1], width, sign,
            op == AST_MACHINE_RISCV_REM || op == AST_MACHINE_RISCV_REMW ||
                op == AST_MACHINE_RISCV_REMU || op == AST_MACHINE_RISCV_REMUW);
    } else
      ok = rv_emit(c, op, 1u, args, count + 1u);
  }
  if (!ok || (home.kind && !rv_move(c, home, result)))
    return 0;
  return rv_restore(c, &work);
}

static int rv_source_constant_u64(RvContext *c, AstMirVReg vreg,
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
static int rv_aggregate_element_offset(const AstSemaType *type, uint64_t index,
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
static int rv_lower_pack(RvContext *c, const MachineInstruction *mi) {
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
  uint32_t object = rv_temp(c, storage, AST_FRAME_OBJECT_ABI_TEMP);
  if (object == AST_MACHINE_INVALID_ID)
    return 0;
  for (uint64_t off = 0u; off < storage; off += 8u)
    if (!rv_move(c, physical_frame(object, 64u, (int64_t)off),
                 physical_immediate(0u, 64u)))
      return 0;
  AstPhysicalOperand first = rv_home(c, mi->info.operands[0].as.vreg);
  AstPhysicalOperand flag = rv_home(c, mi->info.operands[1].as.vreg);
  if (!first.kind || !flag.kind)
    return 0;
  first.bit_width = (uint32_t)first_size * 8u;
  flag.bit_width = 8u;
  if (!rv_move(c, physical_frame(object, first.bit_width, 0), first) ||
      !rv_move(c, physical_frame(object, 8u, (int64_t)first_size), flag))
    return 0;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    AstPhysicalOperand dst = rv_home(c, mi->info.result_first + part);
    if (dst.kind &&
        !rv_move(c, dst,
                 physical_frame(object, dst.bit_width, (int64_t)part * 8)))
      return 0;
  }
  return 1;
}
static int rv_lower_aggregate_extract(RvContext *c,
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
    if (!rv_source_constant_u64(c, source.operands[1], &index) ||
        !rv_aggregate_element_offset(aggregate_vi.type, index, &offset,
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
  uint32_t object = rv_temp(c, storage, AST_FRAME_OBJECT_ABI_TEMP);
  if (object == AST_MACHINE_INVALID_ID)
    return 0;
  for (uint64_t off = 0u; off < storage; off += 8u)
    if (!rv_move(c, physical_frame(object, 64u, (int64_t)off),
                 physical_immediate(0u, 64u)))
      return 0;
  for (uint32_t part = 0u; part < parts; ++part) {
    if (mi->info.operands[part].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    AstPhysicalOperand src = rv_home(c, mi->info.operands[part].as.vreg);
    if (!src.kind ||
        !rv_move(c, physical_frame(object, src.bit_width, (int64_t)part * 8),
                 src))
      return 0;
  }
  AstPhysicalOperand dst = rv_home(c, mi->info.result_first);
  if (!dst.kind)
    return 1;
  return rv_move(c, dst,
                 physical_frame(object, (uint32_t)bytes * 8u, (int64_t)offset));
}

static int rv_cmp_branch_condition(const AstPhysicalInstructionInfo *first,
                                   const AstPhysicalInstructionInfo *second,
                                   const AstPhysicalInstructionInfo *third,
                                   uint32_t count, uint32_t *condition,
                                   AstPhysicalOperand *lhs,
                                   AstPhysicalOperand *rhs) {
  if (!first || !condition || !lhs || !rhs)
    return 0;
  if ((first->opcode == AST_MACHINE_RISCV_SLT ||
       first->opcode == AST_MACHINE_RISCV_SLTU) &&
      first->result_count == 1u && first->operand_count == 3u) {
    uint32_t cond = first->opcode == AST_MACHINE_RISCV_SLT ? 4u : 6u;
    if (count == 2u) {
      if (!second || second->opcode != AST_MACHINE_RISCV_XORI ||
          second->result_count != 1u || second->operand_count != 3u ||
          !physical_same_location(&first->operands[0], &second->operands[0]) ||
          !physical_same_location(&first->operands[0], &second->operands[1]) ||
          second->operands[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
          second->operands[2].immediate_bits != 1u ||
          first->source_instruction != second->source_instruction)
        return 0;
      cond ^= 1u;
    } else if (count != 1u)
      return 0;
    *condition = cond;
    *lhs = first->operands[1];
    *rhs = first->operands[2];
    return 1;
  }
  if (first->opcode == AST_MACHINE_RISCV_XOR && first->result_count == 1u &&
      first->operand_count == 3u && second &&
      second->opcode == AST_MACHINE_RISCV_SLTIU && second->result_count == 1u &&
      second->operand_count == 3u &&
      physical_same_location(&first->operands[0], &second->operands[0]) &&
      physical_same_location(&first->operands[0], &second->operands[1]) &&
      second->operands[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
      second->operands[2].immediate_bits == 1u &&
      first->source_instruction == second->source_instruction) {
    uint32_t cond = 0u;
    if (count == 3u) {
      if (!third || third->opcode != AST_MACHINE_RISCV_XORI ||
          third->result_count != 1u || third->operand_count != 3u ||
          !physical_same_location(&first->operands[0], &third->operands[0]) ||
          !physical_same_location(&first->operands[0], &third->operands[1]) ||
          third->operands[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
          third->operands[2].immediate_bits != 1u ||
          first->source_instruction != third->source_instruction)
        return 0;
      cond = 1u;
    } else if (count != 2u)
      return 0;
    *condition = cond;
    *lhs = first->operands[1];
    *rhs = first->operands[2];
    return 1;
  }
  return 0;
}

static int rv_late_cfg_cleanup(RvContext *c) {
  if (!c || !c->fn || !c->machine)
    return 0;
  for (uint32_t b = 0; b < c->fn->block_count; ++b) {
    PhysicalBlock *pb = &c->fn->blocks[b];
    if (pb->count < 3u)
      continue;
    AstPhysicalInstructionInfo *branch = &pb->instructions[pb->count - 2u];
    AstPhysicalInstructionInfo *jump = &pb->instructions[pb->count - 1u];
    if (branch->opcode != AST_MACHINE_RISCV_BNE || branch->result_count ||
        branch->operand_count != 3u || jump->opcode != AST_MACHINE_RISCV_J ||
        jump->result_count || jump->operand_count != 1u ||
        branch->operands[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        branch->operands[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        branch->operands[1].register_unit != MACHINE_PHYSREG_FILE_GPR * 64u ||
        branch->operands[2].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        jump->operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK)
      continue;
    for (uint32_t seq = 3u; seq >= 1u; --seq) {
      if (pb->count < seq + 2u)
        continue;
      uint32_t start = pb->count - seq - 2u;
      AstPhysicalInstructionInfo *first = &pb->instructions[start];
      AstPhysicalInstructionInfo *second =
          seq >= 2u ? &pb->instructions[start + 1u] : NULL;
      AstPhysicalInstructionInfo *third =
          seq >= 3u ? &pb->instructions[start + 2u] : NULL;
      uint32_t condition = 0u;
      AstPhysicalOperand lhs = {0}, rhs = {0};
      if (!rv_cmp_branch_condition(first, second, third, seq, &condition, &lhs,
                                   &rhs) ||
          !physical_same_location(&first->operands[0], &branch->operands[0]) ||
          first->source_instruction >= c->machine->info.instruction_count)
        continue;
      if (branch->source_instruction >= c->machine->info.instruction_count ||
          first->source_instruction >= c->machine->info.instruction_count ||
          b >= c->machine->info.block_count)
        continue;
      const MachineInstruction *branch_source =
          &c->machine->instructions[branch->source_instruction];
      const MachineInstruction *compare_source =
          &c->machine->instructions[first->source_instruction];
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
      fused.opcode = AST_MACHINE_RISCV_CMP_BR;
      fused.result_count = 0u;
      fused.operand_count = 5u;
      fused.operands[0] = physical_immediate(condition, 8u);
      fused.operands[1] = lhs;
      fused.operands[2] = rhs;
      fused.operands[3] = branch->operands[2];
      fused.operands[4] = jump->operands[0];
      pb->instructions[start] = fused;
      pb->count = start + 1u;
      break;
    }
    /* Even without a removable compare, represent the two destinations in
       one terminator. Layout can then invert BNE and make either arm the
       fallthrough instead of retaining a conditional plus an avoidable J. */
    if (pb->count >= 2u) {
      AstPhysicalInstructionInfo test = pb->instructions[pb->count - 2u];
      AstPhysicalInstructionInfo tail = pb->instructions[pb->count - 1u];
      if (test.opcode == AST_MACHINE_RISCV_BNE && test.operand_count == 3u &&
          tail.opcode == AST_MACHINE_RISCV_J && tail.operand_count == 1u &&
          test.source_instruction < c->machine->info.instruction_count &&
          c->machine->instructions[test.source_instruction].info.source_op !=
              AST_MIR_OP_NATIVE &&
          test.operands[2].kind == AST_PHYSICAL_OPERAND_BLOCK &&
          tail.operands[0].kind == AST_PHYSICAL_OPERAND_BLOCK) {
        AstPhysicalInstructionInfo fused = test;
        fused.opcode = AST_MACHINE_RISCV_CMP_BR;
        fused.operand_count = 5u;
        fused.operands[0] = physical_immediate(1u, 8u);
        fused.operands[1] = test.operands[0];
        fused.operands[2] = test.operands[1];
        fused.operands[3] = test.operands[2];
        fused.operands[4] = tail.operands[0];
        pb->instructions[pb->count - 2u] = fused;
        --pb->count;
      }
    }
  }
  return 1;
}

int physical_lower_riscv64(AstRegAllocProgram *p, uint32_t f) {
  AllocationFunction *af = &p->functions[f];
  const MachineFunction *machine = &p->machine->functions[f];
  RvContext c = {.program = p,
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
    uint32_t lr = MACHINE_PHYSREG_FILE_GPR * 64u + 1u;
    af->callee_save_objects[lr] =
        frame_add_object(af, AST_FRAME_OBJECT_CALLEE_SAVE, 8u, 8u);
    if (af->callee_save_objects[lr] == AST_MACHINE_INVALID_ID)
      goto fail;
  }
  for (uint32_t b = 0; b < machine->info.block_count; ++b)
    if (add_block(&c) == AST_MACHINE_INVALID_ID)
      goto fail;
  if (!rv_entry(&c))
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
        case AST_MACHINE_RISCV_LI:
        case AST_MACHINE_RISCV_UNDEF:
          ok = lower_constant(&c, mi);
          break;
        case AST_MACHINE_RISCV_MV:
          ok = lower_copy(&c, mi);
          break;
        case AST_MACHINE_RISCV_J:
        case AST_MACHINE_RISCV_COND_BR:
          ok = lower_branch(&c, mi, b);
          break;
        case AST_MACHINE_RISCV_RET:
          ok = rv_return(&c, mi);
          break;
        case AST_MACHINE_RISCV_CALL:
        case AST_MACHINE_RISCV_CALL_INDIRECT:
          ok = c.call_sites[c.instruction] != AST_MACHINE_INVALID_ID &&
               rv_call(&c, mi, c.call_sites[c.instruction]);
          break;
        case AST_MACHINE_RISCV_EBREAK:
        case AST_MACHINE_RISCV_FENCE:
          ok = rv_emit(&c, mi->info.opcode, 0u, NULL, 0u);
          break;
        case AST_MACHINE_RISCV_BIND:
          ok = regalloc_verify_bind_instruction(p, f, mi);
          break;
        case AST_MACHINE_RISCV_NATIVE:
        case AST_MACHINE_RISCV_NATIVE_RESULT:
          ok = physical_native_instruction(&c, mi);
          break;
        case AST_MACHINE_RISCV_INTRINSIC:
          ok = mi->info.source_op == AST_MIR_OP_PACK
                   ? rv_lower_pack(&c, mi)
                   : fail_instruction(&c,
                                      "Selected intrinsic still requires "
                                      "target-specific constraint expansion.");
          break;
        case AST_MACHINE_RISCV_INDEX:
          ok = rv_try_fixed_vector_lowering(&c, mi);
          if (!ok)
            ok = rv_lower_aggregate_extract(&c, mi);
          break;
        case AST_MACHINE_RISCV_MEMBER:
          ok = rv_lower_aggregate_extract(&c, mi);
          break;
        case AST_MACHINE_RISCV_ATOMIC:
          ok = physical_atomic_instruction(&c, mi);
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
  if (!physical_run_peephole(&c) || !rv_late_cfg_cleanup(&c) ||
      !rv_frame_finish(&c))
    goto fail;
  free(c.locals);
  free(c.call_sites);
  return 1;
fail:
  free(c.locals);
  free(c.call_sites);
  return 0;
}
