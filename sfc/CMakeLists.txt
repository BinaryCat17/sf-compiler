add_executable(sfc src/main.c)

target_link_libraries(sfc PRIVATE 
    sf_compiler
    sf_isa
    sf_base
)

target_include_directories(sfc PRIVATE src)

install(TARGETS sfc DESTINATION bin)
