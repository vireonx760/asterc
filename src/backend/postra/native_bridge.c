#include "../../isa/aarch64_data_contract.h"
#include "../../isa/aarch64_scalar_contract.h"
#include "../../isa/riscv64_memory_contract.h"
#include "../../isa/riscv64_scalar_contract.h"
#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "physical_internal.h"
#include <ctype.h>

static int value(const AstTargetConfig *target,
                 const AstHirNativeOperandInfo *source,
                 const NativeBindings *bindings, int writing,
                 AstPhysicalOperand *out) {
  AstHirNativeExprInfo e;
  if (!source || source->is_memory ||
      !ast_hir_native_expr_info(source->value, &e))
    return 0;
  if (e.kind != AST_HIR_NATIVE_EXPR_NAME) {
    int64_t number;
    if (writing || !physical_native_vector_integer(source->value, &number))
      return 0;
    *out = physical_immediate((uint64_t)number, 64u);
    return 1;
  }
  if (e.binding_kind == AST_HIR_NATIVE_BINDING_INPUT) {
    if (!bindings || writing || e.binding_index >= bindings->input_count)
      return 0;
    *out = bindings->inputs[e.binding_index];
  } else if (e.binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT) {
    if (!bindings || e.binding_index >= bindings->output_count ||
        (!writing && !bindings->defined[e.binding_index]))
      return 0;
    *out = bindings->outputs[e.binding_index];
  } else {
    MachinePhysRegView view;
    char name[AST_MACHINE_REGISTER_NAME_CAP];
    if (e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !e.name.text ||
        !e.name.span.length || e.name.span.length >= sizeof(name))
      return 0;
    for (uint32_t j = 0; j < e.name.span.length; ++j)
      name[j] = (char)tolower((unsigned char)e.name.text[j]);
    name[e.name.span.length] = 0;
    if (!machine_physreg_view(target, name, &view) || view.special ||
        (view.file != MACHINE_PHYSREG_FILE_GPR &&
         view.file != MACHINE_PHYSREG_FILE_FP_VECTOR))
      return 0;
    if (view.file == MACHINE_PHYSREG_FILE_GPR &&
        (target->arch == AST_TARGET_ARCH_AARCH64
             ? view.index == 18u || (writing && view.index == 30u)
             : (writing && view.index >= 1u && view.index <= 4u)))
      return 0;
    if (view.file == MACHINE_PHYSREG_FILE_FP_VECTOR &&
        (target->arch != AST_TARGET_ARCH_AARCH64 || view.width != 128u))
      return 0;
    memset(out, 0, sizeof(*out));
    out->kind = AST_PHYSICAL_OPERAND_REGISTER;
    out->bank = view.file == MACHINE_PHYSREG_FILE_GPR ? AST_MACHINE_BANK_GPR
                                                      : AST_MACHINE_BANK_VECTOR;
    out->bit_width = view.width;
    out->register_unit = view.alias_unit;
    memcpy(out->register_name, name, e.name.span.length + 1u);
  }
  if (out->bank == AST_MACHINE_BANK_GPR) {
    uint32_t bits =
        target->arch == AST_TARGET_ARCH_AARCH64 && out->bit_width <= 32u ? 32u
                                                                         : 64u;
    if (!machine_physreg_name(target, out->bank, out->register_unit % 64u, bits,
                              out->register_name))
      return 0;
    out->bit_width = bits;
  }
  return 1;
}

static int packet_name_equal(AstName a, AstName b) {
  return a.span.length == b.span.length &&
         (!a.span.length ||
          (a.text && b.text && !memcmp(a.text, b.text, a.span.length)));
}
int physical_native_packet_equal(const AstPhysicalInstructionInfo *a,
                                 const AstPhysicalInstructionInfo *b) {
  if (a->opcode != b->opcode || a->result_count != b->result_count ||
      a->operand_count != b->operand_count ||
      a->operand_count > AST_PHYSICAL_MAX_OPERANDS || a->raw_bytes ||
      a->raw_byte_count || b->raw_bytes || b->raw_byte_count)
    return 0;
  for (uint32_t j = 0; j < a->operand_count; ++j) {
    const AstPhysicalOperand *x = &a->operands[j], *y = &b->operands[j];
    if (x->kind != y->kind || x->bank != y->bank ||
        x->bit_width != y->bit_width || x->register_unit != y->register_unit ||
        x->immediate_bits != y->immediate_bits ||
        x->displacement != y->displacement ||
        x->index_register_unit != y->index_register_unit ||
        x->scale != y->scale || x->block != y->block ||
        !packet_name_equal(x->module, y->module) ||
        !packet_name_equal(x->symbol, y->symbol) ||
        memcmp(x->register_name, y->register_name, sizeof(x->register_name)) ||
        memcmp(x->index_register_name, y->index_register_name,
               sizeof(x->index_register_name)))
      return 0;
  }
  return 1;
}

