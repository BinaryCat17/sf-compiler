#include <sionflow/compiler/sf_compiler.h>
#include "sf_compiler_internal.h"
#include "sf_passes.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// Note: Logic moved to sub-modules (sf_json_parser.c, sf_semantics.c, sf_codegen.c)

// --- Diagnostics ---

void sf_compiler_diag_init(sf_compiler_diag* diag, sf_arena* arena) {
    memset(diag, 0, sizeof(sf_compiler_diag));
    diag->error_capacity = 32;
    diag->errors = SF_ARENA_PUSH(arena, sf_compiler_error, diag->error_capacity);
}

void sf_compiler_diag_report(sf_compiler_diag* diag, sf_source_loc loc, const char* fmt, ...) {
    if (!diag) return;
    diag->has_error = true;
    if (diag->error_count >= diag->error_capacity) return;

    sf_compiler_error* err = &diag->errors[diag->error_count++];
    err->loc = loc;

    va_list args;
    va_start(args, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);

    // Also log to console for immediate feedback during development
    SF_LOG_ERROR("%s:%u:%u: error: %s", loc.file ? loc.file : "unknown", loc.line, loc.column, err->message);
}

// --- Compilation ---

sf_program* sf_compile(sf_graph_ir* ir, sf_arena* arena, sf_compiler_diag* diag) {
    // 0. Optimizations
    if (!sf_pass_fuse(ir, diag)) {
        return NULL;
    }

    // 1. Sort
    size_t sorted_count = 0;
    sf_ir_node** sorted = sf_topo_sort(ir, arena, &sorted_count);
    if (!sorted) {
        sf_source_loc loc = {0};
        sf_compiler_diag_report(diag, loc, "Cycle detected in graph or sorting failed.");
        return NULL; 
    }

    // 2. Static Analysis (Types & Shapes)
    if (!sf_pass_analyze(ir, sorted, sorted_count, diag)) {
        return NULL;
    }

    // 2.5 Strict Architectural Validation
    if (!sf_pass_validate(ir, sorted, sorted_count, diag)) {
        return NULL;
    }

    // 2a. Register Allocation (Liveness Analysis)
    if (!sf_pass_liveness(ir, sorted, sorted_count, diag)) {
        return NULL;
    }

    // 2b. Domain Splitting (Multi-Domain Support)
    if (!sf_pass_domain_split(ir, diag)) {
        return NULL;
    }

    // 3. Allocate Program Structure
    sf_program* prog = SF_ARENA_PUSH(arena, sf_program, 1);
    memset(&prog->meta, 0, sizeof(sf_bin_header));

    // 4. Emit Code (Tensors, Instructions, State)
    if (!sf_codegen_emit(prog, ir, sorted, sorted_count, arena)) {
        sf_source_loc loc = {0};
        sf_compiler_diag_report(diag, loc, "Code generation failed.");
        return NULL;
    }
    
    return prog;
}

static size_t _write_program(const sf_program* prog, FILE* f) {
    size_t start = ftell(f);
    
    // 1. Header
    fwrite(&prog->meta, sizeof(sf_bin_header), 1, f);
    
    // 2. Code
    fwrite(prog->code, sizeof(sf_instruction), prog->meta.instruction_count, f);

    // 3. Symbol Table
    if (prog->meta.symbol_count > 0) {
        fwrite(prog->symbols, sizeof(sf_bin_symbol), prog->meta.symbol_count, f);
    }

    // 4. Tasks
    if (prog->meta.task_count > 0) {
        fwrite(prog->tasks, sizeof(sf_task), prog->meta.task_count, f);
    }

    // 4.5 Task Bindings
    if (prog->meta.binding_count > 0) {
        fwrite(prog->bindings, sizeof(sf_bin_task_binding), prog->meta.binding_count, f);
    }

    // 5. Tensor Metadata
    for (u32 i = 0; i < prog->meta.tensor_count; ++i) {
        sf_type_info* info = &prog->tensor_infos[i];
        sf_bin_tensor_desc desc = {0};
        desc.dtype = (u8)info->dtype;
        desc.ndim = info->ndim;
        desc.builtin_id = prog->builtin_ids[i];
        desc.builtin_axis = prog->builtin_axes[i];
        desc.flags = prog->tensor_flags[i];
        void* data_ptr = prog->tensor_data[i];
        desc.is_constant = (data_ptr != NULL);
        if (info->ndim > 0) {
            memcpy(desc.shape, info->shape, sizeof(i32) * info->ndim);
        }
        
        if (desc.is_constant) {
            desc.data_size = sf_shape_calc_bytes(info->dtype, info->shape, info->ndim);
        }
        
        fwrite(&desc, sizeof(sf_bin_tensor_desc), 1, f);
    }

    // 5. Tensor Data Blob
    for (u32 i = 0; i < prog->meta.tensor_count; ++i) {
        void* data_ptr = prog->tensor_data[i];
        if (data_ptr) {
            sf_type_info* info = &prog->tensor_infos[i];
            size_t sz = sf_shape_calc_bytes(info->dtype, info->shape, info->ndim);
            fwrite(data_ptr, 1, sz, f);
        }
    }

    return ftell(f) - start;
}

bool sf_compile_save_program(const sf_program* prog, const char* path) {
    sf_section_desc desc = { "main", SF_SECTION_PROGRAM, prog, 0 };
    return sf_compile_save_cartridge(path, NULL, &desc, 1);
}

bool sf_compile_save_cartridge(const char* path, const sf_graph_ir* ir, const sf_section_desc* sections, u32 section_count) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    sf_cartridge_header cart = {0};
    cart.magic = SF_BINARY_MAGIC;
    cart.version = SF_BINARY_VERSION;
    
    if (ir) {
        strncpy(cart.app_title, ir->app_title, SF_MAX_TITLE_NAME - 1);
        cart.window_width = ir->window_width;
        cart.window_height = ir->window_height;
        cart.num_threads = ir->num_threads;
        cart.vsync = ir->vsync;
        cart.fullscreen = ir->fullscreen;
        cart.resizable = ir->resizable;
    } else {
        strncpy(cart.app_title, "SionFlow Cartridge", SF_MAX_TITLE_NAME - 1);
        cart.window_width = 800;
        cart.window_height = 600;
        cart.resizable = 1;
    }

    cart.section_count = section_count;
    for (u32 i = 0; i < section_count; ++i) {
        strncpy(cart.sections[i].name, sections[i].name, SF_MAX_SYMBOL_NAME - 1);
        cart.sections[i].type = sections[i].type;
    }

    // Write header placeholder
    fwrite(&cart, sizeof(sf_cartridge_header), 1, f);

    for (u32 i = 0; i < section_count; ++i) {
        cart.sections[i].offset = (uint32_t)ftell(f);
        if (sections[i].type == SF_SECTION_PROGRAM) {
            cart.sections[i].size = (uint32_t)_write_program((const sf_program*)sections[i].data, f);
        } else {
            fwrite(sections[i].data, 1, sections[i].size, f);
            cart.sections[i].size = sections[i].size;
        }
    }

    // Rewrite header with correct offsets and sizes
    fseek(f, 0, SEEK_SET);
    fwrite(&cart, sizeof(sf_cartridge_header), 1, f);

    fclose(f);
    return true;
}
