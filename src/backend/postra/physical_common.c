#include "physical_internal.h"

AstPhysicalOperand physical_register(PhysicalContext *c, AstMachineRegBank bank,
                                     uint32_t index, uint32_t bits) {
  AstPhysicalOperand op = {0};
  MachinePhysRegView view;
  if (!machine_physreg_name(&c->program->target, bank, index, bits,
                            op.register_name) ||
      !machine_physreg_view(&c->program->target, op.register_name, &view))
    return op;
  op.kind = AST_PHYSICAL_OPERAND_REGISTER;
  op.bank = bank;
  op.bit_width = bits;
  op.register_unit = view.alias_unit;
  return op;
}
AstPhysicalOperand physical_named_register(PhysicalContext *c, const char *name,
                                           uint32_t bits) {
  AstPhysicalOperand op = {0};
  MachinePhysRegView view;
  if (!name || !machine_physreg_view(&c->program->target, name, &view) ||
      strlen(name) >= sizeof(op.register_name))
    return op;
  if (view.width != bits) {
    /* ABI descriptions intentionally use canonical architectural register
       names (for example rdi/rax) independent of the scalar payload width.
       x86 has real 8/16/32/64-bit aliases, so resolve the requested view
       instead of rejecting a valid narrow ABI transfer.  AArch64/RV64 do
       not have 8/16-bit GPR names; those logical narrow values travel in
       W/X registers and are normalized by the target move lowering. */
    if (view.file == MACHINE_PHYSREG_FILE_GPR &&
        c->program->target.arch == AST_TARGET_ARCH_X86_64 && bits <= 64u) {
      /* Packed fixed-vector pieces can have logical widths such as 24
         or 48 bits even though x86 only names 8/16/32/64-bit GPR
         views.  Materialize the containing architectural view; vector
         lowering keeps the logical byte count separately. */
      uint32_t storage_bits = bits <= 8u    ? 8u
                              : bits <= 16u ? 16u
                              : bits <= 32u ? 32u
                                            : 64u;
      return physical_register(c, AST_MACHINE_BANK_GPR, view.alias_unit % 64u,
                               storage_bits);
    }
    if (view.file == MACHINE_PHYSREG_FILE_GPR &&
        c->program->target.arch == AST_TARGET_ARCH_AARCH64 && bits <= 32u) {
      AstPhysicalOperand narrow = physical_register(c, AST_MACHINE_BANK_GPR,
                                                    view.alias_unit % 64u, 32u);
      /* Keep the logical payload width while naming the architectural W
         view.  A64 LOAD/STORE uses the memory width; register-register
         moves are normalized to W by aarch64_moves.c. */
      if (narrow.kind)
        narrow.bit_width = bits;
      return narrow;
    }
    if (view.file == MACHINE_PHYSREG_FILE_FP_VECTOR &&
        (bits == 32u || bits == 64u || bits == 128u) &&
        (c->program->target.arch == AST_TARGET_ARCH_AARCH64 ||
         c->program->target.arch == AST_TARGET_ARCH_X86_64 ||
         (c->program->target.arch == AST_TARGET_ARCH_RISCV64 && bits <= 64u))) {
      /* ABI tables use canonical/full-width alias names (vN/xmmN/fN)
         while scalar FP payloads may use narrower architectural views.
         RV64D has 64-bit F registers but single-precision instructions
         still address those same registers with a logical 32-bit value. */
      AstMachineRegBank bank =
          bits <= 64u ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_VECTOR;
      return physical_register(c, bank, view.alias_unit % 64u, bits);
    }
    int legal_narrow_gpr_view =
        view.file == MACHINE_PHYSREG_FILE_GPR &&
        (c->program->target.arch == AST_TARGET_ARCH_RISCV64 && bits < 64u &&
         view.width == 64u);
    if (!legal_narrow_gpr_view)
      return op;
  }
  op.kind = AST_PHYSICAL_OPERAND_REGISTER;
  op.bank = view.file == MACHINE_PHYSREG_FILE_GPR ? AST_MACHINE_BANK_GPR
            : view.file == MACHINE_PHYSREG_FILE_MASK_PREDICATE
                ? AST_MACHINE_BANK_MASK
            : view.file == MACHINE_PHYSREG_FILE_FP_VECTOR && bits <= 64u
                ? AST_MACHINE_BANK_FP
                : AST_MACHINE_BANK_VECTOR;
  op.bit_width = bits;
  op.register_unit = view.alias_unit;
  memcpy(op.register_name, name, strlen(name) + 1u);
  return op;
}
AstPhysicalOperand physical_frame(uint32_t object, uint32_t bits,
                                  int64_t offset) {
  AstPhysicalOperand op = {0};
  op.kind = AST_PHYSICAL_OPERAND_MEMORY;
  op.bit_width = bits;
  op.register_unit = object;
  op.displacement = offset;
  memcpy(op.register_name, "@frame", 7u);
  return op;
}
AstPhysicalOperand physical_entry(uint32_t bits, int64_t offset) {
  AstPhysicalOperand op = physical_frame(0u, bits, offset);
  memcpy(op.register_name, "@entry", 7u);
  return op;
}
AstPhysicalOperand physical_stack(PhysicalContext *c, uint32_t bits,
                                  int64_t offset) {
  AstPhysicalOperand op = physical_register(c, AST_MACHINE_BANK_GPR, 4u, 64u);
  op.kind = AST_PHYSICAL_OPERAND_MEMORY;
  op.bit_width = bits;
  op.displacement = offset;
  return op;
}
AstPhysicalOperand physical_immediate(uint64_t bits, uint32_t width) {
  AstPhysicalOperand op = {0};
  op.kind = AST_PHYSICAL_OPERAND_IMMEDIATE;
  op.immediate_bits = bits;
  op.bit_width = width;
  return op;
}
static AstPhysicalOperand physical_home_at(PhysicalContext *c, uint32_t v,
                                           uint32_t slot) {
  AstPhysicalOperand op = {0};
  if (v >= c->machine->info.vreg_count)
    return op;
  const AstRegAllocLocation *loc = regalloc_location_at(c->fn, v, slot);
  if (!loc)
    return op;
  if (loc->kind == AST_REGALLOC_LOCATION_REGISTER) {
    op = physical_named_register(c, loc->register_name, loc->bit_width);
    op.bank = loc->bank;
  } else if (loc->kind == AST_REGALLOC_LOCATION_SPILL)
    op = physical_frame(loc->frame_object, loc->bit_width, 0);
  else if (loc->kind == AST_REGALLOC_LOCATION_REMATERIALIZE)
    op = physical_immediate(loc->immediate_bits, loc->bit_width);
  return op;
}
AstPhysicalOperand physical_home(PhysicalContext *c, uint32_t v) {
  uint32_t slot = c->instruction < c->machine->info.instruction_count
                      ? c->fn->liveness.instruction_slots[c->instruction]
                      : (c->block < c->fn->liveness.block_count
                             ? c->fn->liveness.block_begin[c->block]
                             : UINT32_MAX);
  return physical_home_at(c, v, slot);
}
AstPhysicalOperand physical_home_at_slot(PhysicalContext *c, uint32_t v,
                                         uint32_t slot) {
  return physical_home_at(c, v, slot);
}
uint32_t physical_temp_object(PhysicalContext *c, uint64_t bytes,
                              AstFrameObjectKind kind) {
  uint32_t align = 1u;
  while (align < 16u && align < bytes)
    align *= 2u;
  uint32_t object = frame_add_object(c->fn, kind, bytes, align);
  if (object == AST_MACHINE_INVALID_ID)
    regalloc_fail(c->program, AST_REGALLOC_DIAG_OUT_OF_MEMORY, c->function,
                  c->instruction, "Cannot allocate physical temporary.");
  return object;
}
int physical_emit(PhysicalContext *c, AstMachineOpcode op, uint32_t defs,
                  const AstPhysicalOperand *args, uint32_t count) {
  if (c->block >= c->fn->block_count || count > AST_PHYSICAL_MAX_OPERANDS ||
      defs > count)
    return regalloc_fail(c->program, AST_REGALLOC_DIAG_IMPOSSIBLE_CONSTRAINT,
                         c->function, c->instruction,
                         "Physical instruction operand limit exceeded.");
  PhysicalBlock *b = &c->fn->blocks[c->block];
  if (b->count == b->capacity) {
    uint32_t cap = b->capacity ? b->capacity * 2u : 32u;
    size_t bytes;
    if (cap < b->capacity ||
        ast_size_mul_overflow(cap, sizeof(*b->instructions), &bytes))
      return 0;
    AstPhysicalInstructionInfo *next =
        (AstPhysicalInstructionInfo *)realloc(b->instructions, bytes);
    if (!next)
      return regalloc_fail(c->program, AST_REGALLOC_DIAG_OUT_OF_MEMORY,
                           c->function, c->instruction,
                           "Cannot grow physical instruction stream.");
    b->instructions = next;
    b->capacity = cap;
  }
  AstPhysicalInstructionInfo *mi = &b->instructions[b->count++];
  memset(mi, 0, sizeof(*mi));
  mi->opcode = op;
  mi->source_instruction = c->instruction;
  mi->result_count = defs;
  mi->operand_count = count;
  if (c->instruction < c->machine->info.instruction_count) {
    mi->span = c->machine->instructions[c->instruction].info.span;
    const MachineInstruction *source =
        &c->machine->instructions[c->instruction];
    for (uint32_t o = 0; o < source->info.operand_count; ++o)
      if (source->info.operands[o].kind == AST_MACHINE_OPERAND_VREG) {
        mi->is_signed = (uint8_t)ast_sema_type_is_signed(
            c->machine->vregs[source->info.operands[o].as.vreg].type);
        break;
      }
    if (source->info.source_op == AST_MIR_OP_ATOMIC ||
        source->info.source_op == AST_MIR_OP_FENCE) {
      mi->memory_order =
          c->machine->instructions[c->instruction].info.memory_order;
      mi->failure_memory_order =
          c->machine->instructions[c->instruction].info.failure_memory_order;
    }
  }
  if (count)
    memcpy(mi->operands, args, (size_t)count * sizeof(*args));
  return 1;
}
int physical_same_location(const AstPhysicalOperand *a,
                           const AstPhysicalOperand *b) {
  if (a->kind != b->kind)
    return 0;
  if (a->kind == AST_PHYSICAL_OPERAND_REGISTER)
    return a->register_unit == b->register_unit && a->bit_width == b->bit_width;
  if (a->kind == AST_PHYSICAL_OPERAND_MEMORY)
    return a->register_unit == b->register_unit &&
           a->index_register_unit == b->index_register_unit &&
           a->scale == b->scale && a->displacement == b->displacement &&
           a->bit_width == b->bit_width &&
           strcmp(a->register_name, b->register_name) == 0 &&
           strcmp(a->index_register_name, b->index_register_name) == 0;
  if (a->kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
    return a->bit_width == b->bit_width &&
           a->immediate_bits == b->immediate_bits;
  return 0;
}
static int emit_simple_move(PhysicalContext *c, AstPhysicalOperand dst,
                            AstPhysicalOperand src) {
  AstPhysicalOperand args[2] = {dst, src};
  if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY) {
    args[0] = src;
    args[1] = dst;
    return physical_emit(c, AST_MACHINE_X86_STORE, 0u, args, 2u);
  }
  return physical_emit(
      c,
      src.kind == AST_PHYSICAL_OPERAND_MEMORY      ? AST_MACHINE_X86_LOAD
      : src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE ? AST_MACHINE_X86_MOV_IMM
                                                   : AST_MACHINE_X86_MOV,
      1u, args, 2u);
}

