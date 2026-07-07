cmake_minimum_required(VERSION 3.10)
project(sofabuffers_cpp VERSION LANGUAGES C CXX)

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
# consumer opts in). Package builds (vcpkg / Conan) add this via add_subdirectory
# / FetchContent and just want the header-only install target.
if(NOT DEFINED SOFAB_BUILD_TESTING)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SOFAB_BUILD_TESTING ON)
    else()
        set(SOFAB_BUILD_TESTING OFF)
    endif()
endif()

if(SOFAB_BUILD_TESTING)
enable_testing()

add_executable(test_roundtrip test/test_roundtrip.cpp)
target_compile_options(test_roundtrip PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_roundtrip PRIVATE sofab_cpp)
add_test(NAME test_roundtrip COMMAND test_roundtrip)

# Full conformance against the shared vectors (uses the vendored JSON reader).
add_executable(test_vectors
    test/test_vectors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/shared/sofab_test_json.c
)
target_include_directories(test_vectors PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/shared)
target_compile_definitions(test_vectors PRIVATE
    SOFAB_TEST_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/test_vectors.json")
target_compile_options(test_vectors PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_vectors PRIVATE sofab_cpp)
add_test(NAME test_vectors COMMAND test_vectors)

if(SOFAB_ENABLE_COVERAGE)
    target_link_libraries(test_roundtrip PRIVATE sofab_coverage)
    target_link_libraries(test_vectors PRIVATE sofab_coverage)
endif()

# Benchmarks — always built at -O3 (this library targets speed). Mirror the C /
# C++ benches (same workloads) so figures are directly comparable; the bench
# binary doubles as the Callgrind single-shot driver (run_<workload> symbols).
add_executable(bench bench/bench.cpp)
target_compile_options(bench PRIVATE -O3 -g -DNDEBUG -Wall -Wextra)
target_link_libraries(bench PRIVATE sofab_cpp)

add_executable(perf bench/perf.cpp)
target_compile_options(perf PRIVATE -O3 -g -DNDEBUG -Wall -Wextra)
target_link_libraries(perf PRIVATE sofab_cpp)

add_custom_target(run_bench COMMAND bench DEPENDS bench USES_TERMINAL)
add_custom_target(run_perf COMMAND perf DEPENDS perf USES_TERMINAL)

# Machine-independent instruction counts: run each bench workload once under
# Callgrind, collecting only the run_<workload> entry point (extern "C" + noinline
# in bench.cpp), so the figures line up with the C / C++ corelib benchmarks.
find_program(VALGRIND_EXECUTABLE valgrind)
if(VALGRIND_EXECUTABLE)
    add_custom_target(run_bench_callgrind
        COMMAND ${CMAKE_COMMAND} -E echo "Counting instructions/op with Callgrind"
        DEPENDS bench
        USES_TERMINAL)
    foreach(workload encode_u64_array encode_typical decode_u64_array decode_typical)
        add_custom_command(TARGET run_bench_callgrind POST_BUILD
            COMMAND ${VALGRIND_EXECUTABLE} --tool=callgrind --quiet
                    --callgrind-out-file=${CMAKE_BINARY_DIR}/callgrind.${workload}.out
                    --collect-atstart=no --toggle-collect=run_${workload}
                    $<TARGET_FILE:bench> ${workload}
            VERBATIM)
    endforeach()
endif()
endif() # SOFAB_BUILD_TESTING

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
# sofa-buffers-corelib-cpp package config is what the vcpkg port and the Conan
# recipe consume (find_package(sofa-buffers-corelib-cpp CONFIG)).
if(SOFAB_INSTALL)
    include(CMakePackageConfigHelpers)

    set(SOFAB_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/sofa-buffers-corelib-cpp
        CACHE STRING "Install location for the CMake package config files")

    set_target_properties(sofab_cpp PROPERTIES EXPORT_NAME corelib)

    install(TARGETS sofab_cpp EXPORT sofab_cpp_targets)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT sofab_cpp_targets
        FILE sofa-buffers-corelib-cpp-targets.cmake
        NAMESPACE sofa-buffers::
        DESTINATION ${SOFAB_INSTALL_CMAKEDIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sofa-buffers-corelib-cpp-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/sofa-buffers-corelib-cpp-config.cmake
        INSTALL_DESTINATION ${SOFAB_INSTALL_CMAKEDIR})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/sofa-buffers-corelib-cpp-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/sofa-buffers-corelib-cpp-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/sofa-buffers-corelib-cpp-config-version.cmake
        DESTINATION ${SOFAB_INSTALL_CMAKEDIR})
endif()
