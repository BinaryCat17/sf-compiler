#include "../sf_passes.h"
#include "../sf_graph_utils.h"

/**
 * Decomposition Pass (Lowering)
 * Replaces complex nodes (like MEAN) with simpler atomic subgraphs (SUM + DIV).
 * The rules are defined in compiler_spec.json and generated as SF_LOWERING_RULES.
 */
bool sf_pass_decompose(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;
    
    // We iterate over the initial nodes. 
    // New nodes added during decomposition won't be visited in this pass.
    u32 initial_count = (u32)ir->node_count;

    for (u32 i = 0; i < initial_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;

        // Find a matching lowering rule
        for (size_t r = 0; r < SF_LOWERING_RULE_COUNT; ++r) {
            if (SF_LOWERING_RULES[r].target_type == node->type) {
                if (!sf_ir_node_replace_with_subgraph(ir, i, &SF_LOWERING_RULES[r], arena, diag)) {
                    return false;
                }
                break;
            }
        }
    }

    return true;
}
