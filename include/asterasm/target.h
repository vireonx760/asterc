#ifndef ASTERASM_TARGET_H
#define ASTERASM_TARGET_H

#include "asterasm/mir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstTargetProgram AstTargetProgram;

typedef enum AstTargetArch {
  AST_TARGET_ARCH_INVALID = 0,
  AST_TARGET_ARCH_X86_64,
  AST_TARGET_ARCH_AARCH64,
  AST_TARGET_ARCH_RISCV64,
  AST_TARGET_ARCH_X86_32,
  AST_TARGET_ARCH_X86_16
} AstTargetArch;

typedef enum AstTargetOS {
  AST_TARGET_OS_UNKNOWN = 0,
  AST_TARGET_OS_LINUX,
  AST_TARGET_OS_WINDOWS,
  AST_TARGET_OS_MACOS,
  AST_TARGET_OS_BARE_METAL
} AstTargetOS;

typedef uint64_t AstTargetFeature;

#define AST_TARGET_FEATURE_NONE UINT64_C(0)
#define AST_TARGET_FEATURE_X86_SSE2 (UINT64_C(1) << 0)
#define AST_TARGET_FEATURE_X86_SSE41 (UINT64_C(1) << 1)
#define AST_TARGET_FEATURE_X86_AVX (UINT64_C(1) << 2)
#define AST_TARGET_FEATURE_X86_AVX2 (UINT64_C(1) << 3)
#define AST_TARGET_FEATURE_X86_AVX512F (UINT64_C(1) << 4)
#define AST_TARGET_FEATURE_X86_AVX512BW (UINT64_C(1) << 5)
#define AST_TARGET_FEATURE_X86_AVX512DQ (UINT64_C(1) << 6)
#define AST_TARGET_FEATURE_X86_F16C (UINT64_C(1) << 7)
#define AST_TARGET_FEATURE_X86_POPCNT (UINT64_C(1) << 8)
#define AST_TARGET_FEATURE_X86_CMPXCHG (UINT64_C(1) << 9)
#define AST_TARGET_FEATURE_X86_CMPXCHG8B (UINT64_C(1) << 10)
#define AST_TARGET_FEATURE_X86_XSAVE (UINT64_C(1) << 11)
#define AST_TARGET_FEATURE_X86_SMAP (UINT64_C(1) << 12)
#define AST_TARGET_FEATURE_X86_RDTSCP (UINT64_C(1) << 13)
#define AST_TARGET_FEATURE_X86_AVX512VL (UINT64_C(1) << 14)
#define AST_TARGET_FEATURE_A64_FP16 (UINT64_C(1) << 16)
#define AST_TARGET_FEATURE_A64_BF16 (UINT64_C(1) << 17)
#define AST_TARGET_FEATURE_A64_SVE (UINT64_C(1) << 18)
#define AST_TARGET_FEATURE_A64_SVE2 (UINT64_C(1) << 19)
#define AST_TARGET_FEATURE_A64_SME (UINT64_C(1) << 20)
#define AST_TARGET_FEATURE_A64_SME_I16I64 (UINT64_C(1) << 21)
#define AST_TARGET_FEATURE_A64_SME_F64F64 (UINT64_C(1) << 22)
#define AST_TARGET_FEATURE_A64_SME_FA64 (UINT64_C(1) << 23)
#define AST_TARGET_FEATURE_A64_SVE2_AES (UINT64_C(1) << 24)
#define AST_TARGET_FEATURE_A64_SVE2_SHA3 (UINT64_C(1) << 25)
#define AST_TARGET_FEATURE_A64_SVE2_SM4 (UINT64_C(1) << 26)
#define AST_TARGET_FEATURE_A64_SVE2_BITPERM (UINT64_C(1) << 27)
#define AST_TARGET_FEATURE_A64_I8MM (UINT64_C(1) << 28)
#define AST_TARGET_FEATURE_A64_F32MM (UINT64_C(1) << 29)
#define AST_TARGET_FEATURE_A64_F64MM (UINT64_C(1) << 30)
#define AST_TARGET_FEATURE_RV_M (UINT64_C(1) << 32)
#define AST_TARGET_FEATURE_RV_A (UINT64_C(1) << 33)
#define AST_TARGET_FEATURE_RV_F (UINT64_C(1) << 34)
#define AST_TARGET_FEATURE_RV_D (UINT64_C(1) << 35)
#define AST_TARGET_FEATURE_RV_ZBB (UINT64_C(1) << 36)
#define AST_TARGET_FEATURE_RV_ZFH (UINT64_C(1) << 37)
#define AST_TARGET_FEATURE_RV_V (UINT64_C(1) << 38)
#define AST_TARGET_FEATURE_RV_ZICSR (UINT64_C(1) << 39)
#define AST_TARGET_FEATURE_RV_ZIFENCEI (UINT64_C(1) << 40)
#define AST_TARGET_FEATURE_RV_H (UINT64_C(1) << 41)
#define AST_TARGET_FEATURE_RV_SVINVAL (UINT64_C(1) << 42)
#define AST_TARGET_FEATURE_RV_ZBA (UINT64_C(1) << 43)
#define AST_TARGET_FEATURE_RV_ZBC (UINT64_C(1) << 44)
#define AST_TARGET_FEATURE_RV_ZBS (UINT64_C(1) << 45)

