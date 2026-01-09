#ifndef SF_COMPILER_INTERNAL_H
#define SF_COMPILER_INTERNAL_H

#include <sionflow/compiler/sf_compiler.h>
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_json.h>

// Utilities
sf_ir_node* find_input_source(sf_graph_ir* ir, u32 dst_node_idx, u32 dst_port);
sf_ir_node* sf_ir_find_input_by_name(sf_graph_ir* ir, u32 dst_node_idx, const char* port_name);
sf_node_type sf_compiler_get_node_type(const char* type_str);
u32 sf_compiler_get_port_index(sf_node_type type, const char* port_name);

// --- Internal: CodeGen ---
// Emits instructions into the program
typedef struct sf_pass_ctx sf_pass_ctx;
bool sf_codegen_emit(sf_program* prog, sf_pass_ctx* ctx, sf_arena* arena);

#endif // SF_COMPILER_INTERNAL_H