/* Compile-time exact 64-bit unsigned division strength-reduction plan.
   The constant is 2^(64+floor_log2(d)) / d, calculated with pure C17 limb
   arithmetic (no __int128 or target compiler intrinsics). For the add marker
   use the branchfree Granlund/Montgomery correction. No shifts by 64. */
#ifndef ASTERASM_DIV_MAGIC_H
#define ASTERASM_DIV_MAGIC_H
#include <stdint.h>

typedef struct AstDivMagicU64 {
  uint64_t multiplier;
  uint32_t shift;
  uint8_t add;
} AstDivMagicU64;

static inline int ast_div_magic_u64(uint64_t divisor, AstDivMagicU64 *out) {
  if (!out || divisor < 3u || !(divisor & (divisor - 1u)))
    return 0;
  uint32_t shift = 0u;
  for (uint64_t v = divisor; v > 1u; v >>= 1u)
    ++shift;
  const uint64_t high = UINT64_C(1) << shift;
  uint64_t remainder = high, quotient = 0u;
  /* quotient := floor((high * 2^64) / divisor), remainder is exact.
     The high limb is < divisor, and one 65-bit subtraction can be
     represented as modular uint64_t subtraction plus the carry flag. */
  for (int bit = 63; bit >= 0; --bit) {
    uint64_t carry = remainder >> 63u;
    remainder <<= 1u;
    if (carry || remainder >= divisor) {
      remainder -= divisor;
      quotient |= UINT64_C(1) << bit;
    }
  }
  uint8_t add = 0u;
  if (divisor - remainder >= high) {
    uint64_t twice = remainder + remainder;
    quotient += quotient;
    if (twice >= divisor || twice < remainder)
      ++quotient;
    add = 1u;
  }
  out->multiplier = quotient + UINT64_C(1);
  out->shift = shift;
  out->add = add;
  return 1;
}
/* The 32-bit counterpart is computed in 64-bit *unsigned* arithmetic.
   The largest dividend is 2^(32 + 31), which is representable in u64.
   Multipliers intentionally wrap modulo 2^32; the add marker corrects
   the quotient without using host signed overflow or __int128. */
static inline int ast_div_magic_u32(uint32_t divisor, AstDivMagicU64 *out) {
  if (!out || divisor < 3u || !(divisor & (divisor - 1u)))
    return 0;
  uint32_t shift = 0u;
  for (uint32_t v = divisor; v > 1u; v >>= 1u)
    ++shift;
  uint64_t numerator = UINT64_C(1) << (32u + shift);
  uint64_t proposed = numerator / divisor;
  uint64_t remainder = numerator % divisor;
  uint8_t add = 0u;
  if ((uint64_t)divisor - remainder >= (UINT64_C(1) << shift)) {
    proposed *= 2u;
    if (remainder * 2u >= divisor)
      ++proposed;
    add = 1u;
  }
  out->multiplier = (uint32_t)(proposed + 1u);
  out->shift = shift;
  out->add = add;
  return 1;
}

#endif
