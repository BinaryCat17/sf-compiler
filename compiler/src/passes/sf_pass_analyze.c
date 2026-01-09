#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <sionflow/isa/sf_opcodes.h>
#include <sionflow/isa/sf_op_defs.h>
#include <stdio.h>
#include <string.h>

typedef bool (*sf_shape_resolver)(sf_ir_node* node, sf_ir_node* inputs[4], sf_compiler_diag* diag);

extern const sf_shape_resolver SF_GENERATED_SHAPE_RESOLVERS[];

bool sf_pass_analyze(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    
    // 1. First pass: Initialize all nodes (including metadata nodes like INPUT/OUTPUT)
    for (size_t i = 0; i < ir->node_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;
        sf_shape_calc_strides(&node->out_info);
    }

    // 2. Second pass: Resolve shapes and dtypes for computational nodes in topological order
    sf_ir_node** sorted_nodes = ctx->sorted_nodes;
    size_t count = ctx->sorted_count;

    bool success = true;
    for (size_t i = 0; i < count; ++i) {
        sf_ir_node* node = sorted_nodes[i];
        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        
        sf_ir_node* inputs[4] = {0};
        for (u8 k = 0; k < 4; ++k) {
            if (meta->ports[k]) inputs[k] = find_input_source(ir, (u32)(node - ir->nodes), k);
        }

        // Resolve Shape: Using strictly generated resolvers
        if (node->type < SF_NODE_COUNT) {
            if (SF_GENERATED_SHAPE_RESOLVERS[node->type]) {
                if (!SF_GENERATED_SHAPE_RESOLVERS[node->type](node, inputs, diag)) success = false;
            }
        }

        // Resolve DType
        static const sf_dtype RULE_TO_DTYPE[] = {
            [SF_OUT_SAME_AS_INPUT]   = SF_DTYPE_UNKNOWN, // Handled below
            [SF_OUT_SAME_AS_INPUT_2] = SF_DTYPE_UNKNOWN, 
            [SF_OUT_FORCE_F32]       = SF_DTYPE_F32,
            [SF_OUT_FORCE_U8]        = SF_DTYPE_U8,
            [SF_OUT_FORCE_I32]       = SF_DTYPE_I32
        };

        if (meta->out_rule >= SF_OUT_FORCE_F32) {
            node->out_info.dtype = RULE_TO_DTYPE[meta->out_rule];
        } else if (meta->out_rule == SF_OUT_SAME_AS_INPUT && inputs[0]) {
            node->out_info.dtype = inputs[0]->out_info.dtype;
        } else if (meta->out_rule == SF_OUT_SAME_AS_INPUT_2 && inputs[1]) {
            node->out_info.dtype = inputs[1]->out_info.dtype;
        }

        if (node->out_info.dtype == SF_DTYPE_UNKNOWN) node->out_info.dtype = SF_DTYPE_F32;

        // Domain Analysis
        u32 dom_idx = node->domain_node_idx;
        size_t task_cnt = 1;
        if (dom_idx != UINT32_MAX) {
            task_cnt = sf_shape_calc_count(ir->nodes[dom_idx].out_info.shape, ir->nodes[dom_idx].out_info.ndim);
        }
        
        bool is_generator = (meta->flags & SF_OP_FLAG_GENERATOR);
        node->is_spatial = (task_cnt > 1) || is_generator;
        
        if (is_generator && dom_idx != UINT32_MAX && !(meta->flags & SF_OP_FLAG_FORCE_DOM)) {
            node->out_info = ir->nodes[dom_idx].out_info;
        }
        
        sf_shape_calc_strides(&node->out_info);
    }
    return success;
}
