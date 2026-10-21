#include "sema_internal.h"

AstSemaSymbol *sema_resolve_value_name(AstSema *s, AstSemaModule *m,
                                       AstName name, int require_export,
                                       AstSpan use_span) {
  if (!s || !m)
    return NULL;
  AstSemaSymbol *sym = sema_module_find_value(m, name);
  if (!sym)
    return NULL;
  if (require_export && !(sym->flags & AST_SEMA_SYMF_EXPORTED)) {
    sema_diag(s, AST_SEMA_DIAG_SYMBOL_NOT_EXPORTED, AST_DIAG_ERROR, use_span,
              m->file.id, "symbol '%.*s' is not exported",
              (int)name.span.length, name.text);
    return NULL;
  }
  return sym;
}

#define AST_SEMA_MAX_QUALIFIED_SEGMENTS 64u

static uint32_t flatten_member_segments(const AstExpr *expr, AstName *segments,
                                        uint32_t capacity) {
  if (!expr || !segments || capacity == 0u)
    return 0u;
  AstName reverse[AST_SEMA_MAX_QUALIFIED_SEGMENTS];
  uint32_t reverse_count = 0u;
  const AstExpr *cursor = expr;
  while (cursor && cursor->kind == AST_EXPR_MEMBER) {
    if (reverse_count >= AST_SEMA_MAX_QUALIFIED_SEGMENTS)
      return 0u;
    reverse[reverse_count++] = cursor->as.member.member;
    cursor = cursor->as.member.base;
  }
  if (!cursor || cursor->kind != AST_EXPR_NAME || reverse_count + 1u > capacity)
    return 0u;
  segments[0] = cursor->as.name.name;
  for (uint32_t i = 0u; i < reverse_count; ++i)
    segments[i + 1u] = reverse[reverse_count - i - 1u];
  return reverse_count + 1u;
}

static uint32_t qualified_name_segment_count(AstName name) {
  if (!name.text || name.span.length == 0u)
    return 0u;
  uint32_t count = 1u;
  for (uint32_t i = 0u; i < name.span.length; ++i)
    if (name.text[i] == '.')
      count++;
  return count;
}

static int qualified_name_matches_segments(AstName qualified,
                                           const AstName *segments,
                                           uint32_t segment_count) {
  if (!qualified.text || !segments || segment_count == 0u)
    return 0;
  uint32_t expected = qualified_name_segment_count(qualified);
  if (expected != segment_count)
    return 0;
  uint32_t begin = 0u;
  uint32_t segment = 0u;
  for (uint32_t i = 0u; i <= qualified.span.length; ++i) {
    if (i != qualified.span.length && qualified.text[i] != '.')
      continue;
    uint32_t len = i - begin;
    if (segment >= segment_count || segments[segment].span.length != len ||
        memcmp(segments[segment].text, qualified.text + begin, len) != 0)
      return 0;
    segment++;
    begin = i + 1u;
  }
  return segment == segment_count;
}

AstSemaSymbol *sema_resolve_import_member(AstSema *s, AstSemaModule *m,
                                          const AstExpr *expr,
                                          AstName *final_name, int want_type) {
  if (!s || !m || !expr)
    return NULL;
  AstName segments[AST_SEMA_MAX_QUALIFIED_SEGMENTS];
  uint32_t count =
      flatten_member_segments(expr, segments, AST_SEMA_MAX_QUALIFIED_SEGMENTS);
  if (count < 2u)
    return NULL;

  AstSemaImport *match = NULL;
  uint32_t prefix_count = 0u;
  for (size_t i = 0u; i < m->import_count; ++i) {
    AstSemaImport *imp = &m->imports[i];
    if (!imp->target)
      continue;
    if (sema_name_eq(imp->alias, segments[0]) && count == 2u) {
      match = imp;
      prefix_count = 1u;
      break;
    }
    uint32_t module_segments = qualified_name_segment_count(imp->module_name);
    if (module_segments + 1u == count &&
        qualified_name_matches_segments(imp->module_name, segments,
                                        module_segments)) {
      match = imp;
      prefix_count = module_segments;
      break;
    }
  }
  if (!match || prefix_count + 1u != count)
    return NULL;

  AstName member = segments[count - 1u];
  AstSemaSymbol *sym = want_type
                           ? sema_module_find_type(match->target, member)
                           : sema_module_find_value(match->target, member);
  if (!sym)
    return NULL;
  if (!(sym->flags & AST_SEMA_SYMF_EXPORTED)) {
    sema_diag(s, AST_SEMA_DIAG_SYMBOL_NOT_EXPORTED, AST_DIAG_ERROR, expr->span,
              m->file.id, "symbol '%.*s' in module '%.*s' is not exported",
              (int)member.span.length, member.text,
              (int)match->module_name.span.length, match->module_name.text);
    return NULL;
  }
  if (final_name)
    *final_name = member;
  return sym;
}

