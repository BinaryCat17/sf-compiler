#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <string.h>
#include <stdlib.h>

bool sf_pass_decompose(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;
    bool changed = false;

    // We iterate over the initial nodes. New nodes added during decomposition 
    // will be appended to the end and don't need decomposition themselves (usually).
    u32 initial_count = (u32)ir->node_count;

    for (u32 i = 0; i < initial_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;

        const sf_lowering_rule* rule = NULL;
        for (size_t r = 0; r < SF_LOWERING_RULE_COUNT; ++r) {
            if (SF_LOWERING_RULES[r].target_type == node->type) {
                rule = &SF_LOWERING_RULES[r];
                break;
            }
        }

        if (rule) {
            if (ir->node_count + rule->step_count > ir->node_cap) {
                SF_REPORT(diag, &node->loc, "Decomposition Error: Node capacity exceeded for '%s'", node->id);
                return false;
            }

            SF_LOG_DEBUG("Decomposing node '%s' (%s) into %d nodes", node->id, SF_OP_METADATA[node->type].name, rule->step_count);
            
            u32 base_idx = (u32)ir->node_count;
            ir->node_count += rule->step_count;
            
            // 1. Create Subgraph Nodes
            for (u32 s = 0; s < rule->step_count; ++s) {
                const sf_lowering_step* step = &rule->steps[s];
                sf_ir_node* dst = &ir->nodes[base_idx + s];
                dst->id = sf_arena_sprintf(arena, "%s.%s", node->id, step->id);
                dst->type = step->type;
                dst->loc = node->loc;
                dst->domain_node_idx = node->domain_node_idx;
                // Inherit other properties if necessary
            }

            // 2. Map Connections (Internal)
            for (u32 s = 0; s < rule->step_count; ++s) {
                const sf_lowering_step* step = &rule->steps[s];
                const sf_op_metadata* meta = &SF_OP_METADATA[step->type];
                
                for (u32 p = 0; p < 4; ++p) {
                    const char* input_source = step->input_map[p];
                    if (!input_source) continue;

                    // Search if it's an internal node ID
                    u32 src_sub_idx = UINT32_MAX;
                    for (u32 j = 0; j < rule->step_count; ++j) {
                        if (strcmp(input_source, rule->steps[j].id) == 0) {
                            src_sub_idx = j;
                            break;
                        }
                    }

                    if (src_sub_idx != UINT32_MAX) {
                        // Create internal link
                        if (ir->link_count >= ir->link_cap) {
                             SF_REPORT(diag, NULL, "Decomposition Error: Link capacity exceeded");
                             return false;
                        }

                        sf_ir_link* link = &ir->links[ir->link_count++];
                        link->src_node_idx = base_idx + src_sub_idx;
                        link->src_port_name = "out";
                        link->src_port = 0;
                        link->dst_node_idx = base_idx + s;
                        link->dst_port_name = sf_arena_strdup(arena, meta->ports[p]);
                        link->dst_port = p;
                    }
                }
            }

            // 3. Re-route External Links
            u32 final_output_idx = UINT32_MAX;
            for (u32 j = 0; j < rule->step_count; ++j) {
                if (strcmp(rule->output_node_id, rule->steps[j].id) == 0) {
                    final_output_idx = base_idx + j;
                    break;
                }
            }

            for (size_t l = 0; l < ir->link_count; ++l) {
                sf_ir_link* link = &ir->links[l];
                if (link->dst_node_idx == i) {
                    // This link was going to the original node. 
                    // Redirect it to the internal node that expects this port.
                    for (u32 s = 0; s < rule->step_count; ++s) {
                        for (u32 p = 0; p < 4; ++p) {
                            if (rule->steps[s].input_map[p] && strcmp(rule->steps[s].input_map[p], link->dst_port_name) == 0) {
                                link->dst_node_idx = base_idx + s;
                                // Port names usually match (e.g. "in" -> "in")
                                break;
                            }
                        }
                    }
                } else if (link->src_node_idx == i) {
                    link->src_node_idx = final_output_idx;
                }
            }

            node->type = SF_NODE_UNKNOWN;
            changed = true;
        }
    }

    return true;
}