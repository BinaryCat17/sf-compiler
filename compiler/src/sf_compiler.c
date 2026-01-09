#include <sionflow/compiler/sf_compiler.h>
#include "sf_passes.h"
#include "sf_compiler_internal.h"
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
        
        if (!pass->func(&ctx, diag) || (diag && diag->has_error)) {
            SF_LOG_ERROR("Pass '%s' failed", pass->name);
            return NULL;
        }
    }

    // 3. Allocate Program Structure
    sf_program* prog = SF_ARENA_PUSH(arena, sf_program, 1);
    memset(&prog->meta, 0, sizeof(sf_bin_header));

    // 4. Emit Code (Tensors, Instructions, State)
    if (!sf_codegen_emit(prog, &ctx, arena)) {
        sf_source_loc loc = {0};
        sf_compiler_diag_report(diag, loc, "Code generation failed.");
        return NULL;
    }
    
    return prog;
}

bool sf_compile_save_program(const sf_program* prog, const char* path) {
    sf_section_desc desc = { "main", SF_SECTION_PROGRAM, prog, 0 };
    return sf_compile_save_cartridge(path, NULL, &desc, 1);
}

bool sf_compile_save_cartridge(const char* path, const sf_graph_ir* ir, const sf_section_desc* sections, u32 section_count) {
    sf_cartridge_params params = {0};
    if (ir) {
        strncpy(params.app_title, ir->app_title, SF_MAX_TITLE_NAME - 1);
        params.window_width = ir->window_width;
        params.window_height = ir->window_height;
        params.num_threads = ir->num_threads;
        params.vsync = ir->vsync;
        params.fullscreen = ir->fullscreen;
        params.resizable = ir->resizable;
    } else {
        strncpy(params.app_title, "SionFlow Cartridge", SF_MAX_TITLE_NAME - 1);
        params.window_width = 800;
        params.window_height = 600;
        params.resizable = 1;
    }

    size_t total_sz = sf_cartridge_calc_size(&params, sections, section_count);
    void* buffer = malloc(total_sz);
    if (!buffer) return false;

    if (!sf_cartridge_save_to_buffer(&params, sections, section_count, buffer, total_sz)) {
        free(buffer);
        return false;
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(buffer);
        return false;
    }

    bool success = (fwrite(buffer, 1, total_sz, f) == total_sz);
    fclose(f);
    free(buffer);
    
    return success;
}
