#include "hir_internal.h"

static AstHirInitializer *new_init(AstHirProgram *program,
                                   AstHirInitializerKind kind,
                                   const AstSemaType *type, AstSpan span) {
  AstHirInitializer *out = AST_ARENA_NEW(&program->arena, AstHirInitializer);
  if (!out) {
    program->oom = 1u;
    return NULL;
  }
  memset(out, 0, sizeof(*out));
  out->kind = kind;
  out->type = type;
  out->span = span;
  return out;
}

static const AstSemaType *aggregate_child_type(const AstSemaType *type,
                                               uint32_t index) {
  if (!type)
    return NULL;
  switch (ast_sema_type_kind(type)) {
  case AST_SEMA_TYPE_ARRAY:
    return ast_sema_type_element(type);
  case AST_SEMA_TYPE_TUPLE:
    return ast_sema_type_tuple_item(type, index);
  case AST_SEMA_TYPE_STRUCT: {
    AstSemaFieldInfo field;
    memset(&field, 0, sizeof(field));
    return ast_sema_type_struct_field(type, index, &field) ? field.type : NULL;
  }
  default:
    return NULL;
  }
}

#define HIR_INIT_MAX_SEGMENTS 64u
static uint32_t flatten_symbol_expr(const AstExpr *expr, AstName *segments,
                                    uint32_t capacity) {
  AstName reverse[HIR_INIT_MAX_SEGMENTS];
  uint32_t reverse_count = 0u;
  const AstExpr *cur = expr;
  while (cur && cur->kind == AST_EXPR_MEMBER) {
    if (reverse_count >= HIR_INIT_MAX_SEGMENTS)
      return 0u;
    reverse[reverse_count++] = cur->as.member.member;
    cur = cur->as.member.base;
  }
  if (!cur || cur->kind != AST_EXPR_NAME || reverse_count + 1u > capacity)
    return 0u;
  segments[0] = cur->as.name.name;
  for (uint32_t i = 0u; i < reverse_count; ++i)
    segments[i + 1u] = reverse[reverse_count - 1u - i];
  return reverse_count + 1u;
}

static uint32_t module_segment_count(AstName module) {
  if (!module.text || module.span.length == 0u)
    return 0u;
  uint32_t n = 1u;
  for (uint32_t i = 0u; i < module.span.length; ++i)
    if (module.text[i] == '.')
      n++;
  return n;
}

static int module_matches(AstName module, const AstName *segments,
                          uint32_t count) {
  if (!module.text || !segments || count == 0u)
    return 0;
  uint32_t seg = 0u, begin = 0u;
  for (uint32_t i = 0u; i <= module.span.length; ++i) {
    if (i != module.span.length && module.text[i] != '.')
      continue;
    if (seg >= count)
      return 0;
    uint32_t len = i - begin;
    if (segments[seg].span.length != len ||
        memcmp(segments[seg].text, module.text + begin, len) != 0)
      return 0;
    seg++;
    begin = i + 1u;
  }
  return seg == count;
}

static int resolve_symbol(const AstModule *module, const AstExpr *expr,
                          AstName *module_out, AstName *symbol_out) {
  AstName segs[HIR_INIT_MAX_SEGMENTS];
  uint32_t count = flatten_symbol_expr(expr, segs, HIR_INIT_MAX_SEGMENTS);
  if (count == 0u)
    return 0;
  if (count == 1u) {
    *module_out = module->name;
    *symbol_out = segs[0];
    return 1;
  }
  for (uint32_t i = 0u; i < module->declarations.count; ++i) {
    const AstDecl *decl = module->declarations.items[i];
    if (!decl || decl->kind != AST_DECL_IMPORT)
      continue;
    if (decl->as.import_decl.has_alias && count == 2u &&
        hir_name_eq(decl->as.import_decl.alias, segs[0])) {
      *module_out = decl->as.import_decl.module;
      *symbol_out = segs[1];
      return 1;
    }
    uint32_t prefix = module_segment_count(decl->as.import_decl.module);
    if (prefix + 1u == count &&
        module_matches(decl->as.import_decl.module, segs, prefix)) {
      *module_out = decl->as.import_decl.module;
      *symbol_out = segs[count - 1u];
      return 1;
    }
  }
  return 0;
}

static int symbol_address_expr(const AstExpr *expr,
                               const AstExpr **symbol_expr) {
  if (!expr)
    return 0;
  if (expr->kind == AST_EXPR_UNARY && expr->as.unary.op == AST_TOK_AMP) {
    *symbol_expr = expr->as.unary.operand;
    return 1;
  }
  if (expr->kind == AST_EXPR_CALL && expr->as.call.callee &&
      expr->as.call.callee->kind == AST_EXPR_NAME &&
      hir_name_eq_text(expr->as.call.callee->as.name.name, "symbol_addr") &&
      expr->as.call.args.count == 1u) {
    *symbol_expr = expr->as.call.args.items[0];
    return 1;
  }
  return 0;
}

static int fact_integer_i64(const AstSema *sema, const AstExpr *expr,
                            int64_t *out) {
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  if (!ast_sema_expr_fact(sema, expr, &fact) ||
      fact.constant.kind != AST_SEMA_CONST_INTEGER || fact.constant.hi != 0u)
    return 0;
  if (fact.constant.is_negative) {
    if (fact.constant.lo > (uint64_t)INT64_MAX + 1u)
      return 0;
    *out = fact.constant.lo == (uint64_t)INT64_MAX + 1u
               ? INT64_MIN
               : -(int64_t)fact.constant.lo;
  } else {
    if (fact.constant.lo > (uint64_t)INT64_MAX)
      return 0;
    *out = (int64_t)fact.constant.lo;
  }
  return 1;
}

