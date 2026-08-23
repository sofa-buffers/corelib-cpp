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

# The README's generator example, compiled and run. The example lives here as a
# real translation unit; the test also checks that README.md still shows exactly
# these lines, so a documented example cannot drift from one that compiles.
add_executable(test_readme_example test_readme_example.cpp)
target_compile_options(test_readme_example PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(test_readme_example PRIVATE
    SOFAB_README_PATH="${PROJECT_SOURCE_DIR}/README.md"
    SOFAB_SELF_PATH="${CMAKE_CURRENT_LIST_DIR}/test_readme_example.cpp")
target_link_libraries(test_readme_example PRIVATE sofab_cpp)
add_test(NAME test_readme_example COMMAND test_readme_example)

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


# CORELIB_PLAN §8: the shared vectors and the JSON reader are copies of
# corelib-c-cpp, the authoritative source. Their provenance record — which
# merged upstream commit each copy came from, and what it hashed to — is the
# only thing that makes a re-sync checkable, and no C++ test can see it rot:
# test_vectors replays whatever bytes are on disk, pinned or not. This check
# reads test/shared/README.md and fails on a pin that is not a bare merged
# commit (a branch or an unmerged PR head leaves a re-syncer nothing to diff
# against), on a recorded checksum that no longer matches the vendored file, and
# on a vendored file with no row at all.
add_test(NAME test_vendored_provenance
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/test_vendored_provenance.sh
            ${CMAKE_CURRENT_LIST_DIR}/shared/README.md
            ${CMAKE_COMMAND})
set_tests_properties(test_vendored_provenance PROPERTIES SKIP_RETURN_CODE 77)

if(SOFAB_ENABLE_COVERAGE)
    target_link_libraries(test_roundtrip PRIVATE sofab_coverage)
    target_link_libraries(test_vectors PRIVATE sofab_coverage)
endif()
