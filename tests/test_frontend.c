#include "asterasm/asterasm.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x);  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  const char *root = ASTERASM_SOURCE_DIR "/examples/modules";
  const char *entry = ASTERASM_SOURCE_DIR "/examples/modules/app.aa";
  AstFrontendConfig cfg = ast_frontend_config_default();
  cfg.thread_count = 8u;
  AstFrontend *f = ast_frontend_create(&cfg);
  CHECK(f != NULL);
  CHECK(ast_frontend_add_import_root(f, root) == 0);
  const char *entries[] = {entry};
  int rc = ast_frontend_load(f, entries, 1u);
  if (rc != 0)
    ast_print_frontend_diagnostics(stderr, f, NULL);
  CHECK(rc == 0);
  AstFrontendStats stats = ast_frontend_stats(f);
  CHECK(stats.file_count == 3u);
  CHECK(stats.module_count == 3u);
  CHECK(stats.import_edge_count == 3u);
  CHECK(stats.lex_error_count == 0u);
  CHECK(stats.parse_error_count == 0u);
  CHECK(stats.frontend_error_count == 0u);
  AstFrontendFile math;
  CHECK(ast_frontend_find_module(f, "math.fast", &math));
  CHECK(math.parsed && math.parsed->module);
  CHECK(ast_name_equal_cstr(math.parsed->module->name, "math.fast"));
  size_t edge_count = 0u;
  const AstImportEdge *edges = ast_frontend_import_edges(f, &edge_count);
  CHECK(edges != NULL && edge_count == 3u);
  uint32_t order[3];
  AstModuleGraph graph = {3u, edges, edge_count};
  CHECK(ast_module_graph_topological_sort(&graph, order, 3u) == 0);
  ast_frontend_destroy(f);

  AstFrontendConfig parse_only_cfg = ast_frontend_config_default();
  parse_only_cfg.flags = 0u;
  AstFrontend *parse_only = ast_frontend_create(&parse_only_cfg);
  CHECK(parse_only != NULL);
  const char *parse_only_entries[] = {entry};
  CHECK(ast_frontend_load(parse_only, parse_only_entries, 1u) == 0);
  AstFrontendStats parse_only_stats = ast_frontend_stats(parse_only);
  CHECK(parse_only_stats.file_count == 1u);
  CHECK(parse_only_stats.import_edge_count == 0u);
  CHECK(parse_only_stats.frontend_error_count == 0u);
  ast_frontend_destroy(parse_only);

  const char *cycle_root = ASTERASM_SOURCE_DIR "/examples/cycles";
  const char *cycle_entry = ASTERASM_SOURCE_DIR "/examples/cycles/cycle/a.aa";
  AstFrontend *cycle_frontend = ast_frontend_create(NULL);
  CHECK(cycle_frontend != NULL);
  CHECK(ast_frontend_add_import_root(cycle_frontend, cycle_root) == 0);
  const char *cycle_entries[] = {cycle_entry};
  CHECK(ast_frontend_load(cycle_frontend, cycle_entries, 1u) == 1);
  AstFrontendStats cycle_stats = ast_frontend_stats(cycle_frontend);
  CHECK(cycle_stats.file_count == 2u);
  CHECK(cycle_stats.frontend_error_count >= 1u);
  size_t cycle_diag_count = 0u;
  const AstFrontendDiagnostic *cycle_diags =
      ast_frontend_diagnostics(cycle_frontend, &cycle_diag_count);
  int saw_cycle = 0;
  for (size_t i = 0u; i < cycle_diag_count; ++i) {
    if (cycle_diags[i].code == AST_FRONTEND_DIAG_IMPORT_CYCLE)
      saw_cycle = 1;
  }
  CHECK(saw_cycle);
  ast_frontend_destroy(cycle_frontend);

  puts("frontend tests: OK");
  return 0;
}
