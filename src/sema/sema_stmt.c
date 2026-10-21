#include "sema_internal.h"

static int sema_reserve_local(void **data, size_t elem_size, size_t *cap,
                              size_t need) {
  if (*cap >= need)
    return 0;
  size_t n = *cap ? *cap : 16u;
  while (n < need) {
    if (n > SIZE_MAX / 2u)
      return -1;
    n *= 2u;
  }
  size_t bytes;
  if (ast_size_mul_overflow(n, elem_size, &bytes))
    return -1;
  void *p = realloc(*data, bytes);
  if (!p)
    return -1;
  *data = p;
  *cap = n;
  return 0;
}

static AstSemaLocal *add_local(AstSemaFnCtx *fn, AstName name,
                               const AstSemaType *type, AstSemaSymbolKind kind,
                               int mutable_value, int initialized) {
  for (size_t i = fn->local_count; i > 0u; --i) {
    AstSemaLocal *old = &fn->locals[i - 1u];
    if (old->depth < fn->scope_depth)
      break;
    if (sema_name_eq(old->name, name)) {
      sema_diag(
          fn->sema,
          kind == AST_SEMA_SYMBOL_PARAMETER ? AST_SEMA_DIAG_DUPLICATE_PARAMETER
                                            : AST_SEMA_DIAG_DUPLICATE_LOCAL,
          AST_DIAG_ERROR, name.span, fn->module->file.id,
          "duplicate local name '%.*s'", (int)name.span.length, name.text);
      return old;
    }
  }
  if (sema_reserve_local((void **)&fn->locals, sizeof(*fn->locals),
                         &fn->local_capacity, fn->local_count + 1u) != 0) {
    fn->sema->oom = 1u;
    return NULL;
  }
  AstSemaLocal *l = &fn->locals[fn->local_count++];
  memset(l, 0, sizeof(*l));
  l->name = name;
  l->declaration_span = name.span;
  l->type = type;
  l->kind = kind;
  l->depth = fn->scope_depth;
  l->mutable_value = (uint8_t)(mutable_value != 0);
  l->initialized = (uint8_t)(initialized != 0);
  return l;
}

static void leave_scope(AstSemaFnCtx *fn, size_t old_count) {
  if (fn->local_count > old_count)
    fn->local_count = old_count;
  if (fn->scope_depth)
    fn->scope_depth--;
}

static int require_bool(AstSemaFnCtx *fn, const AstExpr *expr) {
  AstExprInfo info = sema_check_expr(fn, expr, sema_builtin_bool(fn->sema));
  if (!sema_type_is_bool(info.type) && info.type->kind != AST_SEMA_TYPE_ERROR) {
    sema_diag(fn->sema, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "control-flow condition must have type bool");
    return 0;
  }
  return 1;
}

static int eval_compile_time_bool(AstSemaFnCtx *fn, const AstExpr *expr,
                                  int *value_out) {
  AstSema *s = fn->sema;
  size_t errors_before = s->stats.error_count;
  AstExprInfo typed = sema_check_expr(fn, expr, sema_builtin_bool(s));
  if (!sema_type_is_bool(typed.type)) {
    if (s->stats.error_count == errors_before) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "compile-time condition must have bool type");
    }
    return 0;
  }
  if (s->stats.error_count != errors_before)
    return 0;
  AstSemaConstCtx cc;
  memset(&cc, 0, sizeof(cc));
  cc.sema = s;
  cc.module = fn->module;
  cc.fn = fn;
  AstSemaConstValue value =
      sema_eval_const_expr(&cc, expr, sema_builtin_bool(s));
  if (!sema_const_is_valid(value) ||
      (value.flags & AST_SEMA_CONSTF_BOOL) == 0u) {
    sema_diag(s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "compile-time condition must be a constant bool expression");
    return 0;
  }
  *value_out = (value.lo != 0u || value.hi != 0u);
  return 1;
}

static void collect_labels_stmt(AstSemaFnCtx *fn, const AstStmt *stmt,
                                uint32_t depth);
static void collect_labels_block(AstSemaFnCtx *fn, const AstBlock *block,
                                 uint32_t depth);

static AstFlowKind check_comptime_if(AstSemaFnCtx *fn, const AstStmt *if_stmt) {
  int selected = 0;
  if (!if_stmt || if_stmt->kind != AST_STMT_IF)
    return AST_FLOW_FALLTHROUGH;
  if (!eval_compile_time_bool(fn, if_stmt->as.if_stmt.condition, &selected)) {
    require_bool(fn, if_stmt->as.if_stmt.condition);
    return AST_FLOW_FALLTHROUGH;
  }
  if (selected) {
    collect_labels_block(fn, if_stmt->as.if_stmt.then_block,
                         fn->scope_depth + 1u);
    return sema_check_block(fn, if_stmt->as.if_stmt.then_block, 1);
  }
  if (if_stmt->as.if_stmt.else_branch) {
    if (if_stmt->as.if_stmt.else_branch->kind == AST_STMT_IF) {
      return check_comptime_if(fn, if_stmt->as.if_stmt.else_branch);
    }
    collect_labels_stmt(fn, if_stmt->as.if_stmt.else_branch, fn->scope_depth);
    return sema_check_stmt(fn, if_stmt->as.if_stmt.else_branch);
  }
  return AST_FLOW_FALLTHROUGH;
}

static int check_initializer_expr(AstSemaFnCtx *fn, const AstSemaType *type,
                                  const AstExpr *expr, AstSpan span) {
  AstExprInfo value = sema_check_expr(fn, expr, type);
  if (!sema_type_assignable(fn->sema, type, value, span, fn->module->file.id)) {
    sema_diag(fn->sema, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, span,
              fn->module->file.id,
              "initializer is not assignable to the declared type");
    return 0;
  }
  return 1;
}

static void snapshot_init(AstSemaFnCtx *fn, uint8_t *dst, size_t count) {
  for (size_t i = 0u; i < count; ++i)
    dst[i] = fn->locals[i].initialized;
}

static int flow_falls_through(AstFlowKind flow) {
  return flow == AST_FLOW_FALLTHROUGH;
}

static AstFlowKind check_if(AstSemaFnCtx *fn, const AstStmt *stmt) {
  require_bool(fn, stmt->as.if_stmt.condition);
  size_t base = fn->local_count;
  uint8_t *before = base ? (uint8_t *)malloc(base) : NULL;
  uint8_t *then_state = base ? (uint8_t *)malloc(base) : NULL;
  uint8_t *else_state = base ? (uint8_t *)malloc(base) : NULL;
  if (base && (!before || !then_state || !else_state)) {
    free(before);
    free(then_state);
    free(else_state);
    fn->sema->oom = 1u;
    return AST_FLOW_FALLTHROUGH;
  }
  snapshot_init(fn, before, base);
  AstFlowKind then_flow = sema_check_block(fn, stmt->as.if_stmt.then_block, 1);
  snapshot_init(fn, then_state, base);
  for (size_t i = 0u; i < base; ++i)
    fn->locals[i].initialized = before[i];

  AstFlowKind else_flow = AST_FLOW_FALLTHROUGH;
  if (stmt->as.if_stmt.else_branch)
    else_flow = sema_check_stmt(fn, stmt->as.if_stmt.else_branch);
  snapshot_init(fn, else_state, base);
  if (!stmt->as.if_stmt.else_branch && base != 0u)
    memcpy(else_state, before, base);

  int then_continues = flow_falls_through(then_flow);
  int else_continues = flow_falls_through(else_flow);
  for (size_t i = 0u; i < base; ++i) {
    if (then_continues && else_continues)
      fn->locals[i].initialized = (uint8_t)(then_state[i] && else_state[i]);
    else if (then_continues)
      fn->locals[i].initialized = then_state[i];
    else if (else_continues)
      fn->locals[i].initialized = else_state[i];
    else
      fn->locals[i].initialized = before[i];
  }
  free(before);
  free(then_state);
  free(else_state);

  if (!then_continues && !else_continues)
    return then_flow == else_flow ? then_flow : AST_FLOW_DIVERGE;
  return AST_FLOW_FALLTHROUGH;
}

