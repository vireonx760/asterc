#ifndef ASTERASM_AARCH64_SCALABLE_H
#define ASTERASM_AARCH64_SCALABLE_H
#include "asterasm/regalloc.h"

/* Element width lives in the form; Z/P/ZA storage has runtime width. */
typedef enum A64ScalableKind {
  A64_SC_Z,
  A64_SC_P,
  A64_SC_G,
  A64_SC_SP,
  A64_SC_F,
  A64_SC_ZA,
  A64_SC_ZAH,
  A64_SC_ZAV,
  A64_SC_I,
  A64_SC_ROT,
  A64_SC_LOGICAL,
  A64_SC_CPY8,
  A64_SC_IMM8,
  A64_SC_FP_PAIR,
  A64_SC_FP8,
  A64_SC_INC,
  A64_SC_MODE
} A64ScalableKind;
typedef struct A64ScalableType {
  const char *name;
  A64ScalableKind kind;
  uint16_t width, lane, first, last, group, extend, shift;
  int32_t low, high, scale, transform;
} A64ScalableType;
typedef struct A64ScalableParameter {
  const char *role;
  uint16_t type;
  int16_t tied, fallback;
} A64ScalableParameter;
typedef struct A64ScalableField {
  uint8_t parameter, source, destination, count;
} A64ScalableField;
typedef struct A64ScalableForm {
  const char *name, *assembly;
  uint64_t required_all, required_any;
  uint32_t encoding, mask, parameter_offset, field_offset, field_count;
  uint16_t parameter_count, results, bits, input_bits, flags, extend, shift,
      prefix_bits;
} A64ScalableForm;
enum {
  A64_SC_LOAD = 1u,
  A64_SC_STORE = 2u,
  A64_SC_ZA_ACCESS = 4u,
  A64_SC_STREAMING = 8u,
  A64_SC_START = 16u,
  A64_SC_STOP = 32u,
  A64_SC_PREFIX = 64u,
  A64_SC_DESTRUCTIVE = 128u,
  A64_SC_NZCV = 256u,
  A64_SC_NONSTREAMING = 512u,
  A64_SC_PREFIX_COMPAT = 1024u,
  A64_SC_MERGING = 2048u
};
extern const A64ScalableType a64_scalable_types[];
extern const A64ScalableParameter a64_scalable_parameters[];
extern const A64ScalableField a64_scalable_fields[];
extern const A64ScalableForm a64_scalable_forms[];
extern const uint32_t a64_scalable_form_count, a64_scalable_type_count;

uint32_t a64_scalable_find(AstName mnemonic);
int a64_scalable_statement(AstTargetArch arch, const AstHirNativeStmtInfo *s);
const A64ScalableForm *a64_scalable_form(const AstPhysicalInstructionInfo *i);
int a64_scalable_features(const AstTargetConfig *target,
                          const A64ScalableForm *f);
int a64_scalable_register(const A64ScalableType *t, uint32_t index,
                          AstPhysicalOperand *out);
int a64_scalable_operand(const A64ScalableType *t, const AstPhysicalOperand *o,
                         uint64_t *encoded);
int a64_scalable_decode_operand(const A64ScalableType *t, uint64_t encoded,
                                AstPhysicalOperand *o);
int a64_scalable_contract(const AstPhysicalInstructionInfo *i);
uint32_t a64_scalable_word(const A64ScalableForm *f,
                           const AstPhysicalInstructionInfo *i);
int a64_scalable_decode(uint32_t word, AstPhysicalInstructionInfo *out);
#endif
