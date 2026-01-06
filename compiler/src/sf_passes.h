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
} sf_pass_ctx;

bool sf_pass_lower(sf_ast_graph* ast, sf_graph_ir* out_ir, sf_arena* arena, const char* base_path, sf_compiler_diag* diag);

// --- Pass: Inline Subgraphs ---
// Recursively expands SF_NODE_CALL into flattened nodes.
// Handles port remapping and unique ID generation.
bool sf_pass_inline(sf_graph_ir* ir, sf_arena* arena, sf_compiler_diag* diag);

// Static Analysis (Types, Shapes, Strides)
bool sf_pass_analyze(sf_graph_ir* ir, sf_ir_node** sorted_nodes, size_t count, sf_compiler_diag* diag);

// --- Pass: Validation (Strict Consistency) ---
// Performs final structural and semantic checks before codegen.
// - Checks Identity compatibility (e.g. SPATIAL into UNIFORM)
// - Checks Domain consistency
bool sf_pass_validate(sf_graph_ir* ir, sf_ir_node** sorted_nodes, size_t count, sf_compiler_diag* diag);

// --- Pass: Domain Splitting ---
// Groups nodes into execution tasks based on their output shapes and dependencies.
bool sf_pass_domain_split(sf_graph_ir* ir, sf_compiler_diag* diag);

// --- Pass: Optimization (Instruction Fusion) ---
// Fuses (Mul + Add) into FMA instructions.
bool sf_pass_fuse(sf_graph_ir* ir, sf_compiler_diag* diag);

// --- Pass: Register Allocation (Liveness Analysis) ---
// Minimizes the number of registers by reusing them for non-overlapping lifetimes.
bool sf_pass_liveness(sf_graph_ir* ir, sf_ir_node** sorted, size_t count, sf_compiler_diag* diag);

#endif // SF_PASSES_H
