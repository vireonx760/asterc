#include "hir_internal.h"

static int hir_verify_name_is(AstName name, const char *text) {
  size_t n = text ? strlen(text) : 0u;
  return name.text && name.span.length == n && memcmp(name.text, text, n) == 0;
}

static int hir_memory_order_valid(AstMemoryOrder order) {
  return order >= AST_MEMORY_ORDER_RELAXED && order <= AST_MEMORY_ORDER_SEQ_CST;
}

static int hir_atomic_order_shape_valid(const AstHirInstruction *inst) {
  if (!inst || !hir_memory_order_valid(inst->memory_order))
    return 0;
  if (hir_verify_name_is(inst->symbol, "cas")) {
    if (!hir_memory_order_valid(inst->failure_memory_order) ||
        inst->failure_memory_order == AST_MEMORY_ORDER_RELEASE ||
        inst->failure_memory_order == AST_MEMORY_ORDER_ACQ_REL)
      return 0;
  } else if (inst->failure_memory_order != AST_MEMORY_ORDER_NONE)
    return 0;
  if (hir_verify_name_is(inst->symbol, "load") &&
      (inst->memory_order == AST_MEMORY_ORDER_RELEASE ||
       inst->memory_order == AST_MEMORY_ORDER_ACQ_REL))
    return 0;
  if (hir_verify_name_is(inst->symbol, "store") &&
      (inst->memory_order == AST_MEMORY_ORDER_ACQUIRE ||
       inst->memory_order == AST_MEMORY_ORDER_ACQ_REL))
    return 0;
  return 1;
}

static uint32_t block_successors(const AstHirBlock *b, AstHirBlockId out[2]) {
  if (!b)
    return 0u;
  switch (b->term.kind) {
  case AST_HIR_TERM_BR:
    out[0] = b->term.target_true;
    return 1u;
  case AST_HIR_TERM_COND_BR:
    out[0] = b->term.target_true;
    out[1] = b->term.target_false;
    return 2u;
  default:
    return 0u;
  }
}

static int dfs_rpo(AstHirFunction *fn, AstHirBlockId entry,
                   AstHirBlockId *rpo_out, uint32_t *rpo_count_out) {
  if (!fn || entry >= fn->block_count)
    return 0;
  typedef struct Frame {
    AstHirBlockId block;
    uint8_t next;
  } Frame;
  uint8_t *seen = (uint8_t *)calloc(fn->block_count ? fn->block_count : 1u, 1u);
  Frame *stack = (Frame *)malloc(
      (size_t)(fn->block_count ? fn->block_count : 1u) * sizeof(*stack));
  AstHirBlockId *post = (AstHirBlockId *)malloc(
      (size_t)(fn->block_count ? fn->block_count : 1u) * sizeof(*post));
  if (!seen || !stack || !post) {
    free(seen);
    free(stack);
    free(post);
    return 0;
  }
  uint32_t sp = 0u, post_count = 0u;
  stack[sp++] = (Frame){entry, 0u};
  seen[entry] = 1u;
  while (sp) {
    Frame *f = &stack[sp - 1u];
    AstHirBlockId succ[2];
    uint32_t count = block_successors(&fn->blocks[f->block], succ);
    if (f->next < count) {
      AstHirBlockId s = succ[f->next++];
      if (s < fn->block_count && !seen[s]) {
        seen[s] = 1u;
        stack[sp++] = (Frame){s, 0u};
      }
      continue;
    }
    post[post_count++] = f->block;
    sp--;
  }
  for (uint32_t i = 0u; i < post_count; ++i)
    rpo_out[i] = post[post_count - 1u - i];
  *rpo_count_out = post_count;
  free(seen);
  free(stack);
  free(post);
  return 1;
}

static AstHirBlockId intersect_idom(const AstHirBlockId *idom,
                                    const uint32_t *rpo_index, AstHirBlockId a,
                                    AstHirBlockId b) {
  while (a != b) {
    while (rpo_index[a] > rpo_index[b])
      a = idom[a];
    while (rpo_index[b] > rpo_index[a])
      b = idom[b];
  }
  return a;
}

