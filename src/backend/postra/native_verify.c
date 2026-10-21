#include "../../isa/aarch64_scalable.h"
#include "../../isa/aarch64_scalar_contract.h"
#include "../../isa/riscv64_branch_contract.h"
#include "../../isa/riscv64_memory_contract.h"
#include "../../isa/riscv64_scalar_contract.h"
#include "../../isa/system_contract.h"
#include "../../isa/vector_contract.h"
#include "physical_internal.h"
#include <ctype.h>

static int same_span(AstSpan a, AstSpan b) {
  return a.file_id == b.file_id && a.offset == b.offset && a.length == b.length;
}
static int mnemonic(AstName n, const char *name) {
  if (!n.text || n.span.length != strlen(name))
    return 0;
  for (uint32_t i = 0; i < n.span.length; ++i)
    if (tolower((unsigned char)n.text[i]) != name[i])
      return 0;
  return 1;
}
static int source_opcode(AstName name, AstMachineOpcode op) {
  if (mnemonic(name, "mov"))
    return op == AST_MACHINE_X86_MOV || op == AST_MACHINE_X86_MOV_IMM ||
           op == AST_MACHINE_X86_LOAD || op == AST_MACHINE_X86_STORE;
  if (mnemonic(name, "lea"))
    return op == AST_MACHINE_X86_LEA;
  if (mnemonic(name, "add"))
    return op == AST_MACHINE_X86_ADD;
  if (mnemonic(name, "sub"))
    return op == AST_MACHINE_X86_SUB;
  if (mnemonic(name, "and"))
    return op == AST_MACHINE_X86_AND;
  if (mnemonic(name, "or"))
    return op == AST_MACHINE_X86_OR;
  if (mnemonic(name, "xor"))
    return op == AST_MACHINE_X86_XOR;
  if (mnemonic(name, "imul"))
    return op == AST_MACHINE_X86_MUL;
  if (mnemonic(name, "shl"))
    return op == AST_MACHINE_X86_SHL;
  if (mnemonic(name, "shr"))
    return op == AST_MACHINE_X86_LSHR;
  if (mnemonic(name, "sar"))
    return op == AST_MACHINE_X86_ASHR;
  if (mnemonic(name, "popcnt"))
    return op == AST_MACHINE_X86_POPCNT;
  if (mnemonic(name, "nop"))
    return op == AST_MACHINE_X86_NOP;
  if (mnemonic(name, "cli"))
    return op == AST_MACHINE_X86_CLI;
  if (mnemonic(name, "sti"))
    return op == AST_MACHINE_X86_STI;
  if (mnemonic(name, "hlt"))
    return op == AST_MACHINE_X86_HLT;
  if (mnemonic(name, "cld"))
    return op == AST_MACHINE_X86_CLD;
  if (mnemonic(name, "in"))
    return op == AST_MACHINE_X86_IN;
  if (mnemonic(name, "out"))
    return op == AST_MACHINE_X86_OUT;
  if (mnemonic(name, "lidt"))
    return op == AST_MACHINE_X86_LIDT;
  if (mnemonic(name, "lgdt"))
    return op == AST_MACHINE_X86_LGDT;
  if (mnemonic(name, "push"))
    return op == AST_MACHINE_X86_PUSH;
  if (mnemonic(name, "pop"))
    return op == AST_MACHINE_X86_POP;
  if (mnemonic(name, "call"))
    return op == AST_MACHINE_X86_CALL || op == AST_MACHINE_X86_CALL_INDIRECT;
  if (mnemonic(name, "jmp"))
    return op == AST_MACHINE_X86_JMP || op == AST_MACHINE_X86_JMP_INDIRECT;

  if (mnemonic(name, "ret"))
    return op == AST_MACHINE_X86_RET;
  return 0;
}
static int writes(const AstPhysicalInstructionInfo *i, uint32_t unit) {
  for (uint32_t d = 0; d < i->result_count; ++d)
    if (i->operands[d].register_unit == unit)
      return 1;
  return 0;
}
static int transfer(const AstRegAllocProgram *p,
                    const AstPhysicalInstructionInfo *i, uint32_t unit,
                    int restore, uint32_t required) {
  AstMachineOpcode opcode =
      p->target.arch == AST_TARGET_ARCH_AARCH64
          ? (restore ? AST_MACHINE_A64_LOAD : AST_MACHINE_A64_STORE)
      : p->target.arch == AST_TARGET_ARCH_RISCV64
          ? (restore ? AST_MACHINE_RISCV_LD : AST_MACHINE_RISCV_SD)
          : (restore ? AST_MACHINE_X86_LOAD : AST_MACHINE_X86_STORE);
  uint32_t sp = p->target.arch == AST_TARGET_ARCH_AARCH64   ? 320u
                : p->target.arch == AST_TARGET_ARCH_RISCV64 ? 66u
                                                            : 68u;
  if (i->opcode != opcode || i->operand_count != 2u ||
      i->operands[0].kind != AST_PHYSICAL_OPERAND_REGISTER ||
      i->operands[0].register_unit != unit ||
      i->operands[1].kind != AST_PHYSICAL_OPERAND_MEMORY ||
      i->operands[1].register_unit != sp)
    return 0;
  uint32_t bits = i->operands[0].bit_width;
  int valid = unit / 64u == MACHINE_PHYSREG_FILE_GPR
                  ? bits == 64u
                  : unit / 64u == MACHINE_PHYSREG_FILE_FP_VECTOR &&
                        (bits == 128u || bits == 256u || bits == 512u);
  return valid && bits >= required && i->operands[1].bit_width == bits;
}
static int overwrites_saved(const AstPhysicalInstructionInfo *i,
                            const AstPhysicalOperand *saved) {
  if ((i->opcode != AST_MACHINE_X86_STORE &&
       i->opcode != AST_MACHINE_A64_STORE &&
       i->opcode != AST_MACHINE_RISCV_SD && i->opcode != AST_MACHINE_RISCV_SW &&
       i->opcode != AST_MACHINE_RISCV_SH &&
       i->opcode != AST_MACHINE_RISCV_SB) ||
      i->operand_count != 2u)
    return 0;
  const AstPhysicalOperand *mem = &i->operands[1];
  if (mem->kind != AST_PHYSICAL_OPERAND_MEMORY ||
      mem->register_unit != saved->register_unit)
    return 0;
  /* Unsigned differences avoid signed overflow for negative red-zone offsets.
   */
  return mem->displacement < saved->displacement
             ? (uint64_t)saved->displacement - (uint64_t)mem->displacement <
                   (mem->bit_width + 7u) / 8u
             : (uint64_t)mem->displacement - (uint64_t)saved->displacement <
                   (saved->bit_width + 7u) / 8u;
}
typedef struct ManualStackEntry {
  uint32_t unit;
  uint8_t preserved;
  uint8_t dirty_before;
} ManualStackEntry;

static uint32_t preserved_bits_for_unit(const AstRegAllocProgram *p,
                                        uint32_t unit) {
  MachinePhysRegView view = {0};
  view.file = (MachinePhysRegFile)(unit / 64u);
  view.index = unit % 64u;
  return machine_physreg_preserved_bits(p->abi, &view);
}
static int manual_terminal(AstMachineOpcode op) {
  return op == AST_MACHINE_X86_RET || op == AST_MACHINE_X86_JMP ||
         op == AST_MACHINE_X86_JMP_INDIRECT;
}
/* Manual functions have no allocator-owned frame. Prove the pieces an ABI
   caller relies on directly from the final instruction stream: entry RSP is
   restored, nonvolatile registers are restored, calls are aligned, and Win64
   calls own at least 32 bytes of shadow/home space. */
