#include "../sf_passes.h"
#include "../sf_graph_utils.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <string.h>

/**
 * Inlining Pass
 * Recursively replaces CALL nodes with the actual contents of the referenced subgraphs.
 * Uses the professional Smart Graph API for all manipulations.
 */

// Function defined in sf_json_parser.c
bool sf_compile_load_json_ir(const char* json_path, sf_graph_ir* out_ir, sf_arena* arena, sf_compiler_diag* diag, const char* base_path);

bool sf_pass_inline_wrapper(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;
    bool changed = true;

    // We keep inlining as long as we find CALL nodes
    while (changed) {
        changed = false;
        u32 initial_count = (u32)ir->node_count;

        for (u32 i = 0; i < initial_count; ++i) {
            sf_ir_node* node = &ir->nodes[i];
            if (node->type != SF_NODE_CALL || !node->sub_graph_path) continue;

            SF_LOG_DEBUG("Inlining subgraph: %s", node->sub_graph_path);

            // 1. Load Subgraph IR
            sf_graph_ir subgraph = {0};
            if (!sf_compile_load_json_ir(node->sub_graph_path, &subgraph, arena, diag, ctx->base_path)) {
                SF_LOG_ERROR("Failed to load subgraph for inlining: %s", node->sub_graph_path);
                return false;
            }

            // 2. Perform Professional Inline
            if (!sf_ir_node_inline(ir, i, &subgraph, node->id, arena)) {
                SF_REPORT_NODE(diag, node, "Inlining Error: Graph capacity exceeded");
                return false;
            }

            changed = true;
            break; // Restart loop because node indices and counts have changed
        }
    }

    return true;
}