const AstSemaField *sema_find_struct_field(const AstSemaType *type,
                                           AstName name) {
  if (!type)
    return NULL;
  if (sema_type_is_pointer(type))
    type = type->as.pointer.element;
  if (!type || type->kind != AST_SEMA_TYPE_STRUCT)
    return NULL;
  for (uint32_t i = 0u; i < type->as.record.field_count; ++i) {
    if (sema_name_eq(type->as.record.fields[i].name, name))
      return &type->as.record.fields[i];
  }
  return NULL;
}

const AstSemaType *sema_resolve_type_operand(AstSema *s, AstSemaModule *m,
                                             AstSemaFnCtx *fn,
                                             const AstExpr *expr,
                                             int *is_type_name) {
  if (is_type_name)
    *is_type_name = 0;
  if (!s || !m || !expr)
    return NULL;

  if (expr->kind == AST_EXPR_TYPE) {
    if (is_type_name)
      *is_type_name = 1;
    return sema_resolve_type(s, m, expr->as.type_operand.type, fn, 0u);
  }

  if (expr->kind == AST_EXPR_NAME) {
    if (fn) {
      for (size_t i = fn->generic_count; i > 0u; --i) {
        AstSemaGenericBinding *binding = &fn->generics[i - 1u];
        if (!binding->is_const &&
            sema_name_eq(binding->name, expr->as.name.name)) {
          if (is_type_name)
            *is_type_name = 1;
          return binding->type_value;
        }
      }
    }
    AstSemaSymbol *type_symbol = sema_module_find_type(m, expr->as.name.name);
    if (type_symbol && type_symbol->type) {
      if (is_type_name)
        *is_type_name = 1;
      return type_symbol->type;
    }
    if (fn) {
      AstSemaLocal *local = sema_fn_find_local(fn, expr->as.name.name);
      if (local)
        return local->type;
    }
    AstSemaSymbol *value_symbol = sema_module_find_value(m, expr->as.name.name);
    return value_symbol ? value_symbol->type : NULL;
  }

  if (expr->kind == AST_EXPR_MEMBER) {
    AstSemaSymbol *type_symbol =
        sema_resolve_import_member(s, m, expr, NULL, 1);
    if (type_symbol && type_symbol->type) {
      if (is_type_name)
        *is_type_name = 1;
      return type_symbol->type;
    }
    AstSemaSymbol *value_symbol =
        sema_resolve_import_member(s, m, expr, NULL, 0);
    if (value_symbol)
      return value_symbol->type;
  }
  return NULL;
}

static AstSemaType *make_type_parameter(AstSema *s, AstName name) {
  AstSemaType *t = (AstSemaType *)sema_arena_alloc(s, sizeof(*t),
                                                   ASTER_ALIGNOF(AstSemaType));
  if (!t)
    return NULL;
  t->kind = AST_SEMA_TYPE_TYPE_PARAMETER;
  t->size = AST_SEMA_UNKNOWN_SIZE;
  t->alignment = 1u;
  t->as.record.name = name;
  t->hash =
      (name.hash ? name.hash : ast_hash_bytes(name.text, name.span.length)) ^
      UINT64_C(0x71f98dc53aa1c8b7);
  s->stats.type_count++;
  return t;
}