static int verify_manual_contract(const AstRegAllocProgram *p,
                                  const PhysicalBlock *block) {
  if (!block || !block->count)
    return 0;
  ManualStackEntry *stack = (ManualStackEntry *)calloc(
      block->count ? block->count : 1u, sizeof(*stack));
  if (!stack)
    return 0;
  uint32_t depth = 0u, saved[MACHINE_REGISTER_UNITS] = {0};
  uint8_t dirty[MACHINE_REGISTER_UNITS] = {0};
  int64_t delta = 0;
  uint64_t shadow_bytes = 0u;
  int ok = 1;
  int terminal = 0;
  const uint32_t rsp = MACHINE_PHYSREG_FILE_GPR * 64u + 4u;
  for (uint32_t i = 0; i < block->count && ok; ++i) {
    const AstPhysicalInstructionInfo *inst = &block->instructions[i];
    if (terminal) {
      ok = 0;
      break;
    }
    if (inst->opcode == AST_MACHINE_X86_PUSH) {
      uint32_t unit = inst->operands[0].register_unit;
      if (delta < INT64_MIN + 8) {
        ok = 0;
        break;
      }
      ManualStackEntry e = {0};
      e.unit = unit;
      e.preserved = preserved_bits_for_unit(p, unit) != 0u;
      e.dirty_before = unit < MACHINE_REGISTER_UNITS ? dirty[unit] : 0u;
      if (e.preserved)
        ++saved[unit];
      stack[depth++] = e;
      delta -= 8;
      shadow_bytes = 0u;
    } else if (inst->opcode == AST_MACHINE_X86_POP) {
      if (!depth || delta > -8) {
        ok = 0;
        break;
      }
      ManualStackEntry e = stack[--depth];
      uint32_t target = inst->operands[0].register_unit;
      if (e.preserved && saved[e.unit])
        --saved[e.unit];
      if (target < MACHINE_REGISTER_UNITS && preserved_bits_for_unit(p, target))
        dirty[target] = target == e.unit && e.preserved ? e.dirty_before : 1u;
      delta += 8;
      shadow_bytes = 0u;
    } else {
      int stack_math =
          (inst->opcode == AST_MACHINE_X86_ADD ||
           inst->opcode == AST_MACHINE_X86_SUB) &&
          inst->operand_count == 3u &&
          inst->operands[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
          inst->operands[0].register_unit == rsp;
      if (stack_math) {
        if (inst->operands[1].register_unit != rsp ||
            inst->operands[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
            inst->operands[2].immediate_bits > INT32_MAX) {
          ok = 0;
          break;
        }
        int64_t amount = (int64_t)inst->operands[2].immediate_bits;
        if (inst->opcode == AST_MACHINE_X86_SUB) {
          if (delta < INT64_MIN + amount) {
            ok = 0;
            break;
          }
          delta -= amount;
          if (UINT64_MAX - shadow_bytes < (uint64_t)amount) {
            ok = 0;
            break;
          }
          shadow_bytes += (uint64_t)amount;
        } else {
          if (delta > INT64_MAX - amount) {
            ok = 0;
            break;
          }
          delta += amount;
          shadow_bytes = (uint64_t)amount <= shadow_bytes
                             ? shadow_bytes - (uint64_t)amount
                             : 0u;
        }
        if (delta > 0) {
          ok = 0;
          break;
        }
      } else if (writes(inst, rsp)) {
        ok = 0;
        break;
      }

      for (uint32_t d = 0; d < inst->result_count; ++d) {
        uint32_t unit = inst->operands[d].register_unit;
        if (unit < MACHINE_REGISTER_UNITS && unit != rsp &&
            preserved_bits_for_unit(p, unit))
          dirty[unit] = 1u;
      }
    }
    if (inst->opcode == AST_MACHINE_X86_CALL ||
        inst->opcode == AST_MACHINE_X86_CALL_INDIRECT) {
      if ((((uint64_t)(delta + 8)) & 15u) != 0u) {
        ok = 0;
        break;
      }
      if (p->abi == AST_ABI_WINDOWS_X64 && shadow_bytes < 32u) {
        ok = 0;
        break;
      }
    }
    if (manual_terminal(inst->opcode) ||
        (inst->opcode == AST_MACHINE_X86_RAW_BYTES && i + 1u == block->count)) {
      if (delta || depth) {
        ok = 0;
        break;
      }
      for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit)
        if (dirty[unit] || saved[unit]) {
          ok = 0;
          break;
        }
      terminal = ok;
    }
  }
  free(stack);
  return ok && terminal;
}

/* Validate region boundaries and original register preservation from the final
   stream. No allocator-owned NativeLowerState is trusted here. */
int physical_verify_native(const AstRegAllocProgram *p, uint32_t f) {
  const MachineFunction *mf = &p->machine->functions[f];
  const AllocationFunction *af = &p->functions[f];
  const AstMirFunction *mir = ast_mir_function_at(p->machine->mir, f);
  const AstHirFunction *hir = ast_mir_function_source_hir(mir);
  AstMirFunctionInfo fi;
  if (!ast_mir_function_info(mir, &fi))
    return 0;
  int manual = (fi.flags & AST_HIR_FUNCTION_NAKED) ||
               fi.frame_policy == AST_HIR_FRAME_NAKED ||
               fi.frame_policy == AST_HIR_FRAME_VALIDATE;
  for (uint32_t id = 0; id < mf->info.instruction_count; ++id) {
    const MachineInstruction *mi = &mf->instructions[id];
    if (mi->info.source_op != AST_MIR_OP_NATIVE)
      continue;
    AstMirInstructionInfo source;
    AstHirNativeRegionInfo info;
    if (!ast_mir_function_instruction(mir, mi->info.source_instruction,
                                      &source))
      return 0;
    const AstHirNativeRegion *region =
        ast_hir_function_native_region(hir, source.native_region);
    if (!region || !ast_hir_native_region_info(region, &info))
      return 0;
    if (!info.statement_count)
      continue;
    AstHirNativeStmtInfo first;
    if (!ast_hir_native_region_statement(region, 0u, &first))
      return 0;
    const PhysicalBlock *block = NULL;
    uint32_t start = 0u, matches = 0u;
    for (uint32_t b = 0; b < af->block_count; ++b)
      for (uint32_t i = 0; i < af->blocks[b].count; ++i) {
        const AstPhysicalInstructionInfo *pi = &af->blocks[b].instructions[i];
        if (pi->source_instruction == id && same_span(pi->span, first.span)) {
          block = &af->blocks[b];
          start = i;
          ++matches;
        }
      }
    if (matches != 1u || !block || info.statement_count > block->count - start)
      return 0;
    uint32_t end = start + info.statement_count, begin = start, tail = end;
    while (begin && block->instructions[begin - 1u].source_instruction == id)
      --begin;
    while (tail < block->count &&
           block->instructions[tail].source_instruction == id)
      ++tail;
    int other = p->target.arch != AST_TARGET_ARCH_X86_64;
    if (other && !physical_verify_native_other_region(p, f, id, block, begin,
                                                      start, end, tail))
      return 0;
    if (!other && !physical_verify_native_x86_region(p, f, id, block, begin,
                                                     start, end, tail, manual))
      return 0;
    uint16_t changed[MACHINE_REGISTER_UNITS] = {0};
    /* Input shuffles also overwrite registers even when the native body
       only reads them. Their original values must survive the boundary. */
    for (uint32_t i = begin; i < start; ++i)
      for (uint32_t d = 0; d < block->instructions[i].result_count; ++d) {
        uint32_t unit = block->instructions[i].operands[d].register_unit;
        if (unit >= MACHINE_REGISTER_UNITS)
          return 0;
        uint16_t bits = (uint16_t)block->instructions[i].operands[d].bit_width;
        if (bits > changed[unit])
          changed[unit] = bits;
      }
    for (uint32_t i = start; i < end; ++i) {
      AstHirNativeStmtInfo statement;
      const AstPhysicalInstructionInfo *pi = &block->instructions[i];
      if (!ast_hir_native_region_statement(region, i - start, &statement) ||
          pi->source_instruction != id || !same_span(pi->span, statement.span))
        return 0;
      if (statement.kind == AST_HIR_NATIVE_BYTES) {
        if (pi->opcode != AST_MACHINE_X86_RAW_BYTES || pi->operand_count ||
            pi->result_count || !pi->raw_bytes ||
            pi->raw_byte_count != statement.byte_count ||
            memcmp(pi->raw_bytes, statement.bytes, statement.byte_count) != 0)
          return 0;
      } else if (other) {
        /* Complete source packets and boundary transfers checked above. */
      } else if (pi->opcode == AST_MACHINE_X86_SYSTEM) {
        if (!physical_verify_system_source(p, &statement, pi))
          return 0;
      } else if (pi->opcode == AST_MACHINE_X86_VECTOR) {
        if (!physical_verify_vector_source(p, &statement, manual, pi))
          return 0;
      } else if (system_source_form(p->target.arch, &statement) ||
                 vector_source_form(p->target.arch, statement.mnemonic) ||
                 !source_opcode(statement.mnemonic, pi->opcode))
        return 0;
      for (uint32_t d = 0; d < pi->result_count; ++d) {
        if (pi->operands[d].register_unit >= MACHINE_REGISTER_UNITS)
          return 0;
        uint32_t unit = pi->operands[d].register_unit;
        if ((p->target.arch == AST_TARGET_ARCH_RISCV64 && unit == 64u) ||
            (p->target.arch == AST_TARGET_ARCH_AARCH64 && unit == 321u))
          continue;
        const VectorForm *vector = vector_physical_form(pi);
        uint16_t bits =
            (uint16_t)(p->target.arch == AST_TARGET_ARCH_AARCH64 &&
                               unit / 64u == MACHINE_PHYSREG_FILE_FP_VECTOR
                           ? 128u
                       : vector && (vector_evex(vector) ||
                                    (vector_vex(vector) &&
                                     (p->target.features &
                                      AST_TARGET_FEATURE_X86_AVX512F)))
                           ? 512u
                       : vector && vector_vex(vector)
                           ? 256u
                           : pi->operands[d].bit_width);
        if (bits > changed[unit])
          changed[unit] = bits;
      }
    }
    for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit)
      if (changed[unit]) {
        if (manual || other)
          continue; /* Other targets use bit provenance. */
        const AstPhysicalOperand *saved = NULL;
        uint32_t save_at = 0u, restore_at = 0u;
        for (uint32_t i = begin; i < start; ++i) {
          if (transfer(p, &block->instructions[i], unit, 0, changed[unit])) {
            saved = &block->instructions[i].operands[1];
            save_at = i;
            break;
          }
          if (writes(&block->instructions[i], unit))
            return 0;
        }
        if (!saved)
          return 0;
        int restored = 0;
        for (uint32_t i = end; i < tail; ++i)
          if (writes(&block->instructions[i], unit)) {
            restored =
                transfer(p, &block->instructions[i], unit, 1, changed[unit]) &&
                saved->bit_width ==
                    block->instructions[i].operands[1].bit_width &&
                physical_same_location(saved,
                                       &block->instructions[i].operands[1]);
            restore_at = i;
          }
        if (!restored)
          return 0;
        for (uint32_t i = save_at + 1u; i < restore_at; ++i)
          if (overwrites_saved(&block->instructions[i], saved))
            return 0;
      }
  }
  if (manual &&
      (af->block_count != 1u || !verify_manual_contract(p, &af->blocks[0])))
    return 0;
  return 1;
}

/* Independent post-lowering proof for the first A64/RV64 exact/naked subset.
   The source statements, result registers and complete final instruction
   stream are checked without using NativePlan's writable bookkeeping. */
static const AstHirNativeOperandInfo *manual_role(const AstHirNativeStmtInfo *s,
                                                  const char *role_name) {
  const AstHirNativeOperandInfo *found = NULL;
  for (uint32_t i = 0; i < s->operand_count; ++i)
    if (mnemonic(s->operands[i].name, role_name)) {
      if (found)
        return NULL;
      found = &s->operands[i];
    }
  return found;
}
static int manual_register_matches(const AstRegAllocProgram *p,
                                   const AstHirNativeOperandInfo *src,
                                   const AstPhysicalOperand *dst, int writing) {
  AstHirNativeExprInfo expr;
  char name[AST_MACHINE_REGISTER_NAME_CAP];
  MachinePhysRegView view;
  if (!src || src->is_memory || !ast_hir_native_expr_info(src->value, &expr) ||
      expr.kind != AST_HIR_NATIVE_EXPR_NAME ||
      expr.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !expr.name.text ||
      expr.name.span.length >= sizeof(name) || !expr.name.span.length)
    return 0;
  for (uint32_t i = 0; i < expr.name.span.length; ++i)
    name[i] = (char)tolower((unsigned char)expr.name.text[i]);
  name[expr.name.span.length] = '\0';
  if (!machine_physreg_view(&p->target, name, &view) || view.special ||
      (view.reserved && !(p->target.arch == AST_TARGET_ARCH_RISCV64 &&
                          view.index == 0u && !writing)) ||
      view.file != MACHINE_PHYSREG_FILE_GPR || view.index >= 32u ||
      dst->kind != AST_PHYSICAL_OPERAND_REGISTER ||
      dst->bank != AST_MACHINE_BANK_GPR ||
      dst->register_unit != view.alias_unit || dst->bit_width != view.width ||
      strcmp(dst->register_name, name) != 0)
    return 0;
  if (p->target.arch == AST_TARGET_ARCH_AARCH64) {
    if ((view.width != 32u && view.width != 64u) || view.index == 18u ||
        (writing && view.index == 30u) || (writing == 1 && view.index >= 19u))
      return 0;
  } else if (p->target.arch == AST_TARGET_ARCH_RISCV64) {
    if (view.width != 64u || (view.index >= 1u && view.index <= 4u) ||
        (writing && view.index == 0u) ||
        (writing == 1 && (view.index == 8u || view.index == 9u ||
                          (view.index >= 18u && view.index <= 27u))))
      return 0;
  } else
    return 0;
  return 1;
}
/* ABI link register may only participate in exact full-width stack
   transfers; all arithmetic and general register forms still reject it. */
