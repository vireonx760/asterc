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
  memset(f, 0, sizeof(*f));
}
static void print_dump(void *user, const char *text, size_t length) {
  (void)fwrite(text, 1u, length, (FILE *)user);
}
static int open_fixture(Fixture *f, int windows, uint32_t limit, int promote) {
  char root[4096], entry[4096];
  memset(f, 0, sizeof(*f));
  (void)snprintf(root, sizeof(root), "%s/tests/regalloc_cases/core",
                 ASTERASM_SOURCE_DIR);
  (void)snprintf(entry, sizeof(entry), "%s/tests/regalloc_cases/core/main.aa",
                 ASTERASM_SOURCE_DIR);
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0)
    goto fail;
  const char *entries[] = {entry};
  if (ast_frontend_load(f->frontend, entries, 1u) != 0) {
    fputs("frontend failed\n", stderr);
    goto fail;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0) {
    fputs("sema failed\n", stderr);
    goto fail;
  }
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir)) {
    fputs("hir failed\n", stderr);
    goto fail;
  }
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  mc.promote_locals = (uint8_t)promote;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir)) {
    fputs("mir failed\n", stderr);
    goto fail;
  }
  AstTargetConfig tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  tc.os = windows ? AST_TARGET_OS_WINDOWS : AST_TARGET_OS_LINUX;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target)) {
    fputs("target failed\n", stderr);
    goto fail;
  }
  AstAbiConfig ac = ast_abi_config_default(windows ? AST_ABI_WINDOWS_X64
                                                   : AST_ABI_SYSV_AMD64);
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi)) {
    fputs("abi failed\n", stderr);
    goto fail;
  }
  f->machine = ast_backend_lower(f->abi, NULL);
  if (!f->machine) {
    fputs("machine failed\n", stderr);
    goto fail;
  }
  AstRegAllocConfig rc = ast_regalloc_config_default();
  rc.register_limit = limit;
  f->allocation = ast_regalloc_build(f->machine, &rc);
  if (!f->allocation || !ast_regalloc_verify(f->allocation) ||
      !ast_regalloc_finalize(f->allocation)) {
    if (f->allocation)
      ast_regalloc_dump(f->allocation, print_dump, stderr);
    goto fail;
  }
  return 1;
fail:
  close_fixture(f);
  return 0;
}
static int name_is(AstName n, const char *s) {
  return n.text && n.span.length == strlen(s) &&
         memcmp(n.text, s, n.span.length) == 0;
}
static uint32_t find_function(const Fixture *f, const char *name) {
  for (uint32_t i = 0; i < f->machine->function_count; ++i)
    if (name_is(f->machine->functions[i].info.name, name))
      return i;
  return AST_MACHINE_INVALID_ID;
}

static AstPhysicalInstructionInfo *find_opcode(AllocationFunction *af,
                                               AstMachineOpcode op) {
  for (uint32_t b = 0; b < af->block_count; ++b)
    for (uint32_t i = 0; i < af->blocks[b].count; ++i)
      if (af->blocks[b].instructions[i].opcode == op)
        return &af->blocks[b].instructions[i];
  return NULL;
}
static void make_nop(AstPhysicalInstructionInfo *i) {
  i->opcode = AST_MACHINE_X86_FENCE;
  i->operand_count = i->result_count = 0u;
}
static int insert_instruction(PhysicalBlock *block, uint32_t index,
                              const AstPhysicalInstructionInfo *instruction) {
  if (index > block->count || block->count == UINT32_MAX)
    return 0;
  size_t bytes;
  if (ast_size_mul_overflow((size_t)block->count + 1u,
                            sizeof(*block->instructions), &bytes))
    return 0;
  AstPhysicalInstructionInfo *next =
      (AstPhysicalInstructionInfo *)realloc(block->instructions, bytes);
  if (!next)
    return 0;
  block->instructions = next;
  memmove(next + index + 1u, next + index,
          (size_t)(block->count - index) * sizeof(*next));
  next[index] = *instruction;
  ++block->count;
  block->capacity = block->count;
  return 1;
}
static int simple_case(uint32_t c) {
  AstTargetConfig tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  MachinePhysRegView view;
  MachineLiveSegment as[] = {{2u, 5u}, {9u, 12u}}, bs[] = {{5u, 9u}},
                     cs[] = {{11u, 14u}};
  MachineLiveInterval a = {0}, b = {0}, d = {0};
  a.segments = as;
  a.segment_count = 2u;
  b.segments = bs;
  b.segment_count = 1u;
  d.segments = cs;
  d.segment_count = 1u;
  switch (c) {
  case 0u:
    return machine_physreg_alias(&tc, "r9", "r9b") &&
           machine_physreg_alias(&tc, "eax", "ax") &&
           !machine_physreg_alias(&tc, "rax", "rdx");
  case 1u:
    tc.features |= AST_TARGET_FEATURE_X86_AVX | AST_TARGET_FEATURE_X86_AVX512F;
    return machine_physreg_alias(&tc, "xmm7", "ymm7") &&
           machine_physreg_alias(&tc, "ymm7", "zmm7") &&
           !machine_physreg_alias(&tc, "xmm6", "xmm7");
  case 2u:
    tc.features |= AST_TARGET_FEATURE_X86_AVX;
    return machine_physreg_view(&tc, "ymm6", &view) &&
           machine_physreg_preserved_bits(AST_ABI_WINDOWS_X64, &view) == 128u &&
           machine_physreg_view(&tc, "xmm5", &view) &&
           !machine_physreg_preserved_bits(AST_ABI_WINDOWS_X64, &view);
  case 3u:
    return machine_physreg_view(&tc, "xmm6", &view) &&
           !machine_physreg_preserved_bits(AST_ABI_SYSV_AMD64, &view) &&
           machine_physreg_view(&tc, "rbx", &view) &&
           machine_physreg_preserved_bits(AST_ABI_SYSV_AMD64, &view) == 64u;
  case 4u:
    return !machine_intervals_overlap(&a, &b) &&
           machine_intervals_overlap(&a, &d) &&
           machine_intervals_overlap(&d, &a);
  case 5u:
    return machine_interval_covers(&a, 2u) && machine_interval_covers(&a, 4u) &&
           !machine_interval_covers(&a, 5u) &&
           !machine_interval_covers(&a, 8u) &&
           machine_interval_covers(&a, 9u) && !machine_interval_covers(&a, 12u);
  default:
    return 0;
  }
}

