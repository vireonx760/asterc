#include "../src/backend/regalloc/allocation_internal.h"
#include "asterasm/asterasm.h"
#include "machine_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

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

static void fixture_close(Fixture *f) {
  if (!f)
    return;
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

static AstTargetArch fixture_arch = AST_TARGET_ARCH_X86_64;
static int fixture_open(Fixture *f, const char *relative_case) {
  char root[4096], entry[4096];
  memset(f, 0, sizeof(*f));
  if (snprintf(root, sizeof(root), "%s/%s", ASTERASM_SOURCE_DIR,
               relative_case) <= 0 ||
      snprintf(entry, sizeof(entry), "%s/main.aa", root) <= 0)
    return 0;

  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 2u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0)
    goto fail;
  const char *entries[] = {entry};
  if (ast_frontend_load(f->frontend, entries, 1u) != 0)
    goto fail;

  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 2u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0)
    goto fail;

  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 2u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir))
    goto fail;

  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 2u;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir))
    goto fail;

  AstTargetConfig tc = ast_target_config_default(fixture_arch);
  if (fixture_arch == AST_TARGET_ARCH_RISCV64)
    tc.features |= AST_TARGET_FEATURE_RV_A;
  tc.os = AST_TARGET_OS_LINUX;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target))
    goto fail;

  AstAbiConfig ac = ast_abi_config_default(ast_abi_default_for_target(&tc));
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi))
    goto fail;

  AstBackendConfig bc = ast_backend_config_default();
  f->machine = ast_backend_lower(f->abi, &bc);
  if (!f->machine || !ast_machine_verify(f->machine))
    goto fail;
  return 1;
fail:
  fixture_close(f);
  return 0;
}

static int check_atomic_orders_hir(const AstHirProgram *hir) {
  const AstMemoryOrder want[][2] = {
      {AST_MEMORY_ORDER_ACQUIRE, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_RELEASE, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_ACQ_REL, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_ACQ_REL, AST_MEMORY_ORDER_ACQUIRE}};
  uint32_t seen = 0u;
  for (size_t fi = 0; fi < ast_hir_function_count(hir); ++fi) {
    const AstHirFunction *fn = ast_hir_function_at(hir, fi);
    AstHirFunctionInfo info;
    if (!fn || !ast_hir_function_info(fn, &info))
      return 0;
    for (uint32_t ii = 0u; ii < info.instruction_count; ++ii) {
      AstHirInstructionInfo inst;
      if (!ast_hir_function_instruction(fn, ii, &inst))
        return 0;
      if (inst.op != AST_HIR_OP_ATOMIC)
        continue;
      if (seen >= 4u || inst.memory_order != want[seen][0] ||
          inst.failure_memory_order != want[seen][1])
        return 0;
      seen++;
    }
  }
  return seen == 4u;
}

static int check_atomic_orders_mir(const AstMirProgram *mir) {
  const AstMemoryOrder want[][2] = {
      {AST_MEMORY_ORDER_ACQUIRE, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_RELEASE, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_ACQ_REL, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_ACQ_REL, AST_MEMORY_ORDER_ACQUIRE}};
  uint32_t seen = 0u;
  for (size_t fi = 0; fi < ast_mir_function_count(mir); ++fi) {
    const AstMirFunction *fn = ast_mir_function_at(mir, fi);
    AstMirFunctionInfo info;
    if (!fn || !ast_mir_function_info(fn, &info))
      return 0;
    for (uint32_t ii = 0u; ii < info.instruction_count; ++ii) {
      AstMirInstructionInfo inst;
      if (!ast_mir_function_instruction(fn, ii, &inst))
        return 0;
      if (inst.op != AST_MIR_OP_ATOMIC)
        continue;
      if (seen >= 4u || inst.memory_order != want[seen][0] ||
          inst.failure_memory_order != want[seen][1])
        return 0;
      seen++;
    }
  }
  return seen == 4u;
}

static int check_atomic_orders_machine(const AstMachineProgram *p) {
  const AstMemoryOrder want[][2] = {
      {AST_MEMORY_ORDER_ACQUIRE, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_RELEASE, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_ACQ_REL, AST_MEMORY_ORDER_NONE},
      {AST_MEMORY_ORDER_ACQ_REL, AST_MEMORY_ORDER_ACQUIRE}};
  uint32_t seen = 0u;
  for (size_t fi = 0; fi < ast_machine_function_count(p); ++fi) {
    AstMachineFunctionInfo info;
    if (!ast_machine_function_info(p, fi, &info))
      return 0;
    for (uint32_t ii = 0u; ii < info.instruction_count; ++ii) {
      AstMachineInstructionInfo inst;
      if (!ast_machine_function_instruction(p, fi, ii, &inst))
        return 0;
      if (inst.source_op != AST_MIR_OP_ATOMIC)
        continue;
      if (seen >= 4u || inst.opcode != AST_MACHINE_X86_ATOMIC ||
          inst.memory_order != want[seen][0] ||
          inst.failure_memory_order != want[seen][1])
        return 0;
      seen++;
    }
  }
  return seen == 4u;
}

