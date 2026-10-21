#include "parser_internal.h"

static AstStmt *ast_parse_if_stmt(AstParser *p);
static AstStmt *ast_parse_for_stmt(AstParser *p);

static int ast_is_store_memory_attribute(AstName name) {
  return ast_name_equal_cstr(name, "align") ||
         ast_name_equal_cstr(name, "volatile") ||
         ast_name_equal_cstr(name, "nontemporal");
}

/* Expression postfix attributes normally bind to the expression immediately to
   their left.  For store syntax, the language grammar gives trailing memory
   attributes to the store itself.  The expression parser has necessarily seen
   those tokens first, so lift only the known memory attributes from the
   rightmost expression spine.  Unrelated expression attributes stay attached
   to the value. */
static AstExpr *ast_peel_store_memory_attributes(AstParser *p, AstExpr *expr,
                                                 AstBuildVec *attrs) {
  AstExpr **tail = &expr;
  while (*tail) {
    AstExpr *node = *tail;
    if (node->kind == AST_EXPR_ATTR && node->as.attributed.attribute &&
        ast_is_store_memory_attribute(node->as.attributed.attribute->name)) {
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

AstBlock *ast_parse_block(AstParser *p) {
  const AstToken *lb = ast_p_expect(p, AST_TOK_LBRACE, "'{'");
  AstSpan span = ast_span_token(p, lb);
  AstBuildVec stmts;
  ast_vec_init(&stmts, sizeof(AstStmt *), ASTER_ALIGNOF(AstStmt *));
  while (ast_p_kind(p) != AST_TOK_RBRACE && ast_p_kind(p) != AST_TOK_EOF) {
    size_t before = p->token_index;
    AstStmt *stmt = ast_parse_statement(p);
    if (stmt)
      ast_vec_push(p, &stmts, &stmt);
    if (p->panic_mode)
      ast_p_sync_statement(p);
    if (p->token_index == before && ast_p_kind(p) != AST_TOK_EOF)
      ast_p_advance(p);
  }
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACE, "'}'");
  if (rb)
    span = ast_span_join(span, ast_span_token(p, rb));
  AstBlock *block = ast_new_block(p, span);
  if (block) {
    block->statements.items = (AstStmt **)ast_vec_finish(p, &stmts);
    block->statements.count = stmts.count;
  }
  return block;
}

static AstStmt *ast_parse_let_like(AstParser *p, int is_const) {
  const AstToken *start = ast_p_advance(p);
  AstStmt *s = ast_new_stmt(p, is_const ? AST_STMT_CONST : AST_STMT_LET,
                            ast_span_token(p, start));
  AstName name;
  memset(&name, 0, sizeof(name));
  ast_parse_name(p, &name, 0);
  AstType *type = NULL;
  AstExpr *value = NULL;
  if (ast_p_match(p, AST_TOK_COLON))
    type = ast_parse_type(p);
  if (ast_p_match(p, AST_TOK_ASSIGN))
    value = ast_parse_expression(p);
  else if (is_const)
    ast_p_diag_current(p, AST_PARSE_DIAG_EXPECTED_TOKEN,
                       "const declaration requires an initializer");
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (s) {
    s->span =
        ast_span_join(s->span, semi ? ast_span_token(p, semi) : name.span);
    if (is_const) {
      s->as.const_stmt.name = name;
      s->as.const_stmt.type = type;
      s->as.const_stmt.value = value;
    } else {
      s->as.let_stmt.name = name;
      s->as.let_stmt.type = type;
      s->as.let_stmt.value = value;
    }
  }
  return s;
}

static AstStmt *ast_parse_store(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstStmt *s = ast_new_stmt(p, AST_STMT_STORE, ast_span_token(p, start));
  ast_p_expect(p, AST_TOK_DOT, "'.'");
  AstType *type = ast_parse_type(p);
  ast_p_expect(p, AST_TOK_LBRACKET, "'['");
  AstExpr *address = ast_parse_expression(p);
  ast_p_expect(p, AST_TOK_RBRACKET, "']'");
  ast_p_expect(p, AST_TOK_ASSIGN, "'='");
  AstExpr *value = ast_parse_expression(p);
  AstBuildVec memory_attrs;
  ast_vec_init(&memory_attrs, sizeof(AstAttribute),
               ASTER_ALIGNOF(AstAttribute));
  value = ast_peel_store_memory_attributes(p, value, &memory_attrs);
  AstAttributeArray direct_attrs = ast_parse_attributes(p);
  for (uint32_t i = 0u; i < direct_attrs.count; ++i)
    ast_vec_push(p, &memory_attrs, &direct_attrs.items[i]);
  AstAttributeArray attrs;
  attrs.items = (AstAttribute *)ast_vec_finish(p, &memory_attrs);
  attrs.count = memory_attrs.count;
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (s) {
    s->as.store_stmt.type = type;
    s->as.store_stmt.address = address;
    s->as.store_stmt.value = value;
    s->as.store_stmt.attributes = attrs;
    if (semi)
      s->span = ast_span_join(s->span, ast_span_token(p, semi));
  }
  return s;
}

static AstStmt *ast_parse_if_stmt(AstParser *p) {
  const AstToken *start = ast_p_expect(p, AST_TOK_KW_IF, "'if'");
  AstStmt *s = ast_new_stmt(p, AST_STMT_IF, ast_span_token(p, start));
  AstExpr *condition = ast_parse_expression(p);
  AstBlock *then_block = ast_parse_block(p);
  AstStmt *else_branch = NULL;
  if (ast_p_match(p, AST_TOK_KW_ELSE)) {
    if (ast_p_kind(p) == AST_TOK_KW_IF) {
      else_branch = ast_parse_if_stmt(p);
    } else {
      AstBlock *else_block = ast_parse_block(p);
      else_branch =
          ast_new_stmt(p, AST_STMT_BLOCK,
                       else_block ? else_block->span : ast_span_current(p));
      if (else_branch)
        else_branch->as.block_stmt.block = else_block;
    }
  }
  if (s) {
    s->as.if_stmt.condition = condition;
    s->as.if_stmt.then_block = then_block;
    s->as.if_stmt.else_branch = else_branch;
    if (else_branch)
      s->span = ast_span_join(s->span, else_branch->span);
    else if (then_block)
      s->span = ast_span_join(s->span, then_block->span);
  }
  return s;
}

static AstStmt *ast_parse_while(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstStmt *s = ast_new_stmt(p, AST_STMT_WHILE, ast_span_token(p, start));
  AstExpr *condition = ast_parse_expression(p);
  AstBlock *body = ast_parse_block(p);
  if (s) {
    s->as.while_stmt.condition = condition;
    s->as.while_stmt.body = body;
    if (body)
      s->span = ast_span_join(s->span, body->span);
  }
  return s;
}

static AstStmt *ast_parse_loop(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstBlock *body = ast_parse_block(p);
  AstStmt *s =
      ast_new_stmt(p, AST_STMT_LOOP,
                   body ? ast_span_join(ast_span_token(p, start), body->span)
                        : ast_span_token(p, start));
  if (s)
    s->as.loop_stmt.body = body;
  return s;
}

static AstStmt *ast_parse_return(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstStmt *s = ast_new_stmt(p, AST_STMT_RETURN, ast_span_token(p, start));
  AstExpr *value = NULL;
  if (ast_p_kind(p) != AST_TOK_SEMICOLON)
    value = ast_parse_expression(p);
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (s) {
    s->as.return_stmt.value = value;
    if (semi)
      s->span = ast_span_join(s->span, ast_span_token(p, semi));
  }
  return s;
}

static AstStmt *ast_parse_break_continue(AstParser *p, int is_break) {
  const AstToken *start = ast_p_advance(p);
  AstStmt *s = ast_new_stmt(p, is_break ? AST_STMT_BREAK : AST_STMT_CONTINUE,
                            ast_span_token(p, start));
  AstName label;
  memset(&label, 0, sizeof(label));
  uint8_t has = 0u;
  if (ast_p_kind(p) == AST_TOK_IDENTIFIER) {
    ast_parse_name(p, &label, 0);
    has = 1u;
  }
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (s) {
    s->as.jump_stmt.label = label;
    s->as.jump_stmt.has_label = has;
    if (semi)
      s->span = ast_span_join(s->span, ast_span_token(p, semi));
  }
  return s;
}

static AstStmt *ast_parse_block_label(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstName label;
  memset(&label, 0, sizeof(label));
  ast_parse_name(p, &label, 0);
  const AstToken *colon = ast_p_expect(p, AST_TOK_COLON, "':'");
  AstStmt *s = ast_new_stmt(
      p, AST_STMT_BLOCK_LABEL,
      colon ? ast_span_join(ast_span_token(p, start), ast_span_token(p, colon))
            : ast_span_token(p, start));
  if (s)
    s->as.block_label.label = label;
  return s;
}

static AstStmt *ast_parse_branch(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstName label;
  memset(&label, 0, sizeof(label));
  ast_parse_name(p, &label, 0);
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  AstStmt *s = ast_new_stmt(
      p, AST_STMT_BRANCH,
      semi ? ast_span_join(ast_span_token(p, start), ast_span_token(p, semi))
           : ast_span_token(p, start));
  if (s)
    s->as.branch_stmt.label = label;
  return s;
}

static AstStmt *ast_parse_for_stmt(AstParser *p) {
  const AstToken *start = ast_p_expect(p, AST_TOK_KW_FOR, "'for'");
  AstStmt *s = ast_new_stmt(p, AST_STMT_FOR, ast_span_token(p, start));
  AstName variable;
  memset(&variable, 0, sizeof(variable));
  ast_parse_name(p, &variable, 0);
  ast_p_expect(p, AST_TOK_KW_IN, "'in'");
  AstExpr *begin = ast_parse_expression(p);
  ast_p_expect(p, AST_TOK_DOT_DOT, "'..'");
  AstExpr *end = ast_parse_expression(p);
  AstBlock *body = ast_parse_block(p);
  if (s) {
    s->as.for_stmt.variable = variable;
    s->as.for_stmt.begin = begin;
    s->as.for_stmt.end = end;
    s->as.for_stmt.body = body;
    if (body)
      s->span = ast_span_join(s->span, body->span);
  }
  return s;
}

static AstStmt *ast_parse_comptime(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstStmt *s = ast_new_stmt(p, AST_STMT_COMPTIME, ast_span_token(p, start));
  AstStmt *inner = NULL;
  AstBlock *body = NULL;
  if (ast_p_kind(p) == AST_TOK_KW_IF)
    inner = ast_parse_if_stmt(p);
  else if (ast_p_kind(p) == AST_TOK_KW_FOR)
    inner = ast_parse_for_stmt(p);
  else if (ast_p_kind(p) == AST_TOK_LBRACE)
    body = ast_parse_block(p);
  else {
    ast_p_diag_current(p, AST_PARSE_DIAG_EXPECTED_STATEMENT,
                       "expected if, for, or block after comptime");
    ast_p_sync_statement(p);
  }
  if (s) {
    s->as.comptime_stmt.inner = inner;
    s->as.comptime_stmt.body = body;
    if (inner)
      s->span = ast_span_join(s->span, inner->span);
    else if (body)
      s->span = ast_span_join(s->span, body->span);
  }
  return s;
}

static AstStmt *ast_parse_requires(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstExpr *condition = ast_parse_expression(p);
  AstBlock *body = ast_parse_block(p);
  AstStmt *s =
      ast_new_stmt(p, AST_STMT_REQUIRES,
                   body ? ast_span_join(ast_span_token(p, start), body->span)
                        : ast_span_token(p, start));
  if (s) {
    s->as.requires_stmt.condition = condition;
    s->as.requires_stmt.body = body;
  }
  return s;
}

static AstStmt *ast_parse_bind(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstName value, target;
  memset(&value, 0, sizeof(value));
  memset(&target, 0, sizeof(target));
  ast_parse_name(p, &value, 0);
  ast_p_expect(p, AST_TOK_KW_TO, "'to'");
  uint8_t role = 0u;
  if (ast_p_kind(p) == AST_TOK_KW_REG &&
      ast_p_peek_kind(p, 1u) == AST_TOK_LPAREN) {
    ast_p_advance(p);
    ast_p_advance(p);
    const AstToken *str =
        ast_p_expect(p, AST_TOK_STRING, "register role string");
    if (str)
      target = ast_name_from_token(p, str);
    ast_p_expect(p, AST_TOK_RPAREN, "')'");
    role = 1u;
  } else {
    ast_parse_qualified_name(p, &target);
  }
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  AstStmt *s = ast_new_stmt(
      p, AST_STMT_BIND,
      semi ? ast_span_join(ast_span_token(p, start), ast_span_token(p, semi))
           : ast_span_token(p, start));
  if (s) {
    s->as.bind_stmt.value = value;
    s->as.bind_stmt.target = target;
    s->as.bind_stmt.target_is_reg_role = role;
  }
  return s;
}

static AstName *ast_parse_bridge_names(AstParser *p, uint32_t *count_out) {
  AstBuildVec names;
  ast_vec_init(&names, sizeof(AstName), ASTER_ALIGNOF(AstName));
  ast_p_expect(p, AST_TOK_LPAREN, "'('");
  if (ast_p_kind(p) != AST_TOK_RPAREN) {
    for (;;) {
      AstName n;
      memset(&n, 0, sizeof(n));
      ast_parse_name(p, &n, 0);
      ast_vec_push(p, &names, &n);
      if (!ast_p_match(p, AST_TOK_COMMA))
        break;
    }
  }
  ast_p_expect(p, AST_TOK_RPAREN, "')'");
  *count_out = names.count;
  return (AstName *)ast_vec_finish(p, &names);
}

static AstNativeOperand ast_parse_native_operand(AstParser *p) {
  AstNativeOperand op;
  memset(&op, 0, sizeof(op));
  op.span = ast_span_current(p);
  ast_parse_name(p, &op.name, 1);
  ast_p_expect(p, AST_TOK_COLON, "':'");
  if (ast_p_kind(p) == AST_TOK_IDENTIFIER &&
      ast_p_token_text_eq(p, p->current, "mem") &&
      ast_p_peek_kind(p, 1u) == AST_TOK_LT) {
    const AstToken *start = ast_p_advance(p);
    ast_p_advance(p);
    op.memory_type = ast_parse_type(p);
    ast_p_expect(p, AST_TOK_GT, "'>'");
    ast_p_expect(p, AST_TOK_LBRACKET, "'['");
    op.memory_address = ast_parse_expression(p);
    const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACKET, "']'");
    op.is_memory = 1u;
    if (rb)
      op.span = ast_span_join(ast_span_token(p, start), ast_span_token(p, rb));
  } else {
    op.value = ast_parse_expression(p);
    if (op.value)
      op.span = ast_span_join(op.name.span, op.value->span);
  }
  return op;
}

static AstNativeStmt ast_parse_native_instruction(AstParser *p,
                                                  int allow_bytes) {
  AstNativeStmt n;
  memset(&n, 0, sizeof(n));
  n.span = ast_span_current(p);
  if (allow_bytes && ast_p_kind(p) == AST_TOK_KW_BYTES) {
    const AstToken *start = ast_p_advance(p);
    n.kind = AST_NATIVE_BYTES;
    ast_p_expect(p, AST_TOK_LBRACE, "'{'");
    AstBuildVec bytes;
    ast_vec_init(&bytes, sizeof(AstExpr *), ASTER_ALIGNOF(AstExpr *));
    if (ast_p_kind(p) != AST_TOK_RBRACE) {
      for (;;) {
        AstExpr *b = ast_parse_expression(p);
        ast_vec_push(p, &bytes, &b);
        if (!ast_p_match(p, AST_TOK_COMMA))
          break;
      }
    }
    ast_p_expect(p, AST_TOK_RBRACE, "'}'");
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    n.bytes.items = (AstExpr **)ast_vec_finish(p, &bytes);
    n.bytes.count = bytes.count;
    n.span =
        semi ? ast_span_join(ast_span_token(p, start), ast_span_token(p, semi))
             : ast_span_token(p, start);
    return n;
  }
  n.kind = AST_NATIVE_INSTRUCTION;
  /* ISA suffixes are part of the mnemonic: fence.i, sfence.vma,
     vadd.vv, lr.d, b.eq. They share the existing qualified-name spelling. */
  ast_parse_qualified_name(p, &n.mnemonic);
  n.span = n.mnemonic.span;
  if (ast_p_match(p, AST_TOK_LBRACE)) {
    AstBuildVec ops;
    ast_vec_init(&ops, sizeof(AstNativeOperand),
                 ASTER_ALIGNOF(AstNativeOperand));
    if (ast_p_kind(p) != AST_TOK_RBRACE) {
      for (;;) {
        AstNativeOperand op = ast_parse_native_operand(p);
        ast_vec_push(p, &ops, &op);
        if (!ast_p_match(p, AST_TOK_COMMA))
          break;
      }
    }
    ast_p_expect(p, AST_TOK_RBRACE, "'}'");
    n.operands.items = (AstNativeOperand *)ast_vec_finish(p, &ops);
    n.operands.count = ops.count;
  }
  n.attributes = ast_parse_attributes(p);
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (semi)
    n.span = ast_span_join(n.span, ast_span_token(p, semi));
  return n;
}

static AstStmt *ast_parse_native_block(AstParser *p, int exact,
                                       int unsafe_exact) {
  const AstToken *start = ast_p_advance(p);
  AstStmt *s = ast_new_stmt(p, exact ? AST_STMT_EXACT : AST_STMT_NATIVE,
                            ast_span_token(p, start));
  AstName arch;
  memset(&arch, 0, sizeof(arch));
  ast_parse_name(p, &arch, 0);
  AstNativeBridge bridge;
  memset(&bridge, 0, sizeof(bridge));
  while (ast_p_kind(p) == AST_TOK_KW_INPUTS ||
         ast_p_kind(p) == AST_TOK_KW_OUTPUTS) {
    AstTokenKind k = ast_p_kind(p);
    ast_p_advance(p);
    if (k == AST_TOK_KW_INPUTS)
      bridge.inputs = ast_parse_bridge_names(p, &bridge.input_count);
    else
      bridge.outputs = ast_parse_bridge_names(p, &bridge.output_count);
  }
  ast_p_expect(p, AST_TOK_LBRACE, "'{'");
  AstBuildVec body;
  ast_vec_init(&body, sizeof(AstNativeStmt), ASTER_ALIGNOF(AstNativeStmt));
  while (ast_p_kind(p) != AST_TOK_RBRACE && ast_p_kind(p) != AST_TOK_EOF) {
    size_t before = p->token_index;
    AstNativeStmt n = ast_parse_native_instruction(p, exact);
    ast_vec_push(p, &body, &n);
    if (p->panic_mode)
      ast_p_sync_statement(p);
    if (p->token_index == before)
      ast_p_advance(p);
  }
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACE, "'}'");
  if (s) {
    s->as.native_stmt.arch = arch;
    s->as.native_stmt.bridge = bridge;
    s->as.native_stmt.statements.items =
        (AstNativeStmt *)ast_vec_finish(p, &body);
    s->as.native_stmt.statements.count = body.count;
    s->as.native_stmt.unsafe_exact = (uint8_t)unsafe_exact;
    if (rb)
      s->span = ast_span_join(s->span, ast_span_token(p, rb));
  }
  return s;
}