static int mutate_allocation(Fixture *f, uint32_t c) {
  uint32_t function =
      find_function(f, c >= 21u && c <= 23u ? "loop_sum" : "call9");
  if (function == AST_MACHINE_INVALID_ID)
    return 0;
  AllocationFunction *af = &f->allocation->functions[function];
  MachineFunction *mf = &f->machine->functions[function];
  uint32_t v = 0u;
  while (v < mf->info.vreg_count &&
         af->locations[v].kind != AST_REGALLOC_LOCATION_REGISTER)
    ++v;
  if (v == mf->info.vreg_count)
    return 0;
  switch (c) {
  case 6u:
    mf->info.entry_block = mf->info.block_count;
    return !machine_cfg_validate(mf);
  case 7u:
    if (mf->blocks[0].info.instruction_count < 2u)
      return 0;
    mf->blocks[0].instructions[1] = mf->blocks[0].instructions[0];
    return !machine_cfg_validate(mf);
  case 8u:
    --mf->blocks[0].info.instruction_count;
    return !machine_cfg_validate(mf);
  case 9u: {
    function = find_function(f, "choose");
    mf = &f->machine->functions[function];
    for (size_t o = 0; o < mf->operand_count; ++o)
      if (mf->operands[o].kind == AST_MACHINE_OPERAND_BLOCK) {
        mf->operands[o].as.block = mf->info.block_count;
        return !machine_cfg_validate(mf);
      }
    return 0;
  }
  case 10u:
    for (size_t o = 0; o < mf->operand_count; ++o)
      if (mf->operands[o].kind == AST_MACHINE_OPERAND_VREG) {
        mf->operands[o].as.vreg = mf->info.vreg_count;
        return !machine_cfg_validate(mf);
      }
    return 0;
  case 11u:
    for (uint32_t i = 0; i < mf->info.instruction_count; ++i)
      if (mf->instructions[i].info.result_count) {
        mf->instructions[i].info.result_first = mf->info.vreg_count;
        return !machine_cfg_validate(mf);
      }
    return 0;
  case 12u:
    af->liveness.blocks[0].live_in.words[0] ^= UINT64_C(1) << v;
    break;
  case 13u:
    af->liveness.blocks[0].live_out.words[0] ^= UINT64_C(1) << v;
    break;
  case 14u:
    af->liveness.intervals[v].segments[0].end++;
    break;
  case 15u:
    while (v < mf->info.vreg_count && !af->liveness.intervals[v].use_count)
      ++v;
    if (v == mf->info.vreg_count)
      return 0;
    af->liveness.intervals[v].uses[0].fixed ^= 1u;
    break;
  case 16u:
    af->liveness.instruction_slots[0]++;
    break;
  case 17u:
    af->locations[v].bank = AST_MACHINE_BANK_FP;
    break;
  case 18u:
    af->locations[v].bit_width = 32u;
    break;
  case 19u:
    af->locations[v].register_unit++;
    break;
  case 20u:
    memcpy(af->locations[v].register_name, "rsp", 4u);
    af->locations[v].register_unit = MACHINE_PHYSREG_FILE_GPR * 64u + 4u;
    break;
  case 21u:
  case 22u:
  case 23u:
    v = 0u;
    while (v < mf->info.vreg_count &&
           af->locations[v].kind != (c == 23u
                                         ? AST_REGALLOC_LOCATION_REMATERIALIZE
                                         : AST_REGALLOC_LOCATION_SPILL))
      ++v;
    if (v == mf->info.vreg_count)
      return 0;
    if (c == 21u)
      af->locations[v].frame_object = af->object_count;
    if (c == 22u)
      af->objects[af->locations[v].frame_object].size = 1u;
    if (c == 23u)
      af->locations[v].immediate_bits ^= UINT64_C(1);
    break;
  case 24u:
    for (uint32_t x = 0; x < mf->info.vreg_count; ++x)
      for (uint32_t y = x + 1u; y < mf->info.vreg_count; ++y)
        if (af->locations[x].kind == AST_REGALLOC_LOCATION_REGISTER &&
            af->locations[y].kind == AST_REGALLOC_LOCATION_REGISTER &&
            af->locations[x].bit_width == af->locations[y].bit_width &&
            machine_intervals_overlap(&af->liveness.intervals[x],
                                      &af->liveness.intervals[y])) {
          af->locations[y] = af->locations[x];
          return !ast_regalloc_verify(f->allocation);
        }
    return 0;
  default:
    return 0;
  }
  return !ast_regalloc_verify(f->allocation);
}

