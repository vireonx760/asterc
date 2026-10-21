#include "../sema/sema_internal.h"
#include "mir_internal.h"

AstMirTermKind mir_term_from_hir(AstHirTermKind kind) {
  switch (kind) {
  case AST_HIR_TERM_BR:
    return AST_MIR_TERM_BR;
  case AST_HIR_TERM_COND_BR:
    return AST_MIR_TERM_COND_BR;
  case AST_HIR_TERM_RETURN:
    return AST_MIR_TERM_RETURN;
  case AST_HIR_TERM_UNREACHABLE:
    return AST_MIR_TERM_UNREACHABLE;
  case AST_HIR_TERM_NONE:
  default:
    return AST_MIR_TERM_NONE;
  }
}

AstMirOp mir_op_from_hir(AstHirOp op) {
  switch (op) {
  case AST_HIR_OP_CONSTANT:
    return AST_MIR_OP_CONSTANT;
  case AST_HIR_OP_LOCAL_LOAD:
    return AST_MIR_OP_STACK_LOAD;
  case AST_HIR_OP_LOCAL_STORE:
    return AST_MIR_OP_STACK_STORE;
  case AST_HIR_OP_LOCAL_ADDR:
    return AST_MIR_OP_STACK_ADDR;
  case AST_HIR_OP_GLOBAL_ADDR:
    return AST_MIR_OP_GLOBAL_ADDR;
  case AST_HIR_OP_GLOBAL_LOAD:
    return AST_MIR_OP_GLOBAL_LOAD;
  case AST_HIR_OP_GLOBAL_STORE:
    return AST_MIR_OP_GLOBAL_STORE;
  case AST_HIR_OP_FUNCTION_REF:
    return AST_MIR_OP_FUNCTION_ADDR;
  case AST_HIR_OP_GENERIC_CONST:
    return AST_MIR_OP_GENERIC_CONST;
  case AST_HIR_OP_UNARY:
    return AST_MIR_OP_UNARY;
  case AST_HIR_OP_BINARY:
    return AST_MIR_OP_BINARY;
  case AST_HIR_OP_CAST:
    return AST_MIR_OP_CAST;
  case AST_HIR_OP_SELECT:
    return AST_MIR_OP_SELECT;
  case AST_HIR_OP_CALL:
    return AST_MIR_OP_CALL_INDIRECT;
  case AST_HIR_OP_INDEX:
    return AST_MIR_OP_INDEX;
  case AST_HIR_OP_MEMBER:
    return AST_MIR_OP_MEMBER;
  case AST_HIR_OP_ADDRESS_OF:
    return AST_MIR_OP_ADDRESS_OF;
  case AST_HIR_OP_LOAD:
    return AST_MIR_OP_LOAD;
  case AST_HIR_OP_STORE:
    return AST_MIR_OP_STORE;
  case AST_HIR_OP_ATOMIC:
    return AST_MIR_OP_ATOMIC;
  case AST_HIR_OP_FENCE:
    return AST_MIR_OP_FENCE;
  case AST_HIR_OP_INTRINSIC:
    return AST_MIR_OP_INTRINSIC;
  case AST_HIR_OP_BIND:
    return AST_MIR_OP_BIND;
  case AST_HIR_OP_NATIVE:
    return AST_MIR_OP_NATIVE;
  case AST_HIR_OP_NATIVE_RESULT:
    return AST_MIR_OP_NATIVE_RESULT;
  case AST_HIR_OP_UNDEF:
    return AST_MIR_OP_UNDEF;
  case AST_HIR_OP_INVALID:
  default:
    return AST_MIR_OP_INVALID;
  }
}

static const AstHirInstruction *
mir_hir_value_definition(const AstHirFunction *function, AstHirValueId value) {
  if (!function || value >= function->value_count)
    return NULL;
  const AstHirValue *info = &function->values[value];
  if (info->kind != AST_HIR_VALUE_INSTRUCTION_RESULT ||
      info->definition_index >= function->instruction_count)
    return NULL;
  return &function->instructions[info->definition_index];
}

static const AstHirInstruction *
mir_hir_direct_call_target(const AstHirFunction *function,
                           const AstHirInstruction *call) {
  if (!function || !call || call->op != AST_HIR_OP_CALL ||
      call->operand_count == 0u)
    return NULL;
  const AstHirInstruction *target =
      mir_hir_value_definition(function, call->operands[0]);
  return target && target->op == AST_HIR_OP_FUNCTION_REF ? target : NULL;
}

static int mir_prepare_hir_uses(MirLowerContext *context) {
  const AstHirFunction *hir = context->hir;
  if (!hir || hir->value_count == 0u)
    return 1;
  /* Calls without parameters or local variables still have function refs.
   * This scan must not depend on local-slot preparation. */
  for (uint32_t i = 0u; i < hir->instruction_count; ++i)
    if (hir->instructions[i].op == AST_HIR_OP_FUNCTION_REF) {
      context->has_function_refs = 1u;
      break;
    }
  if (!context->has_function_refs)
    return 1;
  size_t count = 0u, bytes = 0u;
  if (ast_size_mul_overflow(hir->value_count, 2u, &count) ||
      ast_size_mul_overflow(count, sizeof(uint32_t), &bytes)) {
    context->function->oom = 1u;
    return 0;
  }
  context->hir_value_use_count = (uint32_t *)calloc(1u, bytes);
  if (!context->hir_value_use_count) {
    context->function->oom = 1u;
    return 0;
  }
  context->hir_value_direct_call_count =
      context->hir_value_use_count + hir->value_count;
  for (uint32_t ii = 0u; ii < hir->instruction_count; ++ii) {
    const AstHirInstruction *inst = &hir->instructions[ii];
    for (uint32_t oi = 0u; oi < inst->operand_count; ++oi) {
      AstHirValueId value = inst->operands[oi];
      if (value >= hir->value_count ||
          context->hir_value_use_count[value] == UINT32_MAX)
        return 0;
      context->hir_value_use_count[value]++;
    }
    if (mir_hir_direct_call_target(hir, inst)) {
      AstHirValueId callee = inst->operands[0];
      if (context->hir_value_direct_call_count[callee] == UINT32_MAX)
        return 0;
      context->hir_value_direct_call_count[callee]++;
    }
  }
  for (uint32_t bi = 0u; bi < hir->block_count; ++bi) {
    const AstHirTerminator *term = &hir->blocks[bi].term;
    AstHirValueId value = term->condition_or_value;
    if (value != AST_HIR_INVALID_ID) {
      if (value >= hir->value_count ||
          context->hir_value_use_count[value] == UINT32_MAX)
        return 0;
      context->hir_value_use_count[value]++;
    }
    for (uint32_t ai = 0u; ai < term->true_arg_count; ++ai) {
      value = term->true_args[ai];
      if (value >= hir->value_count ||
          context->hir_value_use_count[value] == UINT32_MAX)
        return 0;
      context->hir_value_use_count[value]++;
    }
    for (uint32_t ai = 0u; ai < term->false_arg_count; ++ai) {
      value = term->false_args[ai];
      if (value >= hir->value_count ||
          context->hir_value_use_count[value] == UINT32_MAX)
        return 0;
      context->hir_value_use_count[value]++;
    }
  }
  return 1;
}

static int mir_function_ref_is_direct_only(const MirLowerContext *context,
                                           const AstHirInstruction *source) {
  if (!context || !source || source->op != AST_HIR_OP_FUNCTION_REF ||
      source->result == AST_HIR_INVALID_ID ||
      source->result >= context->hir->value_count ||
      !context->hir_value_use_count || !context->hir_value_direct_call_count)
    return 0;
  uint32_t direct = context->hir_value_direct_call_count[source->result];
  return direct != 0u && context->hir_value_use_count[source->result] == direct;
}

static size_t mir_local_index(const MirLowerContext *context,
                              AstMirBlockId block, AstHirLocalId local) {
  uint32_t promoted = context->local_promoted_index[local];
  return (size_t)block * context->promoted_local_count + promoted;
}

