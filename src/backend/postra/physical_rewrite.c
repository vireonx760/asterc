#include "../common/div_cost.h"
#include "../common/div_magic.h"
#include "physical_internal.h"
#include <limits.h>

typedef struct WorkRegisters {
  uint8_t excluded[MACHINE_REGISTER_UNITS];
  AstPhysicalOperand saved[AST_PHYSICAL_MAX_OPERANDS];
  uint32_t count;
} WorkRegisters;

static int fail_instruction(PhysicalContext *c, const char *message) {
  return regalloc_fail(c->program, AST_REGALLOC_DIAG_UNSUPPORTED_INSTRUCTION,
                       c->function, c->instruction, message);
}
static int physical_x86_late_cfg_cleanup(PhysicalContext *c);
static uint32_t physical_current_slot(const PhysicalContext *c) {
  if (!c || !c->fn)
    return UINT32_MAX;
  if (c->instruction < c->machine->info.instruction_count)
    return c->fn->liveness.instruction_slots[c->instruction];
  if (c->block < c->fn->liveness.block_count)
    return c->fn->liveness.block_begin[c->block];
  return UINT32_MAX;
}
static int borrow_exact(PhysicalContext *c, WorkRegisters *w,
                        AstPhysicalOperand r) {
  if (!r.kind || r.register_unit >= MACHINE_REGISTER_UNITS ||
      w->count >= AST_PHYSICAL_MAX_OPERANDS)
    return 0;
  uint32_t bits = r.bank == AST_MACHINE_BANK_GPR ? 64u
                  : r.bit_width < 128u           ? 128u
                                                 : r.bit_width;
  for (uint32_t v = 0; v < c->machine->info.vreg_count; ++v)
    if (c->fn->locations[v].kind == AST_REGALLOC_LOCATION_REGISTER &&
        c->fn->locations[v].register_unit == r.register_unit &&
        c->fn->locations[v].bit_width > bits)
      bits = c->fn->locations[v].bit_width;
  r = physical_register(c, r.bank, r.register_unit % 64u, bits);
  uint32_t unit = r.register_unit;
  w->excluded[unit] = 1u;

  /* A post-RA temporary only needs preservation when the allocator has a
     value live in this alias unit at the current machine slot.  The old
     policy unconditionally saved/restored every borrowed GPR (especially
     r11), creating stack traffic even for provably dead scratch registers. */
  uint32_t slot = physical_current_slot(c);
  int allocator_live =
      slot == UINT32_MAX || regalloc_unit_live_at(c->fn, unit, slot);
  MachinePhysRegView abi_view;
  uint32_t preserved = 0u;
  if (machine_physreg_view(&c->program->target, r.register_name, &abi_view))
    preserved = machine_physreg_preserved_bits(c->program->abi, &abi_view);
  /* вЂњDead to the allocatorвЂќ is not enough for an ABI-preserved register:
     its incoming caller value is still live across the whole function.  If
     the normal prologue already owns a callee-save slot, it protects that
     value; otherwise a temporary borrow must save/restore locally. */
  int abi_unprotected =
      preserved && c->fn->callee_save_objects[unit] == AST_MACHINE_INVALID_ID;
  if (!allocator_live && !abi_unprotected)
    return 1;

  if (c->fn->work_objects[unit] == AST_MACHINE_INVALID_ID)
    c->fn->work_objects[unit] = physical_temp_object(
        c, ((uint64_t)bits + 7u) / 8u, AST_FRAME_OBJECT_EMERGENCY);
  else if (c->fn->objects[c->fn->work_objects[unit]].size <
           ((uint64_t)bits + 7u) / 8u)
    c->fn->objects[c->fn->work_objects[unit]].size = ((uint64_t)bits + 7u) / 8u;
  uint32_t object = c->fn->work_objects[unit];
  if (object == AST_MACHINE_INVALID_ID ||
      !physical_move(c, physical_frame(object, bits, 0), r))
    return 0;
  w->saved[w->count++] = r;
  return 1;
}
static AstPhysicalOperand borrow_register(PhysicalContext *c, WorkRegisters *w,
                                          AstMachineRegBank bank,
                                          uint32_t bits) {
  static const uint32_t gprs[] = {11u, 10u, 0u, 1u,  2u,  8u,  9u,
                                  6u,  7u,  3u, 12u, 13u, 14u, 15u};
  AstPhysicalOperand invalid = {0};
  for (uint32_t i = 0; i < (bank == AST_MACHINE_BANK_GPR ? 14u : 16u); ++i) {
    uint32_t index = bank == AST_MACHINE_BANK_GPR ? gprs[i] : 15u - i;
    AstPhysicalOperand r = physical_register(c, bank, index, bits);
    if (r.kind && !w->excluded[r.register_unit]) {
      if (!borrow_exact(c, w, r))
        return invalid;
      return r;
    }
  }
  return invalid;
}
static int restore_work(PhysicalContext *c, WorkRegisters *w) {
  while (w->count) {
    AstPhysicalOperand r = w->saved[--w->count];
    if (!physical_move(c, r,
                       physical_frame(c->fn->work_objects[r.register_unit],
                                      r.bit_width, 0)))
      return 0;
  }
  return 1;
}
static AstPhysicalOperand ensure_register(PhysicalContext *c, WorkRegisters *w,
                                          AstPhysicalOperand value,
                                          AstMachineRegBank bank) {
  if (value.kind == AST_PHYSICAL_OPERAND_REGISTER)
    return value;
  AstPhysicalOperand r = borrow_register(c, w, bank, value.bit_width);
  if (!r.kind || !physical_move(c, r, value)) {
    AstPhysicalOperand invalid = {0};
    return invalid;
  }
  return r;
}
static uint32_t add_block(PhysicalContext *c) {
  if (c->fn->block_count == c->fn->block_capacity) {
    uint32_t cap = c->fn->block_capacity ? c->fn->block_capacity * 2u : 8u;
    size_t bytes;
    if (cap < c->fn->block_capacity ||
        ast_size_mul_overflow(cap, sizeof(*c->fn->blocks), &bytes))
      return AST_MACHINE_INVALID_ID;
    PhysicalBlock *next = (PhysicalBlock *)realloc(c->fn->blocks, bytes);
    if (!next)
      return AST_MACHINE_INVALID_ID;
    memset(next + c->fn->block_capacity, 0,
           (size_t)(cap - c->fn->block_capacity) * sizeof(*next));
    c->fn->blocks = next;
    c->fn->block_capacity = cap;
  }
  return c->fn->block_count++;
}
static int lower_branch(PhysicalContext *c, const MachineInstruction *mi) {
  AstPhysicalOperand args[3] = {{0}};
  uint32_t cursor = 0u, count = 0u;
  WorkRegisters work = {0};
  int conditional = mi->info.opcode == AST_MACHINE_X86_COND_BR;
  if (conditional) {
    if (!mi->info.operand_count ||
        mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    AstPhysicalOperand condition =
        physical_home(c, mi->info.operands[0].as.vreg);
    /* Branch conditions can read their stack home directly. This avoids
       a scratch restoration after a terminator. */
    args[count++] = condition;
    cursor = 1u;
  }
  uint32_t predecessor = c->block;
  for (uint32_t edge = 0; edge < (conditional ? 2u : 1u); ++edge) {
    if (cursor >= mi->info.operand_count ||
        mi->info.operands[cursor].kind != AST_MACHINE_OPERAND_BLOCK)
      return 0;
    uint32_t target = mi->info.operands[cursor++].as.block;
    const MachineBlock *successor = &c->machine->blocks[target];
    uint32_t n = successor->info.argument_count;
    if (n > mi->info.operand_count - cursor)
      return 0;
    size_t copy_capacity = (size_t)n + c->machine->info.vreg_count;
    if (copy_capacity < n)
      return 0;
    PhysicalCopy *copies = (PhysicalCopy *)calloc(
        copy_capacity ? copy_capacity : 1u, sizeof(*copies));
    if (!copies)
      return 0;
    uint32_t used = 0u;
    for (uint32_t a = 0; a < n; ++a) {
      AstPhysicalOperand dst = physical_home_at_slot(
          c, successor->arguments[a], c->fn->liveness.block_begin[target]);
      if (!dst.kind)
        continue;
      if (mi->info.operands[cursor + a].kind != AST_MACHINE_OPERAND_VREG) {
        free(copies);
        return 0;
      }
      copies[used].dst = dst;
      copies[used].src =
          physical_home(c, mi->info.operands[cursor + a].as.vreg);
      ++used;
    }
    cursor += n;
    /* A split home also needs a transition for values that flow through
       the edge without being block arguments. */
    for (uint32_t v = 0; v < c->machine->info.vreg_count; ++v) {
      if (!machine_liveness_test_bit(
              &c->fn->liveness.blocks[predecessor].live_out, v) ||
          !machine_liveness_test_bit(&c->fn->liveness.blocks[target].live_in,
                                     v))
        continue;
      AstPhysicalOperand src = physical_home(c, v);
      AstPhysicalOperand dst =
          physical_home_at_slot(c, v, c->fn->liveness.block_begin[target]);
      if (!src.kind || !dst.kind || physical_same_location(&src, &dst))
        continue;
      if ((size_t)used >= copy_capacity) {
        free(copies);
        return 0;
      }
      copies[used++] = (PhysicalCopy){dst, src};
    }
    uint32_t branch_target = target;
    if (used) {
      /* All conditional transfers belong to their own edge block, even
         if both branches name the same successor with different values. */
      if (conditional) {
        branch_target = add_block(c);
        if (branch_target == AST_MACHINE_INVALID_ID) {
          free(copies);
          return 0;
        }
        c->block = branch_target;
        c->program->stats.edge_blocks++;
      }
      int ok = physical_parallel_copy(c, copies, used);
      if (conditional && ok) {
        AstPhysicalOperand dest = {0};
        dest.kind = AST_PHYSICAL_OPERAND_BLOCK;
        dest.block = target;
        ok = physical_emit(c, AST_MACHINE_X86_BR, 0u, &dest, 1u);
      }
      c->block = predecessor;
      if (!ok) {
        free(copies);
        return 0;
      }
    }
    free(copies);
    args[count].kind = AST_PHYSICAL_OPERAND_BLOCK;
    args[count++].block = branch_target;
  }
  return cursor == mi->info.operand_count && restore_work(c, &work) &&
         physical_emit(c, mi->info.opcode, 0u, args, count);
}
static int lower_constant(PhysicalContext *c, const MachineInstruction *mi) {
  AstMirInstructionInfo source = {0};
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  if (mi->info.source_instruction != AST_MIR_INVALID_ID &&
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source))
    return 0;
  for (uint32_t part = 0; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    AstPhysicalOperand dst = physical_home(c, v);
    if (!dst.kind || dst.kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
      continue;
    uint64_t bits = part == 0u   ? source.constant.lo
                    : part == 1u ? source.constant.hi
                                 : 0u;
    if (source.constant.is_negative) {
      if (!part)
        bits = UINT64_C(0) - bits;
      else
        bits = ~bits + (source.constant.lo == 0u ? 1u : 0u);
    }
    if (mi->info.opcode == AST_MACHINE_X86_UNDEF)
      bits = 0u;
    if (dst.bit_width > 64u)
      return fail_instruction(
          c, "Wide constants need source-aware lane expansion.");
    if (c->machine->vregs[v].bank == AST_MACHINE_BANK_FP) {
      if (dst.bit_width == 32u) {
        float value = (float)source.constant.floating;
        uint32_t raw;
        memcpy(&raw, &value, sizeof(raw));
        bits = raw;
      } else if (dst.bit_width == 64u)
        memcpy(&bits, &source.constant.floating, sizeof(bits));
      else
        return fail_instruction(
            c, "This floating format needs explicit constant conversion.");
      uint32_t image = physical_temp_object(c, dst.bit_width / 8u,
                                            AST_FRAME_OBJECT_ABI_TEMP);
      if (image == AST_MACHINE_INVALID_ID ||
          !physical_move(c, physical_frame(image, dst.bit_width, 0),
                         physical_immediate(bits, dst.bit_width)) ||
          !physical_move(c, dst, physical_frame(image, dst.bit_width, 0)))
        return 0;
      continue;
    }
    if (!physical_move(c, dst, physical_immediate(bits, dst.bit_width)))
      return 0;
  }
  return 1;
}
static int lower_copy(PhysicalContext *c, const MachineInstruction *mi) {
  if (mi->info.result_count != mi->info.operand_count)
    return 0;
  PhysicalCopy local[AST_PHYSICAL_MAX_OPERANDS];
  int heap = mi->info.result_count > AST_PHYSICAL_MAX_OPERANDS;
  PhysicalCopy *copies =
      heap ? (PhysicalCopy *)malloc((size_t)mi->info.result_count *
                                    sizeof(*copies))
           : local;
  if (!copies)
    return 0;
  uint32_t count = 0u;
  for (uint32_t i = 0; i < mi->info.result_count; ++i) {
    AstPhysicalOperand dst = physical_home(c, mi->info.result_first + i);
    if (!dst.kind)
      continue;
    if (mi->info.operands[i].kind != AST_MACHINE_OPERAND_VREG) {
      if (heap)
        free(copies);
      return 0;
    }
    copies[count].dst = dst;
    copies[count++].src = physical_home(c, mi->info.operands[i].as.vreg);
  }
  int ok = physical_parallel_copy(c, copies, count);
  if (heap)
    free(copies);
  return ok;
}

static int source_constant_bits(PhysicalContext *c, AstMirVReg vreg,
                                uint64_t *out) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirVRegInfo vi;
  AstMirInstructionInfo def;
  if (!mf || !out || !ast_mir_function_vreg(mf, vreg, &vi) ||
      vi.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
      !ast_mir_function_instruction(mf, vi.definition_index, &def) ||
      def.op != AST_MIR_OP_CONSTANT ||
      def.constant.kind != AST_SEMA_CONST_INTEGER || def.constant.hi != 0u)
    return 0;
  *out = def.constant.is_negative ? UINT64_C(0) - def.constant.lo
                                  : def.constant.lo;
  return 1;
}

