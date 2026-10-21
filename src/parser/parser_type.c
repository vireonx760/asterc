#include "parser_internal.h"

static int ast_is_primitive(AstTokenKind k) {
  return k >= AST_TOK_KW_VOID && k <= AST_TOK_KW_F64;
}

static AstType *ast_parse_type_inner(AstParser *p);

typedef struct AstTypeParseCheckpoint {
  AstTokenCursor cursor;
  const AstToken *current;
  const AstToken *previous;
  AstToken split_gt_first;
  AstToken split_gt_second;
  size_t token_index;
  size_t error_count;
  size_t ast_nodes;
  uint32_t expr_node_count;
  uint32_t type_node_count;
  uint32_t depth;
  uint32_t diagnostic_count;
  uint32_t expr_next;
  uint32_t type_next;
  uint32_t stmt_next;
  uint32_t decl_next;
  uint32_t block_next;
  uint8_t split_gt_pending;
  uint8_t panic_mode;
  uint8_t diag_limit_reported;
} AstTypeParseCheckpoint;

static void ast_type_checkpoint_save(const AstParser *p,
                                     AstTypeParseCheckpoint *cp) {
  memset(cp, 0, sizeof(*cp));
  cp->cursor = p->cursor;
  cp->current = p->current;
  cp->previous = p->previous;
  cp->split_gt_first = p->split_gt_first;
  cp->split_gt_second = p->split_gt_second;
  cp->token_index = p->token_index;
  cp->error_count = p->result->error_count;
  cp->ast_nodes = p->ast_nodes;
  cp->expr_node_count = p->expr_node_count;
  cp->type_node_count = p->type_node_count;
  cp->depth = p->depth;
  cp->diagnostic_count = p->diagnostics.count;
  cp->expr_next = p->expr_pool.next_index;
  cp->type_next = p->type_pool.next_index;
  cp->stmt_next = p->stmt_pool.next_index;
  cp->decl_next = p->decl_pool.next_index;
  cp->block_next = p->block_pool.next_index;
  cp->split_gt_pending = p->split_gt_pending;
  cp->panic_mode = p->panic_mode;
  cp->diag_limit_reported = p->diag_limit_reported;
}

static void ast_type_checkpoint_restore(AstParser *p,
                                        const AstTypeParseCheckpoint *cp) {
  p->cursor = cp->cursor;
  p->current = cp->current;
  p->previous = cp->previous;
  p->split_gt_first = cp->split_gt_first;
  p->split_gt_second = cp->split_gt_second;
  p->token_index = cp->token_index;
  p->result->error_count = cp->error_count;
  p->ast_nodes = cp->ast_nodes;
  p->expr_node_count = cp->expr_node_count;
  p->type_node_count = cp->type_node_count;
  p->depth = cp->depth;
  p->diagnostics.count = cp->diagnostic_count;
  p->expr_pool.next_index = cp->expr_next;
  p->type_pool.next_index = cp->type_next;
  p->stmt_pool.next_index = cp->stmt_next;
  p->decl_pool.next_index = cp->decl_next;
  p->block_pool.next_index = cp->block_next;
  p->split_gt_pending = cp->split_gt_pending;
  p->panic_mode = cp->panic_mode;
  p->diag_limit_reported = cp->diag_limit_reported;
}

static int ast_generic_arg_delimiter(AstTokenKind kind) {
  return kind == AST_TOK_COMMA || kind == AST_TOK_GT ||
         kind == AST_TOK_SHIFT_RIGHT || kind == AST_TOK_GT_EQ;
}

static int ast_generic_const_obvious_start(AstTokenKind kind) {
  return kind == AST_TOK_INTEGER || kind == AST_TOK_FLOAT ||
         kind == AST_TOK_CHAR || kind == AST_TOK_STRING ||
         kind == AST_TOK_BYTE_STRING || kind == AST_TOK_C_STRING ||
         kind == AST_TOK_KW_TRUE || kind == AST_TOK_KW_FALSE ||
         kind == AST_TOK_PLUS || kind == AST_TOK_MINUS ||
         kind == AST_TOK_BANG || kind == AST_TOK_TILDE ||
         kind == AST_TOK_KW_CAST || kind == AST_TOK_KW_TRUNC ||
         kind == AST_TOK_KW_ZEXT || kind == AST_TOK_KW_SEXT ||
         kind == AST_TOK_KW_BITCAST || kind == AST_TOK_KW_PTRCAST ||
         kind == AST_TOK_KW_LOAD || kind == AST_TOK_KW_ATOMIC ||
         0; /* contextual identifiers are resolved by type/value lookahead */
}

