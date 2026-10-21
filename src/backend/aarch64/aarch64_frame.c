#include "aarch64_internal.h"
#include <limits.h>

/* ADD/SUB immediate accepts imm12, optionally shifted left by 12. Keep the

 * physical operand as its effective byte amount; encoding chooses the shift. */
static uint64_t frame_step(uint64_t remaining) {
  if (remaining >= 4096u) {
    uint64_t pages = remaining >> 12u;
    return (pages > 4095u ? 4095u : pages) << 12u;
  }
  return remaining;
}

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
  AstPhysicalOperand sp = a64_sp(c);
  AstPhysicalOperand adjust = physical_immediate(0u, 64u);
  memcpy(adjust.register_name, "@adjust", 8u);
  AstPhysicalOperand args[] = {sp, sp, adjust};
  return physical_emit(c, AST_MACHINE_A64_ADD, 1u, args, 3u);
}
int physical_emit_prologue(PhysicalContext *c) {
  PhysicalBlock original = c->fn->blocks[c->block];
  memset(&c->fn->blocks[c->block], 0, sizeof(PhysicalBlock));
  AstPhysicalOperand sp = a64_sp(c);
  uint64_t remaining = c->fn->frame.stack_adjustment;
  while (remaining) {
    uint64_t step = frame_step(remaining);
    AstPhysicalOperand args[] = {sp, sp, physical_immediate(step, 64u)};
    if (!physical_emit(c, AST_MACHINE_A64_SUB, 1u, args, 3u))
      goto fail;
    if (c->fn->frame.needs_stack_probe) {
      /* Probe each descending page using the AAPCS64 IP1 scratch. */
      AstPhysicalOperand probe[] = {
          physical_register(c, AST_MACHINE_BANK_GPR, 17u, 64u),
          physical_stack(c, 64u, 0)};
      if (!physical_emit(c, AST_MACHINE_A64_LOAD, 1u, probe, 2u))
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
static int emit_original(PhysicalContext *c,
                         const AstPhysicalInstructionInfo *inst) {
  if (!physical_emit(c, inst->opcode, inst->result_count, inst->operands,
                     inst->operand_count))
    return 0;
  PhysicalBlock *block = &c->fn->blocks[c->block];
  block->instructions[block->count - 1u].span = inst->span;
  return 1;
}
int physical_resolve_frames(PhysicalContext *c) {
  for (uint32_t b = 0; b < c->fn->block_count; ++b) {
    PhysicalBlock original = c->fn->blocks[b];
    memset(&c->fn->blocks[b], 0, sizeof(PhysicalBlock));
    c->block = b;
    for (uint32_t i = 0; i < original.count; ++i) {
      AstPhysicalInstructionInfo inst = original.instructions[i];
      c->instruction = inst.source_instruction;
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
            if (op->register_unit >= c->fn->object_count) {
              free(original.instructions);
              return 0;
            }
            offset += c->fn->objects[op->register_unit].offset;
          } else if (strcmp(op->register_name, "@entry") == 0) {
            offset += (int64_t)c->fn->frame.stack_adjustment;
          } else {
            free(original.instructions);
            return 0;
          }
          if (offset < INT32_MIN || offset > INT32_MAX) {
            free(original.instructions);
            return 0;
          }
          *op = physical_stack(c, op->bit_width, offset);
        }
      }
      if (skip)
        continue;
      if (inst.opcode == AST_MACHINE_A64_ADD && inst.operand_count == 3u &&
          inst.operands[0].register_unit == a64_sp(c).register_unit &&
          inst.operands[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
        uint64_t remaining = inst.operands[2].immediate_bits;
        while (remaining) {
          uint64_t step = frame_step(remaining);
          inst.operands[2].immediate_bits = step;
          if (!emit_original(c, &inst)) {
            free(original.instructions);
            return 0;
          }
          remaining -= step;
        }
        continue;
      }
      int memory = -1;
      if (inst.opcode == AST_MACHINE_A64_LOAD ||
          inst.opcode == AST_MACHINE_A64_STORE)
        memory = 1;
      if (memory >= 0) {
        AstPhysicalOperand *address = &inst.operands[(uint32_t)memory];
        uint32_t bytes = (address->bit_width + 7u) / 8u;
        int64_t offset = address->displacement;
        int encodable =
            (offset >= -256 && offset <= 255) ||
            (offset >= 0 && bytes && (uint64_t)offset % bytes == 0u &&
             (uint64_t)offset / bytes <= 4095u);
        if (!encodable) {
          /* A local 16-byte save area bootstraps address materialization,
 even
           * when the frame/outgoing area itself exceeds LDR's range.
 The
           * borrowed home is restored before any call or branch. */
          uint32_t reg = 16u;
          for (; reg < 18u; ++reg) {
            AstPhysicalOperand candidate =
                physical_register(c, AST_MACHINE_BANK_GPR, reg, 64u);
            if (candidate.register_unit != address->register_unit &&
                candidate.register_unit != inst.operands[0].register_unit)
              break;
          }
          if (reg == 18u)
            reg = 9u;
          AstPhysicalOperand scratch = physical_register(
                                 c, AST_MACHINE_BANK_GPR, reg, 64u),
                             sp = a64_sp(c);
          if (scratch.register_unit == address->register_unit ||
              scratch.register_unit == inst.operands[0].register_unit) {
            free(original.instructions);
            return 0;
          }
          AstPhysicalOperand adjust[] = {sp, sp, physical_immediate(16u, 64u)};
          AstPhysicalOperand save[] = {scratch, physical_stack(c, 64u, 0)};
          AstPhysicalOperand effective = *address;
          effective.bit_width = 64u;
          if (effective.register_unit == sp.register_unit)
            effective.displacement += 16;
          AstPhysicalOperand lea[] = {scratch, effective};
          AstPhysicalOperand concrete = scratch;
          concrete.kind = AST_PHYSICAL_OPERAND_MEMORY;
          concrete.bit_width = address->bit_width;
          concrete.displacement = 0;
          *address = concrete;
          if (!physical_emit(c, AST_MACHINE_A64_SUB, 1u, adjust, 3u) ||
              !physical_emit(c, AST_MACHINE_A64_STORE, 0u, save, 2u) ||
              !physical_emit(c, AST_MACHINE_A64_LEA, 1u, lea, 2u) ||
              !emit_original(c, &inst) ||
              !physical_emit(c, AST_MACHINE_A64_LOAD, 1u, save, 2u) ||
              !physical_emit(c, AST_MACHINE_A64_ADD, 1u, adjust, 3u)) {
            free(original.instructions);
            return 0;
          }
          continue;
        }
      }
      if (!emit_original(c, &inst)) {
        free(original.instructions);
        return 0;
      }
    }
    free(original.instructions);
  }
  return 1;
}

int a64_frame_layout(AstRegAllocProgram *p, uint32_t f) {
  AllocationFunction *af = &p->functions[f];
  uint64_t cursor = af->frame.outgoing_size;
  af->frame.alignment = 16u;
  for (uint32_t o = 0; o < af->object_count; ++o) {
    AstFrameObjectInfo *obj = &af->objects[o];
    if (!obj->alignment || (obj->alignment & (obj->alignment - 1u)) ||
        obj->alignment > 16u)
      return regalloc_fail(
          p, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, f, AST_MACHINE_INVALID_ID,
          "AAPCS64 over-aligned frames need dynamic stack realignment.");
    cursor = (cursor + obj->alignment - 1u) & ~((uint64_t)obj->alignment - 1u);
    obj->offset = (int64_t)cursor;
    cursor += obj->size;
    if (cursor > INT32_MAX - 16u)
      return regalloc_fail(
          p, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, f, AST_MACHINE_INVALID_ID,
          "AAPCS64 frame exceeds the supported address range.");
  }
  af->frame.object_count = af->object_count;
  af->frame.stack_adjustment = (cursor + 15u) & ~UINT64_C(15);
  af->frame.uses_red_zone = 0u;
  af->frame.needs_stack_probe = 0u;
  return 1;
}