static int mutate_call_transfer(Fixture *f, AllocationFunction *af,
                                uint32_t mode) {
  uint32_t function = (uint32_t)(af - f->allocation->functions);
  MachineFunction *mf = &f->machine->functions[function];
  for (uint32_t b = 0; b < af->block_count; ++b)
    for (uint32_t i = 0; i < af->blocks[b].count; ++i) {
      AstPhysicalInstructionInfo *call = &af->blocks[b].instructions[i];
      if (call->opcode != AST_MACHINE_X86_CALL)
        continue;
      uint32_t slot = af->liveness.instruction_slots[call->source_instruction];
      for (uint32_t v = 0; v < mf->info.vreg_count; ++v) {
        AstRegAllocLocation *loc = &af->locations[v];
        MachinePhysRegView view;
        if (loc->kind != AST_REGALLOC_LOCATION_REGISTER ||
            !machine_interval_covers(&af->liveness.intervals[v], slot) ||
            !machine_interval_covers(&af->liveness.intervals[v], slot + 1u) ||
            !machine_physreg_view(&f->allocation->target, loc->register_name,
                                  &view) ||
            machine_physreg_preserved_bits(f->allocation->abi, &view) >=
                loc->bit_width)
          continue;
        uint32_t begin = i;
        while (begin &&
               af->blocks[b].instructions[begin - 1u].source_instruction ==
                   call->source_instruction)
          --begin;
        int restore = mode == 1u || mode == 2u || mode == 4u;
        for (uint32_t j = restore ? i + 1u : begin; j < af->blocks[b].count;
             ++j) {
          AstPhysicalInstructionInfo *transfer = &af->blocks[b].instructions[j];
          if (transfer->source_instruction != call->source_instruction)
            break;
          if (transfer->opcode ==
                  (restore ? AST_MACHINE_X86_LOAD : AST_MACHINE_X86_STORE) &&
              transfer->operands[0].kind == AST_PHYSICAL_OPERAND_REGISTER &&
              transfer->operands[0].register_unit == loc->register_unit) {
            if (mode <= 1u)
              make_nop(transfer);
            else if (mode == 2u)
              transfer->operands[1].displacement += 8;
            else {
              AstPhysicalInstructionInfo clobber = *transfer;
              clobber.opcode =
                  mode == 3u ? AST_MACHINE_X86_STORE : AST_MACHINE_X86_MOV_IMM;
              clobber.result_count = mode == 3u ? 0u : 1u;
              if (mode == 3u)
                clobber.operands[0] = physical_immediate(0u, loc->bit_width);
              else
                clobber.operands[1] = physical_immediate(0u, loc->bit_width);
              if (!insert_instruction(&af->blocks[b], j + 1u, &clobber))
                return 0;
            }
            return 1;
          }
        }
      }
    }
  return 0;
}
static int mutate_physical(Fixture *f, uint32_t c) {
  const char *name = c <= 26u   ? "shift_left"
                     : c <= 29u ? "divide"
                     : c == 30u ? "signed_divide"
                     : c == 36u ? "choose"
                     : (c >= 42u && c <= 44u) || (c >= 71u && c <= 73u)
                         ? "call_live"
                     : c == 45u ? "divide"
                     : c == 46u ? "triple_call"
                                : "call9";
  uint32_t function = find_function(f, name);
  if (function == AST_MACHINE_INVALID_ID)
    return 0;
  AllocationFunction *af = &f->allocation->functions[function];
  AstPhysicalInstructionInfo *i = NULL;
  PhysicalContext context = {.program = f->allocation,
                             .fn = af,
                             .machine = &f->machine->functions[function],
                             .function = function,
                             .block = 0u,
                             .instruction = 0u};
  switch (c) {
  case 25u:
  case 26u:
    i = find_opcode(af, AST_MACHINE_X86_SHL);
    if (!i)
      return 0;
    i->operands[2] = physical_register(&context, AST_MACHINE_BANK_GPR,
                                       c == 25u ? 2u : 1u, c == 25u ? 8u : 64u);
    break;
  case 27u:
  case 28u:
  case 29u:
    i = find_opcode(af, AST_MACHINE_X86_UDIV);
    if (!i)
      return 0;
    i->operands[c == 27u   ? 0u
                : c == 28u ? 3u
                           : 4u] =
        physical_register(&context, AST_MACHINE_BANK_GPR, c == 29u ? 11u : 1u,
                          c == 29u ? 32u : 64u);
    break;
  case 30u:
    i = find_opcode(af, AST_MACHINE_X86_CQO);
    if (!i)
      return 0;
    i->operands[1] = physical_register(&context, AST_MACHINE_BANK_GPR, 2u, 64u);
    break;
  case 31u:
  case 34u:
    i = find_opcode(af, AST_MACHINE_X86_LOAD);
    if (!i)
      return 0;
    if (c == 31u)
      memcpy(i->operands[1].register_name, "@frame", 7u);
    else
      i->operands[1].bit_width = 32u;
    break;
  case 32u:
    i = find_opcode(af, AST_MACHINE_X86_SUB);
    if (!i)
      return 0;
    memcpy(i->operands[2].register_name, "@adjust", 8u);
    break;
  case 33u:
    af->blocks[0].instructions[0].opcode = AST_MACHINE_X86_SREM;
    break;
  case 35u:
    i = find_opcode(af, AST_MACHINE_X86_LOAD);
    if (!i)
      return 0;
    i->result_count = 0u;
    break;
  case 36u:
    i = find_opcode(af, AST_MACHINE_X86_COND_BR);
    if (!i)
      return 0;
    i->operands[1].block = af->block_count;
    break;
  case 37u:
    i = &af->blocks[0].instructions[0];
    i->opcode = AST_MACHINE_X86_RET;
    i->operand_count = i->result_count = 0u;
    break;
  case 38u:
  case 39u:
  case 40u:
  case 41u: {
    uint32_t unit = 0u;
    while (unit < MACHINE_REGISTER_UNITS &&
           af->callee_save_objects[unit] == AST_MACHINE_INVALID_ID)
      ++unit;
    if (unit == MACHINE_REGISTER_UNITS)
      return 0;
    if (c == 40u) {
      af->callee_save_objects[unit] = AST_MACHINE_INVALID_ID;
      break;
    }
    if (c == 41u) {
      af->callee_save_objects[unit] = af->object_count;
      break;
    }
    int changed = 0;
    for (uint32_t b = 0; b < af->block_count && !changed; ++b)
      for (uint32_t n = 0; n < af->blocks[b].count; ++n) {
        i = &af->blocks[b].instructions[n];
        if (i->opcode ==
                (c == 38u ? AST_MACHINE_X86_STORE : AST_MACHINE_X86_LOAD) &&
            i->operands[0].register_unit == unit &&
            i->operands[1].displacement ==
                af->objects[af->callee_save_objects[unit]].offset) {
          make_nop(i);
          changed = 1;
          break;
        }
      }
    if (!changed)
      return 0;
    break;
  }
  case 42u:
  case 43u:
    if (!mutate_call_transfer(f, af, c == 43u))
      return 0;
    break;
  case 44u:
    if (f->allocation->abi != AST_ABI_WINDOWS_X64)
      return 0;
    af->frame.outgoing_size = 16u;
    break;
  case 45u:
    if (!af->frame.uses_red_zone || !af->object_count)
      return 0;
    af->objects[0].offset++;
    break;
  case 46u:
    if (af->object_count < 2u)
      return 0;
    af->objects[1].offset = af->objects[0].offset;
    break;
  case 47u:
    if (!af->object_count)
      return 0;
    af->objects[0].id++;
    break;
  case 48u:
    if (!af->object_count)
      return 0;
    af->objects[0].size = UINT64_MAX;
    break;
  case 49u:
    i = find_opcode(af, AST_MACHINE_X86_SUB);
    if (!i)
      return 0;
    i->opcode = AST_MACHINE_X86_MOV_IMM;
    i->operands[1] = i->operands[2];
    i->operand_count = 2u;
    break;
  case 50u:
    af->frame.needs_stack_probe ^= 1u;
    break;
  case 51u:
    /* Locate the epilogue stack adjustment, not arithmetic in the body. */
    i = NULL;
    for (uint32_t b = 0; b < af->block_count; ++b)
      for (uint32_t n = 0; n < af->blocks[b].count; ++n) {
        AstPhysicalInstructionInfo *candidate = &af->blocks[b].instructions[n];
        if (candidate->opcode == AST_MACHINE_X86_ADD &&
            candidate->operands[0].register_unit ==
                MACHINE_PHYSREG_FILE_GPR * 64u + 4u)
          i = candidate;
      }
    if (!i)
      return 0;
    i->operands[2].immediate_bits += 8u;
    break;
  case 63u:
  case 64u:
  case 65u:
  case 66u:
    i = find_opcode(af, AST_MACHINE_X86_LOAD);
    if (!i)
      return 0;
    if (c == 63u)
      i->operands[0].bank = AST_MACHINE_BANK_FP;
    if (c == 64u)
      i->operands[0].register_unit++;
    if (c == 65u)
      memset(i->operands[0].register_name, 'r',
             sizeof(i->operands[0].register_name));
    if (c == 66u)
      memcpy(i->operands[1].register_name, "esp", 4u);
    break;
  case 67u:
  case 68u:
  case 69u:
  case 70u:
  case 74u:
  case 75u: {
    uint32_t unit = 0u;
    while (unit < MACHINE_REGISTER_UNITS &&
           af->callee_save_objects[unit] == AST_MACHINE_INVALID_ID)
      ++unit;
    if (unit == MACHINE_REGISTER_UNITS)
      return 0;
    uint32_t object = af->callee_save_objects[unit];
    int changed = 0;
    AstMachineOpcode transfer = c == 69u || c == 70u || c == 75u
                                    ? AST_MACHINE_X86_LOAD
                                    : AST_MACHINE_X86_STORE;
    for (uint32_t b = 0; b < af->block_count && !changed; ++b)
      for (uint32_t n = 0; n < af->blocks[b].count; ++n) {
        i = &af->blocks[b].instructions[n];
        if (i->opcode != transfer || i->operands[0].register_unit != unit ||
            i->operands[1].displacement != af->objects[object].offset)
          continue;
        if (c == 74u || c == 75u) {
          i->operands[0] = physical_register(&context, AST_MACHINE_BANK_GPR,
                                             unit % 64u, 32u);
          i->operands[1].bit_width = 32u;
        } else if (c == 70u) {
          uint32_t add = n + 1u;
          while (
              add < af->blocks[b].count &&
              !(af->blocks[b].instructions[add].opcode == AST_MACHINE_X86_ADD &&
                af->blocks[b].instructions[add].operands[0].register_unit ==
                    MACHINE_PHYSREG_FILE_GPR * 64u + 4u))
            ++add;
          if (add == af->blocks[b].count)
            return 0;
          AstPhysicalInstructionInfo saved = *i;
          *i = af->blocks[b].instructions[add];
          af->blocks[b].instructions[add] = saved;
        } else {
          AstPhysicalInstructionInfo clobber = *i;
          clobber.opcode =
              c == 67u ? AST_MACHINE_X86_STORE : AST_MACHINE_X86_MOV_IMM;
          clobber.result_count = c == 67u ? 0u : 1u;
          if (c == 67u)
            clobber.operands[0] =
                physical_immediate(0u, i->operands[0].bit_width);
          else
            clobber.operands[1] =
                physical_immediate(0u, i->operands[0].bit_width);
          if (!insert_instruction(&af->blocks[b], n + (c == 68u ? 0u : 1u),
                                  &clobber))
            return 0;
        }
        changed = 1;
        break;
      }
    if (!changed)
      return 0;
    break;
  }
  case 71u:
  case 72u:
  case 73u:
    if (!mutate_call_transfer(f, af, c - 69u))
      return 0;
    break;
  case 76u:
    i = find_opcode(af, AST_MACHINE_X86_CALL);
    if (!i)
      return 0;
    i->source_instruction =
        f->machine->functions[function].info.instruction_count;
    break;
  case 77u:
    af->blocks[0].instructions[0].opcode = AST_MACHINE_X86_UNDEF;
    break;
  case 78u:
    af->blocks[0].instructions[0].operand_count =
        AST_PHYSICAL_MAX_OPERANDS + 1u;
    break;
  case 79u:
    i = find_opcode(af, AST_MACHINE_X86_LOAD);
    if (!i)
      return 0;
    i->operands[0] = i->operands[1];
    break;
  case 80u:
  case 81u:
  case 82u:
  case 83u:
    if (!af->object_count)
      return 0;
    if (c == 80u)
      af->objects[0].alignment = 3u;
    if (c == 81u)
      af->objects[0].alignment = 32u;
    if (c == 82u)
      af->objects[0].offset = -8;
    if (c == 83u)
      af->objects[0].offset = (int64_t)af->frame.stack_adjustment;
    break;
  default:
    return 0;
  }
  return !ast_physical_verify(f->allocation);
}

