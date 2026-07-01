cmake_minimum_required(VERSION 3.16)
project(HonestEQDSP LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Strict warnings — the DSP code must be clean.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wno-unused-parameter)
endif()

# Header-only library (interface target).
add_library(HonestEQDSP INTERFACE)
target_include_directories(HonestEQDSP INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(HonestEQDSP INTERFACE cxx_std_17)

# Tests
enable_testing()
add_subdirectory(tests)
