/* C17 independent 8-bit exhaustive + 16/32-bit boundary/random
   reciprocal arithmetic oracle, plus deterministic target/feature costs. */
#include "../src/backend/common/div_cost.h"
#include "../src/backend/common/div_magic.h"
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t rng_state = UINT64_C(0x232323A57EABCDEF);
static uint32_t rand32(void) {
  uint64_t v = rng_state;
  v ^= v >> 12u;
  v ^= v << 25u;
  v ^= v >> 27u;
  rng_state = v;
  return (uint32_t)(v * UINT64_C(0x2545f4914f6cdd1d));
}
static int check(uint32_t raw, uint32_t divisor, uint32_t bits, int signed_op,
                 uint64_t *count) {
  AstDivMagicU64 magic;
  uint32_t magnitude = divisor;
  if (signed_op && (divisor & (UINT32_C(1) << (bits - 1u))))
    magnitude = (uint32_t)((UINT64_C(1) << bits) - divisor);
  if (magnitude < 3u || !(magnitude & (magnitude - 1u)))
    return 1;
  if (!ast_div_magic_u32(magnitude, &magic) || magic.shift > 31u ||
      magic.add > 1u)
    return 0;
  uint32_t mask = bits == 32u ? UINT32_MAX : (UINT32_C(1) << bits) - 1u;
  uint32_t n = raw & mask;
  uint32_t divisor_bits = divisor & mask;
  uint32_t sign =
      signed_op && (n & (UINT32_C(1) << (bits - 1u))) ? UINT32_MAX : 0u;
  uint32_t sign_d = signed_op && (divisor_bits & (UINT32_C(1) << (bits - 1u)))
                        ? UINT32_MAX
                        : 0u;
  uint32_t numerator32 = signed_op ? (n | (sign & ~mask)) : n;
  uint32_t abs_n = (numerator32 ^ sign) - sign;
  uint32_t q = (uint32_t)(((uint64_t)abs_n * magic.multiplier) >> 32u);
  if (magic.add)
    q += (abs_n - q) >> 1u;
  q >>= magic.shift;
  uint32_t sign_q = sign ^ sign_d;
  q = (q ^ sign_q) - sign_q;
  uint32_t div32 = signed_op ? (divisor_bits | (sign_d & ~mask)) : divisor_bits;
  uint32_t r = numerator32 - q * div32;
  uint64_t q_ref, r_ref;
  if (signed_op) {
    int64_t sn = (int64_t)numerator32;
    int64_t sd = (int64_t)div32;
    if (numerator32 & UINT32_C(0x80000000))
      sn -= INT64_C(4294967296);
    if (div32 & UINT32_C(0x80000000))
      sd -= INT64_C(4294967296);
    if (sn == INT32_MIN && sd == -1)
      return 1; /* target contract: overflow not optimized */
    q_ref = (uint32_t)(sn / sd);
    r_ref = (uint32_t)(sn % sd);
  } else {
    q_ref = numerator32 / div32;
    r_ref = numerator32 % div32;
  }
  if ((q & mask) != (q_ref & mask) || (r & mask) != (r_ref & mask)) {
    fprintf(stderr,
            "narrow mismatch bits=%u sign=%d n=%" PRIu32 " d=%" PRIu32
            " q=%" PRIu32 " ref=%" PRIu64 " r=%" PRIu32 " ref=%" PRIu64 "\n",
            bits, signed_op, raw, divisor, q, q_ref, r, r_ref);
    return 0;
  }
  ++*count;
  return 1;
}
static int check_cost_contract(uint32_t *checked) {
  static const uint64_t divisors[] = {0u,  1u,  2u,  3u,         7u,        8u,
                                      10u, 63u, 64u, UINT32_MAX, UINT64_MAX};
  for (unsigned target = 0; target < 3u; ++target)
    for (unsigned bits = 8u; bits <= 64u; bits *= 2u)
      for (unsigned signed_op = 0; signed_op < 2u; ++signed_op)
        for (unsigned remainder = 0; remainder < 2u; ++remainder)
          for (unsigned hardware = 0; hardware < 2u; ++hardware)
            for (unsigned size = 0; size < 2u; ++size)
              for (size_t d = 0; d < sizeof(divisors) / sizeof(*divisors);
                   ++d) {
                if (target != AST_DIV_COST_RV64 && !hardware)
                  continue;
                uint64_t mask =
                    bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
                uint64_t divisor = divisors[d] & mask;
                AstDivCostChoice plan = ast_div_cost_select(
                    (AstDivCostTarget)target, bits, (int)signed_op,
                    (int)remainder, divisor, (int)hardware, (int)size);
                /* These are feature/semantic contracts, not predictions of CPU
                 * timing. */
                if (!plan.reason || !*plan.reason ||
                    !plan.estimated_hardware_bytes ||
                    !plan.estimated_magic_bytes ||
                    !plan.estimated_hardware_latency ||
                    !plan.estimated_magic_latency)
                  return 0;
                if (!hardware && (plan.mode == AST_DIV_COST_HARDWARE ||
                                  plan.mode == AST_DIV_COST_MAGIC))
                  return 0;
                if (size && plan.mode == AST_DIV_COST_MAGIC)
                  return 0;
                if (!divisor && plan.mode != (hardware ? AST_DIV_COST_HARDWARE
                                                       : AST_DIV_COST_SOFTWARE))
                  return 0;
                if (divisor && !(divisor & (divisor - 1u)) && !signed_op &&
                    plan.mode != AST_DIV_COST_POWER2)
                  return 0;
                if (plan.mode == AST_DIV_COST_POWER2 &&
                    (signed_op || !divisor || (divisor & (divisor - 1u))))
                  return 0;
                if (plan.mode == AST_DIV_COST_MAGIC &&
                    (target != AST_DIV_COST_X86_64 && bits < 64u))
                  return 0;
                (*checked)++;
              }
  return 1;
}