int hir_analyze_cfg(AstHirFunction *fn) {
  if (!fn)
    return 0;
  for (uint32_t i = 0u; i < fn->block_count; ++i) {
    fn->blocks[i].predecessor_count = 0u;
    fn->blocks[i].successor_count = 0u;
    fn->blocks[i].immediate_dominator = AST_HIR_INVALID_ID;
    fn->blocks[i].reverse_postorder = UINT32_MAX;
    fn->blocks[i].reachable = 0u;
  }
  if (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH))
    return 1;
  if (fn->entry_block >= fn->block_count)
    return 0;

  for (uint32_t i = 0u; i < fn->block_count; ++i) {
    AstHirBlockId succ[2];
    uint32_t count = block_successors(&fn->blocks[i], succ);
    fn->blocks[i].successor_count = count;
    for (uint32_t j = 0u; j < count; ++j)
      if (succ[j] < fn->block_count)
        fn->blocks[succ[j]].predecessor_count++;
  }

  AstHirBlockId *rpo =
      (AstHirBlockId *)malloc((size_t)fn->block_count * sizeof(*rpo));
  uint32_t rpo_count = 0u;
  if (!rpo || !dfs_rpo(fn, fn->entry_block, rpo, &rpo_count)) {
    free(rpo);
    return 0;
  }
  uint32_t *rpo_index =
      (uint32_t *)malloc((size_t)fn->block_count * sizeof(*rpo_index));
  AstHirBlockId *idom =
      (AstHirBlockId *)malloc((size_t)fn->block_count * sizeof(*idom));
  uint32_t *offsets =
      (uint32_t *)calloc((size_t)fn->block_count + 1u, sizeof(*offsets));
  uint32_t *cursor = (uint32_t *)calloc(fn->block_count ? fn->block_count : 1u,
                                        sizeof(*cursor));
  if (!rpo_index || !idom || !offsets || !cursor) {
    free(rpo);
    free(rpo_index);
    free(idom);
    free(offsets);
    free(cursor);
    return 0;
  }
  for (uint32_t i = 0u; i < fn->block_count; ++i) {
    rpo_index[i] = UINT32_MAX;
    idom[i] = AST_HIR_INVALID_ID;
  }
  for (uint32_t i = 0u; i < rpo_count; ++i) {
    rpo_index[rpo[i]] = i;
    fn->blocks[rpo[i]].reverse_postorder = i;
    fn->blocks[rpo[i]].reachable = 1u;
  }
  uint32_t edge_count = 0u;
  for (uint32_t i = 0u; i < fn->block_count; ++i) {
    offsets[i] = edge_count;
    edge_count += fn->blocks[i].predecessor_count;
  }
  offsets[fn->block_count] = edge_count;
  uint32_t *preds = (uint32_t *)malloc((size_t)(edge_count ? edge_count : 1u) *
                                       sizeof(*preds));
  if (!preds) {
    free(rpo);
    free(rpo_index);
    free(idom);
    free(offsets);
    free(cursor);
    return 0;
  }
  for (uint32_t i = 0u; i < fn->block_count; ++i)
    cursor[i] = offsets[i];
  for (uint32_t i = 0u; i < fn->block_count; ++i) {
    AstHirBlockId succ[2];
    uint32_t count = block_successors(&fn->blocks[i], succ);
    for (uint32_t j = 0u; j < count; ++j)
      if (succ[j] < fn->block_count)
        preds[cursor[succ[j]]++] = i;
  }

  idom[fn->entry_block] = fn->entry_block;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (uint32_t ri = 1u; ri < rpo_count; ++ri) {
      AstHirBlockId block = rpo[ri];
      AstHirBlockId next_idom = AST_HIR_INVALID_ID;
      for (uint32_t pi = offsets[block]; pi < offsets[block + 1u]; ++pi) {
        AstHirBlockId pred = preds[pi];
        if (idom[pred] == AST_HIR_INVALID_ID)
          continue;
        if (next_idom == AST_HIR_INVALID_ID)
          next_idom = pred;
        else
          next_idom = intersect_idom(idom, rpo_index, pred, next_idom);
      }
      if (next_idom != AST_HIR_INVALID_ID && idom[block] != next_idom) {
        idom[block] = next_idom;
        changed = 1;
      }
    }
  }
  for (uint32_t i = 0u; i < fn->block_count; ++i) {
    if (!fn->blocks[i].reachable || i == fn->entry_block)
      fn->blocks[i].immediate_dominator = AST_HIR_INVALID_ID;
    else
      fn->blocks[i].immediate_dominator = idom[i];
  }
  free(rpo);
  free(rpo_index);
  free(idom);
  free(offsets);
  free(cursor);
  free(preds);
  return 1;
}

static int block_dominates(const AstHirFunction *fn, AstHirBlockId dom,
                           AstHirBlockId block) {
  if (dom == block)
    return 1;
  if (!fn || dom >= fn->block_count || block >= fn->block_count ||
      !fn->blocks[block].reachable)
    return 0;
  AstHirBlockId cur = fn->blocks[block].immediate_dominator;
  uint32_t steps = 0u;
  while (cur != AST_HIR_INVALID_ID && steps++ <= fn->block_count) {
    if (cur == dom)
      return 1;
    cur = fn->blocks[cur].immediate_dominator;
  }
  return 0;
}

static int verify_branch_args(AstHirProgram *program, AstHirFunction *fn,
                              AstSpan span, AstHirBlockId target,
                              const AstHirValueId *args, uint32_t count) {
  if (target >= fn->block_count) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_SUCCESSOR, AST_DIAG_ERROR,
                     span, "branch target block is out of range");
    return 0;
  }
  AstHirBlock *dst = &fn->blocks[target];
  if (dst->argument_count != count) {
    hir_program_diag(
        program, AST_HIR_DIAG_BRANCH_ARGUMENT_MISMATCH, AST_DIAG_ERROR, span,
        "branch supplies %u arguments but target block requires %u", count,
        dst->argument_count);
    return 0;
  }
  int ok = 1;
  for (uint32_t i = 0u; i < count; ++i) {
    if (args[i] >= fn->value_count) {
      ok = 0;
      continue;
    }
    const AstSemaType *src = fn->values[args[i]].type;
    const AstSemaType *want = fn->values[dst->arguments[i]].type;
    if (!hir_type_equal(src, want)) {
      hir_program_diag(
          program, AST_HIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, span,
          "branch argument %u does not match target block argument type", i);
      ok = 0;
    }
  }
  return ok;
}

