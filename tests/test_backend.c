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
} Fixture;

static void close_fixture(Fixture *f) {
  if (!f)
    return;
  ast_machine_destroy(f->machine);
  ast_abi_destroy(f->abi);
  ast_target_destroy(f->target);
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
  memset(f, 0, sizeof(*f));
}

static int open_fixture(Fixture *f, AstTargetOS os, AstAbiKind kind,
                        int full_backend) {
  char root[4096], entry[4096];
  memset(f, 0, sizeof(*f));
  if (snprintf(root, sizeof(root), "%s/tests/abi_cases/core",
               ASTERASM_SOURCE_DIR) <= 0 ||
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
  AstTargetConfig tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  tc.os = os;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target))
    goto fail;
  AstAbiConfig ac = ast_abi_config_default(kind);
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi))
    goto fail;
  if (full_backend) {
    AstBackendConfig bc = ast_backend_config_default();
    f->machine = ast_backend_lower(f->abi, &bc);
  } else {
    AstMachineConfig cc = ast_machine_config_default();
    f->machine = ast_machine_build(f->abi, &cc);
  }
  if (!f->machine)
    goto fail;
  return 1;
fail:
  close_fixture(f);
  return 0;
}

static int name_eq(AstName n, const char *s) {
  size_t z = strlen(s);
  return n.text && n.span.length == z && memcmp(n.text, s, z) == 0;
}
static int find_machine_function(const AstMachineProgram *p, const char *name,
                                 uint32_t *out) {
  for (size_t i = 0; i < ast_machine_function_count(p); ++i) {
    AstMachineFunctionInfo fi;
    if (ast_machine_function_info(p, i, &fi) && name_eq(fi.name, name)) {
      *out = (uint32_t)i;
      return 1;
    }
  }
  return 0;
}
static int find_mir_function(const Fixture *f, const char *name,
                             uint32_t *out) {
  for (size_t i = 0; i < ast_mir_function_count(f->mir); ++i) {
    AstMirFunctionInfo fi;
    const AstMirFunction *mf = ast_mir_function_at(f->mir, i);
    if (mf && ast_mir_function_info(mf, &fi) && name_eq(fi.name, name)) {
      *out = (uint32_t)i;
      return 1;
    }
  }
  return 0;
}
static int source_first_machine(const AstMachineProgram *p, uint32_t fi,
                                AstMirVReg source, AstMachineVRegInfo *out) {
  AstMachineFunctionInfo f;
  if (!ast_machine_function_info(p, fi, &f))
    return 0;
  for (uint32_t v = 0; v < f.vreg_count; ++v) {
    AstMachineVRegInfo vi;
    if (ast_machine_function_vreg(p, fi, v, &vi) && vi.source_vreg == source &&
        vi.part_index == 0u) {
      if (out)
        *out = vi;
      return 1;
    }
  }
  return 0;
}

typedef struct Capture {
  char *data;
  size_t len, cap;
  int fail;
} Capture;
static void capture(void *u, const char *t, size_t n) {
  Capture *c = (Capture *)u;
  if (!c || c->fail)
    return;
  if (n > SIZE_MAX - c->len - 1u) {
    c->fail = 1;
    return;
  }
  size_t need = c->len + n + 1u;
  if (need > c->cap) {
    size_t cap = c->cap ? c->cap : 4096u;
    while (cap < need) {
      if (cap > SIZE_MAX / 2u) {
        cap = need;
        break;
      }
      cap *= 2u;
    }
    char *x = (char *)realloc(c->data, cap);
    if (!x) {
      c->fail = 1;
      return;
    }
    c->data = x;
    c->cap = cap;
  }
  memcpy(c->data + c->len, t, n);
  c->len += n;
  c->data[c->len] = 0;
}
static void cap_free(Capture *c) {
  free(c->data);
  memset(c, 0, sizeof(*c));
}

