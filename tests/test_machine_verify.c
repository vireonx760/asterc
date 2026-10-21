#include "asterasm/asterasm.h"
#include "machine_internal.h"
#include <stdio.h>

typedef struct Fixture {
  AstFrontend *frontend;
  AstSema *sema;
  AstHirProgram *hir;
  AstMirProgram *mir;
  AstTargetProgram *target;
  AstAbiProgram *abi;
  AstMachineProgram *machine;
} Fixture;

static void close_fixture(Fixture *f) {
  ast_machine_destroy(f->machine);
  ast_abi_destroy(f->abi);
  ast_target_destroy(f->target);
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
}

static int open_fixture(Fixture *f, int arch, int stage) {
  const char *root = ASTERASM_SOURCE_DIR "/tests/regalloc_cases/core";
  const char *entry = ASTERASM_SOURCE_DIR "/tests/regalloc_cases/core/main.aa";
  const AstTargetArch arches[] = {
      AST_TARGET_ARCH_X86_64, AST_TARGET_ARCH_AARCH64, AST_TARGET_ARCH_RISCV64};
  const AstAbiKind abis[] = {AST_ABI_SYSV_AMD64, AST_ABI_AAPCS64,
                             AST_ABI_RISCV_LP64D};
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) ||
      ast_frontend_load(f->frontend, &entry, 1u))
    return 0;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema))
    return 0;
  f->hir = ast_hir_build(f->frontend, f->sema, NULL);
  AstMirConfig mc = ast_mir_config_default();
  mc.promote_locals = 0u;
  f->mir = f->hir ? ast_mir_build(f->frontend, f->hir, &mc) : NULL;
  AstTargetConfig tc = ast_target_config_default(arches[arch]);
  tc.os = AST_TARGET_OS_LINUX;
  if (arch == 2)
    tc.features |= AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_F |
                   AST_TARGET_FEATURE_RV_D;
  f->target = f->mir ? ast_target_legalize(f->mir, &tc) : NULL;
  AstAbiConfig ac = ast_abi_config_default(abis[arch]);
  f->abi = f->target ? ast_abi_lower(f->target, &ac) : NULL;
  f->machine = f->abi ? ast_machine_build(f->abi, NULL) : NULL;
  if (!f->machine || (stage >= 1 && !ast_machine_assign_regbanks(f->machine)) ||
      (stage >= 2 && !ast_machine_select_instructions(f->machine)))
    return 0;
  return ast_machine_verify(f->machine);
}

static MachineInstruction *find_op(MachineFunction *f, AstMirOp op) {
  for (uint32_t i = 0; i < f->info.instruction_count; ++i)
    if (f->instructions[i].info.source_op == op)
      return &f->instructions[i];
  return NULL;
}

/* Each mutation stays within allocated storage. A verifier must reject semantic
   corruption as well as invalid IDs; restoring the image must make it valid. */
