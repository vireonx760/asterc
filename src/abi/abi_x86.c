#include "abi_internal.h"

typedef enum SysVClass {
  SYSV_NO = 0,
  SYSV_INTEGER,
  SYSV_SSE,
  SYSV_SSEUP,
  SYSV_MEMORY
} SysVClass;

typedef struct SysVTypeClass {
  SysVClass cls[2];
  uint32_t count;
  uint8_t memory;
} SysVTypeClass;

static SysVClass sysv_merge(SysVClass a, SysVClass b) {
  if (a == SYSV_MEMORY || b == SYSV_MEMORY)
    return SYSV_MEMORY;
  if (a == SYSV_NO)
    return b;
  if (b == SYSV_NO)
    return a;
  if (a == SYSV_INTEGER || b == SYSV_INTEGER)
    return SYSV_INTEGER;
  if (a == SYSV_SSE || b == SYSV_SSE)
    return SYSV_SSE;
  return SYSV_SSEUP;
}

static int sysv_classify_into(const AstSemaType *type, uint64_t base_off,
                              SysVClass out[2]) {
  if (!type)
    return 0;
  uint64_t size = abi_type_size(type);
  if (size == 0u)
    return 1;
  if (base_off + size > 16u)
    return 0;

  if (abi_is_integer(type)) {
    uint32_t first = (uint32_t)(base_off / 8u);
    uint32_t last = (uint32_t)((base_off + size - 1u) / 8u);
    for (uint32_t i = first; i <= last && i < 2u; ++i)
      out[i] = sysv_merge(out[i], SYSV_INTEGER);
    return 1;
  }
  if (abi_is_float(type) || abi_is_vector(type)) {
    if (abi_is_vector(type) && size > 16u)
      return 0;
    uint32_t first = (uint32_t)(base_off / 8u);
    uint32_t last = (uint32_t)((base_off + size - 1u) / 8u);
    for (uint32_t i = first; i <= last && i < 2u; ++i) {
      SysVClass part =
          (abi_is_vector(type) && i > first) ? SYSV_SSEUP : SYSV_SSE;
      out[i] = sysv_merge(out[i], part);
    }
    return 1;
  }
  if (!abi_is_aggregate(type))
    return 0;

  AstSemaTypeKind kind = ast_sema_type_kind(type);
  if (kind == AST_SEMA_TYPE_STRUCT) {
    uint32_t n = ast_sema_type_struct_field_count(type);
    for (uint32_t i = 0; i < n; ++i) {
      AstSemaFieldInfo field;
      if (!ast_sema_type_struct_field(type, i, &field))
        return 0;
      uint32_t fa = abi_type_align(field.type);
      if (fa > 1u && (field.offset % fa) != 0u)
        return 0;
      if (!sysv_classify_into(field.type, base_off + field.offset, out))
        return 0;
    }
    return 1;
  }
  if (kind == AST_SEMA_TYPE_ARRAY) {
    uint64_t count = 0;
    const AstSemaType *elem = ast_sema_type_element(type);
    if (!elem || !ast_sema_type_array_count(type, &count))
      return 0;
    uint64_t stride = abi_align_up(abi_type_size(elem), abi_type_align(elem));
    for (uint64_t i = 0; i < count; ++i)
      if (!sysv_classify_into(elem, base_off + i * stride, out))
        return 0;
    return 1;
  }
  if (kind == AST_SEMA_TYPE_TUPLE) {
    uint32_t n = ast_sema_type_tuple_count(type);
    uint64_t off = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const AstSemaType *item = ast_sema_type_tuple_item(type, i);
      if (!item)
        return 0;
      off = abi_align_up(off, abi_type_align(item));
      if (!sysv_classify_into(item, base_off + off, out))
        return 0;
      off += abi_type_size(item);
    }
    return 1;
  }
  return 0;
}