static int mir_add_stack_slot(MirLowerContext *context, AstHirLocalId local,
                              const AstHirLocal *source) {
  AstMirFunction *fn = context->function;
  size_t capacity = fn->stack_slot_capacity;
  if (!mir_reserve((void **)&fn->stack_slots, sizeof(*fn->stack_slots),
                   &capacity, (size_t)fn->stack_slot_count + 1u)) {
    fn->oom = 1u;
    return 0;
  }
  if (capacity > UINT32_MAX) {
    fn->oom = 1u;
    return 0;
  }
  fn->stack_slot_capacity = (uint32_t)capacity;
  AstMirStackSlotId id = fn->stack_slot_count++;
  AstMirStackSlotInfo *slot = &fn->stack_slots[id].info;
  memset(slot, 0, sizeof(*slot));
  slot->id = id;
  slot->source_local = local;
  slot->name = mir_copy_name(&fn->arena, source->name);
  slot->type = source->type;
  slot->span = source->span;
  slot->size = ast_sema_type_size(source->type);
  slot->alignment = ast_sema_type_alignment(source->type);
  if (slot->alignment == 0u)
    slot->alignment = 1u;
  if (source->flags & AST_HIR_LOCAL_MUTABLE)
    slot->flags |= AST_MIR_STACK_SLOT_MUTABLE;
  if (source->flags & AST_HIR_LOCAL_PARAMETER)
    slot->flags |= AST_MIR_STACK_SLOT_PARAMETER;
  if (context->local_address_taken[local])
    slot->flags |= AST_MIR_STACK_SLOT_ADDRESS_TAKEN;
  if (slot->size == AST_MIR_UNKNOWN_SIZE)
    slot->flags |= AST_MIR_STACK_SLOT_DYNAMIC_SIZE;
  context->local_slots[local] = id;
  if (source->name.span.length && !slot->name.text) {
    fn->oom = 1u;
    return 0;
  }
  return 1;
}

static int mir_append_preliminary_terminator(AstMirFunction *fn,
                                             AstMirBlockId block_id,
                                             const AstHirTerminator *source) {
  AstMirTerminator term;
  memset(&term, 0, sizeof(term));
  term.info.kind = mir_term_from_hir(source->kind);
  term.info.span = source->span;
  term.info.condition_or_value = AST_MIR_INVALID_ID;
  term.info.target_true = source->target_true;
  term.info.target_false = source->target_false;
  term.info.memory = AST_MIR_INVALID_ID;
  return mir_set_terminator(fn, block_id, term, NULL, 0u, NULL, 0u);
}

static AstMirVReg mir_create_local_phi(MirLowerContext *context,
                                       AstMirBlockId block_id,
                                       AstHirLocalId local) {
  size_t index = mir_local_index(context, block_id, local);
  if (context->local_phis[index] != AST_MIR_INVALID_ID)
    return context->local_phis[index];
  if (!context->hir || local >= context->hir->local_count) {
    mir_function_diag(context->function, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                      context->function->blocks[block_id].span,
                      "cannot read HIR local %u while constructing a MIR phi",
                      local);
    return AST_MIR_INVALID_ID;
  }
  const AstHirLocal *source = &context->hir->locals[local];
  AstMirBlock *block = &context->function->blocks[block_id];
  AstMirVReg vreg = mir_add_vreg(context->function, AST_MIR_VREG_BLOCK_ARGUMENT,
                                 source->type, source->span, block_id,
                                 block->argument_count, AST_MIR_VREG_PHI);
  if (vreg == AST_MIR_INVALID_ID ||
      !mir_append_block_argument(context->function, block_id, vreg, local))
    return AST_MIR_INVALID_ID;
  context->local_phis[index] = vreg;
  context->local_defs[index] = vreg;
  context->function->phi_count++;
  return vreg;
}

static AstMirVReg mir_emit_local_undef(MirLowerContext *context,
                                       AstMirBlockId block_id,
                                       AstHirLocalId local) {
  if (!context->hir || local >= context->hir->local_count)
    return AST_MIR_INVALID_ID;
  const AstHirLocal *source = &context->hir->locals[local];
  AstMirInstruction inst;
  memset(&inst, 0, sizeof(inst));
  inst.info.op = AST_MIR_OP_UNDEF;
  inst.info.type = source->type;
  inst.info.span = source->span;
  inst.info.stack_slot = AST_MIR_INVALID_ID;
  inst.info.source_local = local;
  inst.info.memory_in = AST_MIR_INVALID_ID;
  inst.info.memory_out = AST_MIR_INVALID_ID;
  inst.info.source_hir_instruction = AST_HIR_INVALID_ID;
  AstMirInstId id = context->function->instruction_count;
  AstMirVReg result = mir_add_vreg(
      context->function, AST_MIR_VREG_INSTRUCTION_RESULT, source->type,
      source->span, block_id, id, AST_MIR_VREG_UNDEFINED);
  if (result == AST_MIR_INVALID_ID)
    return result;
  inst.info.result = result;
  if (mir_emit_instruction(context->function, block_id, inst, NULL, 0u) ==
      AST_MIR_INVALID_ID)
    return AST_MIR_INVALID_ID;
  return result;
}

static AstMirVReg mir_read_local_end(MirLowerContext *context,
                                     AstMirBlockId block_id,
                                     AstHirLocalId local);

static AstMirVReg mir_read_local_entry(MirLowerContext *context,
                                       AstMirBlockId block_id,
                                       AstHirLocalId local) {
  size_t index = mir_local_index(context, block_id, local);
  if (context->local_defs[index] != AST_MIR_INVALID_ID)
    return context->local_defs[index];
  if (context->local_reading[index])
    return mir_create_local_phi(context, block_id, local);
  context->local_reading[index] = 1u;
  AstMirBlock *block = &context->function->blocks[block_id];
  AstMirVReg value = AST_MIR_INVALID_ID;
  if (block->predecessor_count == 0u) {
    value = mir_emit_local_undef(context, block_id, local);
  } else if (block->predecessor_count == 1u) {
    value = mir_read_local_end(context, block->predecessors[0], local);
  } else {
    value = mir_create_local_phi(context, block_id, local);
  }
  if (context->local_defs[index] == AST_MIR_INVALID_ID)
    context->local_defs[index] = value;
  context->local_reading[index] = 0u;
  return context->local_defs[index];
}

static AstMirVReg mir_read_local_end(MirLowerContext *context,
                                     AstMirBlockId block_id,
                                     AstHirLocalId local) {
  size_t index = mir_local_index(context, block_id, local);
  if (context->local_defs[index] != AST_MIR_INVALID_ID)
    return context->local_defs[index];
  return mir_read_local_entry(context, block_id, local);
}

static int mir_map_hir_operands(MirLowerContext *context,
                                const AstHirInstruction *source,
                                uint32_t first_operand, AstMirVReg *small,
                                uint32_t small_capacity,
                                AstMirVReg **operands_out,
                                uint32_t *operand_count_out) {
  *operands_out = NULL;
  *operand_count_out = 0u;
  if (first_operand > source->operand_count)
    return 0;
  uint32_t count = source->operand_count - first_operand;
  if (count == 0u)
    return 1;
  AstMirVReg *operands = small;
  if (count > small_capacity) {
    size_t bytes = 0u;
    if (ast_size_mul_overflow(count, sizeof(AstMirVReg), &bytes))
      return 0;
    operands = (AstMirVReg *)malloc(bytes);
    if (!operands)
      return 0;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    AstHirValueId value = source->operands[first_operand + i];
    if (value >= context->hir->value_count ||
        context->hir_values[value] == AST_MIR_INVALID_ID) {
      if (operands != small)
        free(operands);
      mir_function_diag(context->function, AST_MIR_DIAG_INVALID_VREG,
                        AST_DIAG_ERROR, source->span,
                        "HIR operand %u has no lowered MIR virtual register",
                        value);
      return 0;
    }
    operands[i] = context->hir_values[value];
  }
  *operands_out = operands;
  *operand_count_out = count;
  return 1;
}

static int mir_copy_instruction_names(AstMirFunction *fn,
                                      AstMirInstruction *inst,
                                      const AstHirInstruction *source) {
  inst->info.symbol = mir_copy_name(&fn->arena, source->symbol);
  inst->info.module = mir_copy_name(&fn->arena, source->module);
  if ((source->symbol.span.length && !inst->info.symbol.text) ||
      (source->module.span.length && !inst->info.module.text)) {
    fn->oom = 1u;
    return 0;
  }
  return 1;
}

/* Portable scalar core intrinsics are deliberately expanded in generic MIR
 * when their semantics can be expressed without target state.  Keeping these
 * expansions here gives x86-64, AArch64 and RV64 exactly the same edge-case
 * behaviour and prevents selected *_INTRINSIC pseudos from leaking into the
 * physical backends. */
