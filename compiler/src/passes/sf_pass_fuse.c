#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <string.h>
#include <stdlib.h>

bool sf_pass_fuse(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    if (!ir) {
        SF_REPORT(diag, NULL, "Fuse Pass: Internal Error - IR is NULL");
        return false;
    }

    u32* use_count = SF_ARENA_PUSH(ctx->arena, u32, ir->node_count);
    if (!use_count) {
        SF_REPORT(diag, NULL, "Fuse Pass: Out of memory for use_count");
        return false;
    }
    memset(use_count, 0, sizeof(u32) * ir->node_count);

    for (size_t i = 0; i < ir->link_count; ++i) {
        if (ir->links[i].src_node_idx < ir->node_count) {
            use_count[ir->links[i].src_node_idx]++;
        }
    }

    bool changed = false;

    for (size_t i = 0; i < ir->node_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;

        for (size_t r = 0; r < SF_FUSION_RULE_COUNT; ++r) {
            const sf_fusion_rule* rule = &SF_FUSION_RULES[r];
            if (node->type != rule->target_type) continue;

            // Try each pattern defined in the rule
            for (u8 p = 0; p < rule->match_count; ++p) {
                const sf_fusion_match* match = &rule->matches[p];
                sf_ir_node* inner_node = sf_ir_find_input_by_name(ir, (u32)i, match->port_name);
                
                if (inner_node && inner_node->type == match->match_type && use_count[inner_node - ir->nodes] <= match->max_use_count) {
                    u32 inner_idx = (u32)(inner_node - ir->nodes);
                    u32 outer_idx = (u32)i;

                    SF_LOG_DEBUG("Fusing %s and %s into %s", inner_node->id, node->id, SF_OP_METADATA[rule->replace_with].name);
                    
                    // Transform outer node
                    node->type = rule->replace_with;
                    
                    // Remap links
                    const char* other_port_name = (p == 0) ? rule->matches[1].port_name : rule->matches[0].port_name;

                    for (size_t l = 0; l < ir->link_count; ++l) {
                        sf_ir_link* link = &ir->links[l];
                        
                        // 1. Links that were going to inner_node -> now go to outer_node (with original port names)
                        if (link->dst_node_idx == inner_idx) {
                            link->dst_node_idx = outer_idx;
                        }
                        // 2. Links that were going to outer_node.other_port -> now to remapped port
                        else if (link->dst_node_idx == outer_idx && strcmp(link->dst_port_name, other_port_name) == 0) {
                            link->dst_port_name = match->remap_to_port;
                            // Reset dst_port to force re-resolve if needed, but names are safer
                        }
                        // 3. Link from inner_node to outer_node -> kill it
                        else if (link->src_node_idx == inner_idx && link->dst_node_idx == outer_idx) {
                            link->src_node_idx = UINT32_MAX;
                            link->dst_node_idx = UINT32_MAX;
                        }
                    }

                    inner_node->type = SF_NODE_UNKNOWN;
                    changed = true;
                    goto next_node; // Move to next node after successful fusion
                }
            }
        }
        next_node:;
    }

    if (changed) {
        size_t write_idx = 0;
        for (size_t l = 0; l < ir->link_count; ++l) {
            if (ir->links[l].src_node_idx != UINT32_MAX) {
                ir->links[write_idx++] = ir->links[l];
            }
        }
        ir->link_count = write_idx;
    }

    return true;
}