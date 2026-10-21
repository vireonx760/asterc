#include "asterasm/asterasm.h"
#include "physical_internal.h"
#include "test_process.h"

typedef struct Fixture {
  AstFrontend *frontend;
  AstSema *sema;
  AstHirProgram *hir;
  AstMirProgram *mir;
  AstTargetProgram *target;
  AstAbiProgram *abi;
  AstMachineProgram *machine;
  AstRegAllocProgram *allocation;
} Fixture;

static void close_fixture(Fixture *f) {
  ast_regalloc_destroy(f->allocation);
  ast_machine_destroy(f->machine);
  ast_abi_destroy(f->abi);
  ast_target_destroy(f->target);
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
}
static void print_dump(void *user, const char *text, size_t length) {
  (void)fwrite(text, 1u, length, (FILE *)user);
}
static int open_fixture(Fixture *f, uint32_t test, uint32_t abi,
                        uint32_t promote) {
  char root[4096], path[4096];
  const char *fixture =
      test >= 12u && test < 18u ? "crossarch_scalar" : "call_preservation";
  (void)snprintf(root, sizeof(root), "%s/tests/regalloc_cases/%s",
                 ASTERASM_SOURCE_DIR, fixture);
  (void)snprintf(path, sizeof(path), "%s/tests/regalloc_cases/%s/main.aa",
                 ASTERASM_SOURCE_DIR, fixture);
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  const char *entries[] = {path};
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) ||
      ast_frontend_load(f->frontend, entries, 1u))
    return 0;
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema))
    return 0;
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir))
    return 0;
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  mc.promote_locals = (uint8_t)promote;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir))
    return 0;
  AstTargetConfig tc = ast_target_config_default(abi ? AST_TARGET_ARCH_RISCV64
                                                     : AST_TARGET_ARCH_AARCH64);
  tc.os = AST_TARGET_OS_LINUX;
  if (abi)
    tc.features |= AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_F |
                   AST_TARGET_FEATURE_RV_D;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target))
    return 0;
  static const AstAbiKind abis[] = {AST_ABI_AAPCS64, AST_ABI_RISCV_LP64,
                                    AST_ABI_RISCV_LP64F, AST_ABI_RISCV_LP64D};
  AstAbiConfig ac = ast_abi_config_default(abis[abi]);
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi))
    return 0;
  f->machine = ast_backend_lower(f->abi, NULL);
  if (!f->machine || !ast_machine_verify(f->machine))
    return 0;
  AstRegAllocConfig rc = ast_regalloc_config_default();
  rc.register_limit = 2u;
  f->allocation = ast_regalloc_build(f->machine, &rc);
  if (!f->allocation || !ast_regalloc_verify(f->allocation))
    return 0;
  if (test >= 18u)
    for (uint32_t i = 0; i < f->machine->function_count; ++i)
      if (frame_add_object(&f->allocation->functions[i], AST_FRAME_OBJECT_LOCAL,
                           test == 21u ? 8192u : 65536u,
                           16u) == AST_MACHINE_INVALID_ID)
        return 0;
  return ast_regalloc_finalize(f->allocation) &&
         ast_physical_verify(f->allocation);
}
static uint32_t find_function(const Fixture *f, const char *name) {
  for (uint32_t i = 0; i < f->machine->function_count; ++i) {
    AstName n = f->machine->functions[i].info.name;
    if (n.span.length == strlen(name) && !memcmp(n.text, name, n.span.length))
      return i;
  }
  return AST_MACHINE_INVALID_ID;
}
static void nop(AstPhysicalInstructionInfo *i, int rv) {
  i->opcode = rv ? AST_MACHINE_RISCV_FENCE : AST_MACHINE_A64_FENCE;
  i->operand_count = i->result_count = 0u;
}
static int set_register(const AstRegAllocProgram *p, AstPhysicalOperand *o,
                        AstMachineRegBank bank, uint32_t index, uint32_t bits) {
  MachinePhysRegView view;
  memset(o, 0, sizeof(*o));
  o->kind = AST_PHYSICAL_OPERAND_REGISTER;
  o->bank = bank;
  o->bit_width = bits;
  if (!machine_physreg_name(&p->target, bank, index, bits, o->register_name) ||
      !machine_physreg_view(&p->target, o->register_name, &view))
    return 0;
  o->register_unit = view.alias_unit;
  return 1;
}
static int insert(PhysicalBlock *block, uint32_t at,
                  const AstPhysicalInstructionInfo *i) {
  size_t bytes;
  if (at > block->count || block->count == UINT32_MAX ||
      ast_size_mul_overflow((size_t)block->count + 1u,
                            sizeof(*block->instructions), &bytes))
    return 0;
  AstPhysicalInstructionInfo *next =
      (AstPhysicalInstructionInfo *)realloc(block->instructions, bytes);
  if (!next)
    return 0;
  block->instructions = next;
  memmove(next + at + 1u, next + at,
          (size_t)(block->count - at) * sizeof(*next));
  next[at] = *i;
  ++block->count;
  block->capacity = block->count;
  return 1;
}
static AstPhysicalInstructionInfo *find_opcode(AllocationFunction *af,
                                               AstMachineOpcode opcode) {
  for (uint32_t b = 0; b < af->block_count; ++b)
    for (uint32_t i = 0; i < af->blocks[b].count; ++i)
      if (af->blocks[b].instructions[i].opcode == opcode)
        return &af->blocks[b].instructions[i];
  return NULL;
}
static int mutate(Fixture *f, uint32_t test, int rv) {
  AstRegAllocProgram *p = f->allocation;
  const char *name = test == 12u                  ? "signed_to_float"
                     : test == 13u                ? "widen_float"
                     : test == 14u || test == 15u ? "float_less"
                     : test == 16u                ? "select_float"
                     : test == 17u                ? "double_to_signed"
                                                  : "preserve";
  uint32_t fi = find_function(f, name);
  if (fi == AST_MACHINE_INVALID_ID)
    return 0;
  AllocationFunction *af = &p->functions[fi];
  PhysicalBlock *block = &af->blocks[0];
  AstMachineOpcode load = rv ? AST_MACHINE_RISCV_LD : AST_MACHINE_A64_LOAD;
  AstMachineOpcode store = rv ? AST_MACHINE_RISCV_SD : AST_MACHINE_A64_STORE;
  AstMachineOpcode call = rv ? AST_MACHINE_RISCV_CALL : AST_MACHINE_A64_CALL;
  uint32_t link = MACHINE_PHYSREG_FILE_GPR * 64u + (rv ? 1u : 30u);
  if (test == 25u || test == 26u) {
    if (block->count < 3u)
      return 0;
    if (rv) {
      if (block->instructions[0].opcode != AST_MACHINE_RISCV_LI ||
          block->instructions[1].opcode != AST_MACHINE_RISCV_ADD)
        return 0;
      if (test == 25u) {
        /* An argument register is live on entry and cannot be used to
           materialize the frame size before argument transfers. */
        return set_register(p, &block->instructions[0].operands[0],
                            AST_MACHINE_BANK_GPR, 10u, 64u) &&
               set_register(p, &block->instructions[1].operands[2],
                            AST_MACHINE_BANK_GPR, 10u, 64u);
      }
      block->instructions[0].operands[1].immediate_bits += 16u;
    } else {
      if (block->instructions[0].opcode != AST_MACHINE_A64_SUB ||
          block->instructions[1].opcode != AST_MACHINE_A64_SUB)
        return 0;
      block->instructions[0].operands[2].immediate_bits += 16u;
      if (test == 25u)
        block->instructions[1].operands[2].immediate_bits -=
            16u; /* same total, unencodable first immediate */
    }
    return 1;
  }
  if (test == 24u) {
    /* Reuse is legal only for disjoint intervals. Force genuinely live
       overlapping values to one otherwise legal physical register. */
    for (uint32_t a = 0; a < f->machine->functions[fi].info.vreg_count; ++a)
      for (uint32_t b = 0; b < a; ++b)
        if (af->locations[a].kind == AST_REGALLOC_LOCATION_REGISTER &&
            af->locations[b].kind == AST_REGALLOC_LOCATION_REGISTER &&
            af->locations[a].bit_width == af->locations[b].bit_width &&
            machine_intervals_overlap(&af->liveness.intervals[a],
                                      &af->liveness.intervals[b])) {
          af->locations[a] = af->locations[b];
          return 1;
        }
    return 0;
  }
  if (test < 6u) {
    uint32_t object = af->callee_save_objects[link];
    if (object == AST_MACHINE_INVALID_ID)
      return 0;
    uint32_t at = AST_MACHINE_INVALID_ID;
    int saving = test == 0u || test == 3u;
    for (uint32_t i = 0; i < block->count; ++i) {
      const AstPhysicalInstructionInfo *pi = &block->instructions[i];
      if (pi->opcode == (saving ? store : load) &&
          pi->operands[0].register_unit == link &&
          pi->operands[1].displacement == af->objects[object].offset) {
        at = i;
        break;
      }
    }
    if (at == AST_MACHINE_INVALID_ID)
      return 0;
    AstPhysicalInstructionInfo *pi = &block->instructions[at];
    if (test < 2u)
      nop(pi, rv);
    else if (test == 2u) {
      AstPhysicalInstructionInfo clobber = *pi;
      clobber.opcode = rv ? AST_MACHINE_RISCV_MV : AST_MACHINE_A64_MOV;
      if (!set_register(p, &clobber.operands[1], AST_MACHINE_BANK_GPR,
                        rv ? 10u : 0u, 64u))
        return 0;
      return insert(block, at + 1u, &clobber);
    } else if (test == 3u) {
      AstPhysicalInstructionInfo overwrite = *pi;
      if (!set_register(p, &overwrite.operands[0], AST_MACHINE_BANK_GPR,
                        rv ? 10u : 0u, 64u))
        return 0;
      return insert(block, at + 1u, &overwrite);
    } else if (test == 4u) {
      if (!set_register(p, &pi->operands[0], AST_MACHINE_BANK_GPR,
                        rv ? 1u : 30u, rv ? 64u : 32u))
        return 0;
      pi->operands[1].bit_width = 32u;
      if (rv)
        pi->opcode = AST_MACHINE_RISCV_LW;
    } else
      pi->operands[1].displacement += 8;
    return 1;
  }
  if (test <= 10u) {
    uint32_t at = AST_MACHINE_INVALID_ID;
    for (uint32_t i = 0; i < block->count; ++i)
      if (block->instructions[i].opcode == call) {
        at = i;
        break;
      }
    if (at == AST_MACHINE_INVALID_ID)
      return 0;
    if (test == 10u) {
      block->instructions[at].source_instruction = AST_MACHINE_INVALID_ID;
      return 1;
    }
    uint32_t id = block->instructions[at].source_instruction;
    uint32_t unit = MACHINE_PHYSREG_FILE_GPR * 64u + (rv ? 10u : 0u),
             save_at = AST_MACHINE_INVALID_ID;
    for (uint32_t i = 0; i < at; ++i)
      if (block->instructions[i].source_instruction == id &&
          block->instructions[i].opcode == store &&
          block->instructions[i].operands[0].register_unit == unit) {
        save_at = i;
        break;
      }
    if (save_at == AST_MACHINE_INVALID_ID)
      return 0;
    AstPhysicalInstructionInfo saved = block->instructions[save_at];
    uint32_t restore_at = AST_MACHINE_INVALID_ID;
    for (uint32_t i = at + 1u; i < block->count; ++i)
      if (block->instructions[i].source_instruction == id &&
          block->instructions[i].opcode == load &&
          block->instructions[i].operands[0].register_unit == unit &&
          physical_same_location(&saved.operands[1],
                                 &block->instructions[i].operands[1])) {
        restore_at = i;
        break;
      }
    if (restore_at == AST_MACHINE_INVALID_ID)
      return 0;
    if (test == 6u || test == 7u)
      nop(&block->instructions[test == 6u ? save_at : restore_at], rv);
    else if (test == 8u)
      return insert(block, at + 1u, &saved);
    else {
      AstPhysicalInstructionInfo clobber = block->instructions[restore_at];
      clobber.opcode = rv ? AST_MACHINE_RISCV_MV : AST_MACHINE_A64_MOV;
      if (!set_register(p, &clobber.operands[1], AST_MACHINE_BANK_GPR,
                        rv ? 11u : 1u, 64u))
        return 0;
      return insert(block, restore_at + 1u, &clobber);
    }
    return 1;
  }
  if (test == 11u) {
    AstPhysicalInstructionInfo *i = find_opcode(af, call);
    if (!i)
      return 0;
    i->opcode = rv ? AST_MACHINE_RISCV_INTRINSIC : AST_MACHINE_A64_INTRINSIC;
    return 1;
  }
  if (test < 18u) {
    AstMachineOpcode op =
        test == 12u ? (rv ? AST_MACHINE_RISCV_FCVT_S_W : AST_MACHINE_A64_SCVTF)
        : test == 13u ? (rv ? AST_MACHINE_RISCV_FCVT_D_S : AST_MACHINE_A64_FCVT)
        : test == 14u || test == 15u
            ? (rv ? AST_MACHINE_RISCV_FLT_D : AST_MACHINE_A64_FCMP)
        : test == 16u
            ? (rv ? AST_MACHINE_RISCV_FMV_S : AST_MACHINE_A64_FCSEL)
            : (rv ? AST_MACHINE_RISCV_FCVT_L_D : AST_MACHINE_A64_FCVTZS);
    AstPhysicalInstructionInfo *i = find_opcode(af, op);
    if (!i)
      return 0;
    if (test == 12u)
      return set_register(p, &i->operands[0], AST_MACHINE_BANK_GPR,
                          rv ? 5u : 9u, rv ? 64u : 32u);
    if (test == 13u)
      return set_register(p, &i->operands[1], AST_MACHINE_BANK_FP,
                          i->operands[1].register_unit % 64u,
                          i->operands[0].bit_width);
    if (test == 14u)
      return set_register(p, &i->operands[rv ? 2u : 1u], AST_MACHINE_BANK_FP,
                          3u, 32u);
    if (test == 15u) {
      if (rv)
        i->opcode = AST_MACHINE_RISCV_FLT_S;
      else
        nop(i, rv);
    } else if (test == 16u) {
      if (rv)
        i->operands[1].bit_width = 16u;
      else
        i->source_instruction = AST_MACHINE_INVALID_ID;
    } else {
      if (rv)
        i->operands[2].immediate_bits =
            0u; /* integer conversion must use RTZ */
      else
        return set_register(p, &i->operands[1], AST_MACHINE_BANK_GPR, 9u, 64u);
    }
    return 1;
  }
  /* Break one element of a large-offset borrow sequence, retaining otherwise
     legal instructions. The normalizer must not erase the corruption. */
  for (uint32_t i = 0; i + (rv ? 6u : 5u) < block->count; ++i) {
    AstPhysicalInstructionInfo *sequence = &block->instructions[i];
    int start = rv ? sequence->opcode == AST_MACHINE_RISCV_ADDI &&
                         (int64_t)sequence->operands[2].immediate_bits == -16
                   : sequence->opcode == AST_MACHINE_A64_SUB &&
                         sequence->operands[2].immediate_bits == 16u;
    if (!start || sequence[1].opcode != store ||
        sequence[rv ? 5u : 4u].opcode != load ||
        sequence[1].operands[1].displacement != 0)
      continue;
    if (test == 18u)
      return set_register(p, &sequence[1].operands[0], AST_MACHINE_BANK_GPR,
                          rv ? 10u : 0u, 64u);
    if (test == 19u)
      sequence[2].source_instruction ^= 1u;
    else if (test == 23u) {
      /* Remove the real SP-bias compensation, leaving a valid instruction
         sequence whose effective frame address is now wrong by 16. */
      if (rv)
        sequence[2].operands[1].immediate_bits -= 16u;
      else
        sequence[2].operands[1].displacement -= 16;
    } else
      sequence[rv ? 5u : 4u].operands[1].displacement = 8;
    return 1;
  }
  return 0;
}
static int audit_link_address(Fixture *f, int rv) {
  uint32_t fi = find_function(f, "preserve");
  if (fi == AST_MACHINE_INVALID_ID)
    return 0;
  const AllocationFunction *af = &f->allocation->functions[fi];
  const PhysicalBlock *block = &af->blocks[0];
  uint32_t sp = rv ? MACHINE_PHYSREG_FILE_GPR * 64u + 2u
                   : MACHINE_PHYSREG_FILE_SPECIAL * 64u;
  uint32_t link = MACHINE_PHYSREG_FILE_GPR * 64u + (rv ? 1u : 30u),
           object = af->callee_save_objects[link];
  if (object >= af->object_count)
    return 0;
  uint64_t regs[MACHINE_REGISTER_UNITS] = {0};
  const uint64_t entry_sp = UINT64_C(0x100000);
  regs[sp] = entry_sp;
  uint32_t adjustments = 0u;
  for (uint32_t n = 0; n < block->count; ++n) {
    const AstPhysicalInstructionInfo *i = &block->instructions[n];
    const AstPhysicalOperand *o = i->operands;
    if (i->opcode == (rv ? AST_MACHINE_RISCV_SD : AST_MACHINE_A64_STORE) &&
        o[0].register_unit == link) {
      uint64_t address = regs[o[1].register_unit] + (uint64_t)o[1].displacement;
      uint64_t frame_sp = entry_sp - af->frame.stack_adjustment;
      printf("%s frame=%llu link-slot=0x%llx effective-offset=0x%llx "
             "SP-adjustments=%u\n",
             rv ? "RV64" : "A64",
             (unsigned long long)af->frame.stack_adjustment,
             (unsigned long long)af->objects[object].offset,
             (unsigned long long)(address - frame_sp), adjustments);
      if (!rv && af->frame.stack_adjustment <= UINT64_C(0xfff000) &&
          adjustments > 3u)
        return 0;
      if (rv && af->frame.stack_adjustment > 6096u && adjustments > 2u)
        return 0;
      return address == frame_sp + (uint64_t)af->objects[object].offset;
    }
    if (i->opcode == AST_MACHINE_RISCV_LI)
      regs[o[0].register_unit] = o[1].immediate_bits;
    else if (i->opcode == AST_MACHINE_A64_LEA)
      regs[o[0].register_unit] =
          regs[o[1].register_unit] + (uint64_t)o[1].displacement;
    else if (i->opcode == AST_MACHINE_RISCV_ADDI ||
             i->opcode == AST_MACHINE_RISCV_ADD ||
             i->opcode == AST_MACHINE_A64_SUB ||
             i->opcode == AST_MACHINE_A64_ADD) {
      uint64_t rhs = o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE
                         ? o[2].immediate_bits
                         : regs[o[2].register_unit];
      regs[o[0].register_unit] = i->opcode == AST_MACHINE_A64_SUB
                                     ? regs[o[1].register_unit] - rhs
                                     : regs[o[1].register_unit] + rhs;
      if (o[0].register_unit == sp)
        ++adjustments;
    } else if (i->opcode != (rv ? AST_MACHINE_RISCV_SD : AST_MACHINE_A64_STORE))
      return 0;
  }
  return 0;
}
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc != 4)
    return 2;
  uint32_t test = (uint32_t)strtoul(argv[1], NULL, 10),
           abi = (uint32_t)strtoul(argv[2], NULL, 10);
  uint32_t promote = (uint32_t)strtoul(argv[3], NULL, 10);
  if (test > 26u || abi > 3u || promote > 1u)
    return 2;
  Fixture fixture = {0};
  int built = open_fixture(&fixture, test, abi, promote);
  int audit = test == 21u || test == 22u;
  int changed = built && (audit ? audit_link_address(&fixture, abi != 0u)
                                : mutate(&fixture, test, abi != 0u));
  int ok = changed && (audit ? ast_physical_verify(fixture.allocation)
                             : !ast_physical_verify(fixture.allocation));
  if (!ok) {
    fprintf(
        stderr,
        "architecture mutation case=%u abi=%u promote=%u built=%d changed=%d\n",
        test, abi, promote, built, changed);
    if (fixture.allocation)
      ast_regalloc_dump(fixture.allocation, print_dump, stderr);
  }
  close_fixture(&fixture);
  return ok ? 0 : 1;
}