static int manual_stack_link_matches(const AstRegAllocProgram *p,
                                     const AstHirNativeOperandInfo *src,
                                     const AstPhysicalOperand *dst) {
  AstHirNativeExprInfo expr;
  MachinePhysRegView view;
  char name[AST_MACHINE_REGISTER_NAME_CAP];
  const char *expected =
      p->target.arch == AST_TARGET_ARCH_AARCH64 ? "x30" : "ra";
  if (!src || src->is_memory || !ast_hir_native_expr_info(src->value, &expr) ||
      expr.kind != AST_HIR_NATIVE_EXPR_NAME ||
      expr.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !expr.name.text ||
      !expr.name.span.length || expr.name.span.length >= sizeof(name))
    return 0;
  for (uint32_t j = 0; j < expr.name.span.length; ++j)
    name[j] = (char)tolower((unsigned char)expr.name.text[j]);
  name[expr.name.span.length] = '\0';
  return (!strcmp(name, expected) ||
          (p->target.arch == AST_TARGET_ARCH_RISCV64 && !strcmp(name, "x1"))) &&
         machine_physreg_view(&p->target, name, &view) &&
         view.file == MACHINE_PHYSREG_FILE_GPR && view.width == 64u &&
         dst->kind == AST_PHYSICAL_OPERAND_REGISTER &&
         dst->bank == AST_MACHINE_BANK_GPR && dst->bit_width == 64u &&
         dst->register_unit == view.alias_unit &&
         !strcmp(dst->register_name, name);
}

static int manual_source_sp(const AstHirNativeOperandInfo *src) {
  AstHirNativeExprInfo expr;
  return src && !src->is_memory &&
         ast_hir_native_expr_info(src->value, &expr) &&
         expr.kind == AST_HIR_NATIVE_EXPR_NAME &&
         expr.binding_kind == AST_HIR_NATIVE_BINDING_TARGET &&
         mnemonic(expr.name, "sp");
}
static int manual_physical_sp(const AstRegAllocProgram *p,
                              const AstPhysicalOperand *op) {
  MachinePhysRegView view;
  if (!machine_physreg_view(&p->target, "sp", &view))
    return 0;
  return op->kind == AST_PHYSICAL_OPERAND_REGISTER && op->bit_width == 64u &&
         !strcmp(op->register_name, "sp") &&
         op->register_unit == view.alias_unit &&
         op->bank == (p->target.arch == AST_TARGET_ARCH_AARCH64
                          ? AST_MACHINE_BANK_SPECIAL
                          : AST_MACHINE_BANK_GPR);
}
/* Independently resolve manual labels from immutable HIR (never trust the
   lowering's branch immediate). Labels consume no machine instructions;
   negative and zero PC-relative offsets are legitimate direct branches. */
static int verify_label_name(const AstHirNativeStmtInfo *stmt, AstName *name) {
  const AstHirNativeOperandInfo *op = manual_role(stmt, "name");
  AstHirNativeExprInfo expr;
  if (stmt->kind != AST_HIR_NATIVE_INSTRUCTION || stmt->attribute_count ||
      stmt->operand_count != 1u || !op || op->is_memory ||
      !ast_hir_native_expr_info(op->value, &expr) ||
      expr.kind != AST_HIR_NATIVE_EXPR_NAME ||
      expr.binding_kind != AST_HIR_NATIVE_BINDING_TARGET ||
      !expr.name.span.length || expr.name.span.length > 80u)
    return 0;
  *name = expr.name;
  return 1;
}
static int verify_name_same(AstName a, AstName b) {
  return a.text && b.text && a.span.length == b.span.length &&
         !memcmp(a.text, b.text, a.span.length);
}
static uint32_t verify_native_width(AstTargetArch arch,
                                    const AstHirNativeStmtInfo *stmt) {
  if (mnemonic(stmt->mnemonic, "label"))
    return 0u;
  if (arch == AST_TARGET_ARCH_RISCV64 && mnemonic(stmt->mnemonic, "jal") &&
      manual_role(stmt, "symbol"))
    return 8u;
  return 4u;
}
static int verify_signed_target(AstTargetArch arch,
                                const AstHirNativeRegion *region,
                                uint32_t start, AstName target,
                                int64_t *offset) {
  AstHirNativeRegionInfo info;
  uint32_t found = UINT32_MAX, byte_pc = 0u, branch_pc = 0u, target_pc = 0u;
  if (!ast_hir_native_region_info(region, &info) ||
      info.statement_count > 4096u)
    return 0;
  for (uint32_t j = 0; j < info.statement_count; ++j) {
    AstHirNativeStmtInfo stmt;
    AstName name;
    if (!ast_hir_native_region_statement(region, j, &stmt))
      return 0;
    if (j == start)
      branch_pc = byte_pc;
    if (mnemonic(stmt.mnemonic, "label")) {
      if (!verify_label_name(&stmt, &name))
        return 0;
      if (verify_name_same(name, target)) {
        if (found != UINT32_MAX)
          return 0;
        found = j;
        target_pc = byte_pc;
      }
    }
    byte_pc += verify_native_width(arch, &stmt);
  }
  if (found == UINT32_MAX || target_pc >= byte_pc)
    return 0;
  *offset = (int64_t)target_pc - (int64_t)branch_pc;
  return 1;
}
static AstMachineOpcode manual_other_opcode(AstTargetArch arch, AstName n) {
  if (mnemonic(n, "ret"))
    return arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_RET
                                           : AST_MACHINE_RISCV_RET;
  if (arch == AST_TARGET_ARCH_AARCH64) {
    if (mnemonic(n, "add"))
      return AST_MACHINE_A64_ADD;
    if (mnemonic(n, "sub"))
      return AST_MACHINE_A64_SUB;
    if (mnemonic(n, "and"))
      return AST_MACHINE_A64_AND;
    if (mnemonic(n, "orr"))
      return AST_MACHINE_A64_OR;
    if (mnemonic(n, "eor"))
      return AST_MACHINE_A64_XOR;
    if (mnemonic(n, "b"))
      return AST_MACHINE_A64_BR;
    if (mnemonic(n, "bl"))
      return AST_MACHINE_A64_NATIVE_BL;
    if (mnemonic(n, "cbz"))
      return AST_MACHINE_A64_NATIVE_CBZ;
    if (mnemonic(n, "cbnz"))
      return AST_MACHINE_A64_NATIVE_CBNZ;
    if (mnemonic(n, "ldr"))
      return AST_MACHINE_A64_LOAD;
    if (mnemonic(n, "str"))
      return AST_MACHINE_A64_STORE;
  } else if (arch == AST_TARGET_ARCH_RISCV64) {
    if (mnemonic(n, "add"))
      return AST_MACHINE_RISCV_ADD;
    if (mnemonic(n, "sub"))
      return AST_MACHINE_RISCV_SUB;
    if (mnemonic(n, "and"))
      return AST_MACHINE_RISCV_AND;
    if (mnemonic(n, "or"))
      return AST_MACHINE_RISCV_OR;
    if (mnemonic(n, "xor"))
      return AST_MACHINE_RISCV_XOR;
    if (mnemonic(n, "addi"))
      return AST_MACHINE_RISCV_ADDI;
    /* RV64 JAL x0 and JAL ra share source mnemonic; distinguish in the
       independent source-versus-physical operand validator. */
    if (mnemonic(n, "jal"))
      return AST_MACHINE_RISCV_J;
    AstMachineOpcode branch = rv_native_branch_source(n);
    if (branch != AST_MACHINE_OP_INVALID)
      return branch;
    if (mnemonic(n, "ld"))
      return AST_MACHINE_RISCV_LD;
    if (mnemonic(n, "sd"))
      return AST_MACHINE_RISCV_SD;
  }
  return AST_MACHINE_OP_INVALID;
}
/* Independently reconstruct a source immediate, including syntactic unary
   sign and base prefixes; do not trust the lowered immediate when verifying
   an exact region. Unsigned arithmetic is explicit to avoid signed overflow. */
static int manual_parse_literal(AstHirText text, uint64_t *value) {
  size_t n = 0u;
  uint32_t base = 10u, digits = 0u;
  uint64_t result = 0u;
  if (!text.data || !text.length)
    return 0;
  if (text.length > 2u && text.data[0] == '0') {
    if (text.data[1] == 'x' || text.data[1] == 'X') {
      base = 16u;
      n = 2u;
    } else if (text.data[1] == 'b' || text.data[1] == 'B') {
      base = 2u;
      n = 2u;
    } else if (text.data[1] == 'o' || text.data[1] == 'O') {
      base = 8u;
      n = 2u;
    }
  }
  for (; n < text.length; ++n) {
    unsigned char c = (unsigned char)text.data[n];
    if (c == '_')
      continue;
    uint32_t digit = c >= '0' && c <= '9'   ? (uint32_t)(c - '0')
                     : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10)
                                            : UINT32_MAX;
    if (digit >= base || result > (UINT64_MAX - digit) / base)
      return 0;
    result = result * base + digit;
    ++digits;
  }
  if (!digits)
    return 0;
  *value = result;
  return 1;
}
static int manual_immediate_expr(const AstHirNativeExpr *expr,
                                 uint64_t *value) {
  AstHirNativeExprInfo v;
  if (!expr || !ast_hir_native_expr_info(expr, &v))
    return 0;
  if (v.kind == AST_HIR_NATIVE_EXPR_CONSTANT && !v.constant.hi) {
    *value =
        v.constant.is_negative ? UINT64_C(0) - v.constant.lo : v.constant.lo;
    return 1;
  }
  if (v.kind == AST_HIR_NATIVE_EXPR_LITERAL && v.token_kind == AST_TOK_INTEGER)
    return manual_parse_literal(v.literal, value);
  if (v.kind == AST_HIR_NATIVE_EXPR_UNARY && v.child_count == 1u &&
      (v.token_kind == AST_TOK_MINUS || v.token_kind == AST_TOK_PLUS) &&
      manual_immediate_expr(ast_hir_native_expr_child(expr, 0u), value)) {
    if (v.token_kind == AST_TOK_MINUS)
      *value = UINT64_C(0) - *value;
    return 1;
  }
  return 0;
}
/* Source and emitted stack transfers are checked independently of native
   lowering. Architectural addressing is intentionally limited to unsigned,
   naturally aligned offsets from the literal SP. */
