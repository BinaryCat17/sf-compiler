#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <stdlib.h>
#include <string.h>

static u32 trace_register_source(sf_graph_ir* ir, u32 node_idx, int depth) {
    if (depth > 32) return node_idx; // Safety break

    sf_ir_node* node = &ir->nodes[node_idx];
    bool is_bridge = (node->type == SF_NODE_INPUT || 
                      node->type == SF_NODE_OUTPUT || 
                      node->type == SF_NODE_RESHAPE || 
                      node->type == SF_NODE_SLICE);

    if (!is_bridge) return node_idx;

    // Trace back to producer using O(1) connectivity
    u32 src_idx = node->inputs[0].src_node_idx;
    if (src_idx != UINT32_MAX) {
        return trace_register_source(ir, src_idx, depth + 1);
    }
    
    return node_idx;
}

bool sf_pass_liveness(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_ir_node** sorted = ctx->sorted_nodes;
    size_t count = ctx->sorted_count;

    if (!ir || !sorted) {
        SF_REPORT(diag, NULL, "Liveness Pass: Internal Error - IR or sorted nodes is NULL");
        return false;
    }

    // 1. Initial pass: Assign unique registers to all computation nodes and constants
    u16 next_reg = 0;
    for (size_t i = 0; i < ir->node_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        
        bool is_compute = (meta->category != SF_OP_CAT_SPECIAL && node->type != SF_NODE_RESHAPE && node->type != SF_NODE_SLICE);
        bool is_const = (node->type == SF_NODE_CONST);

        if (is_compute || is_const) {
            node->out_reg_idx = next_reg++;
        } else {
            node->out_reg_idx = 0xFFFF; // Unassigned
        }
    }

    // 2. Resolve Bridge Aliasing (Recursive)
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].out_reg_idx == 0xFFFF) {
            u32 real_src = trace_register_source(ir, (u32)i, 0);
            ir->nodes[i].out_reg_idx = ir->nodes[real_src].out_reg_idx;
            
            // Fallback if still unassigned (should not happen in valid graph)
            if (ir->nodes[i].out_reg_idx == 0xFFFF) {
                ir->nodes[i].out_reg_idx = next_reg++;
            }
        }
    }

    // 3. (Optional) Register reuse algorithm for temporary buffers
    // For now, we keep it simple: every compute node has its own register 
    // unless it was aliased. Optimization of 'next_reg' can be added back later
    // once we verify stability.

    SF_LOG_INFO("Liveness: Allocated %u registers for %zu compute nodes", next_reg, count);

    return true;
}