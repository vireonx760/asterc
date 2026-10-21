#include "machine_internal.h"

static int span_equal(AstSpan a, AstSpan b) {
  return a.offset == b.offset && a.file_id == b.file_id && a.length == b.length;
}

static int name_equal(AstName a, AstName b) {
  return a.text == b.text && a.hash == b.hash && span_equal(a.span, b.span);
}

static int verify_source_value(const MachineFunction *fn,
                               const MachineInstruction *mi, uint32_t source,
                               uint32_t *cursor) {
  if (source >= fn->source_vreg_count)
    return 0;
  for (uint32_t part = 0; part < fn->source_parts[source]; ++part) {
    if (*cursor >= mi->info.operand_count)
      return 0;
    const AstMachineOperand *op =
        &fn->operands[mi->operand_offset + (*cursor)++];
    if (op->kind != AST_MACHINE_OPERAND_VREG ||
        op->as.vreg !=
            machine_canonical_vreg(fn, fn->source_first[source] + part))
      return 0;
  }
  return 1;
}

/* These stages preserve MIR instructions one for one. Validate the source
   contract before using its types in the architecture's opcode mapping. */
static int verify_source_instruction(const AstMachineProgram *p, uint32_t fi,
                                     const MachineFunction *fn,
                                     const MachineInstruction *mi,
                                     const AstMirInstructionInfo *src) {
  AstTargetInstructionInfo ti;
  if (!ast_target_instruction_info(p->target, fi, src->id, &ti) ||
      mi->info.token_op != src->token_op ||
      mi->info.intrinsic != src->intrinsic ||
      mi->info.effects != ti.narrowed_effects || mi->info.flags != src->flags ||
      !span_equal(mi->info.span, src->span))
    return 0;
  if (src->result == AST_MIR_INVALID_ID) {
    if (mi->info.result_count ||
        mi->info.result_first != AST_MACHINE_INVALID_ID)
      return 0;
  } else if (src->result >= fn->source_vreg_count ||
             mi->info.result_first != fn->source_first[src->result] ||
             mi->info.result_count != fn->source_parts[src->result])
    return 0;
  uint32_t cursor = 0u;
  for (uint32_t o = 0; o < src->operand_count; ++o)
    if (!verify_source_value(fn, mi, src->operands[o], &cursor))
      return 0;
  if (src->op == AST_MIR_OP_STACK_ADDR || src->op == AST_MIR_OP_STACK_LOAD ||
      src->op == AST_MIR_OP_STACK_STORE) {
    if (cursor >= mi->info.operand_count)
      return 0;
    const AstMachineOperand *op = &fn->operands[mi->operand_offset + cursor++];
    if (op->kind != AST_MACHINE_OPERAND_STACK_SLOT ||
        op->as.stack_slot != src->stack_slot)
      return 0;
  }
  if ((src->op == AST_MIR_OP_CALL_DIRECT || src->op == AST_MIR_OP_GLOBAL_ADDR ||
       src->op == AST_MIR_OP_GLOBAL_LOAD ||
       src->op == AST_MIR_OP_GLOBAL_STORE ||
       src->op == AST_MIR_OP_FUNCTION_ADDR ||
       src->op == AST_MIR_OP_GENERIC_CONST) &&
      src->symbol.span.length) {
    if (cursor >= mi->info.operand_count)
      return 0;
    const AstMachineOperand *op = &fn->operands[mi->operand_offset + cursor++];
    if (op->kind != AST_MACHINE_OPERAND_SYMBOL ||
        !name_equal(op->as.symbol, src->symbol) ||
        !name_equal(op->module, src->module))
      return 0;
  }
  if (src->op == AST_MIR_OP_CONSTANT &&
      src->constant.kind != AST_SEMA_CONST_NONE) {
    if (cursor >= mi->info.operand_count)
      return 0;
    const AstMachineOperand *op = &fn->operands[mi->operand_offset + cursor++];
    uint64_t bits = src->constant.is_negative ? UINT64_C(0) - src->constant.lo
                                              : src->constant.lo;
    if (op->kind != AST_MACHINE_OPERAND_IMMEDIATE ||
        (uint64_t)op->as.immediate != bits)
      return 0;
  }
  if (cursor != mi->info.operand_count)
    return 0;
  AstMachineOpcode expected = machine_generic_opcode(src->op);
  if (p->stage == AST_MACHINE_STAGE_SELECTED) {
    MachineInstruction generic = *mi;
    generic.info.opcode = expected;
    AstTargetArch arch = ast_target_config(p->target).arch;
    expected =
        arch == AST_TARGET_ARCH_X86_64    ? machine_x86_opcode(fn, &generic)
        : arch == AST_TARGET_ARCH_AARCH64 ? machine_a64_opcode(fn, &generic)
        : arch == AST_TARGET_ARCH_RISCV64 ? machine_rv_opcode(fn, &generic)
                                          : AST_MACHINE_OP_INVALID;
  }
  return expected != AST_MACHINE_OP_INVALID && mi->info.opcode == expected;
}