static int source_constant_u64(PhysicalContext *c, AstMirVReg vreg,
                               uint64_t *out) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirVRegInfo vi;
  AstMirInstructionInfo def;
  if (!mf || !out || !ast_mir_function_vreg(mf, vreg, &vi) ||
      vi.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
      !ast_mir_function_instruction(mf, vi.definition_index, &def) ||
      def.op != AST_MIR_OP_CONSTANT ||
      def.constant.kind != AST_SEMA_CONST_INTEGER || def.constant.is_negative ||
      def.constant.hi != 0u)
    return 0;
  *out = def.constant.lo;
  return 1;
}

static int x86_scalar_immediate_ok(AstMachineOpcode opcode, uint32_t bits,
                                   uint64_t value) {
  if (bits != 8u && bits != 16u && bits != 32u && bits != 64u)
    return 0;
  if (opcode == AST_MACHINE_X86_SHL || opcode == AST_MACHINE_X86_ASHR ||
      opcode == AST_MACHINE_X86_LSHR)
    return value <= UINT8_MAX;
  if (opcode != AST_MACHINE_X86_ADD && opcode != AST_MACHINE_X86_SUB &&
      opcode != AST_MACHINE_X86_AND && opcode != AST_MACHINE_X86_OR &&
      opcode != AST_MACHINE_X86_XOR && opcode != AST_MACHINE_X86_MUL &&
      (opcode < AST_MACHINE_X86_ICMP_EQ || opcode > AST_MACHINE_X86_ICMP_GE))
    return 0;
  if (opcode == AST_MACHINE_X86_MUL && bits == 8u)
    return 0;
  if (bits < 64u)
    return 1;
  return value == (uint64_t)(int64_t)(int32_t)(uint32_t)value;
}

static int aggregate_element_offset(const AstSemaType *type, uint64_t index,
                                    uint64_t *offset,
                                    const AstSemaType **element) {
  if (!type || !offset || !element)
    return 0;
  AstSemaTypeKind kind = ast_sema_type_kind(type);
  if (kind == AST_SEMA_TYPE_ARRAY) {
    uint64_t count = 0u;
    const AstSemaType *item = ast_sema_type_element(type);
    uint64_t size = item ? ast_sema_type_size(item) : UINT64_MAX;
    if (!item || !ast_sema_type_array_count(type, &count) || index >= count ||
        size == UINT64_MAX || (size && index > UINT64_MAX / size))
      return 0;
    *offset = index * size;
    *element = item;
    return 1;
  }
  if (kind == AST_SEMA_TYPE_TUPLE) {
    uint32_t count = ast_sema_type_tuple_count(type);
    if (index >= count)
      return 0;
    uint64_t cursor = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
      const AstSemaType *item = ast_sema_type_tuple_item(type, i);
      uint64_t size = item ? ast_sema_type_size(item) : UINT64_MAX;
      uint32_t align = item ? ast_sema_type_alignment(item) : 0u;
      if (!item || size == UINT64_MAX || !align || (align & (align - 1u)))
        return 0;
      cursor = (cursor + align - 1u) & ~((uint64_t)align - 1u);
      if (i == index) {
        *offset = cursor;
        *element = item;
        return 1;
      }
      if (cursor > UINT64_MAX - size)
        return 0;
      cursor += size;
    }
  }
  return 0;
}

static int lower_pack(PhysicalContext *c, const MachineInstruction *mi) {
  if (!mi || mi->info.source_op != AST_MIR_OP_PACK ||
      mi->info.operand_count != 2u ||
      (mi->info.result_count != 1u && mi->info.result_count != 2u) ||
      mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG ||
      mi->info.operands[1].kind != AST_MACHINE_OPERAND_VREG)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_PACK ||
      ast_sema_type_kind(source.type) != AST_SEMA_TYPE_TUPLE ||
      ast_sema_type_tuple_count(source.type) != 2u)
    return 0;
  const AstSemaType *first_type = ast_sema_type_tuple_item(source.type, 0u);
  const AstSemaType *second_type = ast_sema_type_tuple_item(source.type, 1u);
  uint64_t first_size =
      first_type ? ast_sema_type_size(first_type) : UINT64_MAX;
  uint64_t second_size =
      second_type ? ast_sema_type_size(second_type) : UINT64_MAX;
  uint64_t total_size = ast_sema_type_size(source.type);
  if (first_size == UINT64_MAX || second_size != 1u ||
      total_size == UINT64_MAX ||
      (first_size != 1u && first_size != 2u && first_size != 4u &&
       first_size != 8u) ||
      !total_size || total_size > 16u)
    return 0;

  /* Materialize the tiny aggregate through an ABI temporary.  Besides being
     independent of fixed shift registers, this exactly follows the language
     aggregate layout (including padding) and therefore works for both the
     one-register and two-register target representation. */
  uint64_t storage = total_size <= 8u ? 8u : 16u;
  uint32_t object = physical_temp_object(c, storage, AST_FRAME_OBJECT_ABI_TEMP);
  if (object == AST_MACHINE_INVALID_ID)
    return 0;
  for (uint64_t off = 0u; off < storage; off += 8u)
    if (!physical_move(c, physical_frame(object, 64u, (int64_t)off),
                       physical_immediate(0u, 64u)))
      return 0;

  AstPhysicalOperand first = physical_home(c, mi->info.operands[0].as.vreg);
  AstPhysicalOperand flag = physical_home(c, mi->info.operands[1].as.vreg);
  if (!first.kind || !flag.kind)
    return 0;
  first.bit_width = (uint32_t)first_size * 8u;
  flag.bit_width = 8u;
  if (!physical_move(c, physical_frame(object, first.bit_width, 0), first) ||
      !physical_move(c, physical_frame(object, 8u, (int64_t)first_size), flag))
    return 0;

  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    AstPhysicalOperand dst = physical_home(c, mi->info.result_first + part);
    if (!dst.kind)
      continue;
    if (!physical_move(
            c, dst, physical_frame(object, dst.bit_width, (int64_t)part * 8)))
      return 0;
  }
  return 1;
}

