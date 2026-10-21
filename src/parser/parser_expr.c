#include "parser_internal.h"

static int ast_binary_bp(AstTokenKind k, int *left_bp, int *right_bp) {
  int p = 0;
  switch (k) {
  case AST_TOK_PIPE_PIPE:
    p = 1;
    break;
  case AST_TOK_AMP_AMP:
    p = 2;
    break;
  case AST_TOK_PIPE:
    p = 3;
    break;
  case AST_TOK_CARET:
    p = 4;
    break;
  case AST_TOK_AMP:
    p = 5;
    break;
  case AST_TOK_EQ_EQ:
  case AST_TOK_BANG_EQ:
    p = 6;
    break;
  case AST_TOK_LT:
  case AST_TOK_LT_EQ:
  case AST_TOK_GT:
  case AST_TOK_GT_EQ:
    p = 7;
    break;
  case AST_TOK_SHIFT_LEFT:
  case AST_TOK_SHIFT_RIGHT:
    p = 8;
    break;
  case AST_TOK_PLUS:
  case AST_TOK_MINUS:
    p = 9;
    break;
  case AST_TOK_STAR:
  case AST_TOK_SLASH:
  case AST_TOK_PERCENT:
    p = 10;
    break;
  default:
    return 0;
  }
  *left_bp = p * 2;
  *right_bp = p * 2 + 1;
  return 1;
}

static int ast_is_unary(AstTokenKind k) {
  return k == AST_TOK_BANG || k == AST_TOK_TILDE || k == AST_TOK_MINUS ||
         k == AST_TOK_PLUS || k == AST_TOK_AMP || k == AST_TOK_STAR;
}

static AstExprKind ast_literal_expr_kind(AstTokenKind k) {
  switch (k) {
  case AST_TOK_INTEGER:
    return AST_EXPR_INTEGER;
  case AST_TOK_FLOAT:
    return AST_EXPR_FLOAT;
  case AST_TOK_STRING:
    return AST_EXPR_STRING;
  case AST_TOK_BYTE_STRING:
    return AST_EXPR_BYTE_STRING;
  case AST_TOK_C_STRING:
    return AST_EXPR_C_STRING;
  case AST_TOK_CHAR:
    return AST_EXPR_CHAR;
  default:
    return AST_EXPR_ERROR;
  }
}

static int ast_is_expr_name_keyword(AstTokenKind k) {
  return ast_p_is_contextual_name(k);
}

/* Delimiters establish a value context inside a generic constant argument. */
static AstExpr *ast_parse_delimited_expression(AstParser *p) {
  uint32_t depth = p->generic_const_expr_depth;
  p->generic_const_expr_depth = 0u;
  AstExpr *expr = ast_parse_expression(p);
  p->generic_const_expr_depth = depth;
  return expr;
}

static AstExpr *ast_parse_primary(AstParser *p);

static int ast_expr_name_is(const AstExpr *expr, const char *text) {
  return expr && expr->kind == AST_EXPR_NAME &&
         ast_name_equal_cstr(expr->as.name.name, text);
}

static int ast_intrinsic_type_operand_position(const AstExpr *callee,
                                               uint32_t arg_index) {
  if (arg_index != 0u)
    return 0;
  return ast_expr_name_is(callee, "sizeof") ||
         ast_expr_name_is(callee, "offsetof");
}

static int ast_token_starts_unambiguous_type_operand(AstParser *p) {
  AstTokenKind k = ast_p_kind(p);
  if (k == AST_TOK_IDENTIFIER)
    return ast_p_peek_kind(p, 1u) == AST_TOK_LT;
  if (k == AST_TOK_LPAREN)
    return 0;
  return ast_is_type_start(k);
}

static AstExpr *ast_parse_type_operand_expr(AstParser *p) {
  AstType *type = ast_parse_type(p);
  AstExpr *expr =
      ast_new_expr(p, AST_EXPR_TYPE, type ? type->span : ast_span_current(p));
  if (expr)
    expr->as.type_operand.type = type;
  return expr;
}

