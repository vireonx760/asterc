#include "sema_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>

static uint64_t sema_mix64(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static int sema_dependent_expr_equal(const AstExpr *a, const AstExpr *b);

static int sema_eval_u64_with_bindings(AstSema *s, AstSemaModule *m,
                                       const AstExpr *expr,
                                       AstSemaGenericBinding *bindings,
                                       size_t binding_count, uint64_t *out);

static uint64_t sema_hash_type2(AstSemaTypeKind kind, const AstSemaType *a,
                                uint64_t b) {
  uint64_t h = sema_mix64((uint64_t)kind + UINT64_C(0x9e3779b97f4a7c15));
  h ^= sema_mix64((uint64_t)(uintptr_t)a);
  h ^= sema_mix64(b + UINT64_C(0x517cc1b727220a95));
  return h;
}

int sema_type_is_integer(const AstSemaType *t) {
  return t && (t->kind == AST_SEMA_TYPE_INT || t->kind == AST_SEMA_TYPE_BITS ||
               t->kind == AST_SEMA_TYPE_UNTYPED_INT);
}

int sema_type_is_numeric(const AstSemaType *t) {
  return t && (sema_type_is_integer(t) || t->kind == AST_SEMA_TYPE_FLOAT ||
               t->kind == AST_SEMA_TYPE_UNTYPED_FLOAT);
}

int sema_type_is_pointer(const AstSemaType *t) {
  return t && (t->kind == AST_SEMA_TYPE_PTR || t->kind == AST_SEMA_TYPE_CAP);
}

int sema_type_is_bool(const AstSemaType *t) {
  return t && t->kind == AST_SEMA_TYPE_BOOL;
}

int sema_type_is_value_type(const AstSemaType *t) {
  if (!t)
    return 0;
  switch (t->kind) {
  case AST_SEMA_TYPE_ERROR:
  case AST_SEMA_TYPE_BOOL:
  case AST_SEMA_TYPE_INT:
  case AST_SEMA_TYPE_FLOAT:
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
  case AST_SEMA_TYPE_BITS:
  case AST_SEMA_TYPE_VECTOR:
  case AST_SEMA_TYPE_MASK:
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED:
  case AST_SEMA_TYPE_STRUCT:
  case AST_SEMA_TYPE_UNTYPED_INT:
  case AST_SEMA_TYPE_UNTYPED_FLOAT:
  case AST_SEMA_TYPE_STRING:
  case AST_SEMA_TYPE_BYTE_STRING:
  case AST_SEMA_TYPE_C_STRING:
  case AST_SEMA_TYPE_TYPE_PARAMETER:
    return 1;
  case AST_SEMA_TYPE_ARRAY:
    return sema_type_is_value_type(t->as.array.element);
  case AST_SEMA_TYPE_TUPLE:
    for (uint32_t i = 0u; i < t->as.tuple.count; ++i) {
      if (!sema_type_is_value_type(t->as.tuple.items[i]))
        return 0;
    }
    return 1;
  case AST_SEMA_TYPE_VOID:
  case AST_SEMA_TYPE_FUNCTION:
  default:
    return 0;
  }
}

int sema_type_is_lane_scalar(const AstSemaType *t) {
  if (!t)
    return 0;
  return t->kind == AST_SEMA_TYPE_INT || t->kind == AST_SEMA_TYPE_BITS ||
         t->kind == AST_SEMA_TYPE_FLOAT;
}

int sema_type_is_vector_numeric(const AstSemaType *t) {
  return t &&
         (t->kind == AST_SEMA_TYPE_VECTOR ||
          t->kind == AST_SEMA_TYPE_SVECTOR) &&
         sema_type_is_lane_scalar(t->kind == AST_SEMA_TYPE_VECTOR
                                      ? t->as.vector.element
                                      : t->as.scalable.element);
}

int sema_type_is_vector_integer(const AstSemaType *t) {
  if (!t ||
      (t->kind != AST_SEMA_TYPE_VECTOR && t->kind != AST_SEMA_TYPE_SVECTOR))
    return 0;
  const AstSemaType *element = t->kind == AST_SEMA_TYPE_VECTOR
                                   ? t->as.vector.element
                                   : t->as.scalable.element;
  return element && (element->kind == AST_SEMA_TYPE_INT ||
                     element->kind == AST_SEMA_TYPE_BITS);
}

int sema_type_equal(const AstSemaType *a, const AstSemaType *b) {
  if (a == b)
    return 1;
  if (!a || !b || a->kind != b->kind)
    return 0;
  if (a->kind == AST_SEMA_TYPE_ERROR)
    return 1;
  switch (a->kind) {
  case AST_SEMA_TYPE_INT:
    return a->bit_width == b->bit_width &&
           ((a->flags ^ b->flags) & AST_SEMA_TYPEF_SIGNED) == 0u;
  case AST_SEMA_TYPE_FLOAT:
    return a->bit_width == b->bit_width &&
           ((a->flags ^ b->flags) & AST_SEMA_TYPEF_BFLOAT) == 0u;
  case AST_SEMA_TYPE_BITS:
    if (a->as.bits.dependent != b->as.bits.dependent)
      return 0;
    if (!a->as.bits.dependent)
      return a->as.bits.width == b->as.bits.width;
    if (a->as.bits.dependent_expr || b->as.bits.dependent_expr) {
      return sema_dependent_expr_equal(a->as.bits.dependent_expr,
                                       b->as.bits.dependent_expr);
    }
    return sema_name_eq(a->as.bits.parameter, b->as.bits.parameter);
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
    return a->as.pointer.qualifiers == b->as.pointer.qualifiers &&
           sema_type_equal(a->as.pointer.element, b->as.pointer.element);
  case AST_SEMA_TYPE_ARRAY:
    if (a->as.array.dependent != b->as.array.dependent)
      return 0;
    if (a->as.array.dependent) {
      if (a->as.array.dependent_expr || b->as.array.dependent_expr) {
        if (!sema_dependent_expr_equal(a->as.array.dependent_expr,
                                       b->as.array.dependent_expr))
          return 0;
      } else if (!sema_name_eq(a->as.array.parameter, b->as.array.parameter))
        return 0;
    } else if (a->as.array.count != b->as.array.count)
      return 0;
    return sema_type_equal(a->as.array.element, b->as.array.element);
  case AST_SEMA_TYPE_VECTOR:
    if (a->as.vector.dependent != b->as.vector.dependent)
      return 0;
    if (a->as.vector.dependent) {
      if (a->as.vector.dependent_expr || b->as.vector.dependent_expr) {
        if (!sema_dependent_expr_equal(a->as.vector.dependent_expr,
                                       b->as.vector.dependent_expr))
          return 0;
      } else if (!sema_name_eq(a->as.vector.parameter, b->as.vector.parameter))
        return 0;
    } else if (a->as.vector.lanes != b->as.vector.lanes)
      return 0;
    return sema_type_equal(a->as.vector.element, b->as.vector.element);
  case AST_SEMA_TYPE_MASK:
    if (a->as.mask.dependent != b->as.mask.dependent)
      return 0;
    if (!a->as.mask.dependent)
      return a->as.mask.lanes == b->as.mask.lanes;
    if (a->as.mask.dependent_expr || b->as.mask.dependent_expr) {
      return sema_dependent_expr_equal(a->as.mask.dependent_expr,
                                       b->as.mask.dependent_expr);
    }
    return sema_name_eq(a->as.mask.parameter, b->as.mask.parameter);
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED:
    return sema_type_equal(a->as.scalable.element, b->as.scalable.element);
  case AST_SEMA_TYPE_TUPLE:
    if (a->as.tuple.count != b->as.tuple.count)
      return 0;
    for (uint32_t i = 0u; i < a->as.tuple.count; ++i) {
      if (!sema_type_equal(a->as.tuple.items[i], b->as.tuple.items[i]))
        return 0;
    }
    return 1;
  case AST_SEMA_TYPE_FUNCTION:
    if (a->as.function.param_count != b->as.function.param_count ||
        !sema_type_equal(a->as.function.result, b->as.function.result))
      return 0;
    for (uint32_t i = 0u; i < a->as.function.param_count; ++i) {
      if (!sema_type_equal(a->as.function.params[i], b->as.function.params[i]))
        return 0;
    }
    return 1;
  case AST_SEMA_TYPE_STRUCT:
    if (a->as.record.decl != b->as.record.decl ||
        a->as.record.generic_arg_count != b->as.record.generic_arg_count)
      return 0;
    for (uint32_t i = 0u; i < a->as.record.generic_arg_count; ++i) {
      const AstSemaType *at = a->as.record.generic_type_args
                                  ? a->as.record.generic_type_args[i]
                                  : NULL;
      const AstSemaType *bt = b->as.record.generic_type_args
                                  ? b->as.record.generic_type_args[i]
                                  : NULL;
      if (at || bt) {
        if (!at || !bt || !sema_type_equal(at, bt))
          return 0;
        continue;
      }
      uint8_t ad = a->as.record.generic_const_dependent
                       ? a->as.record.generic_const_dependent[i]
                       : 0u;
      uint8_t bd = b->as.record.generic_const_dependent
                       ? b->as.record.generic_const_dependent[i]
                       : 0u;
      if (ad || bd) {
        if (!ad || !bd || !a->as.record.generic_const_parameters ||
            !b->as.record.generic_const_parameters ||
            !sema_name_eq(a->as.record.generic_const_parameters[i],
                          b->as.record.generic_const_parameters[i]))
          return 0;
        continue;
      }
      AstSemaConstValue ac = a->as.record.generic_const_args[i];
      AstSemaConstValue bc = b->as.record.generic_const_args[i];
      uint32_t mask = AST_SEMA_CONSTF_VALID | AST_SEMA_CONSTF_SIGNED |
                      AST_SEMA_CONSTF_BOOL | AST_SEMA_CONSTF_NEGATIVE |
                      AST_SEMA_CONSTF_FLOAT;
      if ((ac.flags & mask) != (bc.flags & mask) || ac.lo != bc.lo ||
          ac.hi != bc.hi)
        return 0;
      if ((ac.flags & AST_SEMA_CONSTF_FLOAT) && ac.floating != bc.floating)
        return 0;
    }
    return 1;
  case AST_SEMA_TYPE_TYPE_PARAMETER:
    return a == b;
  default:
    return 1;
  }
}

static int sema_u128_le(uint64_t hi, uint64_t lo, uint64_t bound_hi,
                        uint64_t bound_lo) {
  return hi < bound_hi || (hi == bound_hi && lo <= bound_lo);
}

static void sema_u128_low_ones(uint32_t bits, uint64_t *hi, uint64_t *lo) {
  if (bits == 0u) {
    *hi = 0u;
    *lo = 0u;
  } else if (bits < 64u) {
    *hi = 0u;
    *lo = (UINT64_C(1) << bits) - 1u;
  } else if (bits == 64u) {
    *hi = 0u;
    *lo = UINT64_MAX;
  } else if (bits < 128u) {
    *hi = (UINT64_C(1) << (bits - 64u)) - 1u;
    *lo = UINT64_MAX;
  } else {
    *hi = UINT64_MAX;
    *lo = UINT64_MAX;
  }
}

int sema_const_fits_type(const AstSemaType *dst, AstSemaConstValue v) {
  if (!dst || !sema_const_is_valid(v) || (v.flags & AST_SEMA_CONSTF_FLOAT) ||
      !sema_type_is_integer(dst))
    return 0;
  uint32_t bits = dst->bit_width;
  if (dst->kind == AST_SEMA_TYPE_BITS)
    bits = (uint32_t)(dst->as.bits.width > UINT32_MAX ? UINT32_MAX
                                                      : dst->as.bits.width);
  if (bits == 0u || bits > 128u)
    return 0;

  int negative = (v.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
  int signed_type = (dst->flags & AST_SEMA_TYPEF_SIGNED) != 0u;
  if (negative && !signed_type)
    return 0;

  uint64_t bound_hi = 0u, bound_lo = 0u;
  if (signed_type && negative) {
    uint32_t bit = bits - 1u;
    if (bit < 64u)
      bound_lo = UINT64_C(1) << bit;
    else
      bound_hi = UINT64_C(1) << (bit - 64u);
  } else {
    uint32_t value_bits = signed_type ? bits - 1u : bits;
    sema_u128_low_ones(value_bits, &bound_hi, &bound_lo);
  }
  return sema_u128_le(v.hi, v.lo, bound_hi, bound_lo);
}

static int sema_untyped_float_fits(const AstSemaType *dst,
                                   AstSemaConstValue value) {
  if (!dst || dst->kind != AST_SEMA_TYPE_FLOAT || !sema_const_is_valid(value) ||
      !(value.flags & AST_SEMA_CONSTF_FLOAT))
    return 1;
  double max_value = DBL_MAX;
  if (dst->bit_width == 16u && (dst->flags & AST_SEMA_TYPEF_BFLOAT))
    max_value = 3.3895313892515355e38;
  else if (dst->bit_width == 16u)
    max_value = 65504.0;
  else if (dst->bit_width == 32u)
    max_value = FLT_MAX;
  return AST_SEMA_ISFINITE(value.floating) && fabs(value.floating) <= max_value;
}

int sema_type_can_assign(const AstSemaType *dst, AstExprInfo src) {
  if (!dst || !src.type)
    return 0;
  if (dst->kind == AST_SEMA_TYPE_ERROR || src.type->kind == AST_SEMA_TYPE_ERROR)
    return 1;
  if (sema_type_equal(dst, src.type))
    return 1;
  if (src.type->kind == AST_SEMA_TYPE_UNTYPED_INT &&
      sema_type_is_integer(dst)) {
    return !sema_const_is_valid(src.constant) ||
           sema_const_fits_type(dst, src.constant);
  }
  if (src.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT &&
      dst->kind == AST_SEMA_TYPE_FLOAT) {
    return sema_untyped_float_fits(dst, src.constant);
  }
  if ((dst->kind == AST_SEMA_TYPE_PTR || dst->kind == AST_SEMA_TYPE_CAP) &&
      src.type->kind == dst->kind) {
    uint32_t src_q = src.type->as.pointer.qualifiers;
    uint32_t dst_q = dst->as.pointer.qualifiers;
    if ((src_q & ~dst_q) == 0u &&
        sema_type_equal(dst->as.pointer.element, src.type->as.pointer.element))
      return 1;
  }
  if ((src.type->kind == AST_SEMA_TYPE_C_STRING ||
       src.type->kind == AST_SEMA_TYPE_BYTE_STRING ||
       src.type->kind == AST_SEMA_TYPE_STRING) &&
      dst->kind == AST_SEMA_TYPE_PTR && dst->as.pointer.element &&
      dst->as.pointer.element->kind == AST_SEMA_TYPE_INT &&
      dst->as.pointer.element->bit_width == 8u &&
      (dst->as.pointer.qualifiers & AST_TYPE_QUAL_CONST) != 0u)
    return 1;
  return 0;
}

int sema_type_assignable(AstSema *s, const AstSemaType *dst, AstExprInfo src,
                         AstSpan span, uint32_t file_id) {
  if (sema_type_can_assign(dst, src))
    return 1;
  if (!dst || !src.type)
    return 0;
  if (src.type->kind == AST_SEMA_TYPE_UNTYPED_INT &&
      sema_type_is_integer(dst) && sema_const_is_valid(src.constant)) {
    sema_diag(s, AST_SEMA_DIAG_INTEGER_LITERAL_OVERFLOW, AST_DIAG_ERROR, span,
              file_id, "integer literal does not fit the destination type");
    return 0;
  }
  if (src.type->kind == AST_SEMA_TYPE_UNTYPED_FLOAT &&
      dst->kind == AST_SEMA_TYPE_FLOAT && sema_const_is_valid(src.constant) &&
      (src.constant.flags & AST_SEMA_CONSTF_FLOAT)) {
    sema_diag(
        s, AST_SEMA_DIAG_FLOAT_LITERAL_OVERFLOW, AST_DIAG_ERROR, span, file_id,
        "floating-point literal is not representable by the destination type");
    return 0;
  }
  return 0;
}

static const AstSemaType *sema_new_type(AstSema *s, AstSemaTypeKind kind) {
  AstSemaType *t = (AstSemaType *)sema_arena_alloc(s, sizeof(*t),
                                                   ASTER_ALIGNOF(AstSemaType));
  if (!t)
    return sema_builtin_error(s);
  t->kind = kind;
  t->size = AST_SEMA_UNKNOWN_SIZE;
  t->alignment = 1u;
  s->stats.type_count++;
  return t;
}

const AstSemaType *sema_make_pointer(AstSema *s, AstSemaTypeKind kind,
                                     const AstSemaType *element,
                                     uint32_t qualifiers) {
  AstSemaType *t = (AstSemaType *)sema_new_type(s, kind);
  if (!t || t == sema_builtin_error(s))
    return sema_builtin_error(s);
  t->bit_width = s->config.pointer_bits;
  t->size = s->config.pointer_bits / 8u;
  if (!t->size)
    t->size = 8u;
  t->alignment = (uint32_t)t->size;
  t->flags = AST_SEMA_TYPEF_COMPLETE;
  t->as.pointer.element = element;
  t->as.pointer.qualifiers = qualifiers;
  t->hash = sema_hash_type2(kind, element, qualifiers);
  return t;
}

const AstSemaType *sema_make_array(AstSema *s, const AstSemaType *element,
                                   uint64_t count) {
  AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_ARRAY);
  if (!t || t == sema_builtin_error(s))
    return sema_builtin_error(s);
  t->as.array.element = element;
  t->as.array.count = count;
  t->hash = sema_hash_type2(AST_SEMA_TYPE_ARRAY, element, count);
  if (element && element->size != AST_SEMA_UNKNOWN_SIZE &&
      count <= UINT64_MAX / (element->size ? element->size : 1u)) {
    t->size = element->size * count;
    t->alignment = element->alignment ? element->alignment : 1u;
    t->flags |= AST_SEMA_TYPEF_COMPLETE;
  }
  return t;
}

const AstSemaType *sema_make_function(AstSema *s, const AstSemaType **params,
                                      uint32_t count,
                                      const AstSemaType *result) {
  AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_FUNCTION);
  if (!t || t == sema_builtin_error(s))
    return sema_builtin_error(s);
  if (count) {
    const AstSemaType **copy = (const AstSemaType **)sema_arena_alloc(
        s, (size_t)count * sizeof(*copy), ASTER_ALIGNOF(AstSemaType *));
    if (!copy)
      return sema_builtin_error(s);
    memcpy(copy, params, (size_t)count * sizeof(*copy));
    t->as.function.params = copy;
  }
  t->as.function.param_count = count;
  t->as.function.result = result ? result : sema_builtin_void(s);
  t->size = 0u;
  t->alignment = 1u;
  t->flags = AST_SEMA_TYPEF_COMPLETE;
  uint64_t h = sema_hash_type2(AST_SEMA_TYPE_FUNCTION, result, count);
  for (uint32_t i = 0u; i < count; ++i)
    h ^= sema_mix64((uint64_t)(uintptr_t)params[i] + i);
  t->hash = h;
  return t;
}

