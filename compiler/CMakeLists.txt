# --- Metadata Generation ---
find_package(Python3 REQUIRED)

set(SF_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SF_GEN_DIR}")

set(COMPILER_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/../tools/metadata/compiler_spec.json")

set(FUSION_TMPL "${CMAKE_CURRENT_SOURCE_DIR}/../tools/templates/sf_fusion_rules.c.j2")
set(FUSION_C "${SF_GEN_DIR}/sf_fusion_rules.c")
set(PIPELINE_TMPL "${CMAKE_CURRENT_SOURCE_DIR}/../tools/templates/sf_compiler_pipeline.c.j2")
set(PIPELINE_C "${SF_GEN_DIR}/sf_compiler_pipeline.c")
set(MANIFEST_TMPL "${SF_ISA_TEMPLATES}/sf_manifest_gen.c.j2")
set(MANIFEST_C "${SF_GEN_DIR}/sf_manifest_gen.c")

add_custom_command(
    OUTPUT "${FUSION_C}" "${PIPELINE_C}" "${MANIFEST_C}"
    COMMAND Python3::Interpreter "${SF_ISA_GEN_SCRIPT}" 
            --isa "${SF_ISA_METADATA}" 
            --compiler "${COMPILER_SPEC}"
            --manifest "${SF_MANIFEST_METADATA}"
            --render "${FUSION_TMPL}:${FUSION_C}"
                     "${PIPELINE_TMPL}:${PIPELINE_C}"
                     "${MANIFEST_TMPL}:${MANIFEST_C}"
    DEPENDS "${SF_ISA_METADATA}" "${COMPILER_SPEC}" "${SF_MANIFEST_METADATA}" "${SF_ISA_GEN_SCRIPT}" 
            "${FUSION_TMPL}" "${PIPELINE_TMPL}" "${MANIFEST_TMPL}"
    COMMENT "Generating Compiler Metadata and Fusion Rules"
    VERBATIM
)

add_library(compiler STATIC
    src/sf_compiler.c
    src/sf_compiler_manifest.c
    "${FUSION_C}"
    "${PIPELINE_C}"
    "${MANIFEST_C}"
    src/passes/sf_pass_lower.c
    src/passes/sf_pass_inline.c
    src/passes/sf_pass_decompose.c
    src/passes/sf_pass_analyze.c
    src/passes/sf_pass_validate.c
    src/passes/sf_pass_domain_split.c
    src/passes/sf_pass_fuse.c
    src/passes/sf_pass_liveness.c
    src/sf_json_parser.c
    src/sf_codegen.c
    src/sf_graph_utils.c
)
add_library(SionFlow::compiler ALIAS compiler)

target_include_directories(compiler 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE 
        src
)

target_link_libraries(compiler PUBLIC SionFlow::isa SionFlow::base)


# Compiler only needs ISA definitions and cJSON for parsing.
target_link_libraries(compiler 
    PUBLIC 
        SionFlow::isa
        SionFlow::base
)