static int add_label(AstSemaFnCtx *fn, AstName name, uint32_t depth) {
  for (size_t i = 0u; i < fn->label_count; ++i) {
    if (sema_name_eq(fn->labels[i].name, name)) {
      sema_diag(fn->sema, AST_SEMA_DIAG_DUPLICATE_LABEL, AST_DIAG_ERROR,
                name.span, fn->module->file.id, "duplicate block label '%.*s'",
                (int)name.span.length, name.text);
      return 0;
    }
  }
  if (sema_reserve_local((void **)&fn->labels, sizeof(*fn->labels),
                         &fn->label_capacity, fn->label_count + 1u) != 0) {
    fn->sema->oom = 1u;
    return 0;
  }
  AstSemaLabel *label = &fn->labels[fn->label_count++];
  memset(label, 0, sizeof(*label));
  label->name = name;
  label->depth = depth;
  return 1;
}

static AstSemaLabel *find_label(AstSemaFnCtx *fn, AstName name) {
  for (size_t i = fn->label_count; i > 0u; --i) {
    if (sema_name_eq(fn->labels[i - 1u].name, name))
      return &fn->labels[i - 1u];
  }
  return NULL;
}

static void collect_labels_block(AstSemaFnCtx *fn, const AstBlock *block,
                                 uint32_t depth) {
  if (!block)
    return;
  for (uint32_t i = 0u; i < block->statements.count; ++i)
    collect_labels_stmt(fn, block->statements.items[i], depth);
}

static void collect_labels_stmt(AstSemaFnCtx *fn, const AstStmt *stmt,
                                uint32_t depth) {
  if (!stmt)
    return;
  switch (stmt->kind) {
  case AST_STMT_BLOCK_LABEL:
    add_label(fn, stmt->as.block_label.label, depth);
    break;
  case AST_STMT_BLOCK:
    collect_labels_block(fn, stmt->as.block_stmt.block, depth + 1u);
    break;
  case AST_STMT_IF:
    collect_labels_block(fn, stmt->as.if_stmt.then_block, depth + 1u);
    if (stmt->as.if_stmt.else_branch)
      collect_labels_stmt(fn, stmt->as.if_stmt.else_branch, depth);
    break;
  case AST_STMT_WHILE:
    collect_labels_block(fn, stmt->as.while_stmt.body, depth + 1u);
    break;
  case AST_STMT_LOOP:
    collect_labels_block(fn, stmt->as.loop_stmt.body, depth + 1u);
    break;
  case AST_STMT_UNSAFE:
    collect_labels_block(fn, stmt->as.unsafe_stmt.body, depth + 1u);
    break;
  case AST_STMT_COMPTIME:
  case AST_STMT_FOR:
  case AST_STMT_REQUIRES:
    break;
  default:
    break;
  }
}

static int push_loop_frame(AstSemaFnCtx *fn, size_t local_count) {
  if (sema_reserve_local((void **)&fn->loop_frames, sizeof(*fn->loop_frames),
                         &fn->loop_frame_capacity,
                         fn->loop_frame_count + 1u) != 0) {
    fn->sema->oom = 1u;
    return 0;
  }
  AstSemaLoopFrame *frame = &fn->loop_frames[fn->loop_frame_count++];
  memset(frame, 0, sizeof(*frame));
  frame->local_count = local_count;
  if (local_count) {
    frame->break_state = (uint8_t *)malloc(local_count);
    if (!frame->break_state) {
      fn->loop_frame_count--;
      fn->sema->oom = 1u;
      return 0;
    }
  }
  return 1;
}

static void record_loop_break(AstSemaFnCtx *fn) {
  if (fn->unreachable_depth != 0u || fn->loop_frame_count == 0u)
    return;
  AstSemaLoopFrame *frame = &fn->loop_frames[fn->loop_frame_count - 1u];
  if (!frame->has_break) {
    snapshot_init(fn, frame->break_state, frame->local_count);
    frame->has_break = 1u;
    return;
  }
  for (size_t i = 0u; i < frame->local_count; ++i) {
    frame->break_state[i] =
        (uint8_t)(frame->break_state[i] && fn->locals[i].initialized);
  }
}

static int pop_loop_frame(AstSemaFnCtx *fn, int apply_state) {
  if (fn->loop_frame_count == 0u)
    return 0;
  AstSemaLoopFrame *frame = &fn->loop_frames[fn->loop_frame_count - 1u];
  int has_break = frame->has_break != 0u;
  if (apply_state && has_break) {
    for (size_t i = 0u; i < frame->local_count; ++i)
      fn->locals[i].initialized = frame->break_state[i];
  }
  free(frame->break_state);
  memset(frame, 0, sizeof(*frame));
  fn->loop_frame_count--;
  return has_break;
}

static AstFlowKind check_comptime_for(AstSemaFnCtx *fn, const AstStmt *stmt) {
  AstSema *s = fn->sema;
  AstSemaConstCtx cc;
  memset(&cc, 0, sizeof(cc));
  cc.sema = s;
  cc.module = fn->module;
  cc.fn = fn;

  const AstSemaType *usize_type = sema_builtin_usize(s);
  AstExprInfo begin_info =
      sema_check_expr(fn, stmt->as.for_stmt.begin, usize_type);
  AstExprInfo end_info = sema_check_expr(fn, stmt->as.for_stmt.end, usize_type);
  AstSemaConstValue begin_value = begin_info.constant;
  AstSemaConstValue end_value = end_info.constant;
  if (!sema_const_is_valid(begin_value)) {
    begin_value =
        sema_eval_const_expr(&cc, stmt->as.for_stmt.begin, usize_type);
  }
  if (!sema_const_is_valid(end_value)) {
    end_value = sema_eval_const_expr(&cc, stmt->as.for_stmt.end, usize_type);
  }
  uint64_t begin_u64 = 0u;
  uint64_t end_u64 = 0u;
  if (!sema_const_to_u64(begin_value, &begin_u64) ||
      !sema_const_to_u64(end_value, &end_u64)) {
    sema_diag(s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR,
              stmt->span, fn->module->file.id,
              "comptime for bounds must be compile-time unsigned integer "
              "expressions");
    return AST_FLOW_FALLTHROUGH;
  }
  if (begin_u64 >= end_u64)
    return AST_FLOW_FALLTHROUGH;

  uint64_t iteration_count = end_u64 - begin_u64;
  int exceeds_limit =
      iteration_count > (uint64_t)s->config.max_comptime_iterations;
  if (exceeds_limit) {
    sema_diag(s, AST_SEMA_DIAG_COMPTIME_ITERATION_LIMIT, AST_DIAG_ERROR,
              stmt->span, fn->module->file.id,
              "comptime for expands to %llu iterations, exceeding the "
              "configured limit of %u",
              (unsigned long long)iteration_count,
              s->config.max_comptime_iterations);
  }

  size_t base = fn->local_count;
  uint8_t *before = base ? (uint8_t *)malloc(base) : NULL;
  if (base && !before) {
    s->oom = 1u;
    return AST_FLOW_FALLTHROUGH;
  }
  snapshot_init(fn, before, base);

  fn->scope_depth++;
  AstSemaLocal *added =
      add_local(fn, stmt->as.for_stmt.variable, sema_builtin_usize(s),
                AST_SEMA_SYMBOL_LOCAL_CONST, 0, 1);
  if (!added) {
    leave_scope(fn, base);
    free(before);
    return AST_FLOW_FALLTHROUGH;
  }
  size_t iterator_index = base;
  collect_labels_block(fn, stmt->as.for_stmt.body, fn->scope_depth + 1u);

  uint64_t checks = exceeds_limit ? 1u : iteration_count;
  AstFlowKind result = AST_FLOW_FALLTHROUGH;
  fn->loop_depth++;
  for (uint64_t n = 0u; n < checks; ++n) {
    uint64_t value = begin_u64 + n;
    fn->locals[iterator_index].const_value =
        sema_const_u64(value, sema_builtin_usize(s));
    AstFlowKind body_flow = sema_check_block(fn, stmt->as.for_stmt.body, 1);
    if (body_flow == AST_FLOW_CONTINUE)
      continue;
    if (body_flow == AST_FLOW_BREAK)
      break;
    if (body_flow == AST_FLOW_RETURN || body_flow == AST_FLOW_BRANCH ||
        body_flow == AST_FLOW_DIVERGE) {
      result = body_flow;
      break;
    }
  }
  fn->loop_depth--;
  leave_scope(fn, base);

  if (exceeds_limit) {
    for (size_t i = 0u; i < base; ++i)
      fn->locals[i].initialized = before[i];
    result = AST_FLOW_FALLTHROUGH;
  }
  free(before);
  return result;
}