const AstSemaType *sema_make_tuple(AstSema *s, const AstSemaType **items,
                                   uint32_t count) {
  AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_TUPLE);
  if (!t || t == sema_builtin_error(s))
    return sema_builtin_error(s);
  if (count) {
    const AstSemaType **copy = (const AstSemaType **)sema_arena_alloc(
        s, (size_t)count * sizeof(*copy), ASTER_ALIGNOF(AstSemaType *));
    if (!copy)
      return sema_builtin_error(s);
    memcpy(copy, items, (size_t)count * sizeof(*copy));
    t->as.tuple.items = copy;
  }
  t->as.tuple.count = count;
  uint64_t size = 0u;
  uint32_t alignment = 1u;
  uint64_t hash = sema_mix64((uint64_t)AST_SEMA_TYPE_TUPLE ^ count);
  for (uint32_t i = 0u; i < count; ++i) {
    const AstSemaType *item = items[i];
    uint32_t a = item && item->alignment ? item->alignment : 1u;
    if (a > alignment)
      alignment = a;
    hash ^= sema_mix64((uint64_t)(uintptr_t)item +
                       (uint64_t)i * UINT64_C(0x9e3779b97f4a7c15));
    if (!item || item->size == AST_SEMA_UNKNOWN_SIZE) {
      size = AST_SEMA_UNKNOWN_SIZE;
      continue;
    }
    if (size != AST_SEMA_UNKNOWN_SIZE) {
      uint64_t pad = (a - (size % a)) % a;
      if (size > UINT64_MAX - pad || size + pad > UINT64_MAX - item->size)
        size = AST_SEMA_UNKNOWN_SIZE;
      else
        size += pad + item->size;
    }
  }
  if (size != AST_SEMA_UNKNOWN_SIZE) {
    uint64_t pad = (alignment - (size % alignment)) % alignment;
    if (size > UINT64_MAX - pad)
      size = AST_SEMA_UNKNOWN_SIZE;
    else
      size += pad;
  }
  t->size = size;
  t->alignment = alignment;
  if (size != AST_SEMA_UNKNOWN_SIZE)
    t->flags = AST_SEMA_TYPEF_COMPLETE;
  t->hash = hash;
  return t;
}

static AstSemaGenericBinding *sema_find_binding(AstSemaGenericBinding *bindings,
                                                size_t count, AstName name) {
  for (size_t i = count; i > 0u; --i)
    if (sema_name_eq(bindings[i - 1u].name, name))
      return &bindings[i - 1u];
  return NULL;
}

static const AstSemaGenericBinding *
sema_find_const_binding_ro(const AstSemaGenericBinding *bindings, size_t count,
                           AstName name) {
  for (size_t i = count; i > 0u; --i) {
    if (bindings[i - 1u].is_const && sema_name_eq(bindings[i - 1u].name, name))
      return &bindings[i - 1u];
  }
  return NULL;
}

