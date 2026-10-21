/* Independently verify signed quotient/remainder reconstructed from a
   positive-magnitude unsigned magic plan.  This oracle does not execute the
   target binaries.  All arithmetic is defined in C17 (no signed overflow). */
#include "../src/backend/common/div_magic.h"
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t rng(void) {
  uint64_t v = state;
  v ^= v >> 12u;
  v ^= v << 25u;
  v ^= v >> 27u;
  state = v;
  return v * UINT64_C(0x2545f4914f6cdd1d);
}
static uint64_t mul_high(uint64_t a, uint64_t b) {
  uint64_t a0 = (uint32_t)a, a1 = a >> 32u;
  uint64_t b0 = (uint32_t)b, b1 = b >> 32u;
  uint64_t lo = a0 * b0, mid1 = a0 * b1, mid2 = a1 * b0;
  uint64_t middle = (lo >> 32u) + (uint32_t)mid1 + (uint32_t)mid2;
  return a1 * b1 + (mid1 >> 32u) + (mid2 >> 32u) + (middle >> 32u);
}
static uint64_t i64_bits(int64_t value) {
  /* C17 conversion to uint64_t has defined modulo-2^64 semantics. */
  return (uint64_t)value;
}
static int test_case(int64_t n, int64_t d, uint64_t *checks) {
  uint64_t ub = i64_bits(d);
  uint64_t magnitude = d < 0 ? UINT64_C(0) - ub : ub;
  AstDivMagicU64 plan;
  if (!ast_div_magic_u64(magnitude, &plan))
    return 1;
  uint64_t sign_n = n < 0 ? UINT64_MAX : 0u;
  uint64_t sign_q = ((n < 0) != (d < 0)) ? UINT64_MAX : 0u;
  uint64_t absolute = (i64_bits(n) ^ sign_n) - sign_n;
  uint64_t quotient = mul_high(absolute, plan.multiplier);
  if (plan.add)
    quotient += ((absolute - quotient) >> 1u);
  quotient >>= plan.shift;
  uint64_t signed_quotient = (quotient ^ sign_q) - sign_q;
  uint64_t signed_rem = i64_bits(n) - signed_quotient * i64_bits(d);
  if (signed_quotient != i64_bits(n / d) || signed_rem != i64_bits(n % d)) {
    fprintf(stderr,
            "signed magic mismatch n=%" PRId64 " d=%" PRId64 " q=%" PRIu64
            " r=%" PRIu64 "\n",
            n, d, signed_quotient, signed_rem);
    return 0;
  }
  ++*checks;
  return 1;
}
int main(void) {
  static const int64_t edge[] = {INT64_MIN,
                                 INT64_MIN + 1,
                                 INT64_MIN / 2,
                                 -65536,
                                 -257,
                                 -128,
                                 -127,
                                 -64,
                                 -31,
                                 -11,
                                 -7,
                                 -5,
                                 -3,
                                 -2,
                                 -1,
                                 0,
                                 1,
                                 2,
                                 3,
                                 5,
                                 7,
                                 11,
                                 31,
                                 63,
                                 127,
                                 255,
                                 257,
                                 65535,
                                 INT64_MAX / 2,
                                 INT64_MAX - 1,
                                 INT64_MAX};
  uint64_t checks = 0u;
  for (uint32_t i = 0; i < 2048u; ++i) {
    int64_t divisor = (int64_t)(rng() >> 1u);
    /* Avoid implementation-defined unsigned->signed negative conversions.
       The chosen magnitude stays in the signed positive range. */
    if (i < sizeof(edge) / sizeof(edge[0]))
      divisor = edge[i];
    if (divisor == 0 || divisor == 1 || divisor == -1)
      continue;
    uint64_t magnitude =
        divisor < 0 ? UINT64_C(0) - i64_bits(divisor) : i64_bits(divisor);
    if (magnitude < 3u || !(magnitude & (magnitude - 1u)))
      continue;
    for (uint32_t j = 0; j < 256u; ++j) {
      int64_t numerator =
          j < sizeof(edge) / sizeof(edge[0])
              ? edge[j]
              : (j & 1u ? (int64_t)(rng() >> 1u) : -(int64_t)(rng() >> 1u));
      if (!test_case(numerator, divisor, &checks) ||
          !test_case(numerator, -divisor, &checks))
        return 1;
    }
  }
  printf("signed magic: %" PRIu64 " quotient/remainder comparisons passed\n",
         checks);
  return 0;
}
