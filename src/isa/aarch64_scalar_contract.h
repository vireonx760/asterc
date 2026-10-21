#ifndef ASTERASM_AARCH64_SCALAR_CONTRACT_H
#define ASTERASM_AARCH64_SCALAR_CONTRACT_H
#include "asterasm/regalloc.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum {
  A64_SCALAR_READ_DST = 1,
  A64_SCALAR_READ_FLAGS = 2,
  A64_SCALAR_WRITE_FLAGS = 4
};
typedef enum A64ScalarShape {
  A64_SCALAR_R,
  A64_SCALAR_ADD,
  A64_SCALAR_UNARY,
  A64_SCALAR_FOUR,
  A64_SCALAR_LONG,
  A64_SCALAR_EXTRACT,
  A64_SCALAR_WIDE,
  A64_SCALAR_BITFIELD
} A64ScalarShape;
typedef enum A64ScalarId {
#define AST_A64_SCALAR(id, name, shape, w, x, effects) A64_SCALAR_ID_##id,
#include "asterasm/detail/aarch64_scalar_instructions.inc"
#undef AST_A64_SCALAR
  A64_SCALAR_COUNT
} A64ScalarId;
typedef struct A64ScalarForm {
  const char *name;
  A64ScalarShape shape;
  uint32_t word32, word64;
  uint8_t effects;
} A64ScalarForm;
static const A64ScalarForm a64_scalar_forms[A64_SCALAR_COUNT] = {
#define AST_A64_SCALAR(id, name, shape, w, x, effects)                         \
  {name, A64_SCALAR_##shape, w, x, effects},
#include "asterasm/detail/aarch64_scalar_instructions.inc"
#undef AST_A64_SCALAR
};
static inline const A64ScalarForm *a64_scalar_source(AstTargetArch arch,
                                                     AstName name) {
  if (arch != AST_TARGET_ARCH_AARCH64 || !name.text)
    return NULL;
  for (uint32_t j = 0; j < A64_SCALAR_COUNT; ++j) {
    const char *s = a64_scalar_forms[j].name;
    if (strlen(s) != name.span.length)
      continue;
    uint32_t n = 0;
    while (n < name.span.length &&
           s[n] == (char)tolower((unsigned char)name.text[n]))
      ++n;
    if (n == name.span.length)
      return &a64_scalar_forms[j];
  }
  return NULL;
}
static inline uint32_t a64_scalar_operands(const A64ScalarForm *f) {
  return f->shape == A64_SCALAR_UNARY ? 2u
         : f->shape == A64_SCALAR_R || f->shape == A64_SCALAR_ADD ||
                 f->shape == A64_SCALAR_WIDE
             ? 3u
             : 4u;
}
static inline const A64ScalarForm *
a64_scalar_form(const AstPhysicalInstructionInfo *i) {
  if (i->opcode != AST_MACHINE_A64_SCALAR || !i->operand_count ||
      i->operand_count > AST_PHYSICAL_MAX_OPERANDS)
    return NULL;
  const AstPhysicalOperand *tag = &i->operands[i->operand_count - 1u];
  return tag->kind == AST_PHYSICAL_OPERAND_IMMEDIATE && tag->bit_width == 8u &&
                 tag->immediate_bits < A64_SCALAR_COUNT
             ? &a64_scalar_forms[tag->immediate_bits]
             : NULL;
}
static inline uint32_t a64_scalar_index(const AstPhysicalOperand *o) {
  return o->register_unit == 321u ? 31u : o->register_unit - 64u;
}
static inline int a64_scalar_register(const AstPhysicalOperand *o,
                                      uint32_t bits) {
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER || o->bit_width != bits ||
      !memchr(o->register_name, 0, sizeof(o->register_name)))
    return 0;
  if (o->register_unit == 321u)
    return o->bank == AST_MACHINE_BANK_SPECIAL &&
           !strcmp(o->register_name, bits == 32u ? "wzr" : "xzr");
  if (o->bank != AST_MACHINE_BANK_GPR || o->register_unit < 64u ||
      o->register_unit >= 95u)
    return 0;
  char name[AST_MACHINE_REGISTER_NAME_CAP];
  (void)snprintf(name, sizeof(name), "%c%u", bits == 32u ? 'w' : 'x',
                 o->register_unit - 64u);
  return !strcmp(o->register_name, name);
}
static inline int a64_scalar_immediate(const AstPhysicalOperand *o,
                                       uint64_t limit) {
  return o->kind == AST_PHYSICAL_OPERAND_IMMEDIATE && o->bit_width &&
         o->bit_width <= 64u && o->immediate_bits < limit;
}
static inline int a64_scalar_contract(const AstPhysicalInstructionInfo *i) {
  const A64ScalarForm *f = a64_scalar_form(i);
  if (!f || i->raw_bytes || i->raw_byte_count || i->result_count != 1u ||
      i->operand_count != a64_scalar_operands(f) + 1u)
    return 0;
  const AstPhysicalOperand *o = i->operands;
  uint32_t bits = o[0].bit_width;
  if ((bits != 32u && bits != 64u) || !(bits == 32u ? f->word32 : f->word64) ||
      !a64_scalar_register(&o[0], bits))
    return 0;
  if (f->shape == A64_SCALAR_WIDE)
    return a64_scalar_immediate(&o[1], 65536u) &&
           a64_scalar_immediate(&o[2], bits) && !(o[2].immediate_bits % 16u);
  uint32_t source_bits = f->shape == A64_SCALAR_LONG ? 32u : bits;
  if (!a64_scalar_register(&o[1], source_bits))
    return 0;
  if (f->shape == A64_SCALAR_UNARY)
    return 1;
  if (f->shape == A64_SCALAR_BITFIELD)
    return a64_scalar_immediate(&o[2], bits) &&
           a64_scalar_immediate(&o[3], bits);
  if (f->shape == A64_SCALAR_ADD &&
      o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    uint64_t v = o[2].immediate_bits;
    /* Rn == 31 denotes SP in the immediate form, ZR in the register form. */
    return o[1].register_unit != 321u &&
           a64_scalar_immediate(&o[2], UINT64_C(0xfff001)) &&
           (v <= 4095u || !(v & 4095u));
  }
  if (!a64_scalar_register(&o[2], source_bits))
    return 0;
  if (f->shape == A64_SCALAR_FOUR || f->shape == A64_SCALAR_LONG)
    return a64_scalar_register(&o[3], bits);
  return f->shape != A64_SCALAR_EXTRACT || a64_scalar_immediate(&o[3], bits);
}
static inline uint32_t a64_scalar_word(const A64ScalarForm *f,
                                       const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  uint32_t w =
      (o[0].bit_width == 32u ? f->word32 : f->word64) | a64_scalar_index(&o[0]);
  if (f->shape == A64_SCALAR_WIDE)
    return w | ((uint32_t)o[1].immediate_bits << 5u) |
           ((uint32_t)o[2].immediate_bits / 16u << 21u);
  w |= a64_scalar_index(&o[1]) << 5u;
  if (f->shape == A64_SCALAR_UNARY)
    return w;
  if (f->shape == A64_SCALAR_BITFIELD)
    return w | ((uint32_t)o[2].immediate_bits << 16u) |
           ((uint32_t)o[3].immediate_bits << 10u);
  if (f->shape == A64_SCALAR_ADD &&
      o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    uint32_t imm = (uint32_t)o[2].immediate_bits, shift = imm > 4095u;
    return (w ^ 0x1a000000u) | (shift << 22u) |
           ((shift ? imm >> 12u : imm) << 10u);
  }
  w |= a64_scalar_index(&o[2]) << 16u;
  if (f->shape == A64_SCALAR_FOUR || f->shape == A64_SCALAR_LONG)
    w |= a64_scalar_index(&o[3]) << 10u;
  if (f->shape == A64_SCALAR_EXTRACT)
    w |= (uint32_t)o[3].immediate_bits << 10u;
  return w;
}
/* Match every fixed/reserved bit. In particular, W bitfields and EXTR must
   have N=0 and cannot use a six-bit immediate. */
