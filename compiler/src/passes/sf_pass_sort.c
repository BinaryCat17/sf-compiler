#include "../sf_passes.h"
#include "../sf_graph_utils.h"
#include <sionflow/base/sf_memory.h>

/**
 * Topological Sort Pass
 * Orders nodes so that all inputs for a node appear before the node itself.
 * This is essential for shape analysis and code generation.
 */
bool sf_pass_sort(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;

    u32* order = SF_ARENA_PUSH(arena, u32, ir->node_count);
    if (!sf_ir_graph_sort(ir, order, arena, diag)) {
        return false;
    }

    // Convert indices to pointers for the compiler context
    ctx->sorted_nodes = SF_ARENA_PUSH(arena, sf_ir_node*, ir->node_count);
    ctx->sorted_count = 0;

    for (u32 i = 0; i < ir->node_count; ++i) {
        u32 idx = order[i];
        if (ir->nodes[idx].type != SF_NODE_UNKNOWN) {
            ctx->sorted_nodes[ctx->sorted_count++] = &ir->nodes[idx];
        }
    }

    return true;
}