static int verify_block_transport(const MachineFunction *fn,
                                  const MachineBlock *mb,
                                  const AstMirBlockInfo *source) {
  uint32_t cursor = 0u;
  if (source->instruction_count != mb->info.instruction_count - 1u ||
      source->reachable != mb->info.reachable ||
      source->loop_depth != mb->info.loop_depth)
    return 0;
  for (uint32_t i = 0; i < source->instruction_count; ++i)
    if (fn->instructions[mb->instructions[i]].info.source_instruction !=
        source->instructions[i])
      return 0;
  if (mb->info.argument_count && mb->info.arguments != mb->arguments)
    return 0;
  for (uint32_t a = 0; a < source->argument_count; ++a) {
    uint32_t v = source->arguments[a];
    if (v >= fn->source_vreg_count)
      return 0;
    for (uint32_t part = 0; part < fn->source_parts[v]; ++part) {
      if (cursor >= mb->info.argument_count ||
          mb->arguments[cursor++] != fn->source_first[v] + part)
        return 0;
    }
  }
  if (cursor != mb->info.argument_count || !mb->info.instruction_count)
    return 0;
  const MachineInstruction *term =
      &fn->instructions[mb->instructions[mb->info.instruction_count - 1u]];
  const AstMirTerminatorInfo *t = &source->terminator;
  if (machine_opcode_terminator_kind(term->info.opcode) != (int)t->kind ||
      term->info.source_instruction != AST_MIR_INVALID_ID ||
      term->info.source_op != AST_MIR_OP_INVALID || term->info.result_count ||
      term->info.result_first != AST_MACHINE_INVALID_ID ||
      term->info.token_op || term->info.intrinsic || term->info.effects ||
      term->info.flags || !span_equal(term->info.span, t->span))
    return 0;
  cursor = 0u;
  if (t->kind == AST_MIR_TERM_RETURN) {
    if (t->condition_or_value != AST_MIR_INVALID_ID &&
        !verify_source_value(fn, term, t->condition_or_value, &cursor))
      return 0;
    return cursor == term->info.operand_count;
  }
  if (t->kind == AST_MIR_TERM_UNREACHABLE)
    return term->info.operand_count == 0u;
  if (t->kind != AST_MIR_TERM_BR && t->kind != AST_MIR_TERM_COND_BR)
    return 0;
  if (t->kind == AST_MIR_TERM_COND_BR &&
      !verify_source_value(fn, term, t->condition_or_value, &cursor))
    return 0;
  uint32_t edges = t->kind == AST_MIR_TERM_COND_BR ? 2u : 1u;
  for (uint32_t edge = 0; edge < edges; ++edge) {
    const AstMirVReg *args = edge ? t->false_args : t->true_args;
    uint32_t count = edge ? t->false_arg_count : t->true_arg_count;
    uint32_t target = edge ? t->target_false : t->target_true;
    if (cursor >= term->info.operand_count)
      return 0;
    const AstMachineOperand *op =
        &fn->operands[term->operand_offset + cursor++];
    if (op->kind != AST_MACHINE_OPERAND_BLOCK || op->as.block != target)
      return 0;
    for (uint32_t a = 0; a < count; ++a) {
      if (args[a] >= fn->source_vreg_count)
        return 0;
      for (uint32_t part = 0; part < fn->source_parts[args[a]]; ++part) {
        if (cursor >= term->info.operand_count)
          return 0;
        op = &fn->operands[term->operand_offset + cursor++];
        if (op->kind != AST_MACHINE_OPERAND_VREG ||
            op->as.vreg != fn->source_first[args[a]] + part)
          return 0;
      }
    }
  }
  return cursor == term->info.operand_count;
}