static int verify_operand_dominance(AstHirProgram *program, AstHirFunction *fn,
                                    AstHirBlockId use_block, uint32_t use_pos,
                                    AstHirValueId operand, AstSpan span,
                                    const uint32_t *inst_block,
                                    const uint32_t *inst_pos) {
  if (operand >= fn->value_count) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_OPERAND, AST_DIAG_ERROR,
                     span, "HIR operand value is out of range");
    return 0;
  }
  const AstHirValue *v = &fn->values[operand];
  AstHirBlockId def_block = v->block;
  if (def_block >= fn->block_count) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_OPERAND, AST_DIAG_ERROR,
                     span, "HIR value has invalid defining block");
    return 0;
  }
  if (def_block == use_block) {
    if (v->kind == AST_HIR_VALUE_BLOCK_ARGUMENT)
      return 1;
    if (v->definition_index >= fn->instruction_count)
      return 0;
    uint32_t pos = inst_pos[v->definition_index];
    if (pos < use_pos)
      return 1;
    hir_program_diag(
        program, AST_HIR_DIAG_DOMINANCE, AST_DIAG_ERROR, span,
        "HIR value is used before its definition in the same block");
    return 0;
  }
  if (!fn->blocks[use_block].reachable || !fn->blocks[def_block].reachable)
    return 1;
  if (v->kind == AST_HIR_VALUE_INSTRUCTION_RESULT &&
      inst_block[v->definition_index] != def_block)
    return 0;
  if (!block_dominates(fn, def_block, use_block)) {
    hir_program_diag(program, AST_HIR_DIAG_DOMINANCE, AST_DIAG_ERROR, span,
                     "HIR value definition does not dominate its use");
    return 0;
  }
  return 1;
}

static int verify_operand_count(AstHirProgram *program,
                                const AstHirInstruction *inst, uint32_t minimum,
                                uint32_t maximum) {
  if (inst->operand_count >= minimum && inst->operand_count <= maximum)
    return 1;
  hir_program_diag(
      program, AST_HIR_DIAG_INVALID_OPERAND, AST_DIAG_ERROR, inst->span,
      "HIR instruction '%s' has %u operands; expected %u..%u",
      ast_hir_op_name(inst->op), inst->operand_count, minimum, maximum);
  return 0;
}

static int verify_native_expr(AstHirProgram *program,
                              const AstHirNativeRegion *region,
                              const AstHirNativeExpr *expr, uint32_t depth) {
  if (!expr) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_NATIVE_REGION,
                     AST_DIAG_ERROR, region->span,
                     "canonical native expression is missing");
    return 0;
  }
  if (depth > program->config.recursion_limit) {
    hir_program_diag(program, AST_HIR_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR,
                     expr->span,
                     "canonical native expression exceeds HIR recursion limit");
    return 0;
  }
  int ok = 1;
  uint32_t expected_min = 0u, expected_max = 0u;
  switch (expr->kind) {
  case AST_HIR_NATIVE_EXPR_NAME:
    if (!expr->name.text || expr->name.span.length == 0u)
      ok = 0;
    if (expr->binding_kind == AST_HIR_NATIVE_BINDING_INPUT) {
      if (expr->binding_index >= region->input_count ||
          !hir_name_eq(expr->name, region->inputs[expr->binding_index]))
        ok = 0;
    } else if (expr->binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT) {
      if (expr->binding_index >= region->output_count ||
          !hir_name_eq(expr->name, region->outputs[expr->binding_index]))
        ok = 0;
    } else if (expr->binding_index != UINT32_MAX)
      ok = 0;
    break;
  case AST_HIR_NATIVE_EXPR_CONSTANT:
    if (expr->constant.kind == AST_SEMA_CONST_NONE)
      ok = 0;
    break;
  case AST_HIR_NATIVE_EXPR_LITERAL:
    if (!expr->literal.data && expr->literal.length != 0u)
      ok = 0;
    if (!expr->literal.data)
      ok = 0;
    switch (expr->token_kind) {
    case AST_TOK_INTEGER:
    case AST_TOK_FLOAT:
    case AST_TOK_CHAR:
    case AST_TOK_KW_TRUE:
    case AST_TOK_KW_FALSE:
    case AST_TOK_STRING:
    case AST_TOK_BYTE_STRING:
    case AST_TOK_C_STRING:
      break;
    default:
      ok = 0;
      break;
    }
    break;
  case AST_HIR_NATIVE_EXPR_TYPE:
    if (!expr->type)
      ok = 0;
    break;
  case AST_HIR_NATIVE_EXPR_UNARY:
    expected_min = expected_max = 1u;
    break;
  case AST_HIR_NATIVE_EXPR_BINARY:
    expected_min = expected_max = 2u;
    break;
  case AST_HIR_NATIVE_EXPR_CONDITIONAL:
    expected_min = expected_max = 3u;
    break;
  case AST_HIR_NATIVE_EXPR_CALL:
    expected_min = 1u;
    expected_max = UINT32_MAX;
    break;
  case AST_HIR_NATIVE_EXPR_INDEX:
    expected_min = expected_max = 2u;
    break;
  case AST_HIR_NATIVE_EXPR_MEMBER:
    expected_min = expected_max = 1u;
    break;
  case AST_HIR_NATIVE_EXPR_LOAD:
    expected_min = expected_max = 1u;
    break;
  case AST_HIR_NATIVE_EXPR_CAST:
    expected_min = expected_max = 1u;
    break;
  case AST_HIR_NATIVE_EXPR_ATOMIC:
    expected_min = 1u;
    expected_max = UINT32_MAX;
    break;
  case AST_HIR_NATIVE_EXPR_ATTRIBUTE:
    expected_min = 1u;
    expected_max = UINT32_MAX;
    break;
  case AST_HIR_NATIVE_EXPR_INVALID:
  default:
    ok = 0;
    break;
  }
  if (expected_max != 0u &&
      (expr->child_count < expected_min || expr->child_count > expected_max))
    ok = 0;
  if (expected_max == 0u && expr->child_count != 0u)
    ok = 0;
  for (uint32_t i = 0u; i < expr->child_count; ++i) {
    if (!verify_native_expr(program, region,
                            expr->children ? expr->children[i] : NULL,
                            depth + 1u))
      ok = 0;
  }
  if (!ok) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_NATIVE_REGION,
                     AST_DIAG_ERROR, expr->span,
                     "invalid canonical native expression");
  }
  return ok;
}

