#include "abi_internal.h"

typedef struct RvState {
  uint32_t gpr;
  uint32_t fpr;
  uint64_t stack;
  uint32_t abi_flen;
  uint32_t vector_used;
  uint8_t vector_mask_seen;
} RvState;

static void rv_reg(char buf[AST_ABI_REGISTER_NAME_CAP], const char *prefix,
                   uint32_t i) {
  (void)snprintf(buf, AST_ABI_REGISTER_NAME_CAP, "%s%u", prefix, i);
}

static int rv_contains_scalable(const AstSemaType *type) {
  if (!type)
    return 0;
  if (abi_is_scalable(type))
    return 1;
  if (!abi_is_aggregate(type))
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(type);
  if (k == AST_SEMA_TYPE_ARRAY)
    return rv_contains_scalable(ast_sema_type_element(type));
  if (k == AST_SEMA_TYPE_TUPLE) {
    uint32_t n = ast_sema_type_tuple_count(type);
    for (uint32_t i = 0; i < n; ++i)
      if (rv_contains_scalable(ast_sema_type_tuple_item(type, i)))
        return 1;
  } else if (k == AST_SEMA_TYPE_STRUCT) {
    uint32_t n = ast_sema_type_struct_field_count(type);
    for (uint32_t i = 0; i < n; ++i) {
      AstSemaFieldInfo f;
      if (ast_sema_type_struct_field(type, i, &f) &&
          rv_contains_scalable(f.type))
        return 1;
    }
  }
  return 0;
}

static int rv_vector_variant(AstAbiKind kind) {
  return kind == AST_ABI_RISCV_LP64V || kind == AST_ABI_RISCV_LP64FV ||
         kind == AST_ABI_RISCV_LP64DV;
}

static int rv_scalable_leaf(const AstSemaType *type, int *predicate) {
  if (predicate)
    *predicate = 0;
  if (!type)
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  if (kind == AST_SEMA_TYPE_SVECTOR)
    return 1;
  if (kind == AST_SEMA_TYPE_PRED) {
    if (predicate)
      *predicate = 1;
    return 1;
  }
  return 0;
}

static int rv_lower_scalable(AstAbiValueInfo *v, RvState *s) {
  int predicate = 0;
  if (!rv_scalable_leaf(v ? v->type : NULL, &predicate))
    return 0;
  uint32_t index = UINT32_MAX;
  if (predicate && !s->vector_mask_seen) {
    index = 0u;
    s->vector_mask_seen = 1u;
  } else {
    for (uint32_t i = 8u; i < 24u; ++i) {
      if (!(s->vector_used & (UINT32_C(1) << i))) {
        index = i;
        break;
      }
    }
  }
  if (index == UINT32_MAX)
    return 0;
  s->vector_used |= UINT32_C(1) << index;
  char r[AST_ABI_REGISTER_NAME_CAP];
  rv_reg(r, "v", index);
  abi_set_reg(&v->location, 0u, r);
  return 1;
}

static void rv_stack(AstAbiValueInfo *v, RvState *s, uint64_t size) {
  uint32_t a = abi_type_align(v->type);
  if (a < 8u)
    a = 8u;
  if (a > 16u)
    a = 16u;
  s->stack = abi_align_up(s->stack, a);
  v->location.kind = AST_ABI_LOCATION_STACK;
  v->location.stack_offset = s->stack;
  v->location.stack_size = size;
  v->location.stack_alignment = a;
  v->payload_size = size;
  s->stack += abi_align_up(size, 8u);
}

static int rv_is_simple_fp_aggregate(const AstSemaType *type,
                                     const AstSemaType **a,
                                     const AstSemaType **b, uint32_t *count) {
  *a = NULL;
  *b = NULL;
  *count = 0u;
  if (!abi_is_aggregate(type))
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(type);
  if (k == AST_SEMA_TYPE_STRUCT) {
    uint32_t n = ast_sema_type_struct_field_count(type);
    if (n < 1u || n > 2u)
      return 0;
    AstSemaFieldInfo f0, f1;
    if (!ast_sema_type_struct_field(type, 0u, &f0) || !abi_is_float(f0.type))
      return 0;
    *a = f0.type;
    *count = 1u;
    if (n == 2u) {
      if (!ast_sema_type_struct_field(type, 1u, &f1) || !abi_is_float(f1.type))
        return 0;
      *b = f1.type;
      *count = 2u;
    }
    return 1;
  }
  if (k == AST_SEMA_TYPE_TUPLE) {
    uint32_t n = ast_sema_type_tuple_count(type);
    if (n < 1u || n > 2u)
      return 0;
    *a = ast_sema_type_tuple_item(type, 0u);
    if (!abi_is_float(*a))
      return 0;
    *count = 1u;
    if (n == 2u) {
      *b = ast_sema_type_tuple_item(type, 1u);
      if (!abi_is_float(*b))
        return 0;
      *count = 2u;
    }
    return 1;
  }
  return 0;
}