static int bank_matches_class(AstMachineRegBank b, AstTargetRegClass rc) {
  AstMachineRegBank want = machine_bank_for_class(rc);
  return want == AST_MACHINE_BANK_INVALID || b == want;
}

static int verify_source_vregs(const AstMachineProgram *p, uint32_t fi,
                               const MachineFunction *fn,
                               const AstMirFunction *mf,
                               const AstMirFunctionInfo *mfi) {
  if (fn->source_vreg_count != mfi->vreg_count)
    return 0;
  uint32_t cursor = 0u;
  for (uint32_t source = 0; source < fn->source_vreg_count; ++source) {
    AstTargetValueInfo tv;
    AstMirVRegInfo mv;
    if (!ast_target_value_info(p->target, fi, source, &tv) ||
        !ast_mir_function_vreg(mf, source, &mv))
      return 0;
    uint32_t parts = tv.part_count ? tv.part_count : 1u;
    if (cursor > fn->info.vreg_count || parts > fn->info.vreg_count - cursor ||
        fn->source_first[source] != cursor || fn->source_parts[source] != parts)
      return 0;
    uint32_t bits = tv.legal_bit_width ? tv.legal_bit_width : mv.bit_width;
    if (!bits && mv.type) {
      bits = ast_sema_type_bit_width(mv.type);
      if (!bits) {
        uint64_t bytes = ast_sema_type_size(mv.type);
        if (bytes <= UINT32_MAX / 8u)
          bits = (uint32_t)bytes * 8u;
      }
    }
    uint32_t source_flags = (mv.flags & AST_MIR_VREG_FUNCTION_ARGUMENT
                                 ? AST_MACHINE_VREG_FUNCTION_ARGUMENT
                                 : 0u) |
                            (parts > 1u ? AST_MACHINE_VREG_SPLIT_PART : 0u);
    for (uint32_t part = 0; part < parts; ++part) {
      const AstMachineVRegInfo *v = &fn->vregs[cursor + part];
      if (v->source_vreg != source || v->part_index != part ||
          v->part_count != parts || v->type != mv.type ||
          v->reg_class != tv.reg_class || v->bit_width != bits ||
          (v->flags & (AST_MACHINE_VREG_FUNCTION_ARGUMENT |
                       AST_MACHINE_VREG_SPLIT_PART)) != source_flags)
        return 0;
    }
    cursor += parts;
  }
  return cursor == fn->info.vreg_count;
}

static int abi_value_compatible(const AstMachineProgram *p,
                                const MachineFunction *fn,
                                const AstAbiValueInfo *av, uint32_t first,
                                uint32_t parts) {
  if (!p || !fn || !av || first == AST_MACHINE_INVALID_ID || parts == 0u ||
      first >= fn->info.vreg_count || parts > fn->info.vreg_count - first)
    return 0;
  if (av->location.kind != AST_ABI_LOCATION_REGISTER &&
      av->location.kind != AST_ABI_LOCATION_REGISTER_LIST)
    return 0;
  if (av->location.register_count != parts)
    return 0;
  AstTargetConfig tc = ast_target_config(p->target);
  for (uint32_t i = 0; i < parts; ++i) {
    AstMachineRegBank expected =
        machine_bank_for_class(fn->vregs[first + i].reg_class);
    if (expected == AST_MACHINE_BANK_INVALID ||
        !machine_register_compatible(&tc, av->location.registers[i], expected))
      return 0;
  }
  return 1;
}

static int verify_vreg_constraint(const AstMachineProgram *p,
                                  const MachineFunction *fn,
                                  const AstAbiValueInfo *av) {
  if (!av || av->source_vreg == AST_MIR_INVALID_ID ||
      av->source_vreg >= fn->source_vreg_count)
    return 1;
  uint32_t first = fn->source_first[av->source_vreg],
           parts = fn->source_parts[av->source_vreg];
  if (first == AST_MACHINE_INVALID_ID || !parts ||
      first >= fn->info.vreg_count || parts > fn->info.vreg_count - first)
    return 0;
  int compatible = abi_value_compatible(p, fn, av, first, parts);
  for (uint32_t i = 0; i < parts; ++i) {
    const AstMachineVRegInfo *v = &fn->vregs[first + i];
    if (compatible) {
      if (!(v->flags & AST_MACHINE_VREG_FIXED_REGISTER) ||
          strcmp(v->fixed_register, av->location.registers[i]) != 0)
        return 0;
    } else {
      if (!(v->flags & AST_MACHINE_VREG_ABI_DEFERRED) ||
          (v->flags & AST_MACHINE_VREG_FIXED_REGISTER) || v->fixed_register[0])
        return 0;
    }
  }
  return 1;
}

