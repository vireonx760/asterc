#include "physical_internal.h"

/* This pass runs before frame resolution. Only allocator-owned spill and
   scratch objects are tracked; source locals and ABI/callee-save images may
   have observable addresses or verifier-mandated transfer sequences. */
typedef struct SpillValue {
  AstPhysicalOperand memory, value;
  uint32_t store;
  uint8_t valid;
} SpillValue;

typedef struct TransferForms {
  AstTargetArch arch;
  AstMachineOpcode copy, immediate, load, store, branch;
} TransferForms;
static TransferForms transfer_forms(AstTargetArch arch) {
  if (arch == AST_TARGET_ARCH_AARCH64)
    return (TransferForms){arch,
                           AST_MACHINE_A64_MOV,
                           AST_MACHINE_OP_INVALID,
                           AST_MACHINE_A64_LOAD,
                           AST_MACHINE_A64_STORE,
                           AST_MACHINE_A64_BR};
  if (arch == AST_TARGET_ARCH_RISCV64)
    return (TransferForms){arch,
                           AST_MACHINE_RISCV_MV,
                           AST_MACHINE_RISCV_LI,
                           AST_MACHINE_RISCV_LD,
                           AST_MACHINE_RISCV_SD,
                           AST_MACHINE_RISCV_J};
  return (TransferForms){arch,
                         AST_MACHINE_X86_MOV,
                         AST_MACHINE_X86_MOV_IMM,
                         AST_MACHINE_X86_LOAD,
                         AST_MACHINE_X86_STORE,
                         AST_MACHINE_X86_BR};
}
static int gpr(const AstPhysicalOperand *o) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_GPR &&
         o->register_unit / 64u == MACHINE_PHYSREG_FILE_GPR &&
         o->register_unit % 64u < 32u &&
         (o->bit_width == 8u || o->bit_width == 16u || o->bit_width == 32u ||
          o->bit_width == 64u);
}
static int same_register(const AstPhysicalOperand *a,
                         const AstPhysicalOperand *b) {
  /* AL and AH share an alias unit and width, but have different bits. */
  return gpr(a) && gpr(b) && physical_same_location(a, b) &&
         strcmp(a->register_name, b->register_name) == 0;
}
static int private_slot(const AllocationFunction *fn,
                        const AstPhysicalOperand *o) {
  if (o->kind != AST_PHYSICAL_OPERAND_MEMORY || o->scale ||
      strcmp(o->register_name, "@frame") ||
      o->register_unit >= fn->object_count)
    return 0;
  AstFrameObjectKind k = fn->objects[o->register_unit].kind;
  return k == AST_FRAME_OBJECT_SPILL || k == AST_FRAME_OBJECT_COPY_TEMP ||
         k == AST_FRAME_OBJECT_EMERGENCY;
}
static int simple_transfer(const TransferForms *forms,
                           const AllocationFunction *fn,
                           const AstPhysicalInstructionInfo *i) {
  if (i->nontemporal || i->memory_order || i->failure_memory_order ||
      i->operand_count != 2u)
    return 0;
  const AstPhysicalOperand *o = i->operands;
  if (o[0].bit_width != o[1].bit_width)
    return 0;
  if (i->result_count && forms->arch == AST_TARGET_ARCH_RISCV64 &&
      o[0].register_unit == MACHINE_PHYSREG_FILE_GPR * 64u)
    return 0;
  if (i->opcode == forms->copy)
    return i->result_count == 1u && gpr(&o[0]) && gpr(&o[1]);
  if (i->opcode == forms->immediate)
    return i->result_count == 1u && gpr(&o[0]) &&
           o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE;
  /* A narrow load may extend the payload. Track only exact architectural
     transfers until an independently proven upper-bit value is available. */
  if (forms->arch == AST_TARGET_ARCH_AARCH64 && o[1].bit_width < 32u)
    return 0;
  if (forms->arch == AST_TARGET_ARCH_RISCV64 && o[1].bit_width != 64u)
    return 0;
  if (i->opcode == forms->load)
    return i->result_count == 1u && gpr(&o[0]) && private_slot(fn, &o[1]);
  if (i->opcode == forms->store)
    return !i->result_count && private_slot(fn, &o[1]) &&
           (gpr(&o[0]) || o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE);
  return 0;
}
static void invalidate_register(AstPhysicalOperand *copies, SpillValue *slots,
                                uint32_t count, uint32_t unit) {
  for (uint32_t u = 0; u < 32u; ++u)
    if (u == unit % 64u || (copies[u].kind && copies[u].register_unit == unit))
      copies[u].kind = AST_PHYSICAL_OPERAND_INVALID;
  for (uint32_t s = 0; s < count; ++s)
    if (slots[s].valid && gpr(&slots[s].value) &&
        slots[s].value.register_unit == unit)
      slots[s].valid = 0u;
}
static void clear_values(AstPhysicalOperand *copies, SpillValue *slots,
                         uint32_t count) {
  for (uint32_t u = 0; u < 32u; ++u)
    copies[u].kind = AST_PHYSICAL_OPERAND_INVALID;
  for (uint32_t s = 0; s < count; ++s)
    slots[s].valid = 0u;
}

