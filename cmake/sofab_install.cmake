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