static int resolve_relocation(const AstHirProgram *program,
                              const AstModule *module, const AstExpr *expr,
                              AstName *module_out, AstName *symbol_out,
                              int64_t *addend_out) {
  const AstExpr *sym = NULL;
  if (symbol_address_expr(expr, &sym)) {
    if (!resolve_symbol(module, sym, module_out, symbol_out))
      return 0;
    *addend_out = 0;
    return 1;
  }
  if (expr && expr->kind == AST_EXPR_BINARY &&
      (expr->as.binary.op == AST_TOK_PLUS ||
       expr->as.binary.op == AST_TOK_MINUS)) {
    const AstExpr *left_sym = NULL;
    if (symbol_address_expr(expr->as.binary.left, &left_sym)) {
      int64_t addend = 0;
      if (!fact_integer_i64(program->sema, expr->as.binary.right, &addend))
        return 0;
      if (expr->as.binary.op == AST_TOK_MINUS) {
        if (addend == INT64_MIN)
          return 0;
        addend = -addend;
      }
      if (!resolve_symbol(module, left_sym, module_out, symbol_out))
        return 0;
      *addend_out = addend;
      return 1;
    }
  }
  return 0;
}

AstHirInitializer *hir_build_initializer(AstHirProgram *program,
                                         const AstModule *module,
                                         const AstInitializer *source,
                                         const AstSemaType *type) {
  if (!program || !module)
    return NULL;
  if (!source)
    return new_init(program, AST_HIR_INIT_ZERO, type, (AstSpan){0});
  if (source->kind == AST_INIT_LIST) {
    AstHirInitializer *out =
        new_init(program, AST_HIR_INIT_AGGREGATE, type, source->span);
    if (!out)
      return NULL;
    out->child_count = source->as.list.count;
    if (out->child_count) {
      out->children = AST_ARENA_ARRAY(&program->arena, AstHirInitializer *,
                                      out->child_count);
      if (!out->children) {
        program->oom = 1u;
        return out;
      }
      for (uint32_t i = 0u; i < out->child_count; ++i) {
        out->children[i] =
            hir_build_initializer(program, module, source->as.list.items[i],
                                  aggregate_child_type(type, i));
      }
    }
    return out;
  }

  const AstExpr *expr = source->as.expr;
  if (!expr)
    return new_init(program, AST_HIR_INIT_ZERO, type, source->span);
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  (void)ast_sema_expr_fact(program->sema, expr, &fact);
  if (fact.constant.kind != AST_SEMA_CONST_NONE) {
    AstHirInitializer *out = new_init(program, AST_HIR_INIT_CONSTANT,
                                      type ? type : fact.type, source->span);
    if (out)
      out->constant = fact.constant;
    return out;
  }
  if (expr->kind == AST_EXPR_STRING || expr->kind == AST_EXPR_BYTE_STRING ||
      expr->kind == AST_EXPR_C_STRING) {
    AstHirInitializer *out = new_init(program, AST_HIR_INIT_LITERAL,
                                      type ? type : fact.type, source->span);
    if (out) {
      out->literal_kind = expr->as.literal.token_kind;
      out->literal_span = expr->span;
      if (!hir_decode_literal_text(program, &program->arena, expr->span,
                                   out->literal_kind, &out->literal)) {
        hir_program_diag(program, AST_HIR_DIAG_INVALID_INITIALIZER,
                         AST_DIAG_ERROR, expr->span,
                         "cannot canonicalize string initializer literal");
        out->kind = AST_HIR_INIT_NONE;
      }
    }
    return out;
  }
  AstName ref_module = {0}, ref_symbol = {0};
  int64_t addend = 0;
  if (resolve_relocation(program, module, expr, &ref_module, &ref_symbol,
                         &addend)) {
    AstHirInitializer *out = new_init(program, AST_HIR_INIT_SYMBOL,
                                      type ? type : fact.type, source->span);
    if (out) {
      out->module = hir_copy_name(&program->arena, ref_module);
      out->symbol = hir_copy_name(&program->arena, ref_symbol);
      out->addend = addend;
    }
    return out;
  }
  hir_program_diag(
      program, AST_HIR_DIAG_INVALID_INITIALIZER, AST_DIAG_ERROR, source->span,
      "static initializer cannot be represented by canonical HIR data");
  return new_init(program, AST_HIR_INIT_NONE, type ? type : fact.type,
                  source->span);
}

const AstHirInitializer *
ast_hir_global_initializer(const AstHirProgram *program, size_t global_index) {
  if (!program || global_index >= program->global_count)
    return NULL;
  return program->globals[global_index].initializer;
}

int ast_hir_initializer_info(const AstHirInitializer *initializer,
                             AstHirInitializerInfo *out) {
  if (!initializer || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->kind = initializer->kind;
  out->type = initializer->type;
  out->span = initializer->span;
  out->constant = initializer->constant;
  out->literal_kind = initializer->literal_kind;
  out->literal = initializer->literal;
  out->literal_span = initializer->literal_span;
  out->module = initializer->module;
  out->symbol = initializer->symbol;
  out->addend = initializer->addend;
  out->child_count = initializer->child_count;
  return 1;
}

const AstHirInitializer *
ast_hir_initializer_child(const AstHirInitializer *initializer,
                          uint32_t index) {
  if (!initializer || index >= initializer->child_count ||
      !initializer->children)
    return NULL;
  return initializer->children[index];
}
