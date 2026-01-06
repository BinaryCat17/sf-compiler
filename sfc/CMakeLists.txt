add_executable(sfc src/main.c)

target_link_libraries(sfc PRIVATE 
    compiler
    SionFlow::isa
    SionFlow::base
)

target_include_directories(sfc PRIVATE src)

install(TARGETS sfc DESTINATION bin)
