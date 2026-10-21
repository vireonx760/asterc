#include "riscv64_internal.h"

typedef struct RvPiece {
  uint64_t offset;
  uint32_t bits, transport_bits, reg;
  const AstSemaType *type;
} RvPiece;
static uint64_t rv_size(const AstAbiValueInfo *v) {
  return v->type ? ast_sema_type_size(v->type) : 0u;
}
static int rv_aggregate_type(const AstSemaType *type) {
  AstSemaTypeKind kind = type ? ast_sema_type_kind(type) : AST_SEMA_TYPE_VOID;
  return kind == AST_SEMA_TYPE_ARRAY || kind == AST_SEMA_TYPE_TUPLE ||
         kind == AST_SEMA_TYPE_STRUCT || kind == AST_SEMA_TYPE_VECTOR;
}
static uint64_t rv_image_size(const AstAbiValueInfo *v) {
  uint64_t size = rv_size(v);
  if (!size || !rv_aggregate_type(v->type))
    return size;
  if (size > UINT64_MAX - 7u)
    return 0u;
  return (size + 7u) & ~UINT64_C(7);
}
static uint32_t rv_pieces(const AstAbiValueInfo *v, RvPiece pieces[8]) {
  uint64_t size = rv_size(v);
  uint32_t count = 0u;
  int fp = 0;
  for (uint32_t n = 0; n < v->location.register_count; ++n)
    if (v->location.registers[n][0] == 'f')
      fp = 1;
  AstSemaTypeKind kind = ast_sema_type_kind(v->type);
  if (fp && (kind == AST_SEMA_TYPE_STRUCT || kind == AST_SEMA_TYPE_TUPLE)) {
    count = kind == AST_SEMA_TYPE_STRUCT
                ? ast_sema_type_struct_field_count(v->type)
                : ast_sema_type_tuple_count(v->type);
    if (count != v->location.register_count || count > 8u)
      return 0;
    uint64_t cursor = 0u;
    for (uint32_t n = 0; n < count; ++n) {
      const AstSemaType *t;
      uint64_t offset;
      if (kind == AST_SEMA_TYPE_STRUCT) {
        AstSemaFieldInfo field;
        if (!ast_sema_type_struct_field(v->type, n, &field))
          return 0;
        t = field.type;
        offset = field.offset;
      } else {
        t = ast_sema_type_tuple_item(v->type, n);
        uint32_t a = ast_sema_type_alignment(t);
        if (!a)
          return 0;
        cursor = (cursor + a - 1u) & ~((uint64_t)a - 1u);
        offset = cursor;
        cursor += ast_sema_type_size(t);
      }
      uint64_t bytes = ast_sema_type_size(t);
      if (!bytes || bytes > 8u || offset + bytes > size)
        return 0;
      pieces[n] =
          (RvPiece){offset, (uint32_t)bytes * 8u, (uint32_t)bytes * 8u, n, t};
    }
    return count;
  }
  int aggregate = rv_aggregate_type(v->type);
  for (uint64_t off = 0; off < size;) {
    uint64_t bytes = size - off;
    if (bytes > 8u)
      bytes = 8u;
    if (count == 8u)
      return 0;
    pieces[count] = (RvPiece){off, (uint32_t)bytes * 8u,
                              aggregate ? 64u : (uint32_t)bytes * 8u, count,
                              count == 0u ? v->type : NULL};
    off += bytes;
    ++count;
  }
  return count;
}
static AstPhysicalOperand rv_transport(RvContext *c, const AstAbiValueInfo *v,
                                       const RvPiece *piece, int entry) {
  if (piece->reg < v->location.register_count)
    return rv_named(c, v->location.registers[piece->reg],
                    piece->transport_bits);
  uint64_t stack_piece = piece->offset;
  if (v->location.kind == AST_ABI_LOCATION_SPLIT_REG_STACK)
    stack_piece -= (uint64_t)v->location.register_count * 8u;
  int64_t off = (int64_t)(v->location.stack_offset + stack_piece);
  return entry ? physical_entry(piece->transport_bits, off)
               : rv_stack(c, piece->transport_bits, off);
}
static int rv_image_value(RvContext *c, const AstAbiValueInfo *v,
                          uint32_t image, uint32_t source, int incoming) {
  uint64_t size = rv_size(v);
  int padded = rv_aggregate_type(v->type);
  if (source >= c->machine->source_vreg_count)
    return 0;
  uint32_t first = c->machine->source_first[source],
           parts = c->machine->source_parts[source];
  uint64_t offset = 0u;
  for (uint32_t part = 0; part < parts && offset < size; ++part) {
    uint32_t bits = c->machine->vregs[first + part].bit_width;
    uint64_t bytes = ((uint64_t)bits + 7u) / 8u;
    uint64_t payload = bytes > size - offset ? size - offset : bytes;
    AstPhysicalOperand home = rv_home(c, first + part);
    if (home.kind) {
      if (!padded)
        home.bit_width = (uint32_t)payload * 8u;
      AstPhysicalOperand memory =
          physical_frame(image, home.bit_width, (int64_t)offset);
      if (!rv_move(c, incoming ? home : memory, incoming ? memory : home))
        return 0;
    } else if (!incoming)
      return 0;
    offset += payload;
  }
  return offset == size;
}
static int rv_direct_u64_operands(RvContext *c, const AstAbiValueInfo *v,
                                  uint32_t source, int incoming,
                                  AstPhysicalOperand *dst,
                                  AstPhysicalOperand *src) {
  if (!c || !v || !dst || !src || (v->flags & AST_ABI_VALUE_HIDDEN) ||
      v->location.kind == AST_ABI_LOCATION_INDIRECT ||
      v->location.register_count != 1u ||
      source >= c->machine->source_vreg_count || rv_size(v) != 8u)
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(v->type);
  if (kind != AST_SEMA_TYPE_INT && kind != AST_SEMA_TYPE_BITS &&
      kind != AST_SEMA_TYPE_PTR && kind != AST_SEMA_TYPE_CAP &&
      kind != AST_SEMA_TYPE_STRING && kind != AST_SEMA_TYPE_BYTE_STRING &&
      kind != AST_SEMA_TYPE_C_STRING)
    return 0;
  uint32_t first = c->machine->source_first[source],
           parts = c->machine->source_parts[source];
  if (parts != 1u || c->machine->vregs[first].bit_width != 64u)
    return 0;
  AstPhysicalOperand home = rv_home(c, first);
  if (!home.kind) {
    memset(dst, 0, sizeof(*dst));
    memset(src, 0, sizeof(*src));
    return 1;
  }
  AstPhysicalOperand reg = rv_named(c, v->location.registers[0], 64u);
  if (!reg.kind || reg.bank != AST_MACHINE_BANK_GPR)
    return 0;
  *dst = incoming ? home : reg;
  *src = incoming ? reg : home;
  return 1;
}
static int rv_direct_u64_value(RvContext *c, const AstAbiValueInfo *v,
                               uint32_t source, int incoming) {
  AstPhysicalOperand dst = {0}, src = {0};
  if (!rv_direct_u64_operands(c, v, source, incoming, &dst, &src))
    return 0;
  return !dst.kind || rv_move(c, dst, src);
}