static SysVTypeClass sysv_classify(const AstSemaType *type,
                                   AstTargetConfig tc) {
  SysVTypeClass r;
  memset(&r, 0, sizeof(r));
  uint64_t size = abi_type_size(type);
  if (abi_is_vector(type) && size > 16u) {
    if ((size == 32u && (tc.features & AST_TARGET_FEATURE_X86_AVX)) ||
        (size == 64u && (tc.features & AST_TARGET_FEATURE_X86_AVX512F))) {
      r.cls[0] = SYSV_SSE;
      r.count = 1u;
      return r;
    }
    r.memory = 1u;
    return r;
  }
  if (size > 16u || !sysv_classify_into(type, 0u, r.cls)) {
    r.memory = 1u;
    return r;
  }
  r.count = size > 8u ? 2u : (size ? 1u : 0u);
  for (uint32_t i = 0; i < r.count; ++i) {
    if (r.cls[i] == SYSV_NO)
      r.cls[i] = SYSV_INTEGER;
    if (r.cls[i] == SYSV_SSEUP &&
        (i == 0u || (r.cls[i - 1u] != SYSV_SSE && r.cls[i - 1u] != SYSV_SSEUP)))
      r.cls[i] = SYSV_SSE;
  }
  return r;
}

static void abi_stack_value(AstAbiValueInfo *v, uint64_t *stack,
                            uint32_t min_align, uint64_t payload) {
  uint32_t a = abi_type_align(v->type);
  if (a < min_align)
    a = min_align;
  *stack = abi_align_up(*stack, a);
  v->location.kind = AST_ABI_LOCATION_STACK;
  v->location.stack_offset = *stack;
  v->location.stack_alignment = a;
  v->location.stack_size = payload;
  v->payload_size = payload;
  *stack += abi_align_up(payload, 8u);
}

static void sysv_return_value(AstAbiValueInfo *result, const AstSemaType *type,
                              AstTargetConfig tc, int *needs_sret) {
  abi_init_value(result, type, AST_MIR_INVALID_ID, UINT32_MAX);
  result->flags |= AST_ABI_VALUE_RESULT;
  if (!type || ast_sema_type_kind(type) == AST_SEMA_TYPE_VOID)
    return;
  SysVTypeClass c = sysv_classify(type, tc);
  if (c.memory) {
    *needs_sret = 1;
    result->location.kind = AST_ABI_LOCATION_INDIRECT;
    abi_set_reg(&result->location, 0u, "rax");
    result->location.kind = AST_ABI_LOCATION_INDIRECT;
    result->extension = AST_ABI_EXT_BY_REFERENCE;
    result->flags |=
        AST_ABI_VALUE_INDIRECT_COPY | AST_ABI_VALUE_RETURN_POINTER_IN_GPR;
    result->payload_size = abi_type_size(type);
    return;
  }
  if (abi_is_vector(type)) {
    uint64_t size = abi_type_size(type);
    abi_set_reg(&result->location, 0u,
                size > 32u ? "zmm0" : (size > 16u ? "ymm0" : "xmm0"));
    return;
  }
  static const char *gpr[] = {"rax", "rdx"};
  static const char *sse[] = {"xmm0", "xmm1"};
  uint32_t gi = 0, si = 0;
  for (uint32_t i = 0; i < c.count; ++i) {
    if (c.cls[i] == SYSV_INTEGER)
      abi_set_reg(&result->location, result->location.register_count,
                  gpr[gi++]);
    else if (c.cls[i] == SYSV_SSE)
      abi_set_reg(&result->location, result->location.register_count,
                  sse[si++]);
  }
}