static int ast_is_atomic_order_attribute(AstName name) {
  return ast_name_equal_cstr(name, "relaxed") ||
         ast_name_equal_cstr(name, "acquire") ||
         ast_name_equal_cstr(name, "release") ||
         ast_name_equal_cstr(name, "acq_rel") ||
         ast_name_equal_cstr(name, "seq_cst") ||
         ast_name_equal_cstr(name, "order") ||
         ast_name_equal_cstr(name, "success") ||
         ast_name_equal_cstr(name, "failure");
}

static AstExpr *ast_peel_atomic_attributes(AstParser *p, AstExpr *expr,
                                           AstBuildVec *attrs) {
  /* Postfix attributes bind inside a unary operand or the rightmost arm of
     a value expression. Lift trailing orders from that spine as well, so
     `&value @release`, `-value @relaxed` and `a + b @acq_rel` have the same
     atomic-order contract as a plain value. Do not enter calls, loads, or
     nested atomic expressions: those own their argument/attribute scopes. */
  AstExpr **tail = &expr;
  while (*tail) {
    AstExpr *node = *tail;
    if (node->kind == AST_EXPR_ATTR && node->as.attributed.attribute &&
        ast_is_atomic_order_attribute(node->as.attributed.attribute->name)) {
      AstAttribute attr = *node->as.attributed.attribute;
      if (!ast_vec_push(p, attrs, &attr))
        return expr;
      *tail = node->as.attributed.base;
    } else if (node->kind == AST_EXPR_UNARY) {
      tail = &node->as.unary.operand;
    } else if (node->kind == AST_EXPR_BINARY) {
      tail = &node->as.binary.right;
    } else if (node->kind == AST_EXPR_CONDITIONAL) {
      tail = &node->as.conditional.else_expr;
    } else {
      break;
    }
  }
  return expr;
}

static AstExpr *ast_parse_load(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstExpr *e = ast_new_expr(p, AST_EXPR_LOAD, ast_span_token(p, start));
  ast_p_expect(p, AST_TOK_DOT, "'.'");
  AstType *type = ast_parse_type(p);
  ast_p_expect(p, AST_TOK_LBRACKET, "'['");
  AstExpr *address = ast_parse_delimited_expression(p);
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACKET, "']'");
  AstAttributeArray attrs = ast_parse_attributes(p);
  if (e) {
    e->as.load.type = type;
    e->as.load.address = address;
    e->as.load.attributes = attrs;
    if (rb)
      e->span = ast_span_join(e->span, ast_span_token(p, rb));
    if (attrs.count)
      e->span = ast_span_join(e->span, attrs.items[attrs.count - 1u].span);
  }
  return e;
}

static AstExpr *ast_parse_cast(AstParser *p) {
  const AstToken *op = ast_p_advance(p);
  AstExpr *e = ast_new_expr(p, AST_EXPR_CAST, ast_span_token(p, op));
  ast_p_expect(p, AST_TOK_LT, "'<'");
  AstType *target = ast_parse_type(p);
  ast_p_expect(p, AST_TOK_GT, "'>'");
  ast_p_expect(p, AST_TOK_LPAREN, "'('");
  AstExpr *value = ast_parse_delimited_expression(p);
  const AstToken *rp = ast_p_expect(p, AST_TOK_RPAREN, "')'");
  if (e) {
    e->as.cast.op = (AstTokenKind)op->kind;
    e->as.cast.target = target;
    e->as.cast.value = value;
    if (rp)
      e->span = ast_span_join(e->span, ast_span_token(p, rp));
  }
  return e;
}

