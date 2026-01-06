#include "sf_compiler_internal.h"
#include "sf_passes.h"
#include <sionflow/base/sf_json.h>
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_log.h>
#include <string.h>

bool sf_compile_load_json_ir(const char* json_path, sf_graph_ir* out_ir, sf_arena* arena, sf_compiler_diag* diag) {
    char* json_content = sf_file_read(json_path, arena);
    if (!json_content) {
        sf_source_loc loc = {json_path, 0, 0};
        sf_compiler_diag_report(diag, loc, "Could not read file");
        return false;
    }

    // 1. Parse JSON -> AST (Source Tracking)
    sf_ast_graph* ast = sf_json_parse_graph(json_content, arena);
    if (!ast) {
        sf_source_loc loc = {json_path, 0, 0};
        sf_compiler_diag_report(diag, loc, "Failed to parse JSON AST");
        return false;
    }

    // 2. Lower AST -> IR (Validation & Type resolution)
    if (!sf_pass_lower(ast, out_ir, arena, json_path, diag)) {
        return false;
    }

    return true;
}

// Main Entry Point exposed in sf_compiler.h
bool sf_compile_load_json(const char* json_path, sf_graph_ir* out_ir, sf_arena* arena, sf_compiler_diag* diag) {
    // 1. Load the Root Graph (Raw IR)
    if (!sf_compile_load_json_ir(json_path, out_ir, arena, diag)) {
        return false;
    }

    // 2. Run the Inline Pass (Expand Subgraphs recursively)
    if (!sf_pass_inline(out_ir, arena, diag)) {
        return false;
    }

    return true;
}