/* A base RV64 select needs no optional extension. Use one dead caller-saved
   register for a full-width mask and the destination for the XOR delta.
   If pressure leaves no dead scratch, retain the original conditional path. */
int physical_rv_select(PhysicalContext *c, AstPhysicalOperand dst,
                       AstPhysicalOperand condition, AstPhysicalOperand yes,
                       AstPhysicalOperand no, int *handled) {
  *handled = 0;
  if (!gpr(&dst) || !gpr(&yes) || !gpr(&no) ||
      condition.kind != AST_PHYSICAL_OPERAND_REGISTER ||
      condition.bank != AST_MACHINE_BANK_GPR ||
      dst.bit_width != yes.bit_width || dst.bit_width != no.bit_width ||
      dst.register_unit % 64u == 0u)
    return 1;
  if (same_register(&yes, &no)) {
    *handled = 1;
    AstPhysicalOperand args[] = {dst, yes};
    return same_register(&dst, &yes) ||
           physical_emit(c, AST_MACHINE_RISCV_MV, 1u, args, 2u);
  }
  if (c->instruction >= c->machine->info.instruction_count ||
      !c->fn->liveness.instruction_slots)
    return 1;
  uint32_t slot = c->fn->liveness.instruction_slots[c->instruction];
  static const uint32_t order[] = {31u, 30u, 29u, 28u, 7u,  6u,  5u, 17u,
                                   16u, 15u, 14u, 13u, 12u, 11u, 10u};
  AstPhysicalOperand mask = {0};
  for (uint32_t n = 0; n < ASTER_ARRAY_COUNT(order); ++n) {
    uint32_t unit = MACHINE_PHYSREG_FILE_GPR * 64u + order[n];
    if (unit == dst.register_unit || unit == condition.register_unit ||
        unit == yes.register_unit || unit == no.register_unit ||
        regalloc_unit_live_at(c->fn, unit, slot))
      continue;
    mask = physical_register(c, AST_MACHINE_BANK_GPR, order[n], 64u);
    break;
  }
  if (!mask.kind)
    return 1;
  *handled = 1;
  int base_is_yes = dst.register_unit == no.register_unit;
  AstPhysicalOperand base = base_is_yes ? yes : no;
  AstPhysicalOperand zero = physical_register(c, AST_MACHINE_BANK_GPR, 0u, 64u);
  AstPhysicalOperand compare[] = {mask, base_is_yes ? condition : zero,
                                  base_is_yes ? physical_immediate(1u, 64u)
                                              : condition};
  AstPhysicalOperand negate[] = {mask, mask};
  AstPhysicalOperand delta[] = {dst, yes, no};
  AstPhysicalOperand filter[] = {dst, dst, mask};
  AstPhysicalOperand finish[] = {dst, base, dst};
  return physical_emit(
             c, base_is_yes ? AST_MACHINE_RISCV_SLTIU : AST_MACHINE_RISCV_SLTU,
             1u, compare, 3u) &&
         physical_emit(c, AST_MACHINE_RISCV_NEG, 1u, negate, 2u) &&
         physical_emit(c, AST_MACHINE_RISCV_XOR, 1u, delta, 3u) &&
         physical_emit(c, AST_MACHINE_RISCV_AND, 1u, filter, 3u) &&
         physical_emit(c, AST_MACHINE_RISCV_XOR, 1u, finish, 3u);
}