static void validate_generic_parameter_names(AstSema *s, AstSemaModule *m,
                                             const AstDecl *decl) {
  AstGenericParamArray gp;
  AstParamArray params;
  memset(&gp, 0, sizeof(gp));
  memset(&params, 0, sizeof(params));
  if (!decl)
    return;
  if (decl->kind == AST_DECL_TYPE) {
    gp = decl->as.type_decl.generics;
  } else if (decl->kind == AST_DECL_FN) {
    gp = decl->as.fn_decl.generics;
    params = decl->as.fn_decl.params;
  } else {
    return;
  }

  for (uint32_t i = 0u; i < gp.count; ++i) {
    for (uint32_t j = 0u; j < i; ++j) {
      if (sema_name_eq(gp.items[i].name, gp.items[j].name)) {
        sema_diag(s, AST_SEMA_DIAG_DUPLICATE_PARAMETER, AST_DIAG_ERROR,
                  gp.items[i].name.span, m->file.id,
                  "duplicate generic parameter '%.*s'",
                  (int)gp.items[i].name.span.length, gp.items[i].name.text);
        break;
      }
    }
    for (uint32_t j = 0u; j < params.count; ++j) {
      if (sema_name_eq(gp.items[i].name, params.items[j].name)) {
        sema_diag(s, AST_SEMA_DIAG_DUPLICATE_PARAMETER, AST_DIAG_ERROR,
                  params.items[j].name.span, m->file.id,
                  "function parameter '%.*s' conflicts with a generic "
                  "parameter of the same name",
                  (int)params.items[j].name.span.length,
                  params.items[j].name.text);
        break;
      }
    }
  }
}

AstSemaGenericBinding *sema_build_generic_bindings(AstSema *s, AstSemaModule *m,
                                                   AstGenericParamArray gp,
                                                   size_t *count_out) {
  *count_out = gp.count;
  if (!gp.count)
    return NULL;
  AstSemaGenericBinding *b = (AstSemaGenericBinding *)sema_arena_alloc(
      s, (size_t)gp.count * sizeof(*b), ASTER_ALIGNOF(AstSemaGenericBinding));
  if (!b)
    return NULL;
  for (uint32_t i = 0u; i < gp.count; ++i) {
    b[i].name = gp.items[i].name;
    if (gp.items[i].kind == AST_GENERIC_TYPE)
      b[i].type_value = make_type_parameter(s, gp.items[i].name);
    else {
      b[i].is_const = 1u;
      b[i].const_value = sema_const_invalid();
      b[i].const_value.type = sema_resolve_type_with_bindings(
          s, m, gp.items[i].const_type, b, i, 0u);
      b[i].dependent_const = gp.items[i].name;
      b[i].has_dependent_const = 1u;
    }
  }
  return b;
}

static const AstSemaType *
resolve_function_signature(AstSema *s, AstSemaModule *m, AstSemaSymbol *sym) {
  const AstDecl *d = sym->decl;
  AstParamArray params;
  AstType *ret;
  AstGenericParamArray gp;
  memset(&gp, 0, sizeof(gp));
  if (d->kind == AST_DECL_FN) {
    params = d->as.fn_decl.params;
    ret = d->as.fn_decl.return_type;
    gp = d->as.fn_decl.generics;
  } else {
    params = d->as.extern_fn.params;
    ret = d->as.extern_fn.return_type;
  }
  size_t generic_count = 0u;
  AstSemaGenericBinding *bindings =
      sema_build_generic_bindings(s, m, gp, &generic_count);
  sym->generic_bindings = bindings;
  sym->generic_count = generic_count;
  const AstSemaType **pt = NULL;
  if (params.count) {
    pt = (const AstSemaType **)sema_arena_alloc(
        s, (size_t)params.count * sizeof(*pt), ASTER_ALIGNOF(AstSemaType *));
    if (!pt)
      return sema_builtin_error(s);
  }
  for (uint32_t i = 0u; i < params.count; ++i) {
    for (uint32_t j = 0u; j < i; ++j)
      if (sema_name_eq(params.items[i].name, params.items[j].name)) {
        sema_diag(
            s, AST_SEMA_DIAG_DUPLICATE_PARAMETER, AST_DIAG_ERROR,
            params.items[i].name.span, m->file.id, "duplicate parameter '%.*s'",
            (int)params.items[i].name.span.length, params.items[i].name.text);
      }
    pt[i] = sema_resolve_type_with_bindings(s, m, params.items[i].type,
                                            bindings, generic_count, 0u);
    if (!sema_type_is_value_type(pt[i])) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                params.items[i].type->span, m->file.id,
                "function parameter type must be a value type");
    }
  }
  const AstSemaType *rt = ret ? sema_resolve_type_with_bindings(
                                    s, m, ret, bindings, generic_count, 0u)
                              : sema_builtin_void(s);
  if (rt->kind != AST_SEMA_TYPE_VOID && !sema_type_is_value_type(rt)) {
    sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
              ret ? ret->span : d->span, m->file.id,
              "function result must be void or a value type");
  }
  return sema_make_function(s, pt, params.count, rt);
}