static int physical_unit_live_now(const PhysicalContext *c, uint32_t unit) {
  if (!c || !c->fn || !c->machine || c->instruction == AST_MACHINE_INVALID_ID ||
      c->instruction >= c->machine->info.instruction_count)
    return 1;
  return regalloc_unit_live_at(
      c->fn, unit, c->fn->liveness.instruction_slots[c->instruction]);
}

static int physical_memory_uses_unit(const AstPhysicalOperand *op,
                                     uint32_t unit) {
  return op && op->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         op->register_name[0] != '@' &&
         (op->register_unit == unit ||
          (op->scale && op->index_register_unit == unit));
}
static int
physical_unit_has_unprotected_abi_value(const PhysicalContext *c,
                                        const AstPhysicalOperand *r) {
  MachinePhysRegView view;
  if (!c || !c->fn || !r || r->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      !machine_physreg_view(&c->program->target, r->register_name, &view))
    return 0;
  return machine_physreg_preserved_bits(c->program->abi, &view) != 0u &&
         c->fn->callee_save_objects[view.alias_unit] == AST_MACHINE_INVALID_ID;
}

static AstPhysicalOperand physical_move_scratch(PhysicalContext *c,
                                                const AstPhysicalOperand *a,
                                                const AstPhysicalOperand *b,
                                                int *needs_restore) {
  static const uint32_t order[] = {11u, 10u, 0u, 1u,  2u,  8u,  9u,
                                   6u,  7u,  3u, 12u, 13u, 14u, 15u};
  AstPhysicalOperand fallback = {0};
  if (needs_restore)
    *needs_restore = 0;
  for (uint32_t i = 0u; i < ASTER_ARRAY_COUNT(order); ++i) {
    AstPhysicalOperand r =
        physical_register(c, AST_MACHINE_BANK_GPR, order[i], 64u);
    if (!r.kind || physical_memory_uses_unit(a, r.register_unit) ||
        physical_memory_uses_unit(b, r.register_unit))
      continue;
    if (!fallback.kind)
      fallback = r;
    /* A callee-saved register can be dead in allocator liveness while its
       incoming caller value is still ABI-live.  Treat it as free only if
       the normal prologue already preserves that unit; otherwise keep
       searching and fall back to an explicitly save/restored scratch. */
    if (!physical_unit_live_now(c, r.register_unit) &&
        !physical_unit_has_unprotected_abi_value(c, &r))
      return r;
  }
  if (fallback.kind && needs_restore)
    *needs_restore = 1;
  return fallback;
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
    /* Prefer a genuinely dead scratch.  The allocator's inverse live
       index makes this O(log ranges-on-unit), so ordinary memory copies
       no longer save/restore r11 just because it is the traditional
       emergency register. */
    int restore = 0;
    AstPhysicalOperand scratch = physical_move_scratch(c, &src, &dst, &restore);
    if (!scratch.kind)
      return 0;
    uint32_t unit = scratch.register_unit;
    uint32_t save = AST_MACHINE_INVALID_ID;
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
      AstPhysicalOperand r = physical_register(
          c, AST_MACHINE_BANK_GPR, scratch.register_unit % 64u, bytes * 8u);
      if (!emit_simple_move(c, r, s) || !emit_simple_move(c, d, r))
        return 0;
      offset += bytes;
      remaining -= bytes;
    }
    return !restore ||
           emit_simple_move(c, scratch, physical_frame(save, 64u, 0));
  }
  if (dst.kind == AST_PHYSICAL_OPERAND_MEMORY &&
      src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE && dst.bit_width == 64u) {
    /* MOV r/m64, imm32 sign-extends. Use a borrowed register for arbitrary
     * imm64. */
    if (src.immediate_bits ==
        (uint64_t)(int64_t)(int32_t)(uint32_t)src.immediate_bits)
      return emit_simple_move(c, dst, src);
    int restore = 0;
    AstPhysicalOperand scratch = physical_move_scratch(c, &dst, NULL, &restore);
    if (!scratch.kind)
      return 0;
    uint32_t unit = scratch.register_unit;
    uint32_t save = AST_MACHINE_INVALID_ID;
    if (restore) {
      if (c->fn->emergency_objects[unit] == AST_MACHINE_INVALID_ID)
        c->fn->emergency_objects[unit] =
            physical_temp_object(c, 8u, AST_FRAME_OBJECT_EMERGENCY);
      save = c->fn->emergency_objects[unit];
      if (save == AST_MACHINE_INVALID_ID ||
          !emit_simple_move(c, physical_frame(save, 64u, 0), scratch))
        return 0;
    }
    if (!emit_simple_move(c, scratch, src) ||
        !emit_simple_move(c, dst, scratch))
      return 0;
    return !restore ||
           emit_simple_move(c, scratch, physical_frame(save, 64u, 0));
  }
  return emit_simple_move(c, dst, src);
}
int physical_parallel_copy(PhysicalContext *c, PhysicalCopy *copies,
                           uint32_t count) {
  /* Register/immediate parallel copies are a dependency graph, not a reason to
     snapshot the whole group.  Emit leaves first (a destination that is no
     longer needed as another source) and use one frame temporary only to
     break a genuine cycle.  This matters at ABI boundaries where aliases
     such as CL/RCX otherwise caused every argument to spill and reload. */
  int register_only = 1;
  for (uint32_t i = 0; i < count; ++i) {
    if (!copies[i].src.bit_width ||
        copies[i].src.bit_width != copies[i].dst.bit_width)
      return 0;
    if (physical_same_location(&copies[i].dst, &copies[i].src))
      continue;
    if ((copies[i].src.kind != AST_PHYSICAL_OPERAND_REGISTER &&
         copies[i].src.kind != AST_PHYSICAL_OPERAND_IMMEDIATE) ||
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
        int destination_still_needed = 0;
        for (uint32_t j = 0; j < count; ++j) {
          if (i == j || !active[j] ||
              copies[j].src.kind != AST_PHYSICAL_OPERAND_REGISTER)
            continue;
          if (copies[i].dst.register_unit == copies[j].src.register_unit) {
            destination_still_needed = 1;
            break;
          }
        }
        if (destination_still_needed)
          continue;
        if (!physical_move(c, copies[i].dst, copies[i].src)) {
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

      /* Genuine cycle. Save one source once, then turn that edge into a
         frame-source edge; the regular leaf scheduler can finish it. */
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
        c->fn->copy_temp =
            physical_temp_object(c, bytes, AST_FRAME_OBJECT_COPY_TEMP);
      else if (c->fn->objects[c->fn->copy_temp].size < bytes)
        c->fn->objects[c->fn->copy_temp].size = bytes;
      if (c->fn->copy_temp == AST_MACHINE_INVALID_ID) {
        if (heap)
          free(active);
        return 0;
      }
      AstPhysicalOperand temp =
          physical_frame(c->fn->copy_temp, copies[cycle].src.bit_width, 0);
      if (!physical_move(c, temp, copies[cycle].src)) {
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

  /* Mixed register/memory copies retain the conservative snapshot contract:
     destination addresses belong to the pre-copy state, so all source
     values are captured before any address register can change. */
  uint64_t bytes = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    if (!copies[i].src.bit_width ||
        copies[i].src.bit_width != copies[i].dst.bit_width)
      return 0;
    if (!physical_same_location(&copies[i].dst, &copies[i].src))
      bytes += ((uint64_t)copies[i].src.bit_width + 7u) / 8u;
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
  for (uint32_t phase = 0; phase < 2u; ++phase) {
    offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (physical_same_location(&copies[i].dst, &copies[i].src))
        continue;
      AstPhysicalOperand temp =
          physical_frame(c->fn->copy_temp, copies[i].src.bit_width, offset);
      if ((copies[i].dst.kind == AST_PHYSICAL_OPERAND_MEMORY) ==
              (phase == 0u) &&
          !physical_move(c, copies[i].dst, temp))
        return 0;
      offset += (int64_t)(((uint64_t)copies[i].src.bit_width + 7u) / 8u);
    }
  }
  return 1;
}
void physical_release_function(AllocationFunction *fn) {
  physical_native_release(fn);
  if (!fn)
    return;
  if (fn->blocks)
    for (uint32_t b = 0; b < fn->block_count; ++b)
      free(fn->blocks[b].instructions);
  free(fn->blocks);
  fn->blocks = NULL;
  fn->block_count = fn->block_capacity = 0u;
}
size_t ast_physical_block_count(const AstRegAllocProgram *p, size_t f) {
  return p && p->finalized && !p->stats.error_count && f < p->function_count
             ? p->functions[f].block_count
             : 0u;
}
int ast_physical_block(const AstRegAllocProgram *p, size_t f, uint32_t b,
                       AstPhysicalBlockInfo *out) {
  if (!p || !out || !p->finalized || p->stats.error_count ||
      f >= p->function_count || b >= p->functions[f].block_count)
    return 0;
  out->id = b;
  out->instruction_count = p->functions[f].blocks[b].count;
  out->instructions = p->functions[f].blocks[b].instructions;
  return 1;
}
