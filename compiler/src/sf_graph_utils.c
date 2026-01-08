#include "sf_compiler_internal.h"
#include "sf_passes.h"
#include <sionflow/base/sf_json.h>
#include <string.h>

// sf_ir_parse_window_settings is now automatically generated in sf_manifest_gen.c

sf_node_type sf_compiler_get_node_type(const char* type_str) {
    if (!type_str) return SF_NODE_UNKNOWN;
    
    // 1. Check dynamic aliases from compiler_spec.json
    for (size_t i = 0; i < SF_COMPILER_ALIAS_COUNT; ++i) {
        if (strcmp(type_str, SF_COMPILER_ALIASES[i].from) == 0) return SF_COMPILER_ALIASES[i].to;
    }

    // 2. Check built-in node names
    for (int i = 1; i < SF_NODE_COUNT; ++i) {
        if (strcmp(type_str, SF_OP_METADATA[i].name) == 0) return (sf_node_type)i;
    }
    return SF_NODE_UNKNOWN;
}

u32 sf_compiler_get_port_index(sf_node_type type, const char* port_name) {
    if (!port_name || type >= SF_NODE_COUNT) return 0;
    const sf_op_metadata* meta = &SF_OP_METADATA[type];
    for (u32 i = 0; i < 4; ++i) {
        if (meta->ports[i] && strcmp(meta->ports[i], port_name) == 0) return i;
    }
    return 0;
}

// --- Helper: Find Input Source ---
sf_ir_node* find_input_source(sf_graph_ir* ir, u32 dst_node_idx, u32 dst_port) {
    for (size_t i = 0; i < ir->link_count; ++i) {
        if (ir->links[i].dst_node_idx == dst_node_idx && ir->links[i].dst_port == dst_port) {
            return &ir->nodes[ir->links[i].src_node_idx];
        }
    }
    return NULL;
}

sf_ir_node* sf_ir_find_input_by_name(sf_graph_ir* ir, u32 dst_node_idx, const char* port_name) {
    if (!port_name) return NULL;
    for (size_t i = 0; i < ir->link_count; ++i) {
        if (ir->links[i].dst_node_idx == dst_node_idx) {
            if (ir->links[i].dst_port_name && strcmp(ir->links[i].dst_port_name, port_name) == 0) {
                return &ir->nodes[ir->links[i].src_node_idx];
            }
        }
    }
    return NULL;
}

// --- Topological Sort Helpers ---

typedef struct {
    sf_ir_node** sorted_nodes; 
    u8* visited;
    size_t count;
    sf_graph_ir* ir;
} sort_ctx;

static bool visit_node(sort_ctx* ctx, u32 node_idx) {
    if (ctx->visited[node_idx] == 2) return true;
    if (ctx->visited[node_idx] == 1) return false; 
    
    ctx->visited[node_idx] = 1;

    for (size_t i = 0; i < ctx->ir->link_count; ++i) {
        if (ctx->ir->links[i].dst_node_idx == node_idx) {
            if (!visit_node(ctx, ctx->ir->links[i].src_node_idx)) return false;
        }
    }

    ctx->visited[node_idx] = 2;
    
    // The Filter: only include nodes that generate actual bytecode instructions.
    // Everything else (INPUT, OUTPUT, CONST, COPY, CALL, UNKNOWN) is metadata.
    sf_ir_node* node = &ctx->ir->nodes[node_idx];
    const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
    sf_node_type type = node->type;
    
    bool is_compute = (meta->category == SF_OP_CAT_ATOMIC || 
                       meta->category == SF_OP_CAT_REDUCTION || 
                       meta->category == SF_OP_CAT_ACCEL || 
                       (meta->category == SF_OP_CAT_MEMORY && type != SF_NODE_SLICE && type != SF_NODE_RESHAPE));

    if (is_compute) {
        ctx->sorted_nodes[ctx->count++] = node;
    }
    return true;
}

bool sf_pass_sort(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;
    
    sf_ir_node** sorted = SF_ARENA_PUSH(arena, sf_ir_node*, ir->node_count);
    u8* visited = SF_ARENA_PUSH(arena, u8, ir->node_count);
    if (!sorted || !visited) return false;
    
    memset(visited, 0, ir->node_count);

    sort_ctx s_ctx = { .sorted_nodes = sorted, .visited = visited, .count = 0, .ir = ir };
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (visited[i] == 0) {
            if (!visit_node(&s_ctx, (u32)i)) {
                sf_source_loc loc = {0};
                sf_compiler_diag_report(diag, loc, "Cycle detected in graph or sorting failed.");
                return false;
            }
        }
    }
    
    ctx->sorted_nodes = sorted;
    ctx->sorted_count = s_ctx.count;
    return true;
}