int physical_native_scalar_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s,
                                const NativeBindings *bindings,
                                AstPhysicalInstructionInfo *out) {
  const RvScalarForm *form =
      s ? rv_scalar_source(target->arch, s->mnemonic) : NULL;
  if (!form || (form->feature & ~target->features) ||
      s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count ||
      s->operand_count !=
          (form->shape == RV_SCALAR_U || form->shape == RV_SCALAR_N ? 2u : 3u))
    return 0;
  memset(out, 0, sizeof(*out));
  out->opcode = form->opcode;
  out->result_count = 1u;
  out->operand_count = s->operand_count;
  if (!value(target, system_role(s, "dst"), bindings, 1, &out->operands[0]))
    return 0;
  if (form->shape == RV_SCALAR_U) {
    if (!value(target, system_role(s, "imm"), bindings, 0, &out->operands[1]))
      return 0;
  } else if (form->shape == RV_SCALAR_N) {
    if (!value(target, system_role(s, "src"), bindings, 0, &out->operands[1]))
      return 0;
  } else if (!value(target, system_role(s, "lhs"), bindings, 0,
                    &out->operands[1]) ||
             !value(target, system_role(s, "rhs"), bindings, 0,
                    &out->operands[2]))
    return 0;
  return rv_scalar_contract(out);
}

int physical_native_memory_info(const AstTargetConfig *target,
                                const AstHirNativeStmtInfo *s,
                                const NativeBindings *bindings, int manual,
                                AstPhysicalInstructionInfo *out) {
  const RvMemoryForm *f =
      s ? rv_memory_source(target->arch, s->mnemonic) : NULL;
  if (!f || s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count)
    return 0;
  const AstHirNativeOperandInfo *base = system_role(s, "base"),
                                *offset = system_role(s, "offset");
  const AstHirNativeOperandInfo *reg = system_role(s, f->load ? "dst" : "src");
  if (s->operand_count != (base ? 3u : 2u) || (!base && !manual) || !offset ||
      !reg)
    return 0;
  AstPhysicalOperand address = {0}, displacement;
  if (base) {
    if (!value(target, base, bindings, 0, &address) ||
        !rv_scalar_register(&address))
      return 0;
  } else {
    address.kind = AST_PHYSICAL_OPERAND_REGISTER;
    address.bank = AST_MACHINE_BANK_GPR;
    address.bit_width = 64u;
    address.register_unit = 66u;
    memcpy(address.register_name, "sp", 3u);
  }
  if (!value(target, offset, bindings, 0, &displacement) ||
      displacement.kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
      (displacement.immediate_bits > 2047u &&
       displacement.immediate_bits < UINT64_C(0xfffffffffffff800)))
    return 0;
  if (address.register_unit == 66u &&
      (!manual || displacement.immediate_bits > 2047u ||
       displacement.immediate_bits % (f->bits / 8u)))
    return 0;
  memset(out, 0, sizeof(*out));
  out->opcode = f->opcode;
  out->result_count = f->load;
  out->operand_count = 2u;
  if (!value(target, reg, bindings, f->load, &out->operands[0])) {
    /* Only a tracked, full-width stack reload may restore incoming RA. */
    if (!manual || !f->load || f->bits != 64u || address.register_unit != 66u ||
        !value(target, reg, bindings, 0, &out->operands[0]) ||
        out->operands[0].register_unit != 65u)
      return 0;
  }
  address.kind = AST_PHYSICAL_OPERAND_MEMORY;
  address.bit_width = f->bits;
  uint64_t raw = displacement.immediate_bits;
  address.displacement = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
  out->operands[1] = address;
  return rv_memory_contract(out);
}