int main(void) {
  uint64_t checked = 0u;
  for (uint32_t sign = 0; sign < 2u; ++sign) {
    for (uint32_t d = 1u; d < 256u; ++d) {
      if (sign && ((d & 127u) == 0u))
        continue;
      for (uint32_t n = 0u; n < 256u; ++n)
        if (!check(n, d, 8u, (int)sign, &checked))
          return 1;
    }
  }
  for (uint32_t bits = 16u; bits <= 32u; bits += 16u) {
    const uint32_t edges[] = {0u,
                              1u,
                              2u,
                              3u,
                              5u,
                              7u,
                              19u,
                              31u,
                              63u,
                              127u,
                              255u,
                              256u,
                              257u,
                              65535u,
                              65536u,
                              UINT32_C(0x7fffffff),
                              UINT32_C(0x80000000),
                              UINT32_MAX};
    for (uint32_t signed_op = 0u; signed_op < 2u; ++signed_op) {
      for (uint32_t i = 0u; i < 2048u; ++i) {
        uint32_t d = i < sizeof(edges) / sizeof(edges[0]) ? edges[i] : rand32();
        uint32_t mask = bits == 16u ? UINT16_MAX : UINT32_MAX;
        d &= mask;
        if (!d)
          continue;
        for (uint32_t j = 0u; j < 256u; ++j) {
          uint32_t n =
              j < sizeof(edges) / sizeof(edges[0]) ? edges[j] : rand32();
          if (!check(n, d, bits, (int)signed_op, &checked))
            return 2;
        }
      }
    }
  }
  AstDivCostChoice a =
      ast_div_cost_select(AST_DIV_COST_X86_64, 8u, 1, 0, 7u, 1, 0);
  AstDivCostChoice b =
      ast_div_cost_select(AST_DIV_COST_X86_64, 8u, 1, 0, 7u, 1, 1);
  AstDivCostChoice c =
      ast_div_cost_select(AST_DIV_COST_RV64, 64u, 0, 0, 7u, 0, 0);
  AstDivCostChoice d =
      ast_div_cost_select(AST_DIV_COST_RV64, 64u, 0, 0, 8u, 0, 1);
  AstDivCostChoice e =
      ast_div_cost_select(AST_DIV_COST_AARCH64, 32u, 1, 1, 7u, 1, 0);
  AstDivCostChoice f =
      ast_div_cost_select(AST_DIV_COST_X86_64, 32u, 0, 0, 0u, 1, 0);
  if (a.mode != AST_DIV_COST_MAGIC || b.mode != AST_DIV_COST_HARDWARE ||
      c.mode != AST_DIV_COST_SOFTWARE || d.mode != AST_DIV_COST_POWER2 ||
      e.mode != AST_DIV_COST_HARDWARE || f.mode != AST_DIV_COST_HARDWARE)
    return 3;
  uint32_t policies = 0;
  if (!check_cost_contract(&policies))
    return 4;
  printf("narrow C17 arithmetic: %" PRIu64
         " comparisons, %u cost-contract profiles PASS\n",
         checked, policies);
  return 0;
}