static void rv_exclude_abi(RvWork *w) {
  for (uint32_t r = 10u; r <= 17u; ++r) {
    w->excluded[MACHINE_PHYSREG_FILE_GPR * 64u + r] = 1u;
    w->excluded[MACHINE_PHYSREG_FILE_FP_VECTOR * 64u + r] = 1u;
  }
}
static int rv_capture(RvContext *c, const AstAbiValueInfo *v, uint32_t image,
                      int entry) {
  uint64_t size = rv_size(v);
  if (!size || size > UINT32_MAX / 8u)
    return 0;
  if (v->location.kind == AST_ABI_LOCATION_INDIRECT) {
    RvWork w = {0};
    rv_exclude_abi(&w);
    AstPhysicalOperand ptr = rv_borrow(c, &w, AST_MACHINE_BANK_GPR, 64u);
    AstPhysicalOperand source =
        v->location.register_count ? rv_named(c, v->location.registers[0], 64u)
        : entry ? physical_entry(64u, (int64_t)v->location.stack_offset)
                : rv_stack(c, 64u, (int64_t)v->location.stack_offset);
    return ptr.kind && rv_move(c, ptr, source) &&
           rv_move(c, physical_frame(image, (uint32_t)size * 8u, 0),
                   rv_memory(ptr, (uint32_t)size * 8u, 0)) &&
           rv_restore(c, &w);
  }
  RvPiece pieces[8];
  uint32_t count = rv_pieces(v, pieces);
  if (!count)
    return 0;
  /* Initialize compiler-owned padding before assembling ABI slots. */
  uint64_t image_size = rv_image_size(v);
  for (uint64_t off = 0; off < image_size; off += 8u) {
    uint64_t n = image_size - off;
    if (n > 8u)
      n = 8u;
    if (!rv_move(c, physical_frame(image, (uint32_t)n * 8u, (int64_t)off),
                 physical_immediate(0u, (uint32_t)n * 8u)))
      return 0;
  }
  for (uint32_t n = 0; n < count; ++n) {
    AstPhysicalOperand src = rv_transport(c, v, &pieces[n], entry);
    if (!rv_move(c,
                 physical_frame(image, pieces[n].transport_bits,
                                (int64_t)pieces[n].offset),
                 src))
      return 0;
  }
  return 1;
}
static int rv_send(RvContext *c, const AstAbiValueInfo *v, uint32_t image) {
  RvWork w = {0};
  rv_exclude_abi(&w);
  if (v->location.kind == AST_ABI_LOCATION_INDIRECT) {
    AstPhysicalOperand ptr = rv_borrow(c, &w, AST_MACHINE_BANK_GPR, 64u);
    AstPhysicalOperand dst =
        v->location.register_count
            ? rv_named(c, v->location.registers[0], 64u)
            : rv_stack(c, 64u, (int64_t)v->location.stack_offset);
    return ptr.kind && rv_address(c, ptr, physical_frame(image, 64u, 0)) &&
           rv_move(c, dst, ptr) && rv_restore(c, &w);
  }
  RvPiece pieces[8];
  uint32_t count = rv_pieces(v, pieces);
  if (!count)
    return 0;
  for (uint32_t n = 0; n < count; ++n) {
    AstPhysicalOperand dst = rv_transport(c, v, &pieces[n], 0);
    AstPhysicalOperand src = physical_frame(image, pieces[n].transport_bits,
                                            (int64_t)pieces[n].offset);
    const AstSemaType *type = pieces[n].type;
    AstSemaTypeKind kind = type ? ast_sema_type_kind(type) : AST_SEMA_TYPE_VOID;
    if (dst.kind == AST_PHYSICAL_OPERAND_REGISTER &&
        dst.bank == AST_MACHINE_BANK_GPR && type &&
        (kind == AST_SEMA_TYPE_INT || kind == AST_SEMA_TYPE_BOOL) &&
        ast_sema_type_bit_width(type) < 64u) {
      uint32_t bits = ast_sema_type_bit_width(type);
      if (!bits)
        bits = pieces[n].bits;
      dst.bit_width = 64u;
      if (!rv_move(c, dst, src) ||
          !rv_normalize(c, dst, dst, bits, ast_sema_type_is_signed(type)))
        return 0;
      AstPhysicalOperand args[] = {dst, dst, physical_immediate(0u, 64u)};
      if (!rv_emit(c, AST_MACHINE_RISCV_ADDIW, 1u, args, 3u))
        return 0;
    } else if (!rv_move(c, dst, src))
      return 0;
  }
  return 1;
}
int rv_entry(RvContext *c) {
  AstAbiFunctionInfo info;
  if (!ast_abi_function_info(c->program->machine->abi, c->function, &info))
    return 0;
  uint32_t count = info.argument_count + info.hidden_argument_count;
  uint32_t *images =
      (uint32_t *)malloc((count ? count : 1u) * sizeof(uint32_t));
  if (!images)
    return 0;
  for (uint32_t a = 0; a < count; ++a)
    images[a] = AST_MACHINE_INVALID_ID;
  int ok = 0;
  if (count && info.hidden_argument_count == 0u) {
    PhysicalCopy *direct = (PhysicalCopy *)calloc(count, sizeof(*direct));
    if (!direct)
      goto done;
    int eligible = 1, hazard = 0;
    for (uint32_t a = 0; a < count; ++a) {
      AstAbiValueInfo v;
      if (!ast_abi_function_argument(c->program->machine->abi, c->function, a,
                                     &v) ||
          !rv_direct_u64_operands(c, &v, v.source_vreg, 1, &direct[a].dst,
                                  &direct[a].src)) {
        eligible = 0;
        break;
      }
    }
    if (eligible)
      for (uint32_t i = 0; i < count && !hazard; ++i) {
        if (direct[i].dst.kind != AST_PHYSICAL_OPERAND_REGISTER ||
            physical_same_location(&direct[i].dst, &direct[i].src))
          continue;
        for (uint32_t j = 0; j < count; ++j) {
          if (i == j || direct[j].src.kind != AST_PHYSICAL_OPERAND_REGISTER ||
              !direct[j].dst.kind)
            continue;
          if (direct[i].dst.register_unit == direct[j].src.register_unit) {
            hazard = 1;
            break;
          }
        }
      }
    if (eligible && !hazard) {
      ok = 1;
      for (uint32_t a = 0; a < count; ++a)
        if (direct[a].dst.kind &&
            !physical_same_location(&direct[a].dst, &direct[a].src) &&
            !rv_move(c, direct[a].dst, direct[a].src)) {
          ok = 0;
          break;
        }
      free(direct);
      if (ok)
        goto done;
    } else
      free(direct);
  }
  for (uint32_t a = 0; a < count; ++a) {
    AstAbiValueInfo v;
    if (!ast_abi_function_argument(c->program->machine->abi, c->function, a,
                                   &v))
      goto done;
    if (v.flags & AST_ABI_VALUE_HIDDEN) {
      c->fn->sret_home = rv_temp(c, 8u, AST_FRAME_OBJECT_ABI_TEMP);
      if (c->fn->sret_home == AST_MACHINE_INVALID_ID ||
          !v.location.register_count ||
          !rv_move(c, physical_frame(c->fn->sret_home, 64u, 0),
                   rv_named(c, v.location.registers[0], 64u)))
        goto done;
      continue;
    }
    images[a] = rv_temp(c, rv_image_size(&v), AST_FRAME_OBJECT_ABI_TEMP);
    if (images[a] == AST_MACHINE_INVALID_ID || !rv_capture(c, &v, images[a], 1))
      goto done;
  }
  for (uint32_t a = 0; a < count; ++a)
    if (images[a] != AST_MACHINE_INVALID_ID) {
      AstAbiValueInfo v;
      if (!ast_abi_function_argument(c->program->machine->abi, c->function, a,
                                     &v) ||
          !rv_image_value(c, &v, images[a], v.source_vreg, 1))
        goto done;
    }
  ok = 1;
done:
  free(images);
  return ok;
}
int rv_call(RvContext *c, const MachineInstruction *mi, uint32_t call) {
  const AstAbiProgram *abi = c->program->machine->abi;
  AstAbiCallInfo info;
  if (!ast_abi_call_info(abi, call, &info))
    return 0;
  c->fn->frame.has_calls = 1u;
  if (c->fn->frame.outgoing_size < info.outgoing_stack_bytes)
    c->fn->frame.outgoing_size = info.outgoing_stack_bytes;
  uint32_t nvalues = c->machine->info.vreg_count, count = info.argument_count;
  uint32_t *saved =
      (uint32_t *)malloc((nvalues ? nvalues : 1u) * sizeof(uint32_t));
  uint32_t *images =
      (uint32_t *)malloc((count ? count : 1u) * sizeof(uint32_t));
  if (!saved || !images) {
    free(saved);
    free(images);
    return 0;
  }
  for (uint32_t v = 0; v < nvalues; ++v)
    saved[v] = AST_MACHINE_INVALID_ID;
  int ok = 0;
  uint32_t slot = c->fn->liveness.instruction_slots[c->instruction];
  for (uint32_t v = 0; v < nvalues; ++v) {
    AstPhysicalOperand home = rv_home(c, v);
    MachinePhysRegView view;
    if (home.kind != AST_PHYSICAL_OPERAND_REGISTER ||
        !machine_interval_covers(&c->fn->liveness.intervals[v], slot) ||
        !machine_interval_covers(&c->fn->liveness.intervals[v], slot + 1u))
      continue;
    if (!machine_physreg_view(&c->program->target, home.register_name, &view))
      goto done;
    if (machine_physreg_preserved_bits(c->program->abi, &view) >=
        home.bit_width)
      continue;
    saved[v] = rv_temp(c, (home.bit_width + 7u) / 8u, AST_FRAME_OBJECT_SPILL);
    if (saved[v] == AST_MACHINE_INVALID_ID ||
        !rv_move(c, physical_frame(saved[v], home.bit_width, 0), home))
      goto done;
  }
  AstPhysicalOperand target = {0};
  uint32_t target_image = AST_MACHINE_INVALID_ID;
  if (mi->info.opcode == AST_MACHINE_RISCV_CALL_INDIRECT) {
    if (!mi->info.operand_count ||
        mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
      goto done;
    target_image = rv_temp(c, 8u, AST_FRAME_OBJECT_ABI_TEMP);
    if (target_image == AST_MACHINE_INVALID_ID ||
        !rv_move(c, physical_frame(target_image, 64u, 0),
                 rv_home(c, mi->info.operands[0].as.vreg)))
      goto done;
  } else
    for (uint32_t n = 0; n < mi->info.operand_count; ++n)
      if (mi->info.operands[n].kind == AST_MACHINE_OPERAND_SYMBOL) {
        target.kind = AST_PHYSICAL_OPERAND_SYMBOL;
        target.symbol = mi->info.operands[n].as.symbol;
        target.module = mi->info.operands[n].module;
      }
  AstAbiValueInfo hidden_value = {0};
  uint32_t hidden =
      (uint32_t)(ast_abi_call_argument(abi, call, 0u, &hidden_value) &&
                 (hidden_value.flags & AST_ABI_VALUE_HIDDEN));
  for (uint32_t a = 0; a < count; ++a) {
    AstAbiValueInfo v;
    if (!ast_abi_call_argument(abi, call, a + hidden, &v))
      goto done;
    images[a] = rv_temp(c, rv_image_size(&v), AST_FRAME_OBJECT_ABI_TEMP);
    if (images[a] == AST_MACHINE_INVALID_ID ||
        !rv_image_value(c, &v, images[a], v.source_vreg, 0))
      goto done;
  }
  uint32_t result_image = AST_MACHINE_INVALID_ID;
  if (info.result.location.kind != AST_ABI_LOCATION_NONE) {
    result_image =
        rv_temp(c, rv_image_size(&info.result), AST_FRAME_OBJECT_ABI_TEMP);
    if (result_image == AST_MACHINE_INVALID_ID)
      goto done;
  }
  for (uint32_t a = 0; a < count; ++a) {
    AstAbiValueInfo v;
    if (!ast_abi_call_argument(abi, call, a + hidden, &v) ||
        !rv_send(c, &v, images[a]))
      goto done;
  }
  if (hidden) {
    AstPhysicalOperand pointer =
        rv_named(c, hidden_value.location.registers[0], 64u);
    if (!pointer.kind ||
        !rv_address(c, pointer, physical_frame(result_image, 64u, 0)))
      goto done;
  }
  RvWork work = {0};
  rv_exclude_abi(&work);
  if (target_image != AST_MACHINE_INVALID_ID) {
    target = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
    if (!target.kind ||
        !rv_move(c, target, physical_frame(target_image, 64u, 0)))
      goto done;
  }
  if (!target.kind || !rv_emit(c, mi->info.opcode, 0u, &target, 1u) ||
      !rv_restore(c, &work))
    goto done;
  if (result_image != AST_MACHINE_INVALID_ID) {
    if (!hidden && !rv_capture(c, &info.result, result_image, 0))
      goto done;
    if (info.result.source_vreg != AST_MIR_INVALID_ID &&
        !rv_image_value(c, &info.result, result_image, info.result.source_vreg,
                        1))
      goto done;
  }
  for (uint32_t v = 0; v < nvalues; ++v)
    if (saved[v] != AST_MACHINE_INVALID_ID) {
      AstPhysicalOperand home = rv_home(c, v);
      if (!rv_move(c, home, physical_frame(saved[v], home.bit_width, 0)))
        goto done;
    }
  ok = 1;
done:
  free(saved);
  free(images);
  return ok;
}
int rv_return(RvContext *c, const MachineInstruction *mi) {
  AstAbiFunctionInfo info;
  if (!ast_abi_function_info(c->program->machine->abi, c->function, &info))
    return 0;
  if (mi->info.operand_count) {
    if (mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    uint32_t source =
        c->machine->vregs[mi->info.operands[0].as.vreg].source_vreg;
    uint64_t size = rv_size(&info.result);
    if (rv_direct_u64_value(c, &info.result, source, 0))
      return rv_epilogue(c) && rv_emit(c, AST_MACHINE_RISCV_RET, 0u, NULL, 0u);
    uint32_t image = rv_temp(c, size, AST_FRAME_OBJECT_ABI_TEMP);
    if (image == AST_MACHINE_INVALID_ID ||
        !rv_image_value(c, &info.result, image, source, 0))
      return 0;
    if (info.result.location.kind == AST_ABI_LOCATION_INDIRECT) {
      if (c->fn->sret_home == AST_MACHINE_INVALID_ID)
        return 0;
      RvWork work = {0};
      rv_exclude_abi(&work);
      AstPhysicalOperand ptr = rv_borrow(c, &work, AST_MACHINE_BANK_GPR, 64u);
      if (!ptr.kind ||
          !rv_move(c, ptr, physical_frame(c->fn->sret_home, 64u, 0)) ||
          !rv_move(c, rv_memory(ptr, (uint32_t)size * 8u, 0),
                   physical_frame(image, (uint32_t)size * 8u, 0)) ||
          !rv_restore(c, &work))
        return 0;
      /* LP64 psABI does not require returning the sret pointer in a0. */
    } else if (!rv_send(c, &info.result, image))
      return 0;
  }
  return rv_epilogue(c) && rv_emit(c, AST_MACHINE_RISCV_RET, 0u, NULL, 0u);
}