static int frame_case(uint32_t c) {
  AstRegAllocProgram p = {0};
  AllocationFunction af = {0};
  int ok = 0;
  p.functions = &af;
  p.function_count = 1u;
  p.target = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  p.abi = c == 54u ? AST_ABI_WINDOWS_X64 : AST_ABI_SYSV_AMD64;
  p.config = ast_regalloc_config_default();
  uint32_t a = frame_add_object(&af, AST_FRAME_OBJECT_LOCAL,
                                c == 52u   ? 16u
                                : c == 53u ? 128u
                                           : 8200u,
                                c == 52u ? 16u : 8u);
  if (a == AST_MACHINE_INVALID_ID || !frame_layout(&p, 0u))
    goto done;
  if (c == 52u)
    ok = af.frame.uses_red_zone && !af.frame.stack_adjustment &&
         af.objects[0].offset == -24;
  if (c == 53u)
    ok = af.frame.uses_red_zone && !af.frame.stack_adjustment &&
         af.objects[0].offset == -128;
  if (c == 54u) {
    MachineFunction machine = {0};
    PhysicalContext pc = {.program = &p,
                          .fn = &af,
                          .machine = &machine,
                          .function = 0u,
                          .block = 0u,
                          .instruction = AST_MACHINE_INVALID_ID};
    af.blocks = (PhysicalBlock *)calloc(1u, sizeof(*af.blocks));
    af.block_count = 1u;
    af.callee_save_objects =
        (uint32_t *)malloc(MACHINE_REGISTER_UNITS * sizeof(uint32_t));
    if (!af.blocks || !af.callee_save_objects)
      goto done;
    for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u)
      af.callee_save_objects[u] = AST_MACHINE_INVALID_ID;
    if (!physical_emit_prologue(&pc))
      goto done;
    uint64_t adjustment = 0u;
    ok = af.frame.needs_stack_probe && af.blocks[0].count == 6u;
    for (uint32_t i = 0; i < af.blocks[0].count && ok; i += 2u) {
      const AstPhysicalInstructionInfo *sub = &af.blocks[0].instructions[i],
                                       *probe = sub + 1;
      ok = sub->opcode == AST_MACHINE_X86_SUB &&
           sub->operands[2].immediate_bits <= 4096u &&
           probe->opcode == AST_MACHINE_X86_LOAD &&
           probe->operands[1].displacement == 0;
      adjustment += sub->operands[2].immediate_bits;
    }
    ok = ok && adjustment == af.frame.stack_adjustment;
  }