static int bridge_contains_name(const AstNativeBridge *bridge, AstName name) {
  if (!bridge)
    return 0;
  for (uint32_t i = 0u; i < bridge->input_count; ++i)
    if (sema_name_eq(bridge->inputs[i], name))
      return 1;
  for (uint32_t i = 0u; i < bridge->output_count; ++i)
    if (sema_name_eq(bridge->outputs[i], name))
      return 1;
  return 0;
}

static int check_native_expr_names(AstSemaFnCtx *fn,
                                   const AstNativeBridge *bridge,
                                   const AstExpr *expr, uint32_t depth) {
  if (!expr)
    return 1;
  if (depth > fn->sema->config.recursion_limit) {
    sema_diag(fn->sema, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "native operand expression exceeds semantic recursion limit");
    return 0;
  }
  if (expr->kind == AST_EXPR_NAME) {
    AstSemaLocal *local = sema_fn_find_local(fn, expr->as.name.name);
    if (local && !bridge_contains_name(bridge, expr->as.name.name)) {
      sema_diag(fn->sema, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "semantic local '%.*s' used by native code is missing from "
                "inputs/outputs",
                (int)expr->as.name.name.span.length, expr->as.name.name.text);
      return 0;
    }
    return 1;
  }
  int valid = 1;
  switch (expr->kind) {
  case AST_EXPR_UNARY:
    valid &=
        check_native_expr_names(fn, bridge, expr->as.unary.operand, depth + 1u);
    break;
  case AST_EXPR_BINARY:
    valid &=
        check_native_expr_names(fn, bridge, expr->as.binary.left, depth + 1u);
    valid &=
        check_native_expr_names(fn, bridge, expr->as.binary.right, depth + 1u);
    break;
  case AST_EXPR_CONDITIONAL:
    valid &= check_native_expr_names(fn, bridge, expr->as.conditional.condition,
                                     depth + 1u);
    valid &= check_native_expr_names(fn, bridge, expr->as.conditional.then_expr,
                                     depth + 1u);
    valid &= check_native_expr_names(fn, bridge, expr->as.conditional.else_expr,
                                     depth + 1u);
    break;
  case AST_EXPR_CALL:
    valid &=
        check_native_expr_names(fn, bridge, expr->as.call.callee, depth + 1u);
    for (uint32_t i = 0u; i < expr->as.call.args.count; ++i) {
      valid &= check_native_expr_names(fn, bridge, expr->as.call.args.items[i],
                                       depth + 1u);
    }
    break;
  case AST_EXPR_INDEX:
    valid &=
        check_native_expr_names(fn, bridge, expr->as.index.base, depth + 1u);
    valid &=
        check_native_expr_names(fn, bridge, expr->as.index.index, depth + 1u);
    break;
  case AST_EXPR_MEMBER:
    valid &=
        check_native_expr_names(fn, bridge, expr->as.member.base, depth + 1u);
    break;
  case AST_EXPR_LOAD:
    valid &=
        check_native_expr_names(fn, bridge, expr->as.load.address, depth + 1u);
    break;
  case AST_EXPR_CAST:
    valid &=
        check_native_expr_names(fn, bridge, expr->as.cast.value, depth + 1u);
    break;
  case AST_EXPR_ATOMIC:
    valid &= check_native_expr_names(fn, bridge, expr->as.atomic.address,
                                     depth + 1u);
    for (uint32_t i = 0u; i < expr->as.atomic.args.count; ++i) {
      valid &= check_native_expr_names(
          fn, bridge, expr->as.atomic.args.items[i], depth + 1u);
    }
    break;
  case AST_EXPR_ATTR:
    valid &= check_native_expr_names(fn, bridge, expr->as.attributed.base,
                                     depth + 1u);
    break;
  default:
    break;
  }
  return valid;
}

static void check_native_bridge(AstSemaFnCtx *fn, const AstStmt *stmt) {
  if (!fn->sema->config.check_native_boundaries)
    return;
  AstSema *s = fn->sema;
  AstNativeBridge bridge = stmt->as.native_stmt.bridge;
  int valid = 1;

  for (uint32_t i = 0u; i < bridge.input_count; ++i) {
    for (uint32_t j = 0u; j < i; ++j) {
      if (sema_name_eq(bridge.inputs[i], bridge.inputs[j])) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                  bridge.inputs[i].span, fn->module->file.id,
                  "native input '%.*s' is listed more than once",
                  (int)bridge.inputs[i].span.length, bridge.inputs[i].text);
        valid = 0;
      }
    }
    AstSemaLocal *local = sema_fn_find_local(fn, bridge.inputs[i]);
    if (!local) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                bridge.inputs[i].span, fn->module->file.id,
                "native input '%.*s' is not a local semantic value",
                (int)bridge.inputs[i].span.length, bridge.inputs[i].text);
      valid = 0;
    } else if (!local->initialized) {
      sema_diag(s, AST_SEMA_DIAG_UNINITIALIZED_VALUE, AST_DIAG_ERROR,
                bridge.inputs[i].span, fn->module->file.id,
                "native input '%.*s' is uninitialized",
                (int)bridge.inputs[i].span.length, bridge.inputs[i].text);
      valid = 0;
    }
  }
  for (uint32_t i = 0u; i < bridge.output_count; ++i) {
    for (uint32_t j = 0u; j < i; ++j) {
      if (sema_name_eq(bridge.outputs[i], bridge.outputs[j])) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                  bridge.outputs[i].span, fn->module->file.id,
                  "native output '%.*s' is listed more than once",
                  (int)bridge.outputs[i].span.length, bridge.outputs[i].text);
        valid = 0;
      }
    }
    AstSemaLocal *local = sema_fn_find_local(fn, bridge.outputs[i]);
    if (!local) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                bridge.outputs[i].span, fn->module->file.id,
                "native output '%.*s' must name a declared local",
                (int)bridge.outputs[i].span.length, bridge.outputs[i].text);
      valid = 0;
    } else if (!local->mutable_value) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                bridge.outputs[i].span, fn->module->file.id,
                "native output '%.*s' cannot overwrite a const local",
                (int)bridge.outputs[i].span.length, bridge.outputs[i].text);
      valid = 0;
    }
  }

  for (uint32_t i = 0u; i < stmt->as.native_stmt.statements.count; ++i) {
    const AstNativeStmt *native_stmt =
        &stmt->as.native_stmt.statements.items[i];
    if (native_stmt->kind == AST_NATIVE_BYTES) {
      if (stmt->kind != AST_STMT_EXACT || !stmt->as.native_stmt.unsafe_exact) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                  native_stmt->span, fn->module->file.id,
                  "bytes{} is only valid inside unsafe exact blocks");
        valid = 0;
      }
      for (uint32_t bi = 0u; bi < native_stmt->bytes.count; ++bi) {
        AstSemaConstCtx cc;
        memset(&cc, 0, sizeof(cc));
        cc.sema = s;
        cc.module = fn->module;
        cc.fn = fn;
        const AstSemaType *u8_type = sema_builtin_for_token(s, AST_TOK_KW_U8);
        AstExprInfo typed_byte =
            sema_check_expr(fn, native_stmt->bytes.items[bi], u8_type);
        AstSemaConstValue value = typed_byte.constant;
        if (!sema_const_is_valid(value))
          value =
              sema_eval_const_expr(&cc, native_stmt->bytes.items[bi], u8_type);
        uint64_t byte = 0u;
        if (!sema_const_to_u64(value, &byte) || byte > UINT8_MAX) {
          sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                    native_stmt->bytes.items[bi]->span, fn->module->file.id,
                    "bytes{} elements must be compile-time integers in the "
                    "range 0..255");
          valid = 0;
        }
      }
      continue;
    }
    for (uint32_t oi = 0u; oi < native_stmt->operands.count; ++oi) {
      const AstNativeOperand *operand = &native_stmt->operands.items[oi];
      if (operand->is_memory) {
        if (operand->memory_type)
          (void)sema_resolve_type(s, fn->module, operand->memory_type, fn, 0u);
        valid &=
            check_native_expr_names(fn, &bridge, operand->memory_address, 0u);
      } else
        valid &= check_native_expr_names(fn, &bridge, operand->value, 0u);
    }
  }

  if (valid) {
    for (uint32_t i = 0u; i < bridge.output_count; ++i) {
      AstSemaLocal *local = sema_fn_find_local(fn, bridge.outputs[i]);
      if (local && local->mutable_value)
        local->initialized = 1u;
    }
  }
}