typedef struct RvMixedAggregate {
  const AstSemaType *first;
  const AstSemaType *second;
  uint8_t first_is_float;
  uint8_t valid;
} RvMixedAggregate;

static RvMixedAggregate rv_mixed_fp_int_aggregate(const AstSemaType *type) {
  RvMixedAggregate m;
  memset(&m, 0, sizeof(m));
  if (!abi_is_aggregate(type))
    return m;
  const AstSemaType *a = NULL, *b = NULL;
  AstSemaTypeKind k = ast_sema_type_kind(type);
  if (k == AST_SEMA_TYPE_STRUCT) {
    if (ast_sema_type_struct_field_count(type) != 2u)
      return m;
    AstSemaFieldInfo fa, fb;
    if (!ast_sema_type_struct_field(type, 0u, &fa) ||
        !ast_sema_type_struct_field(type, 1u, &fb))
      return m;
    a = fa.type;
    b = fb.type;
  } else if (k == AST_SEMA_TYPE_TUPLE) {
    if (ast_sema_type_tuple_count(type) != 2u)
      return m;
    a = ast_sema_type_tuple_item(type, 0u);
    b = ast_sema_type_tuple_item(type, 1u);
  } else
    return m;
  int af = abi_is_float(a), bf = abi_is_float(b);
  int ai = abi_is_integer(a) && abi_type_size(a) <= 8u;
  int bi = abi_is_integer(b) && abi_type_size(b) <= 8u;
  if (!((af && bi) || (bf && ai)))
    return m;
  m.first = a;
  m.second = b;
  m.first_is_float = af ? 1u : 0u;
  m.valid = 1u;
  return m;
}