static size_t
sema_find_type_binding_index(const AstSemaGenericBinding *bindings,
                             size_t count, const AstSemaType *parameter) {
  for (size_t i = 0u; i < count; ++i) {
    if (!bindings[i].is_const && bindings[i].type_value == parameter)
      return i;
  }
  return SIZE_MAX;
}

static uint64_t sema_dependent_expr_hash(const AstExpr *expr) {
  if (!expr)
    return 0u;
  uint64_t x = ((uint64_t)expr->span.file_id << 32u) ^ expr->span.offset ^
               ((uint64_t)expr->span.length << 17u);
  return sema_mix64(x ^ (uint64_t)expr->kind);
}

static int sema_dependent_expr_equal(const AstExpr *a, const AstExpr *b) {
  if (a == b)
    return 1;
  if (!a || !b || a->kind != b->kind)
    return 0;
  switch (a->kind) {
  case AST_EXPR_NAME:
    return sema_name_eq(a->as.name.name, b->as.name.name);
  case AST_EXPR_UNARY:
    return a->as.unary.op == b->as.unary.op &&
           sema_dependent_expr_equal(a->as.unary.operand, b->as.unary.operand);
  case AST_EXPR_BINARY:
    return a->as.binary.op == b->as.binary.op &&
           sema_dependent_expr_equal(a->as.binary.left, b->as.binary.left) &&
           sema_dependent_expr_equal(a->as.binary.right, b->as.binary.right);
  case AST_EXPR_CONDITIONAL:
    return sema_dependent_expr_equal(a->as.conditional.condition,
                                     b->as.conditional.condition) &&
           sema_dependent_expr_equal(a->as.conditional.then_expr,
                                     b->as.conditional.then_expr) &&
           sema_dependent_expr_equal(a->as.conditional.else_expr,
                                     b->as.conditional.else_expr);
  default:
    return a->span.file_id == b->span.file_id &&
           a->span.offset == b->span.offset && a->span.length == b->span.length;
  }
}

static int
sema_expr_depends_on_unresolved_const(const AstExpr *expr,
                                      const AstSemaGenericBinding *bindings,
                                      size_t binding_count, uint32_t depth) {
  if (!expr || depth > 256u)
    return 0;
  switch (expr->kind) {
  case AST_EXPR_NAME: {
    const AstSemaGenericBinding *binding =
        sema_find_const_binding_ro(bindings, binding_count, expr->as.name.name);
    return binding && !sema_const_is_valid(binding->const_value);
  }
  case AST_EXPR_UNARY:
    return sema_expr_depends_on_unresolved_const(
        expr->as.unary.operand, bindings, binding_count, depth + 1u);
  case AST_EXPR_BINARY:
    return sema_expr_depends_on_unresolved_const(expr->as.binary.left, bindings,
                                                 binding_count, depth + 1u) ||
           sema_expr_depends_on_unresolved_const(
               expr->as.binary.right, bindings, binding_count, depth + 1u);
  case AST_EXPR_CONDITIONAL:
    return sema_expr_depends_on_unresolved_const(expr->as.conditional.condition,
                                                 bindings, binding_count,
                                                 depth + 1u) ||
           sema_expr_depends_on_unresolved_const(expr->as.conditional.then_expr,
                                                 bindings, binding_count,
                                                 depth + 1u) ||
           sema_expr_depends_on_unresolved_const(expr->as.conditional.else_expr,
                                                 bindings, binding_count,
                                                 depth + 1u);
  case AST_EXPR_CALL:
    for (uint32_t i = 0u; i < expr->as.call.args.count; ++i) {
      if (sema_expr_depends_on_unresolved_const(
              expr->as.call.args.items[i], bindings, binding_count, depth + 1u))
        return 1;
    }
    return 0;
  case AST_EXPR_INDEX:
    return sema_expr_depends_on_unresolved_const(expr->as.index.base, bindings,
                                                 binding_count, depth + 1u) ||
           sema_expr_depends_on_unresolved_const(expr->as.index.index, bindings,
                                                 binding_count, depth + 1u);
  case AST_EXPR_MEMBER:
    return sema_expr_depends_on_unresolved_const(expr->as.member.base, bindings,
                                                 binding_count, depth + 1u);
  case AST_EXPR_CAST:
    return sema_expr_depends_on_unresolved_const(expr->as.cast.value, bindings,
                                                 binding_count, depth + 1u);
  case AST_EXPR_ATTR:
    return sema_expr_depends_on_unresolved_const(
        expr->as.attributed.base, bindings, binding_count, depth + 1u);
  default:
    return 0;
  }
}

const AstSemaType *sema_make_mask(AstSema *s, uint64_t lanes) {
  if (!s || lanes == 0u)
    return sema_builtin_error(s);
  AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_MASK);
  if (!t || t == sema_builtin_error(s))
    return sema_builtin_error(s);
  t->as.mask.lanes = lanes;
  t->bit_width = (uint32_t)(lanes > UINT32_MAX ? UINT32_MAX : lanes);
  t->size = (lanes + 7u) / 8u;
  t->alignment = 1u;
  t->flags = AST_SEMA_TYPEF_COMPLETE;
  t->hash = sema_mix64(AST_SEMA_TYPE_MASK ^ lanes);
  return t;
}

const AstSemaType *sema_make_pred(AstSema *s, const AstSemaType *element) {
  if (!s || !element)
    return sema_builtin_error(s);
  AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_PRED);
  if (!t || t == sema_builtin_error(s))
    return sema_builtin_error(s);
  t->as.scalable.element = element;
  t->size = AST_SEMA_UNKNOWN_SIZE;
  t->alignment = 1u;
  t->hash = sema_hash_type2(AST_SEMA_TYPE_PRED, element, 0u);
  return t;
}

int sema_type_contains_parameter(const AstSemaType *type,
                                 const AstSemaGenericBinding *bindings,
                                 size_t binding_count) {
  if (!type)
    return 0;
  if (type->kind == AST_SEMA_TYPE_TYPE_PARAMETER) {
    return sema_find_type_binding_index(bindings, binding_count, type) !=
           SIZE_MAX;
  }
  switch (type->kind) {
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP:
    return sema_type_contains_parameter(type->as.pointer.element, bindings,
                                        binding_count);
  case AST_SEMA_TYPE_BITS:
    return type->as.bits.dependent &&
           (type->as.bits.dependent_expr != NULL ||
            sema_find_const_binding_ro(bindings, binding_count,
                                       type->as.bits.parameter) != NULL);
  case AST_SEMA_TYPE_VECTOR:
    return (type->as.vector.dependent &&
            (type->as.vector.dependent_expr != NULL ||
             sema_find_const_binding_ro(bindings, binding_count,
                                        type->as.vector.parameter) != NULL)) ||
           sema_type_contains_parameter(type->as.vector.element, bindings,
                                        binding_count);
  case AST_SEMA_TYPE_MASK:
    return type->as.mask.dependent &&
           (type->as.mask.dependent_expr != NULL ||
            sema_find_const_binding_ro(bindings, binding_count,
                                       type->as.mask.parameter) != NULL);
  case AST_SEMA_TYPE_ARRAY:
    return (type->as.array.dependent &&
            (type->as.array.dependent_expr != NULL ||
             sema_find_const_binding_ro(bindings, binding_count,
                                        type->as.array.parameter) != NULL)) ||
           sema_type_contains_parameter(type->as.array.element, bindings,
                                        binding_count);
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED:
    return sema_type_contains_parameter(type->as.scalable.element, bindings,
                                        binding_count);
  case AST_SEMA_TYPE_TUPLE:
    for (uint32_t i = 0u; i < type->as.tuple.count; ++i) {
      if (sema_type_contains_parameter(type->as.tuple.items[i], bindings,
                                       binding_count))
        return 1;
    }
    return 0;
  case AST_SEMA_TYPE_STRUCT:
    if (type->as.record.generic_type_args) {
      for (uint32_t i = 0u; i < type->as.record.generic_arg_count; ++i) {
        if (type->as.record.generic_type_args[i] &&
            sema_type_contains_parameter(type->as.record.generic_type_args[i],
                                         bindings, binding_count))
          return 1;
      }
    }
    for (uint32_t i = 0u; i < type->as.record.field_count; ++i) {
      if (sema_type_contains_parameter(type->as.record.fields[i].type, bindings,
                                       binding_count))
        return 1;
    }
    return 0;
  case AST_SEMA_TYPE_FUNCTION:
    if (sema_type_contains_parameter(type->as.function.result, bindings,
                                     binding_count))
      return 1;
    for (uint32_t i = 0u; i < type->as.function.param_count; ++i) {
      if (sema_type_contains_parameter(type->as.function.params[i], bindings,
                                       binding_count))
        return 1;
    }
    return 0;
  default:
    return 0;
  }
}

static int
sema_substitute_dependent_count(const AstSemaGenericBinding *template_bindings,
                                const AstSemaGenericBinding *values,
                                size_t binding_count, AstName parameter,
                                uint64_t *value_out, AstName *dependent_out,
                                int *is_dependent_out) {
  for (size_t i = 0u; i < binding_count; ++i) {
    if (!template_bindings[i].is_const ||
        !sema_name_eq(template_bindings[i].name, parameter))
      continue;
    if (sema_const_is_valid(values[i].const_value)) {
      if (!sema_const_to_u64(values[i].const_value, value_out))
        return 0;
      *is_dependent_out = 0;
      return 1;
    }
    if (values[i].has_dependent_const) {
      *dependent_out = values[i].dependent_const;
      *is_dependent_out = 1;
      return 1;
    }
    return 0;
  }
  return 0;
}

