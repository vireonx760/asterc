#include "asterasm/common.h"
#include "asterasm/module.h"

#include <stdlib.h>
#include <string.h>

size_t ast_module_graph_dependency_count(const AstModuleGraph *graph,
                                         uint32_t file_id) {
  if (!graph || file_id == 0u || file_id > graph->node_count)
    return 0u;
  size_t count = 0u;
  for (size_t i = 0; i < graph->edge_count; ++i) {
    if (graph->edges[i].from_file_id == file_id)
      count++;
  }
  return count;
}

int ast_module_graph_topological_sort(const AstModuleGraph *graph,
                                      uint32_t *order_out,
                                      size_t order_capacity) {
  if (!graph || (!order_out && graph->node_count != 0u) ||
      order_capacity < graph->node_count)
    return -1;
  if (graph->node_count == 0u)
    return 0;

  size_t node_count = graph->node_count;
  size_t edge_count = graph->edge_count;
  size_t offsets_count = node_count + 1u;
  size_t offsets_bytes;
  size_t nodes_bytes;
  size_t edges_bytes;
  if (ast_size_mul_overflow(offsets_count, sizeof(size_t), &offsets_bytes) ||
      ast_size_mul_overflow(node_count, sizeof(uint32_t), &nodes_bytes) ||
      ast_size_mul_overflow(edge_count, sizeof(uint32_t), &edges_bytes))
    return -1;

  uint32_t *dependency_count =
      (uint32_t *)calloc(node_count, sizeof(*dependency_count));
  uint32_t *queue = (uint32_t *)malloc(nodes_bytes);
  size_t *reverse_offsets =
      (size_t *)calloc(offsets_count, sizeof(*reverse_offsets));
  uint32_t *dependents = edge_count ? (uint32_t *)malloc(edges_bytes) : NULL;
  if (!dependency_count || !queue || !reverse_offsets ||
      (edge_count != 0u && !dependents)) {
    free(dependency_count);
    free(queue);
    free(reverse_offsets);
    free(dependents);
    return -1;
  }

  for (size_t i = 0; i < edge_count; ++i) {
    uint32_t from = graph->edges[i].from_file_id;
    uint32_t to = graph->edges[i].to_file_id;
    if (from == 0u || to == 0u || from > graph->node_count ||
        to > graph->node_count)
      continue;
    if (dependency_count[from - 1u] == UINT32_MAX) {
      free(dependency_count);
      free(queue);
      free(reverse_offsets);
      free(dependents);
      return -1;
    }
    dependency_count[from - 1u]++;
    reverse_offsets[to]++;
  }

  for (size_t i = 1u; i < offsets_count; ++i)
    reverse_offsets[i] += reverse_offsets[i - 1u];
  size_t *cursor = (size_t *)malloc(offsets_bytes);
  if (!cursor) {
    free(dependency_count);
    free(queue);
    free(reverse_offsets);
    free(dependents);
    return -1;
  }
  memcpy(cursor, reverse_offsets, offsets_bytes);
  for (size_t i = 0; i < edge_count; ++i) {
    uint32_t from = graph->edges[i].from_file_id;
    uint32_t to = graph->edges[i].to_file_id;
    if (from == 0u || to == 0u || from > graph->node_count ||
        to > graph->node_count)
      continue;
    dependents[cursor[to - 1u]++] = from;
  }
  free(cursor);

  size_t head = 0u;
  size_t tail = 0u;
  for (uint32_t i = 0u; i < graph->node_count; ++i) {
    if (dependency_count[i] == 0u)
      queue[tail++] = i + 1u;
  }

  size_t produced = 0u;
  while (head < tail) {
    uint32_t node = queue[head++];
    order_out[produced++] = node;
    size_t begin = reverse_offsets[node - 1u];
    size_t end = reverse_offsets[node];
    for (size_t i = begin; i < end; ++i) {
      uint32_t dependent_id = dependents[i];
      uint32_t *remaining = &dependency_count[dependent_id - 1u];
      if (*remaining != 0u) {
        (*remaining)--;
        if (*remaining == 0u)
          queue[tail++] = dependent_id;
      }
    }
  }

  free(dependency_count);
  free(queue);
  free(reverse_offsets);
  free(dependents);
  return produced == node_count ? 0 : 1;
}
