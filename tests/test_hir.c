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
} Fixture;

static int name_is(AstName name, const char *text) {
  return ast_name_equal_cstr(name, text);
}
static int text_is(AstHirText value, const char *text) {
  size_t n = strlen(text);
  return value.data && value.length == n && memcmp(value.data, text, n) == 0;
}

static void close_fixture(Fixture *f) {
  if (!f)
    return;
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
  memset(f, 0, sizeof(*f));
}

static int open_path(const char *root, const char *entry, Fixture *f) {
  if (!f)
    return 0;
  memset(f, 0, sizeof(*f));
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 3u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend)
    return 0;
  if (ast_frontend_add_import_root(f->frontend, root) != 0) {
    close_fixture(f);
    return 0;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(f->frontend, entries, 1u) != 0) {
    close_fixture(f);
    return 0;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 3u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0) {
    close_fixture(f);
    return 0;
  }
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 3u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir) ||
      ast_hir_stats(f->hir).error_count != 0u) {
    close_fixture(f);
    return 0;
  }
  return 1;
}

static int open_case(const char *group, const char *name, Fixture *f) {
  char root[4096], entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/%s/%s", ASTERASM_SOURCE_DIR,
                    group, name);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 0;
  return open_path(root, entry, f);
}

static int test_metadata(void) {
  Fixture f;
  if (!open_case("hir_cases", "metadata", &f))
    return 0;
  int ok = 1;
  if (ast_hir_target_count(f.hir) != 1u)
    ok = 0;
  AstHirTargetInfo target;
  memset(&target, 0, sizeof(target));
  if (!ast_hir_target_at(f.hir, 0u, &target) ||
      !name_is(target.module_name, "hir.metadata") ||
      !text_is(target.triple, "x86_64-unknown-linux-gnu") ||
      target.item_count != 3u)
    ok = 0;
  if (ok) {
    if (!name_is(target.items[0].name, "cpu") ||
        target.items[0].value_literal_kind != AST_TOK_STRING ||
        !text_is(target.items[0].value_text, "baseline"))
      ok = 0;
    if (!name_is(target.items[1].name, "tune") ||
        target.items[1].constant.kind != AST_SEMA_CONST_INTEGER ||
        target.items[1].constant.lo != 4u)
      ok = 0;
    if (!target.items[2].is_feature || target.items[2].feature_sign != 1 ||
        !text_is(target.items[2].feature, "sse2"))
      ok = 0;
  }
  const AstHirFunction *fn = NULL;
  AstHirFunctionInfo fi;
  memset(&fi, 0, sizeof(fi));
  if (!ast_hir_find_function(f.hir, "hir.metadata", "f", &fn) ||
      !ast_hir_function_info(fn, &fi))
    ok = 0;
  if (ok) {
    uint32_t required = AST_HIR_FUNCTION_EXPORT | AST_HIR_FUNCTION_LEAF |
                        AST_HIR_FUNCTION_NO_UNWIND;
    if ((fi.flags & required) != required ||
        fi.frame_policy != AST_HIR_FRAME_AUTO || fi.stack_alignment != 16u)
      ok = 0;
    if (!fi.abi.is_present || fi.abi.is_literal || !text_is(fi.abi.text, "c"))
      ok = 0;
    if (!fi.target_constraint.is_present || !fi.target_constraint.is_literal ||
        !text_is(fi.target_constraint.text, "x86_64"))
      ok = 0;
    if (!fi.requires_constraint.is_present ||
        !fi.requires_constraint.is_literal ||
        !text_is(fi.requires_constraint.text, "sse2"))
      ok = 0;
  }
  const AstHirFunction *ext = NULL;
  AstHirFunctionInfo ei;
  memset(&ei, 0, sizeof(ei));
  if (!ast_hir_find_function(f.hir, "hir.metadata", "ext", &ext) ||
      !ast_hir_function_info(ext, &ei))
    ok = 0;
  if (ok &&
      (!(ei.flags & AST_HIR_FUNCTION_EXTERN) || !ei.link_name.is_present ||
       !ei.link_name.is_literal || !text_is(ei.link_name.text, "ext_alias")))
    ok = 0;
  const AstHirFunction *dispatch = NULL;
  AstHirFunctionInfo di;
  memset(&di, 0, sizeof(di));
  if (!ast_hir_find_function(f.hir, "hir.metadata", "choose", &dispatch) ||
      !ast_hir_function_info(dispatch, &di))
    ok = 0;
  if (ok && (!(di.flags & AST_HIR_FUNCTION_DISPATCH) ||
             di.dispatch_case_count != 2u || di.block_count != 0u))
    ok = 0;
  if (ok) {
    AstHirDispatchCaseInfo c0, c1;
    memset(&c0, 0, sizeof(c0));
    memset(&c1, 0, sizeof(c1));
    if (!ast_hir_function_dispatch_case(dispatch, 0u, &c0) ||
        !ast_hir_function_dispatch_case(dispatch, 1u, &c1))
      ok = 0;
    if (!c0.is_else && c0.condition.kind == AST_SEMA_CONST_BOOL &&
        name_is(c0.target, "fast")) { /* expected */
    } else
      ok = 0;
    if (!c1.is_else || !name_is(c1.target, "slow"))
      ok = 0;
  }
  close_fixture(&f);
  return ok;
}

