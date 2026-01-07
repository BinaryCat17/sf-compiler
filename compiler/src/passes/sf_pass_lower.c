#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>
#include <stdio.h>

// --- Metadata Lookups ---

static sf_node_type get_node_type(const char* type_str) {
    if (!type_str) return SF_NODE_UNKNOWN;
    
    // 1. Check dynamic aliases from compiler_spec.json
    for (size_t i = 0; i < SF_COMPILER_ALIAS_COUNT; ++i) {
        if (strcmp(type_str, SF_COMPILER_ALIASES[i].from) == 0) return SF_COMPILER_ALIASES[i].to;
    }

    // 2. Check built-in node names
    for (int i = 1; i < SF_NODE_COUNT; ++i) {
        if (strcmp(type_str, SF_OP_METADATA[i].name) == 0) return (sf_node_type)i;
    }
    return SF_NODE_UNKNOWN;
}

static u32 get_port_index(sf_node_type type, const char* port_name) {
    if (!port_name || type >= SF_NODE_COUNT) return 0;
    const sf_op_metadata* meta = &SF_OP_METADATA[type];
    for (u32 i = 0; i < 4; ++i) {
        if (meta->ports[i] && strcmp(meta->ports[i], port_name) == 0) return i;
    }
    return 0;
}

// --- Helpers ---

static const sf_lowering_rule* find_lowering_rule(sf_node_type type) {
    for (size_t i = 0; i < SF_LOWERING_RULE_COUNT; ++i) {
        if (SF_LOWERING_RULES[i].target_type == type) return &SF_LOWERING_RULES[i];
    }
    return NULL;
}

static void parse_const_tensor(const sf_json_value* val, const sf_json_value* node_data, sf_type_info* info, void** out_data, sf_arena* arena) {
    if (!val || !info || !out_data) return;

    // Default to scalar F32 if no metadata provided
    sf_dtype dtype = SF_DTYPE_F32;
    int32_t shape[SF_MAX_DIMS] = {0};
    uint8_t ndim = 0;

    const sf_json_value* meta = sf_json_get_field(node_data, "meta");
    if (meta) {
        const sf_json_value* j_dtype = sf_json_get_field(meta, "dtype");
        if (j_dtype) dtype = sf_dtype_from_str(j_dtype->as.s);

        const sf_json_value* j_shape = sf_json_get_field(meta, "shape");
        if (j_shape && j_shape->type == SF_JSON_VAL_ARRAY) {
            ndim = (uint8_t)j_shape->as.array.count;
            for (int i = 0; i < ndim; ++i) {
                shape[i] = (int32_t)j_shape->as.array.items[i].as.n;
            }
        }
    } else if (val->type == SF_JSON_VAL_ARRAY) {
        ndim = 1;
        shape[0] = (int32_t)val->as.array.count;
    }

    sf_type_info_init_contiguous(info, dtype, shape, ndim);
    size_t count = sf_shape_calc_count(shape, ndim);
    size_t bytes = count * sf_dtype_size(dtype);
    
    *out_data = SF_ARENA_PUSH(arena, uint8_t, bytes);
    
    if (val->type == SF_JSON_VAL_ARRAY) {
        for (size_t i = 0; i < val->as.array.count && i < count; ++i) {
            sf_json_value* item = &val->as.array.items[i];
            if (dtype == SF_DTYPE_F32) ((f32*)*out_data)[i] = (f32)item->as.n;
            else if (dtype == SF_DTYPE_I32) ((i32*)*out_data)[i] = (i32)item->as.n;
            else if (dtype == SF_DTYPE_U8) ((u8*)*out_data)[i] = (u8)item->as.n;
        }
    } else if (val->type == SF_JSON_VAL_NUMBER) {
        if (dtype == SF_DTYPE_F32) ((f32*)*out_data)[0] = (f32)val->as.n;
        else if (dtype == SF_DTYPE_I32) ((i32*)*out_data)[0] = (i32)val->as.n;
        else if (dtype == SF_DTYPE_U8) ((u8*)*out_data)[0] = (u8)val->as.n;
    }
}

