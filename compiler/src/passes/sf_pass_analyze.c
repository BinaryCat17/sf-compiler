#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <sionflow/isa/sf_opcodes.h>
#include <sionflow/isa/sf_op_defs.h>
#include <stdio.h>
#include <string.h>

typedef bool (*sf_shape_resolver)(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag);

static bool check_broadcast(sf_ir_node* node, const sf_type_info* a, const sf_type_info* b, sf_type_info* out, sf_compiler_diag* diag) {
    if (sf_shape_broadcast(a, b, out)) return true;
    char s_a[64], s_b[64];
    sf_shape_format(a, s_a, sizeof(s_a));
    sf_shape_format(b, s_b, sizeof(s_b));
    SF_REPORT_NODE(diag, node, "Incompatible shapes for broadcast: %s vs %s", s_a, s_b);
    return false;
}

// --- Shape Resolvers ---

static bool resolve_same_as_s1(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag) {
    if (!inputs[0]) return false;
    node->out_info.ndim = inputs[0]->out_info.ndim;
    memcpy(node->out_info.shape, inputs[0]->out_info.shape, sizeof(int32_t) * SF_MAX_DIMS);
    return true;
}

static bool resolve_same_as_s2(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag) {
    if (!inputs[1]) return false;
    node->out_info.ndim = inputs[1]->out_info.ndim;
    memcpy(node->out_info.shape, inputs[1]->out_info.shape, sizeof(int32_t) * SF_MAX_DIMS);
    return true;
}

static bool resolve_broadcast(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag) {
    if (!inputs[0] || !inputs[1]) return false;
    if (inputs[2]) {
        sf_type_info tmp;
        if (!check_broadcast(node, &inputs[0]->out_info, &inputs[1]->out_info, &tmp, diag)) return false;
        return check_broadcast(node, &tmp, &inputs[2]->out_info, &node->out_info, diag);
    }
    return check_broadcast(node, &inputs[0]->out_info, &inputs[1]->out_info, &node->out_info, diag);
}

static bool resolve_matmul(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag) {
    if (!inputs[0] || !inputs[1]) return false;
    node->out_info.ndim = 2;
    node->out_info.shape[0] = inputs[0]->out_info.shape[inputs[0]->out_info.ndim - 2];
    node->out_info.shape[1] = inputs[1]->out_info.shape[inputs[1]->out_info.ndim - 1];
    return true;
}

static bool resolve_dot(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag) {
    if (!inputs[0]) return false;
    node->out_info.ndim = inputs[0]->out_info.ndim > 0 ? inputs[0]->out_info.ndim - 1 : 0;
    for (int k = 0; k < node->out_info.ndim; ++k) node->out_info.shape[k] = inputs[0]->out_info.shape[k];
    return true;
}

static bool resolve_scalar(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag) {
    node->out_info.ndim = 0;
    node->out_info.shape[0] = 1;
    return true;
}

static const sf_shape_resolver SHAPE_RESOLVERS[SF_SHAPE_COUNT] = {
    [SF_SHAPE_SAME_AS_S1] = resolve_same_as_s1,
    [SF_SHAPE_SAME_AS_S2] = resolve_same_as_s2,
    [SF_SHAPE_BROADCAST]  = resolve_broadcast,
    [SF_SHAPE_MATMUL]     = resolve_matmul,
    [SF_SHAPE_DOT]        = resolve_dot,
    [SF_SHAPE_SCALAR]     = resolve_scalar
};

bool sf_pass_analyze(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_ir_node** sorted_nodes = ctx->sorted_nodes;
    size_t count = ctx->sorted_count;

    for (size_t i = 0; i < count; ++i) {
        sf_ir_node* node = sorted_nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;

        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        sf_ir_node* inputs[4] = {0};
        for (u8 k = 0; k < 4; ++k) {
            if (meta->ports[k]) inputs[k] = sf_ir_find_input_by_name(ir, (u32)(node - ir->nodes), meta->ports[k]);
        }

        // 1. Resolve Shape
        if (meta->shape_rule < SF_SHAPE_COUNT && SHAPE_RESOLVERS[meta->shape_rule]) {
            SHAPE_RESOLVERS[meta->shape_rule](node, inputs, diag);
        }

        // 2. Resolve DType
        static const sf_dtype RULE_TO_DTYPE[] = {
            [SF_OUT_FORCE_F32] = SF_DTYPE_F32,
            [SF_OUT_FORCE_U8]  = SF_DTYPE_U8,
            [SF_OUT_FORCE_I32] = SF_DTYPE_I32
        };

        if (meta->out_rule >= SF_OUT_FORCE_F32) {
            node->out_info.dtype = RULE_TO_DTYPE[meta->out_rule];
        } else if (meta->out_rule == SF_OUT_SAME_AS_INPUT && inputs[0]) {
            node->out_info.dtype = inputs[0]->out_info.dtype;
        } else if (meta->out_rule == SF_OUT_SAME_AS_INPUT_2 && inputs[1]) {
            node->out_info.dtype = inputs[1]->out_info.dtype;
        }

        if (node->out_info.dtype == SF_DTYPE_UNKNOWN) node->out_info.dtype = SF_DTYPE_F32;

        // 3. Domain & Spatial Analysis
        u32 dom_idx = node->domain_node_idx;
        if (dom_idx == UINT32_MAX) {
            // Find default domain (first output) if none assigned
            for (u32 j = 0; j < (u32)ir->node_count; ++j) {
                if (ir->nodes[j].type == SF_NODE_OUTPUT) { dom_idx = j; break; }
            }
        }
        
        size_t task_cnt = 1;
        if (dom_idx != UINT32_MAX) {
            task_cnt = sf_shape_calc_count(ir->nodes[dom_idx].out_info.shape, ir->nodes[dom_idx].out_info.ndim);
        }
        
        bool is_generator = (meta->flags & SF_OP_FLAG_GENERATOR);
        bool has_spatial_input = false;
        for (int k = 0; k < 4; ++k) if (inputs[k] && inputs[k]->is_spatial) has_spatial_input = true;
        
        node->is_spatial = (task_cnt > 1) || is_generator || has_spatial_input;

        if (is_generator && dom_idx != UINT32_MAX) {
            node->out_info = ir->nodes[dom_idx].out_info;
        }
        
        sf_shape_calc_strides(&node->out_info);
    }
    return true;
}