static int is_memory_order_name(AstName n) {
  return sema_name_eq_text(n, "relaxed") || sema_name_eq_text(n, "acquire") ||
         sema_name_eq_text(n, "release") || sema_name_eq_text(n, "acq_rel") ||
         sema_name_eq_text(n, "seq_cst");
}

static int fence_attribute_is_order(const AstAttribute *attr) {
  if (!attr)
    return 0;
  if (is_memory_order_name(attr->name))
    return attr->args.count == 0u;
  return sema_name_eq_text(attr->name, "order") && attr->args.count == 1u &&
         attr->args.items[0] && attr->args.items[0]->kind == AST_EXPR_NAME &&
         is_memory_order_name(attr->args.items[0]->as.name.name);
}

static int check_fence(AstSemaFnCtx *fn, const AstExpr *expr) {
  if (!expr)
    return 0;
  const AstExpr *base = expr;
  uint32_t attr_count = 0u;
  uint32_t valid_order_count = 0u;
  int invalid_attribute = 0;
  while (base && base->kind == AST_EXPR_ATTR) {
    const AstAttribute *attr = base->as.attributed.attribute;
    attr_count++;
    if (fence_attribute_is_order(attr))
      valid_order_count++;
    else
      invalid_attribute = 1;
    base = base->as.attributed.base;
  }
  if (!base || base->kind != AST_EXPR_NAME ||
      !sema_name_eq_text(base->as.name.name, "fence"))
    return 0;

  if (attr_count == 0u || valid_order_count == 0u) {
    sema_diag(fn->sema, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "fence requires one memory order: @relaxed, @acquire, @release, "
              "@acq_rel, @seq_cst, or @order(...)");
  } else if (invalid_attribute || valid_order_count != 1u || attr_count != 1u) {
    sema_diag(fn->sema, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "fence accepts exactly one memory-order attribute");
  }
  return 1;
}

static AstFlowKind sema_check_stmt_impl(AstSemaFnCtx *fn, const AstStmt *stmt);

AstFlowKind sema_check_stmt(AstSemaFnCtx *fn, const AstStmt *stmt) {
  if (!fn || !stmt)
    return AST_FLOW_FALLTHROUGH;
  if (++fn->recursion_depth > fn->sema->config.recursion_limit) {
    fn->recursion_depth--;
    sema_diag(fn->sema, AST_SEMA_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR,
              stmt->span, fn->module->file.id,
              "semantic statement recursion limit exceeded");
    return AST_FLOW_FALLTHROUGH;
  }
  AstFlowKind flow = sema_check_stmt_impl(fn, stmt);
  fn->recursion_depth--;
  return flow;
}

