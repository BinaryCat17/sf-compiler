#include "../sf_passes.h"
#include "../sf_graph_utils.h"

/**
 * Op Fusion Pass
 * Fuses multiple operations into a single specialized opcode (e.g., MUL+ADD -> FMA).
 */
bool sf_pass_fuse(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;
    bool changed = true;

    // We keep fusing as long as changes are made
    while (changed) {
        changed = false;
        u32 initial_count = (u32)ir->node_count;

        for (u32 i = 0; i < initial_count; ++i) {
            sf_ir_node* node = &ir->nodes[i];
            if (node->type == SF_NODE_UNKNOWN) continue;

            for (size_t r = 0; r < SF_FUSION_RULE_COUNT; ++r) {
                if (sf_ir_node_try_fuse(ir, i, &SF_FUSION_RULES[r], arena)) {
                    changed = true;
                    break;
                }
            }
            if (changed) break;
        }
    }

    return true;
}
