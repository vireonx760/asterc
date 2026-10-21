#ifndef ASTERASM_ABI_INTERNAL_H
#define ASTERASM_ABI_INTERNAL_H

#include "asterasm/abi.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AbiFunction {
  AstAbiFunctionInfo info;
  AstAbiValueInfo *arguments;
  uint32_t total_argument_count;
} AbiFunction;

typedef struct AbiCall {
  AstAbiCallInfo info;
  AstAbiValueInfo *arguments;
  uint32_t total_argument_count;
} AbiCall;

struct AstAbiProgram {
  const AstTargetProgram *target;
  const AstMirProgram *mir;
  AstAbiConfig config;
  AbiFunction *functions;
  size_t function_count;
  AbiCall *calls;
  size_t call_count;
  size_t call_capacity;
  AstAbiDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  char **owned_messages;
  size_t owned_message_count;
  size_t owned_message_capacity;
  AstAbiStats stats;
};

typedef struct AbiSignatureResult {
  AstAbiValueInfo *arguments;
  uint32_t argument_count;
  uint32_t hidden_count;
  AstAbiValueInfo result;
  uint64_t stack_bytes;
  uint32_t shadow_space;
  uint32_t stack_alignment;
  uint32_t red_zone;
} AbiSignatureResult;

void abi_diag(AstAbiProgram *p, AstAbiDiagCode code, AstDiagSeverity severity,
              AstSpan span, uint32_t function_index, AstMirInstId inst,
              const char *fmt, ...) ASTER_PRINTF_LIKE(7, 8);
uint64_t abi_align_up(uint64_t value, uint64_t alignment);
uint64_t abi_type_size(const AstSemaType *type);
uint32_t abi_type_align(const AstSemaType *type);
int abi_is_aggregate(const AstSemaType *type);
int abi_is_integer(const AstSemaType *type);
int abi_is_float(const AstSemaType *type);
int abi_is_vector(const AstSemaType *type);
int abi_is_scalable(const AstSemaType *type);
void abi_set_reg(AstAbiLocation *loc, uint32_t slot, const char *name);
void abi_init_value(AstAbiValueInfo *v, const AstSemaType *type,
                    AstMirVReg source_vreg, uint32_t source_index);
int abi_lower_signature(AstAbiProgram *p, const AstSemaType *signature,
                        const AstMirVReg *source_vregs,
                        uint32_t source_vreg_count, uint32_t function_index,
                        AstMirInstId call_inst, AbiSignatureResult *out);
int abi_lower_i386(AstAbiProgram *p, const AstSemaType *signature,
                   const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                   uint32_t function_index, AstMirInstId call_inst,
                   AbiSignatureResult *out);
int abi_lower_sysv(AstAbiProgram *p, const AstSemaType *signature,
                   const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                   uint32_t function_index, AstMirInstId call_inst,
                   AbiSignatureResult *out);
int abi_lower_win64(AstAbiProgram *p, const AstSemaType *signature,
                    const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                    uint32_t function_index, AstMirInstId call_inst,
                    AbiSignatureResult *out);
int abi_lower_aapcs64(AstAbiProgram *p, const AstSemaType *signature,
                      const AstMirVReg *source_vregs,
                      uint32_t source_vreg_count, uint32_t function_index,
                      AstMirInstId call_inst, AbiSignatureResult *out);
int abi_lower_riscv(AstAbiProgram *p, const AstSemaType *signature,
                    const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                    uint32_t function_index, AstMirInstId call_inst,
                    AbiSignatureResult *out);
void abi_fill_masks(AstAbiKind kind, AstAbiRegisterMask *caller,
                    AstAbiRegisterMask *callee);

#endif
