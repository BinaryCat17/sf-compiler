#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <string.h>

// Ultimate source tracing: skips through ANY node that is zero-copy/metadata
static u32 trace_real_source(sf_graph_ir* ir, u32 node_idx, const char* port_name, const char** out_port) {
    sf_ir_node* node = &ir->nodes[node_idx];
    const sf_op_metadata* meta = &SF_OP_METADATA[node->type];

    // Computational nodes (Real Sources)
    bool is_compute = (meta->category == SF_OP_CAT_ATOMIC || 
                       meta->category == SF_OP_CAT_REDUCTION || 
                       meta->category == SF_OP_CAT_ACCEL || 
                       (meta->category == SF_OP_CAT_MEMORY && node->type != SF_NODE_SLICE && node->type != SF_NODE_RESHAPE));

    if (is_compute) {
        if (out_port) *out_port = port_name;
        return node_idx;
    }

    // Bridge nodes (INPUT, SLICE, RESHAPE, etc.) -> Trace back to their producer
    for (u32 i = 0; i < ir->link_count; ++i) {
        sf_ir_link* l = &ir->links[i];
        if (l->dst_node_idx == node_idx) {
            return trace_real_source(ir, l->src_node_idx, l->src_port_name, out_port);
        }
    }
    return node_idx;
}

bool sf_pass_simplify(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    if (!ir) return false;

    // 1. Redirect all links to bypass all metadata/bridge nodes
    for (u32 i = 0; i < ir->link_count; ++i) {
        sf_ir_link* l = &ir->links[i];
        const char* real_port = l->src_port_name;
        u32 real_src = trace_real_source(ir, l->src_node_idx, l->src_port_name, &real_port);
        
        if (real_src != l->src_node_idx) {
            l->src_node_idx = real_src;
            l->src_port_name = (char*)real_port;
        }
    }

    // 2. Redirect domain indices
    for (u32 i = 0; i < ir->node_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->domain_node_idx != UINT32_MAX) {
            const char* dummy;
            node->domain_node_idx = trace_real_source(ir, node->domain_node_idx, "out", &dummy);
        }
    }

    // 3. Cleanup: Mark bridge nodes as UNKNOWN so they are skipped by everything
    for (u32 i = 0; i < ir->node_count; ++i) {
        sf_node_type type = ir->nodes[i].type;
        const sf_op_metadata* meta = &SF_OP_METADATA[type];
        
        bool is_compute = (meta->category == SF_OP_CAT_ATOMIC || 
                           meta->category == SF_OP_CAT_REDUCTION || 
                           meta->category == SF_OP_CAT_ACCEL || 
                           (meta->category == SF_OP_CAT_MEMORY && type != SF_NODE_SLICE && type != SF_NODE_RESHAPE));

        if (!is_compute && type != SF_NODE_CONST && type != SF_NODE_INPUT && type != SF_NODE_OUTPUT) {
            ir->nodes[i].type = SF_NODE_UNKNOWN;
        }
    }

    return true;
}