static AstExpr *ast_parse_generic_const_expr(AstParser *p) {
  p->generic_const_expr_depth++;
  AstExpr *expr = ast_parse_expression(p);
  p->generic_const_expr_depth--;
  return expr;
}

AstAttributeArray ast_parse_attributes(AstParser *p) {
  AstBuildVec vec;
  ast_vec_init(&vec, sizeof(AstAttribute), ASTER_ALIGNOF(AstAttribute));
  while (ast_p_kind(p) == AST_TOK_AT) {
    const AstToken *at = ast_p_advance(p);
    AstAttribute a;
    memset(&a, 0, sizeof(a));
    a.span = ast_span_token(p, at);
    if (!ast_parse_name(p, &a.name, 1)) {
      ast_p_sync_statement(p);
      continue;
    }
    a.span = ast_span_join(a.span, a.name.span);
    if (ast_p_match(p, AST_TOK_LPAREN)) {
      AstBuildVec args;
      ast_vec_init(&args, sizeof(AstExpr *), ASTER_ALIGNOF(AstExpr *));
      if (ast_p_kind(p) != AST_TOK_RPAREN) {
        for (;;) {
          AstExpr *expr = ast_parse_expression(p);
          ast_vec_push(p, &args, &expr);
          if (!ast_p_match(p, AST_TOK_COMMA))
            break;
          if (ast_p_kind(p) == AST_TOK_RPAREN)
            break;
        }
      }
      const AstToken *rp = ast_p_expect(p, AST_TOK_RPAREN, "')'");
      if (rp)
        a.span = ast_span_join(a.span, ast_span_token(p, rp));
      a.args.items = (AstExpr **)ast_vec_finish(p, &args);
      a.args.count = args.count;
    }
    ast_vec_push(p, &vec, &a);
  }
  AstAttributeArray out;
  out.items = (AstAttribute *)ast_vec_finish(p, &vec);
  out.count = vec.count;
  return out;
}

static uint32_t ast_parse_optional_qualifier(AstParser *p) {
  uint32_t q = AST_TYPE_QUAL_NONE;
  if (ast_p_match(p, AST_TOK_KW_CONST))
    q |= AST_TYPE_QUAL_CONST;
  else if (ast_p_match(p, AST_TOK_KW_VOLATILE))
    q |= AST_TYPE_QUAL_VOLATILE;
  return q;
}

static AstType *ast_parse_named_type(AstParser *p) {
  AstName name;
  AstSpan start = ast_span_current(p);
  if (!ast_parse_qualified_name(p, &name))
    return ast_new_type(p, AST_TYPE_ERROR, start);
  AstType *type = ast_new_type(p, AST_TYPE_NAMED, name.span);
  if (!type)
    return NULL;
  type->as.named.name = name;

  if (ast_p_match(p, AST_TOK_LT)) {
    AstBuildVec args;
    ast_vec_init(&args, sizeof(AstTypeArg), ASTER_ALIGNOF(AstTypeArg));
    if (ast_p_kind(p) != AST_TOK_GT) {
      for (;;) {
        AstTypeArg arg;
        memset(&arg, 0, sizeof(arg));
        arg.span = ast_span_current(p);
        AstTokenKind k = ast_p_kind(p);
        if (ast_generic_const_obvious_start(k)) {
          arg.kind = AST_TYPE_ARG_CONST;
          arg.as.expr = ast_parse_generic_const_expr(p);
          if (arg.as.expr)
            arg.span = arg.as.expr->span;
        } else if (k == AST_TOK_IDENTIFIER || ast_p_is_contextual_name(k) ||
                   k == AST_TOK_LPAREN) {
          AstTypeParseCheckpoint checkpoint;
          ast_type_checkpoint_save(p, &checkpoint);
          AstType *candidate = ast_parse_type_inner(p);
          int type_ok = candidate && candidate->kind != AST_TYPE_ERROR &&
                        p->diagnostics.count == checkpoint.diagnostic_count &&
                        ast_generic_arg_delimiter(ast_p_kind(p));
          if (type_ok) {
            arg.kind = AST_TYPE_ARG_TYPE;
            arg.as.type = candidate;
            arg.span = candidate->span;
          } else {
            ast_type_checkpoint_restore(p, &checkpoint);
            arg.kind = AST_TYPE_ARG_CONST;
            arg.as.expr = ast_parse_generic_const_expr(p);
            if (arg.as.expr)
              arg.span = arg.as.expr->span;
          }
        } else {
          arg.kind = AST_TYPE_ARG_TYPE;
          arg.as.type = ast_parse_type_inner(p);
          if (arg.as.type)
            arg.span = arg.as.type->span;
        }
        ast_vec_push(p, &args, &arg);
        if (!ast_p_match(p, AST_TOK_COMMA))
          break;
      }
    }
    const AstToken *gt = ast_p_expect(p, AST_TOK_GT, "'>'");
    if (gt)
      type->span = ast_span_join(type->span, ast_span_token(p, gt));
    type->as.named.args.items = (AstTypeArg *)ast_vec_finish(p, &args);
    type->as.named.args.count = args.count;
  }
  return type;
}

