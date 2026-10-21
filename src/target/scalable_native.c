#include "../isa/aarch64_data_contract.h"
#include "../isa/aarch64_scalable.h"
#include "../isa/system_contract.h"
#include "../isa/vector_contract.h"
#include <ctype.h>

static int scalable_name(const AstHirNativeExpr *expr, uint32_t depth) {
  AstHirNativeExprInfo e;
  if (depth > 32u || !expr || !ast_hir_native_expr_info(expr, &e))
    return 0;
  if (e.kind == AST_HIR_NATIVE_EXPR_NAME &&
      e.binding_kind == AST_HIR_NATIVE_BINDING_TARGET &&
      e.name.span.length >= 2u) {
    int c = tolower((unsigned char)e.name.text[0]),
        n = tolower((unsigned char)e.name.text[1]);
    if ((c == 'z' && (n == 'a' || isdigit(n))) || (c == 'p' && isdigit(n)))
      return 1;
  }
  for (uint32_t j = 0; j < e.child_count; ++j)
    if (scalable_name(ast_hir_native_expr_child(expr, j), depth + 1u))
      return 1;
  return 0;
}
int a64_scalable_statement(AstTargetArch arch, const AstHirNativeStmtInfo *s) {
  if (arch != AST_TARGET_ARCH_AARCH64 || !s ||
      a64_scalable_find(s->mnemonic) == a64_scalable_form_count)
    return 0;
  for (uint32_t j = 0; j < s->operand_count; ++j)
    if (scalable_name(s->operands[j].is_memory ? s->operands[j].memory_address
                                               : s->operands[j].value,
                      0u))
      return 1;
  if (vector_statement_form(arch, s) || a64_scalar_source(arch, s->mnemonic) ||
      a64_data_source(arch, s->mnemonic))
    return 0;
  static const char *const shared[] = {
      "add",  "sub",  "and",    "orr",    "eor",   "bic",  "orn",  "lsl",
      "lsr",  "asr",  "mul",    "smulh",  "umulh", "sdiv", "udiv", "clz",
      "rbit", "rev",  "fadd",   "fsub",   "fmul",  "fdiv", "fneg", "fabs",
      "fmin", "fmax", "fminnm", "fmaxnm", "ldr",   "str",  "mov",  "fmov"};
  for (uint32_t j = 0; j < sizeof(shared) / sizeof(shared[0]); ++j)
    if (system_name(s->mnemonic, shared[j]))
      return 0;
  return 1;
}
