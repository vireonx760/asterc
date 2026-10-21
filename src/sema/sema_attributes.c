#include "sema_internal.h"

static int attr_is(const AstAttribute *attr, const char *name) {
  return attr && sema_name_eq_text(attr->name, name);
}

const AstAttribute *sema_find_attribute(AstAttributeArray attrs,
                                        const char *name) {
  for (uint32_t i = 0u; i < attrs.count; ++i) {
    if (attr_is(&attrs.items[i], name))
      return &attrs.items[i];
  }
  return NULL;
}

static void check_duplicate_attributes(AstSema *s, AstSemaModule *m,
                                       AstAttributeArray attrs) {
  for (uint32_t i = 0u; i < attrs.count; ++i) {
    for (uint32_t j = 0u; j < i; ++j) {
      if (sema_name_eq(attrs.items[i].name, attrs.items[j].name)) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR,
                  attrs.items[i].span, m->file.id,
                  "attribute '@%.*s' is specified more than once",
                  (int)attrs.items[i].name.span.length,
                  attrs.items[i].name.text);
        break;
      }
    }
  }
}

static int attr_arg_is_name_or_string(const AstExpr *expr) {
  if (!expr)
    return 0;
  return expr->kind == AST_EXPR_NAME || expr->kind == AST_EXPR_STRING ||
         expr->kind == AST_EXPR_BYTE_STRING || expr->kind == AST_EXPR_C_STRING;
}

static int attr_expect_arity(AstSema *s, AstSemaModule *m,
                             const AstAttribute *attr, uint32_t expected) {
  if (attr->args.count == expected)
    return 1;
  sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR, attr->span,
            m->file.id,
            "attribute '@%.*s' expects %u argument%s but received %u",
            (int)attr->name.span.length, attr->name.text, expected,
            expected == 1u ? "" : "s", attr->args.count);
  return 0;
}

static int attr_const_u64(AstSema *s, AstSemaModule *m, AstSemaFnCtx *fn,
                          const AstAttribute *attr, uint64_t *out) {
  if (!attr_expect_arity(s, m, attr, 1u))
    return 0;
  AstSemaConstCtx cc;
  memset(&cc, 0, sizeof(cc));
  cc.sema = s;
  cc.module = m;
  cc.fn = fn;
  AstSemaConstValue value =
      sema_eval_const_expr(&cc, attr->args.items[0], sema_builtin_usize(s));
  if (sema_const_is_valid(value)) {
    AstExprInfo fact_info;
    memset(&fact_info, 0, sizeof(fact_info));
    fact_info.type = value.type ? value.type : sema_builtin_usize(s);
    fact_info.constant = value;
    (void)sema_record_expr_fact(s, attr->args.items[0], fact_info);
  }
  if (!sema_const_to_u64(value, out)) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR, attr->span,
              m->file.id,
              "attribute '@%.*s' requires a non-negative compile-time integer",
              (int)attr->name.span.length, attr->name.text);
    return 0;
  }
  return 1;
}

static int is_power_of_two_u64(uint64_t value) {
  return value != 0u && (value & (value - 1u)) == 0u;
}

int sema_attribute_const_u64(AstSema *s, AstSemaModule *m, AstSemaFnCtx *fn,
                             const AstAttribute *attr, uint64_t *out) {
  return attr_const_u64(s, m, fn, attr, out);
}

static void validate_align_attribute(AstSema *s, AstSemaModule *m,
                                     AstSemaFnCtx *fn,
                                     const AstAttribute *attr) {
  uint64_t value = 0u;
  if (!attr_const_u64(s, m, fn, attr, &value))
    return;
  if (!is_power_of_two_u64(value) || value > UINT32_MAX) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR, attr->span,
              m->file.id,
              "alignment must be a positive power of two representable by the "
              "target frontend");
  }
}

static void validate_function_attributes(AstSema *s, AstSemaModule *m,
                                         const AstDecl *decl) {
  AstAttributeArray attrs = decl->attributes;
  check_duplicate_attributes(s, m, attrs);
  int has_naked = 0;
  int has_frame = 0;
  int force_inline = 0, no_inline = 0;
  for (uint32_t i = 0u; i < attrs.count; ++i) {
    const AstAttribute *attr = &attrs.items[i];
    if (attr_is(attr, "abi")) {
      if (attr_expect_arity(s, m, attr, 1u) &&
          !attr_arg_is_name_or_string(attr->args.items[0])) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR,
                  attr->span, m->file.id,
                  "@abi requires an ABI name or string");
      }
    } else if (attr_is(attr, "frame")) {
      has_frame = 1;
      if (attr_expect_arity(s, m, attr, 1u)) {
        const AstExpr *arg = attr->args.items[0];
        if (!arg || arg->kind != AST_EXPR_NAME ||
            (!sema_name_eq_text(arg->as.name.name, "auto") &&
             !sema_name_eq_text(arg->as.name.name, "validate"))) {
          sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR,
                    attr->span, m->file.id,
                    "@frame accepts only auto or validate");
        }
      }
    } else if (attr_is(attr, "naked")) {
      has_naked = 1;
      (void)attr_expect_arity(s, m, attr, 0u);
    } else if (attr_is(attr, "leaf") || attr_is(attr, "no_unwind")) {
      (void)attr_expect_arity(s, m, attr, 0u);
    } else if (attr_is(attr, "inline") || attr_is(attr, "noinline")) {
      (void)attr_expect_arity(s, m, attr, 0u);
      if (attr_is(attr, "inline"))
        force_inline = 1;
      else
        no_inline = 1;
    } else if (attr_is(attr, "stack_align")) {
      validate_align_attribute(s, m, NULL, attr);
    } else if (attr_is(attr, "requires") || attr_is(attr, "target") ||
               attr_is(attr, "link_name")) {
      if (attr_expect_arity(s, m, attr, 1u) &&
          !attr_arg_is_name_or_string(attr->args.items[0])) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR,
                  attr->span, m->file.id,
                  "@%.*s requires a name or string argument",
                  (int)attr->name.span.length, attr->name.text);
      }
    }
  }
  AstParamArray params = decl->kind == AST_DECL_FN ? decl->as.fn_decl.params
                                                   : decl->as.extern_fn.params;
  for (uint32_t pi = 0u; pi < params.count; ++pi) {
    check_duplicate_attributes(s, m, params.items[pi].attributes);
  }
  if (has_naked && has_frame) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR, decl->span,
              m->file.id, "@naked cannot be combined with @frame");
  }
  if (force_inline &&
      (no_inline || has_naked || decl->kind == AST_DECL_EXTERN_FN)) {
    sema_diag(
        s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR, decl->span,
        m->file.id,
        "@inline requires a managed definition without @noinline or @naked");
  }
}