static AstExpr *ast_parse_atomic(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstExpr *e = ast_new_expr(p, AST_EXPR_ATOMIC, ast_span_token(p, start));
  ast_p_expect(p, AST_TOK_DOT, "'.'");
  AstName op;
  memset(&op, 0, sizeof(op));
  ast_parse_name(p, &op, 1);
  ast_p_expect(p, AST_TOK_DOT, "'.'");
  if (op.text && op.span.length == 3u && memcmp(op.text, "rmw", 3u) == 0 &&
      ast_p_is_word(ast_p_kind(p)) && ast_p_peek_kind(p, 1u) == AST_TOK_DOT) {
    AstSpan op_span = op.span;
    op_span = ast_span_join(op_span, ast_span_token(p, ast_p_advance(p)));
    op = ast_name_from_span(p, op_span);
    ast_p_expect(p, AST_TOK_DOT, "'.'");
  }
  AstType *type = ast_parse_type(p);
  ast_p_expect(p, AST_TOK_LBRACKET, "'['");
  AstExpr *address = ast_parse_delimited_expression(p);
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACKET, "']'");

  AstBuildVec args;
  AstBuildVec atomic_attrs;
  ast_vec_init(&args, sizeof(AstExpr *), ASTER_ALIGNOF(AstExpr *));
  ast_vec_init(&atomic_attrs, sizeof(AstAttribute),
               ASTER_ALIGNOF(AstAttribute));
  if (op.text && op.span.length == 5u && memcmp(op.text, "store", 5u) == 0 &&
      ast_p_match(p, AST_TOK_ASSIGN)) {
    AstExpr *arg = ast_parse_expression(p);
    arg = ast_peel_atomic_attributes(p, arg, &atomic_attrs);
    ast_vec_push(p, &args, &arg);
  }
  while (ast_p_match(p, AST_TOK_COMMA)) {
    AstExpr *arg = ast_parse_expression(p);
    arg = ast_peel_atomic_attributes(p, arg, &atomic_attrs);
    ast_vec_push(p, &args, &arg);
  }
  AstAttributeArray direct_attrs = ast_parse_attributes(p);
  for (uint32_t i = 0u; i < direct_attrs.count; ++i)
    ast_vec_push(p, &atomic_attrs, &direct_attrs.items[i]);
  AstAttributeArray attrs;
  attrs.items = (AstAttribute *)ast_vec_finish(p, &atomic_attrs);
  attrs.count = atomic_attrs.count;
  if (e) {
    e->as.atomic.op = op;
    e->as.atomic.type = type;
    e->as.atomic.address = address;
    e->as.atomic.args.items = (AstExpr **)ast_vec_finish(p, &args);
    e->as.atomic.args.count = args.count;
    e->as.atomic.attributes = attrs;
    if (rb)
      e->span = ast_span_join(e->span, ast_span_token(p, rb));
    if (attrs.count)
      e->span = ast_span_join(e->span, attrs.items[attrs.count - 1u].span);
  }
  return e;
}

