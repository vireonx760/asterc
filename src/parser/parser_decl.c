#include "parser_internal.h"

static AstAttributeArray ast_merge_attrs(AstParser *p, AstAttributeArray a,
                                         AstAttributeArray b) {
  if (!a.count)
    return b;
  if (!b.count)
    return a;
  uint32_t count = a.count + b.count;
  AstAttribute *items = AST_ARENA_ARRAY(&p->result->arena, AstAttribute, count);
  if (!items) {
    p->out_of_memory = 1u;
    return a;
  }
  memcpy(items, a.items, (size_t)a.count * sizeof(*items));
  memcpy(items + a.count, b.items, (size_t)b.count * sizeof(*items));
  AstAttributeArray out = {items, count};
  return out;
}

AstInitializer *ast_parse_initializer(AstParser *p) {
  AstSpan start = ast_span_current(p);
  AstInitializer *init = AST_ARENA_NEW(&p->result->arena, AstInitializer);
  if (!init) {
    p->out_of_memory = 1u;
    return NULL;
  }
  p->ast_nodes++;
  init->span = start;
  if (!ast_p_match(p, AST_TOK_LBRACE)) {
    init->kind = AST_INIT_EXPR;
    init->as.expr = ast_parse_expression(p);
    if (init->as.expr)
      init->span = init->as.expr->span;
    return init;
  }
  init->kind = AST_INIT_LIST;
  AstBuildVec list;
  ast_vec_init(&list, sizeof(AstInitializer *),
               ASTER_ALIGNOF(AstInitializer *));
  if (ast_p_kind(p) != AST_TOK_RBRACE) {
    for (;;) {
      AstInitializer *child = ast_parse_initializer(p);
      ast_vec_push(p, &list, &child);
      if (!ast_p_match(p, AST_TOK_COMMA))
        break;
      if (ast_p_kind(p) == AST_TOK_RBRACE)
        break;
    }
  }
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACE, "'}'");
  init->as.list.items = (AstInitializer **)ast_vec_finish(p, &list);
  init->as.list.count = list.count;
  if (rb)
    init->span = ast_span_join(start, ast_span_token(p, rb));
  return init;
}

static AstDecl *ast_parse_target_decl(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstDecl *d = ast_new_decl(p, AST_DECL_TARGET, ast_span_token(p, start));
  const AstToken *triple =
      ast_p_expect(p, AST_TOK_STRING, "target triple string");
  if (d && triple)
    d->as.target_decl.triple = ast_span_token(p, triple);
  ast_p_expect(p, AST_TOK_LBRACE, "'{'");
  AstBuildVec items;
  ast_vec_init(&items, sizeof(AstTargetItem), ASTER_ALIGNOF(AstTargetItem));
  while (ast_p_kind(p) != AST_TOK_RBRACE && ast_p_kind(p) != AST_TOK_EOF) {
    size_t before = p->token_index;
    AstTargetItem item;
    memset(&item, 0, sizeof(item));
    item.span = ast_span_current(p);
    if (ast_p_match(p, AST_TOK_KW_FEATURE)) {
      item.is_feature = 1u;
      if (ast_p_match(p, AST_TOK_PLUS))
        item.feature_sign = 1;
      else if (ast_p_match(p, AST_TOK_MINUS))
        item.feature_sign = -1;
      else
        ast_p_diag_current(p, AST_PARSE_DIAG_EXPECTED_TOKEN,
                           "expected '+' or '-' after feature");
      const AstToken *str = ast_p_expect(p, AST_TOK_STRING, "feature string");
      if (str)
        item.feature_string = ast_span_token(p, str);
    } else {
      ast_parse_name(p, &item.name, 1);
      ast_p_expect(p, AST_TOK_ASSIGN, "'='");
      item.value = ast_parse_expression(p);
    }
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    if (semi)
      item.span = ast_span_join(item.span, ast_span_token(p, semi));
    ast_vec_push(p, &items, &item);
    if (p->panic_mode)
      ast_p_sync_statement(p);
    if (p->token_index == before && ast_p_kind(p) != AST_TOK_RBRACE &&
        ast_p_kind(p) != AST_TOK_EOF)
      ast_p_advance(p);
  }
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACE, "'}'");
  if (d) {
    d->as.target_decl.items.items = (AstTargetItem *)ast_vec_finish(p, &items);
    d->as.target_decl.items.count = items.count;
    if (rb)
      d->span = ast_span_join(d->span, ast_span_token(p, rb));
  }
  return d;
}

