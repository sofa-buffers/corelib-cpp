enable_testing()

add_executable(test_roundtrip test_roundtrip.cpp)
# -Wno-mismatched-new-delete: this TU *replaces* global operator new/delete with a
# counting malloc/free pair (see the allocation hook in test_roundtrip.cpp). gcc 15
# matches the std::malloc inside the replacement against the ::operator delete at
# the call site and reports a mismatch, which it is not — the pair is consistent.
target_compile_options(test_roundtrip PRIVATE -Wall -Wextra -Wpedantic -Werror
    -Wno-mismatched-new-delete)
# headerParsersEnforceCeilings() reads the header source itself: a copy of the
# field-header parser that no entry point reaches cannot be caught by feeding
# bytes, so the check that every decode site carries the §4.6/§4.8/§6.2 ceilings
# has to be structural.
target_compile_definitions(test_roundtrip PRIVATE
    SOFAB_HEADER_SOURCE_PATH="${PROJECT_SOURCE_DIR}/include/sofab/sofab.hpp")
target_link_libraries(test_roundtrip PRIVATE sofab_cpp)
add_test(NAME test_roundtrip COMMAND test_roundtrip)

# Full conformance against the shared vectors (uses the vendored JSON reader).
add_executable(test_vectors
    test_vectors.cpp
    ${CMAKE_CURRENT_LIST_DIR}/shared/sofab_test_json.c
)
target_include_directories(test_vectors PRIVATE ${CMAKE_CURRENT_LIST_DIR}/shared)
target_compile_definitions(test_vectors PRIVATE
    SOFAB_TEST_VECTORS_PATH="${PROJECT_SOURCE_DIR}/assets/test_vectors.json")
target_compile_options(test_vectors PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_vectors PRIVATE sofab_cpp)
add_test(NAME test_vectors COMMAND test_vectors)

# CORELIB_PLAN §12.1 requires the suite to run against a debug *and* a release
# build. Which configurations CI builds is a property of the workflow files, so
# a Release-only defect cannot be caught from inside the C++ suites — this check
# reads .github/workflows/ and fails when a required leg is missing.
add_test(NAME test_ci_workflows
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/test_ci_workflows.sh
            ${PROJECT_SOURCE_DIR}/.github/workflows)
set_tests_properties(test_ci_workflows PROPERTIES SKIP_RETURN_CODE 77)

if(SOFAB_ENABLE_COVERAGE)
    target_link_libraries(test_roundtrip PRIVATE sofab_coverage)
    target_link_libraries(test_vectors PRIVATE sofab_coverage)
endif()
