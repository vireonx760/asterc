#include "asterasm/asterasm.h"
#include "target_internal.h"
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
} Fixture;

static void fixture_close(Fixture *f) {
  if (!f)
    return;
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
  memset(f, 0, sizeof(*f));
}

static int fixture_open_at(Fixture *f, const char *relative_root) {
  char root[4096], entry[4096];
  if (!relative_root ||
      snprintf(root, sizeof(root), "%s/%s", ASTERASM_SOURCE_DIR,
               relative_root) <= 0 ||
      snprintf(entry, sizeof(entry), "%s/main.aa", root) <= 0)
    return 0;
  memset(f, 0, sizeof(*f));
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 2u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0) {
    fixture_close(f);
    return 0;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(f->frontend, entries, 1u) != 0) {
    fixture_close(f);
    return 0;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 2u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0) {
    fixture_close(f);
    return 0;
  }
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 2u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir)) {
    fixture_close(f);
    return 0;
  }
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 2u;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir)) {
    fixture_close(f);
    return 0;
  }
  return 1;
}

static int fixture_open(Fixture *f) {
  return fixture_open_at(f, "tests/abi_cases/core");
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
    char *next = (char *)realloc(c->data, cap);
    if (!next) {
      c->failed = 1;
      return;
    }
    c->data = next;
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

static AstTargetProgram *fixture_target(Fixture *f, AstTargetArch arch,
                                        AstTargetOS os, uint64_t features) {
  AstTargetConfig tc = ast_target_config_default(arch);
  tc.os = os;
  tc.features |= features;
  return ast_target_legalize(f->mir, &tc);
}

static int target_has_diag(const AstTargetProgram *p, AstTargetDiagCode code) {
  size_t count = 0u;
  const AstTargetDiagnostic *d = ast_target_diagnostics(p, &count);
  for (size_t i = 0u; i < count; ++i)
    if (d[i].code == code)
      return 1;
  return 0;
}

static int native_validation_smoke(void) {
  Fixture f;
  AstTargetProgram *tp = NULL;
  int ok = 1;

  if (!fixture_open_at(&f, "tests/target_cases/native_feature"))
    return 0;
  tp = fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u);
  ok = ok && tp && !ast_target_verify(tp) &&
       target_has_diag(tp, AST_TARGET_DIAG_REQUIRED_FEATURE);
  ast_target_destroy(tp);
  tp = fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX,
                      AST_TARGET_FEATURE_X86_AVX);
  ok = ok && tp && ast_target_verify(tp);
  ast_target_destroy(tp);
  fixture_close(&f);

  if (!fixture_open_at(&f, "tests/target_cases/native_high8_rex"))
    return 0;
  tp = fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u);
  ok = ok && tp && !ast_target_verify(tp) &&
       target_has_diag(tp, AST_TARGET_DIAG_INVALID_NATIVE_FORM);
  ast_target_destroy(tp);
  fixture_close(&f);

  if (!fixture_open_at(&f, "tests/target_cases/native_invalid_reg"))
    return 0;
  tp = fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u);
  ok = ok && tp && !ast_target_verify(tp) &&
       target_has_diag(tp, AST_TARGET_DIAG_INVALID_NATIVE_REGISTER);
  ast_target_destroy(tp);
  fixture_close(&f);

  if (!fixture_open_at(&f, "tests/target_cases/exact_unknown"))
    return 0;
  tp = fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u);
  ok = ok && tp && !ast_target_verify(tp) &&
       target_has_diag(tp, AST_TARGET_DIAG_INVALID_NATIVE_FORM);
  ast_target_destroy(tp);
  fixture_close(&f);
  return ok;
}

