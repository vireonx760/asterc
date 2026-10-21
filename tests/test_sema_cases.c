#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

typedef struct SemaCase {
  const char *name;
  AstSemaDiagCode expected;
  int expect_success;
} SemaCase;

static const SemaCase cases[] = {
    {"core_success", AST_SEMA_DIAG_NONE, 1},
    {"modules_success", AST_SEMA_DIAG_NONE, 1},
    {"generics_success", AST_SEMA_DIAG_NONE, 1},
    {"flow_success", AST_SEMA_DIAG_NONE, 1},
    {"atomic_success", AST_SEMA_DIAG_NONE, 1},
    {"native_success", AST_SEMA_DIAG_NONE, 1},
    {"duplicate_symbol", AST_SEMA_DIAG_DUPLICATE_SYMBOL, 0},
    {"unknown_name", AST_SEMA_DIAG_UNKNOWN_NAME, 0},
    {"unknown_type", AST_SEMA_DIAG_UNKNOWN_TYPE, 0},
    {"not_exported", AST_SEMA_DIAG_SYMBOL_NOT_EXPORTED, 0},
    {"literal_overflow", AST_SEMA_DIAG_INTEGER_LITERAL_OVERFLOW, 0},
    {"const_cycle", AST_SEMA_DIAG_CONST_CYCLE, 0},
    {"static_assert_fail", AST_SEMA_DIAG_STATIC_ASSERT_FAILED, 0},
    {"call_arity", AST_SEMA_DIAG_CALL_ARITY, 0},
    {"return_type", AST_SEMA_DIAG_RETURN_TYPE, 0},
    {"missing_return", AST_SEMA_DIAG_MISSING_RETURN, 0},
    {"uninitialized", AST_SEMA_DIAG_UNINITIALIZED_VALUE, 0},
    {"assign_const", AST_SEMA_DIAG_ASSIGN_TO_CONST, 0},
    {"invalid_store", AST_SEMA_DIAG_INVALID_STORE, 0},
    {"atomic_order_bad", AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, 0},
    {"aggregate_success", AST_SEMA_DIAG_NONE, 1},
    {"aggregate_type_bad", AST_SEMA_DIAG_INVALID_INITIALIZER, 0},
    {"branch_forward_success", AST_SEMA_DIAG_NONE, 1},
    {"branch_into_scope_bad", AST_SEMA_DIAG_INVALID_BRANCH_SCOPE, 0},
    {"loop_break_init_success", AST_SEMA_DIAG_NONE, 1},
    {"loop_break_uninit_bad", AST_SEMA_DIAG_UNINITIALIZED_VALUE, 0},
    {"unreachable_init_bad", AST_SEMA_DIAG_UNINITIALIZED_VALUE, 0},
    {"comptime_dead_branch_success", AST_SEMA_DIAG_NONE, 1},
    {"requires_false_success", AST_SEMA_DIAG_NONE, 1},
    {"attribute_leaf_bad", AST_SEMA_DIAG_INVALID_ATTRIBUTE, 0},
    {"attribute_conflict_bad", AST_SEMA_DIAG_INVALID_ATTRIBUTE, 0},
    {"align_layout_success", AST_SEMA_DIAG_NONE, 1},
    {"load_pointee_bad", AST_SEMA_DIAG_INVALID_LOAD, 0},
    {"string_mutable_bad", AST_SEMA_DIAG_CALL_ARGUMENT_TYPE, 0},
    {"naked_success", AST_SEMA_DIAG_NONE, 1},
    {"generic_fn_expected_success", AST_SEMA_DIAG_NONE, 1},
    {"generic_fn_arg_success", AST_SEMA_DIAG_NONE, 1},
    {"generic_fn_const_success", AST_SEMA_DIAG_NONE, 1},
    {"generic_fn_conflict_bad", AST_SEMA_DIAG_GENERIC_INFERENCE, 0},
    {"generic_fn_uninferred_bad", AST_SEMA_DIAG_GENERIC_INFERENCE, 0},
    {"generic_fn_import_success", AST_SEMA_DIAG_NONE, 1},
    {"pointer_qual_success", AST_SEMA_DIAG_NONE, 1},
    {"pointer_qual_bad", AST_SEMA_DIAG_RETURN_TYPE, 0},
    {"pointer_nested_bad", AST_SEMA_DIAG_RETURN_TYPE, 0},
    {"float_overflow_bad", AST_SEMA_DIAG_FLOAT_LITERAL_OVERFLOW, 0},
    {"float_format_bad", AST_SEMA_DIAG_RETURN_TYPE, 0},
    {"cast_success", AST_SEMA_DIAG_NONE, 1},
    {"cast_trunc_same_bad", AST_SEMA_DIAG_INVALID_CAST, 0},
    {"ptrcast_const_bad", AST_SEMA_DIAG_INVALID_CAST, 0},
    {"ptrcast_unsafe_success", AST_SEMA_DIAG_NONE, 1},
    {"ptrcast_integer_bad", AST_SEMA_DIAG_INVALID_CAST, 0},
    {"bitcast_pointer_bad", AST_SEMA_DIAG_INVALID_CAST, 0},
    {"naked_semantic_bad", AST_SEMA_DIAG_INVALID_ATTRIBUTE, 0},
    {"generic_nominal_const_success", AST_SEMA_DIAG_NONE, 1},
    {"generic_const_expr_success", AST_SEMA_DIAG_NONE, 1},
    {"generic_fn_const_expr_success", AST_SEMA_DIAG_NONE, 1},
    {"generic_fn_const_expr_bad", AST_SEMA_DIAG_GENERIC_INFERENCE, 0},
    {"target_success", AST_SEMA_DIAG_NONE, 1},
    {"target_duplicate_property_bad", AST_SEMA_DIAG_INVALID_TARGET, 0},
    {"target_duplicate_feature_bad", AST_SEMA_DIAG_INVALID_TARGET, 0},
    {"target_nonconst_bad", AST_SEMA_DIAG_INVALID_TARGET, 0},
    {"dispatch_nonconst_bad", AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, 0},
    {"dispatch_success", AST_SEMA_DIAG_NONE, 1},
    {"comptime_for_success", AST_SEMA_DIAG_NONE, 1},
    {"comptime_for_late_assert_bad", AST_SEMA_DIAG_STATIC_ASSERT_FAILED, 0},
    {"comptime_for_zero_dead_success", AST_SEMA_DIAG_NONE, 1},
    {"comptime_for_limit_bad", AST_SEMA_DIAG_COMPTIME_ITERATION_LIMIT, 0},
    {"comptime_for_nonconst_bad", AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION, 0},
    {"generic_duplicate_param_bad", AST_SEMA_DIAG_DUPLICATE_PARAMETER, 0},
    {"generic_param_value_collision_bad", AST_SEMA_DIAG_DUPLICATE_PARAMETER, 0},
    {"comptime_dead_label_success", AST_SEMA_DIAG_NONE, 1},
    {"requires_dead_label_success", AST_SEMA_DIAG_NONE, 1},
    {"comptime_else_if_success", AST_SEMA_DIAG_NONE, 1},
    {"sema_recursion_limit_bad", AST_SEMA_DIAG_RECURSION_LIMIT, 0},
    {"fence_success", AST_SEMA_DIAG_NONE, 1},
    {"fence_missing_order_bad", AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, 0},
    {"fence_duplicate_order_bad", AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, 0},
    {"fence_invalid_attribute_bad", AST_SEMA_DIAG_INVALID_ATOMIC_ORDER, 0},
    {"integer_wrap_success", AST_SEMA_DIAG_NONE, 1},
    {"signed_wrap_success", AST_SEMA_DIAG_NONE, 1},
    {"context_literal_overflow_bad", AST_SEMA_DIAG_INTEGER_LITERAL_OVERFLOW, 0},
    {"signed_min_literal_success", AST_SEMA_DIAG_NONE, 1},
    {"signed_min_literal_overflow_bad", AST_SEMA_DIAG_INTEGER_LITERAL_OVERFLOW,
     0},
    {"intrinsic_select_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_select_bad", AST_SEMA_DIAG_EXPECTED_BOOL, 0},
    {"intrinsic_bitops_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_bitops_bad", AST_SEMA_DIAG_EXPECTED_INTEGER, 0},
    {"intrinsic_rotate_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_rotate_bad", AST_SEMA_DIAG_EXPECTED_INTEGER, 0},
    {"intrinsic_ptr_int_unsafe_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_ptr_to_int_safe_bad", AST_SEMA_DIAG_INVALID_CAST, 0},
    {"intrinsic_int_to_ptr_safe_bad", AST_SEMA_DIAG_INVALID_CAST, 0},
    {"intrinsic_checked_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_wrap_sat_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_checked_type_bad", AST_SEMA_DIAG_EXPECTED_INTEGER, 0},
    {"intrinsic_mul_sat_bad", AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, 0},
    {"intrinsic_const_select_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_const_bitops_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_const_rotate_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_const_wrap_sat_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_u128_wrap_sat_success", AST_SEMA_DIAG_NONE, 1},
    {"intrinsic_bswap_width_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"checked_tuple_index_success", AST_SEMA_DIAG_NONE, 1},
    {"checked_tuple_index_nonconst_bad", AST_SEMA_DIAG_NON_CONSTANT_EXPRESSION,
     0},
    {"checked_tuple_index_oob_bad", AST_SEMA_DIAG_INVALID_ARRAY_SIZE, 0},
    {"shift_modulo_success", AST_SEMA_DIAG_NONE, 1},
    {"signed_shift_success", AST_SEMA_DIAG_NONE, 1},
    {"signed_bitwise_success", AST_SEMA_DIAG_NONE, 1},
    {"bits_shift_success", AST_SEMA_DIAG_NONE, 1},
    {"division_by_zero_bad", AST_SEMA_DIAG_DIVISION_BY_ZERO, 0},
    {"cast_const_success", AST_SEMA_DIAG_NONE, 1},
    {"cast_const_range_bad", AST_SEMA_DIAG_INVALID_CAST, 0},
    {"facts_success", AST_SEMA_DIAG_NONE, 1},
    {"vector_arithmetic_success", AST_SEMA_DIAG_NONE, 1},
    {"vector_compare_mask_success", AST_SEMA_DIAG_NONE, 1},
    {"vector_select_success", AST_SEMA_DIAG_NONE, 1},
    {"vector_float_remainder_bad", AST_SEMA_DIAG_INVALID_BINARY_OPERANDS, 0},
    {"vector_element_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"mask_index_success", AST_SEMA_DIAG_NONE, 1},
    {"svector_pred_success", AST_SEMA_DIAG_NONE, 1},
    {"void_param_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"void_local_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"void_field_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"ptr_void_success", AST_SEMA_DIAG_NONE, 1},
    {"labeled_continue_success", AST_SEMA_DIAG_NONE, 1},
    {"labeled_continue_unknown_bad", AST_SEMA_DIAG_UNKNOWN_LABEL, 0},
    {"labeled_break_success", AST_SEMA_DIAG_NONE, 1},
    {"division_overflow_bad", AST_SEMA_DIAG_INTEGER_DIVISION_OVERFLOW, 0},
    {"remainder_min_success", AST_SEMA_DIAG_NONE, 1},
    {"float_div_zero_success", AST_SEMA_DIAG_NONE, 1},
    {"static_assert_message_success", AST_SEMA_DIAG_NONE, 1},
    {"static_assert_message_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"pred_select_mismatch_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"void_deref_bad", AST_SEMA_DIAG_INVALID_UNARY_OPERAND, 0},
    {"void_index_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0},
    {"sizeof_void_bad", AST_SEMA_DIAG_TYPE_MISMATCH, 0}};

static int contains_diag(const AstSema *sema, AstSemaDiagCode code) {
  size_t count = 0u;
  const AstSemaDiagnostic *diags = ast_sema_diagnostics(sema, &count);
  for (size_t i = 0u; i < count; ++i) {
    if (diags[i].code == code && diags[i].severity == AST_DIAG_ERROR)
      return 1;
  }
  return 0;
}

static int diagnostics_equal(const AstSema *a, const AstSema *b) {
  size_t ac = 0u;
  size_t bc = 0u;
  const AstSemaDiagnostic *ad = ast_sema_diagnostics(a, &ac);
  const AstSemaDiagnostic *bd = ast_sema_diagnostics(b, &bc);
  if (ac != bc)
    return 0;
  for (size_t i = 0u; i < ac; ++i) {
    if (ad[i].code != bd[i].code || ad[i].severity != bd[i].severity ||
        ad[i].span.file_id != bd[i].span.file_id ||
        ad[i].span.offset != bd[i].span.offset ||
        ad[i].span.length != bd[i].span.length || ad[i].line != bd[i].line ||
        ad[i].column != bd[i].column)
      return 0;
    const char *am = ad[i].message ? ad[i].message : "";
    const char *bm = bd[i].message ? bd[i].message : "";
    if (strcmp(am, bm) != 0)
      return 0;
  }
  return 1;
}

static int diagnostics_are_well_formed(const AstSema *sema) {
  size_t count = 0u;
  const AstSemaDiagnostic *diags = ast_sema_diagnostics(sema, &count);
  for (size_t i = 0u; i < count; ++i) {
    if (diags[i].code == AST_SEMA_DIAG_INTERNAL ||
        diags[i].code == AST_SEMA_DIAG_TOO_MANY_DIAGNOSTICS)
      return 0;
    for (size_t j = 0u; j < i; ++j) {
      if (diags[i].code != diags[j].code ||
          diags[i].severity != diags[j].severity)
        continue;
      if (diags[i].span.file_id != diags[j].span.file_id ||
          diags[i].span.offset != diags[j].span.offset ||
          diags[i].span.length != diags[j].span.length)
        continue;
      const char *a = diags[i].message ? diags[i].message : "";
      const char *b = diags[j].message ? diags[j].message : "";
      if (strcmp(a, b) == 0)
        return 0;
    }
  }
  return 1;
}

static int run_case(size_t index) {
  if (index >= sizeof(cases) / sizeof(cases[0]))
    return 2;
  const SemaCase *tc = &cases[index];
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/sema_cases/%s",
                    ASTERASM_SOURCE_DIR, tc->name);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 2;

  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 4u;
  AstFrontend *frontend = ast_frontend_create(&fc);
  if (!frontend)
    return 2;
  if (ast_frontend_add_import_root(frontend, root) != 0) {
    ast_frontend_destroy(frontend);
    return 2;
  }
  const char *entries[] = {entry};
  int frc = ast_frontend_load(frontend, entries, 1u);
  if (frc != 0) {
    fprintf(stderr, "frontend failed for sema case %s\n", tc->name);
    ast_print_frontend_diagnostics(stderr, frontend, NULL);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstSemaConfig sc = ast_sema_config_default();
  if (strcmp(tc->name, "sema_recursion_limit_bad") == 0)
    sc.recursion_limit = 6u;
  sc.thread_count = 1u;
  AstSema *serial = ast_sema_create(frontend, &sc);
  if (!serial) {
    ast_frontend_destroy(frontend);
    return 2;
  }
  int serial_rc = ast_sema_check(serial);
  int ok = 1;
  if (tc->expect_success) {
    if (serial_rc != 0 || ast_sema_stats(serial).error_count != 0u)
      ok = 0;
  } else {
    if (serial_rc != 1 || !contains_diag(serial, tc->expected))
      ok = 0;
  }
  if (!diagnostics_are_well_formed(serial))
    ok = 0;

  AstSemaConfig pc = sc;
  pc.thread_count = 5u;
  AstSema *parallel = ast_sema_create(frontend, &pc);
  if (!parallel) {
    ast_sema_destroy(serial);
    ast_frontend_destroy(frontend);
    return 2;
  }
  int parallel_rc = ast_sema_check(parallel);
  AstSemaStats serial_stats = ast_sema_stats(serial);
  AstSemaStats parallel_stats = ast_sema_stats(parallel);
  if (parallel_rc != serial_rc || !diagnostics_equal(serial, parallel) ||
      serial_stats.expression_count != parallel_stats.expression_count ||
      serial_stats.statement_count != parallel_stats.statement_count ||
      serial_stats.constant_expression_count !=
          parallel_stats.constant_expression_count) {
    ok = 0;
  }
  if (!diagnostics_are_well_formed(parallel))
    ok = 0;

  if (!ok) {
    fprintf(stderr,
            "sema case '%s' failed: serial_rc=%d parallel_rc=%d expected=%s\n",
            tc->name, serial_rc, parallel_rc,
            ast_sema_diag_code_name(tc->expected));
    fputs("serial diagnostics:\n", stderr);
    ast_print_sema_diagnostics(stderr, serial, NULL);
    fputs("parallel diagnostics:\n", stderr);
    ast_print_sema_diagnostics(stderr, parallel, NULL);
    fprintf(stderr,
            "semantic counters: serial expr=%zu stmt=%zu const=%zu; parallel "
            "expr=%zu stmt=%zu const=%zu\n",
            serial_stats.expression_count, serial_stats.statement_count,
            serial_stats.constant_expression_count,
            parallel_stats.expression_count, parallel_stats.statement_count,
            parallel_stats.constant_expression_count);
  }
  ast_sema_destroy(parallel);
  ast_sema_destroy(serial);
  ast_frontend_destroy(frontend);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2 && argc != 4) {
    fprintf(stderr, "usage: %s <case-index> [--repeat 1..1000]\n", argv[0]);
    return 2;
  }
  char *end = NULL;
  unsigned long index = strtoul(argv[1], &end, 10);
  if (!end || *end != '\0')
    return 2;
  unsigned long repeats = 1u;
  if (argc == 4) {
    if (strcmp(argv[2], "--repeat") != 0)
      return 2;
    repeats = strtoul(argv[3], &end, 10);
    if (!end || *end != '\0' || repeats < 1u || repeats > 1000u)
      return 2;
  }
  for (unsigned long i = 0u; i < repeats; ++i) {
    int rc = run_case((size_t)index);
    if (rc)
      return rc;
  }
  return 0;
}
