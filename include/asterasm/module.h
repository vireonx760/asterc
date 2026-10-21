#ifndef ASTERASM_MODULE_H
#define ASTERASM_MODULE_H

#include "asterasm/frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AstModuleGraph {
  uint32_t node_count;
  const AstImportEdge *edges;
  size_t edge_count;
} AstModuleGraph;

ASTER_API int ast_module_graph_topological_sort(const AstModuleGraph *graph,
                                                uint32_t *order_out,
                                                size_t order_capacity);
ASTER_API size_t ast_module_graph_dependency_count(const AstModuleGraph *graph,
                                                   uint32_t file_id);

#ifdef __cplusplus
}
#endif

#endif