static AstDecl *ast_parse_import_decl(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstDecl *d = ast_new_decl(p, AST_DECL_IMPORT, ast_span_token(p, start));
  AstName module, alias;
  memset(&module, 0, sizeof(module));
  memset(&alias, 0, sizeof(alias));
  ast_parse_qualified_name(p, &module);
  uint8_t has_alias = 0u;
  if (ast_p_match(p, AST_TOK_KW_AS)) {
    ast_parse_name(p, &alias, 1);
    has_alias = 1u;
  }
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (d) {
    d->as.import_decl.module = module;
    d->as.import_decl.alias = alias;
    d->as.import_decl.has_alias = has_alias;
    if (semi)
      d->span = ast_span_join(d->span, ast_span_token(p, semi));
  }
  return d;
}

static AstFieldArray ast_parse_struct_fields(AstParser *p,
                                             AstAttributeArray *struct_attrs,
                                             AstSpan *span_io) {
  *struct_attrs = ast_parse_attributes(p);
  const AstToken *lb = ast_p_expect(p, AST_TOK_LBRACE, "'{'");
  if (lb)
    *span_io = ast_span_join(*span_io, ast_span_token(p, lb));
  AstBuildVec fields;
  ast_vec_init(&fields, sizeof(AstField), ASTER_ALIGNOF(AstField));
  while (ast_p_kind(p) != AST_TOK_RBRACE && ast_p_kind(p) != AST_TOK_EOF) {
    size_t before = p->token_index;
    AstField f;
    memset(&f, 0, sizeof(f));
    f.span = ast_span_current(p);
    ast_parse_name(p, &f.name, 0);
    ast_p_expect(p, AST_TOK_COLON, "':'");
    f.type = ast_parse_type(p);
    f.attributes = ast_parse_attributes(p);
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    if (semi)
      f.span = ast_span_join(f.span, ast_span_token(p, semi));
    ast_vec_push(p, &fields, &f);
    if (p->panic_mode)
      ast_p_sync_statement(p);
    if (p->token_index == before && ast_p_kind(p) != AST_TOK_RBRACE &&
        ast_p_kind(p) != AST_TOK_EOF)
      ast_p_advance(p);
  }
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACE, "'}'");
  if (rb)
    *span_io = ast_span_join(*span_io, ast_span_token(p, rb));
  AstFieldArray out = {(AstField *)ast_vec_finish(p, &fields), fields.count};
  return out;
}

static AstDecl *ast_parse_type_decl(AstParser *p, uint8_t is_export) {
  const AstToken *start = ast_p_expect(p, AST_TOK_KW_TYPE, "'type'");
  AstDecl *d = ast_new_decl(p, AST_DECL_TYPE, ast_span_token(p, start));
  AstName name;
  memset(&name, 0, sizeof(name));
  ast_parse_name(p, &name, 0);
  AstGenericParamArray generics = ast_parse_generic_params(p);
  ast_p_expect(p, AST_TOK_ASSIGN, "'='");
  if (d) {
    d->is_export = is_export;
    d->as.type_decl.name = name;
    d->as.type_decl.generics = generics;
  }
  if (ast_p_match(p, AST_TOK_KW_STRUCT)) {
    if (d)
      d->as.type_decl.is_struct = 1u;
    AstSpan span = d ? d->span : name.span;
    AstAttributeArray struct_attrs;
    AstFieldArray fields = ast_parse_struct_fields(p, &struct_attrs, &span);
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    if (d) {
      d->as.type_decl.struct_attributes = struct_attrs;
      d->as.type_decl.fields = fields;
      d->span = semi ? ast_span_join(span, ast_span_token(p, semi)) : span;
    }
  } else {
    AstType *type = ast_parse_type(p);
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    if (d) {
      d->as.type_decl.type = type;
      if (semi)
        d->span = ast_span_join(d->span, ast_span_token(p, semi));
    }
  }
  return d;
}

static AstDecl *ast_parse_const_decl(AstParser *p, uint8_t is_export) {
  const AstToken *start = ast_p_advance(p);
  AstDecl *d = ast_new_decl(p, AST_DECL_CONST, ast_span_token(p, start));
  AstName name;
  memset(&name, 0, sizeof(name));
  ast_parse_name(p, &name, 0);
  AstType *type = NULL;
  if (ast_p_match(p, AST_TOK_COLON))
    type = ast_parse_type(p);
  ast_p_expect(p, AST_TOK_ASSIGN, "'='");
  AstExpr *value = ast_parse_expression(p);
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (d) {
    d->is_export = is_export;
    d->as.const_decl.name = name;
    d->as.const_decl.type = type;
    d->as.const_decl.value = value;
    if (semi)
      d->span = ast_span_join(d->span, ast_span_token(p, semi));
  }
  return d;
}