done:
  free(af.objects);
  free(af.callee_save_objects);
  physical_release_function(&af);
  return ok;
}

/* Execute only the move language emitted by parallel-copy lowering. The oracle
   snapshots source bytes AND destination addresses before any assignment. */
typedef struct CopyState {
  unsigned char regs[MACHINE_REGISTER_UNITS][64];
  unsigned char memory[8192];
} CopyState;
static unsigned char *copy_address(CopyState *s, const AstPhysicalOperand *op) {
  size_t bytes = (op->bit_width + 7u) / 8u;
  if (!bytes || bytes > 64u || op->register_unit >= MACHINE_REGISTER_UNITS)
    return NULL;
  if (op->kind == AST_PHYSICAL_OPERAND_REGISTER)
    return s->regs[op->register_unit];
  if (op->kind != AST_PHYSICAL_OPERAND_MEMORY)
    return NULL;
  uint64_t base = 0u;
  memcpy(&base, s->regs[op->register_unit], sizeof(base));
  int64_t address = (int64_t)base + op->displacement;
  if (address < 0 || (uint64_t)address > sizeof(s->memory) - bytes)
    return NULL;
  return s->memory + (size_t)address;
}
static void copy_write(CopyState *s, const AstPhysicalOperand *dst,
                       const unsigned char *bytes) {
  unsigned char *address = copy_address(s, dst);
  size_t count = (dst->bit_width + 7u) / 8u;
  if (!address)
    return;
  memcpy(address, bytes, count);
  if (dst->kind == AST_PHYSICAL_OPERAND_REGISTER &&
      dst->register_unit / 64u == MACHINE_PHYSREG_FILE_GPR &&
      dst->bit_width == 32u)
    memset(address + 4u, 0, 4u);
}
static int copy_case(uint32_t c) {
  AstRegAllocProgram p = {0};
  AllocationFunction af = {0};
  MachineFunction mf = {0};
  PhysicalContext pc = {.program = &p,
                        .fn = &af,
                        .machine = &mf,
                        .function = 0u,
                        .block = 0u,
                        .instruction = AST_MACHINE_INVALID_ID};
  CopyState actual = {0}, expected = {0};
  PhysicalCopy copies[3];
  uint32_t count = 2u;
  int ok = 0;
  p.functions = &af;
  p.function_count = 1u;
  p.target = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  p.abi = AST_ABI_SYSV_AMD64;
  p.config = ast_regalloc_config_default();
  af.copy_temp = AST_MACHINE_INVALID_ID;
  af.blocks = (PhysicalBlock *)calloc(1u, sizeof(*af.blocks));
  af.block_count = 1u;
  af.emergency_objects =
      (uint32_t *)malloc(MACHINE_REGISTER_UNITS * sizeof(uint32_t));
  if (!af.blocks || !af.emergency_objects)
    goto done;
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u) {
    af.emergency_objects[u] = AST_MACHINE_INVALID_ID;
    for (uint32_t byte = 0; byte < 64u; ++byte)
      actual.regs[u][byte] = (unsigned char)(u * 7u + byte * 13u);
  }
  uint64_t stack = 4104u, pointer = 128u;
  memcpy(actual.regs[MACHINE_PHYSREG_FILE_GPR * 64u + 4u], &stack,
         sizeof(stack));
  for (size_t i = 0; i < sizeof(actual.memory); ++i)
    actual.memory[i] = (unsigned char)(i * 17u + i / 7u);
  AstPhysicalOperand rax =
      physical_register(&pc, AST_MACHINE_BANK_GPR, 0u, 64u);
  AstPhysicalOperand rdx =
      physical_register(&pc, AST_MACHINE_BANK_GPR, 2u, 64u);
  AstPhysicalOperand r8 = physical_register(&pc, AST_MACHINE_BANK_GPR, 8u, 64u);
  AstPhysicalOperand r9 = physical_register(&pc, AST_MACHINE_BANK_GPR, 9u, 64u);
  copies[0] = (PhysicalCopy){rax, rdx};
  copies[1] = (PhysicalCopy){rdx, rax};
  if (c == 56u) {
    copies[1].src = r8;
    copies[2] = (PhysicalCopy){r8, rax};
    count = 3u;
  }
  if (c == 57u) {
    copies[0].dst = physical_register(&pc, AST_MACHINE_BANK_GPR, 0u, 32u);
    copies[0].src = physical_register(&pc, AST_MACHINE_BANK_GPR, 2u, 32u);
  }
  if (c == 58u || c == 59u) {
    AstPhysicalOperand a = physical_stack(&pc, c == 59u ? 128u : 64u, 128);
    AstPhysicalOperand b =
        physical_stack(&pc, c == 59u ? 128u : 64u, c == 59u ? 136 : 152);
    copies[0] = (PhysicalCopy){a, b};
    copies[1] = (PhysicalCopy){b, a};
  }
  if (c == 60u || c == 61u) {
    memcpy(actual.regs[rax.register_unit], &pointer, sizeof(pointer));
    AstPhysicalOperand memory = rax;
    memory.kind = AST_PHYSICAL_OPERAND_MEMORY;
    copies[0] = (PhysicalCopy){rax, r8};
    copies[1] =
        c == 60u ? (PhysicalCopy){r9, memory} : (PhysicalCopy){memory, r9};
  }
  if (c == 62u) {
    AstPhysicalOperand xmm0 =
        physical_register(&pc, AST_MACHINE_BANK_VECTOR, 0u, 128u);
    AstPhysicalOperand xmm1 =
        physical_register(&pc, AST_MACHINE_BANK_VECTOR, 1u, 128u);
    copies[0] = (PhysicalCopy){xmm0, xmm1};
    copies[1] = (PhysicalCopy){xmm1, xmm0};
  }
  expected = actual;
  unsigned char sources[3][64] = {{0}};
  ptrdiff_t destinations[3] = {0};
  for (uint32_t i = 0; i < count; ++i) {
    unsigned char *src = copy_address(&actual, &copies[i].src),
                  *dst = copy_address(&expected, &copies[i].dst);
    if (!src || !dst)
      goto done;
    memcpy(sources[i], src, (copies[i].src.bit_width + 7u) / 8u);
    destinations[i] = dst - (unsigned char *)&expected;
  }
  for (uint32_t i = 0; i < count; ++i) {
    unsigned char *dst = (unsigned char *)&expected + destinations[i];
    memcpy(dst, sources[i], (copies[i].dst.bit_width + 7u) / 8u);
    if (copies[i].dst.kind == AST_PHYSICAL_OPERAND_REGISTER &&
        copies[i].dst.register_unit / 64u == MACHINE_PHYSREG_FILE_GPR &&
        copies[i].dst.bit_width == 32u)
      memset(dst + 4u, 0, 4u);
  }
  if (!physical_parallel_copy(&pc, copies, count) || !frame_layout(&p, 0u) ||
      !physical_resolve_frames(&pc))
    goto done;
  for (uint32_t i = 0; i < af.blocks[0].count; ++i) {
    const AstPhysicalInstructionInfo *inst = &af.blocks[0].instructions[i];
    const AstPhysicalOperand *src, *dst;
    if (inst->opcode == AST_MACHINE_X86_STORE) {
      src = &inst->operands[0];
      dst = &inst->operands[1];
    } else if (inst->opcode == AST_MACHINE_X86_LOAD ||
               inst->opcode == AST_MACHINE_X86_MOV) {
      src = &inst->operands[1];
      dst = &inst->operands[0];
    } else
      goto done;
    unsigned char *address = copy_address(&actual, src);
    unsigned char value[64];
    if (!address || !copy_address(&actual, dst))
      goto done;
    memcpy(value, address, (src->bit_width + 7u) / 8u);
    copy_write(&actual, dst, value);
  }
  ok = !memcmp(actual.regs, expected.regs, sizeof(actual.regs));
  /* The frame is private scratch; all user memory outside it stays intact. */
  for (size_t i = 0; i < sizeof(actual.memory) && ok; ++i)
    if ((i < stack - 128u || i >= stack) &&
        actual.memory[i] != expected.memory[i])
      ok = 0;
