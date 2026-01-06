#ifndef SF_COMPILER_INTERNAL_H
#define SF_COMPILER_INTERNAL_H

#include <sionflow/compiler/sf_compiler.h>
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_json.h>

// Utilities
void sf_ir_parse_window_settings(const sf_json_value* root, sf_graph_ir* out_ir);
sf_ir_node* find_input_source(sf_graph_ir* ir, u32 dst_node_idx, u32 dst_port);
sf_ir_node* sf_ir_find_input_by_name(sf_graph_ir* ir, u32 dst_node_idx, const char* port_name);

sf_ir_node** sf_topo_sort(sf_graph_ir* ir, sf_arena* arena, size_t* out_count);

// --- Internal: CodeGen ---
// Emits instructions into the program
bool sf_codegen_emit(sf_program* prog, sf_graph_ir* ir, sf_ir_node** sorted_nodes, size_t sorted_count, sf_arena* arena);

#endif // SF_COMPILER_INTERNAL_H