static AstDecl *ast_parse_section_decl(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstDecl *d = ast_new_decl(p, AST_DECL_SECTION, ast_span_token(p, start));
  const AstToken *name = ast_p_expect(p, AST_TOK_STRING, "section name string");
  AstAttributeArray section_attrs = ast_parse_attributes(p);
  ast_p_expect(p, AST_TOK_LBRACE, "'{'");
  AstBuildVec data;
  ast_vec_init(&data, sizeof(AstDataDecl), ASTER_ALIGNOF(AstDataDecl));
  while (ast_p_kind(p) != AST_TOK_RBRACE && ast_p_kind(p) != AST_TOK_EOF) {
    size_t before = p->token_index;
    AstDataDecl item;
    memset(&item, 0, sizeof(item));
    item.span = ast_span_current(p);
    item.is_export = (uint8_t)ast_p_match(p, AST_TOK_KW_EXPORT);
    if (ast_p_match(p, AST_TOK_KW_GLOBAL))
      item.is_const = 0u;
    else if (ast_p_match(p, AST_TOK_KW_CONST))
      item.is_const = 1u;
    else {
      ast_p_diag_current(p, AST_PARSE_DIAG_EXPECTED_DECLARATION,
                         "expected global or const in section");
      ast_p_sync_statement(p);
      if (ast_p_kind(p) == AST_TOK_RBRACE)
        break;
      /* Statement recovery can stop on an expression-start token that
       * is not a data declaration. Enforce progress in this container,
       * just as the top-level and statement parsers already do. */
      if (p->token_index == before && ast_p_kind(p) != AST_TOK_EOF)
        ast_p_advance(p);
      continue;
    }
    ast_parse_name(p, &item.name, 0);
    ast_p_expect(p, AST_TOK_COLON, "':'");
    item.type = ast_parse_type(p);
    if (ast_p_match(p, AST_TOK_ASSIGN))
      item.initializer = ast_parse_initializer(p);
    item.attributes = ast_parse_attributes(p);
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    if (semi)
      item.span = ast_span_join(item.span, ast_span_token(p, semi));
    ast_vec_push(p, &data, &item);
    if (p->panic_mode)
      ast_p_sync_statement(p);
  }
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACE, "'}'");
  if (d) {
    if (name)
      d->as.section_decl.name = ast_span_token(p, name);
    d->as.section_decl.section_attributes = section_attrs;
    d->as.section_decl.data.items = (AstDataDecl *)ast_vec_finish(p, &data);
    d->as.section_decl.data.count = data.count;
    if (rb)
      d->span = ast_span_join(d->span, ast_span_token(p, rb));
  }
  return d;
}

static AstType *ast_parse_optional_return_type(AstParser *p) {
  if (!ast_p_match(p, AST_TOK_ARROW))
    return NULL;
  return ast_parse_type(p);
}

static AstDecl *ast_parse_extern_fn(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  ast_p_expect(p, AST_TOK_KW_FN, "'fn'");
  AstDecl *d = ast_new_decl(p, AST_DECL_EXTERN_FN, ast_span_token(p, start));
  AstName name;
  memset(&name, 0, sizeof(name));
  ast_parse_name(p, &name, 0);
  AstParamArray params = ast_parse_params(p);
  AstType *ret = ast_parse_optional_return_type(p);
  AstAttributeArray attrs = ast_parse_attributes(p);
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  if (d) {
    d->attributes = attrs;
    d->as.extern_fn.name = name;
    d->as.extern_fn.params = params;
    d->as.extern_fn.return_type = ret;
    if (semi)
      d->span = ast_span_join(d->span, ast_span_token(p, semi));
  }
  return d;
}