static const MachineInstruction *
find_source_instruction(const MachineFunction *fn, AstMirInstId source) {
  if (!fn)
    return NULL;
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i)
    if (fn->instructions[i].info.source_instruction == source)
      return &fn->instructions[i];
  return NULL;
}

static int verify_operand_fixed(const AstMachineOperand *o, const char *reg) {
  return o && o->kind == AST_MACHINE_OPERAND_VREG &&
         (o->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) && reg &&
         strcmp(o->fixed_register, reg) == 0;
}

static int verify_call_constraints(const AstMachineProgram *p,
                                   size_t call_index,
                                   const AstAbiCallInfo *ci) {
  if (!p || !ci || ci->caller_function_index >= p->function_count)
    return 0;
  const MachineFunction *fn = &p->functions[ci->caller_function_index];
  const MachineInstruction *machine =
      find_source_instruction(fn, ci->instruction);
  const AstMirFunction *mf =
      ast_mir_function_at(p->mir, ci->caller_function_index);
  AstMirInstructionInfo mir;
  if (!machine || !mf ||
      !ast_mir_function_instruction(mf, ci->instruction, &mir) ||
      (mir.op != AST_MIR_OP_CALL_DIRECT && mir.op != AST_MIR_OP_CALL_INDIRECT))
    return 0;
  uint32_t hidden = 0u;
  AstAbiValueInfo first_av;
  if (ast_abi_call_argument(p->abi, call_index, 0u, &first_av) &&
      first_av.source_vreg == AST_MIR_INVALID_ID &&
      (first_av.flags & AST_ABI_VALUE_HIDDEN))
    hidden = 1u;
  uint32_t source_first = mir.op == AST_MIR_OP_CALL_INDIRECT ? 1u : 0u;
  uint32_t operand_cursor = 0u;
  if (source_first) {
    if (!mir.operand_count || mir.operands[0] >= fn->source_vreg_count)
      return 0;
    operand_cursor += fn->source_parts[mir.operands[0]];
  }
  for (uint32_t ai = 0; ai < ci->argument_count; ++ai) {
    AstAbiValueInfo av;
    if (!ast_abi_call_argument(p->abi, call_index, hidden + ai, &av) ||
        source_first + ai >= mir.operand_count ||
        av.source_vreg != mir.operands[source_first + ai] ||
        av.source_vreg >= fn->source_vreg_count)
      return 0;
    uint32_t first = fn->source_first[av.source_vreg],
             parts = fn->source_parts[av.source_vreg];
    if (operand_cursor > machine->info.operand_count ||
        parts > machine->info.operand_count - operand_cursor)
      return 0;
    int compatible = abi_value_compatible(p, fn, &av, first, parts);
    for (uint32_t pi = 0; pi < parts; ++pi) {
      const AstMachineOperand *o = &machine->info.operands[operand_cursor + pi];
      if (compatible) {
        if (!verify_operand_fixed(o, av.location.registers[pi]))
          return 0;
      } else if ((o->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) ||
                 o->fixed_register[0])
        return 0;
    }
    operand_cursor += parts;
  }
  if (ci->result.source_vreg != AST_MIR_INVALID_ID &&
      !verify_vreg_constraint(p, fn, &ci->result))
    return 0;
  return 1;
}

