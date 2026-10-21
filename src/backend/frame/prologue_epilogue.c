#include "../postra/physical_internal.h"
#include <limits.h>

static AstPhysicalOperand saved_register(PhysicalContext *c, uint32_t unit,
                                         uint32_t object) {
  AstMachineRegBank bank = unit / 64u == MACHINE_PHYSREG_FILE_GPR
                               ? AST_MACHINE_BANK_GPR
                               : AST_MACHINE_BANK_VECTOR;
  return physical_register(c, bank, unit % 64u,
                           (uint32_t)c->fn->objects[object].size * 8u);
}
int physical_emit_epilogue(PhysicalContext *c) {
  for (uint32_t rev = MACHINE_REGISTER_UNITS; rev; --rev) {
    uint32_t u = rev - 1u, object = c->fn->callee_save_objects[u];
    if (object != AST_MACHINE_INVALID_ID) {
      AstPhysicalOperand r = saved_register(c, u, object);
      if (!physical_move(c, r, physical_frame(object, r.bit_width, 0)))
        return 0;
    }
  }
  AstPhysicalOperand sp = physical_register(c, AST_MACHINE_BANK_GPR, 4u, 64u);
  if (c->fn->frame.realigns_stack) {
    AstPhysicalOperand bp = physical_register(c, AST_MACHINE_BANK_GPR, 5u, 64u);
    AstPhysicalOperand args[] = {sp, sp, physical_immediate(8u, 64u)};
    return physical_move(c, sp, bp) &&
           physical_move(c, bp, physical_stack(c, 64u, 0)) &&
           physical_emit(c, AST_MACHINE_X86_ADD, 1u, args, 3u);
  }
  AstPhysicalOperand adjust = physical_immediate(0u, 64u);
  memcpy(adjust.register_name, "@adjust", 8u);
  AstPhysicalOperand args[] = {sp, sp, adjust};
  return physical_emit(c, AST_MACHINE_X86_ADD, 1u, args, 3u);
}
int physical_emit_prologue(PhysicalContext *c) {
  PhysicalBlock original = c->fn->blocks[c->block];
  memset(&c->fn->blocks[c->block], 0, sizeof(PhysicalBlock));
  AstPhysicalOperand sp = physical_register(c, AST_MACHINE_BANK_GPR, 4u, 64u);
  if (c->fn->frame.realigns_stack) {
    AstPhysicalOperand bp = physical_register(c, AST_MACHINE_BANK_GPR, 5u, 64u);
    AstPhysicalOperand reserve[] = {sp, sp, physical_immediate(8u, 64u)};
    AstPhysicalOperand align[] = {
        sp, sp, physical_immediate(UINT64_C(0) - c->fn->frame.alignment, 64u)};
    if (!physical_emit(c, AST_MACHINE_X86_SUB, 1u, reserve, 3u) ||
        !physical_move(c, physical_stack(c, 64u, 0), bp) ||
        !physical_move(c, bp, sp) ||
        !physical_emit(c, AST_MACHINE_X86_AND, 1u, align, 3u))
      goto fail;
  }
  uint64_t remaining = c->fn->frame.stack_adjustment;
  while (remaining) {
    uint64_t step =
        c->fn->frame.needs_stack_probe && remaining > 4096u ? 4096u : remaining;
    AstPhysicalOperand args[] = {sp, sp, physical_immediate(step, 64u)};
    if (!physical_emit(c, AST_MACHINE_X86_SUB, 1u, args, 3u))
      goto fail;
    if (c->fn->frame.needs_stack_probe) {
      /* Probe each page while descending. R11 is volatile in both x86 ABIs
         and is not an incoming argument/result register. */
      AstPhysicalOperand probe[] = {
          physical_register(c, AST_MACHINE_BANK_GPR, 11u, 64u),
          physical_stack(c, 64u, 0)};
      if (!physical_emit(c, AST_MACHINE_X86_LOAD, 1u, probe, 2u))
        goto fail;
    }
    remaining -= step;
  }
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    uint32_t object = c->fn->callee_save_objects[unit];
    if (object != AST_MACHINE_INVALID_ID) {
      AstPhysicalOperand r = saved_register(c, unit, object);
      if (!physical_move(c, physical_frame(object, r.bit_width, 0), r))
        goto fail;
    }
  }
  PhysicalBlock *block = &c->fn->blocks[c->block];
  if (original.count > UINT32_MAX - block->count)
    goto fail;
  uint32_t total = original.count + block->count;
  size_t bytes;
  if (ast_size_mul_overflow(total ? total : 1u, sizeof(*block->instructions),
                            &bytes))
    goto fail;
  AstPhysicalInstructionInfo *next =
      (AstPhysicalInstructionInfo *)realloc(block->instructions, bytes);
  if (!next)
    goto fail;
  block->instructions = next;
  block->capacity = total;
  if (original.count)
    memcpy(block->instructions + block->count, original.instructions,
           (size_t)original.count * sizeof(*next));
  block->count = total;
  free(original.instructions);
  return 1;
fail:
  free(original.instructions);
  return 0;
}
int physical_resolve_frames(PhysicalContext *c) {
  for (uint32_t b = 0; b < c->fn->block_count; ++b) {
    PhysicalBlock *block = &c->fn->blocks[b];
    uint32_t write = 0u;
    for (uint32_t i = 0; i < block->count; ++i) {
      AstPhysicalInstructionInfo inst = block->instructions[i];
      int skip = 0;
      for (uint32_t o = 0; o < inst.operand_count; ++o) {
        AstPhysicalOperand *op = &inst.operands[o];
        if (op->register_name[0] != '@')
          continue;
        if (strcmp(op->register_name, "@adjust") == 0) {
          memset(op->register_name, 0, sizeof(op->register_name));
          op->immediate_bits = c->fn->frame.stack_adjustment;
          if (!op->immediate_bits)
            skip = 1;
        } else {
          int64_t offset = op->displacement;
          if (strcmp(op->register_name, "@frame") == 0) {
            if (op->register_unit >= c->fn->object_count)
              return 0;
            offset += c->fn->objects[op->register_unit].offset;
          } else if (strcmp(op->register_name, "@entry") == 0) {
            if (c->fn->frame.realigns_stack) {
              offset += 16;
              if (offset < INT32_MIN || offset > INT32_MAX)
                return 0;
              uint32_t bits = op->bit_width;
              *op = physical_register(c, AST_MACHINE_BANK_GPR, 5u, 64u);
              op->kind = AST_PHYSICAL_OPERAND_MEMORY;
              op->bit_width = bits;
              op->displacement = offset;
              continue;
            }
            offset += (int64_t)c->fn->frame.stack_adjustment + 8;
          } else
            return 0;
          if (offset < INT32_MIN || offset > INT32_MAX)
            return 0;
          *op = physical_stack(c, op->bit_width, offset);
        }
      }
      if (!skip)
        block->instructions[write++] = inst;
    }
    block->count = write;
  }
  return 1;
}