static AstDispatchCaseArray ast_parse_dispatch_body(AstParser *p,
                                                    AstSpan *span_io) {
  AstBuildVec cases;
  ast_vec_init(&cases, sizeof(AstDispatchCase), ASTER_ALIGNOF(AstDispatchCase));
  ast_p_expect(p, AST_TOK_LBRACE, "'{'");
  while (ast_p_kind(p) == AST_TOK_KW_WHEN) {
    AstDispatchCase c;
    memset(&c, 0, sizeof(c));
    const AstToken *start = ast_p_advance(p);
    c.span = ast_span_token(p, start);
    c.condition = ast_parse_expression(p);
    ast_p_expect(p, AST_TOK_FAT_ARROW, "'=>'");
    ast_parse_name(p, &c.target, 0);
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    if (semi)
      c.span = ast_span_join(c.span, ast_span_token(p, semi));
    ast_vec_push(p, &cases, &c);
  }
  if (ast_p_match(p, AST_TOK_KW_ELSE)) {
    AstDispatchCase c;
    memset(&c, 0, sizeof(c));
    c.is_else = 1u;
    c.span = p->previous ? ast_span_token(p, p->previous) : ast_span_current(p);
    ast_p_expect(p, AST_TOK_FAT_ARROW, "'=>'");
    ast_parse_name(p, &c.target, 0);
    const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
    if (semi)
      c.span = ast_span_join(c.span, ast_span_token(p, semi));
    ast_vec_push(p, &cases, &c);
  } else {
    ast_p_diag_current(p, AST_PARSE_DIAG_EXPECTED_TOKEN,
                       "dispatch body requires an else case");
  }
  const AstToken *rb = ast_p_expect(p, AST_TOK_RBRACE, "'}'");
  if (rb)
    *span_io = ast_span_join(*span_io, ast_span_token(p, rb));
  AstDispatchCaseArray out = {(AstDispatchCase *)ast_vec_finish(p, &cases),
                              cases.count};
  return out;
}

static AstDecl *ast_parse_fn(AstParser *p, uint8_t is_export,
                             uint8_t is_dispatch) {
  const AstToken *start = ast_p_expect(p, AST_TOK_KW_FN, "'fn'");
  AstDecl *d = ast_new_decl(p, AST_DECL_FN, ast_span_token(p, start));
  AstName name;
  memset(&name, 0, sizeof(name));
  ast_parse_name(p, &name, 0);
  AstGenericParamArray generics = ast_parse_generic_params(p);
  AstParamArray params = ast_parse_params(p);
  AstType *ret = ast_parse_optional_return_type(p);
  AstAttributeArray attrs = ast_parse_attributes(p);
  if (d) {
    d->is_export = is_export;
    d->attributes = attrs;
    d->as.fn_decl.name = name;
    d->as.fn_decl.generics = generics;
    d->as.fn_decl.params = params;
    d->as.fn_decl.return_type = ret;
    d->as.fn_decl.is_dispatch = is_dispatch;
  }
  if (is_dispatch) {
    AstSpan span = d ? d->span : name.span;
    AstDispatchCaseArray cases = ast_parse_dispatch_body(p, &span);
    if (d) {
      d->as.fn_decl.dispatch_cases = cases;
      d->span = span;
    }
  } else {
    AstBlock *body = ast_parse_block(p);
    if (d) {
      d->as.fn_decl.body = body;
      if (body)
        d->span = ast_span_join(d->span, body->span);
    }
  }
  return d;
}

static AstDecl *ast_parse_macro_decl(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  AstDecl *d = ast_new_decl(p, AST_DECL_MACRO, ast_span_token(p, start));
  AstName name;
  memset(&name, 0, sizeof(name));
  ast_parse_name(p, &name, 0);
  ast_p_expect(p, AST_TOK_LPAREN, "'('");
  AstBuildVec params;
  ast_vec_init(&params, sizeof(AstMacroParam), ASTER_ALIGNOF(AstMacroParam));
  if (ast_p_kind(p) != AST_TOK_RPAREN) {
    for (;;) {
      AstMacroParam m;
      memset(&m, 0, sizeof(m));
      m.span = ast_span_current(p);
      ast_p_expect(p, AST_TOK_DOLLAR, "'$'");
      ast_parse_name(p, &m.name, 1);
      ast_p_expect(p, AST_TOK_COLON, "':'");
      if (ast_p_match(p, AST_TOK_KW_CONST)) {
        m.is_const = 1u;
        m.const_type = ast_parse_type(p);
      } else {
        ast_parse_name(p, &m.category, 1);
      }
      if (p->previous)
        m.span = ast_span_join(m.span, ast_span_token(p, p->previous));
      ast_vec_push(p, &params, &m);
      if (!ast_p_match(p, AST_TOK_COMMA))
        break;
    }
  }
  ast_p_expect(p, AST_TOK_RPAREN, "')'");
  const AstToken *lb = ast_p_expect(p, AST_TOK_LBRACE, "'{'");
  size_t body_first = p->token_index;
  size_t body_count = 0u;
  unsigned depth = 1u;
  AstSpan body_span = ast_span_token(p, lb);
  while (ast_p_kind(p) != AST_TOK_EOF && depth) {
    AstTokenKind k = ast_p_kind(p);
    const AstToken *tok = ast_p_advance(p);
    if (k == AST_TOK_LBRACE)
      depth++;
    else if (k == AST_TOK_RBRACE) {
      depth--;
      if (!depth) {
        body_span = ast_span_join(body_span, ast_span_token(p, tok));
        break;
      }
    }
    if (depth)
      body_count++;
  }
  if (depth)
    ast_p_diag_current(p, AST_PARSE_DIAG_EXPECTED_TOKEN,
                       "unterminated macro body");
  if (d) {
    d->as.macro_decl.name = name;
    d->as.macro_decl.params.items = (AstMacroParam *)ast_vec_finish(p, &params);
    d->as.macro_decl.params.count = params.count;
    d->as.macro_decl.body_tokens.first_token = body_first;
    d->as.macro_decl.body_tokens.token_count = body_count;
    d->as.macro_decl.body_tokens.span = body_span;
    d->span = ast_span_join(d->span, body_span);
  }
  return d;
}

