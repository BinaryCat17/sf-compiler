#include <sionflow/compiler/sf_compiler.h>
#include "sf_compiler_internal.h"
#include "sf_passes.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// --- Diagnostics ---

void sf_compiler_diag_init(sf_compiler_diag* diag, sf_arena* arena) {
    memset(diag, 0, sizeof(sf_compiler_diag));
    diag->error_capacity = 32;
    diag->errors = SF_ARENA_PUSH(arena, sf_compiler_error, diag->error_capacity);
}

void sf_compiler_diag_report(sf_compiler_diag* diag, sf_source_loc loc, const char* fmt, ...) {
    if (!diag) return;
    diag->has_error = true;
    if (diag->error_count >= diag->error_capacity) {
        if (diag->error_count == diag->error_capacity) {
            SF_LOG_ERROR("Error capacity reached, suppressing further errors.");
            diag->error_count++;
        }
        return;
    }

    sf_compiler_error* err = &diag->errors[diag->error_count++];
    err->loc = loc;

    va_list args;
    va_start(args, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);

    // Also log to console for immediate feedback during development
    const char* file = loc.file ? loc.file : "unknown";
    if (loc.line > 0) {
        SF_LOG_ERROR("%s:%u:%u: error: %s", file, loc.line, loc.column, err->message);
    } else {
        SF_LOG_ERROR("%s: error: %s", file, err->message);
    }
}

#include <stdarg.h>

// Forward declarations of generated pipeline
extern const sf_pipeline_pass_def SF_COMPILER_PIPELINE[];
extern const size_t SF_COMPILER_PIPELINE_COUNT;

// --- Compilation ---

sf_program* sf_compile(sf_graph_ir* ir, sf_arena* arena, sf_compiler_diag* diag) {
    sf_pass_ctx ctx = {0};
    ctx.ir = ir;
    ctx.arena = arena;

    // Execute Declarative Pipeline
    for (size_t i = 0; i < SF_COMPILER_PIPELINE_COUNT; ++i) {
        const sf_pipeline_pass_def* pass = &SF_COMPILER_PIPELINE[i];
        SF_LOG_DEBUG("Running pass: %s", pass->name);
        
        if (!pass->func(&ctx, diag)) {
            SF_LOG_ERROR("Pass '%s' failed", pass->name);
            return NULL;
        }
    }

    // 3. Allocate Program Structure
    sf_program* prog = SF_ARENA_PUSH(arena, sf_program, 1);
    memset(&prog->meta, 0, sizeof(sf_bin_header));

    // 4. Emit Code (Tensors, Instructions, State)
    if (!sf_codegen_emit(prog, ir, ctx.sorted_nodes, ctx.sorted_count, arena)) {
        sf_source_loc loc = {0};
        sf_compiler_diag_report(diag, loc, "Code generation failed.");
        return NULL;
    }
    
    return prog;
}

static bool _safe_write(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (size == 0 || nmemb == 0) return true;
    size_t written = fwrite(ptr, size, nmemb, stream);
    if (written != nmemb) {
        SF_LOG_ERROR("File write failed: wrote %zu of %zu elements", written, nmemb);
        return false;
    }
    return true;
}

static size_t _write_program(const sf_program* prog, FILE* f) {
    size_t start = ftell(f);
    
    // 1. Header
    if (!_safe_write(&prog->meta, sizeof(sf_bin_header), 1, f)) return 0;
    
    // 2. Code
    if (!_safe_write(prog->code, sizeof(sf_instruction), prog->meta.instruction_count, f)) return 0;

    // 3. Symbol Table
    if (prog->meta.symbol_count > 0) {
        if (!_safe_write(prog->symbols, sizeof(sf_bin_symbol), prog->meta.symbol_count, f)) return 0;
    }

    // 4. Tasks
    if (prog->meta.task_count > 0) {
        if (!_safe_write(prog->tasks, sizeof(sf_task), prog->meta.task_count, f)) return 0;
    }

    // 4.5 Task Bindings
    if (prog->meta.binding_count > 0) {
        if (!_safe_write(prog->bindings, sizeof(sf_bin_task_binding), prog->meta.binding_count, f)) return 0;
    }

    // 5. Tensor Metadata
    for (u32 i = 0; i < prog->meta.tensor_count; ++i) {
        sf_type_info* info = &prog->tensor_infos[i];
        sf_bin_tensor_desc desc = {0};
        desc.dtype = (u8)info->dtype;
        desc.ndim = info->ndim;
        desc.flags = prog->tensor_flags[i];
        void* data_ptr = prog->tensor_data[i];
        desc.is_constant = (data_ptr != NULL);
        if (info->ndim > 0) {
            memcpy(desc.shape, info->shape, sizeof(i32) * info->ndim);
        }
        
        if (desc.is_constant) {
            desc.data_size = sf_shape_calc_bytes(info->dtype, info->shape, info->ndim);
        }
        
        if (!_safe_write(&desc, sizeof(sf_bin_tensor_desc), 1, f)) return 0;
    }

    // 6. Push Constant Block
    if (prog->meta.push_constants_size > 0) {
        if (!_safe_write(prog->push_constants_data, 1, prog->meta.push_constants_size, f)) return 0;
    }

    // 7. Remaining Tensor Data (Non-scalars)
    for (u32 i = 0; i < prog->meta.tensor_count; ++i) {
        void* data_ptr = prog->tensor_data[i];
        if (data_ptr) {
            sf_type_info* info = &prog->tensor_infos[i];
            if (info->ndim == 0) continue; // Skip scalar constants (already in PC block)
            
            size_t sz = sf_shape_calc_bytes(info->dtype, info->shape, info->ndim);
            if (!_safe_write(data_ptr, 1, sz, f)) return 0;
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
    if (!_safe_write(&cart, sizeof(sf_cartridge_header), 1, f)) { fclose(f); return false; }

    for (u32 i = 0; i < section_count; ++i) {
        cart.sections[i].offset = (uint32_t)ftell(f);
        if (sections[i].type == SF_SECTION_PROGRAM) {
            size_t sz = _write_program((const sf_program*)sections[i].data, f);
            if (sz == 0) { fclose(f); return false; }
            cart.sections[i].size = (uint32_t)sz;
        } else {
            if (!_safe_write(sections[i].data, 1, sections[i].size, f)) { fclose(f); return false; }
            cart.sections[i].size = sections[i].size;
        }
    }

    // Rewrite header with correct offsets and sizes
    fseek(f, 0, SEEK_SET);
    if (!_safe_write(&cart, sizeof(sf_cartridge_header), 1, f)) { fclose(f); return false; }

    fclose(f);
    return true;
}
