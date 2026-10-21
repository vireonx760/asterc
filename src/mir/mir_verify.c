#include "mir_internal.h"

static int mir_text_equal(AstHirText a, AstHirText b) {
  return a.length == b.length &&
         (a.length == 0u ||
          (a.data && b.data && memcmp(a.data, b.data, a.length) == 0));
}

static int mir_text_value_equal(AstHirTextValue a, AstHirTextValue b) {
  return a.is_present == b.is_present && a.is_literal == b.is_literal &&
         a.source_kind == b.source_kind && mir_text_equal(a.text, b.text);
}

static int mir_constant_equal(AstSemaConstInfo a, AstSemaConstInfo b) {
  uint64_t a_bits = 0u, b_bits = 0u;
  memcpy(&a_bits, &a.floating, sizeof(a_bits));
  memcpy(&b_bits, &b.floating, sizeof(b_bits));
  return a.kind == b.kind && a.lo == b.lo && a.hi == b.hi && a_bits == b_bits &&
         a.is_negative == b.is_negative && a.is_signed == b.is_signed;
}

static const AstHirInstruction *
mir_verify_hir_value_definition(const AstHirFunction *function,
                                AstHirValueId value) {
  if (!function || value >= function->value_count)
    return NULL;
  const AstHirValue *info = &function->values[value];
  if (info->kind != AST_HIR_VALUE_INSTRUCTION_RESULT ||
      info->definition_index >= function->instruction_count)
    return NULL;
  return &function->instructions[info->definition_index];
}

static const AstHirInstruction *
mir_verify_direct_call_target(const AstHirFunction *function,
                              const AstHirInstruction *call) {
  if (!function || !call || call->op != AST_HIR_OP_CALL ||
      call->operand_count == 0u)
    return NULL;
  const AstHirInstruction *target =
      mir_verify_hir_value_definition(function, call->operands[0]);
  return target && target->op == AST_HIR_OP_FUNCTION_REF ? target : NULL;
}

static int mir_verify_source_instruction(AstMirProgram *program,
                                         AstMirFunction *fn,
                                         const AstMirInstruction *inst) {
  if (inst->info.source_hir_instruction == AST_HIR_INVALID_ID) {
    int synthetic = (inst->info.flags & AST_MIR_INST_SYNTHETIC) != 0u ||
                    (inst->info.op == AST_MIR_OP_UNDEF &&
                     inst->info.result < fn->vreg_count &&
                     (fn->vregs[inst->info.result].info.flags &
                      AST_MIR_VREG_UNDEFINED) != 0u);
    if (!synthetic)
      mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                       inst->info.span,
                       "non-synthetic MIR instruction has no HIR origin");
    return synthetic;
  }
  if (!fn->source_hir ||
      inst->info.source_hir_instruction >= fn->source_hir->instruction_count) {
    mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                     inst->info.span,
                     "MIR instruction references an invalid HIR origin");
    return 0;
  }
  const AstHirInstruction *source =
      &fn->source_hir->instructions[inst->info.source_hir_instruction];
  const AstHirInstruction *direct_target =
      mir_verify_direct_call_target(fn->source_hir, source);
  uint32_t source_operand_offset = direct_target ? 1u : 0u;
  if (source_operand_offset > source->operand_count)
    return 0;
  uint32_t expected_operand_count =
      source->operand_count - source_operand_offset;
  AstMirOp expected_op =
      direct_target ? AST_MIR_OP_CALL_DIRECT : mir_op_from_hir(source->op);
  uint32_t expected_effects = source->effects;
  if (source->op == AST_HIR_OP_LOCAL_LOAD)
    expected_effects |= AST_SEMA_EFFECT_READ_MEMORY;
  if (source->op == AST_HIR_OP_LOCAL_STORE)
    expected_effects |= AST_SEMA_EFFECT_WRITE_MEMORY;
  int expected_result = source->result != AST_HIR_INVALID_ID;
  int actual_result = inst->info.result != AST_MIR_INVALID_ID;
  int ok = inst->info.op == expected_op && expected_result == actual_result &&
           mir_type_equal(inst->info.type, source->type) &&
           inst->info.operand_count == expected_operand_count &&
           inst->info.effects == expected_effects &&
           inst->info.memory_order == source->memory_order &&
           inst->info.failure_memory_order == source->failure_memory_order &&
           (inst->info.operand_count == 0u || inst->operands != NULL);
  switch (source->op) {
  case AST_HIR_OP_CONSTANT:
    ok &= mir_constant_equal(inst->info.constant, source->constant);
    break;
  case AST_HIR_OP_GLOBAL_ADDR:
  case AST_HIR_OP_GLOBAL_LOAD:
  case AST_HIR_OP_GLOBAL_STORE:
  case AST_HIR_OP_FUNCTION_REF:
    ok &= hir_name_eq(inst->info.symbol, source->symbol) &&
          hir_name_eq(inst->info.module, source->module);
    break;
  case AST_HIR_OP_GENERIC_CONST:
    ok &= hir_name_eq(inst->info.symbol, source->symbol) &&
          inst->info.aux_index == source->aux_index;
    break;
  case AST_HIR_OP_UNARY:
  case AST_HIR_OP_BINARY:
  case AST_HIR_OP_CAST:
    ok &= inst->info.token_op == source->token_op;
    break;
  case AST_HIR_OP_CALL:
    ok &= inst->info.call_signature && source->call_signature &&
          mir_type_equal(inst->info.call_signature, source->call_signature);
    if (direct_target) {
      ok &= hir_name_eq(inst->info.symbol, direct_target->symbol) &&
            hir_name_eq(inst->info.module, direct_target->module);
    } else {
      ok &= inst->info.symbol.span.length == 0u &&
            inst->info.module.span.length == 0u;
    }
    break;
  case AST_HIR_OP_MEMBER:
    ok &= hir_name_eq(inst->info.symbol, source->symbol) &&
          inst->info.aux_index == source->aux_index;
    break;
  case AST_HIR_OP_ATOMIC:
    ok &= hir_name_eq(inst->info.symbol, source->symbol);
    break;
  case AST_HIR_OP_INTRINSIC:
    ok &= inst->info.intrinsic == source->intrinsic;
    break;
  case AST_HIR_OP_BIND:
    ok &= hir_name_eq(inst->info.symbol, source->symbol) &&
          inst->info.source_local == source->local;
    break;
  case AST_HIR_OP_NATIVE:
    ok &= hir_name_eq(inst->info.symbol, source->symbol) &&
          inst->info.native_region == source->native_region;
    break;
  case AST_HIR_OP_NATIVE_RESULT:
    ok &= inst->info.native_region == source->native_region &&
          inst->info.aux_index == source->aux_index &&
          inst->info.source_local == source->local;
    break;
  default:
    break;
  }
  if (source->op == AST_HIR_OP_LOCAL_LOAD ||
      source->op == AST_HIR_OP_LOCAL_STORE ||
      source->op == AST_HIR_OP_LOCAL_ADDR) {
    if (inst->info.source_local != source->local ||
        inst->info.stack_slot >= fn->stack_slot_count ||
        fn->stack_slots[inst->info.stack_slot].info.source_local !=
            source->local)
      ok = 0;
  } else if (inst->info.stack_slot != AST_MIR_INVALID_ID) {
    ok = 0;
  }
  for (uint32_t oi = 0u; inst->operands && oi < inst->info.operand_count &&
                         oi + source_operand_offset < source->operand_count;
       ++oi) {
    AstHirValueId source_value_id =
        source->operands[oi + source_operand_offset];
    if (inst->operands[oi] >= fn->vreg_count ||
        source_value_id >= fn->source_hir->value_count ||
        !mir_type_equal(fn->vregs[inst->operands[oi]].info.type,
                        fn->source_hir->values[source_value_id].type))
      ok = 0;
  }
  if (!ok)
    mir_program_diag(
        program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR, inst->info.span,
        "MIR instruction %u does not faithfully preserve its HIR origin",
        inst->info.id);
  return ok;
}

static int mir_verify_dominates(const AstMirFunction *fn,
                                AstMirBlockId dominator, AstMirBlockId block);

