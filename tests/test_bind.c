#include "allocation_internal.h"
#include "asterasm/asterasm.h"
#include "test_process.h"

static void print_dump(void *user, const char *text, size_t length) {
  (void)fwrite(text, 1u, length, (FILE *)user);
}
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc != 5)
    return 2;
  uint32_t test = (uint32_t)strtoul(argv[1], NULL, 10);
  uint32_t abi_index = (uint32_t)strtoul(argv[2], NULL, 10);
  uint32_t limit = (uint32_t)strtoul(argv[3], NULL, 10);
  uint32_t promote = (uint32_t)strtoul(argv[4], NULL, 10);
  if (test > 24u || abi_index > 5u || promote > 1u ||
      (test >= 7u && abi_index > 1u))
    return 2;
  AstTargetArch arch = abi_index < 2u    ? AST_TARGET_ARCH_X86_64
                       : abi_index == 2u ? AST_TARGET_ARCH_AARCH64
                                         : AST_TARGET_ARCH_RISCV64;
  static const AstAbiKind abis[] = {AST_ABI_SYSV_AMD64,  AST_ABI_WINDOWS_X64,
                                    AST_ABI_AAPCS64,     AST_ABI_RISCV_LP64,
                                    AST_ABI_RISCV_LP64F, AST_ABI_RISCV_LP64D};
  static const char *const fixtures[] = {"bind_role", "bind_conflict",
                                         "bind_incompatible", "bind_invalid",
                                         "bind_address"};
  const char *fixture = test < 5u                         ? fixtures[test]
                        : arch == AST_TARGET_ARCH_X86_64  ? "bind_x86"
                        : arch == AST_TARGET_ARCH_AARCH64 ? "bind_a64"
                                                          : "bind_rv";
  static const char *const native_fixtures[] = {
      "native_address_bad",   "native_attribute_bad", "native_effect_bad",
      "native_immediate_bad", "native_memory_bad",    "native_stack_bad",
      "native_callee_bad"};
  if (test >= 7u && test <= 13u)
    fixture = native_fixtures[test - 7u];
  if (test >= 14u)
    fixture = test == 18u ? "native_naked" : "native_scalar";
  char root[4096], path[4096];
  (void)snprintf(root, sizeof(root), "%s/tests/regalloc_cases/%s",
                 ASTERASM_SOURCE_DIR, fixture);
  (void)snprintf(path, sizeof(path), "%s/tests/regalloc_cases/%s/main.aa",
                 ASTERASM_SOURCE_DIR, fixture);
  AstFrontend *frontend = NULL;
  AstSema *sema = NULL;
  AstHirProgram *hir = NULL;
  AstMirProgram *mir = NULL;
  AstTargetProgram *target = NULL;
  AstAbiProgram *abi = NULL;
  AstMachineProgram *machine = NULL;
  AstRegAllocProgram *p = NULL;
  int ok = 0;
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  frontend = ast_frontend_create(&fc);
  const char *entries[] = {path};
  if (!frontend || ast_frontend_add_import_root(frontend, root) ||
      ast_frontend_load(frontend, entries, 1u))
    goto done;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  sema = ast_sema_create(frontend, &sc);
  if (!sema || ast_sema_check(sema))
    goto done;
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  hir = ast_hir_build(frontend, sema, &hc);
  if (!hir || !ast_hir_verify(hir)) {
    fputs("HIR failed\n", stderr);
    goto done;
  }
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  mc.promote_locals = (uint8_t)promote;
  mir = ast_mir_build(frontend, hir, &mc);
  if (!mir || !ast_mir_verify(mir)) {
    fputs("MIR failed\n", stderr);
    if (mir)
      ast_mir_dump_program(mir, print_dump, stderr);
    goto done;
  }
  AstTargetConfig tc = ast_target_config_default(arch);
  if (arch == AST_TARGET_ARCH_X86_64 && test != 19u)
    tc.features |= AST_TARGET_FEATURE_X86_POPCNT;
  if (arch == AST_TARGET_ARCH_RISCV64)
    tc.features |= AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_F |
                   AST_TARGET_FEATURE_RV_D;
  tc.os = abi_index == 1u ? AST_TARGET_OS_WINDOWS : AST_TARGET_OS_LINUX;
  target = ast_target_legalize(mir, &tc);
  if (test == 19u) {
    size_t count = 0u;
    const AstTargetDiagnostic *diagnostics =
        ast_target_diagnostics(target, &count);
    for (size_t i = 0; i < count; ++i)
      if (diagnostics[i].code == AST_TARGET_DIAG_REQUIRED_FEATURE)
        ok = 1;
    ok = ok && !ast_target_verify(target);
    goto done;
  }
  if (!target || !ast_target_verify(target)) {
    fputs("target failed\n", stderr);
    goto done;
  }
  AstAbiConfig ac = ast_abi_config_default(abis[abi_index]);
  abi = ast_abi_lower(target, &ac);
  if (!abi || !ast_abi_verify(abi)) {
    fputs("ABI failed\n", stderr);
    goto done;
  }
  machine = ast_backend_lower(abi, NULL);
  if (!machine || !ast_machine_verify(machine)) {
    fputs("machine failed\n", stderr);
    goto done;
  }
  AstRegAllocConfig rc = ast_regalloc_config_default();
  rc.register_limit = limit;
  p = ast_regalloc_build(machine, &rc);
  if (!p)
    goto done;
  if (test >= 7u && test <= 13u) {
    if (test == 13u) {
      /* The final native verifier rejects the unsafe naked callee-save
         clobber while physical materialization is being finalized. */
      int verified = ast_regalloc_verify(p);
      int finalized = verified ? ast_regalloc_finalize(p) : 1;
      ok = verified && !finalized && p->stats.error_count == 1u &&
           p->diagnostic.code == AST_REGALLOC_DIAG_INVALID_PHYSICAL;
    } else {
      ok = ast_regalloc_verify(p) && !ast_regalloc_finalize(p) &&
           p->stats.error_count == 1u &&
           p->diagnostic.code == AST_REGALLOC_DIAG_IMPOSSIBLE_CONSTRAINT;
    }
    goto done;
  }
  if (test >= 1u && test <= 4u) {
    ok = p->stats.error_count == 1u &&
         p->diagnostic.code == AST_REGALLOC_DIAG_IMPOSSIBLE_CONSTRAINT &&
         !ast_regalloc_verify(p);
    goto done;
  }
  if (!ast_regalloc_verify(p) || !ast_regalloc_finalize(p) ||
      !ast_physical_verify(p))
    goto done;
  if (test == 20u) {
    p->target.features = 0u;
    ok = !ast_physical_verify(p);
    goto done;
  }
  if (test >= 14u) {
    AstMirInstructionInfo source = {0};
    uint32_t id = AST_MACHINE_INVALID_ID;
    for (uint32_t i = 0; i < machine->functions[0].info.instruction_count; ++i)
      if (machine->functions[0].instructions[i].info.source_op ==
          AST_MIR_OP_NATIVE) {
        id = i;
        break;
      }
    if (id == AST_MACHINE_INVALID_ID ||
        !ast_mir_function_instruction(
            ast_mir_function_at(mir, 0u),
            machine->functions[0].instructions[id].info.source_instruction,
            &source))
      goto done;
    const AstHirFunction *hf =
        ast_mir_function_source_hir(ast_mir_function_at(mir, 0u));
    AstHirNativeStmtInfo first;
    if (!ast_hir_native_region_statement(
            ast_hir_function_native_region(hf, source.native_region), 0u,
            &first))
      goto done;
    PhysicalBlock *block = &p->functions[0].blocks[0];
    uint32_t at = AST_MACHINE_INVALID_ID;
    for (uint32_t i = 0; i < block->count; ++i)
      if (block->instructions[i].source_instruction == id &&
          block->instructions[i].span.offset == first.span.offset) {
        at = i;
        break;
      }
    if (at == AST_MACHINE_INVALID_ID)
      goto done;
    AstPhysicalInstructionInfo *body = &block->instructions[at],
                               *changed = body;
    if (test == 21u || test == 22u) {
      const AstPhysicalOperand *saved = NULL;
      for (uint32_t i = 0; i < at; ++i) {
        const AstPhysicalInstructionInfo *pi = &block->instructions[i];
        if (pi->source_instruction == id &&
            pi->opcode == AST_MACHINE_X86_STORE &&
            pi->operands[0].register_unit == body->operands[0].register_unit)
          saved = &pi->operands[1];
      }
      if (!saved)
        goto done;
      changed = NULL;
      for (uint32_t i = at + 1u; i < block->count; ++i) {
        AstPhysicalInstructionInfo *pi = &block->instructions[i];
        if (pi->source_instruction == id &&
            pi->opcode == AST_MACHINE_X86_STORE &&
            pi->operands[0].register_unit == body->operands[0].register_unit) {
          changed = pi;
          break;
        }
      }
      if (!changed)
        goto done;
      changed->operands[1] = *saved;
      if (test == 22u) {
        /* A one-byte store into the last byte also destroys the original. */
        changed->operands[0].bit_width = changed->operands[1].bit_width = 8u;
        changed->operands[1].displacement += 7;
        if (!machine_physreg_name(&p->target, AST_MACHINE_BANK_GPR,
                                  changed->operands[0].register_unit % 64u, 8u,
                                  changed->operands[0].register_name))
          goto done;
      }
    } else if (test == 15u)
      body->span.offset++;
    else if (test == 18u) {
      body->operands[0].register_unit = MACHINE_PHYSREG_FILE_GPR * 64u + 3u;
      memcpy(body->operands[0].register_name, "rbx", 4u);
    } else {
      if (test == 16u || test == 17u || test == 23u || test == 24u) {
        int save = test == 16u || test == 24u;
        uint32_t unit = body->operands[test >= 23u ? 1u : 0u].register_unit;
        changed = NULL;
        for (uint32_t i = save ? 0u : at + 1u; i < (save ? at : block->count);
             ++i) {
          AstPhysicalInstructionInfo *pi = &block->instructions[i];
          if (pi->source_instruction == id &&
              pi->opcode ==
                  (save ? AST_MACHINE_X86_STORE : AST_MACHINE_X86_LOAD) &&
              pi->operands[0].register_unit == unit &&
              pi->operands[0].bit_width == 64u) {
            changed = pi;
            break;
          }
        }
      }
      if (!changed)
        goto done;
      changed->opcode = AST_MACHINE_X86_NOP;
      changed->operand_count = changed->result_count = 0u;
    }
    ok = !ast_physical_verify(p);
    goto done;
  }
  uint32_t expected = test == 0u                        ? (abi_index == 0u   ? 7u
                                                           : abi_index == 1u ? 1u
                                                           : abi_index == 2u ? 0u
                                                                             : 10u)
                      : arch == AST_TARGET_ARCH_X86_64  ? 12u
                      : arch == AST_TARGET_ARCH_AARCH64 ? 19u
                                                        : 9u;
  expected += MACHINE_PHYSREG_FILE_GPR * 64u;
  uint32_t binds = 0u;
  for (uint32_t f = 0; f < machine->function_count; ++f)
    for (uint32_t i = 0; i < machine->functions[f].info.instruction_count;
         ++i) {
      const MachineInstruction *mi = &machine->functions[f].instructions[i];
      if (mi->info.source_op != AST_MIR_OP_BIND)
        continue;
      ++binds;
      uint32_t v = mi->info.operands[0].as.vreg;
      AstRegAllocLocation *loc = &p->functions[f].locations[v];
      if (loc->kind != AST_REGALLOC_LOCATION_REGISTER ||
          loc->register_unit != expected)
        goto done;
      if (test == 6u) {
        /* A different, otherwise valid register must violate bind even
           when there is no overlapping interval to catch it. */
        AstRegAllocLocation saved = *loc;
        MachinePhysRegView view;
        uint32_t wrong = arch == AST_TARGET_ARCH_X86_64    ? 11u
                         : arch == AST_TARGET_ARCH_AARCH64 ? 17u
                                                           : 31u;
        if (!machine_physreg_name(&p->target, loc->bank, wrong, loc->bit_width,
                                  loc->register_name) ||
            !machine_physreg_view(&p->target, loc->register_name, &view))
          goto done;
        loc->register_unit = view.alias_unit;
        int rejected = !ast_regalloc_verify(p);
        *loc = saved;
        if (!rejected)
          goto done;
      }
    }
  ok = binds == (test == 0u ? 2u : 1u);
done:
  if (!ok) {
    fprintf(stderr, "bind case=%u abi=%u limit=%u promote=%u failed\n", test,
            abi_index, limit, promote);
    if (p)
      ast_regalloc_dump(p, print_dump, stderr);
  }
  ast_regalloc_destroy(p);
  ast_machine_destroy(machine);
  ast_abi_destroy(abi);
  ast_target_destroy(target);
  ast_mir_destroy(mir);
  ast_hir_destroy(hir);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  return ok ? 0 : 1;
}
