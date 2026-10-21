#include "sema_internal.h"

#include <limits.h>

static AstExprInfo expr_info(const AstSemaType *type) {
  AstExprInfo out;
  memset(&out, 0, sizeof(out));
  out.type = type;
  return out;
}

static AstExprInfo expr_error(AstSema *s) {
  return expr_info(sema_builtin_error(s));
}

AstSemaLocal *sema_fn_find_local(AstSemaFnCtx *fn, AstName name) {
  if (!fn)
    return NULL;
  for (size_t i = fn->local_count; i > 0u; --i) {
    if (sema_name_eq(fn->locals[i - 1u].name, name))
      return &fn->locals[i - 1u];
  }
  return NULL;
}

static int name_text(AstName n, const char *text) {
  return sema_name_eq_text(n, text);
}

static const AstSemaType *coerce_binary_type(AstSemaFnCtx *fn, AstExprInfo a,
                                             AstExprInfo b, AstSpan span) {
  AstSema *s = fn->sema;
  if (a.type->kind == AST_SEMA_TYPE_ERROR ||
      b.type->kind == AST_SEMA_TYPE_ERROR)
    return sema_builtin_error(s);
  if (sema_type_equal(a.type, b.type))
    return a.type;
  if (a.type->kind == AST_SEMA_TYPE_UNTYPED_INT &&
      sema_type_is_integer(b.type) &&
      sema_type_assignable(s, b.type, a, span, fn->module->file.id))
    return b.type;
  if (b.type->kind == AST_SEMA_TYPE_UNTYPED_INT &&
      sema_type_is_integer(a.type) &&
      sema_type_assignable(s, a.type, b, span, fn->module->file.id))
    return a.type;
  if (a.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT &&
      b.type->kind == AST_SEMA_TYPE_FLOAT)
    return b.type;
  if (b.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT &&
      a.type->kind == AST_SEMA_TYPE_FLOAT)
    return a.type;
  return NULL;
}

static AstExprInfo check_name(AstSemaFnCtx *fn, const AstExpr *expr) {
  AstName n = expr->as.name.name;
  AstSemaLocal *local = sema_fn_find_local(fn, n);
  if (local) {
    if (!local->initialized) {
      sema_diag(fn->sema, AST_SEMA_DIAG_UNINITIALIZED_VALUE, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "value '%.*s' is used before initialization",
                (int)n.span.length, n.text);
    }
    AstExprInfo out = expr_info(local->type);
    out.is_lvalue = 1u;
    out.is_mutable = local->mutable_value;
    out.declaration_span = local->declaration_span;
    out.resolution_kind = local->kind == AST_SEMA_SYMBOL_PARAMETER
                              ? AST_SEMA_RESOLUTION_PARAMETER
                          : local->kind == AST_SEMA_SYMBOL_LOCAL_CONST
                              ? AST_SEMA_RESOLUTION_LOCAL_CONST
                              : AST_SEMA_RESOLUTION_LOCAL;
    if (local->kind == AST_SEMA_SYMBOL_LOCAL_CONST &&
        sema_const_is_valid(local->const_value)) {
      out.constant = local->const_value;
    }
    return out;
  }
  for (size_t i = fn->generic_count; i > 0u; --i) {
    AstSemaGenericBinding *binding = &fn->generics[i - 1u];
    if (!binding->is_const || !sema_name_eq(binding->name, n))
      continue;
    AstExprInfo out =
        expr_info(binding->const_value.type ? binding->const_value.type
                                            : sema_builtin_usize(fn->sema));
    out.constant = binding->const_value;
    out.declaration_span = binding->name.span;
    out.resolution_kind = AST_SEMA_RESOLUTION_GENERIC_CONST;
    return out;
  }
  AstSemaSymbol *sym = sema_module_find_value(fn->module, n);
  if (!sym) {
    sema_diag(fn->sema, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR, expr->span,
              fn->module->file.id, "unknown value '%.*s'", (int)n.span.length,
              n.text);
    return expr_error(fn->sema);
  }
  AstExprInfo out =
      expr_info(sym->type ? sym->type : sema_builtin_error(fn->sema));
  out.is_lvalue =
      sym->kind == AST_SEMA_SYMBOL_GLOBAL || sym->kind == AST_SEMA_SYMBOL_CONST;
  out.is_mutable = (sym->flags & AST_SEMA_SYMF_MUTABLE) != 0u;
  out.declaration_span = sym->name.span;
  out.resolution_kind = AST_SEMA_RESOLUTION_TOP_LEVEL;
  if (sym->kind == AST_SEMA_SYMBOL_CONST) {
    AstSemaConstCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.sema = fn->sema;
    cc.module = fn->module;
    cc.fn = fn;
    out.constant = sema_eval_const_expr(&cc, expr, sym->type);
  }
  return out;
}

static AstExprInfo check_unary(AstSemaFnCtx *fn, const AstExpr *expr,
                               const AstSemaType *expected) {
  AstSema *s = fn->sema;
  AstTokenKind op = expr->as.unary.op;
  const AstSemaType *operand_expected = NULL;
  if (op == AST_TOK_PLUS && expected && sema_type_is_numeric(expected))
    operand_expected = expected;
  else if (op == AST_TOK_TILDE && expected && sema_type_is_integer(expected))
    operand_expected = expected;
  AstExprInfo a = sema_check_expr(fn, expr->as.unary.operand, operand_expected);
  if (op == AST_TOK_BANG) {
    if (sema_type_is_bool(a.type))
      return expr_info(sema_builtin_bool(s));
    if (a.type->kind == AST_SEMA_TYPE_MASK ||
        a.type->kind == AST_SEMA_TYPE_PRED)
      return expr_info(a.type);
    sema_diag(s, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "logical negation requires bool, mask<N>, or pred<T>");
    return expr_error(s);
  }
  if (op == AST_TOK_AMP) {
    if (!a.is_lvalue && a.type->kind != AST_SEMA_TYPE_FUNCTION) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_UNARY_OPERAND, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "address-of requires an addressable value");
      return expr_error(s);
    }
    return expr_info(sema_make_pointer(
        s, AST_SEMA_TYPE_PTR, a.type, a.is_mutable ? 0u : AST_TYPE_QUAL_CONST));
  }
  if (op == AST_TOK_STAR) {
    if (!sema_type_is_pointer(a.type)) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_POINTER, AST_DIAG_ERROR, expr->span,
                fn->module->file.id, "dereference requires ptr<T> or cap<T>");
      return expr_error(s);
    }
    if (!a.type->as.pointer.element ||
        a.type->as.pointer.element->kind == AST_SEMA_TYPE_VOID) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_UNARY_OPERAND, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "ptr<void> and cap<void> cannot be dereferenced without an "
                "explicit ptrcast");
      return expr_error(s);
    }
    AstExprInfo out = expr_info(a.type->as.pointer.element);
    out.is_lvalue = 1u;
    out.is_mutable =
        (a.type->as.pointer.qualifiers & AST_TYPE_QUAL_CONST) == 0u;
    return out;
  }
  if (op == AST_TOK_TILDE) {
    if (a.type->kind == AST_SEMA_TYPE_MASK ||
        a.type->kind == AST_SEMA_TYPE_PRED)
      return expr_info(a.type);
    if (!sema_type_is_integer(a.type) && !sema_type_is_vector_integer(a.type)) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "bitwise complement requires an integer, integer vector, mask, "
                "or predicate type");
      return expr_error(s);
    }
    const AstSemaType *result_type = a.type;
    if (a.type->kind == AST_SEMA_TYPE_UNTYPED_INT && expected &&
        sema_type_is_integer(expected) &&
        sema_type_assignable(s, expected, a, expr->as.unary.operand->span,
                             fn->module->file.id))
      result_type = expected;
    AstExprInfo out = expr_info(result_type);
    if (sema_const_is_valid(a.constant)) {
      AstSemaConstCtx cc;
      memset(&cc, 0, sizeof(cc));
      cc.sema = s;
      cc.module = fn->module;
      cc.fn = fn;
      out.constant = sema_eval_const_expr(&cc, expr, result_type);
    }
    return out;
  }
  if (op == AST_TOK_PLUS || op == AST_TOK_MINUS) {
    if (!sema_type_is_numeric(a.type) && !sema_type_is_vector_numeric(a.type)) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_NUMERIC, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "unary arithmetic requires a numeric scalar or vector type");
      return expr_error(s);
    }
    const AstSemaType *result_type = a.type;
    AstSemaConstValue raw_constant = sema_const_invalid();
    if (sema_const_is_valid(a.constant)) {
      AstSemaConstCtx cc;
      memset(&cc, 0, sizeof(cc));
      cc.sema = s;
      cc.module = fn->module;
      cc.fn = fn;
      raw_constant = sema_eval_const_expr(&cc, expr, NULL);
    }
    if ((a.type->kind == AST_SEMA_TYPE_UNTYPED_INT ||
         a.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT) &&
        expected && sema_type_is_numeric(expected)) {
      AstExprInfo candidate = a;
      if (sema_const_is_valid(raw_constant))
        candidate.constant = raw_constant;
      if (sema_type_assignable(s, expected, candidate, expr->span,
                               fn->module->file.id))
        result_type = expected;
    }
    AstExprInfo out = expr_info(result_type);
    if (sema_const_is_valid(raw_constant)) {
      if (result_type != a.type) {
        AstSemaConstCtx cc;
        memset(&cc, 0, sizeof(cc));
        cc.sema = s;
        cc.module = fn->module;
        cc.fn = fn;
        out.constant = sema_eval_const_expr(&cc, expr, result_type);
      } else
        out.constant = raw_constant;
    }
    return out;
  }
  return expr_error(s);
}

static const AstSemaType *vector_comparison_result(AstSema *s,
                                                   const AstSemaType *type) {
  if (!type)
    return sema_builtin_error(s);
  if (type->kind == AST_SEMA_TYPE_VECTOR) {
    if (type->as.vector.dependent)
      return sema_builtin_error(s);
    return sema_make_mask(s, type->as.vector.lanes);
  }
  if (type->kind == AST_SEMA_TYPE_SVECTOR)
    return sema_make_pred(s, type->as.scalable.element);
  return sema_builtin_error(s);
}