static int test_initializers(void) {
  Fixture f;
  if (!open_case("hir_cases", "globals", &f))
    return 0;
  int ok = ast_hir_global_count(f.hir) == 6u;
  int saw_magic = 0, saw_pair = 0, saw_pointer = 0, saw_bss = 0;
  for (size_t i = 0u; ok && i < ast_hir_global_count(f.hir); ++i) {
    AstHirGlobalInfo gi;
    memset(&gi, 0, sizeof(gi));
    if (!ast_hir_global_at(f.hir, i, &gi)) {
      ok = 0;
      break;
    }
    const AstHirInitializer *init = ast_hir_global_initializer(f.hir, i);
    AstHirInitializerInfo ii;
    memset(&ii, 0, sizeof(ii));
    if (!init || !ast_hir_initializer_info(init, &ii)) {
      ok = 0;
      break;
    }
    if (!gi.section_name.data ||
        (name_is(gi.name, "scratch") ? !text_is(gi.section_name, ".bss")
         : (name_is(gi.name, "counter") || name_is(gi.name, "pmagic"))
             ? !text_is(gi.section_name, ".data")
             : !text_is(gi.section_name, ".rodata"))) {
      ok = 0;
      break;
    }
    if (name_is(gi.name, "magic")) {
      saw_magic = 1;
      if (ii.kind != AST_HIR_INIT_CONSTANT ||
          ii.constant.kind != AST_SEMA_CONST_INTEGER || ii.constant.lo != 42u ||
          gi.section_alignment != 16u)
        ok = 0;
    } else if (name_is(gi.name, "pair")) {
      saw_pair = 1;
      if (ii.kind != AST_HIR_INIT_AGGREGATE || ii.child_count != 2u)
        ok = 0;
      for (uint32_t c = 0u; ok && c < ii.child_count; ++c) {
        AstHirInitializerInfo ci;
        memset(&ci, 0, sizeof(ci));
        if (!ast_hir_initializer_info(ast_hir_initializer_child(init, c),
                                      &ci) ||
            ci.kind != AST_HIR_INIT_CONSTANT)
          ok = 0;
      }
    } else if (name_is(gi.name, "pmagic")) {
      saw_pointer = 1;
      if (ii.kind != AST_HIR_INIT_SYMBOL || !name_is(ii.symbol, "magic") ||
          !name_is(ii.module, "hir.globals") || ii.addend != 0)
        ok = 0;
    } else if (name_is(gi.name, "scratch")) {
      saw_bss = 1;
      if (!(gi.flags & AST_HIR_GLOBAL_BSS) || ii.kind != AST_HIR_INIT_ZERO)
        ok = 0;
    }
  }
  if (!saw_magic || !saw_pair || !saw_pointer || !saw_bss)
    ok = 0;
  close_fixture(&f);
  return ok;
}

