#include "sf_passes.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>

static bool shapes_equal(const sf_type_info* a, const sf_type_info* b) {
    if (a->ndim != b->ndim) return false;
    for (int i = 0; i < a->ndim; ++i) {
        if (a->shape[i] != b->shape[i]) return false;
    }
    return true;
}

static void mark_domain(sf_graph_ir* ir, u32 node_idx, u32 domain_idx) {
    sf_ir_node* node = &ir->nodes[node_idx];
    
    if (node->domain_node_idx != UINT32_MAX) {
        if (node->domain_node_idx != domain_idx) {
            // Node is used by multiple domains. 
            // In a pure STEP_N model, shared nodes must either:
            // 1. Be moved to a 'common' domain (calculated once).
            // 2. Be duplicated (bad for perf).
            // 3. Be constants/inputs (already fine).
            
            // For now, if shapes are different, we mark as shared (UINT32_MAX).
            // If shapes are same, we can stick to one domain as they are compatible.
            const sf_type_info* shape_a = &ir->nodes[node->domain_node_idx].out_info;
            const sf_type_info* shape_b = &ir->nodes[domain_idx].out_info;
            
            if (!shapes_equal(shape_a, shape_b)) {
                node->domain_node_idx = UINT32_MAX; 
            }
        }
        return;
    }

    node->domain_node_idx = domain_idx;

    // Recurse to inputs
    for (size_t i = 0; i < ir->link_count; ++i) {
        if (ir->links[i].dst_node_idx == node_idx) {
            mark_domain(ir, ir->links[i].src_node_idx, domain_idx);
        }
    }
}

bool sf_pass_domain_split(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    if (!ir) {
        SF_REPORT(diag, NULL, "Domain Split Pass: IR is NULL");
        return false;
    }

    // 1. Reset all domain indices
    for (size_t i = 0; i < ir->node_count; ++i) {
        ir->nodes[i].domain_node_idx = UINT32_MAX;
    }

    // 2. Find all Outputs and propagate their domain backwards.
    // We want to group by shape, so if multiple outputs have the same shape,
    // they can share the same 'domain representative'.
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].type == SF_NODE_OUTPUT) {
            // Check if we already have a domain for this shape
            u32 rep_idx = (u32)i;
            for (size_t j = 0; j < i; ++j) {
                if (ir->nodes[j].type == SF_NODE_OUTPUT && 
                    shapes_equal(&ir->nodes[j].out_info, &ir->nodes[i].out_info)) 
                {
                    rep_idx = (u32)j;
                    break;
                }
            }
            mark_domain(ir, (u32)i, rep_idx);
        }
    }

    return true;
}
