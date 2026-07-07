#!/usr/bin/env bash
#
# Verify the version is identical across every place a release version is
# declared: the CMake project() version (which drives the generated CMake
# package config) and the vcpkg / Conan package config files.
#
# Usage:
#   check-package-versions.sh              # just check the files agree with each other
#   check-package-versions.sh 1.2.3        # additionally require they all equal 1.2.3
#                                          # (pass the release tag with the leading 'v' stripped)
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

extract() {
    # extract <file> <perl-regex-with-\K-before-the-version>
    local file=$1 regex=$2 value
    value=$(grep -oP "$regex" "$file" || true)
    if [ -z "$value" ]; then
        echo "ERROR: could not find a version in $file" >&2
        exit 1
    fi
    printf '%s' "$value"
}

cmake_ver=$(extract CMakeLists.txt \
    'project\([^)]*VERSION[[:space:]]+\K[0-9]+\.[0-9]+\.[0-9]+')
vcpkg_ver=$(extract ports/sofa-buffers-corelib-cpp/vcpkg.json \
    '"version"[[:space:]]*:[[:space:]]*"\K[0-9]+\.[0-9]+\.[0-9]+')
conan_ver=$(extract conanfile.py \
    'version[[:space:]]*=[[:space:]]*"\K[0-9]+\.[0-9]+\.[0-9]+')

echo "Declared versions:"
echo "  CMakeLists.txt              : $cmake_ver"
echo "  ports/.../vcpkg.json        : $vcpkg_ver"
echo "  conanfile.py                : $conan_ver"

fail=0

if [ "$cmake_ver" != "$vcpkg_ver" ] || [ "$cmake_ver" != "$conan_ver" ]; then
    echo "ERROR: package config versions disagree — bump them together." >&2
    fail=1
fi

if [ "$#" -ge 1 ]; then
    expected=$1
    echo "  git tag (expected)          : $expected"
    for v in "$cmake_ver" "$vcpkg_ver" "$conan_ver"; do
        if [ "$v" != "$expected" ]; then
            echo "ERROR: a package config version ($v) does not match the release tag ($expected)." >&2
            fail=1
        fi
    done
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "OK: all package config versions match."
