#include "sema_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>

AstSemaConstValue sema_const_invalid(void) {
  AstSemaConstValue v;
  memset(&v, 0, sizeof(v));
  return v;
}

AstSemaConstValue sema_const_u64(uint64_t value, const AstSemaType *type) {
  AstSemaConstValue v;
  memset(&v, 0, sizeof(v));
  v.lo = value;
  v.flags = AST_SEMA_CONSTF_VALID;
  v.type = type;
  return v;
}

static AstSemaConstValue sema_const_float(double value,
                                          const AstSemaType *type) {
  AstSemaConstValue v;
  memset(&v, 0, sizeof(v));
  v.floating = value;
  v.flags = AST_SEMA_CONSTF_VALID | AST_SEMA_CONSTF_FLOAT;
  v.type = type;
  return v;
}

int sema_const_is_zero(AstSemaConstValue value) {
  if (!sema_const_is_valid(value))
    return 0;
  if (value.flags & AST_SEMA_CONSTF_FLOAT)
    return value.floating == 0.0;
  return value.lo == 0u && value.hi == 0u;
}

int sema_const_is_valid(AstSemaConstValue value) {
  return (value.flags & AST_SEMA_CONSTF_VALID) != 0u;
}

int sema_const_to_u64(AstSemaConstValue value, uint64_t *out) {
  if (!sema_const_is_valid(value) ||
      (value.flags & (AST_SEMA_CONSTF_FLOAT | AST_SEMA_CONSTF_NEGATIVE)) ||
      value.hi != 0u)
    return 0;
  if (out)
    *out = value.lo;
  return 1;
}

static int digit_value(uint8_t c, uint32_t *out) {
  if (c >= '0' && c <= '9') {
    *out = (uint32_t)(c - '0');
    return 1;
  }
  if (c >= 'a' && c <= 'f') {
    *out = (uint32_t)(c - 'a') + 10u;
    return 1;
  }
  if (c >= 'A' && c <= 'F') {
    *out = (uint32_t)(c - 'A') + 10u;
    return 1;
  }
  return 0;
}

static int u128_is_zero(AstSemaConstValue v) {
  return v.lo == 0u && v.hi == 0u;
}

static int u128_cmp_mag(AstSemaConstValue a, AstSemaConstValue b) {
  if (a.hi < b.hi)
    return -1;
  if (a.hi > b.hi)
    return 1;
  if (a.lo < b.lo)
    return -1;
  if (a.lo > b.lo)
    return 1;
  return 0;
}

static int u128_add_mag(AstSemaConstValue a, AstSemaConstValue b,
                        AstSemaConstValue *out) {
  uint64_t lo = a.lo + b.lo;
  uint64_t carry = lo < a.lo;
  if (a.hi > UINT64_MAX - b.hi - carry)
    return 0;
  out->lo = lo;
  out->hi = a.hi + b.hi + carry;
  return 1;
}

static int u128_sub_mag(AstSemaConstValue a, AstSemaConstValue b,
                        AstSemaConstValue *out) {
  if (u128_cmp_mag(a, b) < 0)
    return 0;
  uint64_t borrow = a.lo < b.lo;
  out->lo = a.lo - b.lo;
  out->hi = a.hi - b.hi - borrow;
  return 1;
}

static void u128_sub_wrap(AstSemaConstValue a, AstSemaConstValue b,
                          AstSemaConstValue *out) {
  uint64_t borrow = a.lo < b.lo;
  out->lo = a.lo - b.lo;
  out->hi = a.hi - b.hi - borrow;
}

static int u128_shift_left_one(AstSemaConstValue *v) {
  if (v->hi & UINT64_C(0x8000000000000000))
    return 0;
  v->hi = (v->hi << 1u) | (v->lo >> 63u);
  v->lo <<= 1u;
  return 1;
}

static void u128_shift_right_one(AstSemaConstValue *v) {
  v->lo = (v->lo >> 1u) | (v->hi << 63u);
  v->hi >>= 1u;
}

static int u128_add_small(uint64_t *hi, uint64_t *lo, uint32_t add) {
  uint64_t old = *lo;
  *lo += add;
  if (*lo < old) {
    if (*hi == UINT64_MAX)
      return 0;
    (*hi)++;
  }
  return 1;
}

static int u128_mul_small_exact(uint64_t *hi, uint64_t *lo, uint32_t mul) {
  AstSemaConstValue a = sema_const_u64(*lo, NULL);
  AstSemaConstValue b = sema_const_u64(mul, NULL);
  AstSemaConstValue result = sema_const_u64(0u, NULL);
  a.hi = *hi;
  while (!u128_is_zero(b)) {
    if (b.lo & 1u) {
      AstSemaConstValue next = result;
      if (!u128_add_mag(result, a, &next))
        return 0;
      result = next;
    }
    u128_shift_right_one(&b);
    if (!u128_is_zero(b) && !u128_shift_left_one(&a))
      return 0;
  }
  *hi = result.hi;
  *lo = result.lo;
  return 1;
}

static int u128_mul_mag(AstSemaConstValue a, AstSemaConstValue b,
                        AstSemaConstValue *out) {
  AstSemaConstValue result = sema_const_u64(0u, a.type);
  while (!u128_is_zero(b)) {
    if (b.lo & 1u) {
      AstSemaConstValue next = result;
      if (!u128_add_mag(result, a, &next))
        return 0;
      result = next;
    }
    u128_shift_right_one(&b);
    if (!u128_is_zero(b) && !u128_shift_left_one(&a))
      return 0;
  }
  *out = result;
  return 1;
}

static uint32_t u128_get_bit(AstSemaConstValue v, uint32_t bit) {
  return bit < 64u ? (uint32_t)((v.lo >> bit) & 1u)
                   : (uint32_t)((v.hi >> (bit - 64u)) & 1u);
}

static void u128_set_bit(AstSemaConstValue *v, uint32_t bit) {
  if (bit < 64u)
    v->lo |= UINT64_C(1) << bit;
  else
    v->hi |= UINT64_C(1) << (bit - 64u);
}

static int u128_divmod_mag(AstSemaConstValue dividend,
                           AstSemaConstValue divisor,
                           AstSemaConstValue *quotient,
                           AstSemaConstValue *remainder) {
  if (u128_is_zero(divisor))
    return 0;
  AstSemaConstValue q = sema_const_u64(0u, dividend.type);
  AstSemaConstValue r = sema_const_u64(0u, dividend.type);
  for (uint32_t bit = 128u; bit > 0u; --bit) {
    uint32_t index = bit - 1u;
    uint32_t carry = (uint32_t)(r.hi >> 63u);
    r.hi = (r.hi << 1u) | (r.lo >> 63u);
    r.lo = (r.lo << 1u) | u128_get_bit(dividend, index);
    if (carry || u128_cmp_mag(r, divisor) >= 0) {
      AstSemaConstValue next = r;
      u128_sub_wrap(r, divisor, &next);
      r = next;
      u128_set_bit(&q, index);
    }
  }
  if (quotient)
    *quotient = q;
  if (remainder)
    *remainder = r;
  return 1;
}

static AstSemaConstValue u128_shift_left(AstSemaConstValue a, uint32_t shift) {
  AstSemaConstValue r = a;
  if (shift >= 128u)
    return sema_const_invalid();
  if (shift == 0u)
    return r;
  if (shift >= 64u) {
    r.hi = a.lo << (shift - 64u);
    r.lo = 0u;
  } else {
    r.hi = (a.hi << shift) | (a.lo >> (64u - shift));
    r.lo = a.lo << shift;
  }
  return r;
}

