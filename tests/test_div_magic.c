/* Independent portable C17 oracle for the high-half multiplication plan.
   No host 128-bit types: reconstruct the high half with four 32x32 products. */
#include "../src/backend/common/div_magic.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t next(void) {
  uint64_t x = seed;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  seed = x;
  return x * UINT64_C(0x2545f4914f6cdd1d);
}
static uint64_t high_product(uint64_t a, uint64_t b) {
  uint64_t a0 = (uint32_t)a, a1 = a >> 32u, b0 = (uint32_t)b, b1 = b >> 32u;
  uint64_t lo = a0 * b0, cross1 = a0 * b1, cross2 = a1 * b0, hi = a1 * b1;
  uint64_t middle = (lo >> 32u) + (uint32_t)cross1 + (uint32_t)cross2;
  return hi + (cross1 >> 32u) + (cross2 >> 32u) + (middle >> 32u);
}
int main(void) {
  const uint64_t edges[] = {0u,
                            1u,
                            2u,
                            3u,
                            6u,
                            7u,
                            8u,
                            9u,
                            15u,
                            31u,
                            255u,
                            256u,
                            UINT64_C(0xffffffff),
                            UINT64_C(0x100000000),
                            UINT64_C(0x7fffffffffffffff),
                            UINT64_C(0x8000000000000000),
                            UINT64_C(0x8000000000000001),
                            UINT64_MAX - 1u,
                            UINT64_MAX};
  uint64_t checked = 0u;
  for (uint32_t i = 0u; i < 4096u; ++i) {
    uint64_t d = i < sizeof(edges) / sizeof(edges[0]) ? edges[i] : next();
    AstDivMagicU64 plan;
    int valid = ast_div_magic_u64(d, &plan);
    if (d < 3u || !(d & (d - 1u))) {
      if (valid) {
        fprintf(stderr, "false plan for divisor=%" PRIu64 "\n", d);
        return 1;
      }
      continue;
    }
    if (!valid || !plan.multiplier || plan.shift > 63u || plan.add > 1u)
      return 2;
    for (uint32_t j = 0; j < 256u; ++j) {
      uint64_t n = j < sizeof(edges) / sizeof(edges[0]) ? edges[j]
                   : j == 20u                           ? d - 1u
                   : j == 21u                           ? d
                   : j == 22u                           ? d + 1u
                                                        : next();
      uint64_t q = high_product(n, plan.multiplier);
      if (plan.add)
        q += ((n - q) >> 1u);
      q >>= plan.shift;
      uint64_t r = n - q * d;
      if (q != n / d || r != n % d) {
        fprintf(stderr,
                "magic mismatch n=%" PRIu64 " d=%" PRIu64 " q=%" PRIu64
                " ref=%" PRIu64 "\n",
                n, d, q, n / d);
        return 3;
      }
      ++checked;
    }
  }
  printf("C17 unsigned magic plan: %" PRIu64
         " differential div/rem cases passed\n",
         checked);
  return 0;
}