static int lower_aggregate_extract(PhysicalContext *c,
                                   const MachineInstruction *mi) {
  if (!mi ||
      (mi->info.source_op != AST_MIR_OP_INDEX &&
       mi->info.source_op != AST_MIR_OP_MEMBER) ||
      mi->info.result_count != 1u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.operand_count < 1u)
    return 0;
  AstMirVRegInfo aggregate_vi;
  if (!ast_mir_function_vreg(mf, source.operands[0], &aggregate_vi) ||
      !aggregate_vi.type)
    return 0;
  uint64_t offset = 0u;
  const AstSemaType *element = NULL;
  if (source.op == AST_MIR_OP_INDEX) {
    if (source.operand_count != 2u)
      return 0;
    uint64_t index = 0u;
    if (!source_constant_u64(c, source.operands[1], &index) ||
        !aggregate_element_offset(aggregate_vi.type, index, &offset, &element))
      return 0;
  } else {
    if (ast_sema_type_kind(aggregate_vi.type) != AST_SEMA_TYPE_STRUCT ||
        source.aux_index >= ast_sema_type_struct_field_count(aggregate_vi.type))
      return 0;
    AstSemaFieldInfo field;
    if (!ast_sema_type_struct_field(aggregate_vi.type, source.aux_index,
                                    &field))
      return 0;
    offset = field.offset;
    element = field.type;
  }
  uint64_t bytes = element ? ast_sema_type_size(element) : UINT64_MAX;
  uint64_t aggregate_size = ast_sema_type_size(aggregate_vi.type);
  if (bytes == UINT64_MAX || bytes == 0u || bytes > 8u ||
      aggregate_size == UINT64_MAX || offset > aggregate_size ||
      bytes > aggregate_size - offset || aggregate_size > 16u)
    return 0;
  AstTargetValueInfo tv;
  if (!ast_target_value_info(c->program->machine->target, c->function,
                             source.operands[0], &tv))
    return 0;
  uint32_t parts = tv.part_count ? tv.part_count : 1u;
  if (!parts || parts > 2u || mi->info.operand_count < parts)
    return 0;

  uint64_t storage = aggregate_size <= 8u ? 8u : 16u;
  uint32_t object = physical_temp_object(c, storage, AST_FRAME_OBJECT_ABI_TEMP);
  if (object == AST_MACHINE_INVALID_ID)
    return 0;
  for (uint64_t off = 0u; off < storage; off += 8u)
    if (!physical_move(c, physical_frame(object, 64u, (int64_t)off),
                       physical_immediate(0u, 64u)))
      return 0;
  for (uint32_t part = 0u; part < parts; ++part) {
    if (mi->info.operands[part].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    AstPhysicalOperand src = physical_home(c, mi->info.operands[part].as.vreg);
    if (!src.kind ||
        !physical_move(
            c, physical_frame(object, src.bit_width, (int64_t)part * 8), src))
      return 0;
  }
  AstPhysicalOperand dst = physical_home(c, mi->info.result_first);
  if (!dst.kind)
    return 1;
  AstPhysicalOperand value =
      physical_frame(object, (uint32_t)bytes * 8u, (int64_t)offset);
  return physical_move(c, dst, value);
}
static int finish_fixed(PhysicalContext *c, WorkRegisters *work,
                        AstPhysicalOperand home, AstPhysicalOperand result) {
  uint64_t bytes = ((uint64_t)result.bit_width + 7u) / 8u;
  if (c->fn->abi_temp == AST_MACHINE_INVALID_ID)
    c->fn->abi_temp = physical_temp_object(c, bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (c->fn->abi_temp == AST_MACHINE_INVALID_ID)
    return 0;
  if (c->fn->objects[c->fn->abi_temp].size < bytes)
    c->fn->objects[c->fn->abi_temp].size = bytes;
  AstPhysicalOperand temp =
      physical_frame(c->fn->abi_temp, result.bit_width, 0);
  if (!physical_move(c, temp, result) || !restore_work(c, work))
    return 0;
  temp.bit_width = home.bit_width;
  return physical_move(c, home, temp);
}
static int extend_integer(PhysicalContext *c, AstPhysicalOperand dst,
                          AstPhysicalOperand src, int is_signed) {
  if (src.bit_width >= dst.bit_width)
    return physical_move(c, dst, src);
  if (src.kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    uint64_t mask = (UINT64_C(1) << src.bit_width) - 1u;
    uint64_t value = src.immediate_bits & mask;
    if (is_signed && (value & (UINT64_C(1) << (src.bit_width - 1u))))
      value |= ~mask;
    return physical_move(c, dst, physical_immediate(value, dst.bit_width));
  }
  AstPhysicalOperand args[] = {dst, src};
  return physical_emit(
      c, is_signed ? AST_MACHINE_X86_MOVSX : AST_MACHINE_X86_MOVZX, 1u, args,
      2u);
}
static int lower_mask_index(PhysicalContext *c, const MachineInstruction *mi) {
  if (!c || !mi || mi->info.source_op != AST_MIR_OP_INDEX ||
      mi->info.result_count != 1u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo base_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_INDEX || source.operand_count != 2u ||
      !ast_mir_function_vreg(mf, source.operands[0], &base_vi) ||
      !base_vi.type || ast_sema_type_kind(base_vi.type) != AST_SEMA_TYPE_MASK)
    return 0;
  uint64_t lanes = 0u;
  if (!ast_sema_type_vector_lanes(base_vi.type, &lanes) || !lanes)
    return 0;
  uint32_t parts = c->machine->source_parts[source.operands[0]];
  if (!parts || mi->info.operand_count < parts + 1u)
    return 0;
  uint64_t constant_index = 0u;
  int constant = source_constant_u64(c, source.operands[1], &constant_index);
  uint32_t part = 0u, bit = 0u;
  if (constant) {
    if (constant_index >= lanes)
      return 0;
    part = (uint32_t)(constant_index / 64u);
    bit = (uint32_t)(constant_index & 63u);
    if (part >= parts)
      return 0;
  } else if (parts != 1u) {
    return fail_instruction(
        c, "Dynamic indexing of a split mask needs part-selection lowering.");
  }
  const AstMachineOperand *mask_op = &mi->info.operands[part];
  const AstMachineOperand *index_op = &mi->info.operands[parts];
  if (mask_op->kind != AST_MACHINE_OPERAND_VREG ||
      index_op->kind != AST_MACHINE_OPERAND_VREG)
    return 0;
  AstPhysicalOperand mask = physical_home(c, mask_op->as.vreg);
  AstPhysicalOperand index = physical_home(c, index_op->as.vreg);
  AstPhysicalOperand home = physical_home(c, mi->info.result_first);
  if (!mask.kind || !home.kind || (!constant && !index.kind))
    return 0;
  WorkRegisters work = {0};
  if (mask.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[mask.register_unit] = 1u;
  if (index.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[index.register_unit] = 1u;
  if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[home.register_unit] = 1u;
  AstPhysicalOperand value =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  if (!value.kind || !extend_integer(c, value, mask, 0))
    return 0;
  AstPhysicalOperand rcx = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 64u);
  work.excluded[rcx.register_unit] = 1u;
  if (!borrow_exact(c, &work, rcx))
    return 0;
  AstPhysicalOperand cl = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 8u);
  if (constant) {
    if (bit && !physical_move(c, cl, physical_immediate(bit, 8u)))
      return 0;
  } else {
    AstPhysicalOperand idx8 = index;
    idx8.bit_width = 8u;
    if (!physical_move(c, cl, idx8))
      return 0;
  }
  if ((!constant || bit) &&
      !physical_emit(c, AST_MACHINE_X86_LSHR, 1u,
                     (AstPhysicalOperand[]){value, value, cl}, 3u))
    return 0;
  if (!physical_emit(
          c, AST_MACHINE_X86_AND, 1u,
          (AstPhysicalOperand[]){value, value, physical_immediate(1u, 64u)},
          3u))
    return 0;
  AstPhysicalOperand low = physical_register(
      c, AST_MACHINE_BANK_GPR, value.register_unit % 64u, home.bit_width);
  return physical_move(c, home, low) && restore_work(c, &work);
}

static int lower_x86_vector_select(PhysicalContext *c,
                                   const MachineInstruction *mi) {
  if (!c || !mi || !mi->info.result_count ||
      (mi->info.source_op != AST_MIR_OP_SELECT &&
       mi->info.source_op != AST_MIR_OP_INTRINSIC))
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo result_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      (source.op != AST_MIR_OP_SELECT &&
       !(source.op == AST_MIR_OP_INTRINSIC &&
         source.intrinsic == AST_HIR_INTRINSIC_SELECT)) ||
      source.operand_count != 3u || source.result == AST_MIR_INVALID_ID ||
      !ast_mir_function_vreg(mf, source.result, &result_vi) ||
      !result_vi.type ||
      ast_sema_type_kind(result_vi.type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = ast_sema_type_element(result_vi.type);
  uint64_t lanes = 0u;
  uint32_t element_bits = element ? ast_sema_type_bit_width(element) : 0u;
  int fp = element && ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT;
  if (!element || !ast_sema_type_vector_lanes(result_vi.type, &lanes) ||
      !lanes ||
      (element_bits != 8u && element_bits != 16u && element_bits != 32u &&
       element_bits != 64u) ||
      (fp && element_bits != 32u && element_bits != 64u))
    return 0;
  uint32_t mask_parts = c->machine->source_parts[source.operands[0]];
  uint32_t value_parts = c->machine->source_parts[source.operands[1]];
  if (!mask_parts ||
      value_parts != c->machine->source_parts[source.operands[2]] ||
      value_parts != mi->info.result_count ||
      mi->info.operand_count != mask_parts + value_parts * 2u)
    return 0;
  uint64_t total_bytes = ast_sema_type_size(result_vi.type);
  if (!total_bytes || total_bytes == UINT64_MAX)
    return 0;
  uint64_t storage_bytes = (total_bytes + 7u) & ~UINT64_C(7);
  uint32_t true_image =
      physical_temp_object(c, storage_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  uint32_t false_image =
      physical_temp_object(c, storage_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  uint32_t out_image =
      physical_temp_object(c, storage_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (true_image == AST_MACHINE_INVALID_ID ||
      false_image == AST_MACHINE_INVALID_ID ||
      out_image == AST_MACHINE_INVALID_ID)
    return 0;
  WorkRegisters work = {0};
  uint64_t offset = 0u;
  for (uint32_t part = 0u; part < value_parts; ++part) {
    const AstMachineOperand *top = &mi->info.operands[mask_parts + part];
    const AstMachineOperand *fop =
        &mi->info.operands[mask_parts + value_parts + part];
    if (top->kind != AST_MACHINE_OPERAND_VREG ||
        fop->kind != AST_MACHINE_OPERAND_VREG ||
        top->as.vreg >= c->machine->info.vreg_count ||
        fop->as.vreg >= c->machine->info.vreg_count)
      return 0;
    uint32_t bits = c->machine->vregs[top->as.vreg].bit_width;
    AstPhysicalOperand th = physical_home(c, top->as.vreg),
                       fh = physical_home(c, fop->as.vreg);
    if (!th.kind || !fh.kind ||
        bits != c->machine->vregs[fop->as.vreg].bit_width ||
        !physical_move(
            c, physical_frame(true_image, th.bit_width, (int64_t)offset), th) ||
        !physical_move(
            c, physical_frame(false_image, fh.bit_width, (int64_t)offset), fh))
      return 0;
    if (th.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[th.register_unit] = 1u;
    if (fh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[fh.register_unit] = 1u;
    offset += bits / 8u;
  }
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand h = physical_home(c, mi->info.result_first + rp);
    if (!h.kind)
      return 0;
    if (h.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[h.register_unit] = 1u;
  }
  for (uint32_t mp = 0u; mp < mask_parts; ++mp) {
    if (mi->info.operands[mp].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    AstPhysicalOperand mh = physical_home(c, mi->info.operands[mp].as.vreg);
    if (!mh.kind)
      return 0;
    if (mh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[mh.register_unit] = 1u;
  }
  AstPhysicalOperand mask_value =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand scalar_t = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, element_bits);
  AstPhysicalOperand scalar_f = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, element_bits);
  AstPhysicalOperand scalar_r = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, element_bits);
  AstPhysicalOperand rcx = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 64u);
  work.excluded[rcx.register_unit] = 1u;
  if (!mask_value.kind || !scalar_t.kind || !scalar_f.kind || !scalar_r.kind ||
      !borrow_exact(c, &work, rcx))
    return 0;
  AstPhysicalOperand cl = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 8u);
  uint64_t lane_bytes = element_bits / 8u;
  for (uint64_t lane = 0u; lane < lanes; ++lane) {
    uint32_t mp = (uint32_t)(lane / 64u), bit = (uint32_t)(lane & 63u);
    if (mp >= mask_parts)
      return 0;
    AstPhysicalOperand mh = physical_home(c, mi->info.operands[mp].as.vreg);
    if (!extend_integer(c, mask_value, mh, 0))
      return 0;
    if (bit) {
      if (!physical_move(c, cl, physical_immediate(bit, 8u)) ||
          !physical_emit(c, AST_MACHINE_X86_LSHR, 1u,
                         (AstPhysicalOperand[]){mask_value, mask_value, cl},
                         3u))
        return 0;
    }
    if (!physical_emit(c, AST_MACHINE_X86_AND, 1u,
                       (AstPhysicalOperand[]){mask_value, mask_value,
                                              physical_immediate(1u, 64u)},
                       3u))
      return 0;
    uint64_t byte_offset = lane * lane_bytes;
    AstPhysicalOperand t = scalar_t, f = scalar_f, r = scalar_r;
    t.bit_width = f.bit_width = r.bit_width = element_bits;
    if (!physical_move(
            c, t,
            physical_frame(true_image, element_bits, (int64_t)byte_offset)) ||
        !physical_move(
            c, f,
            physical_frame(false_image, element_bits, (int64_t)byte_offset)))
      return 0;
    AstPhysicalOperand cond = physical_register(
        c, AST_MACHINE_BANK_GPR, mask_value.register_unit % 64u, 8u);
    AstPhysicalOperand sel[] = {r, cond, t, f};
    if (!physical_emit(c, AST_MACHINE_X86_SELECT, 1u, sel, 4u) ||
        !physical_move(
            c, physical_frame(out_image, element_bits, (int64_t)byte_offset),
            r))
      return 0;
  }
  offset = 0u;
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand h = physical_home(c, mi->info.result_first + rp);
    uint32_t bits = c->machine->vregs[mi->info.result_first + rp].bit_width;
    if (!physical_move(c, h,
                       physical_frame(out_image, h.bit_width, (int64_t)offset)))
      return 0;
    offset += bits / 8u;
  }
  return restore_work(c, &work);
}

/* MIR integer literals are sign/magnitude, not signed host integers.  Do
   not reinterpret `lo` as two's-complement when computing reciprocal magic.
   INT64_MIN magnitude is representable as uint64_t and is handled without
   signed overflow.  The unsigned fast path retains its own extraction. */
static int x86_signed_constant_divisor(PhysicalContext *c, AstMirVReg operand,
                                       uint64_t *magnitude, int *negative) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirVRegInfo vreg;
  AstMirInstructionInfo def;
  if (!magnitude || !negative || !mf ||
      !ast_mir_function_vreg(mf, operand, &vreg) ||
      vreg.kind != AST_MIR_VREG_INSTRUCTION_RESULT ||
      !ast_mir_function_instruction(mf, vreg.definition_index, &def) ||
      def.op != AST_MIR_OP_CONSTANT ||
      def.constant.kind != AST_SEMA_CONST_INTEGER || def.constant.hi ||
      !def.constant.lo ||
      (def.constant.is_negative ? def.constant.lo > (UINT64_C(1) << 63u)
                                : def.constant.lo > INT64_MAX))
    return 0;
  *magnitude = def.constant.lo;
  *negative = !!def.constant.is_negative;
  return 1;
}

/* The 64-bit x86 MUL instruction has implicit RAX and RDX defs.  Stage the
   numerator outside those registers before borrowing either, keep the magic
   multiplier in a distinct scratch register, and use the same stack-backed
   finish_fixed() as the existing IDIV path.  Never treat IMUL's truncated
   low half as an unsigned high product. */
static int lower_x86_magic_u64(PhysicalContext *c, const MachineInstruction *mi,
                               WorkRegisters *w, AstPhysicalOperand home,
                               AstPhysicalOperand numerator) {
  int signed_op = mi->info.opcode == AST_MACHINE_X86_SDIV ||
                  mi->info.opcode == AST_MACHINE_X86_SREM;
  int division = mi->info.opcode == AST_MACHINE_X86_UDIV ||
                 mi->info.opcode == AST_MACHINE_X86_SDIV;
  if (!division && mi->info.opcode != AST_MACHINE_X86_UREM &&
      mi->info.opcode != AST_MACHINE_X86_SREM)
    return 0;
  if (mi->info.source_instruction == AST_MIR_INVALID_ID ||
      mi->info.operand_count != 2u || mi->info.result_count != 1u ||
      (home.bit_width != 8u && home.bit_width != 16u && home.bit_width != 32u &&
       home.bit_width != 64u))
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  uint64_t divisor;
  int divisor_negative = 0;
  AstDivMagicU64 magic;
  const uint32_t width = home.bit_width == 64u ? 64u : 32u;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_BINARY || source.operand_count != 2u ||
      !(signed_op ? x86_signed_constant_divisor(c, source.operands[1], &divisor,
                                                &divisor_negative)
                  : source_constant_u64(c, source.operands[1], &divisor)) ||
      (home.bit_width != 64u &&
       (divisor > (signed_op
                       ? (divisor_negative
                              ? (UINT64_C(1) << (home.bit_width - 1u))
                              : ((UINT64_C(1) << (home.bit_width - 1u)) - 1u))
                       : ((UINT64_C(1) << home.bit_width) - 1u)))) ||
      !(width == 64u ? ast_div_magic_u64(divisor, &magic)
                     : ast_div_magic_u32((uint32_t)divisor, &magic)))
    return 0;
  AstDivCostChoice choice = ast_div_cost_select(
      AST_DIV_COST_X86_64, home.bit_width, signed_op, !division, divisor, 1,
      c->program->target.optimize_size || ast_div_cost_prefer_size());
  ast_div_cost_report(AST_DIV_COST_X86_64, home.bit_width, signed_op, !division,
                      divisor, choice);
  if (choice.mode != AST_DIV_COST_MAGIC)
    return 0;

  AstPhysicalOperand ax = physical_register(c, AST_MACHINE_BANK_GPR, 0u, width);
  AstPhysicalOperand dx = physical_register(c, AST_MACHINE_BANK_GPR, 2u, width);
  if (!ax.kind || !dx.kind)
    return -1;
  /* MUL overwrites *both* fixed registers. Exclude them before borrowing
     values that must remain live through the multiply, not afterwards. */
  w->excluded[ax.register_unit] = w->excluded[dx.register_unit] = 1u;
  AstPhysicalOperand original =
      borrow_register(c, w, AST_MACHINE_BANK_GPR, width);
  AstPhysicalOperand multiplier =
      borrow_register(c, w, AST_MACHINE_BANK_GPR, width);
  if (!original.kind || !multiplier.kind || !ax.kind || !dx.kind ||
      !extend_integer(c, original, numerator, signed_op))
    return -1;
  AstPhysicalOperand sign = {0};
  AstPhysicalOperand magnitude = original;
  if (signed_op) {
    sign = borrow_register(c, w, AST_MACHINE_BANK_GPR, width);
    magnitude = borrow_register(c, w, AST_MACHINE_BANK_GPR, width);
    if (!sign.kind || !magnitude.kind || !physical_move(c, sign, original) ||
        !physical_emit(c, AST_MACHINE_X86_ASHR, 1u,
                       (AstPhysicalOperand[]){
                           sign, sign, physical_immediate(width - 1u, width)},
                       3u) ||
        !physical_move(c, magnitude, original) ||
        !physical_emit(c, AST_MACHINE_X86_XOR, 1u,
                       (AstPhysicalOperand[]){magnitude, magnitude, sign},
                       3u) ||
        !physical_emit(c, AST_MACHINE_X86_SUB, 1u,
                       (AstPhysicalOperand[]){magnitude, magnitude, sign}, 3u))
      return -1;
    if (divisor_negative &&
        !physical_emit(c, AST_MACHINE_X86_XOR, 1u,
                       (AstPhysicalOperand[]){
                           sign, sign,
                           physical_immediate(
                               width == 64u ? UINT64_MAX : UINT32_MAX, width)},
                       3u))
      return -1;
  }
  if (!borrow_exact(c, w, ax) || !borrow_exact(c, w, dx) ||
      !physical_move(c, multiplier,
                     physical_immediate(magic.multiplier, width)) ||
      !physical_move(c, ax, magnitude))
    return -1;
  AstPhysicalOperand product[] = {ax, dx, ax, multiplier};
  if (!physical_emit(c, AST_MACHINE_X86_UMUL_WIDE, 2u, product, 4u))
    return -1;
  if (magic.add) {
    AstPhysicalOperand correction =
        borrow_register(c, w, AST_MACHINE_BANK_GPR, width);
    if (!correction.kind || !physical_move(c, correction, magnitude) ||
        !physical_emit(c, AST_MACHINE_X86_SUB, 1u,
                       (AstPhysicalOperand[]){correction, correction, dx},
                       3u) ||
        !physical_emit(c, AST_MACHINE_X86_LSHR, 1u,
                       (AstPhysicalOperand[]){correction, correction,
                                              physical_immediate(1u, width)},
                       3u) ||
        !physical_emit(c, AST_MACHINE_X86_ADD, 1u,
                       (AstPhysicalOperand[]){dx, dx, correction}, 3u))
      return -1;
  }
  if (magic.shift &&
      !physical_emit(c, AST_MACHINE_X86_LSHR, 1u,
                     (AstPhysicalOperand[]){
                         dx, dx, physical_immediate(magic.shift, width)},
                     3u))
    return -1;
  if (signed_op && (!physical_emit(c, AST_MACHINE_X86_XOR, 1u,
                                   (AstPhysicalOperand[]){dx, dx, sign}, 3u) ||
                    !physical_emit(c, AST_MACHINE_X86_SUB, 1u,
                                   (AstPhysicalOperand[]){dx, dx, sign}, 3u)))
    return -1;
  if (!division) {
    uint64_t literal =
        signed_op && divisor_negative ? UINT64_C(0) - divisor : divisor;
    if (!physical_move(c, multiplier, physical_immediate(literal, width)) ||
        !physical_emit(c, AST_MACHINE_X86_MUL, 1u,
                       (AstPhysicalOperand[]){dx, dx, multiplier}, 3u) ||
        !physical_emit(c, AST_MACHINE_X86_SUB, 1u,
                       (AstPhysicalOperand[]){original, original, dx}, 3u))
      return -1;
  }
  return finish_fixed(c, w, home, division ? dx : original) ? 1 : -1;
}

static int lower_fixed_binary(PhysicalContext *c,
                              const MachineInstruction *mi) {
  if (mi->info.result_count != 1u || mi->info.operand_count != 2u ||
      mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG ||
      mi->info.operands[1].kind != AST_MACHINE_OPERAND_VREG)
    return 0;
  WorkRegisters work = {0};
  AstPhysicalOperand home = physical_home(c, mi->info.result_first);
  AstPhysicalOperand lhs = physical_home(c, mi->info.operands[0].as.vreg);
  AstPhysicalOperand rhs = physical_home(c, mi->info.operands[1].as.vreg);
  uint32_t bits = home.bit_width;
  if (!home.kind || bits > 64u || bits < 8u)
    return 0;
  if (lhs.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[lhs.register_unit] = 1u;
  if (rhs.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[rhs.register_unit] = 1u;
  if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[home.register_unit] = 1u;
  int shift = mi->info.opcode == AST_MACHINE_X86_SHL ||
              mi->info.opcode == AST_MACHINE_X86_ASHR ||
              mi->info.opcode == AST_MACHINE_X86_LSHR;
  uint64_t shift_constant = 0u;
  if (shift &&
      source_constant_bits(c, mi->info.operands[1].as.vreg, &shift_constant) &&
      shift_constant <= UINT8_MAX) {
    AstPhysicalOperand result =
        home.kind == AST_PHYSICAL_OPERAND_REGISTER
            ? home
            : borrow_register(c, &work, AST_MACHINE_BANK_GPR, bits);
    if (!result.kind || !physical_move(c, result, lhs))
      return 0;
    AstPhysicalOperand args[] = {result, result,
                                 physical_immediate(shift_constant, bits)};
    if (!physical_emit(c, mi->info.opcode, 1u, args, 3u))
      return 0;
    if (home.kind != AST_PHYSICAL_OPERAND_REGISTER &&
        !physical_move(c, home, result))
      return 0;
    return restore_work(c, &work);
  }
  if (shift) {
    AstPhysicalOperand rcx =
        physical_register(c, AST_MACHINE_BANK_GPR, 1u, 64u);
    work.excluded[rcx.register_unit] = 1u;
    AstPhysicalOperand result =
        borrow_register(c, &work, AST_MACHINE_BANK_GPR, bits);
    if (!result.kind || !physical_move(c, result, lhs) ||
        !borrow_exact(c, &work, rcx))
      return 0;
    AstPhysicalOperand count =
        physical_register(c, AST_MACHINE_BANK_GPR, 1u, rhs.bit_width);
    if (!physical_move(c, count, rhs))
      return 0;
    AstPhysicalOperand args[] = {
        result, result, physical_register(c, AST_MACHINE_BANK_GPR, 1u, 8u)};
    return physical_emit(c, mi->info.opcode, 1u, args, 3u) &&
           finish_fixed(c, &work, home, result);
  }
  /* Strength-reduce unsigned division by an exact power of two. This is
     valid for every numerator, including UINT64_MAX; signed division must
     not use this rule (round-to-zero differs from arithmetic shift).
     Keep the original DIV path for zero/non-pow2 divisors. An x86-64 AND
     mask larger than signed imm32 needs a register form, so leave that
     case to the general path until the profitability model accounts for it. */
  if (!shift && (mi->info.opcode == AST_MACHINE_X86_UDIV ||
                 mi->info.opcode == AST_MACHINE_X86_UREM)) {
    uint64_t divisor = 0u;
    if (source_constant_bits(c, mi->info.operands[1].as.vreg, &divisor) &&
        divisor != 0u && (divisor & (divisor - UINT64_C(1))) == 0u &&
        (bits == 64u || divisor < (UINT64_C(1) << bits)) &&
        (mi->info.opcode != AST_MACHINE_X86_UREM ||
         divisor <= (UINT64_C(1) << 31u))) {
      uint32_t log2 = 0u;
      for (uint64_t v = divisor; v > 1u; v >>= 1u)
        ++log2;
      AstPhysicalOperand result =
          home.kind == AST_PHYSICAL_OPERAND_REGISTER
              ? home
              : borrow_register(c, &work, AST_MACHINE_BANK_GPR, bits);
      if (!result.kind)
        return 0;
      if (mi->info.opcode == AST_MACHINE_X86_UDIV && !log2) {
        if (!physical_move(c, result, lhs))
          return 0;
      } else if (mi->info.opcode == AST_MACHINE_X86_UREM && !log2) {
        if (!physical_move(c, result, physical_immediate(0u, bits)))
          return 0;
      } else {
        uint64_t operand =
            mi->info.opcode == AST_MACHINE_X86_UDIV ? log2 : divisor - 1u;
        AstMachineOpcode opcode = mi->info.opcode == AST_MACHINE_X86_UDIV
                                      ? AST_MACHINE_X86_LSHR
                                      : AST_MACHINE_X86_AND;
        AstPhysicalOperand args[] = {result, result,
                                     physical_immediate(operand, bits)};
        if (!physical_move(c, result, lhs) ||
            !physical_emit(c, opcode, 1u, args, 3u))
          return 0;
      }
      if (home.kind != AST_PHYSICAL_OPERAND_REGISTER &&
          !physical_move(c, home, result))
        return 0;
      return restore_work(c, &work);
    }
  }
  if (!shift && (mi->info.opcode == AST_MACHINE_X86_UDIV ||
                 mi->info.opcode == AST_MACHINE_X86_UREM ||
                 mi->info.opcode == AST_MACHINE_X86_SDIV ||
                 mi->info.opcode == AST_MACHINE_X86_SREM)) {
    int reduced = lower_x86_magic_u64(c, mi, &work, home, lhs);
    if (reduced < 0)
      return 0;
    if (reduced > 0)
      return 1;
  }
  /* Promote byte/word division to 32 bits. The quotient is truncated only
     after division, so INT8/INT16_MIN divided by -1 does not trap IDIV. */
  int is_signed = mi->info.opcode == AST_MACHINE_X86_SDIV ||
                  mi->info.opcode == AST_MACHINE_X86_SREM;
  if (bits < 32u)
    bits = 32u;
  AstPhysicalOperand ax = physical_register(c, AST_MACHINE_BANK_GPR, 0u, bits);
  AstPhysicalOperand dx = physical_register(c, AST_MACHINE_BANK_GPR, 2u, bits);
  work.excluded[ax.register_unit] = work.excluded[dx.register_unit] = 1u;
  AstPhysicalOperand divisor =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, bits);
  if (!divisor.kind || !extend_integer(c, divisor, rhs, is_signed) ||
      !borrow_exact(c, &work, ax) || !borrow_exact(c, &work, dx) ||
      !extend_integer(c, ax, lhs, is_signed))
    return 0;
  if (is_signed) {
    AstPhysicalOperand prep[] = {dx, ax};
    if (!physical_emit(c,
                       bits == 64u ? AST_MACHINE_X86_CQO : AST_MACHINE_X86_CDQ,
                       1u, prep, 2u))
      return 0;
  } else if (!physical_move(c, dx, physical_immediate(0u, bits)))
    return 0;
  AstPhysicalOperand args[] = {ax, dx, ax, dx, divisor};
  if (!physical_emit(c, is_signed ? AST_MACHINE_X86_SDIV : AST_MACHINE_X86_UDIV,
                     2u, args, 5u))
    return 0;
  int remainder = mi->info.opcode == AST_MACHINE_X86_SREM ||
                  mi->info.opcode == AST_MACHINE_X86_UREM;
  return finish_fixed(c, &work, home, remainder ? dx : ax);
}

static int lower_scalar_cast(PhysicalContext *c, WorkRegisters *w,
                             const MachineInstruction *mi,
                             AstPhysicalOperand dst, AstPhysicalOperand src) {
  const AstSemaType *from =
      c->machine->vregs[mi->info.operands[0].as.vreg].type;
  const AstSemaType *to = c->machine->vregs[mi->info.result_first].type;
  int sign =
      mi->info.token_op == AST_TOK_KW_SEXT ||
      (mi->info.token_op != AST_TOK_KW_ZEXT && ast_sema_type_is_signed(from));
  int raw = mi->info.token_op == AST_TOK_KW_BITCAST ||
            mi->info.token_op == AST_TOK_KW_PTRCAST;
  if (dst.bank == AST_MACHINE_BANK_GPR && src.bank == AST_MACHINE_BANK_GPR) {
    if (dst.bit_width > src.bit_width)
      return extend_integer(c, dst, src, sign);
    return physical_move(
        c, dst,
        physical_register(c, src.bank, src.register_unit % 64u, dst.bit_width));
  }
  AstMachineOpcode opcode;
  if (raw) {
    if (dst.bit_width != src.bit_width ||
        (dst.bit_width != 32u && dst.bit_width != 64u))
      return 0;
    opcode = AST_MACHINE_X86_FMOV_BITS;
  } else if (dst.bank == AST_MACHINE_BANK_FP &&
             src.bank == AST_MACHINE_BANK_GPR) {
    if (!sign && src.bit_width == 64u) {
      AstPhysicalOperand scratch =
          borrow_register(c, w, AST_MACHINE_BANK_GPR, 64u);
      AstPhysicalOperand args[] = {dst, scratch, src};
      return scratch.kind &&
             physical_emit(c, AST_MACHINE_X86_UINT64_TO_FP, 2u, args, 3u);
    }
    uint32_t bits = !sign && src.bit_width == 32u ? 64u
                    : src.bit_width < 32u         ? 32u
                                                  : src.bit_width;
    if (bits != src.bit_width) {
      AstPhysicalOperand normalized =
          borrow_register(c, w, AST_MACHINE_BANK_GPR, bits);
      if (!normalized.kind || !extend_integer(c, normalized, src, sign))
        return 0;
      src = normalized;
    }
    opcode = AST_MACHINE_X86_SINT_TO_FP;
  } else if (dst.bank == AST_MACHINE_BANK_GPR &&
             src.bank == AST_MACHINE_BANK_FP) {
    if (!ast_sema_type_is_signed(to) && dst.bit_width == 64u) {
      AstPhysicalOperand scratch =
          borrow_register(c, w, AST_MACHINE_BANK_GPR, 64u);
      AstPhysicalOperand temp =
          borrow_register(c, w, AST_MACHINE_BANK_FP, src.bit_width);
      AstPhysicalOperand args[] = {dst, scratch, temp, src};
      return scratch.kind && temp.kind &&
             physical_emit(c, AST_MACHINE_X86_FP_TO_UINT64, 3u, args, 4u);
    }
    uint32_t bits = !ast_sema_type_is_signed(to) && dst.bit_width == 32u ? 64u
                    : dst.bit_width <= 32u                               ? 32u
                                                                         : 64u;
    dst = physical_register(c, dst.bank, dst.register_unit % 64u, bits);
    opcode = AST_MACHINE_X86_FP_TO_SINT;
  } else if (dst.bank == AST_MACHINE_BANK_FP &&
             src.bank == AST_MACHINE_BANK_FP) {
    if (dst.bit_width == src.bit_width)
      return physical_move(c, dst, src);
    opcode = AST_MACHINE_X86_FP_CONVERT;
  } else
    return fail_instruction(c, "Vector casts need lane-aware legalization.");
  AstPhysicalOperand args[] = {dst, src};
  return physical_emit(c, opcode, 1u, args, 2u);
}

static AstMachineOpcode x86_scalar_compare_opcode(AstTokenKind token, int fp) {
  if (fp) {
    switch (token) {
    case AST_TOK_EQ_EQ:
      return AST_MACHINE_X86_FCMP_EQ;
    case AST_TOK_BANG_EQ:
      return AST_MACHINE_X86_FCMP_NE;
    case AST_TOK_LT:
      return AST_MACHINE_X86_FCMP_LT;
    case AST_TOK_LT_EQ:
      return AST_MACHINE_X86_FCMP_LE;
    case AST_TOK_GT:
      return AST_MACHINE_X86_FCMP_GT;
    case AST_TOK_GT_EQ:
      return AST_MACHINE_X86_FCMP_GE;
    default:
      return AST_MACHINE_OP_INVALID;
    }
  }
  switch (token) {
  case AST_TOK_EQ_EQ:
    return AST_MACHINE_X86_ICMP_EQ;
  case AST_TOK_BANG_EQ:
    return AST_MACHINE_X86_ICMP_NE;
  case AST_TOK_LT:
    return AST_MACHINE_X86_ICMP_LT;
  case AST_TOK_LT_EQ:
    return AST_MACHINE_X86_ICMP_LE;
  case AST_TOK_GT:
    return AST_MACHINE_X86_ICMP_GT;
  case AST_TOK_GT_EQ:
    return AST_MACHINE_X86_ICMP_GE;
  default:
    return AST_MACHINE_OP_INVALID;
  }
}

static int x86_source_type(PhysicalContext *c, AstMirVReg vreg,
                           AstMirVRegInfo *out) {
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  return mf && out && ast_mir_function_vreg(mf, vreg, out);
}

static uint32_t x86_value_image(PhysicalContext *c, AstMirVReg source,
                                uint64_t *bytes_out) {
  if (!c || source >= c->machine->source_vreg_count)
    return AST_MACHINE_INVALID_ID;
  uint32_t first = c->machine->source_first[source],
           parts = c->machine->source_parts[source];
  if (first == AST_MACHINE_INVALID_ID || !parts ||
      first > c->machine->info.vreg_count ||
      parts > c->machine->info.vreg_count - first)
    return AST_MACHINE_INVALID_ID;
  uint64_t bytes = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t bits = c->machine->vregs[first + part].bit_width;
    if (!bits || (bits & 7u) || bytes > UINT64_MAX - bits / 8u)
      return AST_MACHINE_INVALID_ID;
    bytes += bits / 8u;
  }
  /* A compact vector tail may live in a containing 32/64-bit GPR (for
     example vec<3,u8> in EAX).  Allocate enough backing bytes for that
     architectural store while preserving the logical, padding-free lane
     offsets returned to the scalarized vector lowering. */
  uint64_t storage_bytes = bytes;
  uint64_t probe_off = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t v = first + part, bits = c->machine->vregs[v].bit_width;
    AstPhysicalOperand home = physical_home(c, v);
    if (!home.kind)
      return AST_MACHINE_INVALID_ID;
    uint64_t end = probe_off + ((uint64_t)home.bit_width + 7u) / 8u;
    if (end > storage_bytes)
      storage_bytes = end;
    probe_off += bits / 8u;
  }
  uint32_t image =
      physical_temp_object(c, storage_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (image == AST_MACHINE_INVALID_ID)
    return image;
  uint64_t off = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t v = first + part, bits = c->machine->vregs[v].bit_width;
    AstPhysicalOperand home = physical_home(c, v);
    if (!home.kind ||
        !physical_move(c, physical_frame(image, home.bit_width, (int64_t)off),
                       home))
      return AST_MACHINE_INVALID_ID;
    off += bits / 8u;
  }
  if (bytes_out)
    *bytes_out = bytes;
  return image;
}

static uint32_t x86_result_image(PhysicalContext *c,
                                 const MachineInstruction *mi,
                                 uint64_t *bytes_out) {
  if (!c || !mi || !mi->info.result_count)
    return AST_MACHINE_INVALID_ID;
  uint64_t bytes = 0u;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    if (v >= c->machine->info.vreg_count)
      return AST_MACHINE_INVALID_ID;
    uint32_t bits = c->machine->vregs[v].bit_width;
    if (!bits || (bits & 7u) || bytes > UINT64_MAX - bits / 8u)
      return AST_MACHINE_INVALID_ID;
    bytes += bits / 8u;
  }
  uint64_t storage_bytes = (bytes + 7u) & ~UINT64_C(7);
  if (!storage_bytes)
    storage_bytes = bytes;
  uint32_t image =
      physical_temp_object(c, storage_bytes, AST_FRAME_OBJECT_ABI_TEMP);
  if (image == AST_MACHINE_INVALID_ID)
    return image;
  /* Zero the padding too: committing a compact 24/48-bit result through a
     32/64-bit architectural home must never read uninitialized tail bytes. */
  for (uint64_t off = 0u; off < storage_bytes;) {
    uint64_t remain = storage_bytes - off;
    uint32_t bits = remain >= 8u   ? 64u
                    : remain >= 4u ? 32u
                    : remain >= 2u ? 16u
                                   : 8u;
    if (!physical_move(c, physical_frame(image, bits, (int64_t)off),
                       physical_immediate(0u, bits)))
      return AST_MACHINE_INVALID_ID;
    off += bits / 8u;
  }
  if (bytes_out)
    *bytes_out = bytes;
  return image;
}

