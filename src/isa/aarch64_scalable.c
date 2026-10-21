#include "aarch64_scalable.h"
#include "aarch64_data_contract.h"
#include "aarch64_scalable_tables.inc"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

uint32_t a64_scalable_find(AstName name) {
  if (!name.text || !name.span.length || name.span.length > 32u)
    return a64_scalable_form_count;
  char text[33];
  for (uint32_t j = 0; j < name.span.length; ++j)
    text[j] = (char)tolower((unsigned char)name.text[j]);
  text[name.span.length] = 0;
  uint32_t lo = 0u, hi = a64_scalable_form_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    if (strcmp(a64_scalable_forms[mid].name, text) < 0)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo < a64_scalable_form_count &&
                 !strcmp(a64_scalable_forms[lo].name, text)
             ? lo
             : a64_scalable_form_count;
}
const A64ScalableForm *a64_scalable_form(const AstPhysicalInstructionInfo *i) {
  if (!i || i->opcode != AST_MACHINE_A64_SCALABLE || !i->operand_count ||
      i->operand_count > AST_PHYSICAL_MAX_OPERANDS)
    return NULL;
  const AstPhysicalOperand *tag = &i->operands[i->operand_count - 1u];
  return tag->kind == AST_PHYSICAL_OPERAND_IMMEDIATE && tag->bit_width == 16u &&
                 tag->immediate_bits < a64_scalable_form_count
             ? &a64_scalable_forms[tag->immediate_bits]
             : NULL;
}
int a64_scalable_features(const AstTargetConfig *target,
                          const A64ScalableForm *f) {
  return target && target->arch == AST_TARGET_ARCH_AARCH64 && f &&
         !(f->required_all & ~target->features) &&
         (!f->required_any || (f->required_any & target->features));
}

int a64_scalable_register(const A64ScalableType *t, uint32_t index,
                          AstPhysicalOperand *o) {
  if (index < t->first || index > t->last || t->kind >= A64_SC_I)
    return 0;
  memset(o, 0, sizeof(*o));
  o->kind = AST_PHYSICAL_OPERAND_REGISTER;
  o->bit_width = t->width;
  const char *prefix = "";
  const char *suffix = "";
  switch (t->kind) {
  case A64_SC_Z:
    o->bank = AST_MACHINE_BANK_VECTOR;
    o->register_unit = 128u + index;
    prefix = "z";
    break;
  case A64_SC_P:
    o->bank = AST_MACHINE_BANK_MASK;
    o->register_unit = 256u + index;
    prefix = "p";
    break;
  case A64_SC_F:
    o->bank = AST_MACHINE_BANK_FP;
    o->register_unit = 128u + index;
    prefix = t->width == 8u    ? "b"
             : t->width == 16u ? "h"
             : t->width == 32u ? "s"
             : t->width == 64u ? "d"
                               : "q";
    break;
  case A64_SC_G:
  case A64_SC_SP:
    o->bank = AST_MACHINE_BANK_GPR;
    o->register_unit = 64u + index;
    prefix = t->width == 32u ? "w" : "x";
    if (index == 31u) {
      o->bank = AST_MACHINE_BANK_SPECIAL;
      o->register_unit = t->kind == A64_SC_SP ? 320u : 321u;
      (void)snprintf(o->register_name, sizeof(o->register_name), "%s",
                     t->kind == A64_SC_SP ? (t->width == 32u ? "wsp" : "sp")
                                          : (t->width == 32u ? "wzr" : "xzr"));
      return 1;
    }
    break;
  case A64_SC_ZA:
  case A64_SC_ZAH:
  case A64_SC_ZAV:
    /* Tiles at different element widths overlap one architectural ZA. */
    o->bank = AST_MACHINE_BANK_SPECIAL;
    o->register_unit = 352u;
    prefix = "za";
    suffix = t->kind == A64_SC_ZAH ? "h" : t->kind == A64_SC_ZAV ? "v" : "";
    if (!t->lane) {
      memcpy(o->register_name, "za", 3u);
      return 1;
    }
    break;
  default:
    return 0;
  }
  (void)snprintf(o->register_name, sizeof(o->register_name), "%s%u%s", prefix,
                 index, suffix);
  return 1;
}
static int register_code(const A64ScalableType *t, const AstPhysicalOperand *o,
                         uint64_t *value) {
  if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      !memchr(o->register_name, 0, sizeof(o->register_name)))
    return 0;
  uint32_t index;
  if (t->kind == A64_SC_ZA || t->kind == A64_SC_ZAH || t->kind == A64_SC_ZAV) {
    if (!t->lane)
      index = 0u;
    else {
      const unsigned char *p = (const unsigned char *)o->register_name + 2u;
      if (o->register_name[0] != 'z' || o->register_name[1] != 'a' ||
          !isdigit(*p))
        return 0;
      index = 0u;
      while (isdigit(*p)) {
        if (index > 15u)
          return 0;
        index = index * 10u + (uint32_t)(*p++ - '0');
      }
    }
  } else if (o->bank == AST_MACHINE_BANK_SPECIAL)
    index = 31u;
  else
    index = o->register_unit % 64u;
  AstPhysicalOperand expected;
  if (!a64_scalable_register(t, index, &expected) || expected.bank != o->bank ||
      expected.bit_width != o->bit_width ||
      expected.register_unit != o->register_unit ||
      strcmp(expected.register_name, o->register_name))
    return 0;
  *value = index - t->first;
  return 1;
}