static AstType *ast_parse_type_inner(AstParser *p) {
  if (!ast_p_enter(p)) {
    AstType *err = ast_new_type(p, AST_TYPE_ERROR, ast_span_current(p));
    ast_p_leave(p);
    return err;
  }
  AstTokenKind k = ast_p_kind(p);
  AstType *out = NULL;

  if (ast_is_primitive(k)) {
    const AstToken *tok = ast_p_advance(p);
    out = ast_new_type(p, AST_TYPE_PRIMITIVE, ast_span_token(p, tok));
    if (out)
      out->as.primitive.token_kind = k;
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_IDENTIFIER || ast_p_is_contextual_name(k)) {
    out = ast_parse_named_type(p);
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_KW_PTR || k == AST_TOK_KW_CAP) {
    const AstToken *start = ast_p_advance(p);
    AstTypeKind kind = k == AST_TOK_KW_PTR ? AST_TYPE_PTR : AST_TYPE_CAP;
    out = ast_new_type(p, kind, ast_span_token(p, start));
    ast_p_expect(p, AST_TOK_LT, "'<'");
    uint32_t q = ast_parse_optional_qualifier(p);
    AstType *elem = ast_parse_type_inner(p);
    const AstToken *gt = ast_p_expect(p, AST_TOK_GT, "'>'");
    if (out) {
      out->as.pointer.element = elem;
      out->as.pointer.qualifiers = q;
      if (gt)
        out->span = ast_span_join(out->span, ast_span_token(p, gt));
    }
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_KW_BITS) {
    const AstToken *start = ast_p_advance(p);
    out = ast_new_type(p, AST_TYPE_BITS, ast_span_token(p, start));
    ast_p_expect(p, AST_TOK_LT, "'<'");
    AstExpr *width = ast_parse_generic_const_expr(p);
    const AstToken *gt = ast_p_expect(p, AST_TOK_GT, "'>'");
    if (out) {
      out->as.bits.width = width;
      if (gt)
        out->span = ast_span_join(out->span, ast_span_token(p, gt));
    }
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_KW_VEC) {
    const AstToken *start = ast_p_advance(p);
    out = ast_new_type(p, AST_TYPE_VEC, ast_span_token(p, start));
    ast_p_expect(p, AST_TOK_LT, "'<'");
    AstExpr *lanes = ast_parse_generic_const_expr(p);
    ast_p_expect(p, AST_TOK_COMMA, "','");
    AstType *elem = ast_parse_type_inner(p);
    const AstToken *gt = ast_p_expect(p, AST_TOK_GT, "'>'");
    if (out) {
      out->as.vector.lanes = lanes;
      out->as.vector.element = elem;
      if (gt)
        out->span = ast_span_join(out->span, ast_span_token(p, gt));
    }
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_KW_MASK) {
    const AstToken *start = ast_p_advance(p);
    out = ast_new_type(p, AST_TYPE_MASK, ast_span_token(p, start));
    ast_p_expect(p, AST_TOK_LT, "'<'");
    AstExpr *lanes = ast_parse_generic_const_expr(p);
    const AstToken *gt = ast_p_expect(p, AST_TOK_GT, "'>'");
    if (out) {
      out->as.mask.lanes = lanes;
      if (gt)
        out->span = ast_span_join(out->span, ast_span_token(p, gt));
    }
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_KW_SVEC || k == AST_TOK_KW_PRED) {
    const AstToken *start = ast_p_advance(p);
    out = ast_new_type(p, k == AST_TOK_KW_SVEC ? AST_TYPE_SVEC : AST_TYPE_PRED,
                       ast_span_token(p, start));
    ast_p_expect(p, AST_TOK_LT, "'<'");
    AstType *elem = ast_parse_type_inner(p);
    const AstToken *gt = ast_p_expect(p, AST_TOK_GT, "'>'");
    if (out) {
      out->as.scalable.element = elem;
      if (gt)
        out->span = ast_span_join(out->span, ast_span_token(p, gt));
    }
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_LBRACKET) {
    const AstToken *start = ast_p_advance(p);
    out = ast_new_type(p, AST_TYPE_ARRAY, ast_span_token(p, start));
    AstType *elem = ast_parse_type_inner(p);
    ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    AstExpr *count = ast_parse_expression(p);
    const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACKET, "']'");
    if (out) {
      out->as.array.element = elem;
      out->as.array.count = count;
      if (rb)
        out->span = ast_span_join(out->span, ast_span_token(p, rb));
    }
    ast_p_leave(p);
    return out;
  }

  if (k == AST_TOK_LPAREN) {
    const AstToken *start = ast_p_advance(p);
    out = ast_new_type(p, AST_TYPE_TUPLE, ast_span_token(p, start));
    AstBuildVec elems;
    ast_vec_init(&elems, sizeof(AstType *), ASTER_ALIGNOF(AstType *));
    if (ast_p_kind(p) != AST_TOK_RPAREN) {
      for (;;) {
        AstType *elem = ast_parse_type_inner(p);
        ast_vec_push(p, &elems, &elem);
        if (!ast_p_match(p, AST_TOK_COMMA))
          break;
        if (ast_p_kind(p) == AST_TOK_RPAREN)
          break;
      }
    }
    const AstToken *rp = ast_p_expect(p, AST_TOK_RPAREN, "')'");
    if (out) {
      out->as.tuple.elements.items = (AstType **)ast_vec_finish(p, &elems);
      out->as.tuple.elements.count = elems.count;
      if (rp)
        out->span = ast_span_join(out->span, ast_span_token(p, rp));
    }
    ast_p_leave(p);
    return out;
  }

  ast_p_diag(p, AST_PARSE_DIAG_EXPECTED_TYPE, AST_DIAG_ERROR,
             ast_span_current(p), "expected type, found %s",
             ast_token_kind_name(k));
  const AstToken *bad = ast_p_advance(p);
  out = ast_new_type(p, AST_TYPE_ERROR, ast_span_token(p, bad));
  ast_p_leave(p);
  return out;
}

