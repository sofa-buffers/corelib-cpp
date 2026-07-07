cmake_minimum_required(VERSION 3.10)
project(sofabuffers_cpp
    VERSION 0.8.0
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SOFAB_ENABLE_COVERAGE "Instrument the test binaries for gcov/gcovr coverage" OFF)
option(SOFAB_ENABLE_DOXYGEN  "Add a 'doc' target that builds the Doxygen API docs" OFF)

include(GNUInstallDirs)

option(SOFAB_INSTALL "Generate the install target and CMake package config" ON)

add_library(sofab_cpp INTERFACE)
target_include_directories(sofab_cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# The header is C++20 (concepts, std::bit_cast, std::span): propagate that to
# consumers of the exported target so find_package() users compile at the right
# standard automatically.
target_compile_features(sofab_cpp INTERFACE cxx_std_20)
add_library(sofa-buffers::corelib ALIAS sofab_cpp)

# Coverage: instrument the test executables (not the speed-tuned benches) so the
# gcovr report measures how much of include/sofab/sofab.hpp the suites exercise.
if(SOFAB_ENABLE_COVERAGE)
    add_library(sofab_coverage INTERFACE)
    target_compile_options(sofab_coverage INTERFACE --coverage -O0 -g)
    target_link_options(sofab_coverage INTERFACE --coverage)
endif()

# Only build the tests/benches when SofaBuffers is the top-level project (or the
# consumer opts in). Package builds (Conan) and add_subdirectory / FetchContent
# consumers just want the header-only install target.
if(NOT DEFINED SOFAB_BUILD_TESTING)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SOFAB_BUILD_TESTING ON)
    else()
        set(SOFAB_BUILD_TESTING OFF)
    endif()
endif()

if(SOFAB_BUILD_TESTING)
    # enable_testing() must run in the top-level directory so CTest generates the
    # root CTestTestfile.cmake that recurses into test/. Without it, `ctest` from
    # the build root reports "No tests were found" and every CI test/coverage step
    # silently passes without running a single test.
    enable_testing()
    add_subdirectory(test)
    add_subdirectory(bench)
endif()

# API documentation (Doxygen). Mirrors the C/C++ corelib's `doc` target so the CI
# can publish the HTML to GitHub Pages on every successful main build.
if(SOFAB_ENABLE_DOXYGEN)
    find_package(Doxygen REQUIRED)
    set(DOXYGEN_IN  ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
    add_custom_target(doc
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM)
endif()

# Install / packaging. Header-only, so the install just copies include/ and
# exports an INTERFACE target as sofa-buffers::corelib. The generated
# sofa-buffers-corelib-cpp package config is what the Conan recipe consumes
# (find_package(sofa-buffers-corelib-cpp CONFIG)).
if(SOFAB_INSTALL)
    include(cmake/sofab_install.cmake)
endif()
