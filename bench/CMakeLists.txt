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
