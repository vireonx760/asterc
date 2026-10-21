#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "../postra/atomic_contract.h"
#include "physical_internal.h"
#include "x86_system_contract.h"
#include <limits.h>

#define X86_GPR(n) (MACHINE_PHYSREG_FILE_GPR * 64u + (n))
static int physical_x86_function_is_manual(const AstRegAllocProgram *p,
                                           uint32_t f);
static int physical_verify_manual_base(const AstRegAllocProgram *p, uint32_t f);
static int value_operand(const AstPhysicalOperand *o) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER ||
         o->kind == AST_PHYSICAL_OPERAND_MEMORY ||
         o->kind == AST_PHYSICAL_OPERAND_IMMEDIATE;
}
static int fixed_gpr(const AstPhysicalOperand *o, uint32_t index,
                     uint32_t bits) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->bank == AST_MACHINE_BANK_GPR &&
         o->register_unit == X86_GPR(index) && o->bit_width == bits;
}
/* This is a contract for the post-RA IR, independently of the rewrite pass.
   Multi-step selected pseudos must have disappeared by this stage. */
static int instruction_valid(const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  uint32_t n = i->operand_count, d = i->result_count;
  switch (i->opcode) {
  case AST_MACHINE_X86_VECTOR:
    return vector_physical_contract(i);
  case AST_MACHINE_X86_SYSTEM:
    return system_physical_contract(i);
  case AST_MACHINE_X86_LOCKED_ATOMIC:
    return atomic_physical_contract(i);
  case AST_MACHINE_X86_IN:
  case AST_MACHINE_X86_OUT:
  case AST_MACHINE_X86_LIDT:
  case AST_MACHINE_X86_LGDT:
    return x86_system_contract(i);
  case AST_MACHINE_X86_CLI:
  case AST_MACHINE_X86_STI:
  case AST_MACHINE_X86_HLT:
  case AST_MACHINE_X86_CLD:
  case AST_MACHINE_X86_NOP:
    return !n && !d;
  case AST_MACHINE_X86_RAW_BYTES:
    return !n && !d && i->raw_bytes && i->raw_byte_count;
  case AST_MACHINE_X86_POPCNT:
    return n == 2u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[1].bank == AST_MACHINE_BANK_GPR &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           (o[0].bit_width == 16u || o[0].bit_width == 32u ||
            o[0].bit_width == 64u);
  case AST_MACHINE_X86_BSF:
  case AST_MACHINE_X86_BSR:
    return n == 2u && d == 1u && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR &&
           o[1].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == o[1].bit_width &&
           (o[0].bit_width == 16u || o[0].bit_width == 32u ||
            o[0].bit_width == 64u);
  case AST_MACHINE_X86_BSWAP:
    return n == 2u && d == 1u && physical_same_location(&o[0], &o[1]) &&
           o[0].bank == AST_MACHINE_BANK_GPR &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u);
  case AST_MACHINE_X86_ROL:
  case AST_MACHINE_X86_ROR:
    return n == 3u && d == 1u && physical_same_location(&o[0], &o[1]) &&
           o[0].bank == AST_MACHINE_BANK_GPR && fixed_gpr(&o[2], 1u, 8u) &&
           (o[0].bit_width == 8u || o[0].bit_width == 16u ||
            o[0].bit_width == 32u || o[0].bit_width == 64u);
  case AST_MACHINE_X86_MOV:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bit_width == o[1].bit_width &&
           o[0].register_unit / 64u == o[1].register_unit / 64u;
  case AST_MACHINE_X86_MOV_IMM:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[0].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == o[1].bit_width;
  case AST_MACHINE_X86_LOAD:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
           o[0].bit_width == o[1].bit_width;
  case AST_MACHINE_X86_STORE:
    return n == 2u && !d &&
           (o[0].kind == AST_PHYSICAL_OPERAND_REGISTER ||
            o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) &&
           o[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
           o[0].bit_width == o[1].bit_width;
  case AST_MACHINE_X86_LEA:
    return n == 2u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == 64u &&
           (o[1].kind == AST_PHYSICAL_OPERAND_MEMORY ||
            o[1].kind == AST_PHYSICAL_OPERAND_SYMBOL);
  case AST_MACHINE_X86_SHL:
  case AST_MACHINE_X86_ASHR:
  case AST_MACHINE_X86_LSHR:
    return n == 3u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           physical_same_location(&o[0], &o[1]) &&
           (fixed_gpr(&o[2], 1u, 8u) ||
            (o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
             o[2].bit_width == o[0].bit_width));
  case AST_MACHINE_X86_UMUL_WIDE:
    return n == 4u && d == 2u &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           fixed_gpr(&o[0], 0u, o[0].bit_width) &&
           fixed_gpr(&o[1], 2u, o[0].bit_width) &&
           fixed_gpr(&o[2], 0u, o[0].bit_width) &&
           o[3].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[3].bank == AST_MACHINE_BANK_GPR &&
           o[3].bit_width == o[0].bit_width &&
           o[3].register_unit != X86_GPR(0u) &&
           o[3].register_unit != X86_GPR(2u);
  case AST_MACHINE_X86_SDIV:
  case AST_MACHINE_X86_UDIV: {
    uint32_t bits = o[0].bit_width;
    return n == 5u && d == 2u && (bits == 32u || bits == 64u) &&
           fixed_gpr(&o[0], 0u, bits) && fixed_gpr(&o[1], 2u, bits) &&
           fixed_gpr(&o[2], 0u, bits) && fixed_gpr(&o[3], 2u, bits) &&
           o[4].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[4].bank == AST_MACHINE_BANK_GPR && o[4].bit_width == bits;
  }
  case AST_MACHINE_X86_CDQ:
  case AST_MACHINE_X86_CQO: {
    uint32_t bits = i->opcode == AST_MACHINE_X86_CDQ ? 32u : 64u;
    return n == 2u && d == 1u && fixed_gpr(&o[0], 2u, bits) &&
           fixed_gpr(&o[1], 0u, bits);
  }
  case AST_MACHINE_X86_NEG:
  case AST_MACHINE_X86_NOT:
    return n == 2u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           physical_same_location(&o[0], &o[1]);
  case AST_MACHINE_X86_ADD:
  case AST_MACHINE_X86_SUB:
  case AST_MACHINE_X86_MUL:
  case AST_MACHINE_X86_AND:
  case AST_MACHINE_X86_OR:
  case AST_MACHINE_X86_XOR:
  case AST_MACHINE_X86_FADD:
  case AST_MACHINE_X86_FSUB:
  case AST_MACHINE_X86_FMUL:
  case AST_MACHINE_X86_FDIV:
  case AST_MACHINE_X86_VEC_ADD:
  case AST_MACHINE_X86_VEC_SUB:
  case AST_MACHINE_X86_VEC_MUL:
  case AST_MACHINE_X86_VEC_AND:
  case AST_MACHINE_X86_VEC_OR:
  case AST_MACHINE_X86_VEC_XOR:
    return n == 3u && d == 1u && physical_same_location(&o[0], &o[1]) &&
           (o[2].kind == AST_PHYSICAL_OPERAND_REGISTER ||
            o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) &&
           o[0].bit_width == o[2].bit_width &&
           (o[2].kind != AST_PHYSICAL_OPERAND_REGISTER ||
            o[0].register_unit / 64u == o[2].register_unit / 64u) &&
           ((i->opcode >= AST_MACHINE_X86_FADD &&
             i->opcode <= AST_MACHINE_X86_FDIV)
                ? o[0].bank == AST_MACHINE_BANK_FP
            : (i->opcode >= AST_MACHINE_X86_VEC_ADD &&
               i->opcode <= AST_MACHINE_X86_VEC_XOR)
                ? o[0].bank == AST_MACHINE_BANK_VECTOR
                : o[0].bank == AST_MACHINE_BANK_GPR);
  case AST_MACHINE_X86_ICMP_EQ:
  case AST_MACHINE_X86_ICMP_NE:
  case AST_MACHINE_X86_ICMP_LT:
  case AST_MACHINE_X86_ICMP_LE:
  case AST_MACHINE_X86_ICMP_GT:
  case AST_MACHINE_X86_ICMP_GE:
    return n == 3u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           (o[2].kind == AST_PHYSICAL_OPERAND_REGISTER ||
            o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) &&
           o[1].bit_width == o[2].bit_width &&
           (o[2].kind != AST_PHYSICAL_OPERAND_REGISTER ||
            o[1].bank == o[2].bank);
  case AST_MACHINE_X86_FCMP_EQ:
  case AST_MACHINE_X86_FCMP_NE:
  case AST_MACHINE_X86_FCMP_LT:
  case AST_MACHINE_X86_FCMP_LE:
  case AST_MACHINE_X86_FCMP_GT:
  case AST_MACHINE_X86_FCMP_GE:
  case AST_MACHINE_X86_VEC_CMP:
    return n == 3u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].bank == o[2].bank && o[1].bit_width == o[2].bit_width;
  case AST_MACHINE_X86_ICMP_BR:
    return n == 5u && !d && o[0].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           o[0].immediate_bits >= AST_MACHINE_X86_ICMP_EQ &&
           o[0].immediate_bits <= AST_MACHINE_X86_ICMP_GE &&
           o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           (o[2].kind == AST_PHYSICAL_OPERAND_REGISTER ||
            o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE) &&
           o[1].bit_width == o[2].bit_width &&
           (o[2].kind != AST_PHYSICAL_OPERAND_REGISTER ||
            o[1].bank == o[2].bank) &&
           o[3].kind == AST_PHYSICAL_OPERAND_BLOCK &&
           o[4].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_X86_CAST:
    return 0; /* Selected pseudo must be resolved. */
  case AST_MACHINE_X86_FMOV_BITS:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           o[0].bit_width == o[1].bit_width &&
           ((o[0].bank == AST_MACHINE_BANK_GPR &&
             o[1].bank == AST_MACHINE_BANK_FP) ||
            (o[0].bank == AST_MACHINE_BANK_FP &&
             o[1].bank == AST_MACHINE_BANK_GPR));
  case AST_MACHINE_X86_SINT_TO_FP:
  case AST_MACHINE_X86_FP_TO_SINT:
  case AST_MACHINE_X86_FP_CONVERT:
    return n == 2u && d == 1u && o[1].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           (o[1].bit_width == 32u || o[1].bit_width == 64u) &&
           o[0].bank == (i->opcode == AST_MACHINE_X86_FP_TO_SINT
                             ? AST_MACHINE_BANK_GPR
                             : AST_MACHINE_BANK_FP) &&
           o[1].bank == (i->opcode == AST_MACHINE_X86_SINT_TO_FP
                             ? AST_MACHINE_BANK_GPR
                             : AST_MACHINE_BANK_FP) &&
           (i->opcode != AST_MACHINE_X86_FP_CONVERT ||
            o[0].bit_width != o[1].bit_width);
  case AST_MACHINE_X86_UINT64_TO_FP:
    return n == 3u && d == 2u && o[0].bank == AST_MACHINE_BANK_FP &&
           (o[0].bit_width == 32u || o[0].bit_width == 64u) &&
           o[1].bank == AST_MACHINE_BANK_GPR && o[1].bit_width == 64u &&
           o[2].bank == AST_MACHINE_BANK_GPR && o[2].bit_width == 64u &&
           o[2].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[1].register_unit != o[2].register_unit;
  case AST_MACHINE_X86_FP_TO_UINT64:
    return n == 4u && d == 3u && o[0].bank == AST_MACHINE_BANK_GPR &&
           o[0].bit_width == 64u && o[1].bank == AST_MACHINE_BANK_GPR &&
           o[1].bit_width == 64u && o[0].register_unit != o[1].register_unit &&
           o[2].bank == AST_MACHINE_BANK_FP &&
           (o[2].bit_width == 32u || o[2].bit_width == 64u) &&
           o[3].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[3].bank == AST_MACHINE_BANK_FP &&
           o[2].bit_width == o[3].bit_width &&
           o[2].register_unit != o[3].register_unit;
  case AST_MACHINE_X86_MOVSX:
  case AST_MACHINE_X86_MOVZX:
    return n == 2u && d == 1u && o[0].bank == AST_MACHINE_BANK_GPR &&
           (o[1].kind == AST_PHYSICAL_OPERAND_REGISTER ||
            o[1].kind == AST_PHYSICAL_OPERAND_MEMORY) &&
           (o[1].kind != AST_PHYSICAL_OPERAND_REGISTER ||
            o[1].bank == AST_MACHINE_BANK_GPR) &&
           (o[1].bit_width == 8u || o[1].bit_width == 16u ||
            o[1].bit_width == 32u) &&
           (o[0].bit_width == 16u || o[0].bit_width == 32u ||
            o[0].bit_width == 64u) &&
           o[0].bit_width > o[1].bit_width;
  case AST_MACHINE_X86_SELECT:
    return n == 4u && d == 1u && value_operand(&o[1]) && value_operand(&o[2]) &&
           value_operand(&o[3]) && o[0].bit_width == o[2].bit_width &&
           o[0].bit_width == o[3].bit_width;
  case AST_MACHINE_X86_CALL:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_SYMBOL &&
           o[0].symbol.text && o[0].symbol.span.length;
  case AST_MACHINE_X86_CALL_INDIRECT:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u;
  case AST_MACHINE_X86_PUSH:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u;
  case AST_MACHINE_X86_POP:
    return n == 1u && d == 1u && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u;
  case AST_MACHINE_X86_JMP:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_SYMBOL &&
           o[0].symbol.text && o[0].symbol.span.length;
  case AST_MACHINE_X86_JMP_INDIRECT:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
           o[0].bank == AST_MACHINE_BANK_GPR && o[0].bit_width == 64u;
  case AST_MACHINE_X86_BR:
    return n == 1u && !d && o[0].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_X86_COND_BR:
    return n == 3u && !d && value_operand(&o[0]) &&
           o[1].kind == AST_PHYSICAL_OPERAND_BLOCK &&
           o[2].kind == AST_PHYSICAL_OPERAND_BLOCK;
  case AST_MACHINE_X86_RET:
  case AST_MACHINE_X86_TRAP:
  case AST_MACHINE_X86_FENCE:
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
  if (o->kind == AST_PHYSICAL_OPERAND_MEMORY) {
    if (v.file != MACHINE_PHYSREG_FILE_GPR || v.width != 64u)
      return 0;
    if (!o->scale)
      return !o->index_register_name[0] && !o->index_register_unit;
    if (o->scale != 1u && o->scale != 2u && o->scale != 4u && o->scale != 8u)
      return 0;
    MachinePhysRegView ix;
    return memchr(o->index_register_name, 0, sizeof(o->index_register_name)) &&
           machine_physreg_view(&p->target, o->index_register_name, &ix) &&
           ix.file == MACHINE_PHYSREG_FILE_GPR && ix.width == 64u &&
           !ix.special && ix.alias_unit == o->index_register_unit;
  }
  if (v.special || v.alias_unit >= MACHINE_REGISTER_UNITS)
    return 0;
  if (v.file == MACHINE_PHYSREG_FILE_GPR &&
      v.width != (o->bit_width < 8u ? 8u : o->bit_width))
    return 0;
  return v.width >= o->bit_width &&
         machine_register_compatible(&p->target, o->register_name, o->bank);
}
static int is_sp(const AstPhysicalOperand *o) {
  return o->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         o->register_unit == MACHINE_PHYSREG_FILE_GPR * 64u + 4u &&
         o->bit_width == 64u;
}
static int saved_transfer(const AstPhysicalInstructionInfo *i, uint32_t unit,
                          const AstFrameObjectInfo *obj, int restore) {
  if (i->operand_count != 2u ||
      i->opcode != (restore ? AST_MACHINE_X86_LOAD : AST_MACHINE_X86_STORE))
    return 0;
  const AstPhysicalOperand *reg = &i->operands[0], *mem = &i->operands[1];
  return reg->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         reg->register_unit == unit && reg->bit_width == obj->size * 8u &&
         mem->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         mem->register_unit == MACHINE_PHYSREG_FILE_GPR * 64u + 4u &&
         mem->displacement == obj->offset && mem->bit_width == obj->size * 8u;
}
static int writes_unit(const AstPhysicalInstructionInfo *i, uint32_t unit) {
  for (uint32_t d = 0; d < i->result_count; ++d)
    if (i->operands[d].kind == AST_PHYSICAL_OPERAND_REGISTER &&
        i->operands[d].register_unit == unit)
      return 1;
  return 0;
}
static int stack_memory_overlap(const AstPhysicalOperand *a,
                                const AstPhysicalOperand *b) {
  return a->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         b->kind == AST_PHYSICAL_OPERAND_MEMORY &&
         a->register_unit == X86_GPR(4u) && b->register_unit == X86_GPR(4u) &&
         a->displacement <
             b->displacement + (int64_t)((b->bit_width + 7u) / 8u) &&
         b->displacement <
             a->displacement + (int64_t)((a->bit_width + 7u) / 8u);
}
/* A call's volatile live-through homes must have a dominating save before any
   argument shuffle and a matching final reload. Scratch saves alone cannot
   satisfy this: their slot may be overwritten before the call returns. */