static AstFlowKind sema_check_stmt_impl(AstSemaFnCtx *fn, const AstStmt *stmt) {
  AstSema *s = fn->sema;
  s->stats.statement_count++;
  switch (stmt->kind) {
  case AST_STMT_ERROR:
    return AST_FLOW_FALLTHROUGH;
  case AST_STMT_BLOCK:
    return sema_check_block(fn, stmt->as.block_stmt.block, 1);
  case AST_STMT_LET:
  case AST_STMT_CONST: {
    int is_const = stmt->kind == AST_STMT_CONST;
    AstType *ast_type =
        is_const ? stmt->as.const_stmt.type : stmt->as.let_stmt.type;
    AstExpr *value =
        is_const ? stmt->as.const_stmt.value : stmt->as.let_stmt.value;
    AstName name = is_const ? stmt->as.const_stmt.name : stmt->as.let_stmt.name;
    const AstSemaType *type =
        ast_type ? sema_resolve_type(s, fn->module, ast_type, fn, 0u) : NULL;
    AstExprInfo vi;
    memset(&vi, 0, sizeof(vi));
    if (value)
      vi = sema_check_expr(fn, value, type);
    if (!type)
      type = value ? vi.type : sema_builtin_error(s);
    if (type && !sema_type_is_value_type(type)) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                ast_type ? ast_type->span : stmt->span, fn->module->file.id,
                "local declaration requires a value type");
    }
    if (!value && !ast_type)
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, stmt->span,
                fn->module->file.id,
                "uninitialized local declaration requires an explicit type");
    if (value && type &&
        !sema_type_assignable(s, type, vi, value->span, fn->module->file.id))
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, value->span,
                fn->module->file.id,
                "local initializer is not assignable to its declared type");
    AstSemaLocal *l = add_local(fn, name, type,
                                is_const ? AST_SEMA_SYMBOL_LOCAL_CONST
                                         : AST_SEMA_SYMBOL_LOCAL,
                                !is_const, value != NULL);
    if (is_const && value && l) {
      AstSemaConstCtx cc;
      memset(&cc, 0, sizeof(cc));
      cc.sema = s;
      cc.module = fn->module;
      cc.fn = fn;
      l->const_value = sema_eval_const_expr(&cc, value, type);
      if (!sema_const_is_valid(l->const_value))
        sema_diag(s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR,
                  value->span, fn->module->file.id,
                  "local const initializer must be a compile-time expression");
    }
    return AST_FLOW_FALLTHROUGH;
  }
  case AST_STMT_ASSIGN: {
    AstSemaLocal *l = sema_fn_find_local(fn, stmt->as.assign_stmt.name);
    if (!l) {
      AstSemaSymbol *sym =
          sema_module_find_value(fn->module, stmt->as.assign_stmt.name);
      if (!sym)
        sema_diag(s, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR,
                  stmt->as.assign_stmt.name.span, fn->module->file.id,
                  "assignment target '%.*s' is unknown",
                  (int)stmt->as.assign_stmt.name.span.length,
                  stmt->as.assign_stmt.name.text);
      else if (!(sym->flags & AST_SEMA_SYMF_MUTABLE))
        sema_diag(s, AST_SEMA_DIAG_ASSIGN_TO_CONST, AST_DIAG_ERROR, stmt->span,
                  fn->module->file.id, "cannot assign to constant '%.*s'",
                  (int)sym->name.span.length, sym->name.text);
      else
        check_initializer_expr(fn, sym->type, stmt->as.assign_stmt.value,
                               stmt->span);
    } else {
      if (!l->mutable_value)
        sema_diag(s, AST_SEMA_DIAG_ASSIGN_TO_CONST, AST_DIAG_ERROR, stmt->span,
                  fn->module->file.id, "cannot assign to const local '%.*s'",
                  (int)l->name.span.length, l->name.text);
      check_initializer_expr(fn, l->type, stmt->as.assign_stmt.value,
                             stmt->span);
      l->initialized = 1u;
    }
    return AST_FLOW_FALLTHROUGH;
  }
  case AST_STMT_EXPR:
    if (!check_fence(fn, stmt->as.expr_stmt.expr))
      sema_check_expr(fn, stmt->as.expr_stmt.expr, NULL);
    return AST_FLOW_FALLTHROUGH;
  case AST_STMT_STORE: {
    const AstSemaType *t =
        sema_resolve_type(s, fn->module, stmt->as.store_stmt.type, fn, 0u);
    sema_validate_memory_attributes(fn, stmt->as.store_stmt.attributes, 1);
    AstExprInfo addr = sema_check_expr(fn, stmt->as.store_stmt.address, NULL);
    if (!sema_type_is_pointer(addr.type)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_STORE, AST_DIAG_ERROR, stmt->span,
                fn->module->file.id,
                "store address must have pointer or capability type");
    } else {
      if (addr.type->as.pointer.qualifiers & AST_TYPE_QUAL_CONST) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_STORE, AST_DIAG_ERROR, stmt->span,
                  fn->module->file.id,
                  "store through ptr<const T> is not allowed");
      }
      if (addr.type->as.pointer.element &&
          addr.type->as.pointer.element->kind != AST_SEMA_TYPE_VOID &&
          !sema_type_equal(addr.type->as.pointer.element, t)) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_STORE, AST_DIAG_ERROR, stmt->span,
                  fn->module->file.id,
                  "typed store does not match the pointee type; use an "
                  "explicit ptrcast for reinterpretation");
      }
    }
    check_initializer_expr(fn, t, stmt->as.store_stmt.value, stmt->span);
    return AST_FLOW_FALLTHROUGH;
  }
  case AST_STMT_IF:
    return check_if(fn, stmt);
  case AST_STMT_WHILE: {
    require_bool(fn, stmt->as.while_stmt.condition);
    size_t base = fn->local_count;
    uint8_t *before = base ? (uint8_t *)malloc(base) : NULL;
    if (base && !before) {
      s->oom = 1u;
      return AST_FLOW_FALLTHROUGH;
    }
    snapshot_init(fn, before, base);
    fn->loop_depth++;
    sema_check_block(fn, stmt->as.while_stmt.body, 1);
    fn->loop_depth--;
    for (size_t i = 0u; i < base; ++i) {
      fn->locals[i].initialized = before[i];
    }
    free(before);
    return AST_FLOW_FALLTHROUGH;
  }
  case AST_STMT_LOOP: {
    size_t base = fn->local_count;
    if (!push_loop_frame(fn, base))
      return AST_FLOW_FALLTHROUGH;
    fn->loop_depth++;
    AstFlowKind body_flow = sema_check_block(fn, stmt->as.loop_stmt.body, 1);
    fn->loop_depth--;
    int has_break = pop_loop_frame(fn, 1);
    if (has_break)
      return AST_FLOW_FALLTHROUGH;
    if (body_flow == AST_FLOW_RETURN || body_flow == AST_FLOW_BRANCH)
      return body_flow;
    return AST_FLOW_DIVERGE;
  }
  case AST_STMT_RETURN:
    if (stmt->as.return_stmt.value) {
      AstExprInfo v =
          sema_check_expr(fn, stmt->as.return_stmt.value, fn->return_type);
      if (!sema_type_assignable(s, fn->return_type, v,
                                stmt->as.return_stmt.value->span,
                                fn->module->file.id))
        sema_diag(s, AST_SEMA_DIAG_RETURN_TYPE, AST_DIAG_ERROR, stmt->span,
                  fn->module->file.id,
                  "returned value is incompatible with function result type");
    } else if (fn->return_type->kind != AST_SEMA_TYPE_VOID)
      sema_diag(s, AST_SEMA_DIAG_RETURN_TYPE, AST_DIAG_ERROR, stmt->span,
                fn->module->file.id, "non-void function must return a value");
    return AST_FLOW_RETURN;
  case AST_STMT_BREAK:
    if (!stmt->as.jump_stmt.has_label) {
      if (!fn->loop_depth) {
        sema_diag(s, AST_SEMA_DIAG_BREAK_OUTSIDE_LOOP, AST_DIAG_ERROR,
                  stmt->span, fn->module->file.id,
                  "break is only valid inside a loop");
      } else {
        record_loop_break(fn);
      }
    } else {
      AstSemaLabel *label = find_label(fn, stmt->as.jump_stmt.label);
      if (!label) {
        sema_diag(s, AST_SEMA_DIAG_UNKNOWN_LABEL, AST_DIAG_ERROR, stmt->span,
                  fn->module->file.id, "unknown break label");
      } else if (label->depth > fn->scope_depth) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_BRANCH_SCOPE, AST_DIAG_ERROR,
                  stmt->span, fn->module->file.id,
                  "break cannot enter a deeper lexical scope");
      } else {
        label->referenced = 1u;
      }
    }
    return stmt->as.jump_stmt.has_label ? AST_FLOW_BRANCH : AST_FLOW_BREAK;
  case AST_STMT_CONTINUE:
    if (!stmt->as.jump_stmt.has_label) {
      if (!fn->loop_depth)
        sema_diag(s, AST_SEMA_DIAG_CONTINUE_OUTSIDE_LOOP, AST_DIAG_ERROR,
                  stmt->span, fn->module->file.id,
                  "continue is only valid inside a loop");
      return AST_FLOW_CONTINUE;
    } else {
      AstSemaLabel *label = find_label(fn, stmt->as.jump_stmt.label);
      if (!label) {
        sema_diag(s, AST_SEMA_DIAG_UNKNOWN_LABEL, AST_DIAG_ERROR, stmt->span,
                  fn->module->file.id, "unknown continue label");
      } else if (label->depth > fn->scope_depth) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_BRANCH_SCOPE, AST_DIAG_ERROR,
                  stmt->span, fn->module->file.id,
                  "continue cannot enter a deeper lexical scope");
      } else {
        label->referenced = 1u;
      }
      return AST_FLOW_BRANCH;
    }
  case AST_STMT_BLOCK_LABEL:
    return AST_FLOW_FALLTHROUGH;
  case AST_STMT_BRANCH: {
    AstSemaLabel *label = find_label(fn, stmt->as.branch_stmt.label);
    if (!label) {
      sema_diag(s, AST_SEMA_DIAG_UNKNOWN_LABEL, AST_DIAG_ERROR, stmt->span,
                fn->module->file.id, "branch target label is unknown");
    } else if (label->depth > fn->scope_depth) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_BRANCH_SCOPE, AST_DIAG_ERROR,
                stmt->span, fn->module->file.id,
                "branch cannot enter a deeper lexical scope");
    } else {
      label->referenced = 1u;
    }
    return AST_FLOW_BRANCH;
  }
  case AST_STMT_UNSAFE:
    fn->unsafe_depth++;
    {
      AstFlowKind f = sema_check_block(fn, stmt->as.unsafe_stmt.body, 1);
      fn->unsafe_depth--;
      return f;
    }
  case AST_STMT_COMPTIME:
    if (stmt->as.comptime_stmt.inner) {
      if (stmt->as.comptime_stmt.inner->kind == AST_STMT_IF)
        return check_comptime_if(fn, stmt->as.comptime_stmt.inner);
      collect_labels_stmt(fn, stmt->as.comptime_stmt.inner, fn->scope_depth);
      return sema_check_stmt(fn, stmt->as.comptime_stmt.inner);
    }
    collect_labels_block(fn, stmt->as.comptime_stmt.body, fn->scope_depth + 1u);
    return sema_check_block(fn, stmt->as.comptime_stmt.body, 1);
  case AST_STMT_FOR:
    return check_comptime_for(fn, stmt);
  case AST_STMT_REQUIRES: {
    int enabled = 0;
    if (!eval_compile_time_bool(fn, stmt->as.requires_stmt.condition,
                                &enabled)) {
      require_bool(fn, stmt->as.requires_stmt.condition);
      return AST_FLOW_FALLTHROUGH;
    }
    if (!enabled)
      return AST_FLOW_FALLTHROUGH;
    collect_labels_block(fn, stmt->as.requires_stmt.body, fn->scope_depth + 1u);
    return sema_check_block(fn, stmt->as.requires_stmt.body, 1);
  }
  case AST_STMT_BIND: {
    AstSemaLocal *l = sema_fn_find_local(fn, stmt->as.bind_stmt.value);
    if (!l)
      sema_diag(s, AST_SEMA_DIAG_INVALID_NATIVE_BRIDGE, AST_DIAG_ERROR,
                stmt->span, fn->module->file.id,
                "bind source '%.*s' is not a local semantic value",
                (int)stmt->as.bind_stmt.value.span.length,
                stmt->as.bind_stmt.value.text);
    else if (!l->initialized)
      sema_diag(s, AST_SEMA_DIAG_UNINITIALIZED_VALUE, AST_DIAG_ERROR,
                stmt->span, fn->module->file.id,
                "bind source '%.*s' is uninitialized",
                (int)stmt->as.bind_stmt.value.span.length,
                stmt->as.bind_stmt.value.text);
    return AST_FLOW_FALLTHROUGH;
  }
  case AST_STMT_NATIVE:
  case AST_STMT_EXACT:
    check_native_bridge(fn, stmt);
    return AST_FLOW_FALLTHROUGH;
  case AST_STMT_STATIC_ASSERT: {
    if (stmt->as.static_assert_stmt.message) {
      AstExprInfo message =
          sema_check_expr(fn, stmt->as.static_assert_stmt.message, NULL);
      if (!message.type || message.type->kind != AST_SEMA_TYPE_STRING) {
        sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                  stmt->as.static_assert_stmt.message->span,
                  fn->module->file.id,
                  "static_assert message must be a UTF-8 string literal");
      }
    }
    size_t errors_before = s->stats.error_count;
    AstExprInfo typed = sema_check_expr(
        fn, stmt->as.static_assert_stmt.condition, sema_builtin_bool(s));
    if (!sema_type_is_bool(typed.type)) {
      if (s->stats.error_count == errors_before) {
        sema_diag(s, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR,
                  stmt->as.static_assert_stmt.condition->span,
                  fn->module->file.id,
                  "static_assert condition must have bool type");
      }
      return AST_FLOW_FALLTHROUGH;
    }
    if (s->stats.error_count != errors_before)
      return AST_FLOW_FALLTHROUGH;
    AstSemaConstCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.sema = s;
    cc.module = fn->module;
    cc.fn = fn;
    AstSemaConstValue v = sema_eval_const_expr(
        &cc, stmt->as.static_assert_stmt.condition, sema_builtin_bool(s));
    if (!sema_const_is_valid(v) || !(v.flags & AST_SEMA_CONSTF_BOOL)) {
      sema_diag(
          s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR, stmt->span,
          fn->module->file.id,
          "static_assert condition must be a compile-time bool expression");
    } else if (v.lo == 0u && v.hi == 0u) {
      sema_diag(s, AST_SEMA_DIAG_STATIC_ASSERT_FAILED, AST_DIAG_ERROR,
                stmt->span, fn->module->file.id, "static assertion failed");
    }
    return AST_FLOW_FALLTHROUGH;
  }
  default:
    return AST_FLOW_FALLTHROUGH;
  }
}

