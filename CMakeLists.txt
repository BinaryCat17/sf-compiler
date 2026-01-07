cmake_minimum_required(VERSION 3.25)
project(sf-compiler LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Dependencies
if(NOT TARGET isa)
    find_package(sf-spec REQUIRED)
endif()

# Modules
add_subdirectory(compiler)
add_subdirectory(sfc)

# --- Installation & Export ---
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS compiler sfc
    EXPORT SionFlowCompilerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

export(EXPORT SionFlowCompilerTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/SionFlowCompilerTargets.cmake"
    NAMESPACE SionFlow::
)

install(DIRECTORY compiler/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT SionFlowCompilerTargets
    FILE SionFlowCompilerTargets.cmake
    NAMESPACE SionFlow::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sf-compiler
)

configure_package_config_file(
    cmake/sf-compiler-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sf-compiler-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sf-compiler
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/sf-compiler-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sf-compiler
)