done:
  free(af.objects);
  free(af.emergency_objects);
  physical_release_function(&af);
  return ok;
}

static const char *const cases[] = {"gpr_aliases",
                                    "simd_aliases",
                                    "win64_partial_simd_preservation",
                                    "sysv_preservation",
                                    "interval_holes",
                                    "interval_half_open",
                                    "cfg_bad_entry",
                                    "cfg_duplicate_instruction",
                                    "cfg_orphan_instruction",
                                    "cfg_bad_successor",
                                    "cfg_bad_use",
                                    "cfg_bad_definition",
                                    "liveness_live_in_mutation",
                                    "liveness_live_out_mutation",
                                    "liveness_segment_mutation",
                                    "liveness_fixed_use_mutation",
                                    "liveness_slot_mutation",
                                    "allocation_bank_mutation",
                                    "allocation_width_mutation",
                                    "allocation_alias_unit_mutation",
                                    "allocation_reserved_rsp",
                                    "spill_object_out_of_bounds",
                                    "spill_slot_too_small",
                                    "rematerialization_payload_mutation",
                                    "overlapping_register_homes",
                                    "shift_wrong_cl_alias",
                                    "shift_wrong_cl_width",
                                    "division_wrong_quotient",
                                    "division_wrong_high_dividend",
                                    "division_wrong_divisor_width",
                                    "sign_extension_wrong_rax",
                                    "unresolved_frame_pseudo",
                                    "unresolved_adjust_pseudo",
                                    "unexpanded_remainder_pseudo",
                                    "load_width_mismatch",
                                    "load_result_count",
                                    "physical_bad_successor",
                                    "early_return_terminator",
                                    "missing_callee_save",
                                    "missing_callee_restore",
                                    "missing_callee_save_metadata",
                                    "callee_save_object_out_of_bounds",
                                    "missing_call_live_save",
                                    "missing_call_live_reload",
                                    "win64_shadow_space",
                                    "redzone_alignment_mutation",
                                    "frame_overlap",
                                    "frame_id_mutation",
                                    "frame_size_overflow",
                                    "illegal_rsp_definition",
                                    "stack_probe_flag_mutation",
                                    "unbalanced_epilogue",
                                    "redzone_16_byte_alignment",
                                    "redzone_128_byte_boundary",
                                    "win64_page_probes",
                                    "parallel_register_swap",
                                    "parallel_three_register_cycle",
                                    "parallel_subregister_alias_cycle",
                                    "parallel_memory_swap",
                                    "parallel_overlapping_memory",
                                    "parallel_source_address_alias",
                                    "parallel_destination_address_alias",
                                    "parallel_simd_swap",
                                    "physical_wrong_result_bank",
                                    "physical_alias_unit_mismatch",
                                    "physical_unterminated_register_name",
                                    "physical_narrow_address_base",
                                    "callee_save_slot_clobbered",
                                    "callee_save_after_first_clobber",
                                    "callee_restore_before_last_clobber",
                                    "callee_restore_after_sp_adjustment",
                                    "call_live_reload_wrong_slot",
                                    "call_live_save_slot_clobbered",
                                    "call_live_reload_overwritten",
                                    "callee_save_wrong_width",
                                    "callee_restore_wrong_width",
                                    "call_source_out_of_bounds",
                                    "unexpanded_undef_pseudo",
                                    "physical_operand_count_overflow",
                                    "physical_memory_result",
                                    "frame_non_power_two_alignment",
                                    "frame_overalignment",
                                    "frame_negative_offset",
                                    "frame_offset_out_of_bounds"};
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc != 2)
    return 2;
  if (!strcmp(argv[1], "--list")) {
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c)
      printf("%zu %s\n", c, cases[c]);
    return 0;
  }
  char *end = NULL;
  long parsed = strtol(argv[1], &end, 10);
  if (!end || end == argv[1] || *end || parsed < 0 ||
      (unsigned long)parsed >= sizeof(cases) / sizeof(cases[0]))
    return 2;
  uint32_t c = (uint32_t)parsed;
  int ok;
  if (c < 6u)
    ok = simple_case(c);
  else if (c >= 55u && c <= 62u)
    ok = copy_case(c);
  else if (c >= 52u && c <= 54u)
    ok = frame_case(c);
  else {
    Fixture f;
    uint32_t limit =
        (c >= 21u && c <= 23u) || c == 42u || c == 43u || (c >= 71u && c <= 73u)
            ? 1u
            : 0u;
    if (!open_fixture(&f, c == 44u, limit, 1))
      return 1;
    ok = c <= 24u ? mutate_allocation(&f, c) : mutate_physical(&f, c);
    close_fixture(&f);
  }
  if (!ok)
    fprintf(stderr, "regalloc check %u (%s) failed\n", c, cases[c]);
  return ok ? 0 : 1;
}