static int check(Fixture *f, int test, int arch, int stage) {
  for (size_t fi = 0; fi < f->machine->function_count; ++fi) {
    MachineFunction *fn = &f->machine->functions[fi];
    MachineInstruction *mi =
        find_op(fn, test == 7                   ? AST_MIR_OP_CONSTANT
                    : test == 8                 ? AST_MIR_OP_STACK_LOAD
                    : (test == 9 || test == 30) ? AST_MIR_OP_CALL_DIRECT
                                                : AST_MIR_OP_BINARY);
    if (!mi || mi->info.result_count != 1u)
      continue;
    AstMachineInstructionInfo old = mi->info;
    AstMachineVRegInfo *v = &fn->vregs[mi->info.result_first], saved_v = *v;
    AstMachineOperand *op = &fn->operands[mi->operand_offset], saved_op = *op;
    uint32_t source = v->source_vreg, old_first = fn->source_first[source],
             old_parts = fn->source_parts[source];
    uint32_t old_source_count = fn->source_vreg_count;
    MachineBlock *block = &fn->blocks[fn->info.entry_block];
    MachineInstruction *term =
        &fn->instructions[block->instructions[block->info.instruction_count -
                                              1u]];
    AstMachineInstructionInfo old_term = term->info;
    uint8_t old_dead = mi->dead, old_term_dead = term->dead;
    uint32_t old_alias = fn->vreg_alias[mi->info.result_first];
    uint32_t first_id = block->instructions[0],
             second_id = block->instructions[1];
    AstMachineOperand *extra = NULL, saved_extra = {0};
    switch (test) {
    case 0:
      mi->info.token_op = AST_TOK_MINUS;
      break;
    case 1:
      mi->info.intrinsic = AST_HIR_INTRINSIC_SELECT;
      break;
    case 2:
      mi->info.effects ^= 1u;
      break;
    case 3:
      mi->info.flags ^= 1u;
      break;
    case 4:
      mi->info.result_first = op->as.vreg;
      break;
    case 5:
      op->as.vreg = mi->info.result_first;
      break; /* self-use before definition */
    case 6:
      op->kind = AST_MACHINE_OPERAND_IMMEDIATE;
      break;
    case 7:
      op->as.immediate ^= 1;
      break;
    case 8:
    case 9:
    case 30:
      extra = op + mi->info.operand_count - 1u;
      saved_extra = *extra;
      if (test == 8)
        extra->as.stack_slot = AST_MIR_INVALID_ID;
      else if (test == 9)
        extra->as.symbol.hash ^= 1u;
      else
        extra->module.hash ^= 1u;
      break;
    case 10:
      fn->source_first[source] = fn->info.vreg_count;
      break;
    case 11:
      fn->source_parts[source] = 0u;
      break;
    case 12:
      fn->source_vreg_count--;
      break;
    case 13:
      v->part_index++;
      v->part_count++;
      break;
    case 14:
      v->bit_width = 8u;
      break;
    case 15:
      v->reg_class = AST_TARGET_REGCLASS_GPR8;
      break;
    case 16:
      v->flags ^= AST_MACHINE_VREG_FUNCTION_ARGUMENT;
      break;
    case 17:
      op->flags |= UINT32_C(0x80000000);
      break;
    case 18:
      v->flags |= UINT32_C(0x80000000);
      break;
    case 19:
      mi->info.span.offset++;
      break;
    case 20:
      mi->info.opcode = stage < 2   ? AST_MACHINE_G_UNARY
                        : arch == 0 ? AST_MACHINE_X86_SUB
                        : arch == 1 ? AST_MACHINE_A64_SUB
                                    : AST_MACHINE_RISCV_SUB;
      break;
    case 21:
      mi->info.source_instruction = AST_MIR_INVALID_ID;
      break;
    case 22:
      if (block->info.instruction_count < 3u)
        continue;
      block->instructions[0] = second_id;
      block->instructions[1] = first_id;
      break;
    case 23:
      if (machine_opcode_terminator_kind(term->info.opcode) !=
          AST_MIR_TERM_RETURN)
        continue;
      term->info.opcode = stage < 2   ? AST_MACHINE_G_UNREACHABLE
                          : arch == 0 ? AST_MACHINE_X86_TRAP
                          : arch == 1 ? AST_MACHINE_A64_TRAP
                                      : AST_MACHINE_RISCV_EBREAK;
      term->info.operand_count = 0u;
      term->info.operands = NULL;
      break;
    case 24:
      mi->info.result_count = 0u;
      break;
    case 25:
      mi->dead = 1u;
      break;
    case 26:
      term->dead = 1u;
      break;
    case 27:
      mi->dead = 2u;
      break;
    case 28:
      fn->vreg_alias[mi->info.result_first] = fn->info.vreg_count;
      break;
    case 29:
      fn->vreg_alias[mi->info.result_first] = op->as.vreg;
      break;
    default:
      return 0;
    }
    int rejected = !ast_machine_verify(f->machine);
    mi->info = old;
    *v = saved_v;
    *op = saved_op;
    mi->dead = old_dead;
    term->dead = old_term_dead;
    fn->vreg_alias[mi->info.result_first] = old_alias;
    if (extra)
      *extra = saved_extra;
    fn->source_first[source] = old_first;
    fn->source_parts[source] = old_parts;
    fn->source_vreg_count = old_source_count;
    term->info = old_term;
    block->instructions[0] = first_id;
    block->instructions[1] = second_id;
    return rejected && ast_machine_verify(f->machine);
  }
  fprintf(stderr, "No eligible mutation for test %d\n", test);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  int test = atoi(argv[1]), arch = atoi(argv[2]), stage = atoi(argv[3]);
  if (test < 0 || test > 30 || arch < 0 || arch > 2 || stage < 0 || stage > 2)
    return 2;
  Fixture f = {0};
  int ok = open_fixture(&f, arch, stage) && check(&f, test, arch, stage);
  close_fixture(&f);
  if (!ok)
    fprintf(stderr, "Machine verification case %d arch %d stage %d failed\n",
            test, arch, stage);
  return ok ? 0 : 1;
}