static AstExpr *ast_parse_primary(AstParser *p) {
  AstTokenKind k = ast_p_kind(p);
  if (k == AST_TOK_INTEGER || k == AST_TOK_FLOAT || k == AST_TOK_STRING ||
      k == AST_TOK_BYTE_STRING || k == AST_TOK_C_STRING || k == AST_TOK_CHAR) {
    const AstToken *tok = ast_p_advance(p);
    AstExpr *e =
        ast_new_expr(p, ast_literal_expr_kind(k), ast_span_token(p, tok));
    if (e)
      e->as.literal.token_kind = k;
    return e;
  }
  if (k == AST_TOK_KW_TRUE || k == AST_TOK_KW_FALSE) {
    const AstToken *tok = ast_p_advance(p);
    AstExpr *e = ast_new_expr(p, AST_EXPR_BOOL, ast_span_token(p, tok));
    if (e)
      e->as.literal.token_kind = k;
    return e;
  }
  if (k == AST_TOK_IDENTIFIER || ast_is_expr_name_keyword(k)) {
    const AstToken *tok = ast_p_advance(p);
    AstExpr *e = ast_new_expr(p, AST_EXPR_NAME, ast_span_token(p, tok));
    if (e)
      e->as.name.name = ast_name_from_token(p, tok);
    return e;
  }
  if (k == AST_TOK_DOLLAR) {
    const AstToken *start = ast_p_advance(p);
    AstName name;
    memset(&name, 0, sizeof(name));
    ast_parse_name(p, &name, 0);
    AstExpr *e =
        ast_new_expr(p, AST_EXPR_MACRO_REF,
                     ast_span_join(ast_span_token(p, start), name.span));
    if (e)
      e->as.macro_ref.name = name;
    return e;
  }
  if (k == AST_TOK_LPAREN) {
    const AstToken *lp = ast_p_advance(p);
    uint32_t saved_generic_const_depth = p->generic_const_expr_depth;
    p->generic_const_expr_depth = 0u;
    AstExpr *e = ast_parse_expression(p);
    p->generic_const_expr_depth = saved_generic_const_depth;
    const AstToken *rp = ast_p_expect(p, AST_TOK_RPAREN, "')'");
    if (e) {
      AstSpan outer = ast_span_token(p, lp);
      if (rp)
        outer = ast_span_join(outer, ast_span_token(p, rp));
      e->span = outer;
    }
    return e;
  }
  if (k == AST_TOK_KW_LOAD)
    return ast_parse_load(p);
  if (k == AST_TOK_KW_ATOMIC)
    return ast_parse_atomic(p);
  if (k == AST_TOK_KW_CAST || k == AST_TOK_KW_TRUNC || k == AST_TOK_KW_ZEXT ||
      k == AST_TOK_KW_SEXT || k == AST_TOK_KW_BITCAST ||
      k == AST_TOK_KW_PTRCAST) {
    return ast_parse_cast(p);
  }
  ast_p_diag(p, AST_PARSE_DIAG_EXPECTED_EXPRESSION, AST_DIAG_ERROR,
             ast_span_current(p), "expected expression, found %s",
             ast_token_kind_name(k));
  const AstToken *bad = ast_p_advance(p);
  return ast_new_expr(p, AST_EXPR_ERROR, ast_span_token(p, bad));
}

static AstExpr *ast_parse_postfix(AstParser *p) {
  AstExpr *left = ast_parse_primary(p);
  for (;;) {
    if (ast_p_match(p, AST_TOK_LPAREN)) {
      AstBuildVec args;
      ast_vec_init(&args, sizeof(AstExpr *), ASTER_ALIGNOF(AstExpr *));
      if (ast_p_kind(p) != AST_TOK_RPAREN) {
        for (;;) {
          uint32_t arg_index = args.count;
          AstExpr *arg;
          if (ast_intrinsic_type_operand_position(left, arg_index) &&
              ast_token_starts_unambiguous_type_operand(p))
            arg = ast_parse_type_operand_expr(p);
          else
            arg = ast_parse_delimited_expression(p);
          ast_vec_push(p, &args, &arg);
          if (!ast_p_match(p, AST_TOK_COMMA))
            break;
          if (ast_p_kind(p) == AST_TOK_RPAREN)
            break;
        }
      }
      const AstToken *rp = ast_p_expect(p, AST_TOK_RPAREN, "')'");
      AstSpan span = left ? left->span : ast_span_current(p);
      if (rp)
        span = ast_span_join(span, ast_span_token(p, rp));
      AstExpr *call = ast_new_expr(p, AST_EXPR_CALL, span);
      if (call) {
        call->as.call.callee = left;
        call->as.call.args.items = (AstExpr **)ast_vec_finish(p, &args);
        call->as.call.args.count = args.count;
      }
      left = call;
      continue;
    }
    if (ast_p_match(p, AST_TOK_LBRACKET)) {
      AstExpr *index = ast_parse_delimited_expression(p);
      const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACKET, "']'");
      AstSpan span = left ? left->span : ast_span_current(p);
      if (rb)
        span = ast_span_join(span, ast_span_token(p, rb));
      AstExpr *idx = ast_new_expr(p, AST_EXPR_INDEX, span);
      if (idx) {
        idx->as.index.base = left;
        idx->as.index.index = index;
      }
      left = idx;
      continue;
    }
    if (ast_p_match(p, AST_TOK_DOT)) {
      AstName member;
      memset(&member, 0, sizeof(member));
      ast_parse_name(p, &member, 1);
      AstExpr *m = ast_new_expr(p, AST_EXPR_MEMBER,
                                left ? ast_span_join(left->span, member.span)
                                     : member.span);
      if (m) {
        m->as.member.base = left;
        m->as.member.member = member;
      }
      left = m;
      continue;
    }
    if (ast_p_kind(p) == AST_TOK_AT) {
      AstAttributeArray attrs = ast_parse_attributes(p);
      for (uint32_t i = 0; i < attrs.count; ++i) {
        AstExpr *a =
            ast_new_expr(p, AST_EXPR_ATTR,
                         left ? ast_span_join(left->span, attrs.items[i].span)
                              : attrs.items[i].span);
        if (a) {
          AstAttribute *copy = AST_ARENA_NEW(&p->result->arena, AstAttribute);
          if (!copy)
            p->out_of_memory = 1u;
          else
            *copy = attrs.items[i];
          a->as.attributed.base = left;
          a->as.attributed.attribute = copy;
        }
        left = a;
      }
      continue;
    }
    break;
  }
  return left;
}

