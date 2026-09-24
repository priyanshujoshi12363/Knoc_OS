#ifndef MEMGRAPH_H
#define MEMGRAPH_H

#include <stdint.h>
#include "syscall_abi.h"

int memgraph_init(uint32_t boot);
int memgraph_ready(void);
int memgraph_record(uint32_t kind_a, const char *a, uint32_t relation,
                    uint32_t kind_b, const char *b, const char *actor, uint32_t confidence);
int memgraph_stats(graph_stats_t *stats);
int memgraph_recent(uint32_t index, graph_edge_info_t *info);
int memgraph_edges(uint32_t kind, const char *name, uint32_t index, graph_edge_info_t *info);
int memgraph_find(const char *text, uint32_t index, graph_node_info_t *info);
int memgraph_forget(uint32_t kind, const char *name);

#endif
