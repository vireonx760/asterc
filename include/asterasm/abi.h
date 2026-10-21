#ifndef ASTERASM_ABI_H
#define ASTERASM_ABI_H

#include "asterasm/target.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstAbiProgram AstAbiProgram;

#define AST_ABI_MAX_REGISTERS 8u
#define AST_ABI_REGISTER_NAME_CAP 12u

typedef enum AstAbiKind {
  AST_ABI_INVALID = 0,
  AST_ABI_SYSV_AMD64,
  AST_ABI_WINDOWS_X64,
  AST_ABI_AAPCS64,
  /* AAPCS64 with the scalable-vector/predicate register contract.  This is
     explicit because the base ABI remains deliberately fail-closed for
     scalable values. */
  AST_ABI_AAPCS64_SVE,
  AST_ABI_RISCV_LP64,
  AST_ABI_RISCV_LP64F,
  AST_ABI_RISCV_LP64D,
  /* RISC-V vector calling-convention variants.  The suffix mirrors the
     scalar ABI used for non-vector arguments. */
  AST_ABI_RISCV_LP64V,
  AST_ABI_RISCV_LP64FV,
  AST_ABI_RISCV_LP64DV,
  AST_ABI_I386_CDECL
} AstAbiKind;

typedef enum AstAbiLocationKind {
  AST_ABI_LOCATION_NONE = 0,
  AST_ABI_LOCATION_REGISTER,
  AST_ABI_LOCATION_REGISTER_LIST,
  AST_ABI_LOCATION_STACK,
  AST_ABI_LOCATION_INDIRECT,
  AST_ABI_LOCATION_SPLIT_REG_STACK
} AstAbiLocationKind;

typedef enum AstAbiExtension {
  AST_ABI_EXT_NONE = 0,
  AST_ABI_EXT_SIGN,
  AST_ABI_EXT_ZERO,
  AST_ABI_EXT_SIGN_TO_XLEN_AFTER_32,
  AST_ABI_EXT_NAN_BOX,
  AST_ABI_EXT_COERCE_INTEGER,
  AST_ABI_EXT_BY_REFERENCE
} AstAbiExtension;

typedef enum AstAbiValueFlags {
  AST_ABI_VALUE_NONE = 0,
  AST_ABI_VALUE_HIDDEN = 1u << 0,
  AST_ABI_VALUE_RESULT = 1u << 1,
  AST_ABI_VALUE_INDIRECT_COPY = 1u << 2,
  AST_ABI_VALUE_HFA = 1u << 3,
  AST_ABI_VALUE_HVA = 1u << 4,
  AST_ABI_VALUE_SPLIT = 1u << 5,
  AST_ABI_VALUE_NAN_BOXED = 1u << 6,
  AST_ABI_VALUE_RETURN_POINTER_IN_GPR = 1u << 7
} AstAbiValueFlags;

typedef enum AstAbiDiagCode {
  AST_ABI_DIAG_NONE = 7000,
  AST_ABI_DIAG_TARGET_REQUIRED,
  AST_ABI_DIAG_TARGET_INVALID,
  AST_ABI_DIAG_ABI_TARGET_MISMATCH,
  AST_ABI_DIAG_UNSUPPORTED_SIGNATURE,
  AST_ABI_DIAG_UNSUPPORTED_SCALABLE_PCS,
  AST_ABI_DIAG_REQUIRED_FEATURE,
  AST_ABI_DIAG_INVALID_CALL_SIGNATURE,
  AST_ABI_DIAG_INTERNAL,
  AST_ABI_DIAG_TOO_MANY_DIAGNOSTICS
} AstAbiDiagCode;

typedef struct AstAbiConfig {
  AstAbiKind kind;
  uint32_t max_diagnostics;
  uint8_t verify_target_before_lowering;
  uint8_t fail_closed_scalable_pcs;
} AstAbiConfig;

typedef struct AstAbiLocation {
  AstAbiLocationKind kind;
  uint32_t register_count;
  char registers[AST_ABI_MAX_REGISTERS][AST_ABI_REGISTER_NAME_CAP];
  uint64_t stack_offset;
  uint64_t stack_size;
  uint32_t stack_alignment;
} AstAbiLocation;