static int test_case(int c) {
  AstTargetConfig tc;
  AstTargetRegisterInfo r;
  switch (c) {
  case 0:
    return ast_target_config_from_triple("x86_64-unknown-linux-gnu", &tc) &&
           tc.arch == AST_TARGET_ARCH_X86_64 && tc.os == AST_TARGET_OS_LINUX &&
           tc.pointer_bits == 64u;
  case 1:
    return ast_target_config_from_triple("x86_64-pc-windows-msvc", &tc) &&
           tc.os == AST_TARGET_OS_WINDOWS;
  case 2: {
    uint64_t f = ast_target_feature_closure(AST_TARGET_ARCH_X86_64,
                                            AST_TARGET_FEATURE_X86_AVX2);
    return (f & (AST_TARGET_FEATURE_X86_AVX2 | AST_TARGET_FEATURE_X86_AVX |
                 AST_TARGET_FEATURE_X86_SSE2)) ==
           (AST_TARGET_FEATURE_X86_AVX2 | AST_TARGET_FEATURE_X86_AVX |
            AST_TARGET_FEATURE_X86_SSE2);
  }
  case 3: {
    uint64_t f = ast_target_feature_closure(AST_TARGET_ARCH_AARCH64,
                                            AST_TARGET_FEATURE_A64_SVE2);
    return (f & AST_TARGET_FEATURE_A64_SVE) != 0u;
  }
  case 4: {
    uint64_t f = ast_target_feature_closure(AST_TARGET_ARCH_RISCV64,
                                            AST_TARGET_FEATURE_RV_D |
                                                AST_TARGET_FEATURE_RV_ZFH);
    return (f & AST_TARGET_FEATURE_RV_F) != 0u;
  }
  case 5: {
    const char *regs1[] = {"ah", "rbx"};
    const char *regs2[] = {"ah", "r8b"};
    return ast_target_x86_registers_compatible(regs1, 2u, 0) &&
           !ast_target_x86_registers_compatible(regs2, 2u, 0) &&
           !ast_target_x86_registers_compatible(regs1, 2u, 1);
  }
  case 6:
    tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
    return ast_target_parse_register(&tc, "rip", &r) && r.special &&
           r.reg_class == AST_TARGET_REGCLASS_SPECIAL;
  case 7:
    tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
    if (ast_target_parse_register(&tc, "ymm0", &r))
      return 0;
    tc.features |= AST_TARGET_FEATURE_X86_AVX;
    return ast_target_parse_register(&tc, "ymm0", &r) &&
           r.reg_class == AST_TARGET_REGCLASS_VEC256;
  case 8:
    tc = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
    return !ast_target_parse_register(&tc, "x31", &r) &&
           ast_target_parse_register(&tc, "sp", &r) && r.special &&
           ast_target_parse_register(&tc, "xzr", &r) && r.special;
  case 9:
    tc = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
    if (ast_target_parse_register(&tc, "z0", &r))
      return 0;
    tc.features |= AST_TARGET_FEATURE_A64_SVE;
    return ast_target_parse_register(&tc, "z0", &r) &&
           r.reg_class == AST_TARGET_REGCLASS_SCALABLE_VECTOR;
  case 10:
    tc = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
    return ast_target_parse_register(&tc, "fp", &r) && r.index == 8u &&
           ast_target_parse_register(&tc, "a7", &r) && r.index == 17u;
  case 11:
    tc = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
    if (ast_target_parse_register(&tc, "fa0", &r))
      return 0;
    tc.features |= AST_TARGET_FEATURE_RV_F;
    return ast_target_parse_register(&tc, "fa0", &r) && r.index == 10u &&
           r.reg_class == AST_TARGET_REGCLASS_FP32;
  case 12:
    return ast_target_immediate_fits(-2048, 12u, 1) &&
           ast_target_immediate_fits(2047, 12u, 1) &&
           !ast_target_immediate_fits(2048, 12u, 1);
  case 13:
    return ast_target_immediate_fits(4095, 12u, 0) &&
           !ast_target_immediate_fits(4096, 12u, 0) &&
           !ast_target_immediate_fits(-1, 12u, 0);
  case 14: {
    Fixture f;
    if (!fixture_open(&f))
      return 0;
    tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
    tc.os = AST_TARGET_OS_LINUX;
    AstTargetProgram *tp = ast_target_legalize(f.mir, &tc);
    int ok = tp && ast_target_verify(tp) &&
             ast_target_function_count(tp) == ast_mir_function_count(f.mir) &&
             ast_target_stats(tp).unsupported_count == 0u;
    ast_target_destroy(tp);
    fixture_close(&f);
    return ok && native_validation_smoke();
  }
  case 15:
    tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
    return ast_target_parse_register(&tc, "rax", &r) && r.index == 0u &&
           ast_target_parse_register(&tc, "rcx", &r) && r.index == 1u &&
           ast_target_parse_register(&tc, "rdx", &r) && r.index == 2u &&
           ast_target_parse_register(&tc, "rbx", &r) && r.index == 3u &&
           ast_target_parse_register(&tc, "rsp", &r) && r.index == 4u &&
           ast_target_parse_register(&tc, "rbp", &r) && r.index == 5u &&
           ast_target_parse_register(&tc, "rsi", &r) && r.index == 6u &&
           ast_target_parse_register(&tc, "rdi", &r) && r.index == 7u;
  case 16:
    tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
    return ast_target_parse_register(&tc, "r8b", &r) && r.index == 8u &&
           r.width == 8u && r.extended &&
           ast_target_parse_register(&tc, "r15d", &r) && r.index == 15u &&
           r.width == 32u && r.extended &&
           ast_target_parse_register(&tc, "sil", &r) && r.index == 6u &&
           r.extended;
  case 17:
    tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
    if (ast_target_parse_register(&tc, "xmm16", &r) ||
        ast_target_parse_register(&tc, "zmm31", &r) ||
        ast_target_parse_register(&tc, "k7", &r))
      return 0;
    tc.features |= AST_TARGET_FEATURE_X86_AVX512F;
    return ast_target_parse_register(&tc, "xmm16", &r) && r.index == 16u &&
           ast_target_parse_register(&tc, "zmm31", &r) && r.index == 31u &&
           ast_target_parse_register(&tc, "k7", &r) && r.index == 7u;
  case 18:
    tc = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
    return ast_target_parse_register(&tc, "w0", &r) && r.index == 0u &&
           r.width == 32u && ast_target_parse_register(&tc, "x30", &r) &&
           r.index == 30u && r.width == 64u &&
           ast_target_parse_register(&tc, "d31", &r) && r.index == 31u &&
           r.width == 64u && ast_target_parse_register(&tc, "q31", &r) &&
           r.index == 31u && r.width == 128u &&
           ast_target_parse_register(&tc, "wsp", &r) && r.special &&
           r.index == 31u;
  case 19:
    tc = ast_target_config_default(AST_TARGET_ARCH_AARCH64);
    if (ast_target_parse_register(&tc, "p15", &r))
      return 0;
    tc.features |= AST_TARGET_FEATURE_A64_SVE;
    return ast_target_parse_register(&tc, "p15", &r) && r.index == 15u &&
           r.reg_class == AST_TARGET_REGCLASS_PREDICATE;
  case 20:
    tc = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
    tc.features |= AST_TARGET_FEATURE_RV_D;
    return ast_target_parse_register(&tc, "ft11", &r) && r.index == 31u &&
           ast_target_parse_register(&tc, "fs11", &r) && r.index == 27u &&
           ast_target_parse_register(&tc, "fa7", &r) && r.index == 17u;
  case 21:
    tc = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
    if (ast_target_parse_register(&tc, "v31", &r))
      return 0;
    tc.features |= AST_TARGET_FEATURE_RV_V;
    return ast_target_parse_register(&tc, "v31", &r) && r.index == 31u &&
           r.reg_class == AST_TARGET_REGCLASS_SCALABLE_VECTOR;
  case 22:
    return ast_target_feature_from_name(AST_TARGET_ARCH_X86_64, "AVX2") ==
               AST_TARGET_FEATURE_X86_AVX2 &&
           ast_target_feature_from_name(AST_TARGET_ARCH_AARCH64, "SvE2") ==
               AST_TARGET_FEATURE_A64_SVE2 &&
           ast_target_feature_from_name(AST_TARGET_ARCH_RISCV64, "ZBB") ==
               AST_TARGET_FEATURE_RV_ZBB;
  case 23:
    return ast_target_config_from_triple("aarch64-apple-darwin", &tc) &&
           tc.os == AST_TARGET_OS_MACOS &&
           ast_target_config_from_triple("riscv64-unknown-elf", &tc) &&
           tc.os == AST_TARGET_OS_BARE_METAL;
  case 24:
    return !ast_target_config_from_triple("x86_64-unknown-mysteryos", &tc) &&
           !ast_target_config_from_triple("mips64-unknown-linux-gnu", &tc);
  case 25:
    return ast_target_immediate_fits(INT64_MIN, 64u, 1) &&
           ast_target_immediate_fits(INT64_MAX, 64u, 1) &&
           ast_target_immediate_fits(INT64_MAX, 64u, 0) &&
           !ast_target_immediate_fits(-1, 64u, 0) &&
           !ast_target_immediate_fits(0, 0u, 0) &&
           !ast_target_immediate_fits(0, 65u, 1);
  case 26: {
    Fixture f;
    if (!fixture_open(&f))
      return 0;
    AstTargetProgram *tp =
        fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u);
    if (!tp || !ast_target_verify(tp) || !tp->function_count ||
        !tp->functions[0].info.vreg_count) {
      ast_target_destroy(tp);
      fixture_close(&f);
      return 0;
    }
    AstLegalizeAction old = tp->functions[0].values[0].action;
    tp->functions[0].values[0].action = AST_LEGALIZE_INVALID;
    int ok = !ast_target_verify(tp);
    tp->functions[0].values[0].action = old;
    ok = ok && ast_target_verify(tp);
    ast_target_destroy(tp);
    fixture_close(&f);
    return ok;
  }
  case 27: {
    Fixture f;
    if (!fixture_open(&f))
      return 0;
    AstTargetProgram *tp =
        fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u);
    if (!tp || !ast_target_verify(tp) || !tp->function_count) {
      ast_target_destroy(tp);
      fixture_close(&f);
      return 0;
    }
    size_t fi = 0u;
    while (fi < tp->function_count &&
           tp->functions[fi].info.instruction_count == 0u)
      fi++;
    if (fi == tp->function_count) {
      ast_target_destroy(tp);
      fixture_close(&f);
      return 0;
    }
    AstMirInstId old = tp->functions[fi].instructions[0].id;
    tp->functions[fi].instructions[0].id = old + 1u;
    int ok = !ast_target_verify(tp);
    tp->functions[fi].instructions[0].id = old;
    ok = ok && ast_target_verify(tp);
    ast_target_destroy(tp);
    fixture_close(&f);
    return ok;
  }
  case 28: {
    Fixture f;
    if (!fixture_open(&f))
      return 0;
    AstTargetProgram *tp =
        fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX, 0u);
    if (!tp || !ast_target_verify(tp)) {
      ast_target_destroy(tp);
      fixture_close(&f);
      return 0;
    }
    tp->stats.legal_count++;
    int ok = !ast_target_verify(tp);
    tp->stats.legal_count--;
    ok = ok && ast_target_verify(tp);
    ast_target_destroy(tp);
    fixture_close(&f);
    return ok;
  }
  case 29: {
    Fixture f;
    if (!fixture_open(&f))
      return 0;
    AstTargetProgram *a =
        fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX,
                       AST_TARGET_FEATURE_X86_AVX2);
    AstTargetProgram *b =
        fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX,
                       AST_TARGET_FEATURE_X86_AVX2);
    TextCapture x = {0}, y = {0};
    if (!a || !b || !ast_target_verify(a) || !ast_target_verify(b)) {
      ast_target_destroy(a);
      ast_target_destroy(b);
      fixture_close(&f);
      return 0;
    }
    ast_target_dump_program(a, capture_write, &x);
    ast_target_dump_program(b, capture_write, &y);
    int ok = !x.failed && !y.failed && x.length == y.length && x.length > 0u &&
             memcmp(x.data, y.data, x.length) == 0;
    capture_free(&x);
    capture_free(&y);
    ast_target_destroy(a);
    ast_target_destroy(b);
    fixture_close(&f);
    return ok;
  }
  case 30: {
    uint64_t avx = ast_target_feature_closure(AST_TARGET_ARCH_X86_64,
                                              AST_TARGET_FEATURE_X86_AVX2);
    return !(avx & AST_TARGET_FEATURE_X86_POPCNT) &&
           ast_target_feature_from_name(AST_TARGET_ARCH_X86_64, "POPCNT") ==
               AST_TARGET_FEATURE_X86_POPCNT &&
           !ast_target_feature_from_name(AST_TARGET_ARCH_AARCH64, "popcnt");
  }
  case 31:
  case 32:
  case 33:
  case 34: {
    Fixture f;
    if (!fixture_open_at(&f, "tests/target_cases/native_popcnt"))
      return 0;
    AstTargetProgram *tp =
        fixture_target(&f, AST_TARGET_ARCH_X86_64, AST_TARGET_OS_LINUX,
                       c == 31 ? 0u : AST_TARGET_FEATURE_X86_POPCNT);
    int ok = 0;
    if (c == 31)
      ok = tp && !ast_target_verify(tp) &&
           target_has_diag(tp, AST_TARGET_DIAG_REQUIRED_FEATURE);
    else if (tp && ast_target_verify(tp)) {
      uint32_t native_count = 0u;
      for (size_t fi = 0; fi < tp->function_count; ++fi)
        for (uint32_t i = 0; i < tp->functions[fi].info.instruction_count;
             ++i) {
          AstTargetInstructionInfo *instruction =
              &tp->functions[fi].instructions[i];
          if (instruction->op != AST_MIR_OP_NATIVE)
            continue;
          if (instruction->required_features == AST_TARGET_FEATURE_X86_POPCNT)
            ++native_count;
          if (c == 32)
            instruction->required_features = 0u;
        }
      if (c == 33)
        tp->config.features &= ~AST_TARGET_FEATURE_X86_POPCNT;
      ok = native_count == 2u &&
           (c == 34 ? ast_target_verify(tp) : !ast_target_verify(tp));
    }
    ast_target_destroy(tp);
    fixture_close(&f);
    return ok;
  }
  case 35:
  case 36: {
    Fixture f;
    if (!fixture_open_at(&f, "tests/target_cases/legacy_supported_scalars"))
      return 0;
    AstTargetArch arch =
        c == 35 ? AST_TARGET_ARCH_X86_32 : AST_TARGET_ARCH_X86_16;
    AstTargetProgram *tp =
        fixture_target(&f, arch, AST_TARGET_OS_BARE_METAL, 0u);
    AstTargetConfig cfg =
        tp ? ast_target_config(tp)
           : ast_target_config_default(AST_TARGET_ARCH_INVALID);
    int ok = tp && cfg.pointer_bits == 32u && ast_target_verify(tp) &&
             ast_target_stats(tp).unsupported_count == 0u;
    ast_target_destroy(tp);
    fixture_close(&f);
    return ok;
  }
  case 37:
  case 38: {
    Fixture f;
    if (!fixture_open_at(&f, "tests/target_cases/legacy_unsupported_scalars"))
      return 0;
    AstTargetArch arch =
        c == 37 ? AST_TARGET_ARCH_X86_32 : AST_TARGET_ARCH_X86_16;
    AstTargetProgram *tp =
        fixture_target(&f, arch, AST_TARGET_OS_BARE_METAL, 0u);
    AstTargetStats st = tp ? ast_target_stats(tp) : (AstTargetStats){0};
    int ok = tp && !ast_target_verify(tp) && st.unsupported_count >= 2u &&
             target_has_diag(tp, AST_TARGET_DIAG_UNSUPPORTED_TYPE);
    ast_target_destroy(tp);
    fixture_close(&f);
    return ok;
  }
  default:
    return 0;
  }
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  char *end = NULL;
  long c = strtol(argv[1], &end, 10);
  if (!end || *end || c < 0 || c > 38)
    return 2;
  if (!test_case((int)c)) {
    fprintf(stderr, "target case %ld failed\n", c);
    return 1;
  }
  return 0;
}
