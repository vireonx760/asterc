/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ASTERASM_DUMP_SCALABLE_H
#define ASTERASM_DUMP_SCALABLE_H
#include "../isa/aarch64_scalable.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static inline char scalable_suffix(uint32_t bits) {
  return bits == 8u    ? 'b'
         : bits == 16u ? 'h'
         : bits == 32u ? 's'
         : bits == 64u ? 'd'
                       : 'q';
}
static inline int scalable_append(char *out, size_t capacity, size_t *used,
                                  const char *text) {
  size_t length = strlen(text);
  if (*used >= capacity || length >= capacity - *used)
    return 0;
  memcpy(out + *used, text, length + 1u);
  *used += length;
  return 1;
}
static inline int dump_scalable_operand(const A64ScalableType *t,
                                        const AstPhysicalOperand *o,
                                        uint64_t raw, char *out,
                                        size_t capacity) {
  if (t->kind < A64_SC_I) {
    uint32_t index = o->register_unit % 64u;
    size_t used = 0u;
    out[0] = 0;
    if (t->kind == A64_SC_Z && t->group > 1u) {
      if (!scalable_append(out, capacity, &used, "{"))
        return 0;
      for (uint32_t j = 0; j < t->group; ++j) {
        char reg[24];
        (void)snprintf(reg, sizeof(reg), "%sz%u.%c", j ? ", " : "",
                       (index + j) % 32u, scalable_suffix(t->lane));
        if (!scalable_append(out, capacity, &used, reg))
          return 0;
      }
      return scalable_append(out, capacity, &used, "}");
    }
    char reg[48];
    if (t->kind == A64_SC_F && t->width == 128u)
      (void)snprintf(reg, sizeof(reg), "v%u", index);
    else if (t->lane)
      (void)snprintf(reg, sizeof(reg), "%s.%c", o->register_name,
                     scalable_suffix(t->lane));
    else
      (void)snprintf(reg, sizeof(reg), "%s", o->register_name);
    if (!scalable_append(out, capacity, &used, reg))
      return 0;
    if (t->extend || t->shift) {
      (void)snprintf(reg, sizeof(reg), ", %s #%u",
                     t->extend == 1u   ? "uxtw"
                     : t->extend == 2u ? "sxtw"
                                       : "lsl",
                     t->shift);
      if (!scalable_append(out, capacity, &used, reg))
        return 0;
    }
    return 1;
  }
  uint64_t bits = o->immediate_bits;
  if (t->kind == A64_SC_FP_PAIR || t->kind == A64_SC_FP8) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    int n = snprintf(out, capacity, "#%.17g", d);
    if (n < 0 || (size_t)n >= capacity)
      return 0;
    if (!strchr(out, '.') && !strchr(out, 'e')) {
      size_t used = (size_t)n;
      return scalable_append(out, capacity, &used, ".0");
    }
    return 1;
  }
  if (t->kind == A64_SC_MODE) {
    (void)snprintf(out, capacity, "%s",
                   bits == 1u   ? "sm"
                   : bits == 2u ? "za"
                                : "");
    return 1;
  }
  if (t->kind == A64_SC_LOGICAL) {
    (void)snprintf(out, capacity, "#0x%" PRIx64, bits);
    return 1;
  }
  if (!strcmp(t->name, "MatrixTileList")) {
    size_t used = 0u;
    out[0] = 0;
    if (!scalable_append(out, capacity, &used, "{"))
      return 0;
    for (uint32_t j = 0, n = 0; j < 8u; ++j)
      if (bits & (UINT64_C(1) << j)) {
        char tile[20];
        (void)snprintf(tile, sizeof(tile), "%sza%u.d", n++ ? ", " : "", j);
        if (!scalable_append(out, capacity, &used, tile))
          return 0;
      }
    return scalable_append(out, capacity, &used, "}");
  }
  if (t->kind == A64_SC_IMM8 || t->kind == A64_SC_CPY8) {
    int value = (int)(raw & 255u);
    if (t->kind == A64_SC_CPY8 && value >= 128)
      value -= 256;
    (void)snprintf(out, capacity, "#%d%s", value, raw & 256u ? ", lsl #8" : "");
    return 1;
  }
  int64_t value = bits <= INT64_MAX ? (int64_t)bits : -(int64_t)(~bits) - 1;
  if (!strncmp(t->name, "VectorIndex", 11u) ||
      !strncmp(t->name, "sve_elm_idx_extdup", 18u))
    (void)snprintf(out, capacity, "[%" PRId64 "]", value);
  else
    (void)snprintf(out, capacity, "#%" PRId64, value);
  return 1;
}
static inline int dump_scalable_word(uint32_t word, char *out,
                                     size_t capacity) {
  AstPhysicalInstructionInfo packet;
  if (!a64_scalable_decode(word, &packet))
    return 0;
  const A64ScalableForm *f = a64_scalable_form(&packet);
  uint64_t raw[AST_PHYSICAL_MAX_OPERANDS] = {0};
  for (uint32_t j = 0; j < f->field_count; ++j) {
    const A64ScalableField *field = &a64_scalable_fields[f->field_offset + j];
    raw[field->parameter] |=
        (uint64_t)((word >> field->destination) & ((1u << field->count) - 1u))
        << field->source;
  }
  size_t used = 0u;
  out[0] = 0;
  for (const char *p = f->assembly; *p;) {
    if (*p != '$') {
      char c[2] = {*p++, 0};
      if (!scalable_append(out, capacity, &used, c))
        return 0;
    } else {
      ++p;
      uint32_t index = 0u;
      while (*p >= '0' && *p <= '9')
        index = index * 10u + (uint32_t)(*p++ - '0');
      if (index >= f->parameter_count)
        return 0;
      const A64ScalableParameter *parameter =
          &a64_scalable_parameters[f->parameter_offset + index];
      char value[128];
      if (!dump_scalable_operand(&a64_scalable_types[parameter->type],
                                 &packet.operands[index], raw[index], value,
                                 sizeof(value)) ||
          !scalable_append(out, capacity, &used, value))
        return 0;
    }
  }
  return 1;
}
#endif