static AstMirVReg
mir_emit_synthetic(MirLowerContext *context, AstMirBlockId block_id,
                   AstHirInstId source_id, AstMirOp op, const AstSemaType *type,
                   AstTokenKind token_op, AstHirIntrinsicKind intrinsic,
                   AstSemaConstInfo constant, const AstMirVReg *operands,
                   uint32_t operand_count, AstSpan span) {
  AstMirFunction *fn = context->function;
  (void)source_id;
  AstMirInstruction inst;
  memset(&inst, 0, sizeof(inst));
  inst.info.op = op;
  inst.info.result = AST_MIR_INVALID_ID;
  inst.info.type = type;
  inst.info.span = span;
  inst.info.effects = 0u;
  inst.info.flags = mir_instruction_flags(0u, op) | AST_MIR_INST_SYNTHETIC;
  inst.info.token_op = token_op;
  inst.info.intrinsic = intrinsic;
  inst.info.stack_slot = AST_MIR_INVALID_ID;
  inst.info.source_local = AST_HIR_INVALID_ID;
  inst.info.native_region = AST_MIR_INVALID_ID;
  inst.info.constant = constant;
  inst.info.memory_in = AST_MIR_INVALID_ID;
  inst.info.memory_out = AST_MIR_INVALID_ID;
  /* This instruction is part of a one-to-many expansion.  It deliberately
   * has no one-to-one HIR provenance; the verifier recognizes the source
   * portable intrinsic as an eliminated high-level operation. */
  inst.info.source_hir_instruction = AST_HIR_INVALID_ID;
  AstMirInstId iid = fn->instruction_count;
  AstMirVReg result = mir_add_vreg(fn, AST_MIR_VREG_INSTRUCTION_RESULT, type,
                                   span, block_id, iid, 0u);
  if (result == AST_MIR_INVALID_ID)
    return AST_MIR_INVALID_ID;
  inst.info.result = result;
  if (mir_emit_instruction(fn, block_id, inst, operands, operand_count) ==
      AST_MIR_INVALID_ID)
    return AST_MIR_INVALID_ID;
  return result;
}

static AstMirVReg mir_emit_int_constant(MirLowerContext *context,
                                        AstMirBlockId block_id,
                                        AstHirInstId source_id,
                                        const AstSemaType *type, uint64_t bits,
                                        AstSpan span) {
  AstSemaConstInfo ci;
  memset(&ci, 0, sizeof(ci));
  ci.kind = AST_SEMA_CONST_INTEGER;
  ci.lo = bits;
  ci.is_signed = ast_sema_type_is_signed(type) ? 1u : 0u;
  return mir_emit_synthetic(context, block_id, source_id, AST_MIR_OP_CONSTANT,
                            type, AST_TOK_INVALID, AST_HIR_INTRINSIC_NONE, ci,
                            NULL, 0u, span);
}

static AstMirVReg mir_emit_binary(MirLowerContext *context,
                                  AstMirBlockId block_id,
                                  AstHirInstId source_id,
                                  const AstSemaType *type, AstTokenKind token,
                                  AstMirVReg a, AstMirVReg b, AstSpan span) {
  AstMirVReg ops[2] = {a, b};
  AstSemaConstInfo ci = {0};
  return mir_emit_synthetic(context, block_id, source_id, AST_MIR_OP_BINARY,
                            type, token, AST_HIR_INTRINSIC_NONE, ci, ops, 2u,
                            span);
}

static AstMirVReg mir_emit_cast(MirLowerContext *context,
                                AstMirBlockId block_id, AstHirInstId source_id,
                                const AstSemaType *type, AstMirVReg value,
                                AstSpan span) {
  AstMirVReg ops[1] = {value};
  AstSemaConstInfo ci = {0};
  return mir_emit_synthetic(context, block_id, source_id, AST_MIR_OP_CAST, type,
                            AST_TOK_KW_CAST, AST_HIR_INTRINSIC_NONE, ci, ops,
                            1u, span);
}

static AstMirVReg mir_emit_select(MirLowerContext *context,
                                  AstMirBlockId block_id,
                                  AstHirInstId source_id,
                                  const AstSemaType *type, AstMirVReg cond,
                                  AstMirVReg a, AstMirVReg b, AstSpan span) {
  AstMirVReg ops[3] = {cond, a, b};
  AstSemaConstInfo ci = {0};
  return mir_emit_synthetic(context, block_id, source_id, AST_MIR_OP_SELECT,
                            type, AST_TOK_INVALID, AST_HIR_INTRINSIC_NONE, ci,
                            ops, 3u, span);
}

static AstMirVReg mir_emit_pack(MirLowerContext *context,
                                AstMirBlockId block_id, AstHirInstId source_id,
                                const AstSemaType *type,
                                const AstMirVReg *values, uint32_t value_count,
                                AstSpan span) {
  AstSemaConstInfo ci = {0};
  return mir_emit_synthetic(context, block_id, source_id, AST_MIR_OP_PACK, type,
                            AST_TOK_INVALID, AST_HIR_INTRINSIC_NONE, ci, values,
                            value_count, span);
}

static const AstSemaType *mir_unsigned_type_for_width(MirLowerContext *context,
                                                      uint32_t width) {
  if (!context || !context->program || !context->program->hir ||
      !context->program->hir->sema)
    return NULL;
  AstTokenKind token = AST_TOK_INVALID;
  switch (width) {
  case 8u:
    token = AST_TOK_KW_U8;
    break;
  case 16u:
    token = AST_TOK_KW_U16;
    break;
  case 32u:
    token = AST_TOK_KW_U32;
    break;
  case 64u:
    token = AST_TOK_KW_U64;
    break;
  default:
    return NULL;
  }
  return sema_builtin_for_token((AstSema *)context->program->hir->sema, token);
}

static const AstSemaType *mir_bool_type(MirLowerContext *context) {
  if (!context || !context->program || !context->program->hir ||
      !context->program->hir->sema)
    return NULL;
  return sema_builtin_bool((AstSema *)context->program->hir->sema);
}

static AstMirVReg
mir_expand_popcnt_unsigned(MirLowerContext *context, AstMirBlockId block_id,
                           AstHirInstId source_id, const AstSemaType *ut,
                           AstMirVReg value, uint32_t width, AstSpan span) {
  /* Hacker's Delight SWAR popcount, valid for 8/16/32/64-bit unsigned values.
   */
  uint64_t width_mask =
      width == 64u ? UINT64_MAX : ((UINT64_C(1) << width) - 1u);
  uint64_t m1 = UINT64_C(0x5555555555555555) & width_mask;
  uint64_t m2 = UINT64_C(0x3333333333333333) & width_mask;
  uint64_t m4 = UINT64_C(0x0f0f0f0f0f0f0f0f) & width_mask;
  AstMirVReg one =
      mir_emit_int_constant(context, block_id, source_id, ut, 1u, span);
  AstMirVReg two =
      mir_emit_int_constant(context, block_id, source_id, ut, 2u, span);
  AstMirVReg four =
      mir_emit_int_constant(context, block_id, source_id, ut, 4u, span);
  AstMirVReg cm1 =
      mir_emit_int_constant(context, block_id, source_id, ut, m1, span);
  AstMirVReg cm2 =
      mir_emit_int_constant(context, block_id, source_id, ut, m2, span);
  AstMirVReg cm4 =
      mir_emit_int_constant(context, block_id, source_id, ut, m4, span);
  if (one == AST_MIR_INVALID_ID || two == AST_MIR_INVALID_ID ||
      four == AST_MIR_INVALID_ID || cm1 == AST_MIR_INVALID_ID ||
      cm2 == AST_MIR_INVALID_ID || cm4 == AST_MIR_INVALID_ID)
    return AST_MIR_INVALID_ID;
  AstMirVReg t = mir_emit_binary(context, block_id, source_id, ut,
                                 AST_TOK_SHIFT_RIGHT, value, one, span);
  t = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP, t, cm1,
                      span);
  AstMirVReg x = mir_emit_binary(context, block_id, source_id, ut,
                                 AST_TOK_MINUS, value, t, span);
  AstMirVReg a = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP,
                                 x, cm2, span);
  t = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_RIGHT, x,
                      two, span);
  t = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP, t, cm2,
                      span);
  x = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_PLUS, a, t,
                      span);
  t = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_RIGHT, x,
                      four, span);
  x = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_PLUS, x, t,
                      span);
  x = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP, x, cm4,
                      span);
  for (uint32_t shift = 8u; shift < width; shift <<= 1u) {
    AstMirVReg cs =
        mir_emit_int_constant(context, block_id, source_id, ut, shift, span);
    t = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_RIGHT,
                        x, cs, span);
    x = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_PLUS, x, t,
                        span);
    if (cs == AST_MIR_INVALID_ID || t == AST_MIR_INVALID_ID ||
        x == AST_MIR_INVALID_ID)
      return AST_MIR_INVALID_ID;
  }
  uint64_t final_mask =
      width == 64u ? 0x7fu
                   : (width == 32u ? 0x3fu : (width == 16u ? 0x1fu : 0x0fu));
  AstMirVReg cm =
      mir_emit_int_constant(context, block_id, source_id, ut, final_mask, span);
  return mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP, x, cm,
                         span);
}

