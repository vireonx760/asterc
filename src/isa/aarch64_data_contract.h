#ifndef ASTERASM_AARCH64_DATA_CONTRACT_H
#define ASTERASM_AARCH64_DATA_CONTRACT_H
#include "aarch64_scalar_contract.h"

typedef enum A64DataShape {
  A64_DATA_ADD,
  A64_DATA_LOGIC,
  A64_DATA_SELECT,
  A64_DATA_COMPARE
} A64DataShape;
typedef enum A64DataId {
#define AST_A64_DATA(id, name, shape, w, x) A64_DATA_ID_##id,
#include "asterasm/detail/aarch64_data_instructions.inc"
#undef AST_A64_DATA
  A64_DATA_COUNT
} A64DataId;
typedef struct A64DataForm {
  const char *name;
  A64DataShape shape;
  uint32_t word32, word64;
} A64DataForm;
static const A64DataForm a64_data_forms[A64_DATA_COUNT] = {
#define AST_A64_DATA(id, name, shape, w, x) {name, A64_DATA_##shape, w, x},
#include "asterasm/detail/aarch64_data_instructions.inc"
#undef AST_A64_DATA
};
enum { A64_DATA_EXTEND = 8, A64_DATA_MASK = 16, A64_DATA_IMMEDIATE = 17 };
static inline uint64_t a64_data_low_mask(uint32_t bits) {
  return UINT64_MAX >> (64u - bits);
}
static inline uint64_t a64_data_rotate(uint64_t v, uint32_t shift,
                                       uint32_t bits) {
  shift &= bits - 1u;
  return shift
             ? ((v >> shift) | (v << (bits - shift))) & a64_data_low_mask(bits)
             : v;
}
static inline uint64_t a64_data_replicate(uint64_t pattern, uint32_t size,
                                          uint32_t bits) {
  uint64_t value = pattern;
  for (uint32_t j = size; j < bits; j += size)
    value |= pattern << j;
  return value;
}
/* Select the smallest repeated element and its circular run of ones. This
   costs at most 64 rotations, rather than scanning all 8192 field patterns. */