AstFlowKind sema_check_block(AstSemaFnCtx *fn, const AstBlock *block,
                             int create_scope) {
  if (!fn || !block)
    return AST_FLOW_FALLTHROUGH;
  size_t local_base = fn->local_count;
  if (create_scope)
    fn->scope_depth++;
  AstFlowKind flow = AST_FLOW_FALLTHROUGH;
  for (uint32_t i = 0u; i < block->statements.count; ++i) {
    const AstStmt *stmt = block->statements.items[i];
    if (flow != AST_FLOW_FALLTHROUGH && stmt &&
        stmt->kind == AST_STMT_BLOCK_LABEL) {
      AstSemaLabel *label = find_label(fn, stmt->as.block_label.label);
      if (label && label->referenced)
        flow = AST_FLOW_FALLTHROUGH;
    }
    if (flow == AST_FLOW_FALLTHROUGH) {
      AstFlowKind current = sema_check_stmt(fn, stmt);
      if (current != AST_FLOW_FALLTHROUGH)
        flow = current;
    } else {
      size_t preserve_count = fn->local_count;
      uint8_t *preserve =
          preserve_count ? (uint8_t *)malloc(preserve_count) : NULL;
      if (preserve_count && !preserve) {
        fn->sema->oom = 1u;
        break;
      }
      snapshot_init(fn, preserve, preserve_count);
      fn->unreachable_depth++;
      (void)sema_check_stmt(fn, stmt);
      fn->unreachable_depth--;
      for (size_t j = 0u; j < preserve_count; ++j)
        fn->locals[j].initialized = preserve[j];
      free(preserve);
    }
  }
  if (create_scope)
    leave_scope(fn, local_base);
  return flow;
}

static int check_initializer_recursive(AstSema *s, AstSemaModule *m,
                                       const AstSemaType *type,
                                       const AstInitializer *init,
                                       uint32_t depth) {
  if (!type || !init)
    return 1;
  if (depth > s->config.recursion_limit) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_INITIALIZER, AST_DIAG_ERROR, init->span,
              m->file.id,
              "initializer nesting exceeds semantic recursion limit");
    return 0;
  }
  if (init->kind == AST_INIT_EXPR) {
    AstSemaFnCtx fake;
    memset(&fake, 0, sizeof(fake));
    fake.sema = s;
    fake.module = m;
    fake.return_type = sema_builtin_void(s);
    AstExprInfo value = sema_check_expr(&fake, init->as.expr, type);
    int ok = sema_type_assignable(s, type, value, init->span, m->file.id);
    if (!ok) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_INITIALIZER, AST_DIAG_ERROR,
                init->span, m->file.id,
                "initializer expression is not assignable to the aggregate "
                "element type");
    }
    free(fake.locals);
    free(fake.labels);
    return ok;
  }

  uint64_t capacity = 0u;
  const AstSemaType **element_types = NULL;
  const AstSemaType *repeated_type = NULL;
  switch (type->kind) {
  case AST_SEMA_TYPE_ARRAY:
    capacity = type->as.array.count;
    repeated_type = type->as.array.element;
    break;
  case AST_SEMA_TYPE_TUPLE:
    capacity = type->as.tuple.count;
    element_types = type->as.tuple.items;
    break;
  case AST_SEMA_TYPE_STRUCT:
    capacity = type->as.record.field_count;
    break;
  default:
    sema_diag(s, AST_SEMA_DIAG_INVALID_INITIALIZER, AST_DIAG_ERROR, init->span,
              m->file.id,
              "brace initializer requires an array, tuple, or struct type");
    return 0;
  }

  if ((uint64_t)init->as.list.count > capacity) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_INITIALIZER, AST_DIAG_ERROR, init->span,
              m->file.id,
              "aggregate initializer contains %u elements but the destination "
              "has only %llu",
              init->as.list.count, (unsigned long long)capacity);
    return 0;
  }

  int ok = 1;
  for (uint32_t i = 0u; i < init->as.list.count; ++i) {
    const AstSemaType *element_type = repeated_type;
    if (type->kind == AST_SEMA_TYPE_TUPLE)
      element_type = element_types[i];
    else if (type->kind == AST_SEMA_TYPE_STRUCT)
      element_type = type->as.record.fields[i].type;
    if (!check_initializer_recursive(s, m, element_type, init->as.list.items[i],
                                     depth + 1u))
      ok = 0;
  }
  return ok;
}