static AstMirVReg mir_expand_bit_intrinsic(MirLowerContext *context,
                                           AstMirBlockId block_id,
                                           AstHirInstId source_id,
                                           const AstHirInstruction *source,
                                           AstMirVReg *operands,
                                           uint32_t operand_count) {
  uint32_t width = ast_sema_type_bit_width(source->type);
  if (width != 8u && width != 16u && width != 32u && width != 64u)
    return AST_MIR_INVALID_ID;
  const AstSemaType *ut = mir_unsigned_type_for_width(context, width);
  if (!ut || operand_count < 1u)
    return AST_MIR_INVALID_ID;
  AstMirVReg x = operands[0];
  if (ast_sema_type_is_signed(source->type)) {
    x = mir_emit_cast(context, block_id, source_id, ut, x, source->span);
    if (x == AST_MIR_INVALID_ID)
      return x;
  }
  AstMirVReg result = AST_MIR_INVALID_ID;
  if (source->intrinsic == AST_HIR_INTRINSIC_POPCNT) {
    result = mir_expand_popcnt_unsigned(context, block_id, source_id, ut, x,
                                        width, source->span);
  } else if (source->intrinsic == AST_HIR_INTRINSIC_CTZ) {
    AstMirVReg zero = mir_emit_int_constant(context, block_id, source_id, ut,
                                            0u, source->span);
    AstMirVReg one = mir_emit_int_constant(context, block_id, source_id, ut, 1u,
                                           source->span);
    AstMirVReg neg = mir_emit_binary(context, block_id, source_id, ut,
                                     AST_TOK_MINUS, zero, x, source->span);
    AstMirVReg low = mir_emit_binary(context, block_id, source_id, ut,
                                     AST_TOK_AMP, x, neg, source->span);
    AstMirVReg mask = mir_emit_binary(context, block_id, source_id, ut,
                                      AST_TOK_MINUS, low, one, source->span);
    result = mir_expand_popcnt_unsigned(context, block_id, source_id, ut, mask,
                                        width, source->span);
  } else if (source->intrinsic == AST_HIR_INTRINSIC_CLZ) {
    AstMirVReg y = x;
    for (uint32_t shift = 1u; shift < width; shift <<= 1u) {
      AstMirVReg cs = mir_emit_int_constant(context, block_id, source_id, ut,
                                            shift, source->span);
      AstMirVReg q = mir_emit_binary(context, block_id, source_id, ut,
                                     AST_TOK_SHIFT_RIGHT, y, cs, source->span);
      y = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_PIPE, y, q,
                          source->span);
      if (cs == AST_MIR_INVALID_ID || q == AST_MIR_INVALID_ID ||
          y == AST_MIR_INVALID_ID)
        return AST_MIR_INVALID_ID;
    }
    AstMirVReg pc = mir_expand_popcnt_unsigned(context, block_id, source_id, ut,
                                               y, width, source->span);
    AstMirVReg cw = mir_emit_int_constant(context, block_id, source_id, ut,
                                          width, source->span);
    result = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_MINUS,
                             cw, pc, source->span);
  } else if (source->intrinsic == AST_HIR_INTRINSIC_BITREV) {
    static const uint64_t masks[] = {
        UINT64_C(0x5555555555555555), UINT64_C(0x3333333333333333),
        UINT64_C(0x0f0f0f0f0f0f0f0f), UINT64_C(0x00ff00ff00ff00ff),
        UINT64_C(0x0000ffff0000ffff), UINT64_C(0x00000000ffffffff)};
    uint64_t width_mask =
        width == 64u ? UINT64_MAX : ((UINT64_C(1) << width) - 1u);
    AstMirVReg y = x;
    for (uint32_t stage = 0u, shift = 1u; shift < width;
         ++stage, shift <<= 1u) {
      AstMirVReg cm =
          mir_emit_int_constant(context, block_id, source_id, ut,
                                masks[stage] & width_mask, source->span);
      AstMirVReg cs = mir_emit_int_constant(context, block_id, source_id, ut,
                                            shift, source->span);
      AstMirVReg lo = mir_emit_binary(context, block_id, source_id, ut,
                                      AST_TOK_AMP, y, cm, source->span);
      lo = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_LEFT,
                           lo, cs, source->span);
      AstMirVReg hi = mir_emit_binary(context, block_id, source_id, ut,
                                      AST_TOK_SHIFT_RIGHT, y, cs, source->span);
      hi = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP, hi,
                           cm, source->span);
      y = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_PIPE, lo,
                          hi, source->span);
      if (y == AST_MIR_INVALID_ID)
        return y;
    }
    result = y;
  } else if (source->intrinsic == AST_HIR_INTRINSIC_BSWAP) {
    AstMirVReg y = x;
    static const uint64_t masks[] = {UINT64_C(0x00ff00ff00ff00ff),
                                     UINT64_C(0x0000ffff0000ffff),
                                     UINT64_C(0x00000000ffffffff)};
    uint64_t width_mask =
        width == 64u ? UINT64_MAX : ((UINT64_C(1) << width) - 1u);
    for (uint32_t stage = 0u, shift = 8u; shift < width;
         ++stage, shift <<= 1u) {
      AstMirVReg cm =
          mir_emit_int_constant(context, block_id, source_id, ut,
                                masks[stage] & width_mask, source->span);
      AstMirVReg cs = mir_emit_int_constant(context, block_id, source_id, ut,
                                            shift, source->span);
      AstMirVReg lo = mir_emit_binary(context, block_id, source_id, ut,
                                      AST_TOK_AMP, y, cm, source->span);
      lo = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_LEFT,
                           lo, cs, source->span);
      AstMirVReg hi = mir_emit_binary(context, block_id, source_id, ut,
                                      AST_TOK_SHIFT_RIGHT, y, cs, source->span);
      hi = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP, hi,
                           cm, source->span);
      y = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_PIPE, lo,
                          hi, source->span);
      if (y == AST_MIR_INVALID_ID)
        return y;
    }
    result = y;
  } else if (source->intrinsic == AST_HIR_INTRINSIC_ROL ||
             source->intrinsic == AST_HIR_INTRINSIC_ROR) {
    if (operand_count != 2u)
      return AST_MIR_INVALID_ID;
    AstMirVReg count = operands[1];
    count =
        mir_emit_cast(context, block_id, source_id, ut, count, source->span);
    AstMirVReg mask = mir_emit_int_constant(context, block_id, source_id, ut,
                                            width - 1u, source->span);
    count = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP,
                            count, mask, source->span);
    AstMirVReg w = mir_emit_int_constant(context, block_id, source_id, ut,
                                         width, source->span);
    AstMirVReg inv = mir_emit_binary(context, block_id, source_id, ut,
                                     AST_TOK_MINUS, w, count, source->span);
    inv = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_AMP, inv,
                          mask, source->span);
    AstMirVReg a, b;
    if (source->intrinsic == AST_HIR_INTRINSIC_ROL) {
      a = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_LEFT,
                          x, count, source->span);
      b = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_RIGHT,
                          x, inv, source->span);
    } else {
      a = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_RIGHT,
                          x, count, source->span);
      b = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SHIFT_LEFT,
                          x, inv, source->span);
    }
    result = mir_emit_binary(context, block_id, source_id, ut, AST_TOK_PIPE, a,
                             b, source->span);
  }
  if (result == AST_MIR_INVALID_ID)
    return result;
  if (ast_sema_type_is_signed(source->type))
    result = mir_emit_cast(context, block_id, source_id, source->type, result,
                           source->span);
  return result;
}

