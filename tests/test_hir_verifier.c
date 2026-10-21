#include "asterasm/diagnostics.h"
#include "hir_internal.h"

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

static void fixture_destroy(Fixture *f) {
  if (!f)
    return;
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
  memset(f, 0, sizeof(*f));
}

static int fixture_open(const char *case_name, Fixture *out) {
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/sema_cases/%s",
                    ASTERASM_SOURCE_DIR, case_name);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (!out || rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 0;
  memset(out, 0, sizeof(*out));
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 2u;
  out->frontend = ast_frontend_create(&fc);
  if (!out->frontend)
    return 0;
  if (ast_frontend_add_import_root(out->frontend, root) != 0) {
    fixture_destroy(out);
    return 0;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(out->frontend, entries, 1u) != 0) {
    fixture_destroy(out);
    return 0;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 2u;
  out->sema = ast_sema_create(out->frontend, &sc);
  if (!out->sema || ast_sema_check(out->sema) != 0) {
    fixture_destroy(out);
    return 0;
  }
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  hc.verify_after_build = 0u;
  out->hir = ast_hir_build(out->frontend, out->sema, &hc);
  if (!out->hir || !ast_hir_verify(out->hir)) {
    fixture_destroy(out);
    return 0;
  }
  return 1;
}

static int has_code(const AstHirProgram *hir, AstHirDiagCode code) {
  size_t count = 0u;
  const AstHirDiagnostic *diags = ast_hir_diagnostics(hir, &count);
  for (size_t i = 0u; i < count; ++i)
    if (diags[i].code == code)
      return 1;
  return 0;
}

static AstHirInstruction *find_inst(AstHirFunction *fn, AstHirOp op,
                                    uint32_t occurrence) {
  if (!fn)
    return NULL;
  for (uint32_t i = 0u; i < fn->instruction_count; ++i) {
    if (fn->instructions[i].op == op) {
      if (occurrence == 0u)
        return &fn->instructions[i];
      --occurrence;
    }
  }
  return NULL;
}

static int expect_failure(Fixture *f, AstHirDiagCode code, const char *name) {
  int accepted = ast_hir_verify(f->hir);
  int ok = !accepted && has_code(f->hir, code);
  if (!ok) {
    fprintf(stderr, "verifier mutation '%s' was not rejected with %s\n", name,
            ast_hir_diag_code_name(code));
    size_t count = 0u;
    const AstHirDiagnostic *diags = ast_hir_diagnostics(f->hir, &count);
    for (size_t i = 0u; i < count; ++i) {
      fprintf(stderr, "  %s: %s\n", ast_hir_diag_code_name(diags[i].code),
              diags[i].message ? diags[i].message : "");
    }
  }
  return ok;
}

static int test_missing_terminator(void) {
  Fixture f;
  if (!fixture_open("flow_success", &f))
    return 0;
  f.hir->functions[0]->blocks[0].term.kind = AST_HIR_TERM_NONE;
  int ok =
      expect_failure(&f, AST_HIR_DIAG_MISSING_TERMINATOR, "missing terminator");
  fixture_destroy(&f);
  return ok;
}

static int test_invalid_successor(void) {
  Fixture f;
  if (!fixture_open("flow_success", &f))
    return 0;
  f.hir->functions[0]->blocks[0].term.target_true =
      f.hir->functions[0]->block_count + 7u;
  int ok =
      expect_failure(&f, AST_HIR_DIAG_INVALID_SUCCESSOR, "invalid successor");
  fixture_destroy(&f);
  return ok;
}

static int test_branch_argument_count(void) {
  Fixture f;
  if (!fixture_open("flow_success", &f))
    return 0;
  AstHirFunction *fn = f.hir->functions[0];
  AstHirValueId arg = 0u;
  fn->blocks[1].term.true_args = &arg;
  fn->blocks[1].term.true_arg_count = 1u;
  int ok = expect_failure(&f, AST_HIR_DIAG_BRANCH_ARGUMENT_MISMATCH,
                          "branch argument count");
  fixture_destroy(&f);
  return ok;
}

static int test_condition_type(void) {
  Fixture f;
  if (!fixture_open("flow_success", &f))
    return 0;
  AstHirFunction *fn = f.hir->functions[0];
  const AstSemaType *u64_type = fn->values[2].type;
  fn->values[1].type = u64_type;
  int ok = expect_failure(&f, AST_HIR_DIAG_TYPE_MISMATCH, "condition type");
  fixture_destroy(&f);
  return ok;
}

static int test_cross_branch_dominance(void) {
  Fixture f;
  if (!fixture_open("flow_success", &f))
    return 0;
  AstHirFunction *fn = f.hir->functions[1];
  AstHirInstruction *left_store = NULL;
  for (uint32_t i = 0u; i < fn->blocks[1].instruction_count; ++i) {
    AstHirInstruction *inst = &fn->instructions[fn->blocks[1].instructions[i]];
    if (inst->op == AST_HIR_OP_LOCAL_STORE) {
      left_store = inst;
      break;
    }
  }
  if (!left_store || left_store->operand_count != 1u) {
    fixture_destroy(&f);
    return 0;
  }
  AstHirValueId right_value = AST_HIR_INVALID_ID;
  for (uint32_t i = 0u; i < fn->blocks[2].instruction_count; ++i) {
    AstHirInstruction *inst = &fn->instructions[fn->blocks[2].instructions[i]];
    if (inst->result != AST_HIR_INVALID_ID) {
      right_value = inst->result;
      break;
    }
  }
  if (right_value == AST_HIR_INVALID_ID) {
    fixture_destroy(&f);
    return 0;
  }
  left_store->operands[0] = right_value;
  int ok = expect_failure(&f, AST_HIR_DIAG_DOMINANCE, "cross branch dominance");
  fixture_destroy(&f);
  return ok;
}

static int test_invalid_local(void) {
  Fixture f;
  if (!fixture_open("facts_success", &f))
    return 0;
  AstHirInstruction *load =
      find_inst(f.hir->functions[0], AST_HIR_OP_LOCAL_LOAD, 0u);
  if (!load) {
    fixture_destroy(&f);
    return 0;
  }
  load->local = UINT32_MAX;
  int ok = expect_failure(&f, AST_HIR_DIAG_INVALID_OPERAND, "invalid local");
  fixture_destroy(&f);
  return ok;
}

static int test_invalid_effects(void) {
  Fixture f;
  if (!fixture_open("facts_success", &f))
    return 0;
  AstHirInstruction *load = NULL;
  for (size_t fi = 0u; fi < f.hir->function_count && !load; ++fi)
    load = find_inst(f.hir->functions[fi], AST_HIR_OP_LOAD, 0u);
  if (!load) {
    fixture_destroy(&f);
    return 0;
  }
  load->effects = 0u;
  int ok = expect_failure(&f, AST_HIR_DIAG_INVALID_EFFECTS, "load effects");
  fixture_destroy(&f);
  return ok;
}

static int test_definition_index(void) {
  Fixture f;
  if (!fixture_open("facts_success", &f))
    return 0;
  AstHirFunction *fn = f.hir->functions[1];
  AstHirInstruction *inst = find_inst(fn, AST_HIR_OP_BINARY, 0u);
  if (!inst || inst->result == AST_HIR_INVALID_ID) {
    fixture_destroy(&f);
    return 0;
  }
  fn->values[inst->result].definition_index = fn->instruction_count + 3u;
  int ok = expect_failure(&f, AST_HIR_DIAG_INVALID_CFG, "definition index");
  fixture_destroy(&f);
  return ok;
}

static int test_native_region_index(void) {
  Fixture f;
  if (!fixture_open("native_success", &f))
    return 0;
  AstHirInstruction *native =
      find_inst(f.hir->functions[0], AST_HIR_OP_NATIVE, 0u);
  if (!native) {
    fixture_destroy(&f);
    return 0;
  }
  native->native_region = UINT32_MAX;
  int ok =
      expect_failure(&f, AST_HIR_DIAG_INVALID_OPERAND, "native region index");
  fixture_destroy(&f);
  return ok;
}

static int test_native_binding(void) {
  Fixture f;
  if (!fixture_open("native_success", &f))
    return 0;
  AstHirFunction *fn = f.hir->functions[0];
  if (fn->native_region_count == 0u ||
      fn->native_regions[0].statement_count == 0u) {
    fixture_destroy(&f);
    return 0;
  }
  AstHirNativeStmtCanonical *stmt = &fn->native_regions[0].statements[0];
  if (stmt->operand_count == 0u || !stmt->operands[0].value) {
    fixture_destroy(&f);
    return 0;
  }
  AstHirNativeExpr *value = (AstHirNativeExpr *)stmt->operands[0].value;
  value->binding_kind = AST_HIR_NATIVE_BINDING_INPUT;
  value->binding_index = UINT32_MAX;
  int ok =
      expect_failure(&f, AST_HIR_DIAG_INVALID_NATIVE_REGION, "native binding");
  fixture_destroy(&f);
  return ok;
}

static int test_initializer_type(void) {
  Fixture f;
  if (!fixture_open("aggregate_success", &f))
    return 0;
  if (f.hir->global_count == 0u || !f.hir->globals[0].initializer) {
    fixture_destroy(&f);
    return 0;
  }
  f.hir->globals[0].initializer->type = NULL;
  int ok =
      expect_failure(&f, AST_HIR_DIAG_INVALID_INITIALIZER, "initializer type");
  fixture_destroy(&f);
  return ok;
}

static int test_return_type(void) {
  Fixture f;
  if (!fixture_open("flow_success", &f))
    return 0;
  AstHirFunction *fn = f.hir->functions[0];
  AstHirBlock *return_block = &fn->blocks[2];
  if (return_block->term.kind != AST_HIR_TERM_RETURN) {
    fixture_destroy(&f);
    return 0;
  }
  return_block->term.condition_or_value = AST_HIR_INVALID_ID;
  int ok = expect_failure(&f, AST_HIR_DIAG_TYPE_MISMATCH, "return type");
  fixture_destroy(&f);
  return ok;
}

static int test_missing_constant_payload(void) {
  Fixture f;
  if (!fixture_open("flow_success", &f))
    return 0;
  AstHirInstruction *inst =
      find_inst(f.hir->functions[0], AST_HIR_OP_CONSTANT, 0u);
  if (!inst) {
    fixture_destroy(&f);
    return 0;
  }
  inst->constant.kind = AST_SEMA_CONST_NONE;
  int ok = expect_failure(&f, AST_HIR_DIAG_INVALID_OPERAND,
                          "missing constant payload");
  fixture_destroy(&f);
  return ok;
}

int main(void) {
  struct Test {
    const char *name;
    int (*fn)(void);
  } tests[] = {{"missing_terminator", test_missing_terminator},
               {"invalid_successor", test_invalid_successor},
               {"branch_argument_count", test_branch_argument_count},
               {"condition_type", test_condition_type},
               {"cross_branch_dominance", test_cross_branch_dominance},
               {"invalid_local", test_invalid_local},
               {"invalid_effects", test_invalid_effects},
               {"definition_index", test_definition_index},
               {"native_region_index", test_native_region_index},
               {"native_binding", test_native_binding},
               {"initializer_type", test_initializer_type},
               {"return_type", test_return_type},
               {"missing_constant_payload", test_missing_constant_payload}};
  int ok = 1;
  for (size_t i = 0u; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    int passed = tests[i].fn();
    if (!passed)
      fprintf(stderr, "verifier test failed: %s\n", tests[i].name);
    ok &= passed;
  }
  if (!ok)
    return 1;
  puts("HIR verifier mutation tests passed");
  return 0;
}
