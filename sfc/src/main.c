#include <sionflow/compiler/sf_compiler.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_memory.h>
#include <sionflow/base/sf_utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_usage() {
    printf("SionFlow Cartridge Compiler (sfc) v1.3\n");
    printf("Usage: sfc <input.mfapp|input.json> [output.sfc]\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char* input_path = argv[1];
    char output_path[256];
    if (argc >= 3) {
        strncpy(output_path, argv[2], 255);
    } else {
        strncpy(output_path, input_path, 250);
        char* ext = strrchr(output_path, '.');
        if (ext) *ext = '\0';
        strcat(output_path, ".sfc");
    }

    size_t arena_size = 1024 * 1024 * 128; // 128MB
    void* backing = malloc(arena_size);
    sf_arena arena;
    sf_arena_init(&arena, backing, arena_size);

    sf_section_desc sections[SF_MAX_SECTIONS];
    u32 section_count = 0;
    sf_graph_ir app_ir = {0};

    const char* ext = sf_path_get_ext(input_path);
    bool success = false;

    if (strcmp(ext, "mfapp") == 0) {
        sf_compiler_manifest manifest;
        if (sf_compiler_load_manifest(input_path, &manifest, &arena)) {
            app_ir = manifest.app_ir;
            for (u32 i = 0; i < manifest.kernel_count; ++i) {
                SF_LOG_INFO("Compiling kernel \'%s\'...", manifest.kernels[i].id);
                sf_compiler_diag diag; sf_compiler_diag_init(&diag, &arena);
                sf_graph_ir k_ir = {0};
                if (sf_compile_load_json(manifest.kernels[i].path, &k_ir, &arena, &diag)) {
                    sf_program* prog = sf_compile(&k_ir, &arena, &diag);
                    if (prog) {
                        sections[section_count++] = (sf_section_desc){ manifest.kernels[i].id, SF_SECTION_PROGRAM, prog, 0 };
                    } else success = false;
                } else success = false;
            }
            // Embed assets
            for (u32 i = 0; i < manifest.asset_count; ++i) {
                size_t f_size = 0;
                void* f_data = sf_file_read_bin(manifest.assets[i].path, &f_size);
                if (f_data) {
                    void* arena_data = sf_arena_alloc((sf_allocator*)&arena, f_size);
                    memcpy(arena_data, f_data, f_size);
                    free(f_data);
                    sections[section_count++] = (sf_section_desc){ manifest.assets[i].name, manifest.assets[i].type, arena_data, (u32)f_size };
                    SF_LOG_INFO("Embedded asset \'%s\'", manifest.assets[i].name);
                }
            }
            // Embed pipeline
            sections[section_count++] = (sf_section_desc){ "pipeline", SF_SECTION_PIPELINE, manifest.raw_json, manifest.raw_json_size };
            success = true;
        }
    } else {
        SF_LOG_INFO("Compiling single graph %s...", input_path);
        sf_compiler_diag diag;
        sf_compiler_diag_init(&diag, &arena);
        if (sf_compile_load_json(input_path, &app_ir, &arena, &diag)) {
            sf_program* prog = sf_compile(&app_ir, &arena, &diag);
            if (prog) {
                sections[section_count++] = (sf_section_desc){ "main", SF_SECTION_PROGRAM, prog, 0 };
                success = true;
            }
        }
    }

    if (success) {
        if (!sf_compile_save_cartridge(output_path, &app_ir, sections, section_count)) {
            SF_LOG_ERROR("Failed to save cartridge.");
            success = false;
        } else {
            SF_LOG_INFO("Successfully created cartridge: %s", output_path);
        }
    }

    free(backing);
    return success ? 0 : 1;
}