static AstMirVReg mir_expand_sat_intrinsic(MirLowerContext *context,
                                           AstMirBlockId block_id,
                                           AstHirInstId source_id,
                                           const AstHirInstruction *source,
                                           AstMirVReg *operands,
                                           uint32_t operand_count) {
  if (operand_count != 2u ||
      ast_sema_type_kind(source->type) != AST_SEMA_TYPE_INT)
    return AST_MIR_INVALID_ID;
  uint32_t width = ast_sema_type_bit_width(source->type);
  if (width == 0u || width > 64u)
    return AST_MIR_INVALID_ID;
  const AstSemaType *bt = mir_bool_type(context);
  if (!bt)
    return AST_MIR_INVALID_ID;
  AstMirVReg a = operands[0], b = operands[1];
  AstTokenKind arith = source->intrinsic == AST_HIR_INTRINSIC_ADD_SAT
                           ? AST_TOK_PLUS
                           : AST_TOK_MINUS;
  AstMirVReg wrapped = mir_emit_binary(context, block_id, source_id,
                                       source->type, arith, a, b, source->span);
  if (wrapped == AST_MIR_INVALID_ID)
    return wrapped;
  AstMirVReg overflow;
  if (!ast_sema_type_is_signed(source->type)) {
    overflow = mir_emit_binary(
        context, block_id, source_id, bt, AST_TOK_LT,
        source->intrinsic == AST_HIR_INTRINSIC_ADD_SAT ? wrapped : a,
        source->intrinsic == AST_HIR_INTRINSIC_ADD_SAT ? a : b, source->span);
    AstMirVReg sat = mir_emit_int_constant(
        context, block_id, source_id, source->type,
        source->intrinsic == AST_HIR_INTRINSIC_ADD_SAT
            ? (width == 64u ? UINT64_MAX : ((UINT64_C(1) << width) - 1u))
            : 0u,
        source->span);
    return mir_emit_select(context, block_id, source_id, source->type, overflow,
                           sat, wrapped, source->span);
  }
  AstMirVReg x1, x2;
  if (source->intrinsic == AST_HIR_INTRINSIC_ADD_SAT) {
    x1 = mir_emit_binary(context, block_id, source_id, source->type,
                         AST_TOK_CARET, wrapped, a, source->span);
    x2 = mir_emit_binary(context, block_id, source_id, source->type,
                         AST_TOK_CARET, wrapped, b, source->span);
  } else {
    x1 = mir_emit_binary(context, block_id, source_id, source->type,
                         AST_TOK_CARET, a, b, source->span);
    x2 = mir_emit_binary(context, block_id, source_id, source->type,
                         AST_TOK_CARET, wrapped, a, source->span);
  }
  AstMirVReg ovbits =
      mir_emit_binary(context, block_id, source_id, source->type, AST_TOK_AMP,
                      x1, x2, source->span);
  AstMirVReg zero = mir_emit_int_constant(context, block_id, source_id,
                                          source->type, 0u, source->span);
  overflow = mir_emit_binary(context, block_id, source_id, bt, AST_TOK_LT,
                             ovbits, zero, source->span);
  AstMirVReg neg_a = mir_emit_binary(context, block_id, source_id, bt,
                                     AST_TOK_LT, a, zero, source->span);
  uint64_t min_bits = UINT64_C(1) << (width - 1u);
  uint64_t max_bits = min_bits - 1u;
  AstSemaConstInfo min_ci = {0};
  min_ci.kind = AST_SEMA_CONST_INTEGER;
  min_ci.lo = min_bits;
  min_ci.is_negative = 1u;
  min_ci.is_signed = 1u;
  AstMirVReg minv = mir_emit_synthetic(
      context, block_id, source_id, AST_MIR_OP_CONSTANT, source->type,
      AST_TOK_INVALID, AST_HIR_INTRINSIC_NONE, min_ci, NULL, 0u, source->span);
  AstMirVReg maxv = mir_emit_int_constant(context, block_id, source_id,
                                          source->type, max_bits, source->span);
  AstMirVReg sat = mir_emit_select(context, block_id, source_id, source->type,
                                   neg_a, minv, maxv, source->span);
  return mir_emit_select(context, block_id, source_id, source->type, overflow,
                         sat, wrapped, source->span);
}

static AstMirVReg mir_expand_checked_intrinsic(MirLowerContext *context,
                                               AstMirBlockId block_id,
                                               AstHirInstId source_id,
                                               const AstHirInstruction *source,
                                               AstMirVReg *operands,
                                               uint32_t operand_count) {
  if (operand_count != 2u ||
      ast_sema_type_kind(source->type) != AST_SEMA_TYPE_TUPLE ||
      ast_sema_type_tuple_count(source->type) != 2u)
    return AST_MIR_INVALID_ID;
  const AstSemaType *value_type = ast_sema_type_tuple_item(source->type, 0u);
  const AstSemaType *overflow_type = ast_sema_type_tuple_item(source->type, 1u);
  if (!value_type || !overflow_type ||
      ast_sema_type_kind(overflow_type) != AST_SEMA_TYPE_BOOL ||
      ast_sema_type_kind(value_type) != AST_SEMA_TYPE_INT)
    return AST_MIR_INVALID_ID;
  uint32_t width = ast_sema_type_bit_width(value_type);
  if (width != 8u && width != 16u && width != 32u && width != 64u)
    return AST_MIR_INVALID_ID;
  AstMirVReg a = operands[0], b = operands[1];
  AstTokenKind arith =
      source->intrinsic == AST_HIR_INTRINSIC_ADD_CHECKED   ? AST_TOK_PLUS
      : source->intrinsic == AST_HIR_INTRINSIC_SUB_CHECKED ? AST_TOK_MINUS
                                                           : AST_TOK_STAR;
  AstMirVReg wrapped = mir_emit_binary(context, block_id, source_id, value_type,
                                       arith, a, b, source->span);
  if (wrapped == AST_MIR_INVALID_ID)
    return wrapped;
  AstMirVReg overflow = AST_MIR_INVALID_ID;
  int sign = ast_sema_type_is_signed(value_type);

  if (source->intrinsic == AST_HIR_INTRINSIC_ADD_CHECKED ||
      source->intrinsic == AST_HIR_INTRINSIC_SUB_CHECKED) {
    if (!sign) {
      overflow = mir_emit_binary(
          context, block_id, source_id, overflow_type, AST_TOK_LT,
          source->intrinsic == AST_HIR_INTRINSIC_ADD_CHECKED ? wrapped : a,
          source->intrinsic == AST_HIR_INTRINSIC_ADD_CHECKED ? a : b,
          source->span);
    } else {
      AstMirVReg x1, x2;
      if (source->intrinsic == AST_HIR_INTRINSIC_ADD_CHECKED) {
        x1 = mir_emit_binary(context, block_id, source_id, value_type,
                             AST_TOK_CARET, wrapped, a, source->span);
        x2 = mir_emit_binary(context, block_id, source_id, value_type,
                             AST_TOK_CARET, wrapped, b, source->span);
      } else {
        x1 = mir_emit_binary(context, block_id, source_id, value_type,
                             AST_TOK_CARET, a, b, source->span);
        x2 = mir_emit_binary(context, block_id, source_id, value_type,
                             AST_TOK_CARET, wrapped, a, source->span);
      }
      AstMirVReg bits =
          mir_emit_binary(context, block_id, source_id, value_type, AST_TOK_AMP,
                          x1, x2, source->span);
      AstMirVReg zero = mir_emit_int_constant(context, block_id, source_id,
                                              value_type, 0u, source->span);
      overflow = mir_emit_binary(context, block_id, source_id, overflow_type,
                                 AST_TOK_LT, bits, zero, source->span);
    }
  } else if (source->intrinsic == AST_HIR_INTRINSIC_MUL_CHECKED) {
    const AstSemaType *ut = mir_unsigned_type_for_width(context, width);
    if (!ut)
      return AST_MIR_INVALID_ID;
    AstMirVReg ua = a, ub = b;
    if (sign) {
      ua = mir_emit_cast(context, block_id, source_id, ut, a, source->span);
      ub = mir_emit_cast(context, block_id, source_id, ut, b, source->span);
    }
    AstMirVReg uzero = mir_emit_int_constant(context, block_id, source_id, ut,
                                             0u, source->span);
    AstMirVReg uone = mir_emit_int_constant(context, block_id, source_id, ut,
                                            1u, source->span);
    if (uzero == AST_MIR_INVALID_ID || uone == AST_MIR_INVALID_ID)
      return AST_MIR_INVALID_ID;
    AstMirVReg magnitude_a = ua, magnitude_b = ub, limit;
    if (sign) {
      AstMirVReg szero = mir_emit_int_constant(context, block_id, source_id,
                                               value_type, 0u, source->span);
      AstMirVReg neg_a =
          mir_emit_binary(context, block_id, source_id, overflow_type,
                          AST_TOK_LT, a, szero, source->span);
      AstMirVReg neg_b =
          mir_emit_binary(context, block_id, source_id, overflow_type,
                          AST_TOK_LT, b, szero, source->span);
      AstMirVReg ua_neg =
          mir_emit_binary(context, block_id, source_id, ut, AST_TOK_MINUS,
                          uzero, ua, source->span);
      AstMirVReg ub_neg =
          mir_emit_binary(context, block_id, source_id, ut, AST_TOK_MINUS,
                          uzero, ub, source->span);
      magnitude_a = mir_emit_select(context, block_id, source_id, ut, neg_a,
                                    ua_neg, ua, source->span);
      magnitude_b = mir_emit_select(context, block_id, source_id, ut, neg_b,
                                    ub_neg, ub, source->span);
      AstMirVReg different_sign =
          mir_emit_binary(context, block_id, source_id, overflow_type,
                          AST_TOK_BANG_EQ, neg_a, neg_b, source->span);
      uint64_t neg_limit_bits = UINT64_C(1) << (width - 1u);
      uint64_t pos_limit_bits = neg_limit_bits - 1u;
      AstMirVReg neg_limit = mir_emit_int_constant(
          context, block_id, source_id, ut, neg_limit_bits, source->span);
      AstMirVReg pos_limit = mir_emit_int_constant(
          context, block_id, source_id, ut, pos_limit_bits, source->span);
      limit = mir_emit_select(context, block_id, source_id, ut, different_sign,
                              neg_limit, pos_limit, source->span);
    } else {
      uint64_t max_bits =
          width == 64u ? UINT64_MAX : ((UINT64_C(1) << width) - 1u);
      limit = mir_emit_int_constant(context, block_id, source_id, ut, max_bits,
                                    source->span);
    }
    AstMirVReg a_is_zero =
        mir_emit_binary(context, block_id, source_id, overflow_type,
                        AST_TOK_EQ_EQ, magnitude_a, uzero, source->span);
    AstMirVReg safe_a =
        mir_emit_select(context, block_id, source_id, ut, a_is_zero, uone,
                        magnitude_a, source->span);
    AstMirVReg quotient =
        mir_emit_binary(context, block_id, source_id, ut, AST_TOK_SLASH, limit,
                        safe_a, source->span);
    overflow = mir_emit_binary(context, block_id, source_id, overflow_type,
                               AST_TOK_GT, magnitude_b, quotient, source->span);
  }
  if (overflow == AST_MIR_INVALID_ID)
    return overflow;
  AstMirVReg packed[2] = {wrapped, overflow};
  return mir_emit_pack(context, block_id, source_id, source->type, packed, 2u,
                       source->span);
}