const AstSemaType *
sema_substitute_type(AstSema *s, const AstSemaType *type,
                     const AstSemaGenericBinding *template_bindings,
                     const AstSemaGenericBinding *values, size_t binding_count,
                     AstSpan span, uint32_t file_id, uint32_t depth) {
  if (!type)
    return sema_builtin_error(s);
  if (depth > s->config.recursion_limit) {
    sema_diag(s, AST_SEMA_DIAG_RECURSIVE_TYPE, AST_DIAG_ERROR, span, file_id,
              "generic type substitution exceeded the recursion limit");
    return sema_builtin_error(s);
  }
  if (type->kind == AST_SEMA_TYPE_TYPE_PARAMETER) {
    size_t index =
        sema_find_type_binding_index(template_bindings, binding_count, type);
    if (index == SIZE_MAX || !values[index].type_value)
      return type;
    return values[index].type_value;
  }
  switch (type->kind) {
  case AST_SEMA_TYPE_PTR:
  case AST_SEMA_TYPE_CAP: {
    const AstSemaType *element =
        sema_substitute_type(s, type->as.pointer.element, template_bindings,
                             values, binding_count, span, file_id, depth + 1u);
    if (element == type->as.pointer.element)
      return type;
    return sema_make_pointer(s, type->kind, element,
                             type->as.pointer.qualifiers);
  }
  case AST_SEMA_TYPE_BITS: {
    if (!type->as.bits.dependent)
      return type;
    uint64_t width = 0u;
    if (type->as.bits.dependent_expr) {
      AstSemaModule *module =
          sema_module_by_file_id(s, type->as.bits.dependent_expr->span.file_id);
      if (module &&
          sema_eval_u64_with_bindings(s, module, type->as.bits.dependent_expr,
                                      (AstSemaGenericBinding *)values,
                                      binding_count, &width)) {
        if (width == 0u || width > UINT32_MAX)
          return sema_builtin_error(s);
        AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_BITS);
        out->as.bits.width = width;
        out->bit_width = (uint32_t)width;
        out->size = (width + 7u) / 8u;
        out->alignment =
            out->size >= 16u ? 16u : (uint32_t)(out->size ? out->size : 1u);
        out->flags = AST_SEMA_TYPEF_COMPLETE;
        out->hash = sema_mix64(AST_SEMA_TYPE_BITS ^ width);
        return out;
      }
      return type;
    }
    AstName dependent;
    memset(&dependent, 0, sizeof(dependent));
    int is_dependent = 0;
    if (!sema_substitute_dependent_count(template_bindings, values,
                                         binding_count, type->as.bits.parameter,
                                         &width, &dependent, &is_dependent))
      return type;
    AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_BITS);
    if (is_dependent) {
      out->as.bits.dependent = 1u;
      out->as.bits.parameter = dependent;
      out->hash = sema_mix64(AST_SEMA_TYPE_BITS ^ dependent.hash);
    } else {
      out->as.bits.width = width;
      out->bit_width = (uint32_t)(width > UINT32_MAX ? UINT32_MAX : width);
      out->size = (width + 7u) / 8u;
      out->alignment =
          out->size >= 16u ? 16u : (uint32_t)(out->size ? out->size : 1u);
      out->flags = AST_SEMA_TYPEF_COMPLETE;
      out->hash = sema_mix64(AST_SEMA_TYPE_BITS ^ width);
    }
    return out;
  }
  case AST_SEMA_TYPE_ARRAY: {
    const AstSemaType *element =
        sema_substitute_type(s, type->as.array.element, template_bindings,
                             values, binding_count, span, file_id, depth + 1u);
    if (!type->as.array.dependent)
      return element == type->as.array.element
                 ? type
                 : sema_make_array(s, element, type->as.array.count);
    uint64_t count = 0u;
    if (type->as.array.dependent_expr) {
      AstSemaModule *module = sema_module_by_file_id(
          s, type->as.array.dependent_expr->span.file_id);
      if (module &&
          sema_eval_u64_with_bindings(s, module, type->as.array.dependent_expr,
                                      (AstSemaGenericBinding *)values,
                                      binding_count, &count)) {
        return sema_make_array(s, element, count);
      }
      if (element == type->as.array.element)
        return type;
      AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_ARRAY);
      out->as.array.element = element;
      out->as.array.dependent = 1u;
      out->as.array.dependent_expr = type->as.array.dependent_expr;
      out->size = AST_SEMA_UNKNOWN_SIZE;
      out->alignment = element->alignment ? element->alignment : 1u;
      out->hash = sema_hash_type2(
          AST_SEMA_TYPE_ARRAY, element,
          sema_dependent_expr_hash(type->as.array.dependent_expr));
      return out;
    }
    AstName dependent;
    memset(&dependent, 0, sizeof(dependent));
    int is_dependent = 0;
    if (!sema_substitute_dependent_count(
            template_bindings, values, binding_count, type->as.array.parameter,
            &count, &dependent, &is_dependent))
      return type;
    if (!is_dependent)
      return sema_make_array(s, element, count);
    AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_ARRAY);
    out->as.array.element = element;
    out->as.array.dependent = 1u;
    out->as.array.parameter = dependent;
    out->size = AST_SEMA_UNKNOWN_SIZE;
    out->alignment = element->alignment ? element->alignment : 1u;
    out->hash = sema_hash_type2(AST_SEMA_TYPE_ARRAY, element, dependent.hash);
    return out;
  }
  case AST_SEMA_TYPE_VECTOR: {
    const AstSemaType *element =
        sema_substitute_type(s, type->as.vector.element, template_bindings,
                             values, binding_count, span, file_id, depth + 1u);
    if (!type->as.vector.dependent) {
      if (element == type->as.vector.element)
        return type;
      AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_VECTOR);
      out->as.vector.element = element;
      out->as.vector.lanes = type->as.vector.lanes;
      if (element->size != AST_SEMA_UNKNOWN_SIZE &&
          type->as.vector.lanes <=
              UINT64_MAX / (element->size ? element->size : 1u)) {
        out->size = element->size * type->as.vector.lanes;
        out->alignment = out->size >= 16u
                             ? 16u
                             : (element->alignment ? element->alignment : 1u);
        out->flags = AST_SEMA_TYPEF_COMPLETE;
      }
      out->hash =
          sema_hash_type2(AST_SEMA_TYPE_VECTOR, element, type->as.vector.lanes);
      return out;
    }
    uint64_t lanes = 0u;
    if (type->as.vector.dependent_expr) {
      AstSemaModule *module = sema_module_by_file_id(
          s, type->as.vector.dependent_expr->span.file_id);
      if (module &&
          sema_eval_u64_with_bindings(s, module, type->as.vector.dependent_expr,
                                      (AstSemaGenericBinding *)values,
                                      binding_count, &lanes)) {
        if (lanes == 0u)
          return sema_builtin_error(s);
        AstSemaType *out =
            (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_VECTOR);
        out->as.vector.element = element;
        out->as.vector.lanes = lanes;
        if (element->size != AST_SEMA_UNKNOWN_SIZE &&
            lanes <= UINT64_MAX / (element->size ? element->size : 1u)) {
          out->size = element->size * lanes;
          out->alignment = out->size >= 16u
                               ? 16u
                               : (element->alignment ? element->alignment : 1u);
          out->flags = AST_SEMA_TYPEF_COMPLETE;
        }
        out->hash = sema_hash_type2(AST_SEMA_TYPE_VECTOR, element, lanes);
        return out;
      }
      AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_VECTOR);
      out->as.vector.element = element;
      out->as.vector.dependent = 1u;
      out->as.vector.dependent_expr = type->as.vector.dependent_expr;
      out->size = AST_SEMA_UNKNOWN_SIZE;
      out->alignment = element->alignment ? element->alignment : 1u;
      out->hash = sema_hash_type2(
          AST_SEMA_TYPE_VECTOR, element,
          sema_dependent_expr_hash(type->as.vector.dependent_expr));
      return out;
    }
    AstName dependent;
    memset(&dependent, 0, sizeof(dependent));
    int is_dependent = 0;
    if (!sema_substitute_dependent_count(
            template_bindings, values, binding_count, type->as.vector.parameter,
            &lanes, &dependent, &is_dependent))
      return type;
    AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_VECTOR);
    out->as.vector.element = element;
    if (is_dependent) {
      out->as.vector.dependent = 1u;
      out->as.vector.parameter = dependent;
      out->size = AST_SEMA_UNKNOWN_SIZE;
      out->alignment = element->alignment ? element->alignment : 1u;
      out->hash =
          sema_hash_type2(AST_SEMA_TYPE_VECTOR, element, dependent.hash);
    } else {
      out->as.vector.lanes = lanes;
      if (element->size != AST_SEMA_UNKNOWN_SIZE &&
          lanes <= UINT64_MAX / (element->size ? element->size : 1u)) {
        out->size = element->size * lanes;
        out->alignment = out->size >= 16u
                             ? 16u
                             : (element->alignment ? element->alignment : 1u);
        out->flags = AST_SEMA_TYPEF_COMPLETE;
      }
      out->hash = sema_hash_type2(AST_SEMA_TYPE_VECTOR, element, lanes);
    }
    return out;
  }
  case AST_SEMA_TYPE_MASK: {
    if (!type->as.mask.dependent)
      return type;
    uint64_t lanes = 0u;
    if (type->as.mask.dependent_expr) {
      AstSemaModule *module =
          sema_module_by_file_id(s, type->as.mask.dependent_expr->span.file_id);
      if (module &&
          sema_eval_u64_with_bindings(s, module, type->as.mask.dependent_expr,
                                      (AstSemaGenericBinding *)values,
                                      binding_count, &lanes)) {
        if (lanes == 0u)
          return sema_builtin_error(s);
        AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_MASK);
        out->as.mask.lanes = lanes;
        out->bit_width = (uint32_t)(lanes > UINT32_MAX ? UINT32_MAX : lanes);
        out->size = (lanes + 7u) / 8u;
        out->alignment = 1u;
        out->flags = AST_SEMA_TYPEF_COMPLETE;
        out->hash = sema_mix64(AST_SEMA_TYPE_MASK ^ lanes);
        return out;
      }
      return type;
    }
    AstName dependent;
    memset(&dependent, 0, sizeof(dependent));
    int is_dependent = 0;
    if (!sema_substitute_dependent_count(template_bindings, values,
                                         binding_count, type->as.mask.parameter,
                                         &lanes, &dependent, &is_dependent))
      return type;
    AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_MASK);
    if (is_dependent) {
      out->as.mask.dependent = 1u;
      out->as.mask.parameter = dependent;
      out->size = AST_SEMA_UNKNOWN_SIZE;
      out->alignment = 1u;
      out->hash = sema_mix64(AST_SEMA_TYPE_MASK ^ dependent.hash);
    } else {
      out->as.mask.lanes = lanes;
      out->bit_width = (uint32_t)(lanes > UINT32_MAX ? UINT32_MAX : lanes);
      out->size = (lanes + 7u) / 8u;
      out->alignment = 1u;
      out->flags = AST_SEMA_TYPEF_COMPLETE;
      out->hash = sema_mix64(AST_SEMA_TYPE_MASK ^ lanes);
    }
    return out;
  }
  case AST_SEMA_TYPE_SVECTOR:
  case AST_SEMA_TYPE_PRED: {
    const AstSemaType *element =
        sema_substitute_type(s, type->as.scalable.element, template_bindings,
                             values, binding_count, span, file_id, depth + 1u);
    if (element == type->as.scalable.element)
      return type;
    AstSemaType *out = (AstSemaType *)sema_new_type(s, type->kind);
    out->as.scalable.element = element;
    out->size = AST_SEMA_UNKNOWN_SIZE;
    out->alignment = 1u;
    out->hash = sema_hash_type2(type->kind, element, 0u);
    return out;
  }
  case AST_SEMA_TYPE_TUPLE: {
    int changed = 0;
    const AstSemaType **items = NULL;
    if (type->as.tuple.count) {
      items = (const AstSemaType **)sema_arena_alloc(
          s, (size_t)type->as.tuple.count * sizeof(*items),
          ASTER_ALIGNOF(AstSemaType *));
      if (!items)
        return sema_builtin_error(s);
    }
    uint64_t size = 0u;
    uint32_t alignment = 1u;
    for (uint32_t i = 0u; i < type->as.tuple.count; ++i) {
      items[i] = sema_substitute_type(s, type->as.tuple.items[i],
                                      template_bindings, values, binding_count,
                                      span, file_id, depth + 1u);
      changed |= items[i] != type->as.tuple.items[i];
      uint32_t a = items[i]->alignment ? items[i]->alignment : 1u;
      if (a > alignment)
        alignment = a;
      if (items[i]->size == AST_SEMA_UNKNOWN_SIZE)
        size = AST_SEMA_UNKNOWN_SIZE;
      else if (size != AST_SEMA_UNKNOWN_SIZE) {
        uint64_t pad = (a - (size % a)) % a;
        if (size > UINT64_MAX - pad - items[i]->size)
          size = AST_SEMA_UNKNOWN_SIZE;
        else
          size += pad + items[i]->size;
      }
    }
    if (!changed)
      return type;
    if (size != AST_SEMA_UNKNOWN_SIZE)
      size += (alignment - (size % alignment)) % alignment;
    AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_TUPLE);
    out->as.tuple.items = items;
    out->as.tuple.count = type->as.tuple.count;
    out->size = size;
    out->alignment = alignment;
    if (size != AST_SEMA_UNKNOWN_SIZE)
      out->flags = AST_SEMA_TYPEF_COMPLETE;
    return out;
  }
  case AST_SEMA_TYPE_STRUCT: {
    int changed = 0;
    AstSemaType *out = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_STRUCT);
    out->as.record.name = type->as.record.name;
    out->as.record.decl = type->as.record.decl;
    out->as.record.generic_arg_count = type->as.record.generic_arg_count;
    if (type->as.record.generic_arg_count) {
      out->as.record.generic_type_args = (const AstSemaType **)sema_arena_alloc(
          s,
          (size_t)type->as.record.generic_arg_count *
              sizeof(*out->as.record.generic_type_args),
          ASTER_ALIGNOF(AstSemaType *));
      out->as.record.generic_const_args = (AstSemaConstValue *)sema_arena_alloc(
          s,
          (size_t)type->as.record.generic_arg_count *
              sizeof(*out->as.record.generic_const_args),
          ASTER_ALIGNOF(AstSemaConstValue));
      out->as.record.generic_const_parameters = (AstName *)sema_arena_alloc(
          s,
          (size_t)type->as.record.generic_arg_count *
              sizeof(*out->as.record.generic_const_parameters),
          ASTER_ALIGNOF(AstName));
      out->as.record.generic_const_dependent = (uint8_t *)sema_arena_alloc(
          s,
          (size_t)type->as.record.generic_arg_count *
              sizeof(*out->as.record.generic_const_dependent),
          ASTER_ALIGNOF(uint8_t));
      if (!out->as.record.generic_type_args ||
          !out->as.record.generic_const_args ||
          !out->as.record.generic_const_parameters ||
          !out->as.record.generic_const_dependent)
        return sema_builtin_error(s);
      for (uint32_t i = 0u; i < type->as.record.generic_arg_count; ++i) {
        const AstSemaType *arg = type->as.record.generic_type_args
                                     ? type->as.record.generic_type_args[i]
                                     : NULL;
        if (arg) {
          out->as.record.generic_type_args[i] =
              sema_substitute_type(s, arg, template_bindings, values,
                                   binding_count, span, file_id, depth + 1u);
          out->as.record.generic_const_args[i] = sema_const_invalid();
          changed |= out->as.record.generic_type_args[i] != arg;
        } else {
          out->as.record.generic_type_args[i] = NULL;
          uint8_t dependent = type->as.record.generic_const_dependent
                                  ? type->as.record.generic_const_dependent[i]
                                  : 0u;
          if (dependent && type->as.record.generic_const_parameters) {
            uint64_t concrete = 0u;
            AstName dependent_name;
            memset(&dependent_name, 0, sizeof(dependent_name));
            int remains_dependent = 0;
            if (sema_substitute_dependent_count(
                    template_bindings, values, binding_count,
                    type->as.record.generic_const_parameters[i], &concrete,
                    &dependent_name, &remains_dependent)) {
              changed = 1;
              if (remains_dependent) {
                out->as.record.generic_const_args[i] = sema_const_invalid();
                out->as.record.generic_const_parameters[i] = dependent_name;
                out->as.record.generic_const_dependent[i] = 1u;
              } else {
                const AstSemaType *const_type =
                    type->as.record.generic_const_args[i].type;
                out->as.record.generic_const_args[i] = sema_const_u64(
                    concrete, const_type ? const_type : sema_builtin_usize(s));
              }
            } else {
              out->as.record.generic_const_args[i] =
                  type->as.record.generic_const_args[i];
              out->as.record.generic_const_parameters[i] =
                  type->as.record.generic_const_parameters[i];
              out->as.record.generic_const_dependent[i] = 1u;
            }
          } else {
            out->as.record.generic_const_args[i] =
                type->as.record.generic_const_args[i];
          }
        }
      }
    }
    out->as.record.field_count = type->as.record.field_count;
    if (type->as.record.field_count) {
      out->as.record.fields = (AstSemaField *)sema_arena_alloc(
          s,
          (size_t)type->as.record.field_count * sizeof(*out->as.record.fields),
          ASTER_ALIGNOF(AstSemaField));
      if (!out->as.record.fields)
        return sema_builtin_error(s);
      for (uint32_t i = 0u; i < type->as.record.field_count; ++i) {
        out->as.record.fields[i].name = type->as.record.fields[i].name;
        out->as.record.fields[i].type = sema_substitute_type(
            s, type->as.record.fields[i].type, template_bindings, values,
            binding_count, span, file_id, depth + 1u);
        changed |=
            out->as.record.fields[i].type != type->as.record.fields[i].type;
      }
    }
    if (!changed)
      return type;
    (void)sema_compute_layout(s, out, span, file_id, depth + 1u);
    return out;
  }
  case AST_SEMA_TYPE_FUNCTION: {
    const AstSemaType **params = NULL;
    int changed = 0;
    if (type->as.function.param_count) {
      params = (const AstSemaType **)sema_arena_alloc(
          s, (size_t)type->as.function.param_count * sizeof(*params),
          ASTER_ALIGNOF(AstSemaType *));
      if (!params)
        return sema_builtin_error(s);
    }
    for (uint32_t i = 0u; i < type->as.function.param_count; ++i) {
      params[i] = sema_substitute_type(s, type->as.function.params[i],
                                       template_bindings, values, binding_count,
                                       span, file_id, depth + 1u);
      changed |= params[i] != type->as.function.params[i];
    }
    const AstSemaType *result =
        sema_substitute_type(s, type->as.function.result, template_bindings,
                             values, binding_count, span, file_id, depth + 1u);
    changed |= result != type->as.function.result;
    return changed ? sema_make_function(s, params,
                                        type->as.function.param_count, result)
                   : type;
  }
  default:
    return type;
  }
}

