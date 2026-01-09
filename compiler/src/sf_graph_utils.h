#ifndef SF_GRAPH_UTILS_H
#define SF_GRAPH_UTILS_H

#include <sionflow/compiler/sf_compiler.h>

/**
 * Smart Graph API for SionFlow Compiler (Graph 2.0).
 * Provides O(1) traversal and professional-grade utilities for graph manipulation.
 */

// --- Port-Centric Abstraction ---

typedef struct {
    u32 node_idx;
    u32 port_idx;
} sf_port;

#define SF_PORT_NULL ((sf_port){UINT32_MAX, 0})
#define SF_PORT_IS_NULL(p) ((p).node_idx == UINT32_MAX)

// --- Graph Builder API ---

void sf_builder_connect(sf_graph_ir* ir, sf_arena* arena, sf_port src, sf_port dst);
void sf_builder_disconnect(sf_graph_ir* ir, sf_port dst);
sf_port sf_builder_get_source(sf_graph_ir* ir, sf_port dst);
void sf_builder_replace_node(sf_graph_ir* ir, u32 old_node_idx, u32 new_node_idx);
void sf_builder_remove_node(sf_graph_ir* ir, u32 node_idx);

// --- Basic Manipulations ---

sf_ir_node* sf_ir_node_add(sf_graph_ir* ir, sf_arena* arena, const char* id, sf_node_type type);
void sf_ir_node_remove(sf_graph_ir* ir, u32 node_idx);

u32 sf_compiler_get_port_index(sf_node_type type, const char* port_name);
sf_node_type sf_compiler_get_node_type(const char* type_str);

bool sf_ir_graph_sort(sf_graph_ir* ir, u32* out_order, sf_arena* arena, sf_compiler_diag* diag);

// --- Advanced Rewriting ---

void sf_ir_links_remap_node(sf_graph_ir* ir, u32 old_node_idx, u32 new_node_idx);
bool sf_ir_node_replace_with_subgraph(sf_graph_ir* ir, u32 node_idx, const sf_lowering_rule* rule, sf_arena* arena, sf_compiler_diag* diag);
bool sf_ir_node_try_fuse(sf_graph_ir* ir, u32 node_idx, const sf_fusion_rule* rule, sf_arena* arena);
bool sf_ir_node_inline(sf_graph_ir* ir, u32 call_node_idx, const sf_graph_ir* subgraph, const char* prefix, sf_arena* arena);
u32* sf_ir_graph_graft(sf_graph_ir* dst, const sf_graph_ir* src, const char* prefix, sf_arena* arena);

// --- Helpers ---

u32 sf_ir_find_node_by_id(const sf_graph_ir* ir, const char* id);
u32 find_node_by_reg(const sf_graph_ir* ir, u16 reg_idx);
const char* sf_ir_get_port_name(sf_node_type type, u32 port_idx);
sf_ir_node* find_input_source(sf_graph_ir* ir, u32 dst_node_idx, u32 dst_port);
sf_ir_node* sf_ir_find_input_by_name(sf_graph_ir* ir, u32 dst_node_idx, const char* port_name);

bool sf_ir_parse_data(const struct sf_json_value* val, sf_dtype dtype, size_t count, void* out_data);

#endif // SF_GRAPH_UTILS_H