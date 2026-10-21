#include "aarch64_internal.h"

static uint64_t value_size(const AstAbiValueInfo *v) {
  uint64_t size = v->type ? ast_sema_type_size(v->type) : 0u;
  return size == UINT64_MAX ? 0u : size;
}
static int aggregate_type(const AstSemaType *type) {
  AstSemaTypeKind kind = type ? ast_sema_type_kind(type) : AST_SEMA_TYPE_VOID;
  return kind == AST_SEMA_TYPE_ARRAY || kind == AST_SEMA_TYPE_TUPLE ||
         kind == AST_SEMA_TYPE_STRUCT || kind == AST_SEMA_TYPE_VECTOR;
}
static uint64_t abi_image_size(const AstAbiValueInfo *v) {
  uint64_t size = value_size(v);
  if (!size || !aggregate_type(v->type))
    return size;
  if (size > UINT64_MAX - 7u)
    return 0u;
  return (size + 7u) & ~UINT64_C(7);
}
static AstPhysicalOperand resize_value(PhysicalContext *c,
                                       AstPhysicalOperand op, uint32_t bits) {
  if (op.kind == AST_PHYSICAL_OPERAND_REGISTER)
    return physical_register(c, op.bank, op.register_unit % 64u, bits);
  op.bit_width = bits;
  return op;
}
static int value_to_image(PhysicalContext *c, const AstAbiValueInfo *v,
                          uint32_t source, uint32_t image) {
  uint64_t size = value_size(v);
  int padded = aggregate_type(v->type);
  if (source >= c->machine->source_vreg_count)
    return 0;
  uint32_t first = c->machine->source_first[source],
           parts = c->machine->source_parts[source];
  uint64_t offset = 0u;
  for (uint32_t part = 0; part < parts && offset < size; ++part) {
    AstPhysicalOperand home = physical_home(c, first + part);
    uint64_t bytes = ((uint64_t)home.bit_width + 7u) / 8u;
    uint64_t payload = bytes > size - offset ? size - offset : bytes;
    if (!padded)
      home = resize_value(c, home, (uint32_t)payload * 8u);
    if (!physical_move(
            c, physical_frame(image, home.bit_width, (int64_t)offset), home))
      return 0;
    offset += payload;
  }
  return offset == size;
}
static int image_to_value(PhysicalContext *c, const AstAbiValueInfo *v,
                          uint32_t image, uint32_t source) {
  uint64_t size = value_size(v);
  int padded = aggregate_type(v->type);
  if (source >= c->machine->source_vreg_count)
    return 0;
  uint32_t first = c->machine->source_first[source],
           parts = c->machine->source_parts[source];
  uint64_t offset = 0u;
  for (uint32_t part = 0; part < parts && offset < size; ++part) {
    AstPhysicalOperand home = physical_home(c, first + part);
    uint64_t bytes =
        ((uint64_t)c->machine->vregs[first + part].bit_width + 7u) / 8u;
    uint64_t payload = bytes > size - offset ? size - offset : bytes;
    if (home.kind) {
      if (!padded)
        home = resize_value(c, home, (uint32_t)payload * 8u);
      if (!physical_move(
              c, home, physical_frame(image, home.bit_width, (int64_t)offset)))
        return 0;
    }
    offset += payload;
  }
  return offset == size;
}
static AstPhysicalOperand abi_piece(PhysicalContext *c,
                                    const AstAbiValueInfo *v, uint32_t piece,
                                    uint32_t bits, int incoming) {
  if (piece < v->location.register_count)
    return physical_named_register(c, v->location.registers[piece], bits);
  int64_t offset = (int64_t)v->location.stack_offset + (int64_t)piece * 8;
  return incoming ? physical_entry(bits, offset)
                  : physical_stack(c, bits, offset);
}
static int direct_value_copies(PhysicalContext *c, const AstAbiValueInfo *v,
                               uint32_t source, int incoming,
                               PhysicalCopy *copies, uint32_t *count) {
  if (v->location.kind == AST_ABI_LOCATION_INDIRECT ||
      source >= c->machine->source_vreg_count ||
      (v->flags & (AST_ABI_VALUE_HFA | AST_ABI_VALUE_HVA)))
    return 0;
  uint32_t first = c->machine->source_first[source],
           parts = c->machine->source_parts[source];
  uint64_t size = value_size(v), offset = 0u;
  if (v->location.register_count && v->location.register_count != parts)
    return 0;
  uint32_t begin = *count;
  for (uint32_t part = 0; part < parts && offset < size; ++part) {
    uint32_t width = c->machine->vregs[first + part].bit_width;
    uint64_t bytes = ((uint64_t)width + 7u) / 8u;
    uint64_t payload = bytes > size - offset ? size - offset : bytes;
    AstPhysicalOperand home = physical_home(c, first + part);
    if (home.kind) {
      if (!aggregate_type(v->type))
        home = resize_value(c, home, (uint32_t)payload * 8u);
      AstPhysicalOperand transport =
          abi_piece(c, v, part, home.bit_width, incoming);
      if (transport.kind == AST_PHYSICAL_OPERAND_REGISTER &&
          home.kind == AST_PHYSICAL_OPERAND_REGISTER) {
        int same_file =
            transport.register_unit / 64u == home.register_unit / 64u;
        if (!same_file) {
          *count = begin;
          return 0;
        }
      }
      copies[*count].dst = incoming ? home : transport;
      copies[*count].src = incoming ? transport : home;
      ++*count;
    }
    offset += payload;
  }
  if (offset != size) {
    *count = begin;
    return 0;
  }
  return 1;
}