static int test_native(void) {
  Fixture f;
  if (!open_case("sema_cases", "native_success", &f))
    return 0;
  int ok = 1;
  const AstHirFunction *fn = NULL;
  if (!ast_hir_find_function(f.hir, "sema.native_success", "bridge", &fn) ||
      ast_hir_function_native_region_count(fn) != 1u)
    ok = 0;
  if (ok) {
    const AstHirNativeRegion *region = ast_hir_function_native_region(fn, 0u);
    AstHirNativeRegionInfo ri;
    memset(&ri, 0, sizeof(ri));
    AstHirNativeStmtInfo si;
    memset(&si, 0, sizeof(si));
    if (!region || !ast_hir_native_region_info(region, &ri) ||
        !name_is(ri.arch, "x86_64") || ri.input_count != 1u ||
        ri.output_count != 1u || ri.statement_count != 1u)
      ok = 0;
    if (ok && (!ast_hir_native_region_statement(region, 0u, &si) ||
               si.kind != AST_HIR_NATIVE_INSTRUCTION ||
               !name_is(si.mnemonic, "mov") || si.operand_count != 2u))
      ok = 0;
    if (ok) {
      AstHirNativeExprInfo src;
      memset(&src, 0, sizeof(src));
      if (!si.operands[1].value ||
          !ast_hir_native_expr_info(si.operands[1].value, &src) ||
          src.kind != AST_HIR_NATIVE_EXPR_NAME ||
          src.binding_kind != AST_HIR_NATIVE_BINDING_INPUT ||
          src.binding_index != 0u)
        ok = 0;
    }
  }
  close_fixture(&f);
  if (!ok)
    return 0;

  if (!open_case("hir_cases", "exact_bytes", &f))
    return 0;
  fn = NULL;
  if (!ast_hir_find_function(f.hir, "hir.exact_bytes", "raw", &fn))
    ok = 0;
  if (ok) {
    const AstHirNativeRegion *region = ast_hir_function_native_region(fn, 0u);
    AstHirNativeRegionInfo ri;
    AstHirNativeStmtInfo si;
    memset(&ri, 0, sizeof(ri));
    memset(&si, 0, sizeof(si));
    if (!region || !ast_hir_native_region_info(region, &ri) ||
        (ri.flags & (AST_HIR_NATIVE_EXACT | AST_HIR_NATIVE_UNSAFE_EXACT)) !=
            (AST_HIR_NATIVE_EXACT | AST_HIR_NATIVE_UNSAFE_EXACT))
      ok = 0;
    if (ok && (!ast_hir_native_region_statement(region, 0u, &si) ||
               si.kind != AST_HIR_NATIVE_BYTES || si.byte_count != 2u ||
               si.bytes[0] != 0x90u || si.bytes[1] != 0xc3u))
      ok = 0;
  }
  close_fixture(&f);
  return ok;
}

