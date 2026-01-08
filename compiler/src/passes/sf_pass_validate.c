#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/isa/sf_op_defs.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>

static bool check_broadcast_compat(const sf_type_info* a, const sf_type_info* b) {
    sf_type_info dummy;
    return sf_shape_broadcast(a, b, &dummy);
}

bool sf_pass_validate(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_ir_node** sorted_nodes = ctx->sorted_nodes;
    size_t count = ctx->sorted_count;
    bool success = true;

    for (size_t i = 0; i < count; ++i) {
        sf_ir_node* node = sorted_nodes[i];
        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        if (node->type == SF_NODE_UNKNOWN) continue;

        u32 node_idx = (u32)(node - ir->nodes);
        sf_ir_node* inputs[4] = {0};
        
        // 1. Generic Arity & Type Validation
        for (u8 k = 0; k < meta->arity; ++k) {
            inputs[k] = sf_ir_find_input_by_name(ir, node_idx, meta->ports[k]);
            if (!inputs[k]) {
                // Special case: Join allows 2 or 3 inputs (c and d are optional)
                if (node->type == SF_NODE_JOIN && k >= 2) continue;

                SF_REPORT_NODE(diag, node, "Validation Error: Missing required input port '%s' for node '%s' (%s)", 
                    meta->ports[k], node->id, meta->name);
                success = false;
                continue;
            }

            u32 type_bit = (1 << inputs[k]->out_info.dtype);
            if (!(meta->input_mask & type_bit)) {
                SF_REPORT_NODE(diag, node, "Type Mismatch: Input '%s' of node '%s' has invalid dtype. Operation '%s' expects mask 0x%X",
                    meta->ports[k], node->id, meta->name, meta->input_mask);
                success = false;
            }
        }

        // 2. Rank Validation
        if (inputs[0]) {
            i8 rank = inputs[0]->out_info.ndim;
            if (meta->min_rank != -1 && rank < meta->min_rank) {
                SF_REPORT_NODE(diag, node, "Rank Error: Operation '%s' requires minimum rank %d, but got %d", meta->name, meta->min_rank, rank);
                success = false;
            }
            if (meta->max_rank != -1 && rank > meta->max_rank) {
                SF_REPORT_NODE(diag, node, "Rank Error: Operation '%s' allows maximum rank %d, but got %d", meta->name, meta->max_rank, rank);
                success = false;
            }
        }

        // 3. Declarative Assertions
        for (u8 a = 0; a < meta->assertion_count; ++a) {
            const sf_op_assert* asrt = &meta->assertions[a];
            
            if (asrt->type == SF_ASSERT_MATCH_DIM) {
                if (!inputs[asrt->p0] || !inputs[asrt->p1]) continue;
                
                const sf_type_info* info0 = &inputs[asrt->p0]->out_info;
                const sf_type_info* info1 = &inputs[asrt->p1]->out_info;
                
                int axis0 = (asrt->a0 < 0) ? (info0->ndim + asrt->a0) : asrt->a0;
                int axis1 = (asrt->a1 < 0) ? (info1->ndim + asrt->a1) : asrt->a1;
                
                if (axis0 < 0 || axis0 >= info0->ndim || axis1 < 0 || axis1 >= info1->ndim) {
                     SF_REPORT_NODE(diag, node, "Validation Error: Axis %d/%d out of bounds in assertion for '%s'", axis0, axis1, node->id);
                     success = false;
                     continue;
                }
                
                if (info0->shape[axis0] != info1->shape[axis1]) {
                    SF_REPORT_NODE(diag, node, "Validation Error: %s in '%s' (%d vs %d)", 
                        asrt->msg[0] ? asrt->msg : "Dimension mismatch", node->id, info0->shape[axis0], info1->shape[axis1]);
                    success = false;
                }
            }
            else if (asrt->type == SF_ASSERT_BROADCAST_COMPATIBLE) {
                if (!inputs[0] || !inputs[1]) continue;
                if (!check_broadcast_compat(&inputs[0]->out_info, &inputs[1]->out_info)) {
                    char s1[64], s2[64];
                    sf_shape_format(&inputs[0]->out_info, s1, sizeof(s1));
                    sf_shape_format(&inputs[1]->out_info, s2, sizeof(s2));
                    SF_REPORT_NODE(diag, node, "Shape Mismatch: Cannot broadcast inputs of node '%s' (%s vs %s)", node->id, s1, s2);
                    success = false;
                }
            }
        }
    }

    return success;
}