static int rv_lower_one(AstAbiProgram *p, AstAbiValueInfo *v, RvState *s,
                        int allow_stack, int count_stats) {
  const AstSemaType *type = v->type;
  uint64_t size = abi_type_size(type);
  if (rv_contains_scalable(type)) {
    if (!rv_vector_variant(p->config.kind) || !rv_lower_scalable(v, s))
      return 0;
    return 1;
  }

  if (abi_is_float(type) && s->abi_flen &&
      ast_sema_type_bit_width(type) <= s->abi_flen && s->fpr < 8u) {
    char r[AST_ABI_REGISTER_NAME_CAP];
    rv_reg(r, "fa", s->fpr++);
    abi_set_reg(&v->location, 0u, r);
    if (ast_sema_type_bit_width(type) < s->abi_flen) {
      v->extension = AST_ABI_EXT_NAN_BOX;
      v->flags |= AST_ABI_VALUE_NAN_BOXED;
    }
    return 1;
  }
  const AstSemaType *fa = NULL, *fb = NULL;
  uint32_t fc = 0u;
  if (s->abi_flen && rv_is_simple_fp_aggregate(type, &fa, &fb, &fc) &&
      s->fpr + fc <= 8u && ast_sema_type_bit_width(fa) <= s->abi_flen &&
      (!fb || ast_sema_type_bit_width(fb) <= s->abi_flen)) {
    for (uint32_t j = 0; j < fc; ++j) {
      char r[AST_ABI_REGISTER_NAME_CAP];
      rv_reg(r, "fa", s->fpr++);
      abi_set_reg(&v->location, j, r);
    }
    if (ast_sema_type_bit_width(fa) < s->abi_flen ||
        (fb && ast_sema_type_bit_width(fb) < s->abi_flen))
      v->flags |= AST_ABI_VALUE_NAN_BOXED;
    return 1;
  }
  RvMixedAggregate mix = rv_mixed_fp_int_aggregate(type);
  if (s->abi_flen && mix.valid && s->fpr < 8u && s->gpr < 8u) {
    const AstSemaType *flt = mix.first_is_float ? mix.first : mix.second;
    if (ast_sema_type_bit_width(flt) <= s->abi_flen) {
      char fr[AST_ABI_REGISTER_NAME_CAP], gr[AST_ABI_REGISTER_NAME_CAP];
      rv_reg(fr, "fa", s->fpr++);
      rv_reg(gr, "a", s->gpr++);
      if (mix.first_is_float) {
        abi_set_reg(&v->location, 0u, fr);
        abi_set_reg(&v->location, 1u, gr);
      } else {
        abi_set_reg(&v->location, 0u, gr);
        abi_set_reg(&v->location, 1u, fr);
      }
      if (ast_sema_type_bit_width(flt) < s->abi_flen)
        v->flags |= AST_ABI_VALUE_NAN_BOXED;
      return 1;
    }
  }

  int fixed_vector = type && ast_sema_type_kind(type) == AST_SEMA_TYPE_VECTOR;
  int aggregate_like = abi_is_aggregate(type) || fixed_vector;
  int indirect =
      (aggregate_like && size > 16u) ||
      (!abi_is_integer(type) && !abi_is_float(type) && !aggregate_like);
  if (indirect) {
    v->flags |= AST_ABI_VALUE_INDIRECT_COPY;
    v->extension = AST_ABI_EXT_BY_REFERENCE;
    v->payload_size = 8u;
    v->alignment = 8u;
    if (s->gpr < 8u) {
      char r[AST_ABI_REGISTER_NAME_CAP];
      rv_reg(r, "a", s->gpr++);
      abi_set_reg(&v->location, 0u, r);
      v->location.kind = AST_ABI_LOCATION_INDIRECT;
    } else if (allow_stack) {
      rv_stack(v, s, 8u);
      v->location.kind = AST_ABI_LOCATION_INDIRECT;
      if (count_stats)
        p->stats.stack_argument_count++;
    } else
      return 0;
    if (count_stats)
      p->stats.indirect_argument_count++;
    return 1;
  }

  uint32_t chunks = size > 8u ? 2u : 1u;
  if (chunks == 1u) {
    if (s->gpr < 8u) {
      char r[AST_ABI_REGISTER_NAME_CAP];
      rv_reg(r, "a", s->gpr++);
      abi_set_reg(&v->location, 0u, r);
    } else if (allow_stack) {
      rv_stack(v, s, 8u);
      if (count_stats)
        p->stats.stack_argument_count++;
    } else
      return 0;
    if (abi_is_integer(type) && ast_sema_type_bit_width(type) &&
        ast_sema_type_bit_width(type) < 64u) {
      v->extension = AST_ABI_EXT_SIGN_TO_XLEN_AFTER_32;
      v->payload_size = 8u;
    }
    return 1;
  }
  if (s->gpr <= 6u) {
    for (uint32_t j = 0; j < 2u; ++j) {
      char r[AST_ABI_REGISTER_NAME_CAP];
      rv_reg(r, "a", s->gpr++);
      abi_set_reg(&v->location, j, r);
    }
    return 1;
  }
  if (s->gpr == 7u && allow_stack) {
    char r[AST_ABI_REGISTER_NAME_CAP];
    rv_reg(r, "a", s->gpr++);
    abi_set_reg(&v->location, 0u, r);
    s->stack = abi_align_up(s->stack, 8u);
    v->location.kind = AST_ABI_LOCATION_SPLIT_REG_STACK;
    v->location.stack_offset = s->stack;
    v->location.stack_size = 8u;
    v->location.stack_alignment = 8u;
    v->flags |= AST_ABI_VALUE_SPLIT;
    s->stack += 8u;
    if (count_stats) {
      p->stats.stack_argument_count++;
      p->stats.split_argument_count++;
    }
    return 1;
  }
  if (allow_stack) {
    rv_stack(v, s, 16u);
    if (count_stats)
      p->stats.stack_argument_count++;
    return 1;
  }
  return 0;
}

