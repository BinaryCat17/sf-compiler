#include "sf_passes.h"
#include "sf_graph_utils.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_memory.h>
#include <sionflow/base/sf_utils.h>
#include "sf_compiler_internal.h"
#include <string.h>

sf_ir_node* sf_ir_node_add(sf_graph_ir* ir, sf_arena* arena, const char* id, sf_node_type type) {
    if (ir->node_count >= ir->node_cap) return NULL;
    sf_ir_node* node = &ir->nodes[ir->node_count++];
    memset(node, 0, sizeof(sf_ir_node));
    node->id = sf_arena_strdup(arena, id);
    node->type = type;
    node->domain_node_idx = UINT32_MAX;
    for (int i = 0; i < 4; ++i) node->inputs[i].src_node_idx = UINT32_MAX;
    return node;
}

void sf_ir_node_remove(sf_graph_ir* ir, u32 node_idx) {
    if (node_idx < ir->node_count) {
        ir->nodes[node_idx].type = SF_NODE_UNKNOWN;
        ir->nodes[node_idx].users = NULL;
        for (int i = 0; i < 4; ++i) ir->nodes[node_idx].inputs[i].src_node_idx = UINT32_MAX;
    }
}

void sf_ir_links_remap_node(sf_graph_ir* ir, u32 old_node_idx, u32 new_node_idx) {
    sf_builder_replace_node(ir, old_node_idx, new_node_idx);
}

// --- Graph Builder API ---

void sf_builder_connect(sf_graph_ir* ir, sf_arena* arena, sf_port src, sf_port dst) {
    if (SF_PORT_IS_NULL(src) || SF_PORT_IS_NULL(dst)) return;
    
    // 1. Maintain O(1) Inputs
    ir->nodes[dst.node_idx].inputs[dst.port_idx].src_node_idx = src.node_idx;
    ir->nodes[dst.node_idx].inputs[dst.port_idx].src_port_idx = src.port_idx;

    // 2. Maintain O(1) Users (Linked List)
    sf_ir_user* user = SF_ARENA_PUSH(arena, sf_ir_user, 1);
    user->node_idx = dst.node_idx;
    user->port_idx = dst.port_idx;
    user->next = ir->nodes[src.node_idx].users;
    ir->nodes[src.node_idx].users = user;
}

void sf_builder_disconnect(sf_graph_ir* ir, sf_port dst) {
    if (SF_PORT_IS_NULL(dst)) return;
    
    u32 old_src_idx = ir->nodes[dst.node_idx].inputs[dst.port_idx].src_node_idx;
    if (old_src_idx != UINT32_MAX) {
        // Remove from source's user list
        sf_ir_user** curr = &ir->nodes[old_src_idx].users;
        while (*curr) {
            if ((*curr)->node_idx == dst.node_idx && (*curr)->port_idx == dst.port_idx) {
                sf_ir_user* to_free = *curr;
                *curr = (*curr)->next;
                // Note: to_free is in arena, we can't 'free' it, but we unlinked it.
                break;
            }
            curr = &((*curr)->next);
        }
    }

    ir->nodes[dst.node_idx].inputs[dst.port_idx].src_node_idx = UINT32_MAX;
}

sf_port sf_builder_get_source(sf_graph_ir* ir, sf_port dst) {
    if (SF_PORT_IS_NULL(dst)) return SF_PORT_NULL;
    u32 src_idx = ir->nodes[dst.node_idx].inputs[dst.port_idx].src_node_idx;
    if (src_idx == UINT32_MAX) return SF_PORT_NULL;
    return (sf_port){ src_idx, ir->nodes[dst.node_idx].inputs[dst.port_idx].src_port_idx };
}

sf_ir_node* find_input_source(sf_graph_ir* ir, u32 dst_node_idx, u32 dst_port) {
    sf_port src = sf_builder_get_source(ir, (sf_port){ dst_node_idx, dst_port });
    return SF_PORT_IS_NULL(src) ? NULL : &ir->nodes[src.node_idx];
}

sf_ir_node* sf_ir_find_input_by_name(sf_graph_ir* ir, u32 dst_node_idx, const char* port_name) {
    if (!port_name) return NULL;
    u32 pi = sf_compiler_get_port_index(ir->nodes[dst_node_idx].type, port_name);
    return find_input_source(ir, dst_node_idx, pi);
}