/* Post-GVN certificate validation is intentionally separate from HIR fidelity.
 * The original node MUST still satisfy all canonical HIR constraints, and the
 * replacement must be an exact, dominating, same-block value. */
static int mir_verify_gvn_certificate(AstMirProgram *program,
                                      AstMirFunction *fn,
                                      const AstMirInstruction *inst,
                                      const AstMirBlockId *instruction_blocks,
                                      const uint32_t *instruction_positions) {
  if (!fn->gvn_certificates || inst->info.id >= fn->instruction_count)
    return 0;
  const MirGvnCertificate *cert = &fn->gvn_certificates[inst->info.id];
  if (!cert->valid || !cert->original.operands ||
      !mir_verify_source_instruction(program, fn, &cert->original) ||
      cert->original.info.id != inst->info.id ||
      cert->original.info.result != inst->info.result ||
      cert->original.info.source_hir_instruction !=
          inst->info.source_hir_instruction ||
      cert->original.info.effects || cert->original.info.flags ||
      inst->info.effects || inst->info.flags ||
      !mir_type_equal(cert->original.info.type, inst->info.type) ||
      ((cert->valid == 3u || cert->valid == 4u)
           ? (inst->info.op != AST_MIR_OP_CONSTANT ||
              inst->info.operand_count != 0u ||
              cert->witness != AST_MIR_INVALID_ID)
           : (inst->info.op != AST_MIR_OP_COPY ||
              inst->info.operand_count != 1u || !inst->operands ||
              cert->witness >= fn->instruction_count))) {
    mir_program_diag(
        program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR, inst->info.span,
        "GVN rewrite lacks a valid, effect-free original HIR certificate");
    return 0;
  }
  for (uint32_t oi = 0u; oi < cert->original.info.operand_count; ++oi) {
    AstMirVReg vr = cert->original.operands[oi];
    if (vr >= fn->vreg_count ||
        (fn->vregs[vr].info.flags & AST_MIR_VREG_UNDEFINED))
      return 0;
    AstSemaTypeKind kind = ast_sema_type_kind(fn->vregs[vr].info.type);
    if (kind != AST_SEMA_TYPE_INT && kind != AST_SEMA_TYPE_BITS &&
        kind != AST_SEMA_TYPE_BOOL)
      return 0;
  }
  if (cert->valid == 3u || cert->valid == 4u) {
    AstSemaConstInfo expected;
    if (cert->valid == 3u &&
        (!mir_opt_fold_unsigned(fn, &cert->original, &expected) ||
         !mir_constant_equal(inst->info.constant, expected))) {
      mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                       inst->info.span,
                       "constant-fold certificate does not reproduce the "
                       "original expression");
      return 0;
    }
    /* Original operands were removed from the executed instruction, but
     * must still satisfy the SSA definition and dominance contract. */
    AstMirBlockId use = instruction_blocks[inst->info.id];
    for (uint32_t oi = 0u; oi < cert->original.info.operand_count; ++oi) {
      AstMirVReg vr = cert->original.operands[oi];
      AstMirInstId definition = fn->vregs[vr].info.definition_index;
      AstMirBlockId from = instruction_blocks[definition];
      if (from >= fn->block_count || use >= fn->block_count ||
          (from == use && instruction_positions[definition] >=
                              instruction_positions[inst->info.id]) ||
          (from != use && !mir_verify_dominates(fn, from, use)))
        return 0;
    }
    return 1;
  }
  const AstMirInstruction *witness = &fn->instructions[cert->witness];
  AstMirVReg replacement = AST_MIR_INVALID_ID;
  int equivalent =
      cert->valid == 1u && mir_opt_gvn_match(&cert->original, witness);
  if (equivalent)
    replacement = witness->info.result;
  if (cert->valid == 2u)
    equivalent =
        mir_opt_identity_match(fn, &cert->original, witness, &replacement);
  if (!equivalent || inst->operands[0] != replacement ||
      (fn->gvn_certificates && fn->gvn_certificates[cert->witness].valid)) {
    mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                     inst->info.span,
                     "GVN witness is not the identical original computation");
    return 0;
  }
  /* Cached positions were rebuilt independently from CFG block ownership;
   * the CFG verifier separately recomputes the immediate dominator tree. */
  AstMirBlockId witness_block = instruction_blocks[cert->witness];
  AstMirBlockId use_block = instruction_blocks[inst->info.id];
  if (witness_block < fn->block_count && use_block < fn->block_count &&
      fn->blocks[witness_block].reachable && fn->blocks[use_block].reachable &&
      ((witness_block == use_block &&
        instruction_positions[cert->witness] <
            instruction_positions[inst->info.id]) ||
       (witness_block != use_block &&
        mir_verify_dominates(fn, witness_block, use_block))))
    return 1;
  mir_program_diag(
      program, AST_MIR_DIAG_DOMINANCE, AST_DIAG_ERROR, inst->info.span,
      "GVN witness does not dominate replacement in the validated CFG");
  return 0;
}

static int mir_verify_dominates(const AstMirFunction *fn,
                                AstMirBlockId dominator, AstMirBlockId block) {
  if (dominator >= fn->block_count || block >= fn->block_count)
    return 0;
  if (!fn->blocks[block].reachable)
    return dominator == block;
  AstMirBlockId cursor = block;
  uint32_t steps = 0u;
  while (cursor != AST_MIR_INVALID_ID) {
    if (cursor >= fn->block_count || steps++ >= fn->block_count)
      return 0;
    if (cursor == dominator)
      return 1;
    cursor = fn->blocks[cursor].immediate_dominator;
  }
  return 0;
}

static int mir_verify_vreg_use(AstMirProgram *program, AstMirFunction *fn,
                               AstMirBlockId use_block, uint32_t use_position,
                               AstMirVReg vreg, AstSpan span,
                               const AstMirBlockId *instruction_blocks,
                               const uint32_t *instruction_positions) {
  if (vreg >= fn->vreg_count) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR, span,
                     "MIR operand virtual register %u is out of range", vreg);
    return 0;
  }
  const AstMirVRegInfo *value = &fn->vregs[vreg].info;
  AstMirBlockId definition_block = value->block;
  uint32_t definition_position = 0u;
  if (value->kind == AST_MIR_VREG_INSTRUCTION_RESULT) {
    if (value->definition_index >= fn->instruction_count) {
      mir_program_diag(
          program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR, span,
          "MIR virtual register %u has an out-of-range defining instruction",
          vreg);
      return 0;
    }
    definition_block = instruction_blocks[value->definition_index];
    definition_position = instruction_positions[value->definition_index] + 1u;
  } else if (value->kind != AST_MIR_VREG_BLOCK_ARGUMENT) {
    mir_program_diag(
        program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR, span,
        "MIR operand virtual register %u has an invalid definition kind", vreg);
    return 0;
  }
  if (definition_block == use_block) {
    if (value->kind == AST_MIR_VREG_INSTRUCTION_RESULT &&
        definition_position > use_position) {
      mir_program_diag(program, AST_MIR_DIAG_DOMINANCE, AST_DIAG_ERROR, span,
                       "MIR virtual register %u is used before its definition",
                       vreg);
      return 0;
    }
    return 1;
  }
  if (!mir_verify_dominates(fn, definition_block, use_block)) {
    mir_program_diag(program, AST_MIR_DIAG_DOMINANCE, AST_DIAG_ERROR, span,
                     "MIR virtual register %u does not dominate its use", vreg);
    return 0;
  }
  return 1;
}

static int mir_verify_operand_count(AstMirProgram *program,
                                    const AstMirInstruction *inst,
                                    uint32_t minimum, uint32_t maximum) {
  if (inst->info.operand_count >= minimum &&
      inst->info.operand_count <= maximum)
    return 1;
  mir_program_diag(program, AST_MIR_DIAG_INVALID_OPERAND, AST_DIAG_ERROR,
                   inst->info.span,
                   "MIR instruction '%s' has %u operands; expected %u..%u",
                   ast_mir_op_name(inst->info.op), inst->info.operand_count,
                   minimum, maximum);
  return 0;
}

