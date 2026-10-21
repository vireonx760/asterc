#include "hir_internal.h"

static int native_name_index(const AstName *names, uint32_t count, AstName name,
                             uint32_t *index_out) {
  for (uint32_t i = 0u; i < count; ++i) {
    if (hir_name_eq(names[i], name)) {
      if (index_out)
        *index_out = i;
      return 1;
    }
  }
  return 0;
}

static AstHirNativeExpr *copy_native_expr(HirBuilder *b,
                                          const AstNativeBridge *bridge,
                                          const AstExpr *expr, uint32_t depth);

static AstHirNativeExpr **alloc_children(HirBuilder *b, uint32_t count) {
  if (!count)
    return NULL;
  AstHirNativeExpr **items =
      AST_ARENA_ARRAY(&b->function->arena, AstHirNativeExpr *, count);
  if (!items)
    b->function->oom = 1u;
  return items;
}

static AstHirNativeExpr *new_native_expr(HirBuilder *b,
                                         AstHirNativeExprKind kind,
                                         const AstExpr *source) {
  AstHirNativeExpr *out = AST_ARENA_NEW(&b->function->arena, AstHirNativeExpr);
  if (!out) {
    b->function->oom = 1u;
    return NULL;
  }
  memset(out, 0, sizeof(*out));
  out->kind = kind;
  out->binding_kind = AST_HIR_NATIVE_BINDING_TARGET;
  out->binding_index = UINT32_MAX;
  if (source) {
    out->span = source->span;
    AstSemaExprFact fact;
    memset(&fact, 0, sizeof(fact));
    if (ast_sema_expr_fact(b->program->sema, source, &fact)) {
      out->type = fact.type;
      out->constant = fact.constant;
    }
  }
  return out;
}