static bool lower_node(const sf_json_value* node_val, sf_ir_node* ir_node, sf_arena* arena, sf_compiler_diag* diag) {
    // ... search for usage of constant ...
    // I need to read more of this file to find where parse_const_tensor is called.
}

static bool is_field_allowed(sf_node_type type, const char* key) {
    if (type == SF_NODE_INPUT || type == SF_NODE_OUTPUT) {
        const char* allowed[] = {"shape", "dtype", "readonly", "persistent", "screen_size", "output", "name", NULL};
        for (int i = 0; allowed[i]; ++i) if (strcmp(key, allowed[i]) == 0) return true;
    } else if (type == SF_NODE_INDEX_X || type == SF_NODE_INDEX_Y || type == SF_NODE_INDEX_Z) {
        const char* allowed[] = {"axis", "dtype", NULL};
        for (int i = 0; allowed[i]; ++i) if (strcmp(key, allowed[i]) == 0) return true;
    } else if (type == SF_NODE_CONST) {
        const char* allowed[] = {"value", "meta", NULL};
        for (int i = 0; allowed[i]; ++i) if (strcmp(key, allowed[i]) == 0) return true;
    } else if (type == SF_NODE_CALL) {
        const char* allowed[] = {"path", NULL};
        for (int i = 0; allowed[i]; ++i) if (strcmp(key, allowed[i]) == 0) return true;
    } else if (type == SF_NODE_RANGE) {
        const char* allowed[] = {"dtype", NULL};
        for (int i = 0; allowed[i]; ++i) if (strcmp(key, allowed[i]) == 0) return true;
    }
    return false;
}