static int x86_commit_result_image(PhysicalContext *c,
                                   const MachineInstruction *mi,
                                   uint32_t image) {
  uint64_t off = 0u;
  for (uint32_t part = 0u; part < mi->info.result_count; ++part) {
    uint32_t v = mi->info.result_first + part;
    AstPhysicalOperand home = physical_home(c, v);
    uint32_t bits = c->machine->vregs[v].bit_width;
    if (home.kind &&
        !physical_move(c, home,
                       physical_frame(image, home.bit_width, (int64_t)off)))
      return 0;
    off += bits / 8u;
  }
  return 1;
}

static int x86_vector_shape(const AstSemaType *type,
                            const AstSemaType **element_out,
                            uint64_t *lanes_out, uint32_t *bits_out) {
  if (!type || ast_sema_type_kind(type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = ast_sema_type_element(type);
  uint64_t lanes = 0u;
  uint32_t bits = element ? ast_sema_type_bit_width(element) : 0u;
  if (!element || !ast_sema_type_vector_lanes(type, &lanes) || !lanes ||
      (bits != 8u && bits != 16u && bits != 32u && bits != 64u))
    return 0;
  if (element_out)
    *element_out = element;
  if (lanes_out)
    *lanes_out = lanes;
  if (bits_out)
    *bits_out = bits;
  return 1;
}

static AstMachineOpcode x86_scalar_binary_opcode(AstTokenKind token, int fp) {
  if (fp) {
    switch (token) {
    case AST_TOK_PLUS:
      return AST_MACHINE_X86_FADD;
    case AST_TOK_MINUS:
      return AST_MACHINE_X86_FSUB;
    case AST_TOK_STAR:
      return AST_MACHINE_X86_FMUL;
    case AST_TOK_SLASH:
      return AST_MACHINE_X86_FDIV;
    default:
      return AST_MACHINE_OP_INVALID;
    }
  }
  switch (token) {
  case AST_TOK_PLUS:
    return AST_MACHINE_X86_ADD;
  case AST_TOK_MINUS:
    return AST_MACHINE_X86_SUB;
  case AST_TOK_STAR:
    return AST_MACHINE_X86_MUL;
  case AST_TOK_AMP:
  case AST_TOK_AMP_AMP:
    return AST_MACHINE_X86_AND;
  case AST_TOK_PIPE:
  case AST_TOK_PIPE_PIPE:
    return AST_MACHINE_X86_OR;
  case AST_TOK_CARET:
    return AST_MACHINE_X86_XOR;
  case AST_TOK_SHIFT_LEFT:
    return AST_MACHINE_X86_SHL;
  case AST_TOK_SHIFT_RIGHT:
    return AST_MACHINE_X86_LSHR;
  default:
    return AST_MACHINE_OP_INVALID;
  }
}

static int lower_x86_vector_binary(PhysicalContext *c,
                                   const MachineInstruction *mi) {
  if (!c || !mi || mi->info.source_instruction == AST_MIR_INVALID_ID ||
      !mi->info.result_count)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo result_vi, rhs_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_BINARY || source.result == AST_MIR_INVALID_ID ||
      source.operand_count != 2u ||
      !x86_source_type(c, source.result, &result_vi) || !result_vi.type ||
      ast_sema_type_kind(result_vi.type) != AST_SEMA_TYPE_VECTOR ||
      !x86_source_type(c, source.operands[1], &rhs_vi) || !rhs_vi.type)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!x86_vector_shape(result_vi.type, &element, &lanes, &bits))
    return 0;
  int fp = ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT;
  int rhs_vector = ast_sema_type_kind(rhs_vi.type) == AST_SEMA_TYPE_VECTOR;
  int shift = source.token_op == AST_TOK_SHIFT_LEFT ||
              source.token_op == AST_TOK_SHIFT_RIGHT;
  if (fp && (bits != 32u && bits != 64u))
    return 0;
  if (!rhs_vector && !shift)
    return 0;
  AstMachineOpcode op = x86_scalar_binary_opcode(source.token_op, fp);
  if (op == AST_MACHINE_OP_INVALID)
    return 0;
  int signed_element = !fp && ast_sema_type_is_signed(element);
  if (source.token_op == AST_TOK_SHIFT_RIGHT && signed_element)
    op = AST_MACHINE_X86_ASHR;

  uint64_t lhs_bytes = 0u, rhs_bytes = 0u, out_bytes = 0u;
  uint32_t lhs_image = x86_value_image(c, source.operands[0], &lhs_bytes);
  uint32_t rhs_image = AST_MACHINE_INVALID_ID;
  if (lhs_image == AST_MACHINE_INVALID_ID)
    return 0;
  if (rhs_vector) {
    rhs_image = x86_value_image(c, source.operands[1], &rhs_bytes);
    if (rhs_image == AST_MACHINE_INVALID_ID || rhs_bytes != lhs_bytes)
      return 0;
  }
  uint32_t out_image = x86_result_image(c, mi, &out_bytes);
  uint64_t logical = ast_sema_type_size(result_vi.type);
  if (out_image == AST_MACHINE_INVALID_ID || !logical ||
      logical == UINT64_MAX || logical > lhs_bytes || logical > out_bytes)
    return 0;

  WorkRegisters work = {0};
  uint32_t compute_bits = fp ? bits : (bits <= 32u ? 32u : 64u);
  AstPhysicalOperand lhs = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, compute_bits);
  AstPhysicalOperand rhs = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, compute_bits);
  AstPhysicalOperand dst = borrow_register(
      c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR, compute_bits);
  if (!lhs.kind || !rhs.kind || !dst.kind)
    return 0;
  AstPhysicalOperand rcx = {0}, cl = {0};
  if (shift) {
    rcx = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 64u);
    work.excluded[rcx.register_unit] = 1u;
    if (!borrow_exact(c, &work, rcx))
      return 0;
    cl = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 8u);
    uint32_t first = c->machine->source_first[source.operands[1]];
    if (first == AST_MACHINE_INVALID_ID ||
        c->machine->source_parts[source.operands[1]] != 1u)
      return 0;
    AstPhysicalOperand count_home = physical_home(c, first);
    if (!count_home.kind || !extend_integer(c, rcx, count_home, 0) ||
        !physical_emit(c, AST_MACHINE_X86_AND, 1u,
                       (AstPhysicalOperand[]){
                           rcx, rcx, physical_immediate(bits - 1u, 64u)},
                       3u))
      return 0;
  }
  uint64_t lane_bytes = bits / 8u;
  for (uint64_t lane = 0u; lane < lanes; ++lane) {
    uint64_t off = lane * lane_bytes;
    if (fp) {
      AstPhysicalOperand l = lhs, r = rhs, d = dst;
      l.bit_width = r.bit_width = d.bit_width = bits;
      if (!physical_move(c, l, physical_frame(lhs_image, bits, (int64_t)off)))
        return 0;
      if (rhs_vector &&
          !physical_move(c, r, physical_frame(rhs_image, bits, (int64_t)off)))
        return 0;
      if (!physical_move(c, d, l) ||
          !physical_emit(c, op, 1u, (AstPhysicalOperand[]){d, d, r}, 3u) ||
          !physical_move(c, physical_frame(out_image, bits, (int64_t)off), d))
        return 0;
      continue;
    }
    AstPhysicalOperand lraw = physical_frame(lhs_image, bits, (int64_t)off);
    if (!extend_integer(c, lhs, lraw,
                        signed_element &&
                            source.token_op == AST_TOK_SHIFT_RIGHT))
      return 0;
    if (shift) {
      if (!physical_move(c, dst, lhs) ||
          !physical_emit(c, op, 1u, (AstPhysicalOperand[]){dst, dst, cl}, 3u))
        return 0;
    } else {
      AstPhysicalOperand rraw = physical_frame(rhs_image, bits, (int64_t)off);
      if (!extend_integer(c, rhs, rraw, 0) || !physical_move(c, dst, lhs) ||
          !physical_emit(c, op, 1u, (AstPhysicalOperand[]){dst, dst, rhs}, 3u))
        return 0;
    }
    AstPhysicalOperand low = physical_register(c, AST_MACHINE_BANK_GPR,
                                               dst.register_unit % 64u, bits);
    if (!physical_move(c, physical_frame(out_image, bits, (int64_t)off), low))
      return 0;
  }
  if (!restore_work(c, &work))
    return 0;
  return x86_commit_result_image(c, mi, out_image);
}

