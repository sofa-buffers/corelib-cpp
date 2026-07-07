enable_testing()

add_executable(test_roundtrip test_roundtrip.cpp)
target_compile_options(test_roundtrip PRIVATE -Wall -Wextra -Wpedantic -Werror)
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

if(SOFAB_ENABLE_COVERAGE)
    target_link_libraries(test_roundtrip PRIVATE sofab_coverage)
    target_link_libraries(test_vectors PRIVATE sofab_coverage)
endif()