u32 sf_compiler_get_port_index(sf_node_type type, const char* port_name) {
    if (type >= SF_NODE_COUNT || !port_name) return 0;
    const sf_op_metadata* meta = &SF_OP_METADATA[type];
    for (u32 i = 0; i < 4; ++i) {
        if (meta->ports[i] && strcmp(meta->ports[i], port_name) == 0) return i;
    }
    return 0;
}

sf_node_type sf_compiler_get_node_type(const char* type_str) {
    if (!type_str) return SF_NODE_UNKNOWN;
    for (int i = 0; i < SF_NODE_COUNT; ++i) {
        if (strcmp(SF_OP_METADATA[i].name, type_str) == 0) return (sf_node_type)i;
    }
    for (size_t i = 0; i < SF_COMPILER_ALIAS_COUNT; ++i) {
        if (strcmp(SF_COMPILER_ALIASES[i].from, type_str) == 0) return SF_COMPILER_ALIASES[i].to;
    }
    return SF_NODE_UNKNOWN;
}

const char* sf_ir_get_port_name(sf_node_type type, u32 port_idx) {
    if (type >= SF_NODE_COUNT || port_idx >= 4) return "unknown";
    const char* name = SF_OP_METADATA[type].ports[port_idx];
    return name ? name : "unknown";
}

void sf_builder_replace_node(sf_graph_ir* ir, u32 old_node_idx, u32 new_node_idx) {
    if (old_node_idx == new_node_idx) return;
    sf_ir_user* user = ir->nodes[old_node_idx].users;
    while (user) {
        ir->nodes[user->node_idx].inputs[user->port_idx].src_node_idx = new_node_idx;
        // The list is valid since we only care about who uses the 'channel'.
        // In a true pointer IR, we would move the 'users' list head to the new node.
        user = user->next;
    }
    // Append the users list to the new node
    sf_ir_user** tail = &ir->nodes[new_node_idx].users;
    while (*tail) tail = &((*tail)->next);
    *tail = ir->nodes[old_node_idx].users;
    ir->nodes[old_node_idx].users = NULL;
}

void sf_builder_remove_node(sf_graph_ir* ir, u32 node_idx) {
    for (int p = 0; p < 4; ++p) sf_builder_disconnect(ir, (sf_port){ node_idx, p });
    sf_ir_node_remove(ir, node_idx);
}

u32 find_node_by_reg(const sf_graph_ir* ir, u16 reg_idx) {
    for (u32 i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].type != SF_NODE_UNKNOWN && ir->nodes[i].out_reg_idx == reg_idx) return i;
    }
    return UINT32_MAX;
}

bool sf_ir_parse_data(const struct sf_json_value* val, sf_dtype dtype, size_t count, void* out_data) {
    if (!val || !out_data) return false;
    typedef struct { f64 n; } json_num; // Dummy for as.n
    if (val->type == SF_JSON_VAL_ARRAY) {
        for (size_t i = 0; i < val->as.array.count && i < count; ++i) {
            sf_json_value* item = &val->as.array.items[i];
            if (dtype == SF_DTYPE_F32) ((f32*)out_data)[i] = (f32)item->as.n;
            else if (dtype == SF_DTYPE_I32) ((i32*)out_data)[i] = (i32)item->as.n;
            else if (dtype == SF_DTYPE_U8) ((u8*)out_data)[i] = (u8)item->as.n;
        }
    } else if (val->type == SF_JSON_VAL_NUMBER) {
        if (dtype == SF_DTYPE_F32) ((f32*)out_data)[0] = (f32)val->as.n;
        else if (dtype == SF_DTYPE_I32) ((i32*)out_data)[0] = (i32)val->as.n;
        else if (dtype == SF_DTYPE_U8) ((u8*)out_data)[0] = (u8)val->as.n;
    }
    return true;
}

// --- Rewriting logic ---