static int verify_return_constraints(const AstMachineProgram *p,
                                     size_t function_index,
                                     const AstAbiFunctionInfo *afi) {
  if (!p || !afi || function_index >= p->function_count)
    return 0;
  const MachineFunction *fn = &p->functions[function_index];
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i) {
    const MachineInstruction *mi = &fn->instructions[i];
    if (machine_opcode_terminator_kind(mi->info.opcode) !=
            AST_MIR_TERM_RETURN ||
        mi->info.operand_count == 0u)
      continue;
    int compatible =
        (afi->result.location.kind == AST_ABI_LOCATION_REGISTER ||
         afi->result.location.kind == AST_ABI_LOCATION_REGISTER_LIST) &&
        afi->result.location.register_count == mi->info.operand_count;
    AstTargetConfig tc = ast_target_config(p->target);
    if (compatible) {
      for (uint32_t oi = 0; oi < mi->info.operand_count; ++oi) {
        const AstMachineOperand *o = &mi->info.operands[oi];
        if (o->kind != AST_MACHINE_OPERAND_VREG ||
            o->as.vreg >= fn->info.vreg_count) {
          compatible = 0;
          break;
        }
        AstMachineRegBank expected =
            machine_bank_for_class(fn->vregs[o->as.vreg].reg_class);
        if (!machine_register_compatible(
                &tc, afi->result.location.registers[oi], expected)) {
          compatible = 0;
          break;
        }
      }
    }
    for (uint32_t oi = 0; oi < mi->info.operand_count; ++oi) {
      const AstMachineOperand *o = &mi->info.operands[oi];
      if (compatible) {
        if (!verify_operand_fixed(o, afi->result.location.registers[oi]))
          return 0;
      } else if ((o->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) ||
                 o->fixed_register[0])
        return 0;
    }
  }
  return 1;
}

static int verify_abi_constraints(const AstMachineProgram *p) {
  if (!p)
    return 0;
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    AstAbiFunctionInfo afi;
    if (!ast_abi_function_info(p->abi, fi, &afi))
      return 0;
    uint32_t total = afi.hidden_argument_count + afi.argument_count;
    for (uint32_t ai = 0; ai < total; ++ai) {
      AstAbiValueInfo av;
      if (!ast_abi_function_argument(p->abi, fi, ai, &av))
        return 0;
      if (!(av.flags & AST_ABI_VALUE_HIDDEN) &&
          !verify_vreg_constraint(p, &p->functions[fi], &av))
        return 0;
    }
    if (!verify_return_constraints(p, fi, &afi))
      return 0;
  }
  for (size_t ci = 0; ci < ast_abi_call_count(p->abi); ++ci) {
    AstAbiCallInfo info;
    if (!ast_abi_call_info(p->abi, ci, &info) ||
        !verify_call_constraints(p, ci, &info))
      return 0;
  }
  return 1;
}

/* Independently replay the locality proof from the original MIR identities,
   before trusting an alias map that has already rewritten Machine uses. */
static int verify_copy_scope(const MachineFunction *fn,
                             const AstMirFunction *mf, uint32_t v) {
  AstMirVRegInfo original;
  AstMirFunctionInfo info;
  uint32_t source = fn->vregs[v].source_vreg;
  if (!ast_mir_function_vreg(mf, source, &original) ||
      original.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
      !ast_mir_function_info(mf, &info))
    return 0;
  for (uint32_t b = 0; b < info.block_count; ++b) {
    AstMirBlockInfo block;
    if (!ast_mir_function_block(mf, b, &block) ||
        (b == original.block && block.loop_depth))
      return 0;
    const AstMirTerminatorInfo *term = &block.terminator;
    if (term->condition_or_value == source)
      return 0;
    for (uint32_t a = 0; a < term->true_arg_count; ++a)
      if (term->true_args[a] == source)
        return 0;
    for (uint32_t a = 0; a < term->false_arg_count; ++a)
      if (term->false_args[a] == source)
        return 0;
    for (uint32_t j = 0; j < block.instruction_count; ++j) {
      AstMirInstructionInfo inst;
      if (!ast_mir_function_instruction(mf, block.instructions[j], &inst))
        return 0;
      uint32_t cursor = 0u;
      for (uint32_t o = 0; o < inst.operand_count; ++o) {
        uint32_t value = inst.operands[o];
        if (value >= fn->source_vreg_count)
          return 0;
        if (value == source) {
          if (b != original.block || inst.op == AST_MIR_OP_CALL_DIRECT ||
              inst.op == AST_MIR_OP_CALL_INDIRECT ||
              inst.op == AST_MIR_OP_BIND || inst.op == AST_MIR_OP_NATIVE)
            return 0;
          const MachineInstruction *mi = NULL;
          if (inst.id < fn->info.instruction_count &&
              fn->instructions[inst.id].info.source_instruction == inst.id)
            mi = &fn->instructions[inst.id];
          else
            for (uint32_t k = 0; k < fn->info.instruction_count; ++k)
              if (fn->instructions[k].info.source_instruction == inst.id) {
                mi = &fn->instructions[k];
                break;
              }
          if (!mi || cursor > mi->info.operand_count ||
              fn->source_parts[value] > mi->info.operand_count - cursor)
            return 0;
          for (uint32_t part = 0; part < fn->source_parts[value]; ++part)
            if (mi->info.operands[cursor + part].flags &
                AST_MACHINE_OPERAND_FIXED_REGISTER)
              return 0;
        }
        cursor += fn->source_parts[value];
      }
    }
  }
  return 1;
}

