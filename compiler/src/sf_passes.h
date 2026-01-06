#ifndef SF_PASSES_H
#define SF_PASSES_H

#include <sionflow/base/sf_json.h>
#include <sionflow/compiler/sf_compiler.h>
#include <sionflow/base/sf_memory.h>

#define SF_REPORT(diag, loc_ptr, msg, ...) \
    do { \
        sf_source_loc _loc = {0}; \
        if (loc_ptr) _loc = *((const sf_source_loc*)(loc_ptr)); \
        sf_compiler_diag_report(diag, _loc, msg, ##__VA_ARGS__); \
    } while(0)

#define SF_REPORT_NODE(diag, node, msg, ...) \
    sf_compiler_diag_report(diag, (node)->loc, msg, ##__VA_ARGS__)

// --- Pass: AST -> IR (Lowering & Validation) ---
// Converts the AST into the Semantic Graph IR.
// - Resolves Node Types and Enums
// - Validates Data schemas
// - Resolves Port Names to Indices

typedef struct {
    sf_graph_ir* ir;
    sf_arena* arena;
    const char* base_path; // For resolving sub-graphs
    
    // Analysis results shared between passes
    sf_ir_node** sorted_nodes;
    size_t sorted_count;
} sf_pass_ctx;

typedef bool (*sf_pass_fn)(sf_pass_ctx* ctx, sf_compiler_diag* diag);

typedef struct {
    const char* id;
    const char* name;
    sf_pass_fn func;
} sf_pipeline_pass_def;

// --- Pass: AST -> IR (Lowering & Validation) ---
// Converts the AST into the Semantic Graph IR.
// - Resolves Node Types and Enums
// - Validates Data schemas
// - Resolves Port Names to Indices
bool sf_pass_lower(sf_ast_graph* ast, sf_graph_ir* out_ir, sf_arena* arena, const char* base_path, sf_compiler_diag* diag);

// --- Pass: Inline Subgraphs ---
// Recursively expands SF_NODE_CALL into flattened nodes.
// Handles port remapping and unique ID generation.
bool sf_pass_inline(sf_graph_ir* ir, sf_arena* arena, sf_compiler_diag* diag);

// --- Pass Components (Internal) ---
bool sf_pass_decompose(sf_pass_ctx* ctx, sf_compiler_diag* diag);
bool sf_pass_sort(sf_pass_ctx* ctx, sf_compiler_diag* diag);
bool sf_pass_analyze(sf_pass_ctx* ctx, sf_compiler_diag* diag);
bool sf_pass_validate(sf_pass_ctx* ctx, sf_compiler_diag* diag);
bool sf_pass_domain_split(sf_pass_ctx* ctx, sf_compiler_diag* diag);
bool sf_pass_fuse(sf_pass_ctx* ctx, sf_compiler_diag* diag);
bool sf_pass_liveness(sf_pass_ctx* ctx, sf_compiler_diag* diag);

#endif // SF_PASSES_H
