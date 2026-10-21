#include "asterasm/asterasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ASTERASM_SOURCE_DIR
#define ASTERASM_SOURCE_DIR "."
#endif

typedef struct TextBuffer {
  char *data;
  size_t length;
  size_t capacity;
  int failed;
} TextBuffer;

static void write_buffer(void *user, const char *text, size_t length) {
  TextBuffer *buffer = (TextBuffer *)user;
  if (!buffer || !text || length == 0u || buffer->failed)
    return;
  if (buffer->length > SIZE_MAX - length - 1u) {
    buffer->failed = 1;
    return;
  }
  size_t needed = buffer->length + length + 1u;
  if (needed > buffer->capacity) {
    size_t next = buffer->capacity ? buffer->capacity : 4096u;
    while (next < needed) {
      if (next > SIZE_MAX / 2u) {
        next = needed;
        break;
      }
      next *= 2u;
    }
    char *grown = (char *)realloc(buffer->data, next);
    if (!grown) {
      buffer->failed = 1;
      return;
    }
    buffer->data = grown;
    buffer->capacity = next;
  }
  memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

static int stats_equal(AstMirStats a, AstMirStats b) {
  return a.module_count == b.module_count &&
         a.function_count == b.function_count &&
         a.extern_function_count == b.extern_function_count &&
         a.dispatch_function_count == b.dispatch_function_count &&
         a.global_count == b.global_count && a.block_count == b.block_count &&
         a.instruction_count == b.instruction_count &&
         a.vreg_count == b.vreg_count &&
         a.memory_value_count == b.memory_value_count &&
         a.stack_slot_count == b.stack_slot_count &&
         a.promoted_local_count == b.promoted_local_count &&
         a.phi_count == b.phi_count &&
         a.memory_phi_count == b.memory_phi_count &&
         a.terminator_count == b.terminator_count &&
         a.unreachable_block_count == b.unreachable_block_count &&
         a.error_count == b.error_count && a.warning_count == b.warning_count;
}

static void print_mir_diagnostics(const AstMirProgram *mir) {
  size_t count = 0u;
  const AstMirDiagnostic *diagnostics = ast_mir_diagnostics(mir, &count);
  for (size_t i = 0u; i < count; ++i) {
    fprintf(stderr, "%s:%u:%u: %s[%s]: %s\n",
            diagnostics[i].path ? diagnostics[i].path : "<source>",
            diagnostics[i].line, diagnostics[i].column,
            diagnostics[i].severity == AST_DIAG_ERROR ? "error" : "warning",
            ast_mir_diag_code_name(diagnostics[i].code),
            diagnostics[i].message ? diagnostics[i].message : "");
  }
}

static int validate_public_graph(const AstMirProgram *mir) {
  for (size_t fi = 0u; fi < ast_mir_function_count(mir); ++fi) {
    const AstMirFunction *function = ast_mir_function_at(mir, fi);
    AstMirFunctionInfo info;
    if (!function || !ast_mir_function_info(function, &info) ||
        !ast_mir_function_source_hir(function))
      return 0;
    for (uint32_t bi = 0u; bi < info.block_count; ++bi) {
      AstMirBlockInfo block;
      if (!ast_mir_function_block(function, bi, &block) || block.id != bi ||
          block.terminator.kind == AST_MIR_TERM_NONE ||
          block.memory_argument >= info.memory_value_count)
        return 0;
      AstMirMemoryInfo memory;
      if (!ast_mir_function_memory(function, block.memory_argument, &memory) ||
          memory.block != bi ||
          memory.incoming_count !=
              (bi == info.entry_block ? 0u : block.predecessor_count))
        return 0;
      for (uint32_t si = 0u; si < block.successor_count; ++si) {
        AstMirBlockId successor = AST_MIR_INVALID_ID;
        if (!ast_mir_function_successor(function, bi, si, &successor) ||
            successor >= info.block_count)
          return 0;
      }
      for (uint32_t ii = 0u; ii < block.instruction_count; ++ii) {
        AstMirInstructionInfo instruction;
        if (!ast_mir_function_instruction(function, block.instructions[ii],
                                          &instruction) ||
            instruction.id != block.instructions[ii] ||
            instruction.op == AST_MIR_OP_INVALID)
          return 0;
      }
    }
  }
  return 1;
}

static int run_case(const char *name) {
  char root[4096];
  char entry[4096];
  int rn = snprintf(root, sizeof(root), "%s/tests/sema_cases/%s",
                    ASTERASM_SOURCE_DIR, name);
  int en = snprintf(entry, sizeof(entry), "%s/main.aa", root);
  if (rn <= 0 || en <= 0 || (size_t)rn >= sizeof(root) ||
      (size_t)en >= sizeof(entry))
    return 2;

  AstFrontendConfig frontend_config = ast_frontend_config_default();
  frontend_config.thread_count = 4u;
  AstFrontend *frontend = ast_frontend_create(&frontend_config);
  if (!frontend)
    return 2;
  if (ast_frontend_add_import_root(frontend, root) != 0) {
    ast_frontend_destroy(frontend);
    return 2;
  }
  const char *entries[] = {entry};
  if (ast_frontend_load(frontend, entries, 1u) != 0) {
    fprintf(stderr, "MIR corpus frontend failed for %s\n", name);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstSemaConfig sema_config = ast_sema_config_default();
  sema_config.thread_count = 5u;
  AstSema *sema = ast_sema_create(frontend, &sema_config);
  if (!sema || ast_sema_check(sema) != 0) {
    fprintf(stderr, "MIR corpus sema failed for %s\n", name);
    ast_sema_destroy(sema);
    ast_frontend_destroy(frontend);
    return 1;
  }
  AstHirConfig hir_config = ast_hir_config_default();
  hir_config.thread_count = 5u;
  AstHirProgram *hir = ast_hir_build(frontend, sema, &hir_config);
  if (!hir || !ast_hir_verify(hir) || ast_hir_stats(hir).error_count != 0u) {
    fprintf(stderr, "MIR corpus HIR failed for %s\n", name);
    ast_hir_destroy(hir);
    ast_sema_destroy(sema);
    ast_frontend_destroy(frontend);
    return 1;
  }

  AstMirConfig one = ast_mir_config_default();
  one.thread_count = 1u;
  AstMirProgram *serial = ast_mir_build(frontend, hir, &one);
  AstMirConfig many = one;
  many.thread_count = 5u;
  AstMirProgram *parallel = ast_mir_build(frontend, hir, &many);
  AstMirConfig materialized_config = one;
  materialized_config.promote_locals = 0u;
  AstMirProgram *materialized =
      ast_mir_build(frontend, hir, &materialized_config);
  int serial_valid = serial && ast_mir_verify(serial) &&
                     ast_mir_stats(serial).error_count == 0u;
  int parallel_valid = parallel && ast_mir_verify(parallel) &&
                       ast_mir_stats(parallel).error_count == 0u;
  int materialized_valid = materialized && ast_mir_verify(materialized) &&
                           ast_mir_stats(materialized).error_count == 0u;
  int equal_stats = serial && parallel &&
                    stats_equal(ast_mir_stats(serial), ast_mir_stats(parallel));
  int public_graph_valid = serial && materialized &&
                           validate_public_graph(serial) &&
                           validate_public_graph(materialized);
  /* Run the independent structural verifier across the entire MIR corpus,
   * in both SSA-promoted and materialized-local lowering modes. */
  int structural_valid = serial_valid && parallel_valid && materialized_valid &&
                         ast_mir_verify_structural(serial) &&
                         ast_mir_verify_structural(parallel) &&
                         ast_mir_verify_structural(materialized);
  int ok = serial_valid && parallel_valid && materialized_valid &&
           equal_stats && public_graph_valid && structural_valid;

  TextBuffer serial_dump = {0};
  TextBuffer parallel_dump = {0};
  if (serial)
    ast_mir_dump_program(serial, write_buffer, &serial_dump);
  if (parallel)
    ast_mir_dump_program(parallel, write_buffer, &parallel_dump);
  if (serial_dump.failed || parallel_dump.failed ||
      serial_dump.length != parallel_dump.length ||
      (serial_dump.length &&
       memcmp(serial_dump.data, parallel_dump.data, serial_dump.length) != 0))
    ok = 0;
  if (!ok) {
    fprintf(stderr,
            "MIR corpus validation failed for %s "
            "(serial=%d parallel=%d materialized=%d stats=%d api=%d "
            "structural=%d dumps=%d)\n",
            name, serial_valid, parallel_valid, materialized_valid, equal_stats,
            public_graph_valid, structural_valid,
            !serial_dump.failed && !parallel_dump.failed &&
                serial_dump.length == parallel_dump.length);
    if (serial)
      print_mir_diagnostics(serial);
    if (parallel)
      print_mir_diagnostics(parallel);
    if (materialized)
      print_mir_diagnostics(materialized);
  }
  free(serial_dump.data);
  free(parallel_dump.data);
  ast_mir_destroy(materialized);
  ast_mir_destroy(parallel);
  ast_mir_destroy(serial);
  ast_hir_destroy(hir);
  ast_sema_destroy(sema);
  ast_frontend_destroy(frontend);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <sema-positive-case-name>\n", argv[0]);
    return 2;
  }
  return run_case(argv[1]);
}
