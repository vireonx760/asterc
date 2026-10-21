#include "aarch64_internal.h"
int a64_materialize_integer(PhysicalContext *c, AstPhysicalOperand dst,
                            uint64_t value) {
  if (dst.bank != AST_MACHINE_BANK_GPR || !dst.bit_width || dst.bit_width > 64u)
    return 0;
  if (dst.bit_width < 64u)
    value &= (UINT64_C(1) << dst.bit_width) - 1u;
  dst = physical_register(c, AST_MACHINE_BANK_GPR, dst.register_unit % 64u,
                          dst.bit_width <= 32u ? 32u : 64u);
  int first = 1;
  for (uint32_t shift = 0; shift < dst.bit_width; shift += 16u) {
    uint64_t piece = (value >> shift) & UINT64_C(65535);
    if (!piece && !(first && shift + 16u == dst.bit_width))
      continue;
    AstPhysicalOperand args[] = {dst, physical_immediate(piece, 16u),
                                 physical_immediate(shift, 8u)};
    if (!physical_emit(c, first ? AST_MACHINE_A64_MOVZ : AST_MACHINE_A64_MOVK,
                       1u, args, 3u))
      return 0;
    first = 0;
  }
  return 1;
}
AstPhysicalOperand a64_sp(PhysicalContext *c) {
  AstPhysicalOperand op = {0};
  MachinePhysRegView view;
  if (!machine_physreg_view(&c->program->target, "sp", &view))
    return op;
  op.kind = AST_PHYSICAL_OPERAND_REGISTER;
  op.bank = AST_MACHINE_BANK_SPECIAL;
  op.bit_width = 64u;
  op.register_unit = view.alias_unit;
  memcpy(op.register_name, "sp", 3u);
  return op;
}
AstPhysicalOperand a64_stack(PhysicalContext *c, uint32_t bits,
                             int64_t offset) {
  AstPhysicalOperand op = a64_sp(c);
  op.kind = AST_PHYSICAL_OPERAND_MEMORY;
  op.bit_width = bits;
  op.displacement = offset;
  return op;
}
static int a64_unit_live_now(const PhysicalContext *c, uint32_t unit) {
  if (!c || !c->fn || !c->machine || c->instruction == AST_MACHINE_INVALID_ID ||
      c->instruction >= c->machine->info.instruction_count)
    return 1;
  return regalloc_unit_live_at(
      c->fn, unit, c->fn->liveness.instruction_slots[c->instruction]);
}
static int a64_memory_uses_unit(const AstPhysicalOperand *op, uint32_t unit) {
  return op && op->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         op->register_name[0] != '@' &&
         (op->register_unit == unit ||
          (op->scale && op->index_register_unit == unit));
}
static AstPhysicalOperand a64_move_scratch(PhysicalContext *c,
                                           const AstPhysicalOperand *a,
                                           const AstPhysicalOperand *b,
                                           int *needs_restore) {
  static const uint32_t order[] = {11u, 10u, 9u, 8u, 12u, 13u, 14u, 15u, 16u,
                                   17u, 7u,  6u, 5u, 4u,  3u,  2u,  1u,  0u};
  AstPhysicalOperand fallback = {0};
  if (needs_restore)
    *needs_restore = 0;
  for (uint32_t i = 0u; i < ASTER_ARRAY_COUNT(order); ++i) {
    AstPhysicalOperand r =
        physical_register(c, AST_MACHINE_BANK_GPR, order[i], 64u);
    if (!r.kind || a64_memory_uses_unit(a, r.register_unit) ||
        a64_memory_uses_unit(b, r.register_unit))
      continue;
    if (!fallback.kind)
      fallback = r;
    if (!a64_unit_live_now(c, r.register_unit))
      return r;
  }
  if (fallback.kind && needs_restore)
    *needs_restore = 1;
  return fallback;
}
static int emit_simple_move(PhysicalContext *c, AstPhysicalOperand dst,
                            AstPhysicalOperand src) {
  if (src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
      dst.kind == AST_PHYSICAL_OPERAND_REGISTER)
    return a64_materialize_integer(c, dst, src.immediate_bits);
  if (dst.kind == AST_PHYSICAL_OPERAND_REGISTER &&
      src.kind == AST_PHYSICAL_OPERAND_REGISTER &&
      dst.bank == AST_MACHINE_BANK_GPR && src.bank == AST_MACHINE_BANK_GPR) {
    dst = physical_register(c, AST_MACHINE_BANK_GPR, dst.register_unit % 64u,
                            dst.bit_width <= 32u ? 32u : 64u);
    src = physical_register(c, AST_MACHINE_BANK_GPR, src.register_unit % 64u,
                            dst.bit_width);
  } else if (dst.kind == AST_PHYSICAL_OPERAND_REGISTER &&
             src.kind == AST_PHYSICAL_OPERAND_MEMORY &&
             dst.bank == AST_MACHINE_BANK_GPR) {
    /* LDRB/LDRH/LDR Wt materialize narrow integer loads in a W register.
       Keep the memory operand width as the access size, but canonicalize
       the architectural register view for the encoder/verifier. */
    dst = physical_register(c, AST_MACHINE_BANK_GPR, dst.register_unit % 64u,
                            src.bit_width <= 32u ? 32u : 64u);
  } else if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY &&
             src.kind == AST_PHYSICAL_OPERAND_REGISTER &&
             src.bank == AST_MACHINE_BANK_GPR) {
    /* STRB/STRH/STR Wt likewise names Wt even when the memory access is
       only 8 or 16 bits. */
    src = physical_register(c, AST_MACHINE_BANK_GPR, src.register_unit % 64u,
                            dst.bit_width <= 32u ? 32u : 64u);
  }
  AstPhysicalOperand args[2] = {dst, src};
  if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY) {
    args[0] = src;
    args[1] = dst;
    return physical_emit(c, AST_MACHINE_A64_STORE, 0u, args, 2u);
  }
  return physical_emit(
      c,
      src.kind == AST_PHYSICAL_OPERAND_MEMORY      ? AST_MACHINE_A64_LOAD
      : src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE ? AST_MACHINE_A64_MOV_IMM
                                                   : AST_MACHINE_A64_MOV,
      1u, args, 2u);
}
int physical_move(PhysicalContext *c, AstPhysicalOperand dst,
                  AstPhysicalOperand src) {
  if (physical_same_location(&dst, &src))
    return 1;
  if (!dst.kind || !src.kind ||
      (dst.kind != AST_PHYSICAL_OPERAND_REGISTER &&
       dst.kind != AST_PHYSICAL_OPERAND_MEMORY))
    return 0;
  if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY &&
      src.kind == AST_PHYSICAL_OPERAND_MEMORY) {
    if (dst.bit_width != src.bit_width || !dst.bit_width || dst.bit_width % 8u)
      return 0;
    int restore = 0;
    AstPhysicalOperand scratch = a64_move_scratch(c, &src, &dst, &restore);
    if (!scratch.kind)
      return 0;
    uint32_t unit = scratch.register_unit, save = AST_MACHINE_INVALID_ID;
    if (restore) {
      if (c->fn->emergency_objects[unit] == AST_MACHINE_INVALID_ID)
        c->fn->emergency_objects[unit] =
            physical_temp_object(c, 8u, AST_FRAME_OBJECT_EMERGENCY);
      save = c->fn->emergency_objects[unit];
      if (save == AST_MACHINE_INVALID_ID ||
          !emit_simple_move(c, physical_frame(save, 64u, 0), scratch))
        return 0;
    }
    uint32_t remaining = dst.bit_width / 8u;
    int64_t offset = 0;
    while (remaining) {
      uint32_t bytes = remaining >= 8u   ? 8u
                       : remaining >= 4u ? 4u
                       : remaining >= 2u ? 2u
                                         : 1u;
      AstPhysicalOperand s = src, d = dst;
      s.bit_width = d.bit_width = bytes * 8u;
      s.displacement += offset;
      d.displacement += offset;
      uint32_t access_bits = bytes * 8u;
      AstPhysicalOperand r = physical_register(
          c, AST_MACHINE_BANK_GPR, scratch.register_unit % 64u,
          access_bits < 32u ? 32u : access_bits);
      if (!emit_simple_move(c, r, s) || !emit_simple_move(c, d, r))
        return 0;
      offset += bytes;
      remaining -= bytes;
    }
    return !restore ||
           emit_simple_move(c, scratch, physical_frame(save, 64u, 0));
  }
  if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY &&
      src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    /* AArch64 cannot store immediates. Prefer a scratch whose final
       allocation unit is dead at this instruction, avoiding the old
       unconditional emergency spill/reload pair. */
    int restore = 0;
    AstPhysicalOperand scratch = a64_move_scratch(c, &dst, NULL, &restore);
    if (!scratch.kind)
      return 0;
    uint32_t unit = scratch.register_unit, save = AST_MACHINE_INVALID_ID;
    if (restore) {
      if (c->fn->emergency_objects[unit] == AST_MACHINE_INVALID_ID)
        c->fn->emergency_objects[unit] =
            physical_temp_object(c, 8u, AST_FRAME_OBJECT_EMERGENCY);
      save = c->fn->emergency_objects[unit];
      if (save == AST_MACHINE_INVALID_ID ||
          !emit_simple_move(c, physical_frame(save, 64u, 0), scratch))
        return 0;
    }
    uint32_t reg_bits = dst.bit_width < 32u ? 32u : dst.bit_width;
    AstPhysicalOperand value = physical_register(
        c, AST_MACHINE_BANK_GPR, scratch.register_unit % 64u, reg_bits);
    if (!emit_simple_move(c, value, src) || !emit_simple_move(c, dst, value))
      return 0;
    return !restore ||
           emit_simple_move(c, scratch, physical_frame(save, 64u, 0));
  }
  return emit_simple_move(c, dst, src);
}
int physical_parallel_copy(PhysicalContext *c, PhysicalCopy *copies,
                           uint32_t count) {
  /* Fast path: no destination destroys any other source. Memory sources use
     a snapshot when aliasing cannot be ruled out from their base/offset. */
  int snapshot = 0;
  uint64_t bytes = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    if (!copies[i].src.bit_width ||
        copies[i].src.bit_width != copies[i].dst.bit_width)
      return 0;
    if (physical_same_location(&copies[i].dst, &copies[i].src))
      continue;
    bytes += ((uint64_t)copies[i].src.bit_width + 7u) / 8u;
    for (uint32_t j = 0; j < count; ++j) {
      if (i == j)
        continue;
      if (copies[i].dst.kind == AST_PHYSICAL_OPERAND_REGISTER &&
          ((copies[j].src.kind == AST_PHYSICAL_OPERAND_REGISTER &&
            copies[i].dst.register_unit == copies[j].src.register_unit) ||
           (copies[j].src.kind == AST_PHYSICAL_OPERAND_MEMORY &&
            copies[j].src.register_name[0] != '@' &&
            copies[i].dst.register_unit == copies[j].src.register_unit)))
        snapshot = 1;
      if (copies[i].dst.kind == AST_PHYSICAL_OPERAND_MEMORY &&
          copies[j].src.kind == AST_PHYSICAL_OPERAND_MEMORY)
        snapshot = 1;
    }
  }
  if (!snapshot) {
    for (uint32_t i = 0; i < count; ++i)
      if (!physical_move(c, copies[i].dst, copies[i].src))
        return 0;
    return 1;
  }
  if (!bytes)
    return 1;
  if (c->fn->copy_temp == AST_MACHINE_INVALID_ID)
    c->fn->copy_temp =
        physical_temp_object(c, bytes, AST_FRAME_OBJECT_COPY_TEMP);
  else if (c->fn->objects[c->fn->copy_temp].size < bytes)
    c->fn->objects[c->fn->copy_temp].size = bytes;
  if (c->fn->copy_temp == AST_MACHINE_INVALID_ID)
    return 0;
  int64_t offset = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (physical_same_location(&copies[i].dst, &copies[i].src))
      continue;
    AstPhysicalOperand temp =
        physical_frame(c->fn->copy_temp, copies[i].src.bit_width, offset);
    if (!physical_move(c, temp, copies[i].src))
      return 0;
    offset += (int64_t)(((uint64_t)copies[i].src.bit_width + 7u) / 8u);
  }
  offset = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (physical_same_location(&copies[i].dst, &copies[i].src))
      continue;
    AstPhysicalOperand temp =
        physical_frame(c->fn->copy_temp, copies[i].src.bit_width, offset);
    if (!physical_move(c, copies[i].dst, temp))
      return 0;
    offset += (int64_t)(((uint64_t)copies[i].src.bit_width + 7u) / 8u);
  }
  return 1;
}