static const AstSemaType *homogeneous_base(const AstSemaType *t) {
  if (!t)
    return NULL;
  switch (ast_sema_type_kind(t)) {
  case AST_SEMA_TYPE_ARRAY:
    return homogeneous_base(ast_sema_type_element(t));
  case AST_SEMA_TYPE_TUPLE:
    return homogeneous_base(ast_sema_type_tuple_item(t, 0u));
  case AST_SEMA_TYPE_STRUCT: {
    AstSemaFieldInfo field;
    return ast_sema_type_struct_field(t, 0u, &field)
               ? homogeneous_base(field.type)
               : NULL;
  }
  default:
    return t;
  }
}
static uint64_t abi_register_bytes(const AstAbiValueInfo *v) {
  if (v->flags & (AST_ABI_VALUE_HFA | AST_ABI_VALUE_HVA)) {
    /* Direct fixed vectors wider than 128 bits use the internal extended
       short-vector representation: each ABI V-register carries one
       128-bit piece. Normal HFA/HVA aggregates retain their leaf size. */
    if ((v->flags & AST_ABI_VALUE_HVA) && v->type &&
        ast_sema_type_kind(v->type) == AST_SEMA_TYPE_VECTOR &&
        ast_sema_type_size(v->type) > 16u)
      return 16u;
    const AstSemaType *base = homogeneous_base(v->type);
    return base ? ast_sema_type_size(base) : 0u;
  }
  if (v->location.register_count == 1u && v->location.registers[0][0] == 'v') {
    uint64_t size = value_size(v);
    if (v->type && ast_sema_type_kind(v->type) == AST_SEMA_TYPE_VECTOR)
      return size <= 8u ? 8u : size <= 16u ? 16u : size;
    return size;
  }
  return 8u;
}
static int transfer_image(PhysicalContext *c, const AstAbiValueInfo *v,
                          uint32_t image, int capture, int incoming) {
  uint64_t size = value_size(v), offset = 0u;
  if (!size || size > UINT32_MAX / 8u)
    return 0;
  if (v->alignment > 16u)
    return regalloc_fail(
        c->program, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, c->function,
        c->instruction,
        "AAPCS64 over-aligned arguments need dynamic stack realignment.");
  if (v->location.register_count) {
    uint64_t stride = abi_register_bytes(v);
    if (!stride || stride > 16u)
      return 0;
    int integer_composite =
        aggregate_type(v->type) &&
        !(v->flags & (AST_ABI_VALUE_HFA | AST_ABI_VALUE_HVA));
    for (uint32_t piece = 0;
         piece < v->location.register_count && offset < size; ++piece) {
      uint64_t payload = stride < size - offset ? stride : size - offset;
      int fixed_vector_slot =
          v->type && ast_sema_type_kind(v->type) == AST_SEMA_TYPE_VECTOR &&
          v->location.registers[piece][0] == 'v';
      uint64_t bytes =
          (integer_composite && v->location.registers[piece][0] != 'v') ||
                  fixed_vector_slot
              ? stride
              : payload;
      AstPhysicalOperand reg = physical_named_register(
          c, v->location.registers[piece], (uint32_t)bytes * 8u);
      AstPhysicalOperand mem =
          physical_frame(image, reg.bit_width, (int64_t)offset);
      if (!(capture ? physical_move(c, mem, reg) : physical_move(c, reg, mem)))
        return 0;
      offset += payload;
    }
    /* Homogeneous aggregates can have tail padding. It carries no ABI value;
       initialize the image so no later home reads uninitialized padding. */
    if (capture)
      while (offset < size) {
        uint64_t bytes = size - offset > 8u ? 8u : size - offset;
        if (!physical_move(
                c, physical_frame(image, (uint32_t)bytes * 8u, (int64_t)offset),
                physical_immediate(0u, (uint32_t)bytes * 8u)))
          return 0;
        offset += bytes;
      }
    return 1;
  }
  AstPhysicalOperand mem = physical_frame(image, (uint32_t)size * 8u, 0);
  AstPhysicalOperand stack =
      incoming
          ? physical_entry(mem.bit_width, (int64_t)v->location.stack_offset)
          : physical_stack(c, mem.bit_width, (int64_t)v->location.stack_offset);
  return capture ? physical_move(c, mem, stack) : physical_move(c, stack, mem);
}
static int capture_abi_value(PhysicalContext *c, const AstAbiValueInfo *v,
                             uint32_t image, int incoming) {
  uint64_t size = value_size(v);
  if (!size || size > UINT32_MAX / 8u)
    return 0;
  if (v->location.kind == AST_ABI_LOCATION_INDIRECT) {
    AstPhysicalOperand ptr =
        physical_register(c, AST_MACHINE_BANK_GPR, 16u, 64u);
    AstPhysicalOperand source =
        v->location.register_count
            ? physical_named_register(c, v->location.registers[0], 64u)
        : incoming ? physical_entry(64u, (int64_t)v->location.stack_offset)
                   : physical_stack(c, 64u, (int64_t)v->location.stack_offset);
    if (!physical_move(c, ptr, source))
      return 0;
    ptr.kind = AST_PHYSICAL_OPERAND_MEMORY;
    ptr.bit_width = (uint32_t)size * 8u;
    return physical_move(c, physical_frame(image, ptr.bit_width, 0), ptr);
  }
  return transfer_image(c, v, image, 1, incoming);
}
static int send_abi_image(PhysicalContext *c, const AstAbiValueInfo *v,
                          uint32_t image) {
  return transfer_image(c, v, image, 0, 0);
}
int physical_abi_entry(PhysicalContext *c) {
  AstAbiFunctionInfo info;
  if (!ast_abi_function_info(c->program->machine->abi, c->function, &info))
    return 0;
  uint32_t count = info.argument_count + info.hidden_argument_count;
  uint32_t *images =
      (uint32_t *)malloc((count ? count : 1u) * sizeof(uint32_t));
  PhysicalCopy *copies = (PhysicalCopy *)calloc(
      c->machine->info.vreg_count ? c->machine->info.vreg_count : 1u,
      sizeof(*copies));
  if (!images || !copies) {
    free(images);
    free(copies);
    return 0;
  }
  uint32_t copy_count = 0u;
  for (uint32_t a = 0; a < count; ++a)
    images[a] = AST_MACHINE_INVALID_ID;
  int ok = 0;
  /* Capture all ABI sources before writing any allocator home. This also
     preserves register permutations and deferred mixed-bank aggregates. */
  for (uint32_t a = 0; a < count; ++a) {
    AstAbiValueInfo v;
    if (!ast_abi_function_argument(c->program->machine->abi, c->function, a,
                                   &v))
      goto done;
    if (v.flags & AST_ABI_VALUE_HIDDEN) {
      c->fn->sret_home = physical_temp_object(c, 8u, AST_FRAME_OBJECT_ABI_TEMP);
      if (c->fn->sret_home == AST_MACHINE_INVALID_ID ||
          !physical_move(c, physical_frame(c->fn->sret_home, 64u, 0),
                         abi_piece(c, &v, 0u, 64u, 1)))
        goto done;
      continue;
    }
    if (v.source_vreg >= c->machine->source_vreg_count)
      goto done;
    uint32_t first = c->machine->source_first[v.source_vreg],
             parts = c->machine->source_parts[v.source_vreg];
    int used = 0;
    for (uint32_t part = 0; part < parts; ++part)
      if (c->fn->locations[first + part].kind)
        used = 1;
    if (!used)
      continue;
    if (direct_value_copies(c, &v, v.source_vreg, 1, copies, &copy_count))
      continue;
    images[a] =
        physical_temp_object(c, abi_image_size(&v), AST_FRAME_OBJECT_ABI_TEMP);
    if (images[a] == AST_MACHINE_INVALID_ID ||
        !capture_abi_value(c, &v, images[a], 1))
      goto done;
  }
  /* Homes fed from captured aggregate images can overwrite registers that
     are still sources of direct transfers. Resolve the direct group first. */
  if (!physical_parallel_copy(c, copies, copy_count))
    goto done;
  for (uint32_t a = 0; a < count; ++a)
    if (images[a] != AST_MACHINE_INVALID_ID) {
      AstAbiValueInfo v;
      if (!ast_abi_function_argument(c->program->machine->abi, c->function, a,
                                     &v) ||
          !image_to_value(c, &v, images[a], v.source_vreg))
        goto done;
    }
  ok = 1;
done:
  free(images);
  free(copies);
  return ok;
}