static int check_fence_orders(const Fixture *f) {
  AstMemoryOrder hir_orders[2] = {AST_MEMORY_ORDER_NONE, AST_MEMORY_ORDER_NONE};
  AstMemoryOrder mir_orders[2] = {AST_MEMORY_ORDER_NONE, AST_MEMORY_ORDER_NONE};
  AstMemoryOrder machine_orders[2] = {AST_MEMORY_ORDER_NONE,
                                      AST_MEMORY_ORDER_NONE};
  uint32_t hs = 0u, ms = 0u, xs = 0u;
  for (size_t fi = 0; fi < ast_hir_function_count(f->hir); ++fi) {
    const AstHirFunction *fn = ast_hir_function_at(f->hir, fi);
    AstHirFunctionInfo info;
    if (!fn || !ast_hir_function_info(fn, &info))
      return 0;
    for (uint32_t ii = 0; ii < info.instruction_count; ++ii) {
      AstHirInstructionInfo x;
      if (!ast_hir_function_instruction(fn, ii, &x))
        return 0;
      if (x.op == AST_HIR_OP_FENCE) {
        if (hs >= 2u || x.failure_memory_order != AST_MEMORY_ORDER_NONE)
          return 0;
        hir_orders[hs++] = x.memory_order;
      }
    }
  }
  for (size_t fi = 0; fi < ast_mir_function_count(f->mir); ++fi) {
    const AstMirFunction *fn = ast_mir_function_at(f->mir, fi);
    AstMirFunctionInfo info;
    if (!fn || !ast_mir_function_info(fn, &info))
      return 0;
    for (uint32_t ii = 0; ii < info.instruction_count; ++ii) {
      AstMirInstructionInfo x;
      if (!ast_mir_function_instruction(fn, ii, &x))
        return 0;
      if (x.op == AST_MIR_OP_FENCE) {
        if (ms >= 2u || x.failure_memory_order != AST_MEMORY_ORDER_NONE)
          return 0;
        mir_orders[ms++] = x.memory_order;
      }
    }
  }
  for (size_t fi = 0; fi < ast_machine_function_count(f->machine); ++fi) {
    AstMachineFunctionInfo info;
    if (!ast_machine_function_info(f->machine, fi, &info))
      return 0;
    for (uint32_t ii = 0; ii < info.instruction_count; ++ii) {
      AstMachineInstructionInfo x;
      if (!ast_machine_function_instruction(f->machine, fi, ii, &x))
        return 0;
      if (x.source_op == AST_MIR_OP_FENCE) {
        if (xs >= 2u || x.opcode != AST_MACHINE_X86_FENCE ||
            x.failure_memory_order != AST_MEMORY_ORDER_NONE)
          return 0;
        machine_orders[xs++] = x.memory_order;
      }
    }
  }
  return hs == 2u && ms == 2u && xs == 2u &&
         hir_orders[0] == AST_MEMORY_ORDER_SEQ_CST &&
         hir_orders[1] == AST_MEMORY_ORDER_ACQUIRE &&
         mir_orders[0] == hir_orders[0] && mir_orders[1] == hir_orders[1] &&
         machine_orders[0] == hir_orders[0] &&
         machine_orders[1] == hir_orders[1];
}

typedef struct Capture {
  char *data;
  size_t len, cap;
  int failed;
} Capture;
static void capture_write(void *user, const char *text, size_t length) {
  Capture *c = (Capture *)user;
  if (!c || c->failed)
    return;
  if (length > SIZE_MAX - c->len - 1u) {
    c->failed = 1;
    return;
  }
  size_t need = c->len + length + 1u;
  if (need > c->cap) {
    size_t cap = c->cap ? c->cap : 2048u;
    while (cap < need) {
      if (cap > SIZE_MAX / 2u) {
        cap = need;
        break;
      }
      cap *= 2u;
    }
    char *n = (char *)realloc(c->data, cap);
    if (!n) {
      c->failed = 1;
      return;
    }
    c->data = n;
    c->cap = cap;
  }
  memcpy(c->data + c->len, text, length);
  c->len += length;
  c->data[c->len] = 0;
}