bool sf_ir_node_replace_with_subgraph(sf_graph_ir* ir, u32 node_idx, const sf_lowering_rule* rule, sf_arena* arena, sf_compiler_diag* diag) {
    sf_ir_node* original = &ir->nodes[node_idx];
    u32 base_idx = (u32)ir->node_count;
    if (ir->node_count + rule->step_count > ir->node_cap) return false;

    for (u32 i = 0; i < rule->step_count; ++i) {
        const sf_lowering_step* s = &rule->steps[i];
        sf_ir_node* n = sf_ir_node_add(ir, arena, sf_arena_sprintf(arena, "%s.%s", original->id, s->id), s->type);
        n->loc = original->loc; n->domain_node_idx = original->domain_node_idx;
    }

    for (u32 i = 0; i < rule->step_count; ++i) {
        for (u32 p = 0; p < 4; ++p) {
            const char* input_id = rule->steps[i].input_map[p];
            if (!input_id) continue;
            bool internal = false;
            for (u32 j = 0; j < rule->step_count; ++j) {
                if (strcmp(input_id, rule->steps[j].id) == 0) {
                    sf_builder_connect(ir, arena, (sf_port){ base_idx + j, 0 }, (sf_port){ base_idx + i, p });
                    internal = true; break;
                }
            }
            if (!internal) {
                for (u32 op = 0; op < 4; ++op) {
                    if (strcmp(input_id, sf_ir_get_port_name(original->type, op)) == 0) {
                        sf_port producer = sf_builder_get_source(ir, (sf_port){ node_idx, op });
                        if (!SF_PORT_IS_NULL(producer)) sf_builder_connect(ir, arena, producer, (sf_port){ base_idx + i, p });
                        break;
                    }
                }
            }
        }
    }

    u32 final_out = UINT32_MAX;
    for (u32 i = 0; i < rule->step_count; ++i) if (strcmp(rule->output_node_id, rule->steps[i].id) == 0) { final_out = base_idx + i; break; }
    if (final_out != UINT32_MAX) sf_builder_replace_node(ir, node_idx, final_out);
    sf_builder_remove_node(ir, node_idx); return true;
}

bool sf_ir_node_try_fuse(sf_graph_ir* ir, u32 node_idx, const sf_fusion_rule* rule, sf_arena* arena) {
    sf_ir_node* target = &ir->nodes[node_idx];
    if (target->type != rule->target_type) return false;
    u32 m[2] = { UINT32_MAX, UINT32_MAX };
    for (u8 i = 0; i < rule->match_count; ++i) {
        u32 pi = sf_compiler_get_port_index(target->type, rule->matches[i].port_name);
        sf_port src = sf_builder_get_source(ir, (sf_port){ node_idx, pi });
        if (SF_PORT_IS_NULL(src) || ir->nodes[src.node_idx].type != rule->matches[i].match_type) return false;
        m[i] = src.node_idx;
    }
    sf_ir_node* f = sf_ir_node_add(ir, arena, sf_arena_sprintf(arena, "%s_f", target->id), rule->replace_with);
    u32 fused_idx = (u32)(f - ir->nodes);
    f->loc = target->loc; f->domain_node_idx = target->domain_node_idx;
    sf_builder_replace_node(ir, node_idx, fused_idx);
    for (u8 i = 0; i < rule->match_count; ++i) {
        u32 target_p = sf_compiler_get_port_index(f->type, rule->matches[i].remap_to_port);
        const sf_op_metadata* m_meta = &SF_OP_METADATA[ir->nodes[m[i]].type];
        for (u32 p = 0; p < 4; ++p) if (m_meta->ports[p]) {
            sf_port s = sf_builder_get_source(ir, (sf_port){ m[i], p });
            if (!SF_PORT_IS_NULL(s)) sf_builder_connect(ir, arena, s, (sf_port){ fused_idx, target_p + p });
        }
        sf_builder_remove_node(ir, m[i]);
    }
    sf_builder_remove_node(ir, node_idx); return true;
}