static int mir_verify_stack_slot(AstMirProgram *program, AstMirFunction *fn,
                                 const AstMirInstruction *inst) {
  if (inst->info.stack_slot < fn->stack_slot_count)
    return 1;
  mir_program_diag(program, AST_MIR_DIAG_INVALID_STACK_SLOT, AST_DIAG_ERROR,
                   inst->info.span,
                   "MIR instruction '%s' references invalid stack slot %u",
                   ast_mir_op_name(inst->info.op), inst->info.stack_slot);
  return 0;
}

static int mir_verify_instruction_shape(AstMirProgram *program,
                                        AstMirFunction *fn,
                                        const AstMirInstruction *inst) {
  int ok = 1;
  int has_result = inst->info.result != AST_MIR_INVALID_ID;
  if (inst->info.operands != inst->operands)
    ok = 0;
  if (inst->info.operand_count != 0u && !inst->operands)
    ok = 0;
  switch (inst->info.op) {
  case AST_MIR_OP_CONSTANT:
  case AST_MIR_OP_UNDEF:
  case AST_MIR_OP_GLOBAL_ADDR:
  case AST_MIR_OP_GLOBAL_LOAD:
  case AST_MIR_OP_FUNCTION_ADDR:
  case AST_MIR_OP_GENERIC_CONST:
  case AST_MIR_OP_NATIVE_RESULT:
    ok &= mir_verify_operand_count(program, inst, 0u, 0u);
    ok &= has_result && inst->info.type != NULL;
    break;
  case AST_MIR_OP_COPY:
  case AST_MIR_OP_UNARY:
  case AST_MIR_OP_CAST:
  case AST_MIR_OP_MEMBER:
  case AST_MIR_OP_ADDRESS_OF:
  case AST_MIR_OP_LOAD:
    ok &= mir_verify_operand_count(program, inst, 1u, 1u);
    ok &= has_result && inst->info.type != NULL;
    break;
  case AST_MIR_OP_BINARY:
  case AST_MIR_OP_INDEX:
    ok &= mir_verify_operand_count(program, inst, 2u, 2u);
    ok &= has_result && inst->info.type != NULL;
    break;
  case AST_MIR_OP_SELECT:
    ok &= mir_verify_operand_count(program, inst, 3u, 3u);
    ok &= has_result && inst->info.type != NULL;
    break;
  case AST_MIR_OP_PACK:
    ok &= mir_verify_operand_count(program, inst, 1u, UINT32_MAX);
    ok &= has_result && inst->info.type != NULL &&
          (ast_sema_type_kind(inst->info.type) == AST_SEMA_TYPE_TUPLE ||
           ast_sema_type_kind(inst->info.type) == AST_SEMA_TYPE_STRUCT ||
           ast_sema_type_kind(inst->info.type) == AST_SEMA_TYPE_ARRAY);
    break;
  case AST_MIR_OP_CALL_DIRECT:
    ok &= inst->info.symbol.span.length != 0u;
    ok &= inst->info.call_signature != NULL;
    break;
  case AST_MIR_OP_CALL_INDIRECT:
    ok &= mir_verify_operand_count(program, inst, 1u, UINT32_MAX);
    ok &= inst->info.symbol.span.length == 0u &&
          inst->info.module.span.length == 0u;
    ok &= inst->info.call_signature != NULL;
    break;
  case AST_MIR_OP_INTRINSIC:
    ok &= inst->info.intrinsic != AST_HIR_INTRINSIC_NONE;
    break;
  case AST_MIR_OP_STACK_ADDR:
    ok &= mir_verify_operand_count(program, inst, 0u, 0u);
    ok &= has_result && mir_verify_stack_slot(program, fn, inst);
    break;
  case AST_MIR_OP_STACK_LOAD:
    ok &= mir_verify_operand_count(program, inst, 0u, 0u);
    ok &= has_result && mir_verify_stack_slot(program, fn, inst);
    if (inst->info.stack_slot < fn->stack_slot_count)
      ok &= mir_type_equal(inst->info.type,
                           fn->stack_slots[inst->info.stack_slot].info.type);
    break;
  case AST_MIR_OP_STACK_STORE:
    ok &= mir_verify_operand_count(program, inst, 1u, 1u);
    ok &= !has_result && mir_verify_stack_slot(program, fn, inst);
    if (inst->operands && inst->info.stack_slot < fn->stack_slot_count &&
        inst->info.operand_count == 1u && inst->operands[0] < fn->vreg_count)
      ok &= mir_type_equal(fn->vregs[inst->operands[0]].info.type,
                           fn->stack_slots[inst->info.stack_slot].info.type);
    break;
  case AST_MIR_OP_GLOBAL_STORE:
  case AST_MIR_OP_BIND:
    ok &= mir_verify_operand_count(program, inst, 1u, 1u);
    ok &= !has_result;
    break;
  case AST_MIR_OP_STORE:
    ok &= mir_verify_operand_count(program, inst, 2u, 2u);
    ok &= !has_result;
    break;
  case AST_MIR_OP_ATOMIC:
    ok &= mir_verify_operand_count(program, inst, 1u, UINT32_MAX);
    ok &= (inst->info.effects & AST_SEMA_EFFECT_ATOMIC) != 0u;
    ok &= inst->info.memory_order >= AST_MEMORY_ORDER_RELAXED &&
          inst->info.memory_order <= AST_MEMORY_ORDER_SEQ_CST;
    if (inst->info.failure_memory_order != AST_MEMORY_ORDER_NONE)
      ok &= inst->info.failure_memory_order >= AST_MEMORY_ORDER_RELAXED &&
            inst->info.failure_memory_order <= AST_MEMORY_ORDER_SEQ_CST &&
            inst->info.failure_memory_order != AST_MEMORY_ORDER_RELEASE &&
            inst->info.failure_memory_order != AST_MEMORY_ORDER_ACQ_REL;
    break;
  case AST_MIR_OP_FENCE:
    ok &= mir_verify_operand_count(program, inst, 0u, 0u);
    ok &= !has_result && (inst->info.effects & AST_SEMA_EFFECT_ATOMIC) != 0u;
    ok &= inst->info.memory_order >= AST_MEMORY_ORDER_RELAXED &&
          inst->info.memory_order <= AST_MEMORY_ORDER_SEQ_CST &&
          inst->info.failure_memory_order == AST_MEMORY_ORDER_NONE;
    break;
  case AST_MIR_OP_NATIVE: {
    ok &= !has_result;
    const AstHirNativeRegion *region = ast_hir_function_native_region(
        fn->source_hir, inst->info.native_region);
    AstHirNativeRegionInfo region_info;
    if (!region || !ast_hir_native_region_info(region, &region_info) ||
        inst->info.operand_count != region_info.input_count)
      ok = 0;
    break;
  }
  case AST_MIR_OP_INVALID:
  default:
    ok = 0;
    break;
  }
  if (has_result && (inst->info.result >= fn->vreg_count || !inst->info.type))
    ok = 0;
  if (!ok) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_OPERAND, AST_DIAG_ERROR,
                     inst->info.span,
                     "invalid operand/result shape for MIR instruction '%s'",
                     ast_mir_op_name(inst->info.op));
  }
  uint32_t expected_flags =
      mir_instruction_flags(inst->info.effects, inst->info.op) |
      (inst->info.flags & AST_MIR_INST_SYNTHETIC);
  if (inst->info.flags != expected_flags) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_EFFECTS, AST_DIAG_ERROR,
                     inst->info.span,
                     "MIR instruction '%s' has inconsistent effect flags",
                     ast_mir_op_name(inst->info.op));
    ok = 0;
  }
  if ((inst->info.op == AST_MIR_OP_LOAD ||
       inst->info.op == AST_MIR_OP_GLOBAL_LOAD ||
       inst->info.op == AST_MIR_OP_STACK_LOAD) &&
      !(inst->info.effects & AST_SEMA_EFFECT_READ_MEMORY)) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_EFFECTS, AST_DIAG_ERROR,
                     inst->info.span,
                     "MIR load is missing its read-memory effect");
    ok = 0;
  }
  if ((inst->info.op == AST_MIR_OP_STORE ||
       inst->info.op == AST_MIR_OP_GLOBAL_STORE ||
       inst->info.op == AST_MIR_OP_STACK_STORE) &&
      !(inst->info.effects & AST_SEMA_EFFECT_WRITE_MEMORY)) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_EFFECTS, AST_DIAG_ERROR,
                     inst->info.span,
                     "MIR store is missing its write-memory effect");
    ok = 0;
  }
  return ok;
}