typedef enum AstLegalizeAction {
  AST_LEGALIZE_INVALID = 0,
  AST_LEGALIZE_LEGAL,
  AST_LEGALIZE_PROMOTE,
  AST_LEGALIZE_SPLIT,
  AST_LEGALIZE_CUSTOM,
  AST_LEGALIZE_INDIRECT,
  AST_LEGALIZE_UNSUPPORTED
} AstLegalizeAction;

typedef enum AstTargetRegClass {
  AST_TARGET_REGCLASS_INVALID = 0,
  AST_TARGET_REGCLASS_GPR8,
  AST_TARGET_REGCLASS_GPR16,
  AST_TARGET_REGCLASS_GPR32,
  AST_TARGET_REGCLASS_GPR64,
  AST_TARGET_REGCLASS_GPR_PAIR,
  AST_TARGET_REGCLASS_FP16,
  AST_TARGET_REGCLASS_FP32,
  AST_TARGET_REGCLASS_FP64,
  AST_TARGET_REGCLASS_FP128,
  AST_TARGET_REGCLASS_VEC64,
  AST_TARGET_REGCLASS_VEC128,
  AST_TARGET_REGCLASS_VEC256,
  AST_TARGET_REGCLASS_VEC512,
  AST_TARGET_REGCLASS_MASK,
  AST_TARGET_REGCLASS_SCALABLE_VECTOR,
  AST_TARGET_REGCLASS_PREDICATE,
  AST_TARGET_REGCLASS_MEMORY,
  AST_TARGET_REGCLASS_SPECIAL
} AstTargetRegClass;

typedef enum AstTargetDiagCode {
  AST_TARGET_DIAG_NONE = 6000,
  AST_TARGET_DIAG_MIR_REQUIRED,
  AST_TARGET_DIAG_MIR_INVALID,
  AST_TARGET_DIAG_INVALID_CONFIG,
  AST_TARGET_DIAG_UNSUPPORTED_TYPE,
  AST_TARGET_DIAG_UNSUPPORTED_OPERATION,
  AST_TARGET_DIAG_REQUIRED_FEATURE,
  AST_TARGET_DIAG_NATIVE_ARCH_MISMATCH,
  AST_TARGET_DIAG_INVALID_NATIVE_REGISTER,
  AST_TARGET_DIAG_INVALID_NATIVE_FORM,
  AST_TARGET_DIAG_INTERNAL,
  AST_TARGET_DIAG_TOO_MANY_DIAGNOSTICS
} AstTargetDiagCode;

typedef struct AstTargetConfig {
  AstTargetArch arch;
  AstTargetOS os;
  uint64_t features;
  uint32_t pointer_bits;
  uint32_t max_diagnostics;
  uint8_t verify_mir_before_build;
  uint8_t fail_on_unsupported;
  uint8_t exact_native_fail_closed;
  uint8_t optimize_size;
} AstTargetConfig;

typedef struct AstTargetDiagnostic {
  AstTargetDiagCode code;
  AstDiagSeverity severity;
  AstSpan span;
  uint32_t function_index;
  AstMirInstId instruction;
  const char *message;
} AstTargetDiagnostic;