int abi_lower_sysv(AstAbiProgram *p, const AstSemaType *signature,
                   const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                   uint32_t function_index, AstMirInstId call_inst,
                   AbiSignatureResult *out) {
  (void)function_index;
  (void)call_inst;
  memset(out, 0, sizeof(*out));
  uint32_t pc = ast_sema_type_function_param_count(signature);
  if (source_vreg_count != pc)
    return 0;
  AstTargetConfig tc = ast_target_config(p->target);
  const AstSemaType *ret = ast_sema_type_function_result(signature);
  int sret = 0;
  sysv_return_value(&out->result, ret, tc, &sret);

  out->hidden_count = sret ? 1u : 0u;
  out->argument_count = pc + out->hidden_count;
  out->arguments = (AstAbiValueInfo *)calloc(
      out->argument_count ? out->argument_count : 1u, sizeof(*out->arguments));
  if (!out->arguments)
    return 0;
  uint32_t gp = 0u, fp = 0u;
  uint64_t stack = 0u;
  if (sret) {
    AstAbiValueInfo *h = &out->arguments[0];
    abi_init_value(h, ret, AST_MIR_INVALID_ID, UINT32_MAX);
    h->flags = AST_ABI_VALUE_HIDDEN | AST_ABI_VALUE_INDIRECT_COPY;
    h->extension = AST_ABI_EXT_BY_REFERENCE;
    h->payload_size = 8u;
    h->alignment = 8u;
    abi_set_reg(&h->location, 0u, "rdi");
    gp = 1u;
    p->stats.hidden_sret_count++;
  }
  static const char *gp_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
  static const char *fp_regs[] = {"xmm0", "xmm1", "xmm2", "xmm3",
                                  "xmm4", "xmm5", "xmm6", "xmm7"};

  for (uint32_t i = 0; i < pc; ++i) {
    const AstSemaType *type = ast_sema_type_function_param(signature, i);
    AstAbiValueInfo *v = &out->arguments[out->hidden_count + i];
    abi_init_value(v, type, source_vregs ? source_vregs[i] : AST_MIR_INVALID_ID,
                   i);
    SysVTypeClass c = sysv_classify(type, tc);
    if (c.memory) {
      abi_stack_value(v, &stack, 8u, abi_align_up(abi_type_size(type), 8u));
      p->stats.stack_argument_count++;
      continue;
    }
    if (abi_is_vector(type)) {
      uint64_t vs = abi_type_size(type);
      if (fp < 8u) {
        char reg[AST_ABI_REGISTER_NAME_CAP];
        (void)snprintf(reg, sizeof(reg), "%s%u",
                       vs > 32u ? "zmm" : (vs > 16u ? "ymm" : "xmm"), fp++);
        abi_set_reg(&v->location, 0u, reg);
      } else {
        abi_stack_value(v, &stack, vs >= 32u ? (uint32_t)vs : 16u, vs);
        p->stats.stack_argument_count++;
      }
      continue;
    }
    uint32_t need_gp = 0u, need_fp = 0u;
    for (uint32_t j = 0; j < c.count; ++j) {
      if (c.cls[j] == SYSV_INTEGER)
        need_gp++;
      else if (c.cls[j] == SYSV_SSE)
        need_fp++;
    }
    if (gp + need_gp > 6u || fp + need_fp > 8u) {
      abi_stack_value(v, &stack, 8u, abi_align_up(abi_type_size(type), 8u));
      p->stats.stack_argument_count++;
      continue;
    }
    for (uint32_t j = 0; j < c.count; ++j) {
      if (c.cls[j] == SYSV_INTEGER)
        abi_set_reg(&v->location, v->location.register_count, gp_regs[gp++]);
      else if (c.cls[j] == SYSV_SSE)
        abi_set_reg(&v->location, v->location.register_count, fp_regs[fp++]);
    }
    if (abi_is_integer(type) && ast_sema_type_bit_width(type) &&
        ast_sema_type_bit_width(type) < 32u)
      v->extension =
          ast_sema_type_is_signed(type) ? AST_ABI_EXT_SIGN : AST_ABI_EXT_ZERO;
  }
  out->stack_alignment = 16u;
  for (uint32_t i = 0; i < out->argument_count; ++i)
    if (out->arguments[i].location.kind == AST_ABI_LOCATION_STACK &&
        out->arguments[i].location.stack_alignment > out->stack_alignment)
      out->stack_alignment = out->arguments[i].location.stack_alignment;
  out->stack_bytes = abi_align_up(stack, out->stack_alignment);
  out->red_zone = 128u;
  return 1;
}

static int win_direct_integer_payload(const AstSemaType *type) {
  uint64_t size = abi_type_size(type);
  if (abi_is_integer(type))
    return size == 1u || size == 2u || size == 4u || size == 8u;
  if (abi_is_aggregate(type))
    return size == 1u || size == 2u || size == 4u || size == 8u;
  return 0;
}

static int win_result_needs_sret(const AstSemaType *type) {
  if (!type || ast_sema_type_kind(type) == AST_SEMA_TYPE_VOID)
    return 0;
  if (abi_is_float(type))
    return 0;
  if (abi_is_vector(type) && abi_type_size(type) == 16u)
    return 0;
  return !win_direct_integer_payload(type);
}

