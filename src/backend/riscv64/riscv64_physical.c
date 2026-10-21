#include "riscv64_internal.h"
#include <limits.h>

AstPhysicalOperand rv_reg(RvContext *c, AstMachineRegBank bank, uint32_t index,
                          uint32_t bits) {
  return physical_register(c, bank, index, bits);
}
AstPhysicalOperand rv_named(RvContext *c, const char *name, uint32_t bits) {
  MachinePhysRegView view;
  AstPhysicalOperand invalid = {0};
  if (!machine_physreg_view(&c->program->target, name, &view))
    return invalid;
  return rv_reg(c,
                view.file == MACHINE_PHYSREG_FILE_GPR ? AST_MACHINE_BANK_GPR
                                                      : AST_MACHINE_BANK_FP,
                view.index, bits);
}
AstPhysicalOperand rv_home(RvContext *c, uint32_t value) {
  /* Split intervals may assign a different home at each instruction. Edge
   * copies and their consumers must use the same slot-aware lookup. */
  return physical_home(c, value);
}
AstPhysicalOperand rv_memory(AstPhysicalOperand base, uint32_t bits,
                             int64_t offset) {
  base.kind = AST_PHYSICAL_OPERAND_MEMORY;
  base.bit_width = bits;
  base.displacement = offset;
  return base;
}
AstPhysicalOperand rv_stack(RvContext *c, uint32_t bits, int64_t offset) {
  return rv_memory(rv_reg(c, AST_MACHINE_BANK_GPR, 2u, 64u), bits, offset);
}
AstPhysicalOperand rv_block(uint32_t block) {
  AstPhysicalOperand o = {0};
  o.kind = AST_PHYSICAL_OPERAND_BLOCK;
  o.block = block;
  return o;
}
int rv_emit(RvContext *c, AstMachineOpcode op, uint32_t defs,
            const AstPhysicalOperand *args, uint32_t count) {
  if (!physical_emit(c, op, defs, args, count))
    return 0;
  if (c->instruction < c->machine->info.instruction_count) {
    AstPhysicalInstructionInfo *i =
        &c->fn->blocks[c->block]
             .instructions[c->fn->blocks[c->block].count - 1u];
    i->memory_order =
        c->machine->instructions[c->instruction].info.memory_order;
    i->failure_memory_order =
        c->machine->instructions[c->instruction].info.failure_memory_order;
  }
  return 1;
}
uint32_t rv_temp(RvContext *c, uint64_t bytes, AstFrameObjectKind kind) {
  return physical_temp_object(c, bytes, kind);
}
static AstMachineOpcode rv_load_op(uint32_t bits, int fp) {
  if (fp)
    return bits == 32u   ? AST_MACHINE_RISCV_FLW
           : bits == 64u ? AST_MACHINE_RISCV_FLD
                         : AST_MACHINE_OP_INVALID;
  if (bits <= 8u)
    return AST_MACHINE_RISCV_LBU;
  if (bits == 16u)
    return AST_MACHINE_RISCV_LHU;
  if (bits == 32u)
    return AST_MACHINE_RISCV_LWU;
  return bits == 64u ? AST_MACHINE_RISCV_LD : AST_MACHINE_OP_INVALID;
}
static AstMachineOpcode rv_store_op(uint32_t bits, int fp) {
  if (fp)
    return bits == 32u   ? AST_MACHINE_RISCV_FSW
           : bits == 64u ? AST_MACHINE_RISCV_FSD
                         : AST_MACHINE_OP_INVALID;
  if (bits <= 8u)
    return AST_MACHINE_RISCV_SB;
  if (bits == 16u)
    return AST_MACHINE_RISCV_SH;
  if (bits == 32u)
    return AST_MACHINE_RISCV_SW;
  return bits == 64u ? AST_MACHINE_RISCV_SD : AST_MACHINE_OP_INVALID;
}
static int rv_unit_live_now(const RvContext *c, uint32_t unit) {
  if (!c || !c->fn || !c->machine || c->instruction == AST_MACHINE_INVALID_ID ||
      c->instruction >= c->machine->info.instruction_count)
    return 1;
  return regalloc_unit_live_at(
      c->fn, unit, c->fn->liveness.instruction_slots[c->instruction]);
}
static int rv_memory_uses_unit(const AstPhysicalOperand *op, uint32_t unit) {
  return op && op->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         op->register_name[0] != '@' && op->register_unit == unit;
}
static AstPhysicalOperand rv_move_scratch(RvContext *c,
                                          const AstPhysicalOperand *src,
                                          const AstPhysicalOperand *dst,
                                          int *needs_restore) {
  static const uint32_t order[] = {31u, 30u, 29u, 28u, 7u,  6u,  5u, 17u,
                                   16u, 15u, 14u, 13u, 12u, 11u, 10u};
  AstPhysicalOperand fallback = {0};
  if (needs_restore)
    *needs_restore = 0;
  for (uint32_t n = 0; n < ASTER_ARRAY_COUNT(order); ++n) {
    AstPhysicalOperand r = rv_reg(c, AST_MACHINE_BANK_GPR, order[n], 64u);
    if (!r.kind || rv_memory_uses_unit(src, r.register_unit) ||
        rv_memory_uses_unit(dst, r.register_unit))
      continue;
    if (!fallback.kind)
      fallback = r;
    if (!rv_unit_live_now(c, r.register_unit))
      return r;
  }
  if (fallback.kind && needs_restore)
    *needs_restore = 1;
  return fallback;
}
static int rv_raw_move(RvContext *c, AstPhysicalOperand dst,
                       AstPhysicalOperand src) {
  AstPhysicalOperand args[] = {dst, src};
  AstMachineOpcode op = AST_MACHINE_OP_INVALID;
  if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY &&
      src.kind == AST_PHYSICAL_OPERAND_REGISTER) {
    args[0] = src;
    args[1] = dst;
    op = rv_store_op(dst.bit_width, src.bank == AST_MACHINE_BANK_FP);
    return op && rv_emit(c, op, 0u, args, 2u);
  }
  if (dst.kind != AST_PHYSICAL_OPERAND_REGISTER)
    return 0;
  if (src.kind == AST_PHYSICAL_OPERAND_MEMORY)
    op = rv_load_op(src.bit_width, dst.bank == AST_MACHINE_BANK_FP);
  else if (src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           dst.bank == AST_MACHINE_BANK_GPR)
    op = AST_MACHINE_RISCV_LI;
  else if (src.kind == AST_PHYSICAL_OPERAND_REGISTER) {
    if (dst.bank == AST_MACHINE_BANK_GPR && src.bank == AST_MACHINE_BANK_GPR)
      op = AST_MACHINE_RISCV_MV;
    else if (dst.bank == AST_MACHINE_BANK_FP && src.bank == AST_MACHINE_BANK_FP)
      op = dst.bit_width == 32u ? AST_MACHINE_RISCV_FMV_S
                                : AST_MACHINE_RISCV_FMV_D;
    else if (dst.bank == AST_MACHINE_BANK_FP)
      op = dst.bit_width == 32u ? AST_MACHINE_RISCV_FMV_W_X
                                : AST_MACHINE_RISCV_FMV_D_X;
    else
      op = src.bit_width == 32u ? AST_MACHINE_RISCV_FMV_X_W
                                : AST_MACHINE_RISCV_FMV_X_D;
  }
  return op && rv_emit(c, op, 1u, args, 2u);
}
int rv_move(RvContext *c, AstPhysicalOperand dst, AstPhysicalOperand src) {
  if (physical_same_location(&dst, &src))
    return 1;
  if (!dst.kind || !src.kind)
    return 0;
  if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY &&
      src.kind != AST_PHYSICAL_OPERAND_REGISTER) {
    int restore = 0;
    AstPhysicalOperand r = rv_move_scratch(c, &src, &dst, &restore);
    if (!r.kind)
      return 0;
    uint32_t u = r.register_unit, save = AST_MACHINE_INVALID_ID;
    if (restore) {
      if (c->fn->emergency_objects[u] == AST_MACHINE_INVALID_ID)
        c->fn->emergency_objects[u] =
            rv_temp(c, 8u, AST_FRAME_OBJECT_EMERGENCY);
      save = c->fn->emergency_objects[u];
      if (save == AST_MACHINE_INVALID_ID ||
          !rv_raw_move(c, physical_frame(save, 64u, 0), r))
        return 0;
    }
    uint32_t bytes = (dst.bit_width + 7u) / 8u;
    for (uint32_t off = 0; off < bytes;) {
      uint32_t n = bytes - off;
      n = n >= 8u ? 8u : n >= 4u ? 4u : n >= 2u ? 2u : 1u;
      AstPhysicalOperand sv = src, dv = dst;
      sv.bit_width = dv.bit_width = n * 8u;
      sv.displacement += off;
      dv.displacement += off;
      if (sv.kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
        if (off >= 8u)
          return 0;
        sv.immediate_bits >>= off * 8u;
      }
      AstPhysicalOperand part =
          rv_reg(c, AST_MACHINE_BANK_GPR, r.register_unit % 64u, n * 8u);
      if (!rv_raw_move(c, part, sv) || !rv_raw_move(c, dv, part))
        return 0;
      off += n;
    }
    return !restore || rv_raw_move(c, r, physical_frame(save, 64u, 0));
  }
  if (dst.kind == AST_PHYSICAL_OPERAND_REGISTER &&
      dst.bank == AST_MACHINE_BANK_FP &&
      src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    RvWork w = {0};
    w.excluded[dst.register_unit] = 1u;
    AstPhysicalOperand r =
        rv_borrow(c, &w, AST_MACHINE_BANK_GPR, src.bit_width);
    return r.kind && rv_raw_move(c, r, src) && rv_raw_move(c, dst, r) &&
           rv_restore(c, &w);
  }
  return rv_raw_move(c, dst, src);
}
AstPhysicalOperand rv_borrow(RvContext *c, RvWork *w, AstMachineRegBank bank,
                             uint32_t bits) {
  static const uint32_t gprs[] = {31u, 30u, 29u, 28u, 7u,  6u,  5u, 17u,
                                  16u, 15u, 14u, 13u, 12u, 11u, 10u};
  AstPhysicalOperand invalid = {0};
  for (uint32_t n = 0; n < (bank == AST_MACHINE_BANK_GPR ? 15u : 32u); ++n) {
    uint32_t index = bank == AST_MACHINE_BANK_GPR ? gprs[n] : 31u - n;
    AstPhysicalOperand r = rv_reg(c, bank, index, bits);
    if (!r.kind || w->excluded[r.register_unit])
      continue;
    uint32_t u = r.register_unit;
    w->excluded[u] = 1u;
    if (!rv_unit_live_now(c, u))
      return r;
    if (w->count >= AST_PHYSICAL_MAX_OPERANDS)
      return invalid;
    uint32_t save_bits =
        bank == AST_MACHINE_BANK_GPR                              ? 64u
        : (c->program->target.features & AST_TARGET_FEATURE_RV_D) ? 64u
                                                                  : 32u;
    AstPhysicalOperand saved = rv_reg(c, bank, index, save_bits);
    if (c->fn->work_objects[u] == AST_MACHINE_INVALID_ID)
      c->fn->work_objects[u] = rv_temp(c, ((uint64_t)save_bits + 7u) / 8u,
                                       AST_FRAME_OBJECT_EMERGENCY);
    if (c->fn->work_objects[u] == AST_MACHINE_INVALID_ID ||
        !rv_move(c, physical_frame(c->fn->work_objects[u], save_bits, 0),
                 saved))
      return invalid;
    w->saved[w->count++] = saved;
    return r;
  }
  return invalid;
}
AstPhysicalOperand rv_ensure(RvContext *c, RvWork *w, AstPhysicalOperand value,
                             AstMachineRegBank bank) {
  if (value.kind == AST_PHYSICAL_OPERAND_REGISTER && value.bank == bank)
    return value;
  AstPhysicalOperand r = rv_borrow(c, w, bank, value.bit_width);
  if (!r.kind || !rv_move(c, r, value)) {
    AstPhysicalOperand invalid = {0};
    return invalid;
  }
  return r;
}
int rv_restore(RvContext *c, const RvWork *w) {
  for (uint32_t rev = w->count; rev; --rev) {
    AstPhysicalOperand r = w->saved[rev - 1u];
    if (!rv_move(c, r,
                 physical_frame(c->fn->work_objects[r.register_unit],
                                r.bit_width, 0)))
      return 0;
  }
  return 1;
}
int rv_address(RvContext *c, AstPhysicalOperand dst,
               AstPhysicalOperand address) {
  AstPhysicalOperand args[] = {dst, address};
  return dst.kind == AST_PHYSICAL_OPERAND_REGISTER &&
         rv_emit(c, AST_MACHINE_RISCV_LA, 1u, args, 2u);
}
int rv_normalize(RvContext *c, AstPhysicalOperand dst, AstPhysicalOperand src,
                 uint32_t bits, int sign) {
  if (!bits || bits > 64u)
    return 0;
  if (bits == 64u)
    return rv_move(c, dst, src);
  AstPhysicalOperand args[] = {dst, src, physical_immediate(64u - bits, 64u)};
  if (!rv_emit(c, AST_MACHINE_RISCV_SLLI, 1u, args, 3u))
    return 0;
  args[1] = dst;
  return rv_emit(c, sign ? AST_MACHINE_RISCV_SRAI : AST_MACHINE_RISCV_SRLI, 1u,
                 args, 3u);
}
int rv_epilogue(RvContext *c) {
  for (uint32_t rev = MACHINE_REGISTER_UNITS; rev; --rev) {
    uint32_t u = rev - 1u, obj = c->fn->callee_save_objects[u];
    if (obj == AST_MACHINE_INVALID_ID)
      continue;
    uint32_t bits = (uint32_t)c->fn->objects[obj].size * 8u;
    AstMachineRegBank bank = u / 64u == MACHINE_PHYSREG_FILE_GPR
                                 ? AST_MACHINE_BANK_GPR
                                 : AST_MACHINE_BANK_FP;
    if (!rv_move(c, rv_reg(c, bank, u % 64u, bits),
                 physical_frame(obj, bits, 0)))
      return 0;
  }
  AstPhysicalOperand adjust = physical_immediate(0, 64u);
  memcpy(adjust.register_name, "@adjust", 8u);
  AstPhysicalOperand sp = rv_reg(c, AST_MACHINE_BANK_GPR, 2u, 64u),
                     args[] = {sp, sp, adjust};
  return rv_emit(c, AST_MACHINE_RISCV_ADDI, 1u, args, 3u);
}
static int rv_emit_original(RvContext *c,
                            const AstPhysicalInstructionInfo *inst) {
  if (!rv_emit(c, inst->opcode, inst->result_count, inst->operands,
               inst->operand_count))
    return 0;
  PhysicalBlock *block = &c->fn->blocks[c->block];
  block->instructions[block->count - 1u].span = inst->span;
  return 1;
}
static int rv_relax_frames(RvContext *c) {
  AstPhysicalOperand sp = rv_reg(c, AST_MACHINE_BANK_GPR, 2u, 64u);
  for (uint32_t b = 0; b < c->fn->block_count; ++b) {
    PhysicalBlock original = c->fn->blocks[b];
    memset(&c->fn->blocks[b], 0, sizeof(PhysicalBlock));
    c->block = b;
    int ok = 1;
    for (uint32_t n = 0; ok && n < original.count; ++n) {
      AstPhysicalInstructionInfo i = original.instructions[n];
      c->instruction = i.source_instruction;
      if (i.opcode == AST_MACHINE_RISCV_ADDI &&
          i.operands[0].register_unit == sp.register_unit) {
        int64_t remaining = (int64_t)i.operands[2].immediate_bits;
        int entry =
            b == c->machine->info.entry_block && n == 0u && remaining < 0;
        int exit =
            n + 2u == original.count &&
            original.instructions[n + 1u].opcode == AST_MACHINE_RISCV_RET &&
            remaining > 0;
        if ((entry || exit) && (remaining < -6096 || remaining > 6096)) {
          /* t6 is caller-clobbered and is neither an ABI argument nor
             a return register. It is dead before entry transfers and
             after return transfers, so these boundaries need no save. */
          AstPhysicalOperand scratch =
              rv_reg(c, AST_MACHINE_BANK_GPR, 31u, 64u);
          AstPhysicalOperand imm[] = {
              scratch, physical_immediate((uint64_t)remaining, 64u)};
          AstPhysicalOperand add[] = {sp, sp, scratch};
          ok = rv_emit(c, AST_MACHINE_RISCV_LI, 1u, imm, 2u) &&
               rv_emit(c, AST_MACHINE_RISCV_ADD, 1u, add, 3u);
          continue;
        }
        while (ok && remaining) {
          int64_t step = remaining < -2032  ? -2032
                         : remaining > 2032 ? 2032
                                            : remaining;
          AstPhysicalOperand args[] = {sp, sp,
                                       physical_immediate((uint64_t)step, 64u)};
          ok = rv_emit(c, AST_MACHINE_RISCV_ADDI, 1u, args, 3u);
          remaining -= step;
        }
        continue;
      }
      if (i.operand_count == 2u &&
          i.operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
          (i.operands[1].displacement < -2048 ||
           i.operands[1].displacement > 2047)) {
        AstPhysicalOperand base = i.operands[1];
        int64_t offset = base.displacement;
        base.kind = AST_PHYSICAL_OPERAND_REGISTER;
        base.bit_width = 64u;
        base.displacement = 0;
        if (i.opcode == AST_MACHINE_RISCV_LA &&
            i.operands[0].register_unit != base.register_unit) {
          AstPhysicalOperand imm[] = {
              i.operands[0], physical_immediate((uint64_t)offset, 64u)};
          AstPhysicalOperand add[] = {i.operands[0], base, i.operands[0]};
          ok = rv_emit(c, AST_MACHINE_RISCV_LI, 1u, imm, 2u) &&
               rv_emit(c, AST_MACHINE_RISCV_ADD, 1u, add, 3u);
          continue;
        }
        uint32_t reg = 31u;
        while (reg > 4u &&
               (MACHINE_PHYSREG_FILE_GPR * 64u + reg == base.register_unit ||
                MACHINE_PHYSREG_FILE_GPR * 64u + reg ==
                    i.operands[0].register_unit))
          --reg;
        AstPhysicalOperand scratch = rv_reg(c, AST_MACHINE_BANK_GPR, reg, 64u);
        AstPhysicalOperand down[] = {sp, sp,
                                     physical_immediate((uint64_t)-16, 64u)};
        AstPhysicalOperand save[] = {scratch, rv_stack(c, 64u, 0)};
        AstPhysicalOperand imm[] = {
            scratch,
            physical_immediate(
                (uint64_t)(offset +
                           (base.register_unit == sp.register_unit ? 16 : 0)),
                64u)};
        AstPhysicalOperand address[] = {scratch, base, scratch};
        AstPhysicalOperand restore[] = {scratch, rv_stack(c, 64u, 0)};
        AstPhysicalOperand up[] = {sp, sp, physical_immediate(16u, 64u)};
        ok = rv_emit(c, AST_MACHINE_RISCV_ADDI, 1u, down, 3u) &&
             rv_emit(c, AST_MACHINE_RISCV_SD, 0u, save, 2u) &&
             rv_emit(c, AST_MACHINE_RISCV_LI, 1u, imm, 2u) &&
             rv_emit(c, AST_MACHINE_RISCV_ADD, 1u, address, 3u);
        if (i.opcode == AST_MACHINE_RISCV_LA) {
          i.opcode = AST_MACHINE_RISCV_MV;
          i.operands[1] = scratch;
        } else
          i.operands[1] = rv_memory(scratch, i.operands[1].bit_width, 0);
        ok = ok && rv_emit_original(c, &i) &&
             rv_emit(c, AST_MACHINE_RISCV_LD, 1u, restore, 2u) &&
             rv_emit(c, AST_MACHINE_RISCV_ADDI, 1u, up, 3u);
        continue;
      }
      ok = rv_emit_original(c, &i);
    }
    free(original.instructions);
    if (!ok)
      return 0;
  }
  return 1;
}