/* Check the optimization proof against immutable source MIR. In particular,
   forged aliases must not make a forged operand look source-equivalent. */
static int verify_optimizations(const AstMachineProgram *p,
                                const MachineFunction *fn,
                                const AstMirFunction *mf) {
  uint32_t n = fn->info.vreg_count;
  uint8_t *live = (uint8_t *)calloc(n ? n : 1u, 1u);
  uint8_t *aliased = (uint8_t *)calloc(n ? n : 1u, 1u);
  int ok = 0;
  if (!live || !aliased)
    goto done;
  if (fn->vreg_alias)
    for (uint32_t v = 0; v < n; ++v) {
      uint32_t a = fn->vreg_alias[v];
      if (a >= n || fn->vreg_alias[a] != a)
        goto done;
      if (a != v) {
        if (!p->config.enable_peephole || !verify_copy_scope(fn, mf, v) ||
            (fn->vregs[v].flags &
             (AST_MACHINE_VREG_FIXED_REGISTER | AST_MACHINE_VREG_ABI_DEFERRED |
              AST_MACHINE_VREG_FUNCTION_ARGUMENT)) ||
            fn->vregs[v].type != fn->vregs[a].type ||
            fn->vregs[v].part_index != fn->vregs[a].part_index ||
            fn->vregs[v].bit_width != fn->vregs[a].bit_width ||
            fn->vregs[v].reg_class != fn->vregs[a].reg_class)
          goto done;
        aliased[v] = 1u;
      }
    }
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i) {
    const MachineInstruction *mi = &fn->instructions[i];
    if (mi->dead > 1u || (mi->dead && (!p->config.enable_peephole ||
                                       !machine_instruction_removable(fn, mi))))
      goto done;
    if (!mi->dead)
      for (uint32_t o = 0; o < mi->info.operand_count; ++o)
        if (mi->info.operands[o].kind == AST_MACHINE_OPERAND_VREG)
          live[mi->info.operands[o].as.vreg] = 1u;
    for (uint32_t r = 0; r < mi->info.result_count; ++r) {
      uint32_t v = mi->info.result_first + r;
      if (!aliased[v])
        continue;
      AstMirInstructionInfo src;
      if (mi->info.source_op != AST_MIR_OP_COPY || mi->info.effects ||
          mi->info.flags ||
          !ast_mir_function_instruction(mf, mi->info.source_instruction,
                                        &src) ||
          src.op != AST_MIR_OP_COPY || src.operand_count != 1u ||
          src.operands[0] >= fn->source_vreg_count ||
          r >= fn->source_parts[src.operands[0]])
        goto done;
      uint32_t source = fn->source_first[src.operands[0]] + r;
      if (source == v ||
          machine_canonical_vreg(fn, source) != fn->vreg_alias[v])
        goto done;
      aliased[v] = 0u;
    }
  }
  for (uint32_t v = 0; v < n; ++v)
    if (aliased[v])
      goto done;
  for (uint32_t i = 0; i < fn->info.instruction_count; ++i)
    if (fn->instructions[i].dead) {
      const MachineInstruction *mi = &fn->instructions[i];
      for (uint32_t r = 0; r < mi->info.result_count; ++r)
        if (live[mi->info.result_first + r])
          goto done;
    }
  ok = 1;
done:
  free(live);
  free(aliased);
  return ok;
}