int physical_native_indirect_info(const AstTargetConfig *target,
                                  const AstHirNativeStmtInfo *s,
                                  AstPhysicalInstructionInfo *out) {
  if (target->arch != AST_TARGET_ARCH_RISCV64 || !s ||
      s->kind != AST_HIR_NATIVE_INSTRUCTION ||
      !system_name(s->mnemonic, "jalr") || s->attribute_count ||
      s->operand_count != 3u)
    return 0;
  memset(out, 0, sizeof(*out));
  out->opcode = AST_MACHINE_RISCV_NATIVE_JALR;
  out->operand_count = 3u;
  if (!value(target, system_role(s, "dst"), NULL, 0, &out->operands[0]) ||
      !value(target, system_role(s, "base"), NULL, 0, &out->operands[1]) ||
      !value(target, system_role(s, "offset"), NULL, 0, &out->operands[2]) ||
      !rv_scalar_register(&out->operands[0]) ||
      !rv_scalar_register(&out->operands[1]) ||
      (out->operands[0].register_unit != 64u &&
       out->operands[0].register_unit != 65u) ||
      out->operands[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE)
    return 0;
  uint64_t offset = out->operands[2].immediate_bits;
  return offset <= 2047u || offset >= UINT64_C(0xfffffffffffff800);
}

static int bound(const AstHirNativeOperandInfo *o) {
  AstHirNativeExprInfo e;
  return o && !o->is_memory && ast_hir_native_expr_info(o->value, &e) &&
         e.kind == AST_HIR_NATIVE_EXPR_NAME &&
         (e.binding_kind == AST_HIR_NATIVE_BINDING_INPUT ||
          e.binding_kind == AST_HIR_NATIVE_BINDING_OUTPUT);
}

static int widths(const AstTargetConfig *target,
                  const AstHirNativeOperandInfo *const *source,
                  AstPhysicalOperand *o, uint32_t count) {
  uint32_t bits = 0u;
  for (uint32_t j = 0; j < count; ++j)
    if (o[j].kind == AST_PHYSICAL_OPERAND_REGISTER && !bound(source[j])) {
      if (bits && bits != o[j].bit_width)
        return 0;
      bits = o[j].bit_width;
    }
  if (!bits)
    for (uint32_t j = 0; j < count; ++j)
      if (o[j].kind == AST_PHYSICAL_OPERAND_REGISTER && o[j].bit_width > bits)
        bits = o[j].bit_width;
  for (uint32_t j = 0; j < count; ++j) {
    if (o[j].kind != AST_PHYSICAL_OPERAND_REGISTER)
      continue;
    if (!bound(source[j]) && o[j].bit_width != bits)
      return 0;
    if (o[j].bank != o[0].bank ||
        !machine_physreg_name(target, o[j].bank, o[j].register_unit % 64u, bits,
                              o[j].register_name))
      return 0;
    o[j].bit_width = bits;
  }
  return bits != 0u;
}

static int a64_scalar_value(const AstTargetConfig *target,
                            const AstHirNativeOperandInfo *source,
                            const NativeBindings *bindings, int writing,
                            AstPhysicalOperand *out) {
  AstHirNativeExprInfo e;
  if (source && !source->is_memory &&
      ast_hir_native_expr_info(source->value, &e) &&
      e.kind == AST_HIR_NATIVE_EXPR_NAME &&
      e.binding_kind == AST_HIR_NATIVE_BINDING_TARGET &&
      (system_name(e.name, "xzr") || system_name(e.name, "wzr"))) {
    memset(out, 0, sizeof(*out));
    out->kind = AST_PHYSICAL_OPERAND_REGISTER;
    out->bank = AST_MACHINE_BANK_SPECIAL;
    out->register_unit = 321u;
    out->bit_width = system_name(e.name, "wzr") ? 32u : 64u;
    memcpy(out->register_name, out->bit_width == 32u ? "wzr" : "xzr", 4u);
    return 1;
  }
  return value(target, source, bindings, writing, out);
}
const A64DataForm *
physical_native_a64_data_form(const AstTargetConfig *target,
                              const AstHirNativeStmtInfo *s) {
  if (!s || target->arch != AST_TARGET_ARCH_AARCH64)
    return NULL;
  if (system_name(s->mnemonic, "cmp"))
    return &a64_data_forms[A64_DATA_ID_SUBS];
  if (system_name(s->mnemonic, "cmn"))
    return &a64_data_forms[A64_DATA_ID_ADDS];
  const A64DataForm *f = a64_data_source(target->arch, s->mnemonic);
  if (!f)
    return NULL;
  if (f->shape == A64_DATA_SELECT || f->shape == A64_DATA_COMPARE ||
      system_role(s, "shift") || system_role(s, "extend") ||
      system_role(s, "amount"))
    return f;
  /* Keep existing default packets. Select the expanded form only when its
     operands require it, including literal masks, ZR and read-only SP. */
  const AstHirNativeOperandInfo *rhs = system_role(s, "rhs");
  AstHirNativeExprInfo e;
  if (f->shape == A64_DATA_LOGIC && rhs && !rhs->is_memory &&
      ast_hir_native_expr_info(rhs->value, &e) &&
      e.kind != AST_HIR_NATIVE_EXPR_NAME)
    return f;
  if (f->shape == A64_DATA_ADD &&
      !a64_scalar_source(target->arch, s->mnemonic) && rhs && !rhs->is_memory &&
      ast_hir_native_expr_info(rhs->value, &e) &&
      e.kind != AST_HIR_NATIVE_EXPR_NAME) {
    const AstHirNativeOperandInfo *dst = system_role(s, "dst");
    AstHirNativeExprInfo d;
    if (dst && ast_hir_native_expr_info(dst->value, &d) &&
        (system_name(d.name, "sp") || system_name(d.name, "wsp")))
      return NULL;
    return f;
  }
  for (uint32_t j = 0; j < s->operand_count; ++j)
    if (!s->operands[j].is_memory &&
        ast_hir_native_expr_info(s->operands[j].value, &e) &&
        e.binding_kind == AST_HIR_NATIVE_BINDING_TARGET &&
        (system_name(e.name, "xzr") || system_name(e.name, "wzr") ||
         system_name(e.name, "sp") || system_name(e.name, "wsp"))) {
      if (system_name(e.name, "sp") || system_name(e.name, "wsp"))
        return f;
      /* R34's scalar family already models ZR for its original forms. */
      if (!a64_scalar_source(target->arch, s->mnemonic))
        return f;
    }
  return NULL;
}
static int data_selector(const AstHirNativeOperandInfo *o,
                         const char *const *names, uint32_t count,
                         uint32_t *out) {
  if (!o || o->is_memory)
    return 0;
  int64_t integer;
  if (physical_native_vector_integer(o->value, &integer)) {
    if (integer < 0 || (uint64_t)integer >= count)
      return 0;
    *out = (uint32_t)integer;
    return 1;
  }
  AstHirNativeExprInfo e;
  if (!ast_hir_native_expr_info(o->value, &e) ||
      e.kind != AST_HIR_NATIVE_EXPR_NAME ||
      e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET)
    return 0;
  for (uint32_t j = 0; j < count; ++j)
    if (system_name(e.name, names[j])) {
      *out = j;
      return 1;
    }
  if (count == 16u &&
      (system_name(e.name, "hs") || system_name(e.name, "lo"))) {
    *out = system_name(e.name, "hs") ? 2u : 3u;
    return 1;
  }
  return 0;
}
static int data_integer_bits(const AstHirNativeExpr *expr, uint32_t depth,
                             uint64_t *out) {
  AstHirNativeExprInfo e;
  if (depth > 16u || !expr || !ast_hir_native_expr_info(expr, &e))
    return 0;
  if (e.kind == AST_HIR_NATIVE_EXPR_CONSTANT && !e.constant.hi) {
    *out = e.constant.is_negative ? UINT64_C(0) - e.constant.lo : e.constant.lo;
    return 1;
  }
  if (e.kind == AST_HIR_NATIVE_EXPR_UNARY && e.child_count == 1u &&
      (e.token_kind == AST_TOK_PLUS || e.token_kind == AST_TOK_MINUS ||
       e.token_kind == AST_TOK_TILDE)) {
    if (!data_integer_bits(ast_hir_native_expr_child(expr, 0u), depth + 1u,
                           out))
      return 0;
    if (e.token_kind == AST_TOK_MINUS)
      *out = UINT64_C(0) - *out;
    if (e.token_kind == AST_TOK_TILDE)
      *out = ~*out;
    return 1;
  }
  if (e.kind != AST_HIR_NATIVE_EXPR_LITERAL ||
      e.token_kind != AST_TOK_INTEGER || !e.literal.data)
    return 0;
  uint32_t base = 10u, start = 0u, digits = 0u;
  uint64_t value = 0u;
  if (e.literal.length > 2u && e.literal.data[0] == '0') {
    int p = tolower((unsigned char)e.literal.data[1]);
    if (p == 'x') {
      base = 16u;
      start = 2u;
    }
    if (p == 'b') {
      base = 2u;
      start = 2u;
    }
    if (p == 'o') {
      base = 8u;
      start = 2u;
    }
  }
  for (uint32_t j = start; j < e.literal.length; ++j) {
    unsigned char c = (unsigned char)e.literal.data[j];
    if (c == '_')
      continue;
    uint32_t d = c >= '0' && c <= '9'   ? (uint32_t)(c - '0')
                 : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a') + 10u
                 : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A') + 10u
                                        : UINT32_MAX;
    if (d >= base || value > (UINT64_MAX - d) / base)
      return 0;
    value = value * base + d;
    ++digits;
  }
  *out = value;
  return digits != 0u;
}
int physical_native_integer_bits(const AstHirNativeExpr *expr, uint64_t *out) {
  return data_integer_bits(expr, 0u, out);
}
static int data_value(const AstTargetConfig *target,
                      const AstHirNativeOperandInfo *src,
                      const NativeBindings *bindings, int writing,
                      AstPhysicalOperand *out) {
  AstHirNativeExprInfo e;
  uint64_t literal;
  if (!writing && src && !src->is_memory &&
      data_integer_bits(src->value, 0u, &literal)) {
    *out = physical_immediate(literal, 64u);
    return 1;
  }
  if (src && !src->is_memory && ast_hir_native_expr_info(src->value, &e) &&
      e.kind == AST_HIR_NATIVE_EXPR_NAME &&
      e.binding_kind == AST_HIR_NATIVE_BINDING_TARGET &&
      (system_name(e.name, "sp") || system_name(e.name, "wsp"))) {
    if (writing)
      return 0;
    memset(out, 0, sizeof(*out));
    out->kind = AST_PHYSICAL_OPERAND_REGISTER;
    out->bank = AST_MACHINE_BANK_SPECIAL;
    out->register_unit = 320u;
    out->bit_width = system_name(e.name, "sp") ? 64u : 32u;
    memcpy(out->register_name, out->bit_width == 64u ? "sp" : "wsp",
           out->bit_width == 64u ? 3u : 4u);
    return 1;
  }
  return a64_scalar_value(target, src, bindings, writing, out);
}
int physical_native_a64_data_info(const AstTargetConfig *target,
                                  const AstHirNativeStmtInfo *s,
                                  const NativeBindings *bindings,
                                  AstPhysicalInstructionInfo *out) {
  const A64DataForm *f = physical_native_a64_data_form(target, s);
  if (!f || s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count)
    return 0;
  static const char *const shifts[] = {"lsl", "lsr", "asr", "ror"};
  static const char *const extends[] = {"uxtb", "uxth", "uxtw", "uxtx",
                                        "sxtb", "sxth", "sxtw", "sxtx"};
  static const char *const conditions[] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                           "vs", "vc", "hi", "ls", "ge", "lt",
                                           "gt", "le", "al", "nv"};
  int compare = f->shape == A64_DATA_COMPARE,
      alias =
          system_name(s->mnemonic, "cmp") || system_name(s->mnemonic, "cmn");
  const AstHirNativeOperandInfo *src[3] = {
      compare ? system_role(s, "lhs") : system_role(s, "dst"),
      system_role(s, compare ? "rhs" : "lhs"),
      compare ? NULL : system_role(s, "rhs")};
  uint32_t count = compare || f->shape == A64_DATA_SELECT ? 4u
                   : alias                                ? 2u
                                                          : 3u;
  uint32_t mode = 0u, amount = 0u, cc = 0u;
  const AstHirNativeOperandInfo *shift = system_role(s, "shift"),
                                *extend = system_role(s, "extend"),
                                *amt = system_role(s, "amount");
  if (shift || extend || amt) {
    if (compare || f->shape == A64_DATA_SELECT || (shift && extend) ||
        (!shift && !extend))
      return 0;
    if (!data_selector(extend ? extend : shift, extend ? extends : shifts,
                       extend ? 8u : 4u, &mode))
      return 0;
    if (extend)
      mode += A64_DATA_EXTEND;
    if (amt) {
      int64_t n;
      if (amt->is_memory || !physical_native_vector_integer(amt->value, &n) ||
          n < 0 || n >= 64)
        return 0;
      amount = (uint32_t)n;
    }
    count += 1u + (amt != NULL);
  }
  if (s->operand_count != count)
    return 0;
  memset(out, 0, sizeof(*out));
  out->opcode = AST_MACHINE_A64_DATA;
  out->result_count = !compare;
  out->operand_count = compare || f->shape == A64_DATA_SELECT ? 5u : 6u;
  AstPhysicalOperand *o = out->operands;
  if (alias) {
    o[0].kind = AST_PHYSICAL_OPERAND_REGISTER;
    o[0].bank = AST_MACHINE_BANK_SPECIAL;
    o[0].register_unit = 321u;
  } else if (!data_value(target, src[0], bindings, !compare, &o[0]))
    return 0;
  uint32_t registers = compare ? 2u : 3u;
  for (uint32_t j = 1u; j < registers; ++j)
    if (!data_value(target, src[j], bindings, 0, &o[j]))
      return 0;
  uint32_t bits = 0u;
  for (uint32_t j = alias ? 1u : 0u; j < registers; ++j) {
    if (o[j].kind != AST_PHYSICAL_OPERAND_REGISTER || bound(src[j]) ||
        (j == 2u && mode >= A64_DATA_EXTEND))
      continue;
    if (bits && bits != o[j].bit_width)
      return 0;
    bits = o[j].bit_width;
  }
  if (!bits)
    for (uint32_t j = 0; j < registers; ++j)
      if (o[j].kind == AST_PHYSICAL_OPERAND_REGISTER && o[j].bit_width > bits)
        bits = o[j].bit_width;
  if (bits != 32u && bits != 64u)
    return 0;
  for (uint32_t j = 0; j < registers; ++j)
    if (o[j].kind == AST_PHYSICAL_OPERAND_REGISTER) {
      uint32_t width = j == 2u && mode >= A64_DATA_EXTEND &&
                               ((mode - A64_DATA_EXTEND) & 3u) != 3u
                           ? 32u
                           : bits;
      if (!alias && !bound(src[j]) && o[j].bit_width != width)
        return 0;
      if (alias && j && !bound(src[j]) && o[j].bit_width != width)
        return 0;
      o[j].bit_width = width;
      if (o[j].register_unit == 321u)
        memcpy(o[j].register_name, width == 32u ? "wzr" : "xzr", 4u);
      else if (o[j].register_unit != 320u &&
               (o[j].bank != AST_MACHINE_BANK_GPR ||
                !machine_physreg_name(target, AST_MACHINE_BANK_GPR,
                                      o[j].register_unit % 64u, width,
                                      o[j].register_name)))
        return 0;
    }
  if (compare || f->shape == A64_DATA_SELECT) {
    if (!data_selector(system_role(s, "cond"), conditions, 16u, &cc))
      return 0;
    if (compare && !value(target, system_role(s, "nzcv"), bindings, 0, &o[2]))
      return 0;
    o[3] = physical_immediate(cc, 8u);
  } else {
    if (o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      if (shift || extend || amt)
        return 0;
      mode = f->shape == A64_DATA_ADD ? A64_DATA_IMMEDIATE : A64_DATA_MASK;
    }
    o[3] = physical_immediate(mode, 8u);
    o[4] = physical_immediate(amount, 8u);
  }
  o[out->operand_count - 1u] =
      physical_immediate((uint64_t)(f - a64_data_forms), 8u);
  return a64_data_contract(out);
}
int physical_native_a64_scalar_info(const AstTargetConfig *target,
                                    const AstHirNativeStmtInfo *s,
                                    const NativeBindings *bindings,
                                    AstPhysicalInstructionInfo *out) {
  const A64ScalarForm *f =
      s ? a64_scalar_source(target->arch, s->mnemonic) : NULL;
  if (!f || s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count ||
      s->operand_count != a64_scalar_operands(f))
    return 0;
  const AstHirNativeOperandInfo *src[4] = {system_role(s, "dst"), NULL, NULL,
                                           NULL};
  if (f->shape == A64_SCALAR_WIDE) {
    src[1] = system_role(s, "imm");
    src[2] = system_role(s, "shift");
  } else if (f->shape == A64_SCALAR_UNARY)
    src[1] = system_role(s, "src");
  else if (f->shape == A64_SCALAR_BITFIELD) {
    src[1] = system_role(s, "src");
    src[2] = system_role(s, "immr");
    src[3] = system_role(s, "imms");
  } else {
    src[1] = system_role(s, "lhs");
    src[2] = system_role(s, "rhs");
    if (f->shape == A64_SCALAR_FOUR || f->shape == A64_SCALAR_LONG)
      src[3] = system_role(s, "acc");
    if (f->shape == A64_SCALAR_EXTRACT)
      src[3] = system_role(s, "lsb");
  }
  AstPhysicalOperand old;
  if ((f->effects & A64_SCALAR_READ_DST) &&
      !a64_scalar_value(target, src[0], bindings, 0, &old))
    return 0;
  memset(out, 0, sizeof(*out));
  out->opcode = AST_MACHINE_A64_SCALAR;
  out->result_count = 1u;
  out->operand_count = s->operand_count + 1u;
  uint32_t bits = f->word32 ? 0u : 64u;
  for (uint32_t j = 0; j < s->operand_count; ++j) {
    AstPhysicalOperand *o = &out->operands[j];
    if (!a64_scalar_value(target, src[j], bindings, j == 0u, o))
      return 0;
    if (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
        f->shape == A64_SCALAR_LONG || bound(src[j]))
      continue;
    if (bits && bits != o->bit_width)
      return 0;
    bits = o->bit_width;
  }
  if (f->shape == A64_SCALAR_LONG)
    bits = 64u;
  if (!bits)
    for (uint32_t j = 0; j < s->operand_count; ++j)
      if (out->operands[j].kind == AST_PHYSICAL_OPERAND_REGISTER &&
          out->operands[j].bit_width > bits)
        bits = out->operands[j].bit_width;
  for (uint32_t j = 0; j < s->operand_count; ++j) {
    AstPhysicalOperand *o = &out->operands[j];
    if (o->kind != AST_PHYSICAL_OPERAND_REGISTER)
      continue;
    uint32_t width =
        f->shape == A64_SCALAR_LONG && (j == 1u || j == 2u) ? 32u : bits;
    if (!bound(src[j]) && o->bit_width != width)
      return 0;
    if (o->register_unit != 321u &&
        (o->bank != AST_MACHINE_BANK_GPR ||
         !machine_physreg_name(target, AST_MACHINE_BANK_GPR,
                               o->register_unit % 64u, width,
                               o->register_name)))
      return 0;
    o->bit_width = width;
  }
  out->operands[s->operand_count] =
      physical_immediate((uint64_t)(f - a64_scalar_forms), 8u);
  return a64_scalar_contract(out);
}