static int lower_x86_vector_index(PhysicalContext *c,
                                  const MachineInstruction *mi) {
  if (!c || !mi || mi->info.source_instruction == AST_MIR_INVALID_ID ||
      mi->info.result_count != 1u)
    return 0;
  const AstMirFunction *mf =
      ast_mir_function_at(c->program->machine->mir, c->function);
  AstMirInstructionInfo source;
  AstMirVRegInfo base_vi;
  if (!mf ||
      !ast_mir_function_instruction(mf, mi->info.source_instruction, &source) ||
      source.op != AST_MIR_OP_INDEX || source.operand_count != 2u ||
      !x86_source_type(c, source.operands[0], &base_vi) || !base_vi.type ||
      ast_sema_type_kind(base_vi.type) != AST_SEMA_TYPE_VECTOR)
    return 0;
  const AstSemaType *element = NULL;
  uint64_t lanes = 0u;
  uint32_t bits = 0u;
  if (!x86_vector_shape(base_vi.type, &element, &lanes, &bits))
    return 0;
  uint64_t image_bytes = 0u;
  uint32_t image = x86_value_image(c, source.operands[0], &image_bytes);
  if (image == AST_MACHINE_INVALID_ID)
    return 0;
  uint64_t index = 0u;
  AstPhysicalOperand home = physical_home(c, mi->info.result_first);
  if (!home.kind)
    return 1;
  if (source_constant_u64(c, source.operands[1], &index)) {
    if (index >= lanes)
      return 0;
    return physical_move(
        c, home, physical_frame(image, bits, (int64_t)(index * (bits / 8u))));
  }
  uint32_t first = c->machine->source_first[source.operands[1]];
  if (first == AST_MACHINE_INVALID_ID ||
      c->machine->source_parts[source.operands[1]] != 1u)
    return 0;
  AstPhysicalOperand index_home = physical_home(c, first);
  if (!index_home.kind)
    return 0;
  WorkRegisters work = {0};
  if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[home.register_unit] = 1u;
  if (index_home.kind == AST_PHYSICAL_OPERAND_REGISTER)
    work.excluded[index_home.register_unit] = 1u;
  AstPhysicalOperand address =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand offset =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand rcx = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 64u);
  work.excluded[rcx.register_unit] = 1u;
  if (!address.kind || !offset.kind || !borrow_exact(c, &work, rcx) ||
      !physical_emit(
          c, AST_MACHINE_X86_LEA, 1u,
          (AstPhysicalOperand[]){address, physical_frame(image, 64u, 0)}, 2u) ||
      !extend_integer(c, offset, index_home, 0))
    return 0;
  uint32_t lane_bytes = bits / 8u;
  if (lane_bytes > 1u) {
    uint32_t shift = lane_bytes == 2u ? 1u : lane_bytes == 4u ? 2u : 3u;
    if (!physical_move(c, physical_register(c, AST_MACHINE_BANK_GPR, 1u, 8u),
                       physical_immediate(shift, 8u)) ||
        !physical_emit(c, AST_MACHINE_X86_SHL, 1u,
                       (AstPhysicalOperand[]){
                           offset, offset,
                           physical_register(c, AST_MACHINE_BANK_GPR, 1u, 8u)},
                       3u))
      return 0;
  }
  if (!physical_emit(c, AST_MACHINE_X86_ADD, 1u,
                     (AstPhysicalOperand[]){address, address, offset}, 3u))
    return 0;
  AstPhysicalOperand memory = address;
  memory.kind = AST_PHYSICAL_OPERAND_MEMORY;
  memory.bit_width = bits;
  memory.displacement = 0;
  if (!physical_move(c, home, memory))
    return 0;
  return restore_work(c, &work);
}