int abi_lower_win64(AstAbiProgram *p, const AstSemaType *signature,
                    const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                    uint32_t function_index, AstMirInstId call_inst,
                    AbiSignatureResult *out) {
  (void)function_index;
  (void)call_inst;
  memset(out, 0, sizeof(*out));
  uint32_t pc = ast_sema_type_function_param_count(signature);
  if (source_vreg_count != pc)
    return 0;
  const AstSemaType *ret = ast_sema_type_function_result(signature);
  int sret = win_result_needs_sret(ret);
  abi_init_value(&out->result, ret, AST_MIR_INVALID_ID, UINT32_MAX);
  out->result.flags |= AST_ABI_VALUE_RESULT;
  if (ret && ast_sema_type_kind(ret) != AST_SEMA_TYPE_VOID) {
    if (sret) {
      out->result.location.kind = AST_ABI_LOCATION_INDIRECT;
      abi_set_reg(&out->result.location, 0u, "rax");
      out->result.location.kind = AST_ABI_LOCATION_INDIRECT;
      out->result.extension = AST_ABI_EXT_BY_REFERENCE;
      out->result.flags |=
          AST_ABI_VALUE_INDIRECT_COPY | AST_ABI_VALUE_RETURN_POINTER_IN_GPR;
    } else if (abi_is_float(ret) || abi_is_vector(ret))
      abi_set_reg(&out->result.location, 0u, "xmm0");
    else
      abi_set_reg(&out->result.location, 0u, "rax");
  }
  out->hidden_count = sret ? 1u : 0u;
  out->argument_count = pc + out->hidden_count;
  out->arguments = (AstAbiValueInfo *)calloc(
      out->argument_count ? out->argument_count : 1u, sizeof(*out->arguments));
  if (!out->arguments)
    return 0;
  static const char *gpr[] = {"rcx", "rdx", "r8", "r9"};
  static const char *xmm[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
  uint32_t pos = 0u;
  uint64_t stack_end = 32u;
  if (sret) {
    AstAbiValueInfo *h = &out->arguments[0];
    abi_init_value(h, ret, AST_MIR_INVALID_ID, UINT32_MAX);
    h->flags = AST_ABI_VALUE_HIDDEN | AST_ABI_VALUE_INDIRECT_COPY;
    h->extension = AST_ABI_EXT_BY_REFERENCE;
    h->payload_size = 8u;
    h->alignment = 8u;
    abi_set_reg(&h->location, 0u, gpr[0]);
    pos = 1u;
    p->stats.hidden_sret_count++;
  }
  for (uint32_t i = 0; i < pc; ++i, ++pos) {
    const AstSemaType *type = ast_sema_type_function_param(signature, i);
    AstAbiValueInfo *v = &out->arguments[out->hidden_count + i];
    abi_init_value(v, type, source_vregs ? source_vregs[i] : AST_MIR_INVALID_ID,
                   i);
    int indirect = !(abi_is_float(type) || win_direct_integer_payload(type));
    if (abi_is_vector(type))
      indirect = 1;
    if (indirect) {
      v->flags |= AST_ABI_VALUE_INDIRECT_COPY;
      v->extension = AST_ABI_EXT_BY_REFERENCE;
      v->payload_size = 8u;
      v->alignment = 8u;
      if (pos < 4u) {
        abi_set_reg(&v->location, 0u, gpr[pos]);
        v->location.kind = AST_ABI_LOCATION_INDIRECT;
      } else {
        v->location.kind = AST_ABI_LOCATION_INDIRECT;
        v->location.stack_offset = 32u + (uint64_t)(pos - 4u) * 8u;
        v->location.stack_size = 8u;
        v->location.stack_alignment = 8u;
        stack_end = v->location.stack_offset + 8u;
        p->stats.stack_argument_count++;
      }
      p->stats.indirect_argument_count++;
      continue;
    }
    if (pos < 4u) {
      if (abi_is_float(type))
        abi_set_reg(&v->location, 0u, xmm[pos]);
      else
        abi_set_reg(&v->location, 0u, gpr[pos]);
    } else {
      v->location.kind = AST_ABI_LOCATION_STACK;
      v->location.stack_offset = 32u + (uint64_t)(pos - 4u) * 8u;
      v->location.stack_size = 8u;
      v->location.stack_alignment = 8u;
      v->payload_size = abi_type_size(type) < 8u ? 8u : abi_type_size(type);
      stack_end = v->location.stack_offset + 8u;
      p->stats.stack_argument_count++;
    }
  }
  out->shadow_space = 32u;
  out->stack_bytes = abi_align_up(stack_end, 16u);
  out->stack_alignment = 16u;
  return 1;
}