static int const_is_negative_one(AstSemaConstValue value) {
  return sema_const_is_valid(value) &&
         (value.flags & AST_SEMA_CONSTF_FLOAT) == 0u &&
         (value.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u && value.hi == 0u &&
         value.lo == 1u;
}

static int const_is_signed_minimum(AstSemaConstValue value,
                                   const AstSemaType *type) {
  if (!type || type->kind != AST_SEMA_TYPE_INT ||
      (type->flags & AST_SEMA_TYPEF_SIGNED) == 0u ||
      !sema_const_is_valid(value) ||
      (value.flags & AST_SEMA_CONSTF_NEGATIVE) == 0u)
    return 0;
  uint32_t bits = type->bit_width;
  if (bits == 0u || bits > 128u)
    return 0;
  if (bits <= 64u) {
    uint64_t magnitude = UINT64_C(1) << (bits - 1u);
    return value.hi == 0u && value.lo == magnitude;
  }
  uint64_t hi = UINT64_C(1) << (bits - 65u);
  return value.lo == 0u && value.hi == hi;
}

static int vector_binary_operator_valid(const AstSemaType *type,
                                        AstTokenKind op) {
  if (!type || (type->kind != AST_SEMA_TYPE_VECTOR &&
                type->kind != AST_SEMA_TYPE_SVECTOR))
    return 0;
  if (op == AST_TOK_PLUS || op == AST_TOK_MINUS || op == AST_TOK_STAR ||
      op == AST_TOK_SLASH)
    return sema_type_is_vector_numeric(type);
  if (op == AST_TOK_PERCENT || op == AST_TOK_AMP || op == AST_TOK_PIPE ||
      op == AST_TOK_CARET || op == AST_TOK_SHIFT_LEFT ||
      op == AST_TOK_SHIFT_RIGHT)
    return sema_type_is_vector_integer(type);
  return 0;
}

static AstExprInfo constant_comparison(AstSemaFnCtx *fn, const AstExpr *expr,
                                       AstExprInfo left, AstExprInfo right) {
  AstExprInfo out = expr_info(sema_builtin_bool(fn->sema));
  if (sema_const_is_valid(left.constant) &&
      sema_const_is_valid(right.constant)) {
    AstSemaConstCtx cc = {0};
    cc.sema = fn->sema;
    cc.module = fn->module;
    cc.fn = fn;
    out.constant = sema_eval_const_expr(&cc, expr, out.type);
  }
  return out;
}

static AstExprInfo check_binary(AstSemaFnCtx *fn, const AstExpr *expr,
                                const AstSemaType *expected) {
  AstSema *s = fn->sema;
  AstTokenKind op = expr->as.binary.op;
  int arithmetic_like = op == AST_TOK_PLUS || op == AST_TOK_MINUS ||
                        op == AST_TOK_STAR || op == AST_TOK_SLASH ||
                        op == AST_TOK_PERCENT || op == AST_TOK_AMP ||
                        op == AST_TOK_PIPE || op == AST_TOK_CARET ||
                        op == AST_TOK_SHIFT_LEFT || op == AST_TOK_SHIFT_RIGHT;
  const AstSemaType *operand_expected =
      arithmetic_like && expected && sema_type_is_numeric(expected) ? expected
                                                                    : NULL;
  AstExprInfo a = sema_check_expr(fn, expr->as.binary.left, operand_expected);
  AstExprInfo b = sema_check_expr(fn, expr->as.binary.right, operand_expected);
  if ((op == AST_TOK_PLUS || op == AST_TOK_MINUS) &&
      sema_type_is_pointer(a.type) && sema_type_is_integer(b.type))
    return expr_info(a.type);
  if (op == AST_TOK_MINUS && sema_type_is_pointer(a.type) &&
      sema_type_equal(a.type, b.type))
    return expr_info(sema_builtin_for_token(s, AST_TOK_KW_ISIZE));
  if ((op == AST_TOK_SHIFT_LEFT || op == AST_TOK_SHIFT_RIGHT) &&
      sema_type_is_vector_integer(a.type) && sema_type_is_integer(b.type)) {
    return expr_info(a.type);
  }
  if (op == AST_TOK_AMP_AMP || op == AST_TOK_PIPE_PIPE) {
    if (!sema_type_is_bool(a.type) || !sema_type_is_bool(b.type)) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR, expr->span,
                fn->module->file.id, "logical operators require bool operands");
    }
    return constant_comparison(fn, expr, a, b);
  }
  if (op == AST_TOK_EQ_EQ || op == AST_TOK_BANG_EQ) {
    const AstSemaType *common = coerce_binary_type(fn, a, b, expr->span);
    if (common && (common->kind == AST_SEMA_TYPE_VECTOR ||
                   common->kind == AST_SEMA_TYPE_SVECTOR))
      return expr_info(vector_comparison_result(s, common));
    if (common && (common->kind == AST_SEMA_TYPE_MASK ||
                   common->kind == AST_SEMA_TYPE_PRED))
      return expr_info(common);
    if (!common &&
        !(sema_type_is_pointer(a.type) && sema_type_equal(a.type, b.type))) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "equality operands must have the same compatible type");
    }
    return constant_comparison(fn, expr, a, b);
  }
  if (op == AST_TOK_LT || op == AST_TOK_LT_EQ || op == AST_TOK_GT ||
      op == AST_TOK_GT_EQ) {
    const AstSemaType *common = coerce_binary_type(fn, a, b, expr->span);
    if (common && (common->kind == AST_SEMA_TYPE_VECTOR ||
                   common->kind == AST_SEMA_TYPE_SVECTOR)) {
      if (!sema_type_is_vector_numeric(common)) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, AST_DIAG_ERROR,
                  expr->span, fn->module->file.id,
                  "ordered vector comparison requires numeric lane types");
        return expr_error(s);
      }
      return expr_info(vector_comparison_result(s, common));
    }
    if (!common ||
        (!sema_type_is_numeric(common) && !sema_type_is_pointer(common))) {
      sema_diag(
          s, AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, AST_DIAG_ERROR, expr->span,
          fn->module->file.id,
          "ordered comparison requires compatible numeric or pointer operands");
    }
    return constant_comparison(fn, expr, a, b);
  }
  const AstSemaType *common = NULL;
  if (arithmetic_like && expected && sema_type_is_numeric(expected) &&
      (a.type->kind == AST_SEMA_TYPE_UNTYPED_INT ||
       a.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT) &&
      (b.type->kind == AST_SEMA_TYPE_UNTYPED_INT ||
       b.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT)) {
    int a_ok = sema_type_assignable(s, expected, a, expr->as.binary.left->span,
                                    fn->module->file.id);
    int b_ok = sema_type_assignable(s, expected, b, expr->as.binary.right->span,
                                    fn->module->file.id);
    if (a_ok && b_ok)
      common = expected;
  }
  if (!common)
    common = coerce_binary_type(fn, a, b, expr->span);
  if (!common) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "binary operands have incompatible types");
    return expr_error(s);
  }
  if (common->kind == AST_SEMA_TYPE_MASK ||
      common->kind == AST_SEMA_TYPE_PRED) {
    if (op != AST_TOK_AMP && op != AST_TOK_PIPE && op != AST_TOK_CARET) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "mask and predicate values support only &, |, and ^ as binary "
                "arithmetic operators");
      return expr_error(s);
    }
    return expr_info(common);
  }
  if (common->kind == AST_SEMA_TYPE_VECTOR ||
      common->kind == AST_SEMA_TYPE_SVECTOR) {
    if (!vector_binary_operator_valid(common, op)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "operator is not valid for this semantic vector lane type");
      return expr_error(s);
    }
    return expr_info(common);
  }
  if (op == AST_TOK_AMP || op == AST_TOK_PIPE || op == AST_TOK_CARET ||
      op == AST_TOK_SHIFT_LEFT || op == AST_TOK_SHIFT_RIGHT) {
    if (!sema_type_is_integer(common)) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "bitwise and shift operators require integer operands");
      return expr_error(s);
    }
  } else if (!sema_type_is_numeric(common)) {
    sema_diag(s, AST_SEMA_DIAG_EXPECTED_NUMERIC, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "arithmetic operators require numeric operands");
    return expr_error(s);
  } else if (op == AST_TOK_PERCENT && !sema_type_is_integer(common)) {
    sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "remainder operator requires integer operands");
    return expr_error(s);
  }
  AstExprInfo out = expr_info(common);
  if ((op == AST_TOK_SLASH || op == AST_TOK_PERCENT) &&
      sema_type_is_integer(common) && sema_const_is_valid(b.constant) &&
      sema_const_is_zero(b.constant)) {
    sema_diag(s, AST_SEMA_DIAG_DIVISION_BY_ZERO, AST_DIAG_ERROR,
              expr->as.binary.right->span, fn->module->file.id,
              "division or remainder by a compile-time zero divisor");
  }
  if (op == AST_TOK_SLASH && common->kind == AST_SEMA_TYPE_INT &&
      (common->flags & AST_SEMA_TYPEF_SIGNED) != 0u &&
      const_is_signed_minimum(a.constant, common) &&
      const_is_negative_one(b.constant)) {
    sema_diag(
        s, AST_SEMA_DIAG_INTEGER_DIVISION_OVERFLOW, AST_DIAG_ERROR, expr->span,
        fn->module->file.id,
        "signed division of the minimum value by -1 is not representable");
  }
  if (sema_const_is_valid(a.constant) && sema_const_is_valid(b.constant)) {
    AstSemaConstCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.sema = s;
    cc.module = fn->module;
    cc.fn = fn;
    out.constant = sema_eval_const_expr(&cc, expr, common);
  }
  return out;
}

static const AstSemaType *coerce_intrinsic_pair(AstSemaFnCtx *fn,
                                                const AstExpr *expr,
                                                const AstSemaType *expected,
                                                AstExprInfo *a_out,
                                                AstExprInfo *b_out) {
  AstSema *s = fn->sema;
  const AstSemaType *arg_expected =
      expected && sema_type_is_numeric(expected) ? expected : NULL;
  AstExprInfo a =
      sema_check_expr(fn, expr->as.call.args.items[0], arg_expected);
  AstExprInfo b =
      sema_check_expr(fn, expr->as.call.args.items[1], arg_expected);
  const AstSemaType *common = NULL;
  if (arg_expected &&
      (a.type->kind == AST_SEMA_TYPE_UNTYPED_INT ||
       a.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT) &&
      (b.type->kind == AST_SEMA_TYPE_UNTYPED_INT ||
       b.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT)) {
    int a_ok = sema_type_assignable(s, arg_expected, a,
                                    expr->as.call.args.items[0]->span,
                                    fn->module->file.id);
    int b_ok = sema_type_assignable(s, arg_expected, b,
                                    expr->as.call.args.items[1]->span,
                                    fn->module->file.id);
    if (a_ok && b_ok)
      common = arg_expected;
  }
  if (!common)
    common = coerce_binary_type(fn, a, b, expr->span);
  if (a_out)
    *a_out = a;
  if (b_out)
    *b_out = b;
  return common;
}

static AstExprInfo check_arithmetic_mode_call(AstSemaFnCtx *fn,
                                              const AstExpr *expr,
                                              AstName operation, AstName mode,
                                              const AstSemaType *expected,
                                              int *handled) {
  AstSema *s = fn->sema;
  int is_add = name_text(operation, "add");
  int is_sub = name_text(operation, "sub");
  int is_mul = name_text(operation, "mul");
  int is_checked = name_text(mode, "checked");
  int is_wrap = name_text(mode, "wrap");
  int is_sat = name_text(mode, "sat");
  if ((!is_add && !is_sub && !is_mul) || (!is_checked && !is_wrap && !is_sat)) {
    *handled = 0;
    return expr_error(s);
  }
  *handled = 1;
  if (is_sat && is_mul) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "mul.sat is not a Core semantic primitive");
    return expr_error(s);
  }
  if (expr->as.call.args.count != 2u) {
    sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
              fn->module->file.id, "%.*s.%.*s expects exactly two operands",
              (int)operation.span.length, operation.text, (int)mode.span.length,
              mode.text);
    for (uint32_t i = 0u; i < expr->as.call.args.count; ++i)
      sema_check_expr(fn, expr->as.call.args.items[i], NULL);
    return expr_error(s);
  }

  const AstSemaType *value_expected = expected;
  if (is_checked && expected && expected->kind == AST_SEMA_TYPE_TUPLE &&
      expected->as.tuple.count == 2u &&
      sema_type_is_bool(expected->as.tuple.items[1]))
    value_expected = expected->as.tuple.items[0];
  AstExprInfo a, b;
  const AstSemaType *common =
      coerce_intrinsic_pair(fn, expr, value_expected, &a, &b);
  (void)a;
  (void)b;
  if (!common || !sema_type_is_integer(common)) {
    sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "%.*s.%.*s requires two compatible integer operands",
              (int)operation.span.length, operation.text, (int)mode.span.length,
              mode.text);
    return expr_error(s);
  }
  if (!is_checked)
    return expr_info(common);
  const AstSemaType *items[2] = {common, sema_builtin_bool(s)};
  return expr_info(sema_make_tuple(s, items, 2u));
}

