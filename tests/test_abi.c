#include "abi_internal.h"
#include "asterasm/asterasm.h"
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
} Fixture;

static void close_fixture(Fixture *f) {
  if (!f)
    return;
  ast_abi_destroy(f->abi);
  ast_target_destroy(f->target);
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
  memset(f, 0, sizeof(*f));
}

static int open_fixture_named(AstTargetArch arch, AstTargetOS os,
                              uint64_t features, AstAbiKind abi,
                              const char *case_name, Fixture *f) {
  char root[4096], entry[4096];
  if (!case_name ||
      snprintf(root, sizeof(root), "%s/tests/abi_cases/%s", ASTERASM_SOURCE_DIR,
               case_name) <= 0 ||
      snprintf(entry, sizeof(entry), "%s/main.aa", root) <= 0)
    return 0;
  memset(f, 0, sizeof(*f));
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 2u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0) {
    close_fixture(f);
    return 0;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(f->frontend, entries, 1u) != 0) {
    close_fixture(f);
    return 0;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 2u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0) {
    close_fixture(f);
    return 0;
  }
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 2u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir)) {
    close_fixture(f);
    return 0;
  }
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 2u;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir)) {
    close_fixture(f);
    return 0;
  }
  AstTargetConfig tc = ast_target_config_default(arch);
  tc.os = os;
  tc.features |= features;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target)) {
    close_fixture(f);
    return 0;
  }
  AstAbiConfig ac = ast_abi_config_default(abi);
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi)) {
    close_fixture(f);
    return 0;
  }
  return 1;
}

static int open_fixture(AstTargetArch arch, AstTargetOS os, uint64_t features,
                        AstAbiKind abi, Fixture *f) {
  return open_fixture_named(arch, os, features, abi, "core", f);
}

typedef struct TextCapture {
  char *data;
  size_t length;
  size_t capacity;
  int failed;
} TextCapture;
static void capture_write(void *user, const char *text, size_t length) {
  TextCapture *c = (TextCapture *)user;
  if (!c || c->failed || !text)
    return;
  if (length > SIZE_MAX - c->length - 1u) {
    c->failed = 1;
    return;
  }
  size_t need = c->length + length + 1u;
  if (need > c->capacity) {
    size_t cap = c->capacity ? c->capacity : 4096u;
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
    c->capacity = cap;
  }
  memcpy(c->data + c->length, text, length);
  c->length += length;
  c->data[c->length] = '\0';
}
static void capture_free(TextCapture *c) {
  if (c) {
    free(c->data);
    memset(c, 0, sizeof(*c));
  }
}

static int name_eq(AstName n, const char *s) {
  size_t z = strlen(s);
  return n.text && (size_t)n.span.length == z && memcmp(n.text, s, z) == 0;
}
static int find_index(Fixture *f, const char *name, size_t *out) {
  for (size_t i = 0; i < ast_mir_function_count(f->mir); ++i) {
    AstMirFunctionInfo mi;
    const AstMirFunction *fn = ast_mir_function_at(f->mir, i);
    if (fn && ast_mir_function_info(fn, &mi) && name_eq(mi.name, name)) {
      *out = i;
      return 1;
    }
  }
  return 0;
}
static int reg_is(const AstAbiValueInfo *v, uint32_t i, const char *r) {
  return i < v->location.register_count &&
         strcmp(v->location.registers[i], r) == 0;
}
static int arg(Fixture *f, const char *fn, uint32_t i, AstAbiValueInfo *v,
               AstAbiFunctionInfo *fi) {
  size_t ix;
  return find_index(f, fn, &ix) && ast_abi_function_info(f->abi, ix, fi) &&
         ast_abi_function_argument(f->abi, ix, i, v);
}