static int mir_lower_regular_instruction(
    MirLowerContext *context, AstMirBlockId block_id, AstHirInstId source_id,
    const AstHirInstruction *source, AstMirVReg *operands,
    uint32_t operand_count, const AstHirInstruction *direct_target) {
  AstMirFunction *fn = context->function;

  if (source->op == AST_HIR_OP_INTRINSIC &&
      source->result != AST_HIR_INVALID_ID) {
    AstMirVReg expanded = AST_MIR_INVALID_ID;
    switch (source->intrinsic) {
    case AST_HIR_INTRINSIC_CLZ:
    case AST_HIR_INTRINSIC_CTZ:
    case AST_HIR_INTRINSIC_POPCNT:
    case AST_HIR_INTRINSIC_BSWAP:
    case AST_HIR_INTRINSIC_BITREV:
    case AST_HIR_INTRINSIC_ROL:
    case AST_HIR_INTRINSIC_ROR:
      expanded = mir_expand_bit_intrinsic(context, block_id, source_id, source,
                                          operands, operand_count);
      break;
    case AST_HIR_INTRINSIC_ADD_SAT:
    case AST_HIR_INTRINSIC_SUB_SAT:
      expanded = mir_expand_sat_intrinsic(context, block_id, source_id, source,
                                          operands, operand_count);
      break;
    case AST_HIR_INTRINSIC_ADD_CHECKED:
    case AST_HIR_INTRINSIC_SUB_CHECKED:
    case AST_HIR_INTRINSIC_MUL_CHECKED:
      expanded = mir_expand_checked_intrinsic(context, block_id, source_id,
                                              source, operands, operand_count);
      break;
    default:
      break;
    }
    if (expanded != AST_MIR_INVALID_ID) {
      if (source->result >= context->hir->value_count)
        return 0;
      context->hir_values[source->result] = expanded;
      return 1;
    }
  }

  AstMirInstruction inst;
  memset(&inst, 0, sizeof(inst));
  inst.info.op =
      direct_target ? AST_MIR_OP_CALL_DIRECT : mir_op_from_hir(source->op);
  inst.info.result = AST_MIR_INVALID_ID;
  inst.info.type = source->type;
  inst.info.span = source->span;
  inst.info.effects = source->effects;
  inst.info.flags = mir_instruction_flags(source->effects, inst.info.op);
  inst.info.token_op = source->token_op;
  inst.info.intrinsic = source->intrinsic;
  inst.info.memory_order = source->memory_order;
  inst.info.failure_memory_order = source->failure_memory_order;
  inst.info.stack_slot = AST_MIR_INVALID_ID;
  inst.info.source_local = source->local;
  inst.info.aux_index = source->aux_index;
  inst.info.native_region = source->native_region;
  inst.info.constant = source->constant;
  inst.info.call_signature = source->call_signature;
  inst.info.memory_in = AST_MIR_INVALID_ID;
  inst.info.memory_out = AST_MIR_INVALID_ID;
  inst.info.source_hir_instruction = source_id;
  if (inst.info.op == AST_MIR_OP_INVALID) {
    mir_function_diag(fn, AST_MIR_DIAG_UNSUPPORTED_HIR, AST_DIAG_ERROR,
                      source->span, "unsupported HIR instruction %u",
                      (unsigned)source->op);
    return 0;
  }
  if (direct_target) {
    inst.info.symbol = mir_copy_name(&fn->arena, direct_target->symbol);
    inst.info.module = mir_copy_name(&fn->arena, direct_target->module);
    if ((direct_target->symbol.span.length && !inst.info.symbol.text) ||
        (direct_target->module.span.length && !inst.info.module.text)) {
      fn->oom = 1u;
      return 0;
    }
  } else if (!mir_copy_instruction_names(fn, &inst, source))
    return 0;
  AstMirInstId expected_id = fn->instruction_count;
  if (source->result != AST_HIR_INVALID_ID) {
    AstMirVReg result = mir_add_vreg(
        fn, AST_MIR_VREG_INSTRUCTION_RESULT, source->type, source->span,
        block_id, expected_id,
        source->op == AST_HIR_OP_UNDEF ? AST_MIR_VREG_UNDEFINED : 0u);
    if (result == AST_MIR_INVALID_ID)
      return 0;
    inst.info.result = result;
    context->hir_values[source->result] = result;
  }
  if (mir_instruction_is_effectful(&inst)) {
    inst.info.memory_in = context->current_memory;
    inst.info.memory_out = mir_add_memory(fn, AST_MIR_MEMORY_INSTRUCTION_RESULT,
                                          block_id, expected_id);
    if (inst.info.memory_out == AST_MIR_INVALID_ID)
      return 0;
    context->current_memory = inst.info.memory_out;
  }
  return mir_emit_instruction(fn, block_id, inst, operands, operand_count) !=
         AST_MIR_INVALID_ID;
}

static int mir_lower_local_instruction(MirLowerContext *context,
                                       AstMirBlockId block_id,
                                       AstHirInstId source_id,
                                       const AstHirInstruction *source,
                                       AstMirVReg *operands) {
  if (source->local >= context->local_count)
    return 0;
  if (context->local_promotable[source->local]) {
    size_t index = mir_local_index(context, block_id, source->local);
    if (source->op == AST_HIR_OP_LOCAL_LOAD) {
      AstMirVReg value = mir_read_local_entry(context, block_id, source->local);
      if (value == AST_MIR_INVALID_ID ||
          source->result >= context->hir->value_count)
        return 0;
      context->hir_values[source->result] = value;
      return 1;
    }
    if (source->op == AST_HIR_OP_LOCAL_STORE) {
      if (source->operand_count != 1u)
        return 0;
      context->local_defs[index] = operands[0];
      return 1;
    }
  }

  AstMirFunction *fn = context->function;
  AstMirInstruction inst;
  memset(&inst, 0, sizeof(inst));
  inst.info.op = mir_op_from_hir(source->op);
  inst.info.result = AST_MIR_INVALID_ID;
  inst.info.type = source->type;
  inst.info.span = source->span;
  inst.info.effects = source->effects;
  if (source->op == AST_HIR_OP_LOCAL_LOAD)
    inst.info.effects |= AST_SEMA_EFFECT_READ_MEMORY;
  if (source->op == AST_HIR_OP_LOCAL_STORE)
    inst.info.effects |= AST_SEMA_EFFECT_WRITE_MEMORY;
  inst.info.flags = mir_instruction_flags(inst.info.effects, inst.info.op);
  inst.info.stack_slot = context->local_slots[source->local];
  inst.info.source_local = source->local;
  inst.info.native_region = AST_MIR_INVALID_ID;
  inst.info.memory_in = AST_MIR_INVALID_ID;
  inst.info.memory_out = AST_MIR_INVALID_ID;
  inst.info.source_hir_instruction = source_id;
  AstMirInstId expected_id = fn->instruction_count;
  if (source->result != AST_HIR_INVALID_ID) {
    AstMirVReg result =
        mir_add_vreg(fn, AST_MIR_VREG_INSTRUCTION_RESULT, source->type,
                     source->span, block_id, expected_id, 0u);
    if (result == AST_MIR_INVALID_ID)
      return 0;
    inst.info.result = result;
    context->hir_values[source->result] = result;
  }
  if (mir_instruction_is_effectful(&inst)) {
    inst.info.memory_in = context->current_memory;
    inst.info.memory_out = mir_add_memory(fn, AST_MIR_MEMORY_INSTRUCTION_RESULT,
                                          block_id, expected_id);
    if (inst.info.memory_out == AST_MIR_INVALID_ID)
      return 0;
    context->current_memory = inst.info.memory_out;
  }
  return mir_emit_instruction(fn, block_id, inst, operands,
                              source->operand_count) != AST_MIR_INVALID_ID;
}

static int mir_map_term_values(MirLowerContext *context,
                               const AstHirValueId *source, uint32_t count,
                               AstMirVReg *small, uint32_t small_capacity,
                               AstMirVReg **out) {
  *out = NULL;
  if (count == 0u)
    return 1;
  AstMirVReg *values = small;
  if (count > small_capacity) {
    size_t bytes = 0u;
    if (ast_size_mul_overflow(count, sizeof(AstMirVReg), &bytes))
      return 0;
    values = (AstMirVReg *)malloc(bytes);
    if (!values)
      return 0;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (source[i] >= context->hir->value_count ||
        context->hir_values[source[i]] == AST_MIR_INVALID_ID) {
      if (values != small)
        free(values);
      return 0;
    }
    values[i] = context->hir_values[source[i]];
  }
  *out = values;
  return 1;
}

