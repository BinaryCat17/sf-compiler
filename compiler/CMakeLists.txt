add_library(compiler STATIC
    src/sf_compiler.c
    src/sf_compiler_manifest.c
    src/sf_op_metadata.c
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