static int physical_mutation(unsigned which) {
  fixture_arch = which / 6u == 0u   ? AST_TARGET_ARCH_X86_64
                 : which / 6u == 1u ? AST_TARGET_ARCH_AARCH64
                                    : AST_TARGET_ARCH_RISCV64;
  Fixture f;
  if (!fixture_open(&f, "tests/sema_cases/atomic_success"))
    return 0;
  f.allocation = ast_regalloc_build(f.machine, NULL);
  int ok = 0;
  if (!f.allocation || !ast_regalloc_finalize(f.allocation))
    goto done;
  AstPhysicalInstructionInfo *first = NULL, *second = NULL, *cas = NULL;
  AllocationFunction *af = &f.allocation->functions[0];
  for (uint32_t b = 0; b < af->block_count; ++b)
    for (uint32_t j = 0; j < af->blocks[b].count; ++j) {
      AstPhysicalInstructionInfo *i = &af->blocks[b].instructions[j];
      if (i->opcode != AST_MACHINE_X86_LOCKED_ATOMIC &&
          i->opcode != AST_MACHINE_A64_EXCLUSIVE_ATOMIC &&
          i->opcode != AST_MACHINE_RISCV_LRSC_ATOMIC)
        continue;
      if (!first)
        first = i;
      else if (!second)
        second = i;
      if ((i->operands[i->result_count].immediate_bits & 255u) == 8u)
        cas = i;
    }
  if (!first || !second || !cas)
    goto done;
  AstPhysicalInstructionInfo *change = which % 6 == 5   ? cas
                                       : which % 6 == 4 ? second
                                                        : first;
  AstPhysicalInstructionInfo saved = *change;
  switch (which % 6) {
  case 0:
    change->memory_order = AST_MEMORY_ORDER_RELAXED;
    break;
  case 1:
    change->operands[1] = change->operands[0];
    break;
  case 2:
    change->operands[change->result_count].immediate_bits = 12u << 8u;
    break;
  case 3:
    change->opcode =
        fixture_arch == AST_TARGET_ARCH_X86_64    ? AST_MACHINE_X86_MOV
        : fixture_arch == AST_TARGET_ARCH_AARCH64 ? AST_MACHINE_A64_MOV
                                                  : AST_MACHINE_RISCV_MV;
    change->result_count = 1;
    change->operand_count = 2;
    change->operands[1] = change->operands[0];
    break;
  case 4:
    *change = *first;
    break;
  case 5:
    change->failure_memory_order = AST_MEMORY_ORDER_RELEASE;
    break;
  }
  ok = !ast_physical_verify(f.allocation);
  *change = saved;
  ok = ok && ast_physical_verify(f.allocation);
done:
  fixture_close(&f);
  return ok;
}

static int run_case(int which) {
  if (which >= 6)
    return physical_mutation((unsigned)which - 6u);
  Fixture f;
  if (which <= 3 || which == 5) {
    if (!fixture_open(&f, "tests/sema_cases/atomic_success"))
      return 0;
  } else {
    if (!fixture_open(&f, "tests/sema_cases/fence_success"))
      return 0;
  }
  int ok = 0;
  switch (which) {
  case 0:
    ok = check_atomic_orders_hir(f.hir);
    break;
  case 1:
    ok = check_atomic_orders_mir(f.mir);
    break;
  case 2:
    ok = check_atomic_orders_machine(f.machine);
    break;
  case 3: {
    int mutated = 0;
    for (size_t fi = 0; fi < f.machine->function_count && !mutated; ++fi) {
      MachineFunction *fn = &f.machine->functions[fi];
      for (uint32_t ii = 0; ii < fn->info.instruction_count; ++ii) {
        MachineInstruction *inst = &fn->instructions[ii];
        if (inst->info.source_op == AST_MIR_OP_ATOMIC) {
          AstMemoryOrder old = inst->info.memory_order;
          inst->info.memory_order = AST_MEMORY_ORDER_NONE;
          ok = !ast_machine_verify(f.machine);
          inst->info.memory_order = old;
          mutated = 1;
          break;
        }
      }
    }
    ok = mutated && ok && ast_machine_verify(f.machine);
    break;
  }
  case 4:
    ok = check_fence_orders(&f);
    break;
  case 5: {
    Capture c = {0};
    ast_machine_dump_program(f.machine, capture_write, &c);
    ok = !c.failed && c.data && strstr(c.data, "order[acquire]") &&
         strstr(c.data, "order[release]") &&
         strstr(c.data, "order[acq_rel,fail=acquire]");
    free(c.data);
    break;
  }
  default:
    ok = 0;
    break;
  }
  fixture_close(&f);
  return ok;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  char *end = NULL;
  long which = strtol(argv[1], &end, 10);
  if (!end || *end || which < 0 || which > 23)
    return 2;
  if (!run_case((int)which)) {
    fprintf(stderr, "memory-order case %ld failed\n", which);
    return 1;
  }
  return 0;
}
