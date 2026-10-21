/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ASTERASM_DUMP_SYSTEM_H
#define ASTERASM_DUMP_SYSTEM_H
#include "../isa/system_contract.h"
#include <stdio.h>

static inline int dump_system_word(AstTargetArch arch, uint32_t w, char *out,
                                   size_t capacity) {
  for (uint32_t j = 0; j < SYSTEM_COUNT; ++j) {
    const SystemForm *f = &system_forms[j];
    if (f->arch != arch)
      continue;
    if ((f->shape == SYSTEM_SHAPE_FIXED ||
         f->shape == SYSTEM_SHAPE_EXCEPTION_RETURN) &&
        w == f->encoding) {
      (void)snprintf(out, capacity, "%s", f->name);
      return 1;
    }
  }
  if (arch == AST_TARGET_ARCH_AARCH64) {
    for (uint32_t j = SYSTEM_A64_DAIFSET; j <= SYSTEM_A64_SPSEL; ++j) {
      const SystemForm *f = &system_forms[j];
      unsigned value = (w >> 8u) & 15u;
      if ((w & UINT32_C(0xfffff0ff)) == f->encoding &&
          (j != SYSTEM_A64_SPSEL || value <= 1u)) {
        static const char *const names[] = {"daifset", "daifclr", "spsel"};
        (void)snprintf(out, capacity, "msr %s, #%u",
                       names[j - SYSTEM_A64_DAIFSET], value);
        return 1;
      }
    }
    unsigned r = w & 31u;
    char rt[8];
    if (r == 31u)
      memcpy(rt, "xzr", 4u);
    else
      (void)snprintf(rt, sizeof(rt), "x%u", r);
    uint32_t form = w & UINT32_C(0xffe00000);
    if ((form == UINT32_C(0xd5200000) || form == UINT32_C(0xd5000000)) &&
        ((w >> 19u) & 3u) >= 2u) {
      unsigned s = (w >> 5u) & 65535u;
      char sys[40];
      (void)snprintf(sys, sizeof(sys), "s%u_%u_c%u_c%u_%u", s >> 14u,
                     (s >> 11u) & 7u, (s >> 7u) & 15u, (s >> 3u) & 15u, s & 7u);
      if (form == UINT32_C(0xd5200000))
        (void)snprintf(out, capacity, "mrs %s, %s", rt, sys);
      else
        (void)snprintf(out, capacity, "msr %s, %s", sys, rt);
      return 1;
    }
    form = w & UINT32_C(0xfff80000);
    if (form == UINT32_C(0xd5080000) || form == UINT32_C(0xd5280000)) {
      if (form == UINT32_C(0xd5280000))
        (void)snprintf(out, capacity, "sysl %s, #%u, c%u, c%u, #%u", rt,
                       (w >> 16u) & 7u, (w >> 12u) & 15u, (w >> 8u) & 15u,
                       (w >> 5u) & 7u);
      else
        (void)snprintf(out, capacity, "sys #%u, c%u, c%u, #%u, %s",
                       (w >> 16u) & 7u, (w >> 12u) & 15u, (w >> 8u) & 15u,
                       (w >> 5u) & 7u, rt);
      return 1;
    }
    for (uint32_t j = SYSTEM_A64_DMB; j <= SYSTEM_A64_ISB; ++j) {
      const SystemForm *f = &system_forms[j];
      unsigned option = (w >> 8u) & 15u;
      if ((w & UINT32_C(0xfffff0ff)) == f->encoding &&
          (j == SYSTEM_A64_ISB ? option == 15u : (option & 3u) != 0u)) {
        (void)snprintf(out, capacity, "%s #0x%x", f->name, option);
        return 1;
      }
    }
    for (uint32_t j = SYSTEM_A64_SVC; j <= SYSTEM_A64_BRK; ++j)
      if ((w & UINT32_C(0xffe0001f)) == system_forms[j].encoding) {
        (void)snprintf(out, capacity, "%s #0x%x", system_forms[j].name,
                       (w >> 5u) & 65535u);
        return 1;
      }
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    unsigned rd = (w >> 7u) & 31u, rs1 = (w >> 15u) & 31u,
             rs2 = (w >> 20u) & 31u;
    if ((w & 127u) == 0x73u) {
      unsigned funct = (w >> 12u) & 7u;
      if (funct && funct != 4u) {
        static const char *const names[] = {"", "csrrw",  "csrrs",  "csrrc",
                                            "", "csrrwi", "csrrsi", "csrrci"};
        /* Read-only CSR writes have defined illegal-instruction behavior,
           but are outside the accepted instruction contract. */
        if (((w >> 20u) & 0xc00u) == 0xc00u && ((funct & 3u) == 1u || rs1))
          return 0;
        if (funct < 4u)
          (void)snprintf(out, capacity, "%s x%u, 0x%x, x%u", names[funct], rd,
                         w >> 20u, rs1);
        else
          (void)snprintf(out, capacity, "%s x%u, 0x%x, %u", names[funct], rd,
                         w >> 20u, rs1);
        return 1;
      }
      for (uint32_t j = 0; j < SYSTEM_COUNT; ++j) {
        const SystemForm *f = &system_forms[j];
        if (f->arch == arch && f->shape == SYSTEM_SHAPE_RV_FENCE_REG &&
            (w & UINT32_C(0xfe007fff)) == f->encoding) {
          (void)snprintf(out, capacity, "%s x%u, x%u", f->name, rs1, rs2);
          return 1;
        }
      }
    }
    if ((w & UINT32_C(0xf00fffff)) == UINT32_C(0x0000000f)) {
      char sets[2][5];
      for (unsigned k = 0; k < 2u; ++k) {
        unsigned flags = (w >> (k ? 20u : 24u)) & 15u, used = 0u;
        static const char names[] = "iorw";
        for (unsigned bit = 0; bit < 4u; ++bit)
          if (flags & (8u >> bit))
            sets[k][used++] = names[bit];
        if (!used)
          sets[k][used++] = '0';
        sets[k][used] = 0;
      }
      (void)snprintf(out, capacity, "fence %s, %s", sets[0], sets[1]);
      return 1;
    }
  }
  return 0;
}
#endif