static AstExprInfo check_member_intrinsic_call(AstSemaFnCtx *fn,
                                               const AstExpr *expr,
                                               const AstSemaType *expected,
                                               int *handled) {
  *handled = 0;
  if (!expr || !expr->as.call.callee ||
      expr->as.call.callee->kind != AST_EXPR_MEMBER)
    return expr_error(fn->sema);
  const AstExpr *member = expr->as.call.callee;
  if (!member->as.member.base || member->as.member.base->kind != AST_EXPR_NAME)
    return expr_error(fn->sema);
  return check_arithmetic_mode_call(
      fn, expr, member->as.member.base->as.name.name, member->as.member.member,
      expected, handled);
}

static AstExprInfo check_intrinsic_call(AstSemaFnCtx *fn, const AstExpr *expr,
                                        AstName callee,
                                        const AstSemaType *expected,
                                        int *handled) {
  AstSema *s = fn->sema;
  AstExprArray args = expr->as.call.args;
  *handled = 1;
  if (name_text(callee, "feature")) {
    AstExprInfo out = expr_info(sema_builtin_bool(s));
    if (args.count != 1u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id, "feature() expects one string argument");
    } else if (!args.items[0] || args.items[0]->kind != AST_EXPR_STRING) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                args.items[0] ? args.items[0]->span : expr->span,
                fn->module->file.id,
                "feature() requires a string literal feature name");
    } else {
      AstSemaConstCtx cc;
      memset(&cc, 0, sizeof(cc));
      cc.sema = s;
      cc.module = fn->module;
      cc.fn = fn;
      out.constant = sema_eval_const_expr(&cc, expr, out.type);
    }
    return out;
  }
  if (name_text(callee, "sizeof")) {
    AstExprInfo out = expr_info(sema_builtin_usize(s));
    if (args.count != 1u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "sizeof() expects one type or value argument");
      return out;
    }
    int is_type_name = 0;
    const AstSemaType *type = sema_resolve_type_operand(
        s, fn->module, fn, args.items[0], &is_type_name);
    if (!type && args.items[0] && args.items[0]->kind != AST_EXPR_TYPE)
      type = sema_check_expr(fn, args.items[0], NULL).type;
    if (!type || type->kind == AST_SEMA_TYPE_ERROR ||
        !sema_type_is_value_type(type)) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                args.items[0] ? args.items[0]->span : expr->span,
                fn->module->file.id,
                "sizeof() requires a complete value type or a value with "
                "complete type");
    } else if (type->size == AST_SEMA_UNKNOWN_SIZE) {
      /* A generic-dependent layout becomes concrete only when the
       * function is specialized.  Keep the expression well-typed and
       * let specialized HIR fold sizeof to the concrete byte count. */
      if (!sema_type_is_dependent(type, 0u)) {
        sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                  args.items[0] ? args.items[0]->span : expr->span,
                  fn->module->file.id,
                  "sizeof() requires a complete value type or a value with "
                  "complete type");
      }
    } else {
      out.constant = sema_const_u64(type->size, out.type);
    }
    (void)is_type_name;
    return out;
  }
  if (name_text(callee, "offsetof")) {
    AstExprInfo out = expr_info(sema_builtin_usize(s));
    if (args.count != 2u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "offsetof() expects a struct type and a field name");
      return out;
    }
    int is_type_name = 0;
    const AstSemaType *type = sema_resolve_type_operand(
        s, fn->module, fn, args.items[0], &is_type_name);
    if (!type || !is_type_name || type->kind != AST_SEMA_TYPE_STRUCT) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                args.items[0]->span, fn->module->file.id,
                "offsetof() first argument must name a struct type");
      return out;
    }
    if (!args.items[1] || args.items[1]->kind != AST_EXPR_NAME) {
      sema_diag(s, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR,
                args.items[1] ? args.items[1]->span : expr->span,
                fn->module->file.id,
                "offsetof() second argument must be a field name");
      return out;
    }
    const AstSemaField *field =
        sema_find_struct_field(type, args.items[1]->as.name.name);
    if (!field) {
      sema_diag(s, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR,
                args.items[1]->span, fn->module->file.id,
                "offsetof() field '%.*s' does not exist in the struct",
                (int)args.items[1]->as.name.name.span.length,
                args.items[1]->as.name.name.text);
      return out;
    }
    out.constant = sema_const_u64(field->offset, out.type);
    return out;
  }
  if (name_text(callee, "symbol_addr")) {
    if (args.count != 1u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id, "symbol_addr() expects one symbol");
    } else {
      AstSemaSymbol *symbol = NULL;
      if (args.items[0] && args.items[0]->kind == AST_EXPR_NAME) {
        symbol =
            sema_module_find_value(fn->module, args.items[0]->as.name.name);
      } else if (args.items[0] && args.items[0]->kind == AST_EXPR_MEMBER) {
        symbol =
            sema_resolve_import_member(s, fn->module, args.items[0], NULL, 0);
      }
      if (!symbol || (symbol->kind != AST_SEMA_SYMBOL_GLOBAL &&
                      symbol->kind != AST_SEMA_SYMBOL_CONST &&
                      symbol->kind != AST_SEMA_SYMBOL_FUNCTION &&
                      symbol->kind != AST_SEMA_SYMBOL_EXTERN_FUNCTION)) {
        sema_diag(
            s, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR,
            args.items[0] ? args.items[0]->span : expr->span,
            fn->module->file.id,
            "symbol_addr() requires a global, constant, or function symbol");
      }
    }
    return expr_info(
        sema_make_pointer(s, AST_SEMA_TYPE_PTR, sema_builtin_void(s), 0u));
  }
  if (name_text(callee, "select")) {
    if (args.count != 3u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "select() expects condition, true value, and false value");
      for (uint32_t i = 0u; i < args.count; ++i)
        sema_check_expr(fn, args.items[i], NULL);
      return expr_error(s);
    }
    AstExprInfo condition = sema_check_expr(fn, args.items[0], NULL);
    int scalar_condition = sema_type_is_bool(condition.type);
    int vector_condition = condition.type->kind == AST_SEMA_TYPE_MASK ||
                           condition.type->kind == AST_SEMA_TYPE_PRED;
    if (!scalar_condition && !vector_condition) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR,
                args.items[0]->span, fn->module->file.id,
                "select() condition must be bool, mask<N>, or pred<T>");
    }
    AstExprInfo a = sema_check_expr(fn, args.items[1], expected);
    AstExprInfo b = sema_check_expr(fn, args.items[2], expected);
    const AstSemaType *common = NULL;
    if (expected &&
        sema_type_assignable(s, expected, a, args.items[1]->span,
                             fn->module->file.id) &&
        sema_type_assignable(s, expected, b, args.items[2]->span,
                             fn->module->file.id))
      common = expected;
    if (!common)
      common = coerce_binary_type(fn, a, b, expr->span);
    if (!common) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "select() value operands must have compatible types");
      return expr_error(s);
    }
    if (vector_condition) {
      if (condition.type->kind == AST_SEMA_TYPE_MASK) {
        if (common->kind != AST_SEMA_TYPE_VECTOR ||
            common->as.vector.dependent || condition.type->as.mask.dependent ||
            common->as.vector.lanes != condition.type->as.mask.lanes) {
          sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
                    fn->module->file.id,
                    "mask select requires vec<N,T> operands with the same lane "
                    "count N");
          return expr_error(s);
        }
      } else if (common->kind != AST_SEMA_TYPE_SVECTOR ||
                 !sema_type_equal(condition.type->as.scalable.element,
                                  common->as.scalable.element)) {
        sema_diag(
            s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
            fn->module->file.id,
            "predicate select requires svec<T> operands matching pred<T>");
        return expr_error(s);
      }
    }
    return expr_info(common);
  }
  if (name_text(callee, "clz") || name_text(callee, "ctz") ||
      name_text(callee, "popcnt") || name_text(callee, "bswap") ||
      name_text(callee, "bitrev")) {
    if (args.count != 1u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id, "%.*s() expects one integer argument",
                (int)callee.span.length, callee.text);
      for (uint32_t i = 0u; i < args.count; ++i)
        sema_check_expr(fn, args.items[i], NULL);
      return expr_error(s);
    }
    AstExprInfo value = sema_check_expr(
        fn, args.items[0],
        expected && sema_type_is_integer(expected) ? expected : NULL);
    const AstSemaType *type = value.type;
    if (type->kind == AST_SEMA_TYPE_UNTYPED_INT && expected &&
        sema_type_is_integer(expected) &&
        sema_type_assignable(s, expected, value, args.items[0]->span,
                             fn->module->file.id))
      type = expected;
    if (!sema_type_is_integer(type) ||
        type->kind == AST_SEMA_TYPE_UNTYPED_INT) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR,
                args.items[0]->span, fn->module->file.id,
                "%.*s() requires a concrete-width integer argument",
                (int)callee.span.length, callee.text);
      return expr_error(s);
    }
    if (name_text(callee, "bswap") &&
        (type->bit_width == 0u || (type->bit_width & 7u) != 0u)) {
      sema_diag(
          s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, args.items[0]->span,
          fn->module->file.id,
          "bswap() requires an integer width that is a whole number of bytes");
      return expr_error(s);
    }
    return expr_info(type);
  }
  if (name_text(callee, "rol") || name_text(callee, "ror")) {
    if (args.count != 2u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id, "%.*s() expects value and rotation count",
                (int)callee.span.length, callee.text);
      for (uint32_t i = 0u; i < args.count; ++i)
        sema_check_expr(fn, args.items[i], NULL);
      return expr_error(s);
    }
    AstExprInfo value = sema_check_expr(
        fn, args.items[0],
        expected && sema_type_is_integer(expected) ? expected : NULL);
    AstExprInfo count =
        sema_check_expr(fn, args.items[1], sema_builtin_usize(s));
    const AstSemaType *type = value.type;
    if (type->kind == AST_SEMA_TYPE_UNTYPED_INT && expected &&
        sema_type_is_integer(expected) &&
        sema_type_assignable(s, expected, value, args.items[0]->span,
                             fn->module->file.id))
      type = expected;
    if (!sema_type_is_integer(type) || type->kind == AST_SEMA_TYPE_UNTYPED_INT)
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR,
                args.items[0]->span, fn->module->file.id,
                "%.*s() requires a concrete-width integer value",
                (int)callee.span.length, callee.text);
    if (!sema_type_is_integer(count.type))
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR,
                args.items[1]->span, fn->module->file.id,
                "%.*s() rotation count must be an integer",
                (int)callee.span.length, callee.text);
    return sema_type_is_integer(type) && type->kind != AST_SEMA_TYPE_UNTYPED_INT
               ? expr_info(type)
               : expr_error(s);
  }
  if (name_text(callee, "ptr_to_int")) {
    if (args.count != 1u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "ptr_to_int() expects one pointer or capability argument");
      for (uint32_t i = 0u; i < args.count; ++i)
        sema_check_expr(fn, args.items[i], NULL);
      return expr_error(s);
    }
    AstExprInfo value = sema_check_expr(fn, args.items[0], NULL);
    if (!sema_type_is_pointer(value.type))
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_POINTER, AST_DIAG_ERROR,
                args.items[0]->span, fn->module->file.id,
                "ptr_to_int() requires ptr<T> or cap<T>");
    if (fn->unsafe_depth == 0u)
      sema_diag(s, AST_SEMA_DIAG_INVALID_CAST, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "ptr_to_int() is allowed only inside unsafe code");
    return expr_info(sema_builtin_usize(s));
  }
  if (name_text(callee, "int_to_ptr")) {
    if (args.count != 1u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "int_to_ptr() expects one integer address");
      for (uint32_t i = 0u; i < args.count; ++i)
        sema_check_expr(fn, args.items[i], NULL);
      return expr_error(s);
    }
    AstExprInfo value =
        sema_check_expr(fn, args.items[0], sema_builtin_usize(s));
    if (!sema_type_is_integer(value.type))
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR,
                args.items[0]->span, fn->module->file.id,
                "int_to_ptr() requires an integer address");
    if (fn->unsafe_depth == 0u)
      sema_diag(s, AST_SEMA_DIAG_INVALID_CAST, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "int_to_ptr() is allowed only inside unsafe code");
    if (!expected || !sema_type_is_pointer(expected)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_CAST, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "int_to_ptr() requires a ptr<T> or cap<T> destination type "
                "from context");
      return expr_error(s);
    }
    return expr_info(expected);
  }
  if (name_text(callee, "addr")) {
    if (args.count < 1u || args.count > 4u) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "addr() expects between one and four arguments");
      return expr_info(
          sema_make_pointer(s, AST_SEMA_TYPE_PTR, sema_builtin_void(s), 0u));
    }
    AstExprInfo base = sema_check_expr(fn, args.items[0], NULL);
    if (!sema_type_is_pointer(base.type)) {
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_POINTER, AST_DIAG_ERROR,
                args.items[0]->span, fn->module->file.id,
                "addr() base must be a pointer or capability");
    }
    for (uint32_t i = 1u; i < args.count; ++i) {
      AstExprInfo part = sema_check_expr(fn, args.items[i], NULL);
      if (!sema_type_is_integer(part.type)) {
        sema_diag(
            s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR,
            args.items[i]->span, fn->module->file.id,
            "addr() index, scale, and displacement arguments must be integers");
      }
    }
    if (sema_type_is_pointer(base.type))
      return expr_info(base.type);
    return expr_info(
        sema_make_pointer(s, AST_SEMA_TYPE_PTR, sema_builtin_void(s), 0u));
  }
  *handled = 0;
  return expr_error(s);
}