static AstHirNativeExpr *copy_native_expr(HirBuilder *b,
                                          const AstNativeBridge *bridge,
                                          const AstExpr *expr, uint32_t depth) {
  if (!expr)
    return NULL;
  if (depth > b->program->config.recursion_limit) {
    hir_function_diag(b->function, AST_HIR_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR,
                      expr->span,
                      "native HIR expression recursion limit exceeded");
    return NULL;
  }
  AstHirNativeExpr *out = NULL;
  uint32_t child_count = 0u;
  switch (expr->kind) {
  case AST_EXPR_NAME: {
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_NAME, expr);
    if (!out)
      return NULL;
    out->name = hir_copy_name(&b->function->arena, expr->as.name.name);
    uint32_t index = 0u;
    if (native_name_index(bridge->inputs, bridge->input_count, out->name,
                          &index)) {
      out->binding_kind = AST_HIR_NATIVE_BINDING_INPUT;
      out->binding_index = index;
    } else if (native_name_index(bridge->outputs, bridge->output_count,
                                 out->name, &index)) {
      out->binding_kind = AST_HIR_NATIVE_BINDING_OUTPUT;
      out->binding_index = index;
    }
    return out;
  }
  case AST_EXPR_INTEGER:
  case AST_EXPR_FLOAT:
  case AST_EXPR_CHAR:
  case AST_EXPR_BOOL: {
    AstSemaExprFact fact;
    memset(&fact, 0, sizeof(fact));
    AstHirNativeExprKind kind = AST_HIR_NATIVE_EXPR_LITERAL;
    if (ast_sema_expr_fact(b->program->sema, expr, &fact) &&
        fact.constant.kind != AST_SEMA_CONST_NONE) {
      kind = AST_HIR_NATIVE_EXPR_CONSTANT;
    }
    out = new_native_expr(b, kind, expr);
    if (out) {
      out->token_kind = expr->as.literal.token_kind;
      if (kind == AST_HIR_NATIVE_EXPR_CONSTANT) {
        out->type = fact.type;
        out->constant = fact.constant;
      } else if (!hir_copy_source_text(b->program, &b->function->arena,
                                       expr->span, &out->literal)) {
        hir_function_diag(b->function, AST_HIR_DIAG_INVALID_NATIVE_REGION,
                          AST_DIAG_ERROR, expr->span,
                          "cannot canonicalize native literal spelling");
      }
    }
    return out;
  }
  case AST_EXPR_STRING:
  case AST_EXPR_BYTE_STRING:
  case AST_EXPR_C_STRING:
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_LITERAL, expr);
    if (out) {
      out->token_kind = expr->as.literal.token_kind;
      if (!hir_decode_literal_text(b->program, &b->function->arena, expr->span,
                                   out->token_kind, &out->literal)) {
        hir_function_diag(b->function, AST_HIR_DIAG_INVALID_NATIVE_REGION,
                          AST_DIAG_ERROR, expr->span,
                          "cannot canonicalize native string literal");
      }
    }
    return out;
  case AST_EXPR_TYPE: {
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_TYPE, expr);
    if (!out)
      return NULL;
    AstSemaTypeFact tf;
    memset(&tf, 0, sizeof(tf));
    if (expr->as.type_operand.type &&
        ast_sema_type_fact(b->program->sema, expr->as.type_operand.type, &tf))
      out->type = tf.type;
    return out;
  }
  case AST_EXPR_UNARY:
    child_count = 1u;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_UNARY, expr);
    if (out)
      out->token_kind = expr->as.unary.op;
    break;
  case AST_EXPR_BINARY:
    child_count = 2u;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_BINARY, expr);
    if (out)
      out->token_kind = expr->as.binary.op;
    break;
  case AST_EXPR_CONDITIONAL:
    child_count = 3u;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_CONDITIONAL, expr);
    break;
  case AST_EXPR_CALL:
    child_count = 1u + expr->as.call.args.count;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_CALL, expr);
    break;
  case AST_EXPR_INDEX:
    child_count = 2u;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_INDEX, expr);
    break;
  case AST_EXPR_MEMBER:
    child_count = 1u;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_MEMBER, expr);
    if (out)
      out->name = hir_copy_name(&b->function->arena, expr->as.member.member);
    break;
  case AST_EXPR_LOAD:
    child_count = 1u;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_LOAD, expr);
    break;
  case AST_EXPR_CAST:
    child_count = 1u;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_CAST, expr);
    if (out)
      out->token_kind = expr->as.cast.op;
    break;
  case AST_EXPR_ATOMIC:
    child_count = 1u + expr->as.atomic.args.count;
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_ATOMIC, expr);
    if (out)
      out->name = hir_copy_name(&b->function->arena, expr->as.atomic.op);
    break;
  case AST_EXPR_ATTR:
    child_count = 1u + (expr->as.attributed.attribute
                            ? expr->as.attributed.attribute->args.count
                            : 0u);
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_ATTRIBUTE, expr);
    if (out && expr->as.attributed.attribute)
      out->name = hir_copy_name(&b->function->arena,
                                expr->as.attributed.attribute->name);
    break;
  case AST_EXPR_MACRO_REF:
  case AST_EXPR_ERROR:
  default:
    out = new_native_expr(b, AST_HIR_NATIVE_EXPR_INVALID, expr);
    hir_function_diag(
        b->function, AST_HIR_DIAG_INVALID_NATIVE_REGION, AST_DIAG_ERROR,
        expr->span,
        "unsupported expression reached canonical native HIR region");
    return out;
  }
  if (!out)
    return NULL;
  out->child_count = child_count;
  out->children = alloc_children(b, child_count);
  if (child_count && !out->children)
    return out;
  switch (expr->kind) {
  case AST_EXPR_UNARY:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.unary.operand, depth + 1u);
    break;
  case AST_EXPR_BINARY:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.binary.left, depth + 1u);
    out->children[1] =
        copy_native_expr(b, bridge, expr->as.binary.right, depth + 1u);
    break;
  case AST_EXPR_CONDITIONAL:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.conditional.condition, depth + 1u);
    out->children[1] =
        copy_native_expr(b, bridge, expr->as.conditional.then_expr, depth + 1u);
    out->children[2] =
        copy_native_expr(b, bridge, expr->as.conditional.else_expr, depth + 1u);
    break;
  case AST_EXPR_CALL:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.call.callee, depth + 1u);
    for (uint32_t i = 0u; i < expr->as.call.args.count; ++i)
      out->children[i + 1u] =
          copy_native_expr(b, bridge, expr->as.call.args.items[i], depth + 1u);
    break;
  case AST_EXPR_INDEX:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.index.base, depth + 1u);
    out->children[1] =
        copy_native_expr(b, bridge, expr->as.index.index, depth + 1u);
    break;
  case AST_EXPR_MEMBER:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.member.base, depth + 1u);
    break;
  case AST_EXPR_LOAD:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.load.address, depth + 1u);
    break;
  case AST_EXPR_CAST:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.cast.value, depth + 1u);
    break;
  case AST_EXPR_ATOMIC:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.atomic.address, depth + 1u);
    for (uint32_t i = 0u; i < expr->as.atomic.args.count; ++i)
      out->children[i + 1u] = copy_native_expr(
          b, bridge, expr->as.atomic.args.items[i], depth + 1u);
    break;
  case AST_EXPR_ATTR:
    out->children[0] =
        copy_native_expr(b, bridge, expr->as.attributed.base, depth + 1u);
    if (expr->as.attributed.attribute)
      for (uint32_t i = 0u; i < expr->as.attributed.attribute->args.count; ++i)
        out->children[i + 1u] = copy_native_expr(
            b, bridge, expr->as.attributed.attribute->args.items[i],
            depth + 1u);
    break;
  default:
    break;
  }
  return out;
}

