# Build for the pure-C++20 SofaBuffers core library (header-only).
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build

cmake_minimum_required(VERSION 3.10)
project(sofabuffers_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sofab_cpp INTERFACE)
target_include_directories(sofab_cpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(sofab::cpp ALIAS sofab_cpp)

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