static void check_global_initializer(AstSema *s, AstSemaModule *m,
                                     AstSemaSymbol *sym) {
  if (!sym || !sym->data_decl || !sym->data_decl->initializer || !sym->type)
    return;
  (void)check_initializer_recursive(s, m, sym->type,
                                    sym->data_decl->initializer, 0u);
}

static void check_top_const(AstSema *s, AstSemaModule *m, AstSemaSymbol *sym) {
  if (!sym || sym->kind != AST_SEMA_SYMBOL_CONST)
    return;
  const AstExpr *expr = NULL;
  if (sym->data_decl && sym->data_decl->initializer &&
      sym->data_decl->initializer->kind == AST_INIT_EXPR)
    expr = sym->data_decl->initializer->as.expr;
  else if (sym->decl && sym->decl->kind == AST_DECL_CONST)
    expr = sym->decl->as.const_decl.value;
  if (!expr)
    return;

  size_t errors_before_typecheck = s->stats.error_count;
  AstSemaFnCtx fake;
  memset(&fake, 0, sizeof(fake));
  fake.sema = s;
  fake.module = m;
  fake.return_type = sema_builtin_void(s);
  AstExprInfo typed = sema_check_expr(&fake, expr, sym->type);
  free(fake.locals);
  free(fake.labels);
  free(fake.loop_frames);

  AstSemaConstCtx cc;
  memset(&cc, 0, sizeof(cc));
  cc.sema = s;
  cc.module = m;
  AstSemaConstValue cv = sema_eval_const_expr(&cc, expr, sym->type);
  if (!sema_const_is_valid(cv)) {
    if (s->stats.error_count == errors_before_typecheck)
      sema_diag(s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR,
                expr->span, m->file.id,
                "constant initializer is not a compile-time expression");
  } else if (sym->type && s->stats.error_count == errors_before_typecheck &&
             !sema_type_assignable(
                 s, sym->type,
                 (AstExprInfo){.type = typed.type ? typed.type : cv.type,
                               .constant = cv},
                 expr->span, m->file.id)) {
    sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
              m->file.id,
              "constant initializer does not fit its declared type");
  }
  /* Function workers clone this already checked interface. Publish the
   * validated value before cloning, rather than lazily evaluating it once
   * per worker (or once per dynamically scheduled partition). Otherwise
   * imported-constant evaluation counts depend on thread scheduling. */
  if (sema_const_is_valid(cv) &&
      s->stats.error_count == errors_before_typecheck) {
    sym->const_value = cv;
    sym->flags |= AST_SEMA_SYMF_CONST_EVAL;
  }
}

static int target_feature_equal(AstSema *s, AstSpan a, AstSpan b) {
  size_t an = 0u, bn = 0u;
  const uint8_t *ap = sema_span_bytes(s, a, &an);
  const uint8_t *bp = sema_span_bytes(s, b, &bn);
  return ap && bp && an == bn && memcmp(ap, bp, an) == 0;
}

static void check_target_declarations(AstSema *s, AstSemaModule *m) {
  const AstDecl *first = NULL;
  for (uint32_t di = 0u; di < m->ast->declarations.count; ++di) {
    const AstDecl *decl = m->ast->declarations.items[di];
    if (!decl || decl->kind != AST_DECL_TARGET)
      continue;
    if (first) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_TARGET, AST_DIAG_ERROR, decl->span,
                m->file.id, "module contains more than one target declaration");
    } else {
      first = decl;
    }
    for (uint32_t i = 0u; i < decl->as.target_decl.items.count; ++i) {
      const AstTargetItem *item = &decl->as.target_decl.items.items[i];
      for (uint32_t j = 0u; j < i; ++j) {
        const AstTargetItem *previous = &decl->as.target_decl.items.items[j];
        if (item->is_feature != previous->is_feature)
          continue;
        if (item->is_feature) {
          if (target_feature_equal(s, item->feature_string,
                                   previous->feature_string)) {
            sema_diag(s, AST_SEMA_DIAG_INVALID_TARGET, AST_DIAG_ERROR,
                      item->span, m->file.id,
                      "target feature is declared more than once");
            break;
          }
        } else if (sema_name_eq(item->name, previous->name)) {
          sema_diag(s, AST_SEMA_DIAG_INVALID_TARGET, AST_DIAG_ERROR, item->span,
                    m->file.id,
                    "target property '%.*s' is declared more than once",
                    (int)item->name.span.length, item->name.text);
          break;
        }
      }
      if (item->is_feature || !item->value)
        continue;
      if (item->value->kind == AST_EXPR_STRING ||
          item->value->kind == AST_EXPR_BYTE_STRING ||
          item->value->kind == AST_EXPR_C_STRING)
        continue;
      AstSemaConstCtx cc;
      memset(&cc, 0, sizeof(cc));
      cc.sema = s;
      cc.module = m;
      AstSemaConstValue value = sema_eval_const_expr(&cc, item->value, NULL);
      if (sema_const_is_valid(value)) {
        AstExprInfo fact_info;
        memset(&fact_info, 0, sizeof(fact_info));
        fact_info.type = value.type;
        fact_info.constant = value;
        (void)sema_record_expr_fact(s, item->value, fact_info);
      }
      if (!sema_const_is_valid(value)) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_TARGET, AST_DIAG_ERROR,
                  item->value->span, m->file.id,
                  "target property value must be a compile-time literal or "
                  "constant expression");
      }
    }
  }
}

static int dispatch_condition_value(AstSema *s, AstSemaModule *m,
                                    const AstExpr *expr, int *value_out) {
  AstSemaConstCtx cc;
  memset(&cc, 0, sizeof(cc));
  cc.sema = s;
  cc.module = m;
  AstSemaConstValue value =
      sema_eval_const_expr(&cc, expr, sema_builtin_bool(s));
  if (!sema_const_is_valid(value) || (value.flags & AST_SEMA_CONSTF_BOOL) == 0u)
    return 0;
  *value_out = value.lo != 0u || value.hi != 0u;
  return 1;
}

