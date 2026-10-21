#include "abi_internal.h"

typedef struct A64Homogeneous {
  const AstSemaType *base;
  uint32_t count;
  uint64_t next_offset;
  uint8_t valid;
  uint8_t vector;
} A64Homogeneous;

static int a64_short_vector(const AstSemaType *type) {
  if (!type || ast_sema_type_kind(type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  uint64_t size = abi_type_size(type);
  return size == 8u || size == 16u;
}

static int a64_hom_leaf(const AstSemaType *type, uint8_t *is_vector) {
  if (abi_is_float(type)) {
    *is_vector = 0u;
    return 1;
  }
  if (type && ast_sema_type_kind(type) == AST_SEMA_TYPE_VECTOR) {
    uint64_t size = abi_type_size(type);
    if (size == 8u || size == 16u) {
      *is_vector = 1u;
      return 1;
    }
  }
  return 0;
}

static int a64_same_hom_base(const AstSemaType *a, const AstSemaType *b,
                             uint8_t vector) {
  if (!a || !b || ast_sema_type_kind(a) != ast_sema_type_kind(b))
    return 0;
  if (!vector)
    return ast_sema_type_bit_width(a) == ast_sema_type_bit_width(b);
  uint64_t la = 0u, lb = 0u;
  if (!ast_sema_type_vector_lanes(a, &la) ||
      !ast_sema_type_vector_lanes(b, &lb) || la != lb)
    return 0;
  const AstSemaType *ea = ast_sema_type_element(a),
                    *eb = ast_sema_type_element(b);
  return ea && eb && ast_sema_type_kind(ea) == ast_sema_type_kind(eb) &&
         ast_sema_type_bit_width(ea) == ast_sema_type_bit_width(eb) &&
         abi_type_size(a) == abi_type_size(b);
}

static int a64_collect_homogeneous(const AstSemaType *type, uint64_t off,
                                   A64Homogeneous *h) {
  uint8_t vec = 0u;
  if (a64_hom_leaf(type, &vec)) {
    if (!h->base) {
      h->base = type;
      h->vector = vec;
      h->next_offset = off;
    }
    if (h->vector != vec || !a64_same_hom_base(h->base, type, vec) ||
        off != h->next_offset || h->count == 4u)
      return 0;
    h->count++;
    h->next_offset += abi_type_size(type);
    return 1;
  }
  if (!abi_is_aggregate(type))
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  if (kind == AST_SEMA_TYPE_STRUCT) {
    uint32_t n = ast_sema_type_struct_field_count(type);
    for (uint32_t i = 0; i < n; ++i) {
      AstSemaFieldInfo f;
      if (!ast_sema_type_struct_field(type, i, &f) ||
          !a64_collect_homogeneous(f.type, off + f.offset, h))
        return 0;
    }
    return 1;
  }
  if (kind == AST_SEMA_TYPE_ARRAY) {
    uint64_t n = 0u;
    const AstSemaType *elem = ast_sema_type_element(type);
    if (!elem || !ast_sema_type_array_count(type, &n))
      return 0;
    uint64_t stride = abi_align_up(abi_type_size(elem), abi_type_align(elem));
    for (uint64_t i = 0; i < n; ++i)
      if (!a64_collect_homogeneous(elem, off + i * stride, h))
        return 0;
    return 1;
  }
  if (kind == AST_SEMA_TYPE_TUPLE) {
    uint32_t n = ast_sema_type_tuple_count(type);
    uint64_t cur = 0u;
    for (uint32_t i = 0; i < n; ++i) {
      const AstSemaType *item = ast_sema_type_tuple_item(type, i);
      if (!item)
        return 0;
      cur = abi_align_up(cur, abi_type_align(item));
      if (!a64_collect_homogeneous(item, off + cur, h))
        return 0;
      cur += abi_type_size(item);
    }
    return 1;
  }
  return 0;
}

/* AsterASM fixed vectors wider than an architectural short vector are
 * represented at the AAPCS64 boundary as consecutive 128-bit vector pieces.
 * This follows the Arm vector-function ABI model for extended short vectors:
 * a 256/512-bit language vector is a homogeneous sequence of 2/4 short-vector
 * pieces.  Keeping this explicit prevents the backend from inventing a
 * non-existent 256/512-bit AArch64 V register. */
static uint32_t a64_extended_vector_parts(const AstSemaType *type) {
  if (!type || ast_sema_type_kind(type) != AST_SEMA_TYPE_VECTOR)
    return 0u;
  uint64_t size = abi_type_size(type);
  if (size <= 16u || size > 64u || (size & 15u))
    return 0u;
  uint64_t parts = size / 16u;
  return parts >= 2u && parts <= 4u ? (uint32_t)parts : 0u;
}

static A64Homogeneous a64_homogeneous(const AstSemaType *type) {
  A64Homogeneous h;
  memset(&h, 0, sizeof(h));
  if (!abi_is_aggregate(type) || !a64_collect_homogeneous(type, 0u, &h) ||
      h.count == 0u || h.count > 4u)
    return h;
  if (abi_type_size(type) < h.next_offset) {
    memset(&h, 0, sizeof(h));
    return h;
  }
  h.valid = 1u;
  return h;
}

static int a64_contains_scalable(const AstSemaType *type) {
  if (!type)
    return 0;
  if (abi_is_scalable(type))
    return 1;
  if (!abi_is_aggregate(type))
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  if (kind == AST_SEMA_TYPE_ARRAY)
    return a64_contains_scalable(ast_sema_type_element(type));
  if (kind == AST_SEMA_TYPE_TUPLE) {
    uint32_t n = ast_sema_type_tuple_count(type);
    for (uint32_t i = 0; i < n; ++i)
      if (a64_contains_scalable(ast_sema_type_tuple_item(type, i)))
        return 1;
  } else if (kind == AST_SEMA_TYPE_STRUCT) {
    uint32_t n = ast_sema_type_struct_field_count(type);
    for (uint32_t i = 0; i < n; ++i) {
      AstSemaFieldInfo f;
      if (ast_sema_type_struct_field(type, i, &f) &&
          a64_contains_scalable(f.type))
        return 1;
    }
  }
  return 0;
}

static int a64_scalable_leaf(const AstSemaType *type, int *predicate) {
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

static void a64_stack(AstAbiValueInfo *v, uint64_t *stack, uint64_t payload) {
  uint32_t align = abi_type_align(v->type);
  if (align < 8u)
    align = 8u;
  *stack = abi_align_up(*stack, align);
  v->location.kind = AST_ABI_LOCATION_STACK;
  v->location.stack_offset = *stack;
  v->location.stack_alignment = align;
  v->location.stack_size = payload;
  v->payload_size = payload;
  *stack += abi_align_up(payload, 8u);
}

static void a64_reg_name(char buf[AST_ABI_REGISTER_NAME_CAP], char prefix,
                         uint32_t index) {
  (void)snprintf(buf, AST_ABI_REGISTER_NAME_CAP, "%c%u", prefix, index);
}

static int a64_return(AstAbiProgram *p, const AstSemaType *ret,
                      AstAbiValueInfo *out, int *sret, uint32_t function_index,
                      AstMirInstId call_inst) {
  abi_init_value(out, ret, AST_MIR_INVALID_ID, UINT32_MAX);
  out->flags |= AST_ABI_VALUE_RESULT;
  if (!ret || ast_sema_type_kind(ret) == AST_SEMA_TYPE_VOID)
    return 1;
  if (a64_contains_scalable(ret)) {
    int predicate = 0;
    if (p->config.kind != AST_ABI_AAPCS64_SVE ||
        !a64_scalable_leaf(ret, &predicate)) {
      abi_diag(p, AST_ABI_DIAG_UNSUPPORTED_SCALABLE_PCS, AST_DIAG_ERROR,
               (AstSpan){0}, function_index, call_inst,
               "AAPCS64 SVE PCS currently supports only direct scalable vector "
               "and predicate values");
      return 0;
    }
    abi_set_reg(&out->location, 0u, predicate ? "p0" : "z0");
    return 1;
  }
  A64Homogeneous h = a64_homogeneous(ret);
  if (h.valid) {
    out->flags |= h.vector ? AST_ABI_VALUE_HVA : AST_ABI_VALUE_HFA;
    for (uint32_t i = 0; i < h.count; ++i) {
      char r[AST_ABI_REGISTER_NAME_CAP];
      a64_reg_name(r, 'v', i);
      abi_set_reg(&out->location, i, r);
    }
    return 1;
  }
  uint32_t extended_parts = a64_extended_vector_parts(ret);
  if (extended_parts) {
    out->flags |= AST_ABI_VALUE_HVA;
    for (uint32_t i = 0u; i < extended_parts; ++i) {
      char r[AST_ABI_REGISTER_NAME_CAP];
      a64_reg_name(r, 'v', i);
      abi_set_reg(&out->location, i, r);
    }
    return 1;
  }
  if (abi_is_float(ret) || a64_short_vector(ret)) {
    abi_set_reg(&out->location, 0u, "v0");
    return 1;
  }
  uint64_t size = abi_type_size(ret);
  if ((abi_is_integer(ret) || abi_is_aggregate(ret) || abi_is_vector(ret)) &&
      size <= 16u) {
    abi_set_reg(&out->location, 0u, "x0");
    if (size > 8u)
      abi_set_reg(&out->location, 1u, "x1");
    return 1;
  }
  *sret = 1;
  out->location.kind = AST_ABI_LOCATION_INDIRECT;
  out->extension = AST_ABI_EXT_BY_REFERENCE;
  out->flags |= AST_ABI_VALUE_INDIRECT_COPY;
  return 1;
}

int abi_lower_aapcs64(AstAbiProgram *p, const AstSemaType *signature,
                      const AstMirVReg *source_vregs,
                      uint32_t source_vreg_count, uint32_t function_index,
                      AstMirInstId call_inst, AbiSignatureResult *out) {
  memset(out, 0, sizeof(*out));
  uint32_t pc = ast_sema_type_function_param_count(signature);
  if (source_vreg_count != pc)
    return 0;
  for (uint32_t i = 0; i < pc; ++i) {
    const AstSemaType *type = ast_sema_type_function_param(signature, i);
    if (a64_contains_scalable(type) && (p->config.kind != AST_ABI_AAPCS64_SVE ||
                                        !a64_scalable_leaf(type, NULL))) {
      abi_diag(p, AST_ABI_DIAG_UNSUPPORTED_SCALABLE_PCS, AST_DIAG_ERROR,
               (AstSpan){0}, function_index, call_inst,
               "AAPCS64 SVE PCS currently supports only direct scalable vector "
               "and predicate values");
      return 0;
    }
  }
  const AstSemaType *ret = ast_sema_type_function_result(signature);
  int sret = 0;
  if (!a64_return(p, ret, &out->result, &sret, function_index, call_inst))
    return 0;
  out->hidden_count = sret ? 1u : 0u;
  out->argument_count = pc + out->hidden_count;
  out->arguments = (AstAbiValueInfo *)calloc(
      out->argument_count ? out->argument_count : 1u, sizeof(*out->arguments));
  if (!out->arguments)
    return 0;
  if (sret) {
    AstAbiValueInfo *h = &out->arguments[0];
    abi_init_value(h, ret, AST_MIR_INVALID_ID, UINT32_MAX);
    h->flags = AST_ABI_VALUE_HIDDEN | AST_ABI_VALUE_INDIRECT_COPY;
    h->extension = AST_ABI_EXT_BY_REFERENCE;
    h->payload_size = 8u;
    h->alignment = 8u;
    abi_set_reg(&h->location, 0u, "x8");
    p->stats.hidden_sret_count++;
  }
  uint32_t ngrn = 0u, nsrn = 0u, nzrn = 0u, nprn = 0u;
  uint64_t stack = 0u;
  for (uint32_t i = 0; i < pc; ++i) {
    const AstSemaType *type = ast_sema_type_function_param(signature, i);
    AstAbiValueInfo *v = &out->arguments[out->hidden_count + i];
    abi_init_value(v, type, source_vregs ? source_vregs[i] : AST_MIR_INVALID_ID,
                   i);
    uint64_t size = abi_type_size(type);
    if (a64_contains_scalable(type)) {
      int predicate = 0;
      if (!a64_scalable_leaf(type, &predicate) ||
          (predicate ? nprn >= 4u : nzrn >= 8u)) {
        abi_diag(p, AST_ABI_DIAG_UNSUPPORTED_SCALABLE_PCS, AST_DIAG_ERROR,
                 (AstSpan){0}, function_index, call_inst,
                 "AAPCS64 SVE PCS scalable register arguments exceed z0-z7 or "
                 "p0-p3");
        free(out->arguments);
        out->arguments = NULL;
        out->argument_count = 0u;
        return 0;
      }
      char r[AST_ABI_REGISTER_NAME_CAP];
      a64_reg_name(r, predicate ? 'p' : 'z', predicate ? nprn++ : nzrn++);
      abi_set_reg(&v->location, 0u, r);
      continue;
    }
    A64Homogeneous h = a64_homogeneous(type);
    if (h.valid) {
      v->flags |= h.vector ? AST_ABI_VALUE_HVA : AST_ABI_VALUE_HFA;
      if (nsrn + h.count <= 8u) {
        for (uint32_t j = 0; j < h.count; ++j) {
          char r[AST_ABI_REGISTER_NAME_CAP];
          a64_reg_name(r, 'v', nsrn++);
          abi_set_reg(&v->location, j, r);
        }
      } else {
        nsrn = 8u;
        a64_stack(v, &stack, size);
        p->stats.stack_argument_count++;
      }
      continue;
    }
    uint32_t extended_parts = a64_extended_vector_parts(type);
    if (extended_parts) {
      v->flags |= AST_ABI_VALUE_HVA;
      if (nsrn + extended_parts <= 8u) {
        for (uint32_t j = 0u; j < extended_parts; ++j) {
          char r[AST_ABI_REGISTER_NAME_CAP];
          a64_reg_name(r, 'v', nsrn++);
          abi_set_reg(&v->location, j, r);
        }
      } else {
        nsrn = 8u;
        a64_stack(v, &stack, size);
        p->stats.stack_argument_count++;
      }
      continue;
    }
    if (abi_is_float(type) || a64_short_vector(type)) {
      if (nsrn < 8u) {
        char r[AST_ABI_REGISTER_NAME_CAP];
        a64_reg_name(r, 'v', nsrn++);
        abi_set_reg(&v->location, 0u, r);
      } else {
        a64_stack(v, &stack, size < 8u ? 8u : size);
        p->stats.stack_argument_count++;
      }
      continue;
    }
    int indirect =
        (abi_is_aggregate(type) || abi_is_vector(type)) && size > 16u;
    if (indirect) {
      v->flags |= AST_ABI_VALUE_INDIRECT_COPY;
      v->extension = AST_ABI_EXT_BY_REFERENCE;
      v->payload_size = 8u;
      v->alignment = 8u;
      if (ngrn < 8u) {
        char r[AST_ABI_REGISTER_NAME_CAP];
        a64_reg_name(r, 'x', ngrn++);
        abi_set_reg(&v->location, 0u, r);
        v->location.kind = AST_ABI_LOCATION_INDIRECT;
      } else {
        a64_stack(v, &stack, 8u);
        v->location.kind = AST_ABI_LOCATION_INDIRECT;
        p->stats.stack_argument_count++;
      }
      p->stats.indirect_argument_count++;
      continue;
    }
    uint32_t need = size > 8u ? 2u : 1u;
    if (abi_type_align(type) >= 16u && need == 2u && (ngrn & 1u))
      ngrn++;
    if (ngrn + need <= 8u) {
      for (uint32_t j = 0; j < need; ++j) {
        char r[AST_ABI_REGISTER_NAME_CAP];
        a64_reg_name(r, 'x', ngrn++);
        abi_set_reg(&v->location, j, r);
      }
    } else {
      /* C.12/C.13: composites are never split between x7 and the stack. */
      ngrn = 8u;
      a64_stack(v, &stack, size < 8u ? 8u : abi_align_up(size, 8u));
      p->stats.stack_argument_count++;
    }
  }
  out->stack_bytes = abi_align_up(stack, 16u);
  out->stack_alignment = 16u;
  return 1;
}
