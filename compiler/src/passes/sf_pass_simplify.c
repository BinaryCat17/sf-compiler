#include "../sf_passes.h"
#include "../sf_graph_utils.h"
#include <sionflow/base/sf_log.h>
#include <string.h>

/**
 * Pass: Simplify Graph
 * Short-circuits bridge nodes like RESHAPE and SLICE by connecting consumers directly to producers.
 * This ensures that the engine doesn't have to execute "identity" operations.
 */

static u32 trace_real_source(sf_graph_ir* ir, u32 node_idx, u32 port_idx, u32* out_port) {
    sf_ir_node* node = &ir->nodes[node_idx];
    
    // Identity nodes (Zero-Copy) just forward data from input 0
    if (node->type == SF_NODE_RESHAPE || node->type == SF_NODE_SLICE) {
        sf_port src = sf_builder_get_source(ir, (sf_port){ node_idx, 0 });
        if (!SF_PORT_IS_NULL(src)) {
            return trace_real_source(ir, src.node_idx, src.port_idx, out_port);
        }
    }

    *out_port = port_idx;
    return node_idx;
}

bool sf_pass_simplify(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    (void)diag;
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;

    // 1. Short-circuit connections
    for (u32 i = 0; i < ir->node_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;

        for (int p = 0; p < 4; ++p) {
            sf_port src = sf_builder_get_source(ir, (sf_port){ i, p });
            if (SF_PORT_IS_NULL(src)) continue;

            u32 real_port = src.port_idx;
            u32 real_src = trace_real_source(ir, src.node_idx, src.port_idx, &real_port);

            if (real_src != src.node_idx) {
                sf_builder_connect(ir, arena, (sf_port){ real_src, real_port }, (sf_port){ i, p });
            }
        }
    }

    // 2. Metadata cleanup: Identity nodes are no longer needed for computation
    for (u32 i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].type == SF_NODE_RESHAPE || ir->nodes[i].type == SF_NODE_SLICE) {
            // We keep the nodes but they will be skipped by Task Planner 
            // because they are now "floating" (disconnected from consumers)
            // or we can mark them unknown if they have no other side effects.
        }
    }

    return true;
}