static int test_canonical_text(void) {
  Fixture f;
  if (!open_case("hir_cases", "canonical_text", &f))
    return 0;
  int ok = 1;
  AstHirTargetInfo target;
  memset(&target, 0, sizeof(target));
  if (!ast_hir_target_at(f.hir, 0u, &target) ||
      !text_is(target.triple, "x86_64-linux") || target.item_count != 2u ||
      !text_is(target.items[0].value_text, "base\nline") ||
      !text_is(target.items[1].feature, "sse2"))
    ok = 0;

  const AstHirFunction *ext = NULL;
  AstHirFunctionInfo fi;
  memset(&fi, 0, sizeof(fi));
  if (!ast_hir_find_function(f.hir, "hir.canonical_text", "ext", &ext) ||
      !ast_hir_function_info(ext, &fi) || !fi.link_name.is_present ||
      !text_is(fi.link_name.text, "ext\\alias"))
    ok = 0;

  if (ast_hir_global_count(f.hir) != 1u)
    ok = 0;
  AstHirGlobalInfo gi;
  memset(&gi, 0, sizeof(gi));
  if (!ast_hir_global_at(f.hir, 0u, &gi) ||
      !text_is(gi.section_name, ".rodata"))
    ok = 0;

  const AstHirFunction *raw = NULL;
  if (!ast_hir_find_function(f.hir, "hir.canonical_text", "raw", &raw) ||
      ast_hir_function_native_region_count(raw) != 1u)
    ok = 0;
  if (ok) {
    const AstHirNativeRegion *region = ast_hir_function_native_region(raw, 0u);
    AstHirNativeStmtInfo stmt;
    memset(&stmt, 0, sizeof(stmt));
    if (!ast_hir_native_region_statement(region, 0u, &stmt) ||
        stmt.operand_count != 2u)
      ok = 0;
    if (ok) {
      AstHirNativeExprInfo value;
      memset(&value, 0, sizeof(value));
      static const unsigned char expected[] = {'a', '\n', 0xce, 0xbb};
      if (!ast_hir_native_expr_info(stmt.operands[1].value, &value) ||
          value.kind != AST_HIR_NATIVE_EXPR_LITERAL ||
          value.literal.length != sizeof(expected) || !value.literal.data ||
          memcmp(value.literal.data, expected, sizeof(expected)) != 0)
        ok = 0;
    }
  }
  close_fixture(&f);
  return ok;
}

static int test_cfg_public_api(void) {
  Fixture f;
  if (!open_case("sema_cases", "flow_success", &f))
    return 0;
  int ok = 1;
  const AstHirFunction *fn = NULL;
  if (!ast_hir_find_function(f.hir, "sema.flow_success", "choose2", &fn))
    ok = 0;
  AstHirFunctionInfo fi;
  memset(&fi, 0, sizeof(fi));
  if (ok && (!ast_hir_function_info(fn, &fi) || fi.block_count != 4u ||
             fi.entry_block != 0u))
    ok = 0;
  for (uint32_t bi = 0u; ok && bi < fi.block_count; ++bi) {
    AstHirBlockInfo b;
    memset(&b, 0, sizeof(b));
    if (!ast_hir_function_block(fn, bi, &b) || b.id != bi ||
        b.terminator.kind == AST_HIR_TERM_NONE) {
      ok = 0;
      break;
    }
    for (uint32_t ii = 0u; ii < b.instruction_count; ++ii) {
      AstHirInstructionInfo inst;
      memset(&inst, 0, sizeof(inst));
      if (!ast_hir_function_instruction(fn, b.instructions[ii], &inst)) {
        ok = 0;
        break;
      }
      if (inst.result != AST_HIR_INVALID_ID) {
        AstHirValueInfo v;
        memset(&v, 0, sizeof(v));
        if (!ast_hir_function_value(fn, inst.result, &v) ||
            v.kind != AST_HIR_VALUE_INSTRUCTION_RESULT) {
          ok = 0;
          break;
        }
      }
    }
  }
  close_fixture(&f);
  return ok;
}

static int test_instruction_effect_locality(void) {
  Fixture f;
  if (!open_case("hir_cases", "effect_locality", &f))
    return 0;
  const AstHirFunction *fn = NULL;
  AstHirFunctionInfo info;
  memset(&info, 0, sizeof(info));
  int ok = ast_hir_find_function(f.hir, "hir.effect_locality", "probe", &fn) &&
           ast_hir_function_info(fn, &info);
  int saw_call = 0, saw_deref = 0, saw_binary = 0;
  for (uint32_t ii = 0u; ok && ii < info.instruction_count; ++ii) {
    AstHirInstructionInfo inst;
    memset(&inst, 0, sizeof(inst));
    if (!ast_hir_function_instruction(fn, ii, &inst)) {
      ok = 0;
      break;
    }
    if (inst.op == AST_HIR_OP_CALL) {
      saw_call = 1;
      if (inst.effects != AST_SEMA_EFFECT_CALL)
        ok = 0;
    } else if (inst.op == AST_HIR_OP_LOAD && inst.token_op == AST_TOK_STAR) {
      saw_deref = 1;
      if (inst.effects != AST_SEMA_EFFECT_READ_MEMORY)
        ok = 0;
    } else if (inst.op == AST_HIR_OP_BINARY && inst.token_op == AST_TOK_PLUS) {
      saw_binary = 1;
      if (inst.effects != 0u)
        ok = 0;
    }
  }
  ok &= saw_call && saw_deref && saw_binary;
  close_fixture(&f);
  return ok;
}

