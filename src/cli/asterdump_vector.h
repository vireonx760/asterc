/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ASTERASM_DUMP_VECTOR_H
#define ASTERASM_DUMP_VECTOR_H
#include "../isa/vector_contract.h"
#include <inttypes.h>
#include <stdio.h>

static inline int dump_vector_word(AstTargetArch arch, uint32_t w, char *out,
                                   size_t capacity) {
  unsigned rd = w & 31u, rn = (w >> 5u) & 31u, rm = (w >> 16u) & 31u;
  for (uint32_t j = 0; j < VECTOR_COUNT; ++j) {
    const VectorForm *f = &vector_forms[j];
    if (f->arch != arch)
      continue;
    if (arch == AST_TARGET_ARCH_AARCH64) {
      if (f->shape == VECTOR_A64_LOAD || f->shape == VECTOR_A64_STORE) {
        uint32_t code = f->shape == VECTOR_A64_LOAD ? UINT32_C(0x00400000) : 0u;
        uint32_t fixed = w & UINT32_C(0xffc00000);
        unsigned bytes = fixed == (code | UINT32_C(0x3d800000))   ? 16u
                         : fixed == (code | UINT32_C(0xfd000000)) ? 8u
                                                                  : 0u;
        if (!bytes)
          continue;
        char base[12];
        (void)snprintf(base, sizeof(base), "x%u", rn);
        if (rn == 31u)
          memcpy(base, "sp", 3u);
        (void)snprintf(out, capacity, "%s %c%u, [%s, #%u]", f->name,
                       bytes == 16u ? 'q' : 'd', rd, base,
                       ((w >> 10u) & 4095u) * bytes);
        return 1;
      }
      uint32_t variable = UINT32_C(0x401f03ff);
      if (f->shape == VECTOR_A64_INTEGER ||
          f->shape == VECTOR_A64_SMALL_INTEGER)
        variable |= UINT32_C(0x00c00000);
      else if (f->shape == VECTOR_A64_FLOAT)
        variable |= UINT32_C(0x00400000);
      if ((w & ~variable) != f->encoding)
        continue;
      unsigned bits = w & UINT32_C(0x40000000) ? 128u : 64u;
      unsigned element = f->shape == VECTOR_A64_LOGIC ? 8u
                         : f->shape == VECTOR_A64_FLOAT
                             ? ((w & UINT32_C(0x00400000)) ? 64u : 32u)
                             : 8u << ((w >> 22u) & 3u);
      if ((element == 64u && bits == 64u) ||
          (element == 64u && f->shape == VECTOR_A64_SMALL_INTEGER))
        continue;
      char suffix = element == 64u   ? 'd'
                    : element == 32u ? 's'
                    : element == 16u ? 'h'
                                     : 'b';
      (void)snprintf(out, capacity, "%s v%u.%u%c, v%u.%u%c, v%u.%u%c", f->name,
                     rd, bits / element, suffix, rn, bits / element, suffix, rm,
                     bits / element, suffix);
      return 1;
    }
    if (arch != AST_TARGET_ARCH_RISCV64)
      continue;
    unsigned d = (w >> 7u) & 31u, a = (w >> 15u) & 31u, b = (w >> 20u) & 31u;
    if (f->shape == VECTOR_RV_CONFIG || f->shape == VECTOR_RV_ICONFIG ||
        f->shape == VECTOR_RV_RCONFIG) {
      uint32_t mask = f->shape == VECTOR_RV_RCONFIG   ? UINT32_C(0xfe00707f)
                      : f->shape == VECTOR_RV_ICONFIG ? UINT32_C(0xc000707f)
                                                      : UINT32_C(0x8000707f);
      if ((w & mask) != f->encoding)
        continue;
      if (f->shape == VECTOR_RV_RCONFIG)
        (void)snprintf(out, capacity, "%s x%u, x%u, x%u", f->name, d, a, b);
      else {
        unsigned type = w >> 20u;
        if (f->shape == VECTOR_RV_ICONFIG)
          type &= 1023u;
        if (type > 255u || (type & 7u) == 4u || ((type >> 3u) & 7u) > 3u)
          continue;
        static const char *const groups[] = {"m1", "m2",  "m4",  "m8",
                                             "",   "mf8", "mf4", "mf2"};
        (void)snprintf(out, capacity, "%s x%u, %s%u, e%u, %s, %s, %s", f->name,
                       d, f->shape == VECTOR_RV_ICONFIG ? "" : "x", a,
                       8u << ((type >> 3u) & 7u), groups[type & 7u],
                       type & 64u ? "ta" : "tu", type & 128u ? "ma" : "mu");
      }
      return 1;
    }
    int unmasked = (w & UINT32_C(0x02000000)) != 0u;
    if (f->shape == VECTOR_RV_LOAD || f->shape == VECTOR_RV_STORE) {
      if ((w & UINT32_C(0xfdf0707f)) != f->encoding ||
          (!unmasked && f->shape == VECTOR_RV_LOAD && !d))
        continue;
      (void)snprintf(out, capacity, "%s v%u, (x%u)%s", f->name, d, a,
                     unmasked ? "" : ", v0.t");
      return 1;
    }
    if ((w & UINT32_C(0xfc00707f)) != f->encoding || (!unmasked && !d))
      continue;
    char rhs[16];
    if (f->shape == VECTOR_RV_VV)
      (void)snprintf(rhs, sizeof(rhs), "v%u", a);
    else if (f->shape == VECTOR_RV_VX)
      (void)snprintf(rhs, sizeof(rhs), "x%u", a);
    else
      (void)snprintf(rhs, sizeof(rhs), "%d",
                     f->shape == VECTOR_RV_UIMM || a < 16u ? (int)a
                                                           : (int)a - 32);
    (void)snprintf(out, capacity, "%s v%u, v%u, %s%s", f->name, d, b, rhs,
                   unmasked ? "" : ", v0.t");
    return 1;
  }
  return 0;
}
#endif
