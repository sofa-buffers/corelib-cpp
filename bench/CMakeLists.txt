# Benchmarks — always built at -O3 (this library targets speed). Mirror the C /
# C++ benches (same workloads) so figures are directly comparable; the bench
# binary doubles as the Callgrind single-shot driver (run_<workload> symbols).
add_executable(bench bench.cpp)
target_compile_options(bench PRIVATE -O3 -g -DNDEBUG -Wall -Wextra)
target_link_libraries(bench PRIVATE sofab_cpp)

add_executable(perf perf.cpp)
target_compile_options(perf PRIVATE -O3 -g -DNDEBUG -Wall -Wextra)
target_link_libraries(perf PRIVATE sofab_cpp)

add_custom_target(run_bench COMMAND bench DEPENDS bench USES_TERMINAL)
add_custom_target(run_perf COMMAND perf DEPENDS perf USES_TERMINAL)

# Machine-independent instruction counts: run each bench workload once under
# Callgrind, collecting only the run_<workload> entry point (extern "C" + noinline
# in bench.cpp), so the figures line up with the C / C++ corelib benchmarks.
#
# The target drives bench/run_callgrind.sh instead of looping over its own copy of
# the workload names: the list lives once, in the table bench.cpp dispatches on,
# and reaches every tool through `bench --list` (CORELIB_PLAN §10).
find_program(VALGRIND_EXECUTABLE valgrind)
if(VALGRIND_EXECUTABLE)
    add_custom_target(run_bench_callgrind
        COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR}
                bash ${CMAKE_CURRENT_SOURCE_DIR}/run_callgrind.sh
        USES_TERMINAL
        VERBATIM)
    add_dependencies(run_bench_callgrind bench)
endif()

# The three tools stay comparable only while they share one definition of the
# workloads, the stream adapters and the timing rules. This checks that sharing
# structurally, and drives `bench --list` plus run_callgrind.sh end to end (with a
# stub valgrind, so it needs no Valgrind and runs in milliseconds).
add_test(NAME test_bench_tools
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_bench_tools.sh
            $<TARGET_FILE:bench> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_bench_tools PROPERTIES SKIP_RETURN_CODE 77)
