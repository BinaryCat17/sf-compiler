#include "../sf_passes.h"
#include "../sf_graph_utils.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>

/**
 * Professional Schema-Driven Lowering Pass.
 * Strict Explicit Imports: No more implicit searching.
 */

typedef bool (*sf_attr_handler)(sf_ir_node* dst, const sf_json_value* val, const char* base_path, sf_arena* arena, sf_compiler_diag* diag);

// --- Attribute Handlers ---

static bool handle_shape(sf_ir_node* dst, const sf_json_value* val, const char* bp, sf_arena* a, sf_compiler_diag* d) {
    (void)bp; (void)a; (void)d;
    if (val->type != SF_JSON_VAL_ARRAY) return false;
    dst->const_info.ndim = (uint8_t)val->as.array.count;
    for (int i = 0; i < dst->const_info.ndim && i < SF_MAX_DIMS; ++i) {
        dst->const_info.shape[i] = (int32_t)val->as.array.items[i].as.n;
    }
    sf_shape_calc_strides(&dst->const_info);
    if (dst->type == SF_NODE_INPUT || dst->type == SF_NODE_OUTPUT || dst->type == SF_NODE_CONST) dst->out_info = dst->const_info;
    return true;
}

static bool handle_dtype(sf_ir_node* dst, const sf_json_value* val, const char* bp, sf_arena* a, sf_compiler_diag* d) {
    (void)bp; (void)a; (void)d;
    if (val->type != SF_JSON_VAL_STRING) return false;
    dst->const_info.dtype = sf_dtype_from_str(val->as.s);
    dst->out_info.dtype = dst->const_info.dtype;
    return true;
}

static bool handle_flag_readonly(sf_ir_node* dst, const sf_json_value* val, const char* bp, sf_arena* a, sf_compiler_diag* d) {
    (void)bp; (void)a; (void)d;
    if (val->as.b) dst->resource_flags |= SF_RESOURCE_FLAG_READONLY;
    return true;
}

static bool handle_flag_persistent(sf_ir_node* dst, const sf_json_value* val, const char* bp, sf_arena* a, sf_compiler_diag* d) {
    (void)bp; (void)a; (void)d;
    if (val->as.b) dst->resource_flags |= SF_RESOURCE_FLAG_PERSISTENT;
    return true;
}

static bool handle_path(sf_ir_node* dst, const sf_json_value* val, const char* base_path, sf_arena* arena, sf_compiler_diag* diag) {
    (void)diag;
    if (val->type != SF_JSON_VAL_STRING) return false;
    if (base_path && !sf_path_is_absolute(val->as.s)) {
        char* dir = sf_path_get_dir(base_path, arena);
        dst->sub_graph_path = sf_path_join(dir, val->as.s, arena);
    } else {
        dst->sub_graph_path = sf_arena_strdup(arena, val->as.s);
    }
    return true;
}

static bool handle_axis(sf_ir_node* dst, const sf_json_value* val, const char* bp, sf_arena* a, sf_compiler_diag* d) {
    (void)bp; (void)a; (void)d;
    int axis = (int)val->as.n;
    if (axis == 1) dst->type = SF_NODE_INDEX_Y;
    else if (axis == 2) dst->type = SF_NODE_INDEX_Z;
    else dst->type = SF_NODE_INDEX_X;
    return true;
}

static bool handle_value(sf_ir_node* dst, const sf_json_value* val, const char* bp, sf_arena* arena, sf_compiler_diag* d) {
    (void)bp; (void)d;
    if (dst->const_info.ndim == 0 && val->type == SF_JSON_VAL_ARRAY) {
        dst->const_info.ndim = 1; dst->const_info.shape[0] = (int32_t)val->as.array.count;
        sf_shape_calc_strides(&dst->const_info); dst->out_info = dst->const_info;
    }
    size_t count = sf_shape_calc_count(dst->const_info.shape, dst->const_info.ndim);
    if (count == 0) count = 1;
    size_t bytes = count * sf_dtype_size(dst->const_info.dtype);
    dst->const_data = SF_ARENA_PUSH(arena, uint8_t, bytes);
    return sf_ir_parse_data(val, dst->const_info.dtype, count, dst->const_data);
}

typedef struct { const char* key; sf_attr_handler handler; } sf_attr_def;
static const sf_attr_def ATTR_DEFS[] = {
    { "shape", handle_shape }, { "dtype", handle_dtype }, { "readonly", handle_flag_readonly },
    { "persistent", handle_flag_persistent }, { "path", handle_path }, { "axis", handle_axis }, { "value", handle_value },
    { "meta", NULL }, { "domain", NULL }
};

