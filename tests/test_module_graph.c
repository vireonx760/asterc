#include "asterasm/module.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #x);  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  AstImportEdge dag_edges[] = {{1u, 2u, {0}}, {1u, 3u, {0}}, {2u, 3u, {0}}};
  AstModuleGraph dag = {3u, dag_edges, 3u};
  uint32_t order[3] = {0};
  CHECK(ast_module_graph_topological_sort(&dag, order, 3u) == 0);
  CHECK(order[0] == 3u);
  CHECK(ast_module_graph_dependency_count(&dag, 1u) == 2u);

  AstImportEdge cycle_edges[] = {{1u, 2u, {0}}, {2u, 1u, {0}}};
  AstModuleGraph cycle = {2u, cycle_edges, 2u};
  CHECK(ast_module_graph_topological_sort(&cycle, order, 3u) == 1);

  const uint32_t deep_nodes = 100000u;
  AstImportEdge *deep_edges =
      (AstImportEdge *)calloc((size_t)deep_nodes - 1u, sizeof(*deep_edges));
  uint32_t *deep_order = (uint32_t *)calloc(deep_nodes, sizeof(*deep_order));
  CHECK(deep_edges != NULL && deep_order != NULL);
  for (uint32_t i = 1u; i < deep_nodes; ++i) {
    deep_edges[i - 1u].from_file_id = i;
    deep_edges[i - 1u].to_file_id = i + 1u;
  }
  AstModuleGraph deep = {deep_nodes, deep_edges, (size_t)deep_nodes - 1u};
  CHECK(ast_module_graph_topological_sort(&deep, deep_order, deep_nodes) == 0);
  CHECK(deep_order[0] == deep_nodes);
  CHECK(deep_order[deep_nodes - 1u] == 1u);
  free(deep_order);
  free(deep_edges);
  puts("module graph tests: OK");
  return 0;
}
