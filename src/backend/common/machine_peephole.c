#include "machine_internal.h"

/* Machine IR remains source-shaped after this pass.  Removing entries from a
   block would invalidate MIR source ids, ABI call lookup, and the borrowed
   query views, so dead instructions are retained as tombstones and skipped by
   target lowering.  Copy propagation does not change definitions; it only
   rewrites non-constrained SSA uses to their dominating source value. */

AstMachineVReg machine_canonical_vreg(const MachineFunction *fn,
                                      AstMachineVReg vreg) {
  if (!fn || !fn->vreg_alias || vreg >= fn->info.vreg_count)
    return vreg;
  AstMachineVReg current = vreg;
  for (uint32_t steps = 0u; steps < fn->info.vreg_count; ++steps) {
    AstMachineVReg next = fn->vreg_alias[current];
    if (next == current || next >= fn->info.vreg_count)
      return current;
    current = next;
  }
  /* A malformed cycle must never make verification or lowering loop. */
  return vreg;
}

static int copy_pair_compatible(const MachineFunction *fn, AstMachineVReg dst,
                                AstMachineVReg src) {
  if (!fn || dst >= fn->info.vreg_count || src >= fn->info.vreg_count)
    return 0;
  const AstMachineVRegInfo *d = &fn->vregs[dst];
  const AstMachineVRegInfo *s = &fn->vregs[src];
  return d->part_index == s->part_index && d->part_count == s->part_count &&
         d->bit_width == s->bit_width && d->reg_class == s->reg_class &&
         (!d->type || !s->type || d->type == s->type);
}

static int is_block_argument(const MachineFunction *fn, AstMachineVReg vreg) {
  if (!fn)
    return 0;
  for (uint32_t b = 0u; b < fn->info.block_count; ++b) {
    const MachineBlock *block = &fn->blocks[b];
    for (uint32_t i = 0u; i < block->info.argument_count; ++i)
      if (block->arguments[i] == vreg)
        return 1;
  }
  return 0;
}

static int copy_candidate(const MachineFunction *fn,
                          const MachineInstruction *mi,
                          const uint8_t *blocked) {
  if (!fn || !mi || mi->info.source_op != AST_MIR_OP_COPY || mi->info.effects ||
      mi->info.flags || !mi->info.result_count ||
      mi->info.result_count != mi->info.operand_count ||
      mi->info.result_first == AST_MACHINE_INVALID_ID ||
      mi->info.result_count > fn->info.vreg_count ||
      mi->info.result_first > fn->info.vreg_count - mi->info.result_count)
    return 0;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    const AstMachineOperand *o = &mi->info.operands[part];
    AstMachineVReg dst = mi->info.result_first + part;
    if (!o || o->kind != AST_MACHINE_OPERAND_VREG ||
        o->as.vreg >= fn->info.vreg_count || blocked[dst] ||
        is_block_argument(fn, dst) || is_block_argument(fn, o->as.vreg) ||
        (fn->vregs[dst].flags & AST_MACHINE_VREG_FIXED_REGISTER) ||
        (o->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) ||
        !copy_pair_compatible(fn, dst, o->as.vreg))
      return 0;
  }
  return 1;
}

int machine_instruction_removable(const MachineFunction *fn,
                                  const MachineInstruction *mi) {
  if (!fn || !mi || !mi->info.result_count ||
      mi->info.source_instruction == AST_MIR_INVALID_ID ||
      mi->info.effects != 0u ||
      (mi->info.flags &
       (AST_MIR_INST_CONSTRAINT | AST_MIR_INST_CONSERVATIVE_EFFECTS)) ||
      (mi->info.source_op != AST_MIR_OP_CONSTANT &&
       mi->info.source_op != AST_MIR_OP_UNDEF &&
       mi->info.source_op != AST_MIR_OP_COPY &&
       mi->info.source_op != AST_MIR_OP_UNARY &&
       mi->info.source_op != AST_MIR_OP_BINARY &&
       mi->info.source_op != AST_MIR_OP_CAST &&
       mi->info.source_op != AST_MIR_OP_SELECT))
    return 0;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    AstMachineVReg v = mi->info.result_first + part;
    if (v >= fn->info.vreg_count ||
        (fn->vregs[v].flags &
         (AST_MACHINE_VREG_FIXED_REGISTER | AST_MACHINE_VREG_ABI_DEFERRED)))
      return 0;
  }
  for (uint32_t oi = 0u; oi < mi->info.operand_count; ++oi)
    if (mi->info.operands[oi].flags & AST_MACHINE_OPERAND_FIXED_REGISTER)
      return 0;
  return 1;
}