static int mir_verify_cfg_metadata(AstMirProgram *program, AstMirFunction *fn) {
  AstMirFunction expected;
  memset(&expected, 0, sizeof(expected));
  expected.block_count = fn->block_count;
  expected.entry_block = fn->entry_block;
  expected.blocks = (AstMirBlock *)calloc(
      fn->block_count ? fn->block_count : 1u, sizeof(*expected.blocks));
  if (!expected.blocks)
    return 0;
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    expected.blocks[bi].id = bi;
    expected.blocks[bi].term.info.kind = fn->blocks[bi].term.info.kind;
    expected.blocks[bi].term.info.target_true =
        fn->blocks[bi].term.info.target_true;
    expected.blocks[bi].term.info.target_false =
        fn->blocks[bi].term.info.target_false;
    expected.blocks[bi].immediate_dominator = AST_MIR_INVALID_ID;
    expected.blocks[bi].reverse_postorder = AST_MIR_INVALID_ID;
  }
  int ok = mir_analyze_cfg(&expected);
  if (!ok) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                     fn->span, "cannot independently analyze MIR CFG");
  } else {
    for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
      const AstMirBlock *actual = &fn->blocks[bi];
      const AstMirBlock *wanted = &expected.blocks[bi];
      int block_ok = 1;
      if (actual->id != bi ||
          actual->predecessor_count != wanted->predecessor_count ||
          actual->successor_count != wanted->successor_count ||
          actual->immediate_dominator != wanted->immediate_dominator ||
          actual->reverse_postorder != wanted->reverse_postorder ||
          actual->loop_depth != wanted->loop_depth ||
          actual->reachable != wanted->reachable) {
        block_ok = 0;
      }
      if (actual->predecessor_count != 0u && !actual->predecessors)
        block_ok = 0;
      if (actual->predecessors) {
        for (uint32_t pi = 0u;
             pi < actual->predecessor_count && pi < wanted->predecessor_count;
             ++pi)
          if (actual->predecessors[pi] != wanted->predecessors[pi])
            block_ok = 0;
      }
      for (uint32_t si = 0u;
           si < actual->successor_count && si < wanted->successor_count &&
           si < AST_MIR_MAX_SUCCESSORS;
           ++si)
        if (actual->successors[si] != wanted->successors[si])
          block_ok = 0;
      if (!block_ok) {
        mir_program_diag(program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                         actual->span,
                         "stored MIR CFG analysis disagrees with the "
                         "control-flow graph in block %u",
                         bi);
        ok = 0;
      }
    }
  }
  for (uint32_t bi = 0u; bi < expected.block_count; ++bi)
    free(expected.blocks[bi].predecessors);
  free(expected.blocks);
  return ok;
}

static int mir_verify_branch(AstMirProgram *program, AstMirFunction *fn,
                             AstMirBlockId from, AstMirBlockId target,
                             const AstMirVReg *arguments, uint32_t count,
                             AstSpan span, uint32_t use_position,
                             const AstMirBlockId *instruction_blocks,
                             const uint32_t *instruction_positions) {
  if (target >= fn->block_count) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_SUCCESSOR, AST_DIAG_ERROR,
                     span, "MIR branch target %u is out of range", target);
    return 0;
  }
  if (count != 0u && !arguments) {
    mir_program_diag(program, AST_MIR_DIAG_BRANCH_ARGUMENT_MISMATCH,
                     AST_DIAG_ERROR, span,
                     "MIR branch has an argument count but no argument table");
    return 0;
  }
  AstMirBlock *successor = &fn->blocks[target];
  int ok = 1;
  if (count != successor->argument_count) {
    mir_program_diag(
        program, AST_MIR_DIAG_BRANCH_ARGUMENT_MISMATCH, AST_DIAG_ERROR, span,
        "MIR branch passes %u values to block %u, which expects %u", count,
        target, successor->argument_count);
    ok = 0;
  }
  uint32_t common =
      count < successor->argument_count ? count : successor->argument_count;
  for (uint32_t ai = 0u; ai < common; ++ai) {
    if (arguments[ai] >= fn->vreg_count ||
        successor->arguments[ai] >= fn->vreg_count) {
      ok = 0;
      continue;
    }
    if (!mir_type_equal(fn->vregs[arguments[ai]].info.type,
                        fn->vregs[successor->arguments[ai]].info.type)) {
      mir_program_diag(
          program, AST_MIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, span,
          "MIR branch argument %u has the wrong type for block %u", ai, target);
      ok = 0;
    }
    ok &= mir_verify_vreg_use(program, fn, from, use_position, arguments[ai],
                              span, instruction_blocks, instruction_positions);
  }
  return ok;
}

int mir_verify_canonical_liveness(AstMirProgram *program, AstMirFunction *fn) {
  AstMirFunction expected;
  memset(&expected, 0, sizeof(expected));
  expected.block_count = fn->block_count;
  expected.vreg_count = fn->vreg_count;
  expected.instruction_count = fn->instruction_count;
  expected.instructions = fn->instructions;
  expected.liveness_word_count = fn->liveness_word_count;
  expected.blocks = (AstMirBlock *)mir_malloc_array(fn->block_count,
                                                    sizeof(*expected.blocks));
  expected.vregs = (AstMirVRegData *)mir_malloc_array(fn->vreg_count,
                                                      sizeof(*expected.vregs));
  if ((!expected.blocks && fn->block_count) ||
      (!expected.vregs && fn->vreg_count)) {
    free(expected.blocks);
    free(expected.vregs);
    return 0;
  }
  if (fn->block_count)
    memcpy(expected.blocks, fn->blocks,
           (size_t)fn->block_count * sizeof(*expected.blocks));
  if (fn->vreg_count)
    memcpy(expected.vregs, fn->vregs,
           (size_t)fn->vreg_count * sizeof(*expected.vregs));
  int ok = mir_analyze_liveness(&expected);
  if (!ok || expected.liveness_word_count != fn->liveness_word_count)
    ok = 0;
  size_t cells = 0u, bytes = 0u;
  if (ast_size_mul_overflow(fn->block_count, fn->liveness_word_count, &cells) ||
      ast_size_mul_overflow(cells, sizeof(uint64_t), &bytes))
    ok = 0;
  if (ok && bytes &&
      (memcmp(expected.live_in, fn->live_in, bytes) != 0 ||
       memcmp(expected.live_out, fn->live_out, bytes) != 0))
    ok = 0;
  for (uint32_t bi = 0u; ok && bi < fn->block_count; ++bi) {
    if (expected.blocks[bi].live_in_count != fn->blocks[bi].live_in_count ||
        expected.blocks[bi].live_out_count != fn->blocks[bi].live_out_count ||
        expected.blocks[bi].position_begin != fn->blocks[bi].position_begin ||
        expected.blocks[bi].position_end != fn->blocks[bi].position_end)
      ok = 0;
  }
  for (uint32_t vi = 0u; ok && vi < fn->vreg_count; ++vi) {
    const AstMirVRegInfo *a = &expected.vregs[vi].info;
    const AstMirVRegInfo *b = &fn->vregs[vi].info;
    if (a->use_count != b->use_count || a->live_start != b->live_start ||
        a->live_end != b->live_end || a->spill_weight != b->spill_weight)
      ok = 0;
  }
  if (!ok)
    mir_program_diag(program, AST_MIR_DIAG_INVALID_LIVENESS, AST_DIAG_ERROR,
                     fn->span,
                     "stored MIR liveness data is not canonical for the "
                     "current instruction graph");
  free(expected.live_in);
  free(expected.live_out);
  free(expected.blocks);
  free(expected.vregs);
  return ok;
}

