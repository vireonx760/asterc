#include "../../isa/aarch64_scalable.h"
#include "../../isa/system_contract.h"
#include "physical_internal.h"
#include <ctype.h>

static int name(const AstHirNativeExpr *expr, char *out, size_t capacity) {
  AstHirNativeExprInfo e;
  if (!expr || !ast_hir_native_expr_info(expr, &e) ||
      e.kind != AST_HIR_NATIVE_EXPR_NAME ||
      e.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !e.name.text ||
      !e.name.span.length || e.name.span.length >= capacity)
    return 0;
  for (uint32_t j = 0; j < e.name.span.length; ++j)
    out[j] = (char)tolower((unsigned char)e.name.text[j]);
  out[e.name.span.length] = 0;
  return 1;
}
static int fp_literal(const AstHirNativeExpr *expr, uint64_t *value,
                      uint32_t depth) {
  AstHirNativeExprInfo e;
  if (depth > 16u || !expr || !ast_hir_native_expr_info(expr, &e))
    return 0;
  if (e.kind == AST_HIR_NATIVE_EXPR_UNARY && e.child_count == 1u &&
      (e.token_kind == AST_TOK_PLUS || e.token_kind == AST_TOK_MINUS)) {
    if (!fp_literal(ast_hir_native_expr_child(expr, 0u), value, depth + 1u))
      return 0;
    if (e.token_kind == AST_TOK_MINUS)
      *value ^= UINT64_C(1) << 63u;
    return 1;
  }
  double d;
  if (e.kind == AST_HIR_NATIVE_EXPR_LITERAL && e.token_kind == AST_TOK_FLOAT &&
      e.literal.length < 128u) {
    char text[128];
    uint32_t n = 0u;
    for (uint32_t j = 0; j < e.literal.length; ++j)
      if (e.literal.data[j] != '_')
        text[n++] = e.literal.data[j];
    text[n] = 0;
    char *end = NULL;
    d = strtod(text, &end);
    if (!end || (*end && strcmp(end, "f16") && strcmp(end, "f32") &&
                 strcmp(end, "f64")))
      return 0;
  } else {
    uint64_t integer;
    if (!physical_native_integer_bits(expr, &integer))
      return 0;
    d = integer <= INT64_MAX ? (double)integer : -(double)(~integer) - 1.0;
  }
  memcpy(value, &d, sizeof(*value));
  return 1;
}
static int selector(const AstHirNativeExpr *expr, const char *const *names,
                    uint32_t count, uint64_t *value) {
  if (physical_native_integer_bits(expr, value))
    return *value < count;
  char text[32];
  if (!name(expr, text, sizeof(text)))
    return 0;
  for (uint32_t j = 0; j < count; ++j)
    if (names[j] && !strcmp(text, names[j])) {
      *value = j;
      return 1;
    }
  return 0;
}
static int operand(const A64ScalableType *t, const AstHirNativeOperandInfo *src,
                   AstPhysicalOperand *out) {
  if (!src || src->is_memory)
    return 0;
  if (t->kind < A64_SC_I) {
    char text[AST_MACHINE_REGISTER_NAME_CAP];
    if (!name(src->value, text, sizeof(text)))
      return 0;
    for (uint32_t j = t->first; j <= t->last; ++j) {
      AstPhysicalOperand r;
      if (!a64_scalable_register(t, j, &r))
        return 0;
      if (strcmp(text, r.register_name)) {
        if (!(t->kind == A64_SC_F && t->width == 128u && text[0] == 'v' &&
              r.register_name[0] == 'q' &&
              !strcmp(text + 1, r.register_name + 1)))
          continue;
      }
      *out = r;
      return 1;
    }
    return 0;
  }
  uint64_t value;
  if (t->kind == A64_SC_FP_PAIR || t->kind == A64_SC_FP8) {
    if (!fp_literal(src->value, &value, 0u))
      return 0;
  } else if (t->kind == A64_SC_MODE) {
    static const char *const states[] = {NULL, "sm", "za", "all"};
    if (!selector(src->value, states, 4u, &value) || !value)
      return 0;
  } else if (!strcmp(t->name, "sve_pred_enum")) {
    static const char *const patterns[] = {
        "pow2", "vl1",  "vl2",  "vl3",  "vl4",   "vl5",   "vl6",  "vl7",
        "vl8",  "vl16", "vl32", "vl64", "vl128", "vl256", NULL,   NULL,
        NULL,   NULL,   NULL,   NULL,   NULL,    NULL,    NULL,   NULL,
        NULL,   NULL,   NULL,   NULL,   NULL,    "mul4",  "mul3", "all"};
    if (!selector(src->value, patterns, 32u, &value))
      return 0;
  } else if (!physical_native_integer_bits(src->value, &value))
    return 0;
  *out = physical_immediate(value, 64u);
  uint64_t ignored;
  return a64_scalable_operand(t, out, &ignored);
}
static int selected(const AstHirNativeStmtInfo *s, const char *role,
                    uint64_t want, int required, uint32_t *used) {
  const AstHirNativeOperandInfo *o = system_role(s, role);
  uint64_t value;
  if (!o)
    return !required;
  if (o->is_memory || !physical_native_integer_bits(o->value, &value) ||
      value != want)
    return 0;
  ++*used;
  return 1;
}
int physical_native_scalable_info(const AstTargetConfig *target,
                                  const AstHirNativeStmtInfo *s,
                                  AstPhysicalInstructionInfo *out) {
  if (!a64_scalable_statement(target->arch, s) || s->attribute_count ||
      s->kind != AST_HIR_NATIVE_INSTRUCTION)
    return 0;
  uint32_t first = a64_scalable_find(s->mnemonic);
  AstPhysicalInstructionInfo best = {0};
  uint32_t best_inferred = UINT32_MAX;
  for (uint32_t k = first;
       k < a64_scalable_form_count &&
       !strcmp(a64_scalable_forms[k].name, a64_scalable_forms[first].name);
       ++k) {
    const A64ScalableForm *f = &a64_scalable_forms[k];
    uint32_t used = 0u;
    uint32_t group = 1u;
    for (uint32_t j = 0; j < f->parameter_count; ++j) {
      uint32_t count =
          a64_scalable_types[a64_scalable_parameters[f->parameter_offset + j]
                                 .type]
              .group;
      if (count > group)
        group = count;
    }
    if (!selected(s, "group", group, group != 1u, &used))
      continue;
    if (!a64_scalable_features(target, f) ||
        !selected(s, "bits", f->bits, f->bits != 0u, &used) ||
        !selected(s, "input_bits", f->input_bits, f->input_bits != f->bits,
                  &used) ||
        !selected(s, "shift", f->shift, f->shift != 0u, &used))
      continue;
    const AstHirNativeOperandInfo *extension = system_role(s, "extend");
    if (extension) {
      static const char *const names[] = {"lsl", "uxtw", "sxtw"};
      uint64_t value;
      if (extension->is_memory ||
          !selector(extension->value, names, 3u, &value) || value != f->extend)
        continue;
      ++used;
    } else if (f->extend)
      continue;
    const AstHirNativeOperandInfo *merge = system_role(s, "merge");
    if (merge) {
      uint64_t value;
      if ((!strstr(f->assembly, "/m") && !strstr(f->assembly, "/z")) ||
          merge->is_memory ||
          !physical_native_integer_bits(merge->value, &value) ||
          value != (uint64_t)!!(f->flags & A64_SC_MERGING))
        continue;
      ++used;
    }
    uint32_t inferred = 0u;
    memset(out, 0, sizeof(*out));
    out->opcode = AST_MACHINE_A64_SCALABLE;
    out->operand_count = (uint32_t)f->parameter_count + 1u;
    out->result_count = f->results;
    int valid = 1;
    for (uint32_t j = 0; j < f->parameter_count; ++j) {
      const A64ScalableParameter *p =
          &a64_scalable_parameters[f->parameter_offset + j];
      const A64ScalableType *t = &a64_scalable_types[p->type];
      const AstHirNativeOperandInfo *src = system_role(s, p->role);
      if (src) {
        if (!operand(t, src, &out->operands[j])) {
          valid = 0;
          break;
        }
        ++used;
      } else if (p->tied >= 0) {
        ++inferred;
        uint32_t tied = (uint16_t)p->tied;
        const A64ScalableParameter *def =
            &a64_scalable_parameters[f->parameter_offset + tied];
        uint64_t index;
        if (!a64_scalable_operand(&a64_scalable_types[def->type],
                                  &out->operands[tied], &index) ||
            !a64_scalable_decode_operand(t, index, &out->operands[j])) {
          valid = 0;
          break;
        }
      } else if (p->fallback >= 0)
        out->operands[j] = physical_immediate((uint64_t)p->fallback, 64u);
      else {
        valid = 0;
        break;
      }
    }
    if (!valid || used != s->operand_count)
      continue;
    out->operands[f->parameter_count] = physical_immediate(k, 16u);
    if (a64_scalable_contract(out) && inferred < best_inferred) {
      best = *out;
      best_inferred = inferred;
    }
  }
  if (best_inferred == UINT32_MAX)
    return 0;
  *out = best;
  return 1;
}