static AstSemaSymbol *direct_callee_symbol(AstSemaFnCtx *fn,
                                           const AstExpr *callee) {
  if (!fn || !callee)
    return NULL;
  if (callee->kind == AST_EXPR_NAME) {
    if (sema_fn_find_local(fn, callee->as.name.name))
      return NULL;
    return sema_module_find_value(fn->module, callee->as.name.name);
  }
  if (callee->kind == AST_EXPR_MEMBER) {
    return sema_resolve_import_member(fn->sema, fn->module, callee, NULL, 0);
  }
  return NULL;
}

static size_t generic_type_parameter_index(const AstSemaSymbol *symbol,
                                           const AstSemaType *pattern) {
  if (!symbol || !pattern || pattern->kind != AST_SEMA_TYPE_TYPE_PARAMETER)
    return SIZE_MAX;
  for (size_t i = 0u; i < symbol->generic_count; ++i) {
    if (!symbol->generic_bindings[i].is_const &&
        symbol->generic_bindings[i].type_value == pattern)
      return i;
  }
  return SIZE_MAX;
}

static size_t generic_const_parameter_index(const AstSemaSymbol *symbol,
                                            AstName parameter) {
  if (!symbol)
    return SIZE_MAX;
  for (size_t i = 0u; i < symbol->generic_count; ++i) {
    if (symbol->generic_bindings[i].is_const &&
        sema_name_eq(symbol->generic_bindings[i].name, parameter))
      return i;
  }
  return SIZE_MAX;
}

static int bind_inferred_type(AstSemaGenericBinding *values, size_t index,
                              const AstSemaType *actual) {
  if (!actual || actual->kind == AST_SEMA_TYPE_ERROR ||
      actual->kind == AST_SEMA_TYPE_UNTYPED_INT ||
      actual->kind == AST_SEMA_TYPE_UNTYPED_FLOAT)
    return 1;
  if (!values[index].type_value) {
    values[index].type_value = actual;
    return 1;
  }
  return sema_type_equal(values[index].type_value, actual);
}

static int bind_inferred_const(AstSemaFnCtx *fn, const AstSemaSymbol *symbol,
                               AstSemaGenericBinding *values, size_t index,
                               uint64_t concrete, AstName dependent,
                               int is_dependent, AstSpan span) {
  AstSema *s = fn->sema;
  if (is_dependent) {
    if (sema_const_is_valid(values[index].const_value))
      return 0;
    if (values[index].has_dependent_const)
      return sema_name_eq(values[index].dependent_const, dependent);
    values[index].dependent_const = dependent;
    values[index].has_dependent_const = 1u;
    return 1;
  }
  if (values[index].has_dependent_const)
    return 0;
  const AstSemaType *parameter_type =
      symbol->generic_bindings[index].const_value.type;
  AstSemaConstValue value = sema_const_u64(
      concrete, parameter_type ? parameter_type : sema_builtin_usize(s));
  if (sema_const_is_valid(values[index].const_value)) {
    return values[index].const_value.lo == value.lo &&
           values[index].const_value.hi == value.hi &&
           ((values[index].const_value.flags ^ value.flags) &
            AST_SEMA_CONSTF_NEGATIVE) == 0u;
  }
  if (parameter_type) {
    AstExprInfo info = expr_info(value.type);
    info.constant = value;
    if (!sema_type_assignable(s, parameter_type, info, span,
                              fn->module->file.id))
      return 0;
  }
  values[index].const_value = value;
  return 1;
}

static int eval_inference_u64(AstSemaFnCtx *fn, AstSemaGenericBinding *values,
                              size_t value_count, const AstExpr *expr,
                              uint64_t *out) {
  AstSemaConstCtx cc;
  AstSemaFnCtx fake;
  memset(&cc, 0, sizeof(cc));
  memset(&fake, 0, sizeof(fake));
  fake.sema = fn->sema;
  fake.module = fn->module;
  fake.generics = values;
  fake.generic_count = value_count;
  cc.sema = fn->sema;
  cc.module = fn->module;
  cc.fn = &fake;
  AstSemaConstValue value =
      sema_eval_const_expr(&cc, expr, sema_builtin_usize(fn->sema));
  return sema_const_to_u64(value, out);
}

static int infer_const_expr_target(AstSemaFnCtx *fn,
                                   const AstSemaSymbol *symbol,
                                   AstSemaGenericBinding *values,
                                   const AstExpr *expr, uint64_t target,
                                   AstSpan span, uint32_t depth) {
  if (!expr || depth > fn->sema->config.recursion_limit)
    return 0;
  uint64_t known = 0u;
  if (eval_inference_u64(fn, values, symbol->generic_count, expr, &known))
    return known == target;

  if (expr->kind == AST_EXPR_NAME) {
    size_t index = generic_const_parameter_index(symbol, expr->as.name.name);
    if (index == SIZE_MAX)
      return 0;
    return bind_inferred_const(fn, symbol, values, index, target, (AstName){0},
                               0, span);
  }
  if (expr->kind == AST_EXPR_UNARY && expr->as.unary.op == AST_TOK_PLUS) {
    return infer_const_expr_target(fn, symbol, values, expr->as.unary.operand,
                                   target, span, depth + 1u);
  }
  if (expr->kind == AST_EXPR_CONDITIONAL) {
    uint64_t condition = 0u;
    if (!eval_inference_u64(fn, values, symbol->generic_count,
                            expr->as.conditional.condition, &condition))
      return 0;
    return infer_const_expr_target(fn, symbol, values,
                                   condition ? expr->as.conditional.then_expr
                                             : expr->as.conditional.else_expr,
                                   target, span, depth + 1u);
  }
  if (expr->kind != AST_EXPR_BINARY)
    return 0;

  uint64_t left = 0u, right = 0u;
  int left_known = eval_inference_u64(fn, values, symbol->generic_count,
                                      expr->as.binary.left, &left);
  int right_known = eval_inference_u64(fn, values, symbol->generic_count,
                                       expr->as.binary.right, &right);
  switch (expr->as.binary.op) {
  case AST_TOK_PLUS:
    if (left_known && target >= left) {
      return infer_const_expr_target(fn, symbol, values, expr->as.binary.right,
                                     target - left, span, depth + 1u);
    }
    if (right_known && target >= right) {
      return infer_const_expr_target(fn, symbol, values, expr->as.binary.left,
                                     target - right, span, depth + 1u);
    }
    return 0;
  case AST_TOK_MINUS:
    if (right_known && target <= UINT64_MAX - right) {
      return infer_const_expr_target(fn, symbol, values, expr->as.binary.left,
                                     target + right, span, depth + 1u);
    }
    if (left_known && left >= target) {
      return infer_const_expr_target(fn, symbol, values, expr->as.binary.right,
                                     left - target, span, depth + 1u);
    }
    return 0;
  case AST_TOK_STAR:
    if (left_known && left != 0u && target % left == 0u) {
      return infer_const_expr_target(fn, symbol, values, expr->as.binary.right,
                                     target / left, span, depth + 1u);
    }
    if (right_known && right != 0u && target % right == 0u) {
      return infer_const_expr_target(fn, symbol, values, expr->as.binary.left,
                                     target / right, span, depth + 1u);
    }
    return 0;
  case AST_TOK_SHIFT_LEFT:
    if (right_known && right < 64u) {
      uint64_t mask = right == 0u ? 0u : (UINT64_C(1) << right) - 1u;
      if ((target & mask) != 0u)
        return 0;
      return infer_const_expr_target(fn, symbol, values, expr->as.binary.left,
                                     target >> right, span, depth + 1u);
    }
    return 0;
  default:
    return 0;
  }
}

