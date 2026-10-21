#ifndef ASTERASM_COMPILER_PIPELINE_H
#define ASTERASM_COMPILER_PIPELINE_H

#include "asterasm/asterasm.h"
#include <stdint.h>

typedef enum AsterCompilerEmit {
  ASTER_COMPILER_EMIT_BINARY = 0,
  ASTER_COMPILER_EMIT_OBJECT = 1
} AsterCompilerEmit;

typedef struct CompilerBackendProfile {
  double mir_seconds, target_seconds, abi_seconds, machine_seconds;
  double allocation_encoding_seconds, layout_seconds;
  size_t candidates;
} CompilerBackendProfile;

typedef struct AsterCompilerOptions {
  CompilerBackendProfile
      *backend_profile; /* Internal, owned by one CLI build. */
  AstTargetConfig target;
  AstAbiKind abi;
  AsterCompilerEmit emit;
  uint32_t object_format;
  const char *output_path;
  const char *optimization_report;
  double frontend_seconds, sema_seconds;
  uint64_t base_address;
  size_t thread_count;
  uint32_t register_limit;
  uint8_t promote_locals;
  uint8_t optimize_mir;
  uint8_t optimize_size;
  uint8_t disable_inlining;
  uint8_t disable_red_zone;
  uint8_t print_stats;
  uint8_t dump_hir;
  uint8_t dump_mir;
  uint8_t dump_target;
  uint8_t dump_abi;
  uint8_t dump_machine;
  uint8_t dump_regalloc;
  uint8_t retain_backend;
} AsterCompilerOptions;

int aster_compiler_build(AstFrontend *frontend, AstSema *sema,
                         const AsterCompilerOptions *options);

#endif
