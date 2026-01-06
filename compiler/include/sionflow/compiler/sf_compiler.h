#ifndef SF_COMPILER_H
#define SF_COMPILER_H

#include <sionflow/base/sf_types.h>
#include <sionflow/isa/sf_program.h>
#include <sionflow/base/sf_memory.h>

#include <sionflow/isa/sf_opcodes.h>
#include <sionflow/isa/sf_op_defs.h>

// --- Diagnostics ---
typedef struct {
    sf_source_loc loc;
    char message[256];
} sf_compiler_error;

typedef struct {
    sf_compiler_error* errors;
    uint32_t error_count;
    uint32_t error_capacity;
    bool has_error;
} sf_compiler_diag;

void sf_compiler_diag_init(sf_compiler_diag* diag, sf_arena* arena);
void sf_compiler_diag_report(sf_compiler_diag* diag, sf_source_loc loc, const char* fmt, ...);

// --- IR Definitions ---

typedef struct sf_json_value sf_json_value;

// Node metadata and opcodes are provided by SionFlow ISA
#include <sionflow/isa/sf_opcodes.h>

// --- Fusion Rules ---
typedef struct {
    const char* port_name;
    sf_node_type match_type;
    u8 max_use_count;
    const char* remap_to_port;
} sf_fusion_match;

typedef struct {
    sf_node_type target_type;
    sf_node_type replace_with;
    sf_fusion_match matches[2]; // Simplified to 2 for now
    u8 match_count;
} sf_fusion_rule;

extern const sf_fusion_rule SF_FUSION_RULES[];
extern const size_t SF_FUSION_RULE_COUNT;

typedef struct {
    const char* from;
    sf_node_type to;
} sf_compiler_alias;

extern const sf_compiler_alias SF_COMPILER_ALIASES[];
extern const size_t SF_COMPILER_ALIAS_COUNT;

typedef struct {
    const char* id;
    sf_node_type type;
    const char* input_map[4]; // maps subgraph port to local node id
} sf_lowering_step;

typedef struct {
    sf_node_type target_type;
    const sf_lowering_step* steps;
    u32 step_count;
    const char* output_node_id;
} sf_lowering_rule;

extern const sf_lowering_rule SF_LOWERING_RULES[];
extern const size_t SF_LOWERING_RULE_COUNT;

typedef struct {
    const char* id; 
    sf_node_type type;
    
    // Constant Data (valid if type == SF_NODE_CONST)
    sf_type_info const_info;
    void* const_data; 

    // Sub-Graph Data
    const char* sub_graph_path; // For SF_NODE_CALL

    // Debug Info
    sf_source_loc loc;

    // Compiler Generated info
    u16 out_reg_idx;    // Index in the global Tensor Pool
    u32 domain_node_idx; // Index of the node that defines the domain for this node
    sf_type_info out_info; // Predicted output shape and dtype
    bool is_spatial;     // Explicitly tracked spatial status
    uint8_t resource_flags; // SF_RESOURCE_FLAG_*
} sf_ir_node;

typedef struct {
    u32 src_node_idx; 
    u32 src_port;
    const char* src_port_name;

    u32 dst_node_idx; 
    u32 dst_port;
    const char* dst_port_name;
} sf_ir_link;

typedef struct {
    sf_ir_node* nodes;
    size_t node_count;
    size_t node_cap;
    
    sf_ir_link* links;
    size_t link_count;
    size_t link_cap;

    // App Settings (Cartridge Metadata)
    char app_title[SF_MAX_TITLE_NAME];
    u32 window_width;
    u32 window_height;
    u32 num_threads;
    u8 vsync;
    u8 fullscreen;
    u8 resizable;
} sf_graph_ir;

// --- Manifest Interface ---

typedef struct sf_json_value sf_json_value;
void sf_ir_parse_window_settings(const sf_json_value* root, sf_graph_ir* out_ir);

typedef struct {
    const char* id;
    const char* path;
} sf_compiler_kernel_desc;

typedef struct {
    const char* name;
    const char* path;
    uint32_t type; // sf_section_type
} sf_compiler_asset_desc;

typedef struct {
    sf_graph_ir app_ir;
    sf_compiler_kernel_desc* kernels;
    u32 kernel_count;
    sf_compiler_asset_desc* assets;
    u32 asset_count;
    const char* raw_json;
    u32 raw_json_size;
} sf_compiler_manifest;

bool sf_compiler_load_manifest(const char* path, sf_compiler_manifest* out_manifest, sf_arena* arena);

// --- Compiler Interface ---

// 1. Parse JSON -> IR
bool sf_compile_load_json(const char* json_path, sf_graph_ir* out_ir, sf_arena* arena, sf_compiler_diag* diag);

// 2. IR -> Program (Autonomous Compilation)
sf_program* sf_compile(sf_graph_ir* ir, sf_arena* arena, sf_compiler_diag* diag);

// 3. Save Program
bool sf_compile_save_program(const sf_program* prog, const char* path);

// 4. Save Cartridge (Container for multiple programs and assets)
typedef struct {
    const char* name;
    uint32_t type; // sf_section_type
    const void* data;
    uint32_t size;
} sf_section_desc;

bool sf_compile_save_cartridge(const char* path, const sf_graph_ir* ir, const sf_section_desc* sections, u32 section_count);

#endif // SF_COMPILER_H