static int verify_native_region(AstHirProgram *program,
                                const AstHirFunction *fn,
                                const AstHirNativeRegion *region) {
  if (!program || !fn || !region)
    return 0;
  int ok = 1;
  if (!region->arch.text || region->arch.span.length == 0u)
    ok = 0;
  if ((region->flags & AST_HIR_NATIVE_UNSAFE_EXACT) &&
      !(region->flags & AST_HIR_NATIVE_EXACT))
    ok = 0;
  for (uint32_t i = 0u; i < region->input_count; ++i) {
    if (!region->inputs || !region->inputs[i].text)
      ok = 0;
    for (uint32_t j = 0u; j < i; ++j)
      if (hir_name_eq(region->inputs[i], region->inputs[j]))
        ok = 0;
  }
  for (uint32_t i = 0u; i < region->output_count; ++i) {
    if (!region->outputs || !region->outputs[i].text)
      ok = 0;
    for (uint32_t j = 0u; j < i; ++j)
      if (hir_name_eq(region->outputs[i], region->outputs[j]))
        ok = 0;
  }
  for (uint32_t si = 0u; si < region->statement_count; ++si) {
    const AstHirNativeStmtCanonical *stmt =
        region->statements ? &region->statements[si] : NULL;
    if (!stmt) {
      ok = 0;
      break;
    }
    if (stmt->kind == AST_HIR_NATIVE_BYTES) {
      if (!(region->flags & AST_HIR_NATIVE_UNSAFE_EXACT) ||
          stmt->byte_count == 0u || !stmt->bytes)
        ok = 0;
      continue;
    }
    if (stmt->kind != AST_HIR_NATIVE_INSTRUCTION || !stmt->mnemonic.text ||
        stmt->mnemonic.span.length == 0u) {
      ok = 0;
      continue;
    }
    for (uint32_t oi = 0u; oi < stmt->operand_count; ++oi) {
      const AstHirNativeOperand *op =
          stmt->operands ? &stmt->operands[oi] : NULL;
      if (!op || !op->name.text || op->name.span.length == 0u) {
        ok = 0;
        continue;
      }
      for (uint32_t oj = 0u; oj < oi; ++oj) {
        if (hir_name_eq(op->name, stmt->operands[oj].name))
          ok = 0;
      }
      if (op->is_memory) {
        if (!op->memory_type || !op->memory_address || op->value)
          ok = 0;
        if (op->memory_address &&
            !verify_native_expr(program, region, op->memory_address, 0u))
          ok = 0;
      } else {
        if (!op->value || op->memory_address || op->memory_type)
          ok = 0;
        if (op->value && !verify_native_expr(program, region, op->value, 0u))
          ok = 0;
      }
    }
    for (uint32_t ai = 0u; ai < stmt->attribute_count; ++ai) {
      const AstHirNativeAttribute *attr =
          stmt->attributes ? &stmt->attributes[ai] : NULL;
      if (!attr || !attr->name.text) {
        ok = 0;
        continue;
      }
      for (uint32_t j = 0u; j < attr->arg_count; ++j) {
        if (!verify_native_expr(program, region,
                                attr->args ? attr->args[j] : NULL, 0u))
          ok = 0;
      }
    }
  }
  if (!ok) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_NATIVE_REGION,
                     AST_DIAG_ERROR, region->span,
                     "invalid canonical native/exact region");
  }
  return ok;
}

