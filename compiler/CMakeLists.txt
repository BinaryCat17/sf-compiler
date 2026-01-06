# --- Metadata Generation ---
find_package(Python3 REQUIRED)
set(ISA_GEN_PY "${CMAKE_CURRENT_SOURCE_DIR}/../../sf-spec/tools/isa_gen.py")
set(ISA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/../../sf-spec/tools/metadata/isa.json")
set(COMPILER_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/../tools/metadata/compiler_spec.json")
set(MANIFEST_JSON "${CMAKE_CURRENT_SOURCE_DIR}/../../sf-spec/tools/metadata/manifest.json")
set(FUSION_TMPL "${CMAKE_CURRENT_SOURCE_DIR}/../tools/templates/sf_fusion_rules.c.j2")
set(FUSION_C "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generated/sf_fusion_rules.c")
set(PIPELINE_TMPL "${CMAKE_CURRENT_SOURCE_DIR}/../tools/templates/sf_compiler_pipeline.c.j2")
set(PIPELINE_C "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generated/sf_compiler_pipeline.c")
set(MANIFEST_TMPL "${CMAKE_CURRENT_SOURCE_DIR}/../../sf-spec/tools/templates/sf_manifest_gen.c.j2")
set(MANIFEST_C "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generated/sf_manifest_gen.c")

add_custom_command(
    OUTPUT "${FUSION_C}" "${PIPELINE_C}" "${MANIFEST_C}"
    COMMAND Python3::Interpreter "${ISA_GEN_PY}" --isa "${ISA_JSON}" 
            --compiler "${COMPILER_SPEC}"
            --manifest "${MANIFEST_JSON}"
            --render "${FUSION_TMPL}:${FUSION_C}"
                     "${PIPELINE_TMPL}:${PIPELINE_C}"
                     "${MANIFEST_TMPL}:${MANIFEST_C}"
    DEPENDS "${ISA_JSON}" "${COMPILER_SPEC}" "${MANIFEST_JSON}" "${ISA_GEN_PY}" 
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
        $<INSTALL_INTERFACE:include>
    PRIVATE src
)

# Compiler only needs ISA definitions and cJSON for parsing.
target_link_libraries(compiler 
    PUBLIC 
        SionFlow::isa
        SionFlow::base
)
