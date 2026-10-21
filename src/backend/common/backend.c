#include "asterasm/backend.h"
AstBackendConfig ast_backend_config_default(void) {
  AstBackendConfig c;
  c.machine = ast_machine_config_default();
  return c;
}
AstMachineProgram *ast_backend_lower(const AstAbiProgram *abi,
                                     const AstBackendConfig *config) {
  AstBackendConfig c = config ? *config : ast_backend_config_default();
  AstMachineProgram *p = ast_machine_build(abi, &c.machine);
  if (!p)
    return NULL;
  if (!ast_machine_assign_regbanks(p) || !ast_machine_select_instructions(p)) {
    ast_machine_destroy(p);
    return NULL;
  }
  return p;
}
