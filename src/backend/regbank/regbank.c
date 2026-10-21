#include "machine_internal.h"

int machine_assign_regbanks_impl(AstMachineProgram *p) {
  if (!p || p->stage != AST_MACHINE_STAGE_GENERIC)
    return 0;
  for (size_t fi = 0; fi < p->function_count; ++fi) {
    MachineFunction *fn = &p->functions[fi];
    for (uint32_t v = 0; v < fn->info.vreg_count; ++v) {
      AstMachineRegBank b = machine_bank_for_class(fn->vregs[v].reg_class);
      if (b == AST_MACHINE_BANK_INVALID) {
        AstSemaTypeKind k = fn->vregs[v].type
                                ? ast_sema_type_kind(fn->vregs[v].type)
                                : AST_SEMA_TYPE_ERROR;
        if (k == AST_SEMA_TYPE_PTR || k == AST_SEMA_TYPE_CAP ||
            k == AST_SEMA_TYPE_INT || k == AST_SEMA_TYPE_BOOL ||
            k == AST_SEMA_TYPE_BITS)
          b = AST_MACHINE_BANK_GPR;
        else if (k == AST_SEMA_TYPE_FLOAT)
          b = AST_MACHINE_BANK_FP;
        else if (k == AST_SEMA_TYPE_VECTOR || k == AST_SEMA_TYPE_SVECTOR)
          b = AST_MACHINE_BANK_VECTOR;
        else if (k == AST_SEMA_TYPE_MASK || k == AST_SEMA_TYPE_PRED)
          b = AST_MACHINE_BANK_MASK;
        else
          b = AST_MACHINE_BANK_GPR;
      }
      fn->vregs[v].bank = b;
    }
  }
  p->stage = AST_MACHINE_STAGE_REGBANK;
  return 1;
}

static int assign_regbanks_task(void *opaque, size_t index) {
  AstMachineProgram *p = opaque;
  AstMachineProgram local = *p;
  local.functions = &p->functions[index];
  local.function_count = 1u;
  return machine_assign_regbanks_impl(&local);
}
int ast_machine_assign_regbanks(AstMachineProgram *p) {
  if (!p || p->stage != AST_MACHINE_STAGE_GENERIC ||
      !ast_parallel_check(p->function_count, 0u, assign_regbanks_task, p, NULL))
    return 0;
  p->stage = AST_MACHINE_STAGE_REGBANK;
  return !p->config.verify_after_each_stage || ast_machine_verify(p);
}