static bool parse_node_attributes(sf_ir_node* dst, const sf_json_value* data, const char* base_path, sf_arena* arena, sf_compiler_diag* diag) {
    if (!data) return true;

    // Strict Validation: Fail on any unknown field
    if (data->type == SF_JSON_VAL_OBJECT) {
        for (size_t i = 0; i < data->as.object.count; ++i) {
            if (!is_field_allowed(dst->type, data->as.object.keys[i])) {
                sf_compiler_diag_report(diag, dst->loc, "Unknown or unsupported parameter '%s' for node type '%s'", 
                    data->as.object.keys[i], SF_OP_METADATA[dst->type].name);
                return false;
            }
        }
    }

    switch (dst->type) {
        case SF_NODE_INPUT:
        case SF_NODE_OUTPUT: {
            const sf_json_value* v_shape = sf_json_get_field(data, "shape");
            const sf_json_value* v_dtype = sf_json_get_field(data, "dtype");
            const sf_json_value* v_readonly = sf_json_get_field(data, "readonly");
            const sf_json_value* v_persistent = sf_json_get_field(data, "persistent");
            const sf_json_value* v_screen_size = sf_json_get_field(data, "screen_size");
            const sf_json_value* v_output = sf_json_get_field(data, "output");
            
            if (v_readonly && v_readonly->type == SF_JSON_VAL_BOOL && v_readonly->as.b) {
                dst->resource_flags |= SF_RESOURCE_FLAG_READONLY;
            }
            if (v_persistent && v_persistent->type == SF_JSON_VAL_BOOL && v_persistent->as.b) {
                dst->resource_flags |= SF_RESOURCE_FLAG_PERSISTENT;
            }
            if (v_screen_size && v_screen_size->type == SF_JSON_VAL_BOOL && v_screen_size->as.b) {
                dst->resource_flags |= SF_RESOURCE_FLAG_SCREEN_SIZE;
            }
            if (v_output && v_output->type == SF_JSON_VAL_BOOL && v_output->as.b) {
                dst->resource_flags |= SF_RESOURCE_FLAG_OUTPUT;
            }

            if (dst->type == SF_NODE_INPUT && (!v_shape || v_shape->type != SF_JSON_VAL_ARRAY)) {
                sf_compiler_diag_report(diag, dst->loc, "Input node '%s': missing or invalid 'shape'", dst->id);
                return false;
            }
            
            dst->const_info.dtype = v_dtype ? sf_dtype_from_str(v_dtype->as.s) : SF_DTYPE_F32;
            
            if (v_shape && v_shape->type == SF_JSON_VAL_ARRAY) {
                dst->const_info.ndim = (uint8_t)v_shape->as.array.count;
                if (dst->const_info.ndim > SF_MAX_DIMS) dst->const_info.ndim = SF_MAX_DIMS;
                for(int k=0; k<dst->const_info.ndim; ++k) {
                     const sf_json_value* dim = &v_shape->as.array.items[k];
                     if (dim->type == SF_JSON_VAL_NUMBER) {
                         int d = (int)dim->as.n;
                         dst->const_info.shape[k] = (d < 0) ? -1 : d;
                     }
                }
                sf_shape_calc_strides(&dst->const_info);
                // For Input and Output nodes, we copy this to out_info initially
                if (dst->type == SF_NODE_INPUT || dst->type == SF_NODE_OUTPUT) {
                    dst->out_info = dst->const_info;
                }
            }
            break;
        }
        case SF_NODE_INDEX_X:
        case SF_NODE_INDEX_Y:
        case SF_NODE_INDEX_Z: {
            const sf_json_value* v_dtype = sf_json_get_field(data, "dtype");
            dst->out_info.dtype = v_dtype ? sf_dtype_from_str(v_dtype->as.s) : SF_DTYPE_F32;

            const sf_json_value* v_axis = sf_json_get_field(data, "axis");
            int axis = (v_axis && v_axis->type == SF_JSON_VAL_NUMBER) ? (int)v_axis->as.n : 0;
            
            if (axis == 1) {
                dst->type = SF_NODE_INDEX_Y;
            } else if (axis == 2) {
                dst->type = SF_NODE_INDEX_Z;
            } else {
                dst->type = SF_NODE_INDEX_X;
            }

            dst->out_info.ndim = 0; // Scalar per element, will expand to domain in analyze pass
            break;
        }
        case SF_NODE_CONST: {
            const sf_json_value* v_val = sf_json_get_field(data, "value");
            if (v_val) {
                parse_const_tensor(v_val, data, &dst->const_info, &dst->const_data, arena);
                dst->out_info = dst->const_info;
            }
            break;
        }
        case SF_NODE_CALL: {
            const sf_json_value* v_path = sf_json_get_field(data, "path");
            if (v_path && v_path->type == SF_JSON_VAL_STRING) {
                if (base_path) {
                    char* dir = sf_path_get_dir(base_path, arena);
                    dst->sub_graph_path = sf_path_join(dir, v_path->as.s, arena);
                } else {
                    dst->sub_graph_path = sf_arena_strdup(arena, v_path->as.s);
                }
            }
            break;
        }
        default: break;
    }
    return true;
}

// --- Main Pass ---

