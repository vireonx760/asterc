#include "../../isa/riscv64_memory_contract.h"
#include "../../isa/riscv64_scalar_contract.h"
#include "../../isa/system_contract.h"
#include "../postra/atomic_contract.h"
#include "riscv64_internal.h"
#include <limits.h>
#define RV_SP (MACHINE_PHYSREG_FILE_GPR * 64u + 2u)
static int rv_load(AstMachineOpcode op) {
  return (op >= AST_MACHINE_RISCV_LB && op <= AST_MACHINE_RISCV_LD) ||
         op == AST_MACHINE_RISCV_FLW || op == AST_MACHINE_RISCV_FLD;
}
static int rv_store(AstMachineOpcode op) {
  return (op >= AST_MACHINE_RISCV_SB && op <= AST_MACHINE_RISCV_SD) ||
         op == AST_MACHINE_RISCV_FSW || op == AST_MACHINE_RISCV_FSD;
}
static int rv_register(const AstRegAllocProgram *p,
                       const AstPhysicalOperand *o) {
  MachinePhysRegView v;
  if (!memchr(o->register_name, 0, sizeof(o->register_name)) ||
      o->register_name[0] == '@' ||
      !machine_physreg_view(&p->target, o->register_name, &v) ||
      v.alias_unit != o->register_unit || !o->bit_width)
    return 0;
  if (o->kind == AST_PHYSICAL_OPERAND_MEMORY)
    return v.file == MACHINE_PHYSREG_FILE_GPR && v.width == 64u &&
           o->displacement >= INT32_MIN && o->displacement <= INT32_MAX;
  return o->bit_width <= v.width &&
         machine_register_compatible(&p->target, o->register_name, o->bank);
}
static int rv_instruction(const AstPhysicalInstructionInfo *i) {
  if (rv_scalar_form(i->opcode))
    return rv_scalar_contract(i);
  if (rv_memory_form(i->opcode))
    return rv_memory_contract(i);
  uint32_t n = i->operand_count, d = i->result_count;
  const AstPhysicalOperand *o = i->operands;
  if (rv_load(i->opcode) || rv_store(i->opcode)) {
    uint32_t width;
    int fp = i->opcode == AST_MACHINE_RISCV_FLW ||
             i->opcode == AST_MACHINE_RISCV_FLD ||
             i->opcode == AST_MACHINE_RISCV_FSW ||
             i->opcode == AST_MACHINE_RISCV_FSD;
    switch (i->opcode) {
    case AST_MACHINE_RISCV_LB:
    case AST_MACHINE_RISCV_LBU:
    case AST_MACHINE_RISCV_SB:
      width = 8u;
      break;
    case AST_MACHINE_RISCV_LH:
    case AST_MACHINE_RISCV_LHU:
    case AST_MACHINE_RISCV_SH:
      width = 16u;
      break;
    case AST_MACHINE_RISCV_LW:
    case AST_MACHINE_RISCV_LWU:
    case AST_MACHINE_RISCV_SW:
    case AST_MACHINE_RISCV_FLW:
    case AST_MACHINE_RISCV_FSW:
      width = 32u;
      break;
    default:
      width = 64u;
      break;
    }
    return n == 2u && d == (uint32_t)rv_load(i->opcode) &&
           o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
           o[0].bank == (fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR) &&
           ((width == 8u && o[1].bit_width <= 8u) || o[1].bit_width == width) &&
           o[0].bit_width >= o[1].bit_width && (!fp || o[0].bit_width == width);
  }
  switch (i->opcode) {
  case AST_MACHINE_RISCV_SYSTEM:
    return system_physical_contract(i) && !system_exception_return(i);
  case AST_MACHINE_RISCV_LRSC_ATOMIC:
    return atomic_physical_contract(i);
  case AST_MACHINE_RISCV_UNDEF:
  case AST_MACHINE_RISCV_LOAD:
  case AST_MACHINE_RISCV_STORE:
  case AST_MACHINE_RISCV_CMP:
  case AST_MACHINE_RISCV_FCMP:
  case AST_MACHINE_RISCV_CAST:
  case AST_MACHINE_RISCV_SELECT:
  case AST_MACHINE_RISCV_INDEX:
  case AST_MACHINE_RISCV_MEMBER:
  case AST_MACHINE_RISCV_ATOMIC:
  case AST_MACHINE_RISCV_INTRINSIC:
  case AST_MACHINE_RISCV_BIND:
  case AST_MACHINE_RISCV_NATIVE:
  case AST_MACHINE_RISCV_NATIVE_RESULT:
  case AST_MACHINE_RISCV_COND_BR:
    return 0;
  case AST_MACHINE_RISCV_RET:
  case AST_MACHINE_RISCV_EBREAK:
  case AST_MACHINE_RISCV_FENCE:
    return !n && !d;
  case AST_MACHINE_RISCV_J:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_RISCV_BNE:
    return n == 3u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_BLOCK &&
           o[0].bank == AST_MACHINE_BANK_GPR &&
           o[1].bank == AST_MACHINE_BANK_GPR;
  case AST_MACHINE_RISCV_CMP_BR:
    return n == 5u && !d && o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           (o[0].immediate_bits == 0u || o[0].immediate_bits == 1u ||
            o[0].immediate_bits == 4u || o[0].immediate_bits == 5u ||
            o[0].immediate_bits == 6u || o[0].immediate_bits == 7u) &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].bank == AST_MACHINE_BANK_GPR &&
           o[2].bank == AST_MACHINE_BANK_GPR &&
           o[1].bit_width == o[2].bit_width &&
           o[3].kind == AST_PHYSICAL_OPERAND_BLOCK &&
           o[4].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_RISCV_CALL:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_SYMBOL &&
           o[0].symbol.text && o[0].symbol.span.length;
  case AST_MACHINE_RISCV_CALL_INDIRECT:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u;
  case AST_MACHINE_RISCV_LI:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[0].bank == AST_MACHINE_BANK_GPR;
  case AST_MACHINE_RISCV_LA:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_SYMBOL &&
           o[1].symbol.text && o[1].symbol.span.length &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u;
  case AST_MACHINE_RISCV_MV:
  case AST_MACHINE_RISCV_NEG:
  case AST_MACHINE_RISCV_NOT:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR &&
           o[1].bank == AST_MACHINE_BANK_GPR;
  case AST_MACHINE_RISCV_FMV_S:
  case AST_MACHINE_RISCV_FMV_D:
  case AST_MACHINE_RISCV_FNEG_S:
  case AST_MACHINE_RISCV_FNEG_D: {
    uint32_t bits = i->opcode == AST_MACHINE_RISCV_FMV_S ||
                            i->opcode == AST_MACHINE_RISCV_FNEG_S
                        ? 32u
                        : 64u;
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_FP &&
           o[1].bank == AST_MACHINE_BANK_FP && o[0].bit_width == bits &&
           o[1].bit_width == bits;
  }
  case AST_MACHINE_RISCV_FMV_W_X:
  case AST_MACHINE_RISCV_FMV_D_X:
  case AST_MACHINE_RISCV_FMV_X_W:
  case AST_MACHINE_RISCV_FMV_X_D: {
    int to_fp = i->opcode == AST_MACHINE_RISCV_FMV_W_X ||
                i->opcode == AST_MACHINE_RISCV_FMV_D_X;
    uint32_t bits = i->opcode == AST_MACHINE_RISCV_FMV_W_X ||
                            i->opcode == AST_MACHINE_RISCV_FMV_X_W
                        ? 32u
                        : 64u;
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == (to_fp ? AST_MACHINE_BANK_FP : AST_MACHINE_BANK_GPR) &&
           o[1].bank == (to_fp ? AST_MACHINE_BANK_GPR : AST_MACHINE_BANK_FP) &&
           o[to_fp ? 0u : 1u].bit_width == bits &&
           o[to_fp ? 1u : 0u].bit_width >= bits;
  }
  default:
    if (i->opcode >= AST_MACHINE_RISCV_FCVT_S_W &&
        i->opcode <= AST_MACHINE_RISCV_FCVT_D_S) {
      if (n != 3u || d != 1u || o[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
          o[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
          o[2].immediate_bits > 1u)
        return 0;
      if (i->opcode <= AST_MACHINE_RISCV_FCVT_LU_D) {
        uint32_t position =
            (uint32_t)i->opcode - (uint32_t)AST_MACHINE_RISCV_FCVT_S_W;
        uint32_t bits = position < 8u ? 32u : 64u;
        if (!(position & 1u))
          return o[0].bank == AST_MACHINE_BANK_FP && o[0].bit_width == bits &&
                 o[1].bank == AST_MACHINE_BANK_GPR && o[1].bit_width == 64u &&
                 !o[2].immediate_bits;
        return o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u &&
               o[1].bank == AST_MACHINE_BANK_FP && o[1].bit_width == bits &&
               o[2].immediate_bits == 1u;
      }
      uint32_t bits = i->opcode == AST_MACHINE_RISCV_FCVT_S_D ? 32u : 64u;
      return o[0].bank == AST_MACHINE_BANK_FP &&
             o[1].bank == AST_MACHINE_BANK_FP && o[0].bit_width == bits &&
             o[1].bit_width == 96u - bits && !o[2].immediate_bits;
    }
    if (i->opcode >= AST_MACHINE_RISCV_FADD_S &&
        i->opcode <= AST_MACHINE_RISCV_FLE_D) {
      uint32_t bits =
          ((uint32_t)i->opcode - (uint32_t)AST_MACHINE_RISCV_FADD_S) % 2u ? 64u
                                                                          : 32u;
      int compare = i->opcode >= AST_MACHINE_RISCV_FEQ_S;
      return n == 3u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
             o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
             o[0].bank ==
                 (compare ? AST_MACHINE_BANK_GPR : AST_MACHINE_BANK_FP) &&
             o[1].bank == AST_MACHINE_BANK_FP &&
             o[2].bank == AST_MACHINE_BANK_FP && o[1].bit_width == bits &&
             o[2].bit_width == bits && (compare || o[0].bit_width == bits);
    }
    return 0;
  }
}
static int saved_transfer(const AstPhysicalInstructionInfo *i, uint32_t unit,
                          const AstFrameObjectInfo *obj, int restore) {
  if (i->operand_count != 2u ||
      !(restore ? rv_load(i->opcode) : rv_store(i->opcode)))
    return 0;
  const AstPhysicalOperand *reg = &i->operands[0], *mem = &i->operands[1];
  return reg->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         reg->register_unit == unit && reg->bit_width == obj->size * 8u &&
         mem->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         mem->register_unit == RV_SP && mem->displacement == obj->offset &&
         mem->bit_width == obj->size * 8u;
}
static int writes_unit(const AstPhysicalInstructionInfo *i, uint32_t unit) {
  if ((i->opcode == AST_MACHINE_RISCV_CALL ||
       i->opcode == AST_MACHINE_RISCV_CALL_INDIRECT) &&
      unit == MACHINE_PHYSREG_FILE_GPR * 64u + 1u)
    return 1;
  for (uint32_t d = 0; d < i->result_count; ++d)
    if (i->operands[d].kind == AST_PHYSICAL_OPERAND_REGISTER &&
        i->operands[d].register_unit == unit)
      return 1;
  return 0;
}
static int stack_memory_overlap(const AstPhysicalOperand *a,
                                const AstPhysicalOperand *b) {
  return a->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         b->kind == AST_PHYSICAL_OPERAND_MEMORY && a->register_unit == RV_SP &&
         b->register_unit == RV_SP &&
         a->displacement <
             b->displacement + (int64_t)((b->bit_width + 7u) / 8u) &&
         b->displacement <
             a->displacement + (int64_t)((a->bit_width + 7u) / 8u);
}
/* A call's volatile live-through homes must have a dominating save before any
   argument shuffle and a matching final reload. Scratch saves alone cannot
   satisfy this: their slot may be overwritten before the call returns. */
static int call_preservation_valid(const AstRegAllocProgram *p, uint32_t f,
                                   const AllocationFunction *af,
                                   const PhysicalBlock *block, uint32_t index) {
  const MachineFunction *mf = &p->machine->functions[f];
  uint32_t source = block->instructions[index].source_instruction;
  if (source >= mf->info.instruction_count ||
      mf->instructions[source].info.opcode != block->instructions[index].opcode)
    return 0;
  uint32_t begin = index, end = index + 1u,
           slot = af->liveness.instruction_slots[source];
  while (begin && block->instructions[begin - 1u].source_instruction == source)
    --begin;
  while (end < block->count &&
         block->instructions[end].source_instruction == source)
    ++end;
  for (uint32_t v = 0; v < mf->info.vreg_count; ++v) {
    const AstRegAllocLocation *loc = regalloc_location_at(af, v, slot);
    MachinePhysRegView view;
    if (loc->kind != AST_REGALLOC_LOCATION_REGISTER ||
        !machine_interval_covers(&af->liveness.intervals[v], slot) ||
        !machine_interval_covers(&af->liveness.intervals[v], slot + 1u))
      continue;
    if (!machine_physreg_view(&p->target, loc->register_name, &view))
      return 0;
    if (machine_physreg_preserved_bits(p->abi, &view) >= loc->bit_width)
      continue;
    int matched = 0;
    for (uint32_t s = begin; s < index; ++s) {
      const AstPhysicalInstructionInfo *save = &block->instructions[s];
      if (rv_store(save->opcode) && save->operand_count == 2u &&
          save->operands[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
          save->operands[0].register_unit == loc->register_unit &&
          save->operands[0].bit_width == loc->bit_width &&
          save->operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
          save->operands[1].register_unit == RV_SP) {
        for (uint32_t r = index + 1u; r < end; ++r) {
          const AstPhysicalInstructionInfo *restore = &block->instructions[r];
          if (!rv_load(restore->opcode) || restore->operand_count != 2u ||
              restore->operands[0].register_unit != loc->register_unit ||
              restore->operands[0].bit_width != loc->bit_width ||
              !physical_same_location(&restore->operands[1],
                                      &save->operands[1]))
            continue;
          int intact = 1;
          for (uint32_t j = s + 1u; j < r; ++j)
            if (rv_store(block->instructions[j].opcode) &&
                stack_memory_overlap(&save->operands[1],
                                     &block->instructions[j].operands[1]))
              intact = 0;
          for (uint32_t j = r + 1u; j < end; ++j)
            if (writes_unit(&block->instructions[j], loc->register_unit))
              intact = 0;
          if (intact) {
            matched = 1;
            break;
          }
        }
      }
      if (matched || writes_unit(save, loc->register_unit))
        break;
    }
    if (!matched)
      return 0;
  }
  return 1;
}
static int verify_canonical(const AstRegAllocProgram *p, uint32_t f,
                            const AllocationFunction *af) {
  const MachineFunction *mf = &p->machine->functions[f];
  if (!af->blocks || !af->callee_save_objects || !af->block_count ||
      mf->info.entry_block >= af->block_count || af->frame.alignment != 16u ||
      (af->frame.stack_adjustment & 15u) ||
      af->frame.stack_adjustment > INT32_MAX || af->frame.uses_red_zone ||
      af->frame.needs_stack_probe ||
      af->frame.object_count != af->object_count ||
      af->frame.outgoing_size > af->frame.stack_adjustment ||
      (af->object_count && !af->objects) ||
      (p->abi != AST_ABI_RISCV_LP64 && p->abi != AST_ABI_RISCV_LP64F &&
       p->abi != AST_ABI_RISCV_LP64D) ||
      af->block_count < mf->info.block_count)
    return 0;
  for (uint32_t n = 0; n < af->object_count; ++n) {
    const AstFrameObjectInfo *a = &af->objects[n];
    if (a->id != n || a->kind > AST_FRAME_OBJECT_EMERGENCY || !a->size ||
        a->size > INT32_MAX || a->offset > INT32_MAX || !a->alignment ||
        a->alignment > 16u || (a->alignment & (a->alignment - 1u)) ||
        a->offset < (int64_t)af->frame.outgoing_size ||
        ((uint64_t)a->offset & (a->alignment - 1u)) ||
        (uint64_t)a->offset + a->size > af->frame.stack_adjustment)
      return 0;
    for (uint32_t j = 0; j < n; ++j) {
      const AstFrameObjectInfo *b = &af->objects[j];
      if (a->offset < b->offset + (int64_t)b->size &&
          b->offset < a->offset + (int64_t)a->size)
        return 0;
    }
  }
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    uint32_t object = af->callee_save_objects[unit];
    if (object == AST_MACHINE_INVALID_ID)
      continue;
    MachinePhysRegView view = {0};
    view.file = (MachinePhysRegFile)(unit / 64u);
    view.index = unit % 64u;
    uint32_t bits = unit == MACHINE_PHYSREG_FILE_GPR * 64u + 1u
                        ? 64u
                        : machine_physreg_preserved_bits(p->abi, &view);
    if (object >= af->object_count ||
        af->objects[object].kind != AST_FRAME_OBJECT_CALLEE_SAVE || !bits ||
        af->objects[object].size != bits / 8u)
      return 0;
  }
  uint32_t calls = 0;
  uint8_t saves[MACHINE_REGISTER_UNITS] = {0};
  for (uint32_t b = 0; b < af->block_count; ++b) {
    const PhysicalBlock *block = &af->blocks[b];
    if (!block->count || !block->instructions)
      return 0;
    int64_t delta =
        b == mf->info.entry_block ? 0 : -(int64_t)af->frame.stack_adjustment;
    for (uint32_t n = 0; n < block->count; ++n) {
      const AstPhysicalInstructionInfo *i = &block->instructions[n];
      const RvScalarForm *scalar = rv_scalar_form(i->opcode);
      if (scalar && (scalar->feature & ~p->target.features))
        return 0;
      if (i->opcode < AST_MACHINE_RISCV_UNDEF ||
          i->opcode > AST_MACHINE_RISCV_BEXTI ||
          i->operand_count > AST_PHYSICAL_MAX_OPERANDS ||
          i->result_count > i->operand_count)
        return 0;
      for (uint32_t j = 0; j < i->operand_count; ++j) {
        const AstPhysicalOperand *o = &i->operands[j];
        if (o->kind <= AST_PHYSICAL_OPERAND_INVALID ||
            o->kind > AST_PHYSICAL_OPERAND_SYMBOL ||
            !memchr(o->register_name, 0, sizeof(o->register_name)) ||
            o->register_name[0] == '@')
          return 0;
        if ((o->kind == AST_PHYSICAL_OPERAND_REGISTER ||
             o->kind == AST_PHYSICAL_OPERAND_MEMORY) &&
            !rv_register(p, o))
          return 0;
        if (o->kind == AST_PHYSICAL_OPERAND_BLOCK &&
            o->block >= af->block_count)
          return 0;
        if (j < i->result_count &&
            (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
             (o->register_unit == MACHINE_PHYSREG_FILE_GPR * 64u &&
              i->opcode != AST_MACHINE_RISCV_SYSTEM &&
              !rv_scalar_form(i->opcode) && !rv_memory_form(i->opcode))))
          return 0;
      }
      if (i->opcode == AST_MACHINE_RISCV_LRSC_ATOMIC &&
          !physical_atomic_source(p, f, i))
        return 0;
      if (!rv_instruction(i) &&
          !(i->opcode == AST_MACHINE_RISCV_LA && i->operand_count == 2u &&
            i->result_count == 1u &&
            i->operands[0].bank == AST_MACHINE_BANK_GPR &&
            i->operands[0].bit_width == 64u &&
            i->operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY))
        return 0;
      if (rv_store(i->opcode) || rv_load(i->opcode)) {
        for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
          uint32_t object = af->callee_save_objects[unit];
          if (object == AST_MACHINE_INVALID_ID)
            continue;
          const AstFrameObjectInfo *obj = &af->objects[object];
          AstPhysicalOperand slot = i->operands[1];
          slot.kind = AST_PHYSICAL_OPERAND_MEMORY;
          slot.register_unit = RV_SP;
          slot.displacement = obj->offset;
          slot.bit_width = (uint32_t)obj->size * 8u;
          if (rv_store(i->opcode) &&
              stack_memory_overlap(&slot, &i->operands[1]) &&
              (b != mf->info.entry_block ||
               delta != -(int64_t)af->frame.stack_adjustment ||
               !saved_transfer(i, unit, obj, 0)))
            return 0;
          if (saved_transfer(i, unit, obj, 0) && saves[unit]++)
            return 0;
          if (saved_transfer(i, unit, obj, 1) &&
              delta != -(int64_t)af->frame.stack_adjustment)
            return 0;
        }
      }
      if (i->result_count && i->operands[0].register_unit == RV_SP) {
        if (i->operands[0].bit_width != 64u ||
            i->operands[1].register_unit != RV_SP ||
            i->operands[1].bit_width != 64u)
          return 0;
        int64_t amount;
        if (i->opcode == AST_MACHINE_RISCV_ADDI)
          amount = (int64_t)i->operands[2].immediate_bits;
        else if (i->opcode == AST_MACHINE_RISCV_ADD && n &&
                 i->operands[2].register_unit ==
                     MACHINE_PHYSREG_FILE_GPR * 64u + 31u &&
                 i->operands[2].bit_width == 64u) {
          const AstPhysicalInstructionInfo *load = &block->instructions[n - 1u];
          if (load->opcode != AST_MACHINE_RISCV_LI ||
              load->source_instruction != i->source_instruction ||
              !physical_same_location(&load->operands[0], &i->operands[2]))
            return 0;
          amount = (int64_t)load->operands[1].immediate_bits;
          int entry_adjust = b == mf->info.entry_block && n == 1u &&
                             amount == -(int64_t)af->frame.stack_adjustment;
          int exit_adjust =
              n + 2u == block->count &&
              block->instructions[n + 1u].opcode == AST_MACHINE_RISCV_RET &&
              block->instructions[n + 1u].source_instruction ==
                  i->source_instruction &&
              amount == (int64_t)af->frame.stack_adjustment;
          if (!entry_adjust && !exit_adjust)
            return 0;
        } else
          return 0;
        delta += amount;
        if (delta > 0 || delta < -(int64_t)af->frame.stack_adjustment ||
            (delta & 15))
          return 0;
      }
      if (i->opcode == AST_MACHINE_RISCV_CALL ||
          i->opcode == AST_MACHINE_RISCV_CALL_INDIRECT) {
        if (delta != -(int64_t)af->frame.stack_adjustment ||
            !af->frame.has_calls ||
            !call_preservation_valid(p, f, af, block, n))
          return 0;
        ++calls;
      }
      if (i->opcode == AST_MACHINE_RISCV_BNE &&
          (n + 2u != block->count ||
           block->instructions[n + 1u].opcode != AST_MACHINE_RISCV_J))
        return 0;
      if ((i->opcode == AST_MACHINE_RISCV_RET ||
           i->opcode == AST_MACHINE_RISCV_J ||
           i->opcode == AST_MACHINE_RISCV_EBREAK ||
           i->opcode == AST_MACHINE_RISCV_CMP_BR) &&
          n + 1u != block->count)
        return 0;
      if (i->opcode == AST_MACHINE_RISCV_RET) {
        if (delta)
          return 0;
        for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
          uint32_t object = af->callee_save_objects[unit];
          if (object == AST_MACHINE_INVALID_ID)
            continue;
          int found = 0;
          for (uint32_t j = 0; j < n; ++j)
            if (writes_unit(&block->instructions[j], unit))
              found = saved_transfer(&block->instructions[j], unit,
                                     &af->objects[object], 1);
          if (!found)
            return 0;
        }
      }
    }
    AstMachineOpcode last = block->instructions[block->count - 1u].opcode;
    if (last != AST_MACHINE_RISCV_RET && last != AST_MACHINE_RISCV_J &&
        last != AST_MACHINE_RISCV_EBREAK && last != AST_MACHINE_RISCV_CMP_BR)
      return 0;
    if (last != AST_MACHINE_RISCV_RET &&
        delta != -(int64_t)af->frame.stack_adjustment)
      return 0;
  }
  uint32_t expected = 0;
  for (uint32_t n = 0; n < mf->info.instruction_count; ++n)
    if (mf->instructions[n].info.opcode == AST_MACHINE_RISCV_CALL ||
        mf->instructions[n].info.opcode == AST_MACHINE_RISCV_CALL_INDIRECT)
      ++expected;
  if (calls != expected || !!calls != !!af->frame.has_calls)
    return 0;
  if (calls && af->callee_save_objects[MACHINE_PHYSREG_FILE_GPR * 64u + 1u] ==
                   AST_MACHINE_INVALID_ID)
    return 0;
  const PhysicalBlock *entry = &af->blocks[mf->info.entry_block];
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    uint32_t object = af->callee_save_objects[unit];
    if (object == AST_MACHINE_INVALID_ID)
      continue;
    int found = 0;
    for (uint32_t n = 0; n < entry->count; ++n) {
      if (saved_transfer(&entry->instructions[n], unit, &af->objects[object],
                         0)) {
        found = 1;
        break;
      }
      if (writes_unit(&entry->instructions[n], unit))
        return 0;
    }
    if (!found)
      return 0;
  }
  for (uint32_t v = 0; v < mf->info.vreg_count; ++v)
    for (uint32_t pi = 0; pi < regalloc_location_piece_count(af, v); ++pi) {
      const AstRegAllocLocation *loc = regalloc_location_piece(af, v, pi);
      MachinePhysRegView view;
      if (loc->kind == AST_REGALLOC_LOCATION_REGISTER &&
          machine_physreg_view(&p->target, loc->register_name, &view) &&
          machine_physreg_preserved_bits(p->abi, &view) &&
          af->callee_save_objects[view.alias_unit] == AST_MACHINE_INVALID_ID)
        return 0;
    }
  return 1;
}