static double fp_value(uint32_t imm) {
  /* A64 floating immediate: sign, three-bit exponent, four fraction bits. */
  double value = (double)(16u + (imm & 15u)) / 16.0;
  int exponent = (int)((imm >> 4u) & 7u);
  exponent = exponent < 4 ? exponent + 1 : exponent - 7;
  while (exponent > 0) {
    value *= 2.0;
    --exponent;
  }
  while (exponent < 0) {
    value *= 0.5;
    ++exponent;
  }
  return imm & 128u ? -value : value;
}
static int64_t signed_bits(uint64_t value) {
  return value <= INT64_MAX ? (int64_t)value : -(int64_t)(~value) - 1;
}
int a64_scalable_operand(const A64ScalableType *t, const AstPhysicalOperand *o,
                         uint64_t *encoded) {
  if (t->kind < A64_SC_I)
    return register_code(t, o, encoded);
  if (o->kind != AST_PHYSICAL_OPERAND_IMMEDIATE || o->bit_width != 64u)
    return 0;
  uint64_t value = o->immediate_bits;
  int64_t v = signed_bits(value);
  switch (t->kind) {
  case A64_SC_FP8:
  case A64_SC_FP_PAIR: {
    double d;
    memcpy(&d, &value, sizeof(d));
    if (t->kind == A64_SC_FP_PAIR) {
      /* The architectural pair contains positive zero, never IEEE -0.0. */
      if (d == 0.0 && (value >> 63u))
        return 0;
      if (d == (double)t->low / 2.0)
        *encoded = 0u;
      else if (d == (double)t->high / 2.0)
        *encoded = 1u;
      else
        return 0;
      return 1;
    }
    for (uint32_t j = 0; j < 256u; ++j)
      if (d == fp_value(j)) {
        *encoded = j;
        return 1;
      }
    return 0;
  }
  case A64_SC_LOGICAL: {
    uint32_t fields;
    if (!a64_logical_encode(value, 64u, &fields))
      return 0;
    *encoded = (((fields >> 22u) & 1u) << 12u) |
               (((fields >> 16u) & 63u) << 6u) | ((fields >> 10u) & 63u);
    return 1;
  }
  case A64_SC_IMM8:
  case A64_SC_CPY8: {
    int negative = t->kind == A64_SC_CPY8;
    int64_t lo = negative ? -128 : 0, hi = negative ? 127 : 255;
    if (v >= lo && v <= hi) {
      *encoded = value & 255u;
      return 1;
    }
    if (t->width == 8u || v % 256 || v / 256 < lo || v / 256 > hi)
      return 0;
    *encoded = ((uint64_t)(v / 256) & 255u) | 256u;
    return 1;
  }
  default:
    if (v < t->low || v > t->high || (v - t->low) % t->scale)
      return 0;
    if (t->kind == A64_SC_ROT)
      *encoded = (uint64_t)((v - t->low) / t->scale);
    else if (t->kind == A64_SC_INC)
      *encoded = value - 1u;
    else if (t->transform > 0)
      *encoded = (uint64_t)t->transform - value;
    else if (t->transform < 0)
      *encoded = value - (uint64_t)(-t->transform);
    else
      *encoded = (uint64_t)(v / t->scale);
    return 1;
  }
}
int a64_scalable_decode_operand(const A64ScalableType *t, uint64_t encoded,
                                AstPhysicalOperand *o) {
  if (t->kind < A64_SC_I)
    return a64_scalable_register(t, (uint32_t)encoded + t->first, o);
  memset(o, 0, sizeof(*o));
  o->kind = AST_PHYSICAL_OPERAND_IMMEDIATE;
  o->bit_width = 64u;
  uint64_t value = encoded;
  switch (t->kind) {
  case A64_SC_FP8:
  case A64_SC_FP_PAIR: {
    if (encoded > (t->kind == A64_SC_FP8 ? 255u : 1u))
      return 0;
    double d = t->kind == A64_SC_FP8
                   ? fp_value((uint32_t)encoded)
                   : (double)(encoded ? t->high : t->low) / 2.0;
    memcpy(&value, &d, sizeof(value));
    break;
  }
  case A64_SC_LOGICAL: {
    uint32_t fields = (uint32_t)(((encoded >> 12u) & 1u) << 22u) |
                      (uint32_t)(((encoded >> 6u) & 63u) << 16u) |
                      (uint32_t)((encoded & 63u) << 10u);
    if (encoded > 8191u || !a64_logical_decode(fields, 64u, &value))
      return 0;
    break;
  }
  case A64_SC_IMM8:
  case A64_SC_CPY8:
    if (encoded > 511u || (t->width == 8u && (encoded & 256u)))
      return 0;
    value = encoded & 255u;
    if (t->kind == A64_SC_CPY8 && (value & 128u))
      value |= ~UINT64_C(255);
    if (encoded & 256u)
      value *= 256u;
    break;
  case A64_SC_ROT:
    value = encoded * (uint64_t)t->scale + (uint64_t)t->low;
    break;
  case A64_SC_INC:
    value = encoded + 1u;
    break;
  default:
    if (t->transform > 0)
      value = (uint64_t)t->transform - encoded;
    else if (t->transform < 0)
      value = (encoded + (uint64_t)(-t->transform)) &
              ((uint64_t)(-t->transform) - 1u);
    else {
      uint64_t max = (uint64_t)(t->high / t->scale);
      if (t->low < 0) {
        uint64_t sign = max + 1u;
        if (encoded & sign)
          value = encoded | ~(sign * 2u - 1u);
      }
      value *= (uint64_t)t->scale;
    }
    break;
  }
  o->immediate_bits = value;
  uint64_t ignored;
  return a64_scalable_operand(t, o, &ignored);
}
int a64_scalable_contract(const AstPhysicalInstructionInfo *i) {
  const A64ScalableForm *f = a64_scalable_form(i);
  if (!f || i->raw_bytes || i->raw_byte_count || i->nontemporal ||
      i->operand_count != (uint32_t)f->parameter_count + 1u ||
      i->result_count != f->results)
    return 0;
  uint64_t codes[AST_PHYSICAL_MAX_OPERANDS];
  for (uint32_t j = 0; j < f->parameter_count; ++j) {
    const A64ScalableParameter *p =
        &a64_scalable_parameters[f->parameter_offset + j];
    if (!a64_scalable_operand(&a64_scalable_types[p->type], &i->operands[j],
                              &codes[j]))
      return 0;
    if (p->tied >= 0 && codes[j] != codes[(uint32_t)p->tied])
      return 0;
  }
  return 1;
}
uint32_t a64_scalable_word(const A64ScalableForm *f,
                           const AstPhysicalInstructionInfo *i) {
  uint64_t codes[AST_PHYSICAL_MAX_OPERANDS] = {0};
  for (uint32_t j = 0; j < f->parameter_count; ++j) {
    const A64ScalableParameter *p =
        &a64_scalable_parameters[f->parameter_offset + j];
    (void)a64_scalable_operand(&a64_scalable_types[p->type], &i->operands[j],
                               &codes[j]);
  }
  uint32_t word = f->encoding;
  for (uint32_t j = 0; j < f->field_count; ++j) {
    const A64ScalableField *field = &a64_scalable_fields[f->field_offset + j];
    word |= (uint32_t)((codes[field->parameter] >> field->source) &
                       ((UINT64_C(1) << field->count) - 1u))
            << field->destination;
  }
  return word;
}
int a64_scalable_decode(uint32_t word, AstPhysicalInstructionInfo *out) {
  for (uint32_t k = 0; k < a64_scalable_form_count; ++k) {
    const A64ScalableForm *f = &a64_scalable_forms[k];
    if ((word & f->mask) != f->encoding)
      continue;
    uint64_t codes[AST_PHYSICAL_MAX_OPERANDS] = {0};
    for (uint32_t j = 0; j < f->field_count; ++j) {
      const A64ScalableField *field = &a64_scalable_fields[f->field_offset + j];
      codes[field->parameter] |=
          (uint64_t)((word >> field->destination) & ((1u << field->count) - 1u))
          << field->source;
    }
    memset(out, 0, sizeof(*out));
    out->opcode = AST_MACHINE_A64_SCALABLE;
    out->operand_count = (uint32_t)f->parameter_count + 1u;
    out->result_count = f->results;
    int valid = 1;
    for (uint32_t j = 0; j < f->parameter_count; ++j) {
      const A64ScalableParameter *p =
          &a64_scalable_parameters[f->parameter_offset + j];
      if (p->tied >= 0)
        codes[j] = codes[(uint32_t)p->tied];
      if (!a64_scalable_decode_operand(&a64_scalable_types[p->type], codes[j],
                                       &out->operands[j])) {
        valid = 0;
        break;
      }
    }
    if (!valid)
      continue;
    AstPhysicalOperand *tag = &out->operands[f->parameter_count];
    tag->kind = AST_PHYSICAL_OPERAND_IMMEDIATE;
    tag->bit_width = 16u;
    tag->immediate_bits = k;
    if (a64_scalable_contract(out))
      return 1;
  }
  return 0;
}