static AstStmt *ast_parse_static_assert_stmt(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  ast_p_expect(p, AST_TOK_LPAREN, "'('");
  AstExpr *condition = ast_parse_expression(p);
  AstExpr *message = NULL;
  if (ast_p_match(p, AST_TOK_COMMA))
    message = ast_parse_expression(p);
  ast_p_expect(p, AST_TOK_RPAREN, "')'");
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  AstStmt *s = ast_new_stmt(
      p, AST_STMT_STATIC_ASSERT,
      semi ? ast_span_join(ast_span_token(p, start), ast_span_token(p, semi))
           : ast_span_token(p, start));
  if (s) {
    s->as.static_assert_stmt.condition = condition;
    s->as.static_assert_stmt.message = message;
  }
  return s;
}

static AstStmt *ast_parse_unsafe(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  if (ast_p_kind(p) == AST_TOK_KW_EXACT) {
    AstStmt *s = ast_parse_native_block(p, 1, 1);
    if (s)
      s->span = ast_span_join(ast_span_token(p, start), s->span);
    return s;
  }
  uint8_t control = (uint8_t)ast_p_match(p, AST_TOK_KW_CONTROL);
  AstBlock *body = ast_parse_block(p);
  AstStmt *s =
      ast_new_stmt(p, AST_STMT_UNSAFE,
                   body ? ast_span_join(ast_span_token(p, start), body->span)
                        : ast_span_token(p, start));
  if (s) {
    s->as.unsafe_stmt.body = body;
    s->as.unsafe_stmt.control = control;
  }
  return s;
}