int physical_abi_call(PhysicalContext *c, const MachineInstruction *mi,
                      uint32_t call) {
  AstAbiCallInfo info;
  const AstAbiProgram *abi = c->program->machine->abi;
  if (!ast_abi_call_info(abi, call, &info))
    return 0;
  c->fn->frame.has_calls = 1u;
  if (c->fn->frame.outgoing_size < info.outgoing_stack_bytes)
    c->fn->frame.outgoing_size = info.outgoing_stack_bytes;
  if (c->fn->frame.outgoing_size < info.shadow_space_bytes)
    c->fn->frame.outgoing_size = info.shadow_space_bytes;
  const uint32_t vreg_count = c->machine->info.vreg_count;
  uint32_t *saved =
      (uint32_t *)malloc((vreg_count ? vreg_count : 1u) * sizeof(uint32_t));
  uint32_t count = info.argument_count;
  AstAbiValueInfo first = {0};
  uint32_t hidden = (uint32_t)(ast_abi_call_argument(abi, call, 0u, &first) &&
                               (first.flags & AST_ABI_VALUE_HIDDEN));
  uint32_t *images =
      (uint32_t *)malloc((count ? count : 1u) * sizeof(uint32_t));
  int ok = 0;
  if (!saved || !images)
    goto done;
  for (uint32_t v = 0; v < vreg_count; ++v)
    saved[v] = AST_MACHINE_INVALID_ID;
  uint32_t slot = c->fn->liveness.instruction_slots[c->instruction];
  /* Call-clobbered homes are split at the call boundary. Save slots are
     separate from local scratch slots, so nested copy expansion cannot
     overwrite a value waiting for its post-call reload. */
  for (uint32_t v = 0; v < vreg_count; ++v) {
    AstPhysicalOperand home = physical_home(c, v);
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
    saved[v] = physical_temp_object(c, ((uint64_t)home.bit_width + 7u) / 8u,
                                    AST_FRAME_OBJECT_SPILL);
    if (saved[v] == AST_MACHINE_INVALID_ID ||
        !physical_move(c, physical_frame(saved[v], home.bit_width, 0), home))
      goto done;
  }
  AstPhysicalOperand target = {0};
  uint32_t indirect_target = AST_MACHINE_INVALID_ID;
  for (uint32_t o = 0; o < mi->info.operand_count; ++o) {
    const AstMachineOperand *op = &mi->info.operands[o];
    if (op->kind == AST_MACHINE_OPERAND_SYMBOL) {
      target.kind = AST_PHYSICAL_OPERAND_SYMBOL;
      target.symbol = op->as.symbol;
      target.module = op->module;
    }
  }
  if (mi->info.opcode == AST_MACHINE_A64_CALL_INDIRECT) {
    if (!mi->info.operand_count ||
        mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
      goto done;
    indirect_target = physical_temp_object(c, 8u, AST_FRAME_OBJECT_ABI_TEMP);
    if (indirect_target == AST_MACHINE_INVALID_ID ||
        !physical_move(c, physical_frame(indirect_target, 64u, 0),
                       physical_home(c, mi->info.operands[0].as.vreg)))
      goto done;
  }
  for (uint32_t a = 0; a < count; ++a) {
    AstAbiValueInfo v;
    if (!ast_abi_call_argument(abi, call, hidden + a, &v))
      goto done;
    images[a] =
        physical_temp_object(c, abi_image_size(&v), AST_FRAME_OBJECT_ABI_TEMP);
    if (images[a] == AST_MACHINE_INVALID_ID ||
        !value_to_image(c, &v, v.source_vreg, images[a]))
      goto done;
  }
  uint32_t result_image = AST_MACHINE_INVALID_ID;
  if (info.result.location.kind != AST_ABI_LOCATION_NONE) {
    result_image = physical_temp_object(c, abi_image_size(&info.result),
                                        AST_FRAME_OBJECT_ABI_TEMP);
    if (result_image == AST_MACHINE_INVALID_ID)
      goto done;
  }
  for (uint32_t a = 0; a < count; ++a) {
    AstAbiValueInfo v;
    if (!ast_abi_call_argument(abi, call, hidden + a, &v))
      goto done;
    if (v.location.kind == AST_ABI_LOCATION_INDIRECT) {
      AstPhysicalOperand ptr =
          physical_register(c, AST_MACHINE_BANK_GPR, 16u, 64u);
      AstPhysicalOperand args[] = {ptr, physical_frame(images[a], 64u, 0)};
      if (!physical_emit(c, AST_MACHINE_A64_LEA, 1u, args, 2u) ||
          !physical_move(c, abi_piece(c, &v, 0u, 64u, 0), ptr))
        goto done;
    } else if (!send_abi_image(c, &v, images[a]))
      goto done;
  }
  if (hidden) {
    AstPhysicalOperand ptr =
        physical_register(c, AST_MACHINE_BANK_GPR, 16u, 64u);
    AstPhysicalOperand args[] = {ptr, physical_frame(result_image, 64u, 0)};
    if (!physical_emit(c, AST_MACHINE_A64_LEA, 1u, args, 2u) ||
        !physical_move(c, abi_piece(c, &first, 0u, 64u, 0), ptr))
      goto done;
  }
  if (indirect_target != AST_MACHINE_INVALID_ID) {
    target = physical_register(c, AST_MACHINE_BANK_GPR, 17u, 64u);
    if (!physical_move(c, target, physical_frame(indirect_target, 64u, 0)))
      goto done;
  }
  if (!target.kind || !physical_emit(c, mi->info.opcode, 0u, &target, 1u))
    goto done;
  if (result_image != AST_MACHINE_INVALID_ID) {
    if (!hidden && !capture_abi_value(c, &info.result, result_image, 0))
      goto done;
    if (info.result.source_vreg != AST_MIR_INVALID_ID &&
        !image_to_value(c, &info.result, result_image, info.result.source_vreg))
      goto done;
  }
  for (uint32_t v = 0; v < vreg_count; ++v)
    if (saved[v] != AST_MACHINE_INVALID_ID) {
      AstPhysicalOperand home = physical_home(c, v);
      if (!physical_move(c, home, physical_frame(saved[v], home.bit_width, 0)))
        goto done;
    }
  ok = 1;
done:
  free(saved);
  free(images);
  return ok;
}

int physical_abi_return(PhysicalContext *c, const MachineInstruction *mi) {
  AstAbiFunctionInfo info;
  if (!ast_abi_function_info(c->program->machine->abi, c->function, &info))
    return 0;
  if (mi->info.operand_count) {
    if (mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    uint32_t v = mi->info.operands[0].as.vreg;
    PhysicalCopy copies[AST_ABI_MAX_REGISTERS];
    uint32_t copy_count = 0u;
    if (mi->info.operand_count <= AST_ABI_MAX_REGISTERS &&
        direct_value_copies(c, &info.result, c->machine->vregs[v].source_vreg,
                            0, copies, &copy_count)) {
      return physical_parallel_copy(c, copies, copy_count) &&
             physical_emit_epilogue(c) &&
             physical_emit(c, AST_MACHINE_A64_RET, 0u, NULL, 0u);
    }
    uint32_t image = physical_temp_object(c, abi_image_size(&info.result),
                                          AST_FRAME_OBJECT_ABI_TEMP);
    if (image == AST_MACHINE_INVALID_ID ||
        !value_to_image(c, &info.result, c->machine->vregs[v].source_vreg,
                        image))
      return 0;
    if (info.result.location.kind == AST_ABI_LOCATION_INDIRECT) {
      if (c->fn->sret_home == AST_MACHINE_INVALID_ID)
        return 0;
      AstPhysicalOperand ptr =
          physical_register(c, AST_MACHINE_BANK_GPR, 16u, 64u);
      if (!physical_move(c, ptr, physical_frame(c->fn->sret_home, 64u, 0)))
        return 0;
      AstPhysicalOperand dest = ptr;
      dest.kind = AST_PHYSICAL_OPERAND_MEMORY;
      dest.bit_width = (uint32_t)value_size(&info.result) * 8u;
      if (!physical_move(c, dest, physical_frame(image, dest.bit_width, 0)))
        return 0;
      if ((info.result.flags & AST_ABI_VALUE_RETURN_POINTER_IN_GPR) &&
          !physical_move(c, physical_register(c, AST_MACHINE_BANK_GPR, 0u, 64u),
                         ptr))
        return 0;
    } else if (!send_abi_image(c, &info.result, image))
      return 0;
  }
  return physical_emit_epilogue(c) &&
         physical_emit(c, AST_MACHINE_A64_RET, 0u, NULL, 0u);
}
