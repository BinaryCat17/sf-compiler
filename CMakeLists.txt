cmake_minimum_required(VERSION 3.25)
project(sf-compiler LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Dependencies
if(NOT SF_WORKSPACE_MODE)
    find_package(sf-spec REQUIRED)
endif()

# Modules
add_subdirectory(compiler)
add_subdirectory(sfc)

# --- Installation & Export ---

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install targets
install(TARGETS compiler sfc
    EXPORT SionFlowCompilerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install headers
install(DIRECTORY compiler/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Generate and install the export file
install(EXPORT SionFlowCompilerTargets
    FILE SionFlowCompilerTargets.cmake
    NAMESPACE SionFlow::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sf-compiler
)

# Create the config file
configure_package_config_file(
    cmake/sf-compiler-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sf-compiler-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sf-compiler
)

# Install the config file
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/sf-compiler-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sf-compiler
)