int abi_lower_riscv(AstAbiProgram *p, const AstSemaType *signature,
                    const AstMirVReg *source_vregs, uint32_t source_vreg_count,
                    uint32_t function_index, AstMirInstId call_inst,
                    AbiSignatureResult *out) {
  memset(out, 0, sizeof(*out));
  uint32_t pc = ast_sema_type_function_param_count(signature);
  if (source_vreg_count != pc)
    return 0;
  for (uint32_t i = 0; i < pc; ++i)
    if (rv_contains_scalable(ast_sema_type_function_param(signature, i)) &&
        (!rv_vector_variant(p->config.kind) ||
         !rv_scalable_leaf(ast_sema_type_function_param(signature, i), NULL))) {
      abi_diag(p, AST_ABI_DIAG_UNSUPPORTED_SIGNATURE, AST_DIAG_ERROR,
               (AstSpan){0}, function_index, call_inst,
               "RISC-V vector PCS currently supports only direct scalable "
               "vector and predicate values");
      return 0;
    }
  const AstSemaType *ret = ast_sema_type_function_result(signature);
  if (rv_contains_scalable(ret) &&
      (!rv_vector_variant(p->config.kind) || !rv_scalable_leaf(ret, NULL))) {
    abi_diag(p, AST_ABI_DIAG_UNSUPPORTED_SIGNATURE, AST_DIAG_ERROR,
             (AstSpan){0}, function_index, call_inst,
             "RISC-V vector PCS currently supports only direct scalable vector "
             "and predicate values");
    return 0;
  }
  uint32_t flen = (p->config.kind == AST_ABI_RISCV_LP64D ||
                   p->config.kind == AST_ABI_RISCV_LP64DV)
                      ? 64u
                  : (p->config.kind == AST_ABI_RISCV_LP64F ||
                     p->config.kind == AST_ABI_RISCV_LP64FV)
                      ? 32u
                      : 0u;
  AstSemaTypeKind ret_kind = ret ? ast_sema_type_kind(ret) : AST_SEMA_TYPE_VOID;
  int sret = ret && ret_kind != AST_SEMA_TYPE_VOID &&
             (abi_is_aggregate(ret) || ret_kind == AST_SEMA_TYPE_VECTOR) &&
             abi_type_size(ret) > 16u;
  abi_init_value(&out->result, ret, AST_MIR_INVALID_ID, UINT32_MAX);
  out->result.flags |= AST_ABI_VALUE_RESULT;
  if (ret && ast_sema_type_kind(ret) != AST_SEMA_TYPE_VOID) {
    if (sret) {
      out->result.location.kind = AST_ABI_LOCATION_INDIRECT;
      out->result.extension = AST_ABI_EXT_BY_REFERENCE;
      out->result.flags |= AST_ABI_VALUE_INDIRECT_COPY;
    } else {
      RvState rs = {0u, 0u, 0u, flen, 0u, 0u};
      if (!rv_lower_one(p, &out->result, &rs, 0, 0))
        return 0;
    }
  }
  out->hidden_count = sret ? 1u : 0u;
  out->argument_count = pc + out->hidden_count;
  out->arguments = (AstAbiValueInfo *)calloc(
      out->argument_count ? out->argument_count : 1u, sizeof(*out->arguments));
  if (!out->arguments)
    return 0;
  RvState st = {0u, 0u, 0u, flen, 0u, 0u};
  if (sret) {
    AstAbiValueInfo *h = &out->arguments[0];
    abi_init_value(h, ret, AST_MIR_INVALID_ID, UINT32_MAX);
    h->flags = AST_ABI_VALUE_HIDDEN | AST_ABI_VALUE_INDIRECT_COPY;
    h->extension = AST_ABI_EXT_BY_REFERENCE;
    h->payload_size = 8u;
    h->alignment = 8u;
    abi_set_reg(&h->location, 0u, "a0");
    st.gpr = 1u;
    p->stats.hidden_sret_count++;
    /* psABI does not require the hidden result pointer to be returned in a0. */
  }
  for (uint32_t i = 0; i < pc; ++i) {
    AstAbiValueInfo *v = &out->arguments[out->hidden_count + i];
    const AstSemaType *type = ast_sema_type_function_param(signature, i);
    abi_init_value(v, type, source_vregs ? source_vregs[i] : AST_MIR_INVALID_ID,
                   i);
    if (!rv_lower_one(p, v, &st, 1, 1))
      return 0;
  }
  out->stack_bytes = abi_align_up(st.stack, 16u);
  out->stack_alignment = 16u;
  return 1;
}