static int mir_lower_terminator(MirLowerContext *context,
                                AstMirBlockId block_id,
                                const AstHirTerminator *source) {
  AstMirTerminator term;
  memset(&term, 0, sizeof(term));
  term.info.kind = mir_term_from_hir(source->kind);
  term.info.span = source->span;
  term.info.condition_or_value = AST_MIR_INVALID_ID;
  term.info.target_true = source->target_true;
  term.info.target_false = source->target_false;
  term.info.memory = context->current_memory;
  if (source->condition_or_value != AST_HIR_INVALID_ID) {
    if (source->condition_or_value >= context->hir->value_count ||
        context->hir_values[source->condition_or_value] == AST_MIR_INVALID_ID)
      return 0;
    term.info.condition_or_value =
        context->hir_values[source->condition_or_value];
  }
  AstMirVReg true_small[8];
  AstMirVReg false_small[8];
  AstMirVReg *true_args = NULL, *false_args = NULL;
  if (!mir_map_term_values(context, source->true_args, source->true_arg_count,
                           true_small, 8u, &true_args) ||
      !mir_map_term_values(context, source->false_args, source->false_arg_count,
                           false_small, 8u, &false_args)) {
    if (true_args != true_small)
      free(true_args);
    if (false_args != false_small)
      free(false_args);
    return 0;
  }
  int ok = mir_set_terminator(context->function, block_id, term, true_args,
                              source->true_arg_count, false_args,
                              source->false_arg_count);
  if (true_args != true_small)
    free(true_args);
  if (false_args != false_small)
    free(false_args);
  return ok;
}

static int mir_append_edge_argument(AstMirFunction *fn, AstMirTerminator *term,
                                    int false_edge, AstMirVReg value) {
  AstMirVReg **args = false_edge ? &term->false_args : &term->true_args;
  uint32_t *count =
      false_edge ? &term->info.false_arg_count : &term->info.true_arg_count;
  uint32_t *capacity =
      false_edge ? &term->false_arg_capacity : &term->true_arg_capacity;
  size_t cap = *capacity;
  if (!mir_reserve((void **)args, sizeof(**args), &cap, (size_t)*count + 1u)) {
    fn->oom = 1u;
    return 0;
  }
  if (cap > UINT32_MAX) {
    fn->oom = 1u;
    return 0;
  }
  *capacity = (uint32_t)cap;
  (*args)[(*count)++] = value;
  if (false_edge)
    term->info.false_args = *args;
  else
    term->info.true_args = *args;
  return 1;
}

static int mir_discover_phi_inputs(MirLowerContext *context) {
  size_t previous = SIZE_MAX;
  while (previous != context->function->phi_count) {
    previous = context->function->phi_count;
    for (uint32_t block_id = 0u; block_id < context->function->block_count;
         ++block_id) {
      AstMirBlock *block = &context->function->blocks[block_id];
      for (uint32_t ai = block->hir_argument_count; ai < block->argument_count;
           ++ai) {
        AstHirLocalId local = block->argument_source_locals[ai];
        for (uint32_t pi = 0u; pi < block->predecessor_count; ++pi) {
          if (mir_read_local_end(context, block->predecessors[pi], local) ==
              AST_MIR_INVALID_ID)
            return 0;
        }
      }
    }
  }
  return 1;
}

static int mir_append_phi_arguments_for_edge(MirLowerContext *context,
                                             AstMirBlockId from,
                                             AstMirBlockId to, int false_edge) {
  if (to >= context->function->block_count)
    return 0;
  AstMirBlock *target = &context->function->blocks[to];
  AstMirTerminator *term = &context->function->blocks[from].term;
  for (uint32_t ai = target->hir_argument_count; ai < target->argument_count;
       ++ai) {
    AstHirLocalId local = target->argument_source_locals[ai];
    AstMirVReg value = mir_read_local_end(context, from, local);
    if (value == AST_MIR_INVALID_ID ||
        !mir_append_edge_argument(context->function, term, false_edge, value))
      return 0;
  }
  return 1;
}

static int mir_complete_phi_edges(MirLowerContext *context) {
  if (!mir_discover_phi_inputs(context))
    return 0;
  for (uint32_t block_id = 0u; block_id < context->function->block_count;
       ++block_id) {
    AstMirTerminator *term = &context->function->blocks[block_id].term;
    if (term->info.kind == AST_MIR_TERM_BR) {
      if (!mir_append_phi_arguments_for_edge(context, block_id,
                                             term->info.target_true, 0))
        return 0;
    } else if (term->info.kind == AST_MIR_TERM_COND_BR) {
      if (!mir_append_phi_arguments_for_edge(context, block_id,
                                             term->info.target_true, 0) ||
          !mir_append_phi_arguments_for_edge(context, block_id,
                                             term->info.target_false, 1))
        return 0;
    }
  }
  return 1;
}

int mir_link_memory_ssa(AstMirFunction *fn) {
  if (!fn)
    return 0;
  fn->memory_phi_count = 0u;
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstMirBlock *block = &fn->blocks[bi];
    if (block->memory_argument >= fn->memory_value_count)
      return 0;
    AstMirMemoryInfo *memory = &fn->memory_values[block->memory_argument].info;
    memory->incoming = NULL;
    memory->incoming_count = 0u;
    if (bi == fn->entry_block || block->predecessor_count == 0u)
      continue;
    AstMirMemoryId *incoming =
        AST_ARENA_ARRAY(&fn->arena, AstMirMemoryId, block->predecessor_count);
    if (!incoming) {
      fn->oom = 1u;
      return 0;
    }
    for (uint32_t pi = 0u; pi < block->predecessor_count; ++pi) {
      AstMirBlockId predecessor = block->predecessors[pi];
      if (predecessor >= fn->block_count)
        return 0;
      AstMirMemoryId state = fn->blocks[predecessor].term.info.memory;
      if (state >= fn->memory_value_count)
        return 0;
      incoming[pi] = state;
    }
    memory->incoming = incoming;
    memory->incoming_count = block->predecessor_count;
    if (block->predecessor_count > 1u)
      fn->memory_phi_count++;
  }
  return 1;
}

static int mir_prepare_blocks(MirLowerContext *context) {
  AstMirFunction *fn = context->function;
  fn->block_count = context->hir->block_count;
  if (fn->block_count == 0u)
    return 1;
  fn->blocks = (AstMirBlock *)calloc(fn->block_count, sizeof(*fn->blocks));
  if (!fn->blocks) {
    fn->oom = 1u;
    return 0;
  }

  size_t capacity = fn->instruction_capacity;
  if (!mir_reserve((void **)&fn->instructions, sizeof(*fn->instructions),
                   &capacity, context->hir->instruction_count) ||
      capacity > UINT32_MAX) {
    fn->oom = 1u;
    return 0;
  }
  fn->instruction_capacity = (uint32_t)capacity;
  capacity = fn->vreg_capacity;
  if (!mir_reserve((void **)&fn->vregs, sizeof(*fn->vregs), &capacity,
                   context->hir->value_count) ||
      capacity > UINT32_MAX) {
    fn->oom = 1u;
    return 0;
  }
  fn->vreg_capacity = (uint32_t)capacity;
  size_t memory_hint = 0u;
  if (ast_size_add_overflow(context->hir->block_count,
                            context->hir->instruction_count, &memory_hint)) {
    fn->oom = 1u;
    return 0;
  }
  capacity = fn->memory_value_capacity;
  if (!mir_reserve((void **)&fn->memory_values, sizeof(*fn->memory_values),
                   &capacity, memory_hint) ||
      capacity > UINT32_MAX) {
    fn->oom = 1u;
    return 0;
  }
  fn->memory_value_capacity = (uint32_t)capacity;

  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    const AstHirBlock *source = &context->hir->blocks[bi];
    AstMirBlock *block = &fn->blocks[bi];
    block->id = bi;
    block->span = source->span;
    block->immediate_dominator = AST_MIR_INVALID_ID;
    block->reverse_postorder = AST_MIR_INVALID_ID;
    capacity = block->instruction_capacity;
    if (!mir_reserve((void **)&block->instructions,
                     sizeof(*block->instructions), &capacity,
                     source->instruction_count) ||
        capacity > UINT32_MAX) {
      fn->oom = 1u;
      return 0;
    }
    block->instruction_capacity = (uint32_t)capacity;
    AstMirMemoryKind memory_kind = bi == fn->entry_block
                                       ? AST_MIR_MEMORY_ENTRY
                                       : AST_MIR_MEMORY_BLOCK_ARGUMENT;
    block->memory_argument = mir_add_memory(fn, memory_kind, bi, 0u);
    if (block->memory_argument == AST_MIR_INVALID_ID)
      return 0;
    for (uint32_t ai = 0u; ai < source->argument_count; ++ai) {
      AstHirValueId source_arg = source->arguments[ai];
      if (source_arg >= context->hir->value_count)
        return 0;
      const AstHirValue *value = &context->hir->values[source_arg];
      uint32_t flags =
          bi == fn->entry_block ? AST_MIR_VREG_FUNCTION_ARGUMENT : 0u;
      AstMirVReg vreg = mir_add_vreg(fn, AST_MIR_VREG_BLOCK_ARGUMENT,
                                     value->type, value->span, bi, ai, flags);
      if (vreg == AST_MIR_INVALID_ID ||
          !mir_append_block_argument(fn, bi, vreg, AST_HIR_INVALID_ID))
        return 0;
      context->hir_values[source_arg] = vreg;
    }
    block->hir_argument_count = block->argument_count;
    if (!mir_append_preliminary_terminator(fn, bi, &source->term))
      return 0;
  }
  return mir_analyze_cfg(fn);
}