static void resolve_symbol_interface(AstSema *s, AstSemaModule *m,
                                     AstSemaSymbol *sym) {
  if (!sym || (sym->flags & AST_SEMA_SYMF_RESOLVED))
    return;
  if (sym->kind == AST_SEMA_SYMBOL_TYPE) {
    if (sym->decl && sym->decl->as.type_decl.generics.count == 0u) {
      AstType named;
      memset(&named, 0, sizeof(named));
      named.kind = AST_TYPE_NAMED;
      named.node_id = AST_NODE_ID_SYNTHETIC;
      named.span = sym->name.span;
      named.as.named.name = sym->name;
      sym->type = sema_resolve_type(s, m, &named, NULL, 0u);
    }
    return;
  }
  if (sym->flags & AST_SEMA_SYMF_RESOLVING)
    return;
  sym->flags |= AST_SEMA_SYMF_RESOLVING;
  switch (sym->kind) {
  case AST_SEMA_SYMBOL_FUNCTION:
  case AST_SEMA_SYMBOL_EXTERN_FUNCTION:
    sym->type = resolve_function_signature(s, m, sym);
    break;
  case AST_SEMA_SYMBOL_CONST:
    if (sym->data_decl)
      sym->type = sema_resolve_type(s, m, sym->data_decl->type, NULL, 0u);
    else if (sym->decl && sym->decl->kind == AST_DECL_CONST &&
             sym->decl->as.const_decl.type)
      sym->type =
          sema_resolve_type(s, m, sym->decl->as.const_decl.type, NULL, 0u);
    else
      sym->type = sema_builtin_untyped_int(s);
    break;
  case AST_SEMA_SYMBOL_GLOBAL:
    if (sym->data_decl)
      sym->type = sema_resolve_type(s, m, sym->data_decl->type, NULL, 0u);
    break;
  default:
    break;
  }
  if ((sym->kind == AST_SEMA_SYMBOL_CONST ||
       sym->kind == AST_SEMA_SYMBOL_GLOBAL) &&
      sym->type && !sema_type_is_value_type(sym->type)) {
    AstSpan type_span = sym->name.span;
    if (sym->data_decl && sym->data_decl->type)
      type_span = sym->data_decl->type->span;
    else if (sym->decl && sym->decl->kind == AST_DECL_CONST &&
             sym->decl->as.const_decl.type)
      type_span = sym->decl->as.const_decl.type->span;
    sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, type_span,
              m->file.id,
              "global or constant declaration requires a value type");
  }
  sym->flags &= ~AST_SEMA_SYMF_RESOLVING;
  sym->flags |= AST_SEMA_SYMF_RESOLVED;
}

int sema_resolve_module_interfaces(AstSema *s) {
  if (!s)
    return -1;
  for (size_t mi = 0u; mi < s->module_count; ++mi) {
    AstSemaModule *m = &s->modules[mi];
    if (!m->ast)
      continue;
    for (size_t si = 0u; si < m->symbol_count; ++si) {
      if (m->symbols[si].decl)
        validate_generic_parameter_names(s, m, m->symbols[si].decl);
      resolve_symbol_interface(s, m, &m->symbols[si]);
    }
  }
  return s->oom ? -1 : 0;
}