void sema_check_function_body(AstSema *s, AstSemaModule *m,
                              AstSemaSymbol *sym) {
  if (!sym || !sym->decl || sym->decl->kind != AST_DECL_FN)
    return;
  const AstDecl *d = sym->decl;
  if (d->as.fn_decl.is_dispatch) {
    for (uint32_t i = 0u; i < d->as.fn_decl.dispatch_cases.count; ++i) {
      AstDispatchCase *dc = &d->as.fn_decl.dispatch_cases.items[i];
      if (!dc->is_else) {
        AstSemaFnCtx fake;
        memset(&fake, 0, sizeof(fake));
        fake.sema = s;
        fake.module = m;
        fake.return_type = sema_builtin_void(s);
        require_bool(&fake, dc->condition);
        int enabled = 0;
        if (!dispatch_condition_value(s, m, dc->condition, &enabled)) {
          sema_diag(
              s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR,
              dc->condition->span, m->file.id,
              "dispatch condition must be a compile-time bool expression");
        }
        free(fake.locals);
        free(fake.labels);
        (void)enabled;
      }
      AstSemaSymbol *target = sema_module_find_value(m, dc->target);
      if (!target || target->kind != AST_SEMA_SYMBOL_FUNCTION)
        sema_diag(s, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR,
                  dc->target.span, m->file.id,
                  "dispatch target '%.*s' is not a function in this module",
                  (int)dc->target.span.length, dc->target.text);
      else if (sym->type && target->type &&
               !sema_type_equal(sym->type, target->type))
        sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                  dc->target.span, m->file.id,
                  "dispatch target has a different function signature");
    }
    return;
  }
  if (!d->as.fn_decl.body)
    return;
  AstSemaFnCtx fn;
  memset(&fn, 0, sizeof(fn));
  fn.sema = s;
  fn.module = m;
  fn.function_decl = d;
  fn.is_leaf = (uint8_t)sema_decl_has_attribute(d, "leaf");
  fn.is_naked = (uint8_t)sema_decl_has_attribute(d, "naked");
  fn.return_type = sym->type && sym->type->kind == AST_SEMA_TYPE_FUNCTION
                       ? sym->type->as.function.result
                       : sema_builtin_void(s);
  fn.scope_depth = 1u;
  fn.generics = sym->generic_bindings;
  fn.generic_count = sym->generic_count;
  if (fn.is_naked) {
    for (uint32_t i = 0u; i < d->as.fn_decl.body->statements.count; ++i) {
      const AstStmt *stmt = d->as.fn_decl.body->statements.items[i];
      if (!stmt ||
          (stmt->kind != AST_STMT_NATIVE && stmt->kind != AST_STMT_EXACT)) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR,
                  stmt ? stmt->span : d->as.fn_decl.body->span, m->file.id,
                  "@naked function body may contain only native/exact blocks");
      }
    }
  }
  collect_labels_block(&fn, d->as.fn_decl.body, fn.scope_depth);
  for (uint32_t i = 0u; i < d->as.fn_decl.params.count; ++i) {
    const AstSemaType *pt =
        sym->type && sym->type->kind == AST_SEMA_TYPE_FUNCTION &&
                i < sym->type->as.function.param_count
            ? sym->type->as.function.params[i]
            : sema_resolve_type(s, m, d->as.fn_decl.params.items[i].type, &fn,
                                0u);
    add_local(&fn, d->as.fn_decl.params.items[i].name, pt,
              AST_SEMA_SYMBOL_PARAMETER, 0, 1);
  }
  AstFlowKind flow = sema_check_block(&fn, d->as.fn_decl.body, 0);
  if (!fn.is_naked && fn.return_type->kind != AST_SEMA_TYPE_VOID &&
      flow != AST_FLOW_RETURN && flow != AST_FLOW_DIVERGE)
    sema_diag(s, AST_SEMA_DIAG_MISSING_RETURN, AST_DIAG_ERROR, d->span,
              m->file.id,
              "non-void function may reach the end without returning a value");
  for (size_t i = 0u; i < fn.loop_frame_count; ++i)
    free(fn.loop_frames[i].break_state);
  free(fn.loop_frames);
  free(fn.locals);
  free(fn.labels);
}

static void check_top_static_assert(AstSema *s, AstSemaModule *m,
                                    const AstDecl *d) {
  size_t errors_before = s->stats.error_count;
  AstSemaFnCtx fake;
  memset(&fake, 0, sizeof(fake));
  fake.sema = s;
  fake.module = m;
  fake.return_type = sema_builtin_void(s);
  if (d->as.static_assert_decl.message) {
    AstExprInfo message =
        sema_check_expr(&fake, d->as.static_assert_decl.message, NULL);
    if (!message.type || message.type->kind != AST_SEMA_TYPE_STRING) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                d->as.static_assert_decl.message->span, m->file.id,
                "static_assert message must be a UTF-8 string literal");
    }
  }
  AstExprInfo typed = sema_check_expr(&fake, d->as.static_assert_decl.condition,
                                      sema_builtin_bool(s));
  free(fake.locals);
  free(fake.labels);
  free(fake.loop_frames);
  if (!sema_type_is_bool(typed.type)) {
    if (s->stats.error_count == errors_before) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR,
                d->as.static_assert_decl.condition->span, m->file.id,
                "static_assert condition must have bool type");
    }
    return;
  }
  if (s->stats.error_count != errors_before)
    return;
  AstSemaConstCtx cc;
  memset(&cc, 0, sizeof(cc));
  cc.sema = s;
  cc.module = m;
  AstSemaConstValue v = sema_eval_const_expr(
      &cc, d->as.static_assert_decl.condition, sema_builtin_bool(s));
  if (!sema_const_is_valid(v) || !(v.flags & AST_SEMA_CONSTF_BOOL)) {
    sema_diag(s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR, d->span,
              m->file.id,
              "static_assert condition must be a compile-time bool expression");
  } else if (v.lo == 0u && v.hi == 0u) {
    sema_diag(s, AST_SEMA_DIAG_STATIC_ASSERT_FAILED, AST_DIAG_ERROR, d->span,
              m->file.id, "static assertion failed");
  }
}

int sema_check_module_bodies(AstSema *s) {
  if (!s)
    return -1;

  size_t function_count = 0u;
  for (size_t mi = 0u; mi < s->module_count; ++mi) {
    AstSemaModule *m = &s->modules[mi];
    if (!m->ast)
      continue;
    sema_validate_module_attributes(s, m);
    check_target_declarations(s, m);
    for (size_t si = 0u; si < m->symbol_count; ++si) {
      AstSemaSymbol *sym = &m->symbols[si];
      if (sym->kind == AST_SEMA_SYMBOL_CONST) {
        check_top_const(s, m, sym);
        if (sym->data_decl)
          check_global_initializer(s, m, sym);
      }
      if (sym->kind == AST_SEMA_SYMBOL_GLOBAL)
        check_global_initializer(s, m, sym);
      if (sym->kind == AST_SEMA_SYMBOL_FUNCTION)
        function_count++;
    }
    for (uint32_t di = 0u; di < m->ast->declarations.count; ++di) {
      const AstDecl *d = m->ast->declarations.items[di];
      if (d && d->kind == AST_DECL_STATIC_ASSERT)
        check_top_static_assert(s, m, d);
    }
  }
  if (s->oom || function_count == 0u)
    return s->oom ? -1 : 0;

  size_t *module_indices =
      (size_t *)malloc(function_count * sizeof(*module_indices));
  size_t *symbol_indices =
      (size_t *)malloc(function_count * sizeof(*symbol_indices));
  if (!module_indices || !symbol_indices) {
    free(module_indices);
    free(symbol_indices);
    s->oom = 1u;
    return -1;
  }
  size_t cursor = 0u;
  for (size_t mi = 0u; mi < s->module_count; ++mi) {
    AstSemaModule *m = &s->modules[mi];
    if (!m->ast)
      continue;
    for (size_t si = 0u; si < m->symbol_count; ++si) {
      if (m->symbols[si].kind != AST_SEMA_SYMBOL_FUNCTION)
        continue;
      module_indices[cursor] = mi;
      symbol_indices[cursor] = si;
      cursor++;
    }
  }

  int rc = 0;
  size_t requested_threads = s->config.thread_count
                                 ? s->config.thread_count
                                 : ast_hardware_thread_count();
  if (requested_threads <= 1u || function_count <= 1u) {
    for (size_t i = 0u; i < function_count; ++i) {
      sema_check_function_body(
          s, &s->modules[module_indices[i]],
          &s->modules[module_indices[i]].symbols[symbol_indices[i]]);
    }
  } else {
    rc = sema_check_functions_parallel(s, module_indices, symbol_indices,
                                       function_count);
  }
  free(module_indices);
  free(symbol_indices);
  return rc != 0 || s->oom ? -1 : 0;
}