static int verify_initializer(AstHirProgram *program,
                              const AstHirInitializer *init,
                              const AstSemaType *expected, uint32_t depth) {
  if (!init)
    return 1;
  if (depth > program->config.recursion_limit) {
    hir_program_diag(
        program, AST_HIR_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR, init->span,
        "canonical global initializer exceeds HIR recursion limit");
    return 0;
  }
  int ok = 1;
  if (!init->type || (expected && !hir_type_equal(init->type, expected)))
    ok = 0;
  switch (init->kind) {
  case AST_HIR_INIT_ZERO:
    if (init->child_count != 0u)
      ok = 0;
    break;
  case AST_HIR_INIT_CONSTANT:
    if (init->constant.kind == AST_SEMA_CONST_NONE || init->child_count != 0u)
      ok = 0;
    break;
  case AST_HIR_INIT_LITERAL:
    if ((init->literal_kind != AST_TOK_STRING &&
         init->literal_kind != AST_TOK_BYTE_STRING &&
         init->literal_kind != AST_TOK_C_STRING) ||
        init->child_count != 0u || !init->literal.data)
      ok = 0;
    break;
  case AST_HIR_INIT_SYMBOL:
    if (!init->module.text || !init->symbol.text || init->child_count != 0u)
      ok = 0;
    break;
  case AST_HIR_INIT_AGGREGATE: {
    AstSemaTypeKind kind = ast_sema_type_kind(init->type);
    uint64_t capacity = 0u;
    if (kind == AST_SEMA_TYPE_ARRAY) {
      if (!ast_sema_type_array_count(init->type, &capacity))
        ok = 0;
    } else if (kind == AST_SEMA_TYPE_TUPLE)
      capacity = ast_sema_type_tuple_count(init->type);
    else if (kind == AST_SEMA_TYPE_STRUCT)
      capacity = ast_sema_type_struct_field_count(init->type);
    else
      ok = 0;
    if ((uint64_t)init->child_count > capacity ||
        (init->child_count && !init->children))
      ok = 0;
    for (uint32_t i = 0u; i < init->child_count; ++i) {
      const AstSemaType *child_type = NULL;
      if (kind == AST_SEMA_TYPE_ARRAY)
        child_type = ast_sema_type_element(init->type);
      else if (kind == AST_SEMA_TYPE_TUPLE)
        child_type = ast_sema_type_tuple_item(init->type, i);
      else if (kind == AST_SEMA_TYPE_STRUCT) {
        AstSemaFieldInfo field;
        memset(&field, 0, sizeof(field));
        if (ast_sema_type_struct_field(init->type, i, &field))
          child_type = field.type;
      }
      if (!verify_initializer(program, init->children[i], child_type,
                              depth + 1u))
        ok = 0;
    }
    break;
  }
  case AST_HIR_INIT_NONE:
  default:
    ok = 0;
    break;
  }
  if (!ok) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_INITIALIZER, AST_DIAG_ERROR,
                     init->span, "invalid canonical HIR global initializer");
  }
  return ok;
}