static int infer_generic_type(AstSemaFnCtx *fn, const AstSemaSymbol *symbol,
                              AstSemaGenericBinding *values,
                              const AstSemaType *pattern,
                              const AstSemaType *actual, AstSpan span,
                              uint32_t depth) {
  if (!pattern || !actual)
    return 0;
  if (depth > fn->sema->config.recursion_limit)
    return 0;
  if (pattern->kind == AST_SEMA_TYPE_ERROR ||
      actual->kind == AST_SEMA_TYPE_ERROR)
    return 1;
  if (pattern->kind == AST_SEMA_TYPE_TYPE_PARAMETER) {
    size_t index = generic_type_parameter_index(symbol, pattern);
    return index == SIZE_MAX ? sema_type_equal(pattern, actual)
                             : bind_inferred_type(values, index, actual);
  }
  if (actual->kind == AST_SEMA_TYPE_UNTYPED_INT ||
      actual->kind == AST_SEMA_TYPE_UNTYPED_FLOAT)
    return 1;
  if (pattern->kind != actual->kind)
    return 0;
  switch (pattern->kind) {
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
    return infer_generic_type(fn, symbol, values, pattern->as.pointer.element,
                              actual->as.pointer.element, span, depth + 1u);
  case AST_SEMA_TYPE_BITS:
    if (pattern->as.bits.dependent) {
      if (pattern->as.bits.dependent_expr) {
        if (actual->as.bits.dependent)
          return sema_type_equal(pattern, actual);
        return infer_const_expr_target(fn, symbol, values,
                                       pattern->as.bits.dependent_expr,
                                       actual->as.bits.width, span, depth + 1u);
      }
      size_t index =
          generic_const_parameter_index(symbol, pattern->as.bits.parameter);
      if (index == SIZE_MAX)
        return sema_type_equal(pattern, actual);
      if (actual->as.bits.dependent) {
        return bind_inferred_const(fn, symbol, values, index, 0u,
                                   actual->as.bits.parameter, 1, span);
      }
      return bind_inferred_const(fn, symbol, values, index,
                                 actual->as.bits.width, (AstName){0}, 0, span);
    }
    return !actual->as.bits.dependent &&
           pattern->as.bits.width == actual->as.bits.width;
  case AST_SEMA_TYPE_ARRAY: {
    if (!infer_generic_type(fn, symbol, values, pattern->as.array.element,
                            actual->as.array.element, span, depth + 1u))
      return 0;
    if (pattern->as.array.dependent) {
      if (pattern->as.array.dependent_expr) {
        if (actual->as.array.dependent)
          return sema_type_equal(pattern, actual);
        return infer_const_expr_target(
            fn, symbol, values, pattern->as.array.dependent_expr,
            actual->as.array.count, span, depth + 1u);
      }
      size_t index =
          generic_const_parameter_index(symbol, pattern->as.array.parameter);
      if (index == SIZE_MAX)
        return sema_type_equal(pattern, actual);
      if (actual->as.array.dependent) {
        return bind_inferred_const(fn, symbol, values, index, 0u,
                                   actual->as.array.parameter, 1, span);
      }
      return bind_inferred_const(fn, symbol, values, index,
                                 actual->as.array.count, (AstName){0}, 0, span);
    }
    return !actual->as.array.dependent &&
           pattern->as.array.count == actual->as.array.count;
  }
  case AST_SEMA_TYPE_VECTOR: {
    if (!infer_generic_type(fn, symbol, values, pattern->as.vector.element,
                            actual->as.vector.element, span, depth + 1u))
      return 0;
    if (pattern->as.vector.dependent) {
      if (pattern->as.vector.dependent_expr) {
        if (actual->as.vector.dependent)
          return sema_type_equal(pattern, actual);
        return infer_const_expr_target(
            fn, symbol, values, pattern->as.vector.dependent_expr,
            actual->as.vector.lanes, span, depth + 1u);
      }
      size_t index =
          generic_const_parameter_index(symbol, pattern->as.vector.parameter);
      if (index == SIZE_MAX)
        return sema_type_equal(pattern, actual);
      if (actual->as.vector.dependent) {
        return bind_inferred_const(fn, symbol, values, index, 0u,
                                   actual->as.vector.parameter, 1, span);
      }
      return bind_inferred_const(fn, symbol, values, index,
                                 actual->as.vector.lanes, (AstName){0}, 0,
                                 span);
    }
    return !actual->as.vector.dependent &&
           pattern->as.vector.lanes == actual->as.vector.lanes;
  }
  case AST_SEMA_TYPE_MASK:
    if (pattern->as.mask.dependent) {
      if (pattern->as.mask.dependent_expr) {
        if (actual->as.mask.dependent)
          return sema_type_equal(pattern, actual);
        return infer_const_expr_target(fn, symbol, values,
                                       pattern->as.mask.dependent_expr,
                                       actual->as.mask.lanes, span, depth + 1u);
      }
      size_t index =
          generic_const_parameter_index(symbol, pattern->as.mask.parameter);
      if (index == SIZE_MAX)
        return sema_type_equal(pattern, actual);
      if (actual->as.mask.dependent) {
        return bind_inferred_const(fn, symbol, values, index, 0u,
                                   actual->as.mask.parameter, 1, span);
      }
      return bind_inferred_const(fn, symbol, values, index,
                                 actual->as.mask.lanes, (AstName){0}, 0, span);
    }
    return !actual->as.mask.dependent &&
           pattern->as.mask.lanes == actual->as.mask.lanes;
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED:
    return infer_generic_type(fn, symbol, values, pattern->as.scalable.element,
                              actual->as.scalable.element, span, depth + 1u);
  case AST_SEMA_TYPE_TUPLE:
    if (pattern->as.tuple.count != actual->as.tuple.count)
      return 0;
    for (uint32_t i = 0u; i < pattern->as.tuple.count; ++i) {
      if (!infer_generic_type(fn, symbol, values, pattern->as.tuple.items[i],
                              actual->as.tuple.items[i], span, depth + 1u))
        return 0;
    }
    return 1;
  case AST_SEMA_TYPE_STRUCT:
    if (pattern->as.record.decl != actual->as.record.decl)
      return 0;
    if (pattern->as.record.generic_arg_count !=
        actual->as.record.generic_arg_count)
      return 0;
    for (uint32_t i = 0u; i < pattern->as.record.generic_arg_count; ++i) {
      const AstSemaType *pattern_type_arg =
          pattern->as.record.generic_type_args
              ? pattern->as.record.generic_type_args[i]
              : NULL;
      const AstSemaType *actual_type_arg =
          actual->as.record.generic_type_args
              ? actual->as.record.generic_type_args[i]
              : NULL;
      if (pattern_type_arg || actual_type_arg) {
        if (!pattern_type_arg || !actual_type_arg ||
            !infer_generic_type(fn, symbol, values, pattern_type_arg,
                                actual_type_arg, span, depth + 1u))
          return 0;
        continue;
      }
      uint8_t pattern_dependent =
          pattern->as.record.generic_const_dependent
              ? pattern->as.record.generic_const_dependent[i]
              : 0u;
      uint8_t actual_dependent =
          actual->as.record.generic_const_dependent
              ? actual->as.record.generic_const_dependent[i]
              : 0u;
      if (pattern_dependent && pattern->as.record.generic_const_parameters) {
        size_t index = generic_const_parameter_index(
            symbol, pattern->as.record.generic_const_parameters[i]);
        if (index == SIZE_MAX) {
          if (!actual_dependent ||
              !actual->as.record.generic_const_parameters ||
              !sema_name_eq(pattern->as.record.generic_const_parameters[i],
                            actual->as.record.generic_const_parameters[i]))
            return 0;
        } else if (actual_dependent) {
          if (!actual->as.record.generic_const_parameters ||
              !bind_inferred_const(
                  fn, symbol, values, index, 0u,
                  actual->as.record.generic_const_parameters[i], 1, span))
            return 0;
        } else {
          uint64_t concrete = 0u;
          if (!sema_const_to_u64(actual->as.record.generic_const_args[i],
                                 &concrete) ||
              !bind_inferred_const(fn, symbol, values, index, concrete,
                                   (AstName){0}, 0, span))
            return 0;
        }
      } else {
        if (actual_dependent)
          return 0;
        AstSemaConstValue a = pattern->as.record.generic_const_args[i];
        AstSemaConstValue b = actual->as.record.generic_const_args[i];
        if (!sema_const_is_valid(a) || !sema_const_is_valid(b) ||
            a.lo != b.lo || a.hi != b.hi)
          return 0;
      }
    }
    return 1;
  case AST_SEMA_TYPE_FUNCTION:
    if (pattern->as.function.param_count != actual->as.function.param_count)
      return 0;
    for (uint32_t i = 0u; i < pattern->as.function.param_count; ++i) {
      if (!infer_generic_type(fn, symbol, values,
                              pattern->as.function.params[i],
                              actual->as.function.params[i], span, depth + 1u))
        return 0;
    }
    return infer_generic_type(fn, symbol, values, pattern->as.function.result,
                              actual->as.function.result, span, depth + 1u);
  default:
    return sema_type_equal(pattern, actual);
  }
}

int sema_infer_generic_signature_bindings(AstSema *s, AstSemaModule *module,
                                          const AstSemaSymbol *symbol,
                                          const AstSemaType *concrete_signature,
                                          AstSemaGenericBinding *values,
                                          size_t value_count, AstSpan span) {
  if (!s || !module || !symbol || !concrete_signature || !values ||
      symbol->generic_count == 0u || value_count != symbol->generic_count ||
      !symbol->generic_bindings || !symbol->type ||
      symbol->type->kind != AST_SEMA_TYPE_FUNCTION ||
      concrete_signature->kind != AST_SEMA_TYPE_FUNCTION)
    return 0;
  memset(values, 0, value_count * sizeof(*values));
  for (size_t i = 0u; i < value_count; ++i) {
    values[i].name = symbol->generic_bindings[i].name;
    values[i].is_const = symbol->generic_bindings[i].is_const;
    if (values[i].is_const)
      values[i].const_value.type = symbol->generic_bindings[i].const_value.type;
  }
  AstSemaFnCtx fake;
  memset(&fake, 0, sizeof(fake));
  fake.sema = s;
  fake.module = module;
  fake.generics = values;
  fake.generic_count = value_count;
  if (!infer_generic_type(&fake, symbol, values, symbol->type,
                          concrete_signature, span, 0u))
    return 0;
  for (size_t i = 0u; i < value_count; ++i) {
    if (values[i].is_const) {
      if (!sema_const_is_valid(values[i].const_value) &&
          !values[i].has_dependent_const)
        return 0;
    } else if (!values[i].type_value)
      return 0;
  }
  return 1;
}