static int manual_stack_statement(const AstRegAllocProgram *p,
                                  const AstHirNativeStmtInfo *s,
                                  const AstPhysicalInstructionInfo *i,
                                  int load) {
  const AstHirNativeOperandInfo *reg = manual_role(s, load ? "dst" : "src");
  const AstHirNativeOperandInfo *off = manual_role(s, "offset");
  uint64_t offset;
  if (s->operand_count != 2u || i->operand_count != 2u ||
      i->result_count != (uint32_t)load || !reg || !off || off->is_memory ||
      !manual_immediate_expr(off->value, &offset) ||
      !(manual_register_matches(p, reg, &i->operands[0], load ? 2 : 0) ||
        manual_stack_link_matches(p, reg, &i->operands[0])) ||
      (i->operands[0].bit_width != 32u && i->operands[0].bit_width != 64u) ||
      (p->target.arch == AST_TARGET_ARCH_RISCV64 &&
       i->operands[0].bit_width != 64u))
    return 0;
  const AstPhysicalOperand *m = &i->operands[1];
  const uint64_t bytes = i->operands[0].bit_width / 8u;
  MachinePhysRegView sp;
  if (!machine_physreg_view(&p->target, "sp", &sp) ||
      m->kind != AST_PHYSICAL_OPERAND_MEMORY ||
      m->bit_width != i->operands[0].bit_width ||
      m->register_unit != sp.alias_unit || strcmp(m->register_name, "sp") ||
      m->index_register_name[0] || m->scale || m->displacement < 0 ||
      (uint64_t)m->displacement != offset || offset % bytes)
    return 0;
  return p->target.arch == AST_TARGET_ARCH_AARCH64 ? offset / bytes <= 4095u
                                                   : offset <= 2047u;
}