static int stack_adjust(const AstPhysicalInstructionInfo *i, int64_t amount) {
  return i->opcode == AST_MACHINE_RISCV_ADDI && i->operand_count == 3u &&
         i->result_count == 1u &&
         i->operands[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
         i->operands[0].register_unit == RV_SP &&
         i->operands[0].bit_width == 64u &&
         physical_same_location(&i->operands[0], &i->operands[1]) &&
         i->operands[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
         (int64_t)i->operands[2].immediate_bits == amount;
}

/* Check the actual encodable stream, then remove only a proven, balanced
   scratch-register borrowing sequence for frame and ABI preservation checks. */
int physical_verify_riscv64(const AstRegAllocProgram *p, uint32_t f) {
  const AllocationFunction *original = &p->functions[f];
  if (!original->blocks || !original->block_count)
    return 0;
  PhysicalBlock *blocks =
      (PhysicalBlock *)calloc(original->block_count, sizeof(*blocks));
  if (!blocks) {
    return 0;
  }
  AllocationFunction canonical_function = *original;
  canonical_function.blocks = blocks;
  int ok = 0;
  for (uint32_t b = 0; b < original->block_count; ++b) {
    const PhysicalBlock *source = &original->blocks[b];
    if (!source->instructions || !source->count)
      goto done;
    for (uint32_t n = 0; n < source->count; ++n) {
      const AstPhysicalInstructionInfo *i = &source->instructions[n];
      if (i->opcode < AST_MACHINE_RISCV_UNDEF ||
          i->opcode > AST_MACHINE_RISCV_BEXTI ||
          i->operand_count > AST_PHYSICAL_MAX_OPERANDS ||
          i->result_count > i->operand_count || !rv_instruction(i))
        goto done;
      const RvScalarForm *scalar = rv_scalar_form(i->opcode);
      if (scalar && (scalar->feature & ~p->target.features))
        goto done;
      for (uint32_t j = 0; j < i->operand_count; ++j) {
        const AstPhysicalOperand *o = &i->operands[j];
        if (o->kind <= AST_PHYSICAL_OPERAND_INVALID ||
            o->kind > AST_PHYSICAL_OPERAND_SYMBOL ||
            !memchr(o->register_name, 0, sizeof(o->register_name)) ||
            o->register_name[0] == '@')
          goto done;
        if ((o->kind == AST_PHYSICAL_OPERAND_REGISTER ||
             o->kind == AST_PHYSICAL_OPERAND_MEMORY) &&
            !rv_register(p, o))
          goto done;
        if (o->kind == AST_PHYSICAL_OPERAND_MEMORY &&
            (o->displacement < -2048 || o->displacement > 2047))
          goto done;
        if (o->kind == AST_PHYSICAL_OPERAND_BLOCK &&
            o->block >= original->block_count)
          goto done;
        if (j < i->result_count &&
            (o->kind != AST_PHYSICAL_OPERAND_REGISTER ||
             (o->register_unit == MACHINE_PHYSREG_FILE_GPR * 64u &&
              i->opcode != AST_MACHINE_RISCV_SYSTEM &&
              !rv_scalar_form(i->opcode) && !rv_memory_form(i->opcode))))
          goto done;
      }
    }
    blocks[b].instructions = (AstPhysicalInstructionInfo *)calloc(
        source->count, sizeof(*source->instructions));
    if (!blocks[b].instructions)
      goto done;
    for (uint32_t n = 0; n < source->count; ++n) {
      const AstPhysicalInstructionInfo *i = &source->instructions[n];
      if (n + 6u < source->count && stack_adjust(i, -16)) {
        const AstPhysicalInstructionInfo *save = i + 1, *imm = i + 2,
                                         *add = i + 3, *access = i + 4,
                                         *restore = i + 5, *up = i + 6;
        if (save->opcode == AST_MACHINE_RISCV_SD &&
            imm->opcode == AST_MACHINE_RISCV_LI &&
            add->opcode == AST_MACHINE_RISCV_ADD &&
            (rv_load(access->opcode) || rv_store(access->opcode) ||
             access->opcode == AST_MACHINE_RISCV_MV) &&
            restore->opcode == AST_MACHINE_RISCV_LD && stack_adjust(up, 16) &&
            save->operands[0].bank == AST_MACHINE_BANK_GPR &&
            save->operands[0].bit_width == 64u &&
            save->operands[1].register_unit == RV_SP &&
            !save->operands[1].displacement &&
            save->operands[1].bit_width == 64u &&
            physical_same_location(&save->operands[0], &restore->operands[0]) &&
            physical_same_location(&save->operands[1], &restore->operands[1]) &&
            physical_same_location(&save->operands[0], &imm->operands[0]) &&
            physical_same_location(&save->operands[0], &add->operands[0]) &&
            physical_same_location(&save->operands[0], &add->operands[2]) &&
            add->operands[1].bit_width == 64u &&
            add->operands[1].register_unit != save->operands[0].register_unit &&
            access->operands[1].register_unit ==
                save->operands[0].register_unit &&
            !access->operands[1].displacement &&
            access->operands[0].register_unit !=
                save->operands[0].register_unit) {
          for (uint32_t step = 1; step <= 6u; ++step)
            if ((i + step)->source_instruction != i->source_instruction)
              goto done;
          int64_t offset = (int64_t)imm->operands[1].immediate_bits;
          if (offset < INT32_MIN || offset > INT32_MAX)
            goto done;
          if (add->operands[1].register_unit == RV_SP)
            offset -= 16;
          AstPhysicalInstructionInfo canonical = *access;
          canonical.operands[1] = add->operands[1];
          canonical.operands[1].kind = AST_PHYSICAL_OPERAND_MEMORY;
          canonical.operands[1].bit_width = access->operands[1].bit_width;
          canonical.operands[1].displacement = offset;
          if (access->opcode == AST_MACHINE_RISCV_MV)
            canonical.opcode = AST_MACHINE_RISCV_LA;
          blocks[b].instructions[blocks[b].count++] = canonical;
          n += 6u;
          continue;
        }
      }
      blocks[b].instructions[blocks[b].count++] = *i;
    }
  }
  ok = verify_canonical(p, f, &canonical_function);
done:
  for (uint32_t b = 0; b < original->block_count; ++b)
    free(blocks[b].instructions);
  free(blocks);
  return ok;
}
