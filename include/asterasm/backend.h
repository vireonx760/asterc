#ifndef ASTERASM_BACKEND_H
#define ASTERASM_BACKEND_H

#include "asterasm/machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstBackendConfig {
  AstMachineConfig machine;
} AstBackendConfig;

ASTER_API AstBackendConfig ast_backend_config_default(void);
/*
 * Convenience compiler-backend pipeline: ABI-lowered program -> Machine IR ->
 * RegBank -> ISel. The result has the same borrowed-source lifetime contract as
 * ast_machine_build().
 */
ASTER_API AstMachineProgram *ast_backend_lower(const AstAbiProgram *abi,
                                               const AstBackendConfig *config);

#ifdef __cplusplus
}
#endif

#endif