typedef struct AstAbiValueInfo {
  AstMirVReg source_vreg;
  uint32_t source_index;
  const AstSemaType *type;
  AstAbiLocation location;
  uint64_t payload_size;
  uint32_t alignment;
  AstAbiExtension extension;
  uint32_t flags;
} AstAbiValueInfo;

typedef struct AstAbiRegisterMask {
  uint64_t gpr;
  uint64_t fp_vector;
  uint64_t predicate;
  /* For AAPCS64, v8-v15 are only required to preserve their low 64 bits. */
  uint64_t fp_vector_low64_only;
} AstAbiRegisterMask;

typedef struct AstAbiFunctionInfo {
  AstName module_name;
  AstName name;
  uint32_t function_index;
  uint32_t argument_count;
  uint32_t hidden_argument_count;
  AstAbiValueInfo result;
  uint64_t outgoing_stack_bytes;
  uint32_t stack_alignment;
  uint32_t shadow_space_bytes;
  uint32_t red_zone_bytes;
  AstAbiRegisterMask caller_saved;
  AstAbiRegisterMask callee_saved;
} AstAbiFunctionInfo;

typedef struct AstAbiCallInfo {
  uint32_t caller_function_index;
  AstMirInstId instruction;
  uint8_t is_direct;
  uint32_t argument_count;
  AstAbiValueInfo result;
  uint64_t outgoing_stack_bytes;
  uint32_t shadow_space_bytes;
  uint32_t stack_alignment; /* Required SP alignment immediately before call. */
} AstAbiCallInfo;

typedef struct AstAbiDiagnostic {
  AstAbiDiagCode code;
  AstDiagSeverity severity;
  AstSpan span;
  uint32_t function_index;
  AstMirInstId instruction;
  const char *message;
} AstAbiDiagnostic;

typedef struct AstAbiStats {
  size_t function_count;
  size_t argument_count;
  size_t call_site_count;
  size_t call_argument_count;
  size_t indirect_argument_count;
  size_t stack_argument_count;
  size_t split_argument_count;
  size_t hidden_sret_count;
  size_t error_count;
  size_t warning_count;
} AstAbiStats;

ASTER_API AstAbiConfig ast_abi_config_default(AstAbiKind kind);
ASTER_API const char *ast_abi_kind_name(AstAbiKind kind);
ASTER_API const char *ast_abi_location_kind_name(AstAbiLocationKind kind);
ASTER_API const char *ast_abi_diag_code_name(AstAbiDiagCode code);
ASTER_API AstAbiKind ast_abi_default_for_target(const AstTargetConfig *target);

ASTER_API AstAbiProgram *ast_abi_lower(const AstTargetProgram *target,
                                       const AstAbiConfig *config);
ASTER_API void ast_abi_destroy(AstAbiProgram *program);
ASTER_API int ast_abi_verify(const AstAbiProgram *program);
ASTER_API const AstTargetProgram *
ast_abi_source_target(const AstAbiProgram *program);
ASTER_API AstAbiConfig ast_abi_config(const AstAbiProgram *program);
ASTER_API AstAbiStats ast_abi_stats(const AstAbiProgram *program);
ASTER_API const AstAbiDiagnostic *
ast_abi_diagnostics(const AstAbiProgram *program, size_t *count_out);

ASTER_API size_t ast_abi_function_count(const AstAbiProgram *program);
ASTER_API int ast_abi_function_info(const AstAbiProgram *program,
                                    size_t function_index,
                                    AstAbiFunctionInfo *out);
/* Index includes hidden ABI arguments first, then source-language arguments. */
ASTER_API int ast_abi_function_argument(const AstAbiProgram *program,
                                        size_t function_index, uint32_t index,
                                        AstAbiValueInfo *out);

ASTER_API size_t ast_abi_call_count(const AstAbiProgram *program);
ASTER_API int ast_abi_call_info(const AstAbiProgram *program, size_t call_index,
                                AstAbiCallInfo *out);
/* Index includes hidden ABI arguments first, then the actual MIR call operands.
 */
ASTER_API int ast_abi_call_argument(const AstAbiProgram *program,
                                    size_t call_index, uint32_t index,
                                    AstAbiValueInfo *out);
ASTER_API void ast_abi_dump_program(
    const AstAbiProgram *program,
    void (*write_fn)(void *user, const char *text, size_t length), void *user);

#ifdef __cplusplus
}
#endif

#endif