static int run_case(int c) {
  Fixture f = {0};
  uint32_t fi = 0;
  AstMachineFunctionInfo mfi;
  AstMachineVRegInfo vi;
  AstMachineInstructionInfo ii;
  switch (c) {
  case 0:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 0))
      return 0;
    {
      int ok = ast_machine_stage(f.machine) == AST_MACHINE_STAGE_GENERIC &&
               ast_machine_verify(f.machine);
      close_fixture(&f);
      return ok;
    }
  case 1:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 0))
      return 0;
    {
      int ok = ast_machine_assign_regbanks(f.machine) &&
               ast_machine_stage(f.machine) == AST_MACHINE_STAGE_REGBANK &&
               ast_machine_verify(f.machine);
      close_fixture(&f);
      return ok;
    }
  case 2:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      int ok = ast_machine_stage(f.machine) == AST_MACHINE_STAGE_SELECTED &&
               ast_machine_verify(f.machine);
      close_fixture(&f);
      return ok;
    }
  case 3:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      AstMachineStats s = ast_machine_stats(f.machine);
      int ok = s.function_count == ast_machine_function_count(f.machine) &&
               s.instruction_count > 0 && s.vreg_count > 0 && s.block_count > 0;
      close_fixture(&f);
      return ok;
    }
  case 4:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "many9", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstAbiValueInfo av;
      int ok = ast_abi_function_argument(f.abi, fi, 0, &av) &&
               source_first_machine(f.machine, fi, av.source_vreg, &vi) &&
               (vi.flags & AST_MACHINE_VREG_FIXED_REGISTER) &&
               strcmp(vi.fixed_register, "rdi") == 0 &&
               vi.bank == AST_MACHINE_BANK_GPR;
      close_fixture(&f);
      return ok;
    }
  case 5:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "fp_mix", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstAbiValueInfo av;
      int ok = ast_abi_function_argument(f.abi, fi, 1, &av) &&
               source_first_machine(f.machine, fi, av.source_vreg, &vi) &&
               (vi.flags & AST_MACHINE_VREG_FIXED_REGISTER) &&
               strcmp(vi.fixed_register, "xmm0") == 0 &&
               vi.bank == AST_MACHINE_BANK_FP;
      close_fixture(&f);
      return ok;
    }
  case 6:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "wide_after1", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstAbiValueInfo av;
      if (!ast_abi_function_argument(f.abi, fi, 1, &av)) {
        close_fixture(&f);
        return 0;
      }
      uint32_t n = 0;
      AstMachineFunctionInfo x;
      ast_machine_function_info(f.machine, fi, &x);
      for (uint32_t v = 0; v < x.vreg_count; ++v) {
        ast_machine_function_vreg(f.machine, fi, v, &vi);
        if (vi.source_vreg == av.source_vreg)
          n++;
      }
      int ok = n == 2;
      close_fixture(&f);
      return ok;
    }
  case 7:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "hfa_passthru", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstAbiValueInfo av;
      int ok = ast_abi_function_argument(f.abi, fi, 0, &av) &&
               source_first_machine(f.machine, fi, av.source_vreg, &vi) &&
               (vi.flags & AST_MACHINE_VREG_ABI_DEFERRED) &&
               !(vi.flags & AST_MACHINE_VREG_FIXED_REGISTER);
      close_fixture(&f);
      return ok;
    }
  case 8:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "caller", &fi) ||
        !ast_machine_function_info(f.machine, fi, &mfi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int found = 0;
      for (uint32_t i = 0; i < mfi.instruction_count; ++i)
        if (ast_machine_function_instruction(f.machine, fi, i, &ii) &&
            ii.opcode == AST_MACHINE_X86_CALL)
          found = 1;
      close_fixture(&f);
      return found;
    }
  case 9:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "callee", &fi) ||
        !ast_machine_function_info(f.machine, fi, &mfi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int found = 0;
      for (uint32_t i = 0; i < mfi.instruction_count; ++i)
        if (ast_machine_function_instruction(f.machine, fi, i, &ii) &&
            ii.opcode == AST_MACHINE_X86_ADD)
          found = 1;
      close_fixture(&f);
      return found;
    }
  case 10:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "many9", &fi) ||
        !ast_machine_function_info(f.machine, fi, &mfi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int found = 0;
      for (uint32_t i = 0; i < mfi.instruction_count; ++i)
        if (ast_machine_function_instruction(f.machine, fi, i, &ii) &&
            ii.opcode == AST_MACHINE_X86_RET)
          found = 1;
      close_fixture(&f);
      return found;
    }
  case 11:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      Capture a = {0}, b = {0};
      ast_machine_dump_program(f.machine, capture, &a);
      ast_machine_dump_program(f.machine, capture, &b);
      int ok = !a.fail && !b.fail && a.len > 0 && a.len == b.len &&
               memcmp(a.data, b.data, a.len) == 0;
      cap_free(&a);
      cap_free(&b);
      close_fixture(&f);
      return ok;
    }
  case 12:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      MachineFunction *x = &f.machine->functions[0];
      if (!x->info.instruction_count) {
        close_fixture(&f);
        return 0;
      }
      AstMachineOpcode old = x->instructions[0].info.opcode;
      x->instructions[0].info.opcode = AST_MACHINE_G_COPY;
      int ok = !ast_machine_verify(f.machine);
      x->instructions[0].info.opcode = old;
      ok = ok && ast_machine_verify(f.machine);
      close_fixture(&f);
      return ok;
    }
  case 13:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      int done = 0, ok = 0;
      for (size_t q = 0; q < f.machine->function_count && !done; ++q) {
        MachineFunction *x = &f.machine->functions[q];
        for (uint32_t i = 0; i < x->info.instruction_count && !done; ++i) {
          MachineInstruction *m = &x->instructions[i];
          for (uint32_t o = 0; o < m->info.operand_count; ++o)
            if (m->info.operands[o].kind == AST_MACHINE_OPERAND_VREG) {
              AstMachineVReg old = x->operands[m->operand_offset + o].as.vreg;
              x->operands[m->operand_offset + o].as.vreg = x->info.vreg_count;
              ok = !ast_machine_verify(f.machine);
              x->operands[m->operand_offset + o].as.vreg = old;
              done = 1;
              break;
            }
        }
      }
      ok = ok && ast_machine_verify(f.machine);
      close_fixture(&f);
      return done && ok;
    }
  case 14:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "many9", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      MachineFunction *x = &f.machine->functions[fi];
      int found = 0, ok = 0;
      for (uint32_t v = 0; v < x->info.vreg_count; ++v)
        if (x->vregs[v].flags & AST_MACHINE_VREG_FIXED_REGISTER) {
          char old[AST_MACHINE_REGISTER_NAME_CAP];
          memcpy(old, x->vregs[v].fixed_register, sizeof(old));
          memcpy(x->vregs[v].fixed_register, "rsi", sizeof("rsi"));
          ok = !ast_machine_verify(f.machine);
          memcpy(x->vregs[v].fixed_register, old, sizeof(old));
          found = 1;
          break;
        }
      ok = ok && ast_machine_verify(f.machine);
      close_fixture(&f);
      return found && ok;
    }
  case 15:
    if (!open_fixture(&f, AST_TARGET_OS_WINDOWS, AST_ABI_WINDOWS_X64, 1) ||
        !find_machine_function(f.machine, "many9", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstAbiValueInfo av;
      int ok = ast_abi_function_argument(f.abi, fi, 0, &av) &&
               source_first_machine(f.machine, fi, av.source_vreg, &vi) &&
               strcmp(vi.fixed_register, "rcx") == 0;
      close_fixture(&f);
      return ok;
    }
  case 16:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      int ok = !ast_machine_select_instructions(f.machine) &&
               ast_machine_verify(f.machine);
      close_fixture(&f);
      return ok;
    }
  case 17: {
    close_fixture(&f);
    return ast_machine_build(NULL, NULL) == NULL &&
           ast_backend_lower(NULL, NULL) == NULL;
  }
  case 18:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      int ok = ast_machine_source_abi(f.machine) == f.abi;
      close_fixture(&f);
      return ok;
    }
  case 19:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "caller", &fi) ||
        !ast_machine_function_info(f.machine, fi, &mfi)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstMachineBlockInfo b;
      int ok = mfi.block_count > 0 &&
               ast_machine_function_block(f.machine, fi, mfi.entry_block, &b) &&
               b.instruction_count > 0 && b.instructions != NULL;
      close_fixture(&f);
      return ok;
    }
  case 20:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 0))
      return 0;
    {
      int ok = ast_machine_assign_regbanks(f.machine) &&
               ast_machine_select_instructions(f.machine) &&
               ast_machine_stage(f.machine) == AST_MACHINE_STAGE_SELECTED;
      close_fixture(&f);
      return ok;
    }
  case 21:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_mir_function(&f, "wide_after1", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      const AstMirFunction *mf = ast_mir_function_at(f.mir, fi);
      AstMirFunctionInfo x;
      if (!mf || !ast_mir_function_info(mf, &x)) {
        close_fixture(&f);
        return 0;
      }
      int ok = 0;
      for (uint32_t v = 0; v < x.vreg_count; ++v) {
        AstTargetValueInfo tv;
        if (ast_target_value_info(f.target, fi, v, &tv) &&
            tv.part_count == 2u) {
          ok = source_first_machine(f.machine, fi, v, &vi) &&
               vi.part_count == 2u;
          break;
        }
      }
      close_fixture(&f);
      return ok;
    }
  case 22:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      AstMachineStats s = ast_machine_stats(f.machine);
      int ok = s.fixed_register_count > 0 && s.split_value_count > 0 &&
               s.deferred_abi_value_count > 0;
      close_fixture(&f);
      return ok;
    }
  case 23:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      int ok =
          strcmp(ast_machine_regbank_name(AST_MACHINE_BANK_GPR), "gpr") == 0 &&
          strstr(ast_machine_opcode_name(AST_MACHINE_X86_ADD), "X86_ADD") !=
              NULL;
      close_fixture(&f);
      return ok;
    }
  case 24:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "caller", &fi) ||
        !ast_machine_function_info(f.machine, fi, &mfi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = 0;
      for (uint32_t i = 0; i < mfi.instruction_count; ++i) {
        if (!ast_machine_function_instruction(f.machine, fi, i, &ii) ||
            ii.opcode != AST_MACHINE_X86_CALL)
          continue;
        const char *want[] = {"rdi", "rsi", "rdx"};
        uint32_t seen = 0;
        for (uint32_t o = 0; o < ii.operand_count && seen < 3; ++o)
          if (ii.operands[o].kind == AST_MACHINE_OPERAND_VREG) {
            if (!(ii.operands[o].flags & AST_MACHINE_OPERAND_FIXED_REGISTER) ||
                strcmp(ii.operands[o].fixed_register, want[seen]) != 0) {
              seen = 0;
              break;
            }
            seen++;
          }
        ok = seen == 3;
        break;
      }
      close_fixture(&f);
      return ok;
    }
  case 25:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "caller", &fi) ||
        !ast_machine_function_info(f.machine, fi, &mfi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = 0;
      for (uint32_t i = 0; i < mfi.instruction_count; ++i)
        if (ast_machine_function_instruction(f.machine, fi, i, &ii) &&
            ii.opcode == AST_MACHINE_X86_CALL && ii.result_count) {
          if (ast_machine_function_vreg(f.machine, fi, ii.result_first, &vi) &&
              (vi.flags & AST_MACHINE_VREG_FIXED_REGISTER) &&
              strcmp(vi.fixed_register, "rax") == 0)
            ok = 1;
        }
      close_fixture(&f);
      return ok;
    }
  case 26:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "caller", &fi) ||
        !ast_machine_function_info(f.machine, fi, &mfi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = 0;
      for (uint32_t i = 0; i < mfi.instruction_count; ++i)
        if (ast_machine_function_instruction(f.machine, fi, i, &ii) &&
            ii.opcode == AST_MACHINE_X86_RET && ii.operand_count) {
          ok = (ii.operands[0].flags & AST_MACHINE_OPERAND_FIXED_REGISTER) &&
               strcmp(ii.operands[0].fixed_register, "rax") == 0;
        }
      close_fixture(&f);
      return ok;
    }
  case 27:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1) ||
        !find_machine_function(f.machine, "caller", &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      MachineFunction *x = &f.machine->functions[fi];
      int found = 0, ok = 0;
      for (uint32_t i = 0; i < x->info.instruction_count && !found; ++i) {
        MachineInstruction *m = &x->instructions[i];
        if (m->info.opcode != AST_MACHINE_X86_CALL)
          continue;
        for (uint32_t o = 0; o < m->info.operand_count; ++o) {
          AstMachineOperand *op = &x->operands[m->operand_offset + o];
          if ((op->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) &&
              strcmp(op->fixed_register, "rdi") == 0) {
            char old[AST_MACHINE_REGISTER_NAME_CAP];
            memcpy(old, op->fixed_register, sizeof(old));
            memcpy(op->fixed_register, "rsi", sizeof("rsi"));
            ok = !ast_machine_verify(f.machine);
            memcpy(op->fixed_register, old, sizeof(old));
            found = 1;
            break;
          }
        }
      }
      ok = ok && ast_machine_verify(f.machine);
      close_fixture(&f);
      return found && ok;
    }
  case 28:
    if (!open_fixture(&f, AST_TARGET_OS_LINUX, AST_ABI_SYSV_AMD64, 1))
      return 0;
    {
      AstMachineStats s = ast_machine_stats(f.machine);
      int ok = s.fixed_operand_count > 0;
      close_fixture(&f);
      return ok;
    }
  default:
    return 0;
  }
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  char *e = NULL;
  long c = strtol(argv[1], &e, 10);
  if (!e || *e || c < 0 || c > 28)
    return 2;
  if (!run_case((int)c)) {
    fprintf(stderr, "backend case %ld failed\n", c);
    return 1;
  }
  return 0;
}