static int lower_x86_vector_compare(PhysicalContext *c,
                                    const MachineInstruction *mi) {
  if (!c || !mi || !mi->info.result_count || !mi->info.operand_count ||
      (mi->info.operand_count & 1u))
    return 0;
  uint32_t parts = mi->info.operand_count / 2u;
  const AstMachineOperand *first = &mi->info.operands[0];
  if (first->kind != AST_MACHINE_OPERAND_VREG ||
      first->as.vreg >= c->machine->info.vreg_count)
    return 0;
  const AstSemaType *vector_type = c->machine->vregs[first->as.vreg].type;
  const AstSemaType *element = ast_sema_type_element(vector_type);
  uint64_t lanes = 0u;
  uint32_t element_bits = element ? ast_sema_type_bit_width(element) : 0u;
  int fp = element && ast_sema_type_kind(element) == AST_SEMA_TYPE_FLOAT;
  if (!element || !ast_sema_type_vector_lanes(vector_type, &lanes) || !lanes ||
      (element_bits != 8u && element_bits != 16u && element_bits != 32u &&
       element_bits != 64u) ||
      (fp && element_bits != 32u && element_bits != 64u))
    return 0;
  AstMachineOpcode compare = x86_scalar_compare_opcode(mi->info.token_op, fp);
  if (compare == AST_MACHINE_OP_INVALID)
    return 0;

  uint64_t image_bytes = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    const AstMachineOperand *left = &mi->info.operands[part];
    const AstMachineOperand *right = &mi->info.operands[parts + part];
    if (left->kind != AST_MACHINE_OPERAND_VREG ||
        right->kind != AST_MACHINE_OPERAND_VREG ||
        left->as.vreg >= c->machine->info.vreg_count ||
        right->as.vreg >= c->machine->info.vreg_count)
      return 0;
    uint32_t bits = c->machine->vregs[left->as.vreg].bit_width;
    if (!bits || (bits % 8u) ||
        bits != c->machine->vregs[right->as.vreg].bit_width)
      return 0;
    image_bytes += (uint64_t)bits / 8u;
  }
  uint64_t image_storage = (image_bytes + 7u) & ~UINT64_C(7);
  uint32_t left_image =
      physical_temp_object(c, image_storage, AST_FRAME_OBJECT_ABI_TEMP);
  uint32_t right_image =
      physical_temp_object(c, image_storage, AST_FRAME_OBJECT_ABI_TEMP);
  if (left_image == AST_MACHINE_INVALID_ID ||
      right_image == AST_MACHINE_INVALID_ID)
    return 0;

  WorkRegisters work = {0};
  uint64_t offset = 0u;
  for (uint32_t part = 0u; part < parts; ++part) {
    uint32_t lv = mi->info.operands[part].as.vreg;
    uint32_t rv = mi->info.operands[parts + part].as.vreg;
    uint32_t bits = c->machine->vregs[lv].bit_width;
    AstPhysicalOperand lh = physical_home(c, lv), rh = physical_home(c, rv);
    if (!lh.kind || !rh.kind ||
        !physical_move(
            c, physical_frame(left_image, lh.bit_width, (int64_t)offset), lh) ||
        !physical_move(
            c, physical_frame(right_image, rh.bit_width, (int64_t)offset), rh))
      return 0;
    if (lh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[lh.register_unit] = 1u;
    if (rh.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[rh.register_unit] = 1u;
    offset += (uint64_t)bits / 8u;
  }
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand home = physical_home(c, mi->info.result_first + rp);
    if (!home.kind)
      return 0;
    if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[home.register_unit] = 1u;
  }

  AstPhysicalOperand rcx = physical_register(c, AST_MACHINE_BANK_GPR, 1u, 64u);
  work.excluded[rcx.register_unit] = 1u;
  if (!borrow_exact(c, &work, rcx))
    return 0;
  AstPhysicalOperand accumulator =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand bit_register =
      borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
  AstPhysicalOperand left_scalar =
      borrow_register(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                      fp ? element_bits : 64u);
  AstPhysicalOperand right_scalar =
      borrow_register(c, &work, fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR,
                      fp ? element_bits : 64u);
  if (!accumulator.kind || !bit_register.kind || !left_scalar.kind ||
      !right_scalar.kind)
    return 0;

  uint64_t lane = 0u;
  uint64_t lane_bytes = element_bits / 8u;
  for (uint32_t rp = 0u; rp < mi->info.result_count; ++rp) {
    AstPhysicalOperand home = physical_home(c, mi->info.result_first + rp);
    uint32_t output_bits = home.bit_width;
    if (!output_bits || output_bits > 64u)
      return 0;
    if (!physical_move(c, accumulator, physical_immediate(0u, 64u)))
      return 0;
    uint64_t remaining = lanes - lane;
    uint32_t chunk = (uint32_t)(remaining > 64u ? 64u : remaining);
    if (chunk > output_bits)
      chunk = output_bits;
    for (uint32_t li = 0u; li < chunk; ++li, ++lane) {
      uint64_t byte_offset = lane * lane_bytes;
      AstPhysicalOperand lhs = left_scalar, rhs = right_scalar;
      if (fp) {
        lhs.bit_width = rhs.bit_width = element_bits;
      } else {
        lhs = physical_register(c, AST_MACHINE_BANK_GPR,
                                left_scalar.register_unit % 64u, element_bits);
        rhs = physical_register(c, AST_MACHINE_BANK_GPR,
                                right_scalar.register_unit % 64u, element_bits);
      }
      if (!physical_move(
              c, lhs,
              physical_frame(left_image, element_bits, (int64_t)byte_offset)) ||
          !physical_move(
              c, rhs,
              physical_frame(right_image, element_bits, (int64_t)byte_offset)))
        return 0;
      AstPhysicalOperand bit8 = physical_register(
          c, AST_MACHINE_BANK_GPR, bit_register.register_unit % 64u, 8u);
      AstPhysicalOperand cmp_args[] = {bit8, lhs, rhs};
      if (!physical_emit(c, compare, 1u, cmp_args, 3u))
        return 0;
      PhysicalBlock *pb = &c->fn->blocks[c->block];
      if (pb->count)
        pb->instructions[pb->count - 1u].is_signed =
            (uint8_t)(!fp && ast_sema_type_is_signed(element));
      AstPhysicalOperand bit64 = physical_register(
          c, AST_MACHINE_BANK_GPR, bit_register.register_unit % 64u, 64u);
      AstPhysicalOperand zx[] = {bit64, bit8};
      if (!physical_emit(c, AST_MACHINE_X86_MOVZX, 1u, zx, 2u))
        return 0;
      if (li) {
        if (!physical_move(c, rcx, physical_immediate(li, 64u)))
          return 0;
        AstPhysicalOperand sh[] = {
            bit64, bit64, physical_register(c, AST_MACHINE_BANK_GPR, 1u, 8u)};
        if (!physical_emit(c, AST_MACHINE_X86_SHL, 1u, sh, 3u))
          return 0;
      }
      AstPhysicalOperand or_args[] = {accumulator, accumulator, bit64};
      if (!physical_emit(c, AST_MACHINE_X86_OR, 1u, or_args, 3u))
        return 0;
    }
    AstPhysicalOperand packed = physical_register(
        c, AST_MACHINE_BANK_GPR, accumulator.register_unit % 64u,
        output_bits < 8u ? 8u : output_bits);
    if (!physical_move(c, home, packed))
      return 0;
  }
  return lane == lanes && restore_work(c, &work);
}