static AstExpr *ast_parse_prefix(AstParser *p) {
  AstTokenKind k = ast_p_kind(p);
  if (ast_is_unary(k)) {
    const AstToken *op = ast_p_advance(p);
    AstExpr *rhs = ast_parse_expression_bp(p, 22);
    AstExpr *e =
        ast_new_expr(p, AST_EXPR_UNARY,
                     rhs ? ast_span_join(ast_span_token(p, op), rhs->span)
                         : ast_span_token(p, op));
    if (e) {
      e->as.unary.op = k;
      e->as.unary.operand = rhs;
    }
    return e;
  }
  return ast_parse_postfix(p);
}

AstExpr *ast_parse_expression_bp(AstParser *p, int min_bp) {
  if (!ast_p_enter(p)) {
    AstExpr *e = ast_new_expr(p, AST_EXPR_ERROR, ast_span_current(p));
    ast_p_leave(p);
    return e;
  }
  AstExpr *left = ast_parse_prefix(p);
  for (;;) {
    int lbp, rbp;
    AstTokenKind op_kind = ast_p_kind(p);
    if (p->generic_const_expr_depth != 0u &&
        (op_kind == AST_TOK_GT || op_kind == AST_TOK_SHIFT_RIGHT ||
         op_kind == AST_TOK_GT_EQ))
      break;
    if (!ast_binary_bp(op_kind, &lbp, &rbp) || lbp < min_bp)
      break;
    const AstToken *op = ast_p_advance(p);
    AstExpr *right = ast_parse_expression_bp(p, rbp);
    AstSpan span = left ? left->span : ast_span_token(p, op);
    if (right)
      span = ast_span_join(span, right->span);
    AstExpr *binary = ast_new_expr(p, AST_EXPR_BINARY, span);
    if (binary) {
      binary->as.binary.op = op_kind;
      binary->as.binary.left = left;
      binary->as.binary.right = right;
    }
    left = binary;
  }
  ast_p_leave(p);
  return left;
}

AstExpr *ast_parse_expression(AstParser *p) {
  AstExpr *condition = ast_parse_expression_bp(p, 0);
  if (ast_p_match(p, AST_TOK_QUESTION)) {
    AstExpr *then_expr = ast_parse_expression(p);
    ast_p_expect(p, AST_TOK_COLON, "':'");
    AstExpr *else_expr = ast_parse_expression(p);
    AstSpan span = condition ? condition->span : ast_span_current(p);
    if (else_expr)
      span = ast_span_join(span, else_expr->span);
    AstExpr *e = ast_new_expr(p, AST_EXPR_CONDITIONAL, span);
    if (e) {
      e->as.conditional.condition = condition;
      e->as.conditional.then_expr = then_expr;
      e->as.conditional.else_expr = else_expr;
    }
    return e;
  }
  return condition;
}
