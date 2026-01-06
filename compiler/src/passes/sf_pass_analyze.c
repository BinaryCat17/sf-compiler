#include "../sf_passes.h"
#include <sionflow/isa/sf_builtins.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <sionflow/isa/sf_opcodes.h>
#include <sionflow/isa/sf_op_defs.h>
#include <stdio.h>
#include <string.h>

static bool check_broadcast(sf_ir_node* node, const sf_type_info* a, const sf_type_info* b, sf_type_info* out, sf_compiler_diag* diag) {
    if (sf_shape_broadcast(a, b, out)) return true;
    char s_a[64], s_b[64];
    sf_shape_format(a, s_a, sizeof(s_a));
    sf_shape_format(b, s_b, sizeof(s_b));
    SF_REPORT_NODE(diag, node, "Incompatible shapes for broadcast: %s vs %s", s_a, s_b);
    return false;
}

bool sf_pass_analyze(sf_graph_ir* ir, sf_ir_node** sorted_nodes, size_t count, sf_compiler_diag* diag) {
    // 0. Pre-pass: Port & Constant Initialization
    for (size_t i = 0; i < count; ++i) {
        sf_ir_node* node = sorted_nodes[i];
        
        if (node->type == SF_NODE_INPUT || node->type == SF_NODE_OUTPUT) {
            // Autonomous mode: use info already in node (from JSON or defaults)
            node->out_info = node->const_info;
            sf_shape_normalize(&node->out_info);
        }

        if (node->type == SF_NODE_CONST) {
            sf_shape_normalize(&node->const_info);
            node->out_info = node->const_info;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        sf_ir_node* node = sorted_nodes[i];
        if (node->type == SF_NODE_UNKNOWN || node->type >= SF_NODE_COUNT) continue;

        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        u32 node_idx = (u32)(node - ir->nodes);
        
        sf_ir_node* inputs[4] = {0};
        for (u8 k = 0; k < 4; ++k) {
            if (meta->ports[k]) inputs[k] = sf_ir_find_input_by_name(ir, node_idx, meta->ports[k]);
        }

        sf_type_info* out = &node->out_info;

        // 1. Resolve Output Shape
        switch (meta->shape_rule) {
            case SF_SHAPE_SPECIAL:
                if (node->type == SF_NODE_CONST) { /* Handled in pre-pass */ }
                else if (node->type == SF_NODE_INPUT) {
                    if (node->builtin_id == SF_BUILTIN_INDEX) {
                        u32 dom_idx = node->domain_node_idx;
                        if (dom_idx == UINT32_MAX) {
                            for (u32 j = 0; j < (u32)ir->node_count; ++j) if (ir->nodes[j].type == SF_NODE_OUTPUT) { dom_idx = j; break; }
                        }
                        if (dom_idx != UINT32_MAX && dom_idx != node_idx) *out = ir->nodes[dom_idx].out_info;
                        if (out->dtype == SF_DTYPE_UNKNOWN) out->dtype = SF_DTYPE_F32;
                    } else if (inputs[0] && out->ndim == 0) *out = inputs[0]->out_info;
                    else if (out->ndim == 0) *out = node->const_info;
                } else if (node->type == SF_NODE_OUTPUT) {
                    if (out->ndim == 0 && inputs[0]) *out = inputs[0]->out_info;
                    if (node->domain_node_idx == UINT32_MAX && inputs[0])
                        node->domain_node_idx = (inputs[0]->domain_node_idx == UINT32_MAX) ? (u32)(inputs[0] - ir->nodes) : inputs[0]->domain_node_idx;
                }
                break;

            case SF_SHAPE_SAME_AS_S1: if (inputs[0]) { out->ndim = inputs[0]->out_info.ndim; memcpy(out->shape, inputs[0]->out_info.shape, sizeof(int32_t)*SF_MAX_DIMS); } else { SF_REPORT_NODE(diag, node, "Missing S1 input for %s", meta->name); return false; } break;
            case SF_SHAPE_SAME_AS_S2: if (inputs[1]) { out->ndim = inputs[1]->out_info.ndim; memcpy(out->shape, inputs[1]->out_info.shape, sizeof(int32_t)*SF_MAX_DIMS); } else { SF_REPORT_NODE(diag, node, "Missing S2 input for %s", meta->name); return false; } break;
            case SF_SHAPE_BROADCAST:
                if (!inputs[0] || !inputs[1]) { SF_REPORT_NODE(diag, node, "Missing inputs for broadcast in %s", meta->name); return false; }
                if (inputs[2]) { sf_type_info tmp; if (!check_broadcast(node, &inputs[0]->out_info, &inputs[1]->out_info, &tmp, diag)) return false; if (!check_broadcast(node, &tmp, &inputs[2]->out_info, out, diag)) return false; }
                else if (!check_broadcast(node, &inputs[0]->out_info, &inputs[1]->out_info, out, diag)) return false;
                break;

            case SF_SHAPE_MATMUL:
                if (!inputs[0] || !inputs[1]) { SF_REPORT_NODE(diag, node, "Missing inputs for matmul"); return false; }
                out->ndim = 2;
                out->shape[0] = inputs[0]->out_info.shape[inputs[0]->out_info.ndim - 2];
                out->shape[1] = inputs[1]->out_info.shape[inputs[1]->out_info.ndim - 1];
                break;

            case SF_SHAPE_TRANSPOSE: if (!inputs[0]) { SF_REPORT_NODE(diag, node, "Missing input for transpose"); return false; } *out = inputs[0]->out_info; if (out->ndim >= 2) { int32_t t = out->shape[out->ndim-2]; out->shape[out->ndim-2] = out->shape[out->ndim-1]; out->shape[out->ndim-1] = t; } break;
            case SF_SHAPE_DOT: if (!inputs[0]) { SF_REPORT_NODE(diag, node, "Missing input for dot"); return false; } out->ndim = inputs[0]->out_info.ndim > 0 ? inputs[0]->out_info.ndim - 1 : 0; for(int k=0; k<out->ndim; ++k) out->shape[k] = inputs[0]->out_info.shape[k]; break;
            case SF_SHAPE_JOIN: if (!inputs[0] || !inputs[1]) { SF_REPORT_NODE(diag, node, "Missing inputs for join"); return false; } *out = inputs[0]->out_info; { int comps = 2; if (inputs[2]) comps++; if (inputs[3]) comps++; out->shape[out->ndim++] = comps; } break;
            case SF_SHAPE_GATHER: if (!inputs[1]) { SF_REPORT_NODE(diag, node, "Missing indices for gather"); return false; } out->ndim = inputs[1]->out_info.ndim; memcpy(out->shape, inputs[1]->out_info.shape, sizeof(int32_t)*SF_MAX_DIMS); break;
            case SF_SHAPE_RESHAPE: if (!inputs[1] || !inputs[1]->const_data) { SF_REPORT_NODE(diag, node, "Reshape needs constant shape input"); return false; } { int cnt = (int)sf_shape_calc_count(inputs[1]->const_info.shape, inputs[1]->const_info.ndim); out->ndim = (uint8_t)cnt; for(int k=0; k<cnt && k<SF_MAX_DIMS; ++k) out->shape[k] = (inputs[1]->const_info.dtype == SF_DTYPE_F32) ? (int)((f32*)inputs[1]->const_data)[k] : ((int*)inputs[1]->const_data)[k]; } break;
            case SF_SHAPE_SLICE: if (!inputs[1] || !inputs[1]->const_data) { SF_REPORT_NODE(diag, node, "Slice needs constant range input"); return false; } out->ndim = 1; out->shape[0] = (inputs[1]->const_info.dtype == SF_DTYPE_F32) ? (int)((f32*)inputs[1]->const_data)[1] : ((int*)inputs[1]->const_data)[1]; break;
            case SF_SHAPE_SCALAR: out->ndim = 0; out->shape[0] = 1; break;
        }

        // 2. Resolve DType
        sf_dtype dtype = SF_DTYPE_UNKNOWN;
        switch (meta->out_rule) {
            case SF_OUT_FORCE_F32:       dtype = SF_DTYPE_F32; break;
            case SF_OUT_FORCE_U8:        dtype = SF_DTYPE_U8;  break;
            case SF_OUT_FORCE_I32:       dtype = SF_DTYPE_I32; break;
            case SF_OUT_SAME_AS_INPUT:   if (inputs[0]) dtype = inputs[0]->out_info.dtype; break;
            case SF_OUT_SAME_AS_INPUT_2: if (inputs[1]) dtype = inputs[1]->out_info.dtype; break;
        }
        
        if (dtype == SF_DTYPE_UNKNOWN) {
            dtype = (out->dtype != SF_DTYPE_UNKNOWN) ? out->dtype : SF_DTYPE_F32;
        }
        out->dtype = dtype;

        // 3. Strides & Spatial Analysis
        sf_shape_calc_strides(out);
        u32 dom_idx = (node->domain_node_idx == UINT32_MAX) ? node_idx : node->domain_node_idx;
        size_t task_cnt = sf_shape_calc_count(ir->nodes[dom_idx].out_info.shape, ir->nodes[dom_idx].out_info.ndim);
        
        bool is_generator = (node->builtin_id != SF_BUILTIN_NONE);
        bool has_spatial_input = false;
        for (int k = 0; k < 4; ++k) if (inputs[k] && inputs[k]->is_spatial) has_spatial_input = true;

        node->is_spatial = (task_cnt > 1) || is_generator || has_spatial_input;

        // Inflation for generators: they must match the domain to produce a stream
        if (is_generator && task_cnt > 1) {
            const sf_type_info* dom_info = &ir->nodes[dom_idx].out_info;
            out->ndim = dom_info->ndim;
            memcpy(out->shape, dom_info->shape, sizeof(int32_t) * dom_info->ndim);
            sf_shape_calc_strides(out);
        }
    }
    return true;
}
