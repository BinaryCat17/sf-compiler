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

static bool can_broadcast_to(const sf_type_info* src, const sf_type_info* dst) {
    if (src->ndim == 0) return true; // Scalar always broadcasts
    if (src->ndim > dst->ndim) return false;
    
    for (int i = 0; i < src->ndim; ++i) {
        int32_t s = src->shape[src->ndim - 1 - i];
        int32_t d = dst->shape[dst->ndim - 1 - i];
        if (s != d && s != 1) return false;
    }
    return true;
}

static void mark_domain(sf_graph_ir* ir, u32 node_idx, u32 domain_idx) {
    if (node_idx == UINT32_MAX || ir->nodes[node_idx].domain_node_idx != UINT32_MAX) return;

    ir->nodes[node_idx].domain_node_idx = domain_idx;

    // Recurse to inputs using O(1) connectivity
    sf_ir_node* node = &ir->nodes[node_idx];
    for (int p = 0; p < 4; ++p) {
        u32 src_idx = node->inputs[p].src_node_idx;
        if (src_idx != UINT32_MAX) {
            mark_domain(ir, src_idx, domain_idx);
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

    // 2. Find all potential domain representatives (outputs or nodes with unique shapes)
    // and propagate their domain backwards.
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].type == SF_NODE_OUTPUT || ir->nodes[i].domain_node_idx == UINT32_MAX) {
            u32 rep_idx = (u32)i;
            // Try to find an existing representative with the same shape
            for (size_t j = 0; j < ir->node_count; ++j) {
                if (ir->nodes[j].domain_node_idx == (u32)j && 
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