static AstSemaImport *sema_find_import(AstSemaModule *m, const char *text,
                                       uint32_t len) {
  for (size_t i = 0u; i < m->import_count; ++i) {
    AstSemaImport *imp = &m->imports[i];
    if ((imp->alias.span.length == len &&
         memcmp(imp->alias.text, text, len) == 0) ||
        (imp->module_name.span.length == len &&
         memcmp(imp->module_name.text, text, len) == 0))
      return imp;
  }
  return NULL;
}

static AstSemaSymbol *sema_resolve_named_type_symbol(AstSemaModule *m,
                                                     AstName name,
                                                     int *cross_module) {
  *cross_module = 0;
  AstSemaSymbol *local = sema_module_find_type(m, name);
  if (local)
    return local;
  const char *dot = NULL;
  for (uint32_t i = 0u; i < name.span.length; ++i)
    if (name.text[i] == '.')
      dot = name.text + i;
  if (!dot)
    return NULL;
  uint32_t prefix_len = (uint32_t)(dot - name.text);
  uint32_t member_len = name.span.length - prefix_len - 1u;
  AstSemaImport *imp = sema_find_import(m, name.text, prefix_len);
  if (!imp || !imp->target || !member_len)
    return NULL;
  AstName member = name;
  member.text = dot + 1;
  member.span.offset += prefix_len + 1u;
  member.span.length = member_len;
  member.hash = ast_hash_bytes(member.text, member_len);
  AstSemaSymbol *sym = sema_module_find_type(imp->target, member);
  if (sym)
    *cross_module = 1;
  return sym;
}

static const AstSemaType *
sema_resolve_symbol_type(AstSema *s, AstSemaModule *m, AstSemaSymbol *sym,
                         AstSemaGenericBinding *bindings, size_t binding_count,
                         const AstTypeArgArray *args, AstSpan use_span,
                         uint32_t depth);

static int sema_eval_u64_with_bindings(AstSema *s, AstSemaModule *m,
                                       const AstExpr *expr,
                                       AstSemaGenericBinding *bindings,
                                       size_t binding_count, uint64_t *out) {
  if (expr && expr->kind == AST_EXPR_NAME) {
    AstSemaGenericBinding *b =
        sema_find_binding(bindings, binding_count, expr->as.name.name);
    if (b && b->is_const && sema_const_to_u64(b->const_value, out))
      return 1;
  }
  AstSemaConstCtx cc;
  memset(&cc, 0, sizeof(cc));
  cc.sema = s;
  cc.module = m;
  AstSemaFnCtx fake;
  memset(&fake, 0, sizeof(fake));
  fake.sema = s;
  fake.module = m;
  fake.generics = bindings;
  fake.generic_count = binding_count;
  cc.fn = &fake;
  AstSemaConstValue v = sema_eval_const_expr(&cc, expr, sema_builtin_usize(s));
  return sema_const_to_u64(v, out);
}

static AstSemaGenericBinding *
sema_dependent_const_binding(const AstExpr *expr,
                             AstSemaGenericBinding *bindings,
                             size_t binding_count) {
  if (!expr || expr->kind != AST_EXPR_NAME)
    return NULL;
  AstSemaGenericBinding *binding =
      sema_find_binding(bindings, binding_count, expr->as.name.name);
  if (!binding || !binding->is_const ||
      sema_const_is_valid(binding->const_value))
    return NULL;
  return binding;
}

static const AstSemaType *
sema_resolve_type_impl(AstSema *s, AstSemaModule *m, const AstType *type,
                       AstSemaGenericBinding *bindings, size_t binding_count,
                       uint32_t depth);