int ast_machine_verify(const AstMachineProgram *p) {
  if (!p || !p->abi || !p->target || !p->mir || !p->functions)
    return 0;
  if (p->stage < AST_MACHINE_STAGE_GENERIC ||
      p->stage > AST_MACHINE_STAGE_SELECTED)
    return 0;
  if (p->function_count != ast_mir_function_count(p->mir) ||
      p->stats.function_count != p->function_count)
    return 0;
  AstTargetConfig tc = ast_target_config(p->target);
  size_t blocks = 0, insts = 0, vregs = 0, fixed_regs = 0, fixed_ops = 0;
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    const MachineFunction *fn = &p->functions[fi];
    AstMirFunctionInfo mfi;
    const AstMirFunction *mf = ast_mir_function_at(p->mir, fi);
    if (!mf || !ast_mir_function_info(mf, &mfi))
      return 0;
    if (!machine_cfg_validate(fn))
      return 0;
    if (fn->info.function_index != fi ||
        fn->info.block_count != mfi.block_count ||
        fn->info.entry_block != mfi.entry_block ||
        (fn->info.block_count && !fn->blocks) ||
        (fn->info.instruction_count && !fn->instructions) ||
        (fn->info.vreg_count && !fn->vregs) || !fn->source_first ||
        !fn->source_parts)
      return 0;
    if (!verify_source_vregs(p, (uint32_t)fi, fn, mf, &mfi))
      return 0;
    for (uint32_t v = 0; v < fn->info.vreg_count; ++v) {
      const AstMachineVRegInfo *vi = &fn->vregs[v];
      if (vi->id != v || vi->source_vreg >= fn->source_vreg_count ||
          !vi->type || vi->part_count == 0 || vi->part_index >= vi->part_count)
        return 0;
      if (!memchr(vi->fixed_register, 0, sizeof(vi->fixed_register)) ||
          (vi->flags & ~(uint32_t)(AST_MACHINE_VREG_FUNCTION_ARGUMENT |
                                   AST_MACHINE_VREG_FIXED_REGISTER |
                                   AST_MACHINE_VREG_SPLIT_PART |
                                   AST_MACHINE_VREG_ABI_DEFERRED)))
        return 0;
      if (vi->bank > AST_MACHINE_BANK_SPECIAL ||
          vi->bank < AST_MACHINE_BANK_INVALID ||
          (p->stage == AST_MACHINE_STAGE_GENERIC &&
           vi->bank != AST_MACHINE_BANK_INVALID))
        return 0;
      if (p->stage >= AST_MACHINE_STAGE_REGBANK &&
          (vi->bank == AST_MACHINE_BANK_INVALID ||
           !bank_matches_class(vi->bank, vi->reg_class)))
        return 0;
      if ((vi->flags & AST_MACHINE_VREG_FIXED_REGISTER) != 0u) {
        AstMachineRegBank expected =
            p->stage >= AST_MACHINE_STAGE_REGBANK
                ? vi->bank
                : machine_bank_for_class(vi->reg_class);
        if (!vi->fixed_register[0] ||
            !machine_register_compatible(&tc, vi->fixed_register, expected))
          return 0;
        fixed_regs++;
      } else if (vi->fixed_register[0])
        return 0;
    }
    for (uint32_t b = 0; b < fn->info.block_count; ++b) {
      const MachineBlock *mb = &fn->blocks[b];
      if (mb->info.id != b || !mb->instructions ||
          mb->info.instructions != mb->instructions ||
          mb->info.instruction_count == 0)
        return 0;
      AstMirBlockInfo source;
      if (!ast_mir_function_block(mf, b, &source) ||
          !verify_block_transport(fn, mb, &source))
        return 0;
      for (uint32_t j = 0; j < mb->info.instruction_count; ++j)
        if (mb->instructions[j] >= fn->info.instruction_count)
          return 0;
    }
    for (uint32_t i = 0; i < fn->info.instruction_count; ++i) {
      const MachineInstruction *mi = &fn->instructions[i];
      if (mi->info.id != i)
        return 0;
      if (mi->info.operand_count) {
        if (mi->info.operands != fn->operands + mi->operand_offset)
          return 0;
      } else if (mi->info.operands)
        return 0;
      if (mi->info.source_instruction != AST_MIR_INVALID_ID) {
        AstMirInstructionInfo src;
        if (!ast_mir_function_instruction(mf, mi->info.source_instruction,
                                          &src) ||
            mi->info.source_op != src.op ||
            mi->info.memory_order != src.memory_order ||
            mi->info.failure_memory_order != src.failure_memory_order)
          return 0;
        if (!verify_source_instruction(p, (uint32_t)fi, fn, mi, &src))
          return 0;
        if (src.op == AST_MIR_OP_ATOMIC) {
          if (mi->info.memory_order < AST_MEMORY_ORDER_RELAXED ||
              mi->info.memory_order > AST_MEMORY_ORDER_SEQ_CST)
            return 0;
          if (mi->info.failure_memory_order == AST_MEMORY_ORDER_RELEASE ||
              mi->info.failure_memory_order == AST_MEMORY_ORDER_ACQ_REL ||
              mi->info.failure_memory_order > AST_MEMORY_ORDER_SEQ_CST)
            return 0;
        } else if (src.op == AST_MIR_OP_FENCE) {
          if (mi->info.memory_order < AST_MEMORY_ORDER_RELAXED ||
              mi->info.memory_order > AST_MEMORY_ORDER_SEQ_CST ||
              mi->info.failure_memory_order != AST_MEMORY_ORDER_NONE)
            return 0;
        } else if (mi->info.memory_order != AST_MEMORY_ORDER_NONE ||
                   mi->info.failure_memory_order != AST_MEMORY_ORDER_NONE)
          return 0;
      } else if (!machine_opcode_terminator_kind(mi->info.opcode) ||
                 mi->info.memory_order != AST_MEMORY_ORDER_NONE ||
                 mi->info.failure_memory_order != AST_MEMORY_ORDER_NONE)
        return 0;
      if (mi->info.operand_count) {
        if (!fn->operands || mi->operand_offset > fn->operand_count ||
            mi->info.operand_count > fn->operand_count - mi->operand_offset ||
            mi->info.operands != fn->operands + mi->operand_offset)
          return 0;
      } else if (mi->info.operands != NULL)
        return 0;
      if (p->stage == AST_MACHINE_STAGE_SELECTED) {
        if (!machine_opcode_is_selected(tc.arch, mi->info.opcode))
          return 0;
      } else {
        if (!machine_opcode_is_generic(mi->info.opcode))
          return 0;
      }
      if (mi->info.result_count) {
        if (mi->info.result_first == AST_MACHINE_INVALID_ID ||
            mi->info.result_first >= fn->info.vreg_count ||
            mi->info.result_count > fn->info.vreg_count - mi->info.result_first)
          return 0;
      }
      for (uint32_t oi = 0; oi < mi->info.operand_count; ++oi) {
        const AstMachineOperand *o = &mi->info.operands[oi];
        if (o->kind < AST_MACHINE_OPERAND_VREG ||
            o->kind > AST_MACHINE_OPERAND_SYMBOL ||
            (o->flags & ~(uint32_t)AST_MACHINE_OPERAND_FIXED_REGISTER) ||
            !memchr(o->fixed_register, 0, sizeof(o->fixed_register)))
          return 0;
        if (o->kind == AST_MACHINE_OPERAND_VREG &&
            o->as.vreg >= fn->info.vreg_count)
          return 0;
        if (o->kind == AST_MACHINE_OPERAND_BLOCK &&
            o->as.block >= fn->info.block_count)
          return 0;
        if (o->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) {
          if (o->kind != AST_MACHINE_OPERAND_VREG ||
              o->as.vreg >= fn->info.vreg_count || !o->fixed_register[0])
            return 0;
          AstMachineRegBank expected =
              p->stage >= AST_MACHINE_STAGE_REGBANK
                  ? fn->vregs[o->as.vreg].bank
                  : machine_bank_for_class(fn->vregs[o->as.vreg].reg_class);
          if (!machine_register_compatible(&tc, o->fixed_register, expected))
            return 0;
          fixed_ops++;
        } else if (o->fixed_register[0])
          return 0;
      }
    }
    if (!verify_optimizations(p, fn, mf))
      return 0;
    blocks += fn->info.block_count;
    insts += fn->info.instruction_count;
    vregs += fn->info.vreg_count;
  }
  if (blocks != p->stats.block_count || insts != p->stats.instruction_count ||
      vregs != p->stats.vreg_count ||
      fixed_regs != p->stats.fixed_register_count ||
      fixed_ops != p->stats.fixed_operand_count)
    return 0;
  return verify_abi_constraints(p);
}