static AstExprInfo check_generic_call(AstSemaFnCtx *fn, const AstExpr *expr,
                                      AstSemaSymbol *symbol,
                                      const AstSemaType *expected) {
  AstSema *s = fn->sema;
  const AstSemaType *signature = symbol->type;
  if (expr->as.call.callee) {
    AstExprInfo callee_info =
        expr_info(signature ? signature : sema_builtin_error(s));
    callee_info.declaration_span = symbol->name.span;
    callee_info.resolution_kind = AST_SEMA_RESOLUTION_TOP_LEVEL;
    (void)sema_record_expr_fact(s, expr->as.call.callee, callee_info);
  }
  if (!signature || signature->kind != AST_SEMA_TYPE_FUNCTION ||
      !symbol->generic_bindings || symbol->generic_count == 0u) {
    return expr_error(s);
  }
  AstSemaGenericBinding *values = (AstSemaGenericBinding *)sema_arena_alloc(
      s, symbol->generic_count * sizeof(*values),
      ASTER_ALIGNOF(AstSemaGenericBinding));
  if (!values)
    return expr_error(s);
  memset(values, 0, symbol->generic_count * sizeof(*values));
  for (size_t i = 0u; i < symbol->generic_count; ++i) {
    values[i].name = symbol->generic_bindings[i].name;
    values[i].is_const = symbol->generic_bindings[i].is_const;
    if (values[i].is_const) {
      values[i].const_value = sema_const_invalid();
      values[i].const_value.type = symbol->generic_bindings[i].const_value.type;
    }
  }

  int inference_ok = 1;
  if (expected && expected->kind != AST_SEMA_TYPE_ERROR &&
      expected->kind != AST_SEMA_TYPE_UNTYPED_INT &&
      expected->kind != AST_SEMA_TYPE_UNTYPED_FLOAT) {
    inference_ok =
        infer_generic_type(fn, symbol, values, signature->as.function.result,
                           expected, expr->span, 0u);
  }

  uint32_t expected_count = signature->as.function.param_count;
  if (expr->as.call.args.count != expected_count) {
    sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "generic function expects %u arguments but received %u",
              expected_count, expr->as.call.args.count);
  }
  uint32_t n = expr->as.call.args.count < expected_count
                   ? expr->as.call.args.count
                   : expected_count;
  AstExprInfo *arguments = NULL;
  if (n) {
    arguments = (AstExprInfo *)sema_arena_alloc(
        s, (size_t)n * sizeof(*arguments), ASTER_ALIGNOF(AstExprInfo));
    if (!arguments)
      return expr_error(s);
  }
  for (uint32_t i = 0u; i < n; ++i) {
    const AstSemaType *parameter_expected = sema_substitute_type(
        s, signature->as.function.params[i], symbol->generic_bindings, values,
        symbol->generic_count, expr->span, fn->module->file.id, 0u);
    if (sema_type_contains_parameter(parameter_expected,
                                     symbol->generic_bindings,
                                     symbol->generic_count))
      parameter_expected = NULL;
    arguments[i] =
        sema_check_expr(fn, expr->as.call.args.items[i], parameter_expected);
    if (!infer_generic_type(fn, symbol, values,
                            signature->as.function.params[i], arguments[i].type,
                            expr->as.call.args.items[i]->span, 0u))
      inference_ok = 0;
  }
  for (uint32_t i = n; i < expr->as.call.args.count; ++i)
    sema_check_expr(fn, expr->as.call.args.items[i], NULL);

  for (size_t i = 0u; i < symbol->generic_count; ++i) {
    if ((!values[i].is_const && !values[i].type_value) ||
        (values[i].is_const && !sema_const_is_valid(values[i].const_value) &&
         !values[i].has_dependent_const)) {
      inference_ok = 0;
    }
  }
  if (!inference_ok) {
    sema_diag(s, AST_SEMA_DIAG_GENERIC_INFERENCE, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "cannot infer a consistent set of generic arguments for '%.*s'",
              (int)symbol->name.span.length, symbol->name.text);
    return expr_error(s);
  }

  const AstSemaType **instantiated_params = NULL;
  if (expected_count) {
    instantiated_params = (const AstSemaType **)sema_arena_alloc(
        s, (size_t)expected_count * sizeof(*instantiated_params),
        ASTER_ALIGNOF(AstSemaType *));
    if (!instantiated_params)
      return expr_error(s);
  }
  for (uint32_t i = 0u; i < expected_count; ++i) {
    instantiated_params[i] = sema_substitute_type(
        s, signature->as.function.params[i], symbol->generic_bindings, values,
        symbol->generic_count, expr->span, fn->module->file.id, 0u);
  }
  for (uint32_t i = 0u; i < n; ++i) {
    const AstSemaType *parameter = instantiated_params[i];
    if (!sema_type_assignable(s, parameter, arguments[i],
                              expr->as.call.args.items[i]->span,
                              fn->module->file.id)) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARGUMENT_TYPE, AST_DIAG_ERROR,
                expr->as.call.args.items[i]->span, fn->module->file.id,
                "argument %u has an incompatible type after generic inference",
                i + 1u);
    } else {
      (void)sema_refine_expr_fact_type(s, expr->as.call.args.items[i],
                                       parameter);
    }
  }
  const AstSemaType *result = sema_substitute_type(
      s, signature->as.function.result, symbol->generic_bindings, values,
      symbol->generic_count, expr->span, fn->module->file.id, 0u);
  AstExprInfo out = expr_info(result);
  out.call_signature =
      sema_make_function(s, instantiated_params, expected_count, result);
  return out;
}

static AstExprInfo check_call(AstSemaFnCtx *fn, const AstExpr *expr,
                              const AstSemaType *expected) {
  AstSema *s = fn->sema;
  if (fn->is_leaf) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATTRIBUTE, AST_DIAG_ERROR, expr->span,
              fn->module->file.id, "@leaf function contains a call expression");
  }
  if (expr->as.call.callee && expr->as.call.callee->kind == AST_EXPR_MEMBER) {
    int handled = 0;
    AstExprInfo intrinsic =
        check_member_intrinsic_call(fn, expr, expected, &handled);
    if (handled)
      return intrinsic;
  }
  if (expr->as.call.callee && expr->as.call.callee->kind == AST_EXPR_NAME) {
    int handled = 0;
    AstExprInfo intrinsic = check_intrinsic_call(
        fn, expr, expr->as.call.callee->as.name.name, expected, &handled);
    if (handled)
      return intrinsic;
  }
  AstSemaSymbol *direct = direct_callee_symbol(fn, expr->as.call.callee);
  if (direct && direct->kind == AST_SEMA_SYMBOL_FUNCTION &&
      direct->generic_count != 0u) {
    return check_generic_call(fn, expr, direct, expected);
  }
  AstExprInfo callee = sema_check_expr(fn, expr->as.call.callee, NULL);
  if (!callee.type || callee.type->kind != AST_SEMA_TYPE_FUNCTION) {
    sema_diag(s, AST_SEMA_DIAG_CALL_NON_FUNCTION, AST_DIAG_ERROR, expr->span,
              fn->module->file.id, "called expression is not a function");
    for (uint32_t i = 0u; i < expr->as.call.args.count; ++i)
      sema_check_expr(fn, expr->as.call.args.items[i], NULL);
    return expr_error(s);
  }
  uint32_t expected_count = callee.type->as.function.param_count;
  if (expr->as.call.args.count != expected_count) {
    sema_diag(s, AST_SEMA_DIAG_CALL_ARITY, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "function expects %u arguments but received %u", expected_count,
              expr->as.call.args.count);
  }
  uint32_t n = expr->as.call.args.count < expected_count
                   ? expr->as.call.args.count
                   : expected_count;
  for (uint32_t i = 0u; i < n; ++i) {
    AstExprInfo ai = sema_check_expr(fn, expr->as.call.args.items[i],
                                     callee.type->as.function.params[i]);
    if (!sema_type_assignable(s, callee.type->as.function.params[i], ai,
                              expr->as.call.args.items[i]->span,
                              fn->module->file.id)) {
      sema_diag(s, AST_SEMA_DIAG_CALL_ARGUMENT_TYPE, AST_DIAG_ERROR,
                expr->as.call.args.items[i]->span, fn->module->file.id,
                "argument %u has an incompatible type", i + 1u);
    }
  }
  for (uint32_t i = n; i < expr->as.call.args.count; ++i)
    sema_check_expr(fn, expr->as.call.args.items[i], NULL);
  AstExprInfo out = expr_info(callee.type->as.function.result);
  out.call_signature = callee.type;
  return out;
}

static AstExprInfo check_member(AstSemaFnCtx *fn, const AstExpr *expr) {
  AstSemaSymbol *imported =
      sema_resolve_import_member(fn->sema, fn->module, expr, NULL, 0);
  if (imported) {
    AstExprInfo out = expr_info(imported->type ? imported->type
                                               : sema_builtin_error(fn->sema));
    out.declaration_span = imported->name.span;
    out.resolution_kind = AST_SEMA_RESOLUTION_TOP_LEVEL;
    /* Exported `const` values are compile-time symbols, not imported
     * storage.  Preserve their constant fact across a qualified import so
     * HIR emits the immediate value instead of a GLOBAL_LOAD relocation. */
    if (imported->kind == AST_SEMA_SYMBOL_CONST) {
      AstSemaConstCtx cc;
      memset(&cc, 0, sizeof(cc));
      cc.sema = fn->sema;
      cc.module = fn->module;
      cc.fn = fn;
      out.constant = sema_eval_const_expr(&cc, expr, imported->type);
    }
    return out;
  }
  AstExprInfo base = sema_check_expr(fn, expr->as.member.base, NULL);
  const AstSemaType *t = base.type;
  if (sema_type_is_pointer(t))
    t = t->as.pointer.element;
  if (!t || t->kind != AST_SEMA_TYPE_STRUCT) {
    sema_diag(fn->sema, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "member access requires a struct value or pointer to struct");
    return expr_error(fn->sema);
  }
  for (uint32_t i = 0u; i < t->as.record.field_count; ++i)
    if (sema_name_eq(t->as.record.fields[i].name, expr->as.member.member)) {
      AstExprInfo out = expr_info(t->as.record.fields[i].type);
      out.is_lvalue = base.is_lvalue || sema_type_is_pointer(base.type);
      out.is_mutable =
          base.is_mutable ||
          (sema_type_is_pointer(base.type) &&
           !(base.type->as.pointer.qualifiers & AST_TYPE_QUAL_CONST));
      out.declaration_span = t->as.record.fields[i].name.span;
      out.resolution_kind = AST_SEMA_RESOLUTION_FIELD;
      return out;
    }
  sema_diag(fn->sema, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR,
            expr->as.member.member.span, fn->module->file.id,
            "struct has no field '%.*s'",
            (int)expr->as.member.member.span.length,
            expr->as.member.member.text);
  return expr_error(fn->sema);
}

static AstExprInfo check_load(AstSemaFnCtx *fn, const AstExpr *expr) {
  AstSema *s = fn->sema;
  const AstSemaType *load_type =
      sema_resolve_type(s, fn->module, expr->as.load.type, fn, 0u);
  sema_validate_memory_attributes(fn, expr->as.load.attributes, 0);
  AstExprInfo addr = sema_check_expr(fn, expr->as.load.address, NULL);
  if (!sema_type_is_pointer(addr.type)) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_LOAD, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "load address must have pointer or capability type");
  } else if (addr.type->as.pointer.element &&
             addr.type->as.pointer.element->kind != AST_SEMA_TYPE_VOID &&
             !sema_type_equal(addr.type->as.pointer.element, load_type)) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_LOAD, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "typed load does not match the pointee type; use an explicit "
              "ptrcast for reinterpretation");
  }
  return expr_info(load_type);
}

typedef enum AstAtomicOrder {
  AST_ATOMIC_ORDER_NONE = 0,
  AST_ATOMIC_ORDER_RELAXED,
  AST_ATOMIC_ORDER_ACQUIRE,
  AST_ATOMIC_ORDER_RELEASE,
  AST_ATOMIC_ORDER_ACQ_REL,
  AST_ATOMIC_ORDER_SEQ_CST,
  AST_ATOMIC_ORDER_INVALID
} AstAtomicOrder;

static AstAtomicOrder atomic_order_from_name(AstName name) {
  if (sema_name_eq_text(name, "relaxed"))
    return AST_ATOMIC_ORDER_RELAXED;
  if (sema_name_eq_text(name, "acquire"))
    return AST_ATOMIC_ORDER_ACQUIRE;
  if (sema_name_eq_text(name, "release"))
    return AST_ATOMIC_ORDER_RELEASE;
  if (sema_name_eq_text(name, "acq_rel"))
    return AST_ATOMIC_ORDER_ACQ_REL;
  if (sema_name_eq_text(name, "seq_cst"))
    return AST_ATOMIC_ORDER_SEQ_CST;
  return AST_ATOMIC_ORDER_INVALID;
}

