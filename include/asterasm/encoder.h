#ifndef ASTERASM_ENCODER_H
#define ASTERASM_ENCODER_H

#include "asterasm/regalloc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AstEncoderDiagCode {
  AST_ENCODER_DIAG_NONE = 10000,
  AST_ENCODER_DIAG_INVALID_ARGUMENT,
  AST_ENCODER_DIAG_INVALID_PHYSICAL,
  AST_ENCODER_DIAG_UNSUPPORTED_INSTRUCTION,
  AST_ENCODER_DIAG_UNSUPPORTED_OPERAND,
  AST_ENCODER_DIAG_BRANCH_RANGE,
  AST_ENCODER_DIAG_OUT_OF_MEMORY,
  AST_ENCODER_DIAG_UNRESOLVED_SYMBOL,
  AST_ENCODER_DIAG_RELOCATION_RANGE,
  AST_ENCODER_DIAG_INVALID_FIXUP
} AstEncoderDiagCode;

typedef enum AstEncoderFixupKind {
  AST_ENCODER_FIXUP_PC_REL32 = 0,
  AST_ENCODER_FIXUP_ABS64,
  AST_ENCODER_FIXUP_A64_CALL26,
  AST_ENCODER_FIXUP_A64_ADR21,
  AST_ENCODER_FIXUP_RISCV_CALL,
  AST_ENCODER_FIXUP_A64_ADRP_ADD,
  AST_ENCODER_FIXUP_RISCV_PCREL_ADDRESS,
  AST_ENCODER_FIXUP_X86_CALL_REL32,
  AST_ENCODER_FIXUP_ABS32
} AstEncoderFixupKind;

typedef struct AstEncoderFixup {
  size_t offset;
  uint8_t width;
  AstEncoderFixupKind kind;
  AstName symbol;
  AstName module;
  int64_t addend;
} AstEncoderFixup;

/* The output owns bytes and fixups until ast_encoder_output_destroy(). Symbol
   names are borrowed from the source Physical IR. PC-relative fixups are
   relative to the byte immediately following the encoded field. A64 fixups are
   relative to the instruction address; A64_ADRP_ADD patches an ADRP/ADD pair,
   RISCV_CALL an AUIPC/JALR pair, and RISCV_PCREL_ADDRESS an AUIPC/ADDI pair. */
typedef struct AstEncoderOutput {
  uint8_t *bytes;
  size_t size;
  AstEncoderFixup *fixups;
  size_t fixup_count;
  AstEncoderDiagCode diagnostic;
  uint32_t instruction;
  const char *message;
} AstEncoderOutput;

typedef struct AstEncoderModuleOutput {
  AstEncoderOutput *functions;
  size_t function_count;
  size_t failed_function; /* SIZE_MAX when no individual function failed. */
  AstEncoderDiagCode diagnostic;
  const char *message;
} AstEncoderModuleOutput;

/* Batch API verifies Physical IR once for the module, then encodes every
   function. Avoids repeated whole-program verification for large modules.
   Failure is transactional: no partial function array escapes. */
ASTER_API int ast_encoder_module(const AstRegAllocProgram *program,
                                 AstEncoderModuleOutput *output);
/* 386+ semantic backend: 32-bit pointers/cdecl, 16- or 32-bit instruction mode.
   Unsupported operations fail transactionally. MIR must have been verified. */
ASTER_API int ast_encoder_x86_legacy_module(const AstMirProgram *mir,
                                            const AstTargetConfig *target,
                                            AstEncoderModuleOutput *output);
ASTER_API void
ast_encoder_module_output_destroy(AstEncoderModuleOutput *output);

ASTER_API void ast_encoder_output_destroy(AstEncoderOutput *output);

/* Resolve symbol names to final virtual addresses. Return nonzero on success.
   The callback must not mutate output or its borrowed symbol names. */
typedef int (*AstEncoderResolveSymbol)(void *user, AstName symbol,
                                       uint64_t *address);
typedef int (*AstEncoderResolveQualifiedSymbol)(void *user, AstName module,
                                                AstName symbol,
                                                uint64_t *address);
/* Apply relocations for a flat image/JIT buffer at the supplied virtual
   address. Failure leaves bytes and fixups unchanged and sets the diagnostic. A
   failed resolution can be retried. Success consumes the fixup array.

   The legacy resolver intentionally receives only the source symbol name for
   API compatibility. The qualified variant additionally receives the defining
   module and is used by the compiler's whole-program raw/object linker. */
ASTER_API int ast_encoder_apply_fixups(AstEncoderOutput *output,
                                       uint64_t address,
                                       AstEncoderResolveSymbol resolve,
                                       void *user);
ASTER_API int
ast_encoder_apply_fixups_qualified(AstEncoderOutput *output, uint64_t address,
                                   AstEncoderResolveQualifiedSymbol resolve,
                                   void *user);

/* Output must be fresh/uninitialized or previously destroyed. On failure no
   partial bytes/fixups escape. Encoders reject unsupported instruction forms.
   Object-file policy belongs to the separate object-writer component. */
ASTER_API int ast_encoder_function(const AstRegAllocProgram *program,
                                   size_t function, AstEncoderOutput *output);
ASTER_API int ast_encoder_x86_64_function(const AstRegAllocProgram *program,
                                          size_t function,
                                          AstEncoderOutput *output);
ASTER_API int ast_encoder_aarch64_function(const AstRegAllocProgram *program,
                                           size_t function,
                                           AstEncoderOutput *output);
ASTER_API int ast_encoder_riscv64_function(const AstRegAllocProgram *program,
                                           size_t function,
                                           AstEncoderOutput *output);

#ifdef __cplusplus
}
#endif

#endif
