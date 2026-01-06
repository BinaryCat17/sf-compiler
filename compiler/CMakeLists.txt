# --- Metadata Generation ---
find_package(Python3 REQUIRED)
set(ISA_GEN_PY "${CMAKE_CURRENT_SOURCE_DIR}/../../sf-spec/tools/isa_gen.py")
set(ISA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/../../sf-spec/isa/isa.json")
set(METADATA_TMPL "${CMAKE_CURRENT_SOURCE_DIR}/../tools/templates/sf_op_metadata.c.j2")
set(METADATA_C "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generated/sf_op_metadata.c")

add_custom_command(
    OUTPUT "${METADATA_C}"
    COMMAND Python3::Interpreter "${ISA_GEN_PY}" --isa "${ISA_JSON}" 
            --render "${METADATA_TMPL}:${METADATA_C}"
    DEPENDS "${ISA_JSON}" "${ISA_GEN_PY}" "${METADATA_TMPL}"
    COMMENT "Generating Compiler Metadata"
    VERBATIM
)

add_library(compiler STATIC
    src/sf_compiler.c
    src/sf_compiler_manifest.c
    "${METADATA_C}"
    src/passes/sf_pass_lower.c
    src/passes/sf_pass_inline.c
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