static int call_preservation_valid(const AstRegAllocProgram *p, uint32_t f,
                                   const PhysicalBlock *block, uint32_t index) {
  const AllocationFunction *af = &p->functions[f];
  const MachineFunction *mf = &p->machine->functions[f];
  if (physical_x86_function_is_manual(p, f))
    return physical_verify_manual_base(p, f);
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
      if (save->opcode == AST_MACHINE_X86_STORE && save->operand_count == 2u &&
          save->operands[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
          save->operands[0].register_unit == loc->register_unit &&
          save->operands[0].bit_width == loc->bit_width &&
          save->operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
          save->operands[1].register_unit == X86_GPR(4u)) {
        for (uint32_t r = index + 1u; r < end; ++r) {
          const AstPhysicalInstructionInfo *restore = &block->instructions[r];
          if (restore->opcode != AST_MACHINE_X86_LOAD ||
              restore->operand_count != 2u ||
              restore->operands[0].register_unit != loc->register_unit ||
              restore->operands[0].bit_width != loc->bit_width ||
              !physical_same_location(&restore->operands[1],
                                      &save->operands[1]))
            continue;
          int intact = 1;
          for (uint32_t j = s + 1u; j < r; ++j)
            if (block->instructions[j].opcode == AST_MACHINE_X86_STORE &&
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
static int stack_math(const AstPhysicalInstructionInfo *i,
                      AstMachineOpcode opcode, uint64_t amount) {
  return i->opcode == opcode && i->result_count == 1u &&
         i->operand_count == 3u && fixed_gpr(&i->operands[0], 4u, 64u) &&
         fixed_gpr(&i->operands[1], 4u, 64u) &&
         i->operands[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
         i->operands[2].bit_width == 64u &&
         i->operands[2].immediate_bits == amount;
}
static int frame_pointer_move(const AstPhysicalInstructionInfo *i, uint32_t dst,
                              uint32_t src) {
  return i->opcode == AST_MACHINE_X86_MOV && i->result_count == 1u &&
         i->operand_count == 2u && fixed_gpr(&i->operands[0], dst, 64u) &&
         fixed_gpr(&i->operands[1], src, 64u);
}
static int frame_pointer_save(const AstPhysicalInstructionInfo *i,
                              int restore) {
  return i->opcode ==
             (restore ? AST_MACHINE_X86_LOAD : AST_MACHINE_X86_STORE) &&
         i->result_count == (uint32_t)restore && i->operand_count == 2u &&
         fixed_gpr(&i->operands[0], 5u, 64u) &&
         i->operands[1].kind == AST_PHYSICAL_OPERAND_MEMORY &&
         i->operands[1].register_unit == X86_GPR(4u) &&
         !i->operands[1].displacement && i->operands[1].bit_width == 64u;
}
static int physical_x86_function_is_manual(const AstRegAllocProgram *p,
                                           uint32_t f) {
  const AstMirFunction *mir = ast_mir_function_at(p->machine->mir, f);
  AstMirFunctionInfo info;
  return mir && ast_mir_function_info(mir, &info) &&
         ((info.flags & AST_HIR_FUNCTION_NAKED) ||
          info.frame_policy == AST_HIR_FRAME_NAKED ||
          info.frame_policy == AST_HIR_FRAME_VALIDATE);
}
static int physical_verify_manual_base(const AstRegAllocProgram *p,
                                       uint32_t f) {
  const AllocationFunction *af = &p->functions[f];
  if (!af->blocks || !af->callee_save_objects || af->block_count != 1u ||
      af->object_count || af->frame.object_count ||
      af->frame.stack_adjustment || af->frame.alignment != 16u)
    return 0;
  const PhysicalBlock *block = &af->blocks[0];
  if (!block->count || !block->instructions)
    return 0;
  for (uint32_t i = 0; i < block->count; ++i) {
    const AstPhysicalInstructionInfo *inst = &block->instructions[i];
    if (inst->opcode < AST_MACHINE_X86_UNDEF ||
        inst->opcode > AST_MACHINE_X86_ICMP_BR ||
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
      if ((op->kind == AST_PHYSICAL_OPERAND_REGISTER ||
           op->kind == AST_PHYSICAL_OPERAND_MEMORY) &&
          (!op->bit_width || !register_valid(p, op)))
        return 0;
      if (op->kind == AST_PHYSICAL_OPERAND_MEMORY &&
          (op->displacement < INT32_MIN || op->displacement > INT32_MAX))
        return 0;
      if (o < inst->result_count && op->kind != AST_PHYSICAL_OPERAND_REGISTER)
        return 0;
    }
    if (!instruction_valid(inst))
      return 0;
    if (inst->opcode == AST_MACHINE_X86_POPCNT &&
        !(p->target.features & AST_TARGET_FEATURE_X86_POPCNT))
      return 0;
    if ((inst->opcode == AST_MACHINE_X86_RET ||
         inst->opcode == AST_MACHINE_X86_JMP ||
         inst->opcode == AST_MACHINE_X86_JMP_INDIRECT) &&
        i + 1u != block->count)
      return 0;
  }
  AstMachineOpcode last = block->instructions[block->count - 1u].opcode;
  return last == AST_MACHINE_X86_RET || last == AST_MACHINE_X86_JMP ||
         last == AST_MACHINE_X86_JMP_INDIRECT ||
         last == AST_MACHINE_X86_RAW_BYTES;
}

int physical_verify_function(const AstRegAllocProgram *p, uint32_t f) {
  const AllocationFunction *af = &p->functions[f];
  const MachineFunction *mf = &p->machine->functions[f];
  if (physical_x86_function_is_manual(p, f))
    return physical_verify_manual_base(p, f);
  if (!af->blocks || !af->callee_save_objects ||
      mf->info.entry_block >= af->block_count ||
      (af->object_count && !af->objects) ||
      af->block_count < mf->info.block_count ||
      af->frame.object_count != af->object_count ||
      af->frame.stack_adjustment > INT32_MAX ||
      af->frame.outgoing_size > af->frame.stack_adjustment ||
      af->frame.alignment < 16u ||
      (af->frame.alignment & (af->frame.alignment - 1u)) ||
      af->frame.alignment > INT32_MAX ||
      af->frame.alignment != frame_required_alignment(p, f) ||
      af->frame.realigns_stack != (af->frame.alignment > 16u) ||
      (af->frame.realigns_stack &&
       (p->abi != AST_ABI_SYSV_AMD64 || !af->frame.stack_adjustment ||
        af->frame.stack_adjustment % af->frame.alignment ||
        af->frame.uses_red_zone ||
        af->callee_save_objects[X86_GPR(5u)] != AST_MACHINE_INVALID_ID)) ||
      (!af->frame.realigns_stack && af->frame.has_calls &&
       ((af->frame.stack_adjustment + 8u) % 16u)) ||
      (af->frame.uses_red_zone &&
       (p->abi != AST_ABI_SYSV_AMD64 || af->frame.has_calls ||
        af->frame.stack_adjustment)) ||
      af->frame.needs_stack_probe != (p->abi == AST_ABI_WINDOWS_X64 &&
                                      af->frame.stack_adjustment >= 4096u))
    return 0;
  if (p->abi == AST_ABI_WINDOWS_X64 && af->frame.has_calls &&
      af->frame.outgoing_size < 32u)
    return 0;
  uint32_t bias = af->frame.stack_adjustment ? 0u : 8u;
  for (uint32_t o = 0; o < af->object_count; ++o) {
    const AstFrameObjectInfo *a = &af->objects[o];
    if (a->id != o || a->kind > AST_FRAME_OBJECT_EMERGENCY || !a->size ||
        a->size > INT32_MAX || a->offset < INT32_MIN || a->offset > INT32_MAX ||
        !a->alignment || (a->alignment & (a->alignment - 1u)) ||
        a->alignment > af->frame.alignment ||
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
    uint32_t bits = machine_physreg_preserved_bits(p->abi, &view);
    if (object >= af->object_count ||
        af->objects[object].kind != AST_FRAME_OBJECT_CALLEE_SAVE || !bits ||
        af->objects[object].size != bits / 8u)
      return 0;
  }
  uint32_t calls = 0u;
  for (uint32_t b = 0; b < af->block_count; ++b) {
    const PhysicalBlock *block = &af->blocks[b];
    if (!block->count || !block->instructions)
      return 0;
    int realign_entry = af->frame.realigns_stack && b == mf->info.entry_block;
    int realign_return =
        af->frame.realigns_stack &&
        block->instructions[block->count - 1u].opcode == AST_MACHINE_X86_RET;
    if (realign_entry &&
        (block->count < 5u ||
         !stack_math(&block->instructions[0], AST_MACHINE_X86_SUB, 8u) ||
         !frame_pointer_save(&block->instructions[1], 0) ||
         !frame_pointer_move(&block->instructions[2], 5u, 4u) ||
         !stack_math(&block->instructions[3], AST_MACHINE_X86_AND,
                     UINT64_C(0) - af->frame.alignment)))
      return 0;
    if (realign_return &&
        (block->count < 4u ||
         !frame_pointer_move(&block->instructions[block->count - 4u], 4u, 5u) ||
         !frame_pointer_save(&block->instructions[block->count - 3u], 1) ||
         !stack_math(&block->instructions[block->count - 2u],
                     AST_MACHINE_X86_ADD, 8u)))
      return 0;
    int64_t stack_delta =
        b == mf->info.entry_block ? 0 : -(int64_t)af->frame.stack_adjustment;
    uint64_t probed_adjustment = 0u;
    int prologue_open = b == mf->info.entry_block;
    for (uint32_t i = 0; i < block->count; ++i) {
      const AstPhysicalInstructionInfo *inst = &block->instructions[i];
      if (inst->opcode < AST_MACHINE_X86_UNDEF ||
          inst->opcode > AST_MACHINE_X86_ICMP_BR ||
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
        if ((op->kind == AST_PHYSICAL_OPERAND_REGISTER ||
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
          (inst->opcode == AST_MACHINE_X86_LOCKED_ATOMIC &&
           !physical_atomic_source(p, f, inst)))
        return 0;
      if (inst->opcode == AST_MACHINE_X86_POPCNT &&
          !(p->target.features & AST_TARGET_FEATURE_X86_POPCNT))
        return 0;
      if (realign_entry && i < 4u) {
        if (inst->source_instruction != AST_MACHINE_INVALID_ID)
          return 0;
        continue; /* Proven preamble establishes the aligned body base. */
      }
      if (realign_return && i >= block->count - 4u && i < block->count - 1u) {
        if (i == block->count - 4u) {
          if (stack_delta != -(int64_t)af->frame.stack_adjustment)
            return 0;
          stack_delta = 0; /* Exact MOV/LOAD/ADD restores the entry RSP/RBP. */
        }
        continue;
      }
      if (af->frame.realigns_stack) {
        if (writes_unit(inst, X86_GPR(5u)))
          return 0;
        for (uint32_t o = 0; o < inst->operand_count; ++o) {
          const AstPhysicalOperand *operand = &inst->operands[o];
          if ((operand->kind == AST_PHYSICAL_OPERAND_REGISTER ||
               operand->kind == AST_PHYSICAL_OPERAND_MEMORY) &&
              operand->register_unit == X86_GPR(5u) &&
              (operand->kind != AST_PHYSICAL_OPERAND_MEMORY ||
               operand->displacement < 16))
            return 0;
        }
      }
      int stack_sub =
          inst->opcode == AST_MACHINE_X86_SUB && is_sp(&inst->operands[0]);
      int stack_add =
          inst->opcode == AST_MACHINE_X86_ADD && is_sp(&inst->operands[0]);
      int page_probe =
          af->frame.needs_stack_probe && i &&
          inst->opcode == AST_MACHINE_X86_LOAD &&
          block->instructions[i - 1u].opcode == AST_MACHINE_X86_SUB &&
          is_sp(&block->instructions[i - 1u].operands[0]);
      if (stack_sub && !prologue_open)
        return 0;
      if (!stack_sub && !page_probe)
        prologue_open = 0;
      if (stack_add &&
          (i + 1u >= block->count ||
           block->instructions[i + 1u].opcode != AST_MACHINE_X86_RET))
        return 0;
      /* Every resolved frame/argument address is relative to the final
         body SP. A page probe alone may touch a partially adjusted SP. */
      if (!page_probe)
        for (uint32_t o = 0; o < inst->operand_count; ++o)
          if (inst->operands[o].kind == AST_PHYSICAL_OPERAND_MEMORY &&
              inst->operands[o].register_unit == X86_GPR(4u) &&
              stack_delta != -(int64_t)af->frame.stack_adjustment)
            return 0;
      if (inst->opcode == AST_MACHINE_X86_RET ||
          inst->opcode == AST_MACHINE_X86_TRAP ||
          inst->opcode == AST_MACHINE_X86_BR ||
          inst->opcode == AST_MACHINE_X86_COND_BR ||
          inst->opcode == AST_MACHINE_X86_ICMP_BR)
        if (i + 1u != block->count)
          return 0;
      if (writes_unit(inst, X86_GPR(4u)) &&
          inst->opcode != AST_MACHINE_X86_ADD &&
          inst->opcode != AST_MACHINE_X86_SUB)
        return 0;
      if ((inst->opcode == AST_MACHINE_X86_ADD ||
           inst->opcode == AST_MACHINE_X86_SUB) &&
          inst->operand_count == 3u && is_sp(&inst->operands[0])) {
        if (!is_sp(&inst->operands[1]) ||
            inst->operands[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
            inst->operands[2].immediate_bits > INT32_MAX)
          return 0;
        int64_t adjust = (int64_t)inst->operands[2].immediate_bits;
        stack_delta += inst->opcode == AST_MACHINE_X86_ADD ? adjust : -adjust;
        if (stack_delta > 0 ||
            stack_delta < -(int64_t)af->frame.stack_adjustment ||
            (inst->opcode == AST_MACHINE_X86_SUB && b != mf->info.entry_block))
          return 0;
        if (inst->opcode == AST_MACHINE_X86_SUB &&
            af->frame.needs_stack_probe) {
          if (adjust > 4096 || i + 1u >= block->count)
            return 0;
          const AstPhysicalInstructionInfo *probe =
              &block->instructions[i + 1u];
          if (probe->opcode != AST_MACHINE_X86_LOAD ||
              probe->operand_count != 2u ||
              probe->operands[1].kind != AST_PHYSICAL_OPERAND_MEMORY ||
              probe->operands[1].register_unit !=
                  MACHINE_PHYSREG_FILE_GPR * 64u + 4u ||
              probe->operands[1].displacement)
            return 0;
          probed_adjustment += (uint64_t)adjust;
        }
      }
      if (inst->opcode == AST_MACHINE_X86_CALL ||
          inst->opcode == AST_MACHINE_X86_CALL_INDIRECT) {
        if (!af->frame.has_calls ||
            ((uint64_t)(stack_delta + (af->frame.realigns_stack ? 0 : 8)) &
             (af->frame.alignment - 1u)) ||
            stack_delta != -(int64_t)af->frame.stack_adjustment ||
            !call_preservation_valid(p, f, block, i))
          return 0;
        ++calls;
      }
      if (inst->opcode == AST_MACHINE_X86_RET) {
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
    if (last != AST_MACHINE_X86_RET && last != AST_MACHINE_X86_TRAP &&
        last != AST_MACHINE_X86_BR && last != AST_MACHINE_X86_COND_BR &&
        last != AST_MACHINE_X86_ICMP_BR)
      return 0;
    if (last != AST_MACHINE_X86_RET &&
        stack_delta != -(int64_t)af->frame.stack_adjustment)
      return 0;
    if (b == mf->info.entry_block && af->frame.needs_stack_probe &&
        probed_adjustment != af->frame.stack_adjustment)
      return 0;
  }
  uint32_t expected_calls = 0u;
  for (uint32_t i = 0; i < mf->info.instruction_count; ++i)
    if (mf->instructions[i].info.opcode == AST_MACHINE_X86_CALL ||
        mf->instructions[i].info.opcode == AST_MACHINE_X86_CALL_INDIRECT)
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
    /* The saved incoming register value is immutable until restoration.
       A matching save/load pair cannot hide an intervening slot clobber. */
    AstPhysicalOperand saved_memory = {0};
    saved_memory.kind = AST_PHYSICAL_OPERAND_MEMORY;
    saved_memory.register_unit = X86_GPR(4u);
    saved_memory.bit_width = (uint32_t)af->objects[object].size * 8u;
    saved_memory.displacement = af->objects[object].offset;
    uint32_t saves = 0u;
    for (uint32_t b = 0; b < af->block_count; ++b)
      for (uint32_t i = 0; i < af->blocks[b].count; ++i) {
        const AstPhysicalInstructionInfo *inst = &af->blocks[b].instructions[i];
        if (af->frame.realigns_stack && b == mf->info.entry_block && i < 4u)
          continue;
        if (inst->opcode == AST_MACHINE_X86_STORE &&
            stack_memory_overlap(&saved_memory, &inst->operands[1])) {
          if (b != mf->info.entry_block ||
              !saved_transfer(inst, unit, &af->objects[object], 0))
            return 0;
          ++saves;
        }
      }
    if (saves != 1u)
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
static int
physical_verify_source_function_is_extern(const AstRegAllocProgram *p,
                                          uint32_t function) {
  if (!p || !p->machine || !p->machine->mir)
    return 0;
  const AstMirFunction *mf = ast_mir_function_at(p->machine->mir, function);
  AstMirFunctionInfo info;
  return mf && ast_mir_function_info(mf, &info) &&
         (info.flags & AST_HIR_FUNCTION_EXTERN) != 0u;
}

int physical_verify_one_function(const AstRegAllocProgram *p, uint32_t f) {
  if (physical_verify_source_function_is_extern(p, f))
    return 1;
  const AstMirFunction *source_mir = ast_mir_function_at(p->machine->mir, f);
  AstMirFunctionInfo source_info;
  if (!source_mir || !ast_mir_function_info(source_mir, &source_info))
    return 0;
  int manual = (source_info.flags & AST_HIR_FUNCTION_NAKED) ||
               source_info.frame_policy == AST_HIR_FRAME_NAKED ||
               source_info.frame_policy == AST_HIR_FRAME_VALIDATE;
  int ok =
      p->target.arch == AST_TARGET_ARCH_AARCH64
          ? (manual ? physical_verify_native_manual_other(p, f)
                    : (physical_verify_aarch64(p, f) &&
                       physical_verify_native(p, f)))
      : p->target.arch == AST_TARGET_ARCH_RISCV64
          ? (manual ? physical_verify_native_manual_other(p, f)
                    : (physical_verify_riscv64(p, f) &&
                       physical_verify_native(p, f)))
      : p->target.arch == AST_TARGET_ARCH_X86_64
          ? (physical_verify_function(p, f) && physical_verify_native(p, f))
          : 0;
  if (!ok || !physical_atomic_count(p, f))
    return 0;
  return 1;
}
int ast_physical_verify(const AstRegAllocProgram *p) {
  if (!p || !p->finalized || !ast_regalloc_verify(p))
    return 0;
  for (uint32_t f = 0; f < p->function_count; ++f)
    if (!physical_verify_one_function(p, f))
      return 0;
  return 1;
}
