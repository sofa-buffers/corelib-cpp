# Header-only C++20 library: the "build" is just the install of include/ plus the
# generated sofa-buffers-corelib-cpp CMake package config.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sofa-buffers/corelib-cpp
    REF "v${VERSION}"
    # Replace with the SHA512 of the v${VERSION} release tarball:
    #   vcpkg hash <downloaded-tarball>   (or copy it from the failed-install log)
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSOFAB_BUILD_TESTING=OFF
        -DSOFAB_INSTALL=ON
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME sofa-buffers-corelib-cpp
    CONFIG_PATH lib/cmake/sofa-buffers-corelib-cpp
)

# Header-only — nothing arch-specific, so drop the debug tree and the now-empty
# lib directory left behind after the config files were moved to share/.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug"
    "${CURRENT_PACKAGES_DIR}/lib"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