static int verify_instruction_shape(AstHirProgram *program, AstHirFunction *fn,
                                    const AstHirInstruction *inst) {
  int ok = 1;
  switch (inst->op) {
  case AST_HIR_OP_CONSTANT:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type ||
        inst->constant.kind == AST_SEMA_CONST_NONE)
      ok = 0;
    break;
  case AST_HIR_OP_LOCAL_LOAD:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (inst->result == AST_HIR_INVALID_ID || inst->local >= fn->local_count ||
        !hir_type_equal(inst->type, fn->locals[inst->local].type))
      ok = 0;
    break;
  case AST_HIR_OP_LOCAL_STORE:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result != AST_HIR_INVALID_ID || inst->local >= fn->local_count ||
        (inst->operand_count == 1u && inst->operands[0] < fn->value_count &&
         !hir_type_equal(fn->values[inst->operands[0]].type,
                         fn->locals[inst->local].type)))
      ok = 0;
    break;
  case AST_HIR_OP_LOCAL_ADDR:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (inst->result == AST_HIR_INVALID_ID || inst->local >= fn->local_count ||
        !inst->type || ast_sema_type_kind(inst->type) != AST_SEMA_TYPE_PTR)
      ok = 0;
    break;
  case AST_HIR_OP_GLOBAL_ADDR:
  case AST_HIR_OP_GLOBAL_LOAD:
  case AST_HIR_OP_FUNCTION_REF:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type ||
        !inst->module.text || !inst->symbol.text)
      ok = 0;
    break;
  case AST_HIR_OP_GLOBAL_STORE:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result != AST_HIR_INVALID_ID || !inst->module.text ||
        !inst->symbol.text)
      ok = 0;
    break;
  case AST_HIR_OP_GENERIC_CONST:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type ||
        inst->aux_index >= fn->generic_param_count ||
        fn->generic_params[inst->aux_index].kind != AST_HIR_GENERIC_CONST)
      ok = 0;
    break;
  case AST_HIR_OP_UNARY:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type)
      ok = 0;
    break;
  case AST_HIR_OP_BINARY:
    ok &= verify_operand_count(program, inst, 2u, 2u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type)
      ok = 0;
    break;
  case AST_HIR_OP_CAST:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type)
      ok = 0;
    break;
  case AST_HIR_OP_SELECT:
    ok &= verify_operand_count(program, inst, 3u, 3u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type)
      ok = 0;
    break;
  case AST_HIR_OP_CALL:
    ok &= verify_operand_count(program, inst, 1u, UINT32_MAX);
    if (!(inst->effects & AST_SEMA_EFFECT_CALL) || !inst->call_signature)
      ok = 0;
    break;
  case AST_HIR_OP_INDEX:
    ok &= verify_operand_count(program, inst, 2u, 2u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type)
      ok = 0;
    break;
  case AST_HIR_OP_MEMBER:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type)
      ok = 0;
    break;
  case AST_HIR_OP_ADDRESS_OF:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type ||
        ast_sema_type_kind(inst->type) != AST_SEMA_TYPE_PTR)
      ok = 0;
    break;
  case AST_HIR_OP_LOAD:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type ||
        !(inst->effects & AST_SEMA_EFFECT_READ_MEMORY))
      ok = 0;
    break;
  case AST_HIR_OP_STORE:
    ok &= verify_operand_count(program, inst, 2u, 2u);
    if (inst->result != AST_HIR_INVALID_ID ||
        !(inst->effects & AST_SEMA_EFFECT_WRITE_MEMORY))
      ok = 0;
    break;
  case AST_HIR_OP_ATOMIC:
    ok &= verify_operand_count(program, inst, 1u, UINT32_MAX);
    if (!(inst->effects & AST_SEMA_EFFECT_ATOMIC) ||
        !hir_atomic_order_shape_valid(inst))
      ok = 0;
    break;
  case AST_HIR_OP_FENCE:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (!(inst->effects & AST_SEMA_EFFECT_ATOMIC) ||
        !hir_memory_order_valid(inst->memory_order) ||
        inst->failure_memory_order != AST_MEMORY_ORDER_NONE)
      ok = 0;
    break;
  case AST_HIR_OP_INTRINSIC:
    if (inst->intrinsic == AST_HIR_INTRINSIC_NONE)
      ok = 0;
    break;
  case AST_HIR_OP_BIND:
    ok &= verify_operand_count(program, inst, 1u, 1u);
    if (inst->result != AST_HIR_INVALID_ID || inst->local >= fn->local_count)
      ok = 0;
    break;
  case AST_HIR_OP_NATIVE:
    if (inst->result != AST_HIR_INVALID_ID ||
        inst->native_region >= fn->native_region_count ||
        !(inst->effects & AST_SEMA_EFFECT_UNSAFE))
      ok = 0;
    else if (inst->operand_count !=
             fn->native_regions[inst->native_region].input_count)
      ok = 0;
    break;
  case AST_HIR_OP_NATIVE_RESULT:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (inst->result == AST_HIR_INVALID_ID ||
        inst->native_region >= fn->native_region_count ||
        inst->aux_index >=
            fn->native_regions[inst->native_region].output_count ||
        inst->local >= fn->local_count ||
        !hir_type_equal(inst->type, fn->locals[inst->local].type))
      ok = 0;
    break;
  case AST_HIR_OP_UNDEF:
    ok &= verify_operand_count(program, inst, 0u, 0u);
    if (inst->result == AST_HIR_INVALID_ID || !inst->type)
      ok = 0;
    break;
  case AST_HIR_OP_INVALID:
  default:
    ok = 0;
    break;
  }
  if (!ok) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_OPERAND, AST_DIAG_ERROR,
                     inst->span,
                     "invalid operand/result shape for HIR instruction '%s'",
                     ast_hir_op_name(inst->op));
  }
  return ok;
}