typedef struct AstTargetValueInfo {
  AstMirVReg vreg;
  const AstSemaType *type;
  AstLegalizeAction action;
  AstTargetRegClass reg_class;
  uint32_t legal_bit_width;
  uint32_t part_count;
  uint64_t required_features;
} AstTargetValueInfo;

typedef struct AstTargetInstructionInfo {
  AstMirInstId id;
  AstMirOp op;
  AstLegalizeAction action;
  AstTargetRegClass result_class;
  uint64_t required_features;
  uint32_t narrowed_effects;
  uint8_t native_effects_refined;
} AstTargetInstructionInfo;

typedef struct AstTargetFunctionInfo {
  AstName module_name;
  AstName name;
  uint32_t function_index;
  uint32_t vreg_count;
  uint32_t instruction_count;
  uint32_t legal_count;
  uint32_t custom_count;
  uint32_t split_count;
  uint32_t unsupported_count;
} AstTargetFunctionInfo;

typedef struct AstTargetStats {
  size_t function_count;
  size_t vreg_count;
  size_t instruction_count;
  size_t legal_count;
  size_t promote_count;
  size_t split_count;
  size_t custom_count;
  size_t indirect_count;
  size_t unsupported_count;
  size_t error_count;
  size_t warning_count;
} AstTargetStats;

typedef struct AstTargetRegisterInfo {
  AstTargetRegClass reg_class;
  uint32_t width;
  uint32_t index;
  uint8_t valid;
  uint8_t high8;
  uint8_t extended;
  uint8_t special;
} AstTargetRegisterInfo;

ASTER_API AstTargetConfig ast_target_config_default(AstTargetArch arch);
ASTER_API int ast_target_config_from_triple(const char *triple,
                                            AstTargetConfig *out);
ASTER_API uint64_t ast_target_feature_closure(AstTargetArch arch,
                                              uint64_t features);
ASTER_API const char *ast_target_arch_name(AstTargetArch arch);
ASTER_API const char *ast_target_legalize_action_name(AstLegalizeAction action);
ASTER_API const char *ast_target_reg_class_name(AstTargetRegClass reg_class);
ASTER_API const char *ast_target_diag_code_name(AstTargetDiagCode code);
ASTER_API uint64_t ast_target_feature_from_name(AstTargetArch arch,
                                                const char *name);

ASTER_API AstTargetProgram *ast_target_legalize(const AstMirProgram *mir,
                                                const AstTargetConfig *config);
ASTER_API void ast_target_destroy(AstTargetProgram *program);
ASTER_API int ast_target_verify(const AstTargetProgram *program);
ASTER_API const AstMirProgram *
ast_target_source_mir(const AstTargetProgram *program);
ASTER_API AstTargetConfig ast_target_config(const AstTargetProgram *program);
ASTER_API AstTargetStats ast_target_stats(const AstTargetProgram *program);
ASTER_API const AstTargetDiagnostic *
ast_target_diagnostics(const AstTargetProgram *program, size_t *count_out);
ASTER_API size_t ast_target_function_count(const AstTargetProgram *program);
ASTER_API int ast_target_function_info(const AstTargetProgram *program,
                                       size_t function_index,
                                       AstTargetFunctionInfo *out);
ASTER_API int ast_target_value_info(const AstTargetProgram *program,
                                    size_t function_index, AstMirVReg vreg,
                                    AstTargetValueInfo *out);
ASTER_API int ast_target_instruction_info(const AstTargetProgram *program,
                                          size_t function_index,
                                          AstMirInstId instruction,
                                          AstTargetInstructionInfo *out);
ASTER_API void ast_target_dump_program(
    const AstTargetProgram *program,
    void (*write_fn)(void *user, const char *text, size_t length), void *user);

/* Target-register validation used by native/exact lowering and later ISel. */
ASTER_API int ast_target_parse_register(const AstTargetConfig *config,
                                        const char *name,
                                        AstTargetRegisterInfo *out);
/* x86-specific form guard: high 8-bit registers cannot coexist with a required
 * REX prefix. */
ASTER_API int ast_target_x86_registers_compatible(const char *const *registers,
                                                  size_t count,
                                                  int rex_required);
/* Validate immediate against an inclusive signed or unsigned encoding range. */
ASTER_API int ast_target_immediate_fits(int64_t value, uint32_t bits,
                                        int is_signed);

#ifdef __cplusplus
}
#endif

#endif