static int convert_other_diamonds(PhysicalContext *c,
                                  const TransferForms *forms) {
  AllocationFunction *fn = c->fn;
  uint32_t *preds =
      calloc(fn->block_count ? fn->block_count : 1u, sizeof(*preds));
  if (!preds)
    return 0;
  for (uint32_t b = 0; b < fn->block_count; ++b)
    for (uint32_t j = 0; j < fn->blocks[b].count; ++j) {
      const AstPhysicalInstructionInfo *i = &fn->blocks[b].instructions[j];
      for (uint32_t o = 0; o < i->operand_count; ++o)
        if (i->operands[o].kind == AST_PHYSICAL_OPERAND_BLOCK &&
            i->operands[o].block < fn->block_count)
          ++preds[i->operands[o].block];
    }
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    PhysicalBlock *bb = &fn->blocks[b];
    if (bb->count < 2u)
      continue;
    AstPhysicalInstructionInfo branch = bb->instructions[bb->count - 1u];
    uint32_t t, f, removed = 1u;
    AstPhysicalOperand condition = {0};
    if (forms->arch == AST_TARGET_ARCH_AARCH64) {
      if (branch.opcode != AST_MACHINE_A64_COND_BR ||
          branch.operand_count != 2u ||
          bb->instructions[bb->count - 2u].opcode != AST_MACHINE_A64_CMP_ZERO ||
          branch.operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
          branch.operands[1].kind != AST_PHYSICAL_OPERAND_BLOCK)
        continue;
      t = branch.operands[0].block;
      f = branch.operands[1].block;
    } else {
      const AstPhysicalInstructionInfo *test =
          &bb->instructions[bb->count - 2u];
      if (branch.opcode != AST_MACHINE_RISCV_J || branch.operand_count != 1u ||
          branch.operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
          test->opcode != AST_MACHINE_RISCV_BNE || test->operand_count != 3u ||
          test->operands[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
          test->operands[1].register_unit != MACHINE_PHYSREG_FILE_GPR * 64u ||
          test->operands[2].kind != AST_PHYSICAL_OPERAND_BLOCK)
        continue;
      condition = test->operands[0];
      t = test->operands[2].block;
      f = branch.operands[0].block;
      removed = 2u;
    }
    if (t >= fn->block_count || f >= fn->block_count || t == f || t == b ||
        f == b || t == c->machine->info.entry_block ||
        f == c->machine->info.entry_block || preds[t] != 1u || preds[f] != 1u)
      continue;
    PhysicalBlock *yes = &fn->blocks[t], *no = &fn->blocks[f];
    if (yes->count != 2u || no->count != 2u)
      continue;
    AstPhysicalInstructionInfo a = yes->instructions[0],
                               z = no->instructions[0];
    AstPhysicalInstructionInfo at = yes->instructions[1],
                               zt = no->instructions[1];
    if (a.opcode != forms->copy || z.opcode != forms->copy ||
        !simple_transfer(forms, fn, &a) || !simple_transfer(forms, fn, &z) ||
        !same_register(&a.operands[0], &z.operands[0]) ||
        a.operands[0].bit_width < 32u || at.opcode != forms->branch ||
        zt.opcode != forms->branch || at.operand_count != 1u ||
        zt.operand_count != 1u ||
        at.operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        zt.operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        at.operands[0].block != zt.operands[0].block ||
        at.operands[0].block == b || at.operands[0].block == t ||
        at.operands[0].block == f)
      continue;
    uint32_t old_count = bb->count;
    bb->count -= removed;
    c->block = b;
    c->instruction = branch.source_instruction;
    int handled = 1, ok;
    if (forms->arch == AST_TARGET_ARCH_AARCH64) {
      AstPhysicalOperand args[] = {a.operands[0], a.operands[1], z.operands[1],
                                   physical_immediate(1u, 8u)};
      ok = physical_emit(c, AST_MACHINE_A64_CSEL, 1u, args, 4u);
    } else
      ok = physical_rv_select(c, a.operands[0], condition, a.operands[1],
                              z.operands[1], &handled);
    if (!handled) {
      bb->count = old_count;
      continue;
    }
    if (!ok || !physical_emit(c, forms->branch, 0u, &at.operands[0], 1u)) {
      free(preds);
      return 0;
    }
  }
  free(preds);
  return 1;
}

