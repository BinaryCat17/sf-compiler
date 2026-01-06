cmake_minimum_required(VERSION 3.25)
project(sf-compiler LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Temporary: linking to sf-spec as a subdirectory for local development
# In the future, this will be find_package(sf-spec REQUIRED)
if (NOT TARGET sf_base)
    add_subdirectory(../sf-spec/base sf-spec-base)
endif()
if (NOT TARGET sf_isa)
    add_subdirectory(../sf-spec/isa sf-spec-isa)
endif()

# Modules
add_subdirectory(compiler)
add_subdirectory(sfc)
