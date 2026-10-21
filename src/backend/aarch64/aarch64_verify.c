#include "../../isa/aarch64_data_contract.h"
#include "../../isa/aarch64_scalable.h"
#include "../../isa/aarch64_scalar_contract.h"
#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "../postra/atomic_contract.h"
#include "aarch64_internal.h"
#include <limits.h>

#define A64_SP (MACHINE_PHYSREG_FILE_SPECIAL * 64u)
static int instruction_valid(const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  uint32_t n = i->operand_count, d = i->result_count;
  switch (i->opcode) {
  case AST_MACHINE_A64_SCALABLE:
    return a64_scalable_contract(i);
  case AST_MACHINE_A64_SCALAR:
    return a64_scalar_contract(i);
  case AST_MACHINE_A64_DATA:
    return a64_data_contract(i);
  case AST_MACHINE_A64_SYSTEM:
    return system_physical_contract(i) && !system_exception_return(i);
  case AST_MACHINE_A64_VECTOR:
    return vector_physical_contract(i);
  case AST_MACHINE_A64_EXCLUSIVE_ATOMIC:
    return atomic_physical_contract(i);
  case AST_MACHINE_A64_MOV:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           o[0].register_unit / 64u == o[1].register_unit / 64u;
  case AST_MACHINE_A64_MOV_IMM:
    return 0; /* Must become destination-only MOVZ/MOVK operations. */
  case AST_MACHINE_A64_MOVZ:
  case AST_MACHINE_A64_MOVK:
    return n == 3u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[1].immediate_bits <= 65535u &&
           o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[2].immediate_bits < o[0].bit_width && !(o[2].immediate_bits % 16u);
  case AST_MACHINE_A64_LOAD:
  case AST_MACHINE_A64_STORE: {
    int load = i->opcode == AST_MACHINE_A64_LOAD;
    if (n != 2u || d != (uint32_t)load ||
        o[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        o[1].kind != AST_PHYSICAL_OPERAND_MEMORY)
      return 0;
    uint32_t access = o[1].bit_width;
    if (o[0].bank == AST_MACHINE_BANK_GPR)
      return (access == 8u || access == 16u || access == 32u ||
              access == 64u) &&
             o[0].bit_width == (access <= 32u ? 32u : 64u);
    return o[0].bit_width == access;
  }
  case AST_MACHINE_A64_LEA:
    return n == 2u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == 64u &&
           (o[1].kind == AST_PHYSICAL_OPERAND_MEMORY ||
            o[1].kind == AST_PHYSICAL_OPERAND_SYMBOL);
  case AST_MACHINE_A64_NEG:
  case AST_MACHINE_A64_NOT:
  case AST_MACHINE_A64_FNEG:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width && o[0].bank == o[1].bank &&
           o[0].bank == (i->opcode == AST_MACHINE_A64_FNEG
                             ? AST_MACHINE_BANK_FP
                             : AST_MACHINE_BANK_GPR) &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u);
  case AST_MACHINE_A64_SBFM:
  case AST_MACHINE_A64_UBFM:
    return n == 4u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == o[1].bit_width &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           !o[2].immediate_bits &&
           o[3].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[3].immediate_bits < o[0].bit_width;
  case AST_MACHINE_A64_FMOV:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           ((o[0].bank == AST_MACHINE_BANK_GPR &&
             o[1].bank == AST_MACHINE_BANK_FP) ||
            (o[0].bank == AST_MACHINE_BANK_FP &&
             o[1].bank == AST_MACHINE_BANK_GPR));
  case AST_MACHINE_A64_SCVTF:
  case AST_MACHINE_A64_UCVTF:
  case AST_MACHINE_A64_FCVTZS:
  case AST_MACHINE_A64_FCVTZU:
  case AST_MACHINE_A64_FCVT: {
    int to_fp = i->opcode == AST_MACHINE_A64_SCVTF ||
                i->opcode == AST_MACHINE_A64_UCVTF;
    int both_fp = i->opcode == AST_MACHINE_A64_FCVT;
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           (o[1].bit_width == 32u || o[1].bit_width == 64u) &&
           (!both_fp || o[0].bit_width != o[1].bit_width) &&
           o[0].bank == (to_fp || both_fp ? AST_MACHINE_BANK_FP
                                          : AST_MACHINE_BANK_GPR) &&
           o[1].bank == (to_fp ? AST_MACHINE_BANK_GPR : AST_MACHINE_BANK_FP);
  }
  case AST_MACHINE_A64_CMP:
  case AST_MACHINE_A64_FCMP:
    return n == 2u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           o[0].bank == o[1].bank &&
           o[0].bank == (i->opcode == AST_MACHINE_A64_FCMP
                             ? AST_MACHINE_BANK_FP
                             : AST_MACHINE_BANK_GPR);
  case AST_MACHINE_A64_CSET:
    return n == 2u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == 32u &&
           o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[1].immediate_bits < 14u;
  case AST_MACHINE_A64_CSEL:
  case AST_MACHINE_A64_FCSEL:
    return n == 4u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           o[0].bit_width == o[2].bit_width && o[0].bank == o[1].bank &&
           o[0].bank == o[2].bank &&
           o[3].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[3].immediate_bits < 14u &&
           o[0].bank == (i->opcode == AST_MACHINE_A64_FCSEL
                             ? AST_MACHINE_BANK_FP
                             : AST_MACHINE_BANK_GPR) &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u);
  case AST_MACHINE_A64_ADD:
  case AST_MACHINE_A64_SUB:
    if (n == 3u && d == 1u && o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
      if (o[0].register_unit == A64_SP)
        return o[1].register_unit == A64_SP && o[0].bit_width == 64u &&
               o[1].bit_width == 64u;
      return o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
             o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
             o[0].bit_width == o[1].bit_width &&
             (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
             o[0].bank == AST_MACHINE_BANK_GPR &&
             o[1].bank == AST_MACHINE_BANK_GPR;
    }
    /* fall through */
  case AST_MACHINE_A64_MUL:
  case AST_MACHINE_A64_AND:
  case AST_MACHINE_A64_OR:
  case AST_MACHINE_A64_XOR:
  case AST_MACHINE_A64_SHL:
  case AST_MACHINE_A64_ASHR:
  case AST_MACHINE_A64_LSHR:
  case AST_MACHINE_A64_SDIV:
  case AST_MACHINE_A64_UDIV:
    if (n != 3u || d != 1u || o[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
        o[0].bit_width != o[1].bit_width || o[0].bank != AST_MACHINE_BANK_GPR ||
        o[1].bank != AST_MACHINE_BANK_GPR ||
        (o[0].bit_width != 32u && o[0].bit_width != 64u))
      return 0;
    if ((i->opcode == AST_MACHINE_A64_SHL ||
         i->opcode == AST_MACHINE_A64_ASHR ||
         i->opcode == AST_MACHINE_A64_LSHR) &&
        o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE)
      return o[2].immediate_bits < o[0].bit_width;
    return o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[2].bit_width &&
           o[2].bank == AST_MACHINE_BANK_GPR;
  case AST_MACHINE_A64_FADD:
  case AST_MACHINE_A64_FSUB:
  case AST_MACHINE_A64_FMUL:
  case AST_MACHINE_A64_FDIV:
    return n == 3u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           o[0].bit_width == o[2].bit_width &&
           o[0].bank == AST_MACHINE_BANK_FP &&
           o[1].bank == AST_MACHINE_BANK_FP &&
           o[2].bank == AST_MACHINE_BANK_FP &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u);
  case AST_MACHINE_A64_VEC_ADD:
  case AST_MACHINE_A64_VEC_SUB:
  case AST_MACHINE_A64_VEC_MUL:
  case AST_MACHINE_A64_VEC_AND:
  case AST_MACHINE_A64_VEC_OR:
  case AST_MACHINE_A64_VEC_XOR:
    return n == 3u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           o[0].bit_width == o[2].bit_width &&
           o[0].register_unit / 64u == o[1].register_unit / 64u &&
           o[0].register_unit / 64u == o[2].register_unit / 64u;
  case AST_MACHINE_A64_UMULH:
    return n == 3u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == 64u &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].bit_width == 64u && o[2].bit_width == 64u &&
           o[1].bank == AST_MACHINE_BANK_GPR &&
           o[2].bank == AST_MACHINE_BANK_GPR;
  case AST_MACHINE_A64_MSUB:
    return n == 4u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[3].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           o[0].bit_width == o[2].bit_width &&
           o[0].bit_width == o[3].bit_width &&
           o[1].bank == AST_MACHINE_BANK_GPR &&
           o[2].bank == AST_MACHINE_BANK_GPR &&
           o[3].bank == AST_MACHINE_BANK_GPR &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u);
  case AST_MACHINE_A64_ICMP_EQ:
  case AST_MACHINE_A64_ICMP_NE:
  case AST_MACHINE_A64_ICMP_LT:
  case AST_MACHINE_A64_ICMP_LE:
  case AST_MACHINE_A64_ICMP_GT:
  case AST_MACHINE_A64_ICMP_GE:
  case AST_MACHINE_A64_FCMP_EQ:
  case AST_MACHINE_A64_FCMP_NE:
  case AST_MACHINE_A64_FCMP_LT:
  case AST_MACHINE_A64_FCMP_LE:
  case AST_MACHINE_A64_FCMP_GT:
  case AST_MACHINE_A64_FCMP_GE:
  case AST_MACHINE_A64_VEC_CMP:
    return i->opcode == AST_MACHINE_A64_VEC_CMP && n == 3u && d == 1u &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].bank == o[2].bank && o[1].bit_width == o[2].bit_width;
  case AST_MACHINE_A64_SELECT:
  case AST_MACHINE_A64_CAST:
    return 0;
  case AST_MACHINE_A64_CALL:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_SYMBOL &&
           o[0].symbol.text && o[0].symbol.span.length;
  case AST_MACHINE_A64_CALL_INDIRECT:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u;
  case AST_MACHINE_A64_BR:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_A64_CMP_BR:
    return n == 5u && !d && o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[0].immediate_bits < 14u &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].bit_width == o[2].bit_width &&
           (o[1].bit_width == 32u || o[1].bit_width == 64u) &&
           o[1].bank == o[2].bank &&
           (o[1].bank == AST_MACHINE_BANK_GPR ||
            o[1].bank == AST_MACHINE_BANK_FP) &&
           o[3].kind == AST_PHYSICAL_OPERAND_BLOCK &&
           o[4].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_A64_CMP_ZERO:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR;
  case AST_MACHINE_A64_COND_BR:
    return n == 2u && !d && o[0].kind == AST_PHYSICAL_OPERAND_BLOCK &&
           o[1].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_A64_RET:
  case AST_MACHINE_A64_TRAP:
  case AST_MACHINE_A64_FENCE:
    return !n && !d;
  default:
    return 0;
  }
}