static AstSemaConstValue u128_shift_right(AstSemaConstValue a, uint32_t shift) {
  AstSemaConstValue r = a;
  if (shift >= 128u)
    return sema_const_invalid();
  if (shift == 0u)
    return r;
  if (shift >= 64u) {
    r.lo = a.hi >> (shift - 64u);
    r.hi = 0u;
  } else {
    r.lo = (a.lo >> shift) | (a.hi << (64u - shift));
    r.hi = a.hi >> shift;
  }
  return r;
}

static AstSemaConstValue parse_integer_literal(AstSema *s,
                                               const AstExpr *expr) {
  size_t n = 0u;
  const uint8_t *p = sema_span_bytes(s, expr->span, &n);
  if (!p || n == 0u)
    return sema_const_invalid();
  uint32_t base = 10u;
  size_t i = 0u;
  if (n >= 2u && p[0] == '0') {
    if (p[1] == 'x' || p[1] == 'X') {
      base = 16u;
      i = 2u;
    } else if (p[1] == 'b' || p[1] == 'B') {
      base = 2u;
      i = 2u;
    } else if (p[1] == 'o' || p[1] == 'O') {
      base = 8u;
      i = 2u;
    }
  }
  uint64_t hi = 0u, lo = 0u;
  int saw = 0;
  for (; i < n; ++i) {
    if (p[i] == '_')
      continue;
    uint32_t d = 0u;
    if (!digit_value(p[i], &d) || d >= base)
      break;
    saw = 1;
    if (!u128_mul_small_exact(&hi, &lo, base) || !u128_add_small(&hi, &lo, d)) {
      sema_diag(s, AST_SEMA_DIAG_INTEGER_LITERAL_OVERFLOW, AST_DIAG_ERROR,
                expr->span, expr->span.file_id,
                "integer literal exceeds 128-bit semantic literal range");
      return sema_const_invalid();
    }
  }
  if (!saw)
    return sema_const_invalid();
  AstSemaConstValue v = sema_const_u64(lo, sema_builtin_untyped_int(s));
  v.hi = hi;
  return v;
}

static double pow10_integer(int exponent) {
  double result = 1.0;
  double base = exponent < 0 ? 0.1 : 10.0;
  uint32_t power = (uint32_t)(exponent < 0 ? -(int64_t)exponent : exponent);
  while (power) {
    if (power & 1u)
      result *= base;
    power >>= 1u;
    if (power)
      base *= base;
  }
  return result;
}

static AstSemaConstValue parse_float_literal(AstSema *s, const AstExpr *expr) {
  size_t n = 0u;
  const uint8_t *p = sema_span_bytes(s, expr->span, &n);
  if (!p || n == 0u)
    return sema_const_invalid();
  double value = 0.0;
  double fraction_scale = 1.0;
  int in_fraction = 0;
  int exponent_sign = 1;
  int exponent = 0;
  int in_exponent = 0;
  for (size_t i = 0u; i < n; ++i) {
    uint8_t c = p[i];
    if (c == '_')
      continue;
    if (!in_exponent && c == '.') {
      in_fraction = 1;
      continue;
    }
    if (!in_exponent && (c == 'e' || c == 'E')) {
      in_exponent = 1;
      continue;
    }
    if (in_exponent && (c == '+' || c == '-') && exponent == 0) {
      exponent_sign = c == '-' ? -1 : 1;
      continue;
    }
    if (c < '0' || c > '9')
      return sema_const_invalid();
    uint32_t digit = (uint32_t)(c - '0');
    if (in_exponent) {
      if (exponent < 100000)
        exponent = exponent * 10 + (int)digit;
    } else if (in_fraction) {
      fraction_scale *= 0.1;
      value += (double)digit * fraction_scale;
    } else {
      value = value * 10.0 + (double)digit;
    }
  }
  if (exponent)
    value *= pow10_integer(exponent_sign * exponent);
  if (!AST_SEMA_ISFINITE(value))
    return sema_const_invalid();
  return sema_const_float(value, sema_builtin_untyped_float(s));
}

static AstSemaConstValue parse_char_literal(AstSema *s, const AstExpr *expr) {
  size_t n = 0u;
  const uint8_t *p = sema_span_bytes(s, expr->span, &n);
  if (!p || n < 3u || p[0] != '\'' || p[n - 1u] != '\'')
    return sema_const_invalid();
  const uint8_t *q = p + 1u;
  size_t len = n - 2u;
  uint32_t cp = 0u;
  size_t consumed = 0u;
  if (q[0] != '\\') {
    uint32_t width = 0u;
    if (!ast_utf8_decode_one(q, len, &cp, &width) || width != len)
      return sema_const_invalid();
    consumed = width;
  } else {
    if (len < 2u)
      return sema_const_invalid();
    switch (q[1]) {
    case '\\':
      cp = '\\';
      consumed = 2u;
      break;
    case '\'':
      cp = '\'';
      consumed = 2u;
      break;
    case '"':
      cp = '"';
      consumed = 2u;
      break;
    case 'n':
      cp = '\n';
      consumed = 2u;
      break;
    case 'r':
      cp = '\r';
      consumed = 2u;
      break;
    case 't':
      cp = '\t';
      consumed = 2u;
      break;
    case '0':
      cp = 0u;
      consumed = 2u;
      break;
    case 'a':
      cp = 7u;
      consumed = 2u;
      break;
    case 'b':
      cp = 8u;
      consumed = 2u;
      break;
    case 'f':
      cp = 12u;
      consumed = 2u;
      break;
    case 'v':
      cp = 11u;
      consumed = 2u;
      break;
    case 'x': {
      if (len != 4u)
        return sema_const_invalid();
      uint32_t a = 0u, b = 0u;
      if (!digit_value(q[2], &a) || !digit_value(q[3], &b))
        return sema_const_invalid();
      cp = (a << 4u) | b;
      consumed = 4u;
      break;
    }
    case 'u': {
      if (len < 5u || q[2] != '{' || q[len - 1u] != '}')
        return sema_const_invalid();
      uint32_t value = 0u;
      for (size_t i = 3u; i + 1u < len; ++i) {
        uint32_t d = 0u;
        if (!digit_value(q[i], &d))
          return sema_const_invalid();
        if (value > UINT32_C(0x10ffff) / 16u)
          return sema_const_invalid();
        value = value * 16u + d;
      }
      if (value > UINT32_C(0x10ffff) ||
          (value >= UINT32_C(0xd800) && value <= UINT32_C(0xdfff)))
        return sema_const_invalid();
      cp = value;
      consumed = len;
      break;
    }
    default:
      return sema_const_invalid();
    }
  }
  if (consumed != len)
    return sema_const_invalid();
  return sema_const_u64(cp, sema_builtin_untyped_int(s));
}

static AstSemaConstValue const_bool(AstSema *s, int value) {
  AstSemaConstValue v = sema_const_u64(value ? 1u : 0u, sema_builtin_bool(s));
  v.flags |= AST_SEMA_CONSTF_BOOL;
  return v;
}

static int const_truthy(AstSemaConstValue value) {
  if (value.flags & AST_SEMA_CONSTF_FLOAT)
    return value.floating != 0.0;
  return !u128_is_zero(value);
}

static double const_to_double(AstSemaConstValue value) {
  if (value.flags & AST_SEMA_CONSTF_FLOAT)
    return value.floating;
  double d = (double)value.hi * 18446744073709551616.0 + (double)value.lo;
  return (value.flags & AST_SEMA_CONSTF_NEGATIVE) ? -d : d;
}