static const char *atomic_order_name(AstAtomicOrder order) {
  switch (order) {
  case AST_ATOMIC_ORDER_RELAXED:
    return "relaxed";
  case AST_ATOMIC_ORDER_ACQUIRE:
    return "acquire";
  case AST_ATOMIC_ORDER_RELEASE:
    return "release";
  case AST_ATOMIC_ORDER_ACQ_REL:
    return "acq_rel";
  case AST_ATOMIC_ORDER_SEQ_CST:
    return "seq_cst";
  case AST_ATOMIC_ORDER_NONE:
    return "none";
  default:
    return "invalid";
  }
}

static AstAtomicOrder atomic_order_from_attribute(const AstAttribute *attr) {
  if (!attr)
    return AST_ATOMIC_ORDER_INVALID;
  AstAtomicOrder direct = atomic_order_from_name(attr->name);
  if (direct != AST_ATOMIC_ORDER_INVALID)
    return attr->args.count == 0u ? direct : AST_ATOMIC_ORDER_INVALID;
  if ((sema_name_eq_text(attr->name, "order") ||
       sema_name_eq_text(attr->name, "success") ||
       sema_name_eq_text(attr->name, "failure")) &&
      attr->args.count == 1u && attr->args.items[0] &&
      attr->args.items[0]->kind == AST_EXPR_NAME) {
    return atomic_order_from_name(attr->args.items[0]->as.name.name);
  }
  return AST_ATOMIC_ORDER_INVALID;
}

static void atomic_collect_orders(AstSemaFnCtx *fn, const AstExpr *expr,
                                  AstAtomicOrder *order,
                                  AstAtomicOrder *success,
                                  AstAtomicOrder *failure) {
  *order = AST_ATOMIC_ORDER_NONE;
  *success = AST_ATOMIC_ORDER_NONE;
  *failure = AST_ATOMIC_ORDER_NONE;
  for (uint32_t i = 0u; i < expr->as.atomic.attributes.count; ++i) {
    const AstAttribute *attr = &expr->as.atomic.attributes.items[i];
    AstAtomicOrder parsed = atomic_order_from_attribute(attr);
    AstAtomicOrder *slot = NULL;
    if (sema_name_eq_text(attr->name, "success"))
      slot = success;
    else if (sema_name_eq_text(attr->name, "failure"))
      slot = failure;
    else if (sema_name_eq_text(attr->name, "order") ||
             atomic_order_from_name(attr->name) != AST_ATOMIC_ORDER_INVALID)
      slot = order;
    else
      continue;
    if (parsed == AST_ATOMIC_ORDER_INVALID) {
      sema_diag(fn->sema, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
                attr->span, fn->module->file.id,
                "invalid atomic memory-order attribute");
      continue;
    }
    if (*slot != AST_ATOMIC_ORDER_NONE) {
      sema_diag(fn->sema, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
                attr->span, fn->module->file.id,
                "atomic operation specifies the same memory-order role more "
                "than once");
    } else {
      *slot = parsed;
    }
  }
}

static int atomic_order_valid_for_load(AstAtomicOrder order) {
  return order == AST_ATOMIC_ORDER_RELAXED ||
         order == AST_ATOMIC_ORDER_ACQUIRE || order == AST_ATOMIC_ORDER_SEQ_CST;
}

static int atomic_order_valid_for_store(AstAtomicOrder order) {
  return order == AST_ATOMIC_ORDER_RELAXED ||
         order == AST_ATOMIC_ORDER_RELEASE || order == AST_ATOMIC_ORDER_SEQ_CST;
}

static int atomic_order_valid_for_rmw(AstAtomicOrder order) {
  return order >= AST_ATOMIC_ORDER_RELAXED && order <= AST_ATOMIC_ORDER_SEQ_CST;
}

static int atomic_failure_not_stronger(AstAtomicOrder success,
                                       AstAtomicOrder failure) {
  if (failure == AST_ATOMIC_ORDER_RELAXED)
    return 1;
  if (failure == AST_ATOMIC_ORDER_ACQUIRE) {
    return success == AST_ATOMIC_ORDER_ACQUIRE ||
           success == AST_ATOMIC_ORDER_ACQ_REL ||
           success == AST_ATOMIC_ORDER_SEQ_CST;
  }
  if (failure == AST_ATOMIC_ORDER_SEQ_CST)
    return success == AST_ATOMIC_ORDER_SEQ_CST;
  return 0;
}

static int atomic_type_supported(const AstSemaType *type) {
  if (!type)
    return 0;
  return sema_type_is_integer(type) || sema_type_is_pointer(type) ||
         type->kind == AST_SEMA_TYPE_BOOL;
}

static int atomic_rmw_name_supported(const char *op) {
  return strcmp(op, "rmw.add") == 0 || strcmp(op, "rmw.sub") == 0 ||
         strcmp(op, "rmw.and") == 0 || strcmp(op, "rmw.or") == 0 ||
         strcmp(op, "rmw.xor") == 0 || strcmp(op, "rmw.xchg") == 0;
}

static AstExprInfo check_atomic(AstSemaFnCtx *fn, const AstExpr *expr) {
  AstSema *s = fn->sema;
  AstName opn = expr->as.atomic.op;
  char opbuf[64];
  size_t oplen = opn.span.length < sizeof(opbuf) - 1u ? opn.span.length
                                                      : sizeof(opbuf) - 1u;
  memcpy(opbuf, opn.text, oplen);
  opbuf[oplen] = '\0';

  const AstSemaType *type =
      sema_resolve_type(s, fn->module, expr->as.atomic.type, fn, 0u);
  AstExprInfo addr = sema_check_expr(fn, expr->as.atomic.address, NULL);
  int is_load = strcmp(opbuf, "load") == 0;
  int is_store = strcmp(opbuf, "store") == 0;
  int is_cas = strcmp(opbuf, "cas") == 0;
  int is_rmw = strncmp(opbuf, "rmw.", 4u) == 0;

  if (!atomic_type_supported(type)) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_OPERATION, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "semantic atomics require an integer, pointer, capability, or "
              "bool value type");
  }
  if (!sema_type_is_pointer(addr.type) ||
      (sema_type_is_pointer(addr.type) &&
       !sema_type_equal(addr.type->as.pointer.element, type) &&
       addr.type->as.pointer.element->kind != AST_SEMA_TYPE_VOID)) {
    sema_diag(
        s, AST_SEMA_DIAG_INVALID_ATOMIC_OPERATION, AST_DIAG_ERROR, expr->span,
        fn->module->file.id,
        "atomic address must be a pointer or capability to the operation type");
  }
  if (!is_load && sema_type_is_pointer(addr.type) &&
      (addr.type->as.pointer.qualifiers & AST_TYPE_QUAL_CONST)) {
    sema_diag(
        s, AST_SEMA_DIAG_INVALID_ATOMIC_OPERATION, AST_DIAG_ERROR, expr->span,
        fn->module->file.id,
        "modifying atomic operation cannot use ptr<const T> or cap<const T>");
  }

  uint32_t wanted_args = UINT32_MAX;
  const AstSemaType *result = type;
  if (is_load)
    wanted_args = 0u;
  else if (is_store) {
    wanted_args = 1u;
    result = sema_builtin_void(s);
  } else if (is_cas) {
    wanted_args = 2u;
    result = sema_builtin_bool(s);
  } else if (is_rmw && atomic_rmw_name_supported(opbuf))
    wanted_args = 1u;
  else {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_OPERATION, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "unsupported semantic atomic operation '%s'", opbuf);
  }
  if (is_rmw && strcmp(opbuf, "rmw.xchg") != 0 && !sema_type_is_integer(type)) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_OPERATION, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "atomic.%s requires an integer value type", opbuf);
  }
  if (wanted_args != UINT32_MAX && expr->as.atomic.args.count != wanted_args) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_ARGUMENTS, AST_DIAG_ERROR,
              expr->span, fn->module->file.id,
              "atomic.%s expects %u value arguments but received %u", opbuf,
              wanted_args, expr->as.atomic.args.count);
  }
  for (uint32_t i = 0u; i < expr->as.atomic.args.count; ++i) {
    AstExprInfo ai = sema_check_expr(fn, expr->as.atomic.args.items[i], type);
    if (!sema_type_assignable(s, type, ai, expr->as.atomic.args.items[i]->span,
                              fn->module->file.id)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_ARGUMENTS, AST_DIAG_ERROR,
                expr->as.atomic.args.items[i]->span, fn->module->file.id,
                "atomic value argument has an incompatible type");
    }
  }

  AstAtomicOrder order, success, failure;
  atomic_collect_orders(fn, expr, &order, &success, &failure);
  if (is_cas) {
    if (order != AST_ATOMIC_ORDER_NONE && success == AST_ATOMIC_ORDER_NONE &&
        failure == AST_ATOMIC_ORDER_NONE) {
      success = order;
      if (order == AST_ATOMIC_ORDER_RELEASE)
        failure = AST_ATOMIC_ORDER_RELAXED;
      else if (order == AST_ATOMIC_ORDER_ACQ_REL)
        failure = AST_ATOMIC_ORDER_ACQUIRE;
      else
        failure = order;
    } else if (order != AST_ATOMIC_ORDER_NONE) {
      sema_diag(
          s, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR, expr->span,
          fn->module->file.id,
          "atomic.cas cannot mix @order/direct order with @success/@failure");
    }
    if (success == AST_ATOMIC_ORDER_NONE || failure == AST_ATOMIC_ORDER_NONE) {
      sema_diag(
          s, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR, expr->span,
          fn->module->file.id,
          "atomic.cas requires explicit success and failure memory orders");
    } else if (!atomic_order_valid_for_rmw(success) ||
               !atomic_failure_not_stronger(success, failure)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "invalid atomic.cas order pair: success=%s failure=%s",
                atomic_order_name(success), atomic_order_name(failure));
    }
  } else {
    if (success != AST_ATOMIC_ORDER_NONE || failure != AST_ATOMIC_ORDER_NONE) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "@success and @failure are only valid for atomic.cas");
    }
    if (order == AST_ATOMIC_ORDER_NONE) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
                expr->span, fn->module->file.id,
                "atomic.%s requires an explicit memory order", opbuf);
    } else {
      int valid = is_load    ? atomic_order_valid_for_load(order)
                  : is_store ? atomic_order_valid_for_store(order)
                             : atomic_order_valid_for_rmw(order);
      if (!valid) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, AST_DIAG_ERROR,
                  expr->span, fn->module->file.id,
                  "memory order '%s' is invalid for atomic.%s",
                  atomic_order_name(order), opbuf);
      }
    }
  }
  return expr_info(result);
}