static int register_valid(const AstRegAllocProgram *p,
                          const AstPhysicalOperand *o) {
  MachinePhysRegView v;
  if (!memchr(o->register_name, 0, sizeof(o->register_name)) ||
      o->register_name[0] == '@' ||
      !machine_physreg_view(&p->target, o->register_name, &v) ||
      v.alias_unit != o->register_unit)
    return 0;
  if (o->kind == AST_PHYSICAL_OPERAND_MEMORY)
    return (v.file == MACHINE_PHYSREG_FILE_GPR ||
            (v.file == MACHINE_PHYSREG_FILE_SPECIAL &&
             strcmp(o->register_name, "sp") == 0)) &&
           v.width == 64u;
  if (v.alias_unit == 321u)
    return a64_scalar_register(o, o->bit_width) &&
           (o->bit_width == 32u || o->bit_width == 64u);
  if (v.alias_unit == 320u && o->bit_width == 32u)
    return a64_data_sp(o, 32u);
  if (v.alias_unit >= MACHINE_REGISTER_UNITS ||
      (v.special && strcmp(o->register_name, "sp") != 0))
    return 0;
  if (v.file == MACHINE_PHYSREG_FILE_GPR &&
      v.width != (o->bit_width <= 32u ? 32u : 64u))
    return 0;
  return v.width >= o->bit_width &&
         ((v.file == MACHINE_PHYSREG_FILE_SPECIAL &&
           strcmp(o->register_name, "sp") == 0) ||
          machine_register_compatible(&p->target, o->register_name, o->bank));
}
static int is_sp(const AstPhysicalOperand *o) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->register_unit == MACHINE_PHYSREG_FILE_SPECIAL * 64u &&
         o->bit_width == 64u;
}
static int saved_transfer(const AstPhysicalInstructionInfo *i, uint32_t unit,
                          const AstFrameObjectInfo *obj, int restore) {
  if (i->operand_count != 2u ||
      i->opcode != (restore ? AST_MACHINE_A64_LOAD : AST_MACHINE_A64_STORE))
    return 0;
  const AstPhysicalOperand *reg = &i->operands[0], *mem = &i->operands[1];
  return reg->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         reg->register_unit == unit && reg->bit_width == obj->size * 8u &&
         mem->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         mem->register_unit == MACHINE_PHYSREG_FILE_SPECIAL * 64u &&
         mem->displacement == obj->offset && mem->bit_width == obj->size * 8u;
}
static int writes_unit(const AstPhysicalInstructionInfo *i, uint32_t unit) {
  if ((i->opcode == AST_MACHINE_A64_CALL ||
       i->opcode == AST_MACHINE_A64_CALL_INDIRECT) &&
      unit == MACHINE_PHYSREG_FILE_GPR * 64u + 30u)
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
         b->kind == AST_PHYSICAL_OPERAND_MEMORY && a->register_unit == A64_SP &&
         b->register_unit == A64_SP &&
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
    /* STRB/STRH and their reloads name W registers while their memory
     * access preserves only the live 8/16-bit value. */
    uint32_t register_bits =
        loc->bank == AST_MACHINE_BANK_GPR && loc->bit_width < 32u
            ? 32u
            : loc->bit_width;
    int matched = 0;
    for (uint32_t s = begin; s < index; ++s) {
      const AstPhysicalInstructionInfo *save = &block->instructions[s];
      if (save->opcode == AST_MACHINE_A64_STORE && save->operand_count == 2u &&
          save->operands[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
          save->operands[0].register_unit == loc->register_unit &&
          save->operands[0].bit_width == register_bits &&
          save->operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
          save->operands[1].bit_width == loc->bit_width &&
          save->operands[1].register_unit == A64_SP) {
        for (uint32_t r = index + 1u; r < end; ++r) {
          const AstPhysicalInstructionInfo *restore = &block->instructions[r];
          if (restore->opcode != AST_MACHINE_A64_LOAD ||
              restore->operand_count != 2u ||
              restore->operands[0].register_unit != loc->register_unit ||
              restore->operands[0].bit_width != register_bits ||
              !physical_same_location(&restore->operands[1],
                                      &save->operands[1]))
            continue;
          int intact = 1;
          for (uint32_t j = s + 1u; j < r; ++j)
            if (block->instructions[j].opcode == AST_MACHINE_A64_STORE &&
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
  if (!af->blocks || !af->callee_save_objects ||
      mf->info.entry_block >= af->block_count ||
      (af->object_count && !af->objects) ||
      af->block_count < mf->info.block_count ||
      af->frame.object_count != af->object_count ||
      af->frame.stack_adjustment > INT32_MAX ||
      af->frame.outgoing_size > af->frame.stack_adjustment ||
      af->frame.alignment != 16u ||
      (af->frame.has_calls && (af->frame.stack_adjustment % 16u)) ||
      (af->frame.uses_red_zone &&
       (p->abi != AST_ABI_SYSV_AMD64 || af->frame.has_calls ||
        af->frame.stack_adjustment)) ||
      af->frame.needs_stack_probe != (p->abi == AST_ABI_WINDOWS_X64 &&
                                      af->frame.stack_adjustment >= 4096u))
    return 0;
  if (p->abi != AST_ABI_AAPCS64 || af->frame.uses_red_zone ||
      af->frame.needs_stack_probe || (af->frame.stack_adjustment & 15u))
    return 0;
  if (af->frame.has_calls &&
      af->callee_save_objects[MACHINE_PHYSREG_FILE_GPR * 64u + 30u] ==
          AST_MACHINE_INVALID_ID)
    return 0;
  uint32_t bias = 0u;
  for (uint32_t o = 0; o < af->object_count; ++o) {
    const AstFrameObjectInfo *a = &af->objects[o];
    if (a->id != o || a->kind > AST_FRAME_OBJECT_EMERGENCY || !a->size ||
        a->size > INT32_MAX || a->offset < INT32_MIN || a->offset > INT32_MAX ||
        !a->alignment || (a->alignment & (a->alignment - 1u)) ||
        a->alignment > 16u ||
        (((uint64_t)a->offset + bias) & (a->alignment - 1u)))
      return 0;
    if (af->frame.uses_red_zone) {
      if (a->offset < -128 || a->offset + (int64_t)a->size > 0)
        return 0;
    } else if (a->offset < (int64_t)af->frame.outgoing_size ||
               (uint64_t)a->offset + a->size > af->frame.stack_adjustment)
      return 0;
    for (uint32_t j = 0; j < o; ++j) {
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
    uint32_t bits = unit == MACHINE_PHYSREG_FILE_GPR * 64u + 30u
                        ? 64u
                        : machine_physreg_preserved_bits(p->abi, &view);
    if (object >= af->object_count ||
        af->objects[object].kind != AST_FRAME_OBJECT_CALLEE_SAVE || !bits ||
        af->objects[object].size != bits / 8u)
      return 0;
  }
  uint32_t calls = 0u;
  uint8_t saves[MACHINE_REGISTER_UNITS] = {0};
  for (uint32_t b = 0; b < af->block_count; ++b) {
    const PhysicalBlock *block = &af->blocks[b];
    if (!block->count || !block->instructions)
      return 0;
    int64_t stack_delta =
        b == mf->info.entry_block ? 0 : -(int64_t)af->frame.stack_adjustment;
    uint64_t probed_adjustment = 0u;
    uint32_t flags_source = AST_MACHINE_INVALID_ID;
    int flags_available = 0;
    for (uint32_t i = 0; i < block->count; ++i) {
      const AstPhysicalInstructionInfo *inst = &block->instructions[i];
      if (inst->opcode < AST_MACHINE_A64_UNDEF ||
          inst->opcode > AST_MACHINE_A64_SCALABLE ||
          inst->operand_count > AST_PHYSICAL_MAX_OPERANDS ||
          inst->result_count > inst->operand_count)
        return 0;
      for (uint32_t o = 0; o < inst->operand_count; ++o) {
        const AstPhysicalOperand *op = &inst->operands[o];
        if (op->kind <= AST_PHYSICAL_OPERAND_INVALID ||
            op->kind > AST_PHYSICAL_OPERAND_SYMBOL ||
            !memchr(op->register_name, 0, sizeof(op->register_name)) ||
            op->register_name[0] == '@')
          return 0;
        if (inst->opcode != AST_MACHINE_A64_SCALABLE &&
            (op->kind == AST_PHYSICAL_OPERAND_REGISTER ||
             op->kind == AST_PHYSICAL_OPERAND_MEMORY) &&
            (!op->bit_width || !register_valid(p, op)))
          return 0;
        if (op->kind == AST_PHYSICAL_OPERAND_BLOCK &&
            op->block >= af->block_count)
          return 0;
        if (op->kind == AST_PHYSICAL_OPERAND_MEMORY &&
            (op->displacement < INT32_MIN || op->displacement > INT32_MAX))
          return 0;
        if (o < inst->result_count && op->kind != AST_PHYSICAL_OPERAND_REGISTER)
          return 0;
      }
      if (!instruction_valid(inst) ||
          (inst->opcode == AST_MACHINE_A64_EXCLUSIVE_ATOMIC &&
           !physical_atomic_source(p, f, inst)))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_SCALABLE &&
          !a64_scalable_features(&p->target, a64_scalable_form(inst)))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_CSET ||
          inst->opcode == AST_MACHINE_A64_CSEL ||
          inst->opcode == AST_MACHINE_A64_FCSEL ||
          inst->opcode == AST_MACHINE_A64_COND_BR)
        if (!flags_available || flags_source != inst->source_instruction)
          return 0;
      if (inst->opcode == AST_MACHINE_A64_CMP ||
          inst->opcode == AST_MACHINE_A64_FCMP ||
          inst->opcode == AST_MACHINE_A64_CMP_ZERO) {
        flags_available = 1;
        flags_source = inst->source_instruction;
      }
      if (inst->opcode == AST_MACHINE_A64_CALL ||
          inst->opcode == AST_MACHINE_A64_CALL_INDIRECT ||
          inst->opcode == AST_MACHINE_A64_EXCLUSIVE_ATOMIC)
        flags_available = 0;
      if (inst->opcode == AST_MACHINE_A64_STORE ||
          inst->opcode == AST_MACHINE_A64_LOAD) {
        for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
          uint32_t object = af->callee_save_objects[unit];
          if (object == AST_MACHINE_INVALID_ID)
            continue;
          const AstFrameObjectInfo *obj = &af->objects[object];
          AstPhysicalOperand slot = inst->operands[1];
          slot.kind = AST_PHYSICAL_OPERAND_MEMORY;
          slot.register_unit = A64_SP;
          slot.displacement = obj->offset;
          slot.bit_width = (uint32_t)obj->size * 8u;
          if (inst->opcode == AST_MACHINE_A64_STORE &&
              stack_memory_overlap(&slot, &inst->operands[1]) &&
              (b != mf->info.entry_block ||
               stack_delta != -(int64_t)af->frame.stack_adjustment ||
               !saved_transfer(inst, unit, obj, 0)))
            return 0;
          if (saved_transfer(inst, unit, obj, 0) && saves[unit]++)
            return 0;
          if (saved_transfer(inst, unit, obj, 1) &&
              stack_delta != -(int64_t)af->frame.stack_adjustment)
            return 0;
        }
      }
      if (inst->opcode == AST_MACHINE_A64_RET ||
          inst->opcode == AST_MACHINE_A64_TRAP ||
          inst->opcode == AST_MACHINE_A64_BR ||
          inst->opcode == AST_MACHINE_A64_COND_BR ||
          inst->opcode == AST_MACHINE_A64_CMP_BR)
        if (i + 1u != block->count)
          return 0;
      if (writes_unit(inst, A64_SP) && inst->opcode != AST_MACHINE_A64_ADD &&
          inst->opcode != AST_MACHINE_A64_SUB)
        return 0;
      if (inst->opcode == AST_MACHINE_A64_LOAD &&
          (inst->operand_count != 2u || inst->result_count != 1u ||
           inst->operands[1].kind != AST_PHYSICAL_OPERAND_MEMORY))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_STORE &&
          (inst->operand_count != 2u || inst->result_count ||
           inst->operands[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
           inst->operands[1].kind != AST_PHYSICAL_OPERAND_MEMORY))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_CMP_ZERO &&
          (inst->operand_count != 1u || inst->result_count ||
           inst->operands[0].kind != AST_PHYSICAL_OPERAND_REGISTER))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_COND_BR &&
          (inst->operand_count != 2u || inst->result_count ||
           inst->operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK ||
           inst->operands[1].kind != AST_PHYSICAL_OPERAND_BLOCK))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_CMP_BR &&
          (inst->operand_count != 5u || inst->result_count ||
           inst->operands[3].kind != AST_PHYSICAL_OPERAND_BLOCK ||
           inst->operands[4].kind != AST_PHYSICAL_OPERAND_BLOCK))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_BR &&
          (inst->operand_count != 1u || inst->result_count ||
           inst->operands[0].kind != AST_PHYSICAL_OPERAND_BLOCK))
        return 0;
      if ((inst->opcode == AST_MACHINE_A64_SDIV ||
           inst->opcode == AST_MACHINE_A64_UDIV ||
           inst->opcode == AST_MACHINE_A64_MSUB) &&
          (inst->result_count != 1u ||
           inst->operand_count !=
               (inst->opcode == AST_MACHINE_A64_MSUB ? 4u : 3u)))
        return 0;
      if (inst->opcode == AST_MACHINE_A64_SREM ||
          inst->opcode == AST_MACHINE_A64_UREM)
        return 0;
      if ((inst->opcode == AST_MACHINE_A64_ADD ||
           inst->opcode == AST_MACHINE_A64_SUB) &&
          inst->operand_count == 3u && is_sp(&inst->operands[0])) {
        if (!is_sp(&inst->operands[1]) ||
            inst->operands[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
            (inst->operands[2].immediate_bits > 4095u &&
             ((inst->operands[2].immediate_bits & 4095u) ||
              inst->operands[2].immediate_bits > UINT64_C(0xfff000))) ||
            (inst->operands[2].immediate_bits & 15u))
          return 0;
        int64_t adjust = (int64_t)inst->operands[2].immediate_bits;
        stack_delta += inst->opcode == AST_MACHINE_A64_ADD ? adjust : -adjust;
        if (stack_delta > 0 ||
            stack_delta < -(int64_t)af->frame.stack_adjustment ||
            (inst->opcode == AST_MACHINE_A64_SUB && b != mf->info.entry_block))
          return 0;
        if (inst->opcode == AST_MACHINE_A64_SUB &&
            af->frame.needs_stack_probe) {
          if (adjust > 4096 || i + 1u >= block->count)
            return 0;
          const AstPhysicalInstructionInfo *probe =
              &block->instructions[i + 1u];
          if (probe->opcode != AST_MACHINE_A64_LOAD ||
              probe->operand_count != 2u ||
              probe->operands[1].kind != AST_PHYSICAL_OPERAND_MEMORY ||
              probe->operands[1].register_unit !=
                  MACHINE_PHYSREG_FILE_SPECIAL * 64u ||
              probe->operands[1].displacement)
            return 0;
          probed_adjustment += (uint64_t)adjust;
        }
      }
      if (inst->opcode == AST_MACHINE_A64_CALL ||
          inst->opcode == AST_MACHINE_A64_CALL_INDIRECT) {
        if (!af->frame.has_calls || ((uint64_t)stack_delta & 15u) ||
            stack_delta != -(int64_t)af->frame.stack_adjustment ||
            !call_preservation_valid(p, f, af, block, i))
          return 0;
        ++calls;
      }
      if (inst->opcode == AST_MACHINE_A64_RET) {
        if (stack_delta || i + 1u != block->count || inst->operand_count)
          return 0;
        for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
          uint32_t object = af->callee_save_objects[unit];
          if (object == AST_MACHINE_INVALID_ID)
            continue;
          int found = 0;
          for (uint32_t j = 0; j < i; ++j)
            if (writes_unit(&block->instructions[j], unit))
              found = saved_transfer(&block->instructions[j], unit,
                                     &af->objects[object], 1);
          if (!found)
            return 0;
        }
      }
    }
    AstMachineOpcode last = block->instructions[block->count - 1u].opcode;
    if (last != AST_MACHINE_A64_RET && last != AST_MACHINE_A64_TRAP &&
        last != AST_MACHINE_A64_BR && last != AST_MACHINE_A64_COND_BR &&
        last != AST_MACHINE_A64_CMP_BR)
      return 0;
    if (last != AST_MACHINE_A64_RET &&
        stack_delta != -(int64_t)af->frame.stack_adjustment)
      return 0;
    if (b == mf->info.entry_block && af->frame.needs_stack_probe &&
        probed_adjustment != af->frame.stack_adjustment)
      return 0;
  }
  uint32_t expected_calls = 0u;
  for (uint32_t i = 0; i < mf->info.instruction_count; ++i)
    if (mf->instructions[i].info.opcode == AST_MACHINE_A64_CALL ||
        mf->instructions[i].info.opcode == AST_MACHINE_A64_CALL_INDIRECT)
      ++expected_calls;
  if (calls != expected_calls || !!calls != !!af->frame.has_calls)
    return 0;
  const PhysicalBlock *entry = &af->blocks[mf->info.entry_block];
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    uint32_t object = af->callee_save_objects[unit];
    if (object == AST_MACHINE_INVALID_ID)
      continue;
    int found = 0;
    for (uint32_t i = 0; i < entry->count; ++i) {
      if (saved_transfer(&entry->instructions[i], unit, &af->objects[object],
                         0)) {
        found = 1;
        break;
      }
      if (writes_unit(&entry->instructions[i], unit))
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

static int stack_adjust(const AstPhysicalInstructionInfo *i,
                        AstMachineOpcode opcode) {
  return i->opcode == opcode && i->operand_count == 3u &&
         i->result_count == 1u && is_sp(&i->operands[0]) &&
         is_sp(&i->operands[1]) &&
         i->operands[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
         i->operands[2].immediate_bits == 16u;
}
/* Validate explicit address-scratch borrowing before recovering its equivalent
   frame access for the independent frame/call-preservation checks above. */
int physical_verify_aarch64(const AstRegAllocProgram *p, uint32_t f) {
  const AllocationFunction *original = &p->functions[f];
  if (!original->blocks || !original->block_count)
    return 0;
  PhysicalBlock *blocks =
      (PhysicalBlock *)calloc(original->block_count, sizeof(*blocks));
  if (!blocks) {
    return 0;
  }
  /* Other functions may still be allocating or releasing their state. The
     independent verifier only needs this function's canonical block view. */
  AllocationFunction canonical_function = *original;
  canonical_function.blocks = blocks;
  int ok = 0;
  for (uint32_t b = 0; b < original->block_count; ++b) {
    const PhysicalBlock *source = &original->blocks[b];
    if (!source->instructions || !source->count)
      goto done;
    blocks[b].instructions = (AstPhysicalInstructionInfo *)calloc(
        source->count, sizeof(*source->instructions));
    if (!blocks[b].instructions)
      goto done;
    for (uint32_t index = 0; index < source->count; ++index) {
      const AstPhysicalInstructionInfo *i = &source->instructions[index];
      if (i->operand_count > AST_PHYSICAL_MAX_OPERANDS ||
          i->result_count > i->operand_count || !instruction_valid(i))
        goto done;
      if (i->opcode == AST_MACHINE_A64_SCALABLE &&
          !a64_scalable_features(&p->target, a64_scalable_form(i)))
        goto done;
      for (uint32_t o = 0;
           o < i->operand_count && i->opcode != AST_MACHINE_A64_SCALABLE; ++o) {
        const AstPhysicalOperand *operand = &i->operands[o];
        if ((operand->kind == AST_PHYSICAL_OPERAND_REGISTER ||
             operand->kind == AST_PHYSICAL_OPERAND_MEMORY) &&
            (!operand->bit_width || !register_valid(p, operand)))
          goto done;
      }
      if (index + 5u < source->count && stack_adjust(i, AST_MACHINE_A64_SUB)) {
        const AstPhysicalInstructionInfo *save = i + 1, *lea = i + 2,
                                         *access = i + 3, *restore = i + 4,
                                         *adjust = i + 5;
        if (save->opcode == AST_MACHINE_A64_STORE &&
            lea->opcode == AST_MACHINE_A64_LEA &&
            (access->opcode == AST_MACHINE_A64_LOAD ||
             access->opcode == AST_MACHINE_A64_STORE) &&
            restore->opcode == AST_MACHINE_A64_LOAD &&
            stack_adjust(adjust, AST_MACHINE_A64_ADD) &&
            instruction_valid(save) && instruction_valid(lea) &&
            instruction_valid(access) && instruction_valid(restore) &&
            save->operands[0].bank == AST_MACHINE_BANK_GPR &&
            save->operands[0].bit_width == 64u &&
            save->operands[1].register_unit == A64_SP &&
            !save->operands[1].displacement &&
            save->operands[1].bit_width == 64u &&
            physical_same_location(&save->operands[0], &restore->operands[0]) &&
            physical_same_location(&save->operands[1], &restore->operands[1]) &&
            physical_same_location(&save->operands[0], &lea->operands[0]) &&
            lea->operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
            lea->operands[1].register_unit != lea->operands[0].register_unit &&
            access->operands[1].register_unit ==
                lea->operands[0].register_unit &&
            !access->operands[1].displacement &&
            access->operands[0].register_unit !=
                lea->operands[0].register_unit) {
          for (uint32_t step = 1; step <= 5u; ++step) {
            if ((i + step)->source_instruction != i->source_instruction)
              goto done;
            for (uint32_t o = 0; o < (i + step)->operand_count; ++o) {
              const AstPhysicalOperand *operand = &(i + step)->operands[o];
              if ((operand->kind == AST_PHYSICAL_OPERAND_REGISTER ||
                   operand->kind == AST_PHYSICAL_OPERAND_MEMORY) &&
                  !register_valid(p, operand))
                goto done;
            }
          }
          AstPhysicalInstructionInfo canonical = *access;
          canonical.operands[1] = lea->operands[1];
          canonical.operands[1].bit_width = access->operands[1].bit_width;
          if (canonical.operands[1].displacement < INT32_MIN ||
              canonical.operands[1].displacement > INT32_MAX)
            goto done;
          if (canonical.operands[1].register_unit == A64_SP)
            canonical.operands[1].displacement -= 16;
          blocks[b].instructions[blocks[b].count++] = canonical;
          index += 5u;
          continue;
        }
      }
      if (i->opcode == AST_MACHINE_A64_LOAD ||
          i->opcode == AST_MACHINE_A64_STORE) {
        const AstPhysicalOperand *memory = &i->operands[1];
        uint64_t bytes = (memory->bit_width + 7u) / 8u;
        int64_t offset = memory->displacement;
        if (!((offset >= -256 && offset <= 255) ||
              (offset >= 0 && bytes && (uint64_t)offset % bytes == 0u &&
               (uint64_t)offset / bytes <= 4095u)))
          goto done;
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