static const AstSemaType *
sema_resolve_type_impl_raw(AstSema *s, AstSemaModule *m, const AstType *type,
                           AstSemaGenericBinding *bindings,
                           size_t binding_count, uint32_t depth) {
  if (!type)
    return sema_builtin_void(s);
  if (depth > s->config.recursion_limit) {
    sema_diag(s, AST_SEMA_DIAG_RECURSIVE_TYPE, AST_DIAG_ERROR, type->span,
              m->file.id, "type resolution exceeded the recursion limit");
    return sema_builtin_error(s);
  }
  switch (type->kind) {
  case AST_TYPE_ERROR:
    return sema_builtin_error(s);
  case AST_TYPE_PRIMITIVE:
    return sema_builtin_for_token(s, type->as.primitive.token_kind);
  case AST_TYPE_PTR:
  case AST_TYPE_CAP: {
    const AstSemaType *elem = sema_resolve_type_impl(
        s, m, type->as.pointer.element, bindings, binding_count, depth + 1u);
    return sema_make_pointer(
        s, type->kind == AST_TYPE_PTR ? AST_SEMA_TYPE_PTR : AST_SEMA_TYPE_CAP,
        elem, type->as.pointer.qualifiers);
  }
  case AST_TYPE_BITS: {
    uint64_t width = 0u;
    AstSemaGenericBinding *dependent = sema_dependent_const_binding(
        type->as.bits.width, bindings, binding_count);
    int symbolic = dependent != NULL;
    if (!symbolic &&
        !sema_eval_u64_with_bindings(s, m, type->as.bits.width, bindings,
                                     binding_count, &width)) {
      symbolic = sema_expr_depends_on_unresolved_const(
          type->as.bits.width, bindings, binding_count, 0u);
    }
    if (!symbolic && (width == 0u || width > UINT32_MAX)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ARRAY_SIZE, AST_DIAG_ERROR, type->span,
                m->file.id,
                "bits width must be a positive compile-time integer");
      return sema_builtin_error(s);
    }
    if (!symbolic &&
        !sema_eval_u64_with_bindings(s, m, type->as.bits.width, bindings,
                                     binding_count, &width)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ARRAY_SIZE, AST_DIAG_ERROR, type->span,
                m->file.id,
                "bits width must be a positive compile-time integer");
      return sema_builtin_error(s);
    }
    AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_BITS);
    if (symbolic) {
      t->as.bits.dependent = 1u;
      if (dependent)
        t->as.bits.parameter = dependent->name;
      else
        t->as.bits.dependent_expr = type->as.bits.width;
      t->size = AST_SEMA_UNKNOWN_SIZE;
      t->alignment = 1u;
      t->hash = sema_mix64(
          AST_SEMA_TYPE_BITS ^
          (dependent ? dependent->name.hash
                     : sema_dependent_expr_hash(type->as.bits.width)));
      return t;
    }
    t->as.bits.width = width;
    t->bit_width = (uint32_t)width;
    t->size = (width + 7u) / 8u;
    t->alignment = t->size >= 16u ? 16u : (uint32_t)(t->size ? t->size : 1u);
    t->flags = AST_SEMA_TYPEF_COMPLETE;
    t->hash = sema_mix64(AST_SEMA_TYPE_BITS ^ width);
    return t;
  }
  case AST_TYPE_VEC: {
    uint64_t lanes = 0u;
    AstSemaGenericBinding *dependent = sema_dependent_const_binding(
        type->as.vector.lanes, bindings, binding_count);
    int symbolic = dependent != NULL;
    if (!symbolic &&
        !sema_eval_u64_with_bindings(s, m, type->as.vector.lanes, bindings,
                                     binding_count, &lanes)) {
      symbolic = sema_expr_depends_on_unresolved_const(
          type->as.vector.lanes, bindings, binding_count, 0u);
    }
    if (!symbolic && (lanes == 0u || !sema_eval_u64_with_bindings(
                                         s, m, type->as.vector.lanes, bindings,
                                         binding_count, &lanes))) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ARRAY_SIZE, AST_DIAG_ERROR, type->span,
                m->file.id,
                "vector lane count must be a positive compile-time integer");
      return sema_builtin_error(s);
    }
    const AstSemaType *elem = sema_resolve_type_impl(
        s, m, type->as.vector.element, bindings, binding_count, depth + 1u);
    if (elem->kind != AST_SEMA_TYPE_ERROR && !sema_type_is_lane_scalar(elem) &&
        elem->kind != AST_SEMA_TYPE_TYPE_PARAMETER) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                type->as.vector.element->span, m->file.id,
                "vector element type must be an integer, bits<N>, or "
                "floating-point scalar");
      return sema_builtin_error(s);
    }
    AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_VECTOR);
    t->as.vector.element = elem;
    if (symbolic) {
      t->as.vector.dependent = 1u;
      if (dependent)
        t->as.vector.parameter = dependent->name;
      else
        t->as.vector.dependent_expr = type->as.vector.lanes;
      t->size = AST_SEMA_UNKNOWN_SIZE;
      t->alignment = elem->alignment ? elem->alignment : 1u;
      t->hash = sema_hash_type2(
          AST_SEMA_TYPE_VECTOR, elem,
          dependent ? dependent->name.hash
                    : sema_dependent_expr_hash(type->as.vector.lanes));
      return t;
    }
    t->as.vector.lanes = lanes;
    if (elem->size != AST_SEMA_UNKNOWN_SIZE &&
        lanes <= UINT64_MAX / (elem->size ? elem->size : 1u)) {
      t->size = elem->size * lanes;
      t->alignment =
          t->size >= 16u ? 16u : (elem->alignment ? elem->alignment : 1u);
      t->flags = AST_SEMA_TYPEF_COMPLETE;
    }
    t->hash = sema_hash_type2(AST_SEMA_TYPE_VECTOR, elem, lanes);
    return t;
  }
  case AST_TYPE_MASK: {
    uint64_t lanes = 0u;
    AstSemaGenericBinding *dependent = sema_dependent_const_binding(
        type->as.mask.lanes, bindings, binding_count);
    int symbolic = dependent != NULL;
    if (!symbolic &&
        !sema_eval_u64_with_bindings(s, m, type->as.mask.lanes, bindings,
                                     binding_count, &lanes)) {
      symbolic = sema_expr_depends_on_unresolved_const(
          type->as.mask.lanes, bindings, binding_count, 0u);
    }
    if (!symbolic && (lanes == 0u || !sema_eval_u64_with_bindings(
                                         s, m, type->as.mask.lanes, bindings,
                                         binding_count, &lanes))) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ARRAY_SIZE, AST_DIAG_ERROR, type->span,
                m->file.id,
                "mask lane count must be a positive compile-time integer");
      return sema_builtin_error(s);
    }
    AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_MASK);
    if (symbolic) {
      t->as.mask.dependent = 1u;
      if (dependent)
        t->as.mask.parameter = dependent->name;
      else
        t->as.mask.dependent_expr = type->as.mask.lanes;
      t->size = AST_SEMA_UNKNOWN_SIZE;
      t->alignment = 1u;
      t->hash = sema_mix64(
          AST_SEMA_TYPE_MASK ^
          (dependent ? dependent->name.hash
                     : sema_dependent_expr_hash(type->as.mask.lanes)));
      return t;
    }
    t->as.mask.lanes = lanes;
    t->bit_width = (uint32_t)(lanes > UINT32_MAX ? UINT32_MAX : lanes);
    t->size = (lanes + 7u) / 8u;
    t->alignment = 1u;
    t->flags = AST_SEMA_TYPEF_COMPLETE;
    t->hash = sema_mix64(AST_SEMA_TYPE_MASK ^ lanes);
    return t;
  }
  case AST_TYPE_SVEC:
  case AST_TYPE_PRED: {
    const AstSemaType *elem = sema_resolve_type_impl(
        s, m, type->as.scalable.element, bindings, binding_count, depth + 1u);
    if (elem->kind != AST_SEMA_TYPE_ERROR && !sema_type_is_lane_scalar(elem) &&
        elem->kind != AST_SEMA_TYPE_TYPE_PARAMETER) {
      sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                type->as.scalable.element->span, m->file.id,
                "scalable vector/predicate element type must be an integer, "
                "bits<N>, or floating-point scalar");
      return sema_builtin_error(s);
    }
    AstSemaType *t = (AstSemaType *)sema_new_type(s, type->kind == AST_TYPE_SVEC
                                                         ? AST_SEMA_TYPE_SVECTOR
                                                         : AST_SEMA_TYPE_PRED);
    t->as.scalable.element = elem;
    t->size = AST_SEMA_UNKNOWN_SIZE;
    t->alignment = 1u;
    t->hash = sema_hash_type2(t->kind, elem, 0u);
    return t;
  }
  case AST_TYPE_ARRAY: {
    uint64_t count = 0u;
    AstSemaGenericBinding *dependent = sema_dependent_const_binding(
        type->as.array.count, bindings, binding_count);
    int symbolic = dependent != NULL;
    if (!symbolic &&
        !sema_eval_u64_with_bindings(s, m, type->as.array.count, bindings,
                                     binding_count, &count)) {
      symbolic = sema_expr_depends_on_unresolved_const(
          type->as.array.count, bindings, binding_count, 0u);
    }
    if (!symbolic &&
        !sema_eval_u64_with_bindings(s, m, type->as.array.count, bindings,
                                     binding_count, &count)) {
      sema_diag(s, AST_SEMA_DIAG_INVALID_ARRAY_SIZE, AST_DIAG_ERROR, type->span,
                m->file.id,
                "array size must be a compile-time unsigned integer");
      return sema_builtin_error(s);
    }
    const AstSemaType *elem = sema_resolve_type_impl(
        s, m, type->as.array.element, bindings, binding_count, depth + 1u);
    if (!symbolic)
      return sema_make_array(s, elem, count);
    AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_ARRAY);
    t->as.array.element = elem;
    t->as.array.dependent = 1u;
    if (dependent)
      t->as.array.parameter = dependent->name;
    else
      t->as.array.dependent_expr = type->as.array.count;
    t->size = AST_SEMA_UNKNOWN_SIZE;
    t->alignment = elem->alignment ? elem->alignment : 1u;
    t->hash = sema_hash_type2(
        AST_SEMA_TYPE_ARRAY, elem,
        dependent ? dependent->name.hash
                  : sema_dependent_expr_hash(type->as.array.count));
    return t;
  }
  case AST_TYPE_TUPLE: {
    AstSemaType *t = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_TUPLE);
    uint32_t count = type->as.tuple.elements.count;
    if (count) {
      const AstSemaType **items = (const AstSemaType **)sema_arena_alloc(
          s, (size_t)count * sizeof(*items), ASTER_ALIGNOF(AstSemaType *));
      if (!items)
        return sema_builtin_error(s);
      uint64_t size = 0u;
      uint32_t align = 1u;
      for (uint32_t i = 0u; i < count; ++i) {
        items[i] =
            sema_resolve_type_impl(s, m, type->as.tuple.elements.items[i],
                                   bindings, binding_count, depth + 1u);
        uint32_t a = items[i]->alignment ? items[i]->alignment : 1u;
        if (a > align)
          align = a;
        if (items[i]->size == AST_SEMA_UNKNOWN_SIZE)
          size = AST_SEMA_UNKNOWN_SIZE;
        else if (size != AST_SEMA_UNKNOWN_SIZE) {
          uint64_t pad = (a - (size % a)) % a;
          if (size > UINT64_MAX - pad - items[i]->size)
            size = AST_SEMA_UNKNOWN_SIZE;
          else
            size += pad + items[i]->size;
        }
      }
      if (size != AST_SEMA_UNKNOWN_SIZE)
        size += (align - (size % align)) % align;
      t->as.tuple.items = items;
      t->as.tuple.count = count;
      t->size = size;
      t->alignment = align;
      if (size != AST_SEMA_UNKNOWN_SIZE)
        t->flags = AST_SEMA_TYPEF_COMPLETE;
    } else {
      t->size = 0u;
      t->alignment = 1u;
      t->flags = AST_SEMA_TYPEF_COMPLETE;
    }
    return t;
  }
  case AST_TYPE_NAMED: {
    AstSemaGenericBinding *binding =
        sema_find_binding(bindings, binding_count, type->as.named.name);
    if (binding && !binding->is_const)
      return binding->type_value ? binding->type_value : sema_builtin_error(s);
    int cross = 0;
    AstSemaSymbol *sym =
        sema_resolve_named_type_symbol(m, type->as.named.name, &cross);
    if (!sym) {
      sema_diag(s, AST_SEMA_DIAG_UNKNOWN_TYPE, AST_DIAG_ERROR, type->span,
                m->file.id, "unknown type '%.*s'",
                (int)type->as.named.name.span.length, type->as.named.name.text);
      return sema_builtin_error(s);
    }
    if (cross && !(sym->flags & AST_SEMA_SYMF_EXPORTED)) {
      sema_diag(s, AST_SEMA_DIAG_SYMBOL_NOT_EXPORTED, AST_DIAG_ERROR,
                type->span, m->file.id, "type '%.*s' is not exported",
                (int)type->as.named.name.span.length, type->as.named.name.text);
      return sema_builtin_error(s);
    }
    return sema_resolve_symbol_type(
        s, cross ? sema_module_by_file_id(s, sym->file_id) : m, sym, bindings,
        binding_count, &type->as.named.args, type->span, depth + 1u);
  }
  default:
    return sema_builtin_error(s);
  }
}