int hir_verify_function(AstHirProgram *program, AstHirFunction *fn) {
  if (!program || !fn)
    return 0;
  if (!fn->module_name.text || !fn->name.text || !fn->type ||
      (fn->abi.is_present && !fn->abi.text.data) ||
      (fn->link_name.is_present && !fn->link_name.text.data) ||
      (fn->target_constraint.is_present && !fn->target_constraint.text.data) ||
      (fn->requires_constraint.is_present &&
       !fn->requires_constraint.text.data)) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                     fn->span, "HIR function metadata is not canonicalized");
    return 0;
  }
  if (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH)) {
    if (fn->block_count != 0u) {
      hir_program_diag(
          program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR, fn->span,
          "extern/dispatch function must not contain runtime HIR blocks");
      return 0;
    }
    return 1;
  }
  if (!hir_analyze_cfg(fn)) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                     fn->span, "cannot analyze function CFG");
    return 0;
  }
  if (fn->entry_block >= fn->block_count) {
    hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                     fn->span, "function has no valid entry block");
    return 0;
  }
  int ok = 1;
  for (uint32_t ri = 0u; ri < fn->native_region_count; ++ri)
    ok &= verify_native_region(program, fn, &fn->native_regions[ri]);
  uint32_t *inst_block = (uint32_t *)malloc(
      (size_t)(fn->instruction_count ? fn->instruction_count : 1u) *
      sizeof(*inst_block));
  uint32_t *inst_pos = (uint32_t *)malloc(
      (size_t)(fn->instruction_count ? fn->instruction_count : 1u) *
      sizeof(*inst_pos));
  uint8_t *seen_inst =
      (uint8_t *)calloc(fn->instruction_count ? fn->instruction_count : 1u, 1u);
  if (!inst_block || !inst_pos || !seen_inst) {
    free(inst_block);
    free(inst_pos);
    free(seen_inst);
    return 0;
  }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstHirBlock *b = &fn->blocks[bi];
    if (b->term.kind == AST_HIR_TERM_NONE) {
      hir_program_diag(program, AST_HIR_DIAG_MISSING_TERMINATOR, AST_DIAG_ERROR,
                       b->span, "basic block has no terminator");
      ok = 0;
    }
    for (uint32_t ii = 0u; ii < b->instruction_count; ++ii) {
      AstHirInstId id = b->instructions[ii];
      if (id >= fn->instruction_count || seen_inst[id]) {
        hir_program_diag(
            program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR, b->span,
            "basic block contains invalid or duplicate instruction id");
        ok = 0;
        continue;
      }
      seen_inst[id] = 1u;
      inst_block[id] = bi;
      inst_pos[id] = ii;
    }
  }
  for (uint32_t ii = 0u; ii < fn->instruction_count; ++ii)
    if (!seen_inst[ii]) {
      hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                       fn->instructions[ii].span,
                       "HIR instruction is not owned by any block");
      ok = 0;
    }
  for (uint32_t bi = 0u; bi < fn->block_count; ++bi) {
    AstHirBlock *b = &fn->blocks[bi];
    for (uint32_t ai = 0u; ai < b->argument_count; ++ai) {
      AstHirValueId vid = b->arguments[ai];
      if (vid >= fn->value_count ||
          fn->values[vid].kind != AST_HIR_VALUE_BLOCK_ARGUMENT ||
          fn->values[vid].block != bi) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                         b->span, "invalid block argument value");
        ok = 0;
      }
    }
    for (uint32_t pos = 0u; pos < b->instruction_count; ++pos) {
      AstHirInstId iid = b->instructions[pos];
      if (iid >= fn->instruction_count)
        continue;
      AstHirInstruction *inst = &fn->instructions[iid];
      if (inst->result != AST_HIR_INVALID_ID) {
        if (inst->result >= fn->value_count ||
            fn->values[inst->result].kind != AST_HIR_VALUE_INSTRUCTION_RESULT ||
            fn->values[inst->result].definition_index != iid ||
            !hir_type_equal(fn->values[inst->result].type, inst->type)) {
          hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                           inst->span,
                           "instruction result/value table mismatch");
          ok = 0;
        }
      }
      if ((inst->op == AST_HIR_OP_LOCAL_LOAD ||
           inst->op == AST_HIR_OP_LOCAL_STORE ||
           inst->op == AST_HIR_OP_LOCAL_ADDR || inst->op == AST_HIR_OP_BIND ||
           inst->op == AST_HIR_OP_NATIVE_RESULT) &&
          inst->local >= fn->local_count) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_OPERAND, AST_DIAG_ERROR,
                         inst->span, "HIR local id is out of range");
        ok = 0;
      }
      ok &= verify_instruction_shape(program, fn, inst);
      if ((inst->op == AST_HIR_OP_LOAD || inst->op == AST_HIR_OP_GLOBAL_LOAD) &&
          !(inst->effects & AST_SEMA_EFFECT_READ_MEMORY)) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_EFFECTS, AST_DIAG_ERROR,
                         inst->span,
                         "load instruction is missing read-memory effect");
        ok = 0;
      }
      if ((inst->op == AST_HIR_OP_STORE ||
           inst->op == AST_HIR_OP_GLOBAL_STORE) &&
          !(inst->effects & AST_SEMA_EFFECT_WRITE_MEMORY)) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_EFFECTS, AST_DIAG_ERROR,
                         inst->span,
                         "store instruction is missing write-memory effect");
        ok = 0;
      }
      if (inst->op == AST_HIR_OP_ATOMIC &&
          !(inst->effects & AST_SEMA_EFFECT_ATOMIC)) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_EFFECTS, AST_DIAG_ERROR,
                         inst->span,
                         "atomic instruction is missing atomic effect");
        ok = 0;
      }
      for (uint32_t oi = 0u; oi < inst->operand_count; ++oi)
        ok &= verify_operand_dominance(program, fn, bi, pos, inst->operands[oi],
                                       inst->span, inst_block, inst_pos);
    }
    AstHirTerminator *t = &b->term;
    if (t->kind == AST_HIR_TERM_BR) {
      ok &= verify_branch_args(program, fn, t->span, t->target_true,
                               t->true_args, t->true_arg_count);
      for (uint32_t ai = 0u; ai < t->true_arg_count; ++ai)
        ok &= verify_operand_dominance(program, fn, bi, b->instruction_count,
                                       t->true_args[ai], t->span, inst_block,
                                       inst_pos);
    } else if (t->kind == AST_HIR_TERM_COND_BR) {
      ok &= verify_branch_args(program, fn, t->span, t->target_true,
                               t->true_args, t->true_arg_count);
      ok &= verify_branch_args(program, fn, t->span, t->target_false,
                               t->false_args, t->false_arg_count);
      for (uint32_t ai = 0u; ai < t->true_arg_count; ++ai)
        ok &= verify_operand_dominance(program, fn, bi, b->instruction_count,
                                       t->true_args[ai], t->span, inst_block,
                                       inst_pos);
      for (uint32_t ai = 0u; ai < t->false_arg_count; ++ai)
        ok &= verify_operand_dominance(program, fn, bi, b->instruction_count,
                                       t->false_args[ai], t->span, inst_block,
                                       inst_pos);
      if (t->condition_or_value >= fn->value_count ||
          ast_sema_type_kind(fn->values[t->condition_or_value].type) !=
              AST_SEMA_TYPE_BOOL) {
        hir_program_diag(program, AST_HIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                         t->span, "conditional branch requires bool condition");
        ok = 0;
      } else
        ok &= verify_operand_dominance(program, fn, bi, b->instruction_count,
                                       t->condition_or_value, t->span,
                                       inst_block, inst_pos);
    } else if (t->kind == AST_HIR_TERM_RETURN) {
      const AstSemaType *ret =
          fn->type ? ast_sema_type_function_result(fn->type) : NULL;
      int want_value = ret && ast_sema_type_kind(ret) != AST_SEMA_TYPE_VOID;
      int has_value = t->condition_or_value != AST_HIR_INVALID_ID;
      if (want_value != has_value) {
        hir_program_diag(
            program, AST_HIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, t->span,
            want_value ? "non-void function return is missing a value"
                       : "void function return must not carry a value");
        ok = 0;
      } else if (has_value) {
        if (t->condition_or_value >= fn->value_count) {
          hir_program_diag(program, AST_HIR_DIAG_INVALID_OPERAND,
                           AST_DIAG_ERROR, t->span,
                           "return value is out of range");
          ok = 0;
        } else {
          if (ret &&
              !hir_type_equal(ret, fn->values[t->condition_or_value].type)) {
            hir_program_diag(
                program, AST_HIR_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, t->span,
                "return value type does not match function result type");
            ok = 0;
          }
          ok &= verify_operand_dominance(program, fn, bi, b->instruction_count,
                                         t->condition_or_value, t->span,
                                         inst_block, inst_pos);
        }
      }
    } else if (t->kind != AST_HIR_TERM_UNREACHABLE &&
               t->kind != AST_HIR_TERM_NONE) {
      hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                       t->span, "unknown HIR terminator kind");
      ok = 0;
    }
  }
  free(inst_block);
  free(inst_pos);
  free(seen_inst);
  return ok;
}