int mir_verify_function(AstMirProgram *program, AstMirFunction *fn) {
  if (!program || !fn)
    return 0;
  if (fn->inline_site_count) {
    mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                     fn->span,
                     "inline source sites require a whole-module proof");
    return 0;
  }
  if (!fn->module_name.text || !fn->name.text || !fn->type || !fn->source_hir) {
    mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR, fn->span,
                     "MIR function has incomplete canonical metadata");
    return 0;
  }
  int ok = 1;
  AstHirFunctionInfo source_info;
  memset(&source_info, 0, sizeof(source_info));
  if (!ast_hir_function_info(fn->source_hir, &source_info)) {
    mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                     fn->span, "MIR function has no valid source HIR function");
    return 0;
  }
  if (!hir_name_eq(fn->module_name, source_info.module_name) ||
      !hir_name_eq(fn->name, source_info.name) ||
      !mir_type_equal(fn->type, source_info.type) ||
      fn->flags != source_info.flags ||
      fn->entry_block != source_info.entry_block ||
      fn->block_count != source_info.block_count ||
      fn->frame_policy != source_info.frame_policy ||
      fn->stack_alignment != source_info.stack_alignment ||
      !mir_text_value_equal(fn->abi, source_info.abi) ||
      !mir_text_value_equal(fn->link_name, source_info.link_name) ||
      !mir_text_value_equal(fn->target_constraint,
                            source_info.target_constraint) ||
      !mir_text_value_equal(fn->requires_constraint,
                            source_info.requires_constraint)) {
    mir_program_diag(
        program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR, fn->span,
        "MIR function metadata does not faithfully preserve its HIR origin");
    ok = 0;
  }
  if (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH)) {
    if (fn->block_count || fn->instruction_count || fn->vreg_count ||
        fn->memory_value_count || fn->stack_slot_count) {
      mir_program_diag(
          program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR, fn->span,
          "extern/dispatch MIR function must not contain a runtime body");
      ok = 0;
    }
    return ok;
  }
  if ((fn->block_count != 0u && !fn->blocks) ||
      (fn->instruction_count != 0u && !fn->instructions) ||
      (fn->vreg_count != 0u && !fn->vregs) ||
      (fn->memory_value_count != 0u && !fn->memory_values) ||
      (fn->stack_slot_count != 0u && !fn->stack_slots)) {
    mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR, fn->span,
                     "MIR function owns an incomplete internal table");
    return 0;
  }
  for (uint32_t i = 0; i < fn->instruction_count; ++i)
    if (fn->instructions[i].info.inline_site) {
      mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                       fn->span,
                       "original MIR cannot claim an inline source site");
      return 0;
    }
  if (fn->entry_block >= fn->block_count ||
      !mir_verify_cfg_metadata(program, fn))
    ok = 0;
  uint8_t *materialized_locals = (uint8_t *)calloc(
      source_info.local_count ? source_info.local_count : 1u, 1u);
  uint8_t *address_taken_locals = (uint8_t *)calloc(
      source_info.local_count ? source_info.local_count : 1u, 1u);
  if (!materialized_locals || !address_taken_locals) {
    free(materialized_locals);
    free(address_taken_locals);
    return 0;
  }
  for (uint32_t ii = 0u; ii < source_info.instruction_count; ++ii) {
    const AstHirInstruction *source_instruction =
        &fn->source_hir->instructions[ii];
    if (source_instruction->op == AST_HIR_OP_LOCAL_ADDR &&
        source_instruction->local < source_info.local_count)
      address_taken_locals[source_instruction->local] |= 1u;
    if (source_instruction->op == AST_HIR_OP_BIND &&
        source_instruction->local < source_info.local_count)
      address_taken_locals[source_instruction->local] |= 2u;
  }
  for (uint32_t si = 0u; si < fn->stack_slot_count; ++si) {
    AstMirStackSlotInfo *slot = &fn->stack_slots[si].info;
    const AstHirLocal *source =
        slot->source_local < source_info.local_count
            ? &fn->source_hir->locals[slot->source_local]
            : NULL;
    int source_valid = source != NULL;
    if (source_valid && materialized_locals[slot->source_local])
      source_valid = 0;
    if (source_valid)
      materialized_locals[slot->source_local] = 1u;
    int address_taken = slot->source_local < source_info.local_count &&
                        (address_taken_locals[slot->source_local] & 1u) != 0u;
    uint32_t expected_flags = 0u;
    if (source_valid) {
      if (source->flags & AST_HIR_LOCAL_MUTABLE)
        expected_flags |= AST_MIR_STACK_SLOT_MUTABLE;
      if (source->flags & AST_HIR_LOCAL_PARAMETER)
        expected_flags |= AST_MIR_STACK_SLOT_PARAMETER;
      if (address_taken)
        expected_flags |= AST_MIR_STACK_SLOT_ADDRESS_TAKEN;
      if (ast_sema_type_size(source->type) == AST_MIR_UNKNOWN_SIZE)
        expected_flags |= AST_MIR_STACK_SLOT_DYNAMIC_SIZE;
    }
    if (slot->id != si || !source_valid ||
        !hir_name_eq(slot->name, source->name) ||
        !mir_type_equal(slot->type, source->type) || slot->alignment == 0u ||
        slot->size != ast_sema_type_size(source->type) ||
        slot->alignment != (ast_sema_type_alignment(source->type)
                                ? ast_sema_type_alignment(source->type)
                                : 1u) ||
        ((slot->size == AST_MIR_UNKNOWN_SIZE) !=
         ((slot->flags & AST_MIR_STACK_SLOT_DYNAMIC_SIZE) != 0u)) ||
        slot->flags != expected_flags) {
      mir_program_diag(
          program, AST_MIR_DIAG_INVALID_STACK_SLOT, AST_DIAG_ERROR, slot->span,
          "MIR stack slot %u is inconsistent with its HIR local", si);
      ok = 0;
    }
  }
  uint32_t expected_promoted_locals = 0u;
  for (uint32_t li = 0u; li < source_info.local_count; ++li) {
    int address_taken = (address_taken_locals[li] & 1u) != 0u;
    int bound = (address_taken_locals[li] & 2u) != 0u;
    int should_materialize =
        (!program->config.promote_locals && !bound) || address_taken;
    if ((materialized_locals[li] != 0u) != (should_materialize != 0)) {
      mir_program_diag(
          program, AST_MIR_DIAG_INVALID_STACK_SLOT, AST_DIAG_ERROR, fn->span,
          "MIR storage decision for HIR local %u is inconsistent", li);
      ok = 0;
    }
    if (!should_materialize)
      expected_promoted_locals++;
  }
  if (fn->promoted_local_count != expected_promoted_locals) {
    mir_program_diag(
        program, AST_MIR_DIAG_INVALID_STACK_SLOT, AST_DIAG_ERROR, fn->span,
        "MIR promoted-local count is inconsistent with HIR storage decisions");
    ok = 0;
  }

  const uint32_t instruction_count = fn->instruction_count;
  const uint32_t vreg_count = fn->vreg_count;
  const uint32_t memory_value_count = fn->memory_value_count;
  AstMirBlockId *instruction_blocks = (AstMirBlockId *)mir_malloc_array(
      instruction_count, sizeof(*instruction_blocks));
  uint32_t *instruction_positions = (uint32_t *)mir_malloc_array(
      instruction_count, sizeof(*instruction_positions));
  uint8_t *seen_instructions =
      (uint8_t *)calloc(instruction_count ? instruction_count : 1u, 1u);
  uint8_t *seen_vregs = (uint8_t *)calloc(vreg_count ? vreg_count : 1u, 1u);
  uint8_t *seen_memory =
      (uint8_t *)calloc(memory_value_count ? memory_value_count : 1u, 1u);
  uint8_t *seen_hir = (uint8_t *)calloc(
      source_info.instruction_count ? source_info.instruction_count : 1u, 1u);
  uint32_t *source_value_uses = (uint32_t *)calloc(
      source_info.value_count ? source_info.value_count : 1u, sizeof(uint32_t));
  uint32_t *source_value_direct_calls = (uint32_t *)calloc(
      source_info.value_count ? source_info.value_count : 1u, sizeof(uint32_t));
  if (!instruction_blocks || !instruction_positions || !seen_instructions ||
      !seen_vregs || !seen_memory || !seen_hir || !source_value_uses ||
      !source_value_direct_calls) {
    free(instruction_blocks);
    free(instruction_positions);
    free(seen_instructions);
    free(seen_vregs);
    free(seen_memory);
    free(seen_hir);
    free(materialized_locals);
    free(source_value_uses);
    free(source_value_direct_calls);
    free(address_taken_locals);
    return 0;
  }
  for (uint32_t ii = 0u; ii < fn->source_hir->instruction_count; ++ii) {
    const AstHirInstruction *source = &fn->source_hir->instructions[ii];
    for (uint32_t oi = 0u; oi < source->operand_count; ++oi) {
      AstHirValueId value = source->operands[oi];
      if (value < source_info.value_count)
        source_value_uses[value]++;
    }
    if (source->op == AST_HIR_OP_CALL && source->operand_count != 0u &&
        mir_verify_hir_value_definition(fn->source_hir, source->operands[0]) &&
        mir_verify_hir_value_definition(fn->source_hir, source->operands[0])
                ->op == AST_HIR_OP_FUNCTION_REF &&
        source->operands[0] < source_info.value_count)
      source_value_direct_calls[source->operands[0]]++;
  }
  for (uint32_t bi = 0u; bi < fn->source_hir->block_count; ++bi) {
    const AstHirTerminator *term = &fn->source_hir->blocks[bi].term;
    if (term->condition_or_value != AST_HIR_INVALID_ID &&
        term->condition_or_value < source_info.value_count)
      source_value_uses[term->condition_or_value]++;
    for (uint32_t ai = 0u; ai < term->true_arg_count; ++ai)
      if (term->true_args[ai] < source_info.value_count)
        source_value_uses[term->true_args[ai]]++;
    for (uint32_t ai = 0u; ai < term->false_arg_count; ++ai)
      if (term->false_args[ai] < source_info.value_count)
        source_value_uses[term->false_args[ai]]++;
  }
  for (uint32_t ii = 0u; ii < instruction_count; ++ii) {
    instruction_blocks[ii] = AST_MIR_INVALID_ID;
    instruction_positions[ii] = 0u;
  }
  uint32_t expected_memory_phis = 0u;
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstMirBlock *block = &fn->blocks[bi];
    if (block->predecessor_count > 1u)
      expected_memory_phis++;
    if ((block->instruction_count != 0u && !block->instructions) ||
        (block->argument_count != 0u &&
         (!block->arguments || !block->argument_source_locals)) ||
        (block->predecessor_count != 0u && !block->predecessors)) {
      mir_program_diag(program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                       block->span,
                       "MIR block %u owns an incomplete internal table", bi);
      ok = 0;
    }
    if (block->term.info.true_args != block->term.true_args ||
        block->term.info.false_args != block->term.false_args) {
      mir_program_diag(
          program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR, block->span,
          "MIR block %u terminator has inconsistent argument storage", bi);
      ok = 0;
    }
    for (uint32_t ii = 0u; block->instructions && ii < block->instruction_count;
         ++ii) {
      AstMirInstId id = block->instructions[ii];
      if (id >= instruction_count || seen_instructions[id]) {
        mir_program_diag(
            program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR, block->span,
            "MIR block contains an invalid or duplicate instruction id");
        ok = 0;
        continue;
      }
      seen_instructions[id] = 1u;
      instruction_blocks[id] = bi;
      instruction_positions[id] = ii;
    }
  }
  for (uint32_t ii = 0u; ii < instruction_count; ++ii) {
    if (!seen_instructions[ii]) {
      mir_program_diag(program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                       fn->instructions[ii].info.span,
                       "MIR instruction %u is not owned by any block", ii);
      ok = 0;
    }
  }

  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstMirBlock *block = &fn->blocks[bi];
    if (bi >= fn->source_hir->block_count) {
      mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                       block->span, "MIR block %u has no matching HIR block",
                       bi);
      ok = 0;
    } else {
      const AstHirBlock *source_block = &fn->source_hir->blocks[bi];
      /* Structural branches carry the original terminator separately.
       * Check HIR against that snapshot; actual CFG gets checked below
       * and independently by the structural verifier. */
      const AstMirTerminatorInfo *source_term =
          fn->branch_certificates && fn->branch_certificates[bi].valid
              ? &fn->branch_certificates[bi].original.info
              : &block->term.info;
      int source_ok =
          block->hir_argument_count == source_block->argument_count &&
          block->argument_count >= block->hir_argument_count &&
          source_term->kind == mir_term_from_hir(source_block->term.kind) &&
          source_term->target_true == source_block->term.target_true &&
          source_term->target_false == source_block->term.target_false &&
          source_term->true_arg_count >= source_block->term.true_arg_count &&
          source_term->false_arg_count >= source_block->term.false_arg_count &&
          (block->hir_argument_count == 0u ||
           (block->arguments && block->argument_source_locals)) &&
          (source_block->term.true_arg_count == 0u || source_term->true_args) &&
          (source_block->term.false_arg_count == 0u || source_term->false_args);
      for (uint32_t ai = 0u;
           block->arguments && block->argument_source_locals &&
           ai < block->hir_argument_count && ai < source_block->argument_count;
           ++ai) {
        AstHirValueId source_value = source_block->arguments[ai];
        if (block->arguments[ai] >= fn->vreg_count ||
            source_value >= fn->source_hir->value_count ||
            !mir_type_equal(fn->vregs[block->arguments[ai]].info.type,
                            fn->source_hir->values[source_value].type) ||
            block->argument_source_locals[ai] != AST_HIR_INVALID_ID)
          source_ok = 0;
      }
      for (uint32_t ai = 0u;
           source_term->true_args && ai < source_block->term.true_arg_count &&
           ai < source_term->true_arg_count;
           ++ai) {
        AstHirValueId source_value = source_block->term.true_args[ai];
        if (source_term->true_args[ai] >= fn->vreg_count ||
            source_value >= fn->source_hir->value_count ||
            !mir_type_equal(fn->vregs[source_term->true_args[ai]].info.type,
                            fn->source_hir->values[source_value].type))
          source_ok = 0;
      }
      for (uint32_t ai = 0u;
           source_term->false_args && ai < source_block->term.false_arg_count &&
           ai < source_term->false_arg_count;
           ++ai) {
        AstHirValueId source_value = source_block->term.false_args[ai];
        if (source_term->false_args[ai] >= fn->vreg_count ||
            source_value >= fn->source_hir->value_count ||
            !mir_type_equal(fn->vregs[source_term->false_args[ai]].info.type,
                            fn->source_hir->values[source_value].type))
          source_ok = 0;
      }
      if (source_block->term.condition_or_value != AST_HIR_INVALID_ID) {
        AstHirValueId source_value = source_block->term.condition_or_value;
        if (source_term->condition_or_value >= fn->vreg_count ||
            source_value >= fn->source_hir->value_count ||
            !mir_type_equal(
                fn->vregs[source_term->condition_or_value].info.type,
                fn->source_hir->values[source_value].type))
          source_ok = 0;
      } else if (source_term->condition_or_value != AST_MIR_INVALID_ID) {
        source_ok = 0;
      }
      if (!source_ok) {
        mir_program_diag(
            program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR, block->span,
            "MIR block %u does not faithfully preserve its HIR control flow",
            bi);
        ok = 0;
      }
    }
    if (block->term.info.kind == AST_MIR_TERM_NONE) {
      mir_program_diag(program, AST_MIR_DIAG_MISSING_TERMINATOR, AST_DIAG_ERROR,
                       block->span, "MIR basic block %u has no terminator", bi);
      ok = 0;
    }
    if (block->memory_argument >= memory_value_count) {
      mir_program_diag(program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                       AST_DIAG_ERROR, block->span,
                       "MIR block %u has an invalid memory argument", bi);
      ok = 0;
    } else {
      AstMirMemoryInfo *memory =
          &fn->memory_values[block->memory_argument].info;
      AstMirMemoryKind expected = bi == fn->entry_block
                                      ? AST_MIR_MEMORY_ENTRY
                                      : AST_MIR_MEMORY_BLOCK_ARGUMENT;
      uint32_t expected_incoming =
          bi == fn->entry_block ? 0u : block->predecessor_count;
      if (memory->kind != expected || memory->block != bi ||
          memory->definition_index != 0u ||
          memory->incoming_count != expected_incoming ||
          (expected_incoming != 0u && !memory->incoming)) {
        mir_program_diag(program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                         AST_DIAG_ERROR, block->span,
                         "MIR block %u memory argument has invalid metadata",
                         bi);
        ok = 0;
      }
      for (uint32_t pi = 0u;
           memory->incoming && block->predecessors &&
           pi < memory->incoming_count && pi < block->predecessor_count;
           ++pi) {
        AstMirBlockId predecessor = block->predecessors[pi];
        if (memory->incoming[pi] >= memory_value_count ||
            predecessor >= fn->block_count ||
            memory->incoming[pi] != fn->blocks[predecessor].term.info.memory) {
          mir_program_diag(
              program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN, AST_DIAG_ERROR,
              block->span,
              "MIR block %u memory input %u does not match predecessor %u", bi,
              pi, predecessor);
          ok = 0;
        }
      }
      seen_memory[block->memory_argument] = 1u;
    }
    for (uint32_t ai = 0u; block->arguments && block->argument_source_locals &&
                           ai < block->argument_count;
         ++ai) {
      AstMirVReg value = block->arguments[ai];
      if (value >= vreg_count || seen_vregs[value]) {
        mir_program_diag(
            program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR, block->span,
            "MIR block %u argument %u is invalid or multiply defined", bi, ai);
        ok = 0;
        continue;
      }
      AstMirVRegInfo *info = &fn->vregs[value].info;
      if (info->kind != AST_MIR_VREG_BLOCK_ARGUMENT || info->block != bi ||
          info->definition_index != ai ||
          info->bank != mir_type_bank(info->type) ||
          info->bit_width != mir_type_bit_width(info->type) || !info->type) {
        mir_program_diag(program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR,
                         info->span,
                         "MIR block argument virtual register %u has invalid "
                         "definition metadata",
                         value);
        ok = 0;
      }
      if (block->argument_source_locals[ai] == AST_HIR_INVALID_ID) {
        if (info->flags & AST_MIR_VREG_PHI) {
          mir_program_diag(
              program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR, info->span,
              "MIR HIR block argument %u is incorrectly marked as a local phi",
              value);
          ok = 0;
        }
      } else if (!(info->flags & AST_MIR_VREG_PHI) ||
                 block->argument_source_locals[ai] >= source_info.local_count) {
        mir_program_diag(program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR,
                         info->span,
                         "MIR local phi virtual register %u has invalid "
                         "source-local metadata",
                         value);
        ok = 0;
      }
      seen_vregs[value] = 1u;
    }
    AstMirMemoryId current_memory = block->memory_argument;
    for (uint32_t position = 0u;
         block->instructions && position < block->instruction_count;
         ++position) {
      AstMirInstId id = block->instructions[position];
      if (id >= instruction_count)
        continue;
      AstMirInstruction *inst = &fn->instructions[id];
      if (inst->info.id != id) {
        mir_program_diag(
            program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR, inst->info.span,
            "MIR instruction table id does not match block instruction id %u",
            id);
        ok = 0;
      }
      ok &= mir_verify_instruction_shape(program, fn, inst);
      if (fn->gvn_certificates && fn->gvn_certificates[id].valid)
        ok &= mir_verify_gvn_certificate(program, fn, inst, instruction_blocks,
                                         instruction_positions);
      else
        ok &= mir_verify_source_instruction(program, fn, inst);
      if (inst->info.source_hir_instruction != AST_HIR_INVALID_ID &&
          inst->info.source_hir_instruction < source_info.instruction_count) {
        if (seen_hir[inst->info.source_hir_instruction]) {
          mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                           inst->info.span,
                           "multiple MIR instructions claim HIR instruction %u",
                           inst->info.source_hir_instruction);
          ok = 0;
        }
        seen_hir[inst->info.source_hir_instruction] = 1u;
      }
      for (uint32_t oi = 0u; inst->operands && oi < inst->info.operand_count;
           ++oi)
        ok &= mir_verify_vreg_use(program, fn, bi, position + 1u,
                                  inst->operands[oi], inst->info.span,
                                  instruction_blocks, instruction_positions);
      if (inst->info.result != AST_MIR_INVALID_ID) {
        if (inst->info.result >= vreg_count || seen_vregs[inst->info.result]) {
          mir_program_diag(
              program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR,
              inst->info.span,
              "MIR instruction %u result is invalid or multiply defined", id);
          ok = 0;
        } else {
          AstMirVRegInfo *result = &fn->vregs[inst->info.result].info;
          if (result->kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
              result->block != bi || result->definition_index != id ||
              !mir_type_equal(result->type, inst->info.type) ||
              result->bank != mir_type_bank(result->type) ||
              result->bit_width != mir_type_bit_width(result->type)) {
            mir_program_diag(program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR,
                             result->span,
                             "MIR instruction %u result virtual register has "
                             "invalid metadata",
                             id);
            ok = 0;
          }
          seen_vregs[inst->info.result] = 1u;
        }
      }
      if (mir_instruction_is_effectful(inst)) {
        if (inst->info.memory_in != current_memory ||
            inst->info.memory_out >= memory_value_count) {
          mir_program_diag(program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                           AST_DIAG_ERROR, inst->info.span,
                           "MIR effect chain is broken at instruction %u", id);
          ok = 0;
        } else {
          AstMirMemoryInfo *memory =
              &fn->memory_values[inst->info.memory_out].info;
          if (memory->kind != AST_MIR_MEMORY_INSTRUCTION_RESULT ||
              memory->block != bi || memory->definition_index != id ||
              memory->incoming_count != 0u || memory->incoming != NULL ||
              seen_memory[inst->info.memory_out]) {
            mir_program_diag(program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                             AST_DIAG_ERROR, inst->info.span,
                             "MIR instruction %u memory result has invalid "
                             "definition metadata",
                             id);
            ok = 0;
          }
          seen_memory[inst->info.memory_out] = 1u;
          current_memory = inst->info.memory_out;
        }
      } else if (inst->info.memory_in != AST_MIR_INVALID_ID ||
                 inst->info.memory_out != AST_MIR_INVALID_ID) {
        mir_program_diag(program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                         AST_DIAG_ERROR, inst->info.span,
                         "pure MIR instruction %u unexpectedly participates in "
                         "the memory chain",
                         id);
        ok = 0;
      }
    }
    if (block->term.info.memory != current_memory) {
      mir_program_diag(
          program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN, AST_DIAG_ERROR,
          block->term.info.span,
          "MIR block %u terminator does not consume the final memory value",
          bi);
      ok = 0;
    }
    uint32_t term_position = block->instruction_count + 1u;
    if (block->term.info.condition_or_value != AST_MIR_INVALID_ID)
      ok &= mir_verify_vreg_use(
          program, fn, bi, term_position, block->term.info.condition_or_value,
          block->term.info.span, instruction_blocks, instruction_positions);
    if (block->term.info.kind == AST_MIR_TERM_BR) {
      ok &= mir_verify_branch(
          program, fn, bi, block->term.info.target_true, block->term.true_args,
          block->term.info.true_arg_count, block->term.info.span, term_position,
          instruction_blocks, instruction_positions);
    } else if (block->term.info.kind == AST_MIR_TERM_COND_BR) {
      if (block->term.info.condition_or_value >= vreg_count ||
          ast_sema_type_kind(
              fn->vregs[block->term.info.condition_or_value].info.type) !=
              AST_SEMA_TYPE_BOOL) {
        mir_program_diag(program, AST_MIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                         block->term.info.span,
                         "MIR conditional branch condition is not bool");
        ok = 0;
      }
      ok &= mir_verify_branch(
          program, fn, bi, block->term.info.target_true, block->term.true_args,
          block->term.info.true_arg_count, block->term.info.span, term_position,
          instruction_blocks, instruction_positions);
      ok &= mir_verify_branch(program, fn, bi, block->term.info.target_false,
                              block->term.false_args,
                              block->term.info.false_arg_count,
                              block->term.info.span, term_position,
                              instruction_blocks, instruction_positions);
    } else if (block->term.info.kind == AST_MIR_TERM_RETURN) {
      const AstSemaType *result_type = ast_sema_type_function_result(fn->type);
      if (result_type &&
          ast_sema_type_kind(result_type) == AST_SEMA_TYPE_VOID) {
        if (block->term.info.condition_or_value != AST_MIR_INVALID_ID) {
          mir_program_diag(program, AST_MIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                           block->term.info.span,
                           "MIR void function returns a value");
          ok = 0;
        }
      } else if (!result_type ||
                 block->term.info.condition_or_value >= vreg_count ||
                 !mir_type_equal(result_type,
                                 fn->vregs[block->term.info.condition_or_value]
                                     .info.type)) {
        mir_program_diag(
            program, AST_MIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
            block->term.info.span,
            "MIR return value type does not match function result type");
        ok = 0;
      }
    }
  }
  if (fn->memory_phi_count != expected_memory_phis) {
    mir_program_diag(program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN, AST_DIAG_ERROR,
                     fn->span,
                     "MIR memory phi count is inconsistent with the CFG");
    ok = 0;
  }
  for (uint32_t ii = 0u; ii < source_info.instruction_count; ++ii) {
    const AstHirInstruction *source_instruction =
        &fn->source_hir->instructions[ii];
    int eliminated_promoted_local =
        (source_instruction->op == AST_HIR_OP_LOCAL_LOAD ||
         source_instruction->op == AST_HIR_OP_LOCAL_STORE) &&
        source_instruction->local < source_info.local_count &&
        !materialized_locals[source_instruction->local];
    int eliminated_direct_function_ref = 0;
    if (source_instruction->op == AST_HIR_OP_FUNCTION_REF &&
        source_instruction->result < source_info.value_count) {
      uint32_t direct = source_value_direct_calls[source_instruction->result];
      eliminated_direct_function_ref =
          direct != 0u &&
          source_value_uses[source_instruction->result] == direct;
    }
    int eliminated_portable_intrinsic = 0;
    if (source_instruction->op == AST_HIR_OP_INTRINSIC) {
      switch (source_instruction->intrinsic) {
      case AST_HIR_INTRINSIC_CLZ:
      case AST_HIR_INTRINSIC_CTZ:
      case AST_HIR_INTRINSIC_POPCNT:
      case AST_HIR_INTRINSIC_BSWAP:
      case AST_HIR_INTRINSIC_BITREV:
      case AST_HIR_INTRINSIC_ROL:
      case AST_HIR_INTRINSIC_ROR:
      case AST_HIR_INTRINSIC_ADD_SAT:
      case AST_HIR_INTRINSIC_SUB_SAT:
      case AST_HIR_INTRINSIC_ADD_CHECKED:
      case AST_HIR_INTRINSIC_SUB_CHECKED:
      case AST_HIR_INTRINSIC_MUL_CHECKED:
        eliminated_portable_intrinsic = 1;
        break;
      default:
        break;
      }
    }
    if (!eliminated_promoted_local && !eliminated_direct_function_ref &&
        !eliminated_portable_intrinsic && !seen_hir[ii]) {
      mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                       source_instruction->span,
                       "HIR instruction %u has no MIR lowering", ii);
      ok = 0;
    }
  }
  for (uint32_t vi = 0u; vi < vreg_count; ++vi)
    if (!seen_vregs[vi]) {
      mir_program_diag(
          program, AST_MIR_DIAG_INVALID_VREG, AST_DIAG_ERROR, fn->span,
          "MIR virtual register %u has no canonical definition", vi);
      ok = 0;
    }
  for (uint32_t mi = 0u; mi < memory_value_count; ++mi)
    if (!seen_memory[mi]) {
      mir_program_diag(program, AST_MIR_DIAG_INVALID_MEMORY_CHAIN,
                       AST_DIAG_ERROR, fn->span,
                       "MIR memory value %u has no canonical definition", mi);
      ok = 0;
    }
  if (ok)
    ok &= mir_verify_canonical_liveness(program, fn);
  free(instruction_blocks);
  free(instruction_positions);
  free(seen_instructions);
  free(seen_vregs);
  free(seen_memory);
  free(seen_hir);
  free(source_value_uses);
  free(source_value_direct_calls);
  free(materialized_locals);
  free(address_taken_locals);
  return ok;
}