u32* sf_ir_graph_graft(sf_graph_ir* dst, const sf_graph_ir* src, const char* prefix, sf_arena* arena) {
    u32* map = SF_ARENA_PUSH(arena, u32, src->node_count);
    for (u32 i = 0; i < src->node_count; ++i) {
        if (src->nodes[i].type == SF_NODE_UNKNOWN) { map[i] = UINT32_MAX; continue; }
        sf_ir_node* d = sf_ir_node_add(dst, arena, prefix ? sf_arena_sprintf(arena, "%s::%s", prefix, src->nodes[i].id) : src->nodes[i].id, src->nodes[i].type);
        d->loc = src->nodes[i].loc; d->const_info = src->nodes[i].const_info; d->const_data = src->nodes[i].const_data; d->sub_graph_path = src->nodes[i].sub_graph_path;
        d->out_info = src->nodes[i].out_info; map[i] = (u32)(d - dst->nodes);
    }
    for (u32 i = 0; i < src->node_count; ++i) {
        if (src->nodes[i].type == SF_NODE_UNKNOWN) continue;
        for (int p = 0; p < 4; ++p) {
            u32 s_idx = src->nodes[i].inputs[p].src_node_idx;
            if (s_idx != UINT32_MAX) sf_builder_connect(dst, arena, (sf_port){ map[s_idx], src->nodes[i].inputs[p].src_port_idx }, (sf_port){ map[i], p });
        }
    }
    return map;
}

bool sf_ir_node_inline(sf_graph_ir* ir, u32 call_node_idx, const sf_graph_ir* subgraph, const char* prefix, sf_arena* arena) {
    u32* map = sf_ir_graph_graft(ir, subgraph, prefix, arena);
    if (!map) return false;
    for (u32 i = 0; i < subgraph->node_count; ++i) {
        if (subgraph->nodes[i].type == SF_NODE_INPUT) {
            u32 grafted_input_idx = map[i];
            const char* input_id = subgraph->nodes[i].id;
            // Scan through consumers of the CALL node ports to find which one maps to this input ID
            // Since we don't have port names on CALL node anymore, we use the fact that CALL node 
            // ports were named after subgraph inputs in the original manifest.
            for (u32 p = 0; p < 4; ++p) {
                // This is a bit tricky now without the link names, but in SionFlow, 
                // we can look at who provides data to the CALL node's port P.
                sf_port producer = sf_builder_get_source(ir, (sf_port){ call_node_idx, p });
                if (!SF_PORT_IS_NULL(producer)) {
                    // Match by convention or metadata if possible. For now, we assume port P maps to input P.
                    // A better way is to store the port name in the CALL node or use a dedicated map.
                    sf_builder_replace_node(ir, grafted_input_idx, producer.node_idx);
                }
            }
            sf_builder_remove_node(ir, grafted_input_idx);
        } else if (subgraph->nodes[i].type == SF_NODE_OUTPUT) {
            u32 grafted_output_idx = map[i];
            sf_port internal_producer = sf_builder_get_source(ir, (sf_port){ grafted_output_idx, 0 });
            if (!SF_PORT_IS_NULL(internal_producer)) {
                if (ir->nodes[internal_producer.node_idx].out_info.dtype == SF_DTYPE_UNKNOWN) ir->nodes[internal_producer.node_idx].out_info = subgraph->nodes[i].out_info;
                sf_builder_replace_node(ir, call_node_idx, internal_producer.node_idx);
            }
            sf_builder_remove_node(ir, grafted_output_idx);
        }
    }
    sf_builder_remove_node(ir, call_node_idx); return true;
}

static bool sort_recursive(sf_graph_ir* ir, u32 node_idx, bool* visited, bool* stack, u32* order, u32* count, sf_compiler_diag* diag) {
    if (stack[node_idx]) return false;
    if (visited[node_idx]) return true;
    visited[node_idx] = stack[node_idx] = true;
    sf_ir_node* node = &ir->nodes[node_idx];
    for (int p = 0; p < 4; ++p) {
        u32 s_idx = node->inputs[p].src_node_idx;
        if (s_idx != UINT32_MAX) if (!sort_recursive(ir, s_idx, visited, stack, order, count, diag)) return false;
    }
    stack[node_idx] = false; order[(*count)++] = node_idx; return true;
}

bool sf_ir_graph_sort(sf_graph_ir* ir, u32* out_order, sf_arena* arena, sf_compiler_diag* diag) {
    bool *v = SF_ARENA_PUSH(arena, bool, ir->node_count), *s = SF_ARENA_PUSH(arena, bool, ir->node_count);
    memset(v, 0, ir->node_count); memset(s, 0, ir->node_count); u32 c = 0;
    for (u32 i = 0; i < ir->node_count; ++i) if (ir->nodes[i].type != SF_NODE_UNKNOWN && !v[i]) if (!sort_recursive(ir, i, v, s, out_order, &c, diag)) return false;
    return true;
}
