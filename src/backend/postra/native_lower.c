#include "../../isa/aarch64_scalable.h"
#include "../../isa/aarch64_scalar_contract.h"
#include "../../isa/riscv64_branch_contract.h"
#include "../../isa/riscv64_memory_contract.h"
#include "../../isa/riscv64_scalar_contract.h"
#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "physical_internal.h"
#include "x86_system_contract.h"
#include <ctype.h>
#include <limits.h>

int a64_move(PhysicalContext *, AstPhysicalOperand, AstPhysicalOperand);
int rv_move(PhysicalContext *, AstPhysicalOperand, AstPhysicalOperand);
static int native_move(PhysicalContext *c, AstPhysicalOperand dst,
                       AstPhysicalOperand src) {
  return c->program->target.arch == AST_TARGET_ARCH_AARCH64
             ? a64_move(c, dst, src)
         : c->program->target.arch == AST_TARGET_ARCH_RISCV64
             ? rv_move(c, dst, src)
             : physical_move(c, dst, src);
}

/* Native islands have their own short register lifetimes. All transfers and
   preservation happen outside the island, including for exact regions. */
typedef struct NativeOutput {
  uint32_t object;
  uint32_t bits;
} NativeOutput;
typedef struct NativeRegionState {
  NativeOutput *outputs;
  uint32_t output_count;
  uint8_t lowered;
} NativeRegionState;
struct NativeLowerState {
  NativeRegionState *regions;
  uint32_t region_count;
};
typedef struct NativePlan {
  PhysicalContext *context;
  const AstHirNativeRegion *region;
  AstHirNativeRegionInfo info;
  AstPhysicalOperand inputs[14];
  AstPhysicalOperand outputs[14];
  uint8_t output_defined[14];
  uint8_t hard[MACHINE_REGISTER_UNITS];
  uint8_t written[MACHINE_REGISTER_UNITS];
  uint8_t allocated[MACHINE_REGISTER_UNITS];
  uint16_t preserve_bits[MACHINE_REGISTER_UNITS];
  int raw;
  int allow_raw_bytes;
  int terminated;
  uint32_t statement_index;
} NativePlan;