int ast_mir_verify(AstMirProgram *program) {
  if (!program)
    return 0;
  while (program->diagnostic_count > program->build_diagnostic_count) {
    size_t index = --program->diagnostic_count;
    free((void *)program->diagnostics[index].message);
    memset(&program->diagnostics[index], 0,
           sizeof(program->diagnostics[index]));
  }
  if (program->inline_proof) {
    int verified = mir_verify_inlining(program);
    mir_refresh_stats_after_verify(program);
    return verified && !program->stats.error_count && !program->oom;
  }
  int ok =
      program->hir != NULL && program->diagnostic_count == 0u && !program->oom;
  if (!program->hir) {
    mir_program_diag(program, AST_MIR_DIAG_HIR_REQUIRED, AST_DIAG_ERROR,
                     (AstSpan){0}, "MIR has no owning HIR program");
    ok = 0;
  }
  size_t expected_functions = 0u;
  if (program->hir) {
    size_t hir_count = ast_hir_function_count(program->hir);
    for (size_t i = 0u; i < hir_count; ++i) {
      const AstHirFunction *source = ast_hir_function_at(program->hir, i);
      if (source && !(source->flags & AST_HIR_FUNCTION_GENERIC))
        expected_functions++;
    }
  }
  if (program->function_count != expected_functions) {
    mir_program_diag(
        program, AST_MIR_DIAG_INVALID_CFG, AST_DIAG_ERROR, (AstSpan){0},
        "MIR function table does not match executable HIR functions");
    ok = 0;
  }
  for (size_t fi = 0u; fi < program->function_count; ++fi) {
    if (program->functions[fi] && program->functions[fi]->optimization_proof) {
      if (!mir_verify_optimization(program, program->functions[fi]))
        ok = 0;
    } else if (!program->functions[fi] ||
               !mir_verify_function(program, program->functions[fi]))
      ok = 0;
    else if (!mir_branch_verify_certificates(program, program->functions[fi]))
      ok = 0;
  }
  mir_refresh_stats_after_verify(program);
  return ok && program->stats.error_count == 0u;
}

