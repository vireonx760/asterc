#ifndef ASTERASM_TARGET_INTERNAL_H
#define ASTERASM_TARGET_INTERNAL_H

#include "asterasm/target.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TargetFunction {
  AstTargetFunctionInfo info;
  AstTargetValueInfo *values;
  AstTargetInstructionInfo *instructions;
} TargetFunction;

struct AstTargetProgram {
  const AstMirProgram *mir;
  AstTargetConfig config;
  TargetFunction *functions;
  size_t function_count;
  AstTargetDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  char **owned_messages;
  size_t owned_message_count;
  size_t owned_message_capacity;
  AstTargetStats stats;
};

typedef struct TargetTypeClass {
  AstLegalizeAction action;
  AstTargetRegClass reg_class;
  uint32_t legal_width;
  uint32_t part_count;
  uint64_t required_features;
} TargetTypeClass;

TargetTypeClass target_classify_type(const AstTargetConfig *config,
                                     const AstSemaType *type);
AstLegalizeAction target_classify_instruction(const AstTargetConfig *config,
                                              const AstMirFunction *function,
                                              const AstMirInstructionInfo *inst,
                                              AstTargetRegClass *result_class,
                                              uint64_t *required_features);
int target_validate_native(AstTargetProgram *program, size_t function_index,
                           const AstMirFunction *function,
                           const AstMirInstructionInfo *inst,
                           AstTargetInstructionInfo *out);
uint64_t target_native_instruction_features(AstTargetArch arch,
                                            AstName mnemonic);
void target_diag(AstTargetProgram *program, AstTargetDiagCode code,
                 AstDiagSeverity severity, AstSpan span,
                 uint32_t function_index, AstMirInstId instruction,
                 const char *fmt, ...) ASTER_PRINTF_LIKE(7, 8);
int target_name_eq(AstName name, const char *text);
int target_ascii_ieq(const char *a, const char *b);

#endif