static int lower_regular(PhysicalContext *c, const MachineInstruction *mi) {
  if (lower_x86_vector_binary(c, mi))
    return 1;
  if (mi->info.opcode == AST_MACHINE_X86_VEC_CMP)
    return lower_x86_vector_compare(c, mi);
  if (mi->info.opcode == AST_MACHINE_X86_SELECT &&
      (mi->info.source_op == AST_MIR_OP_SELECT ||
       mi->info.source_op == AST_MIR_OP_INTRINSIC)) {
    const AstMirFunction *mf =
        ast_mir_function_at(c->program->machine->mir, c->function);
    AstMirInstructionInfo source;
    AstMirVRegInfo vi;
    if (mf &&
        ast_mir_function_instruction(mf, mi->info.source_instruction,
                                     &source) &&
        source.result != AST_MIR_INVALID_ID &&
        ast_mir_function_vreg(mf, source.result, &vi) && vi.type &&
        ast_sema_type_kind(vi.type) == AST_SEMA_TYPE_VECTOR)
      return lower_x86_vector_select(c, mi);
  }
  if (mi->info.source_op == AST_MIR_OP_STACK_LOAD ||
      mi->info.source_op == AST_MIR_OP_STACK_STORE) {
    if (!mi->info.operand_count)
      return 0;
    const AstMachineOperand *slot =
        &mi->info.operands[mi->info.operand_count - 1u];
    AstMirFunctionInfo info = {0};
    if (!ast_mir_function_info(
            ast_mir_function_at(c->program->machine->mir, c->function),
            &info) ||
        slot->kind != AST_MACHINE_OPERAND_STACK_SLOT ||
        slot->as.stack_slot >= info.stack_slot_count)
      return 0;
    int load = mi->info.source_op == AST_MIR_OP_STACK_LOAD;
    uint32_t parts = load ? mi->info.result_count : mi->info.operand_count - 1u;
    uint32_t object = c->locals[slot->as.stack_slot];
    if (object >= c->fn->object_count)
      return 0;
    uint64_t offset = 0u, size = c->fn->objects[object].size;
    for (uint32_t part = 0; part < parts; ++part) {
      if (!load && mi->info.operands[part].kind != AST_MACHINE_OPERAND_VREG)
        return 0;
      uint32_t v =
          load ? mi->info.result_first + part : mi->info.operands[part].as.vreg;
      AstPhysicalOperand home = physical_home(c, v);
      uint64_t bytes = ((uint64_t)c->machine->vregs[v].bit_width + 7u) / 8u;
      if (offset >= size)
        return 0;
      if (bytes > size - offset)
        bytes = size - offset;
      uint32_t bits = (uint32_t)bytes * 8u;
      if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
        home = physical_register(c, home.bank, home.register_unit % 64u, bits);
      else
        home.bit_width = bits;
      AstPhysicalOperand memory = physical_frame(object, bits, (int64_t)offset);
      if (home.kind &&
          !physical_move(c, load ? home : memory, load ? memory : home))
        return 0;
      offset += bytes;
    }
    return offset == size;
  }
  WorkRegisters work = {0};
  AstPhysicalOperand uses[AST_PHYSICAL_MAX_OPERANDS] = {{0}};
  AstPhysicalOperand result = {0}, home = {0};
  uint32_t count = 0u;
  if (mi->info.result_count > 1u || mi->info.operand_count > 6u)
    return fail_instruction(c,
                            "Multi-part arithmetic needs legalized carry/lane "
                            "operations before physical lowering.");
  if (mi->info.result_count) {
    home = physical_home(c, mi->info.result_first);
    if (!home.kind)
      return 1; /* Unused, pure definitions only enter here. */
    if (home.kind == AST_PHYSICAL_OPERAND_REGISTER)
      work.excluded[home.register_unit] = 1u;
  }
  for (uint32_t i = 0; i < mi->info.operand_count; ++i) {
    const AstMachineOperand *op = &mi->info.operands[i];
    if (op->kind == AST_MACHINE_OPERAND_VREG) {
      uses[count] = physical_home(c, op->as.vreg);
      if (uses[count].kind == AST_PHYSICAL_OPERAND_REGISTER)
        work.excluded[uses[count].register_unit] = 1u;
    } else if (op->kind == AST_MACHINE_OPERAND_STACK_SLOT) {
      uses[count] =
          physical_frame(c->locals[op->as.stack_slot], home.bit_width, 0);
    } else if (op->kind == AST_MACHINE_OPERAND_SYMBOL) {
      uses[count].kind = AST_PHYSICAL_OPERAND_SYMBOL;
      uses[count].symbol = op->as.symbol;
      uses[count].module = op->module;
    } else if (op->kind == AST_MACHINE_OPERAND_IMMEDIATE) {
      uses[count] =
          physical_immediate((uint64_t)op->as.immediate, home.bit_width);
    } else
      return 0;
    ++count;
  }
  if (home.kind) {
    result = home.kind == AST_PHYSICAL_OPERAND_REGISTER
                 ? home
                 : borrow_register(
                       c, &work, c->machine->vregs[mi->info.result_first].bank,
                       home.bit_width);
    if (!result.kind)
      return 0;
  }
  int ok = 0;
  if (mi->info.opcode == AST_MACHINE_X86_LOAD ||
      mi->info.opcode == AST_MACHINE_X86_LEA) {
    if (count != 1u)
      return fail_instruction(
          c, "Load/address requires exactly one selected address.");
    AstPhysicalOperand address = uses[0];
    if (mi->info.operands[0].kind == AST_MACHINE_OPERAND_VREG) {
      address = ensure_register(c, &work, address, AST_MACHINE_BANK_GPR);
      if (!address.kind)
        return 0;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    if (mi->info.opcode == AST_MACHINE_X86_LOAD &&
        address.kind == AST_PHYSICAL_OPERAND_SYMBOL) {
      /* A GPR load result may hold the address until the load overwrites
         it.  Reusing that register avoids a second address scratch plus
         its emergency save/restore around ordinary global loads. */
      AstPhysicalOperand pointer =
          result.bank == AST_MACHINE_BANK_GPR &&
                  result.kind == AST_PHYSICAL_OPERAND_REGISTER
              ? physical_register(c, AST_MACHINE_BANK_GPR,
                                  result.register_unit % 64u, 64u)
              : borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
      AstPhysicalOperand address_args[] = {pointer, address};
      if (!pointer.kind ||
          !physical_emit(c, AST_MACHINE_X86_LEA, 1u, address_args, 2u))
        return 0;
      address = pointer;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    address.bit_width = result.bit_width;
    AstPhysicalOperand args[] = {result, address};
    ok = physical_emit(c, mi->info.opcode, 1u, args, 2u);
  } else if (mi->info.opcode == AST_MACHINE_X86_STORE) {
    if (count != 2u)
      return fail_instruction(c, "Store requires a value and an address.");
    AstPhysicalOperand value, address;
    uint32_t value_index;
    if (mi->info.source_op == AST_MIR_OP_STORE) {
      address = ensure_register(c, &work, uses[0], AST_MACHINE_BANK_GPR);
      if (!address.kind)
        return 0;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      value = uses[1];
      value_index = 1u;
    } else {
      value = uses[0];
      address = uses[1];
      value_index = 0u;
    }
    if (value.kind != AST_PHYSICAL_OPERAND_REGISTER &&
        value.kind != AST_PHYSICAL_OPERAND_IMMEDIATE) {
      uint32_t v = mi->info.operands[value_index].as.vreg;
      value = ensure_register(c, &work, value, c->machine->vregs[v].bank);
    }
    if (address.kind == AST_PHYSICAL_OPERAND_SYMBOL) {
      AstPhysicalOperand pointer =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, 64u);
      AstPhysicalOperand address_args[] = {pointer, address};
      if (!pointer.kind ||
          !physical_emit(c, AST_MACHINE_X86_LEA, 1u, address_args, 2u))
        return 0;
      address = pointer;
      address.kind = AST_PHYSICAL_OPERAND_MEMORY;
      address.displacement = 0;
    }
    address.bit_width = value.bit_width;
    if ((mi->info.flags & AST_MIR_INST_NONTEMPORAL) &&
        ((value.bank == AST_MACHINE_BANK_GPR &&
          (value.bit_width == 32u || value.bit_width == 64u)) ||
         (value.bank == AST_MACHINE_BANK_VECTOR && value.bit_width == 128u))) {
      /* x86-64 baseline has the verified SSE2 MOVNTI/MOVNTDQ forms.  A
         non-register value is materialized first because streaming
         stores do not have an immediate or memory source form. */
      if (value.kind != AST_PHYSICAL_OPERAND_REGISTER) {
        uint32_t v = mi->info.operands[value_index].as.vreg;
        value = ensure_register(c, &work, value, c->machine->vregs[v].bank);
        if (!value.kind)
          return 0;
      }
      AstPhysicalOperand args[] = {value, address};
      ok = physical_emit(c, AST_MACHINE_X86_STORE, 0u, args, 2u);
      if (ok) {
        PhysicalBlock *pb = &c->fn->blocks[c->block];
        if (!pb->count)
          return 0;
        pb->instructions[pb->count - 1u].nontemporal = 1u;
      }
    } else {
      /* Advisory hint: retain ordinary semantically-correct memory
         access where this backend has no verified streaming form. */
      ok = physical_move(c, address, value);
    }
  } else {
    uint64_t rhs_constant = 0u;
    uint32_t rhs_bits = home.bit_width;
    if (mi->info.opcode >= AST_MACHINE_X86_ICMP_EQ &&
        mi->info.opcode <= AST_MACHINE_X86_ICMP_GE && count)
      rhs_bits = uses[0].bit_width;
    int rhs_immediate =
        count == 2u && mi->info.operands[1].kind == AST_MACHINE_OPERAND_VREG &&
        source_constant_bits(c, mi->info.operands[1].as.vreg, &rhs_constant) &&
        x86_scalar_immediate_ok(mi->info.opcode, rhs_bits, rhs_constant);
    for (uint32_t i = 0; i < count; ++i) {
      if (mi->info.operands[i].kind != AST_MACHINE_OPERAND_VREG)
        return 0;
      uint32_t v = mi->info.operands[i].as.vreg;
      if (i == 1u && rhs_immediate) {
        uses[i] = physical_immediate(rhs_constant, rhs_bits);
        continue;
      }
      uses[i] = ensure_register(c, &work, uses[i], c->machine->vregs[v].bank);
      if (!uses[i].kind)
        return 0;
    }
    int comparison = (mi->info.opcode >= AST_MACHINE_X86_ICMP_EQ &&
                      mi->info.opcode <= AST_MACHINE_X86_ICMP_GE) ||
                     (mi->info.opcode >= AST_MACHINE_X86_FCMP_EQ &&
                      mi->info.opcode <= AST_MACHINE_X86_FCMP_GE);
    int unary = mi->info.opcode == AST_MACHINE_X86_NEG ||
                mi->info.opcode == AST_MACHINE_X86_NOT;
    if (mi->info.opcode == AST_MACHINE_X86_MUL && result.bit_width == 8u) {
      /* x86 has no two-operand IMUL form for byte registers.  Compute
         modulo 2^8 in 32-bit GPRs and truncate the low byte.  Zero-
         extension is sufficient for both signed and unsigned wrapping
         multiplication because the low 8 product bits are identical. */
      AstPhysicalOperand wide_lhs =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, 32u);
      AstPhysicalOperand wide_rhs =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, 32u);
      if (!wide_lhs.kind || !wide_rhs.kind ||
          !extend_integer(c, wide_lhs, uses[0], 0) ||
          !extend_integer(c, wide_rhs, uses[1], 0))
        return 0;
      AstPhysicalOperand mul_args[] = {wide_lhs, wide_lhs, wide_rhs};
      AstPhysicalOperand low = physical_register(
          c, AST_MACHINE_BANK_GPR, wide_lhs.register_unit % 64u, 8u);
      ok = physical_emit(c, AST_MACHINE_X86_MUL, 1u, mul_args, 3u) &&
           physical_move(c, result, low);
    } else if (mi->info.opcode == AST_MACHINE_X86_NEG &&
               result.bank == AST_MACHINE_BANK_FP) {
      /* Flip the IEEE sign bit. Integer NEG on an XMM register is
       * invalid; 0-x would also lose the sign of zero and NaN payloads. */
      if (count != 1u || (result.bit_width != 32u && result.bit_width != 64u))
        return 0;
      AstPhysicalOperand bits =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, result.bit_width);
      AstPhysicalOperand mask =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, result.bit_width);
      AstPhysicalOperand to_bits[] = {bits, uses[0]};
      AstPhysicalOperand flip[] = {bits, bits, mask};
      AstPhysicalOperand to_float[] = {result, bits};
      ok = bits.kind && mask.kind &&
           physical_emit(c, AST_MACHINE_X86_FMOV_BITS, 1u, to_bits, 2u) &&
           physical_move(
               c, mask,
               physical_immediate(UINT64_C(1) << (result.bit_width - 1u),
                                  result.bit_width)) &&
           physical_emit(c, AST_MACHINE_X86_XOR, 1u, flip, 3u) &&
           physical_emit(c, AST_MACHINE_X86_FMOV_BITS, 1u, to_float, 2u);
    } else if (mi->info.opcode == AST_MACHINE_X86_NOT &&
               mi->info.token_op == AST_TOK_BANG) {
      if (count != 1u)
        return 0;
      AstPhysicalOperand zero =
          borrow_register(c, &work, AST_MACHINE_BANK_GPR, uses[0].bit_width);
      if (!zero.kind ||
          !physical_move(c, zero, physical_immediate(0u, zero.bit_width)))
        return 0;
      AstPhysicalOperand args[] = {result, uses[0], zero};
      ok = physical_emit(c, AST_MACHINE_X86_ICMP_EQ, 1u, args, 3u);
    } else if (mi->info.opcode == AST_MACHINE_X86_CAST) {
      ok = count == 1u && lower_scalar_cast(c, &work, mi, result, uses[0]);
    } else if (comparison || mi->info.opcode == AST_MACHINE_X86_SELECT) {
      AstPhysicalOperand args[AST_PHYSICAL_MAX_OPERANDS] = {{0}};
      args[0] = result;
      for (uint32_t i = 0; i < count; ++i)
        args[i + 1u] = uses[i];
      ok = physical_emit(c, mi->info.opcode, 1u, args, count + 1u);
    } else {
      if (count != (unary ? 1u : 2u))
        return 0;
      int commutative = mi->info.opcode == AST_MACHINE_X86_ADD ||
                        mi->info.opcode == AST_MACHINE_X86_MUL ||
                        mi->info.opcode == AST_MACHINE_X86_AND ||
                        mi->info.opcode == AST_MACHINE_X86_OR ||
                        mi->info.opcode == AST_MACHINE_X86_XOR;
      if (!unary && uses[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
          result.register_unit == uses[1].register_unit &&
          result.register_unit != uses[0].register_unit) {
        if (commutative && uses[0].kind == AST_PHYSICAL_OPERAND_REGISTER) {
          AstPhysicalOperand swap = uses[0];
          uses[0] = uses[1];
          uses[1] = swap;
        } else {
          result = borrow_register(c, &work, result.bank, result.bit_width);
          if (!result.kind)
            return 0;
        }
      }
      if (!physical_move(c, result, uses[0]))
        return 0;
      AstPhysicalOperand args[] = {result, result, unary ? result : uses[1]};
      ok = physical_emit(c, mi->info.opcode, 1u, args, unary ? 2u : 3u);
    }
  }
  if (!ok || (home.kind && !physical_move(c, home, result)))
    return 0;
  return restore_work(c, &work);
}