static AstExprInfo check_cast(AstSemaFnCtx *fn, const AstExpr *expr) {
  AstSema *s = fn->sema;
  const AstSemaType *dst =
      sema_resolve_type(s, fn->module, expr->as.cast.target, fn, 0u);
  AstExprInfo src = sema_check_expr(fn, expr->as.cast.value, NULL);
  AstTokenKind op = expr->as.cast.op;
  int ok = 0;
  if (op == AST_TOK_KW_CAST)
    ok = (sema_type_is_numeric(dst) && sema_type_is_numeric(src.type)) ||
         sema_type_equal(dst, src.type);
  else if (op == AST_TOK_KW_TRUNC) {
    ok = sema_type_is_integer(dst) && sema_type_is_integer(src.type) &&
         dst->bit_width != 0u && src.type->bit_width != 0u &&
         dst->bit_width < src.type->bit_width;
  } else if (op == AST_TOK_KW_ZEXT) {
    ok = sema_type_is_integer(dst) && sema_type_is_integer(src.type) &&
         dst->bit_width != 0u && src.type->bit_width != 0u &&
         dst->bit_width > src.type->bit_width &&
         !(src.type->flags & AST_SEMA_TYPEF_SIGNED);
  } else if (op == AST_TOK_KW_SEXT) {
    ok = sema_type_is_integer(dst) && sema_type_is_integer(src.type) &&
         dst->bit_width != 0u && src.type->bit_width != 0u &&
         dst->bit_width > src.type->bit_width &&
         (src.type->flags & AST_SEMA_TYPEF_SIGNED);
  } else if (op == AST_TOK_KW_PTRCAST) {
    if (sema_type_is_pointer(dst) && sema_type_is_pointer(src.type)) {
      ok = dst->kind == src.type->kind;
      if (ok && fn->unsafe_depth == 0u) {
        uint32_t removed =
            src.type->as.pointer.qualifiers & ~dst->as.pointer.qualifiers;
        if (removed != 0u)
          ok = 0;
      }
    } else if (sema_type_is_pointer(dst) && sema_type_is_integer(src.type)) {
      ok = fn->unsafe_depth != 0u;
    }
  } else if (op == AST_TOK_KW_BITCAST) {
    int src_bits = src.type->kind == AST_SEMA_TYPE_INT ||
                   src.type->kind == AST_SEMA_TYPE_BITS ||
                   src.type->kind == AST_SEMA_TYPE_FLOAT ||
                   src.type->kind == AST_SEMA_TYPE_VECTOR ||
                   src.type->kind == AST_SEMA_TYPE_MASK;
    int dst_bits =
        dst->kind == AST_SEMA_TYPE_INT || dst->kind == AST_SEMA_TYPE_BITS ||
        dst->kind == AST_SEMA_TYPE_FLOAT || dst->kind == AST_SEMA_TYPE_VECTOR ||
        dst->kind == AST_SEMA_TYPE_MASK;
    ok = src_bits && dst_bits && dst->size != 0u &&
         dst->size != AST_SEMA_UNKNOWN_SIZE && src.type->size == dst->size;
  }
  if (!ok) {
    sema_diag(
        s, AST_SEMA_DIAG_INVALID_CAST, AST_DIAG_ERROR, expr->span,
        fn->module->file.id,
        "invalid explicit low-level cast for the source and destination types");
    return expr_error(s);
  }
  if (op == AST_TOK_KW_CAST && sema_type_is_integer(dst) &&
      sema_type_is_integer(src.type) && sema_const_is_valid(src.constant) &&
      !sema_const_fits_type(dst, src.constant)) {
    sema_diag(s, AST_SEMA_DIAG_INVALID_CAST, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "compile-time integer value is not representable by the "
              "destination type; use trunc for modulo conversion");
    return expr_error(s);
  }
  AstExprInfo out = expr_info(dst);
  if (sema_const_is_valid(src.constant)) {
    AstSemaConstCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.sema = s;
    cc.module = fn->module;
    cc.fn = fn;
    out.constant = sema_eval_const_expr(&cc, expr, dst);
    if (op == AST_TOK_KW_CAST && sema_type_is_numeric(dst) &&
        sema_type_is_numeric(src.type) && !sema_const_is_valid(out.constant)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_CAST, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "compile-time numeric cast is outside the representable "
                "destination range");
      return expr_error(s);
    }
  }
  return out;
}

AstExprInfo sema_check_expr(AstSemaFnCtx *fn, const AstExpr *expr,
                            const AstSemaType *expected) {
  if (!fn || !fn->sema || !expr)
    return expr_error(fn ? fn->sema : NULL);
  AstSema *s = fn->sema;
  s->stats.expression_count++;
  if (++fn->recursion_depth > s->config.recursion_limit) {
    fn->recursion_depth--;
    sema_diag(s, AST_SEMA_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "semantic expression recursion limit exceeded");
    return expr_error(s);
  }
  AstExprInfo out;
  switch (expr->kind) {
  case AST_EXPR_ERROR:
    out = expr_error(s);
    break;
  case AST_EXPR_NAME:
    out = check_name(fn, expr);
    break;
  case AST_EXPR_TYPE:
    sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "a type operand is only valid in a compile-time type intrinsic");
    out = expr_error(s);
    break;
  case AST_EXPR_INTEGER: {
    out = expr_info(sema_builtin_untyped_int(s));
    AstSemaConstCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.sema = s;
    cc.module = fn->module;
    cc.fn = fn;
    out.constant = sema_eval_const_expr(&cc, expr, expected);
    break;
  }
  case AST_EXPR_FLOAT: {
    out = expr_info(sema_builtin_untyped_float(s));
    AstSemaConstCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.sema = s;
    cc.module = fn->module;
    cc.fn = fn;
    out.constant = sema_eval_const_expr(&cc, expr, NULL);
    if (expected && expected->kind == AST_SEMA_TYPE_FLOAT &&
        sema_type_can_assign(expected, out)) {
      AstSemaConstValue quantized = sema_eval_const_expr(&cc, expr, expected);
      if (sema_const_is_valid(quantized))
        out.constant = quantized;
    }
    break;
  }
  case AST_EXPR_STRING:
  case AST_EXPR_BYTE_STRING:
  case AST_EXPR_C_STRING:
    out = expr_info(sema_builtin_string(s, expr->kind));
    break;
  case AST_EXPR_CHAR: {
    out = expr_info(sema_builtin_untyped_int(s));
    AstSemaConstCtx cc;
    memset(&cc, 0, sizeof(cc));
    cc.sema = s;
    cc.module = fn->module;
    cc.fn = fn;
    out.constant = sema_eval_const_expr(&cc, expr, expected);
    break;
  }
  case AST_EXPR_BOOL:
    out = expr_info(sema_builtin_bool(s));
    out.constant = sema_const_u64(
        expr->as.literal.token_kind == AST_TOK_KW_TRUE, out.type);
    out.constant.flags |= AST_SEMA_CONSTF_BOOL;
    break;
  case AST_EXPR_UNARY:
    out = check_unary(fn, expr, expected);
    break;
  case AST_EXPR_BINARY:
    out = check_binary(fn, expr, expected);
    break;
  case AST_EXPR_CONDITIONAL: {
    AstExprInfo c = sema_check_expr(fn, expr->as.conditional.condition,
                                    sema_builtin_bool(s));
    if (!sema_type_is_bool(c.type))
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_BOOL, AST_DIAG_ERROR,
                expr->as.conditional.condition->span, fn->module->file.id,
                "conditional expression requires a bool condition");
    AstExprInfo a =
        sema_check_expr(fn, expr->as.conditional.then_expr, expected);
    AstExprInfo b =
        sema_check_expr(fn, expr->as.conditional.else_expr, expected);
    const AstSemaType *common = coerce_binary_type(fn, a, b, expr->span);
    if (!common) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "conditional branches have incompatible types");
      out = expr_error(s);
    } else
      out = expr_info(common);
    break;
  }
  case AST_EXPR_CALL:
    out = check_call(fn, expr, expected);
    break;
  case AST_EXPR_INDEX: {
    AstExprInfo base = sema_check_expr(fn, expr->as.index.base, NULL);
    AstExprInfo idx =
        sema_check_expr(fn, expr->as.index.index, sema_builtin_usize(s));
    if (!sema_type_is_integer(idx.type))
      sema_diag(s, AST_SEMA_DIAG_EXPECTED_INTEGER, AST_DIAG_ERROR,
                expr->as.index.index->span, fn->module->file.id,
                "index expression must be an integer");
    if (sema_type_is_pointer(base.type)) {
      if (!base.type->as.pointer.element ||
          base.type->as.pointer.element->kind == AST_SEMA_TYPE_VOID) {
        sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
                  fn->module->file.id,
                  "ptr<void> and cap<void> cannot use element indexing without "
                  "an explicit ptrcast");
        out = expr_error(s);
      } else {
        out = expr_info(base.type->as.pointer.element);
        out.is_lvalue = 1u;
        out.is_mutable =
            !(base.type->as.pointer.qualifiers & AST_TYPE_QUAL_CONST);
      }
    } else if (base.type->kind == AST_SEMA_TYPE_ARRAY) {
      out = expr_info(base.type->as.array.element);
      out.is_lvalue = 1u;
      out.is_mutable = base.is_mutable;
    } else if (base.type->kind == AST_SEMA_TYPE_VECTOR) {
      out = expr_info(base.type->as.vector.element);
      out.is_lvalue = base.is_lvalue;
      out.is_mutable = base.is_mutable;
    } else if (base.type->kind == AST_SEMA_TYPE_MASK) {
      out = expr_info(sema_builtin_bool(s));
    } else if (base.type->kind == AST_SEMA_TYPE_TUPLE) {
      uint64_t tuple_index = 0u;
      if (!sema_const_to_u64(idx.constant, &tuple_index)) {
        sema_diag(s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR,
                  expr->as.index.index->span, fn->module->file.id,
                  "tuple index must be a compile-time integer");
        out = expr_error(s);
      } else if (tuple_index >= base.type->as.tuple.count) {
        sema_diag(s, AST_SEMA_DIAG_INVALID_ARRAY_SIZE, AST_DIAG_ERROR,
                  expr->as.index.index->span, fn->module->file.id,
                  "tuple index is outside the tuple bounds");
        out = expr_error(s);
      } else {
        out = expr_info(base.type->as.tuple.items[(uint32_t)tuple_index]);
        out.is_lvalue = base.is_lvalue;
        out.is_mutable = base.is_mutable;
      }
    } else {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, expr->span,
                fn->module->file.id,
                "indexing requires pointer, array, fixed vector, mask, or "
                "tuple type");
      out = expr_error(s);
    }
    break;
  }
  case AST_EXPR_MEMBER:
    out = check_member(fn, expr);
    break;
  case AST_EXPR_LOAD:
    out = check_load(fn, expr);
    break;
  case AST_EXPR_CAST:
    out = check_cast(fn, expr);
    break;
  case AST_EXPR_ATOMIC:
    out = check_atomic(fn, expr);
    break;
  case AST_EXPR_ATTR:
    out = sema_check_expr(fn, expr->as.attributed.base, expected);
    break;
  case AST_EXPR_MACRO_REF:
    sema_diag(s, AST_SEMA_DIAG_UNKNOWN_NAME, AST_DIAG_ERROR, expr->span,
              fn->module->file.id,
              "macro references must be expanded before semantic expression "
              "checking");
    out = expr_error(s);
    break;
  default:
    out = expr_error(s);
    break;
  }
  fn->recursion_depth--;
  AstExprInfo fact_info = out;
  if (expected && sema_type_can_assign(expected, out))
    fact_info.type = expected;
  (void)sema_record_expr_fact(s, expr, fact_info);
  return out;
}
