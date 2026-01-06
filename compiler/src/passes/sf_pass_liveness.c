#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <stdlib.h>
#include <string.h>

bool sf_pass_liveness(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_ir_node** sorted = ctx->sorted_nodes;
    size_t count = ctx->sorted_count;

    if (!ir || !sorted) {
        SF_REPORT(diag, NULL, "Liveness Pass: Internal Error - IR or sorted nodes is NULL");
        return false;
    }

    // 1. Map node pointers to their sorted position
    u32* sorted_pos = SF_ARENA_PUSH(ctx->arena, u32, ir->node_count);
    if (!sorted_pos) return false;
    for(u32 i=0; i<ir->node_count; ++i) sorted_pos[i] = UINT32_MAX;
    for(u32 i=0; i<count; ++i) sorted_pos[sorted[i] - ir->nodes] = (u32)i;

    // 2. Find last use for each node
    u32* last_use = SF_ARENA_PUSH(ctx->arena, u32, ir->node_count);
    if (!last_use) return false;
    for (u32 i = 0; i < ir->node_count; ++i) last_use[i] = 0;

    for (size_t l = 0; l < ir->link_count; ++l) {
        u32 src = ir->links[l].src_node_idx;
        u32 dst = ir->links[l].dst_node_idx;
        if (src < ir->node_count && dst < ir->node_count) {
            u32 pos = sorted_pos[dst];
            if (pos != UINT32_MAX && (pos > last_use[src])) {
                last_use[src] = pos;
            }
        }
    }

    // 3. Register Allocation
    u16 next_reg = 0;
    u32* reg_free_at = SF_ARENA_PUSH(ctx->arena, u32, ir->node_count); 
    if (!reg_free_at) return false;
    for(int i=0; i<ir->node_count; ++i) reg_free_at[i] = 0;

    for (size_t i = 0; i < count; ++i) {
        sf_ir_node* node = sorted[i];
        u32 node_idx = (u32)(node - ir->nodes);

        if (node->type == SF_NODE_UNKNOWN) {
            node->out_reg_idx = 0;
            continue;
        }

        // Special handling for persistent nodes (Inputs, Constants, Outputs)
        // AND for nodes that change shape (to avoid buffer overflow in aliased registers)
        bool change_shape = (node->type == SF_NODE_JOIN || node->type == SF_NODE_RESHAPE || node->type == SF_NODE_SLICE);
        bool persistent = (node->type == SF_NODE_INPUT || node->type == SF_NODE_CONST || node->type == SF_NODE_OUTPUT || change_shape);

        if (persistent) {
            node->out_reg_idx = next_reg++;
            reg_free_at[node->out_reg_idx] = (u32)count + 1; // Never reuse
        } else {
            // Try to find a free register
            int found_reg = -1;
            for (u16 r = 0; r < next_reg; ++r) {
                // Can reuse if register becomes free at or before current instruction
                // AND it's not a persistent register.
                if (reg_free_at[r] <= (u32)i) {
                    // SAFETY CHECK: Does the new node fit in the existing register's "type profile"?
                    // To be safe, we only reuse if dtype and element count match,
                    // OR if the register was only used for compatible types.
                    // Since we only store ONE type info per register in the binary,
                    // we MUST NOT reuse for different dtypes.
                    
                    bool compatible = true;
                    for (u32 j = 0; j < i; ++j) {
                        if (sorted[j]->out_reg_idx == r) {
                            if (sorted[j]->out_info.dtype != node->out_info.dtype) { compatible = false; break; }
                            size_t old_cnt = sf_shape_calc_count(sorted[j]->out_info.shape, sorted[j]->out_info.ndim);
                            size_t new_cnt = sf_shape_calc_count(node->out_info.shape, node->out_info.ndim);
                            if (old_cnt != new_cnt) { compatible = false; break; }
                        }
                    }

                    if (compatible) {
                        found_reg = r;
                        break;
                    }
                }
            }

            if (found_reg >= 0) {
                // Double check: is this register used as an input for the SAME node?
                // If so, we cannot reuse it if the operation is not safe for in-place.
                bool used_as_input = false;
                for (size_t l = 0; l < ir->link_count; ++l) {
                    if (ir->links[l].dst_node_idx == node_idx) {
                        if (ir->nodes[ir->links[l].src_node_idx].out_reg_idx == (u16)found_reg) {
                            used_as_input = true;
                            break;
                        }
                    }
                }

                if (!used_as_input) {
                    node->out_reg_idx = (u16)found_reg;
                } else {
                    node->out_reg_idx = next_reg++;
                }
            } else {
                node->out_reg_idx = next_reg++;
            }
            
            u32 end_of_life = last_use[node_idx];
            if (end_of_life > reg_free_at[node->out_reg_idx]) {
                reg_free_at[node->out_reg_idx] = end_of_life;
            }
        }
    }

    SF_LOG_INFO("Liveness: Allocated %u registers for %u nodes", next_reg, (u32)count);

    return true;
}