int physical_native_other_info(const AstTargetConfig *target,
                               const AstHirNativeStmtInfo *s,
                               const NativeBindings *bindings,
                               AstPhysicalInstructionInfo *out) {
  if (!s || s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count ||
      (target->arch != AST_TARGET_ARCH_AARCH64 &&
       target->arch != AST_TARGET_ARCH_RISCV64))
    return 0;
  if (rv_scalar_source(target->arch, s->mnemonic))
    return physical_native_scalar_info(target, s, bindings, out);
  if (rv_memory_source(target->arch, s->mnemonic))
    return physical_native_memory_info(target, s, bindings, 0, out);
  const SystemForm *system = system_source_form(target->arch, s);
  if (system) {
    /* A managed boundary must continue normally. Trap/call clobbers and
       stack-bank changes need their own control-flow contracts. */
    if (system->shape == SYSTEM_SHAPE_EXCEPTION_RETURN ||
        system->shape == SYSTEM_SHAPE_A64_EXCEPTION ||
        system == &system_forms[SYSTEM_RV_ECALL] ||
        system == &system_forms[SYSTEM_RV_EBREAK] ||
        system == &system_forms[SYSTEM_A64_SPSEL])
      return 0;
    if (!physical_native_system_bound_info(target, s, bindings, out))
      return 0;
    return system != &system_forms[SYSTEM_A64_MSR] ||
           out->operands[1].immediate_bits != UINT64_C(0xc210);
  }
  if (vector_statement_form(target->arch, s))
    return target->arch == AST_TARGET_ARCH_AARCH64 &&
           physical_native_vector_info(target, s, 0, out);
  if (physical_native_a64_data_form(target, s))
    return physical_native_a64_data_info(target, s, bindings, out);
  if (a64_scalar_source(target->arch, s->mnemonic))
    return physical_native_a64_scalar_info(target, s, bindings, out);
  memset(out, 0, sizeof(*out));
  const AstHirNativeOperandInfo *src[3] = {system_role(s, "dst"), NULL, NULL};
  AstPhysicalOperand *o = out->operands;
  if (!value(target, src[0], bindings, 1, &o[0]))
    return 0;
  int arm = target->arch == AST_TARGET_ARCH_AARCH64;
  if (system_name(s->mnemonic, "mov") ||
      (!arm && system_name(s->mnemonic, "mv"))) {
    src[1] = system_role(s, "src");
    if (s->operand_count != 2u || !value(target, src[1], bindings, 0, &o[1]) ||
        !widths(target, src, o, 2u))
      return 0;
    out->opcode = arm ? AST_MACHINE_A64_MOV : AST_MACHINE_RISCV_MV;
    out->operand_count = 2u;
    out->result_count = 1u;
    if (o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint64_t immediate = o[1].immediate_bits;
      if (o[0].bank != AST_MACHINE_BANK_GPR)
        return 0;
      if (arm) {
        if (immediate > 65535u)
          return 0;
        out->opcode = AST_MACHINE_A64_MOVZ;
        out->operand_count = 3u;
        o[1].bit_width = 16u;
        o[2] = physical_immediate(0u, 8u);
      } else {
        if (immediate > 2047u && immediate < UINT64_C(0xfffffffffffff800))
          return 0;
        out->opcode = AST_MACHINE_RISCV_ADDI;
        out->operand_count = 3u;
        o[2] = o[1];
        memset(&o[1], 0, sizeof(o[1]));
        o[1].kind = AST_PHYSICAL_OPERAND_REGISTER;
        o[1].bank = AST_MACHINE_BANK_GPR;
        o[1].register_unit = 64u;
        o[1].bit_width = 64u;
        memcpy(o[1].register_name, "zero", 5u);
      }
    }
    return 1;
  }
  src[1] = system_role(s, "lhs");
  src[2] = system_role(s, "rhs");
  if (s->operand_count != 3u || !value(target, src[1], bindings, 0, &o[1]) ||
      o[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
      !value(target, src[2], bindings, 0, &o[2]) ||
      !widths(target, src, o, 3u) || o[0].bank != AST_MACHINE_BANK_GPR)
    return 0;
  if (arm) {
    if (system_name(s->mnemonic, "add"))
      out->opcode = AST_MACHINE_A64_ADD;
    else if (system_name(s->mnemonic, "sub"))
      out->opcode = AST_MACHINE_A64_SUB;
    else if (system_name(s->mnemonic, "mul"))
      out->opcode = AST_MACHINE_A64_MUL;
    else if (system_name(s->mnemonic, "and"))
      out->opcode = AST_MACHINE_A64_AND;
    else if (system_name(s->mnemonic, "orr"))
      out->opcode = AST_MACHINE_A64_OR;
    else if (system_name(s->mnemonic, "eor"))
      out->opcode = AST_MACHINE_A64_XOR;
    else
      return 0;
    if (o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
        ((out->opcode != AST_MACHINE_A64_ADD &&
          out->opcode != AST_MACHINE_A64_SUB) ||
         (o[2].immediate_bits > 4095u &&
          ((o[2].immediate_bits & 4095u) ||
           o[2].immediate_bits > UINT64_C(0xfff000)))))
      return 0;
  } else {
    if (system_name(s->mnemonic, "add"))
      out->opcode = AST_MACHINE_RISCV_ADD;
    else if (system_name(s->mnemonic, "sub"))
      out->opcode = AST_MACHINE_RISCV_SUB;
    else if (system_name(s->mnemonic, "and"))
      out->opcode = AST_MACHINE_RISCV_AND;
    else if (system_name(s->mnemonic, "or"))
      out->opcode = AST_MACHINE_RISCV_OR;
    else if (system_name(s->mnemonic, "xor"))
      out->opcode = AST_MACHINE_RISCV_XOR;
    else if (system_name(s->mnemonic, "addi"))
      out->opcode = AST_MACHINE_RISCV_ADDI;
    else
      return 0;
    if (out->opcode == AST_MACHINE_RISCV_ADDI) {
      if (o[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
          (o[2].immediate_bits > 2047u &&
           o[2].immediate_bits < UINT64_C(0xfffffffffffff800)))
        return 0;
    } else if (o[2].kind != AST_PHYSICAL_OPERAND_REGISTER)
      return 0;
  }
  out->operand_count = 3u;
  out->result_count = 1u;
  return 1;
}
