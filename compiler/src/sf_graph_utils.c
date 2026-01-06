#include "sf_compiler_internal.h"
#include "sf_passes.h"
#include <sionflow/base/sf_json.h>
#include <string.h>

// sf_ir_parse_window_settings is now automatically generated in sf_manifest_gen.c

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
    ctx->sorted_nodes[ctx->count++] = &ctx->ir->nodes[node_idx];
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