static int convert_diamonds(PhysicalContext *c) {
  TransferForms forms = transfer_forms(c->program->target.arch);
  if (forms.arch != AST_TARGET_ARCH_X86_64)
    return convert_other_diamonds(c, &forms);
  AllocationFunction *fn = c->fn;
  uint32_t *preds =
      calloc(fn->block_count ? fn->block_count : 1u, sizeof(*preds));
  if (!preds)
    return 0;
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    PhysicalBlock *bb = &fn->blocks[b];
    if (!bb->count)
      continue;
    const AstPhysicalInstructionInfo *term = &bb->instructions[bb->count - 1u];
    for (uint32_t o = 0; o < term->operand_count; ++o)
      if (term->operands[o].kind == AST_PHYSICAL_OPERAND_BLOCK &&
          term->operands[o].block < fn->block_count)
        ++preds[term->operands[o].block];
  }
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    PhysicalBlock *bb = &fn->blocks[b];
    if (!bb->count)
      continue;
    AstPhysicalInstructionInfo branch = bb->instructions[bb->count - 1u];
    if (branch.opcode != AST_MACHINE_X86_COND_BR ||
        branch.operand_count != 3u ||
        branch.operands[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        branch.operands[1].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        branch.operands[2].kind != AST_PHYSICAL_OPERAND_BLOCK)
      continue;
    uint32_t t = branch.operands[1].block, f = branch.operands[2].block;
    if (t >= fn->block_count || f >= fn->block_count || t == f || t == b ||
        f == b || t == c->machine->info.entry_block ||
        f == c->machine->info.entry_block || preds[t] != 1u || preds[f] != 1u)
      continue;
    PhysicalBlock *yes = &fn->blocks[t], *no = &fn->blocks[f];
    if (yes->count != 2u || no->count != 2u)
      continue;
    const AstPhysicalInstructionInfo *a = &yes->instructions[0],
                                     *z = &no->instructions[0];
    const AstPhysicalInstructionInfo *at = &yes->instructions[1],
                                     *zt = &no->instructions[1];
    if (a->opcode != AST_MACHINE_X86_MOV || z->opcode != AST_MACHINE_X86_MOV ||
        !simple_transfer(&forms, fn, a) || !simple_transfer(&forms, fn, z) ||
        !same_register(&a->operands[0], &z->operands[0]) ||
        a->operands[0].bit_width < 16u || at->opcode != AST_MACHINE_X86_BR ||
        zt->opcode != AST_MACHINE_X86_BR || at->operand_count != 1u ||
        zt->operand_count != 1u ||
        at->operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        zt->operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        at->operands[0].block != zt->operands[0].block ||
        at->operands[0].block == b || at->operands[0].block == t ||
        at->operands[0].block == f)
      continue;
    /* Both arms only copy already available registers. There are no loads,
       traps, calls, edge cycles or extra predecessors to speculate. Keep
       the old arm blocks as valid views; the encoder omits unreachable CFG. */
    uint32_t old_count = bb->count;
    c->block = b;
    AstPhysicalOperand target = at->operands[0];
    if (!physical_emit(c, AST_MACHINE_X86_BR, 0u, &target, 1u)) {
      free(preds);
      return 0;
    }
    AstPhysicalInstructionInfo *select = &bb->instructions[old_count - 1u];
    *select = branch;
    select->opcode = AST_MACHINE_X86_SELECT;
    select->result_count = 1u;
    select->operand_count = 4u;
    select->operands[0] = a->operands[0];
    select->operands[1] = branch.operands[0];
    select->operands[2] = a->operands[1];
    select->operands[3] = z->operands[1];
    bb->instructions[old_count].source_instruction = branch.source_instruction;
    bb->instructions[old_count].span = branch.span;
  }
  free(preds);
  return 1;
}