static bool parse_node_attributes(sf_ir_node* dst, const sf_json_value* data, const char* base_path, sf_arena* arena, sf_compiler_diag* diag) {
    if (!data || data->type != SF_JSON_VAL_OBJECT) return true;
    for (size_t i = 0; i < data->as.object.count; ++i) {
        const char* key = data->as.object.keys[i];
        const sf_json_value* val = &data->as.object.values[i];
        bool found = false;
        for (size_t j = 0; j < sizeof(ATTR_DEFS)/sizeof(ATTR_DEFS[0]); ++j) {
            if (strcmp(key, ATTR_DEFS[j].key) == 0) {
                if (ATTR_DEFS[j].handler) if (!ATTR_DEFS[j].handler(dst, val, base_path, arena, diag)) return false;
                found = true; break;
            }
        }
        if (strcmp(key, "meta") == 0 && val->type == SF_JSON_VAL_OBJECT) { if (!parse_node_attributes(dst, val, base_path, arena, diag)) return false; found = true; }
        if (!found && strcmp(key, "domain") != 0 && strcmp(key, "output") != 0 && strcmp(key, "name") != 0) {
            sf_compiler_diag_report(diag, dst->loc, "Warning: Unknown attribute '%s' for node '%s'", key, dst->id);
        }
    }
    return true;
}

static const char* find_import_for_type(sf_ast_graph* ast, const char* type_name, const char* base_path, sf_arena* arena) {
    for (size_t i = 0; i < ast->import_count; ++i) {
        const char* path = ast->imports[i];
        char* name = sf_path_get_filename_no_ext(path, arena);
        if (strcmp(name, type_name) == 0) {
            if (sf_path_is_absolute(path)) return path;
            if (base_path) {
                char* dir = sf_path_get_dir(base_path, arena);
                return sf_path_join(dir, path, arena);
            }
            return path;
        }
    }
    return NULL;
}

bool sf_pass_lower(sf_ast_graph* ast, sf_graph_ir* out_ir, sf_arena* arena, const char* base_path, sf_compiler_diag* diag) {
    memset(out_ir, 0, sizeof(sf_graph_ir));
    sf_ir_parse_window_settings(ast->root, out_ir);
    size_t cap = ast->node_count + 128;
    out_ir->node_count = 0; out_ir->node_cap = cap;
    out_ir->nodes = SF_ARENA_PUSH(arena, sf_ir_node, cap);
    sf_str_map map; sf_map_init(&map, ast->node_count * 2, arena);

    for (size_t i = 0; i < ast->node_count; ++i) {
        sf_ast_node* src = &ast->nodes[i];
        sf_node_type type = sf_compiler_get_node_type(src->type);
        const char* sub_path = NULL;

        if (type == SF_NODE_UNKNOWN) {
            // Strict Explicit Imports Only
            sub_path = find_import_for_type(ast, src->type, base_path, arena);
            if (!sub_path) {
                sf_compiler_diag_report(diag, (sf_source_loc){base_path, src->loc.line, src->loc.column}, 
                    "Unknown type '%s'. This type is not in ISA and not found in 'imports'.", src->type);
                return false;
            }
            type = SF_NODE_CALL;
        }

        sf_ir_node* dst = sf_ir_node_add(out_ir, arena, src->id, type);
        dst->loc.file = base_path ? sf_arena_strdup(arena, base_path) : "unknown";
        dst->loc.line = src->loc.line; dst->loc.column = src->loc.column;
        dst->sub_graph_path = sub_path;
        
        // Initial type is unknown, will be resolved by Analyze pass
        dst->out_info.dtype = SF_DTYPE_UNKNOWN; 
        dst->const_info.dtype = SF_DTYPE_UNKNOWN;

        sf_map_put(&map, dst->id, (u32)(dst - out_ir->nodes));
        if (!parse_node_attributes(dst, src->data, base_path, arena, diag)) return false;
    }

    for (size_t i = 0; i < ast->node_count; ++i) {
        const sf_json_value* vd = sf_json_get_field(ast->nodes[i].data, "domain");
        if (vd && vd->type == SF_JSON_VAL_STRING) {
            u32 di; if (sf_map_get(&map, vd->as.s, &di)) out_ir->nodes[i].domain_node_idx = di;
        }
    }

    for (size_t i = 0; i < ast->link_count; ++i) {
        u32 si, di;
        if (sf_map_get(&map, ast->links[i].src, &si) && sf_map_get(&map, ast->links[i].dst, &di)) {
            sf_builder_connect(out_ir, arena, 
                (sf_port){ si, sf_compiler_get_port_index(out_ir->nodes[si].type, ast->links[i].src_port) },
                (sf_port){ di, sf_compiler_get_port_index(out_ir->nodes[di].type, ast->links[i].dst_port) });
        }
    }
    return true;
}