static int test_runtime_strings(void) {
  Fixture f;
  if (!open_case("hir_cases", "runtime_strings", &f))
    return 0;
  int ok = 1, saw_binary = 0, saw_plain = 0, saw_empty = 0;
  size_t end = 0u, literals = 0u;
  for (size_t m = 0u; m < ast_hir_module_count(f.hir); ++m) {
    AstHirModuleInfo module;
    if (!ast_hir_module_at(f.hir, m, &module) || module.first_global != end) {
      ok = 0;
      break;
    }
    end += module.global_count;
    for (size_t g = module.first_global; g < end; ++g) {
      AstHirGlobalInfo global;
      AstHirInitializerInfo init;
      if (!ast_hir_global_at(f.hir, g, &global) ||
          global.module_name.span.length != module.name.span.length ||
          memcmp(global.module_name.text, module.name.text,
                 module.name.span.length) != 0 ||
          !ast_hir_initializer_info(global.initializer, &init)) {
        ok = 0;
        break;
      }
      if (init.kind != AST_HIR_INIT_LITERAL)
        continue;
      ++literals;
      if (!(global.flags & AST_HIR_GLOBAL_READONLY) ||
          (global.flags & AST_HIR_GLOBAL_WRITABLE) ||
          !text_is(global.section_name, ".rodata") ||
          ast_sema_type_kind(global.type) != AST_SEMA_TYPE_ARRAY ||
          ast_sema_type_size(global.type) !=
              (init.literal.length ? init.literal.length : 1u))
        ok = 0;
      if (init.literal_kind == AST_TOK_C_STRING &&
          (!init.literal.length ||
           init.literal.data[init.literal.length - 1u] != 0))
        ok = 0;
      if (init.literal_kind == AST_TOK_BYTE_STRING &&
          init.literal.length == 2u) {
        static const unsigned char expected[] = {0x80u, 0xffu};
        saw_binary = memcmp(init.literal.data, expected, sizeof(expected)) == 0;
      }
      if (init.literal_kind == AST_TOK_STRING && text_is(init.literal, "plain"))
        saw_plain = 1;
      if (init.literal_kind == AST_TOK_STRING && !init.literal.length)
        saw_empty = 1;
    }
  }
  ok &= end == ast_hir_global_count(f.hir) && literals >= 20u && saw_binary &&
        saw_plain && saw_empty;
  close_fixture(&f);
  return ok;
}

int main(void) {
  int ok = 1;
  int r = test_metadata();
  if (!r)
    fputs("metadata failed\n", stderr);
  ok &= r;
  r = test_initializers();
  if (!r)
    fputs("initializers failed\n", stderr);
  ok &= r;
  r = test_native();
  if (!r)
    fputs("native failed\n", stderr);
  ok &= r;
  r = test_canonical_text();
  if (!r)
    fputs("canonical text failed\n", stderr);
  ok &= r;
  r = test_cfg_public_api();
  if (!r)
    fputs("cfg api failed\n", stderr);
  ok &= r;
  r = test_instruction_effect_locality();
  if (!r)
    fputs("effect locality failed\n", stderr);
  ok &= r;
  r = test_runtime_strings();
  if (!r)
    fputs("runtime strings failed\n", stderr);
  ok &= r;
  if (!ok) {
    fputs("HIR core API test failed\n", stderr);
    return 1;
  }
  puts("HIR core API tests passed");
  return 0;
}