AstType *ast_parse_type(AstParser *p) { return ast_parse_type_inner(p); }

AstGenericParamArray ast_parse_generic_params(AstParser *p) {
  AstGenericParamArray out = {0};
  if (!ast_p_match(p, AST_TOK_LT))
    return out;
  AstBuildVec vec;
  ast_vec_init(&vec, sizeof(AstGenericParam), ASTER_ALIGNOF(AstGenericParam));
  if (ast_p_kind(p) != AST_TOK_GT) {
    for (;;) {
      AstGenericParam g;
      memset(&g, 0, sizeof(g));
      g.span = ast_span_current(p);
      ast_parse_name(p, &g.name, 0);
      ast_p_expect(p, AST_TOK_COLON, "':'");
      if (ast_p_match(p, AST_TOK_KW_TYPE)) {
        g.kind = AST_GENERIC_TYPE;
      } else if (ast_p_match(p, AST_TOK_KW_CONST)) {
        g.kind = AST_GENERIC_CONST;
        g.const_type = ast_parse_type(p);
      } else {
        ast_p_diag_current(p, AST_PARSE_DIAG_EXPECTED_TOKEN,
                           "expected 'type' or 'const' in generic parameter");
      }
      if (p->previous)
        g.span = ast_span_join(g.span, ast_span_token(p, p->previous));
      ast_vec_push(p, &vec, &g);
      if (!ast_p_match(p, AST_TOK_COMMA))
        break;
    }
  }
  ast_p_expect(p, AST_TOK_GT, "'>'");
  out.items = (AstGenericParam *)ast_vec_finish(p, &vec);
  out.count = vec.count;
  return out;
}

AstParamArray ast_parse_params(AstParser *p) {
  AstParamArray out = {0};
  AstBuildVec vec;
  ast_vec_init(&vec, sizeof(AstParam), ASTER_ALIGNOF(AstParam));
  ast_p_expect(p, AST_TOK_LPAREN, "'('");
  if (ast_p_kind(p) != AST_TOK_RPAREN) {
    for (;;) {
      AstParam param;
      memset(&param, 0, sizeof(param));
      param.span = ast_span_current(p);
      ast_parse_name(p, &param.name, 0);
      ast_p_expect(p, AST_TOK_COLON, "':'");
      param.type = ast_parse_type(p);
      param.attributes = ast_parse_attributes(p);
      if (p->previous)
        param.span = ast_span_join(param.span, ast_span_token(p, p->previous));
      ast_vec_push(p, &vec, &param);
      if (!ast_p_match(p, AST_TOK_COMMA))
        break;
      if (ast_p_kind(p) == AST_TOK_RPAREN)
        break;
    }
  }
  ast_p_expect(p, AST_TOK_RPAREN, "')'");
  out.items = (AstParam *)ast_vec_finish(p, &vec);
  out.count = vec.count;
  return out;
}
