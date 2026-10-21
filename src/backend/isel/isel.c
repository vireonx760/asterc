#include "machine_internal.h"

typedef struct SelectContext {
  AstMachineProgram *program;
  AstTargetArch arch;
} SelectContext;
static int select_task(void *opaque, size_t index) {
  SelectContext *ctx = opaque;
  AstMachineProgram local = *ctx->program;
  local.functions = &ctx->program->functions[index];
  local.function_count = 1u;
  switch (ctx->arch) {
  case AST_TARGET_ARCH_X86_64:
    return machine_select_x86_64(&local);
  case AST_TARGET_ARCH_AARCH64:
    return machine_select_aarch64(&local);
  case AST_TARGET_ARCH_RISCV64:
    return machine_select_riscv64(&local);
  default:
    return 0;
  }
}
int ast_machine_select_instructions(AstMachineProgram *p) {
  if (!p || p->stage != AST_MACHINE_STAGE_REGBANK)
    return 0;
  SelectContext ctx = {p, ast_target_config(p->target).arch};
  if (ctx.arch != AST_TARGET_ARCH_X86_64 &&
      ctx.arch != AST_TARGET_ARCH_AARCH64 &&
      ctx.arch != AST_TARGET_ARCH_RISCV64)
    return 0;
  if (!ast_parallel_check(p->function_count, 0u, select_task, &ctx, NULL))
    return 0;
  p->stage = AST_MACHINE_STAGE_SELECTED;
  return !p->config.verify_after_each_stage || ast_machine_verify(p);
}