int physical_lower_function(AstRegAllocProgram *p, uint32_t f) {
  AllocationFunction *af = &p->functions[f];
  const MachineFunction *machine = &p->machine->functions[f];
  PhysicalContext c = {.program = p,
                       .fn = af,
                       .machine = machine,
                       .function = f,
                       .block = machine->info.entry_block,
                       .instruction = AST_MACHINE_INVALID_ID};
  const AstMirFunction *mir = ast_mir_function_at(p->machine->mir, f);
  AstMirFunctionInfo info;
  if (!ast_mir_function_info(mir, &info))
    return 0;
  int handled = 0;
  if (!physical_native_function_policy(&c, &handled))
    return 0;
  if (handled)
    return 1;
  if (!frame_init_register_objects(af))
    goto fail;
  c.locals = (uint32_t *)malloc(
      (info.stack_slot_count ? info.stack_slot_count : 1u) * sizeof(uint32_t));
  c.call_sites = (uint32_t *)malloc(
      (machine->info.instruction_count ? machine->info.instruction_count : 1u) *
      sizeof(uint32_t));
  if (!af->callee_save_objects || !af->emergency_objects || !af->work_objects ||
      !c.locals || !c.call_sites)
    goto fail;
  for (uint32_t i = 0; i < machine->info.instruction_count; ++i)
    c.call_sites[i] = AST_MACHINE_INVALID_ID;
  for (uint32_t j = 0; j < machine->info.instruction_count; ++j) {
    AstMirOp op = machine->instructions[j].info.source_op;
    if (op != AST_MIR_OP_CALL_DIRECT && op != AST_MIR_OP_CALL_INDIRECT)
      continue;
    size_t index;
    if (!abi_find_call(p->machine->abi, f,
                       machine->instructions[j].info.source_instruction, &index,
                       NULL) ||
        index > UINT32_MAX)
      goto fail;
    c.call_sites[j] = (uint32_t)index;
  }
  for (uint32_t local = 0; local < info.stack_slot_count; ++local) {
    AstMirStackSlotInfo slot;
    if (!ast_mir_function_stack_slot(mir, local, &slot) || !slot.size ||
        (slot.alignment > 16u && p->abi != AST_ABI_SYSV_AMD64) ||
        (slot.flags & AST_MIR_STACK_SLOT_DYNAMIC_SIZE)) {
      regalloc_fail(
          p, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, f, AST_MACHINE_INVALID_ID,
          "Dynamic or over-aligned local stack objects are unsupported.");
      goto fail;
    }
    c.locals[local] =
        frame_add_object(af, AST_FRAME_OBJECT_LOCAL, slot.size, slot.alignment);
    if (c.locals[local] == AST_MACHINE_INVALID_ID)
      goto fail;
  }
  for (uint32_t v = 0; v < machine->info.vreg_count; ++v)
    for (uint32_t pi = 0; pi < regalloc_location_piece_count(af, v); ++pi) {
      const AstRegAllocLocation *loc = regalloc_location_piece(af, v, pi);
      MachinePhysRegView view;
      if (loc->kind != AST_REGALLOC_LOCATION_REGISTER)
        continue;
      if (!machine_physreg_view(&p->target, loc->register_name, &view))
        goto fail;
      uint32_t preserved = machine_physreg_preserved_bits(p->abi, &view);
      if (preserved &&
          af->callee_save_objects[view.alias_unit] == AST_MACHINE_INVALID_ID) {
        uint32_t bytes = preserved / 8u;
        af->callee_save_objects[view.alias_unit] =
            frame_add_object(af, AST_FRAME_OBJECT_CALLEE_SAVE, bytes,
                             bytes >= 16u ? 16u : bytes);
        if (af->callee_save_objects[view.alias_unit] == AST_MACHINE_INVALID_ID)
          goto fail;
      }
    }
  for (uint32_t b = 0; b < machine->info.block_count; ++b)
    if (add_block(&c) == AST_MACHINE_INVALID_ID)
      goto fail;
  af->frame.realigns_stack = (uint8_t)(frame_required_alignment(p, f) > 16u);
  if (!physical_abi_entry(&c))
    goto fail;
  for (uint32_t b = 0; b < machine->info.block_count; ++b) {
    c.block = b;
    const MachineBlock *block = &machine->blocks[b];
    for (uint32_t i = 0; i < block->info.instruction_count; ++i) {
      c.instruction = block->instructions[i];
      const MachineInstruction *mi = &machine->instructions[c.instruction];
      int ok;
      if (mi->dead)
        ok = 1;
      else
        switch (mi->info.opcode) {
        case AST_MACHINE_X86_MOV_IMM:
        case AST_MACHINE_X86_UNDEF:
          ok = lower_constant(&c, mi);
          break;
        case AST_MACHINE_X86_MOV:
          ok = lower_copy(&c, mi);
          break;
        case AST_MACHINE_X86_BR:
        case AST_MACHINE_X86_COND_BR:
          ok = lower_branch(&c, mi);
          break;
        case AST_MACHINE_X86_RET:
          ok = physical_abi_return(&c, mi);
          break;
        case AST_MACHINE_X86_CALL:
        case AST_MACHINE_X86_CALL_INDIRECT:
          ok = c.call_sites[c.instruction] != AST_MACHINE_INVALID_ID &&
               physical_abi_call(&c, mi, c.call_sites[c.instruction]);
          break;
        case AST_MACHINE_X86_TRAP:
        case AST_MACHINE_X86_FENCE:
          ok = physical_emit(&c, mi->info.opcode, 0u, NULL, 0u);
          break;
        case AST_MACHINE_X86_SDIV:
        case AST_MACHINE_X86_UDIV:
        case AST_MACHINE_X86_SREM:
        case AST_MACHINE_X86_UREM:
        case AST_MACHINE_X86_SHL:
        case AST_MACHINE_X86_ASHR:
        case AST_MACHINE_X86_LSHR:
          ok = lower_fixed_binary(&c, mi);
          break;
        case AST_MACHINE_X86_NATIVE:
        case AST_MACHINE_X86_NATIVE_RESULT:
          ok = physical_native_instruction(&c, mi);
          break;
        case AST_MACHINE_X86_BIND:
          ok = regalloc_verify_bind_instruction(p, f, mi);
          break;
        case AST_MACHINE_X86_INTRINSIC:
          ok = lower_x86_vector_binary(&c, mi) ||
               (mi->info.source_op == AST_MIR_OP_PACK && lower_pack(&c, mi)) ||
               fail_instruction(&c, "Selected intrinsic still requires "
                                    "target-specific constraint expansion.");
          break;
        case AST_MACHINE_X86_INDEX:
        case AST_MACHINE_X86_MEMBER:
          ok = (mi->info.source_op == AST_MIR_OP_INDEX &&
                lower_mask_index(&c, mi)) ||
               (mi->info.source_op == AST_MIR_OP_INDEX &&
                lower_x86_vector_index(&c, mi)) ||
               lower_aggregate_extract(&c, mi);
          break;
        case AST_MACHINE_X86_ATOMIC:
          ok = physical_atomic_instruction(&c, mi);
          break;
        case AST_MACHINE_X86_FREM:
          ok = fail_instruction(&c, "Selected opcode still requires "
                                    "target-specific constraint expansion.");
          break;
        default:
          ok = lower_regular(&c, mi);
          break;
        }
      if (!ok) {
        if (!p->stats.error_count)
          fail_instruction(&c, "Selected instruction cannot be materialized "
                               "with its operand contract.");
        goto fail;
      }
    }
  }
  if (!physical_run_peephole(&c) || !physical_x86_late_cfg_cleanup(&c) ||
      !frame_layout(p, f))
    goto fail;
  c.block = machine->info.entry_block;
  c.instruction = AST_MACHINE_INVALID_ID;
  if (!physical_emit_prologue(&c) || !physical_resolve_frames(&c))
    goto fail;
  free(c.locals);
  free(c.call_sites);
  return 1;
fail:
  free(c.locals);
  free(c.call_sites);
  return 0;
}

static int physical_x86_late_cfg_cleanup(PhysicalContext *c) {
  if (!c || !c->fn || !c->machine)
    return 0;
  for (uint32_t b = 0; b < c->fn->block_count; ++b) {
    PhysicalBlock *pb = &c->fn->blocks[b];
    if (pb->count < 2u)
      continue;
    AstPhysicalInstructionInfo *branch = &pb->instructions[pb->count - 1u];
    AstPhysicalInstructionInfo *cmp = &pb->instructions[pb->count - 2u];
    if (branch->opcode != AST_MACHINE_X86_COND_BR ||
        branch->operand_count != 3u || cmp->opcode < AST_MACHINE_X86_ICMP_EQ ||
        cmp->opcode > AST_MACHINE_X86_ICMP_GE || cmp->result_count != 1u ||
        cmp->operand_count != 3u ||
        !physical_same_location(&cmp->operands[0], &branch->operands[0]) ||
        branch->operands[1].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        branch->operands[2].kind != AST_PHYSICAL_OPERAND_BLOCK ||
        cmp->source_instruction >= c->machine->info.instruction_count)
      continue;
    const MachineInstruction *source =
        &c->machine->instructions[cmp->source_instruction];
    if (source->info.result_count != 1u ||
        source->info.result_first >= c->fn->liveness.vreg_count ||
        c->fn->liveness.intervals[source->info.result_first].use_count != 1u)
      continue;
    const MachineLiveInterval *iv =
        &c->fn->liveness.intervals[source->info.result_first];
    if (!iv->uses || iv->uses[0].instruction != branch->source_instruction)
      continue;

    AstPhysicalInstructionInfo fused = *cmp;
    memset(fused.operands, 0, sizeof(fused.operands));
    fused.opcode = AST_MACHINE_X86_ICMP_BR;
    fused.result_count = 0u;
    fused.operand_count = 5u;
    fused.operands[0] = physical_immediate((uint64_t)cmp->opcode, 32u);
    fused.operands[1] = cmp->operands[1];
    fused.operands[2] = cmp->operands[2];
    fused.operands[3] = branch->operands[1];
    fused.operands[4] = branch->operands[2];
    fused.source_instruction = branch->source_instruction;
    fused.span = branch->span;
    pb->instructions[pb->count - 2u] = fused;
    --pb->count;
  }
  return 1;
}

static int physical_source_function_is_extern(const AstRegAllocProgram *p,
                                              uint32_t function) {
  if (!p || !p->machine || !p->machine->mir)
    return 0;
  const AstMirFunction *mf = ast_mir_function_at(p->machine->mir, function);
  AstMirFunctionInfo info;
  return mf && ast_mir_function_info(mf, &info) &&
         (info.flags & AST_HIR_FUNCTION_EXTERN) != 0u;
}

int regalloc_finalize_function(AstRegAllocProgram *p, uint32_t f) {
  if (physical_source_function_is_extern(p, f))
    return 1;
  int ok = p->target.arch == AST_TARGET_ARCH_AARCH64
               ? physical_lower_aarch64(p, f)
           : p->target.arch == AST_TARGET_ARCH_RISCV64
               ? physical_lower_riscv64(p, f)
               : physical_lower_function(p, f);
  if (!ok) {
    if (!p->stats.error_count)
      regalloc_fail(p, AST_REGALLOC_DIAG_OUT_OF_MEMORY, f,
                    AST_MACHINE_INVALID_ID, "Physical lowering failed.");
    return 0;
  }
  p->stats.frame_bytes += p->functions[f].frame.stack_adjustment;
  for (uint32_t b = 0; b < p->functions[f].block_count; ++b)
    p->stats.physical_instructions += p->functions[f].blocks[b].count;
  return 1;
}
static int finalize_task(AstRegAllocProgram *p, uint32_t f, void *context) {
  (void)context;
  return regalloc_finalize_function(p, f);
}
int ast_regalloc_finalize(AstRegAllocProgram *p) {
  if (!p || p->finalized)
    return 0;
  if (!p->allocation_verified && !ast_regalloc_verify(p))
    return 0;
  if (!regalloc_parallel_functions(p, finalize_task, NULL))
    return 0;
  p->finalized = 1u;
  if (!ast_physical_verify(p))
    return regalloc_fail(p, AST_REGALLOC_DIAG_INVALID_PHYSICAL, 0u,
                         AST_MACHINE_INVALID_ID,
                         "Physical verification failed.");
  return 1;
}
