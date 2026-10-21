#include "../../abi/abi_lookup.h"
#include "../regalloc/allocation_internal.h"
#include <limits.h>

int frame_init_register_objects(AllocationFunction *fn) {
  if (!fn || fn->callee_save_objects || fn->emergency_objects ||
      fn->work_objects)
    return 0;
  size_t count = (size_t)MACHINE_REGISTER_UNITS * 3u;
  uint32_t *storage = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!storage)
    return 0;
  uint32_t *callee = storage;
  uint32_t *emergency = storage + MACHINE_REGISTER_UNITS;
  uint32_t *work = emergency + MACHINE_REGISTER_UNITS;
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u)
    callee[u] = emergency[u] = work[u] = AST_MACHINE_INVALID_ID;
  fn->callee_save_objects = callee;
  fn->emergency_objects = emergency;
  fn->work_objects = work;
  fn->register_object_storage = storage;
  return 1;
}

uint32_t frame_add_object(AllocationFunction *fn, AstFrameObjectKind kind,
                          uint64_t size, uint32_t align) {
  if (!fn || !size || !align || (align & (align - 1u)) || size > INT32_MAX)
    return AST_MACHINE_INVALID_ID;
  if (fn->object_count == fn->object_capacity) {
    uint32_t cap = fn->object_capacity ? fn->object_capacity * 2u : 16u;
    size_t bytes;
    if (cap < fn->object_capacity ||
        ast_size_mul_overflow(cap, sizeof(*fn->objects), &bytes))
      return AST_MACHINE_INVALID_ID;
    AstFrameObjectInfo *next =
        (AstFrameObjectInfo *)realloc(fn->objects, bytes);
    if (!next)
      return AST_MACHINE_INVALID_ID;
    fn->objects = next;
    fn->object_capacity = cap;
  }
  uint32_t id = fn->object_count++;
  fn->objects[id] = (AstFrameObjectInfo){id, kind, size, align, 0};
  return id;
}
static uint64_t align_up(uint64_t n, uint32_t a) {
  return (n + a - 1u) & ~((uint64_t)a - 1u);
}
uint32_t frame_required_alignment(const AstRegAllocProgram *p, uint32_t f) {
  uint32_t alignment = 16u;
  const AllocationFunction *af = &p->functions[f];
  for (uint32_t o = 0; o < af->object_count; ++o)
    if (af->objects[o].alignment > alignment)
      alignment = af->objects[o].alignment;
  const AstAbiProgram *abi = p->machine ? p->machine->abi : NULL;
  for (size_t i = abi_call_lower_bound(abi, f, 0u); i < ast_abi_call_count(abi);
       ++i) {
    AstAbiCallInfo call;
    if (!ast_abi_call_info(abi, i, &call))
      return 0;
    if (call.caller_function_index != f)
      break;
    if (call.stack_alignment > alignment)
      alignment = call.stack_alignment;
  }
  return alignment;
}
int frame_layout(AstRegAllocProgram *p, uint32_t f) {
  AllocationFunction *af = &p->functions[f];
  uint64_t cursor = af->frame.outgoing_size;
  af->frame.alignment = frame_required_alignment(p, f);
  if (!af->frame.alignment ||
      (af->frame.alignment & (af->frame.alignment - 1u)) ||
      af->frame.alignment > INT32_MAX ||
      (af->frame.alignment > 16u && p->abi != AST_ABI_SYSV_AMD64))
    return regalloc_fail(
        p, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, f, AST_MACHINE_INVALID_ID,
        "Stack realignment is supported for SysV x86-64 frames.");
  af->frame.realigns_stack = (uint8_t)(af->frame.alignment > 16u);
  for (uint32_t o = 0; o < af->object_count; ++o) {
    AstFrameObjectInfo *obj = &af->objects[o];
    if (!obj->alignment || (obj->alignment & (obj->alignment - 1u)) ||
        obj->alignment > af->frame.alignment)
      return regalloc_fail(
          p, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, f, AST_MACHINE_INVALID_ID,
          "Over-aligned frames require dynamic stack realignment.");
    cursor = align_up(cursor, obj->alignment);
    obj->offset = (int64_t)cursor;
    cursor += obj->size;
    if (cursor > (uint64_t)INT32_MAX - af->frame.alignment)
      return regalloc_fail(p, AST_REGALLOC_DIAG_UNSUPPORTED_FRAME, f,
                           AST_MACHINE_INVALID_ID,
                           "Frame exceeds signed x86 displacement range.");
  }
  af->frame.object_count = af->object_count;
  af->frame.stack_adjustment =
      af->frame.realigns_stack        ? align_up(cursor, af->frame.alignment)
      : cursor || af->frame.has_calls ? align_up(cursor + 8u, 16u) - 8u
                                      : 0u;
  af->frame.uses_red_zone = 0u;
  if (p->config.enable_red_zone && p->abi == AST_ABI_SYSV_AMD64 &&
      !af->frame.realigns_stack && !af->frame.has_calls &&
      !af->frame.outgoing_size && cursor <= 128u) {
    uint64_t depth = 0u;
    for (uint32_t o = 0; o < af->object_count; ++o) {
      AstFrameObjectInfo *obj = &af->objects[o];
      depth += obj->size;
      while (((8u - depth) & (obj->alignment - 1u)) != 0u)
        ++depth;
    }
    if (depth <= 128u) {
      depth = 0u;
      for (uint32_t o = 0; o < af->object_count; ++o) {
        AstFrameObjectInfo *obj = &af->objects[o];
        depth += obj->size;
        while (((8u - depth) & (obj->alignment - 1u)) != 0u)
          ++depth;
        obj->offset = -(int64_t)depth;
      }
      af->frame.stack_adjustment = 0u;
      af->frame.uses_red_zone = af->object_count != 0u;
    }
  }
  af->frame.needs_stack_probe = (uint8_t)(p->abi == AST_ABI_WINDOWS_X64 &&
                                          af->frame.stack_adjustment >= 4096u);
  return 1;
}
int ast_regalloc_frame(const AstRegAllocProgram *p, size_t f,
                       AstFrameInfo *out) {
  if (!p || !out || !p->finalized || p->stats.error_count ||
      f >= p->function_count)
    return 0;
  *out = p->functions[f].frame;
  return 1;
}
int ast_regalloc_frame_object(const AstRegAllocProgram *p, size_t f, uint32_t o,
                              AstFrameObjectInfo *out) {
  if (!p || !out || !p->finalized || p->stats.error_count ||
      f >= p->function_count || o >= p->functions[f].object_count)
    return 0;
  *out = p->functions[f].objects[o];
  return 1;
}
