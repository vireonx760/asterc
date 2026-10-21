/* Deterministic, conservative *estimated* constant-division
   selection. The latency units are indicative, NOT CPU benchmark results.
   Size estimates exclude function prologues and relocation layout. */
#ifndef ASTERASM_DIV_COST_H
#define ASTERASM_DIV_COST_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum AstDivCostTarget {
  AST_DIV_COST_X86_64,
  AST_DIV_COST_AARCH64,
  AST_DIV_COST_RV64
} AstDivCostTarget;
typedef enum AstDivCostMode {
  AST_DIV_COST_HARDWARE,
  AST_DIV_COST_POWER2,
  AST_DIV_COST_MAGIC,
  AST_DIV_COST_SOFTWARE
} AstDivCostMode;
typedef struct AstDivCostChoice {
  AstDivCostMode mode;
  uint32_t estimated_hardware_bytes, estimated_magic_bytes;
  uint32_t estimated_hardware_latency, estimated_magic_latency;
  uint32_t estimated_extra_registers;
  const char *reason;
} AstDivCostChoice;
static inline const char *ast_div_cost_mode_name(AstDivCostMode mode) {
  switch (mode) {
  case AST_DIV_COST_HARDWARE:
    return "hardware";
  case AST_DIV_COST_POWER2:
    return "power2";
  case AST_DIV_COST_MAGIC:
    return "reciprocal";
  default:
    return "software";
  }
}
/* RV64 M availability is explicit. AArch64 has no selectable integer-divide
   feature; x86-64 has DIV/IDIV in the baseline ISA. Signed pow2 currently
   follows hardware to retain round-towards-zero/overflow semantics. */
static inline AstDivCostChoice
ast_div_cost_select(AstDivCostTarget target, uint32_t bits, int signed_op,
                    int remainder, uint64_t magnitude, int hardware_available,
                    int prefer_size) {
  AstDivCostChoice out = {0};
  int narrow = bits < 64u;
  out.estimated_hardware_bytes = target == AST_DIV_COST_X86_64 ? 24u : 16u;
  out.estimated_magic_bytes = target == AST_DIV_COST_X86_64
                                  ? (narrow ? 36u : 43u)
                                  : (narrow ? 48u : 32u);
  out.estimated_magic_bytes += signed_op ? 22u : 0u;
  out.estimated_magic_bytes +=
      remainder ? (target == AST_DIV_COST_X86_64 ? 11u : 8u) : 0u;
  out.estimated_hardware_latency = target == AST_DIV_COST_AARCH64 ? 30u : 28u;
  out.estimated_magic_latency =
      11u + (signed_op ? 6u : 0u) + (remainder ? 3u : 0u);
  out.estimated_extra_registers = signed_op ? 4u : 3u;
  if (!magnitude) {
    out.mode =
        hardware_available ? AST_DIV_COST_HARDWARE : AST_DIV_COST_SOFTWARE;
    out.reason = "zero is not optimized; preserve target divide contract";
  } else if (!signed_op && !(magnitude & (magnitude - 1u))) {
    out.mode = AST_DIV_COST_POWER2;
    out.reason = "exact unsigned shift/mask";
  } else if (!hardware_available) {
    out.mode = AST_DIV_COST_SOFTWARE;
    out.reason = "missing RV64 M; never emit M instructions";
  } else if (magnitude < 3u || !(magnitude & (magnitude - 1u))) {
    out.mode = AST_DIV_COST_HARDWARE;
    out.reason = "signed special divisor or power-of-two";
  } else if (target != AST_DIV_COST_X86_64 && narrow) {
    out.mode = AST_DIV_COST_HARDWARE;
    out.reason =
        "32-bit target reciprocal is not qualified; use normalized hardware";
  } else if (prefer_size ||
             out.estimated_magic_bytes > out.estimated_hardware_bytes + 70u) {
    out.mode = AST_DIV_COST_HARDWARE;
    out.reason = "size budget favors hardware divider";
  } else if (out.estimated_magic_latency < out.estimated_hardware_latency) {
    out.mode = AST_DIV_COST_MAGIC;
    out.reason = "latency estimate favors reciprocal; size growth permitted";
  } else {
    out.mode = AST_DIV_COST_HARDWARE;
    out.reason = "no estimated latency advantage";
  }
  return out;
}
/* Experimental driver-independent toggles, read only. No mutable global
   state, so different compiler processes can perform A/B comparisons. */
static inline int ast_div_cost_env_equals(const char *name,
                                          const char *expected) {
#if defined(_WIN32) && defined(_MSC_VER)
  /* UCRT deprecates getenv, including under clang's MSVC ABI. Read into
     caller-owned storage without a heap allocation or shared cache. Query
     the length first so an arbitrary long policy cannot truncate to size. */
  char value[8];
  size_t length = 0;
  if (getenv_s(&length, NULL, 0, name) != 0 ||
      length != strlen(expected) + 1u || length > sizeof(value))
    return 0;
  return getenv_s(&length, value, sizeof(value), name) == 0 &&
         strcmp(value, expected) == 0;
#else
  const char *value = getenv(name);
  return value && strcmp(value, expected) == 0;
#endif
}
static inline int ast_div_cost_prefer_size(void) {
  return ast_div_cost_env_equals("ASTERASM_DIV_POLICY", "size");
}
static inline void ast_div_cost_report(AstDivCostTarget target, uint32_t bits,
                                       int signed_op, int remainder,
                                       uint64_t divisor,
                                       AstDivCostChoice plan) {
  if (!ast_div_cost_env_equals("ASTERASM_DIV_REPORT", "1"))
    return;
  fprintf(stderr,
          "[aster-div] %s %c%u %s magnitude=%llu choose=%s hw=%uB magic=%uB "
          "hw-lat=%u magic-lat=%u regs=%u reason=%s\n",
          target == AST_DIV_COST_X86_64    ? "x86_64"
          : target == AST_DIV_COST_AARCH64 ? "aarch64"
                                           : "riscv64",
          signed_op ? 'i' : 'u', bits, remainder ? "rem" : "div",
          (unsigned long long)divisor, ast_div_cost_mode_name(plan.mode),
          plan.estimated_hardware_bytes, plan.estimated_magic_bytes,
          plan.estimated_hardware_latency, plan.estimated_magic_latency,
          plan.estimated_extra_registers, plan.reason);
}
#endif