static int run_case(int c) {
  Fixture f;
  AstAbiValueInfo v, w;
  AstAbiFunctionInfo fi;
  size_t ix;
  memset(&f, 0, sizeof(f));
  switch (c) {
  case 0:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f))
      return 0;
    if (!arg(&f, "many9", 0u, &v, &fi) || !reg_is(&v, 0, "rdi") ||
        !ast_abi_function_argument(f.abi, fi.function_index, 5u, &v) ||
        !reg_is(&v, 0, "r9") ||
        !ast_abi_function_argument(f.abi, fi.function_index, 6u, &v) ||
        v.location.kind != AST_ABI_LOCATION_STACK) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 1:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !find_index(&f, "wide_after7", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi)) {
      close_fixture(&f);
      return 0;
    }
    v = fi.result;
    {
      int ok = v.location.register_count == 2u && reg_is(&v, 0, "rax") &&
               reg_is(&v, 1, "rdx");
      close_fixture(&f);
      return ok;
    }
  case 2:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !arg(&f, "pair_passthru", 0u, &v, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.register_count == 2u && reg_is(&v, 0, "rdi") &&
               reg_is(&v, 1, "rsi");
      close_fixture(&f);
      return ok;
    }
  case 3:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !find_index(&f, "triple_passthru", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        fi.hidden_argument_count != 1u ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = reg_is(&v, 0, "rdi") &&
               (fi.result.flags & AST_ABI_VALUE_RETURN_POINTER_IN_GPR) &&
               reg_is(&fi.result, 0, "rax");
      close_fixture(&f);
      return ok;
    }
  case 4:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_WINDOWS, 0u,
                      AST_ABI_WINDOWS_X64, &f) ||
        !find_index(&f, "fp_mix", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        fi.shadow_space_bytes != 32u ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v) ||
        !reg_is(&v, 0, "xmm1") ||
        !ast_abi_function_argument(f.abi, ix, 3u, &v) ||
        !reg_is(&v, 0, "xmm3") ||
        !ast_abi_function_argument(f.abi, ix, 4u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.kind == AST_ABI_LOCATION_STACK &&
               v.location.stack_offset == 32u;
      close_fixture(&f);
      return ok;
    }
  case 5:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_WINDOWS, 0u,
                      AST_ABI_WINDOWS_X64, &f) ||
        !arg(&f, "pair_passthru", 1u, &v, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.kind == AST_ABI_LOCATION_INDIRECT &&
               reg_is(&v, 0, "rdx") && (v.flags & AST_ABI_VALUE_INDIRECT_COPY);
      close_fixture(&f);
      return ok;
    }
  case 6:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_WINDOWS, 0u,
                      AST_ABI_WINDOWS_X64, &f) ||
        !find_index(&f, "triple_passthru", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = fi.hidden_argument_count == 1u && reg_is(&v, 0, "rcx") &&
               reg_is(&fi.result, 0, "rax") &&
               (fi.result.flags & AST_ABI_VALUE_RETURN_POINTER_IN_GPR);
      close_fixture(&f);
      return ok;
    }
  case 7:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !find_index(&f, "pair_after7", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 7u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.kind == AST_ABI_LOCATION_STACK &&
               v.location.register_count == 0u;
      close_fixture(&f);
      return ok;
    }
  case 8:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !arg(&f, "hfa_passthru", 0u, &v, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = (v.flags & AST_ABI_VALUE_HFA) &&
               v.location.register_count == 2u && reg_is(&v, 0, "v0") &&
               reg_is(&v, 1, "v1");
      close_fixture(&f);
      return ok;
    }
  case 9:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !arg(&f, "hfa_aligned", 0u, &v, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = (v.flags & AST_ABI_VALUE_HFA) &&
               v.location.register_count == 4u && reg_is(&v, 3, "v3");
      close_fixture(&f);
      return ok;
    }
  case 10:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !find_index(&f, "triple_passthru", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      uint64_t mask = UINT64_C(0xff) << 8u;
      int ok = fi.hidden_argument_count == 1u && reg_is(&v, 0, "x8") &&
               (fi.callee_saved.fp_vector_low64_only & mask) == mask;
      close_fixture(&f);
      return ok;
    }
  case 11:
    if (!open_fixture(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                      AST_TARGET_FEATURE_RV_D, AST_ABI_RISCV_LP64D, &f) ||
        !find_index(&f, "wide_after7", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 7u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.kind == AST_ABI_LOCATION_SPLIT_REG_STACK &&
               reg_is(&v, 0, "a7") && v.location.stack_size == 8u &&
               (v.flags & AST_ABI_VALUE_SPLIT);
      close_fixture(&f);
      return ok;
    }
  case 12:
    if (!open_fixture(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                      AST_TARGET_FEATURE_RV_D, AST_ABI_RISCV_LP64D, &f) ||
        !find_index(&f, "fp_mix", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 3u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = reg_is(&v, 0, "fa1") && v.extension == AST_ABI_EXT_NAN_BOX &&
               (v.flags & AST_ABI_VALUE_NAN_BOXED);
      close_fixture(&f);
      return ok;
    }
  case 13:
    if (!open_fixture(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                      AST_TARGET_FEATURE_RV_D, AST_ABI_RISCV_LP64D, &f) ||
        !find_index(&f, "triple_passthru", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = fi.hidden_argument_count == 1u && reg_is(&v, 0, "a0") &&
               !(fi.result.flags & AST_ABI_VALUE_RETURN_POINTER_IN_GPR);
      close_fixture(&f);
      return ok;
    }
  case 14:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !find_index(&f, "caller", &ix)) {
      close_fixture(&f);
      return 0;
    }
    {
      const AstMirFunction *mf = ast_mir_function_at(f.mir, ix);
      AstMirFunctionInfo mi;
      AstMirInstructionInfo ins;
      int found = 0, ok = 1;
      if (!mf || !ast_mir_function_info(mf, &mi)) {
        close_fixture(&f);
        return 0;
      }
      for (uint32_t ii = 0; ii < mi.instruction_count; ++ii)
        if (ast_mir_function_instruction(mf, ii, &ins) &&
            ins.op == AST_MIR_OP_CALL_DIRECT) {
          for (size_t ci = 0; ci < ast_abi_call_count(f.abi); ++ci) {
            AstAbiCallInfo ci_info;
            if (!ast_abi_call_info(f.abi, ci, &ci_info) ||
                ci_info.caller_function_index != ix ||
                ci_info.instruction != ii)
              continue;
            found = 1;
            for (uint32_t a = 0; a < ins.operand_count; ++a) {
              if (!ast_abi_call_argument(f.abi, ci, a, &v) ||
                  v.source_vreg != ins.operands[a])
                ok = 0;
            }
          }
        }
      close_fixture(&f);
      return found && ok;
    }
  case 15:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !find_index(&f, "small_ints", &ix) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        v.extension != AST_ABI_EXT_ZERO ||
        !ast_abi_function_argument(f.abi, ix, 1u, &w) ||
        w.extension != AST_ABI_EXT_SIGN ||
        !ast_abi_function_argument(f.abi, ix, 2u, &v) ||
        v.extension != AST_ABI_EXT_NONE) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 16:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !find_index(&f, "fp_mix", &ix) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        !reg_is(&v, 0, "rdi") ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v) ||
        !reg_is(&v, 0, "xmm0") ||
        !ast_abi_function_argument(f.abi, ix, 2u, &v) ||
        !reg_is(&v, 0, "rsi") ||
        !ast_abi_function_argument(f.abi, ix, 3u, &v) ||
        !reg_is(&v, 0, "xmm1") ||
        !ast_abi_function_argument(f.abi, ix, 4u, &v) ||
        !reg_is(&v, 0, "rdx")) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 17:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !find_index(&f, "many9", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      uint64_t callee = (UINT64_C(1) << 3) | (UINT64_C(1) << 4) |
                        (UINT64_C(1) << 5) | (UINT64_C(1) << 12) |
                        (UINT64_C(1) << 13) | (UINT64_C(1) << 14) |
                        (UINT64_C(1) << 15);
      int ok = fi.red_zone_bytes == 128u &&
               (fi.callee_saved.gpr & callee) == callee &&
               fi.caller_saved.fp_vector == UINT64_C(0xffff);
      close_fixture(&f);
      return ok;
    }
  case 18:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_WINDOWS, 0u,
                      AST_ABI_WINDOWS_X64, &f) ||
        !find_index(&f, "small_ints", &ix) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        !reg_is(&v, 0, "rcx") ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v) ||
        !reg_is(&v, 0, "rdx") ||
        !ast_abi_function_argument(f.abi, ix, 2u, &v) || !reg_is(&v, 0, "r8")) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 19:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_WINDOWS, 0u,
                      AST_ABI_WINDOWS_X64, &f) ||
        !find_index(&f, "many9", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 4u, &v) ||
        v.location.kind != AST_ABI_LOCATION_STACK ||
        v.location.stack_offset != 32u ||
        !ast_abi_function_argument(f.abi, ix, 8u, &v) ||
        v.location.kind != AST_ABI_LOCATION_STACK ||
        v.location.stack_offset != 64u) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = fi.shadow_space_bytes == 32u && fi.outgoing_stack_bytes == 80u;
      close_fixture(&f);
      return ok;
    }
  case 20:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_WINDOWS, 0u,
                      AST_ABI_WINDOWS_X64, &f) ||
        !find_index(&f, "many9", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      uint64_t g =
          (UINT64_C(1) << 3) | (UINT64_C(1) << 4) | (UINT64_C(1) << 5) |
          (UINT64_C(1) << 6) | (UINT64_C(1) << 7) | (UINT64_C(1) << 12) |
          (UINT64_C(1) << 13) | (UINT64_C(1) << 14) | (UINT64_C(1) << 15);
      uint64_t x = UINT64_C(0xffc0);
      int ok = (fi.callee_saved.gpr & g) == g &&
               (fi.callee_saved.fp_vector & x) == x;
      close_fixture(&f);
      return ok;
    }
  case 21:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !find_index(&f, "pair_passthru", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = fi.result.location.register_count == 2u &&
               reg_is(&fi.result, 0, "x0") && reg_is(&fi.result, 1, "x1");
      close_fixture(&f);
      return ok;
    }
  case 22:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !find_index(&f, "wide_after1", &ix) ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.register_count == 2u && reg_is(&v, 0, "x2") &&
               reg_is(&v, 1, "x3");
      close_fixture(&f);
      return ok;
    }
  case 23:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !find_index(&f, "hfa_passthru", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = (fi.result.flags & AST_ABI_VALUE_HFA) &&
               fi.result.location.register_count == 2u &&
               reg_is(&fi.result, 0, "v0") && reg_is(&fi.result, 1, "v1");
      close_fixture(&f);
      return ok;
    }
  case 24:
    if (!open_fixture(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_AAPCS64, &f) ||
        !find_index(&f, "hfa_after8", &ix) ||
        !ast_abi_function_argument(f.abi, ix, 8u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.kind == AST_ABI_LOCATION_STACK &&
               v.location.register_count == 0u;
      close_fixture(&f);
      return ok;
    }
  case 25:
    if (!open_fixture(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                      AST_TARGET_FEATURE_RV_D, AST_ABI_RISCV_LP64D, &f) ||
        !find_index(&f, "mixed_fi", &ix) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.register_count == 2u && reg_is(&v, 0, "fa0") &&
               reg_is(&v, 1, "a0") && (v.flags & AST_ABI_VALUE_NAN_BOXED);
      close_fixture(&f);
      return ok;
    }
  case 26:
    if (!open_fixture(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                      AST_TARGET_FEATURE_RV_D, AST_ABI_RISCV_LP64D, &f) ||
        !find_index(&f, "hfa_passthru", &ix) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v)) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = v.location.register_count == 2u && reg_is(&v, 0, "fa0") &&
               reg_is(&v, 1, "fa1");
      close_fixture(&f);
      return ok;
    }
  case 27:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_WINDOWS, 0u,
                      AST_ABI_WINDOWS_X64, &f) ||
        !find_index(&f, "many9", &ix)) {
      close_fixture(&f);
      return 0;
    }
    {
      uint32_t old = f.abi->functions[ix].info.shadow_space_bytes;
      f.abi->functions[ix].info.shadow_space_bytes = 0u;
      int ok = !ast_abi_verify(f.abi);
      f.abi->functions[ix].info.shadow_space_bytes = old;
      ok = ok && ast_abi_verify(f.abi);
      close_fixture(&f);
      return ok;
    }
  case 28:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f) ||
        !find_index(&f, "many9", &ix)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstAbiLocation old = f.abi->functions[ix].arguments[0].location;
      memset(&f.abi->functions[ix].arguments[0].location, 0,
             sizeof(AstAbiLocation));
      int ok = !ast_abi_verify(f.abi);
      f.abi->functions[ix].arguments[0].location = old;
      ok = ok && ast_abi_verify(f.abi);
      close_fixture(&f);
      return ok;
    }
  case 29:
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstAbiConfig ac = ast_abi_config_default(AST_ABI_SYSV_AMD64);
      AstAbiProgram *b = ast_abi_lower(f.target, &ac);
      TextCapture x = {0}, y = {0};
      if (!b || !ast_abi_verify(b)) {
        ast_abi_destroy(b);
        close_fixture(&f);
        return 0;
      }
      ast_abi_dump_program(f.abi, capture_write, &x);
      ast_abi_dump_program(b, capture_write, &y);
      int ok = !x.failed && !y.failed && x.length == y.length &&
               x.length > 0u && memcmp(x.data, y.data, x.length) == 0;
      capture_free(&x);
      capture_free(&y);
      ast_abi_destroy(b);
      close_fixture(&f);
      return ok;
    }
  case 30:
    if (!open_fixture_named(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX,
                            AST_TARGET_FEATURE_A64_SVE, AST_ABI_AAPCS64_SVE,
                            "vectors", &f) ||
        !arg(&f, "sve_data", 0u, &v, &fi) || !reg_is(&v, 0u, "z0") ||
        !find_index(&f, "sve_data", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !reg_is(&fi.result, 0u, "z0")) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 31:
    if (!open_fixture_named(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX,
                            AST_TARGET_FEATURE_A64_SVE, AST_ABI_AAPCS64_SVE,
                            "vectors", &f) ||
        !find_index(&f, "sve_mixed", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        !reg_is(&v, 0u, "z0") ||
        !ast_abi_function_argument(f.abi, ix, 1u, &w) ||
        !reg_is(&w, 0u, "p0") ||
        !ast_abi_function_argument(f.abi, ix, 2u, &v) ||
        !reg_is(&v, 0u, "z1") || !reg_is(&fi.result, 0u, "z0")) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = (fi.callee_saved.fp_vector & (UINT64_C(0xffff) << 8u)) ==
                   (UINT64_C(0xffff) << 8u) &&
               fi.callee_saved.fp_vector_low64_only == 0u &&
               (fi.callee_saved.predicate & (UINT64_C(0xfff) << 4u)) ==
                   (UINT64_C(0xfff) << 4u);
      close_fixture(&f);
      return ok;
    }
  case 32:
    if (!open_fixture_named(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX,
                            AST_TARGET_FEATURE_A64_SVE, AST_ABI_AAPCS64_SVE,
                            "vectors", &f) ||
        !arg(&f, "sve_predicate", 0u, &v, &fi) || !reg_is(&v, 0u, "p0") ||
        !find_index(&f, "sve_predicate", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !reg_is(&fi.result, 0u, "p0")) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 33:
    if (!open_fixture_named(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                            AST_TARGET_FEATURE_RV_V, AST_ABI_RISCV_LP64V,
                            "vectors", &f) ||
        !arg(&f, "rvv_data", 0u, &v, &fi) || !reg_is(&v, 0u, "v8") ||
        !find_index(&f, "rvv_data", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !reg_is(&fi.result, 0u, "v8")) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = (fi.callee_saved.fp_vector &
                ((UINT64_C(0x7f) << 1u) | (UINT64_C(0xff) << 24u))) ==
               ((UINT64_C(0x7f) << 1u) | (UINT64_C(0xff) << 24u));
      close_fixture(&f);
      return ok;
    }
  case 34:
    if (!open_fixture_named(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                            AST_TARGET_FEATURE_RV_V, AST_ABI_RISCV_LP64V,
                            "vectors", &f) ||
        !find_index(&f, "rvv_predicate", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        !reg_is(&v, 0u, "v0") ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v) ||
        !reg_is(&v, 0u, "v8") || !reg_is(&fi.result, 0u, "v0")) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 35:
    if (open_fixture_named(AST_TARGET_ARCH_AARCH64, AST_TARGET_OS_LINUX,
                           AST_TARGET_FEATURE_A64_SVE, AST_ABI_AAPCS64,
                           "vectors", &f)) {
      close_fixture(&f);
      return 0;
    }
    if (open_fixture_named(AST_TARGET_ARCH_RISCV64, AST_TARGET_OS_LINUX,
                           AST_TARGET_FEATURE_RV_V, AST_ABI_RISCV_LP64,
                           "vectors", &f)) {
      close_fixture(&f);
      return 0;
    }
    return 1;
  case 36:
    if (!open_fixture_named(AST_TARGET_ARCH_X86_32, AST_TARGET_OS_LINUX, 0u,
                            AST_ABI_I386_CDECL, "i386", &f) ||
        !find_index(&f, "many9", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        v.location.kind != AST_ABI_LOCATION_STACK ||
        v.location.stack_offset != 0u || v.location.stack_size != 8u ||
        !ast_abi_function_argument(f.abi, ix, 8u, &v) ||
        v.location.stack_offset != 64u) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = fi.stack_alignment == 16u && fi.outgoing_stack_bytes == 80u &&
               (fi.caller_saved.gpr & UINT64_C(0x7)) == UINT64_C(0x7) &&
               (fi.callee_saved.gpr & UINT64_C(0xf8)) == UINT64_C(0xf8);
      close_fixture(&f);
      return ok;
    }
  case 37:
    if (!open_fixture_named(AST_TARGET_ARCH_X86_32, AST_TARGET_OS_LINUX, 0u,
                            AST_ABI_I386_CDECL, "i386", &f) ||
        !find_index(&f, "small_ints", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        fi.result.location.register_count != 2u ||
        !reg_is(&fi.result, 0u, "eax") || !reg_is(&fi.result, 1u, "edx") ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        v.location.stack_offset != 0u || v.location.stack_size != 4u ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v) ||
        v.location.stack_offset != 4u || v.location.stack_size != 4u ||
        !ast_abi_function_argument(f.abi, ix, 2u, &v) ||
        v.location.stack_offset != 8u || v.location.stack_size != 4u) {
      close_fixture(&f);
      return 0;
    }
    close_fixture(&f);
    return 1;
  case 38:
    if (!open_fixture_named(AST_TARGET_ARCH_X86_32, AST_TARGET_OS_LINUX, 0u,
                            AST_ABI_I386_CDECL, "i386", &f) ||
        !find_index(&f, "fp_mix", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        !reg_is(&fi.result, 0u, "st0") ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v) ||
        v.location.stack_offset != 8u || v.location.stack_size != 8u ||
        !ast_abi_function_argument(f.abi, ix, 3u, &v) ||
        v.location.stack_offset != 24u || v.location.stack_size != 4u) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = fi.outgoing_stack_bytes == 48u;
      close_fixture(&f);
      return ok;
    }
  case 39:
    if (!open_fixture_named(AST_TARGET_ARCH_X86_32, AST_TARGET_OS_LINUX, 0u,
                            AST_ABI_I386_CDECL, "i386", &f) ||
        !find_index(&f, "triple_passthru", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi) ||
        fi.hidden_argument_count != 1u || !reg_is(&fi.result, 0u, "eax") ||
        !(fi.result.flags & AST_ABI_VALUE_RETURN_POINTER_IN_GPR) ||
        !ast_abi_function_argument(f.abi, ix, 0u, &v) ||
        !(v.flags & AST_ABI_VALUE_HIDDEN) ||
        v.location.kind != AST_ABI_LOCATION_STACK ||
        v.location.stack_offset != 0u || v.location.stack_size != 4u ||
        !ast_abi_function_argument(f.abi, ix, 1u, &v) ||
        v.location.stack_offset != 4u || v.location.stack_size != 12u) {
      close_fixture(&f);
      return 0;
    }
    {
      int ok = fi.outgoing_stack_bytes == 16u;
      close_fixture(&f);
      return ok;
    }
  case 40:
    if (!open_fixture_named(AST_TARGET_ARCH_X86_16, AST_TARGET_OS_BARE_METAL,
                            0u, AST_ABI_I386_CDECL, "i386", &f) ||
        !find_index(&f, "many9", &ix) ||
        !ast_abi_function_info(f.abi, ix, &fi)) {
      close_fixture(&f);
      return 0;
    }
    {
      AstTargetConfig tc = ast_target_config(f.target);
      int ok = tc.pointer_bits == 32u && fi.stack_alignment == 16u &&
               fi.outgoing_stack_bytes == 80u;
      close_fixture(&f);
      return ok;
    }
  case 41: {
    if (!open_fixture(AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u,
                      AST_ABI_SYSV_AMD64, &f))
      return 0;
    AstAbiConfig ac = ast_abi_config_default(AST_ABI_I386_CDECL);
    AstAbiProgram *wrong = ast_abi_lower(f.target, &ac);
    int ok = wrong && !ast_abi_verify(wrong) &&
             ast_abi_stats(wrong).error_count != 0u;
    ast_abi_destroy(wrong);
    close_fixture(&f);
    return ok;
  }
  default:
    return 0;
  }
  (void)w;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  char *end = NULL;
  long c = strtol(argv[1], &end, 10);
  if (!end || *end || c < 0 || c > 41)
    return 2;
  if (!run_case((int)c)) {
    fprintf(stderr, "abi case %ld failed\n", c);
    return 1;
  }
  return 0;
}