static void validate_type_decl_attributes(AstSema *s, AstSemaModule *m,
                                          const AstDecl *decl) {
  check_duplicate_attributes(s, m, decl->attributes);
  check_duplicate_attributes(s, m, decl->as.type_decl.struct_attributes);
  for (uint32_t i = 0u; i < decl->as.type_decl.struct_attributes.count; ++i) {
    const AstAttribute *attr = &decl->as.type_decl.struct_attributes.items[i];
    if (attr_is(attr, "layout")) {
      if (attr_expect_arity(s, m, attr, 1u)) {
        const AstExpr *arg = attr->args.items[0];
        if (!arg || arg->kind != AST_EXPR_NAME ||
            !sema_name_eq_text(arg->as.name.name, "c")) {
          sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR,
                    attr->span, m->file.id, "@layout currently accepts only c");
        }
      }
    } else if (attr_is(attr, "align")) {
      validate_align_attribute(s, m, NULL, attr);
    }
  }
  for (uint32_t fi = 0u; fi < decl->as.type_decl.fields.count; ++fi) {
    AstField *field = &decl->as.type_decl.fields.items[fi];
    check_duplicate_attributes(s, m, field->attributes);
    for (uint32_t ai = 0u; ai < field->attributes.count; ++ai) {
      if (attr_is(&field->attributes.items[ai], "align"))
        validate_align_attribute(s, m, NULL, &field->attributes.items[ai]);
    }
  }
}

static void validate_section_attributes(AstSema *s, AstSemaModule *m,
                                        const AstDecl *decl) {
  AstAttributeArray attrs = decl->as.section_decl.section_attributes;
  check_duplicate_attributes(s, m, attrs);
  int readonly = 0;
  int writable = 0;
  for (uint32_t i = 0u; i < attrs.count; ++i) {
    const AstAttribute *attr = &attrs.items[i];
    if (attr_is(attr, "readonly")) {
      readonly = 1;
      (void)attr_expect_arity(s, m, attr, 0u);
    } else if (attr_is(attr, "writable")) {
      writable = 1;
      (void)attr_expect_arity(s, m, attr, 0u);
    } else if (attr_is(attr, "align")) {
      validate_align_attribute(s, m, NULL, attr);
    }
  }
  if (readonly && writable) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR, decl->span,
              m->file.id, "section cannot be both @readonly and @writable");
  }
  for (uint32_t di = 0u; di < decl->as.section_decl.data.count; ++di) {
    AstDataDecl *data = &decl->as.section_decl.data.items[di];
    check_duplicate_attributes(s, m, data->attributes);
    for (uint32_t ai = 0u; ai < data->attributes.count; ++ai) {
      if (attr_is(&data->attributes.items[ai], "align"))
        validate_align_attribute(s, m, NULL, &data->attributes.items[ai]);
    }
  }
}

void sema_validate_module_attributes(AstSema *s, AstSemaModule *m) {
  if (!s || !m || !m->ast)
    return;
  check_duplicate_attributes(s, m, m->ast->attributes);
  for (uint32_t i = 0u; i < m->ast->declarations.count; ++i) {
    const AstDecl *decl = m->ast->declarations.items[i];
    if (!decl)
      continue;
    switch (decl->kind) {
    case AST_DECL_FN:
    case AST_DECL_EXTERN_FN:
      validate_function_attributes(s, m, decl);
      break;
    case AST_DECL_TYPE:
      validate_type_decl_attributes(s, m, decl);
      break;
    case AST_DECL_SECTION:
      validate_section_attributes(s, m, decl);
      break;
    default:
      check_duplicate_attributes(s, m, decl->attributes);
      break;
    }
  }
}

void sema_validate_memory_attributes(AstSemaFnCtx *fn, AstAttributeArray attrs,
                                     int is_store) {
  if (!fn)
    return;
  AstSema *s = fn->sema;
  AstSemaModule *m = fn->module;
  check_duplicate_attributes(s, m, attrs);
  for (uint32_t i = 0u; i < attrs.count; ++i) {
    const AstAttribute *attr = &attrs.items[i];
    if (attr_is(attr, "align")) {
      validate_align_attribute(s, m, fn, attr);
    } else if (attr_is(attr, "volatile")) {
      (void)attr_expect_arity(s, m, attr, 0u);
    } else if (attr_is(attr, "nontemporal")) {
      (void)is_store;
      (void)attr_expect_arity(s, m, attr, 0u);
    }
  }
}

int sema_decl_has_attribute(const AstDecl *decl, const char *name) {
  if (!decl)
    return 0;
  return sema_find_attribute(decl->attributes, name) != NULL;
}
