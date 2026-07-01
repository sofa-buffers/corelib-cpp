# Build for the pure-C++20 SofaBuffers core library (header-only).
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build

cmake_minimum_required(VERSION 3.10)
project(sofabuffers_cpp VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SOFAB_ENABLE_COVERAGE "Instrument the test binaries for gcov/gcovr coverage" OFF)
option(SOFAB_ENABLE_DOXYGEN  "Add a 'doc' target that builds the Doxygen API docs" OFF)

add_library(sofab_cpp INTERFACE)
target_include_directories(sofab_cpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(sofab::cpp ALIAS sofab_cpp)
add_library(sofa-buffers::corelib ALIAS sofab_cpp)

# Coverage: instrument the test executables (not the speed-tuned benches) so the
# gcovr report measures how much of include/sofab/sofab.hpp the suites exercise.
if(SOFAB_ENABLE_COVERAGE)
    add_library(sofab_coverage INTERFACE)
    target_compile_options(sofab_coverage INTERFACE --coverage -O0 -g)
    target_link_options(sofab_coverage INTERFACE --coverage)
endif()

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