static int mir_prepare_locals(MirLowerContext *context) {
  AstMirFunction *fn = context->function;
  context->local_count = context->hir->local_count;
  if (context->local_count == 0u)
    return 1;
  context->local_slots = (AstMirStackSlotId *)mir_malloc_array(
      context->local_count, sizeof(*context->local_slots));
  context->local_promoted_index = (uint32_t *)mir_malloc_array(
      context->local_count, sizeof(*context->local_promoted_index));
  context->local_promotable = (uint8_t *)calloc(context->local_count, 1u);
  context->local_address_taken = (uint8_t *)calloc(context->local_count, 1u);
  if (!context->local_slots || !context->local_promoted_index ||
      !context->local_promotable || !context->local_address_taken) {
    fn->oom = 1u;
    return 0;
  }
  for (uint32_t li = 0u; li < context->local_count; ++li) {
    context->local_slots[li] = AST_MIR_INVALID_ID;
    context->local_promoted_index[li] = AST_MIR_INVALID_ID;
    context->local_promotable[li] =
        context->program->config.promote_locals ? 1u : 0u;
  }
  /* A bound value needs one SSA lifetime even in the unoptimized pipeline;
     binding a transient reload would silently drop the user's constraint. */
  for (uint32_t ii = 0u; ii < context->hir->instruction_count; ++ii) {
    const AstHirInstruction *inst = &context->hir->instructions[ii];
    if (inst->op == AST_HIR_OP_BIND && inst->local < context->local_count)
      context->local_promotable[inst->local] = 1u;
  }
  for (uint32_t ii = 0u; ii < context->hir->instruction_count; ++ii) {
    const AstHirInstruction *inst = &context->hir->instructions[ii];
    if (inst->op == AST_HIR_OP_LOCAL_ADDR &&
        inst->local < context->local_count) {
      context->local_promotable[inst->local] = 0u;
      context->local_address_taken[inst->local] = 1u;
    }
  }
  for (uint32_t li = 0u; li < context->local_count; ++li) {
    const AstHirLocal *source = &context->hir->locals[li];
    if (context->local_promotable[li]) {
      if (context->promoted_local_count == AST_MIR_INVALID_ID) {
        fn->oom = 1u;
        return 0;
      }
      context->local_promoted_index[li] = context->promoted_local_count++;
      fn->promoted_local_count++;
    } else if (!mir_add_stack_slot(context, li, source))
      return 0;
  }
  size_t cells = 0u;
  if (ast_size_mul_overflow(fn->block_count, context->promoted_local_count,
                            &cells)) {
    fn->oom = 1u;
    return 0;
  }
  size_t vreg_bytes = 0u;
  if (ast_size_mul_overflow(cells, sizeof(*context->local_defs), &vreg_bytes)) {
    fn->oom = 1u;
    return 0;
  }
  context->local_defs = (AstMirVReg *)malloc(vreg_bytes);
  context->local_phis = (AstMirVReg *)malloc(vreg_bytes);
  context->local_reading = (uint8_t *)calloc(cells ? cells : 1u, 1u);
  if (!context->local_defs || !context->local_phis || !context->local_reading) {
    fn->oom = 1u;
    return 0;
  }
  for (size_t i = 0u; i < cells; ++i) {
    context->local_defs[i] = AST_MIR_INVALID_ID;
    context->local_phis[i] = AST_MIR_INVALID_ID;
  }
  return 1;
}

static int mir_lower_blocks(MirLowerContext *context) {
  AstMirFunction *fn = context->function;
  uint32_t *rpo = (uint32_t *)mir_malloc_array(fn->block_count, sizeof(*rpo));
  if (!rpo && fn->block_count) {
    fn->oom = 1u;
    return 0;
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi)
    rpo[bi] = AST_MIR_INVALID_ID;
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    uint32_t position = fn->blocks[bi].reverse_postorder;
    if (position < fn->block_count && rpo[position] == AST_MIR_INVALID_ID)
      rpo[position] = bi;
  }
  uint32_t cursor = 0u;
  for (uint32_t pos = 0u; pos < fn->block_count; ++pos) {
    AstMirBlockId block_id = rpo[pos];
    if (block_id == AST_MIR_INVALID_ID) {
      while (cursor < fn->block_count && fn->blocks[cursor].reachable)
        cursor++;
      if (cursor >= fn->block_count) {
        free(rpo);
        return 0;
      }
      block_id = cursor++;
    }
    if (block_id >= fn->block_count || block_id >= context->hir->block_count) {
      free(rpo);
      return 0;
    }
    context->current_block = block_id;
    context->current_memory = fn->blocks[block_id].memory_argument;
    const AstHirBlock *source_block = &context->hir->blocks[block_id];
    for (uint32_t pos_in_block = 0u;
         pos_in_block < source_block->instruction_count; ++pos_in_block) {
      AstHirInstId source_id = source_block->instructions[pos_in_block];
      if (source_id >= context->hir->instruction_count) {
        free(rpo);
        return 0;
      }
      const AstHirInstruction *source = &context->hir->instructions[source_id];
      if (mir_function_ref_is_direct_only(context, source))
        continue;
      const AstHirInstruction *direct_target =
          mir_hir_direct_call_target(context->hir, source);
      AstMirVReg small_operands[8];
      AstMirVReg *operands = NULL;
      uint32_t operand_count = 0u;
      uint32_t first_operand = direct_target ? 1u : 0u;
      if (!mir_map_hir_operands(context, source, first_operand, small_operands,
                                8u, &operands, &operand_count)) {
        free(rpo);
        return 0;
      }
      int ok;
      if (source->op == AST_HIR_OP_LOCAL_LOAD ||
          source->op == AST_HIR_OP_LOCAL_STORE ||
          source->op == AST_HIR_OP_LOCAL_ADDR)
        ok = mir_lower_local_instruction(context, block_id, source_id, source,
                                         operands);
      else
        ok = mir_lower_regular_instruction(context, block_id, source_id, source,
                                           operands, operand_count,
                                           direct_target);
      if (operands != small_operands)
        free(operands);
      if (!ok) {
        free(rpo);
        return 0;
      }
    }
    if (!mir_lower_terminator(context, block_id, &source_block->term)) {
      free(rpo);
      return 0;
    }
  }
  free(rpo);
  return 1;
}

static void mir_lower_context_destroy(MirLowerContext *context) {
  if (!context)
    return;
  free(context->hir_values);
  free(context->hir_value_use_count);
  free(context->local_slots);
  free(context->local_promoted_index);
  free(context->local_promotable);
  free(context->local_address_taken);
  free(context->local_defs);
  free(context->local_phis);
  free(context->local_reading);
}

int mir_lower_function(AstMirProgram *program, AstMirFunction *fn) {
  if (!program || !fn)
    return 0;
  MirLowerContext context;
  memset(&context, 0, sizeof(context));
  context.program = program;
  context.function = fn;
  context.hir = fn->source_hir;
  if (!context.hir)
    return 0;
  if (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH))
    return 1;
  if (context.hir->value_count) {
    context.hir_values = (AstMirVReg *)mir_malloc_array(
        context.hir->value_count, sizeof(*context.hir_values));
    if (!context.hir_values)
      fn->oom = 1u;
    else
      for (uint32_t i = 0u; i < context.hir->value_count; ++i)
        context.hir_values[i] = AST_MIR_INVALID_ID;
  }
  int ok = !fn->oom && mir_prepare_blocks(&context);
  ok = ok && mir_prepare_locals(&context) && mir_prepare_hir_uses(&context) &&
       mir_lower_blocks(&context) && mir_complete_phi_edges(&context) &&
       mir_link_memory_ssa(fn) && mir_analyze_liveness(fn);
  if (!ok && fn->diagnostic_count == 0u && !fn->oom)
    mir_function_diag(fn, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR, fn->span,
                      "MIR lowering failed for function");
  mir_lower_context_destroy(&context);
  return ok && !fn->oom;
}
