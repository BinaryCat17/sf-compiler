#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/isa/sf_op_defs.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>

static bool shapes_match(const sf_type_info* a, const sf_type_info* b) {
    if (a->ndim != b->ndim) return false;
    for (int i = 0; i < a->ndim; ++i) {
        if (a->shape[i] != b->shape[i]) return false;
    }
    return true;
}

static bool is_scalar(const sf_type_info* info) {
    return (info->ndim == 0 || (info->ndim == 1 && info->shape[0] == 1));
}

bool sf_pass_validate(sf_graph_ir* ir, sf_ir_node** sorted_nodes, size_t count, sf_compiler_diag* diag) {
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
                SF_REPORT_NODE(diag, node, "Validation Error: Missing required input port '%s' for node '%s' (%s)", 
                    meta->ports[k], node->id, meta->name);
                success = false;
                continue;
            }

            // Check if input dtype matches operation's mask
            u32 type_bit = (1 << inputs[k]->out_info.dtype);
            if (!(meta->input_mask & type_bit)) {
                SF_REPORT_NODE(diag, node, "Type Mismatch: Input '%s' of node '%s' has invalid dtype. Operation '%s' expects mask 0x%X",
                    meta->ports[k], node->id, meta->name, meta->input_mask);
                success = false;
            }
        }

        // 2. Shape-Rule Specific Validation
        const sf_type_info* info1 = inputs[0] ? &inputs[0]->out_info : NULL;
        const sf_type_info* info2 = inputs[1] ? &inputs[1]->out_info : NULL;

        switch (meta->shape_rule) {
            case SF_SHAPE_BROADCAST:
                if (info1 && info2) {
                    if (!is_scalar(info1) && !is_scalar(info2) && !shapes_match(info1, info2)) {
                        char s1[64], s2[64];
                        sf_shape_format(info1, s1, sizeof(s1));
                        sf_shape_format(info2, s2, sizeof(s2));
                        SF_REPORT_NODE(diag, node, "Shape Mismatch: Cannot broadcast inputs of node '%s' (%s vs %s)", node->id, s1, s2);
                        success = false;
                    }
                }
                break;

            case SF_SHAPE_SAME_AS_S1:
                if (info1 && !shapes_match(&node->out_info, info1)) {
                    char s_out[64], s_in[64];
                    sf_shape_format(&node->out_info, s_out, sizeof(s_out));
                    sf_shape_format(info1, s_in, sizeof(s_in));
                    SF_REPORT_NODE(diag, node, "Shape Error: Output of '%s' must match Input 1 (%s vs %s)", node->id, s_out, s_in);
                    success = false;
                }
                break;

            case SF_SHAPE_MATMUL:
                if (info1 && info2) {
                    if (info1->ndim < 2 || info2->ndim < 2) {
                        SF_REPORT_NODE(diag, node, "MatMul Error: Inputs must be at least 2D in '%s' (got %dD and %dD)", node->id, info1->ndim, info2->ndim);
                        success = false;
                    } else if (info1->shape[1] != info2->shape[0]) {
                        SF_REPORT_NODE(diag, node, "MatMul Error: Inner dimensions mismatch [%d] vs [%d] in '%s'", 
                            info1->shape[1], info2->shape[0], node->id);
                        success = false;
                    }
                }
                break;

            case SF_SHAPE_DOT:
                if (info1 && info2) {
                    if (info1->shape[info1->ndim-1] != info2->shape[info2->ndim-1]) {
                        SF_REPORT_NODE(diag, node, "Dot Error: Last dimensions mismatch in '%s' (%d vs %d)", 
                            node->id, info1->shape[info1->ndim-1], info2->shape[info2->ndim-1]);
                        success = false;
                    }
                }
                break;

            default:
                break;
        }
    }

    return success;
}