/* Never let the optimizer claim PRE provenance for a previously rewritten
 * module.  POST performs the full canonical verifier including certificates;
 * it is not a weaker IR mode. */
int ast_mir_verify_mode(AstMirProgram *program, AstMirVerifyMode mode) {
  if (!program ||
      (mode != AST_MIR_VERIFY_PRE_OPT && mode != AST_MIR_VERIFY_POST_OPT))
    return 0;
  if (!ast_mir_verify(program))
    return 0;
  if (mode == AST_MIR_VERIFY_POST_OPT) {
    if (!ast_mir_verify_structural(program))
      return 0;
    for (size_t fi = 0; fi < program->function_count; ++fi)
      if (!mir_sccp_verify_certificates(program, program->functions[fi]))
        return 0;
  }
  if (mode == AST_MIR_VERIFY_PRE_OPT) {
    if (program->inline_proof)
      return 0;
    for (size_t fi = 0u; fi < program->function_count; ++fi) {
      if (program->functions[fi]->gvn_certificates ||
          program->functions[fi]->branch_certificates ||
          program->functions[fi]->optimization_proof) {
        mir_program_diag(
            program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
            program->functions[fi]->span,
            "PRE-OPT MIR verifier rejects post-optimization certificates");
        return 0;
      }
    }
  }
  return 1;
}