AstStmt *ast_parse_statement(AstParser *p) {
  switch (ast_p_kind(p)) {
  case AST_TOK_LBRACE: {
    AstBlock *b = ast_parse_block(p);
    AstStmt *s =
        ast_new_stmt(p, AST_STMT_BLOCK, b ? b->span : ast_span_current(p));
    if (s)
      s->as.block_stmt.block = b;
    return s;
  }
  case AST_TOK_KW_LET:
    return ast_parse_let_like(p, 0);
  case AST_TOK_KW_CONST:
    return ast_parse_let_like(p, 1);
  case AST_TOK_KW_STORE:
    return ast_parse_store(p);
  case AST_TOK_KW_IF:
    return ast_parse_if_stmt(p);
  case AST_TOK_KW_WHILE:
    return ast_parse_while(p);
  case AST_TOK_KW_LOOP:
    return ast_parse_loop(p);
  case AST_TOK_KW_RETURN:
    return ast_parse_return(p);
  case AST_TOK_KW_BREAK:
    return ast_parse_break_continue(p, 1);
  case AST_TOK_KW_CONTINUE:
    return ast_parse_break_continue(p, 0);
  case AST_TOK_KW_BLOCK:
    return ast_parse_block_label(p);
  case AST_TOK_KW_BR:
    return ast_parse_branch(p);
  case AST_TOK_KW_UNSAFE:
    return ast_parse_unsafe(p);
  case AST_TOK_KW_COMPTIME:
    return ast_parse_comptime(p);
  case AST_TOK_KW_FOR:
    return ast_parse_for_stmt(p);
  case AST_TOK_KW_REQUIRES:
    return ast_parse_requires(p);
  case AST_TOK_KW_BIND:
    return ast_parse_bind(p);
  case AST_TOK_KW_NATIVE:
    return ast_parse_native_block(p, 0, 0);
  case AST_TOK_KW_EXACT:
    return ast_parse_native_block(p, 1, 0);
  case AST_TOK_KW_STATIC_ASSERT:
    return ast_parse_static_assert_stmt(p);
  default:
    break;
  }

  if ((ast_p_kind(p) == AST_TOK_IDENTIFIER ||
       ast_p_is_contextual_name(ast_p_kind(p))) &&
      ast_p_peek_kind(p, 1u) == AST_TOK_ASSIGN) {
    const AstToken *name_tok = ast_p_advance(p);
    ast_p_advance(p);
    AstExpr *value = ast_parse_expression(p);
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    AstStmt *s = ast_new_stmt(p, AST_STMT_ASSIGN,
                              semi ? ast_span_join(ast_span_token(p, name_tok),
                                                   ast_span_token(p, semi))
                                   : ast_span_token(p, name_tok));
    if (s) {
      s->as.assign_stmt.name = ast_name_from_token(p, name_tok);
      s->as.assign_stmt.value = value;
    }
    return s;
  }

  AstSpan start = ast_span_current(p);
  AstExpr *expr = ast_parse_expression(p);
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  AstStmt *s = ast_new_stmt(p, AST_STMT_EXPR,
                            semi ? ast_span_join(start, ast_span_token(p, semi))
                                 : start);
  if (s)
    s->as.expr_stmt.expr = expr;
  return s;
}
