#include "sema_internal.h"

static AstSemaConstInfo public_constant(AstSemaConstValue value) {
  AstSemaConstInfo out;
  memset(&out, 0, sizeof(out));
  if (!sema_const_is_valid(value))
    return out;
  out.lo = value.lo;
  out.hi = value.hi;
  out.floating = value.floating;
  out.is_negative = (uint8_t)((value.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u);
  out.is_signed = (uint8_t)((value.flags & AST_SEMA_CONSTF_SIGNED) != 0u);
  if (value.flags & AST_SEMA_CONSTF_BOOL)
    out.kind = AST_SEMA_CONST_BOOL;
  else if (value.flags & AST_SEMA_CONSTF_FLOAT)
    out.kind = AST_SEMA_CONST_FLOAT;
  else
    out.kind = AST_SEMA_CONST_INTEGER;
  return out;
}

int sema_prepare_fact_files(AstSema *s) {
  if (!s || s->fact_files)
    return 0;
  size_t file_count = ast_frontend_file_count(s->frontend);
  uint32_t max_file_id = 0u;
  for (size_t i = 0u; i < file_count; ++i) {
    AstFrontendFile file;
    if (!ast_frontend_get_file(s->frontend, i, &file))
      continue;
    if (file.id > max_file_id)
      max_file_id = file.id;
  }
  size_t table_count = (size_t)max_file_id + 1u;
  if (table_count == 0u)
    table_count = 1u;
  AstSemaFactFile *files =
      (AstSemaFactFile *)calloc(table_count, sizeof(*files));
  if (!files)
    return -1;

  for (size_t i = 0u; i < file_count; ++i) {
    AstFrontendFile file;
    if (!ast_frontend_get_file(s->frontend, i, &file) || !file.parsed ||
        file.id >= table_count)
      continue;
    AstSemaFactFile *facts = &files[file.id];
    facts->expr_capacity = file.parsed->stats.expr_nodes;
    facts->type_capacity = file.parsed->stats.type_nodes;
    if (facts->expr_capacity) {
      facts->expr_facts = (AstSemaExprFactSlot *)calloc(
          facts->expr_capacity, sizeof(*facts->expr_facts));
      if (!facts->expr_facts)
        goto fail;
    }
    if (facts->type_capacity) {
      facts->type_facts = (AstSemaTypeFactSlot *)calloc(
          facts->type_capacity, sizeof(*facts->type_facts));
      if (!facts->type_facts)
        goto fail;
    }
  }
  s->fact_files = files;
  s->fact_file_count = table_count;
  s->owns_fact_files = 1u;
  s->owns_expr_fact_storage = 1u;
  s->owns_type_fact_storage = 1u;
  return 0;

fail:
  for (size_t i = 0u; i < table_count; ++i) {
    free(files[i].expr_facts);
    free(files[i].type_facts);
  }
  free(files);
  return -1;
}

void sema_destroy_fact_files(AstSema *s) {
  if (!s || !s->fact_files)
    return;
  if (s->owns_fact_files) {
    for (size_t i = 0u; i < s->fact_file_count; ++i) {
      if (s->owns_expr_fact_storage)
        free(s->fact_files[i].expr_facts);
      if (s->owns_type_fact_storage)
        free(s->fact_files[i].type_facts);
    }
    free(s->fact_files);
  }
  s->fact_files = NULL;
  s->fact_file_count = 0u;
  s->owns_fact_files = 0u;
  s->owns_expr_fact_storage = 0u;
  s->owns_type_fact_storage = 0u;
}

static AstSemaExprFactSlot *fact_expr_slot(AstSema *s, const AstExpr *expr) {
  if (!s || !expr || expr->span.file_id >= s->fact_file_count)
    return NULL;
  AstSemaFactFile *file = &s->fact_files[expr->span.file_id];
  if ((size_t)expr->node_id >= file->expr_capacity)
    return NULL;
  return &file->expr_facts[expr->node_id];
}

static const AstSemaExprFactSlot *fact_expr_slot_const(const AstSema *s,
                                                       const AstExpr *expr) {
  if (!s || !expr || expr->span.file_id >= s->fact_file_count)
    return NULL;
  const AstSemaFactFile *file = &s->fact_files[expr->span.file_id];
  if ((size_t)expr->node_id >= file->expr_capacity)
    return NULL;
  return &file->expr_facts[expr->node_id];
}

static int fact_upsert(AstSema *s, const AstExpr *expr, AstSemaExprFact fact) {
  if (!s || !expr)
    return 0;
  AstSemaExprFactSlot *slot = fact_expr_slot(s, expr);
  if (!slot) {
    s->oom = 1u;
    return -1;
  }
  if (!slot->expr) {
    slot->expr = expr;
    slot->fact = fact;
    s->expr_fact_count++;
    return 0;
  }
  if (slot->expr != expr) {
    s->oom = 1u;
    return -1;
  }

  AstSemaExprFact *old = &slot->fact;
  int type_same =
      old->type == fact.type || sema_type_equal(old->type, fact.type);
  int signature_same =
      old->call_signature == fact.call_signature ||
      sema_type_equal(old->call_signature, fact.call_signature);
  int constant_same = old->constant.kind == fact.constant.kind &&
                      old->constant.lo == fact.constant.lo &&
                      old->constant.hi == fact.constant.hi &&
                      memcmp(&old->constant.floating, &fact.constant.floating,
                             sizeof(old->constant.floating)) == 0 &&
                      old->constant.is_negative == fact.constant.is_negative &&
                      old->constant.is_signed == fact.constant.is_signed;
  int resolution_same =
      old->resolution_kind == fact.resolution_kind &&
      old->declaration_span.file_id == fact.declaration_span.file_id &&
      old->declaration_span.offset == fact.declaration_span.offset &&
      old->declaration_span.length == fact.declaration_span.length;
  if (!type_same || !signature_same || !constant_same || !resolution_same ||
      old->effects != fact.effects || old->is_lvalue != fact.is_lvalue ||
      old->is_mutable != fact.is_mutable) {
    old->context_dependent = 1u;
    if (!type_same)
      old->type = NULL;
    if (!signature_same)
      old->call_signature = NULL;
    if (!constant_same)
      memset(&old->constant, 0, sizeof(old->constant));
    if (!resolution_same) {
      old->resolution_kind = AST_SEMA_RESOLUTION_NONE;
      memset(&old->declaration_span, 0, sizeof(old->declaration_span));
    }
    old->effects |= fact.effects;
    if (old->is_lvalue != fact.is_lvalue)
      old->is_lvalue = 0u;
    if (old->is_mutable != fact.is_mutable)
      old->is_mutable = 0u;
  }
  return 0;
}

static const AstSemaExprFact *fact_find_expr(const AstSema *s,
                                             const AstExpr *expr) {
  const AstSemaExprFactSlot *slot = fact_expr_slot_const(s, expr);
  return slot && slot->expr == expr ? &slot->fact : NULL;
}

static uint32_t fact_child_effect(const AstSema *s, const AstExpr *expr) {
  const AstSemaExprFact *fact = fact_find_expr(s, expr);
  return fact ? fact->effects : 0u;
}

static int fact_has_attribute(AstAttributeArray attrs, const char *name) {
  for (uint32_t i = 0u; i < attrs.count; ++i) {
    if (sema_name_eq_text(attrs.items[i].name, name))
      return 1;
  }
  return 0;
}

static int fact_intrinsic_name(const AstExpr *callee, const char *name) {
  return callee && callee->kind == AST_EXPR_NAME &&
         sema_name_eq_text(callee->as.name.name, name);
}

static uint32_t fact_expr_effects(const AstSema *s, const AstExpr *expr,
                                  const AstSemaExprFact *self) {
  if (!expr)
    return 0u;
  uint32_t effects = 0u;
  switch (expr->kind) {
  case AST_EXPR_UNARY:
    effects |= fact_child_effect(s, expr->as.unary.operand);
    if (expr->as.unary.op == AST_TOK_STAR)
      effects |= AST_SEMA_EFFECT_READ_MEMORY;
    break;
  case AST_EXPR_BINARY:
    effects |= fact_child_effect(s, expr->as.binary.left);
    effects |= fact_child_effect(s, expr->as.binary.right);
    if ((expr->as.binary.op == AST_TOK_SLASH ||
         expr->as.binary.op == AST_TOK_PERCENT) &&
        self && self->type && sema_type_is_integer(self->type))
      effects |= AST_SEMA_EFFECT_MAY_TRAP;
    break;
  case AST_EXPR_CONDITIONAL:
    effects |= fact_child_effect(s, expr->as.conditional.condition);
    effects |= fact_child_effect(s, expr->as.conditional.then_expr);
    effects |= fact_child_effect(s, expr->as.conditional.else_expr);
    break;
  case AST_EXPR_CALL:
    effects |= fact_child_effect(s, expr->as.call.callee);
    for (uint32_t i = 0u; i < expr->as.call.args.count; ++i)
      effects |= fact_child_effect(s, expr->as.call.args.items[i]);
    if (self && self->call_signature)
      effects |= AST_SEMA_EFFECT_CALL;
    if (fact_intrinsic_name(expr->as.call.callee, "ptr_to_int") ||
        fact_intrinsic_name(expr->as.call.callee, "int_to_ptr"))
      effects |= AST_SEMA_EFFECT_UNSAFE;
    break;
  case AST_EXPR_INDEX:
    effects |= fact_child_effect(s, expr->as.index.base);
    effects |= fact_child_effect(s, expr->as.index.index);
    break;
  case AST_EXPR_MEMBER:
    effects |= fact_child_effect(s, expr->as.member.base);
    break;
  case AST_EXPR_LOAD:
    effects |= fact_child_effect(s, expr->as.load.address) |
               AST_SEMA_EFFECT_READ_MEMORY;
    if (fact_has_attribute(expr->as.load.attributes, "volatile"))
      effects |= AST_SEMA_EFFECT_VOLATILE;
    break;
  case AST_EXPR_CAST:
    effects |= fact_child_effect(s, expr->as.cast.value);
    if (expr->as.cast.op == AST_TOK_KW_CAST)
      effects |= AST_SEMA_EFFECT_MAY_TRAP;
    if (expr->as.cast.op == AST_TOK_KW_PTRCAST)
      effects |= AST_SEMA_EFFECT_UNSAFE;
    break;
  case AST_EXPR_ATOMIC:
    effects |=
        fact_child_effect(s, expr->as.atomic.address) | AST_SEMA_EFFECT_ATOMIC;
    for (uint32_t i = 0u; i < expr->as.atomic.args.count; ++i)
      effects |= fact_child_effect(s, expr->as.atomic.args.items[i]);
    if (sema_name_eq_text(expr->as.atomic.op, "load"))
      effects |= AST_SEMA_EFFECT_READ_MEMORY;
    else if (sema_name_eq_text(expr->as.atomic.op, "store"))
      effects |= AST_SEMA_EFFECT_WRITE_MEMORY;
    else
      effects |= AST_SEMA_EFFECT_READ_MEMORY | AST_SEMA_EFFECT_WRITE_MEMORY;
    break;
  case AST_EXPR_ATTR:
    effects |= fact_child_effect(s, expr->as.attributed.base);
    break;
  default:
    break;
  }
  return effects;
}

int sema_record_expr_fact(AstSema *s, const AstExpr *expr, AstExprInfo info) {
  AstSemaExprFact fact;
  memset(&fact, 0, sizeof(fact));
  fact.type = info.type;
  fact.call_signature = info.call_signature;
  fact.constant = public_constant(info.constant);
  fact.declaration_span = info.declaration_span;
  fact.resolution_kind = info.resolution_kind;
  fact.effects = fact_expr_effects(s, expr, &fact);
  fact.is_lvalue = info.is_lvalue;
  fact.is_mutable = info.is_mutable;
  return fact_upsert(s, expr, fact);
}

int sema_refine_expr_fact_type(AstSema *s, const AstExpr *expr,
                               const AstSemaType *type) {
  if (!s || !expr || !type)
    return 0;
  AstSemaExprFactSlot *slot = fact_expr_slot(s, expr);
  if (!slot || slot->expr != expr)
    return 0;
  AstSemaExprFact *fact = &slot->fact;
  if (fact->context_dependent)
    return 0;
  if (!fact->type || fact->type->kind == AST_SEMA_TYPE_UNTYPED_INT ||
      fact->type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT ||
      fact->type->kind == AST_SEMA_TYPE_STRING ||
      fact->type->kind == AST_SEMA_TYPE_BYTE_STRING ||
      fact->type->kind == AST_SEMA_TYPE_C_STRING) {
    fact->type = type;
  }
  return 0;
}

int sema_merge_expr_facts(AstSema *dst, const AstSema *src) {
  if (!dst || !src || dst->fact_files == src->fact_files)
    return 0;
  for (size_t f = 0u; f < src->fact_file_count; ++f) {
    const AstSemaFactFile *file = &src->fact_files[f];
    for (size_t i = 0u; i < file->expr_capacity; ++i) {
      if (!file->expr_facts[i].expr)
        continue;
      if (fact_upsert(dst, file->expr_facts[i].expr,
                      file->expr_facts[i].fact) != 0)
        return -1;
    }
  }
  return 0;
}

int ast_sema_expr_fact(const AstSema *s, const AstExpr *expr,
                       AstSemaExprFact *out) {
  if (!s || !expr || !out)
    return 0;
  const AstSemaExprFactSlot *slot = fact_expr_slot_const(s, expr);
  if (!slot || slot->expr != expr)
    return 0;
  *out = slot->fact;
  return 1;
}

static AstSemaTypeFactSlot *fact_type_slot(AstSema *s, const AstType *node) {
  if (!s || !node || node->node_id == AST_NODE_ID_SYNTHETIC ||
      node->span.file_id >= s->fact_file_count)
    return NULL;
  AstSemaFactFile *file = &s->fact_files[node->span.file_id];
  if ((size_t)node->node_id >= file->type_capacity)
    return NULL;
  return &file->type_facts[node->node_id];
}

static const AstSemaTypeFactSlot *fact_type_slot_const(const AstSema *s,
                                                       const AstType *node) {
  if (!s || !node || node->node_id == AST_NODE_ID_SYNTHETIC ||
      node->span.file_id >= s->fact_file_count)
    return NULL;
  const AstSemaFactFile *file = &s->fact_files[node->span.file_id];
  if ((size_t)node->node_id >= file->type_capacity)
    return NULL;
  return &file->type_facts[node->node_id];
}

static int type_fact_upsert(AstSema *s, const AstType *node,
                            AstSemaTypeFact fact) {
  if (!s || !node || node->node_id == AST_NODE_ID_SYNTHETIC)
    return 0;
  AstSemaTypeFactSlot *slot = fact_type_slot(s, node);
  if (!slot) {
    s->oom = 1u;
    return -1;
  }
  if (!slot->node) {
    slot->node = node;
    slot->fact = fact;
    s->type_fact_count++;
    return 0;
  }
  if (slot->node != node) {
    s->oom = 1u;
    return -1;
  }
  AstSemaTypeFact *old = &slot->fact;
  if (old->type != fact.type && !sema_type_equal(old->type, fact.type)) {
    old->type = NULL;
    old->context_dependent = 1u;
  }
  return 0;
}

int sema_record_type_fact(AstSema *s, const AstType *node,
                          const AstSemaType *type) {
  AstSemaTypeFact fact;
  memset(&fact, 0, sizeof(fact));
  fact.type = type;
  return type_fact_upsert(s, node, fact);
}

int sema_merge_type_facts(AstSema *dst, const AstSema *src) {
  if (!dst || !src || dst->fact_files == src->fact_files)
    return 0;
  for (size_t f = 0u; f < src->fact_file_count; ++f) {
    const AstSemaFactFile *file = &src->fact_files[f];
    for (size_t i = 0u; i < file->type_capacity; ++i) {
      if (!file->type_facts[i].node)
        continue;
      if (type_fact_upsert(dst, file->type_facts[i].node,
                           file->type_facts[i].fact) != 0)
        return -1;
    }
  }
  return 0;
}

int ast_sema_type_fact(const AstSema *s, const AstType *node,
                       AstSemaTypeFact *out) {
  if (!s || !node || !out)
    return 0;
  const AstSemaTypeFactSlot *slot = fact_type_slot_const(s, node);
  if (!slot || slot->node != node)
    return 0;
  *out = slot->fact;
  return 1;
}