static AstSemaConstValue const_float_binary(AstSemaConstCtx *ctx,
                                            const AstExpr *expr,
                                            AstSemaConstValue a,
                                            AstSemaConstValue b) {
  AstSema *s = ctx->sema;
  AstTokenKind op = expr->as.binary.op;
  double av = const_to_double(a);
  double bv = const_to_double(b);
  if (op == AST_TOK_EQ_EQ)
    return const_bool(s, av == bv);
  if (op == AST_TOK_BANG_EQ)
    return const_bool(s, av != bv);
  if (op == AST_TOK_LT)
    return const_bool(s, av < bv);
  if (op == AST_TOK_LT_EQ)
    return const_bool(s, av <= bv);
  if (op == AST_TOK_GT)
    return const_bool(s, av > bv);
  if (op == AST_TOK_GT_EQ)
    return const_bool(s, av >= bv);
  if (op == AST_TOK_AMP_AMP)
    return const_bool(s, av != 0.0 && bv != 0.0);
  if (op == AST_TOK_PIPE_PIPE)
    return const_bool(s, av != 0.0 || bv != 0.0);
  if (op == AST_TOK_PLUS)
    return sema_const_float(av + bv, sema_builtin_untyped_float(s));
  if (op == AST_TOK_MINUS)
    return sema_const_float(av - bv, sema_builtin_untyped_float(s));
  if (op == AST_TOK_STAR)
    return sema_const_float(av * bv, sema_builtin_untyped_float(s));
  if (op == AST_TOK_SLASH)
    return sema_const_float(av / bv, sema_builtin_untyped_float(s));
  return sema_const_invalid();
}

static uint32_t const_integer_width(const AstSemaType *type) {
  if (!type)
    return 0u;
  if (type->kind == AST_SEMA_TYPE_INT)
    return type->bit_width;
  if (type->kind == AST_SEMA_TYPE_BITS && !type->as.bits.dependent &&
      type->as.bits.width <= 128u)
    return (uint32_t)type->as.bits.width;
  return 0u;
}

static void const_mask_to_width(AstSemaConstValue *value, uint32_t bits) {
  if (bits < 64u) {
    value->hi = 0u;
    value->lo &= bits == 0u ? 0u : ((UINT64_C(1) << bits) - 1u);
  } else if (bits == 64u) {
    value->hi = 0u;
  } else if (bits < 128u) {
    value->hi &= (UINT64_C(1) << (bits - 64u)) - 1u;
  }
}

static int const_sign_bit(AstSemaConstValue value, uint32_t bits) {
  if (bits == 0u || bits > 128u)
    return 0;
  if (bits <= 64u)
    return (int)((value.lo >> (bits - 1u)) & 1u);
  return (int)((value.hi >> (bits - 65u)) & 1u);
}

static void const_twos_complement(AstSemaConstValue *value, uint32_t bits) {
  value->lo = ~value->lo + 1u;
  value->hi = ~value->hi + (value->lo == 0u ? 1u : 0u);
  const_mask_to_width(value, bits);
}