static int manual_other_statement(const AstRegAllocProgram *p,
                                  const AstHirNativeRegion *region,
                                  uint32_t statement_index,
                                  const AstHirNativeStmtInfo *s,
                                  const AstPhysicalInstructionInfo *i) {
  if (a64_scalable_statement(p->target.arch, s)) {
    AstPhysicalInstructionInfo expected;
    return physical_native_scalable_info(&p->target, s, &expected) &&
           physical_native_packet_equal(&expected, i);
  }
  if (system_source_form(p->target.arch, s))
    return physical_verify_system_source(p, s, i);
  if (vector_statement_form(p->target.arch, s))
    return physical_verify_vector_source(p, s, 1, i);
  if (physical_native_a64_data_form(&p->target, s)) {
    AstPhysicalInstructionInfo expected;
    return physical_native_a64_data_info(&p->target, s, NULL, &expected) &&
           physical_native_packet_equal(&expected, i);
  }
  if (p->target.arch == AST_TARGET_ARCH_RISCV64 &&
      mnemonic(s->mnemonic, "jalr")) {
    AstPhysicalInstructionInfo expected;
    return physical_native_indirect_info(&p->target, s, &expected) &&
           physical_native_packet_equal(&expected, i);
  }
  if (rv_memory_source(p->target.arch, s->mnemonic)) {
    AstPhysicalInstructionInfo expected;
    return physical_native_memory_info(&p->target, s, NULL, 1, &expected) &&
           physical_native_packet_equal(&expected, i);
  }
  if (rv_scalar_source(p->target.arch, s->mnemonic) &&
      !manual_source_sp(manual_role(s, "dst"))) {
    AstPhysicalInstructionInfo expected;
    return physical_native_scalar_info(&p->target, s, NULL, &expected) &&
           physical_native_packet_equal(&expected, i);
  }
  if (a64_scalar_source(p->target.arch, s->mnemonic)) {
    AstPhysicalInstructionInfo expected;
    return physical_native_a64_scalar_info(&p->target, s, NULL, &expected) &&
           physical_native_packet_equal(&expected, i);
  }
  AstMachineOpcode op = manual_other_opcode(p->target.arch, s->mnemonic);
  if (p->target.arch == AST_TARGET_ARCH_RISCV64 &&
      mnemonic(s->mnemonic, "jal")) {
    const AstHirNativeOperandInfo *dst = manual_role(s, "dst");
    AstHirNativeExprInfo rd;
    if (!dst || dst->is_memory || !ast_hir_native_expr_info(dst->value, &rd) ||
        rd.kind != AST_HIR_NATIVE_EXPR_NAME ||
        rd.binding_kind != AST_HIR_NATIVE_BINDING_TARGET)
      return 0;
    if (mnemonic(rd.name, "ra") || mnemonic(rd.name, "x1"))
      op = AST_MACHINE_RISCV_NATIVE_JAL;
  }
  if (s->kind != AST_HIR_NATIVE_INSTRUCTION || s->attribute_count ||
      op == AST_MACHINE_OP_INVALID || i->raw_bytes || i->raw_byte_count)
    return 0;
  const AstHirNativeOperandInfo *symbol_operand = manual_role(s, "symbol");
  if (symbol_operand) {
    AstHirNativeExprInfo expr;
    const AstHirNativeOperandInfo *dst = manual_role(s, "dst");
    if (symbol_operand->is_memory ||
        !ast_hir_native_expr_info(symbol_operand->value, &expr) ||
        expr.kind != AST_HIR_NATIVE_EXPR_NAME ||
        expr.binding_kind != AST_HIR_NATIVE_BINDING_TARGET || !expr.name.text ||
        !expr.name.span.length || expr.name.span.length > 255u ||
        i->operand_count != 1u || i->result_count ||
        i->operands[0].kind != AST_PHYSICAL_OPERAND_SYMBOL ||
        i->operands[0].module.span.length ||
        i->operands[0].symbol.span.length != expr.name.span.length ||
        memcmp(i->operands[0].symbol.text, expr.name.text,
               expr.name.span.length))
      return 0;
    if (p->target.arch == AST_TARGET_ARCH_AARCH64)
      return mnemonic(s->mnemonic, "bl") && s->operand_count == 1u &&
             i->opcode == AST_MACHINE_A64_CALL;
    AstHirNativeExprInfo rd;
    return p->target.arch == AST_TARGET_ARCH_RISCV64 &&
           mnemonic(s->mnemonic, "jal") && s->operand_count == 2u && dst &&
           !dst->is_memory && ast_hir_native_expr_info(dst->value, &rd) &&
           rd.kind == AST_HIR_NATIVE_EXPR_NAME &&
           rd.binding_kind == AST_HIR_NATIVE_BINDING_TARGET &&
           (mnemonic(rd.name, "ra") || mnemonic(rd.name, "x1")) &&
           i->opcode == AST_MACHINE_RISCV_CALL;
  }
  if (op != i->opcode)
    return 0;
  if (op == AST_MACHINE_A64_RET || op == AST_MACHINE_RISCV_RET)
    return !s->operand_count && !i->operand_count && !i->result_count;
  if (op == AST_MACHINE_A64_BR || op == AST_MACHINE_A64_NATIVE_BL ||
      op == AST_MACHINE_RISCV_J || op == AST_MACHINE_RISCV_NATIVE_JAL ||
      op == AST_MACHINE_A64_NATIVE_CBZ || op == AST_MACHINE_A64_NATIVE_CBNZ ||
      rv_native_branch_funct3(op) >= 0) {
    AstHirNativeExprInfo expr;
    int64_t displacement;
    const AstHirNativeOperandInfo *dst = manual_role(s, "dst");
    const AstHirNativeOperandInfo *target = manual_role(s, "target");
    if (!target || target->is_memory ||
        !ast_hir_native_expr_info(target->value, &expr) ||
        expr.kind != AST_HIR_NATIVE_EXPR_NAME ||
        expr.binding_kind != AST_HIR_NATIVE_BINDING_TARGET)
      return 0;
    uint32_t nregs = 0u;
    if (op == AST_MACHINE_A64_BR || op == AST_MACHINE_A64_NATIVE_BL) {
      if (s->operand_count != 1u)
        return 0;
    } else if (op == AST_MACHINE_RISCV_J ||
               op == AST_MACHINE_RISCV_NATIVE_JAL) {
      AstHirNativeExprInfo rd;
      if (s->operand_count != 2u || !dst || dst->is_memory ||
          !ast_hir_native_expr_info(dst->value, &rd) ||
          rd.kind != AST_HIR_NATIVE_EXPR_NAME ||
          rd.binding_kind != AST_HIR_NATIVE_BINDING_TARGET ||
          (op == AST_MACHINE_RISCV_J
               ? (!mnemonic(rd.name, "zero") && !mnemonic(rd.name, "x0"))
               : (!mnemonic(rd.name, "ra") && !mnemonic(rd.name, "x1"))))
        return 0;
    } else if (op == AST_MACHINE_A64_NATIVE_CBZ ||
               op == AST_MACHINE_A64_NATIVE_CBNZ) {
      if (s->operand_count != 2u ||
          !manual_register_matches(p, manual_role(s, "reg"), &i->operands[0],
                                   0) ||
          i->operands[0].register_unit % 64u == 30u)
        return 0;
      nregs = 1u;
    } else {
      if (s->operand_count != 3u ||
          !manual_register_matches(p, manual_role(s, "lhs"), &i->operands[0],
                                   0) ||
          !manual_register_matches(p, manual_role(s, "rhs"), &i->operands[1],
                                   0))
        return 0;
      nregs = 2u;
    }
    int64_t min_offset =
        !nregs
            ? (p->target.arch == AST_TARGET_ARCH_AARCH64 ? -INT64_C(134217728)
                                                         : -INT64_C(1048576))
            : (p->target.arch == AST_TARGET_ARCH_AARCH64 ? -INT64_C(1048576)
                                                         : -INT64_C(4096));
    int64_t max_offset =
        !nregs ? (p->target.arch == AST_TARGET_ARCH_AARCH64 ? INT64_C(134217724)
                                                            : INT64_C(1048572))
               : (p->target.arch == AST_TARGET_ARCH_AARCH64 ? INT64_C(1048572)
                                                            : INT64_C(4092));
    return verify_signed_target(p->target.arch, region, statement_index,
                                expr.name, &displacement) &&
           displacement >= min_offset && displacement <= max_offset &&
           i->result_count == 0u && i->operand_count == nregs + 1u &&
           i->operands[nregs].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           i->operands[nregs].bit_width == 64u &&
           i->operands[nregs].immediate_bits == (uint64_t)displacement;
  }
  if (op == AST_MACHINE_A64_LOAD || op == AST_MACHINE_A64_STORE ||
      op == AST_MACHINE_RISCV_LD || op == AST_MACHINE_RISCV_SD)
    return manual_stack_statement(
        p, s, i, op == AST_MACHINE_A64_LOAD || op == AST_MACHINE_RISCV_LD);
  if (s->operand_count != 3u || i->operand_count != 3u || i->result_count != 1u)
    return 0;
  const AstHirNativeOperandInfo *dst = manual_role(s, "dst");
  const AstHirNativeOperandInfo *lhs = manual_role(s, "lhs");
  const AstHirNativeOperandInfo *rhs = manual_role(s, "rhs");
  if (manual_source_sp(dst) || manual_source_sp(lhs)) {
    uint64_t literal;
    if (!manual_source_sp(dst) || !manual_source_sp(lhs) ||
        !manual_physical_sp(p, &i->operands[0]) ||
        !manual_physical_sp(p, &i->operands[1]) || !rhs || rhs->is_memory ||
        !manual_immediate_expr(rhs->value, &literal) ||
        i->operands[2].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
        i->operands[2].bit_width != 64u ||
        i->operands[2].immediate_bits != literal)
      return 0;
    if (p->target.arch == AST_TARGET_ARCH_AARCH64) {
      return (op == AST_MACHINE_A64_ADD || op == AST_MACHINE_A64_SUB) &&
             literal && !(literal % 16u) &&
             literal <= (UINT64_C(4095) << 12u) &&
             (literal <= 4095u ||
              (!(literal & 4095u) && (literal >> 12u) <= 4095u));
    }
    int64_t amount = (int64_t)literal;
    return op == AST_MACHINE_RISCV_ADDI && amount >= -2048 && amount <= 2047 &&
           amount && !(amount % 16);
  }
  if (!manual_register_matches(p, dst, &i->operands[0], 2) ||
      !manual_register_matches(p, lhs, &i->operands[1], 0) ||
      i->operands[0].bit_width != i->operands[1].bit_width)
    return 0;
  if (op == AST_MACHINE_RISCV_ADDI) {
    int64_t v = (int64_t)i->operands[2].immediate_bits;
    uint64_t value;
    return rhs && !rhs->is_memory &&
           manual_immediate_expr(rhs->value, &value) &&
           i->operands[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE &&
           i->operands[2].immediate_bits == value &&
           i->operands[2].bit_width == 64u && v >= -2048 && v <= 2047;
  }
  return manual_register_matches(p, rhs, &i->operands[2], 0) &&
         i->operands[0].bit_width == i->operands[2].bit_width;
}
/* Stack state is indexed in entry-SP-relative coordinates, so stores and
   loads remain comparable through nested allocations. Invalidated slots may
   never be read after deallocation/reallocation, and pinned saved-register
   slots may not be aliased by writes. */
typedef struct ManualNativeSlot {
  int64_t address;
  uint32_t bytes;
  uint32_t saved_unit;
  uint8_t live;
  uint8_t pinned;
  uint32_t link_pc; /* symbolic link value in a saved LR/RA stack slot */
} ManualNativeSlot;
static int native_preserved_register(AstTargetArch arch, uint32_t unit) {
  if (arch == AST_TARGET_ARCH_AARCH64 && unit >= 136u && unit < 144u)
    return 1;
  if (unit / 64u != MACHINE_PHYSREG_FILE_GPR)
    return 0;
  uint32_t r = unit % 64u;
  /* The link register is caller-clobbered by the ABI, but its *incoming*
     value must survive an eventual nested call to return correctly. Model
     it as pinned in manual frames so arbitrary stores cannot overwrite it. */
  return arch == AST_TARGET_ARCH_AARCH64
             ? r >= 19u && r <= 30u
             : r == 1u || r == 8u || r == 9u || (r >= 18u && r <= 27u);
}
static int native_slot_overlap(int64_t a, uint32_t as, int64_t b, uint32_t bs) {
  return a < b + (int64_t)bs && b < a + (int64_t)as;
}
/* A 1 MiB bound on outstanding allocation lets entry-relative arithmetic
   stay inside int64_t. Each emitted store adds at most one ledger entry. */
static int native_stack_memory(const AstRegAllocProgram *p,
                               const AstPhysicalInstructionInfo *pi,
                               uint64_t depth, ManualNativeSlot *slots,
                               uint32_t *used, uint32_t cap,
                               uint32_t *link_pc) {
  const RvMemoryForm *rv = rv_memory_form(pi->opcode);
  const VectorForm *vf = vector_physical_form(pi);
  int load = pi->opcode == AST_MACHINE_A64_LOAD || (rv && rv->load) ||
             (vf && vf->shape == VECTOR_A64_LOAD);
  const AstPhysicalOperand *r = &pi->operands[0], *m = &pi->operands[1];
  uint32_t bytes = m->bit_width / 8u;
  uint32_t unit = r->register_unit;
  int64_t off = m->displacement;
  if (off < 0 || (uint64_t)off > depth || bytes > depth - (uint64_t)off)
    return 0;
  int64_t addr = off - (int64_t)depth;
  if (load) {
    uint32_t index = *used;
    /* Reject loads from uninitialized, partially overwritten, expired or
       mismatched-width slots rather than guessing byte provenance. */
    for (uint32_t n = 0u; n < *used; ++n)
      if (slots[n].live && slots[n].address == addr && slots[n].bytes == bytes)
        index = n;
    if (index == *used)
      return 0;
    if (native_preserved_register(p->target.arch, unit)) {
      if (bytes < 8u || !slots[index].pinned || slots[index].saved_unit != unit)
        return 0;
      slots[index].pinned = 0u;
      if (unit % 64u == (p->target.arch == AST_TARGET_ARCH_AARCH64 ? 30u : 1u))
        *link_pc = slots[index].link_pc;
    }
    return 1;
  }
  /* Narrow stores of a preserved register are ordinary data writes. Only
     a full 64-bit store can establish a restoration obligation/proof. */
  int preserved =
      bytes >= 8u && native_preserved_register(p->target.arch, unit);
  for (uint32_t n = 0u; n < *used; ++n)
    if (slots[n].live) {
      if (slots[n].pinned &&
          (native_slot_overlap(addr, bytes, slots[n].address, slots[n].bytes) ||
           (preserved && slots[n].saved_unit == unit &&
            unit % 64u !=
                (p->target.arch == AST_TARGET_ARCH_AARCH64 ? 30u : 1u))))
        return 0;
    }
  if (*used >= cap)
    return 0;
  for (uint32_t n = 0u; n < *used; ++n)
    if (slots[n].live &&
        native_slot_overlap(addr, bytes, slots[n].address, slots[n].bytes))
      slots[n].live = 0u;
  ManualNativeSlot *entry = &slots[(*used)++];
  entry->address = addr;
  entry->bytes = bytes;
  entry->saved_unit = unit;
  entry->live = 1u;
  entry->pinned = (uint8_t)preserved;
  entry->link_pc =
      preserved && unit % 64u ==
                       (p->target.arch == AST_TARGET_ARCH_AARCH64 ? 30u : 1u)
          ? *link_pc
          : 0u;
  return 1;
}
static int native_saved_for_write(AstTargetArch arch,
                                  const ManualNativeSlot *slots, uint32_t used,
                                  uint32_t unit) {
  if (!native_preserved_register(arch, unit))
    return 1;
  for (uint32_t n = 0u; n < used; ++n)
    if (slots[n].live && slots[n].pinned && slots[n].saved_unit == unit)
      return 1;
  return 0;
}
static int native_stack_dealloc(ManualNativeSlot *slots, uint32_t used,
                                uint64_t depth) {
  for (uint32_t n = 0u; n < used; ++n)
    if (slots[n].live && (slots[n].address < -(int64_t)depth ||
                          slots[n].address + (int64_t)slots[n].bytes > 0)) {
      if (slots[n].pinned)
        return 0;
      slots[n].live = 0u;
    }
  return 1;
}

/* The state is indexed by (machine-PC, exact bounded call string).  Each
   call may create a separate state for the *same* leaf instruction.  This
   fixes Slice 13's incorrect conflation of distinct continuations.
   Depth <= 3, recursion denied; no heuristic widening is used. */
#define MANUAL_CALL_DEPTH 3u
#define MANUAL_FLOW_VISITS 16384u
#define MANUAL_FLOW_BUCKETS 32768u

typedef struct ManualCallFrame {
  uint32_t call_pc;
  uint32_t return_pc;
  uint32_t callee_pc;
  uint64_t entry_depth;
} ManualCallFrame;
typedef struct ManualFlowState {
  uint64_t depth;
  uint32_t count;
  uint32_t link_pc;
  uint32_t known_gprs; /* definite-value mask; volatile killed by ABI calls */
  uint16_t prefix_unit, prefix_bits, prefix_predicate;
  uint8_t sme_state; /* bit 0: PSTATE.SM; bit 1: PSTATE.ZA */
  uint8_t call_depth;
  ManualCallFrame calls[MANUAL_CALL_DEPTH];
  ManualNativeSlot slots[];
} ManualFlowState;

static ManualFlowState *manual_flow_clone(const ManualFlowState *src,
                                          uint32_t extra) {
  if (src->count > UINT32_MAX - extra)
    return NULL;
  size_t bytes =
      sizeof(*src) + (size_t)(src->count + extra) * sizeof(*src->slots);
  ManualFlowState *copy = (ManualFlowState *)calloc(1u, bytes);
  if (!copy)
    return NULL;
  copy->depth = src->depth;
  copy->count = src->count;
  copy->link_pc = src->link_pc;
  copy->known_gprs = src->known_gprs;
  copy->prefix_unit = src->prefix_unit;
  copy->prefix_bits = src->prefix_bits;
  copy->prefix_predicate = src->prefix_predicate;
  copy->sme_state = src->sme_state;
  copy->call_depth = src->call_depth;
  memcpy(copy->calls, src->calls, sizeof(copy->calls));
  if (src->count)
    memcpy(copy->slots, src->slots, (size_t)src->count * sizeof(*src->slots));
  return copy;
}

static int manual_flow_slot_compare(const void *lhs, const void *rhs) {
  const ManualNativeSlot *a = (const ManualNativeSlot *)lhs;
  const ManualNativeSlot *b = (const ManualNativeSlot *)rhs;
  if (a->address != b->address)
    return a->address < b->address ? -1 : 1;
  if (a->bytes != b->bytes)
    return a->bytes < b->bytes ? -1 : 1;
  if (a->saved_unit != b->saved_unit)
    return a->saved_unit < b->saved_unit ? -1 : 1;
  if (a->pinned != b->pinned)
    return a->pinned < b->pinned ? -1 : 1;
  if (a->link_pc != b->link_pc)
    return a->link_pc < b->link_pc ? -1 : 1;
  return 0;
}
/* Drop expired entries and make state comparisons independent of the order
   in which disjoint slots were first written on alternative CFG paths. */
static void manual_flow_canonicalize(ManualFlowState *s) {
  uint32_t n = 0u;
  for (uint32_t j = 0; j < s->count; ++j)
    if (s->slots[j].live)
      s->slots[n++] = s->slots[j];
  s->count = n;
  if (n > 1u)
    qsort(s->slots, n, sizeof(*s->slots), manual_flow_slot_compare);
}
static int manual_flow_same_slots(const ManualFlowState *a,
                                  const ManualFlowState *b) {
  if (a->depth != b->depth || a->count != b->count)
    return 0;
  for (uint32_t k = 0; k < a->count; ++k) {
    const ManualNativeSlot *x = &a->slots[k], *y = &b->slots[k];
    if (x->address != y->address || x->bytes != y->bytes ||
        x->saved_unit != y->saved_unit || x->pinned != y->pinned ||
        x->link_pc != y->link_pc)
      return 0;
  }
  return 1;
}
static int manual_flow_same_context(const ManualFlowState *a,
                                    const ManualFlowState *b) {
  if (a->call_depth != b->call_depth)
    return 0;
  for (uint32_t k = 0u; k < a->call_depth; ++k)
    if (a->calls[k].call_pc != b->calls[k].call_pc ||
        a->calls[k].return_pc != b->calls[k].return_pc ||
        a->calls[k].callee_pc != b->calls[k].callee_pc ||
        a->calls[k].entry_depth != b->calls[k].entry_depth)
      return 0;
  return 1;
}
static int manual_flow_equal(const ManualFlowState *a,
                             const ManualFlowState *b) {
  return a->link_pc == b->link_pc && a->known_gprs == b->known_gprs &&
         a->prefix_unit == b->prefix_unit && a->prefix_bits == b->prefix_bits &&
         a->prefix_predicate == b->prefix_predicate &&
         a->sme_state == b->sme_state && manual_flow_same_context(a, b) &&
         manual_flow_same_slots(a, b);
}

static uint32_t manual_abi_volatile_gprs(AstTargetArch arch) {
  if (arch == AST_TARGET_ARCH_AARCH64)
    return UINT32_C(0x0003ffff); /* X0..X17 */
  return (UINT32_C(7) << 5u) | (UINT32_C(255) << 10u) |
         (UINT32_C(15) << 28u); /* t0-t2, a0-a7, t3-t6 */
}
static int manual_reg_known(AstTargetArch arch, const AstPhysicalOperand *op,
                            uint32_t known) {
  if ((op->kind != AST_PHYSICAL_OPERAND_REGISTER &&
       op->kind != AST_PHYSICAL_OPERAND_MEMORY) ||
      op->register_unit / 64u != MACHINE_PHYSREG_FILE_GPR)
    return 1;
  uint32_t reg = op->register_unit % 64u;
  /* A64 register 31 is XZR/SP, but RV64 register 31 is caller-saved t6.
     RV64 x0 is the immutable zero register instead. */
  return (arch == AST_TARGET_ARCH_AARCH64 ? reg == 31u : reg == 0u) ||
         (known & (UINT32_C(1) << reg)) != 0u;
}
static void manual_reg_defined(const AstPhysicalOperand *op, uint32_t *known) {
  if (op->kind == AST_PHYSICAL_OPERAND_REGISTER &&
      op->register_unit / 64u == MACHINE_PHYSREG_FILE_GPR &&
      op->register_unit % 64u < 32u)
    *known |= UINT32_C(1) << (op->register_unit % 64u);
}
static int scalable_flow_step(const AstRegAllocProgram *p,
                              const AstPhysicalInstructionInfo *pi,
                              ManualFlowState *s) {
  const A64ScalableForm *f = a64_scalable_form(pi);
  if (s->prefix_unit) {
    if (!f || !(f->flags & A64_SC_PREFIX_COMPAT) || !f->results ||
        pi->operands[0].register_unit != s->prefix_unit)
      return 0;
    uint32_t predicate = 0u;
    for (uint32_t j = f->results; j < f->parameter_count; ++j) {
      const A64ScalableParameter *param =
          &a64_scalable_parameters[f->parameter_offset + j];
      const A64ScalableType *t = &a64_scalable_types[param->type];
      if (t->kind == A64_SC_P)
        predicate = pi->operands[j].register_unit;
      if (t->kind == A64_SC_Z && param->tied < 0 &&
          pi->operands[j].register_unit == s->prefix_unit)
        return 0;
    }
    if (s->prefix_bits &&
        (!(f->flags & A64_SC_MERGING) || s->prefix_bits != f->prefix_bits ||
         predicate != s->prefix_predicate))
      return 0;
    s->prefix_unit = 0u;
    s->prefix_bits = 0u;
    s->prefix_predicate = 0u;
  }
  if (!f) {
    if ((s->sme_state & 1u) && pi->opcode == AST_MACHINE_A64_VECTOR &&
        !(p->target.features & AST_TARGET_FEATURE_A64_SME_FA64))
      return 0;
    return 1;
  }
  if (f->flags & (A64_SC_START | A64_SC_STOP)) {
    uint8_t mask = (uint8_t)pi->operands[0].immediate_bits;
    uint8_t next = (uint8_t)(f->flags & A64_SC_START ? s->sme_state | mask
                                                     : s->sme_state & ~mask);
    if ((next ^ s->sme_state) & 1u) {
      if (p->abi == AST_ABI_AAPCS64_SVE)
        return 0;
      /* An SM transition destroys all Z/P views, including the D8..D15
         values protected by the base AAPCS64 calling convention. */
      for (uint32_t unit = 136u; unit < 144u; ++unit)
        if (!native_saved_for_write(p->target.arch, s->slots, s->count, unit))
          return 0;
    }
    s->sme_state = next;
    return 1;
  }
  if ((f->flags & A64_SC_ZA_ACCESS) && !(s->sme_state & 2u))
    return 0;
  if ((f->flags & A64_SC_STREAMING) && !(s->sme_state & 1u))
    return 0;
  if ((s->sme_state & 1u) && (f->flags & A64_SC_NONSTREAMING) &&
      !(p->target.features & AST_TARGET_FEATURE_A64_SME_FA64))
    return 0;
  if (!(s->sme_state & 1u) &&
      !(p->target.features & AST_TARGET_FEATURE_A64_SVE) &&
      (f->required_any &
       (AST_TARGET_FEATURE_A64_SVE | AST_TARGET_FEATURE_A64_SVE2)))
    return 0;
  for (uint32_t j = 0; j < f->parameter_count; ++j) {
    const AstPhysicalOperand *o = &pi->operands[j];
    const A64ScalableParameter *param =
        &a64_scalable_parameters[f->parameter_offset + j];
    const A64ScalableType *t = &a64_scalable_types[param->type];
    if (!(s->sme_state & 1u) &&
        !(p->target.features & AST_TARGET_FEATURE_A64_SVE) &&
        (t->kind == A64_SC_Z || t->kind == A64_SC_P))
      return 0;
    if (j < f->results && (o->register_unit == 320u ||
                           o->register_unit == 82u || o->register_unit == 94u))
      return 0;
    if ((f->flags & (A64_SC_LOAD | A64_SC_STORE)) && o->register_unit == 320u &&
        o->kind == AST_PHYSICAL_OPERAND_REGISTER)
      return 0;
    if (j < f->results && o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
      if (p->abi == AST_ABI_AAPCS64_SVE &&
          ((t->kind == A64_SC_Z && o->register_unit >= 136u &&
            o->register_unit < 152u) ||
           (t->kind == A64_SC_P && o->register_unit >= 260u)))
        return 0;
      for (uint32_t n = 0; n < t->group; ++n) {
        uint32_t unit = o->register_unit;
        if (t->kind == A64_SC_Z)
          unit = 128u + (unit - 128u + n) % 32u;
        if (p->abi == AST_ABI_AAPCS64_SVE && t->kind == A64_SC_Z &&
            unit >= 136u && unit < 152u)
          return 0;
        if (!native_saved_for_write(p->target.arch, s->slots, s->count, unit))
          return 0;
      }
    }
  }
  if (f->flags & A64_SC_PREFIX) {
    s->prefix_unit = (uint16_t)pi->operands[0].register_unit;
    s->prefix_bits = f->prefix_bits;
    for (uint32_t j = f->results; j < f->parameter_count; ++j)
      if (pi->operands[j].bank == AST_MACHINE_BANK_MASK)
        s->prefix_predicate = (uint16_t)pi->operands[j].register_unit;
  }
  return 1;
}
static int vector_stack_access(const AstPhysicalInstructionInfo *pi) {
  const VectorForm *f = vector_physical_form(pi);
  return f && (f->shape == VECTOR_A64_LOAD || f->shape == VECTOR_A64_STORE) &&
         pi->operands[1].register_unit == 320u;
}
static int manual_flow_step(const AstRegAllocProgram *p,
                            const AstPhysicalInstructionInfo *pi,
                            ManualFlowState *s, uint32_t capacity) {
  const uint64_t limit = UINT64_C(1048576);
  if (!scalable_flow_step(p, pi, s))
    return 0;
  /* Explicit native reads after a call must not reuse an ABI-clobbered GPR
     until the register is freshly defined or reloaded from an initialized
     stack slot. Argument/result ABI registers are not callee-saved. */
  const RvMemoryForm *memory = rv_memory_form(pi->opcode);
  if (pi->opcode == AST_MACHINE_A64_STORE || (memory && !memory->load)) {
    if (!manual_reg_known(p->target.arch, &pi->operands[0], s->known_gprs))
      return 0;
    if (memory &&
        !manual_reg_known(p->target.arch, &pi->operands[1], s->known_gprs))
      return 0;
  } else {
    uint32_t begin = pi->result_count;
    const A64ScalarForm *scalar = a64_scalar_form(pi);
    if (scalar && (scalar->effects & A64_SCALAR_READ_DST))
      begin = 0u;
    for (uint32_t j = begin; j < pi->operand_count; ++j) {
      if (!manual_reg_known(p->target.arch, &pi->operands[j], s->known_gprs))
        return 0;
    }
  }
  if (pi->result_count && manual_physical_sp(p, &pi->operands[0])) {
    int64_t delta;
    if (p->target.arch == AST_TARGET_ARCH_AARCH64) {
      delta = (int64_t)pi->operands[2].immediate_bits;
      if (pi->opcode == AST_MACHINE_A64_SUB)
        delta = -delta;
    } else
      delta = (int64_t)pi->operands[2].immediate_bits;
    if (delta < 0) {
      uint64_t allocation = UINT64_C(0) - (uint64_t)delta;
      if (allocation > limit - s->depth)
        return 0;
      s->depth += allocation;
    } else {
      if ((uint64_t)delta > s->depth)
        return 0;
      s->depth -= (uint64_t)delta;
      if (!native_stack_dealloc(s->slots, s->count, s->depth))
        return 0;
    }
  } else if (pi->opcode == AST_MACHINE_A64_STORE ||
             pi->opcode == AST_MACHINE_A64_LOAD ||
             (memory && pi->operands[1].register_unit == 66u) ||
             vector_stack_access(pi)) {
    if (!native_stack_memory(p, pi, s->depth, s->slots, &s->count, capacity,
                             &s->link_pc))
      return 0;
  } else if (pi->result_count &&
             !native_saved_for_write(p->target.arch, s->slots, s->count,
                                     pi->operands[0].register_unit))
    return 0;
  if (pi->result_count && !manual_physical_sp(p, &pi->operands[0]))
    manual_reg_defined(&pi->operands[0], &s->known_gprs);
  manual_flow_canonicalize(s);
  return 1;
}
typedef struct ManualVisit {
  uint32_t pc;
  uint32_t next; /* bucket index+1, zero is empty */
  ManualFlowState *entry;
} ManualVisit;
typedef struct ManualFlowWork {
  ManualVisit *visits;
  uint32_t *buckets;
  uint32_t *queue;
  uint32_t used, head, tail, code_count;
  uint64_t total_slots;
} ManualFlowWork;

static uint32_t manual_flow_hash(uint32_t pc, const ManualFlowState *state) {
  uint32_t h = (UINT32_C(2166136261) ^ pc) * UINT32_C(16777619);
  h = (h ^ state->call_depth) * UINT32_C(16777619);
  for (uint32_t i = 0u; i < state->call_depth; ++i) {
    h = (h ^ state->calls[i].call_pc) * UINT32_C(16777619);
    h = (h ^ state->calls[i].return_pc) * UINT32_C(16777619);
    h = (h ^ state->calls[i].callee_pc) * UINT32_C(16777619);
  }
  return h & (MANUAL_FLOW_BUCKETS - 1u);
}
static const ManualFlowState *manual_flow_find(const ManualFlowWork *w,
                                               uint32_t pc,
                                               const ManualFlowState *state) {
  uint32_t bucket = manual_flow_hash(pc, state);
  for (uint32_t n = w->buckets[bucket]; n; n = w->visits[n - 1u].next) {
    const ManualVisit *v = &w->visits[n - 1u];
    if (v->pc == pc && manual_flow_same_context(v->entry, state))
      return v->entry;
  }
  return NULL;
}
static int manual_flow_enqueue(ManualFlowWork *w, uint32_t pc,
                               const ManualFlowState *state) {
  if (pc >= w->code_count)
    return 0;
  const ManualFlowState *old = manual_flow_find(w, pc, state);
  if (old)
    return manual_flow_equal(old, state);
  if (w->used == MANUAL_FLOW_VISITS ||
      state->count > UINT64_C(262144) - w->total_slots)
    return 0;
  ManualFlowState *copy = manual_flow_clone(state, 0u);
  if (!copy)
    return 0;
  uint32_t bucket = manual_flow_hash(pc, state);
  uint32_t index = w->used++;
  w->visits[index].pc = pc;
  w->visits[index].entry = copy;
  w->visits[index].next = w->buckets[bucket];
  w->buckets[bucket] = index + 1u;
  w->queue[w->tail++] = index;
  w->total_slots += state->count;
  return 1;
}
static int manual_find_byte_pc(const uint32_t *offsets, uint32_t count,
                               int64_t pc) {
  if (pc < 0 || pc >= (int64_t)offsets[count])
    return -1;
  uint32_t low = 0u, high = count;
  while (low < high) {
    uint32_t mid = low + (high - low) / 2u;
    if ((int64_t)offsets[mid] < pc)
      low = mid + 1u;
    else
      high = mid;
  }
  return low < count && (int64_t)offsets[low] == pc ? (int)low : -1;
}
static int native_indirect_tail(const AstPhysicalInstructionInfo *i) {
  return i->opcode == AST_MACHINE_RISCV_NATIVE_JALR && i->operand_count == 3u &&
         i->operands[0].register_unit == 64u;
}
static int manual_flow_verify(const AstRegAllocProgram *p,
                              const PhysicalBlock *block) {
  /* Independent machine layout: physical opcode sizes, not source tokens.
     This catches a missing word and any branch into the JALR half of a
     paired RV64 external call before the encoder writes output. */
  uint32_t *offsets =
      (uint32_t *)calloc((size_t)block->count + 1u, sizeof(*offsets));
  if (!offsets)
    return 0;
  for (uint32_t i = 0; i < block->count; ++i) {
    uint32_t width =
        p->target.arch == AST_TARGET_ARCH_RISCV64 &&
                block->instructions[i].opcode == AST_MACHINE_RISCV_CALL
            ? 8u
            : 4u;
    if (offsets[i] > UINT32_MAX - width) {
      free(offsets);
      return 0;
    }
    offsets[i + 1u] = offsets[i] + width;
  }
  ManualFlowWork w = {0};
  w.code_count = block->count;
  w.visits = (ManualVisit *)calloc(MANUAL_FLOW_VISITS, sizeof(*w.visits));
  w.queue = (uint32_t *)calloc(MANUAL_FLOW_VISITS, sizeof(*w.queue));
  w.buckets = (uint32_t *)calloc(MANUAL_FLOW_BUCKETS, sizeof(*w.buckets));
  if (!w.visits || !w.queue || !w.buckets) {
    free(w.visits);
    free(w.queue);
    free(w.buckets);
    free(offsets);
    return 0;
  }
  ManualFlowState initial = {0};
  initial.link_pc = UINT32_MAX; /* incoming link is an external continuation */
  initial.known_gprs =
      UINT32_MAX; /* baseline exact assembler: all inputs defined
                    until a call invalidates ABI-volatile GPRs */
  int ok = manual_flow_enqueue(&w, 0u, &initial);
  uint32_t returns = 0u;
  uint8_t *reachable = (uint8_t *)calloc(block->count, 1u);
  if (!reachable)
    ok = 0;
  while (w.head < w.tail && ok) {
    const ManualVisit *visit = &w.visits[w.queue[w.head++]];
    uint32_t pc = visit->pc;
    const ManualFlowState *at = visit->entry;
    reachable[pc] = 1u;
    const AstPhysicalInstructionInfo *pi = &block->instructions[pc];
    int indirect = pi->opcode == AST_MACHINE_RISCV_NATIVE_JALR;
    if (indirect &&
        !manual_reg_known(p->target.arch, &pi->operands[1], at->known_gprs)) {
      ok = 0;
      break;
    }
    int ret = pi->opcode == AST_MACHINE_A64_RET ||
              pi->opcode == AST_MACHINE_RISCV_RET || native_indirect_tail(pi);
    if (system_exception_return(pi)) {
      /* Exception state/target is supplied by the execution environment.
         The explicit frame must still be balanced. This is not a local
         ABI callee return and cannot consume a tracked call context. */
      if (at->call_depth || at->depth || at->count || at->link_pc != UINT32_MAX)
        ok = 0;
      else
        ++returns;
      continue;
    }
    int jump =
        pi->opcode == AST_MACHINE_A64_BR || pi->opcode == AST_MACHINE_RISCV_J;
    int external_call = pi->opcode == AST_MACHINE_A64_CALL ||
                        pi->opcode == AST_MACHINE_RISCV_CALL || indirect;
    int call = external_call || pi->opcode == AST_MACHINE_A64_NATIVE_BL ||
               pi->opcode == AST_MACHINE_RISCV_NATIVE_JAL;
    int conditional = pi->opcode == AST_MACHINE_A64_NATIVE_CBZ ||
                      pi->opcode == AST_MACHINE_A64_NATIVE_CBNZ ||
                      rv_native_branch_funct3(pi->opcode) >= 0;
    if (ret) {
      if (at->sme_state || at->prefix_unit) {
        ok = 0;
        break;
      }
      if (at->call_depth) {
        const ManualCallFrame frame = at->calls[at->call_depth - 1u];
        /* The architectural RET consumes the current LR/RA. Verify
           provenance: a nested call must restore its own link. */
        if (at->link_pc != frame.return_pc) {
          ok = 0;
          break;
        }
        ManualFlowState *out = manual_flow_clone(at, 0u);
        if (!out) {
          ok = 0;
          break;
        }
        --out->call_depth;
        out->link_pc = frame.return_pc;
        /* The callee may destroy all ABI caller-saved GPRs.  The
           exact function's one-register u64 result is defined in
           X0/A0; other volatile registers become unknown. */
        out->known_gprs &= ~manual_abi_volatile_gprs(p->target.arch);
        out->known_gprs |= UINT32_C(1)
                           << (p->target.arch == AST_TARGET_ARCH_AARCH64 ? 0u
                                                                         : 10u);
        const ManualFlowState *caller =
            manual_flow_find(&w, frame.call_pc, out);
        if (!caller || !manual_flow_same_slots(caller, out) ||
            !manual_flow_enqueue(&w, frame.return_pc, out))
          ok = 0;
        free(out);
      } else {
        if (at->depth || at->count || at->link_pc != UINT32_MAX)
          ok = 0;
        else
          ++returns;
      }
      continue;
    }
    ManualFlowState *state = manual_flow_clone(at, 1u);
    if (!state) {
      ok = 0;
      break;
    }
    if (call) {
      uint32_t link_reg =
          MACHINE_PHYSREG_FILE_GPR * 64u +
          (p->target.arch == AST_TARGET_ARCH_AARCH64 ? 30u : 1u);
      int saved = 0;
      uint64_t entered_depth =
          state->call_depth ? state->calls[state->call_depth - 1u].entry_depth
                            : 0u;
      uint32_t protected_link =
          state->call_depth ? state->calls[state->call_depth - 1u].return_pc
                            : UINT32_MAX;
      if ((external_call && state->call_depth) ||
          (!external_call && state->call_depth >= MANUAL_CALL_DEPTH) ||
          pc + 1u >= block->count || state->depth % 16u)
        ok = 0;
      for (uint32_t i = 0u; ok && i < state->count; ++i) {
        const ManualNativeSlot *slot = &state->slots[i];
        if (slot->live && slot->pinned && slot->saved_unit == link_reg &&
            slot->address < -(int64_t)entered_depth &&
            slot->link_pc == protected_link)
          saved = 1;
      }
      if (!saved)
        ok = 0;
    } else {
      /* A local callee owns only stack bytes below its entry SP.
         Until stack-passed arguments are modeled, touching the
         caller's frame cannot be proven safe.  Likewise, a callee
         may not use a *caller's* pinned register slot to justify
         modifying its own callee-saved register. */
      if (at->call_depth) {
        uint64_t boundary = at->calls[at->call_depth - 1u].entry_depth;
        int memory = pi->opcode == AST_MACHINE_A64_STORE ||
                     pi->opcode == AST_MACHINE_A64_LOAD ||
                     (rv_memory_form(pi->opcode) &&
                      pi->operands[1].register_unit == 66u);
        if (memory) {
          int64_t addr = pi->operands[1].displacement - (int64_t)state->depth;
          if (addr >= -(int64_t)boundary)
            ok = 0;
        }
        if (pi->result_count && !manual_physical_sp(p, &pi->operands[0]) &&
            native_preserved_register(p->target.arch,
                                      pi->operands[0].register_unit)) {
          int local_save = 0;
          for (uint32_t i = 0u; i < state->count; ++i) {
            const ManualNativeSlot *slot = &state->slots[i];
            if (slot->live && slot->pinned &&
                slot->address < -(int64_t)boundary &&
                slot->saved_unit == pi->operands[0].register_unit)
              local_save = 1;
          }
          if (!local_save)
            ok = 0;
        }
      }
      if (ok && !manual_flow_step(p, pi, state, at->count + 1u))
        ok = 0;
      if (ok && at->call_depth &&
          state->depth < at->calls[at->call_depth - 1u].entry_depth)
        ok = 0;
    }
    if (ok && external_call) {
      /* External conforming callees may clobber every volatile GPR.
         The declared exact u64 return value is defined in X0/A0.
         Link register is overwritten with the next native address;
         caller must restore its pinned incoming LR/RA before RET. */
      state->known_gprs &= ~manual_abi_volatile_gprs(p->target.arch);
      state->known_gprs |=
          UINT32_C(1) << (p->target.arch == AST_TARGET_ARCH_AARCH64 ? 0u : 10u);
      state->link_pc = pc + 1u;
    }
    int64_t target = -1;
    if (ok && (jump || conditional || (call && !external_call))) {
      const AstPhysicalOperand *operand = &pi->operands[pi->operand_count - 1u];
      uint64_t raw = operand->immediate_bits;
      int64_t bytes = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
      if (bytes % 4u)
        ok = 0;
      else {
        int64_t address = (int64_t)offsets[pc] + bytes;
        target = manual_find_byte_pc(offsets, block->count, address);
        if (target < 0)
          ok = 0;
      }
    }
    if (ok && call && !external_call) {
      for (uint32_t i = 0u; i < state->call_depth; ++i)
        if (state->calls[i].callee_pc == (uint32_t)target)
          ok = 0;
      if (ok) {
        ManualCallFrame *frame = &state->calls[state->call_depth++];
        frame->call_pc = pc;
        frame->callee_pc = (uint32_t)target;
        frame->return_pc = pc + 1u;
        frame->entry_depth = state->depth;
        state->link_pc = pc + 1u;
      }
    }
    if (ok && (jump || conditional || (call && !external_call)))
      if (!manual_flow_enqueue(&w, (uint32_t)target, state))
        ok = 0;
    if (ok && !jump && (!call || external_call))
      if (!manual_flow_enqueue(&w, pc + 1u, state))
        ok = 0;
    free(state);
  }
  if (!returns)
    ok = 0;
  /* Dead native instructions must still be source-verified (done above).
     Do not permit an unreachable call or unproved SP/memory/save effect. */
  for (uint32_t k = 0u; k < block->count && ok; ++k)
    if (!reachable[k]) {
      const AstPhysicalInstructionInfo *pi = &block->instructions[k];
      if (pi->opcode == AST_MACHINE_A64_LOAD ||
          pi->opcode == AST_MACHINE_A64_STORE ||
          pi->opcode == AST_MACHINE_RISCV_LD ||
          pi->opcode == AST_MACHINE_RISCV_SD ||
          pi->opcode == AST_MACHINE_A64_NATIVE_BL ||
          pi->opcode == AST_MACHINE_RISCV_NATIVE_JAL ||
          pi->opcode == AST_MACHINE_A64_CALL ||
          pi->opcode == AST_MACHINE_RISCV_CALL ||
          (pi->result_count &&
           (manual_physical_sp(p, &pi->operands[0]) ||
            native_preserved_register(p->target.arch,
                                      pi->operands[0].register_unit))))
        ok = 0;
    }
  for (uint32_t i = 0u; i < w.used; ++i)
    free(w.visits[i].entry);
  free(reachable);
  free(w.visits);
  free(w.queue);
  free(w.buckets);
  free(offsets);
  return ok;
}

int physical_verify_native_manual_other(const AstRegAllocProgram *p,
                                        uint32_t f) {
  if (p->target.arch != AST_TARGET_ARCH_AARCH64 &&
      p->target.arch != AST_TARGET_ARCH_RISCV64)
    return 0;
  const AllocationFunction *af = &p->functions[f];
  const MachineFunction *mf = &p->machine->functions[f];
  const AstMirFunction *mir = ast_mir_function_at(p->machine->mir, f);
  const AstHirFunction *hir = ast_mir_function_source_hir(mir);
  AstMirFunctionInfo function;
  if (!mir || !hir || !ast_mir_function_info(mir, &function) ||
      !((function.flags & AST_HIR_FUNCTION_NAKED) ||
        function.frame_policy == AST_HIR_FRAME_NAKED ||
        function.frame_policy == AST_HIR_FRAME_VALIDATE) ||
      af->block_count != 1u || !af->blocks || !af->blocks[0].instructions ||
      af->object_count || function.stack_slot_count ||
      af->frame.stack_adjustment || af->frame.alignment != 16u ||
      mf->info.block_count != 1u)
    return 0;
  const PhysicalBlock *block = &af->blocks[0];
  const MachineBlock *source_block = &mf->blocks[0];
  uint32_t cursor = 0u;
  int terminal = 0;
  if (!block->count || block->count > 65536u)
    return 0;
  int vectors = 0, calls = 0;
  int scalable = 0, writes_svcr = 0;
  for (uint32_t j = 0; j < block->count; ++j) {
    AstMachineOpcode op = block->instructions[j].opcode;
    vectors |= op == AST_MACHINE_A64_VECTOR || op == AST_MACHINE_RISCV_VECTOR ||
               op == AST_MACHINE_A64_SCALABLE;
    scalable |= op == AST_MACHINE_A64_SCALABLE;
    const SystemForm *system = system_physical_form(&block->instructions[j]);
    writes_svcr |=
        system && system->shape == SYSTEM_SHAPE_A64_WRITE_SYSREG &&
        block->instructions[j].operands[1].immediate_bits == UINT64_C(0xda12);
    calls |= op == AST_MACHINE_A64_CALL || op == AST_MACHINE_RISCV_CALL ||
             op == AST_MACHINE_A64_NATIVE_BL ||
             op == AST_MACHINE_RISCV_NATIVE_JAL ||
             op == AST_MACHINE_RISCV_NATIVE_JALR;
  }
  /* The existing local-call proof tracks scalar provenance. Vector state
     across calls needs its own width/group-aware proof before admission. */
  if (vectors && calls)
    return 0;
  /* Unknown SVCR register writes cannot participate in an SM/ZA proof.
     The explicit SMSTART/SMSTOP operands describe those transitions. */
  if (scalable && writes_svcr)
    return 0;
  /* Verify all instructions against immutable HIR, including unreachable
     instructions. Semantic state is checked separately, along CFG edges. */
  for (uint32_t j = 0; j < source_block->info.instruction_count; ++j) {
    uint32_t id = source_block->instructions[j];
    const MachineInstruction *mi = &mf->instructions[id];
    if (mi->info.source_op != AST_MIR_OP_NATIVE) {
      if (!terminal || j + 1u != source_block->info.instruction_count ||
          (mi->info.opcode != AST_MACHINE_A64_TRAP &&
           mi->info.opcode != AST_MACHINE_RISCV_EBREAK &&
           mi->info.opcode != AST_MACHINE_A64_RET &&
           mi->info.opcode != AST_MACHINE_RISCV_RET))
        return 0;
      continue;
    }
    AstMirInstructionInfo src;
    AstHirNativeRegionInfo region_info;
    if (terminal ||
        !ast_mir_function_instruction(mir, mi->info.source_instruction, &src))
      return 0;
    const AstHirNativeRegion *region =
        ast_hir_function_native_region(hir, src.native_region);
    if (!region || !ast_hir_native_region_info(region, &region_info) ||
        region_info.statement_count > 4096u || region_info.input_count ||
        region_info.output_count)
      return 0;
    for (uint32_t k = 0; k < region_info.statement_count; ++k) {
      AstHirNativeStmtInfo stmt;
      if (!ast_hir_native_region_statement(region, k, &stmt))
        return 0;
      if (mnemonic(stmt.mnemonic, "label")) {
        AstName name;
        if (!verify_label_name(&stmt, &name))
          return 0;
        for (uint32_t t = 0; t < k; ++t) {
          AstHirNativeStmtInfo previous;
          AstName old;
          if (!ast_hir_native_region_statement(region, t, &previous))
            return 0;
          if (mnemonic(previous.mnemonic, "label") &&
              (!verify_label_name(&previous, &old) ||
               verify_name_same(name, old)))
            return 0;
        }
        continue;
      }
      if (cursor >= block->count)
        return 0;
      const AstPhysicalInstructionInfo *pi = &block->instructions[cursor++];
      if (pi->source_instruction != id || !same_span(pi->span, stmt.span) ||
          !manual_other_statement(p, region, k, &stmt, pi))
        return 0;
      if ((pi->opcode == AST_MACHINE_A64_RET ||
           pi->opcode == AST_MACHINE_RISCV_RET || native_indirect_tail(pi) ||
           system_exception_return(pi)) &&
          k + 1u == region_info.statement_count)
        terminal = 1;
    }
  }
  return terminal && cursor == block->count &&
         (block->instructions[block->count - 1u].opcode ==
              (p->target.arch == AST_TARGET_ARCH_AARCH64
                   ? AST_MACHINE_A64_RET
                   : AST_MACHINE_RISCV_RET) ||
          native_indirect_tail(&block->instructions[block->count - 1u]) ||
          system_exception_return(&block->instructions[block->count - 1u])) &&
         manual_flow_verify(p, block);
}