int physical_run_peephole(PhysicalContext *c) {
  if (!c || !c->fn || !c->machine)
    return 0;
  /* Native packets, bind and manual functions have exact transfer contracts.
     Never rewrite their physical register identities or instruction stream. */
  for (uint32_t i = 0; i < c->machine->info.instruction_count; ++i)
    if (c->machine->instructions[i].info.source_op == AST_MIR_OP_NATIVE ||
        c->machine->instructions[i].info.source_op == AST_MIR_OP_BIND)
      return 1;
  AllocationFunction *fn = c->fn;
  TransferForms forms = transfer_forms(c->program->target.arch);
  size_t slots_count = fn->object_count ? fn->object_count : 1u;
  SpillValue *slots = calloc(slots_count, sizeof(*slots));
  if (!slots)
    return 0;
  AstPhysicalOperand copies[32] = {0};
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    PhysicalBlock *block = &fn->blocks[b];
    uint8_t *dead = calloc(block->count ? block->count : 1u, 1u);
    if (!dead) {
      free(slots);
      return 0;
    }
    clear_values(copies, slots, fn->object_count);
    for (uint32_t j = 0; j < block->count; ++j) {
      AstPhysicalInstructionInfo *i = &block->instructions[j];
      if (!simple_transfer(&forms, fn, i)) {
        /* Calls, unknown/aliased memory, partial effects, atomics and flags
           consumers are barriers. No assumptions about their hidden defs. */
        clear_values(copies, slots, fn->object_count);
        continue;
      }
      AstPhysicalOperand *o = i->operands;
      if (i->opcode == forms.copy) {
        AstPhysicalOperand source = copies[o[1].register_unit % 64u];
        if (source.kind && source.bit_width == o[1].bit_width &&
            /* High-byte aliases cannot use the low-byte copy map. */
            o[1].register_name[1] != 'h')
          o[1] = source;
      }
      if (i->opcode == forms.load) {
        SpillValue *s = &slots[o[1].register_unit];
        if (s->valid && physical_same_location(&s->memory, &o[1])) {
          o[1] = s->value;
          i->opcode = o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE
                          ? forms.immediate
                          : forms.copy;
        }
      }
      if (i->opcode == forms.store) {
        SpillValue *s = &slots[o[1].register_unit];
        if (s->valid && physical_same_location(&s->memory, &o[1])) {
          /* All intervening reads were replaced with register/immediate
             moves. The earlier private-slot store has no remaining reader. */
          if (s->store != AST_MACHINE_INVALID_ID)
            dead[s->store] = 1u;
        }
        s->memory = o[1];
        s->value = o[0];
        s->store = j;
        s->valid = 1u;
        continue;
      }
      if (i->opcode == forms.copy && same_register(&o[0], &o[1]) &&
          (o[0].bit_width != 32u || forms.arch == AST_TARGET_ARCH_RISCV64)) {
        dead[j] = 1u;
        continue;
      }
      AstPhysicalOperand source = o[1];
      invalidate_register(copies, slots, fn->object_count, o[0].register_unit);
      if (i->opcode == forms.copy &&
          o[0].register_unit != source.register_unit &&
          o[0].register_name[1] != 'h' && source.register_name[1] != 'h')
        copies[o[0].register_unit % 64u] = source;
      /* A real private load also establishes its value in the destination. */
      if (i->opcode == forms.load) {
        SpillValue *s = &slots[o[1].register_unit];
        s->memory = o[1];
        s->value = o[0];
        s->store = AST_MACHINE_INVALID_ID;
        s->valid = 1u;
      }
    }
    /* After propagation, remove copies killed before any read. Live-out and
       all implicit contracts remain conservatively live. A partial write
       never kills the rest of an alias unit (including the high byte). */
    uint8_t live[32];
    memset(live, 1, sizeof(live));
    for (uint32_t rev = block->count; rev; --rev) {
      uint32_t j = rev - 1u;
      AstPhysicalInstructionInfo *i = &block->instructions[j];
      if (dead[j])
        continue;
      if (!simple_transfer(&forms, fn, i)) {
        memset(live, 1, sizeof(live));
        continue;
      }
      AstPhysicalOperand *o = i->operands;
      if (i->result_count && !live[o[0].register_unit % 64u]) {
        /* A genuine load can fault; only discard register/immediate moves. */
        if (i->opcode == forms.copy || i->opcode == forms.immediate) {
          dead[j] = 1u;
          continue;
        }
      }
      if (i->result_count &&
          (o[0].bit_width >= 32u || forms.arch == AST_TARGET_ARCH_RISCV64))
        live[o[0].register_unit % 64u] = 0u;
      uint32_t use = i->opcode == forms.store ? 0u : 1u;
      if (gpr(&o[use]))
        live[o[use].register_unit % 64u] = 1u;
    }
    uint32_t written = 0u;
    for (uint32_t j = 0; j < block->count; ++j)
      if (!dead[j])
        block->instructions[written++] = block->instructions[j];
    block->count = written;
    free(dead);
  }
  free(slots);
  return convert_diamonds(c);
}