uint32_t hir_build_native_region(HirBuilder *b, const AstStmt *stmt) {
  if (!b || !stmt ||
      (stmt->kind != AST_STMT_NATIVE && stmt->kind != AST_STMT_EXACT))
    return UINT32_MAX;
  size_t cap = b->function->native_region_capacity;
  if (!hir_reserve((void **)&b->function->native_regions,
                   sizeof(*b->function->native_regions), &cap,
                   (size_t)b->function->native_region_count + 1u)) {
    b->function->oom = 1u;
    return UINT32_MAX;
  }
  b->function->native_region_capacity = (uint32_t)cap;
  uint32_t index = b->function->native_region_count++;
  AstHirNativeRegion *region = &b->function->native_regions[index];
  memset(region, 0, sizeof(*region));
  region->arch = hir_copy_name(&b->function->arena, stmt->as.native_stmt.arch);
  region->span = stmt->span;
  if (stmt->kind == AST_STMT_EXACT)
    region->flags |= AST_HIR_NATIVE_EXACT;
  if (stmt->as.native_stmt.unsafe_exact)
    region->flags |= AST_HIR_NATIVE_UNSAFE_EXACT;
  const AstNativeBridge *bridge = &stmt->as.native_stmt.bridge;
  region->input_count = bridge->input_count;
  region->output_count = bridge->output_count;
  if (bridge->input_count) {
    region->inputs =
        AST_ARENA_ARRAY(&b->function->arena, AstName, bridge->input_count);
    if (!region->inputs) {
      b->function->oom = 1u;
      return index;
    }
    for (uint32_t i = 0u; i < bridge->input_count; ++i)
      region->inputs[i] = hir_copy_name(&b->function->arena, bridge->inputs[i]);
  }
  if (bridge->output_count) {
    region->outputs =
        AST_ARENA_ARRAY(&b->function->arena, AstName, bridge->output_count);
    if (!region->outputs) {
      b->function->oom = 1u;
      return index;
    }
    for (uint32_t i = 0u; i < bridge->output_count; ++i)
      region->outputs[i] =
          hir_copy_name(&b->function->arena, bridge->outputs[i]);
  }
  region->statement_count = stmt->as.native_stmt.statements.count;
  if (region->statement_count) {
    region->statements =
        AST_ARENA_ARRAY(&b->function->arena, AstHirNativeStmtCanonical,
                        region->statement_count);
    if (!region->statements) {
      b->function->oom = 1u;
      return index;
    }
    memset(region->statements, 0,
           (size_t)region->statement_count * sizeof(*region->statements));
  }
  for (uint32_t si = 0u; si < region->statement_count; ++si) {
    const AstNativeStmt *src = &stmt->as.native_stmt.statements.items[si];
    AstHirNativeStmtCanonical *dst = &region->statements[si];
    dst->kind = src->kind == AST_NATIVE_BYTES ? AST_HIR_NATIVE_BYTES
                                              : AST_HIR_NATIVE_INSTRUCTION;
    dst->span = src->span;
    dst->mnemonic = hir_copy_name(&b->function->arena, src->mnemonic);
    if (src->kind == AST_NATIVE_BYTES) {
      dst->byte_count = src->bytes.count;
      if (dst->byte_count) {
        dst->bytes =
            AST_ARENA_ARRAY(&b->function->arena, uint8_t, dst->byte_count);
        if (!dst->bytes) {
          b->function->oom = 1u;
          return index;
        }
        for (uint32_t bi = 0u; bi < dst->byte_count; ++bi) {
          AstSemaExprFact fact;
          memset(&fact, 0, sizeof(fact));
          if (!ast_sema_expr_fact(b->program->sema, src->bytes.items[bi],
                                  &fact) ||
              fact.constant.kind != AST_SEMA_CONST_INTEGER ||
              fact.constant.hi != 0u || fact.constant.is_negative ||
              fact.constant.lo > UINT8_MAX) {
            hir_function_diag(
                b->function, AST_HIR_DIAG_INVALID_NATIVE_REGION, AST_DIAG_ERROR,
                src->bytes.items[bi]->span,
                "exact bytes element has no canonical byte constant");
            dst->bytes[bi] = 0u;
          } else
            dst->bytes[bi] = (uint8_t)fact.constant.lo;
        }
      }
      continue;
    }
    dst->operand_count = src->operands.count;
    if (dst->operand_count) {
      dst->operands = AST_ARENA_ARRAY(&b->function->arena, AstHirNativeOperand,
                                      dst->operand_count);
      if (!dst->operands) {
        b->function->oom = 1u;
        return index;
      }
      memset(dst->operands, 0,
             (size_t)dst->operand_count * sizeof(*dst->operands));
      for (uint32_t oi = 0u; oi < dst->operand_count; ++oi) {
        const AstNativeOperand *so = &src->operands.items[oi];
        AstHirNativeOperand *doo = &dst->operands[oi];
        doo->name = hir_copy_name(&b->function->arena, so->name);
        doo->span = so->span;
        doo->is_memory = so->is_memory;
        if (so->is_memory) {
          AstSemaTypeFact tf;
          memset(&tf, 0, sizeof(tf));
          if (so->memory_type &&
              ast_sema_type_fact(b->program->sema, so->memory_type, &tf))
            doo->memory_type = tf.type;
          doo->memory_address =
              copy_native_expr(b, bridge, so->memory_address, 0u);
        } else
          doo->value = copy_native_expr(b, bridge, so->value, 0u);
      }
    }
    dst->attribute_count = src->attributes.count;
    if (dst->attribute_count) {
      dst->attributes = AST_ARENA_ARRAY(
          &b->function->arena, AstHirNativeAttribute, dst->attribute_count);
      if (!dst->attributes) {
        b->function->oom = 1u;
        return index;
      }
      memset(dst->attributes, 0,
             (size_t)dst->attribute_count * sizeof(*dst->attributes));
      for (uint32_t ai = 0u; ai < dst->attribute_count; ++ai) {
        const AstAttribute *sa = &src->attributes.items[ai];
        AstHirNativeAttribute *da = &dst->attributes[ai];
        da->name = hir_copy_name(&b->function->arena, sa->name);
        da->span = sa->span;
        da->arg_count = sa->args.count;
        if (da->arg_count) {
          const AstHirNativeExpr **args = AST_ARENA_ARRAY(
              &b->function->arena, const AstHirNativeExpr *, da->arg_count);
          if (!args) {
            b->function->oom = 1u;
            return index;
          }
          for (uint32_t j = 0u; j < da->arg_count; ++j)
            args[j] = copy_native_expr(b, bridge, sa->args.items[j], 0u);
          da->args = args;
        }
      }
    }
  }
  return index;
}