static int native_fail(PhysicalContext *c, const char *message) {
  return regalloc_fail(c->program, AST_REGALLOC_DIAG_IMPOSSIBLE_CONSTRAINT,
                       c->function, c->instruction, message);
}
static int native_form_fail(NativePlan *plan, const char *message) {
  AstRegAllocProgram *p = plan->context->program;
  AstHirNativeStmtInfo statement;
  if (p->stats.error_count)
    return 0;
  if (!ast_hir_native_region_statement(plan->region, plan->statement_index,
                                       &statement) ||
      !statement.mnemonic.text)
    return native_fail(plan->context, message);
  uint32_t length = statement.mnemonic.span.length < 64u
                        ? statement.mnemonic.span.length
                        : 64u;
  (void)snprintf(p->diagnostic_storage, sizeof(p->diagnostic_storage),
                 "%s Native instruction: '%.*s'.", message, (int)length,
                 statement.mnemonic.text);
  (void)native_fail(plan->context, p->diagnostic_storage);
  p->diagnostic.span = statement.span;
  return 0;
}
static int name_equal(AstName name, const char *text) {
  size_t length = strlen(text);
  if (!name.text || name.span.length != length)
    return 0;
  for (size_t i = 0; i < length; ++i)
    if (tolower((unsigned char)name.text[i]) != (unsigned char)text[i])
      return 0;
  return 1;
}
static int name_string(AstName name, char out[AST_MACHINE_REGISTER_NAME_CAP]) {
  if (!name.text || !name.span.length ||
      name.span.length >= AST_MACHINE_REGISTER_NAME_CAP)
    return 0;
  for (uint32_t i = 0; i < name.span.length; ++i)
    out[i] = (char)tolower((unsigned char)name.text[i]);
  out[name.span.length] = '\0';
  return 1;
}
static int scalar_bits(uint32_t bits) {
  return bits == 8u || bits == 16u || bits == 32u || bits == 64u;
}
static AstPhysicalOperand native_symbol(AstName name) {
  AstPhysicalOperand op = {0};
  if (!name.text || !name.span.length)
    return op;
  op.kind = AST_PHYSICAL_OPERAND_SYMBOL;
  op.bit_width = 64u;
  op.symbol = name;
  return op;
}
static AstPhysicalOperand hard_register(NativePlan *plan, AstName name) {
  AstPhysicalOperand invalid = {0};
  char text[AST_MACHINE_REGISTER_NAME_CAP];
  MachinePhysRegView view;
  AstTargetRegisterInfo target_register;
  if (plan->context->program->target.arch != AST_TARGET_ARCH_X86_64) {
    if (!name_string(name, text) ||
        !machine_physreg_view(&plan->context->program->target, text, &view) ||
        view.special || view.index >= 32u ||
        (view.file != MACHINE_PHYSREG_FILE_GPR &&
         view.file != MACHINE_PHYSREG_FILE_FP_VECTOR))
      return invalid;
    return physical_named_register(plan->context, text, view.width);
  }
  if (!name_string(name, text) ||
      !machine_physreg_view(&plan->context->program->target, text, &view) ||
      !ast_target_parse_register(&plan->context->program->target, text,
                                 &target_register) ||
      view.index >= 16u || view.special)
    return invalid;
  if (view.file == MACHINE_PHYSREG_FILE_GPR && scalar_bits(view.width))
    return physical_named_register(plan->context, text, view.width);
  /* Generic typed MOV bindings use XMM0..15. Explicit VEX/EVEX forms are
     parsed separately and reserve their complete alias during the scan. */
  if (view.file == MACHINE_PHYSREG_FILE_FP_VECTOR && view.width == 128u &&
      target_register.reg_class == AST_TARGET_REGCLASS_VEC128)
    return physical_register(plan->context, AST_MACHINE_BANK_VECTOR, view.index,
                             128u);
  return invalid;
}
static int scan_hard_registers(NativePlan *plan, const AstHirNativeExpr *expr) {
  AstHirNativeExprInfo info;
  if (!expr || !ast_hir_native_expr_info(expr, &info))
    return 0;
  if (info.kind == AST_HIR_NATIVE_EXPR_NAME &&
      info.binding_kind == AST_HIR_NATIVE_BINDING_TARGET) {
    AstPhysicalOperand reg = hard_register(plan, info.name);
    /* A target-space name that is not a hardware register is a symbolic
       address candidate. Its exact admissibility is checked by the
       instruction form (currently LEA); do not reject it during the
       register pre-scan. */
    if (!reg.kind) {
      char name[AST_MACHINE_REGISTER_NAME_CAP];
      MachinePhysRegView view;
      if (name_string(info.name, name) &&
          machine_physreg_view(&plan->context->program->target, name, &view) &&
          view.file == MACHINE_PHYSREG_FILE_FP_VECTOR && view.index < 32u &&
          (view.width == 128u || view.width == 256u || view.width == 512u))
        plan->hard[view.alias_unit] = 1u;
      return 1;
    }
    if (reg.register_unit >= MACHINE_REGISTER_UNITS)
      return 0;
    plan->hard[reg.register_unit] = 1u;
  }
  for (uint32_t i = 0; i < info.child_count; ++i)
    if (!scan_hard_registers(plan, ast_hir_native_expr_child(expr, i)))
      return 0;
  return 1;
}
static AstPhysicalOperand
allocate_binding(NativePlan *plan, AstMachineRegBank bank, uint32_t bits) {
  static const uint32_t gpr_order[] = {11u, 10u, 0u, 1u,  2u,  8u,  9u,
                                       6u,  7u,  3u, 12u, 13u, 14u, 15u};
  static const uint32_t xmm_order[] = {15u, 14u, 13u, 12u, 11u, 10u, 9u, 8u,
                                       7u,  6u,  5u,  4u,  3u,  2u,  1u, 0u};
  static const uint32_t arm_order[] = {16u, 17u, 9u,  10u, 11u, 12u,
                                       13u, 14u, 15u, 8u,  0u,  1u,
                                       2u,  3u,  4u,  5u,  6u,  7u};
  static const uint32_t rv_order[] = {31u, 30u, 29u, 28u, 7u,  6u,  5u, 10u,
                                      11u, 12u, 13u, 14u, 15u, 16u, 17u};
  AstPhysicalOperand invalid = {0};
  const uint32_t *order = NULL;
  uint32_t count = 0u;
  MachinePhysRegFile file = MACHINE_PHYSREG_FILE_INVALID;
  if (bank == AST_MACHINE_BANK_GPR && scalar_bits(bits)) {
    order = gpr_order;
    count = ASTER_ARRAY_COUNT(gpr_order);
    file = MACHINE_PHYSREG_FILE_GPR;
    if (plan->context->program->target.arch == AST_TARGET_ARCH_AARCH64) {
      order = arm_order;
      count = ASTER_ARRAY_COUNT(arm_order);
    } else if (plan->context->program->target.arch == AST_TARGET_ARCH_RISCV64) {
      order = rv_order;
      count = ASTER_ARRAY_COUNT(rv_order);
    }
  } else if (bank == AST_MACHINE_BANK_VECTOR && bits == 128u) {
    order = xmm_order;
    count = ASTER_ARRAY_COUNT(xmm_order);
    file = MACHINE_PHYSREG_FILE_FP_VECTOR;
  } else
    return invalid;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t index = order[i], unit = (uint32_t)file * 64u + index;
    if (unit >= MACHINE_REGISTER_UNITS || plan->hard[unit] ||
        plan->allocated[unit])
      continue;
    plan->allocated[unit] = plan->written[unit] = 1u;
    return physical_register(plan->context, bank, index, bits);
  }
  return invalid;
}
static int literal_integer(AstHirText text, uint64_t *out) {
  if (!text.data || !text.length)
    return 0;
  size_t i = 0;
  uint32_t base = 10u;
  uint64_t value = 0u;
  if (text.length > 2u && text.data[0] == '0') {
    char prefix = text.data[1];
    if (prefix == 'x' || prefix == 'X') {
      base = 16u;
      i = 2u;
    } else if (prefix == 'b' || prefix == 'B') {
      base = 2u;
      i = 2u;
    } else if (prefix == 'o' || prefix == 'O') {
      base = 8u;
      i = 2u;
    }
  }
  uint32_t digits = 0u;
  for (; i < text.length; ++i) {
    unsigned char ch = (unsigned char)text.data[i];
    if (ch == '_')
      continue;
    uint32_t digit = ch >= '0' && ch <= '9'   ? (uint32_t)(ch - '0')
                     : ch >= 'a' && ch <= 'f' ? (uint32_t)(ch - 'a') + 10u
                     : ch >= 'A' && ch <= 'F' ? (uint32_t)(ch - 'A') + 10u
                                              : UINT32_MAX;
    if (digit >= base || value > (UINT64_MAX - digit) / base)
      return 0;
    value = value * base + digit;
    ++digits;
  }
  *out = value;
  return digits != 0u;
}
static AstPhysicalOperand expression(NativePlan *plan,
                                     const AstHirNativeExpr *expr, int write) {
  AstPhysicalOperand invalid = {0};
  AstHirNativeExprInfo info;
  if (!expr || !ast_hir_native_expr_info(expr, &info))
    return invalid;
  if (!write && info.kind == AST_HIR_NATIVE_EXPR_LITERAL &&
      info.token_kind == AST_TOK_INTEGER) {
    uint64_t value;
    return literal_integer(info.literal, &value)
               ? physical_immediate(value, 64u)
               : invalid;
  }
  if (!write && info.kind == AST_HIR_NATIVE_EXPR_UNARY &&
      info.child_count == 1u &&
      (info.token_kind == AST_TOK_MINUS || info.token_kind == AST_TOK_PLUS)) {
    AstPhysicalOperand child =
        expression(plan, ast_hir_native_expr_child(expr, 0u), 0);
    if (child.kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
      return invalid;
    if (info.token_kind == AST_TOK_MINUS)
      child.immediate_bits = UINT64_C(0) - child.immediate_bits;
    return child;
  }
  if (info.kind == AST_HIR_NATIVE_EXPR_CONSTANT && !write &&
      !info.constant.hi) {
    uint64_t value = info.constant.is_negative ? UINT64_C(0) - info.constant.lo
                                               : info.constant.lo;
    return physical_immediate(value, 64u);
  }
  if (info.kind != AST_HIR_NATIVE_EXPR_NAME)
    return invalid;
  if (info.binding_kind == AST_HIR_NATIVE_BINDING_INPUT) {
    if (info.binding_index >= plan->info.input_count || write)
      return invalid;
    return plan->inputs[info.binding_index];
  }
  if (info.binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT) {
    if (info.binding_index >= plan->info.output_count ||
        (!write && !plan->output_defined[info.binding_index]))
      return invalid;
    return plan->outputs[info.binding_index];
  }
  AstPhysicalOperand reg = hard_register(plan, info.name);
  if (!reg.kind && !write)
    return native_symbol(info.name);
  if (reg.kind && write) {
    uint32_t index = reg.register_unit % 64u;
    if (!plan->raw && reg.bank == AST_MACHINE_BANK_GPR &&
        (index == 4u || index == 5u))
      return invalid;
    if (reg.register_unit >= MACHINE_REGISTER_UNITS)
      return invalid;
    plan->written[reg.register_unit] = 1u;
  }
  return reg;
}
static void define_output(NativePlan *plan,
                          const AstHirNativeOperandInfo *operand) {
  AstHirNativeExprInfo info;
  if (!operand->is_memory && ast_hir_native_expr_info(operand->value, &info) &&
      info.kind == AST_HIR_NATIVE_EXPR_NAME &&
      info.binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT &&
      info.binding_index < plan->info.output_count)
    plan->output_defined[info.binding_index] = 1u;
}
typedef struct NativeAddress {
  AstPhysicalOperand base;
  AstPhysicalOperand index;
  uint32_t scale;
  int64_t displacement;
} NativeAddress;

static int native_address_add_disp(NativeAddress *address, int64_t value) {
  if ((value > 0 && address->displacement > INT64_MAX - value) ||
      (value < 0 && address->displacement < INT64_MIN - value))
    return 0;
  address->displacement += value;
  return address->displacement >= INT32_MIN &&
         address->displacement <= INT32_MAX;
}
static int native_address_terms(NativePlan *plan, const AstHirNativeExpr *expr,
                                int sign, NativeAddress *address) {
  AstHirNativeExprInfo info;
  if (!expr || !ast_hir_native_expr_info(expr, &info))
    return 0;
  if (info.kind == AST_HIR_NATIVE_EXPR_BINARY && info.child_count == 2u &&
      (info.token_kind == AST_TOK_PLUS || info.token_kind == AST_TOK_MINUS)) {
    if (!native_address_terms(plan, ast_hir_native_expr_child(expr, 0u), sign,
                              address))
      return 0;
    return native_address_terms(plan, ast_hir_native_expr_child(expr, 1u),
                                info.token_kind == AST_TOK_MINUS ? -sign : sign,
                                address);
  }
  if (info.kind == AST_HIR_NATIVE_EXPR_BINARY && info.child_count == 2u &&
      info.token_kind == AST_TOK_STAR) {
    if (sign < 0 || address->index.kind)
      return 0;
    AstPhysicalOperand a =
        expression(plan, ast_hir_native_expr_child(expr, 0u), 0);
    AstPhysicalOperand b =
        expression(plan, ast_hir_native_expr_child(expr, 1u), 0);
    AstPhysicalOperand reg = a.kind == AST_PHYSICAL_OPERAND_REGISTER ? a : b;
    AstPhysicalOperand imm = a.kind == AST_PHYSICAL_OPERAND_IMMEDIATE ? a : b;
    if (reg.kind != AST_PHYSICAL_OPERAND_REGISTER ||
        reg.bank != AST_MACHINE_BANK_GPR || reg.bit_width != 64u ||
        imm.kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
        (imm.immediate_bits != 1u && imm.immediate_bits != 2u &&
         imm.immediate_bits != 4u && imm.immediate_bits != 8u))
      return 0;
    address->index = reg;
    address->scale = (uint32_t)imm.immediate_bits;
    return 1;
  }
  AstPhysicalOperand term = expression(plan, expr, 0);
  if (term.kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    int64_t value = (int64_t)term.immediate_bits;
    if (sign < 0) {
      if (value == INT64_MIN)
        return 0;
      value = -value;
    }
    return native_address_add_disp(address, value);
  }
  if (sign < 0 || term.kind != AST_PHYSICAL_OPERAND_REGISTER ||
      term.bank != AST_MACHINE_BANK_GPR || term.bit_width != 64u)
    return 0;
  if (!address->base.kind) {
    address->base = term;
    return 1;
  }
  if (!address->index.kind) {
    address->index = term;
    address->scale = 1u;
    return 1;
  }
  return 0;
}
static AstPhysicalOperand
memory_operand(NativePlan *plan, const AstHirNativeOperandInfo *operand) {
  AstPhysicalOperand invalid = {0};
  NativeAddress address = {0};
  if (!operand->memory_address ||
      !native_address_terms(plan, operand->memory_address, 1, &address) ||
      !address.base.kind)
    return invalid;
  uint64_t size = ast_sema_type_size(operand->memory_type);
  uint32_t bits = (uint32_t)size * 8u;
  if (!size || size > 16u || (!scalar_bits(bits) && bits != 128u))
    return invalid;
  /* Stack-relative native addressing needs explicit frame coordinates. It is
     permitted only in a frameless, manually controlled function. */
  if (!plan->raw && address.base.register_unit % 64u == 4u)
    return invalid;
  AstPhysicalOperand memory = address.base;
  memory.kind = AST_PHYSICAL_OPERAND_MEMORY;
  memory.bit_width = bits;
  memory.displacement = address.displacement;
  if (address.index.kind) {
    memory.index_register_unit = address.index.register_unit;
    memcpy(memory.index_register_name, address.index.register_name,
           sizeof(memory.index_register_name));
    memory.scale = (uint8_t)address.scale;
  }
  return memory;
}
static AstPhysicalOperand operand_value(NativePlan *plan,
                                        const AstHirNativeOperandInfo *operand,
                                        int write) {
  return operand->is_memory ? memory_operand(plan, operand)
                            : expression(plan, operand->value, write);
}
static const AstHirNativeOperandInfo *role(const AstHirNativeStmtInfo *stmt,
                                           const char *name) {
  const AstHirNativeOperandInfo *found = NULL;
  for (uint32_t i = 0; i < stmt->operand_count; ++i)
    if (name_equal(stmt->operands[i].name, name)) {
      if (found)
        return NULL;
      found = &stmt->operands[i];
    }
  return found;
}
/* G1 manual A64/RV64 subset. Each accepted form maps to exactly one
   architectural instruction. Non-volatile, stack, return-link and platform
   registers are deliberately not writable until explicit frame contracts
   have been implemented and proven. SP and zero aliases are NOT generic GPRs.
 */
static AstPhysicalOperand
manual_target_register(NativePlan *plan, const AstHirNativeOperandInfo *operand,
                       int destination) {
  AstPhysicalOperand invalid = {0};
  AstHirNativeExprInfo info;
  char text[AST_MACHINE_REGISTER_NAME_CAP];
  MachinePhysRegView view;
  const AstTargetConfig *target = &plan->context->program->target;
  if (!operand || operand->is_memory ||
      !ast_hir_native_expr_info(operand->value, &info) ||
      info.kind != AST_HIR_NATIVE_EXPR_NAME ||
      info.binding_kind != AST_HIR_NATIVE_BINDING_TARGET ||
      !name_string(info.name, text) ||
      !machine_physreg_view(target, text, &view) ||
      view.file != MACHINE_PHYSREG_FILE_GPR || view.special ||
      (view.reserved && !(target->arch == AST_TARGET_ARCH_RISCV64 &&
                          view.index == 0u && !destination)) ||
      view.index >= 32u)
    return invalid;
  if (target->arch == AST_TARGET_ARCH_AARCH64) {
    if ((view.width != 32u && view.width != 64u) || view.index == 18u ||
        (destination == 1 && view.index >= 19u))
      return invalid;
    /* X30 holds the return address for RET; it is read-only here. */
    if (destination && view.index == 30u)
      return invalid;
  } else if (target->arch == AST_TARGET_ARCH_RISCV64) {
    if (view.width != 64u || view.index == 1u || view.index == 2u ||
        view.index == 3u || view.index == 4u ||
        (destination && view.index == 0u) ||
        (destination == 1 && (view.index == 8u || view.index == 9u ||
                              (view.index >= 18u && view.index <= 27u))))
      return invalid;
  } else
    return invalid;
  return physical_named_register(plan->context, text, view.width);
}

/* Return-link registers are not general scratch registers in @naked code.
   Permit ONLY their full-width stack save/restore forms; the independent
   verifier proves provenance and no outstanding save at RET.  This forms the
   necessary ABI foundation for future explicit calls, without enabling calls
   or arbitrary arithmetic on LR/RA prematurely. */
static AstPhysicalOperand
manual_stack_link_register(NativePlan *plan,
                           const AstHirNativeOperandInfo *operand) {
  AstPhysicalOperand invalid = {0};
  AstHirNativeExprInfo info;
  char text[AST_MACHINE_REGISTER_NAME_CAP];
  AstTargetArch arch = plan->context->program->target.arch;
  const char *canonical = arch == AST_TARGET_ARCH_AARCH64 ? "x30" : "ra";
  if (!operand || operand->is_memory ||
      !ast_hir_native_expr_info(operand->value, &info) ||
      info.kind != AST_HIR_NATIVE_EXPR_NAME ||
      info.binding_kind != AST_HIR_NATIVE_BINDING_TARGET ||
      !name_string(info.name, text) ||
      (strcmp(text, canonical) &&
       !(arch == AST_TARGET_ARCH_RISCV64 && !strcmp(text, "x1"))))
    return invalid;
  return physical_named_register(plan->context, text, 64u);
}

/* Only the literal SP spelling is permitted for an explicitly stack-related
   instruction. AArch64 SP is not a GPR (X31 is context-dependent); RV64 x2 is
   a GPR but reserved by the ABI. Never relax normal register admission here. */
static int manual_sp_source(const AstHirNativeOperandInfo *operand) {
  AstHirNativeExprInfo info;
  return operand && !operand->is_memory &&
         ast_hir_native_expr_info(operand->value, &info) &&
         info.kind == AST_HIR_NATIVE_EXPR_NAME &&
         info.binding_kind == AST_HIR_NATIVE_BINDING_TARGET &&
         name_equal(info.name, "sp");
}
static AstPhysicalOperand manual_sp_operand(NativePlan *plan) {
  AstPhysicalOperand out = {0};
  MachinePhysRegView view;
  if (!machine_physreg_view(&plan->context->program->target, "sp", &view) ||
      view.width != 64u)
    return out;
  out.kind = AST_PHYSICAL_OPERAND_REGISTER;
  out.bank = plan->context->program->target.arch == AST_TARGET_ARCH_AARCH64
                 ? AST_MACHINE_BANK_SPECIAL
                 : AST_MACHINE_BANK_GPR;
  out.bit_width = 64u;
  out.register_unit = view.alias_unit;
  memcpy(out.register_name, "sp", 3u);
  return out;
}
/* SP arithmetic is deliberately restricted to a balanced, stack-aligned
   immediate-only region, proven again from the final physical stream. */
static int manual_sp_adjust(NativePlan *plan,
                            const AstHirNativeOperandInfo *rhs,
                            AstMachineOpcode op, AstPhysicalOperand args[3]) {
  const AstTargetArch arch = plan->context->program->target.arch;
  int64_t amount;
  args[2] = operand_value(plan, rhs, 0);
  if (args[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
    return 0;
  amount = (int64_t)args[2].immediate_bits;
  if (arch == AST_TARGET_ARCH_AARCH64) {
    /* ADD/SUB immediate accept imm12 or imm12<<12. A zero adjustment is
       not a useful stack action and could hide unintended SP operations. */
    uint64_t n = args[2].immediate_bits;
    if ((op != AST_MACHINE_A64_ADD && op != AST_MACHINE_A64_SUB) || !n ||
        n > (UINT64_C(4095) << 12u) || (n % 16u) ||
        (n > 4095u && ((n & 4095u) || (n >> 12u) > 4095u)))
      return 0;
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    if (op != AST_MACHINE_RISCV_ADDI || amount < -2048 || amount > 2047 ||
        !amount || (amount % 16))
      return 0;
  } else
    return 0;
  args[0] = manual_sp_operand(plan);
  args[1] = manual_sp_operand(plan);
  args[2].bit_width = 64u;
  return args[0].kind && args[1].kind;
}

/* Exact stack transfers are SP-relative, never an implicit or symbolic
   address. Both forms have a single 32-bit instruction encoding. Stack-slot
   lifetime and preserved-register restoration are proven after lowering. */
static int manual_stack_transfer(NativePlan *plan,
                                 const AstHirNativeStmtInfo *stmt, int emit,
                                 int load) {
  AstTargetArch arch = plan->context->program->target.arch;
  const AstHirNativeOperandInfo *regop = role(stmt, load ? "dst" : "src");
  const AstHirNativeOperandInfo *offop = role(stmt, "offset");
  if (stmt->operand_count != 2u || !regop || !offop || offop->is_memory)
    return 0;
  AstPhysicalOperand reg = manual_target_register(plan, regop, load ? 2 : 0);
  if (!reg.kind)
    reg = manual_stack_link_register(plan, regop);
  AstPhysicalOperand off = operand_value(plan, offop, 0);
  if (!reg.kind || (reg.bit_width != 32u && reg.bit_width != 64u) ||
      (arch == AST_TARGET_ARCH_RISCV64 && reg.bit_width != 64u) ||
      off.kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
    return 0;
  uint64_t disp = off.immediate_bits;
  uint32_t bytes = reg.bit_width / 8u;
  if (disp % bytes ||
      (arch == AST_TARGET_ARCH_AARCH64 && disp / bytes > 4095u) ||
      (arch == AST_TARGET_ARCH_RISCV64 && disp > 2047u))
    return 0;
  AstPhysicalOperand mem = manual_sp_operand(plan);
  if (!mem.kind)
    return 0;
  mem.kind = AST_PHYSICAL_OPERAND_MEMORY;
  mem.bit_width = reg.bit_width;
  mem.displacement = (int64_t)disp;
  AstPhysicalOperand args[2] = {reg, mem};
  if (!emit)
    return 1;
  AstMachineOpcode op =
      arch == AST_TARGET_ARCH_AARCH64
          ? (load ? AST_MACHINE_A64_LOAD : AST_MACHINE_A64_STORE)
          : (load ? AST_MACHINE_RISCV_LD : AST_MACHINE_RISCV_SD);
  return physical_emit(plan->context, op, load ? 1u : 0u, args, 2u);
}

/* Native-local labels are zero-byte directives. Direct branches are fixed
   32-bit encodings; resolve the signed PC-relative offset in instruction
   words before emission. The bounded native region never needs relaxation. */
static int manual_label_name(const AstHirNativeStmtInfo *stmt, AstName *name) {
  const AstHirNativeOperandInfo *op = role(stmt, "name");
  AstHirNativeExprInfo expr;
  if (!op || stmt->operand_count != 1u || stmt->attribute_count ||
      op->is_memory || !ast_hir_native_expr_info(op->value, &expr) ||
      expr.kind != AST_HIR_NATIVE_EXPR_NAME ||
      expr.binding_kind != AST_HIR_NATIVE_BINDING_TARGET ||
      !expr.name.span.length || expr.name.span.length > 80u)
    return 0;
  *name = expr.name;
  return 1;
}
static int manual_name_same(AstName a, AstName b) {
  return a.span.length == b.span.length && a.text && b.text &&
         !memcmp(a.text, b.text, a.span.length);
}
static uint32_t native_source_width(AstTargetArch arch,
                                    const AstHirNativeStmtInfo *stmt) {
  if (name_equal(stmt->mnemonic, "label"))
    return 0u;
  if (arch == AST_TARGET_ARCH_RISCV64 && name_equal(stmt->mnemonic, "jal") &&
      role(stmt, "symbol"))
    return 8u; /* AUIPC ra; JALR ra, ra */
  return 4u;
}
static int manual_label_target(NativePlan *plan, AstName target,
                               int64_t *byte_offset) {
  uint32_t found = UINT32_MAX;
  uint32_t target_pc = 0u, branch_pc = 0u, byte_pc = 0u;
  AstTargetArch arch = plan->context->program->target.arch;
  if (plan->info.statement_count > 4096u)
    return 0;
  for (uint32_t j = 0; j < plan->info.statement_count; ++j) {
    AstHirNativeStmtInfo stmt;
    AstName name;
    if (!ast_hir_native_region_statement(plan->region, j, &stmt))
      return 0;
    if (j == plan->statement_index)
      branch_pc = byte_pc;
    if (name_equal(stmt.mnemonic, "label")) {
      if (!manual_label_name(&stmt, &name))
        return 0;
      if (manual_name_same(name, target)) {
        if (found != UINT32_MAX)
          return 0;
        found = j;
        target_pc = byte_pc;
      }
    }
    byte_pc += native_source_width(arch, &stmt);
  }
  /* Labels must resolve to an instruction, not the end of a region. */
  if (found == UINT32_MAX || target_pc >= byte_pc)
    return 0;
  *byte_offset = (int64_t)target_pc - (int64_t)branch_pc;
  return 1;
}
static int statement_manual_other(NativePlan *plan,
                                  const AstHirNativeStmtInfo *stmt, int emit) {
  AstTargetArch arch = plan->context->program->target.arch;
  if (!plan->raw || stmt->kind != AST_HIR_NATIVE_INSTRUCTION ||
      stmt->attribute_count || plan->terminated)
    return 0;
  if (name_equal(stmt->mnemonic, "label")) {
    AstName name;
    if (!manual_label_name(stmt, &name))
      return 0;
    /* All labels must be unique, including unreachable ones. */
    for (uint32_t j = 0; j < plan->statement_index; ++j) {
      AstHirNativeStmtInfo prior;
      AstName previous;
      if (!ast_hir_native_region_statement(plan->region, j, &prior))
        return 0;
      if (name_equal(prior.mnemonic, "label") &&
          (!manual_label_name(&prior, &previous) ||
           manual_name_same(name, previous)))
        return 0;
    }
    return 1; /* no physical instruction */
  }
  int local_call =
      (arch == AST_TARGET_ARCH_AARCH64 && name_equal(stmt->mnemonic, "bl")) ||
      (arch == AST_TARGET_ARCH_RISCV64 && name_equal(stmt->mnemonic, "jal"));
  int plain_jump =
      (arch == AST_TARGET_ARCH_AARCH64 && name_equal(stmt->mnemonic, "b")) ||
      (arch == AST_TARGET_ARCH_RISCV64 && name_equal(stmt->mnemonic, "jal"));
  int conditional =
      (arch == AST_TARGET_ARCH_AARCH64 &&
       (name_equal(stmt->mnemonic, "cbz") ||
        name_equal(stmt->mnemonic, "cbnz"))) ||
      (arch == AST_TARGET_ARCH_RISCV64 &&
       rv_native_branch_source(stmt->mnemonic) != AST_MACHINE_OP_INVALID);
  if (plain_jump || conditional || local_call) {
    const AstHirNativeOperandInfo *symbol_operand = role(stmt, "symbol");
    const AstHirNativeOperandInfo *to =
        symbol_operand ? symbol_operand : role(stmt, "target");
    AstHirNativeExprInfo expr;
    int64_t displacement;
    AstPhysicalOperand args[3] = {{0}};
    uint32_t operand_count = 1u;
    int call =
        arch == AST_TARGET_ARCH_AARCH64 && name_equal(stmt->mnemonic, "bl");
    if (!to || to->is_memory || !ast_hir_native_expr_info(to->value, &expr) ||
        expr.kind != AST_HIR_NATIVE_EXPR_NAME ||
        expr.binding_kind != AST_HIR_NATIVE_BINDING_TARGET)
      return 0;
    if (conditional) {
      if (arch == AST_TARGET_ARCH_AARCH64) {
        const AstHirNativeOperandInfo *reg = role(stmt, "reg");
        if (stmt->operand_count != 2u || !reg ||
            !(args[0] = manual_target_register(plan, reg, 0)).kind ||
            args[0].register_unit % 64u == 30u)
          return 0;
        operand_count = 2u;
      } else {
        const AstHirNativeOperandInfo *lhs = role(stmt, "lhs");
        const AstHirNativeOperandInfo *rhs = role(stmt, "rhs");
        if (stmt->operand_count != 3u || !lhs || !rhs ||
            !(args[0] = manual_target_register(plan, lhs, 0)).kind ||
            !(args[1] = manual_target_register(plan, rhs, 0)).kind)
          return 0;
        operand_count = 3u;
      }
    } else if (arch == AST_TARGET_ARCH_AARCH64) {
      if (stmt->operand_count != 1u)
        return 0;
    } else {
      const AstHirNativeOperandInfo *dst = role(stmt, "dst");
      AstHirNativeExprInfo rd;
      if (stmt->operand_count != 2u || !dst || dst->is_memory ||
          !ast_hir_native_expr_info(dst->value, &rd) ||
          rd.kind != AST_HIR_NATIVE_EXPR_NAME ||
          rd.binding_kind != AST_HIR_NATIVE_BINDING_TARGET ||
          (!name_equal(rd.name, "zero") && !name_equal(rd.name, "x0") &&
           !name_equal(rd.name, "ra") && !name_equal(rd.name, "x1")))
        return 0;
      call = name_equal(rd.name, "ra") || name_equal(rd.name, "x1");
    }
    if (symbol_operand) {
      /* Symbolic native calls must be spelled explicitly. The native
         PC map below counts RV64 AUIPC/JALR as eight encoded bytes,
         allowing both local branches and symbols in one region. */
      if (!call || !expr.name.text || !expr.name.span.length ||
          expr.name.span.length > 255u ||
          stmt->operand_count != (arch == AST_TARGET_ARCH_AARCH64 ? 1u : 2u))
        return 0;
      if (!emit)
        return 1;
      AstPhysicalOperand symbol = native_symbol(expr.name);
      if (symbol.kind != AST_PHYSICAL_OPERAND_SYMBOL)
        return 0;
      if (!physical_emit(plan->context,
                         arch == AST_TARGET_ARCH_AARCH64
                             ? AST_MACHINE_A64_CALL
                             : AST_MACHINE_RISCV_CALL,
                         0u, &symbol, 1u))
        return 0;
      PhysicalBlock *block = &plan->context->fn->blocks[plan->context->block];
      block->instructions[block->count - 1u].span = stmt->span;
      return 1;
    }
    if (!manual_label_target(plan, expr.name, &displacement))
      return 0;
    int64_t min_offset =
        !conditional ? (arch == AST_TARGET_ARCH_AARCH64 ? -INT64_C(134217728)
                                                        : -INT64_C(1048576))
                     : (arch == AST_TARGET_ARCH_AARCH64 ? -INT64_C(1048576)
                                                        : -INT64_C(4096));
    int64_t max_offset =
        !conditional ? (arch == AST_TARGET_ARCH_AARCH64 ? INT64_C(134217724)
                                                        : INT64_C(1048572))
                     : (arch == AST_TARGET_ARCH_AARCH64 ? INT64_C(1048572)
                                                        : INT64_C(4092));
    if (displacement < min_offset || displacement > max_offset)
      return 0;
    /* Path-sensitive stack/register preservation is proved from the
       completed Physical IR by the independent CFG verifier. Do not
       reject valid, balanced per-edge stack operations here. */
    if (!emit)
      return 1;
    args[operand_count - 1u] = physical_immediate((uint64_t)displacement, 64u);
    AstMachineOpcode branch_opcode =
        arch == AST_TARGET_ARCH_AARCH64
            ? (conditional
                   ? (name_equal(stmt->mnemonic, "cbz")
                          ? AST_MACHINE_A64_NATIVE_CBZ
                          : AST_MACHINE_A64_NATIVE_CBNZ)
                   : (call ? AST_MACHINE_A64_NATIVE_BL : AST_MACHINE_A64_BR))
            : (conditional ? rv_native_branch_source(stmt->mnemonic)
                           : (call ? AST_MACHINE_RISCV_NATIVE_JAL
                                   : AST_MACHINE_RISCV_J));
    if (!physical_emit(plan->context, branch_opcode, 0u, args, operand_count))
      return 0;
  } else if (name_equal(stmt->mnemonic, "ret")) {
    AstMachineOpcode op = arch == AST_TARGET_ARCH_AARCH64
                              ? AST_MACHINE_A64_RET
                              : AST_MACHINE_RISCV_RET;
    if (stmt->operand_count)
      return 0;
    plan->terminated =
        (plan->statement_index + 1u == plan->info.statement_count);
    if (!emit)
      return 1;
    if (!physical_emit(plan->context, op, 0u, NULL, 0u))
      return 0;
  } else if (arch == AST_TARGET_ARCH_RISCV64 &&
             name_equal(stmt->mnemonic, "jalr")) {
    AstPhysicalInstructionInfo packet;
    if (!physical_native_indirect_info(&plan->context->program->target, stmt,
                                       &packet))
      return 0;
    plan->terminated = packet.operands[0].register_unit == 64u &&
                       plan->statement_index + 1u == plan->info.statement_count;
    if (emit &&
        !physical_emit(plan->context, packet.opcode, 0u, packet.operands, 3u))
      return 0;
  } else if (rv_memory_source(arch, stmt->mnemonic)) {
    AstPhysicalInstructionInfo packet;
    if (!physical_native_memory_info(&plan->context->program->target, stmt,
                                     NULL, 1, &packet))
      return 0;
    if (emit &&
        !physical_emit(plan->context, packet.opcode, packet.result_count,
                       packet.operands, packet.operand_count))
      return 0;
  } else if ((arch == AST_TARGET_ARCH_AARCH64 &&
              (name_equal(stmt->mnemonic, "ldr") ||
               name_equal(stmt->mnemonic, "str"))) ||
             (arch == AST_TARGET_ARCH_RISCV64 &&
              (name_equal(stmt->mnemonic, "ld") ||
               name_equal(stmt->mnemonic, "sd")))) {
    int load =
        name_equal(stmt->mnemonic, "ldr") || name_equal(stmt->mnemonic, "ld");
    if (!manual_stack_transfer(plan, stmt, emit, load))
      return 0;
  } else if (physical_native_a64_data_form(&plan->context->program->target,
                                           stmt)) {
    AstPhysicalInstructionInfo packet;
    if (!physical_native_a64_data_info(&plan->context->program->target, stmt,
                                       NULL, &packet))
      return 0;
    if (emit &&
        !physical_emit(plan->context, packet.opcode, packet.result_count,
                       packet.operands, packet.operand_count))
      return 0;
  } else if (a64_scalar_source(arch, stmt->mnemonic)) {
    AstPhysicalInstructionInfo packet;
    if (!physical_native_a64_scalar_info(&plan->context->program->target, stmt,
                                         NULL, &packet))
      return 0;
    if (emit &&
        !physical_emit(plan->context, packet.opcode, packet.result_count,
                       packet.operands, packet.operand_count))
      return 0;
  } else if (arch == AST_TARGET_ARCH_RISCV64 &&
             rv_scalar_source(arch, stmt->mnemonic) &&
             !manual_sp_source(role(stmt, "dst"))) {
    AstPhysicalInstructionInfo packet;
    if (!physical_native_scalar_info(&plan->context->program->target, stmt,
                                     NULL, &packet))
      return 0;
    if (emit &&
        !physical_emit(plan->context, packet.opcode, packet.result_count,
                       packet.operands, packet.operand_count))
      return 0;
  } else {
    const AstHirNativeOperandInfo *dst = role(stmt, "dst");
    const AstHirNativeOperandInfo *lhs = role(stmt, "lhs");
    const AstHirNativeOperandInfo *rhs = role(stmt, "rhs");
    AstPhysicalOperand args[3] = {{0}};
    AstMachineOpcode op = AST_MACHINE_OP_INVALID;
    if (stmt->operand_count != 3u || !dst || !lhs || !rhs)
      return 0;
    const int sp_dst = manual_sp_source(dst);
    const int sp_lhs = manual_sp_source(lhs);
    if (arch == AST_TARGET_ARCH_AARCH64) {
      if (name_equal(stmt->mnemonic, "add"))
        op = AST_MACHINE_A64_ADD;
      else if (name_equal(stmt->mnemonic, "sub"))
        op = AST_MACHINE_A64_SUB;
      else if (name_equal(stmt->mnemonic, "and"))
        op = AST_MACHINE_A64_AND;
      else if (name_equal(stmt->mnemonic, "orr"))
        op = AST_MACHINE_A64_OR;
      else if (name_equal(stmt->mnemonic, "eor"))
        op = AST_MACHINE_A64_XOR;
    } else if (arch == AST_TARGET_ARCH_RISCV64) {
      if (name_equal(stmt->mnemonic, "add"))
        op = AST_MACHINE_RISCV_ADD;
      else if (name_equal(stmt->mnemonic, "sub"))
        op = AST_MACHINE_RISCV_SUB;
      else if (name_equal(stmt->mnemonic, "and"))
        op = AST_MACHINE_RISCV_AND;
      else if (name_equal(stmt->mnemonic, "or"))
        op = AST_MACHINE_RISCV_OR;
      else if (name_equal(stmt->mnemonic, "xor"))
        op = AST_MACHINE_RISCV_XOR;
      else if (name_equal(stmt->mnemonic, "addi"))
        op = AST_MACHINE_RISCV_ADDI;
    }
    if (op == AST_MACHINE_OP_INVALID)
      return 0;
    if (sp_dst || sp_lhs) {
      if (!sp_dst || !sp_lhs || !manual_sp_adjust(plan, rhs, op, args))
        return 0;
    } else {
      args[0] = manual_target_register(plan, dst, 2);
      args[1] = manual_target_register(plan, lhs, 0);
      if (!args[0].kind || !args[1].kind ||
          args[0].bit_width != args[1].bit_width)
        return 0;
    }
    if (!(sp_dst && sp_lhs)) {
      if (op == AST_MACHINE_RISCV_ADDI) {
        args[2] = operand_value(plan, rhs, 0);
        int64_t signed_imm = (int64_t)args[2].immediate_bits;
        if (args[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
            signed_imm < -2048 || signed_imm > 2047)
          return 0;
        args[2].bit_width = 64u;
      } else {
        args[2] = manual_target_register(plan, rhs, 0);
        if (!args[2].kind || args[2].bit_width != args[0].bit_width)
          return 0;
      }
    }
    if (!emit)
      return 1;
    if (!physical_emit(plan->context, op, 1u, args, 3u))
      return 0;
  }
  if (emit) {
    PhysicalBlock *block = &plan->context->fn->blocks[plan->context->block];
    block->instructions[block->count - 1u].span = stmt->span;
  }
  return 1;
}

static int statement(NativePlan *plan, const AstHirNativeStmtInfo *stmt,
                     int emit, AstPhysicalInstructionInfo *expected) {
  if (plan->terminated || stmt->attribute_count)
    return 0;
  if (stmt->kind == AST_HIR_NATIVE_BYTES) {
    if (!plan->raw || !plan->allow_raw_bytes ||
        !(plan->info.flags & AST_HIR_NATIVE_UNSAFE_EXACT) ||
        !(plan->info.flags & AST_HIR_NATIVE_EXACT) || !stmt->bytes ||
        !stmt->byte_count)
      return 0;
    if (expected) {
      memset(expected, 0, sizeof(*expected));
      expected->opcode = AST_MACHINE_X86_RAW_BYTES;
      expected->raw_bytes = stmt->bytes;
      expected->raw_byte_count = stmt->byte_count;
    }
    if (!emit)
      return 1;
    if (!physical_emit(plan->context, AST_MACHINE_X86_RAW_BYTES, 0u, NULL, 0u))
      return 0;
    PhysicalBlock *raw_block = &plan->context->fn->blocks[plan->context->block];
    AstPhysicalInstructionInfo *raw =
        &raw_block->instructions[raw_block->count - 1u];
    raw->span = stmt->span;
    raw->raw_bytes = stmt->bytes;
    raw->raw_byte_count = stmt->byte_count;
    return 1;
  }
  if (stmt->kind != AST_HIR_NATIVE_INSTRUCTION)
    return 0;
  AstMachineOpcode opcode = AST_MACHINE_OP_INVALID;
  AstPhysicalOperand args[3] = {{0}};
  uint32_t defs = 0u, count = 0u;
  const AstHirNativeOperandInfo *dst = NULL, *src = NULL;
  if (name_equal(stmt->mnemonic, "ret")) {
    if (!plan->raw || stmt->operand_count)
      return 0;
    opcode = AST_MACHINE_X86_RET;
    plan->terminated = 1;
  } else if (plan->raw && (name_equal(stmt->mnemonic, "push") ||
                           name_equal(stmt->mnemonic, "pop"))) {
    int pop = name_equal(stmt->mnemonic, "pop");
    const AstHirNativeOperandInfo *value = role(stmt, pop ? "dst" : "src");
    if (stmt->operand_count != 1u || !value || value->is_memory)
      return 0;
    args[0] = operand_value(plan, value, pop);
    if (args[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        args[0].bank != AST_MACHINE_BANK_GPR || args[0].bit_width != 64u ||
        args[0].register_unit % 64u == 4u)
      return 0;
    opcode = pop ? AST_MACHINE_X86_POP : AST_MACHINE_X86_PUSH;
    defs = pop ? 1u : 0u;
    count = 1u;
  } else if (plan->raw && (name_equal(stmt->mnemonic, "call") ||
                           name_equal(stmt->mnemonic, "jmp"))) {
    int jump = name_equal(stmt->mnemonic, "jmp");
    const AstHirNativeOperandInfo *target = role(stmt, "target");
    if (stmt->operand_count != 1u || !target || target->is_memory)
      return 0;
    args[0] = operand_value(plan, target, 0);
    if (args[0].kind == AST_PHYSICAL_OPERAND_SYMBOL)
      opcode = jump ? AST_MACHINE_X86_JMP : AST_MACHINE_X86_CALL;
    else if (args[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
             args[0].bank == AST_MACHINE_BANK_GPR && args[0].bit_width == 64u)
      opcode =
          jump ? AST_MACHINE_X86_JMP_INDIRECT : AST_MACHINE_X86_CALL_INDIRECT;
    else
      return 0;
    count = 1u;
    if (jump)
      plan->terminated = 1;
  } else if (name_equal(stmt->mnemonic, "in") ||
             name_equal(stmt->mnemonic, "out")) {
    int input = name_equal(stmt->mnemonic, "in");
    const AstHirNativeOperandInfo *value = role(stmt, input ? "dst" : "src");
    const AstHirNativeOperandInfo *port = role(stmt, "port");
    if (stmt->operand_count != 2u || !value || !port || value->is_memory ||
        port->is_memory)
      return 0;
    args[0] = operand_value(plan, value, input);
    args[1] = operand_value(plan, port, 0);
    if (args[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
      args[1].bit_width = 8u;
    opcode = input ? AST_MACHINE_X86_IN : AST_MACHINE_X86_OUT;
    defs = input ? 1u : 0u;
    count = 2u;
    AstPhysicalInstructionInfo form = {0};
    form.opcode = opcode;
    form.result_count = defs;
    form.operand_count = count;
    form.operands[0] = args[0];
    form.operands[1] = args[1];
    if (!x86_system_contract(&form))
      return 0;
    if (input)
      define_output(plan, value);
  } else if (name_equal(stmt->mnemonic, "lidt") ||
             name_equal(stmt->mnemonic, "lgdt")) {
    const AstHirNativeOperandInfo *address = role(stmt, "addr");
    if (stmt->operand_count != 1u || !address || address->is_memory)
      return 0;
    args[0] = operand_value(plan, address, 0);
    opcode = name_equal(stmt->mnemonic, "lidt") ? AST_MACHINE_X86_LIDT
                                                : AST_MACHINE_X86_LGDT;
    count = 1u;
    AstPhysicalInstructionInfo form = {0};
    form.opcode = opcode;
    form.operand_count = count;
    form.operands[0] = args[0];
    if (!x86_system_contract(&form))
      return 0;
  } else if (name_equal(stmt->mnemonic, "nop")) {
    if (stmt->operand_count)
      return 0;
    opcode = AST_MACHINE_X86_NOP;
  } else if (name_equal(stmt->mnemonic, "cli")) {
    if (stmt->operand_count)
      return 0;
    opcode = AST_MACHINE_X86_CLI;
  } else if (name_equal(stmt->mnemonic, "sti")) {
    if (stmt->operand_count)
      return 0;
    opcode = AST_MACHINE_X86_STI;
  } else if (name_equal(stmt->mnemonic, "hlt")) {
    if (stmt->operand_count)
      return 0;
    opcode = AST_MACHINE_X86_HLT;
  } else if (name_equal(stmt->mnemonic, "cld")) {
    if (stmt->operand_count)
      return 0;
    opcode = AST_MACHINE_X86_CLD;
  } else {
    if (stmt->operand_count != 2u || !(dst = role(stmt, "dst")) ||
        !(src = role(stmt, "src")))
      return 0;
    args[0] = operand_value(plan, dst, 1);
    args[1] = operand_value(plan, src, 0);
    if (!args[0].kind || !args[1].kind ||
        args[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
      return 0;
    if (args[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
      args[1].bit_width = args[0].bit_width;
    if (name_equal(stmt->mnemonic, "lea")) {
      if (args[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
          args[0].bank != AST_MACHINE_BANK_GPR || args[0].bit_width != 64u ||
          args[1].kind != AST_PHYSICAL_OPERAND_SYMBOL)
        return 0;
      opcode = AST_MACHINE_X86_LEA;
      defs = 1u;
      count = 2u;
    } else if (name_equal(stmt->mnemonic, "mov")) {
      if (args[0].bit_width != args[1].bit_width)
        return 0;
      if (args[0].kind == AST_PHYSICAL_OPERAND_MEMORY) {
        if (args[1].kind == AST_PHYSICAL_OPERAND_MEMORY ||
            (args[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
             args[1].bit_width == 64u && args[1].immediate_bits > INT32_MAX &&
             args[1].immediate_bits < UINT64_C(0xffffffff80000000)))
          return 0;
        AstPhysicalOperand swap = args[0];
        args[0] = args[1];
        args[1] = swap;
        opcode = AST_MACHINE_X86_STORE;
      } else {
        defs = 1u;
        opcode = args[1].kind == AST_PHYSICAL_OPERAND_MEMORY
                     ? AST_MACHINE_X86_LOAD
                 : args[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE
                     ? AST_MACHINE_X86_MOV_IMM
                     : AST_MACHINE_X86_MOV;
      }
      count = 2u;
    } else {
      if (args[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
          args[0].bank != AST_MACHINE_BANK_GPR ||
          args[1].kind == AST_PHYSICAL_OPERAND_MEMORY ||
          (args[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           args[1].bank != AST_MACHINE_BANK_GPR))
        return 0;
      if (name_equal(stmt->mnemonic, "popcnt")) {
        if (!(plan->context->program->target.features &
              AST_TARGET_FEATURE_X86_POPCNT) ||
            args[0].bit_width < 16u || args[0].bit_width != args[1].bit_width ||
            args[1].kind != AST_PHYSICAL_OPERAND_REGISTER)
          return 0;
        opcode = AST_MACHINE_X86_POPCNT;
        defs = 1u;
        count = 2u;
      } else {
        /* Two-address arithmetic reads its destination before writing.
           An output must have been defined by an earlier statement. */
        if (!operand_value(plan, dst, 0).kind)
          return 0;
        if (name_equal(stmt->mnemonic, "add"))
          opcode = AST_MACHINE_X86_ADD;
        else if (name_equal(stmt->mnemonic, "sub"))
          opcode = AST_MACHINE_X86_SUB;
        else if (name_equal(stmt->mnemonic, "and"))
          opcode = AST_MACHINE_X86_AND;
        else if (name_equal(stmt->mnemonic, "or"))
          opcode = AST_MACHINE_X86_OR;
        else if (name_equal(stmt->mnemonic, "xor"))
          opcode = AST_MACHINE_X86_XOR;
        else if (name_equal(stmt->mnemonic, "imul"))
          opcode = AST_MACHINE_X86_MUL;
        else if (name_equal(stmt->mnemonic, "shl"))
          opcode = AST_MACHINE_X86_SHL;
        else if (name_equal(stmt->mnemonic, "shr"))
          opcode = AST_MACHINE_X86_LSHR;
        else if (name_equal(stmt->mnemonic, "sar"))
          opcode = AST_MACHINE_X86_ASHR;
        else
          return 0;
        int shift = opcode == AST_MACHINE_X86_SHL ||
                    opcode == AST_MACHINE_X86_LSHR ||
                    opcode == AST_MACHINE_X86_ASHR;
        if (shift) {
          if (args[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
              args[1].register_unit % 64u != 1u || args[1].bit_width != 8u)
            return 0;
        } else {
          if (args[0].bit_width != args[1].bit_width)
            return 0;
          if (opcode == AST_MACHINE_X86_MUL &&
              (args[0].bit_width == 8u ||
               args[1].kind != AST_PHYSICAL_OPERAND_REGISTER))
            return 0;
          if (args[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
              args[1].bit_width == 64u && args[1].immediate_bits > INT32_MAX &&
              args[1].immediate_bits < UINT64_C(0xffffffff80000000))
            return 0;
        }
        args[2] = args[1];
        args[1] = args[0];
        defs = 1u;
        count = 3u;
      }
    }
    define_output(plan, dst);
  }
  if (expected) {
    memset(expected, 0, sizeof(*expected));
    expected->opcode = opcode;
    expected->result_count = defs;
    expected->operand_count = count;
    if (count)
      memcpy(expected->operands, args, (size_t)count * sizeof(*args));
  }
  if (!emit)
    return 1;
  if (!physical_emit(plan->context, opcode, defs, args, count))
    return 0;
  PhysicalBlock *block = &plan->context->fn->blocks[plan->context->block];
  block->instructions[block->count - 1u].span = stmt->span;
  return 1;
}
static int statements(NativePlan *plan, int emit) {
  memset(plan->output_defined, 0, sizeof(plan->output_defined));
  plan->terminated = 0;
  for (uint32_t i = 0; i < plan->info.statement_count; ++i) {
    AstHirNativeStmtInfo stmt;
    plan->statement_index = i;
    if (!ast_hir_native_region_statement(plan->region, i, &stmt))
      return 0;
    const SystemForm *system =
        system_source_form(plan->context->program->target.arch, &stmt);
    const VectorForm *vector =
        vector_statement_form(plan->context->program->target.arch, &stmt);
    if (plan->raw &&
        a64_scalable_statement(plan->context->program->target.arch, &stmt)) {
      AstPhysicalInstructionInfo form;
      if (!plan->raw || plan->terminated ||
          !physical_native_scalable_info(&plan->context->program->target, &stmt,
                                         &form))
        return 0;
      if (emit) {
        if (!physical_emit(plan->context, form.opcode, form.result_count,
                           form.operands, form.operand_count))
          return 0;
        PhysicalBlock *b = &plan->context->fn->blocks[plan->context->block];
        b->instructions[b->count - 1u].span = stmt.span;
      }
      continue;
    }
    if (!plan->raw &&
        plan->context->program->target.arch != AST_TARGET_ARCH_X86_64) {
      AstPhysicalInstructionInfo form;
      NativeBindings bindings = {plan->inputs, plan->outputs,
                                 plan->output_defined, plan->info.input_count,
                                 plan->info.output_count};
      if (!physical_native_other_info(&plan->context->program->target, &stmt,
                                      &bindings, &form))
        return 0;
      for (uint32_t d = 0; d < form.result_count; ++d) {
        uint32_t unit = form.operands[d].register_unit;
        if (unit >= MACHINE_REGISTER_UNITS)
          return 0;
        if ((plan->context->program->target.arch == AST_TARGET_ARCH_RISCV64 &&
             unit == 64u) ||
            (plan->context->program->target.arch == AST_TARGET_ARCH_AARCH64 &&
             unit == 321u))
          continue;
        plan->written[unit] = 1u;
        if (form.operands[d].bank == AST_MACHINE_BANK_VECTOR)
          plan->preserve_bits[unit] = 128u;
      }
      const AstHirNativeOperandInfo *dst = role(&stmt, "dst");
      if (dst)
        define_output(plan, dst);
      if (emit) {
        if (!physical_emit(plan->context, form.opcode, form.result_count,
                           form.operands, form.operand_count))
          return 0;
        PhysicalBlock *b = &plan->context->fn->blocks[plan->context->block];
        b->instructions[b->count - 1u].span = stmt.span;
      }
      continue;
    }
    if (vector) {
      AstPhysicalInstructionInfo form;
      if (plan->terminated ||
          !physical_native_vector_info(&plan->context->program->target, &stmt,
                                       plan->raw, &form))
        return 0;
      for (uint32_t d = 0; d < form.result_count; ++d) {
        uint32_t unit = form.operands[d].register_unit;
        if (unit >= MACHINE_REGISTER_UNITS)
          return 0;
        plan->written[unit] = 1u;
        /* VEX.128 also zeros the upper YMM half. Preserve the complete
           alias before any instruction or input shuffle touches it. */
        uint16_t bits =
            vector_evex(vector) || (vector_vex(vector) &&
                                    (plan->context->program->target.features &
                                     AST_TARGET_FEATURE_X86_AVX512F))
                ? 512u
            : vector_vex(vector) ? 256u
                                 : 128u;
        if (bits > plan->preserve_bits[unit])
          plan->preserve_bits[unit] = bits;
      }
      if (emit) {
        if (!physical_emit(plan->context, form.opcode, form.result_count,
                           form.operands, form.operand_count))
          return 0;
        PhysicalBlock *b = &plan->context->fn->blocks[plan->context->block];
        b->instructions[b->count - 1u].span = stmt.span;
      }
    } else if (system) {
      AstPhysicalInstructionInfo form;
      if (plan->terminated ||
          !physical_native_system_info(&plan->context->program->target, &stmt,
                                       &form))
        return 0;
      if (system_exception_return(&form)) {
        if (!plan->raw || i + 1u != plan->info.statement_count)
          return 0;
        plan->terminated = 1;
      }
      for (uint32_t d = 0; d < form.result_count; ++d) {
        uint32_t unit = form.operands[d].register_unit;
        if (unit >= MACHINE_REGISTER_UNITS)
          return 0;
        plan->written[unit] = 1u;
      }
      if (!physical_native_system(plan->context, &stmt, emit))
        return 0;
    } else if (!(plan->raw && plan->context->program->target.arch !=
                                  AST_TARGET_ARCH_X86_64
                     ? statement_manual_other(plan, &stmt, emit)
                     : statement(plan, &stmt, emit, NULL)))
      return 0;
    /* A trailing opaque byte blob is the terminal contract of an unsafe
       exact naked function. The compiler must not append an implicit RET,
       otherwise the requested byte sequence would no longer be exact. */
    if (plan->raw && plan->allow_raw_bytes &&
        stmt.kind == AST_HIR_NATIVE_BYTES &&
        i + 1u == plan->info.statement_count)
      plan->terminated = 1;
  }
  /* Multiple RETs are legal only when a local call is present: the first
     RET terminates the caller, later RETs terminate reachable leaf code.
     Preserve the old single-terminal rule for all other exact regions. */
  if (plan->raw &&
      plan->context->program->target.arch != AST_TARGET_ARCH_X86_64) {
    int call_present = 0;
    for (uint32_t i = 0; i < plan->info.statement_count; ++i) {
      AstHirNativeStmtInfo stmt;
      if (!ast_hir_native_region_statement(plan->region, i, &stmt))
        return 0;
      if (name_equal(stmt.mnemonic, "bl"))
        call_present = 1;
      if (plan->context->program->target.arch == AST_TARGET_ARCH_RISCV64 &&
          name_equal(stmt.mnemonic, "jal")) {
        const AstHirNativeOperandInfo *rd = role(&stmt, "dst");
        AstHirNativeExprInfo e;
        if (rd && ast_hir_native_expr_info(rd->value, &e) &&
            e.kind == AST_HIR_NATIVE_EXPR_NAME &&
            (name_equal(e.name, "ra") || name_equal(e.name, "x1")))
          call_present = 1;
      }
      if (i + 1u < plan->info.statement_count &&
          name_equal(stmt.mnemonic, "ret") && !call_present)
        return 0;
    }
  }
  for (uint32_t i = 0; i < plan->info.output_count; ++i)
    if (!plan->output_defined[i])
      return 0;
  return 1;
}
static int source_info(PhysicalContext *c, const MachineInstruction *mi,
                       AstMirInstructionInfo *source) {
  return mi->info.source_instruction != AST_MIR_INVALID_ID &&
         ast_mir_function_instruction(
             ast_mir_function_at(c->program->machine->mir, c->function),
             mi->info.source_instruction, source);
}
static int region_plan(PhysicalContext *c, const AstMirInstructionInfo *source,
                       NativePlan *plan) {
  const AstHirFunction *hir = ast_mir_function_source_hir(
      ast_mir_function_at(c->program->machine->mir, c->function));
  memset(plan, 0, sizeof(*plan));
  plan->context = c;
  plan->region = ast_hir_function_native_region(hir, source->native_region);
  if (!plan->region || !ast_hir_native_region_info(plan->region, &plan->info) ||
      plan->info.input_count > 14u || plan->info.output_count > 14u)
    return 0;
  for (uint32_t i = 0; i < plan->info.statement_count; ++i) {
    AstHirNativeStmtInfo stmt;
    if (!ast_hir_native_region_statement(plan->region, i, &stmt))
      return 0;
    if (stmt.kind == AST_HIR_NATIVE_BYTES)
      continue;
    if (stmt.kind != AST_HIR_NATIVE_INSTRUCTION)
      return 0;
    const SystemForm *system =
        system_source_form(c->program->target.arch, &stmt);
    if (system && system->shape == SYSTEM_SHAPE_IMPLICIT)
      for (uint32_t r = 0; r < 4u; ++r)
        if ((system->definitions | system->uses) & (1u << r))
          plan->hard[64u + r] = 1u;
    for (uint32_t o = 0; o < stmt.operand_count; ++o)
      if (!scan_hard_registers(plan, stmt.operands[o].is_memory
                                         ? stmt.operands[o].memory_address
                                         : stmt.operands[o].value))
        return 0;
  }
  return 1;
}
static NativeRegionState *region_state(PhysicalContext *c, uint32_t region) {
  if (!c->fn->native_state) {
    const AstHirFunction *hir = ast_mir_function_source_hir(
        ast_mir_function_at(c->program->machine->mir, c->function));
    struct NativeLowerState *state =
        (struct NativeLowerState *)calloc(1u, sizeof(*state));
    if (!state)
      return NULL;
    state->region_count = ast_hir_function_native_region_count(hir);
    state->regions = (NativeRegionState *)calloc(
        state->region_count ? state->region_count : 1u,
        sizeof(*state->regions));
    if (!state->regions) {
      free(state);
      return NULL;
    }
    c->fn->native_state = state;
  }
  return region < c->fn->native_state->region_count
             ? &c->fn->native_state->regions[region]
             : NULL;
}
void physical_native_release(AllocationFunction *fn) {
  if (!fn || !fn->native_state)
    return;
  for (uint32_t i = 0; i < fn->native_state->region_count; ++i)
    free(fn->native_state->regions[i].outputs);
  free(fn->native_state->regions);
  free(fn->native_state);
  fn->native_state = NULL;
}
static int native_result(PhysicalContext *c, const MachineInstruction *mi,
                         const AstMirInstructionInfo *source) {
  NativeRegionState *state = region_state(c, source->native_region);
  if (!state || !state->lowered || source->aux_index >= state->output_count ||
      mi->info.result_count != 1u)
    return 0;
  AstPhysicalOperand home = physical_home(c, mi->info.result_first);
  if (!home.kind)
    return 1;
  NativeOutput *out = &state->outputs[source->aux_index];
  return home.bit_width == out->bits &&
         native_move(c, home, physical_frame(out->object, out->bits, 0));
}
static int prepare_bindings(NativePlan *plan, const MachineInstruction *mi,
                            const AstMirInstructionInfo *source) {
  PhysicalContext *c = plan->context;
  for (uint32_t j = 0; j < plan->info.input_count; ++j) {
    if (j >= mi->info.operand_count ||
        mi->info.operands[j].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    uint32_t v = mi->info.operands[j].as.vreg;
    if (v >= c->machine->info.vreg_count)
      return 0;
    AstMachineRegBank bank = c->machine->vregs[v].bank;
    uint32_t bits = c->machine->vregs[v].bit_width;
    if (!((bank == AST_MACHINE_BANK_GPR && scalar_bits(bits)) ||
          (bank == AST_MACHINE_BANK_VECTOR && bits == 128u &&
           c->program->target.arch != AST_TARGET_ARCH_RISCV64)))
      return 0;
    plan->inputs[j] = allocate_binding(plan, bank, bits);
    if (!plan->inputs[j].kind)
      return 0;
  }
  for (uint32_t j = 0; j < plan->info.output_count; ++j) {
    for (uint32_t k = 0; k < c->machine->info.instruction_count; ++k) {
      const MachineInstruction *result = &c->machine->instructions[k];
      AstMirInstructionInfo rs = {0};
      if (result->info.source_op != AST_MIR_OP_NATIVE_RESULT ||
          !source_info(c, result, &rs) ||
          rs.native_region != source->native_region || rs.aux_index != j)
        continue;
      if (result->info.result_count != 1u ||
          result->info.result_first >= c->machine->info.vreg_count)
        return 0;
      const AstMachineVRegInfo *value =
          &c->machine->vregs[result->info.result_first];
      if (!((value->bank == AST_MACHINE_BANK_GPR &&
             scalar_bits(value->bit_width)) ||
            (value->bank == AST_MACHINE_BANK_VECTOR &&
             value->bit_width == 128u &&
             c->program->target.arch != AST_TARGET_ARCH_RISCV64)))
        return 0;
      plan->outputs[j] = allocate_binding(plan, value->bank, value->bit_width);
    }
    if (!plan->outputs[j].kind)
      return 0;
  }
  return 1;
}
static int native_inputs(PhysicalContext *c, PhysicalCopy *copies,
                         uint32_t count) {
  if (c->program->target.arch == AST_TARGET_ARCH_X86_64)
    return physical_parallel_copy(c, copies, count);
  uint8_t active[14] = {0};
  uint32_t remaining = count;
  if (count > ASTER_ARRAY_COUNT(active))
    return 0;
  for (uint32_t j = 0; j < count; ++j)
    active[j] = 1u;
  while (remaining) {
    int progress = 0;
    for (uint32_t j = 0; j < count; ++j)
      if (active[j]) {
        int needed = 0;
        for (uint32_t k = 0; k < count; ++k)
          if (k != j && active[k] &&
              copies[k].src.kind == AST_PHYSICAL_OPERAND_REGISTER &&
              copies[k].src.register_unit == copies[j].dst.register_unit)
            needed = 1;
        if (needed)
          continue;
        if (!native_move(c, copies[j].dst, copies[j].src))
          return 0;
        active[j] = 0u;
        --remaining;
        progress = 1;
      }
    if (progress)
      continue;
    uint32_t j = 0u;
    while (j < count && !active[j])
      ++j;
    if (j == count || copies[j].src.kind != AST_PHYSICAL_OPERAND_REGISTER)
      return 0;
    AstPhysicalOperand saved = copies[j].src;
    uint32_t object = physical_temp_object(c, saved.bit_width / 8u,
                                           AST_FRAME_OBJECT_ABI_TEMP);
    if (object == AST_MACHINE_INVALID_ID ||
        !native_move(c, physical_frame(object, saved.bit_width, 0), saved))
      return 0;
    for (uint32_t k = 0; k < count; ++k)
      if (active[k] && copies[k].src.kind == AST_PHYSICAL_OPERAND_REGISTER &&
          copies[k].src.register_unit == saved.register_unit)
        copies[k].src = physical_frame(object, copies[k].src.bit_width, 0);
  }
  return 1;
}
static int normalize_inputs(NativePlan *plan) {
  PhysicalContext *c = plan->context;
  if (c->program->target.arch == AST_TARGET_ARCH_X86_64)
    return 1;
  for (uint32_t j = 0; j < plan->info.input_count; ++j) {
    const AstPhysicalOperand *input = &plan->inputs[j];
    if (input->bank != AST_MACHINE_BANK_GPR)
      continue;
    uint32_t bits =
        c->program->target.arch == AST_TARGET_ARCH_AARCH64 ? 32u : 64u;
    if (input->bit_width >= bits)
      continue;
    AstPhysicalOperand r = physical_register(c, AST_MACHINE_BANK_GPR,
                                             input->register_unit % 64u, bits);
    if (bits == 32u) {
      AstPhysicalOperand args[] = {
          r, r, physical_immediate(0u, 64u),
          physical_immediate(input->bit_width - 1u, 64u)};
      if (!physical_emit(c, AST_MACHINE_A64_UBFM, 1u, args, 4u))
        return 0;
    } else {
      AstPhysicalOperand args[] = {
          r, r, physical_immediate(64u - input->bit_width, 64u)};
      if (!physical_emit(c, AST_MACHINE_RISCV_SLLI, 1u, args, 3u) ||
          !physical_emit(c, AST_MACHINE_RISCV_SRLI, 1u, args, 3u))
        return 0;
    }
  }
  return 1;
}
int physical_native_instruction(PhysicalContext *c,
                                const MachineInstruction *mi) {
  AstMirInstructionInfo source = {0};
  if (!source_info(c, mi, &source))
    return native_fail(c, "Native instruction has no source region.");
  if (mi->info.source_op == AST_MIR_OP_NATIVE_RESULT)
    return native_result(c, mi, &source) ||
           native_fail(c, "Native output has no matching completed island.");
  if (mi->info.source_op != AST_MIR_OP_NATIVE)
    return native_fail(
        c, "Bind must be enforced by the allocator before physical lowering.");
  NativePlan plan;
  if (!region_plan(c, &source, &plan) ||
      plan.info.input_count != mi->info.operand_count)
    return native_fail(c, "Native region needs supported scalar bindings and "
                          "explicit register operands.");
  NativeRegionState *state = region_state(c, source.native_region);
  if (!state || state->lowered)
    return native_fail(c, "Native region cannot be materialized twice.");
  state->output_count = plan.info.output_count;
  state->outputs = (NativeOutput *)calloc(
      state->output_count ? state->output_count : 1u, sizeof(*state->outputs));
  if (!state->outputs)
    return 0;
  if (!prepare_bindings(&plan, mi, &source))
    return native_fail(c, "Native binding width, register class or register "
                          "pressure is unsupported.");
  for (uint32_t i = 0; i < plan.info.output_count; ++i) {
    uint32_t bits = plan.outputs[i].bit_width;
    state->outputs[i].bits = bits;
    state->outputs[i].object =
        physical_temp_object(c, bits / 8u, AST_FRAME_OBJECT_ABI_TEMP);
    if (state->outputs[i].object == AST_MACHINE_INVALID_ID)
      return 0;
  }
  if (!statements(&plan, 0))
    return native_form_fail(&plan,
                            "Native form has unsupported operands, effects, "
                            "attributes, or an output read before definition.");
  uint32_t saves[MACHINE_REGISTER_UNITS];
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    saves[unit] = AST_MACHINE_INVALID_ID;
    if (!plan.written[unit])
      continue;
    MachinePhysRegFile file = (MachinePhysRegFile)(unit / 64u);
    uint32_t index = unit % 64u;
    AstMachineRegBank bank;
    uint32_t bits;
    if (file == MACHINE_PHYSREG_FILE_GPR &&
        index <
            (c->program->target.arch == AST_TARGET_ARCH_X86_64 ? 16u : 32u)) {
      bank = AST_MACHINE_BANK_GPR;
      bits = 64u;
    } else if (file == MACHINE_PHYSREG_FILE_FP_VECTOR && index < 32u) {
      bank = AST_MACHINE_BANK_VECTOR;
      bits = plan.preserve_bits[unit] > 128u ? plan.preserve_bits[unit] : 128u;
    } else
      return native_fail(c, "Native island attempted to preserve an "
                            "unsupported physical register class.");
    saves[unit] =
        physical_temp_object(c, bits / 8u, AST_FRAME_OBJECT_EMERGENCY);
    if (saves[unit] == AST_MACHINE_INVALID_ID ||
        !native_move(c, physical_frame(saves[unit], bits, 0),
                     physical_register(c, bank, index, bits)))
      return 0;
  }
  PhysicalCopy inputs[14];
  for (uint32_t i = 0; i < plan.info.input_count; ++i) {
    inputs[i].dst = plan.inputs[i];
    inputs[i].src = physical_home(c, mi->info.operands[i].as.vreg);
  }
  if (!native_inputs(c, inputs, plan.info.input_count) ||
      !normalize_inputs(&plan) || !statements(&plan, 1))
    return 0;
  for (uint32_t i = 0; i < plan.info.output_count; ++i)
    if (!native_move(
            c,
            physical_frame(state->outputs[i].object, state->outputs[i].bits, 0),
            plan.outputs[i]))
      return 0;
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit)
    if (saves[unit] != AST_MACHINE_INVALID_ID) {
      MachinePhysRegFile file = (MachinePhysRegFile)(unit / 64u);
      uint32_t index = unit % 64u;
      AstMachineRegBank bank = file == MACHINE_PHYSREG_FILE_GPR
                                   ? AST_MACHINE_BANK_GPR
                                   : AST_MACHINE_BANK_VECTOR;
      uint32_t bits =
          file == MACHINE_PHYSREG_FILE_GPR
              ? 64u
              : (plan.preserve_bits[unit] > 128u ? plan.preserve_bits[unit]
                                                 : 128u);
      if (!native_move(c, physical_register(c, bank, index, bits),
                       physical_frame(saves[unit], bits, 0)))
        return 0;
    }
  state->lowered = 1u;
  return 1;
}

int physical_verify_native_other_region(const AstRegAllocProgram *p,
                                        uint32_t function, uint32_t instruction,
                                        const PhysicalBlock *block,
                                        uint32_t begin, uint32_t start,
                                        uint32_t end, uint32_t tail) {
  if (function >= p->function_count ||
      instruction >= p->machine->functions[function].info.instruction_count)
    return 0;
  PhysicalContext context = {0};
  NativePlan plan;
  AstMirInstructionInfo source;
  context.program = (AstRegAllocProgram *)p;
  context.function = function;
  context.instruction = instruction;
  context.fn = &context.program->functions[function];
  context.machine = &p->machine->functions[function];
  const MachineInstruction *mi = &context.machine->instructions[instruction];
  if (!source_info(&context, mi, &source) ||
      !region_plan(&context, &source, &plan) || start > end || end > tail ||
      tail > block->count || begin > start ||
      end - start != plan.info.statement_count ||
      !prepare_bindings(&plan, mi, &source))
    return 0;
  for (uint32_t j = 0; j < plan.info.statement_count; ++j) {
    AstHirNativeStmtInfo s;
    AstPhysicalInstructionInfo expected;
    const AstPhysicalInstructionInfo *actual = &block->instructions[start + j];
    NativeBindings bindings = {plan.inputs, plan.outputs, plan.output_defined,
                               plan.info.input_count, plan.info.output_count};
    if (!ast_hir_native_region_statement(plan.region, j, &s) ||
        !physical_native_other_info(&p->target, &s, &bindings, &expected))
      return 0;
    const AstHirNativeOperandInfo *dst = role(&s, "dst");
    if (dst)
      define_output(&plan, dst);
    if (!physical_native_packet_equal(&expected, actual))
      return 0;
  }
  NativeBindings bindings = {plan.inputs, plan.outputs, plan.output_defined,
                             plan.info.input_count, plan.info.output_count};
  return physical_verify_native_transfers(&context, &source, &bindings, block,
                                          begin, start, end, tail);
}

/* Rebuild x86 packets from immutable HIR and allocation, without consulting
   the lowering's output cache or its mutable statement plan. */
int physical_verify_native_x86_region(const AstRegAllocProgram *p,
                                      uint32_t function, uint32_t instruction,
                                      const PhysicalBlock *block,
                                      uint32_t begin, uint32_t start,
                                      uint32_t end, uint32_t tail, int manual) {
  if (p->target.arch != AST_TARGET_ARCH_X86_64 ||
      function >= p->function_count ||
      instruction >= p->machine->functions[function].info.instruction_count ||
      start > end || end > block->count)
    return 0;
  PhysicalContext context = {0};
  NativePlan plan;
  AstMirInstructionInfo source;
  AstMirFunctionInfo fi;
  context.program = (AstRegAllocProgram *)p;
  context.function = function;
  context.instruction = instruction;
  context.fn = &context.program->functions[function];
  context.machine = &p->machine->functions[function];
  const MachineInstruction *mi = &context.machine->instructions[instruction];
  if (!source_info(&context, mi, &source) ||
      !region_plan(&context, &source, &plan) ||
      end - start != plan.info.statement_count ||
      !ast_mir_function_info(ast_mir_function_at(p->machine->mir, function),
                             &fi))
    return 0;
  plan.raw = manual;
  plan.allow_raw_bytes = manual && ((fi.flags & AST_HIR_FUNCTION_NAKED) ||
                                    fi.frame_policy == AST_HIR_FRAME_NAKED);
  if (manual ? plan.info.input_count || plan.info.output_count
             : !prepare_bindings(&plan, mi, &source))
    return 0;
  for (uint32_t j = 0; j < plan.info.statement_count; ++j) {
    AstHirNativeStmtInfo s;
    AstPhysicalInstructionInfo expected;
    const AstPhysicalInstructionInfo *actual = &block->instructions[start + j];
    if (!ast_hir_native_region_statement(plan.region, j, &s))
      return 0;
    if (vector_statement_form(p->target.arch, &s)) {
      if (plan.terminated ||
          !physical_native_vector_info(&p->target, &s, manual, &expected))
        return 0;
    } else if (system_source_form(p->target.arch, &s)) {
      if (plan.terminated ||
          !physical_native_system_info(&p->target, &s, &expected))
        return 0;
    } else if (!statement(&plan, &s, 0, &expected))
      return 0;
    if (s.kind == AST_HIR_NATIVE_BYTES) {
      if (actual->opcode != expected.opcode || actual->operand_count ||
          actual->result_count || !actual->raw_bytes ||
          actual->raw_byte_count != expected.raw_byte_count ||
          memcmp(actual->raw_bytes, expected.raw_bytes,
                 expected.raw_byte_count))
        return 0;
    } else if (!physical_native_packet_equal(&expected, actual))
      return 0;
  }
  for (uint32_t j = 0; j < plan.info.output_count; ++j)
    if (!plan.output_defined[j])
      return 0;
  NativeBindings bindings = {plan.inputs, plan.outputs, plan.output_defined,
                             plan.info.input_count, plan.info.output_count};
  return manual ||
         physical_verify_native_transfers(&context, &source, &bindings, block,
                                          begin, start, end, tail);
}

int physical_native_function_policy(PhysicalContext *c, int *handled) {
  AstMirFunctionInfo info = {0};
  *handled = 0;
  if (!ast_mir_function_info(
          ast_mir_function_at(c->program->machine->mir, c->function), &info))
    return 0;
  if (!(info.flags & AST_HIR_FUNCTION_NAKED) &&
      info.frame_policy != AST_HIR_FRAME_NAKED &&
      info.frame_policy != AST_HIR_FRAME_VALIDATE)
    return 1;
  *handled = 1;
  if (info.stack_slot_count || c->fn->object_count ||
      c->machine->info.block_count != 1u || info.stack_alignment > 16u)
    return native_fail(c,
                       "Manual native functions require a supported explicit "
                       "frame; automatic stack objects are forbidden.");
  /* Share the ordinary frame-ownership path. The old manually malloc'ed
     callee_save_objects array was never owned by register_object_storage,
     and leaked on both successful and failing @naked functions. */
  if (!frame_init_register_objects(c->fn))
    return 0;
  c->fn->blocks = (PhysicalBlock *)calloc(1u, sizeof(*c->fn->blocks));
  if (!c->fn->blocks)
    return 0;
  c->fn->block_count = c->fn->block_capacity = 1u;
  c->fn->frame.alignment = 16u;
  c->block = 0u;
  int returned = 0;
  const MachineBlock *block = &c->machine->blocks[0];
  for (uint32_t j = 0; j < block->info.instruction_count; ++j) {
    c->instruction = block->instructions[j];
    const MachineInstruction *mi = &c->machine->instructions[c->instruction];
    if ((mi->info.opcode == AST_MACHINE_X86_TRAP ||
         mi->info.opcode == AST_MACHINE_A64_TRAP ||
         mi->info.opcode == AST_MACHINE_RISCV_EBREAK) &&
        j + 1u == block->info.instruction_count && returned)
      continue;
    if ((mi->info.opcode == AST_MACHINE_X86_RET ||
         mi->info.opcode == AST_MACHINE_A64_RET ||
         mi->info.opcode == AST_MACHINE_RISCV_RET) &&
        !mi->info.operand_count && j + 1u == block->info.instruction_count &&
        returned)
      continue;
    AstMirInstructionInfo source = {0};
    NativePlan plan;
    if (returned || mi->info.source_op != AST_MIR_OP_NATIVE ||
        !source_info(c, mi, &source) || !region_plan(c, &source, &plan) ||
        plan.info.input_count || plan.info.output_count)
      return native_fail(c, "Manual native function contains semantic "
                            "operations or an implicit return.");
    plan.raw = 1;
    if (c->program->target.arch != AST_TARGET_ARCH_X86_64 &&
        plan.info.statement_count > 4096u)
      return native_fail(
          c,
          "Manual ARM64/RV64 region exceeds the bounded CFG validation limit.");
    /* Opaque bytes bypass instruction semantics and are therefore limited
       to truly naked functions. @frame(validate) still validates explicit
       instructions but may not hide arbitrary stack/register effects. */
    plan.allow_raw_bytes = c->program->target.arch == AST_TARGET_ARCH_X86_64 &&
                           ((info.flags & AST_HIR_FUNCTION_NAKED) ||
                            info.frame_policy == AST_HIR_FRAME_NAKED);
    if (!statements(&plan, 0))
      return native_form_fail(&plan, "Manual native function uses an "
                                     "unsupported explicit instruction form.");
    if (!statements(&plan, 1))
      return 0;
    returned = plan.terminated;
  }
  return returned || native_fail(c, "Manual native function needs an explicit "
                                    "return or exception-return instruction.");
}