static inline int a64_logical_encode(uint64_t value, uint32_t bits,
                                     uint32_t *fields) {
  if (bits != 32u && bits != 64u)
    return 0;
  uint64_t mask = a64_data_low_mask(bits), low = value & mask;
  if (value != low && !(bits == 32u && (low >> 31u) && value == (low | ~mask)))
    return 0;
  if (!low || low == mask)
    return 0;
  for (uint32_t size = 2u; size <= bits; size *= 2u) {
    uint64_t pattern = low & a64_data_low_mask(size);
    if (a64_data_replicate(pattern, size, bits) != low)
      continue;
    uint32_t ones = 0u;
    for (uint64_t p = pattern; p; p >>= 1u)
      ones += (uint32_t)(p & 1u);
    if (!ones || ones == size)
      return 0;
    for (uint32_t r = 0; r < size; ++r)
      if (a64_data_rotate(a64_data_low_mask(ones), r, size) == pattern) {
        *fields = (size == 64u ? 1u << 22u : 0u) | (r << 16u) |
                  ((((~(size - 1u) << 1u) & 63u) | (ones - 1u)) << 10u);
        return 1;
      }
    return 0;
  }
  return 0;
}
static inline int a64_logical_decode(uint32_t word, uint32_t bits,
                                     uint64_t *value) {
  uint32_t n = (word >> 22u) & 1u, s = (word >> 10u) & 63u,
           r = (word >> 16u) & 63u;
  uint32_t selector = (n << 6u) | (~s & 63u), len = 0u;
  for (uint32_t j = 1u; j <= 6u; ++j)
    if (selector & (1u << j))
      len = j;
  if (!len || (1u << len) > bits)
    return 0;
  uint32_t size = 1u << len;
  s &= size - 1u;
  r &= size - 1u;
  if (s == size - 1u)
    return 0;
  *value = a64_data_replicate(
      a64_data_rotate(a64_data_low_mask(s + 1u), r, size), size, bits);
  return 1;
}
static inline const A64DataForm *a64_data_source(AstTargetArch arch,
                                                 AstName name) {
  if (arch != AST_TARGET_ARCH_AARCH64 || !name.text)
    return NULL;
  for (uint32_t j = 0; j < A64_DATA_COUNT; ++j) {
    const char *s = a64_data_forms[j].name;
    if (strlen(s) != name.span.length)
      continue;
    uint32_t k = 0;
    while (k < name.span.length &&
           s[k] == (char)tolower((unsigned char)name.text[k]))
      ++k;
    if (k == name.span.length)
      return &a64_data_forms[j];
  }
  return NULL;
}
static inline const A64DataForm *
a64_data_form(const AstPhysicalInstructionInfo *i) {
  if (i->opcode != AST_MACHINE_A64_DATA || !i->operand_count ||
      i->operand_count > AST_PHYSICAL_MAX_OPERANDS)
    return NULL;
  const AstPhysicalOperand *tag = &i->operands[i->operand_count - 1u];
  return tag->kind == AST_PHYSICAL_OPERAND_IMMEDIATE && tag->bit_width == 8u &&
                 tag->immediate_bits < A64_DATA_COUNT
             ? &a64_data_forms[tag->immediate_bits]
             : NULL;
}
static inline int a64_data_sp(const AstPhysicalOperand *o, uint32_t bits) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_SPECIAL && o->register_unit == 320u &&
         o->bit_width == bits &&
         memchr(o->register_name, 0, sizeof(o->register_name)) &&
         !strcmp(o->register_name, bits == 32u ? "wsp" : "sp");
}
static inline uint32_t a64_data_index(const AstPhysicalOperand *o) {
  return o->register_unit >= 320u ? 31u : o->register_unit - 64u;
}
static inline int a64_data_contract(const AstPhysicalInstructionInfo *i) {
  const A64DataForm *f = a64_data_form(i);
  if (!f || i->raw_bytes || i->raw_byte_count)
    return 0;
  uint32_t count =
      f->shape == A64_DATA_ADD || f->shape == A64_DATA_LOGIC ? 6u : 5u;
  if (i->operand_count != count ||
      i->result_count != (uint32_t)(f->shape != A64_DATA_COMPARE))
    return 0;
  const AstPhysicalOperand *o = i->operands;
  uint32_t bits = o[0].bit_width;
  if ((bits != 32u && bits != 64u) || !a64_scalar_register(&o[0], bits))
    return 0;
  if (f->shape == A64_DATA_COMPARE)
    return (a64_scalar_register(&o[1], bits) ||
            a64_scalar_immediate(&o[1], 32u)) &&
           a64_scalar_immediate(&o[2], 16u) && a64_scalar_immediate(&o[3], 16u);
  if (f->shape == A64_DATA_SELECT)
    return a64_scalar_register(&o[1], bits) &&
           a64_scalar_register(&o[2], bits) && a64_scalar_immediate(&o[3], 16u);
  if (!a64_scalar_immediate(&o[3], 18u) || !a64_scalar_immediate(&o[4], 64u))
    return 0;
  uint32_t mode = (uint32_t)o[3].immediate_bits,
           amount = (uint32_t)o[4].immediate_bits;
  if (mode == A64_DATA_MASK) {
    uint32_t fields;
    return f->shape == A64_DATA_LOGIC && !(f->word32 & 0x00200000u) &&
           !amount && a64_scalar_register(&o[1], bits) &&
           o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           a64_scalar_immediate(&o[2], UINT64_MAX) &&
           a64_logical_encode(o[2].immediate_bits, bits, &fields) &&
           ((f->word32 & 0x60000000u) == 0x60000000u ||
            o[0].register_unit != 321u);
  }
  if (mode == A64_DATA_IMMEDIATE) {
    uint64_t v = o[2].immediate_bits;
    return f->shape == A64_DATA_ADD && !amount &&
           (a64_scalar_register(&o[1], bits) || a64_data_sp(&o[1], bits)) &&
           o[1].register_unit != 321u &&
           a64_scalar_immediate(&o[2], UINT64_C(0xfff001)) &&
           (v <= 4095u || !(v & 4095u)) &&
           ((f->word32 & 0x20000000u) || o[0].register_unit != 321u);
  }
  if (mode >= A64_DATA_EXTEND) {
    uint32_t option = mode - A64_DATA_EXTEND;
    if (f->shape != A64_DATA_ADD || option >= 8u || amount > 4u ||
        o[1].register_unit == 321u ||
        (!a64_scalar_register(&o[1], bits) && !a64_data_sp(&o[1], bits)) ||
        (!(f->word32 & 0x20000000u) && o[0].register_unit == 321u))
      return 0;
    return a64_scalar_register(&o[2],
                               bits == 64u && (option & 3u) == 3u ? 64u : 32u);
  }
  return mode < (f->shape == A64_DATA_ADD ? 3u : 4u) && amount < bits &&
         a64_scalar_register(&o[1], bits) && a64_scalar_register(&o[2], bits);
}
static inline uint32_t a64_data_word(const A64DataForm *f,
                                     const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  uint32_t w = o[0].bit_width == 32u ? f->word32 : f->word64;
  if (f->shape == A64_DATA_COMPARE)
    return w | (a64_data_index(&o[0]) << 5u) |
           (o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE
                ? 0x800u | ((uint32_t)o[1].immediate_bits << 16u)
                : a64_data_index(&o[1]) << 16u) |
           (uint32_t)o[2].immediate_bits |
           ((uint32_t)o[3].immediate_bits << 12u);
  w |= a64_data_index(&o[0]) | (a64_data_index(&o[1]) << 5u);
  if (f->shape == A64_DATA_SELECT)
    return w | (a64_data_index(&o[2]) << 16u) |
           ((uint32_t)o[3].immediate_bits << 12u);
  uint32_t mode = (uint32_t)o[3].immediate_bits,
           amount = (uint32_t)o[4].immediate_bits;
  if (mode == A64_DATA_MASK) {
    uint32_t fields = 0;
    (void)a64_logical_encode(o[2].immediate_bits, o[0].bit_width, &fields);
    return (w ^ 0x18000000u) | fields;
  }
  if (mode == A64_DATA_IMMEDIATE) {
    uint32_t v = (uint32_t)o[2].immediate_bits, shift = v > 4095u;
    return (w ^ 0x1a000000u) | (shift << 22u) | ((shift ? v >> 12u : v) << 10u);
  }
  w |= a64_data_index(&o[2]) << 16u;
  return mode >= A64_DATA_EXTEND
             ? w | 0x00200000u | ((mode - A64_DATA_EXTEND) << 13u) |
                   (amount << 10u)
             : w | (mode << 22u) | (amount << 10u);
}
typedef struct A64DataDecoded {
  const A64DataForm *form;
  uint32_t bits, mode, amount;
  uint64_t mask;
} A64DataDecoded;
static inline int a64_data_decode(uint32_t word, A64DataDecoded *d) {
  memset(d, 0, sizeof(*d));
  d->bits = word >> 31u ? 64u : 32u;
  for (uint32_t j = 0; j < A64_DATA_COUNT; ++j) {
    const A64DataForm *f = &a64_data_forms[j];
    uint32_t root = d->bits == 32u ? f->word32 : f->word64;
    if (f->shape == A64_DATA_SELECT) {
      if ((word & 0xffe00c00u) != root)
        continue;
    } else if (f->shape == A64_DATA_COMPARE) {
      if ((word & 0xffe00410u) != root)
        continue;
    } else if ((word & 0xff200000u) == root) {
      d->mode = (word >> 22u) & 3u;
      d->amount = (word >> 10u) & 63u;
      if (d->amount >= d->bits || (f->shape == A64_DATA_ADD && d->mode == 3u))
        continue;
    } else if (f->shape == A64_DATA_ADD &&
               (word & 0xffe00000u) == (root | 0x00200000u)) {
      d->mode = A64_DATA_EXTEND + ((word >> 13u) & 7u);
      d->amount = (word >> 10u) & 7u;
      if (d->amount > 4u)
        continue;
    } else if (f->shape == A64_DATA_ADD &&
               (word & 0xff800000u) == (root ^ 0x1a000000u))
      d->mode = A64_DATA_IMMEDIATE;
    else if (f->shape == A64_DATA_LOGIC && !(root & 0x00200000u) &&
             (word & 0xff800000u) == (root ^ 0x18000000u)) {
      if (!a64_logical_decode(word, d->bits, &d->mask))
        continue;
      d->mode = A64_DATA_MASK;
    } else
      continue;
    d->form = f;
    return 1;
  }
  return 0;
}
#endif