static int pure_dead_candidate(const MachineFunction *fn,
                               const MachineInstruction *mi,
                               const uint32_t *uses) {
  if (!machine_instruction_removable(fn, mi))
    return 0;
  for (uint32_t r = 0; r < mi->info.result_count; ++r)
    if (uses[mi->info.result_first + r])
      return 0;
  return 1;
}

static int optimize_function(MachineFunction *fn) {
  if (!fn || !fn->info.vreg_count || !fn->vregs || !fn->instructions)
    return 1;
  /* Retain the source identity map on repeated calls. */
  if (fn->vreg_alias)
    return 1;
  uint32_t n = fn->info.vreg_count;
  size_t use_bytes, temp_bytes;
  if (ast_size_mul_overflow((size_t)n, sizeof(uint32_t), &use_bytes) ||
      ast_size_add_overflow(use_bytes, (size_t)n, &temp_bytes) ||
      ast_size_add_overflow(temp_bytes,
                            (size_t)(fn->info.instruction_count
                                         ? fn->info.instruction_count
                                         : 1u),
                            &temp_bytes))
    return 0;
  uint8_t *temporary = (uint8_t *)calloc(temp_bytes ? temp_bytes : 1u, 1u);
  uint32_t *uses = (uint32_t *)temporary;
  uint8_t *blocked = temporary ? temporary + use_bytes : NULL;
  uint8_t *loop_copies = blocked ? blocked + n : NULL;
  AstMachineVReg *aliases =
      (AstMachineVReg *)malloc((size_t)n * sizeof(*aliases));
  uint32_t *definition_block = malloc(use_bytes);
  if (!temporary || !aliases || !definition_block) {
    free(temporary);
    free(aliases);
    free(definition_block);
    return 0;
  }
  for (uint32_t v = 0u; v < n; ++v) {
    aliases[v] = v;
    definition_block[v] = AST_MACHINE_INVALID_ID;
    if (fn->vregs[v].flags &
        (AST_MACHINE_VREG_FIXED_REGISTER | AST_MACHINE_VREG_ABI_DEFERRED))
      blocked[v] = 1u;
  }
  fn->vreg_alias = aliases;
  /* Propagate block-local copies in larger CFGs too. A copied destination
     used on another block/edge remains an allocator split boundary. Loop
     bodies and block parameters retain their edge-sensitive identities. */
  for (uint32_t b = 0u; b < fn->info.block_count; ++b) {
    const MachineBlock *mb = &fn->blocks[b];
    for (uint32_t j = 0u; j < mb->info.instruction_count; ++j) {
      uint32_t id = mb->instructions[j];
      if (id >= fn->info.instruction_count)
        continue;
      const MachineInstruction *mi = &fn->instructions[id];
      if (mb->info.loop_depth)
        loop_copies[id] = 1u;
      for (uint32_t r = 0u; r < mi->info.result_count; ++r)
        if (mi->info.result_first + r < n)
          definition_block[mi->info.result_first + r] = b;
    }
  }
  for (uint32_t b = 0u; b < fn->info.block_count; ++b) {
    const MachineBlock *mb = &fn->blocks[b];
    for (uint32_t j = 0u; j < mb->info.instruction_count; ++j) {
      uint32_t id = mb->instructions[j];
      if (id >= fn->info.instruction_count)
        continue;
      const MachineInstruction *mi = &fn->instructions[id];
      for (uint32_t o = 0u; o < mi->info.operand_count; ++o)
        if (mi->info.operands[o].kind == AST_MACHINE_OPERAND_VREG) {
          uint32_t v = mi->info.operands[o].as.vreg;
          if (v < n && definition_block[v] != AST_MACHINE_INVALID_ID &&
              definition_block[v] != b)
            blocked[v] = 1u;
        }
    }
  }
  free(definition_block);

  for (uint32_t i = 0u; i < fn->info.instruction_count; ++i) {
    const MachineInstruction *mi = &fn->instructions[i];
    for (uint32_t oi = 0u; oi < mi->info.operand_count; ++oi) {
      const AstMachineOperand *o = &mi->info.operands[oi];
      if (o->kind != AST_MACHINE_OPERAND_VREG || o->as.vreg >= n)
        continue;
      if (uses[o->as.vreg] != UINT32_MAX)
        ++uses[o->as.vreg];
      /* Some boundaries read original MIR identities, including
         indirect ABI transport that carries no fixed-register flag. */
      if ((o->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) ||
          mi->info.source_op == AST_MIR_OP_CALL_DIRECT ||
          mi->info.source_op == AST_MIR_OP_CALL_INDIRECT ||
          mi->info.source_op == AST_MIR_OP_BIND ||
          mi->info.source_op == AST_MIR_OP_NATIVE ||
          machine_opcode_terminator_kind(mi->info.opcode))
        blocked[o->as.vreg] = 1u;
    }
  }

  /* Build aliases in source order.  SSA guarantees that a copy source
     dominates its uses; canonical lookup also handles copy chains. */
  for (uint32_t i = 0u; i < fn->info.instruction_count; ++i) {
    const MachineInstruction *mi = &fn->instructions[i];
    if (loop_copies[i] || !copy_candidate(fn, mi, blocked))
      continue;
    AstMachineVReg source =
        machine_canonical_vreg(fn, mi->info.operands[0].as.vreg);
    if (source == mi->info.result_first)
      continue;
    int ok = 1;
    for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
      AstMachineVReg dst = mi->info.result_first + part;
      AstMachineVReg src =
          machine_canonical_vreg(fn, mi->info.operands[part].as.vreg);
      if (src == dst || !copy_pair_compatible(fn, dst, src)) {
        ok = 0;
        break;
      }
    }
    if (!ok)
      continue;
    for (uint32_t part = 0u; part < mi->info.result_count; ++part)
      aliases[mi->info.result_first + part] =
          machine_canonical_vreg(fn, mi->info.operands[part].as.vreg);
  }

  for (uint32_t v = 0u; v < n; ++v)
    aliases[v] = machine_canonical_vreg(fn, v);
  for (uint32_t i = 0u; i < fn->info.instruction_count; ++i) {
    MachineInstruction *mi = &fn->instructions[i];
    for (uint32_t oi = 0u; oi < mi->info.operand_count; ++oi) {
      AstMachineOperand *o = &fn->operands[mi->operand_offset + oi];
      if (o->kind == AST_MACHINE_OPERAND_VREG && !o->flags)
        o->as.vreg = machine_canonical_vreg(fn, o->as.vreg);
    }
  }
  machine_refresh_operand_views(fn);
  memset(uses, 0, (size_t)n * sizeof(*uses));
  for (uint32_t i = 0u; i < fn->info.instruction_count; ++i) {
    const MachineInstruction *mi = &fn->instructions[i];
    for (uint32_t oi = 0u; oi < mi->info.operand_count; ++oi) {
      const AstMachineOperand *o = &mi->info.operands[oi];
      if (o->kind == AST_MACHINE_OPERAND_VREG && o->as.vreg < n &&
          uses[o->as.vreg] != UINT32_MAX)
        ++uses[o->as.vreg];
    }
  }
  /* Remove full dead expression chains; live CFG edge operands count as uses.
   */
  int changed;
  do {
    changed = 0;
    for (uint32_t rev = fn->info.instruction_count; rev; --rev) {
      MachineInstruction *mi = &fn->instructions[rev - 1u];
      if (mi->dead || !pure_dead_candidate(fn, mi, uses))
        continue;
      mi->dead = 1u;
      changed = 1;
      for (uint32_t o = 0; o < mi->info.operand_count; ++o)
        if (mi->info.operands[o].kind == AST_MACHINE_OPERAND_VREG)
          --uses[mi->info.operands[o].as.vreg];
    }
  } while (changed);
  free(temporary);
  return 1;
}

static int peephole_task(void *opaque, size_t index) {
  AstMachineProgram *p = opaque;
  return optimize_function(&p->functions[index]);
}
int machine_run_peephole(AstMachineProgram *p) {
  if (!p || !p->functions)
    return 0;
  return ast_parallel_check(p->function_count, 0u, peephole_task, p, NULL);
}