static const AstSemaType *
sema_resolve_symbol_type(AstSema *s, AstSemaModule *m, AstSemaSymbol *sym,
                         AstSemaGenericBinding *outer, size_t outer_count,
                         const AstTypeArgArray *args, AstSpan use_span,
                         uint32_t depth) {
  if (!sym || !sym->decl || sym->decl->kind != AST_DECL_TYPE)
    return sym && sym->type ? sym->type : sema_builtin_error(s);
  const AstDecl *d = sym->decl;
  uint32_t gp_count = d->as.type_decl.generics.count;
  uint32_t arg_count = args ? args->count : 0u;
  if (gp_count == 0u) {
    if (arg_count != 0u) {
      sema_diag(s, AST_SEMA_DIAG_GENERIC_ARITY, AST_DIAG_ERROR, use_span,
                m->file.id,
                "non-generic type '%.*s' does not accept type arguments",
                (int)sym->name.span.length, sym->name.text);
      return sema_builtin_error(s);
    }
    if ((sym->flags & AST_SEMA_SYMF_RESOLVED) && sym->type)
      return sym->type;
    if (sym->flags & AST_SEMA_SYMF_RESOLVING) {
      if (sym->type && sym->type->kind == AST_SEMA_TYPE_STRUCT)
        return sym->type;
      sema_diag(s, AST_SEMA_DIAG_RECURSIVE_TYPE, AST_DIAG_ERROR, use_span,
                m->file.id, "illegal recursive type alias '%.*s'",
                (int)sym->name.span.length, sym->name.text);
      return sema_builtin_error(s);
    }
    sym->flags |= AST_SEMA_SYMF_RESOLVING;
    const AstSemaType *resolved = NULL;
    if (d->as.type_decl.is_struct) {
      AstSemaType *rec = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_STRUCT);
      rec->as.record.name = d->as.type_decl.name;
      rec->as.record.decl = d;
      sym->type = rec;
      uint32_t n = d->as.type_decl.fields.count;
      if (n) {
        rec->as.record.fields = (AstSemaField *)sema_arena_alloc(
            s, (size_t)n * sizeof(AstSemaField), ASTER_ALIGNOF(AstSemaField));
        if (!rec->as.record.fields)
          return sema_builtin_error(s);
      }
      rec->as.record.field_count = n;
      for (uint32_t i = 0u; i < n; ++i) {
        AstField *f = &d->as.type_decl.fields.items[i];
        for (uint32_t j = 0u; j < i; ++j)
          if (sema_name_eq(rec->as.record.fields[j].name, f->name)) {
            sema_diag(s, AST_SEMA_DIAG_DUPLICATE_FIELD, AST_DIAG_ERROR,
                      f->name.span, m->file.id, "duplicate field '%.*s'",
                      (int)f->name.span.length, f->name.text);
          }
        rec->as.record.fields[i].name = f->name;
        rec->as.record.fields[i].type = sema_resolve_type_impl(
            s, m, f->type, outer, outer_count, depth + 1u);
        if (!sema_type_is_value_type(rec->as.record.fields[i].type)) {
          sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                    f->type->span, m->file.id,
                    "struct field type must be a value type");
        }
      }
      sema_compute_layout(s, rec, d->span, m->file.id, depth + 1u);
      resolved = rec;
    } else {
      resolved = sema_resolve_type_impl(s, m, d->as.type_decl.type, outer,
                                        outer_count, depth + 1u);
    }
    sym->type = resolved;
    sym->flags &= ~AST_SEMA_SYMF_RESOLVING;
    sym->flags |= AST_SEMA_SYMF_RESOLVED;
    return resolved;
  }

  if (arg_count != gp_count) {
    sema_diag(s, AST_SEMA_DIAG_GENERIC_ARITY, AST_DIAG_ERROR, use_span,
              m->file.id,
              "generic type '%.*s' expects %u arguments but received %u",
              (int)sym->name.span.length, sym->name.text, gp_count, arg_count);
    return sema_builtin_error(s);
  }

  size_t total = outer_count + gp_count;
  AstSemaGenericBinding *bindings = (AstSemaGenericBinding *)sema_arena_alloc(
      s, total * sizeof(*bindings), ASTER_ALIGNOF(AstSemaGenericBinding));
  if (!bindings)
    return sema_builtin_error(s);
  if (outer_count)
    memcpy(bindings, outer, outer_count * sizeof(*bindings));
  for (uint32_t i = 0u; i < gp_count; ++i) {
    AstGenericParam *gp = &d->as.type_decl.generics.items[i];
    AstTypeArg *arg = &args->items[i];
    AstSemaGenericBinding *binding = &bindings[outer_count + i];
    memset(binding, 0, sizeof(*binding));
    binding->name = gp->name;
    if (gp->kind == AST_GENERIC_TYPE) {
      if (arg->kind != AST_TYPE_ARG_TYPE) {
        sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, arg->span,
                  m->file.id,
                  "generic parameter '%.*s' expects a type argument",
                  (int)gp->name.span.length, gp->name.text);
        binding->type_value = sema_builtin_error(s);
      } else {
        binding->type_value = sema_resolve_type_impl(s, m, arg->as.type, outer,
                                                     outer_count, depth + 1u);
      }
    } else {
      binding->is_const = 1u;
      const AstSemaType *const_type = sema_resolve_type_impl(
          s, m, gp->const_type, outer, outer_count, depth + 1u);
      binding->const_value = sema_const_invalid();
      binding->const_value.type = const_type;
      int handled_dependent = 0;
      if (arg->kind == AST_TYPE_ARG_TYPE && arg->as.type &&
          arg->as.type->kind == AST_TYPE_NAMED &&
          arg->as.type->as.named.args.count == 0u) {
        AstSemaGenericBinding *outer_binding =
            sema_find_binding(outer, outer_count, arg->as.type->as.named.name);
        if (outer_binding && outer_binding->is_const) {
          binding->const_value = outer_binding->const_value;
          if (!binding->const_value.type)
            binding->const_value.type = const_type;
          binding->has_dependent_const = 1u;
          binding->dependent_const = outer_binding->has_dependent_const
                                         ? outer_binding->dependent_const
                                         : outer_binding->name;
          handled_dependent = !sema_const_is_valid(binding->const_value);
        } else {
          AstExpr fake;
          memset(&fake, 0, sizeof(fake));
          fake.kind = AST_EXPR_NAME;
          fake.span = arg->span;
          fake.as.name.name = arg->as.type->as.named.name;
          AstSemaConstCtx cc;
          memset(&cc, 0, sizeof(cc));
          cc.sema = s;
          cc.module = m;
          binding->const_value = sema_eval_const_expr(&cc, &fake, const_type);
        }
      } else if (arg->kind == AST_TYPE_ARG_CONST) {
        AstSemaConstCtx cc;
        memset(&cc, 0, sizeof(cc));
        cc.sema = s;
        cc.module = m;
        AstSemaFnCtx fake;
        memset(&fake, 0, sizeof(fake));
        fake.sema = s;
        fake.module = m;
        fake.generics = outer;
        fake.generic_count = outer_count;
        cc.fn = &fake;
        binding->const_value =
            sema_eval_const_expr(&cc, arg->as.expr, const_type);
      } else {
        sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, arg->span,
                  m->file.id,
                  "generic parameter '%.*s' expects a constant argument",
                  (int)gp->name.span.length, gp->name.text);
      }
      if (!handled_dependent) {
        if (!sema_const_is_valid(binding->const_value)) {
          sema_diag(s, AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, AST_DIAG_ERROR,
                    arg->span, m->file.id,
                    "generic constant argument must be compile-time evaluable");
        } else {
          AstExprInfo value_info;
          memset(&value_info, 0, sizeof(value_info));
          value_info.type = binding->const_value.type;
          value_info.constant = binding->const_value;
          if (!sema_type_assignable(s, const_type, value_info, arg->span,
                                    m->file.id)) {
            sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR, arg->span,
                      m->file.id,
                      "generic constant argument is not representable by its "
                      "parameter type");
          }
        }
      }
    }
  }

  AstSemaTypeInstantiation *found = NULL;
  for (AstSemaTypeInstantiation *it = sym->instantiations; it; it = it->next) {
    if (it->arg_count != gp_count)
      continue;
    int equal = 1;
    for (uint32_t i = 0u; i < gp_count; ++i) {
      AstGenericParam *gp = &d->as.type_decl.generics.items[i];
      AstSemaGenericBinding *binding = &bindings[outer_count + i];
      if (gp->kind == AST_GENERIC_TYPE) {
        if (!sema_type_equal(it->type_args[i], binding->type_value)) {
          equal = 0;
          break;
        }
      } else {
        if (it->const_dependent && binding->has_dependent_const &&
            it->const_dependent[i]) {
          if (!sema_name_eq(it->const_parameters[i],
                            binding->dependent_const)) {
            equal = 0;
            break;
          }
          continue;
        }
        if ((it->const_dependent && it->const_dependent[i]) !=
            binding->has_dependent_const) {
          equal = 0;
          break;
        }
        AstSemaConstValue a = it->const_args[i];
        AstSemaConstValue b = binding->const_value;
        uint32_t mask = AST_SEMA_CONSTF_VALID | AST_SEMA_CONSTF_SIGNED |
                        AST_SEMA_CONSTF_BOOL | AST_SEMA_CONSTF_NEGATIVE |
                        AST_SEMA_CONSTF_FLOAT;
        if ((a.flags & mask) != (b.flags & mask) || a.lo != b.lo ||
            a.hi != b.hi ||
            ((a.flags & AST_SEMA_CONSTF_FLOAT) && a.floating != b.floating)) {
          equal = 0;
          break;
        }
      }
    }
    if (equal) {
      found = it;
      break;
    }
  }
  if (found) {
    if (found->type)
      return found->type;
    if (found->resolving) {
      sema_diag(
          s, AST_SEMA_DIAG_RECURSIVE_TYPE, AST_DIAG_ERROR, use_span, m->file.id,
          "recursive generic type alias '%.*s' has no indirection boundary",
          (int)sym->name.span.length, sym->name.text);
    }
    return sema_builtin_error(s);
  }

  AstSemaTypeInstantiation *inst = (AstSemaTypeInstantiation *)sema_arena_alloc(
      s, sizeof(*inst), ASTER_ALIGNOF(AstSemaTypeInstantiation));
  if (!inst)
    return sema_builtin_error(s);
  inst->arg_count = gp_count;
  if (gp_count) {
    inst->type_args = (const AstSemaType **)sema_arena_alloc(
        s, (size_t)gp_count * sizeof(*inst->type_args),
        ASTER_ALIGNOF(AstSemaType *));
    inst->const_args = (AstSemaConstValue *)sema_arena_alloc(
        s, (size_t)gp_count * sizeof(*inst->const_args),
        ASTER_ALIGNOF(AstSemaConstValue));
    inst->const_parameters = (AstName *)sema_arena_alloc(
        s, (size_t)gp_count * sizeof(*inst->const_parameters),
        ASTER_ALIGNOF(AstName));
    inst->const_dependent = (uint8_t *)sema_arena_alloc(
        s, (size_t)gp_count * sizeof(*inst->const_dependent),
        ASTER_ALIGNOF(uint8_t));
    if (!inst->type_args || !inst->const_args || !inst->const_parameters ||
        !inst->const_dependent)
      return sema_builtin_error(s);
    for (uint32_t i = 0u; i < gp_count; ++i) {
      AstSemaGenericBinding *binding = &bindings[outer_count + i];
      inst->type_args[i] = binding->is_const ? NULL : binding->type_value;
      inst->const_args[i] =
          binding->is_const ? binding->const_value : sema_const_invalid();
      if (binding->is_const && binding->has_dependent_const &&
          !sema_const_is_valid(binding->const_value)) {
        inst->const_parameters[i] = binding->dependent_const;
        inst->const_dependent[i] = 1u;
      }
    }
  }
  inst->next = sym->instantiations;
  sym->instantiations = inst;
  inst->resolving = 1u;

  if (d->as.type_decl.is_struct) {
    AstSemaType *record = (AstSemaType *)sema_new_type(s, AST_SEMA_TYPE_STRUCT);
    record->as.record.name = d->as.type_decl.name;
    record->as.record.decl = d;
    record->as.record.generic_arg_count = gp_count;
    record->as.record.generic_type_args = inst->type_args;
    record->as.record.generic_const_args = inst->const_args;
    record->as.record.generic_const_parameters = inst->const_parameters;
    record->as.record.generic_const_dependent = inst->const_dependent;
    inst->type = record;
    uint32_t field_count = d->as.type_decl.fields.count;
    if (field_count) {
      record->as.record.fields = (AstSemaField *)sema_arena_alloc(
          s, (size_t)field_count * sizeof(AstSemaField),
          ASTER_ALIGNOF(AstSemaField));
      if (!record->as.record.fields)
        return sema_builtin_error(s);
    }
    record->as.record.field_count = field_count;
    for (uint32_t i = 0u; i < field_count; ++i) {
      AstField *field = &d->as.type_decl.fields.items[i];
      for (uint32_t j = 0u; j < i; ++j) {
        if (sema_name_eq(record->as.record.fields[j].name, field->name)) {
          sema_diag(s, AST_SEMA_DIAG_DUPLICATE_FIELD, AST_DIAG_ERROR,
                    field->name.span, m->file.id, "duplicate field '%.*s'",
                    (int)field->name.span.length, field->name.text);
        }
      }
      record->as.record.fields[i].name = field->name;
      record->as.record.fields[i].type = sema_resolve_type_impl(
          s, m, field->type, bindings, total, depth + 1u);
      if (!sema_type_is_value_type(record->as.record.fields[i].type)) {
        sema_diag(s, AST_SEMA_DIAG_TYPE_MISMATCH, AST_DIAG_ERROR,
                  field->type->span, m->file.id,
                  "struct field type must be a value type");
      }
    }
    sema_compute_layout(s, record, d->span, m->file.id, depth + 1u);
    inst->resolving = 0u;
    return record;
  }

  const AstSemaType *resolved = sema_resolve_type_impl(
      s, m, d->as.type_decl.type, bindings, total, depth + 1u);
  inst->type = (AstSemaType *)resolved;
  inst->resolving = 0u;
  return resolved;
}