bool sf_pass_lower(sf_ast_graph* ast, sf_graph_ir* out_ir, sf_arena* arena, const char* base_path, sf_compiler_diag* diag) {
    if (!ast) {
        SF_REPORT(diag, NULL, "Lowering Pass: AST is NULL");
        return false;
    }

    memset(out_ir, 0, sizeof(sf_graph_ir));

    // --- Process Root App Settings (Cartridge Metadata) ---
    sf_ir_parse_window_settings(ast->root, out_ir);

    size_t cap = ast->node_count + 64; // Reserve space for decompositions
    out_ir->node_count = ast->node_count;
    out_ir->node_cap = cap;
    out_ir->nodes = SF_ARENA_PUSH(arena, sf_ir_node, cap);
    memset(out_ir->nodes, 0, sizeof(sf_ir_node) * cap);

    sf_str_map map;
    sf_map_init(&map, ast->node_count * 2, arena);

    // 1. Process Nodes
    for (size_t i = 0; i < ast->node_count; ++i) {
        sf_ast_node* src = &ast->nodes[i];
        sf_ir_node* dst = &out_ir->nodes[i];

        dst->loc.file = base_path ? sf_arena_strdup(arena, base_path) : "unknown";
        dst->loc.line = src->loc.line;
        dst->loc.column = src->loc.column;

        dst->id = sf_arena_strdup(arena, src->id);
        sf_map_put(&map, dst->id, i);

        dst->type = get_node_type(src->type);
        if (dst->type == SF_NODE_UNKNOWN) {
            // Explicit Import System: Search for <type>.json in imports
            bool found = false;
            
            // 1. Search in local imports
            for (size_t k = 0; k < ast->import_count; ++k) {
                const char* import_path = ast->imports[k];
                char* full_path;
                if (base_path) {
                    char* dir = sf_path_get_dir(base_path, arena);
                    full_path = sf_path_join(dir, import_path, arena);
                } else {
                    full_path = (char*)import_path;
                }
                
                char* file_path = sf_arena_sprintf(arena, "%s/%s.json", full_path, src->type);
                if (sf_file_exists(file_path)) {
                    dst->type = SF_NODE_CALL;
                    dst->sub_graph_path = file_path;
                    found = true;
                    break;
                }
            }
            
            // 2. Search in Global Prelude (assets/lib)
            if (!found) {
                char* file_path = sf_arena_sprintf(arena, "assets/lib/%s.json", src->type);
                if (sf_file_exists(file_path)) {
                    dst->type = SF_NODE_CALL;
                    dst->sub_graph_path = file_path;
                    found = true;
                }
            }

            if (!found) {
                sf_compiler_diag_report(diag, dst->loc, "Unknown node type '%s' (not a built-in and not found in imports)", src->type);
                return false;
            }
        }

        if (!parse_node_attributes(dst, src->data, base_path, arena, diag)) return false;
    }

    // 2. Process Domains (Must happen after all nodes are in the map)
    for (size_t i = 0; i < ast->node_count; ++i) {
        sf_ast_node* src = &ast->nodes[i];
        sf_ir_node* dst = &out_ir->nodes[i];
        dst->domain_node_idx = UINT32_MAX; // Default

        if (src->data) {
            const sf_json_value* v_domain = sf_json_get_field(src->data, "domain");
            if (v_domain && v_domain->type == SF_JSON_VAL_STRING) {
                u32 dom_idx;
                if (sf_map_get(&map, v_domain->as.s, &dom_idx)) {
                    dst->domain_node_idx = dom_idx;
                } else {
                    sf_compiler_diag_report(diag, dst->loc, "Domain node '%s' not found for node '%s'", v_domain->as.s, dst->id);
                    return false;
                }
            }
        }
    }

    // 3. Process Links
    size_t link_cap = ast->link_count + 128; // Reserve space for internal links
    out_ir->link_count = ast->link_count;
    out_ir->link_cap = link_cap;
    out_ir->links = SF_ARENA_PUSH(arena, sf_ir_link, link_cap);

    for (size_t i = 0; i < ast->link_count; ++i) {
        sf_ast_link* l_src = &ast->links[i];
        sf_ir_link* l_dst = &out_ir->links[i];
        
        // Source
        if (!sf_map_get(&map, l_src->src, &l_dst->src_node_idx)) {
            sf_source_loc loc = {base_path, l_src->loc.line, l_src->loc.column};
            sf_compiler_diag_report(diag, loc, "Link source '%s' not found", l_src->src);
            return false;
        }
        sf_ir_node* src_node = &out_ir->nodes[l_dst->src_node_idx];
        l_dst->src_port_name = l_src->src_port ? sf_arena_strdup(arena, l_src->src_port) : "out";
        if (src_node->type != SF_NODE_CALL) {
            l_dst->src_port = get_port_index(src_node->type, l_src->src_port);
        }

        // Dest
        if (!sf_map_get(&map, l_src->dst, &l_dst->dst_node_idx)) {
            sf_source_loc loc = {base_path, l_src->loc.line, l_src->loc.column};
            sf_compiler_diag_report(diag, loc, "Link dst '%s' not found", l_src->dst);
            return false;
        }
        sf_ir_node* dst_node = &out_ir->nodes[l_dst->dst_node_idx];
        l_dst->dst_port_name = l_src->dst_port ? sf_arena_strdup(arena, l_src->dst_port) : "in";
        if (dst_node->type != SF_NODE_CALL) {
            l_dst->dst_port = get_port_index(dst_node->type, l_src->dst_port);
        }
    }

    return true;
}