int ast_hir_verify(AstHirProgram *program) {
  if (!program)
    return 0;
  size_t errors_before = program->stats.error_count;
  int ok = 1;
  for (size_t i = 0u; i < program->target_count; ++i) {
    const AstHirTarget *t = &program->targets[i];
    if (!t->module_name.text || !t->triple.data) {
      hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                       t->span, "HIR target metadata is not canonicalized");
      ok = 0;
    }
    for (uint32_t j = 0u; j < t->item_count; ++j) {
      const AstHirTargetItemInfo *item = &t->items[j];
      if (item->is_feature && !item->feature.data) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                         item->span,
                         "HIR target feature metadata is not canonicalized");
        ok = 0;
      } else if (!item->is_feature &&
                 item->value_literal_kind != AST_TOK_INVALID &&
                 item->constant.kind == AST_SEMA_CONST_NONE &&
                 !item->value_text.data) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_CFG, AST_DIAG_ERROR,
                         item->span,
                         "HIR target property metadata is not canonicalized");
        ok = 0;
      }
    }
  }
  for (size_t i = 0u; i < program->global_count; ++i) {
    const AstHirGlobal *g = &program->globals[i];
    if (!g->module_name.text || !g->name.text || !g->section_name.data) {
      hir_program_diag(program, AST_HIR_DIAG_INVALID_INITIALIZER,
                       AST_DIAG_ERROR, g->span,
                       "HIR global metadata is not canonicalized");
      ok = 0;
    }
    if (!g->initializer) {
      hir_program_diag(program, AST_HIR_DIAG_INVALID_INITIALIZER,
                       AST_DIAG_ERROR, g->span,
                       "HIR global is missing a canonical initializer");
      ok = 0;
    } else {
      ok &= verify_initializer(program, g->initializer, g->type, 0u);
    }
  }
  for (size_t i = 0u; i < program->function_count; ++i)
    if (program->functions[i])
      ok &= hir_verify_function(program, program->functions[i]);
  program->stats.error_count = 0u;
  program->stats.warning_count = 0u;
  for (size_t i = 0u; i < program->diagnostic_count; ++i) {
    if (program->diagnostics[i].severity == AST_DIAG_ERROR)
      program->stats.error_count++;
    else if (program->diagnostics[i].severity == AST_DIAG_WARNING)
      program->stats.warning_count++;
  }
  return ok && program->stats.error_count == errors_before;
}