static AstDecl *ast_parse_static_assert_decl(AstParser *p) {
  const AstToken *start = ast_p_advance(p);
  ast_p_expect(p, AST_TOK_LPAREN, "'('");
  AstExpr *condition = ast_parse_expression(p);
  AstExpr *message = NULL;
  if (ast_p_match(p, AST_TOK_COMMA))
    message = ast_parse_expression(p);
  ast_p_expect(p, AST_TOK_RPAREN, "')'");
  const AstToken *semi = ast_p_expect(p, AST_TOK_SEMICOLON, "';'");
  AstDecl *d = ast_new_decl(
      p, AST_DECL_STATIC_ASSERT,
      semi ? ast_span_join(ast_span_token(p, start), ast_span_token(p, semi))
           : ast_span_token(p, start));
  if (d) {
    d->as.static_assert_decl.condition = condition;
    d->as.static_assert_decl.message = message;
  }
  return d;
}

AstDecl *ast_parse_top_decl(AstParser *p) {
  AstAttributeArray prefix = ast_parse_attributes(p);
  uint8_t is_export = (uint8_t)ast_p_match(p, AST_TOK_KW_EXPORT);
  uint8_t is_dispatch = (uint8_t)ast_p_match(p, AST_TOK_KW_DISPATCH);
  AstDecl *d = NULL;
  switch (ast_p_kind(p)) {
  case AST_TOK_KW_TARGET:
    if (is_export || is_dispatch)
      ast_p_diag_current(p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
                         "target cannot be export or dispatch");
    d = ast_parse_target_decl(p);
    break;
  case AST_TOK_KW_IMPORT:
    if (is_export || is_dispatch)
      ast_p_diag_current(p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
                         "import cannot be export or dispatch");
    d = ast_parse_import_decl(p);
    break;
  case AST_TOK_KW_TYPE:
    if (is_dispatch)
      ast_p_diag_current(p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
                         "type cannot be dispatch");
    d = ast_parse_type_decl(p, is_export);
    break;
  case AST_TOK_KW_CONST:
    if (is_dispatch)
      ast_p_diag_current(p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
                         "const cannot be dispatch");
    d = ast_parse_const_decl(p, is_export);
    break;
  case AST_TOK_KW_SECTION:
    if (is_export || is_dispatch)
      ast_p_diag_current(p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
                         "section cannot be export or dispatch");
    d = ast_parse_section_decl(p);
    break;
  case AST_TOK_KW_EXTERN:
    if (is_export || is_dispatch)
      ast_p_diag_current(
          p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
          "extern fn uses symbol attributes instead of export/dispatch prefix");
    d = ast_parse_extern_fn(p);
    break;
  case AST_TOK_KW_FN:
    d = ast_parse_fn(p, is_export, is_dispatch);
    break;
  case AST_TOK_KW_MACRO:
    if (is_export || is_dispatch)
      ast_p_diag_current(p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
                         "macro cannot be export or dispatch in Core v0.1");
    d = ast_parse_macro_decl(p);
    break;
  case AST_TOK_KW_STATIC_ASSERT:
    if (is_export || is_dispatch)
      ast_p_diag_current(p, AST_PARSE_DIAG_INVALID_TOP_LEVEL,
                         "static_assert cannot be export or dispatch");
    d = ast_parse_static_assert_decl(p);
    break;
  default:
    ast_p_diag(p, AST_PARSE_DIAG_EXPECTED_DECLARATION, AST_DIAG_ERROR,
               ast_span_current(p), "expected top-level declaration, found %s",
               ast_token_kind_name(ast_p_kind(p)));
    if (ast_p_kind(p) != AST_TOK_EOF)
      ast_p_advance(p);
    d = ast_new_decl(p, AST_DECL_ERROR, ast_span_current(p));
    break;
  }
  if (d)
    d->attributes = ast_merge_attrs(p, prefix, d->attributes);
  return d;
}
