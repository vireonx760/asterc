#include "encoder_internal.h"

typedef struct PendingPatch {
  size_t offset;
  uint64_t bits;
  unsigned width;
} PendingPatch;
static uint32_t read_word(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
         ((uint32_t)p[3] << 24u);
}
static int relocation_error(AstEncoderOutput *out, AstEncoderDiagCode code,
                            const char *message) {
  out->diagnostic = code;
  out->message = message;
  out->instruction = AST_MACHINE_INVALID_ID;
  return 0;
}
static int add_signed(uint64_t value, int64_t addend, uint64_t *result) {
  if (addend >= 0) {
    if (value > UINT64_MAX - (uint64_t)addend)
      return 0;
    *result = value + (uint64_t)addend;
  } else {
    uint64_t magnitude = UINT64_C(0) - (uint64_t)addend;
    if (value < magnitude)
      return 0;
    *result = value - magnitude;
  }
  return 1;
}
static int relative(uint64_t target, uint64_t pc, int64_t *delta) {
  if (target >= pc) {
    if (target - pc > INT64_MAX)
      return 0;
    *delta = (int64_t)(target - pc);
  } else {
    uint64_t magnitude = pc - target;
    if (magnitude > (UINT64_C(1) << 63u))
      return 0;
    *delta =
        magnitude == (UINT64_C(1) << 63u) ? INT64_MIN : -(int64_t)magnitude;
  }
  return 1;
}
static int patch_record(AstEncoderOutput *out, const AstEncoderFixup *f,
                        uint64_t base, uint64_t symbol, PendingPatch *patch) {
  uint64_t target, pc;
  int64_t delta;
  if (!f->width || f->width > 8u || f->offset > out->size ||
      f->width > out->size - f->offset || base > UINT64_MAX - f->offset)
    return relocation_error(out, AST_ENCODER_DIAG_INVALID_FIXUP,
                            "Relocation field is outside the encoded buffer.");
  if (!add_signed(symbol, f->addend, &target))
    goto range;
  pc = base + f->offset;
  patch->offset = f->offset;
  patch->width = f->width;
  switch (f->kind) {
  case AST_ENCODER_FIXUP_ABS32:
    if (f->width != 4u)
      goto invalid;
    if (target > UINT32_MAX)
      goto range;
    patch->bits = target;
    return 1;
  case AST_ENCODER_FIXUP_ABS64:
    if (f->width != 8u)
      goto invalid;
    patch->bits = target;
    return 1;
  case AST_ENCODER_FIXUP_PC_REL32:
  case AST_ENCODER_FIXUP_X86_CALL_REL32:
    if (f->width != 4u)
      goto invalid;
    if (pc > UINT64_MAX - 4u || !relative(target, pc + 4u, &delta) ||
        delta < INT32_MIN || delta > INT32_MAX)
      goto range;
    patch->bits = (uint32_t)delta;
    return 1;
  case AST_ENCODER_FIXUP_A64_CALL26: {
    if (f->width != 4u)
      goto invalid;
    uint32_t w = read_word(out->bytes + f->offset);
    if ((w & 0xfc000000u) != 0x94000000u || (pc & 3u))
      goto invalid;
    if (!relative(target, pc, &delta) || delta % 4 ||
        delta < -INT64_C(134217728) || delta > INT64_C(134217724))
      goto range;
    patch->bits = (w & 0xfc000000u) | ((uint32_t)(delta / 4) & 0x03ffffffu);
    return 1;
  }
  case AST_ENCODER_FIXUP_A64_ADR21: {
    if (f->width != 4u)
      goto invalid;
    uint32_t w = read_word(out->bytes + f->offset);
    if ((w & 0x9f000000u) != 0x10000000u || (pc & 3u))
      goto invalid;
    if (!relative(target, pc, &delta) || delta < -INT64_C(1048576) ||
        delta > INT64_C(1048575))
      goto range;
    uint32_t u = (uint32_t)delta & 0x1fffffu;
    patch->bits = (w & 0x9f00001fu) | ((u & 3u) << 29u) | ((u >> 2u) << 5u);
    return 1;
  }
  case AST_ENCODER_FIXUP_A64_ADRP_ADD: {
    if (f->width != 8u)
      goto invalid;
    uint32_t hi = read_word(out->bytes + f->offset),
             lo = read_word(out->bytes + f->offset + 4u);
    if ((pc & 3u) || (hi & 0x9f000000u) != 0x90000000u ||
        (lo & 0xffc00000u) != 0x91000000u || (hi & 31u) == 31u ||
        (lo & 31u) != (hi & 31u) || ((lo >> 5u) & 31u) != (hi & 31u))
      goto invalid;
    if (!relative(target >> 12u, pc >> 12u, &delta) ||
        delta < -INT64_C(1048576) || delta > INT64_C(1048575))
      goto range;
    uint32_t u = (uint32_t)delta & 0x1fffffu;
    hi = (hi & 0x9f00001fu) | ((u & 3u) << 29u) | ((u >> 2u) << 5u);
    lo = (lo & ~UINT32_C(0x003ffc00)) | ((uint32_t)(target & 4095u) << 10u);
    patch->bits = (uint64_t)hi | ((uint64_t)lo << 32u);
    return 1;
  }
  case AST_ENCODER_FIXUP_RISCV_CALL:
  case AST_ENCODER_FIXUP_RISCV_PCREL_ADDRESS: {
    if (f->width != 8u)
      goto invalid;
    uint32_t hi = read_word(out->bytes + f->offset),
             lo = read_word(out->bytes + f->offset + 4u);
    unsigned rd = (hi >> 7u) & 31u;
    uint32_t opcode = f->kind == AST_ENCODER_FIXUP_RISCV_CALL ? 0x67u : 0x13u;
    if ((pc & 3u) || (hi & 127u) != 0x17u || !rd || (lo & 0x707fu) != opcode ||
        ((lo >> 15u) & 31u) != rd || ((lo >> 7u) & 31u) != rd ||
        (f->kind == AST_ENCODER_FIXUP_RISCV_CALL && rd != 1u))
      goto invalid;
    if (!relative(target, pc, &delta) || delta < INT32_MIN ||
        delta > INT32_MAX ||
        (f->kind == AST_ENCODER_FIXUP_RISCV_CALL && (target & 1u)))
      goto range;
    int64_t low = (int64_t)((uint64_t)delta & 4095u);
    if (low >= 2048)
      low -= 4096;
    int64_t high = (delta - low) / 4096;
    if (high < -524288 || high > 524287)
      goto range;
    hi = (hi & 4095u) | (((uint32_t)high & 0xfffffu) << 12u);
    lo = (lo & 0x000fffffu) | (((uint32_t)low & 4095u) << 20u);
    patch->bits = (uint64_t)hi | ((uint64_t)lo << 32u);
    return 1;
  }
  default:
    break;
  }
invalid:
  return relocation_error(
      out, AST_ENCODER_DIAG_INVALID_FIXUP,
      "Relocation kind does not match the instruction field.");
range:
  return relocation_error(
      out, AST_ENCODER_DIAG_RELOCATION_RANGE,
      "Resolved symbol/addend is outside the relocation range.");
}
static int patch_order(const void *a, const void *b) {
  const PendingPatch *x = (const PendingPatch *)a, *y = (const PendingPatch *)b;
  return x->offset < y->offset ? -1 : x->offset > y->offset;
}
static int apply_fixups_impl(AstEncoderOutput *out, uint64_t address,
                             AstEncoderResolveSymbol resolve,
                             AstEncoderResolveQualifiedSymbol qualified_resolve,
                             void *user) {
  if (!out)
    return 0;
  if ((out->size && !out->bytes) ||
      (out->fixup_count &&
       (!out->fixups || (!resolve && !qualified_resolve))) ||
      out->fixup_count > SIZE_MAX / sizeof(PendingPatch) ||
      out->size > UINT64_MAX - address)
    return relocation_error(out, AST_ENCODER_DIAG_INVALID_ARGUMENT,
                            "Invalid fixup buffer, base address or resolver.");
  if (!out->size && out->diagnostic != AST_ENCODER_DIAG_NONE)
    return relocation_error(out, AST_ENCODER_DIAG_INVALID_ARGUMENT,
                            "There is no encoded output to relocate.");
  PendingPatch *patches =
      out->fixup_count
          ? (PendingPatch *)malloc(out->fixup_count * sizeof(*patches))
          : NULL;
  if (out->fixup_count && !patches)
    return relocation_error(out, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                            "Cannot allocate transactional fixup storage.");
  for (size_t n = 0; n < out->fixup_count; ++n) {
    const AstEncoderFixup *f = &out->fixups[n];
    uint64_t symbol = 0u;
    int resolved = 0;
    if (f->symbol.text && f->symbol.span.length) {
      resolved = qualified_resolve
                     ? qualified_resolve(user, f->module, f->symbol, &symbol)
                     : resolve(user, f->symbol, &symbol);
    }
    if (!resolved) {
      free(patches);
      return relocation_error(out, AST_ENCODER_DIAG_UNRESOLVED_SYMBOL,
                              "Relocation symbol was not resolved.");
    }
    if (!patch_record(out, f, address, symbol, &patches[n])) {
      free(patches);
      return 0;
    }
  }
  if (out->fixup_count > 1u)
    qsort(patches, out->fixup_count, sizeof(*patches), patch_order);
  for (size_t n = 1; n < out->fixup_count; ++n)
    if (patches[n - 1u].offset + patches[n - 1u].width > patches[n].offset) {
      free(patches);
      return relocation_error(out, AST_ENCODER_DIAG_INVALID_FIXUP,
                              "Relocation fields overlap.");
    }
  for (size_t n = 0; n < out->fixup_count; ++n)
    for (unsigned byte = 0; byte < patches[n].width; ++byte)
      out->bytes[patches[n].offset + byte] =
          (uint8_t)(patches[n].bits >> (byte * 8u));
  free(patches);
  free(out->fixups);
  out->fixups = NULL;
  out->fixup_count = 0u;
  out->diagnostic = AST_ENCODER_DIAG_NONE;
  out->message = NULL;
  out->instruction = AST_MACHINE_INVALID_ID;
  return 1;
}

int ast_encoder_apply_fixups(AstEncoderOutput *out, uint64_t address,
                             AstEncoderResolveSymbol resolve, void *user) {
  return apply_fixups_impl(out, address, resolve, NULL, user);
}

int ast_encoder_apply_fixups_qualified(AstEncoderOutput *out, uint64_t address,
                                       AstEncoderResolveQualifiedSymbol resolve,
                                       void *user) {
  return apply_fixups_impl(out, address, NULL, resolve, user);
}
