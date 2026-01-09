#include <sionflow/compiler/sf_compiler.h>
#include "sf_passes.h"
#include "sf_compiler_internal.h"
#include <sionflow/base/sf_json.h>
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_log.h>
#include <string.h>
#include <stdlib.h>

bool sf_compiler_load_manifest(const char* path, sf_compiler_manifest* out_manifest, sf_arena* arena) {
    if (!path || !out_manifest) return false;
    memset(out_manifest, 0, sizeof(sf_compiler_manifest));

    char* json_str = sf_file_read(path, arena);
    if (!json_str) return false;

    sf_json_value* root = sf_json_parse(json_str, arena);
    if (!root || root->type != SF_JSON_VAL_OBJECT) return false;

    sf_ir_parse_window_settings(root, &out_manifest->app_ir);
    char* base_dir = sf_path_get_dir(path, arena);

    // 1. Kernels
    const sf_json_value* pipeline = sf_json_get_field(root, "pipeline");
    const sf_json_value* runtime = sf_json_get_field(root, "runtime");

    // Case A: Simple single-kernel app (runtime.entry)
    if (runtime && runtime->type == SF_JSON_VAL_OBJECT && !pipeline) {
        const sf_json_value* entry = sf_json_get_field(runtime, "entry");
        if (entry && entry->type == SF_JSON_VAL_STRING) {
            out_manifest->kernel_count = 1;
            out_manifest->kernels = SF_ARENA_PUSH(arena, sf_compiler_kernel_desc, 1);
            out_manifest->kernels[0].id = "main";
            out_manifest->kernels[0].path = sf_path_join(base_dir, entry->as.s, arena);
        }
    }

    // Case B: Multi-kernel pipeline
    if (pipeline && pipeline->type == SF_JSON_VAL_OBJECT) {
        const sf_json_value* kernels = sf_json_get_field(pipeline, "kernels");
        if (kernels && kernels->type == SF_JSON_VAL_ARRAY) {
            out_manifest->kernel_count = (u32)kernels->as.array.count;
            out_manifest->kernels = SF_ARENA_PUSH(arena, sf_compiler_kernel_desc, out_manifest->kernel_count);
            for (size_t i = 0; i < kernels->as.array.count; ++i) {
                const sf_json_value* k = &kernels->as.array.items[i];
                const sf_json_value* id = sf_json_get_field(k, "id");
                const sf_json_value* entry = sf_json_get_field(k, "entry");
                out_manifest->kernels[i].id = id ? id->as.s : "kernel";
                out_manifest->kernels[i].path = entry ? sf_path_join(base_dir, entry->as.s, arena) : NULL;
            }
        }
    }

    // Case C: Raw Graph (no pipeline/runtime, but has nodes)
    if (out_manifest->kernel_count == 0) {
        const sf_json_value* nodes = sf_json_get_field(root, "nodes");
        if (nodes && nodes->type == SF_JSON_VAL_ARRAY) {
            out_manifest->kernel_count = 1;
            out_manifest->kernels = SF_ARENA_PUSH(arena, sf_compiler_kernel_desc, 1);
            out_manifest->kernels[0].id = "main";
            out_manifest->kernels[0].path = sf_arena_strdup(arena, path);
        }
    }

    // 2. Assets
    const sf_json_value* assets = sf_json_get_field(root, "assets");
    if (assets && assets->type == SF_JSON_VAL_ARRAY) {
        out_manifest->asset_count = (u32)assets->as.array.count;
        out_manifest->assets = SF_ARENA_PUSH(arena, sf_compiler_asset_desc, out_manifest->asset_count);
        for (size_t i = 0; i < assets->as.array.count; ++i) {
            const sf_json_value* a = &assets->as.array.items[i];
            const sf_json_value* name = sf_json_get_field(a, "name");
            const sf_json_value* a_path = sf_json_get_field(a, "path");
            const sf_json_value* type = sf_json_get_field(a, "type");
            
            out_manifest->assets[i].name = name ? name->as.s : "asset";
            out_manifest->assets[i].path = a_path ? sf_path_join(base_dir, a_path->as.s, arena) : NULL;
            
            if (type && type->type == SF_JSON_VAL_STRING) {
                if (strcmp(type->as.s, "image") == 0) out_manifest->assets[i].type = SF_SECTION_IMAGE;
                else if (strcmp(type->as.s, "font") == 0) out_manifest->assets[i].type = SF_SECTION_FONT;
                else out_manifest->assets[i].type = SF_SECTION_RAW;
            } else {
                out_manifest->assets[i].type = SF_SECTION_RAW;
            }
        }
    }

    // Store the raw JSON for the PIPELINE section
    out_manifest->raw_json = json_str;
    out_manifest->raw_json_size = (u32)strlen(json_str);

    return true;
}