int rv_frame_finish(RvContext *c) {
  uint64_t cursor = c->fn->frame.outgoing_size;
  c->fn->frame.alignment = 16u;
  for (uint32_t n = 0; n < c->fn->object_count; ++n) {
    AstFrameObjectInfo *o = &c->fn->objects[n];
    if (!o->alignment || o->alignment > 16u ||
        (o->alignment & (o->alignment - 1u)))
      return regalloc_fail(
          c->program, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, c->function,
          c->instruction,
          "RISC-V over-aligned frame needs dynamic realignment.");
    cursor = (cursor + o->alignment - 1u) & ~((uint64_t)o->alignment - 1u);
    o->offset = (int64_t)cursor;
    cursor += o->size;
  }
  c->fn->frame.object_count = c->fn->object_count;
  c->fn->frame.stack_adjustment = (cursor + 15u) & ~UINT64_C(15);
  /* Address relaxation is intentionally deferred to a dedicated large-frame
   * pass. Never emit an unencodable displacement as an RV64 instruction. */
  if (c->fn->frame.stack_adjustment > INT32_MAX)
    return regalloc_fail(c->program, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME,
                         c->function, c->instruction,
                         "RISC-V fixed frame exceeds supported address range.");
  c->block = c->machine->info.entry_block;
  c->instruction = AST_MACHINE_INVALID_ID;
  PhysicalBlock original = c->fn->blocks[c->block];
  memset(&c->fn->blocks[c->block], 0, sizeof(PhysicalBlock));
  AstPhysicalOperand sp = rv_reg(c, AST_MACHINE_BANK_GPR, 2u, 64u);
  AstPhysicalOperand args[] = {
      sp, sp,
      physical_immediate((uint64_t)-(int64_t)c->fn->frame.stack_adjustment,
                         64u)};
  int ok = 1;
  if (c->fn->frame.stack_adjustment)
    ok = rv_emit(c, AST_MACHINE_RISCV_ADDI, 1u, args, 3u);
  for (uint32_t u = 0; ok && u < MACHINE_REGISTER_UNITS; ++u) {
    uint32_t obj = c->fn->callee_save_objects[u];
    if (obj == AST_MACHINE_INVALID_ID)
      continue;
    uint32_t bits = (uint32_t)c->fn->objects[obj].size * 8u;
    AstMachineRegBank bank = u / 64u == MACHINE_PHYSREG_FILE_GPR
                                 ? AST_MACHINE_BANK_GPR
                                 : AST_MACHINE_BANK_FP;
    ok = rv_move(c, physical_frame(obj, bits, 0),
                 rv_reg(c, bank, u % 64u, bits));
  }
  PhysicalBlock *entry = &c->fn->blocks[c->block];
  size_t bytes;
  if (!ok || original.count > UINT32_MAX - entry->count ||
      ast_size_mul_overflow(original.count + entry->count,
                            sizeof(*entry->instructions), &bytes)) {
    free(original.instructions);
    return 0;
  }
  AstPhysicalInstructionInfo *all = (AstPhysicalInstructionInfo *)realloc(
      entry->instructions, bytes ? bytes : sizeof(*all));
  if (!all) {
    free(original.instructions);
    return 0;
  }
  entry->instructions = all;
  if (original.count)
    memcpy(all + entry->count, original.instructions,
           original.count * sizeof(*all));
  entry->count += original.count;
  entry->capacity = entry->count;
  free(original.instructions);
  for (uint32_t b = 0; b < c->fn->block_count; ++b) {
    PhysicalBlock *block = &c->fn->blocks[b];
    uint32_t written = 0;
    for (uint32_t n = 0; n < block->count; ++n) {
      AstPhysicalInstructionInfo i = block->instructions[n];
      int skip = 0;
      for (uint32_t k = 0; k < i.operand_count; ++k) {
        AstPhysicalOperand *o = &i.operands[k];
        if (strcmp(o->register_name, "@adjust") == 0) {
          memset(o->register_name, 0, sizeof(o->register_name));
          o->immediate_bits = c->fn->frame.stack_adjustment;
          skip = !o->immediate_bits;
        } else if (o->register_name[0] == '@') {
          int64_t offset = o->displacement;
          if (strcmp(o->register_name, "@entry") == 0)
            offset += (int64_t)c->fn->frame.stack_adjustment;
          else if (strcmp(o->register_name, "@frame") == 0 &&
                   o->register_unit < c->fn->object_count)
            offset += c->fn->objects[o->register_unit].offset;
          else
            return 0;
          *o = rv_stack(c, o->bit_width, offset);
        }
      }
      if (i.opcode == AST_MACHINE_RISCV_LA && i.operand_count == 2u &&
          i.operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
          i.operands[1].displacement >= -2048 &&
          i.operands[1].displacement <= 2047) {
        int64_t offset = i.operands[1].displacement;
        i.opcode = AST_MACHINE_RISCV_ADDI;
        i.operand_count = 3u;
        i.operands[1].kind = AST_PHYSICAL_OPERAND_REGISTER;
        i.operands[1].bit_width = 64u;
        i.operands[1].displacement = 0;
        i.operands[2] = physical_immediate((uint64_t)offset, 64u);
      }
      if (!skip)
        block->instructions[written++] = i;
    }
    block->count = written;
  }
  return rv_relax_frames(c);
}
