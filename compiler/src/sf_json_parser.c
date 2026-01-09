#include "sf_passes.h"
#include "sf_compiler_internal.h"
#include <sionflow/base/sf_json.h>
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_log.h>
#include <string.h>

bool sf_compile_load_json_ir(const char* json_path, sf_graph_ir* out_ir, sf_arena* arena, sf_compiler_diag* diag, const char* base_path) {
    char* final_path = (char*)json_path;
    if (base_path && !sf_path_is_absolute(json_path)) {
        char* dir = sf_path_get_dir(base_path, arena);
        final_path = sf_path_join(dir, json_path, arena);
    }
    
    // Safety check: if file doesn't exist at final_path, but exists at json_path, use json_path
    if (base_path && !sf_file_exists(final_path) && sf_file_exists(json_path)) {
        final_path = (char*)json_path;
    }

    char* json_content = sf_file_read(final_path, arena);
    if (!json_content) {
        sf_source_loc loc = {sf_arena_strdup(arena, final_path), 0, 0};
        sf_compiler_diag_report(diag, loc, "Could not read file");
        return false;
    }

    // 1. Parse JSON -> AST (Source Tracking)
    sf_ast_graph* ast = sf_json_parse_graph(json_content, arena);
    if (!ast) {
        sf_source_loc loc = {sf_arena_strdup(arena, final_path), 0, 0};
        sf_compiler_diag_report(diag, loc, "Failed to parse JSON AST");
        return false;
    }

    // 2. Lower AST -> IR (Validation & Type resolution)
    if (!sf_pass_lower(ast, out_ir, arena, final_path, diag)) {
        return false;
    }

    return true;
}

// Main Entry Point exposed in sf_compiler.h

bool sf_compile_load_json(const char* json_path, sf_graph_ir* out_ir, sf_arena* arena, sf_compiler_diag* diag) {

    // 1. Load the Root Graph (Raw IR)
    if (!sf_compile_load_json_ir(json_path, out_ir, arena, diag, NULL)) {
        return false;
    }

    return true;
}