static inline const A64ScalarForm *a64_scalar_decode(uint32_t w) {
  uint32_t bits = w >> 31u ? 64u : 32u;
  for (uint32_t j = 0; j < A64_SCALAR_COUNT; ++j) {
    const A64ScalarForm *f = &a64_scalar_forms[j];
    uint32_t root = bits == 32u ? f->word32 : f->word64;
    if (!root)
      continue;
    uint32_t variable =
        f->shape == A64_SCALAR_WIDE       ? 0x007fffffu
        : f->shape == A64_SCALAR_UNARY    ? 0x000003ffu
        : f->shape == A64_SCALAR_BITFIELD ? 0x003fffffu
        : f->shape == A64_SCALAR_EXTRACT  ? 0x001fffffu
        : f->shape == A64_SCALAR_FOUR || f->shape == A64_SCALAR_LONG
            ? 0x001f7fffu
            : 0x001f03ffu;
    if ((w & ~variable) != root)
      continue;
    if (f->shape == A64_SCALAR_WIDE && ((w >> 21u) & 3u) * 16u >= bits)
      continue;
    if ((f->shape == A64_SCALAR_BITFIELD && ((w >> 16u) & 63u) >= bits) ||
        ((f->shape == A64_SCALAR_BITFIELD || f->shape == A64_SCALAR_EXTRACT) &&
         ((w >> 10u) & 63u) >= bits))
      continue;
    return f;
  }
  return NULL;
}
#endif