static AstSemaConstValue const_wrap_integer(AstSemaConstValue value,
                                            const AstSemaType *type) {
  uint32_t bits = const_integer_width(type);
  if (bits == 0u || bits > 128u || !sema_const_is_valid(value) ||
      (value.flags & AST_SEMA_CONSTF_FLOAT))
    return value;

  uint32_t original_flags = value.flags;
  int was_negative = (original_flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
  value.flags = AST_SEMA_CONSTF_VALID;
  if (type->flags & AST_SEMA_TYPEF_SIGNED)
    value.flags |= AST_SEMA_CONSTF_SIGNED;
  value.type = type;
  const_mask_to_width(&value, bits);
  if (was_negative && (value.lo != 0u || value.hi != 0u))
    const_twos_complement(&value, bits);

  if ((type->flags & AST_SEMA_TYPEF_SIGNED) && const_sign_bit(value, bits)) {
    const_twos_complement(&value, bits);
    if (value.lo != 0u || value.hi != 0u)
      value.flags |= AST_SEMA_CONSTF_NEGATIVE;
  }
  return value;
}

static int const_expr_wraps_integer(const AstExpr *expr) {
  if (!expr)
    return 0;
  if (expr->kind == AST_EXPR_UNARY) {
    return expr->as.unary.op == AST_TOK_PLUS ||
           expr->as.unary.op == AST_TOK_MINUS ||
           expr->as.unary.op == AST_TOK_TILDE;
  }
  if (expr->kind != AST_EXPR_BINARY)
    return 0;
  switch (expr->as.binary.op) {
  case AST_TOK_PLUS:
  case AST_TOK_MINUS:
  case AST_TOK_STAR:
  case AST_TOK_SLASH:
  case AST_TOK_PERCENT:
  case AST_TOK_AMP:
  case AST_TOK_PIPE:
  case AST_TOK_CARET:
  case AST_TOK_SHIFT_LEFT:
  case AST_TOK_SHIFT_RIGHT:
    return 1;
  default:
    return 0;
  }
}

static AstSemaConstValue const_integer_bits(AstSemaConstValue value,
                                            const AstSemaType *type);
static AstSemaConstValue const_bits_to_integer(AstSemaConstValue bits,
                                               const AstSemaType *type);

static uint32_t u128_mod_small(AstSemaConstValue value, uint32_t divisor) {
  if (divisor == 0u)
    return 0u;
  uint32_t remainder = 0u;
  for (uint32_t bit = 128u; bit > 0u; --bit) {
    remainder =
        (uint32_t)(((uint64_t)remainder * 2u + u128_get_bit(value, bit - 1u)) %
                   divisor);
  }
  return remainder;
}

static const AstSemaType *const_binary_integer_type(const AstSemaType *expected,
                                                    AstSemaConstValue a,
                                                    AstSemaConstValue b) {
  if (expected && const_integer_width(expected) != 0u)
    return expected;
  if (a.type && const_integer_width(a.type) != 0u)
    return a.type;
  if (b.type && const_integer_width(b.type) != 0u)
    return b.type;
  return NULL;
}

static AstSemaConstValue const_bitwise_binary(AstSemaConstValue a,
                                              AstSemaConstValue b,
                                              const AstSemaType *type,
                                              AstTokenKind op) {
  uint32_t width = const_integer_width(type);
  AstSemaConstValue ab = const_integer_bits(a, type);
  AstSemaConstValue bb = const_integer_bits(b, type);
  if (width == 0u || !sema_const_is_valid(ab) || !sema_const_is_valid(bb))
    return sema_const_invalid();
  AstSemaConstValue result = sema_const_u64(0u, type);
  if (op == AST_TOK_AMP) {
    result.lo = ab.lo & bb.lo;
    result.hi = ab.hi & bb.hi;
  } else if (op == AST_TOK_PIPE) {
    result.lo = ab.lo | bb.lo;
    result.hi = ab.hi | bb.hi;
  } else if (op == AST_TOK_CARET) {
    result.lo = ab.lo ^ bb.lo;
    result.hi = ab.hi ^ bb.hi;
  } else {
    return sema_const_invalid();
  }
  const_mask_to_width(&result, width);
  return const_bits_to_integer(result, type);
}

static AstSemaConstValue const_shift_binary(AstSemaConstValue a,
                                            AstSemaConstValue b,
                                            const AstSemaType *type,
                                            AstTokenKind op) {
  uint32_t width = const_integer_width(type);
  if (width == 0u || width > 128u || !sema_const_is_valid(b) ||
      (b.flags & AST_SEMA_CONSTF_FLOAT)) {
    return sema_const_invalid();
  }
  AstSemaConstValue value = const_integer_bits(a, type);
  if (!sema_const_is_valid(value))
    return sema_const_invalid();
  AstSemaConstValue count_bits = b;
  if (b.flags & AST_SEMA_CONSTF_NEGATIVE) {
    const AstSemaType *count_type =
        b.type && const_integer_width(b.type) != 0u ? b.type : type;
    count_bits = const_integer_bits(b, count_type);
    if (!sema_const_is_valid(count_bits))
      return sema_const_invalid();
  }
  uint32_t shift = u128_mod_small(count_bits, width);
  if (shift == 0u)
    return const_bits_to_integer(value, type);
  AstSemaConstValue result = sema_const_u64(0u, type);
  if (op == AST_TOK_SHIFT_LEFT) {
    for (uint32_t i = 0u; i < width; ++i) {
      if (!u128_get_bit(value, i))
        continue;
      uint32_t dst = i + shift;
      if (dst < width)
        u128_set_bit(&result, dst);
    }
  } else {
    int arithmetic = (type->flags & AST_SEMA_TYPEF_SIGNED) != 0u;
    int sign = arithmetic && u128_get_bit(value, width - 1u) != 0u;
    for (uint32_t i = 0u; i < width; ++i) {
      uint32_t src = i + shift;
      int bit = src < width ? (int)u128_get_bit(value, src) : sign;
      if (bit)
        u128_set_bit(&result, i);
    }
  }
  return const_bits_to_integer(result, type);
}

static AstSemaConstValue const_binary(AstSemaConstCtx *ctx, const AstExpr *expr,
                                      AstSemaConstValue a, AstSemaConstValue b,
                                      const AstSemaType *expected) {
  AstSema *s = ctx->sema;
  if (!sema_const_is_valid(a) || !sema_const_is_valid(b))
    return sema_const_invalid();
  if ((a.flags | b.flags) & AST_SEMA_CONSTF_FLOAT)
    return const_float_binary(ctx, expr, a, b);
  AstTokenKind op = expr->as.binary.op;
  if (op == AST_TOK_EQ_EQ || op == AST_TOK_BANG_EQ || op == AST_TOK_LT ||
      op == AST_TOK_LT_EQ || op == AST_TOK_GT || op == AST_TOK_GT_EQ) {
    if ((a.flags & AST_SEMA_CONSTF_NEGATIVE) !=
        (b.flags & AST_SEMA_CONSTF_NEGATIVE)) {
      int an = (a.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
      int value = op == AST_TOK_EQ_EQ     ? 0
                  : op == AST_TOK_BANG_EQ ? 1
                  : op == AST_TOK_LT      ? an
                  : op == AST_TOK_LT_EQ   ? an
                  : op == AST_TOK_GT      ? !an
                                          : !an;
      return const_bool(s, value);
    }
    int cmp = u128_cmp_mag(a, b);
    if (a.flags & AST_SEMA_CONSTF_NEGATIVE)
      cmp = -cmp;
    int value = op == AST_TOK_EQ_EQ     ? cmp == 0
                : op == AST_TOK_BANG_EQ ? cmp != 0
                : op == AST_TOK_LT      ? cmp < 0
                : op == AST_TOK_LT_EQ   ? cmp <= 0
                : op == AST_TOK_GT      ? cmp > 0
                                        : cmp >= 0;
    return const_bool(s, value);
  }
  if (op == AST_TOK_AMP_AMP || op == AST_TOK_PIPE_PIPE) {
    int av = const_truthy(a), bv = const_truthy(b);
    return const_bool(s, op == AST_TOK_AMP_AMP ? (av && bv) : (av || bv));
  }
  AstSemaConstValue r =
      sema_const_u64(0u, a.type ? a.type : sema_builtin_untyped_int(s));
  r.flags |= (a.flags | b.flags) & AST_SEMA_CONSTF_SIGNED;
  if (op == AST_TOK_PLUS || op == AST_TOK_MINUS) {
    int an = (a.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
    int bn = (b.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
    if (op == AST_TOK_MINUS)
      bn = !bn;
    if (an == bn) {
      if (!u128_add_mag(a, b, &r))
        return sema_const_invalid();
      if (an)
        r.flags |= AST_SEMA_CONSTF_NEGATIVE;
    } else {
      int cmp = u128_cmp_mag(a, b);
      if (cmp >= 0) {
        if (!u128_sub_mag(a, b, &r))
          return sema_const_invalid();
        if (an)
          r.flags |= AST_SEMA_CONSTF_NEGATIVE;
      } else {
        if (!u128_sub_mag(b, a, &r))
          return sema_const_invalid();
        if (bn)
          r.flags |= AST_SEMA_CONSTF_NEGATIVE;
      }
    }
    if (u128_is_zero(r))
      r.flags &= ~AST_SEMA_CONSTF_NEGATIVE;
    return r;
  }
  if (op == AST_TOK_STAR) {
    int negative = ((a.flags ^ b.flags) & AST_SEMA_CONSTF_NEGATIVE) != 0u;
    if (!u128_mul_mag(a, b, &r))
      return sema_const_invalid();
    if (negative && !u128_is_zero(r))
      r.flags |= AST_SEMA_CONSTF_NEGATIVE;
    return r;
  }
  if (op == AST_TOK_SLASH || op == AST_TOK_PERCENT) {
    AstSemaConstValue q, rem;
    if (!u128_divmod_mag(a, b, &q, &rem))
      return sema_const_invalid();
    if (op == AST_TOK_SLASH) {
      q.flags = r.flags | AST_SEMA_CONSTF_VALID;
      if (((a.flags ^ b.flags) & AST_SEMA_CONSTF_NEGATIVE) && !u128_is_zero(q))
        q.flags |= AST_SEMA_CONSTF_NEGATIVE;
      return q;
    }
    rem.flags = r.flags | AST_SEMA_CONSTF_VALID;
    if ((a.flags & AST_SEMA_CONSTF_NEGATIVE) && !u128_is_zero(rem))
      rem.flags |= AST_SEMA_CONSTF_NEGATIVE;
    return rem;
  }
  if (op == AST_TOK_AMP || op == AST_TOK_PIPE || op == AST_TOK_CARET) {
    const AstSemaType *integer_type = const_binary_integer_type(expected, a, b);
    return integer_type ? const_bitwise_binary(a, b, integer_type, op)
                        : sema_const_invalid();
  }
  if (op == AST_TOK_SHIFT_LEFT || op == AST_TOK_SHIFT_RIGHT) {
    const AstSemaType *integer_type = const_binary_integer_type(expected, a, b);
    return integer_type ? const_shift_binary(a, b, integer_type, op)
                        : sema_const_invalid();
  }
  return sema_const_invalid();
}

static AstSemaSymbol *find_const_symbol(AstSemaConstCtx *ctx, AstName name) {
  AstSemaSymbol *sym = sema_module_find_value(ctx->module, name);
  if (sym && sym->kind == AST_SEMA_SYMBOL_CONST)
    return sym;
  return NULL;
}

static AstSemaConstValue eval_symbol_const(AstSemaConstCtx *ctx,
                                           AstSemaSymbol *sym) {
  if (sym->flags & AST_SEMA_SYMF_CONST_EVAL)
    return sym->const_value;
  if (sym->flags & AST_SEMA_SYMF_CONST_BUSY) {
    sema_diag(ctx->sema, AST_SEMA_DIAG_CONST_CYCLE, AST_DIAG_ERROR,
              sym->name.span, sym->file_id,
              "constant '%.*s' participates in a dependency cycle",
              (int)sym->name.span.length, sym->name.text);
    return sema_const_invalid();
  }
  sym->flags |= AST_SEMA_SYMF_CONST_BUSY;
  const AstExpr *value = NULL;
  const AstSemaType *expected = sym->type;
  if (sym->data_decl) {
    if (sym->data_decl->initializer &&
        sym->data_decl->initializer->kind == AST_INIT_EXPR)
      value = sym->data_decl->initializer->as.expr;
  } else if (sym->decl && sym->decl->kind == AST_DECL_CONST) {
    value = sym->decl->as.const_decl.value;
  }
  AstSemaConstValue result =
      value ? sema_eval_const_expr(ctx, value, expected) : sema_const_invalid();
  sym->const_value = result;
  sym->flags &= ~AST_SEMA_SYMF_CONST_BUSY;
  sym->flags |= AST_SEMA_SYMF_CONST_EVAL;
  return result;
}

static int literal_string_equals(AstSema *s, const AstExpr *expr,
                                 AstSpan string_span) {
  if (!expr || expr->kind != AST_EXPR_STRING)
    return 0;
  size_t an = 0u, bn = 0u;
  const uint8_t *a = sema_span_bytes(s, expr->span, &an);
  const uint8_t *b = sema_span_bytes(s, string_span, &bn);
  if (!a || !b || an < 2u || bn < 2u || a[0] != '"' || b[0] != '"')
    return 0;
  return an == bn && memcmp(a, b, an) == 0;
}

static AstSemaConstValue eval_feature_call(AstSemaConstCtx *ctx,
                                           const AstExpr *expr) {
  AstExprArray args = expr->as.call.args;
  if (args.count != 1u || !args.items[0] ||
      args.items[0]->kind != AST_EXPR_STRING)
    return sema_const_invalid();
  int enabled = 0;
  if (ctx->module && ctx->module->ast) {
    for (uint32_t i = 0u; i < ctx->module->ast->declarations.count; ++i) {
      const AstDecl *decl = ctx->module->ast->declarations.items[i];
      if (!decl || decl->kind != AST_DECL_TARGET)
        continue;
      for (uint32_t j = 0u; j < decl->as.target_decl.items.count; ++j) {
        const AstTargetItem *item = &decl->as.target_decl.items.items[j];
        if (item->is_feature && literal_string_equals(ctx->sema, args.items[0],
                                                      item->feature_string))
          enabled = item->feature_sign > 0;
      }
    }
  }
  return const_bool(ctx->sema, enabled);
}

static AstSemaConstValue eval_sizeof_call(AstSemaConstCtx *ctx,
                                          const AstExpr *expr) {
  if (expr->as.call.args.count != 1u)
    return sema_const_invalid();
  const AstExpr *arg = expr->as.call.args.items[0];
  const AstSemaType *type =
      sema_resolve_type_operand(ctx->sema, ctx->module, ctx->fn, arg, NULL);
  if (!type && ctx->fn)
    type = sema_check_expr(ctx->fn, arg, NULL).type;
  if (!type || type->kind == AST_SEMA_TYPE_ERROR ||
      type->size == AST_SEMA_UNKNOWN_SIZE)
    return sema_const_invalid();
  return sema_const_u64(type->size, sema_builtin_usize(ctx->sema));
}

static AstSemaConstValue eval_offsetof_call(AstSemaConstCtx *ctx,
                                            const AstExpr *expr) {
  if (expr->as.call.args.count != 2u)
    return sema_const_invalid();
  const AstExpr *type_arg = expr->as.call.args.items[0];
  const AstExpr *field_arg = expr->as.call.args.items[1];
  int is_type_name = 0;
  const AstSemaType *type = sema_resolve_type_operand(
      ctx->sema, ctx->module, ctx->fn, type_arg, &is_type_name);
  if (!type || !is_type_name || type->kind != AST_SEMA_TYPE_STRUCT ||
      !field_arg || field_arg->kind != AST_EXPR_NAME)
    return sema_const_invalid();
  const AstSemaField *field =
      sema_find_struct_field(type, field_arg->as.name.name);
  if (!field)
    return sema_const_invalid();
  return sema_const_u64(field->offset, sema_builtin_usize(ctx->sema));
}

static AstSemaConstValue const_integer_bits(AstSemaConstValue value,
                                            const AstSemaType *type) {
  uint32_t width = const_integer_width(type);
  if (width == 0u || width > 128u || !sema_const_is_valid(value) ||
      (value.flags & AST_SEMA_CONSTF_FLOAT)) {
    return sema_const_invalid();
  }
  AstSemaConstValue bits = value;
  int negative = (bits.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
  bits.flags = AST_SEMA_CONSTF_VALID;
  bits.type = type;
  const_mask_to_width(&bits, width);
  if (negative && !u128_is_zero(bits))
    const_twos_complement(&bits, width);
  return bits;
}

static AstSemaConstValue const_bits_to_integer(AstSemaConstValue bits,
                                               const AstSemaType *type) {
  uint32_t width = const_integer_width(type);
  if (width == 0u || width > 128u)
    return sema_const_invalid();
  bits.flags = AST_SEMA_CONSTF_VALID;
  bits.type = type;
  const_mask_to_width(&bits, width);
  if ((type->flags & AST_SEMA_TYPEF_SIGNED) != 0u &&
      const_sign_bit(bits, width)) {
    const_twos_complement(&bits, width);
    if (!u128_is_zero(bits))
      bits.flags |= AST_SEMA_CONSTF_NEGATIVE | AST_SEMA_CONSTF_SIGNED;
  } else if ((type->flags & AST_SEMA_TYPEF_SIGNED) != 0u) {
    bits.flags |= AST_SEMA_CONSTF_SIGNED;
  }
  return bits;
}

static AstSemaConstValue const_modular_add(AstSemaConstValue a,
                                           AstSemaConstValue b,
                                           const AstSemaType *type,
                                           int subtract) {
  uint32_t width = const_integer_width(type);
  AstSemaConstValue ab = const_integer_bits(a, type);
  AstSemaConstValue bb = const_integer_bits(b, type);
  if (!sema_const_is_valid(ab) || !sema_const_is_valid(bb))
    return sema_const_invalid();
  AstSemaConstValue result = sema_const_u64(0u, type);
  if (subtract) {
    u128_sub_wrap(ab, bb, &result);
  } else {
    result.lo = ab.lo + bb.lo;
    uint64_t carry = result.lo < ab.lo;
    result.hi = ab.hi + bb.hi + carry;
  }
  const_mask_to_width(&result, width);
  return const_bits_to_integer(result, type);
}

static AstSemaConstValue const_modular_mul(AstSemaConstValue a,
                                           AstSemaConstValue b,
                                           const AstSemaType *type) {
  uint32_t width = const_integer_width(type);
  AstSemaConstValue multiplicand = const_integer_bits(a, type);
  AstSemaConstValue multiplier = const_integer_bits(b, type);
  if (!sema_const_is_valid(multiplicand) || !sema_const_is_valid(multiplier))
    return sema_const_invalid();
  AstSemaConstValue result = sema_const_u64(0u, type);
  for (uint32_t bit = 0u; bit < width && !u128_is_zero(multiplier); ++bit) {
    if ((multiplier.lo & 1u) != 0u) {
      uint64_t old_lo = result.lo;
      result.lo += multiplicand.lo;
      result.hi += multiplicand.hi + (result.lo < old_lo ? 1u : 0u);
      const_mask_to_width(&result, width);
    }
    multiplier = u128_shift_right(multiplier, 1u);
    multiplicand = u128_shift_left(multiplicand, 1u);
    const_mask_to_width(&multiplicand, width);
  }
  return const_bits_to_integer(result, type);
}

static AstSemaConstValue const_integer_bound(const AstSemaType *type,
                                             int minimum) {
  uint32_t width = const_integer_width(type);
  if (width == 0u || width > 128u)
    return sema_const_invalid();
  AstSemaConstValue out = sema_const_u64(0u, type);
  int signed_type = (type->flags & AST_SEMA_TYPEF_SIGNED) != 0u;
  if (!signed_type) {
    if (minimum)
      return out;
    out.lo = UINT64_MAX;
    out.hi = UINT64_MAX;
    const_mask_to_width(&out, width);
    return out;
  }
  if (minimum) {
    uint32_t bit = width - 1u;
    if (bit < 64u)
      out.lo = UINT64_C(1) << bit;
    else
      out.hi = UINT64_C(1) << (bit - 64u);
    out.flags |= AST_SEMA_CONSTF_SIGNED | AST_SEMA_CONSTF_NEGATIVE;
    return out;
  }
  uint32_t value_bits = width - 1u;
  if (value_bits >= 128u) {
    out.hi = UINT64_MAX;
    out.lo = UINT64_MAX;
  } else if (value_bits >= 64u) {
    out.lo = UINT64_MAX;
    out.hi = value_bits == 64u ? 0u : (UINT64_C(1) << (value_bits - 64u)) - 1u;
  } else if (value_bits != 0u) {
    out.lo = (UINT64_C(1) << value_bits) - 1u;
  }
  out.flags |= AST_SEMA_CONSTF_SIGNED;
  return out;
}

static int const_fits_integer_type(AstSemaConstValue value,
                                   const AstSemaType *type) {
  uint32_t width = const_integer_width(type);
  if (width == 0u || width > 128u || !sema_const_is_valid(value) ||
      (value.flags & AST_SEMA_CONSTF_FLOAT))
    return 0;
  int negative = (value.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
  int signed_type = (type->flags & AST_SEMA_TYPEF_SIGNED) != 0u;
  if (negative && !signed_type)
    return 0;
  AstSemaConstValue bound = const_integer_bound(type, negative && signed_type);
  return u128_cmp_mag(value, bound) <= 0;
}

static AstSemaConstValue const_saturating_addsub(AstSemaConstValue a,
                                                 AstSemaConstValue b,
                                                 const AstSemaType *type,
                                                 int subtract) {
  if (!sema_const_is_valid(a) || !sema_const_is_valid(b) ||
      (a.flags | b.flags) & AST_SEMA_CONSTF_FLOAT) {
    return sema_const_invalid();
  }
  int an = (a.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
  int bn = (b.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u;
  if (subtract)
    bn = !bn;
  AstSemaConstValue exact = sema_const_u64(0u, type);
  if (an == bn) {
    if (!u128_add_mag(a, b, &exact))
      return const_integer_bound(type, an);
    if (an && !u128_is_zero(exact))
      exact.flags |= AST_SEMA_CONSTF_NEGATIVE;
  } else {
    int cmp = u128_cmp_mag(a, b);
    if (cmp >= 0) {
      (void)u128_sub_mag(a, b, &exact);
      if (an && !u128_is_zero(exact))
        exact.flags |= AST_SEMA_CONSTF_NEGATIVE;
    } else {
      (void)u128_sub_mag(b, a, &exact);
      if (bn && !u128_is_zero(exact))
        exact.flags |= AST_SEMA_CONSTF_NEGATIVE;
    }
  }
  if (!const_fits_integer_type(exact, type)) {
    return const_integer_bound(type,
                               (exact.flags & AST_SEMA_CONSTF_NEGATIVE) != 0u);
  }
  exact.type = type;
  if ((type->flags & AST_SEMA_TYPEF_SIGNED) != 0u)
    exact.flags |= AST_SEMA_CONSTF_SIGNED;
  return exact;
}

static double const_integer_to_double(AstSemaConstValue value) {
  double magnitude = ldexp((double)value.hi, 64) + (double)value.lo;
  return (value.flags & AST_SEMA_CONSTF_NEGATIVE) ? -magnitude : magnitude;
}

static int const_float_fits_format(double value, const AstSemaType *type) {
  if (!type || type->kind != AST_SEMA_TYPE_FLOAT || !AST_SEMA_ISFINITE(value))
    return 0;
  double max_value = DBL_MAX;
  if (type->bit_width == 16u && (type->flags & AST_SEMA_TYPEF_BFLOAT))
    max_value = 3.3895313892515355e38;
  else if (type->bit_width == 16u)
    max_value = 65504.0;
  else if (type->bit_width == 32u)
    max_value = FLT_MAX;
  return fabs(value) <= max_value;
}

static int const_float_format_params(const AstSemaType *type,
                                     uint32_t *precision, int *emin,
                                     int *emax) {
  if (!type || type->kind != AST_SEMA_TYPE_FLOAT)
    return 0;
  if (type->bit_width == 16u && (type->flags & AST_SEMA_TYPEF_BFLOAT)) {
    *precision = 8u;
    *emin = -126;
    *emax = 127;
    return 1;
  }
  if (type->bit_width == 16u) {
    *precision = 11u;
    *emin = -14;
    *emax = 15;
    return 1;
  }
  if (type->bit_width == 32u) {
    *precision = 24u;
    *emin = -126;
    *emax = 127;
    return 1;
  }
  if (type->bit_width == 64u) {
    *precision = 53u;
    *emin = -1022;
    *emax = 1023;
    return 1;
  }
  return 0;
}

static uint64_t const_round_ties_even_u64(double value) {
  double base = floor(value);
  double fraction = value - base;
  uint64_t integer = (uint64_t)base;
  if (fraction > 0.5 || (fraction == 0.5 && (integer & 1u)))
    integer++;
  return integer;
}

static AstSemaConstValue const_quantize_float(double value,
                                              const AstSemaType *type) {
  uint32_t precision = 0u;
  int emin = 0, emax = 0;
  if (!const_float_format_params(type, &precision, &emin, &emax))
    return sema_const_invalid();
  if (!AST_SEMA_ISFINITE(value))
    return sema_const_float(value, type);
  if (!const_float_fits_format(value, type))
    return sema_const_invalid();
  if (value == 0.0 || type->bit_width == 64u)
    return sema_const_float(value, type);

  double magnitude = fabs(value);
  int exponent = 0;
  double fraction = frexp(magnitude, &exponent);
  int unbiased = exponent - 1;
  double rounded = 0.0;
  if (unbiased < emin) {
    int quantum_exponent = emin - (int)(precision - 1u);
    double scaled = ldexp(magnitude, -quantum_exponent);
    uint64_t significand = const_round_ties_even_u64(scaled);
    rounded = ldexp((double)significand, quantum_exponent);
  } else {
    double scaled = ldexp(fraction, (int)precision);
    uint64_t significand = const_round_ties_even_u64(scaled);
    uint64_t carry = UINT64_C(1) << precision;
    if (significand == carry) {
      significand >>= 1u;
      exponent++;
    }
    if (exponent - 1 > emax)
      return sema_const_invalid();
    rounded = ldexp((double)significand, exponent - (int)precision);
  }
  if (value < 0.0)
    rounded = -rounded;
  if (rounded == 0.0)
    rounded = copysign(0.0, value);
  return sema_const_float(rounded, type);
}

static int const_double_integer_magnitude(double value,
                                          AstSemaConstValue *out) {
  if (!out || !AST_SEMA_ISFINITE(value) || value < 0.0 || DBL_MANT_DIG > 64)
    return 0;
  value = trunc(value);
  memset(out, 0, sizeof(*out));
  out->flags = AST_SEMA_CONSTF_VALID;
  if (value == 0.0)
    return 1;
  int exponent = 0;
  double fraction = frexp(value, &exponent);
  if (exponent > 128)
    return 0;
  uint64_t mantissa = (uint64_t)ldexp(fraction, DBL_MANT_DIG);
  int shift = exponent - DBL_MANT_DIG;
  if (shift < 0) {
    unsigned right = (unsigned)(-shift);
    out->lo = right >= 64u ? 0u : mantissa >> right;
    return 1;
  }
  if (shift >= 128)
    return 0;
  if (shift >= 64) {
    unsigned left = (unsigned)(shift - 64);
    if (left >= 64u || (left != 0u && (mantissa >> (64u - left)) != 0u))
      return 0;
    out->hi = mantissa << left;
    return 1;
  }
  if (shift == 0) {
    out->lo = mantissa;
    return 1;
  }
  out->lo = mantissa << (unsigned)shift;
  out->hi = mantissa >> (64u - (unsigned)shift);
  return 1;
}

static AstSemaConstValue const_float_to_integer(AstSemaConstValue value,
                                                const AstSemaType *dst) {
  if (!(value.flags & AST_SEMA_CONSTF_FLOAT) || !sema_type_is_integer(dst) ||
      !AST_SEMA_ISFINITE(value.floating)) {
    return sema_const_invalid();
  }
  double truncated = trunc(value.floating);
  AstSemaConstValue integer;
  if (!const_double_integer_magnitude(fabs(truncated), &integer))
    return sema_const_invalid();
  if (AST_SEMA_SIGNBIT(truncated) && (integer.lo != 0u || integer.hi != 0u))
    integer.flags |= AST_SEMA_CONSTF_NEGATIVE;
  if (!sema_const_fits_type(dst, integer))
    return sema_const_invalid();
  return const_wrap_integer(integer, dst);
}

static AstSemaConstValue const_cast_value(AstSemaConstCtx *ctx,
                                          const AstExpr *expr,
                                          AstSemaConstValue value,
                                          const AstSemaType *dst) {
  if (!ctx || !expr || !dst || !sema_const_is_valid(value))
    return sema_const_invalid();
  AstTokenKind op = expr->as.cast.op;
  if (op == AST_TOK_KW_CAST) {
    if (sema_type_is_integer(dst)) {
      if (value.flags & AST_SEMA_CONSTF_FLOAT)
        return const_float_to_integer(value, dst);
      if (!const_fits_integer_type(value, dst))
        return sema_const_invalid();
      return const_wrap_integer(value, dst);
    }
    if (dst->kind == AST_SEMA_TYPE_FLOAT) {
      double converted = (value.flags & AST_SEMA_CONSTF_FLOAT)
                             ? value.floating
                             : const_integer_to_double(value);
      return const_quantize_float(converted, dst);
    }
    return sema_const_invalid();
  }
  if (op == AST_TOK_KW_TRUNC || op == AST_TOK_KW_ZEXT ||
      op == AST_TOK_KW_SEXT) {
    if (!sema_type_is_integer(dst) || (value.flags & AST_SEMA_CONSTF_FLOAT))
      return sema_const_invalid();
    return const_wrap_integer(value, dst);
  }
  if (op == AST_TOK_KW_BITCAST) {
    if (!sema_type_is_integer(dst) || (value.flags & AST_SEMA_CONSTF_FLOAT))
      return sema_const_invalid();
    AstSemaConstValue bits = const_integer_bits(value, value.type);
    if (!sema_const_is_valid(bits))
      return sema_const_invalid();
    return const_bits_to_integer(bits, dst);
  }
  return sema_const_invalid();
}

static uint32_t const_popcount_bits(AstSemaConstValue bits, uint32_t width) {
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < width; ++i)
    count += u128_get_bit(bits, i);
  return count;
}

static AstSemaConstValue const_bit_primitive(AstSemaConstValue value,
                                             const AstSemaType *type,
                                             AstName operation) {
  uint32_t width = const_integer_width(type);
  AstSemaConstValue bits = const_integer_bits(value, type);
  if (!sema_const_is_valid(bits) || width == 0u || width > 128u)
    return sema_const_invalid();
  if (sema_name_eq_text(operation, "popcnt")) {
    return const_wrap_integer(
        sema_const_u64(const_popcount_bits(bits, width), type), type);
  }
  if (sema_name_eq_text(operation, "clz") ||
      sema_name_eq_text(operation, "ctz")) {
    uint32_t count = 0u;
    if (sema_name_eq_text(operation, "clz")) {
      for (uint32_t i = width; i > 0u && u128_get_bit(bits, i - 1u) == 0u; --i)
        ++count;
    } else {
      for (uint32_t i = 0u; i < width && u128_get_bit(bits, i) == 0u; ++i)
        ++count;
    }
    return const_wrap_integer(sema_const_u64(count, type), type);
  }
  AstSemaConstValue result = sema_const_u64(0u, type);
  if (sema_name_eq_text(operation, "bitrev")) {
    for (uint32_t i = 0u; i < width; ++i)
      if (u128_get_bit(bits, i))
        u128_set_bit(&result, width - 1u - i);
    return const_bits_to_integer(result, type);
  }
  if (sema_name_eq_text(operation, "bswap")) {
    if ((width & 7u) != 0u)
      return sema_const_invalid();
    uint32_t bytes = width / 8u;
    for (uint32_t dst = 0u; dst < bytes; ++dst) {
      uint32_t src = bytes - 1u - dst;
      for (uint32_t bit = 0u; bit < 8u; ++bit) {
        if (u128_get_bit(bits, src * 8u + bit))
          u128_set_bit(&result, dst * 8u + bit);
      }
    }
    return const_bits_to_integer(result, type);
  }
  return sema_const_invalid();
}

static AstSemaConstValue const_rotate(AstSemaConstValue value,
                                      AstSemaConstValue count,
                                      const AstSemaType *type, int left) {
  uint32_t width = const_integer_width(type);
  uint64_t shift = 0u;
  if (width == 0u || width > 128u || !sema_const_to_u64(count, &shift))
    return sema_const_invalid();
  AstSemaConstValue bits = const_integer_bits(value, type);
  if (!sema_const_is_valid(bits))
    return sema_const_invalid();
  shift %= width;
  if (shift == 0u)
    return const_bits_to_integer(bits, type);
  AstSemaConstValue result = sema_const_u64(0u, type);
  for (uint32_t i = 0u; i < width; ++i) {
    if (!u128_get_bit(bits, i))
      continue;
    uint32_t dst;
    if (left)
      dst = (uint32_t)(((uint64_t)i + shift) % width);
    else
      dst = (uint32_t)(((uint64_t)i + width - shift) % width);
    u128_set_bit(&result, dst);
  }
  return const_bits_to_integer(result, type);
}

static AstSemaConstValue eval_named_core_call(AstSemaConstCtx *ctx,
                                              const AstExpr *expr, AstName name,
                                              const AstSemaType *expected) {
  AstExprArray args = expr->as.call.args;
  if (sema_name_eq_text(name, "select")) {
    if (args.count != 3u)
      return sema_const_invalid();
    AstSemaConstValue condition =
        sema_eval_const_expr(ctx, args.items[0], sema_builtin_bool(ctx->sema));
    if (!sema_const_is_valid(condition))
      return sema_const_invalid();
    return sema_eval_const_expr(
        ctx, const_truthy(condition) ? args.items[1] : args.items[2], expected);
  }
  if (sema_name_eq_text(name, "clz") || sema_name_eq_text(name, "ctz") ||
      sema_name_eq_text(name, "popcnt") || sema_name_eq_text(name, "bswap") ||
      sema_name_eq_text(name, "bitrev")) {
    if (args.count != 1u)
      return sema_const_invalid();
    AstSemaConstValue value =
        sema_eval_const_expr(ctx, args.items[0], expected);
    const AstSemaType *type =
        expected && const_integer_width(expected) != 0u ? expected : value.type;
    return const_bit_primitive(value, type, name);
  }
  if (sema_name_eq_text(name, "rol") || sema_name_eq_text(name, "ror")) {
    if (args.count != 2u)
      return sema_const_invalid();
    AstSemaConstValue value =
        sema_eval_const_expr(ctx, args.items[0], expected);
    AstSemaConstValue count =
        sema_eval_const_expr(ctx, args.items[1], sema_builtin_usize(ctx->sema));
    const AstSemaType *type =
        expected && const_integer_width(expected) != 0u ? expected : value.type;
    return const_rotate(value, count, type, sema_name_eq_text(name, "rol"));
  }
  return sema_const_invalid();
}

static AstSemaConstValue eval_member_core_call(AstSemaConstCtx *ctx,
                                               const AstExpr *expr,
                                               const AstSemaType *expected) {
  const AstExpr *callee = expr->as.call.callee;
  if (!callee || callee->kind != AST_EXPR_MEMBER || !callee->as.member.base ||
      callee->as.member.base->kind != AST_EXPR_NAME ||
      expr->as.call.args.count != 2u)
    return sema_const_invalid();
  AstName operation = callee->as.member.base->as.name.name;
  AstName mode = callee->as.member.member;
  int is_add = sema_name_eq_text(operation, "add");
  int is_sub = sema_name_eq_text(operation, "sub");
  int is_mul = sema_name_eq_text(operation, "mul");
  int is_wrap = sema_name_eq_text(mode, "wrap");
  int is_sat = sema_name_eq_text(mode, "sat");
  if ((!is_add && !is_sub && !is_mul) || (!is_wrap && !is_sat))
    return sema_const_invalid();
  if (is_sat && is_mul)
    return sema_const_invalid();
  if (!expected || const_integer_width(expected) == 0u)
    return sema_const_invalid();
  AstSemaConstValue a =
      sema_eval_const_expr(ctx, expr->as.call.args.items[0], expected);
  AstSemaConstValue b =
      sema_eval_const_expr(ctx, expr->as.call.args.items[1], expected);
  if (!sema_const_is_valid(a) || !sema_const_is_valid(b))
    return sema_const_invalid();
  if (is_wrap) {
    if (is_add)
      return const_modular_add(a, b, expected, 0);
    if (is_sub)
      return const_modular_add(a, b, expected, 1);
    return const_modular_mul(a, b, expected);
  }
  return const_saturating_addsub(a, b, expected, is_sub);
}

static AstSemaConstValue eval_const_call(AstSemaConstCtx *ctx,
                                         const AstExpr *expr,
                                         const AstSemaType *expected) {
  if (!expr->as.call.callee)
    return sema_const_invalid();
  if (expr->as.call.callee->kind == AST_EXPR_MEMBER)
    return eval_member_core_call(ctx, expr, expected);
  if (expr->as.call.callee->kind != AST_EXPR_NAME)
    return sema_const_invalid();
  AstName name = expr->as.call.callee->as.name.name;
  if (sema_name_eq_text(name, "sizeof"))
    return eval_sizeof_call(ctx, expr);
  if (sema_name_eq_text(name, "offsetof"))
    return eval_offsetof_call(ctx, expr);
  if (sema_name_eq_text(name, "feature"))
    return eval_feature_call(ctx, expr);
  return eval_named_core_call(ctx, expr, name, expected);
}

AstSemaConstValue sema_eval_const_expr(AstSemaConstCtx *ctx,
                                       const AstExpr *expr,
                                       const AstSemaType *expected) {
  if (!ctx || !ctx->sema || !expr)
    return sema_const_invalid();
  AstSema *s = ctx->sema;
  s->stats.constant_expression_count++;
  if (ctx->depth++ > s->config.recursion_limit) {
    ctx->depth--;
    uint32_t file_id = ctx->module ? ctx->module->file.id : expr->span.file_id;
    sema_diag(s, AST_SEMA_DIAG_RECURSION_LIMIT, AST_DIAG_ERROR, expr->span,
              file_id, "compile-time expression recursion limit exceeded");
    return sema_const_invalid();
  }
  AstSemaConstValue r = sema_const_invalid();
  switch (expr->kind) {
  case AST_EXPR_INTEGER:
    r = parse_integer_literal(s, expr);
    break;
  case AST_EXPR_FLOAT:
    r = parse_float_literal(s, expr);
    break;
  case AST_EXPR_BOOL:
    r = const_bool(s, expr->as.literal.token_kind == AST_TOK_KW_TRUE);
    break;
  case AST_EXPR_CHAR:
    r = parse_char_literal(s, expr);
    break;
  case AST_EXPR_NAME: {
    if (ctx->fn) {
      for (size_t i = ctx->fn->generic_count; i > 0u; --i) {
        AstSemaGenericBinding *b = &ctx->fn->generics[i - 1u];
        if (b->is_const && sema_name_eq(b->name, expr->as.name.name)) {
          r = b->const_value;
          goto done;
        }
      }
      AstSemaLocal *local = sema_fn_find_local(ctx->fn, expr->as.name.name);
      if (local && local->kind == AST_SEMA_SYMBOL_LOCAL_CONST &&
          sema_const_is_valid(local->const_value)) {
        r = local->const_value;
        goto done;
      }
    }
    AstSemaSymbol *sym = find_const_symbol(ctx, expr->as.name.name);
    if (sym)
      r = eval_symbol_const(ctx, sym);
    break;
  }
  case AST_EXPR_MEMBER: {
    AstSemaSymbol *sym =
        sema_resolve_import_member(s, ctx->module, expr, NULL, 0);
    if (sym && sym->kind == AST_SEMA_SYMBOL_CONST) {
      AstSemaConstCtx nested = *ctx;
      nested.module = sema_module_by_file_id(s, sym->file_id);
      r = eval_symbol_const(&nested, sym);
    }
    break;
  }
  case AST_EXPR_UNARY: {
    AstSemaConstValue a =
        sema_eval_const_expr(ctx, expr->as.unary.operand, expected);
    if (!sema_const_is_valid(a))
      break;
    if (expr->as.unary.op == AST_TOK_MINUS) {
      if (a.flags & AST_SEMA_CONSTF_FLOAT)
        a.floating = -a.floating;
      else if (!u128_is_zero(a))
        a.flags ^= AST_SEMA_CONSTF_NEGATIVE;
      r = a;
    } else if (expr->as.unary.op == AST_TOK_PLUS) {
      r = a;
    } else if (expr->as.unary.op == AST_TOK_BANG) {
      r = const_bool(s, !const_truthy(a));
    } else if (expr->as.unary.op == AST_TOK_TILDE &&
               !(a.flags & AST_SEMA_CONSTF_FLOAT)) {
      const AstSemaType *type =
          expected && const_integer_width(expected) != 0u ? expected : a.type;
      AstSemaConstValue bits = const_integer_bits(a, type);
      if (sema_const_is_valid(bits)) {
        bits.lo = ~bits.lo;
        bits.hi = ~bits.hi;
        const_mask_to_width(&bits, const_integer_width(type));
        r = const_bits_to_integer(bits, type);
      }
    }
    break;
  }
  case AST_EXPR_BINARY: {
    AstSemaConstValue a =
        sema_eval_const_expr(ctx, expr->as.binary.left, expected);
    AstSemaConstValue b =
        sema_eval_const_expr(ctx, expr->as.binary.right, expected);
    r = const_binary(ctx, expr, a, b, expected);
    break;
  }
  case AST_EXPR_CONDITIONAL: {
    AstSemaConstValue c = sema_eval_const_expr(
        ctx, expr->as.conditional.condition, sema_builtin_bool(s));
    if (sema_const_is_valid(c)) {
      r = sema_eval_const_expr(ctx,
                               const_truthy(c) ? expr->as.conditional.then_expr
                                               : expr->as.conditional.else_expr,
                               expected);
    }
    break;
  }
  case AST_EXPR_CAST: {
    const AstSemaType *dst =
        sema_resolve_type(s, ctx->module, expr->as.cast.target, ctx->fn, 0u);
    AstSemaConstValue a = sema_eval_const_expr(ctx, expr->as.cast.value, NULL);
    if (dst && dst->kind != AST_SEMA_TYPE_ERROR && sema_const_is_valid(a))
      r = const_cast_value(ctx, expr, a, dst);
    break;
  }
  case AST_EXPR_CALL:
    r = eval_const_call(ctx, expr, expected);
    break;
  default:
    break;
  }
done:
  if (sema_const_is_valid(r) && expected && const_expr_wraps_integer(expr) &&
      const_integer_width(expected) != 0u) {
    r = const_wrap_integer(r, expected);
  }
  if (sema_const_is_valid(r) && expected &&
      expected->kind == AST_SEMA_TYPE_FLOAT &&
      (r.flags & AST_SEMA_CONSTF_FLOAT)) {
    r = const_quantize_float(r.floating, expected);
  }
  ctx->depth--;
  return r;
}