static const AstSemaType *
sema_resolve_type_impl(AstSema *s, AstSemaModule *m, const AstType *type,
                       AstSemaGenericBinding *bindings, size_t binding_count,
                       uint32_t depth) {
  const AstSemaType *resolved =
      sema_resolve_type_impl_raw(s, m, type, bindings, binding_count, depth);
  if (type)
    (void)sema_record_type_fact(s, type, resolved);
  return resolved;
}

const AstSemaType *sema_resolve_type_with_bindings(
    AstSema *s, AstSemaModule *m, const AstType *type,
    AstSemaGenericBinding *bindings, size_t binding_count, uint32_t depth) {
  return sema_resolve_type_impl(s, m, type, bindings, binding_count, depth);
}

const AstSemaType *sema_resolve_type(AstSema *s, AstSemaModule *m,
                                     const AstType *type, AstSemaFnCtx *fn,
                                     uint32_t depth) {
  return sema_resolve_type_impl(s, m, type, fn ? fn->generics : NULL,
                                fn ? fn->generic_count : 0u, depth);
}

int sema_type_is_dependent(const AstSemaType *type, uint32_t depth) {
  if (!type || depth > 256u)
    return 0;
  switch (type->kind) {
  case AST_SEMA_TYPE_TYPE_PARAMETER:
    return 1;
  case AST_SEMA_TYPE_BITS:
    return type->as.bits.dependent != 0u;
  case AST_SEMA_TYPE_VECTOR:
    return type->as.vector.dependent != 0u ||
           sema_type_is_dependent(type->as.vector.element, depth + 1u);
  case AST_SEMA_TYPE_MASK:
    return type->as.mask.dependent != 0u;
  case AST_SEMA_TYPE_ARRAY:
    return type->as.array.dependent != 0u ||
           sema_type_is_dependent(type->as.array.element, depth + 1u);
  case AST_SEMA_TYPE_TUPLE:
    for (uint32_t i = 0u; i < type->as.tuple.count; ++i) {
      if (sema_type_is_dependent(type->as.tuple.items[i], depth + 1u))
        return 1;
    }
    return 0;
  case AST_SEMA_TYPE_STRUCT:
    if (type->as.record.generic_type_args) {
      for (uint32_t i = 0u; i < type->as.record.generic_arg_count; ++i) {
        if (type->as.record.generic_type_args[i] &&
            sema_type_is_dependent(type->as.record.generic_type_args[i],
                                   depth + 1u))
          return 1;
        if (type->as.record.generic_const_dependent &&
            type->as.record.generic_const_dependent[i])
          return 1;
      }
    }
    return 0;
  default:
    return 0;
  }
}

int sema_compute_layout(AstSema *s, AstSemaType *type, AstSpan span,
                        uint32_t file_id, uint32_t depth) {
  if (!type)
    return -1;
  if (depth > s->config.recursion_limit)
    return -1;
  if (type->kind != AST_SEMA_TYPE_STRUCT)
    return type->size == AST_SEMA_UNKNOWN_SIZE ? -1 : 0;
  uint64_t offset = 0u;
  uint32_t max_align = 1u;
  const AstDecl *decl = type->as.record.decl;
  AstSemaModule *module = sema_module_by_file_id(s, file_id);
  for (uint32_t i = 0u; i < type->as.record.field_count; ++i) {
    AstSemaField *f = &type->as.record.fields[i];
    if (!f->type || f->type->size == AST_SEMA_UNKNOWN_SIZE) {
      if (f->type && sema_type_is_dependent(f->type, 0u)) {
        type->size = AST_SEMA_UNKNOWN_SIZE;
        type->alignment = max_align;
        return 0;
      }
      sema_diag(s, AST_SEMA_DIAG_RECURSIVE_TYPE, AST_DIAG_ERROR, f->name.span,
                file_id, "field '%.*s' has an incomplete type",
                (int)f->name.span.length, f->name.text);
      type->size = AST_SEMA_UNKNOWN_SIZE;
      return -1;
    }
    uint32_t a = f->type->alignment ? f->type->alignment : 1u;
    if (decl && decl->kind == AST_DECL_TYPE &&
        i < decl->as.type_decl.fields.count && module) {
      const AstAttribute *field_align = sema_find_attribute(
          decl->as.type_decl.fields.items[i].attributes, "align");
      uint64_t explicit_align = 0u;
      if (field_align &&
          sema_attribute_const_u64(s, module, NULL, field_align,
                                   &explicit_align) &&
          explicit_align <= UINT32_MAX && explicit_align > a) {
        a = (uint32_t)explicit_align;
      }
    }
    if (a > max_align)
      max_align = a;
    uint64_t pad = (a - (offset % a)) % a;
    if (offset > UINT64_MAX - pad ||
        offset + pad > UINT64_MAX - f->type->size) {
      sema_diag(s, AST_SEMA_DIAG_RECURSIVE_TYPE, AST_DIAG_ERROR, span, file_id,
                "type layout exceeds representable size");
      return -1;
    }
    offset += pad;
    f->offset = offset;
    offset += f->type->size;
  }
  if (decl && decl->kind == AST_DECL_TYPE && module) {
    const AstAttribute *type_align =
        sema_find_attribute(decl->as.type_decl.struct_attributes, "align");
    uint64_t explicit_align = 0u;
    if (type_align &&
        sema_attribute_const_u64(s, module, NULL, type_align,
                                 &explicit_align) &&
        explicit_align <= UINT32_MAX && explicit_align > max_align) {
      max_align = (uint32_t)explicit_align;
    }
  }
  offset += (max_align - (offset % max_align)) % max_align;
  type->size = offset;
  type->alignment = max_align;
  type->flags |= AST_SEMA_TYPEF_COMPLETE;
  return 0;
}