uint32_t ast_hir_function_native_region_count(const AstHirFunction *function) {
  return function ? function->native_region_count : 0u;
}

const AstHirNativeRegion *
ast_hir_function_native_region(const AstHirFunction *function, uint32_t index) {
  if (!function || index >= function->native_region_count)
    return NULL;
  return &function->native_regions[index];
}

int ast_hir_native_region_info(const AstHirNativeRegion *region,
                               AstHirNativeRegionInfo *out) {
  if (!region || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->arch = region->arch;
  out->span = region->span;
  out->flags = region->flags;
  out->inputs = region->inputs;
  out->input_count = region->input_count;
  out->outputs = region->outputs;
  out->output_count = region->output_count;
  out->statement_count = region->statement_count;
  return 1;
}

int ast_hir_native_region_statement(const AstHirNativeRegion *region,
                                    uint32_t index, AstHirNativeStmtInfo *out) {
  if (!region || !out || index >= region->statement_count)
    return 0;
  const AstHirNativeStmtCanonical *stmt = &region->statements[index];
  memset(out, 0, sizeof(*out));
  out->kind = stmt->kind;
  out->span = stmt->span;
  out->mnemonic = stmt->mnemonic;
  out->operands = stmt->operands;
  out->operand_count = stmt->operand_count;
  out->attributes = stmt->attributes;
  out->attribute_count = stmt->attribute_count;
  out->bytes = stmt->bytes;
  out->byte_count = stmt->byte_count;
  return 1;
}

int ast_hir_native_expr_info(const AstHirNativeExpr *expr,
                             AstHirNativeExprInfo *out) {
  if (!expr || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->kind = expr->kind;
  out->span = expr->span;
  out->type = expr->type;
  out->constant = expr->constant;
  out->token_kind = expr->token_kind;
  out->literal = expr->literal;
  out->name = expr->name;
  out->binding_kind = expr->binding_kind;
  out->binding_index = expr->binding_index;
  out->child_count = expr->child_count;
  return 1;
}

const AstHirNativeExpr *ast_hir_native_expr_child(const AstHirNativeExpr *expr,
                                                  uint32_t index) {
  if (!expr || index >= expr->child_count || !expr->children)
    return NULL;
  return expr->children[index];
}
